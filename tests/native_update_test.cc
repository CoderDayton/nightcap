#include <gtest/gtest.h>
#include <minizip/zip.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "compat/build_profile.h"
#include "compat/elf_build_id.h"
#include "compat/guest_abi.h"
#include "compat/host_abi_profile.h"
#include "update/apkpure_provider.h"
#include "update/compatibility_catalog.h"
#include "update/host_abi_deriver.h"
#include "update/http_download.h"
#include "update/payload_integrity.h"
#include "update/payload_store.h"
#include "update/readiness_canary.h"
#include "update/unsafe_latest_runner.h"
#include "update/update_config.h"
#include "update/update_coordinator.h"
#include "update/zip_archive.h"

namespace mocktail::update {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_native_update_XXXXXX";
    const char* created = mkdtemp(pattern);
    if (created != nullptr) root_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path root_;
};

void Write(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << contents;
  // Approval artifacts are rejected when group or world writable, which a
  // permissive umask would otherwise leave them.
  chmod(path.c_str(), 0600);
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::string MetadataRecord(std::string_view version_name,
                           std::uint64_t version_code,
                           std::string_view artifact, std::string_view url) {
  const std::string code = std::to_string(version_code);
  std::string record("protobuf-prefix\0", 16);
  record.push_back('\x2a');
  record.push_back(static_cast<char>(code.size()));
  record += code;
  record.push_back('\x32');
  record.push_back(static_cast<char>(version_name.size()));
  record += version_name;
  record += ":metadata";
  record.push_back('\0');
  record += artifact;
  record.append("\0\0", 2);
  record += url;
  record.push_back('\0');
  return record;
}

bool CreateZip(const std::filesystem::path& path) {
  zipFile archive = zipOpen64(path.c_str(), APPEND_STATUS_CREATE);
  if (archive == nullptr) return false;
  const std::vector<std::pair<std::string, std::string>> entries = {
      {"ignored.txt", "ignored"},
      {"assets/content/first.txt", "first"},
      {"assets/nested/second.txt", "second"},
  };
  for (const auto& [name, contents] : entries) {
    if (zipOpenNewFileInZip64(archive, name.c_str(), nullptr, nullptr, 0,
                              nullptr, 0, nullptr, Z_DEFLATED,
                              Z_DEFAULT_COMPRESSION, 1) != ZIP_OK ||
        zipWriteInFileInZip(archive, contents.data(), contents.size()) !=
            ZIP_OK ||
        zipCloseFileInZip(archive) != ZIP_OK) {
      zipClose(archive, nullptr);
      return false;
    }
  }
  return zipClose(archive, nullptr) == ZIP_OK;
}

TEST(NativeUpdateConfigTest, ReadsOnlyTypedUpdaterSection) {
  TemporaryDirectory temporary;
  const auto config = temporary.root() / "config.yaml";
  Write(config,
        "version: 1\n"
        "device: pc-windows-11\n"
        "updates:\n"
        "  automatic: false\n"
        "  source: apk-pure\n"
        "  launch_after_update: true\n");
  const UpdateConfigResult result = LoadUpdateConfig(config);
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.file_loaded);
  EXPECT_FALSE(result.config.automatic);
  EXPECT_TRUE(result.config.launch_after_update);
  EXPECT_EQ(result.config.source, "apk-pure");
  EXPECT_TRUE(result.warnings.empty());
}

TEST(NativeUpdateConfigTest, IgnoresDeprecatedTestingLatestOnlyWithWarning) {
  TemporaryDirectory temporary;
  const auto config = temporary.root() / "config.yaml";
  Write(config,
        "version: 1\nupdates:\n  testing_latest_only: true\n");
  const UpdateConfigResult result = LoadUpdateConfig(config);
  ASSERT_TRUE(result) << result.error;
  ASSERT_EQ(result.warnings.size(), 1U);
  EXPECT_NE(result.warnings.front().find("no longer supported"),
            std::string::npos);
}

TEST(NativeUpdateConfigTest, MissingFileUsesSafeDefaults) {
  TemporaryDirectory temporary;
  const UpdateConfigResult result =
      LoadUpdateConfig(temporary.root() / "missing.yaml");
  ASSERT_TRUE(result) << result.error;
  EXPECT_FALSE(result.file_loaded);
  EXPECT_TRUE(result.config.automatic);
  EXPECT_EQ(result.config.source, "apk-pure");
}

TEST(NativeUpdateConfigTest, RejectsUnsupportedSource) {
  TemporaryDirectory temporary;
  const auto config = temporary.root() / "config.yaml";
  Write(config,
        "version: 1\nupdates:\n  source: google-play\n");
  const UpdateConfigResult result = LoadUpdateConfig(config);
  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("apk-pure"), std::string::npos);
}

TEST(ApkPureProviderTest, ParsesExactVersionIdentityAndTrustedUrl) {
  const std::string metadata =
      MetadataRecord("2.727.1199", 2628, "XAPKJ",
                     "https://download.pureapk.com/b/XAPK/roblox");
  const ProviderVersion latest = ParseApkPureLatestMetadata(metadata);
  ASSERT_TRUE(latest) << latest.error;
  EXPECT_EQ(latest.version_name, "2.727.1199");
  EXPECT_EQ(latest.version_code, 2628);
  std::string error;
  const auto urls =
      ParseApkPureExactDownloadUrls(metadata, "2.727.1199", &error);
  ASSERT_TRUE(error.empty()) << error;
  ASSERT_EQ(urls.size(), 1);
  EXPECT_EQ(urls.front(), "https://download.pureapk.com/b/XAPK/roblox");
}

TEST(UnsafeLatestRunnerTest, LaunchesCandidateWithUnapprovedEnvironment) {
  TemporaryDirectory temporary;
  const std::filesystem::path output = temporary.root() / "environment.txt";
  const std::filesystem::path arguments = temporary.root() / "arguments.txt";
  const std::filesystem::path runtime = temporary.root() / "runtime.sh";
  Write(runtime,
        "#!/bin/sh\n"
        "env | sort > \"$ENV_OUTPUT\"\n"
        "printf '%s\\n' \"$@\" > \"$ARGUMENT_OUTPUT\"\n");
  ASSERT_EQ(chmod(runtime.c_str(), 0700), 0);

  const std::filesystem::path payload = temporary.root() / "payload";
  Write(payload / "libroblox.so", "candidate");
  std::filesystem::create_directories(payload / "assets/content");
  const std::filesystem::path compatibility =
      temporary.root() / "compatibility.json";
  const std::filesystem::path profile = temporary.root() / "profile.json";
  Write(compatibility, "{}\n");
  Write(profile, "{}\n");

  UnsafeLatestRunOptions options;
  options.runtime_binary = runtime;
  options.payload_directory = payload;
  options.compatibility_manifest = compatibility;
  options.host_abi_profile = profile;
  options.inherited_environment = {
      "PATH=/usr/bin:/bin", "ENV_OUTPUT=" + output.string(),
      "ARGUMENT_OUTPUT=" + arguments.string(), "UNCHANGED=value",
      "ROBLOX_LIB_PATH=/stale/libroblox.so",
      "MOCKTAIL_HOST_ABI_APPROVAL_RECEIPT=/stale/approval.json",
      "MOCKTAIL_SKIP_UPDATE_CHECK=0",
  };

  const UnsafeLatestRunResult result = RunUnsafeLatestCandidate(options);
  ASSERT_TRUE(result) << result.error;
  const std::string environment = ReadFile(output);
  EXPECT_NE(environment.find("UNCHANGED=value\n"), std::string::npos);
  EXPECT_NE(environment.find("ROBLOX_LIB_PATH=" +
                             (payload / "libroblox.so").string() + "\n"),
            std::string::npos);
  EXPECT_NE(environment.find("MOCKTAIL_COMPATIBILITY_MANIFEST=" +
                             compatibility.string() + "\n"),
            std::string::npos);
  EXPECT_NE(environment.find("MOCKTAIL_HOST_ABI_PROFILE_FILE=" +
                             profile.string() + "\n"),
            std::string::npos);
  EXPECT_NE(environment.find("MOCKTAIL_HOST_ABI_CANARY=1\n"),
            std::string::npos);
  EXPECT_NE(environment.find("MOCKTAIL_ALLOW_CANDIDATE_HOST_ABI=1\n"),
            std::string::npos);
  EXPECT_NE(environment.find("MOCKTAIL_SKIP_UPDATE_CHECK=1\n"),
            std::string::npos);
  EXPECT_NE(environment.find("MOCKTAIL_UNSAFE_LATEST=1\n"),
            std::string::npos);
  EXPECT_EQ(environment.find("/stale/libroblox.so"), std::string::npos);
  EXPECT_EQ(environment.find("/stale/approval.json"), std::string::npos);
  EXPECT_EQ(ReadFile(arguments), "--windowed\n--allow-unverified-build\n");
}

TEST(HttpDownloadPolicyTest, RejectsCredentialsAndHostSuffixTricks) {
  const std::vector<std::string> hosts = {"pureapk.com"};
  EXPECT_TRUE(IsTrustedHttpsUrl("https://download.pureapk.com/a", hosts));
  EXPECT_FALSE(IsTrustedHttpsUrl("http://download.pureapk.com/a", hosts));
  EXPECT_FALSE(IsTrustedHttpsUrl("https://pureapk.com.attacker.test/a", hosts));
  EXPECT_FALSE(IsTrustedHttpsUrl("https://user@pureapk.com/a", hosts));
}

TEST(ReadinessCanaryTest, AcceptsRealPresentWithoutShaderPackSummary) {
  const std::string valid =
      "[compat] legacy binary patches: disabled\n"
      "[compat] signal-recovery handler disabled\n"
      "[compat] native allocator retained; host allocator bridges disabled\n"
      "[window] vkQueuePresentKHR #240 window=0x1234\n"
      "[window] first Roblox Vulkan frame presented\n"
      "[vulkan] SDL WSI adapter shut down\n"
      "[main] Roblox lifecycle shutdown: Stopped\n";
  std::string error;
  EXPECT_TRUE(ValidateReadinessLog(CanaryGraphicsBackend::kDirectVulkan,
                                   valid, &error))
      << error;
  error.clear();
  EXPECT_FALSE(ValidateReadinessLog(CanaryGraphicsBackend::kDirectVulkan,
                                    valid + "[FATAL] crash\n", &error));

  const std::string missing_real_present =
      "[compat] legacy binary patches: disabled\n"
      "[compat] signal-recovery handler disabled\n"
      "[compat] native allocator retained; host allocator bridges disabled\n"
      "Loaded 2988 shaders from pack vulkan_mobile\n"
      "[window] first Roblox Vulkan frame presented\n"
      "[vulkan] SDL WSI adapter shut down\n"
      "[main] Roblox lifecycle shutdown: Stopped\n";
  error.clear();
  EXPECT_FALSE(ValidateReadinessLog(CanaryGraphicsBackend::kDirectVulkan,
                                    missing_real_present, &error));
  EXPECT_NE(error.find("real queue present"), std::string::npos);
}

