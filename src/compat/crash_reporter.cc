#include "compat/crash_reporter.h"

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mocktail::compat {
namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
constexpr int kSignalSlots = 65;
constexpr std::size_t kMaxFrames = 32;
constexpr std::size_t kMaxScanned = 16;
constexpr std::size_t kScanBytes = 16 * 1024;
constexpr std::size_t kMaxMappings = 4096;

struct Mapping {
  std::uintptr_t start = 0;
  std::uintptr_t end = 0;
  std::uintptr_t offset = 0;
  const char* name = nullptr;
  std::size_t name_length = 0;
  bool executable = false;
};

std::atomic<bool> g_installed{false};
std::atomic<bool> g_reported{false};
std::atomic<int> g_guest_calls{0};
struct sigaction g_guest[kSignalSlots];
std::atomic<bool> g_guest_set[kSignalSlots];
alignas(16) char g_alt_stack[64 * 1024];
char g_maps[1 << 20];
std::size_t g_maps_size = 0;
Mapping g_mappings[kMaxMappings];
std::size_t g_mapping_count = 0;
std::uintptr_t g_frames[kMaxFrames];
std::size_t g_frame_count = 0;

class Line {
 public:
  Line() { Text("[crash] "); }

  Line& Text(const char* text) { return Bytes(text, std::strlen(text)); }

  Line& Bytes(const char* text, std::size_t length) {
    for (std::size_t i = 0; i < length && size_ < sizeof(buffer_) - 1; ++i) {
      buffer_[size_++] = text[i];
    }
    return *this;
  }

  Line& Hex(std::uintptr_t value) {
    char digits[2 + sizeof(value) * 2];
    std::size_t count = 0;
    do {
      digits[count++] = "0123456789abcdef"[value & 0xf];
      value >>= 4;
    } while (value != 0);
    Text("0x");
    while (count > 0) {
      Bytes(&digits[--count], 1);
    }
    return *this;
  }

  Line& Decimal(long value) {
    char digits[24];
    std::size_t count = 0;
    const bool negative = value < 0;
    unsigned long magnitude =
        negative ? 0UL - static_cast<unsigned long>(value)
                 : static_cast<unsigned long>(value);
    do {
      digits[count++] = static_cast<char>('0' + magnitude % 10);
      magnitude /= 10;
    } while (magnitude != 0);
    if (negative) {
      Text("-");
    }
    while (count > 0) {
      Bytes(&digits[--count], 1);
    }
    return *this;
  }

  void Write() {
    buffer_[size_++] = '\n';
    std::size_t written = 0;
    while (written < size_) {
      const ssize_t result =
          write(STDERR_FILENO, buffer_ + written, size_ - written);
      if (result <= 0) {
        return;
      }
      written += static_cast<std::size_t>(result);
    }
  }

 private:
  char buffer_[512];
  std::size_t size_ = 0;
};

const char* SignalName(int signal_number) {
  switch (signal_number) {
    case SIGSEGV:
      return "SIGSEGV";
    case SIGBUS:
      return "SIGBUS";
    case SIGILL:
      return "SIGILL";
    case SIGFPE:
      return "SIGFPE";
    case SIGABRT:
      return "SIGABRT";
    default:
      return "signal";
  }
}

bool ParseHex(const char*& cursor, const char* end, std::uintptr_t* value) {
  *value = 0;
  const char* start = cursor;
  while (cursor < end) {
    const char c = *cursor;
    int digit = -1;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    }
    if (digit < 0) {
      break;
    }
    *value = (*value << 4) | static_cast<std::uintptr_t>(digit);
    ++cursor;
  }
  return cursor != start;
}

const char* SkipField(const char* cursor, const char* end) {
  while (cursor < end && *cursor != ' ') {
    ++cursor;
  }
  while (cursor < end && *cursor == ' ') {
    ++cursor;
  }
  return cursor;
}

