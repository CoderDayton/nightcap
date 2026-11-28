#ifndef MOCKTAIL_RUNTIME_ROBLOX_HTTP_FAILURE_BODY_HOOK_H_
#define MOCKTAIL_RUNTIME_ROBLOX_HTTP_FAILURE_BODY_HOOK_H_

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/roblox_code_patch.h"

namespace mocktail {
namespace runtime {

// Roblox's HttpTraceError line gives a failed response's status and body
// size but not its text. Right after writing it, the trace function reads the
// body with `lea rdi,[response+0x48]; call getter`. The hook sends that one
// call through Nightcap, which logs the body of 4xx and 5xx responses.

struct HttpFailureBodyCall {
  std::uintptr_t call = 0;
  std::uintptr_t getter = 0;
};

std::optional<HttpFailureBodyCall> LocateHttpFailureBodyCall(
    const std::vector<CursorHookSegment>& segments, std::string* error);

// Bytes outside printable ASCII escaped, cut at 300, join tickets redacted.
std::string FormatHttpFailureBody(std::string_view body);

class HttpFailureBodySeen {
 public:
  bool FirstTime(int status, std::string_view body);

 private:
  std::mutex mutex_;
  std::vector<std::size_t> seen_;
};

bool InstallRobloxHttpFailureBodyHook(std::uintptr_t image_base,
                                      std::string* error);

}  // namespace runtime
}  // namespace mocktail

#endif