TEST(ReadinessCanaryTest, ValidatesRealOpenGlEsThreeSwap) {
  const std::string valid =
      "[compat] legacy binary patches: disabled\n"
      "[compat] signal-recovery handler disabled\n"
      "[compat] native allocator retained; host allocator bridges disabled\n"
      "[window] OpenGL ES context version=3.2\n"
      "[window] EGL context and surface initialized via SDL3\n"
      "[window] SwapBuffers #1 window=0x1234\n"
      "[window] first Roblox frame presented\n"
      "[main] Roblox lifecycle shutdown: Stopped\n";
  std::string error;
  EXPECT_TRUE(ValidateReadinessLog(CanaryGraphicsBackend::kOpenGlEs, valid,
                                   &error))
      << error;

  std::string gles2 = valid;
  gles2.replace(gles2.find("version=3.2"), std::string("version=3.2").size(),
                "version=2.0");
  error.clear();
  EXPECT_FALSE(ValidateReadinessLog(CanaryGraphicsBackend::kOpenGlEs, gles2,
                                    &error));
  EXPECT_NE(error.find("version=3."), std::string::npos);

  std::string missing_swap = valid;
  const std::string swap_marker =
      "[window] SwapBuffers #1 window=0x1234\n";
  missing_swap.erase(missing_swap.find(swap_marker), swap_marker.size());
  error.clear();
  EXPECT_FALSE(ValidateReadinessLog(CanaryGraphicsBackend::kOpenGlEs,
                                    missing_swap, &error));
  EXPECT_NE(error.find("real EGL swap"), std::string::npos);
}

TEST(ReadinessCanaryTest, MapsWindowBackendsToTheirPresentPath) {
  CanaryGraphicsBackend backend = CanaryGraphicsBackend::kDirectVulkan;
  EXPECT_TRUE(ParseCanaryGraphicsBackend("direct-vulkan", &backend));
  EXPECT_EQ(backend, CanaryGraphicsBackend::kDirectVulkan);
  EXPECT_TRUE(ParseCanaryGraphicsBackend("opengl", &backend));
  EXPECT_EQ(backend, CanaryGraphicsBackend::kOpenGlEs);
  EXPECT_TRUE(ParseCanaryGraphicsBackend("angle-vulkan", &backend));
  EXPECT_EQ(backend, CanaryGraphicsBackend::kAngleVulkan);
  EXPECT_EQ(CanaryGraphicsBackendName(backend), "angle-vulkan");
  EXPECT_FALSE(ParseCanaryGraphicsBackend("unknown", &backend));
}

TEST(ZipArchiveTest, ExtractsSelectedPrefixInOneSequentialPass) {
  TemporaryDirectory temporary;
  const std::filesystem::path archive = temporary.root() / "fixture.zip";
  ASSERT_TRUE(CreateZip(archive));
  std::size_t files = 0;
  std::string error;
  const std::filesystem::path output = temporary.root() / "output";
  ASSERT_TRUE(
      ExtractZipPrefix(archive, "assets/", output, 1024, &files, &error))
      << error;
  EXPECT_EQ(files, 2);
  EXPECT_EQ(ReadFile(output / "content/first.txt"), "first");
  EXPECT_EQ(ReadFile(output / "nested/second.txt"), "second");
  EXPECT_FALSE(std::filesystem::exists(output / "ignored.txt"));
}

std::vector<SupportedPayloadProfile> CatalogFixture() {
  return {
      {"2.725.1142", 2546, "d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21"},
      {"2.727.1199", 2628, "1686400865ae0e408cd7bd67de7a439625c6fd13"},
      {"2.734.917", 2908, "63c5109637b7d7b2bdb8ed8f858023ff5ef49326"},
  };
}

PayloadStoreResult InstalledFixture(std::string_view version_name,
                                    std::uint64_t version_code) {
  PayloadStoreResult installed;
  installed.payload_id = std::to_string(version_code) + "-" +
                         std::string(40, 'a');
  installed.version_name = version_name;
  installed.version_code = version_code;
  return installed;
}

// A rejected candidate used to end the update, leaving the user on whatever
// was installed.
TEST(UpdateCandidatePlanTest, FallsBackToTheNewestSupportedProfile) {
  const auto profiles = CatalogFixture();
  const ProviderVersion latest{"2.736.1408", 2998, {}};
  const PayloadStoreResult current = InstalledFixture("2.725.1142", 2546);

  const std::vector<UpdateCandidatePlan> plans =
      PlanUpdateCandidates(profiles, profiles.back(), &latest, current,
                           current);

  ASSERT_EQ(plans.size(), 2U);
  EXPECT_EQ(plans[0].identity.version_code, 2998U);
  EXPECT_FALSE(plans[0].exact_supported);
  EXPECT_EQ(plans[1].identity.version_code, 2908U);
  EXPECT_TRUE(plans[1].exact_supported);
  EXPECT_EQ(plans[1].identity.exact_build_id,
            "63c5109637b7d7b2bdb8ed8f858023ff5ef49326");
}

TEST(UpdateCandidatePlanTest, OffersTheLatestOnceWhenItIsExactSupported) {
  const auto profiles = CatalogFixture();
  const ProviderVersion latest{"2.734.917", 2908, {}};
  const PayloadStoreResult current = InstalledFixture("2.725.1142", 2546);

  const std::vector<UpdateCandidatePlan> plans =
      PlanUpdateCandidates(profiles, profiles.back(), &latest, current,
                           current);

  ASSERT_EQ(plans.size(), 1U);
  EXPECT_EQ(plans[0].identity.version_code, 2908U);
  EXPECT_TRUE(plans[0].exact_supported);
}

TEST(UpdateCandidatePlanTest, NeverDowngradesARunnablePayload) {
  const auto profiles = CatalogFixture();
  const ProviderVersion latest{"2.736.1408", 2998, {}};
  const PayloadStoreResult current = InstalledFixture("2.736.1408", 2998);

  EXPECT_TRUE(
      PlanUpdateCandidates(profiles, profiles.back(), &latest, current, current)
          .empty());
  // The same holds when the provider cannot be reached at all.
  EXPECT_TRUE(PlanUpdateCandidates(profiles, profiles.back(), nullptr, current,
                                   current)
                  .empty());
}

TEST(UpdateCandidatePlanTest, InstallsTheSupportedProfileWithoutAProvider) {
  const auto profiles = CatalogFixture();
  const PayloadStoreResult current = InstalledFixture("2.725.1142", 2546);

  const std::vector<UpdateCandidatePlan> plans =
      PlanUpdateCandidates(profiles, profiles.back(), nullptr, current,
                           current);

  ASSERT_EQ(plans.size(), 1U);
  EXPECT_EQ(plans[0].identity.version_code, 2908U);
}

TEST(UpdateCandidatePlanTest, ReusesAnApprovedPayloadForTheSameVersion) {
  const auto profiles = CatalogFixture();
  const ProviderVersion latest{"2.736.1408", 2998, {}};
  PayloadStoreResult installed = InstalledFixture("2.736.1408", 2998);
  installed.host_abi_profile = "host_abi_profiles/2998.json";
  installed.compatibility_manifest = "compatibility_profiles/2998.json";

  const std::vector<UpdateCandidatePlan> plans = PlanUpdateCandidates(
      profiles, profiles.back(), &latest, PayloadStoreResult{}, installed);

  ASSERT_FALSE(plans.empty());
  EXPECT_EQ(plans[0].identity.version_code, 2998U);
  EXPECT_TRUE(plans[0].reuse_installed);
}

TEST(HostAbiSidecarTest, RejectsAnIdentityThatDoesNotDescribeItsOwnPayload) {
  TemporaryDirectory temporary;
  const std::string build_id = "1686400865ae0e408cd7bd67de7a439625c6fd13";
  const std::filesystem::path consistent = temporary.root() / "good.json";
  Write(consistent, nlohmann::json({{"schema_version", 1},
                                    {"elf_build_id", build_id},
                                    {"payload_id", "2628-" + build_id},
                                    {"payload_path",
                                     "payloads/2628-" + build_id}})
                            .dump(2) +
                        "\n");
  const HostAbiSidecarIdentity identity =
      ReadHostAbiSidecarIdentity(consistent);
  ASSERT_TRUE(identity) << identity.error;
  EXPECT_EQ(identity.elf_build_id, build_id);
  EXPECT_EQ(identity.payload_id, "2628-" + build_id);

  const std::filesystem::path mismatched = temporary.root() / "bad.json";
  const std::string other = "2908-" + std::string(40, 'b');
  Write(mismatched,
        nlohmann::json({{"schema_version", 1},
                        {"elf_build_id", build_id},
                        {"payload_id", other},
                        {"payload_path", "payloads/" + other}})
                .dump(2) +
            "\n");
  EXPECT_FALSE(ReadHostAbiSidecarIdentity(mismatched));
}

// The regression: the library used to be the newest supported profile while
// the sidecar stayed whatever file shipped, and the pair failed every
// derivation.
TEST(ReferenceProfileTest, FollowsTheSidecarRatherThanTheNewestProfile) {
  TemporaryDirectory temporary;
  const auto profiles = CatalogFixture();
  const auto preferred = PreferredSupportedProfile(profiles);
  ASSERT_TRUE(preferred.has_value());
  const auto& older = profiles[1];
  const std::string payload_id =
      std::to_string(older.version_code) + "-" + older.elf_build_id;
  const auto reference_file = temporary.root() / "reference.json";
  Write(reference_file,
        nlohmann::json({{"schema_version", 1},
                        {"elf_build_id", older.elf_build_id},
                        {"payload_id", payload_id},
                        {"payload_path", "payloads/" + payload_id}})
                .dump(2) +
            "\n");

  std::filesystem::path sidecar;
  const auto reference =
      ResolveReferenceProfile(reference_file, profiles, &sidecar);
  ASSERT_TRUE(reference.has_value());
  EXPECT_EQ(sidecar, reference_file);
  EXPECT_EQ(reference->elf_build_id,
            ReadHostAbiSidecarIdentity(sidecar).elf_build_id);
  EXPECT_EQ(reference->elf_build_id, older.elf_build_id);
  EXPECT_NE(reference->elf_build_id, preferred->elf_build_id);
}

TEST(ReferenceProfileTest, PicksTheNewestSidecarInADirectory) {
  TemporaryDirectory temporary;
  const std::filesystem::path directory = temporary.root() / "host_abi";
  const auto profiles = CatalogFixture();
  for (const SupportedPayloadProfile& profile : {profiles[1], profiles[2]}) {
    const std::string payload_id =
        std::to_string(profile.version_code) + "-" + profile.elf_build_id;
    Write(directory / (profile.elf_build_id + ".json"),
          nlohmann::json({{"schema_version", 1},
                          {"elf_build_id", profile.elf_build_id},
                          {"payload_id", payload_id},
                          {"payload_path", "payloads/" + payload_id}})
                  .dump(2) +
              "\n");
  }

  std::filesystem::path sidecar;
  const auto reference =
      ResolveReferenceProfile(directory, profiles, &sidecar);
  ASSERT_TRUE(reference.has_value());
  EXPECT_EQ(reference->version_code, 2908U);
  EXPECT_EQ(sidecar, directory / (profiles[2].elf_build_id + ".json"));

  // A sidecar for a Build ID no supported profile owns is not a reference.
  const auto orphaned = ResolveReferenceProfile(
      directory, {{"2.740.0", 3100, std::string(40, 'c')}}, &sidecar);
  EXPECT_FALSE(orphaned.has_value());
}