void LoadMappings() {
  g_maps_size = 0;
  g_mapping_count = 0;
  const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  while (g_maps_size < sizeof(g_maps)) {
    const ssize_t result =
        read(fd, g_maps + g_maps_size, sizeof(g_maps) - g_maps_size);
    if (result <= 0) {
      break;
    }
    g_maps_size += static_cast<std::size_t>(result);
  }
  close(fd);

  const char* cursor = g_maps;
  const char* const end = g_maps + g_maps_size;
  while (cursor < end && g_mapping_count < kMaxMappings) {
    const char* line_end = cursor;
    while (line_end < end && *line_end != '\n') {
      ++line_end;
    }
    Mapping mapping;
    const char* field = cursor;
    if (ParseHex(field, line_end, &mapping.start) && field < line_end &&
        *field == '-' && ParseHex(++field, line_end, &mapping.end)) {
      field = SkipField(field, line_end);
      mapping.executable = line_end - field > 2 && field[2] == 'x';
      field = SkipField(field, line_end);
      ParseHex(field, line_end, &mapping.offset);
      field = SkipField(field, line_end);
      field = SkipField(field, line_end);
      field = SkipField(field, line_end);
      const char* name = field;
      for (const char* c = field; c < line_end; ++c) {
        if (*c == '/') {
          name = c + 1;
        }
      }
      mapping.name = name;
      mapping.name_length = static_cast<std::size_t>(line_end - name);
      g_mappings[g_mapping_count++] = mapping;
    }
    cursor = line_end + 1;
  }
}

const Mapping* FindMapping(std::uintptr_t address) {
  for (std::size_t i = 0; i < g_mapping_count; ++i) {
    if (address >= g_mappings[i].start && address < g_mappings[i].end) {
      return &g_mappings[i];
    }
  }
  return nullptr;
}

Line& Describe(Line& line, std::uintptr_t address) {
  const Mapping* mapping = FindMapping(address);
  if (mapping == nullptr || mapping->name_length == 0) {
    return line.Hex(address);
  }
  return line.Bytes(mapping->name, mapping->name_length)
      .Text("+")
      .Hex(address - mapping->start + mapping->offset);
}

_Unwind_Reason_Code CollectFrame(_Unwind_Context* context, void*) {
  if (g_frame_count >= kMaxFrames) {
    return _URC_END_OF_STACK;
  }
  const std::uintptr_t ip = _Unwind_GetIP(context);
  if (ip == 0) {
    return _URC_END_OF_STACK;
  }
  g_frames[g_frame_count++] = ip;
  return _URC_NO_REASON;
}

void ContextRegisters(void* context, std::uintptr_t* pc, std::uintptr_t* sp) {
  *pc = 0;
  *sp = 0;
  if (context == nullptr) {
    return;
  }
  const auto* user = static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
  *pc = static_cast<std::uintptr_t>(user->uc_mcontext.gregs[REG_RIP]);
  *sp = static_cast<std::uintptr_t>(user->uc_mcontext.gregs[REG_RSP]);
#elif defined(__aarch64__)
  *pc = static_cast<std::uintptr_t>(user->uc_mcontext.pc);
  *sp = static_cast<std::uintptr_t>(user->uc_mcontext.sp);
#endif
}

bool AlreadyListed(std::uintptr_t address, std::size_t first) {
  for (std::size_t i = first; i < g_frame_count; ++i) {
    if (g_frames[i] == address || g_frames[i] == address - 1) {
      return true;
    }
  }
  return false;
}

void Report(int signal_number, siginfo_t* info, void* context) {
  LoadMappings();
  char thread_name[17] = {};
  prctl(PR_GET_NAME, thread_name, 0, 0, 0);
  Line header;
  header.Text(SignalName(signal_number)).Text(" (code ");
  header.Decimal(info != nullptr ? info->si_code : 0).Text(")");
  if (info != nullptr && info->si_code > 0 && signal_number != SIGABRT) {
    header.Text(" at ").Hex(reinterpret_cast<std::uintptr_t>(info->si_addr));
  }
  header.Text(" in thread \"").Text(thread_name).Text("\" tid ");
  header.Decimal(static_cast<long>(syscall(SYS_gettid))).Write();

  std::uintptr_t pc = 0;
  std::uintptr_t sp = 0;
  ContextRegisters(context, &pc, &sp);
  Line pc_line;
  Describe(pc_line.Text("pc "), pc).Write();

  g_frame_count = 0;
  _Unwind_Backtrace(CollectFrame, nullptr);
  std::size_t first = g_frame_count;
  for (std::size_t i = 0; i < g_frame_count; ++i) {
    if (g_frames[i] == pc || g_frames[i] == pc + 1) {
      first = i;
      break;
    }
  }
  if (first == g_frame_count) {
    first = 0;
  }
  for (std::size_t i = first; i < g_frame_count; ++i) {
    Line frame;
    frame.Text("#").Decimal(static_cast<long>(i - first)).Text(" ");
    Describe(frame, g_frames[i]).Write();
  }

  const Mapping* stack = FindMapping(sp);
  if (stack == nullptr) {
    return;
  }
  Line scan_header;
  scan_header.Text("code addresses on the stack:").Write();
  const std::uintptr_t limit =
      stack->end - sp < kScanBytes ? stack->end : sp + kScanBytes;
  std::size_t listed = 0;
  for (std::uintptr_t slot = sp & ~std::uintptr_t{7};
       slot + sizeof(std::uintptr_t) <= limit && listed < kMaxScanned;
       slot += sizeof(std::uintptr_t)) {
    const std::uintptr_t value = *reinterpret_cast<const std::uintptr_t*>(slot);
    const Mapping* target = FindMapping(value);
    if (target == nullptr || !target->executable ||
        AlreadyListed(value, first)) {
      continue;
    }
    Line entry;
    Describe(entry.Text("  "), value).Write();
    ++listed;
  }
}

