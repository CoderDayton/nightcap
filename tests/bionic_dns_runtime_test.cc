#include "compat/bionic_dns_runtime.h"

#include <gtest/gtest.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <memory>

namespace mocktail {
namespace compat {
namespace {

using AddressInfoOwner =
    std::unique_ptr<BionicAddressInfo, decltype(&BionicFreeAddressInfo)>;

TEST(BionicDnsRuntimeTest, BlocksOnlyCrashUploadInfrastructure) {
  EXPECT_TRUE(IsBlockedCrashReportUploadHost("upload.crashes.rbxinfra.com"));
  EXPECT_TRUE(IsBlockedCrashReportUploadHost("UPLOAD.CRASHES.RBXINFRA.COM."));
  EXPECT_TRUE(IsBlockedCrashReportUploadHost("uploads.backtrace.rbx.com"));
  EXPECT_TRUE(IsBlockedCrashReportUploadHost("tenant.sp.backtrace.io"));
  EXPECT_TRUE(IsBlockedCrashReportUploadHost("backtrace.io"));

  EXPECT_FALSE(IsBlockedCrashReportUploadHost("auth.roblox.com"));
  EXPECT_FALSE(IsBlockedCrashReportUploadHost("assetdelivery.roblox.com"));
  EXPECT_FALSE(IsBlockedCrashReportUploadHost("apis.roblox.com"));
  EXPECT_FALSE(IsBlockedCrashReportUploadHost("notcrashes.rbxinfra.com"));
  EXPECT_FALSE(IsBlockedCrashReportUploadHost(
      "upload.crashes.rbxinfra.com.example.org"));
}

TEST(BionicDnsRuntimeTest, RejectsCrashHostBeforeAddressResolution) {
  BionicAddressInfo* result =
      reinterpret_cast<BionicAddressInfo*>(uintptr_t{1});

  EXPECT_EQ(BionicGetAddressInfo("upload.crashes.rbxinfra.com", "443", nullptr,
                                 &result),
            kBionicAddressInfoNameNotFound);
  EXPECT_EQ(result, nullptr);

  h_errno = 0;
  EXPECT_EQ(BionicGetHostByName("uploads.backtrace.rbx.com"), nullptr);
  EXPECT_EQ(h_errno, HOST_NOT_FOUND);
}

TEST(BionicDnsRuntimeTest, PreservesOrdinaryLocalDnsAndBionicLayout) {
  BionicAddressInfo hints;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  BionicAddressInfo* result = nullptr;

  ASSERT_EQ(BionicGetAddressInfo("localhost", "80", &hints, &result), 0);
  ASSERT_NE(result, nullptr);
  EXPECT_NE(result->ai_addr, nullptr);
  EXPECT_GT(result->ai_addrlen, 0U);
  EXPECT_EQ(result->ai_socktype, SOCK_STREAM);
  BionicFreeAddressInfo(result);

  EXPECT_NE(BionicGetHostByName("localhost"), nullptr);
}

TEST(BionicDnsRuntimeTest, RejectsMissingResultStorage) {
  EXPECT_NE(BionicGetAddressInfo("localhost", "80", nullptr, nullptr), 0);
  BionicFreeAddressInfo(nullptr);
}

TEST(BionicDnsRuntimeTest, GuestNumericServiceRejectsNamesAndAcceptsNumbers) {
  BionicAddressInfo hints;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = 0x00000008 | 0x00000004;
  BionicAddressInfo* result = nullptr;
  EXPECT_EQ(BionicGetAddressInfo("127.0.0.1", "http", &hints, &result),
            kBionicAddressInfoNameNotFound);
  EXPECT_EQ(result, nullptr);
  BionicFreeAddressInfo(result);

  result = nullptr;
  const int status = BionicGetAddressInfo("127.0.0.1", "80", &hints, &result);
  const AddressInfoOwner owner(result, &BionicFreeAddressInfo);
  ASSERT_EQ(status, 0);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->ai_flags, 0x0000000c);
  ASSERT_NE(result->ai_addr, nullptr);
  ASSERT_EQ(result->ai_family, AF_INET);
  EXPECT_EQ(
      ntohs(reinterpret_cast<const sockaddr_in*>(result->ai_addr)->sin_port),
      80);
}

TEST(BionicDnsRuntimeTest, GuestAddressConfigurationAllowsNamedService) {
  addrinfo host_hints{};
  host_hints.ai_family = AF_INET;
  host_hints.ai_socktype = SOCK_STREAM;
  host_hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICHOST;
  addrinfo* host_result = nullptr;
  const int host_status =
      ::getaddrinfo("127.0.0.1", "http", &host_hints, &host_result);
  ASSERT_EQ(host_status, 0)
      << "Host baseline requires a configured IPv4 interface and http service: "
      << ::gai_strerror(host_status);
  ASSERT_NE(host_result, nullptr);
  ::freeaddrinfo(host_result);

  BionicAddressInfo hints;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = 0x00000400 | 0x00000004;
  BionicAddressInfo* result = nullptr;
  const int status = BionicGetAddressInfo("127.0.0.1", "http", &hints, &result);
  const AddressInfoOwner owner(result, &BionicFreeAddressInfo);
  ASSERT_EQ(status, 0);
  ASSERT_NE(result, nullptr);
  for (const BionicAddressInfo* current = result; current != nullptr;
       current = current->ai_next) {
    EXPECT_EQ(current->ai_flags, 0x00000404);
    EXPECT_EQ(current->ai_flags & 0x00000020, 0);
    ASSERT_NE(current->ai_addr, nullptr);
    ASSERT_EQ(current->ai_family, AF_INET);
    EXPECT_EQ(
        ntohs(reinterpret_cast<const sockaddr_in*>(current->ai_addr)->sin_port),
        80);
  }
}

TEST(BionicDnsRuntimeTest, SupportedGuestFlagsRoundTrip) {
  // Android values are intentional; host AI_* constants are not guest hints.
  for (const int flags : {0x00000001, 0x00000002, 0x00000004, 0x00000008,
                         0x00000400, 0x0000000f, 0x00000407, 0x0000040f}) {
    SCOPED_TRACE(flags);
    BionicAddressInfo hints;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = flags;
    BionicAddressInfo* result = nullptr;
    const int status = BionicGetAddressInfo("127.0.0.1", "80", &hints, &result);
    const AddressInfoOwner owner(result, &BionicFreeAddressInfo);
    ASSERT_EQ(status, 0);
    ASSERT_NE(result, nullptr);
    for (const BionicAddressInfo* current = result; current != nullptr;
         current = current->ai_next) {
      EXPECT_EQ(current->ai_flags, flags);
    }
    if ((flags & 0x00000002) != 0) {
      ASSERT_NE(result->ai_canonname, nullptr);
      EXPECT_STREQ(result->ai_canonname, "127.0.0.1");
    }
  }
}

TEST(BionicDnsRuntimeTest, RejectsUnsupportedGuestFlagsBeforeHostResolution) {
  for (const int flags : {0x00000010, 0x00000020, 0x00000040, 0x00000080,
                         0x00000100, 0x00000200, 0x00000800, 0x00001000,
                         0x00000424, -1}) {
    SCOPED_TRACE(flags);
    BionicAddressInfo hints;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = flags;
    BionicAddressInfo* result =
        reinterpret_cast<BionicAddressInfo*>(uintptr_t{1});
    const int status = BionicGetAddressInfo("127.0.0.1", "80", &hints, &result);
    EXPECT_EQ(status, 3);
    EXPECT_EQ(result, nullptr);
    BionicFreeAddressInfo(result);
  }
}

TEST(BionicDnsRuntimeTest, NullHintsPreserveLocalResolutionAndGuestFlags) {
  BionicAddressInfo* result = nullptr;
  const int status = BionicGetAddressInfo("127.0.0.1", "80", nullptr, &result);
  const AddressInfoOwner owner(result, &BionicFreeAddressInfo);
  ASSERT_EQ(status, 0);
  ASSERT_NE(result, nullptr);
  for (const BionicAddressInfo* current = result; current != nullptr;
       current = current->ai_next) {
    EXPECT_EQ(current->ai_flags & ~0x0000040f, 0);
  }
}

}  // namespace
}  // namespace compat
}  // namespace mocktail