// A sidecar for a Build ID the catalog does not carry disables Tier C.
TEST(ShippedMetadataTest, ReferenceSidecarDescribesASupportedProfile) {
  const std::filesystem::path root = MOCKTAIL_TEST_SOURCE_DIR;
  const CompatibilityCatalogResult catalog =
      LoadCompatibilityCatalog(root / "config/roblox_compatibility.json");
  ASSERT_TRUE(catalog) << catalog.error;
  const HostAbiSidecarIdentity identity = ReadHostAbiSidecarIdentity(
      root / "config/roblox_host_abi_reference.json");
  ASSERT_TRUE(identity) << identity.error;
  std::string sidecar_abi = "x86_64";
  for (const auto& entry :
       nlohmann::json::parse(
           ReadFile(root / "config/roblox_compatibility.json"))
           .at("profiles")) {
    if (entry.value("elf_build_id", "") == identity.elf_build_id) {
      sidecar_abi = entry.value("abi", "x86_64");
    }
  }
  if (sidecar_abi != compat::kGuestAbi) {
    GTEST_SKIP() << "shipped reference sidecar targets " << sidecar_abi;
  }

  const auto supported = std::find_if(
      catalog.profiles.begin(), catalog.profiles.end(),
      [&](const SupportedPayloadProfile& profile) {
        return profile.elf_build_id == identity.elf_build_id;
      });
  ASSERT_NE(supported, catalog.profiles.end())
      << "no supported profile owns reference Build ID "
      << identity.elf_build_id;
  EXPECT_EQ(identity.payload_id,
            std::to_string(supported->version_code) + "-" +
                supported->elf_build_id);

  // A stale reference can derive a runnable client while silently dropping
  // fullscreen synchronization and the host audio output menu.
  const auto preferred = PreferredSupportedProfile(catalog.profiles);
  ASSERT_TRUE(preferred.has_value());
  EXPECT_EQ(identity.elf_build_id, preferred->elf_build_id);
  const auto runtime = compat::FindBuildProfile(
      (root / "config/roblox_compatibility.json").string(),
      identity.elf_build_id);
  ASSERT_TRUE(runtime) << runtime.error;
  ASSERT_TRUE(runtime.profile.has_value());
  EXPECT_TRUE(
      runtime.profile->user_game_settings_fullscreen_setter_rva.has_value());
  EXPECT_TRUE(runtime.profile->fmod_output_device_bridge.has_value());
}

TEST(ShippedMetadataTest, DefaultPayloadHasABuiltinProfileAndMatchingReference) {
  const std::filesystem::path root = MOCKTAIL_TEST_SOURCE_DIR;
  const auto catalog =
      LoadCompatibilityCatalog(root / "config/roblox_compatibility.json");
  ASSERT_TRUE(catalog) << catalog.error;
  const auto preferred = PreferredSupportedProfile(catalog.profiles);
  ASSERT_TRUE(preferred.has_value());
  EXPECT_EQ(preferred->version_name, "2.736.1408");
  EXPECT_EQ(preferred->version_code, 2998U);

  const auto payload = nlohmann::json::parse(
      ReadFile(root / "config/roblox_payload.json"));
  if (payload.at("abi").get_ref<const std::string&>() != compat::kGuestAbi) {
    EXPECT_NE(compat::FindHostAbiProfile(preferred->elf_build_id), nullptr);
    GTEST_SKIP() << "shipped default payload targets "
                 << payload.at("abi").get<std::string>();
  }
  EXPECT_EQ(payload.at("version_name"), preferred->version_name);
  EXPECT_EQ(payload.at("version_code"), preferred->version_code);
  EXPECT_EQ(payload.at("elf_build_id"), preferred->elf_build_id);
  EXPECT_EQ(payload.at("compatibility_status"), "supported");

  const auto reference = nlohmann::json::parse(
      ReadFile(root / "config/roblox_host_abi_reference.json"));
  EXPECT_EQ(reference.at("payload_sha256"),
            payload.at("sha256").at("libroblox"));
  const auto* builtin = compat::FindHostAbiProfile(preferred->elf_build_id);
  ASSERT_NE(builtin, nullptr);
  const auto& profile = reference.at("profile");
  const auto rva = [](const nlohmann::json& value) {
    return std::stoull(value.get<std::string>(), nullptr, 16);
  };
  EXPECT_EQ(builtin->init_array_offset, rva(profile.at("init_array_offset")));
  EXPECT_EQ(builtin->init_array_count, profile.at("init_array_count"));
  ASSERT_EQ(builtin->bridge_entry_count, profile.at("bridge_entries").size());
  for (std::size_t i = 0; i < builtin->bridge_entry_count; ++i) {
    EXPECT_EQ(builtin->bridge_entries[i].rva,
              rva(profile.at("bridge_entries").at(i).at("rva")));
  }
  EXPECT_EQ(builtin->native_allocator.allocate,
            rva(profile.at("native_allocator").at("allocate")));
  EXPECT_EQ(builtin->native_allocator.deallocate,
            rva(profile.at("native_allocator").at("deallocate")));
  EXPECT_EQ(builtin->native_pre_jni_bootstrap.registry_initializer,
            rva(profile.at("native_pre_jni_bootstrap")
                    .at("registry_initializer")));
  EXPECT_EQ(builtin->native_pre_jni_bootstrap.registry_slot,
            rva(profile.at("native_pre_jni_bootstrap").at("registry_slot")));
  EXPECT_TRUE(builtin->HasValidConstructorRanges());
  EXPECT_TRUE(builtin->HasValidNativeMimallocConstructorRanges());
  EXPECT_EQ(builtin->NativeMimallocConstructorRangeEndExclusive(),
            builtin->init_array_count);
  EXPECT_EQ(builtin->default_allocator_strategy,
            compat::HostAllocatorStrategy::kNativeMimalloc);
}

std::string ActivationManifestFixture(const std::string& payload_id) {
  return nlohmann::json({{"schema_version", 1},
                         {"payload_id", payload_id},
                         {"payload_path", "payloads/" + payload_id}})
             .dump(2) +
         "\n";
}

// Both publication entry points must preserve the original rollback manifest.
class PayloadStorePromotionTest : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    root = temporary.root() / "store";
    compatibility = temporary.root() / "compatibility.json";
    profile = temporary.root() / "candidate.json";
    candidate_compatibility = temporary.root() / "candidate-compatibility.json";
    const auto prepared = temporary.root() / "prepared";
    Write(prepared / "libroblox.so", "native-library");
    Write(prepared / "sober_apk/base.apk", "base-apk");
    Write(prepared / "sober_apk" / compat::kGuestSplitApkFile, "split-apk");
    Write(prepared / "assets/content/fixture", "asset");
    std::string error;
    std::size_t asset_count = 0;
    const auto asset_hash =
        HashAssetTree(prepared / "assets", &asset_count, &error);
    ASSERT_TRUE(error.empty()) << error;
    const std::string build_id =
        GetParam() ? "a6c1f5c57f9d7aa3fb99a5fa30565e07e5c88f6a"
                   : "1686400865ae0e408cd7bd67de7a439625c6fd13";
    const auto library_hash = HashRegularFile(prepared / "libroblox.so");
    const nlohmann::json metadata = {
        {"schema_version", 1},
        {"package", "com.roblox.client"},
        {"version_name", "2.727.1199"},
        {"version_code", 2628},
        {"abi", std::string(compat::kGuestAbi)},
        {"elf_build_id", build_id},
        {"sha256",
         {{"libroblox", library_hash},
          {"base_apk", HashRegularFile(prepared / "sober_apk/base.apk")},
          {std::string(compat::kGuestSplitApkHashKey),
           HashRegularFile(prepared / "sober_apk" /
                           compat::kGuestSplitApkFile)}}},
        {"assets", {{"file_count", asset_count}, {"sha256_tree", asset_hash}}}};
    Write(prepared / "roblox_payload.json", metadata.dump(2) + "\n");
    Write(compatibility,
          "{\"schema_version\":1,\"profiles\":[{"
          "\"version_name\":\"2.727.1199\",\"version_code\":2628,"
          "\"elf_build_id\":\"1686400865ae0e408cd7bd67de7a439625c6fd13\","
          "\"status\":\"supported\",\"default_allowed\":true,"
          "\"allow_legacy_binary_patches\":false,\"abi\":\"" +
          std::string(compat::kGuestAbi) + "\"}]}\n");
    runtime = std::filesystem::canonical("/proc/self/exe");
    PayloadStore store(root, compatibility, runtime);
    const auto staged = store.Stage(prepared);
    ASSERT_TRUE(staged) << staged.error;
    candidate_id = staged.payload_id;
    Write(profile,
          nlohmann::json(
              {{"schema_version", 1},
               {"elf_build_id", build_id},
               {"payload_sha256", library_hash},
               {"payload_id", candidate_id},
               {"payload_path", "payloads/" + candidate_id},
               {"reference",
                {{"elf_build_id", "1686400865ae0e408cd7bd67de7a439625c6fd13"},
                 {"payload_sha256",
                  "3e9c26c81186f93458ff65d8a8bc240c220974db02b8388b91340b77b336"
                  "997d"}}},
               {"profile", {{"elf_build_id", build_id}}},
               {"derivation_anchors", {{"signature_version", 1}}}})
                  .dump(2) +
              "\n");
    Write(candidate_compatibility,
          nlohmann::json(
              {{"schema_version", 1},
               {"profiles", nlohmann::json::array(
                                {{{"version_name", "2.727.1199"},
                                  {"version_code", 2628},
                                  {"elf_build_id", build_id},
                                  {"status", "experimental"},
                                  {"default_allowed", true},
                                  {"allow_legacy_binary_patches", false},
                                  {"allow_host_abi_bridges", true},
                                  {"allow_host_constructor_replay", true}}})}})
                  .dump(2) +
              "\n");
    logs = {temporary.root() / "canary-1.log",
            temporary.root() / "canary-2.log"};
    Write(logs[0], "first independent Tier C pass\n");
    Write(logs[1], "second independent Tier C pass\n");
  }

  PayloadStoreResult Promote(ManifestWriteFault fault = {}) {
    PayloadStore store(root, compatibility, runtime, std::move(fault));
    return GetParam() ? store.PromoteProbation(candidate_id, profile,
                                               candidate_compatibility, logs)
                      : store.Promote(candidate_id);
  }

  void ResetManifests(bool previous_present = true) {
    Write(root / "current.json", current_bytes);
    std::filesystem::remove(root / "previous_good.json");
    if (previous_present) Write(root / "previous_good.json", previous_bytes);
  }

  void ExpectManifests(bool previous_present = true) {
    EXPECT_EQ(ReadFile(root / "current.json"), current_bytes);
    EXPECT_EQ(std::filesystem::exists(root / "previous_good.json"),
              previous_present);
    if (previous_present)
      EXPECT_EQ(ReadFile(root / "previous_good.json"), previous_bytes);
    EXPECT_FALSE(
        std::filesystem::exists(root / ".previous_good-recovery.json"));
    EXPECT_FALSE(
        std::filesystem::exists(root / ".previous_good-recovery.absent"));
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
      EXPECT_NE(entry.path().filename().string().find(".manifest-"), 0U);
    }
  }

  TemporaryDirectory temporary;
  std::filesystem::path root, compatibility, runtime, profile,
      candidate_compatibility;
  std::array<std::filesystem::path, 2> logs;
  std::string candidate_id;
  const std::string previous_id = "2908-" + std::string(40, 'b');
  const std::string current_bytes =
      ActivationManifestFixture("2998-" + std::string(40, 'a'));
  std::string previous_bytes =
      "\n\t" + ActivationManifestFixture(previous_id) + " \n";
  const std::array<ManifestWritePhase, 4> phases = {
      ManifestWritePhase::kCreate, ManifestWritePhase::kWrite,
      ManifestWritePhase::kFileFsync, ManifestWritePhase::kRename};
};

