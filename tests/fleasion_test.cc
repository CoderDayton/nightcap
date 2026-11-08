#include "runtime/fleasion.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mocktail::runtime {
namespace {

constexpr char kFlag[] = "FFlagAssetProviderUseAssetDeliveryV3";

class MapEnvironment final : public Environment {
 public:
  explicit MapEnvironment(
      std::unordered_map<std::string, std::string> values = {})
      : values_(std::move(values)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    return found == values_.end() ? std::nullopt
                                  : std::optional<std::string>(found->second);
  }

 private:
  std::unordered_map<std::string, std::string> values_;
};

bool NeedsFreshLaunch(const char* enabled, const char* clear) {
  return WebsiteJoinNeedsFreshLaunch(RuntimeConfig::FromEnvironment(
      MapEnvironment({{"MOCKTAIL_FLEASION_ENABLED", enabled},
                      {"MOCKTAIL_FLEASION_CLEAR_CACHE_ON_LAUNCH", clear}})));
}

TEST(FleasionTest, AWebsiteJoinStartsFreshWhenEveryLaunchClearsTheCache) {
  EXPECT_TRUE(NeedsFreshLaunch("1", "1"));
}

TEST(FleasionTest, AWebsiteJoinGoesToTheOpenNightcapOtherwise) {
  EXPECT_FALSE(NeedsFreshLaunch("1", "0"));
  EXPECT_FALSE(NeedsFreshLaunch("0", "1"));
  EXPECT_FALSE(NeedsFreshLaunch("0", "0"));
}

int LoopbackSocket(bool listening) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  EXPECT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  if (listening) EXPECT_EQ(listen(fd, 1), 0);
  return fd;
}

int BoundPort(int fd) {
  sockaddr_in address{};
  socklen_t length = sizeof(address);
  EXPECT_EQ(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
  return ntohs(address.sin_port);
}

TEST(FleasionTest, ProxyIsReachableWhenSomethingListens) {
  const int fd = LoopbackSocket(true);
  EXPECT_TRUE(FleasionProxyReachable(BoundPort(fd), 500));
  close(fd);
}

TEST(FleasionTest, ProxyIsUnreachableWhenNothingListens) {
  const int fd = LoopbackSocket(false);
  const int port = BoundPort(fd);
  EXPECT_FALSE(FleasionProxyReachable(port, 500));
  close(fd);
}

TEST(FleasionTest, ProxyIsUnreachableOnAPortThatIsOutOfRange) {
  EXPECT_FALSE(FleasionProxyReachable(0, 100));
  EXPECT_FALSE(FleasionProxyReachable(-1, 100));
  EXPECT_FALSE(FleasionProxyReachable(65536, 100));
}

class SoberInstallTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("fleasion_sober_" + std::to_string(getpid()));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_ / "home");
    std::filesystem::create_directories(root_ / "system");
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  void Deploy(const std::filesystem::path& flatpak_root) {
    std::filesystem::create_directories(
        flatpak_root / "app/org.vinegarhq.Sober/current/active");
  }

  std::optional<bool> Installed(MapEnvironment environment = MapEnvironment()) {
    return SoberInstalled(environment, root_ / "home", root_ / "system");
  }

  std::filesystem::path root_;
};

TEST_F(SoberInstallTest, IsMissingWhenNoFlatpakHasIt) {
  EXPECT_EQ(Installed(), std::optional<bool>(false));
}

TEST_F(SoberInstallTest, IsFoundInTheSystemInstallation) {
  Deploy(root_ / "system");
  EXPECT_EQ(Installed(), std::optional<bool>(true));
}

TEST_F(SoberInstallTest, IsFoundInTheUserInstallation) {
  Deploy(root_ / "home/.local/share/flatpak");
  EXPECT_EQ(Installed(), std::optional<bool>(true));
}

TEST_F(SoberInstallTest, IsUnknownInsideNightcapsFlatpak) {
  EXPECT_EQ(Installed(MapEnvironment({{"FLATPAK_ID", "io.github.CoderDayton.nightcap"}})),
            std::nullopt);
}

nlohmann::json Merge(const std::string& base) {
  std::string merged;
  std::string error;
  EXPECT_TRUE(MergeFleasionClientSettingsOverrides(base, &merged, &error))
      << error;
  return nlohmann::json::parse(merged);
}

std::string Value(const nlohmann::json& json, const char* key) {
  return json.value(key, std::string());
}

TEST(FleasionTest, RequestsAssetsFromTheBatchApiFleasionRewrites) {
  EXPECT_EQ(Value(Merge("{}"), kFlag), "False");
}

TEST(FleasionTest, TrustsTheCaBundleThatHoldsFleasionsCertificate) {
  EXPECT_EQ(Value(Merge("{}"), "DFFlagHttpUseEmbeddedCaCerts"), "False");
  EXPECT_EQ(Value(Merge(R"({"DFFlagHttpUseEmbeddedCaCerts":"True"})"),
                  "DFFlagHttpUseEmbeddedCaCerts"),
            "True");
}

TEST(FleasionTest, KeepsOtherOverrides) {
  const nlohmann::json merged = Merge(R"({"FIntTargetRefreshRate":"144"})");
  EXPECT_EQ(Value(merged, "FIntTargetRefreshRate"), "144");
  EXPECT_EQ(Value(merged, kFlag), "False");
}

TEST(FleasionTest, KeepsAnExplicitValue) {
  EXPECT_EQ(
      Value(Merge(R"({"FFlagAssetProviderUseAssetDeliveryV3":"True"})"), kFlag),
      "True");
}

TEST(FleasionTest, RejectsOverridesThatAreNotAnObject) {
  std::string merged;
  std::string error;
  EXPECT_FALSE(MergeFleasionClientSettingsOverrides("[]", &merged, &error));
  EXPECT_FALSE(error.empty());
}

}
}
