#include "runtime/failure_reason.h"

#include <sys/stat.h>

#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mocktail::runtime {
namespace {

constexpr std::size_t kMaximumReasonLines = 4;
constexpr std::size_t kMaximumReasonLineBytes = 300;

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

bool IsCrashHeader(std::string_view line) {
  constexpr std::string_view kPrefix = "[crash] SIG";
  if (!StartsWith(line, kPrefix)) {
    return false;
  }
  std::size_t index = kPrefix.size();
  while (index < line.size() && line[index] >= 'A' && line[index] <= 'Z') {
    ++index;
  }
  const std::string_view rest = line.substr(index);
  return index > kPrefix.size() &&
         (StartsWith(rest, " (code ") || StartsWith(rest, " signal="));
}

std::string Clip(std::string_view line) {
  if (line.size() <= kMaximumReasonLineBytes) {
    return std::string(line);
  }
  std::size_t cut = kMaximumReasonLineBytes;
  while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  return std::string(line.substr(0, cut)) + "...";
}

bool FileSize(const std::filesystem::path& path, off_t* size) {
  struct stat status {};
  if (stat(path.c_str(), &status) != 0) {
    return false;
  }
  *size = status.st_size;
  return true;
}

}

std::string FailureReasonFromLog(std::string_view log) {
  std::vector<std::string> reasons;
  bool after_crash_header = false;
  std::size_t start = 0;
  while (start < log.size() && reasons.size() < kMaximumReasonLines) {
    std::size_t end = log.find('\n', start);
    if (end == std::string_view::npos) {
      end = log.size();
    }
    std::string_view line = log.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    std::string reason;
    const bool crash_location =
        after_crash_header && StartsWith(line, "[crash] pc ");
    after_crash_header = false;
    if (crash_location) {
      reason = Clip(line.substr(8));
    } else if (StartsWith(line, "[FATAL] ")) {
      reason = Clip(line.substr(8));
    } else if (IsCrashHeader(line)) {
      reason = Clip(line.substr(8));
      after_crash_header = true;
    } else if (StartsWith(line, "[memory] limit exceeded")) {
      reason = Clip("Memory " + std::string(line.substr(9)));
    } else if (StartsWith(line, "Bionic assertion failed: ")) {
      reason = Clip(line);
    }
    if (!reason.empty() && (reasons.empty() || reasons.back() != reason)) {
      reasons.push_back(std::move(reason));
    }
  }
  std::string joined;
  for (const std::string& reason : reasons) {
    if (!joined.empty()) {
      joined += '\n';
    }
    joined += reason;
  }
  return joined;
}

std::string ReadSettledLogTail(const std::filesystem::path& path,
                               std::size_t max_bytes,
                               std::chrono::milliseconds poll,
                               std::chrono::milliseconds max_wait) {
  off_t size = 0;
  if (path.empty() || !FileSize(path, &size)) {
    return {};
  }
  const auto deadline = std::chrono::steady_clock::now() + max_wait;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(poll);
    off_t current = 0;
    if (!FileSize(path, &current)) {
      return {};
    }
    if (current == size) {
      break;
    }
    size = current;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  const auto file_size = static_cast<std::size_t>(size);
  const std::size_t offset = file_size > max_bytes ? file_size - max_bytes : 0;
  input.seekg(static_cast<std::streamoff>(offset));
  std::string tail(file_size - offset, '\0');
  input.read(tail.data(), static_cast<std::streamsize>(tail.size()));
  tail.resize(static_cast<std::size_t>(input.gcount()));
  if (offset > 0) {
    const std::size_t first_line_end = tail.find('\n');
    tail.erase(0, first_line_end == std::string::npos ? tail.size()
                                                      : first_line_end + 1);
  }
  return tail;
}

}