TEST_P(PayloadStorePromotionTest, CurrentWriteFailureRestoresPrevious) {
  for (const auto phase : phases) {
    for (const bool present : {true, false}) {
      SCOPED_TRACE(static_cast<int>(phase));
      SCOPED_TRACE(present);
      ResetManifests(present);
      int faults = 0;
      const auto result = Promote([&](const auto& path, auto at) {
        if (path.filename() != "current.json" || at != phase) return false;
        ++faults;
        return true;
      });
      EXPECT_FALSE(result);
      EXPECT_EQ(faults, 1);
      ExpectManifests(present);
    }
  }
}

TEST_P(PayloadStorePromotionTest, FirstWriteFailurePreservesBothManifests) {
  for (const auto phase : phases) {
    for (const bool present : {true, false}) {
      SCOPED_TRACE(static_cast<int>(phase));
      SCOPED_TRACE(present);
      ResetManifests(present);
      int faults = 0;
      const auto result = Promote([&](const auto& path, auto at) {
        if (path.filename() != "previous_good.json" || at != phase)
          return false;
        ++faults;
        return true;
      });
      EXPECT_FALSE(result);
      EXPECT_EQ(faults, 1);
      ExpectManifests(present);
    }
  }
}

TEST_P(PayloadStorePromotionTest, SuccessfulPublicationKeepsOldCurrent) {
  for (const bool present : {true, false}) {
    ResetManifests(present);
    const auto result = Promote();
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(
        nlohmann::json::parse(ReadFile(root / "current.json"))["payload_id"],
        candidate_id);
    EXPECT_EQ(ReadFile(root / "previous_good.json"), current_bytes);
    EXPECT_FALSE(
        std::filesystem::exists(root / ".previous_good-recovery.json"));
    EXPECT_FALSE(
        std::filesystem::exists(root / ".previous_good-recovery.absent"));
  }
}

TEST_P(PayloadStorePromotionTest,
       FailedRestorationRetainsBytesAndBlocksGarbageCollection) {
  for (const auto phase : phases) {
    SCOPED_TRACE(static_cast<int>(phase));
    ResetManifests();
    Write(root / "payloads" / previous_id / "libroblox.so", "last fallback");
    Write(root / "quarantine/collision/libroblox.so", "quarantined");
    int previous_writes = 0;
    const auto result = Promote([&](const auto& path, auto at) {
      if (path.filename() == "current.json" &&
          at == ManifestWritePhase::kCreate)
        return true;
      return path.filename() == "previous_good.json" && at == phase &&
             ++previous_writes == 2;
    });
    EXPECT_FALSE(result);
    EXPECT_EQ(previous_writes, 2);
    EXPECT_NE(result.error.find("cannot create atomic payload manifest"),
              std::string::npos);
    EXPECT_NE(result.error.find("previous_good restoration failed:"),
              std::string::npos);
    EXPECT_NE(result.error.find("recovery retained at"), std::string::npos);
    EXPECT_EQ(ReadFile(root / "current.json"), current_bytes);
    EXPECT_EQ(ReadFile(root / "previous_good.json"), current_bytes);
    EXPECT_EQ(ReadFile(root / ".previous_good-recovery.json"), previous_bytes);
    PayloadStore store(root, compatibility);
    const auto garbage = store.CollectGarbage({});
    EXPECT_FALSE(garbage);
    EXPECT_TRUE(garbage.removed.empty());
    EXPECT_EQ(garbage.freed_bytes, 0U);
    EXPECT_TRUE(std::filesystem::exists(root / "payloads" / previous_id));
    EXPECT_TRUE(
        std::filesystem::exists(root / "quarantine/collision/libroblox.so"));
    EXPECT_FALSE(Promote());
    EXPECT_EQ(ReadFile(root / ".previous_good-recovery.json"), previous_bytes);
    std::filesystem::remove(root / ".previous_good-recovery.json");
  }
}

TEST_P(PayloadStorePromotionTest, FailedAbsenceRestorationRetainsMarker) {
  ResetManifests(false);
  const auto result = Promote([&](const auto& path, auto phase) {
    if (path.filename() != "current.json" ||
        phase != ManifestWritePhase::kCreate)
      return false;
    std::filesystem::remove(root / "previous_good.json");
    Write(root / "previous_good.json/blocked",
          "cannot remove nonempty directory");
    return true;
  });
  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("cannot create atomic payload manifest"),
            std::string::npos);
  EXPECT_NE(result.error.find("cannot restore absence of previous_good"),
            std::string::npos);
  EXPECT_EQ(ReadFile(root / "current.json"), current_bytes);
  EXPECT_EQ(ReadFile(root / ".previous_good-recovery.absent"), "absent\n");
  PayloadStore store(root, compatibility);
  EXPECT_FALSE(store.CollectGarbage({}));
  EXPECT_TRUE(std::filesystem::exists(root / "payloads" / candidate_id));
  EXPECT_FALSE(Promote());
  EXPECT_EQ(ReadFile(root / ".previous_good-recovery.absent"), "absent\n");
}

TEST_P(PayloadStorePromotionTest, RestoresExplicitlyPresentEmptyPrevious) {
  previous_bytes.clear();
  ResetManifests();
  const auto result = Promote([](const auto& path, auto phase) {
    return path.filename() == "current.json" &&
           phase == ManifestWritePhase::kRename;
  });
  EXPECT_FALSE(result);
  ExpectManifests();
}

TEST_P(PayloadStorePromotionTest, SnapshotFailureDoesNotChangeManifests) {
  for (const auto phase : phases) {
    for (const bool present : {true, false}) {
      SCOPED_TRACE(static_cast<int>(phase));
      SCOPED_TRACE(present);
      ResetManifests(present);
      int faults = 0;
      const auto result = Promote([&](const auto& path, auto at) {
        if (path.filename() != (present ? ".previous_good-recovery.json"
                                        : ".previous_good-recovery.absent") ||
            at != phase)
          return false;
        ++faults;
        return true;
      });
      EXPECT_FALSE(result);
      EXPECT_EQ(faults, 1);
      ExpectManifests(present);
    }
  }
  ResetManifests();
  std::filesystem::remove(root / "previous_good.json");
  std::filesystem::create_directory(root / "previous_good.json");
  EXPECT_FALSE(Promote());
  EXPECT_EQ(ReadFile(root / "current.json"), current_bytes);
  EXPECT_TRUE(std::filesystem::is_directory(root / "previous_good.json"));
  EXPECT_FALSE(std::filesystem::exists(root / ".previous_good-recovery.json"));
}

TEST_P(PayloadStorePromotionTest,
       PendingRecoveryIncludingSymlinksBlocksPublicationAndCollection) {
  ResetManifests();
  Write(root / "payloads" / previous_id / "libroblox.so", "last fallback");
  for (const char* name :
       {".previous_good-recovery.json", ".previous_good-recovery.absent"}) {
    for (const bool symlink : {false, true}) {
      SCOPED_TRACE(name);
      SCOPED_TRACE(symlink);
      const auto recovery = root / name;
      if (symlink)
        std::filesystem::create_symlink("missing-target", recovery);
      else
        Write(recovery, "pending recovery bytes");
      const auto result = Promote();
      EXPECT_FALSE(result);
      EXPECT_NE(result.error.find("recovery is pending"), std::string::npos);
      PayloadStore store(root, compatibility);
      EXPECT_FALSE(store.CollectGarbage({}));
      EXPECT_EQ(ReadFile(root / "current.json"), current_bytes);
      EXPECT_EQ(ReadFile(root / "previous_good.json"), previous_bytes);
      EXPECT_TRUE(std::filesystem::exists(root / "payloads" / previous_id));
      if (symlink)
        EXPECT_TRUE(std::filesystem::is_symlink(recovery));
      else
        EXPECT_EQ(ReadFile(recovery), "pending recovery bytes");
      std::filesystem::remove(recovery);
    }
  }
}

TEST_P(PayloadStorePromotionTest,
       NoPreviousExchangeForFirstInstallOrSamePayload) {
  for (const bool same_payload : {false, true}) {
    for (const bool fail_current : {false, true}) {
      for (const bool present : {false, true}) {
        ResetManifests(present);
        if (same_payload)
          Write(root / "current.json", ActivationManifestFixture(candidate_id));
        else
          std::filesystem::remove(root / "current.json");
        const auto before = ReadFile(root / "current.json");
        int other_writes = 0;
        const auto result = Promote([&](const auto& path, auto phase) {
          if (path.filename() != "current.json") ++other_writes;
          return fail_current && path.filename() == "current.json" &&
                 phase == ManifestWritePhase::kCreate;
        });
        EXPECT_EQ(static_cast<bool>(result), !fail_current);
        EXPECT_EQ(other_writes, 0);
        EXPECT_EQ(std::filesystem::exists(root / "previous_good.json"),
                  present);
        if (present)
          EXPECT_EQ(ReadFile(root / "previous_good.json"), previous_bytes);
        if (fail_current) {
          EXPECT_EQ(ReadFile(root / "current.json"), before);
          EXPECT_EQ(std::filesystem::exists(root / "current.json"),
                    same_payload);
        }
      }
    }
  }
}

TEST_P(PayloadStorePromotionTest,
       CleanupFailureReportsWhetherCurrentWasPublished) {
  for (const bool publish : {false, true}) {
    ResetManifests();
    const auto recovery = root / ".previous_good-recovery.json";
    const auto result = Promote([&](const auto& path, auto phase) {
      if (path.filename() != "current.json" ||
          phase != ManifestWritePhase::kCreate)
        return false;
      // Preserve the snapshot in a nonempty directory to make unlink fail.
      const auto bytes = ReadFile(recovery);
      std::filesystem::remove(recovery);
      Write(recovery / "original", bytes);
      return !publish;
    });
    EXPECT_FALSE(result);
    EXPECT_NE(result.error.find("cannot remove previous_good recovery"),
              std::string::npos);
    EXPECT_EQ(
        result.error.find("current manifest published") != std::string::npos,
        publish);
    EXPECT_EQ(ReadFile(recovery / "original"), previous_bytes);
    if (publish) {
      EXPECT_EQ(
          nlohmann::json::parse(ReadFile(root / "current.json"))["payload_id"],
          candidate_id);
      EXPECT_EQ(ReadFile(root / "previous_good.json"), current_bytes);
    } else {
      EXPECT_EQ(ReadFile(root / "current.json"), current_bytes);
      EXPECT_EQ(ReadFile(root / "previous_good.json"), previous_bytes);
    }
    PayloadStore store(root, compatibility);
    EXPECT_FALSE(store.CollectGarbage({}));
    EXPECT_FALSE(Promote());
    std::filesystem::remove_all(recovery);
  }
}

