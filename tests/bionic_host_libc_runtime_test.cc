#include "compat/bionic_host_libc_runtime.h"
#include "compat/bionic_stdio_runtime.h"

#include <gtest/gtest.h>
#include <limits.h>
#include <locale.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <thread>

#include "libc_shim/libc_shim.h"

namespace {

constexpr size_t AlignControlMessage(size_t size) {
  return (size + sizeof(long) - 1U) & ~(sizeof(long) - 1U);
}

constexpr size_t ControlMessageLength(size_t payload_size) {
  return AlignControlMessage(
             sizeof(mocktail::compat::BionicControlMessageHeader)) +
         payload_size;
}

constexpr size_t ControlMessageSpace(size_t payload_size) {
  return AlignControlMessage(
             sizeof(mocktail::compat::BionicControlMessageHeader)) +
         AlignControlMessage(payload_size);
}

struct DestructorProbe {
  std::atomic<int> calls{0};
};

void RecordThreadDestructor(void *argument) {
  auto *probe = static_cast<DestructorProbe *>(argument);
  probe->calls.fetch_add(1, std::memory_order_relaxed);
}

TEST(BionicHostLibcRuntimeTest, WalksValidControlMessages) {
  constexpr size_t kPayloadSize = sizeof(int);
  using Header = mocktail::compat::BionicControlMessageHeader;
  alignas(Header)
      std::array<unsigned char, 2 * ControlMessageSpace(kPayloadSize)>
          control{};
  mocktail::compat::BionicMessageHeader message{};
  message.msg_control = control.data();
  message.msg_controllen = control.size();

  auto *first = reinterpret_cast<Header *>(control.data());
  first->cmsg_len = ControlMessageLength(kPayloadSize);
  auto *second = reinterpret_cast<Header *>(control.data() +
                                            ControlMessageSpace(kPayloadSize));
  second->cmsg_len = ControlMessageLength(kPayloadSize);

  EXPECT_EQ(mocktail_bionic_cmsg_nxthdr(&message, first), second);
  EXPECT_EQ(mocktail_bionic_cmsg_nxthdr(&message, second), nullptr);
}

TEST(BionicHostLibcRuntimeTest, RejectsMalformedControlMessages) {
  using Header = mocktail::compat::BionicControlMessageHeader;
  alignas(Header) std::array<unsigned char, 2 * sizeof(Header)> control{};
  mocktail::compat::BionicMessageHeader message{};
  message.msg_control = control.data();
  message.msg_controllen = control.size();
  auto *header = reinterpret_cast<Header *>(control.data());

  header->cmsg_len = sizeof(Header) - 1;
  EXPECT_EQ(mocktail_bionic_cmsg_nxthdr(&message, header), nullptr);

  header->cmsg_len = std::numeric_limits<size_t>::max();
  EXPECT_EQ(mocktail_bionic_cmsg_nxthdr(&message, header), nullptr);

  header->cmsg_len = sizeof(Header);
  message.msg_controllen = sizeof(Header);
  EXPECT_EQ(mocktail_bionic_cmsg_nxthdr(&message, header), nullptr);
}

TEST(BionicHostLibcRuntimeTest, RunsThreadDestructorAtThreadExit) {
  DestructorProbe probe;
  int result = -1;
  std::thread worker([&probe, &result]() {
    result = mocktail_bionic_cxa_thread_atexit_impl(RecordThreadDestructor,
                                                    &probe, &probe);
    EXPECT_EQ(probe.calls.load(std::memory_order_relaxed), 0);
  });
  worker.join();

  EXPECT_EQ(result, 0);
  EXPECT_EQ(probe.calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(mocktail_bionic_cxa_thread_atexit_impl(nullptr, nullptr, nullptr),
            EINVAL);
}

TEST(BionicHostLibcRuntimeTest, FillsEveryRequestedRandomByte) {
  std::array<unsigned char, 64> first;
  std::array<unsigned char, 64> second;
  first.fill(0xa5);
  second.fill(0xa5);

  mocktail_bionic_arc4random_buf(first.data(), first.size());
  mocktail_bionic_arc4random_buf(second.data(), second.size());
  mocktail_bionic_arc4random_buf(nullptr, 0);

  EXPECT_FALSE(std::all_of(first.begin(), first.end(),
                           [](unsigned char value) { return value == 0xa5; }));
  EXPECT_NE(first, second);
}

TEST(BionicHostLibcRuntimeTest, ReturnsAbiSizedAllocatorSnapshot) {
  static_assert(sizeof(mocktail::compat::BionicMallinfoSnapshot) == 80);
  const auto snapshot = mocktail_bionic_mallinfo();
  if (!mocktail::compat::BionicMallinfoHasHostTelemetry()) {
    EXPECT_EQ(snapshot.arena, 0U);
    EXPECT_EQ(snapshot.ordblks, 0U);
    EXPECT_EQ(snapshot.smblks, 0U);
    EXPECT_EQ(snapshot.hblks, 0U);
    EXPECT_EQ(snapshot.hblkhd, 0U);
    EXPECT_EQ(snapshot.usmblks, 0U);
    EXPECT_EQ(snapshot.fsmblks, 0U);
    EXPECT_EQ(snapshot.uordblks, 0U);
    EXPECT_EQ(snapshot.fordblks, 0U);
    EXPECT_EQ(snapshot.keepcost, 0U);
  }
}

TEST(BionicHostLibcRuntimeTest, PreservesValidSysInfoCounters) {
  struct sysinfo info{};
  info.totalram = 8UL * 1024UL * 1024UL * 1024UL;
  info.freeram = 2UL * 1024UL * 1024UL * 1024UL;
  info.totalswap = 4UL * 1024UL * 1024UL * 1024UL;
  info.freeswap = 1UL * 1024UL * 1024UL * 1024UL;

  mocktail::compat::NormalizeBionicSysInfo(&info);

  EXPECT_EQ(info.totalram, 8UL * 1024UL * 1024UL * 1024UL);
  EXPECT_EQ(info.freeram, 2UL * 1024UL * 1024UL * 1024UL);
  EXPECT_EQ(info.totalswap, 4UL * 1024UL * 1024UL * 1024UL);
  EXPECT_EQ(info.freeswap, 1UL * 1024UL * 1024UL * 1024UL);
}

TEST(BionicHostLibcRuntimeTest, ClearsLinuxulatorUnderflowedSwapCounters) {
  struct sysinfo info{};
  info.totalram = 8UL * 1024UL * 1024UL * 1024UL;
  info.freeram = 2UL * 1024UL * 1024UL * 1024UL;
  info.totalswap = std::numeric_limits<unsigned long>::max() - 40959UL;
  info.freeswap = std::numeric_limits<unsigned long>::max() - 11550719UL;

  mocktail::compat::NormalizeBionicSysInfo(&info);

  EXPECT_EQ(info.totalram, 8UL * 1024UL * 1024UL * 1024UL);
  EXPECT_EQ(info.freeram, 2UL * 1024UL * 1024UL * 1024UL);
  EXPECT_EQ(info.totalswap, 0UL);
  EXPECT_EQ(info.freeswap, 0UL);
}

TEST(BionicHostLibcRuntimeTest, CallsHostSysInfoThroughBionicAdapter) {
  struct sysinfo info{};
  ASSERT_EQ(mocktail_bionic_sysinfo(&info), 0);
  EXPECT_GT(info.totalram, 0UL);
  EXPECT_LE(info.freeswap, info.totalswap);
}

TEST(BionicHostLibcRuntimeTest, NormalizesContradictoryLinuxulatorUname) {
  struct utsname name{};
  std::snprintf(name.sysname, sizeof(name.sysname), "Linux");
  std::snprintf(name.release, sizeof(name.release), "5.15.0");
  std::snprintf(name.version, sizeof(name.version),
                "FreeBSD 15.1-RELEASE-p2 GENERIC");

  EXPECT_TRUE(mocktail::compat::NormalizeLinuxulatorUnameVersion(
      &name, "#4 SMP PREEMPT_DYNAMIC"));
  EXPECT_STREQ(name.sysname, "Linux");
  EXPECT_STREQ(name.release, "5.15.0");
  EXPECT_STREQ(name.version, "#4 SMP PREEMPT_DYNAMIC");
}

TEST(BionicHostLibcRuntimeTest, PreservesNativeLinuxUname) {
  struct utsname name{};
  std::snprintf(name.sysname, sizeof(name.sysname), "Linux");
  std::snprintf(name.version, sizeof(name.version), "#1 SMP PREEMPT_DYNAMIC");

  EXPECT_FALSE(mocktail::compat::NormalizeLinuxulatorUnameVersion(
      &name, "#4 SMP PREEMPT_DYNAMIC"));
  EXPECT_STREQ(name.version, "#1 SMP PREEMPT_DYNAMIC");

  ASSERT_EQ(mocktail_bionic_uname(&name), 0);
  EXPECT_STREQ(name.sysname, "Linux");
}

TEST(BionicHostLibcRuntimeTest, HidesPrivilegedHostIdentityFromAndroidGuest) {
  EXPECT_EQ(mocktail::compat::NormalizeBionicApplicationUid(0), 1000U);
  EXPECT_EQ(mocktail::compat::NormalizeBionicApplicationUid(1000), 1000U);
  EXPECT_EQ(mocktail::compat::NormalizeBionicApplicationUid(12345), 12345U);
  EXPECT_EQ(mocktail_bionic_getuid(),
            mocktail::compat::NormalizeBionicApplicationUid(::getuid()));
  EXPECT_EQ(mocktail_bionic_geteuid(),
            mocktail::compat::NormalizeBionicApplicationUid(::geteuid()));
}

TEST(BionicHostLibcRuntimeTest, ParsesAtoiWithBionicOverflowBehavior) {
  EXPECT_EQ(mocktail_bionic_atoi(""), 0);
  EXPECT_EQ(mocktail_bionic_atoi("invalid"), 0);
  EXPECT_EQ(mocktail_bionic_atoi("  -42suffix"), -42);
  EXPECT_EQ(mocktail_bionic_atoi("2147483647"), INT_MAX);
  EXPECT_EQ(mocktail_bionic_atoi("-2147483648"), INT_MIN);
  EXPECT_EQ(mocktail_bionic_atoi("2147483648"), INT_MIN);
  EXPECT_EQ(mocktail_bionic_atoi("4294967295"), -1);
  EXPECT_EQ(mocktail_bionic_atoi("9999999999"), 1410065407);

  errno = 0;
  EXPECT_EQ(mocktail_bionic_atoi("9223372036854775808"), -1);
  EXPECT_EQ(errno, ERANGE);
}

TEST(BionicHostLibcRuntimeTest, UsesBionicPosixStrErrorContract) {
  std::array<char, 128> buffer{};

  errno = EDOM;
  EXPECT_EQ(mocktail_bionic_strerror_r(EINVAL, buffer.data(), buffer.size()),
            0);
  EXPECT_NE(std::strstr(buffer.data(), "Invalid"), nullptr);
  EXPECT_EQ(errno, EDOM);

  EXPECT_EQ(mocktail_bionic_strerror_r(0, buffer.data(), buffer.size()), 0);
  EXPECT_STREQ(buffer.data(), std::strerror(0));
  EXPECT_EQ(errno, EDOM);

  for (const int error_number : {-1, 41, 58, 134, 4567, INT_MIN, INT_MAX}) {
    errno = EDOM;
    ASSERT_EQ(mocktail_bionic_strerror_r(error_number, buffer.data(),
                                       buffer.size()), 0);
    EXPECT_EQ(std::string(buffer.data()),
              "Unknown error " + std::to_string(error_number));
    EXPECT_EQ(errno, EDOM);
  }

  std::array<char, 2> truncated{};
  errno = 0;
  EXPECT_EQ(
      mocktail_bionic_strerror_r(4567, truncated.data(), truncated.size()), -1);
  EXPECT_EQ(truncated[1], '\0');
  EXPECT_EQ(errno, ERANGE);

  errno = 0;
  EXPECT_EQ(mocktail_bionic_strerror_r(EINVAL, nullptr, 0), -1);
  EXPECT_EQ(errno, ERANGE);
}

TEST(BionicHostLibcRuntimeTest, ParsesIntegersInScopedCLocale) {
  const locale_t original_locale = uselocale(nullptr);
  auto guest_locale =
      reinterpret_cast<mocktail::compat::BionicLocale>(uintptr_t{1});

  char *signed_end = nullptr;
  errno = EDOM;
  EXPECT_EQ(
      mocktail_bionic_strtoll_l("-0x7f-rest", &signed_end, 0, guest_locale),
      -127);
  EXPECT_STREQ(signed_end, "-rest");
  EXPECT_EQ(errno, EDOM);

  char *unsigned_end = nullptr;
  errno = 0;
  EXPECT_EQ(mocktail_bionic_strtoull_l("18446744073709551616!", &unsigned_end,
                                       10, guest_locale),
            ULLONG_MAX);
  EXPECT_EQ(errno, ERANGE);
  EXPECT_STREQ(unsigned_end, "!");
  EXPECT_EQ(uselocale(nullptr), original_locale);
}

int CallVsscanf(const char *text, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  const int result = mocktail_bionic_vsscanf(text, format, arguments);
  va_end(arguments);
  return result;
}

int CallVsnprintfChk(char *buffer, size_t size, size_t buffer_size,
                     const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  const int result = mocktail_bionic___vsnprintf_chk(
      buffer, size, 0, buffer_size, format, arguments);
  va_end(arguments);
  return result;
}

int CallVsprintfChk(char *buffer, size_t buffer_size, const char *format,
                    ...) {
  va_list arguments;
  va_start(arguments, format);
  const int result =
      mocktail_bionic___vsprintf_chk(buffer, 0, buffer_size, format, arguments);
  va_end(arguments);
  return result;
}

int CallVasprintf(char **result, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  const int length = mocktail_bionic_vasprintf(result, format, arguments);
  va_end(arguments);
  return length;
}

// Android reads and writes numbers with a dot whatever the language. Under a
// host comma locale glibc reads "0.2" as 0, which cut camera sensitivity to
// whole numbers (#79). LOCPATH can point at a de_DE built with localedef.
class CommaLocaleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    comma_ = newlocale(LC_ALL_MASK, "de_DE.UTF-8", nullptr);
    if (comma_ == nullptr) {
      GTEST_SKIP() << "de_DE.UTF-8 is not installed";
    }
    previous_ = uselocale(comma_);
    EXPECT_EQ(strtod("0.2", nullptr), 0.0);
  }

