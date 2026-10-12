#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <cstdlib>
#include "mocktail/vr/vr_perf.h"
#include "mocktail/vr/openxr_backend.h"
#include <fstream>
#include <string>
#include <vector>

#include "mocktail/vr/vr_mirror.h"
#include "mocktail/vr/vr_mirror_shm.h"

namespace mocktail::vr {
namespace {

constexpr std::uint32_t kWidth = 8;
constexpr std::uint32_t kHeight = 4;

std::vector<std::uint8_t> EyePattern(std::uint8_t seed) {
  std::vector<std::uint8_t> bytes(kWidth * kHeight * 4);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return bytes;
}

// Maps the segment read-only exactly like the Python viewer does.
class SegmentReader {
 public:
  SegmentReader(const std::string& name, std::uint32_t width,
                std::uint32_t height, bool* ok) {
    *ok = false;
    const int descriptor = shm_open(name.c_str(), O_RDONLY, 0);
    if (descriptor < 0) {
      return;
    }
    bytes_ = VrMirrorBytes(width, height);
    mapping_ = mmap(nullptr, bytes_, PROT_READ, MAP_SHARED, descriptor, 0);
    close(descriptor);
    *ok = mapping_ != MAP_FAILED;
  }
  ~SegmentReader() {
    if (mapping_ != nullptr && mapping_ != MAP_FAILED) {
      munmap(mapping_, bytes_);
    }
  }
  const VrMirrorHeader& header() const {
    return *static_cast<const VrMirrorHeader*>(mapping_);
  }
  const std::uint8_t* eye(int index) const {
    return static_cast<const std::uint8_t*>(mapping_) + kVrMirrorHeaderBytes +
           static_cast<std::size_t>(index) * kWidth * kHeight * 4;
  }
  SegmentReader(const SegmentReader&) = delete;
  SegmentReader& operator=(const SegmentReader&) = delete;

