#include "runtime/roblox_cookie_jar.h"

#include <gtest/gtest.h>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::int64_t kNow = 1800000000;
constexpr char kApis[] = "https://apis.roblox.com/account-switcher/v1/switch";

TEST(RobloxCookieJarTest, KeepsCookieSetForRobloxDomain) {
  RobloxCookieJar jar;
  EXPECT_TRUE(jar.Apply(kApis, {"RBXSessionTracker=sessionid=abc; domain=.roblox.com; path=/"},
                        kNow));
  EXPECT_EQ(jar.StartupHeader(kNow), "RBXSessionTracker=sessionid=abc");
}

TEST(RobloxCookieJarTest, NeverStoresRoblosecurity) {
  RobloxCookieJar jar;
  EXPECT_FALSE(jar.Apply(
      kApis, {".ROBLOSECURITY=_|secret; domain=.roblox.com; path=/; secure; HttpOnly"},
      kNow));
  EXPECT_TRUE(jar.cookies().empty());
}

TEST(RobloxCookieJarTest, IgnoresOtherSites) {
  RobloxCookieJar jar;
  EXPECT_FALSE(jar.Apply("https://example.com/", {"a=b; path=/"}, kNow));
  EXPECT_FALSE(
      jar.Apply(kApis, {"a=b; domain=.example.com; path=/"}, kNow));
  EXPECT_FALSE(jar.Apply("http://www.roblox.com/", {"a=b; path=/"}, kNow));
  EXPECT_TRUE(jar.cookies().empty());
}

TEST(RobloxCookieJarTest, HostOnlyAndPathScopedCookiesAreRestored) {
  RobloxCookieJar jar;
  ASSERT_TRUE(jar.Apply(kApis, {"apisonly=1", "scoped=1; domain=.roblox.com; path=/games"},
                        kNow));
  ASSERT_EQ(jar.cookies().size(), 2u);
  EXPECT_TRUE(jar.cookies()[0].host_only);
  EXPECT_EQ(jar.cookies()[0].domain, "apis.roblox.com");
  EXPECT_EQ(jar.cookies()[0].path, "/account-switcher/v1");
  EXPECT_EQ(jar.StartupHeader(kNow), "apisonly=1; scoped=1");
}

TEST(RobloxCookieJarTest, StartupHeaderKeepsNewestOfSameName) {
  RobloxCookieJar jar;
  ASSERT_TRUE(jar.Apply(kApis, {"a=1; domain=.roblox.com; path=/", "b=1"}, kNow));
  ASSERT_TRUE(jar.Apply(kApis, {"a=2"}, kNow));
  EXPECT_EQ(jar.cookies().size(), 3u);
  EXPECT_EQ(jar.StartupHeader(kNow), "b=1; a=2");
  ASSERT_TRUE(jar.Apply(kApis, {"a=3; domain=.roblox.com; path=/"}, kNow));
  EXPECT_EQ(jar.StartupHeader(kNow), "b=1; a=3");
}

TEST(RobloxCookieJarTest, SameNameDomainAndPathReplaces) {
  RobloxCookieJar jar;
  ASSERT_TRUE(jar.Apply(kApis, {"a=1; domain=.roblox.com; path=/"}, kNow));
  ASSERT_TRUE(jar.Apply(kApis, {"a=2; domain=.roblox.com; path=/"}, kNow));
  EXPECT_EQ(jar.cookies().size(), 1u);
  EXPECT_EQ(jar.StartupHeader(kNow), "a=2");
}

TEST(RobloxCookieJarTest, MaxAgeZeroDeletes) {
  RobloxCookieJar jar;
  ASSERT_TRUE(jar.Apply(kApis, {"a=1; domain=.roblox.com; path=/"}, kNow));
  EXPECT_TRUE(jar.Apply(kApis, {"a=; domain=.roblox.com; path=/; max-age=0"}, kNow));
  EXPECT_TRUE(jar.cookies().empty());
}

TEST(RobloxCookieJarTest, PastExpiresDeletes) {
  RobloxCookieJar jar;
  ASSERT_TRUE(jar.Apply(kApis, {"a=1; domain=.roblox.com; path=/"}, kNow));
  EXPECT_TRUE(jar.Apply(
      kApis, {"a=; domain=.roblox.com; path=/; expires=Thu, 01-Jan-1970 00:00:00 GMT"},
      kNow));
  EXPECT_TRUE(jar.cookies().empty());
}