TEST_P(PayloadStorePromotionTest, JsonManifestSchemaTypeReturnsError) {
  Write(root / "current.json", "{\"schema_version\":\"1\"}\n");
  ASSERT_EXIT(
      {
        PayloadStore store(root, compatibility);
        const auto result = store.InspectCurrent();
        std::_Exit(!result && !result.error.empty() ? 0 : 10);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST_P(PayloadStorePromotionTest, JsonIntegrityNestedHashTypeReturnsError) {
  const auto prepared = temporary.root() / "prepared";
  auto metadata =
      nlohmann::json::parse(ReadFile(prepared / "roblox_payload.json"));
  metadata["sha256"]["libroblox"] = false;
  Write(prepared / "roblox_payload.json", metadata.dump());
  ASSERT_EXIT(
      {
        const auto result = InspectPreparedPayload(prepared);
        std::_Exit(!result && !result.error.empty() ? 0 : 10);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST_P(PayloadStorePromotionTest, JsonCatalogFlagTypeReturnsError) {
  auto catalog = nlohmann::json::parse(ReadFile(compatibility));
  catalog["profiles"][0]["default_allowed"] = "true";
  Write(compatibility, catalog.dump());
  ASSERT_EXIT(
      {
        const auto result = LoadCompatibilityCatalog(compatibility);
        std::_Exit(!result && !result.error.empty() ? 0 : 10);
      },
      ::testing::ExitedWithCode(0), "");
}

std::string JsonWithOverflowNumber(nlohmann::json document,
                                   const nlohmann::json::json_pointer& field) {
  document[field] = "overflow-number";
  std::string contents = document.dump();
  const auto offset = contents.find("\"overflow-number\"");
  contents.replace(offset, std::string("\"overflow-number\"").size(),
                   "18446744073709551616");
  return contents;
}

TEST_P(PayloadStorePromotionTest, JsonManifestRejectsInvalidFields) {
  ASSERT_TRUE(Promote());
  const auto valid = nlohmann::json::parse(ReadFile(root / "current.json"));
  std::vector<std::string> cases = {"null", "[]", "true", "1"};
  for (const char* field :
       {"schema_version", "payload_id", "payload_path", "version_name",
        "version_code", "elf_build_id", "host_abi_profile_path",
        "compatibility_manifest_path", "approval_path", "payload_sha256"}) {
    for (const auto& bad :
         nlohmann::json::array({nullptr, true, 42, nlohmann::json::object(),
                                nlohmann::json::array()})) {
      auto document = valid;
      document[field] = bad;
      cases.push_back(document.dump());
    }
    if (std::string_view(field).find("_path") == std::string_view::npos ||
        std::string_view(field) == "payload_path") {
      if (std::string_view(field) != "payload_sha256") {
        auto document = valid;
        document.erase(field);
        cases.push_back(document.dump());
      }
    }
  }
  for (const auto& bad :
       nlohmann::json::array({"1", 0, -1, 1.0, 4294967297ULL})) {
    for (const char* field : {"schema_version", "version_code"}) {
      auto document = valid;
      document[field] = bad;
      cases.push_back(document.dump());
    }
  }
  cases.push_back(JsonWithOverflowNumber(
      valid, nlohmann::json::json_pointer("/version_code")));
  cases.push_back(JsonWithOverflowNumber(
      valid, nlohmann::json::json_pointer("/schema_version")));
  for (const auto& contents : cases) {
    SCOPED_TRACE(contents);
    Write(root / "current.json", contents);
    ASSERT_EXIT(
        {
          PayloadStore store(root, compatibility);
          const auto inspected = store.InspectCurrent();
          const auto verified = store.VerifyCurrent();
          std::_Exit(!inspected && !inspected.error.empty() && !verified &&
                             !verified.error.empty()
                         ? 0
                         : 10);
        },
        ::testing::ExitedWithCode(0), "");
  }
}

TEST_P(PayloadStorePromotionTest, JsonIntegrityRejectsInvalidFields) {
  const auto prepared = temporary.root() / "prepared";
  const auto valid =
      nlohmann::json::parse(ReadFile(prepared / "roblox_payload.json"));
  std::vector<std::string> cases = {"null", "[]", "true", "1"};
  const std::vector<std::string> fields = {
      "/schema_version",
      "/package",
      "/version_name",
      "/version_code",
      "/elf_build_id",
      "/sha256",
      "/assets",
      "/sha256/libroblox",
      "/sha256/base_apk",
      "/sha256/" + std::string(compat::kGuestSplitApkHashKey),
      "/assets/file_count",
      "/assets/sha256_tree"};
  for (const auto& field : fields) {
    const nlohmann::json::json_pointer pointer(field);
    auto missing = valid;
    const auto slash = field.find_last_of('/');
    missing[nlohmann::json::json_pointer(field.substr(0, slash))].erase(
        field.substr(slash + 1));
    cases.push_back(missing.dump());
    for (const auto& bad : nlohmann::json::array(
             {nullptr, true, -1, 1.5, nlohmann::json::array()})) {
      auto document = valid;
      document[pointer] = bad;
      cases.push_back(document.dump());
    }
  }
  for (const char* field :
       {"/schema_version", "/version_code", "/assets/file_count"}) {
    cases.push_back(
        JsonWithOverflowNumber(valid, nlohmann::json::json_pointer(field)));
  }
  for (const char* field :
       {"/schema_version", "/version_code", "/assets/file_count"}) {
    auto string_number = valid;
    string_number[nlohmann::json::json_pointer(field)] = "1";
    cases.push_back(string_number.dump());
  }
  auto zero_version = valid;
  zero_version["version_code"] = 0;
  cases.push_back(zero_version.dump());
  auto wrapped_schema = valid;
  wrapped_schema["schema_version"] = 4294967297ULL;
  cases.push_back(wrapped_schema.dump());
  if (std::numeric_limits<std::size_t>::max() <
      std::numeric_limits<std::uint64_t>::max()) {
    auto too_many = valid;
    too_many["assets"]["file_count"] =
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) + 1;
    cases.push_back(too_many.dump());
  }
  for (const auto& contents : cases) {
    SCOPED_TRACE(contents);
    Write(prepared / "roblox_payload.json", contents);
    ASSERT_EXIT(
        {
          const auto inspected = InspectPreparedPayload(prepared);
          const auto verified = VerifyPreparedPayload(prepared);
          PayloadStore store(root, compatibility);
          const auto staged = store.Stage(prepared);
          std::_Exit(!inspected && !inspected.error.empty() && !verified &&
                             !verified.error.empty() && !staged &&
                             !staged.error.empty()
                         ? 0
                         : 10);
        },
        ::testing::ExitedWithCode(0), "");
  }
}

TEST_P(PayloadStorePromotionTest, JsonCatalogRejectsInvalidFields) {
  const auto valid = nlohmann::json::parse(ReadFile(compatibility));
  std::vector<std::string> cases = {"null", "[]", "true", "1"};
  for (const char* field : {"schema_version", "profiles"}) {
    auto missing = valid;
    missing.erase(field);
    cases.push_back(missing.dump());
    for (const auto& bad : nlohmann::json::array(
             {nullptr, true, "wrong", 1.5, nlohmann::json::object()})) {
      auto document = valid;
      document[field] = bad;
      cases.push_back(document.dump());
    }
  }
  for (const char* field :
       {"status", "default_allowed", "allow_legacy_binary_patches",
        "version_name", "version_code", "elf_build_id", "abi"}) {
    for (const auto& bad :
         nlohmann::json::array({nullptr, 1.5, nlohmann::json::object(),
                                nlohmann::json::array()})) {
      auto document = valid;
      document["profiles"][0][field] = bad;
      cases.push_back(document.dump());
    }
    if (std::string_view(field) == "version_name" ||
        std::string_view(field) == "version_code" ||
        std::string_view(field) == "elf_build_id") {
      auto missing = valid;
      missing["profiles"][0].erase(field);
      cases.push_back(missing.dump());
    }
  }
  for (const char* field : {"default_allowed", "allow_legacy_binary_patches"}) {
    for (const auto& bad : nlohmann::json::array({"true", 1})) {
      auto document = valid;
      document["profiles"][0][field] = bad;
      cases.push_back(document.dump());
    }
  }
  for (const auto& bad : nlohmann::json::array({-1, "2628", 0})) {
    auto document = valid;
    document["profiles"][0]["version_code"] = bad;
    cases.push_back(document.dump());
  }
  cases.push_back(JsonWithOverflowNumber(
      valid, nlohmann::json::json_pointer("/profiles/0/version_code")));
  cases.push_back(JsonWithOverflowNumber(
      valid, nlohmann::json::json_pointer("/schema_version")));
  auto wrapped_schema = valid;
  wrapped_schema["schema_version"] = 4294967297ULL;
  cases.push_back(wrapped_schema.dump());
  for (const auto& contents : cases) {
    SCOPED_TRACE(contents);
    Write(compatibility, contents);
    ASSERT_EXIT(
        {
          const auto result = LoadCompatibilityCatalog(compatibility);
          std::_Exit(!result && !result.error.empty() ? 0 : 10);
        },
        ::testing::ExitedWithCode(0), "");
  }
}

TEST_P(PayloadStorePromotionTest,
       JsonOptionalDefaultsAndNumericLimitsStayValid) {
  ASSERT_TRUE(Promote());
  const auto activation =
      nlohmann::json::parse(ReadFile(root / "current.json"));
  PayloadStore store(root, compatibility);
  for (const bool absent : {false, true}) {
    auto document = activation;
    for (const char* field : {"host_abi_profile_path",
                              "compatibility_manifest_path", "approval_path"}) {
      if (absent)
        document.erase(field);
      else
        document[field] = "";
    }
    document.erase("payload_sha256");
    Write(root / "current.json", document.dump());
    ASSERT_TRUE(store.InspectCurrent());
  }
  Write(root / "current.json", activation.dump());
  ASSERT_TRUE(store.VerifyCurrent());

  const auto prepared = temporary.root() / "prepared";
  auto metadata =
      nlohmann::json::parse(ReadFile(prepared / "roblox_payload.json"));
  metadata["version_code"] = std::numeric_limits<std::uint64_t>::max();
  metadata["assets"]["file_count"] = 0;
  Write(prepared / "roblox_payload.json", metadata.dump());
  auto inspected = InspectPreparedPayload(prepared);
  ASSERT_TRUE(inspected) << inspected.error;
  EXPECT_EQ(inspected.metadata.version_code,
            std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(inspected.metadata.asset_file_count, 0U);
  metadata["assets"]["file_count"] = std::numeric_limits<std::size_t>::max();
  Write(prepared / "roblox_payload.json", metadata.dump());
  inspected = InspectPreparedPayload(prepared);
  ASSERT_TRUE(inspected) << inspected.error;
  EXPECT_EQ(inspected.metadata.asset_file_count,
            std::numeric_limits<std::size_t>::max());

  const auto catalog = nlohmann::json::parse(ReadFile(compatibility));
  for (const char* field :
       {"status", "default_allowed", "allow_legacy_binary_patches", "abi"}) {
    auto document = catalog;
    auto extra = catalog["profiles"][0];
    extra.erase(field);
    document["profiles"].push_back(extra);
    Write(compatibility, document.dump());
    const auto result = LoadCompatibilityCatalog(compatibility);
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(result.profiles.size(),
              std::string_view(field) == "abi" ? 2U : 1U);
  }
  auto document = catalog;
  document["profiles"][0]["version_code"] =
      std::numeric_limits<std::uint64_t>::max();
  auto foreign = document["profiles"][0];
  foreign["abi"] = "foreign-abi";
  document["profiles"].push_back(foreign);
  document["profiles"].push_back(nullptr);
  document["profiles"].push_back(nlohmann::json::array());
  Write(compatibility, document.dump());
  const auto result = LoadCompatibilityCatalog(compatibility);
  ASSERT_TRUE(result) << result.error;
  ASSERT_EQ(result.profiles.size(), 1U);
  EXPECT_EQ(result.profiles[0].version_code,
            std::numeric_limits<std::uint64_t>::max());
}

INSTANTIATE_TEST_SUITE_P(Publication, PayloadStorePromotionTest,
                         ::testing::Values(false, true));

class UpdateCoordinatorTest : public PayloadStorePromotionTest {
 protected:
  void SetUp() override {
    PayloadStorePromotionTest::SetUp();
    ASSERT_FALSE(GetParam());
    ASSERT_TRUE(Promote());
    active_id = candidate_id;
    const auto prepared = temporary.root() / "newer-prepared";
    std::filesystem::copy(temporary.root() / "prepared", prepared,
                          std::filesystem::copy_options::recursive);
    Write(prepared / "libroblox.so", "newer-native-library");
    auto metadata =
        nlohmann::json::parse(ReadFile(prepared / "roblox_payload.json"));
    metadata["version_name"] = "2.734.917";
    metadata["version_code"] = 2908;
    metadata["elf_build_id"] = std::string(40, 'b');
    metadata["sha256"]["libroblox"] =
        HashRegularFile(prepared / "libroblox.so");
    Write(prepared / "roblox_payload.json", metadata.dump());
    auto catalog = nlohmann::json::parse(ReadFile(compatibility));
    auto newer = catalog["profiles"][0];
    newer["version_name"] = metadata["version_name"];
    newer["version_code"] = metadata["version_code"];
    newer["elf_build_id"] = metadata["elf_build_id"];
    catalog["profiles"].push_back(newer);
    Write(compatibility, catalog.dump());
    PayloadStore store(root, compatibility, runtime);
    const auto staged = store.Stage(prepared);
    ASSERT_TRUE(staged) << staged.error;
    newer_id = staged.payload_id;
    ASSERT_TRUE(store.VerifyCurrent());
    const auto identity = compat::ReadElfBuildId(runtime.string());
    ASSERT_TRUE(identity) << identity.error;
    paths.config_file = temporary.root() / "config.yaml";
    Write(paths.config_file,
          "version: 1\nupdates:\n  automatic: true\n  source: apk-pure\n");
    paths.data_root = root;
    paths.cache_root = temporary.root() / "cache";
    paths.state_root = temporary.root() / "state";
    paths.compatibility_manifest = compatibility;
    paths.runtime_binary = runtime;
    request.startup_preflight = true;
    request.check_latest = false;
    request.run_canary = true;
    script = temporary.root() / "canary.sh";
    WriteScript();
  }

  void WriteScript(bool accepted = true) {
    Write(
        script,
        "#!/bin/sh\ncat <<'READY'\n"
        "[compat] legacy binary patches: disabled\n"
        "[compat] signal-recovery handler disabled\n"
        "[compat] native allocator retained; host allocator bridges disabled\n"
        "[window] vkQueuePresentKHR #240 window=0x1234\n"
        "[window] first Roblox Vulkan frame presented\n"
        "[vulkan] SDL WSI adapter shut down\n"
        "[window] OpenGL ES context version=3.2\n"
        "[window] EGL context and surface initialized via SDL3\n"
        "[window] SwapBuffers #1 window=0x1234\n"
        "[window] first Roblox frame presented\n"
        "[main] Roblox lifecycle shutdown: Stopped\nREADY\n" +
            std::string(accepted ? "" : "echo '[FATAL] rejected graphics'\n"));
    ASSERT_EQ(chmod(script.c_str(), 0700), 0);
  }

  CanarySpawn Spawn(int initial_error = 0) {
    return [this, initial_error](pid_t* child, const char*,
                                 const posix_spawn_file_actions_t* actions,
                                 const posix_spawnattr_t* attributes,
                                 char* const arguments[],
                                 char* const environment[]) {
      ++spawns;
      if (spawns == 1 && initial_error != 0) return initial_error;
      return posix_spawn(child, script.c_str(), actions, attributes, arguments,
                         environment);
    };
  }

  std::filesystem::path Marker() const {
    const auto identity = compat::ReadElfBuildId(paths.runtime_binary.string());
    EXPECT_TRUE(identity) << identity.error;
    return root / "rejections" /
           (newer_id + "-" + identity.build_id + "-" +
            std::string(
                CanaryGraphicsBackendName(request.canary_graphics_backend)) +
            ".txt");
  }

  std::string CurrentId() const {
    PayloadStore store(root, compatibility, paths.runtime_binary);
    const auto current = store.VerifyCurrent();
    EXPECT_TRUE(current) << current.error;
    return current.payload_id;
  }

  void RejectOnce() {
    WriteScript(false);
    request.canary_spawn = Spawn();
    const auto result = RunUpdate(paths, request);
    ASSERT_TRUE(result) << result.error;
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.payload_id, active_id);
    EXPECT_EQ(CurrentId(), active_id);
    EXPECT_EQ(spawns, 1);
    EXPECT_TRUE(std::filesystem::exists(Marker()));
    WriteScript();
  }

  CanaryOptions Options() const {
    CanaryOptions options;
    options.runtime_binary = paths.runtime_binary;
    options.payload_directory = root / "payloads" / newer_id;
    options.compatibility_manifest = compatibility;
    options.cache_root = paths.cache_root;
    options.state_root = paths.state_root;
    options.timeout_seconds = 2;
    return options;
  }

  UpdatePaths paths;
  UpdateRequest request;
  std::filesystem::path script;
  std::string active_id, newer_id;
  int spawns = 0;
};

TEST_P(UpdateCoordinatorTest, RetriesTheSameCandidateOnNextStartupAfterEagain) {
  request.canary_spawn = Spawn(EAGAIN);
  const auto first = RunUpdate(paths, request);
  ASSERT_TRUE(first) << first.error;
  EXPECT_FALSE(first.changed);
  EXPECT_EQ(first.payload_id, active_id);
  EXPECT_EQ(CurrentId(), active_id);
  EXPECT_EQ(spawns, 1);
  EXPECT_FALSE(std::filesystem::exists(Marker()));

  const auto second = RunUpdate(paths, request);
  ASSERT_TRUE(second) << second.error;
  EXPECT_TRUE(second.changed);
  EXPECT_EQ(second.payload_id, newer_id);
  EXPECT_EQ(CurrentId(), newer_id);
  EXPECT_EQ(spawns, 2);
  EXPECT_FALSE(std::filesystem::exists(Marker()));
}

TEST_P(UpdateCoordinatorTest, RealRejectionIsFilteredOnNextStartup) {
  RejectOnce();
  const auto second = RunUpdate(paths, request);
  ASSERT_TRUE(second) << second.error;
  EXPECT_FALSE(second.changed);
  EXPECT_EQ(second.payload_id, active_id);
  EXPECT_EQ(CurrentId(), active_id);
  EXPECT_EQ(spawns, 1);
  EXPECT_TRUE(std::filesystem::exists(Marker()));
  EXPECT_NE(second.message.find("already failed"), std::string::npos);
}

TEST_P(UpdateCoordinatorTest, DifferentBackendRetriesRejectedCandidate) {
  RejectOnce();
  const auto old_marker = Marker();
  request.canary_graphics_backend = CanaryGraphicsBackend::kOpenGlEs;
  EXPECT_FALSE(std::filesystem::exists(Marker()));
  const auto result = RunUpdate(paths, request);
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.changed);
  EXPECT_EQ(CurrentId(), newer_id);
  EXPECT_EQ(spawns, 2);
  EXPECT_TRUE(std::filesystem::exists(old_marker));
  EXPECT_FALSE(std::filesystem::exists(Marker()));
}

TEST_P(UpdateCoordinatorTest, DifferentRuntimeBuildIdRetriesRejectedCandidate) {
  RejectOnce();
  const auto old_marker = Marker();
  const auto original = compat::ReadElfBuildId(paths.runtime_binary.string());
  const auto different = std::filesystem::canonical("/usr/bin/true");
  const auto identity = compat::ReadElfBuildId(different.string());
  ASSERT_TRUE(original) << original.error;
  ASSERT_TRUE(identity) << identity.error;
  ASSERT_NE(identity.build_id, original.build_id);
  paths.runtime_binary = different;
  EXPECT_FALSE(std::filesystem::exists(Marker()));
  const auto result = RunUpdate(paths, request);
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.changed);
  EXPECT_EQ(CurrentId(), newer_id);
  EXPECT_EQ(spawns, 2);
  EXPECT_TRUE(std::filesystem::exists(old_marker));
  EXPECT_FALSE(std::filesystem::exists(Marker()));
}

TEST_P(UpdateCoordinatorTest, ExplicitUpdateCanRetryARejectedCandidate) {
  RejectOnce();
  request.startup_preflight = false;
  const auto result = RunUpdate(paths, request);
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.changed);
  EXPECT_EQ(result.payload_id, newer_id);
  EXPECT_EQ(CurrentId(), newer_id);
  EXPECT_EQ(spawns, 2);
}

TEST_P(UpdateCoordinatorTest, FirstInstallCanRetryARejectedCandidate) {
  RejectOnce();
  ASSERT_TRUE(std::filesystem::remove(root / "current.json"));
  const auto result = RunUpdate(paths, request);
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.changed);
  EXPECT_EQ(result.payload_id, newer_id);
  EXPECT_EQ(CurrentId(), newer_id);
  EXPECT_EQ(spawns, 2);
}

TEST_P(UpdateCoordinatorTest, NonEagainSpawnErrorKeepsExistingRejectionPolicy) {
  request.canary_spawn = Spawn(ENOEXEC);
  const auto first = RunUpdate(paths, request);
  ASSERT_TRUE(first) << first.error;
  EXPECT_FALSE(first.changed);
  EXPECT_EQ(first.payload_id, active_id);
  EXPECT_EQ(spawns, 1);
  EXPECT_TRUE(std::filesystem::exists(Marker()));
  request.canary_spawn = Spawn();
  const auto second = RunUpdate(paths, request);
  ASSERT_TRUE(second) << second.error;
  EXPECT_FALSE(second.changed);
  EXPECT_EQ(CurrentId(), active_id);
  EXPECT_EQ(spawns, 1);
}

TEST_P(UpdateCoordinatorTest, CanaryReportsSpawnEagainAndCleansItsWorkspace) {
  auto options = Options();
  options.spawn = Spawn(EAGAIN);
  const auto result = RunReadinessCanary(options);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.spawn_error, EAGAIN);
  EXPECT_EQ(result.exit_code, -1);
  EXPECT_FALSE(result.error.empty());
  EXPECT_EQ(spawns, 1);
  EXPECT_TRUE(std::filesystem::is_regular_file(result.log_path));
  EXPECT_EQ(CurrentId(), active_id);
  for (const auto& entry :
       std::filesystem::directory_iterator(paths.cache_root)) {
    EXPECT_NE(entry.path().filename().string().find(".native-update-canary"),
              0U);
  }
  EXPECT_FALSE(std::filesystem::exists(Marker()));
}