 private:
  void* mapping_ = nullptr;
  std::uint64_t bytes_ = 0;
};

TEST(VrRuntimeSelection, RuntimeErrorsPreserveSelectionProvenance) {
  EXPECT_NE(VrRuntimeUnavailableHint(true, "/custom.json").find("User-selected"), std::string::npos);
  EXPECT_NE(
      VrRuntimeUnavailableHint(false, "/usr/share/openxr/1/openxr_wivrn.json")
          .find("Selected OpenXR"),
      std::string::npos);
  EXPECT_NE(VrRuntimeUnavailableHint(false, "").find("registered"), std::string::npos);
}

TEST(VrRuntimeSelection, ExplicitManifestWinsAndMissingCandidatesUseLoader) {
  EXPECT_EQ(SelectVrRuntimeManifest("/explicit/runtime.json", {"/dev/null"}),
            "/explicit/runtime.json");
  EXPECT_TRUE(SelectVrRuntimeManifest("", {"/definitely-missing-wivrn/runtime.json"}).empty());
  char path[] = "/tmp/mocktail-wivrn-manifest-XXXXXX";
  const int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);
  EXPECT_EQ(SelectVrRuntimeManifest("", {"/missing/runtime.json", path}), path);
  unlink(path);
}

TEST(VrMirrorProducer, AllocationFailureUsesOwningDevice) {
  MirrorVkProcs vk{};
  vk.GetPhysicalDeviceMemoryProperties = [](VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* out) {
    *out = {};
    out->memoryTypeCount = 1;
    out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  };
  vk.CreateBuffer = [](VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer* out) {
    *out = reinterpret_cast<VkBuffer>(0x22);
    return VK_SUCCESS;
  };
  vk.GetBufferMemoryRequirements = [](VkDevice, VkBuffer, VkMemoryRequirements* out) {
    *out = {}; out->size = 256; out->memoryTypeBits = 1;
  };
  vk.AllocateMemory = [](VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory*) {
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  };
  vk.DestroyBuffer = [](VkDevice device, VkBuffer, const VkAllocationCallbacks*) {
    EXPECT_EQ(device, reinterpret_cast<VkDevice>(0x11));
  };
  VrMirror mirror;
  std::string error;
  EXPECT_FALSE(mirror.EnsureStaging(vk, reinterpret_cast<VkDevice>(0x11),
                                   VK_NULL_HANDLE, 8, 8, &error));
}

TEST(VrMirrorPerf, RingAverageDoesNotAccumulateOverwrittenSamples) {
  setenv("MOCKTAIL_VR_PERF", "1", 1);
  unsetenv("MOCKTAIL_VR_MEASURE_START_NS");
  perf::Collector collector;
  for (int i = 0; i < 8192; ++i) collector.Record(perf::Stage::kFrameCycle, 1000000);
  testing::internal::CaptureStderr();
  collector.EmitIfDue(1, "test");
  collector.EmitIfDue(1000000001, "test");
  const auto output = testing::internal::GetCapturedStderr();
  EXPECT_NE(output.find("avg=1.00ms"), std::string::npos) << output;
}

TEST(VrMirrorProducer, PublishesLatestPairUnderSeqlockAndUnlinksOnClose) {
  const std::string name = "/mocktail-vr-mirror-unittest-a";
  {
    VrMirror mirror;
    std::string error;
    ASSERT_TRUE(mirror.Open(name, kWidth, kHeight, &error)) << error;
    EXPECT_TRUE(mirror.enabled());

    bool opened = false;
    SegmentReader reader(name, kWidth, kHeight, &opened);
    ASSERT_TRUE(opened);
    EXPECT_EQ(reader.header().magic, kVrMirrorMagic);
    EXPECT_EQ(reader.header().header_bytes, kVrMirrorHeaderBytes);
    EXPECT_EQ(reader.header().seq, 0u);
    EXPECT_EQ(reader.header().flags & kVrMirrorFlagValid, 0u);

    const auto eye0 = EyePattern(10);
    const auto eye1 = EyePattern(90);
    mirror.PublishFromBytesForTest(41, VK_FORMAT_R8G8B8A8_UNORM, kWidth,
                                   kHeight, 12345, eye0.data(), eye1.data());

    // Even sequence: the pair is stable for readers.
    EXPECT_EQ(reader.header().seq % 2, 0u);
    EXPECT_EQ(reader.header().seq, 2u);
    EXPECT_EQ(reader.header().frame, 41u);
    EXPECT_EQ(reader.header().producer_ns, 12345u);
    EXPECT_EQ(reader.header().width, kWidth);
    EXPECT_EQ(reader.header().height, kHeight);
    EXPECT_EQ(reader.header().flags & kVrMirrorFlagValid, kVrMirrorFlagValid);
    EXPECT_EQ(reader.header().published_pairs, 1u);
    EXPECT_EQ(std::memcmp(reader.eye(0), eye0.data(), eye0.size()), 0);
    EXPECT_EQ(std::memcmp(reader.eye(1), eye1.data(), eye1.size()), 0);
    // Eyes must never alias into a single buffer.
    EXPECT_NE(std::memcmp(reader.eye(0), reader.eye(1), eye0.size()), 0);

    // A newer publish replaces the single latest slot; readers see only the
    // newest pair and the sequence advances by two per publish.
    const auto next0 = EyePattern(33);
    const auto next1 = EyePattern(77);
    mirror.PublishFromBytesForTest(42, VK_FORMAT_B8G8R8A8_UNORM, kWidth,
                                   kHeight, 54321, next0.data(), next1.data());
    EXPECT_EQ(reader.header().seq, 4u);
    EXPECT_EQ(reader.header().frame, 42u);
    EXPECT_EQ(reader.header().format,
              static_cast<std::uint32_t>(VK_FORMAT_B8G8R8A8_UNORM));
    EXPECT_EQ(std::memcmp(reader.eye(0), next0.data(), next0.size()), 0);
    EXPECT_EQ(std::memcmp(reader.eye(1), next1.data(), next1.size()), 0);
    EXPECT_EQ(mirror.published_pairs(), 2u);

    mirror.Close();
    EXPECT_FALSE(mirror.enabled());
  }
  // The producer owns the segment lifetime: after Close the name is unlinked.
  EXPECT_EQ(shm_open(name.c_str(), O_RDONLY, 0), -1);
}

TEST(VrMirrorProducer, RejectsInvalidNameAndOversizedPublish) {
  VrMirror mirror;
  std::string error;
  EXPECT_FALSE(mirror.Open("", kWidth, kHeight, &error));
  EXPECT_FALSE(mirror.Open("relative-name", kWidth, kHeight, &error));
  EXPECT_FALSE(mirror.Open("/mocktail-vr-mirror-unittest-b", 0, kHeight,
                           &error));
  EXPECT_FALSE(mirror.enabled());

  const std::string name = "/mocktail-vr-mirror-unittest-c";
  ASSERT_TRUE(mirror.Open(name, kWidth, kHeight, &error)) << error;
  // Publishing an extent larger than the mapped segment is refused, not
  // truncated into foreign memory.
  const auto big0 = std::vector<std::uint8_t>(kWidth * kHeight * 4 * 4, 7);
  mirror.PublishFromBytesForTest(1, VK_FORMAT_R8G8B8A8_UNORM, kWidth * 2,
                                 kHeight * 2, 0, big0.data(), big0.data());
  EXPECT_EQ(mirror.published_pairs(), 0u);
  mirror.Close();
}

}  // namespace
}  // namespace mocktail::vr

namespace mocktail::vr {
TEST(VrRuntimeSelection, AlvrSelectsSteamVrWithoutFallingBackToWiVRn) {
  const auto alvr = VrRuntimeManifestCandidates("alvr", "/home/test", "", "");
  EXPECT_EQ(alvr, VrRuntimeManifestCandidates("steamvr", "/home/test", "", ""));
  ASSERT_FALSE(alvr.empty());
  for (const auto &path : alvr) {
    EXPECT_NE(path.find("SteamVR/steamxr_linux64.json"), std::string::npos);
    EXPECT_EQ(path.find("wivrn"), std::string::npos);
  }
  EXPECT_TRUE(
      VrRuntimeManifestCandidates("system", "/home/test", "", "").empty());
  EXPECT_EQ(SelectVrRuntimeManifest("/custom/steamxr_linux64.json", alvr),
            "/custom/steamxr_linux64.json");
}
TEST(VrRuntimeSelection, RegisteredRuntimePrecedesInstalledWiVRn) {
  const auto paths = VrRuntimeManifestCandidates(
      "auto", "/home/test", "/private/config", "/opt/xdg:/etc/xdg");
  ASSERT_GE(paths.size(), 4u);
  EXPECT_EQ(paths[0], "/private/config/openxr/1/active_runtime.x86_64.json");
  EXPECT_EQ(paths[1], "/private/config/openxr/1/active_runtime.json");
  EXPECT_EQ(paths[2], "/opt/xdg/openxr/1/active_runtime.x86_64.json");
  EXPECT_EQ(paths.back(), "/usr/share/openxr/1/openxr_wivrn.json");
}
} // namespace mocktail::vr
