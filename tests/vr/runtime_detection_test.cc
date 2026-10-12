#include "mocktail/vr/openxr_backend.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace mocktail::vr {
namespace {
class VrRuntimeDetectionTest : public ::testing::Test {
protected:
  void SetUp() override {
    char path[] = "/tmp/mocktail-xr-detect-XXXXXX";
    ASSERT_NE(mkdtemp(path), nullptr);
    root = path;
    std::filesystem::create_directories(root / "proc/net");
    std::filesystem::create_directories(root / "config/openxr/1");
    std::filesystem::create_directories(root / "run/wivrn");
    std::filesystem::create_symlink("/proc/net/unix", root / "proc/net/unix");
  }
  void TearDown() override {
    if (socket >= 0)
      close(socket);
    std::error_code error;
    if (!root.empty())
      std::filesystem::remove_all(root, error);
  }
  void File(const std::filesystem::path &path, const std::string &data = "{}") {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << data;
  }
  std::string Steam() {
    const auto install = root / "custom-library/SteamVR";
    File(install / "steamxr_linux64.json");
    File(install / "bin/linux64/vrserver", "test executable");
    File(root / "proc/100/comm", "vrserver\n");
    std::filesystem::create_symlink(install / "bin/linux64/vrserver",
                                    root / "proc/100/exe");
    return (install / "steamxr_linux64.json").string();
  }
  std::string WiVRn(bool listen) {
    const auto install = root / "wivrn-build";
    File(install / "openxr_wivrn-dev.json");
    File(install / "server/wivrn-server", "test executable");
    File(root / "proc/200/comm", "wivrn-server\n");
    std::filesystem::create_symlink(install / "server/wivrn-server",
                                    root / "proc/200/exe");
    const auto path = (root / "run/wivrn/comp_ipc").string();
    socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    EXPECT_GE(socket, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    EXPECT_LT(path.size(), sizeof(address.sun_path));
    std::strcpy(address.sun_path, path.c_str());
    EXPECT_EQ(
        bind(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)),
        0);
    if (listen)
      EXPECT_EQ(::listen(socket, 1), 0);
    else {
      close(socket);
      socket = -1;
    } // Leave a stale socket inode.
    return (install / "openxr_wivrn-dev.json").string();
  }
  VrRuntimeDiscovery Discover(const std::string &preference = "auto") {
    return DiscoverVrRuntime(preference, (root / "home").string(),
                             (root / "config").string(),
                             (root / "etc/xdg").string(),
                             (root / "run").string(), (root / "proc").string());
  }
  std::string Selected(const std::string &preference = "auto") {
    return SelectVrRuntimeManifest("", Discover(preference).candidates);
  }
  std::filesystem::path root;
  int socket = -1;
};
TEST_F(VrRuntimeDetectionTest, RunningSteamOverridesOldRegisteredWiVRn) {
  File(root / "config/openxr/1/active_runtime.json");
  EXPECT_EQ(Selected(),
            (root / "config/openxr/1/active_runtime.json").string());
  const auto steam = Steam();
  EXPECT_EQ(Selected(), steam);
  EXPECT_NE(Discover().reason.find("running SteamVR"), std::string::npos);
}
TEST_F(VrRuntimeDetectionTest, ListeningWiVRnFindsDevelopmentManifest) {
  File(root / "config/openxr/1/active_runtime.json");
  const auto wivrn = WiVRn(true);
  EXPECT_EQ(Selected(), wivrn);
  EXPECT_NE(Discover().reason.find("listening WiVRn"), std::string::npos);
}
TEST_F(VrRuntimeDetectionTest, RegisteredWiVRnBuildWinsOverNativeFallback) {
  const auto registered =
      (root / "config/openxr/1/active_runtime.json").string();
  File(
      registered,
      R"({"runtime":{"library_path":"/var/lib/flatpak/app/wivrn/libwivrn.so"}})");
  WiVRn(true);
  EXPECT_EQ(Selected(), registered);
}
TEST_F(VrRuntimeDetectionTest, StaleWiVRnSocketDoesNotOverrideSteam) {
  const auto steam = Steam();
  WiVRn(false);
  EXPECT_EQ(Selected(), steam);
}
TEST_F(VrRuntimeDetectionTest,
       BothServersRespectRegisteredRuntimeAndExplicitOverrides) {
  const auto registered =
      (root / "config/openxr/1/active_runtime.json").string();
  File(registered);
  const auto steam = Steam();
  const auto wivrn = WiVRn(true);
  EXPECT_EQ(Selected(), registered);
  EXPECT_EQ(Selected("alvr"), steam);
  EXPECT_EQ(Selected("wivrn"), wivrn);
  EXPECT_TRUE(Discover("system").candidates.empty());
  EXPECT_EQ(
      SelectVrRuntimeManifest("/custom/override.json", Discover().candidates),
      "/custom/override.json");
}
TEST_F(VrRuntimeDetectionTest,
       OpenVrRegistryFindsCustomSteamLibraryWithoutRunningServer) {
  const auto install = root / "elsewhere/SteamVR";
  File(install / "steamxr_linux64.json");
  File(root / "config/openvr/openvrpaths.vrpath",
       "{\"runtime\":[42,\"relative/path\",\"" + install.string() + "\"]}");
  EXPECT_EQ(Selected("alvr"), (install / "steamxr_linux64.json").string());
  // No external host registration or installed WiVRn is assumed in this
  // assertion.
  const auto candidates = Discover().candidates;
  EXPECT_NE(std::find(candidates.begin(), candidates.end(),
                      (install / "steamxr_linux64.json").string()),
            candidates.end());
  File(root / "config/openvr/openvrpaths.vrpath", "invalid json");
  EXPECT_NE(Selected("alvr"), (install / "steamxr_linux64.json").string());
}
} // namespace
} // namespace mocktail::vr
