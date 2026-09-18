#include "mocktail/graphics/chrome_trace_writer.h"

#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace mocktail {
namespace graphics {
namespace {

// Buffered bytes that wake the writer before its periodic tick.
constexpr std::size_t kWakeBytes = 256 * 1024;
constexpr std::chrono::milliseconds kWriteInterval{250};
// Every write ends the file with this, and the next write starts over it.
constexpr char kArrayEnd[] = "\n]\n";
constexpr long kArrayEndBytes = sizeof(kArrayEnd) - 1;

void AppendInteger(std::string* out, std::int64_t value) {
  char digits[24];
  const auto [end, error] =
      std::to_chars(digits, digits + sizeof(digits), value);
  if (error == std::errc()) {
    out->append(digits, end);
  } else {
    // Appending nothing would leave "ts": with no value, which costs the
    // whole trace rather than the one field.
    out->push_back('0');
  }
}

void AppendDouble(std::string* out, double value) {
  // to_chars spells inf and nan without quotes, which no JSON reader accepts.
  if (!std::isfinite(value)) {
    out->push_back('0');
    return;
  }
  char digits[32];
  const auto [end, error] =
      std::to_chars(digits, digits + sizeof(digits), value);
  if (error == std::errc()) {
    out->append(digits, end);
  } else {
    out->push_back('0');
  }
}

void AppendString(std::string* out, const char* value) {
  out->push_back('"');
  for (const char* cursor = value != nullptr ? value : ""; *cursor != '\0';
       ++cursor) {
    const unsigned char byte = static_cast<unsigned char>(*cursor);
    if (byte == '"' || byte == '\\') {
      out->push_back('\\');
      out->push_back(static_cast<char>(byte));
    } else if (byte < 0x20) {
      static constexpr char kHex[] = "0123456789abcdef";
      out->append("\\u00");
      out->push_back(kHex[byte >> 4]);
      out->push_back(kHex[byte & 0xf]);
    } else {
      out->push_back(static_cast<char>(byte));
    }
  }
  out->push_back('"');
}

void AppendHeader(std::string* out, const char* name, const char* category,
                  char phase, std::uint64_t timestamp_ns) {
  static const std::int64_t pid = getpid();
  thread_local const std::int64_t tid = gettid();
  out->append("{\"name\":");
  AppendString(out, name);
  if (category != nullptr) {
    out->append(",\"cat\":");
    AppendString(out, category);
  }
  out->append(",\"ph\":\"");
  out->push_back(phase);
  out->append("\",\"ts\":");
  AppendInteger(out, static_cast<std::int64_t>(timestamp_ns / 1000));
  out->append(",\"pid\":");
  AppendInteger(out, pid);
  out->append(",\"tid\":");
  AppendInteger(out, tid);
}

std::string& ThreadScratch() {
  thread_local std::string scratch;
  scratch.clear();
  return scratch;
}

}  // namespace

std::uint64_t TraceClockNanos() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(now.tv_nsec);
}

std::unique_ptr<ChromeTraceWriter> ChromeTraceWriter::Open(
    const std::filesystem::path& path, std::string* error) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr || std::fputs("[", file) == EOF ||
      std::fputs(kArrayEnd, file) == EOF || std::fflush(file) != 0) {
    const int saved = errno;
    if (file != nullptr) {
      std::fclose(file);
    }
    if (error != nullptr) {
      *error = "could not open profile trace " + path.string() + ": " +
               std::strerror(saved);
    }
    return nullptr;
  }
  return std::unique_ptr<ChromeTraceWriter>(new ChromeTraceWriter(file));
}

ChromeTraceWriter::ChromeTraceWriter(std::FILE* file)
    : file_(file), owner_pid_(getpid()) {
  writer_ = std::thread([this] { WriterLoop(); });
}

ChromeTraceWriter::~ChromeTraceWriter() { Close(); }

void ChromeTraceWriter::Slice(const char* name, const char* category,
                              std::uint64_t start_ns, std::uint64_t end_ns,
                              std::initializer_list<TraceArg> args) {
  Slice(name, category, start_ns, end_ns, args.begin(), args.size());
}