TEST(RobloxCookieJarTest, ExpiresAndMaxAgeSetExpiry) {
  RobloxCookieJar jar;
  ASSERT_TRUE(jar.Apply(
      kApis,
      {"dated=1; domain=.roblox.com; path=/; expires=Sat, 15 Jan 2028 08:00:00 GMT",
       "aged=1; domain=.roblox.com; path=/; Max-Age=60"},
      kNow));
  ASSERT_EQ(jar.cookies().size(), 2u);
  EXPECT_EQ(jar.cookies()[0].expires, 1831536000);
  EXPECT_EQ(jar.cookies()[1].expires, kNow + 60);
  EXPECT_EQ(jar.StartupHeader(kNow + 61), "dated=1");
}

TEST(RobloxCookieJarTest, RejectsOversizedCookiesAndCapsCount) {
  RobloxCookieJar jar;
  EXPECT_FALSE(jar.Apply(kApis, {"big=" + std::string(4093, 'x')}, kNow));
  EXPECT_TRUE(jar.Apply(kApis, {"big=" + std::string(4092, 'x')}, kNow));
  std::vector<std::string> many;
  for (int index = 0; index < 200; ++index) {
    many.push_back("c" + std::to_string(index) + "=1");
  }
  EXPECT_TRUE(jar.Apply(kApis, many, kNow));
  EXPECT_EQ(jar.cookies().size(), 180u);
  EXPECT_LT(jar.Serialize().size(), 1024u * 1024u);
}

TEST(RobloxCookieJarTest, SerializeAndParseRoundTrip) {
  RobloxCookieJar jar;
  ASSERT_TRUE(jar.Apply(
      kApis,
      {"a=1; domain=.roblox.com; path=/; Secure; Max-Age=100", "b=2; path=/v1"},
      kNow));
  const RobloxCookieJar loaded = RobloxCookieJar::Parse(jar.Serialize());
  ASSERT_EQ(loaded.cookies().size(), 2u);
  EXPECT_EQ(loaded.StartupHeader(kNow), "a=1; b=2");
  EXPECT_EQ(loaded.cookies()[0].expires, kNow + 100);
  EXPECT_TRUE(loaded.cookies()[0].secure);
}

TEST(RobloxCookieJarTest, ParseRejectsMalformedInput) {
  EXPECT_TRUE(RobloxCookieJar::Parse("").cookies().empty());
  EXPECT_TRUE(RobloxCookieJar::Parse("not json").cookies().empty());
  EXPECT_TRUE(RobloxCookieJar::Parse(R"({"cookies":[{"name":".ROBLOSECURITY",)"
                                     R"("value":"x","domain":"roblox.com","path":"/"}]})")
                  .cookies()
                  .empty());
  EXPECT_TRUE(RobloxCookieJar::Parse(R"({"cookies":{}})").cookies().empty());
  EXPECT_TRUE(RobloxCookieJar::Parse(R"([1,2])").cookies().empty());
  const RobloxCookieJar mixed = RobloxCookieJar::Parse(
      R"({"cookies":[{"name":5},"x",)"
      R"({"name":"a","value":"1","domain":"roblox.com","path":"/","expires":"soon"},)"
      R"({"name":"b","value":"1","domain":"roblox.com","path":"/","secure":"yes"},)"
      R"({"name":"c","value":"1","domain":"roblox.com","path":"/","expires":-5},)"
      R"({"name":"ok","value":"1","domain":"roblox.com","path":"/"}]})");
  ASSERT_EQ(mixed.cookies().size(), 1u);
  EXPECT_EQ(mixed.cookies()[0].name, "ok");
}

TEST(RobloxCookieJarTest, CombinesCredentialAndJar) {
  EXPECT_EQ(CombineRobloxStartupCookies(".ROBLOSECURITY=x", "a=1; b=2"),
            ".ROBLOSECURITY=x; a=1; b=2");
  EXPECT_EQ(CombineRobloxStartupCookies(".ROBLOSECURITY=x", ""), ".ROBLOSECURITY=x");
  EXPECT_EQ(CombineRobloxStartupCookies("", "a=1"), "a=1");
}

}
}
}