TEST_P(UpdateCoordinatorTest, CanaryFailureBeforeSpawnHasNoSpawnError) {
  auto options = Options();
  options.spawn = Spawn(EAGAIN);
  options.timeout_seconds = 0;
  const auto result = RunReadinessCanary(options);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.spawn_error, 0);
  EXPECT_EQ(result.exit_code, -1);
  EXPECT_FALSE(result.error.empty());
  EXPECT_EQ(spawns, 0);
}

INSTANTIATE_TEST_SUITE_P(Startup, UpdateCoordinatorTest,
                         ::testing::Values(false));

TEST(PayloadStoreTest, CollectsSupersededPayloadsAndKeepsTheRollbackTarget) {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.root() / "store";
  const std::string active = "2998-" + std::string(40, 'a');
  const std::string rollback = "2908-" + std::string(40, 'b');
  const std::string reference = "2628-" + std::string(40, 'c');
  const std::string superseded = "2546-" + std::string(40, 'd');
  const std::string abandoned = ".stage-" + superseded + "-4242";
  for (const std::string& id : {active, rollback, reference, superseded,
                                abandoned}) {
    Write(root / "payloads" / id / "libroblox.so", "payload bytes");
  }
  // Payloads are published read-only; the collector has to cope.
  ASSERT_EQ(chmod((root / "payloads" / superseded).c_str(), 0555), 0);
  Write(root / "current.json", ActivationManifestFixture(active));
  Write(root / "previous_good.json", ActivationManifestFixture(rollback));

  PayloadStore store(root, temporary.root() / "compatibility.json");
  const PayloadGarbageResult collected = store.CollectGarbage({reference});
  ASSERT_TRUE(collected) << collected.error;
  EXPECT_EQ(collected.removed.size(), 2U);
  EXPECT_GT(collected.freed_bytes, 0U);
  EXPECT_TRUE(std::filesystem::exists(root / "payloads" / active));
  EXPECT_TRUE(std::filesystem::exists(root / "payloads" / rollback));
  EXPECT_TRUE(std::filesystem::exists(root / "payloads" / reference));
  EXPECT_FALSE(std::filesystem::exists(root / "payloads" / superseded));
  EXPECT_FALSE(std::filesystem::exists(root / "payloads" / abandoned));
}