void ResetAndResend(int signal_number, const siginfo_t* info) {
  struct sigaction fallback {};
  fallback.sa_handler = SIG_DFL;
  sigemptyset(&fallback.sa_mask);
  sigaction(signal_number, &fallback, nullptr);
  if (info == nullptr || info->si_code <= 0) {
    raise(signal_number);
  }
}

void HandleCrash(int signal_number, siginfo_t* info, void* context) {
  const int saved_errno = errno;
  if (!g_reported.exchange(true, std::memory_order_acq_rel)) {
    Report(signal_number, info, context);
  }
  if (signal_number > 0 && signal_number < kSignalSlots &&
      g_guest_set[signal_number].load(std::memory_order_acquire) &&
      g_guest_calls.fetch_add(1, std::memory_order_acq_rel) < 3) {
    const struct sigaction guest = g_guest[signal_number];
    if ((guest.sa_flags & SA_SIGINFO) != 0 && guest.sa_sigaction != nullptr) {
      guest.sa_sigaction(signal_number, info, context);
      errno = saved_errno;
      return;
    }
    if (guest.sa_handler == SIG_IGN) {
      errno = saved_errno;
      return;
    }
    if (guest.sa_handler != SIG_DFL && guest.sa_handler != nullptr) {
      guest.sa_handler(signal_number);
      errno = saved_errno;
      return;
    }
  }
  ResetAndResend(signal_number, info);
  errno = saved_errno;
}

_Unwind_Reason_Code IgnoreFrame(_Unwind_Context*, void*) {
  return _URC_END_OF_STACK;
}

}

bool CrashReporterOwns(int signal_number) {
  if (!g_installed.load(std::memory_order_acquire)) {
    return false;
  }
  for (const int owned : kSignals) {
    if (owned == signal_number) {
      return true;
    }
  }
  return false;
}

int SetGuestCrashAction(int signal_number, const struct sigaction* action,
                        struct sigaction* old_action) {
  if (!CrashReporterOwns(signal_number)) {
    errno = EINVAL;
    return -1;
  }
  if (old_action != nullptr) {
    if (g_guest_set[signal_number].load(std::memory_order_acquire)) {
      *old_action = g_guest[signal_number];
    } else {
      *old_action = {};
      old_action->sa_handler = SIG_DFL;
    }
  }
  if (action != nullptr) {
    const bool handles = (action->sa_flags & SA_SIGINFO) != 0
                             ? action->sa_sigaction != nullptr
                             : action->sa_handler != SIG_DFL &&
                                   action->sa_handler != SIG_IGN;
    if (handles) {
      Line line;
      line.Text("guest installed a ").Text(SignalName(signal_number));
      line.Text(" handler; it runs after the report").Write();
    }
    g_guest_set[signal_number].store(false, std::memory_order_release);
    g_guest[signal_number] = *action;
    g_guest_set[signal_number].store(true, std::memory_order_release);
  }
  return 0;
}

bool InstallCrashReporter() {
  if (g_installed.load(std::memory_order_acquire)) {
    return true;
  }
  _Unwind_Backtrace(IgnoreFrame, nullptr);
  stack_t alt_stack{};
  alt_stack.ss_sp = g_alt_stack;
  alt_stack.ss_size = sizeof(g_alt_stack);
  sigaltstack(&alt_stack, nullptr);
  for (const int signal_number : kSignals) {
    struct sigaction action {};
    action.sa_sigaction = HandleCrash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    struct sigaction previous {};
    if (sigaction(signal_number, &action, &previous) != 0) {
      return false;
    }
    const bool chained = (previous.sa_flags & SA_SIGINFO) != 0
                             ? previous.sa_sigaction != nullptr
                             : previous.sa_handler != SIG_DFL;
    if (chained) {
      g_guest[signal_number] = previous;
      g_guest_set[signal_number].store(true, std::memory_order_release);
    }
  }
  g_installed.store(true, std::memory_order_release);
  return true;
}

}