  void TearDown() override {
    if (comma_ != nullptr) {
      EXPECT_EQ(uselocale(nullptr), comma_);
      uselocale(previous_);
      freelocale(comma_);
    }
  }

  locale_t comma_ = nullptr;
  locale_t previous_ = nullptr;
};

TEST_F(CommaLocaleTest, WritesDecimalsWithADot) {
  std::array<char, 32> buffer{};
  EXPECT_EQ(mocktail_bionic_snprintf(buffer.data(), buffer.size(), "%.1f",
                                     0.2),
            3);
  EXPECT_STREQ(buffer.data(), "0.2");
  EXPECT_EQ(CallVsnprintfChk(buffer.data(), buffer.size(), buffer.size(),
                             "%.1f", 2.3),
            3);
  EXPECT_STREQ(buffer.data(), "2.3");
  EXPECT_EQ(CallVsprintfChk(buffer.data(), buffer.size(), "%.1f", 1.6), 3);
  EXPECT_STREQ(buffer.data(), "1.6");
  char *allocated = nullptr;
  ASSERT_EQ(CallVasprintf(&allocated, "%.3f", 0.001), 5);
  EXPECT_STREQ(allocated, "0.001");
  std::free(allocated);
  EXPECT_STREQ(mocktail_bionic_localeconv()->decimal_point, ".");
}

