#ifndef MOCKTAIL_GRAPHICS_CHROME_TRACE_WRITER_H_
#define MOCKTAIL_GRAPHICS_CHROME_TRACE_WRITER_H_

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mocktail {
namespace graphics {

// Environment variable naming the trace file the Vulkan adapter writes.
inline constexpr char kProfileTraceEnvironment[] = "MOCKTAIL_PROFILE_TRACE";

struct TraceArg {
  const char* key;
  std::int64_t value;
};

// CLOCK_MONOTONIC in nanoseconds.
std::uint64_t TraceClockNanos();

// Writes the Trace Event Format JSON array read by chrome://tracing and
// ui.perfetto.dev. Recording threads only format into a shared buffer; a
// background thread owns file writes, so disk stalls do not land inside the
// measured calls. Events are written in arrival order. Each write ends the
// file with the closing bracket, so a killed session leaves a loadable trace
// holding everything written before it died.
// Names, categories, and keys must outlive the call.
class ChromeTraceWriter final {
 public:
  static std::unique_ptr<ChromeTraceWriter> Open(
      const std::filesystem::path& path, std::string* error);

  ~ChromeTraceWriter();
  ChromeTraceWriter(const ChromeTraceWriter&) = delete;
  ChromeTraceWriter& operator=(const ChromeTraceWriter&) = delete;

  // A complete ("X") slice on the calling thread. Slices on one thread must
  // nest, which scoped timing gives.
  void Slice(const char* name, const char* category, std::uint64_t start_ns,
             std::uint64_t end_ns, std::initializer_list<TraceArg> args = {});
  void Slice(const char* name, const char* category, std::uint64_t start_ns,
             std::uint64_t end_ns, const TraceArg* args,
             std::size_t arg_count);
  // A counter ("C") sample; each name becomes one counter track.
  void Counter(const char* name, std::uint64_t timestamp_ns, double value);

  // Returns once every event recorded before the call is on disk.
  void Flush();
  // Flushes, closes the file, and drops later events. Idempotent.
  void Close();

 private:
  explicit ChromeTraceWriter(std::FILE* file);
  void Append(const char* event, std::size_t size);
  void WriterLoop();
  void WritePending();

  std::FILE* file_;
  const int owner_pid_;
  std::mutex file_mutex_;  // Guards file_ and spare_.
  std::string spare_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::string pending_;
  bool first_event_ = true;
  bool closed_ = false;
  bool stopping_ = false;
  std::thread writer_;
};

// The process trace named by MOCKTAIL_PROFILE_TRACE, opened on first use.
// Null when profiling is off or the file could not be opened. The writer is
// never destroyed; it is closed when the process exits normally.
ChromeTraceWriter* ActiveProfileTrace();

// Times its own lifetime as one slice. A null writer records nothing.
class TraceScope final {
 public:
  TraceScope(ChromeTraceWriter* writer, const char* name,
             const char* category)
      : writer_(writer),
        name_(name),
        category_(category),
        start_ns_(writer != nullptr ? TraceClockNanos() : 0) {}
  ~TraceScope() {
    if (writer_ != nullptr) {
      writer_->Slice(name_, category_, start_ns_, TraceClockNanos(), args_,
                     arg_count_);
    }
  }
  TraceScope(const TraceScope&) = delete;
  TraceScope& operator=(const TraceScope&) = delete;

  // Keeps the first kMaxArgs arguments.
  void Arg(const char* key, std::int64_t value) {
    if (writer_ != nullptr && arg_count_ < kMaxArgs) {
      args_[arg_count_++] = {key, value};
    }
  }

 private:
  static constexpr std::size_t kMaxArgs = 4;
  ChromeTraceWriter* writer_;
  const char* name_;
  const char* category_;
  std::uint64_t start_ns_;
  TraceArg args_[kMaxArgs]{};
  std::size_t arg_count_ = 0;
};

}  // namespace graphics
}  // namespace mocktail

#endif  // MOCKTAIL_GRAPHICS_CHROME_TRACE_WRITER_H_