// #110: the metadata carries the moment of preparation, so restaging the same
// bytes quarantined the running payload, took it out of payloads/, and left
// the next launch to download it again.
TEST(PayloadStoreTest, RestagingIdenticalBytesKeepsTheInstalledPayload) {
  TemporaryDirectory temporary;
  const std::filesystem::path prepared = temporary.root() / "prepared";
  const std::filesystem::path restaged = temporary.root() / "restaged";
  Write(prepared / "libroblox.so", "native-library");
  Write(prepared / "sober_apk/base.apk", "base-apk");
  Write(prepared / "sober_apk" / compat::kGuestSplitApkFile, "split-apk");
  Write(prepared / "assets/content/fixture", "asset");
  std::string error;
  std::size_t asset_count = 0;
  const std::string asset_hash =
      HashAssetTree(prepared / "assets", &asset_count, &error);
  ASSERT_TRUE(error.empty()) << error;
  nlohmann::json metadata = {
      {"schema_version", 1},
      {"package", "com.roblox.client"},
      {"version_name", "2.727.1199"},
      {"version_code", 2628},
      {"elf_build_id", "1686400865ae0e408cd7bd67de7a439625c6fd13"},
      {"sha256",
       {{"libroblox", HashRegularFile(prepared / "libroblox.so")},
        {"base_apk", HashRegularFile(prepared / "sober_apk/base.apk")},
        {std::string(compat::kGuestSplitApkHashKey),
         HashRegularFile(prepared / "sober_apk" / compat::kGuestSplitApkFile)}}},
      {"assets", {{"file_count", asset_count}, {"sha256_tree", asset_hash}}},
      {"source", "apk-pure-native"},
      {"imported_at", "2026-09-04T03:49:16Z"},
  };
  Write(prepared / "roblox_payload.json", metadata.dump(2) + "\n");
  std::filesystem::copy(prepared, restaged,
                        std::filesystem::copy_options::recursive);
  metadata["imported_at"] = "2026-09-04T04:12:55Z";
  Write(restaged / "roblox_payload.json", metadata.dump(2) + "\n");

  const std::filesystem::path store_root = temporary.root() / "store";
  PayloadStore store(store_root, temporary.root() / "compatibility.json");
  const PayloadStoreResult initial = store.Stage(prepared);
  ASSERT_TRUE(initial) << initial.error;
  const auto written_at =
      std::filesystem::last_write_time(initial.payload_directory);

  const PayloadStoreResult again = store.Stage(restaged);
  ASSERT_TRUE(again) << again.error;
  EXPECT_EQ(again.payload_id, initial.payload_id);
  EXPECT_EQ(std::filesystem::last_write_time(initial.payload_directory),
            written_at);
  EXPECT_FALSE(std::filesystem::exists(store_root / "quarantine"));
}

TEST(PayloadStoreTest, CollectsNothingWhileTheStoreIsUnreadable) {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.root() / "store";
  const std::string superseded = "2546-" + std::string(40, 'd');
  Write(root / "payloads" / superseded / "libroblox.so", "payload bytes");
  Write(root / "current.json", "{not json\n");

  PayloadStore store(root, temporary.root() / "compatibility.json");
  const PayloadGarbageResult collected = store.CollectGarbage({});
  EXPECT_FALSE(collected);
  EXPECT_TRUE(std::filesystem::exists(root / "payloads" / superseded));
}

TEST(PayloadStoreTest, StagesAndPromotesVerifiedExactPayload) {
  TemporaryDirectory temporary;
  const std::filesystem::path prepared = temporary.root() / "prepared";
  Write(prepared / "libroblox.so", "native-library");
  Write(prepared / "sober_apk/base.apk", "base-apk");
  Write(prepared / "sober_apk" / compat::kGuestSplitApkFile, "split-apk");
  Write(prepared / "assets/content/fixture", "asset");
  std::string error;
  std::size_t asset_count = 0;
  const std::string asset_hash =
      HashAssetTree(prepared / "assets", &asset_count, &error);
  ASSERT_TRUE(error.empty()) << error;
  constexpr std::string_view kBuildId =
      "1686400865ae0e408cd7bd67de7a439625c6fd13";
  nlohmann::json metadata = {
      {"schema_version", 1},
      {"package", "com.roblox.client"},
      {"version_name", "2.727.1199"},
      {"version_code", 2628},
      {"elf_build_id", kBuildId},
      {"sha256",
       {{"libroblox", HashRegularFile(prepared / "libroblox.so")},
        {"base_apk", HashRegularFile(prepared / "sober_apk/base.apk")},
        {std::string(compat::kGuestSplitApkHashKey),
         HashRegularFile(prepared / "sober_apk" / compat::kGuestSplitApkFile)}}},
      {"assets", {{"file_count", asset_count}, {"sha256_tree", asset_hash}}},
  };
  Write(prepared / "roblox_payload.json", metadata.dump(2) + "\n");
  const std::filesystem::path compatibility =
      temporary.root() / "compatibility.json";
  Write(compatibility,
        "{\"schema_version\":1,\"profiles\":[{"
        "\"version_name\":\"2.727.1199\",\"version_code\":2628,"
        "\"elf_build_id\":\"1686400865ae0e408cd7bd67de7a439625c6fd13\","
        "\"status\":\"supported\",\"default_allowed\":true,"
        "\"allow_legacy_binary_patches\":false,\"abi\":\"" +
        std::string(compat::kGuestAbi) + "\"}]}\n");
  PayloadStore store(temporary.root() / "store", compatibility);
  const PayloadStoreResult staged = store.Stage(prepared);
  ASSERT_TRUE(staged) << staged.error;
  const PayloadStoreResult promoted = store.Promote(staged.payload_id);
  ASSERT_TRUE(promoted) << promoted.error;
  const PayloadStoreResult current = store.VerifyCurrent();
  ASSERT_TRUE(current) << current.error;
  EXPECT_EQ(current.payload_id,
            "2628-1686400865ae0e408cd7bd67de7a439625c6fd13");
  const nlohmann::json activation = nlohmann::json::parse(
      ReadFile(temporary.root() / "store/current.json"));
  EXPECT_EQ(activation["version_name"], metadata["version_name"]);
  EXPECT_EQ(activation["version_code"], metadata["version_code"]);
  EXPECT_EQ(activation["elf_build_id"], metadata["elf_build_id"]);
  EXPECT_EQ(activation["payload_sha256"],
            metadata["sha256"]["libroblox"]);

  nlohmann::json tampered_activation = activation;
  tampered_activation["payload_sha256"] = std::string(64, '0');
  Write(temporary.root() / "store/current.json",
        tampered_activation.dump(2) + "\n");
  const PayloadStoreResult tampered = store.VerifyCurrent();
  EXPECT_FALSE(tampered);
  EXPECT_EQ(tampered.error,
            "active payload manifest hash does not match payload");
}

TEST(PayloadStoreTest, RestagesReadOnlyCorruptPayloadCollision) {
  TemporaryDirectory temporary;
  const std::filesystem::path prepared = temporary.root() / "prepared";
  Write(prepared / "libroblox.so", "native-library");
  Write(prepared / "sober_apk/base.apk", "base-apk");
  Write(prepared / "sober_apk" / compat::kGuestSplitApkFile, "split-apk");
  Write(prepared / "assets/content/fixture", "asset");
  std::string error;
  std::size_t asset_count = 0;
  const std::string asset_hash =
      HashAssetTree(prepared / "assets", &asset_count, &error);
  ASSERT_TRUE(error.empty()) << error;
  constexpr std::string_view kBuildId =
      "1686400865ae0e408cd7bd67de7a439625c6fd13";
  const nlohmann::json metadata = {
      {"schema_version", 1},
      {"package", "com.roblox.client"},
      {"version_name", "2.727.1199"},
      {"version_code", 2628},
      {"elf_build_id", kBuildId},
      {"sha256",
       {{"libroblox", HashRegularFile(prepared / "libroblox.so")},
        {"base_apk", HashRegularFile(prepared / "sober_apk/base.apk")},
        {std::string(compat::kGuestSplitApkHashKey),
         HashRegularFile(prepared / "sober_apk" / compat::kGuestSplitApkFile)}}},
      {"assets", {{"file_count", asset_count}, {"sha256_tree", asset_hash}}},
  };
  Write(prepared / "roblox_payload.json", metadata.dump(2) + "\n");

  const std::filesystem::path store_root = temporary.root() / "store";
  PayloadStore store(store_root, temporary.root() / "compatibility.json");
  const PayloadStoreResult initial = store.Stage(prepared);
  ASSERT_TRUE(initial) << initial.error;

  const std::filesystem::path stored_metadata =
      initial.payload_directory / "roblox_payload.json";
  std::error_code filesystem_error;
  std::filesystem::permissions(stored_metadata,
                               std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add,
                               filesystem_error);
  ASSERT_FALSE(filesystem_error);
  Write(stored_metadata, "corrupt\n");
  std::filesystem::permissions(stored_metadata,
                               std::filesystem::perms::owner_read,
                               std::filesystem::perm_options::replace,
                               filesystem_error);
  ASSERT_FALSE(filesystem_error);

  const PayloadStoreResult recovered = store.Stage(prepared);
  ASSERT_TRUE(recovered) << recovered.error;
  EXPECT_TRUE(VerifyPreparedPayload(recovered.payload_directory));

  const std::filesystem::path quarantine = store_root / "quarantine";
  std::filesystem::directory_iterator iterator(quarantine, filesystem_error);
  ASSERT_FALSE(filesystem_error);
  ASSERT_NE(iterator, std::filesystem::directory_iterator());
  const auto quarantined_status = iterator->symlink_status(filesystem_error);
  ASSERT_FALSE(filesystem_error);
  EXPECT_EQ(quarantined_status.permissions() &
                std::filesystem::perms::owner_write,
            std::filesystem::perms::none);
  ++iterator;
  EXPECT_EQ(iterator, std::filesystem::directory_iterator());
}