TEST_F(CommaLocaleTest, StreamsRoundTripDecimals) {
  FILE *file = std::tmpfile();
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(mocktail_fprintf(file, "%.1f", 0.2), 3);
  std::rewind(file);
  float value = 0;
  EXPECT_EQ(mocktail_fscanf(file, "%f", &value), 1);
  EXPECT_FLOAT_EQ(value, 0.2f);
  std::fclose(file);
}

TEST_F(CommaLocaleTest, NamesDaysInEnglish) {
  std::tm thursday{};
  thursday.tm_year = 126;
  thursday.tm_mon = 10;
  thursday.tm_mday = 5;
  thursday.tm_wday = 4;
  std::array<char, 32> buffer{};
  ASSERT_GT(mocktail_bionic_strftime(buffer.data(), buffer.size(), "%A %b",
                                     &thursday),
            0u);
  EXPECT_STREQ(buffer.data(), "Thursday Nov");
  buffer.fill('\0');
  ASSERT_GT(mocktail_bionic_strftime_l(buffer.data(), buffer.size(), "%A",
                                       &thursday, comma_),
            0u);
  EXPECT_STREQ(buffer.data(), "Thursday");
}

TEST_F(CommaLocaleTest, PrintfWritesADot) {
  FILE *capture = std::tmpfile();
  ASSERT_NE(capture, nullptr);
  std::fflush(stdout);
  const int saved_stdout = dup(STDOUT_FILENO);
  ASSERT_GE(saved_stdout, 0);
  dup2(fileno(capture), STDOUT_FILENO);
  const int written = mocktail_bionic_printf("%.1f", 0.2);
  std::fflush(stdout);
  dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
  EXPECT_EQ(written, 3);
  std::rewind(capture);
  std::array<char, 8> text{};
  ASSERT_NE(std::fgets(text.data(), text.size(), capture), nullptr);
  EXPECT_STREQ(text.data(), "0.2");
  std::fclose(capture);
}