void ChromeTraceWriter::Slice(const char* name, const char* category,
                              std::uint64_t start_ns, std::uint64_t end_ns,
                              const TraceArg* args, std::size_t arg_count) {
  std::string& event = ThreadScratch();
  AppendHeader(&event, name, category, 'X', start_ns);
  event.append(",\"dur\":");
  const std::uint64_t start_us = start_ns / 1000;
  const std::uint64_t end_us = end_ns / 1000;
  AppendInteger(&event, static_cast<std::int64_t>(
                            end_us > start_us ? end_us - start_us : 0));
  if (args != nullptr && arg_count != 0) {
    event.append(",\"args\":{");
    for (std::size_t index = 0; index < arg_count; ++index) {
      if (index != 0) {
        event.push_back(',');
      }
      AppendString(&event, args[index].key);
      event.push_back(':');
      AppendInteger(&event, args[index].value);
    }
    event.push_back('}');
  }
  event.push_back('}');
  Append(event.data(), event.size());
}

void ChromeTraceWriter::Counter(const char* name, std::uint64_t timestamp_ns,
                                double value) {
  std::string& event = ThreadScratch();
  AppendHeader(&event, name, nullptr, 'C', timestamp_ns);
  event.append(",\"args\":{\"value\":");
  AppendDouble(&event, value);
  event.append("}}");
  Append(event.data(), event.size());
}

void ChromeTraceWriter::Append(const char* event, std::size_t size) {
  bool wake = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return;
    }
    pending_.append(first_event_ ? "\n" : ",\n");
    first_event_ = false;
    pending_.append(event, size);
    wake = pending_.size() >= kWakeBytes;
  }
  if (wake) {
    wake_.notify_one();
  }
}

void ChromeTraceWriter::WriterLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    wake_.wait_for(lock, kWriteInterval, [this] {
      return stopping_ || pending_.size() >= kWakeBytes;
    });
    if (stopping_) {
      break;
    }
    lock.unlock();
    WritePending();
    lock.lock();
  }
}

// The file lock is taken before the buffer lock, so chunks reach the file in
// the order they were buffered.
void ChromeTraceWriter::WritePending() {
  std::lock_guard<std::mutex> file_lock(file_mutex_);
  spare_.clear();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.swap(spare_);
  }
  if (file_ == nullptr || spare_.empty()) {
    return;
  }
  if (std::fseek(file_, -kArrayEndBytes, SEEK_END) != 0 ||
      std::fwrite(spare_.data(), 1, spare_.size(), file_) != spare_.size() ||
      std::fputs(kArrayEnd, file_) == EOF || std::fflush(file_) != 0) {
    std::fprintf(stderr, "  [profile] trace write failed: %s\n",
                 std::strerror(errno));
    std::fclose(file_);
    file_ = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
      // Nothing can reach the file now, so the writer thread exits instead of
      // ticking for the rest of the process.
      stopping_ = true;
    }
    wake_.notify_all();
  }
}

void ChromeTraceWriter::Flush() { WritePending(); }

void ChromeTraceWriter::Close() {
  // A forked child shares the file but not the writer thread.
  if (getpid() != owner_pid_) {
    return;
  }
  // A write failure sets stopping_ without joining the thread, so the join
  // below runs even when stopping_ is already set.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (writer_.joinable() && writer_.get_id() != std::this_thread::get_id()) {
    writer_.join();
  }
  WritePending();
  std::lock_guard<std::mutex> file_lock(file_mutex_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

ChromeTraceWriter* ActiveProfileTrace() {
  static ChromeTraceWriter* const trace = []() -> ChromeTraceWriter* {
    const char* path = std::getenv(kProfileTraceEnvironment);
    if (path == nullptr || path[0] == '\0') {
      return nullptr;
    }
    std::string error;
    std::unique_ptr<ChromeTraceWriter> writer =
        ChromeTraceWriter::Open(path, &error);
    if (writer == nullptr) {
      std::fprintf(stderr, "  [profile] %s\n", error.c_str());
      return nullptr;
    }
    std::fprintf(stderr, "  [profile] writing Chrome trace to %s\n", path);
    // Leaked so threads still recording during exit never touch a destroyed
    // writer; they find it closed instead.
    static struct CloseAtExit {
      ChromeTraceWriter* writer;
      ~CloseAtExit() { writer->Close(); }
    } close_at_exit{writer.get()};
    return writer.release();
  }();
  return trace;
}

}  // namespace graphics
}  // namespace mocktail