// Promotion is separated from staging by the readiness canaries, which run the
// real client against the staged directory in a child process. A candidate
// modified in that window must never become current, so promotion rehashes the
// immutable bytes even though the very same instance staged them.
TEST(PayloadStoreTest, PromoteRehashesBytesTamperedAfterStaging) {
  TemporaryDirectory temporary;
  const std::filesystem::path prepared = temporary.root() / "prepared";
  Write(prepared / "libroblox.so", "native-library");
  Write(prepared / "sober_apk/base.apk", "base-apk");
  Write(prepared / "sober_apk" / compat::kGuestSplitApkFile, "split-apk");
  Write(prepared / "assets/content/fixture", "asset");
  std::string error;
  std::size_t asset_count = 0;
  const std::string asset_hash =
      HashAssetTree(prepared / "assets", &asset_count, &error);
  ASSERT_TRUE(error.empty()) << error;
  const nlohmann::json metadata = {
      {"schema_version", 1},
      {"package", "com.roblox.client"},
      {"version_name", "2.727.1199"},
      {"version_code", 2628},
      {"elf_build_id", "1686400865ae0e408cd7bd67de7a439625c6fd13"},
      {"sha256",
       {{"libroblox", HashRegularFile(prepared / "libroblox.so")},
        {"base_apk", HashRegularFile(prepared / "sober_apk/base.apk")},
        {std::string(compat::kGuestSplitApkHashKey),
         HashRegularFile(prepared / "sober_apk" / compat::kGuestSplitApkFile)}}},
      {"assets", {{"file_count", asset_count}, {"sha256_tree", asset_hash}}},
  };
  Write(prepared / "roblox_payload.json", metadata.dump(2) + "\n");
  const std::filesystem::path compatibility =
      temporary.root() / "compatibility.json";
  Write(compatibility,
        "{\"schema_version\":1,\"profiles\":[{"
        "\"version_name\":\"2.727.1199\",\"version_code\":2628,"
        "\"elf_build_id\":\"1686400865ae0e408cd7bd67de7a439625c6fd13\","
        "\"status\":\"supported\",\"default_allowed\":true,"
        "\"allow_legacy_binary_patches\":false,\"abi\":\"" +
        std::string(compat::kGuestAbi) + "\"}]}\n");

  const std::filesystem::path store_root = temporary.root() / "store";
  PayloadStore store(store_root, compatibility);
  const PayloadStoreResult staged = store.Stage(prepared);
  ASSERT_TRUE(staged) << staged.error;

  // Stands in for the canary interval. Content bytes only: the metadata still
  // describes the original payload, so nothing short of hashing the contents
  // can detect this.
  const std::filesystem::path asset =
      staged.payload_directory / "assets/content/fixture";
  std::error_code filesystem_error;
  std::filesystem::permissions(asset, std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add,
                               filesystem_error);
  ASSERT_FALSE(filesystem_error);
  Write(asset, "tampered");

  const PayloadStoreResult promoted = store.Promote(staged.payload_id);
  EXPECT_FALSE(promoted);
  EXPECT_EQ(promoted.error, "payload asset tree does not match metadata");
  EXPECT_FALSE(std::filesystem::exists(store_root / "current.json",
                                       filesystem_error));
}

// The same guarantee holds for a library modified in that window.
TEST(PayloadStoreTest, PromoteRejectsALibraryTamperedAfterStaging) {
  TemporaryDirectory temporary;
  const std::filesystem::path prepared = temporary.root() / "prepared";
  Write(prepared / "libroblox.so", "native-library");
  Write(prepared / "sober_apk/base.apk", "base-apk");
  Write(prepared / "sober_apk" / compat::kGuestSplitApkFile, "split-apk");
  Write(prepared / "assets/content/fixture", "asset");
  std::string error;
  std::size_t asset_count = 0;
  const std::string asset_hash =
      HashAssetTree(prepared / "assets", &asset_count, &error);
  ASSERT_TRUE(error.empty()) << error;
  const nlohmann::json metadata = {
      {"schema_version", 1},
      {"package", "com.roblox.client"},
      {"version_name", "2.727.1199"},
      {"version_code", 2628},
      {"elf_build_id", "1686400865ae0e408cd7bd67de7a439625c6fd13"},
      {"sha256",
       {{"libroblox", HashRegularFile(prepared / "libroblox.so")},
        {"base_apk", HashRegularFile(prepared / "sober_apk/base.apk")},
        {std::string(compat::kGuestSplitApkHashKey),
         HashRegularFile(prepared / "sober_apk" / compat::kGuestSplitApkFile)}}},
      {"assets", {{"file_count", asset_count}, {"sha256_tree", asset_hash}}},
  };
  Write(prepared / "roblox_payload.json", metadata.dump(2) + "\n");
  const std::filesystem::path compatibility =
      temporary.root() / "compatibility.json";
  Write(compatibility,
        "{\"schema_version\":1,\"profiles\":[{"
        "\"version_name\":\"2.727.1199\",\"version_code\":2628,"
        "\"elf_build_id\":\"1686400865ae0e408cd7bd67de7a439625c6fd13\","
        "\"status\":\"supported\",\"default_allowed\":true,"
        "\"allow_legacy_binary_patches\":false,\"abi\":\"" +
        std::string(compat::kGuestAbi) + "\"}]}\n");

  const std::filesystem::path store_root = temporary.root() / "store";
  PayloadStore store(store_root, compatibility);
  const PayloadStoreResult staged = store.Stage(prepared);
  ASSERT_TRUE(staged) << staged.error;

  const std::filesystem::path library =
      staged.payload_directory / "libroblox.so";
  std::error_code filesystem_error;
  std::filesystem::permissions(library, std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add,
                               filesystem_error);
  ASSERT_FALSE(filesystem_error);
  Write(library, "backdoored");

  const PayloadStoreResult promoted = store.Promote(staged.payload_id);
  EXPECT_FALSE(promoted);
  EXPECT_EQ(promoted.error, "payload hash mismatch: libroblox.so");
  EXPECT_FALSE(std::filesystem::exists(store_root / "current.json",
                                       filesystem_error));
}

TEST(PayloadStoreTest, EmptyResultCannotReportFalsePromotion) {
  const PayloadStoreResult result;
  EXPECT_FALSE(result);
}

TEST(PayloadStoreTest, RequiresTwoRuntimeBoundCanariesForLatestCandidate) {
  TemporaryDirectory temporary;
  const std::filesystem::path prepared = temporary.root() / "prepared";
  Write(prepared / "libroblox.so", "new-native-library");
  Write(prepared / "sober_apk/base.apk", "new-base-apk");
  Write(prepared / "sober_apk" / compat::kGuestSplitApkFile, "new-split-apk");
  Write(prepared / "assets/content/fixture", "new-asset");
  std::string error;
  std::size_t asset_count = 0;
  const std::string asset_hash =
      HashAssetTree(prepared / "assets", &asset_count, &error);
  ASSERT_TRUE(error.empty()) << error;
  constexpr std::string_view kBuildId =
      "a6c1f5c57f9d7aa3fb99a5fa30565e07e5c88f6a";
  const nlohmann::json metadata = {
      {"schema_version", 1},
      {"package", "com.roblox.client"},
      {"version_name", "2.732.1043"},
      {"version_code", 2814},
      {"abi", std::string(compat::kGuestAbi)},
      {"elf_build_id", kBuildId},
      {"sha256",
       {{"libroblox", HashRegularFile(prepared / "libroblox.so")},
        {"base_apk", HashRegularFile(prepared / "sober_apk/base.apk")},
        {std::string(compat::kGuestSplitApkHashKey),
         HashRegularFile(prepared / "sober_apk" / compat::kGuestSplitApkFile)}}},
      {"assets", {{"file_count", asset_count}, {"sha256_tree", asset_hash}}},
  };
  Write(prepared / "roblox_payload.json", metadata.dump(2) + "\n");
  const std::filesystem::path compatibility =
      temporary.root() / "repository-compatibility.json";
  Write(compatibility,
        "{\"schema_version\":1,\"profiles\":[{"
        "\"version_name\":\"2.727.1199\",\"version_code\":2628,"
        "\"elf_build_id\":\"1686400865ae0e408cd7bd67de7a439625c6fd13\","
        "\"status\":\"supported\",\"default_allowed\":true,"
        "\"allow_legacy_binary_patches\":false,\"abi\":\"" +
        std::string(compat::kGuestAbi) + "\"}]}\n");
  std::error_code filesystem_error;
  const std::filesystem::path runtime =
      std::filesystem::canonical("/proc/self/exe", filesystem_error);
  ASSERT_FALSE(filesystem_error);
  PayloadStore store(temporary.root() / "store", compatibility, runtime);
  const PayloadStoreResult staged = store.Stage(prepared);
  ASSERT_TRUE(staged) << staged.error;

  const std::string payload_id =
      "2814-a6c1f5c57f9d7aa3fb99a5fa30565e07e5c88f6a";
  const std::filesystem::path profile = temporary.root() / "candidate.json";
  Write(profile,
        nlohmann::json(
            {{"schema_version", 1},
             {"elf_build_id", kBuildId},
             {"payload_sha256", metadata["sha256"]["libroblox"]},
             {"payload_id", payload_id},
             {"payload_path", "payloads/" + payload_id},
             {"reference",
              {{"elf_build_id", "1686400865ae0e408cd7bd67de7a439625c6fd13"},
               {"payload_sha256",
                "3e9c26c81186f93458ff65d8a8bc240c220974db02b8"
                "388b91340b77b336997d"}}},
             {"profile", {{"elf_build_id", kBuildId}}},
             {"derivation_anchors", {{"signature_version", 1}}}})
                .dump(2) +
            "\n");
  const std::filesystem::path candidate_compatibility =
      temporary.root() / "candidate-compatibility.json";
  Write(candidate_compatibility,
        nlohmann::json(
            {{"schema_version", 1},
             {"profiles", nlohmann::json::array(
                              {{{"version_name", "2.732.1043"},
                                {"version_code", 2814},
                                {"elf_build_id", kBuildId},
                                {"status", "experimental"},
                                {"default_allowed", true},
                                {"allow_legacy_binary_patches", false},
                                {"allow_host_abi_bridges", true},
                                {"allow_host_constructor_replay", true}}})}})
                .dump(2) +
            "\n");
  const std::array<std::filesystem::path, 2> canary_logs = {
      temporary.root() / "canary-1.log",
      temporary.root() / "canary-2.log",
  };
  Write(canary_logs[0], "first independent Tier C pass\n");
  Write(canary_logs[1], "second independent Tier C pass\n");

  const PayloadStoreResult promoted = store.PromoteProbation(
      staged.payload_id, profile, candidate_compatibility, canary_logs);
  ASSERT_TRUE(promoted) << promoted.error;
  EXPECT_FALSE(promoted.approval_receipt.empty());
  const PayloadStoreResult current = store.VerifyCurrent();
  ASSERT_TRUE(current) << current.error;
  EXPECT_EQ(current.payload_id, payload_id);
  EXPECT_EQ(current.approval_receipt, promoted.approval_receipt);

  // A Mocktail rebuild does not invalidate a canary result for unchanged
  // Roblox bytes. The immutable receipt still binds the two original canary
  // runs, payload, profile, and compatibility manifest to one evidence set.
  nlohmann::json legacy_activation = nlohmann::json::parse(
      ReadFile(temporary.root() / "store/current.json"));
  legacy_activation.erase("payload_sha256");
  Write(temporary.root() / "store/current.json",
        legacy_activation.dump(2) + "\n");
  PayloadStore rebuilt_runtime_store(temporary.root() / "store", compatibility);
  const PayloadStoreResult cached = rebuilt_runtime_store.VerifyCurrent();
  ASSERT_TRUE(cached) << cached.error;
  EXPECT_EQ(cached.payload_id, payload_id);
  EXPECT_EQ(cached.approval_receipt, promoted.approval_receipt);
  const nlohmann::json activation = nlohmann::json::parse(
      ReadFile(temporary.root() / "store/current.json"));
  EXPECT_EQ(activation["payload_sha256"],
            metadata["sha256"]["libroblox"]);
}

}  // namespace
}  // namespace mocktail::update