// A guest asking for the user's locale gets C, as on Android, even when the
// environment names a comma locale.
TEST_F(CommaLocaleTest, TheUsersLocaleIsC) {
  const char *saved = std::getenv("LC_ALL");
  const std::string restore = saved != nullptr ? saved : "";
  setenv("LC_ALL", "de_DE.UTF-8", 1);
  const locale_t guest = mocktail_bionic_newlocale(LC_ALL_MASK, "", nullptr);
  saved != nullptr ? setenv("LC_ALL", restore.c_str(), 1)
                   : unsetenv("LC_ALL");
  ASSERT_NE(guest, nullptr);
  uselocale(guest);
  std::array<char, 32> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%.1f", 0.2);
  EXPECT_STREQ(buffer.data(), "0.2");
  uselocale(comma_);
  freelocale(guest);
}

TEST(BionicHostLibcRuntimeTest, NewlocaleFollowsBionic) {
  errno = 0;
  EXPECT_EQ(mocktail_bionic_newlocale(LC_ALL_MASK, "de_DE.UTF-8", nullptr),
            nullptr);
  EXPECT_EQ(errno, ENOENT);
  errno = 0;
  EXPECT_EQ(mocktail_bionic_newlocale(LC_ALL_MASK, nullptr, nullptr), nullptr);
  EXPECT_EQ(errno, EINVAL);
  errno = 0;
  EXPECT_EQ(mocktail_bionic_newlocale(1 << 6, "C", nullptr), nullptr);
  EXPECT_EQ(errno, EINVAL);

  const locale_t original = uselocale(nullptr);
  const locale_t base = newlocale(LC_ALL_MASK, "C", nullptr);
  ASSERT_NE(base, nullptr);
  const locale_t utf8 = mocktail_bionic_newlocale(LC_NUMERIC_MASK, "", base);
  ASSERT_NE(utf8, nullptr);
  EXPECT_NE(utf8, base);
  uselocale(utf8);
  EXPECT_GT(MB_CUR_MAX, 1u);
  const locale_t plain = mocktail_bionic_newlocale(LC_ALL_MASK, "POSIX", nullptr);
  ASSERT_NE(plain, nullptr);
  uselocale(plain);
  EXPECT_EQ(MB_CUR_MAX, 1u);
  uselocale(original);
  freelocale(plain);
  freelocale(utf8);
  freelocale(base);
}

TEST(BionicHostLibcRuntimeDeathTest, FortifiedFormattingStopsAnOverflow) {
  std::array<char, 4> buffer{};
  EXPECT_DEATH(CallVsprintfChk(buffer.data(), buffer.size(), "%s", "toolong"),
               "vsprintf");
  EXPECT_DEATH(CallVsnprintfChk(buffer.data(), 8, buffer.size(), "%d", 1),
               "vsnprintf");
}

TEST_F(CommaLocaleTest, ReadsDecimalsWithADot) {
  char *end = nullptr;
  EXPECT_DOUBLE_EQ(mocktail_bionic_strtod("0.2x", &end), 0.2);
  EXPECT_STREQ(end, "x");
  EXPECT_FLOAT_EQ(mocktail_bionic_strtof("2.3", nullptr), 2.3f);
  EXPECT_DOUBLE_EQ(mocktail_bionic_atof("1.6"), 1.6);
  float first = 0;
  float second = 0;
  EXPECT_EQ(mocktail_bionic_sscanf("0.2 1.6", "%f %f", &first, &second), 2);
  EXPECT_FLOAT_EQ(first, 0.2f);
  EXPECT_FLOAT_EQ(second, 1.6f);
  double third = 0;
  EXPECT_EQ(CallVsscanf("0.001", "%lf", &third), 1);
  EXPECT_DOUBLE_EQ(third, 0.001);
}

TEST(BionicHostLibcRuntimeTest, FortifiedReadlinkAcceptsInBoundsRequest) {
  std::array<char, PATH_MAX> executable{};
  const ssize_t length =
      mocktail___readlink_chk("/proc/self/exe", executable.data(),
                              executable.size(), executable.size());
  ASSERT_GT(length, 0);
  EXPECT_LT(static_cast<size_t>(length), executable.size());
}

TEST(BionicHostLibcRuntimeTest, FortifiedReadlinkAbortsOnBufferOverflow) {
  EXPECT_DEATH(
      {
        char buffer[4]{};
        mocktail___readlink_chk("/proc/self/exe", buffer, 5, sizeof(buffer));
      },
      "FORTIFY: readlink");
}

TEST(BionicHostLibcRuntimeTest, FortifiedReadlinkAbortsAboveSsizeLimit) {
  EXPECT_DEATH(
      {
        char buffer{};
        mocktail___readlink_chk("/proc/self/exe", &buffer,
                                static_cast<size_t>(SSIZE_MAX) + size_t{1},
                                std::numeric_limits<size_t>::max());
      },
      "FORTIFY: readlink");
}

} // namespace
