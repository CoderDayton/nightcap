#ifndef MOCKTAIL_VR_OPENXR_BACKEND_H_
#define MOCKTAIL_VR_OPENXR_BACKEND_H_

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <memory>

#include "mocktail/status.h"
#include "mocktail/vr/virtual_keyboard.h"
#include "mocktail/vr/vr_mirror.h"
#include "mocktail/vr/xr_actions.h"
#include "mocktail/vr/xr_controller.h"

namespace mocktail::vr {

namespace internal {
// Swapchain format for guest eyes of `eye` given the runtime's formats:
// `eye` itself, else its UNORM/SRGB sibling (a raw byte copy, since the guest
// writes sRGB-encoded bytes either way), else the other channel order with
// the same sRGB-ness (a blit that only swizzles). VK_FORMAT_UNDEFINED when
// only a gamma-converting blit remains.
VkFormat PreferredEyeSwapchainFormat(VkFormat eye,
                                     const std::vector<std::int64_t>& available);
// Consume binary present dependencies exactly once. On success a later present
// must omit them. VK_NOT_READY means nothing was consumed and the guest's
// present proceeds unchanged; any other failure consumed them (or lost the
// device) and must be propagated, not retried.
VkResult WaitForPresentDependencies(VkDevice device, VkQueue queue,
                                    const VkPresentInfoKHR& present,
                                    VkFence fence, PFN_vkResetFences reset,
                                    PFN_vkQueueSubmit submit,
                                    PFN_vkWaitForFences wait);
// Hands binary present dependencies to the GPU instead: one empty batch on
// `queue` waits on them and signals `signal`, which the present waits on
// instead. Work submitted to `queue` afterwards is ordered after the waits.
// The CPU never blocks. VK_NOT_READY means nothing was consumed.
VkResult ChainPresentDependencies(VkDevice device, VkQueue queue,
                                  const VkPresentInfoKHR& present,
                                  VkSemaphore signal, PFN_vkQueueSubmit submit);
// Makes the Vulkan adapter serialize queue calls for the rest of the process.
// Called when a backend arms; the adapter library may load before or after.
void RequestVulkanQueueSharing();
}  // namespace internal

// Resolved --vr output mode. kXrOutput drives the real Roblox stereo images
// into an OpenXR projection layer; kNativeOnly keeps the diagnostic native
// stereo prototype without any runtime requirement. Selected through
// MOCKTAIL_VR_BACKEND ("xr" default, "native" diagnostic).
enum class VrBackendMode {
  kDisabled,
  kNativeOnly,
  kXrOutput,
};

// Explicit XR_RUNTIME_JSON wins. Runtime preference can select SteamVR (ALVR),
// WiVRn, or the registered OpenXR runtime without changing system registration.
std::string SelectVrRuntimeManifest(const std::string& explicit_manifest,
                                   const std::vector<std::string>& candidates);
std::vector<std::string> VrRuntimeManifestCandidates(
    const std::string &preference, const std::string &home,
    const std::string &config_home, const std::string &config_dirs);
struct VrRuntimeDiscovery {
  std::vector<std::string> candidates;
  std::string reason;
};
VrRuntimeDiscovery DiscoverVrRuntime(
    const std::string& preference, const std::string& home,
    const std::string& config_home, const std::string& config_dirs,
    const std::string& runtime_dir, const std::string& proc_root = "/proc");
std::string VrRuntimeUnavailableHint(bool user_selected, const std::string& manifest);
VrBackendMode ResolveVrBackendMode(bool vr_enabled);
const char* VrBackendModeName(VrBackendMode mode);

// One deterministic head pose sample. Position is metres, orientation is a
// quaternion (x, y, z, w) in the OpenXR LOCAL reference space.
// VrHandPose (tracked controller hands) is defined in xr_controller.h.
struct ScriptedPoseSample {
  float position[3] = {0.f, 0.f, 0.f};
  float orientation[4] = {0.f, 0.f, 0.f, 1.f};
  float eye_offset[2][3] = {};  // metres in head space
  float eye_fov[2][4] = {};  // left, right, up, down angles
  // Index 0 = left, 1 = right (Roblox UserCFrame LeftHand/RightHand).
  VrHandPose hands[2];
  float floor_position[3] = {};
  float floor_orientation[4] = {0.f, 0.f, 0.f, 1.f};
  bool floor_valid = false;
  // Verified DebugDeviceVR input channels, published with the same pose.
  float controller_channels[28] = {};
  bool controllers_connected = false;
  std::uint64_t frame = 0;
  bool valid = false;
};

// Production OpenXR backend for the native stereo prototype.
//
// Lifecycle: main arms the backend before Roblox startup (xrCreateInstance
// and xrGetSystem fail fast with a clear message when no runtime exists).
// The Vulkan adapter delegates the guest's vkCreateInstance/vkCreateDevice
// through xrCreateVulkanInstanceKHR/xrCreateVulkanDeviceKHR so the runtime's
// extension and GPU requirements are satisfied by the very device Roblox
// renders with. The session is created on that device; per-frame XR work runs
// on the render thread from the adapter's host-present notification:
//   xrEndFrame(N) with the two eye images of frame N copied into the
//   projection swapchains, then xrWaitFrame/xrBeginFrame/xrLocateViews for
//   frame N+1 and publishing the scripted/runtime head pose that the device
//   bridge injects into Roblox before its eye passes run.
//
// EGL/OpenGL ES uses XR_MNDX_egl_enable on the guest SDL context and GPU
// framebuffer blits into GLES swapchain textures. It shares the same frame
// scheduling, tracking, pose application and session recovery below.
//
// Eye image provenance is recorded in the adapter during the guest eye
// initializer (owner = DebugDeviceVR object) and bound to an eye index only
// through the real per-frame eye-getter request plus the actual render-pass
// attachment, mirroring the validated evidence-layer semantics. Creation
// order or size alone never selects an eye.
//
// All public entry points are exception-free at their boundary; the
// implementation is compiled with exceptions like openxr_preview.cc and
// translates failures into Status/log outcomes.
class GlesTransport;
enum class VrGraphicsApi { kVulkan, kOpenGles };

class OpenXrBackend final {
 public:
  OpenXrBackend() = default;
  ~OpenXrBackend();
  OpenXrBackend(const OpenXrBackend&) = delete;
  OpenXrBackend& operator=(const OpenXrBackend&) = delete;

  // Claims the process-wide backend slot and creates XrInstance/system.
  Status Arm(VrGraphicsApi graphics_api = VrGraphicsApi::kVulkan);
  // EGL context must be current on the calling thread. Resolver bypasses guest
  // hooks.
  Status AttachGlesContext(void *display, void *config, void *context,
                           void *egl_get_proc, void *(*resolve)(const char *));
  void DetachGlesContext(void *context);
  void NoteGlesPresent();
  void *WrapGlesProcAddress(const char *name, void *raw);
  // Idempotent full teardown (XR resources first, then the instance).
  void Disarm();
  bool armed() const { return armed_.load(std::memory_order_acquire); }

  // Adapter delegation. Return false when the backend does not own creation
  // (caller falls back to the plain host loader path).
  bool CreateVulkanInstance(const VkInstanceCreateInfo* create_info,
                            const VkAllocationCallbacks* allocator,
                            VkInstance* instance, VkResult* result);
  bool CreateVulkanDevice(VkPhysicalDevice physical_device,
                          const VkDeviceCreateInfo* create_info,
                          const VkAllocationCallbacks* allocator,
                          VkDevice* device, VkResult* result);
  void NoteDeviceDestroyed(VkDevice device);
  void NoteInstanceDestroyed(VkInstance instance);
  void NoteQueue(VkDevice device, VkQueue queue, std::uint32_t family,
                 std::uint32_t index);
  std::vector<std::uint32_t> DesktopQueueFamilies(VkDevice device) const;
  void RecordCommandPool(VkDevice device, VkCommandPool pool,
                         std::uint32_t family);
  void RecordCommandBuffers(VkDevice device, VkCommandPool pool,
                            std::uint32_t count,
                            const VkCommandBuffer* buffers);
  void ForgetCommandBuffers(std::uint32_t count,
                            const VkCommandBuffer* buffers);
  void ForgetCommandPool(VkCommandPool pool);
  void ResetCommandPool(VkCommandPool pool);
  void ResetCommandBuffer(VkCommandBuffer buffer);
  void NoteCommandBuffersSubmitted(VkQueue queue, std::uint32_t count,
                                   const VkCommandBuffer* buffers);
  VkImageLayout NormalizeDesktopBarrier(VkCommandBuffer command, VkImage image,
                                        std::uint32_t* source,
                                        std::uint32_t* destination,
                                        VkImageLayout* old_layout,
                                        VkImageLayout new_layout);

  // Resource provenance recording (adapter wrappers).
  bool WantsResourceRecords() const {
    return recording_.load(std::memory_order_acquire);
  }
  void RecordImage(VkDevice device, VkImage image,
                   const VkImageCreateInfo* info);
  void RecordImageView(VkImageView view, VkImage image);
  void RecordFramebuffer(VkFramebuffer framebuffer,
                         const VkFramebufferCreateInfo* info);
  void RecordRenderPass(VkRenderPass render_pass,
                        const VkRenderPassCreateInfo* info);
  void NoteDestroyImage(VkImage image);
  void NoteDestroyImageView(VkImageView view);
  void NoteDestroyFramebuffer(VkFramebuffer framebuffer);
  void NoteRenderPassBegin(VkCommandBuffer command_buffer,
                           const VkRenderPassBeginInfo* info);
  bool EyeBindingComplete() const {
    return eyes_bound_.load(std::memory_order_acquire);
  }

  // Render-thread frame cycle at host present begin.
  void NoteHostPresent(VkQueue queue, VkDevice device);
  // VK_NOT_READY leaves the present untouched until eye submission provenance
  // is known. VK_SUCCESS means its waits were consumed on the graphics queue,
  // so the eye copies that follow there are ordered after them. The present
  // must then wait on *ready instead (none when it is VK_NULL_HANDLE).
  VkResult PrepareHostPresent(VkQueue queue, VkDevice device,
                              const VkPresentInfoKHR* info, VkSemaphore* ready);
  void RecordDesktopSwapchain(VkDevice device, VkSwapchainKHR swapchain,
                              const VkSwapchainCreateInfoKHR* info,
                              const VkImage* images, unsigned count,
                              bool converted_to_concurrent = false);
  // On success the original present semaphores have been consumed by the
  // mirror blit on the GPU. The caller must present waiting only on *ready,
  // which the blit signals. The CPU never waits for the guest's frame here.
  VkResult MirrorDesktop(VkQueue queue, const VkPresentInfoKHR* info,
                         VkSemaphore* ready);

  // Pose published for the frame that is about to render. The device bridge
  // reads it once per frame on the render thread and injects it into the
  // guest. valid=false until the XR session located a usable head pose.
  // hands[0]=left, hands[1]=right carry this frame's tracked controller grip
  // poses (metres, LOCAL space) when a controller layer is active.
  ScriptedPoseSample PublishedHeadPose() const;
  bool RecommendedEyeExtent(std::uint32_t* width, std::uint32_t* height) const;
  void NotePoseApplied(void* owner, std::uint64_t frame);

  // Controller input delivery, consumed by the window-thread input runtime
  // through the C ABI in xr_controller_abi.h. TakeControllerDelivery pops one
  // batch produced by the once-per-frame action sync; it returns false when
  // no controller layer is active or nothing is queued. The snapshot exposes
  // the current levels for overflow resync. Haptics requests are thread-safe.
  bool TakeControllerDelivery(ControllerDelivery* out);
  ControllerSnapshot ControllerSnapshotForFrame() const;
  void RequestControllerHaptics(int hand, float amplitude,
                                std::uint64_t duration_ns, float frequency_hz);

  void StopControllerHaptics(int hand);

  // Diagnostics.
  std::uint64_t submitted_frames() const {
    return submitted_frames_.load(std::memory_order_relaxed);
  }
  std::uint64_t skipped_frames() const {
    return skipped_frames_.load(std::memory_order_relaxed);
  }
  bool session_running() const {
    return session_running_.load(std::memory_order_relaxed);
  }
  std::string runtime_name() const;

 private:
  friend struct OpenXrBackendTestAccess;
  struct ImageRecord {
    VkImage image = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    void* owner = nullptr;
    std::uint64_t order = 0;
  };
  struct EyeBinding {
    bool valid = false;
    std::uint64_t frame = 0;
    VkImage image = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkImageLayout final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    void* owner = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
  };
  struct EyeSwapchain {
    void* swapchain = nullptr;  // XrSwapchain (opaque to keep the header XR-free)
    std::vector<VkImage> images;
    std::vector<std::uint32_t> gl_images;
    std::uint32_t acquired_index = 0;
    bool image_acquired = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
  };

  bool InitializeSessionLocked(VkPhysicalDevice physical_device,
                               VkDevice device,
                               const VkDeviceCreateInfo* create_info,
                               std::string* error);
  // Session-owned controller action layer (poses, buttons, axes, haptics).
  // Created at instance level, attached per session, detached on teardown so
  // a replacement session never reuses stale spaces.
  XrActions actions_;
  bool actions_created_ = false;
  // Shared by action sync, snapshots, haptics and lifecycle operations.
  // Acquired after mutex_ on the render thread, but never held across
  // xrWaitFrame; the input thread takes only this controller lock.
  mutable std::mutex controller_mutex_;
  // Syncs actions once per frame at the predicted display time and folds the
  // resulting hand poses into published_pose_ before the guest renders.
  void SyncControllersLocked(std::uint64_t predicted_display_time);
  bool InitializeGlesSessionLocked(std::string *error);
  bool InitializeSessionResourcesLocked(std::string *error);
  bool CopyGlesEyesIntoSwapchains();
  bool EnsureXrInstanceLocked(std::string* error = nullptr);
  void DestroyXrInstanceLocked();
  void DisarmLocked();
  void* ResolveFunction(const char* name) const;
  void SetupDeviceAfterCreationLocked(VkPhysicalDevice physical_device,
                                      VkDevice device,
                                      const VkDeviceCreateInfo* create_info);
  void TeardownSessionLocked(const char* reason, bool preserve_device = false);
  void HandleSessionState(int state);
  void HandleReferenceSpaceChange(std::int64_t change_time);
  bool RecoverSessionLocked(std::uint64_t now_ns);
  // False only when the runtime names a GPU other than the guest device's.
  bool RuntimeUsesGuestGpuLocked();
  void PollSessionEvents();
  bool SupportsPresentQueueLocked(VkQueue queue) const;
  VkQueue GraphicsQueueForPresentLocked(VkQueue present) const;
  bool unsupported_present_queue_logged_ = false;
  // Drops the pose/images of the frame currently in flight without touching
  // guest ownership, so the next xrEndFrame submits no layer instead of an
  // image paired with a pose that no longer describes the same origin.
  void InvalidateInFlightPose(const char* reason);
  void RunFrameCycle(VkQueue queue, VkDevice device);
  // Returns true when the backend owns this present but ran no XR frame, so
  // the caller paces the guest itself.
  bool NoteHostPresentLocked(VkQueue queue, VkDevice device);
  bool CreateSwapchains(std::string* error);
  // Recreates the eye swapchains once in the format that copies the guest's
  // eye bytes without a gamma change (see PreferredEyeSwapchainFormat).
  void MatchSwapchainFormatToEyesLocked();
  void DestroySwapchains();
  bool EnsureCopyResources(VkDevice device, std::uint32_t queue_family,
                           std::string* error);
  void DestroyCopyResources();
  // Records both eye copies (and, when the mirror is enabled, both staging
  // readbacks) into one command buffer and submits it once. Only the shared
  // memory mirror waits for it; the headset path leaves it to the GPU.
  bool CopyEyesIntoSwapchains(VkQueue queue);
  // Per-frame GPU work (desktop mirror, eye copies) is submitted without a CPU
  // wait. A slot is reused only after its fence signalled; its images let a
  // guest destroy wait for the submissions that still read them.
  struct AsyncSubmission {
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    std::uint64_t serial = 0;  // bumped per submission
    std::vector<VkImage> images;
  };
  // The shared-memory mirror's readback from the previous frame copy. It is
  // published at the next copy, when the GPU has long finished it, so neither
  // the headset nor the mirror makes the CPU wait for the frame.
  struct MirrorReadback {
    AsyncSubmission* slot = nullptr;
    std::uint64_t serial = 0;
    std::uint64_t frame = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
  };
  void PublishMirrorReadbackLocked();
  // The next slot, recorded-ready, or nullptr when its previous submission
  // did not finish in time (the device is hung or lost).
  AsyncSubmission* NextAsyncSubmissionLocked();
  VkResult SubmitAsyncLocked(VkQueue queue, AsyncSubmission* slot,
                             const VkSubmitInfo& submit);
  // Waits for pending submissions reading `image`, or all of them for
  // VK_NULL_HANDLE.
  void WaitAsyncSubmissionsLocked(VkImage image);
  // Semaphore the present of `image_index` waits on, created on first use.
  enum class PresentStep { kMirror, kPrepare };
  VkSemaphore PresentReadySemaphoreLocked(VkSwapchainKHR swapchain,
                                          std::uint32_t image_index,
                                          PresentStep step);
  void DestroyRetiredSemaphoresLocked();
  // In-headset keyboard: show/hide from the host's TextBox request, pointer
  // input, and trigger masking. Runs inside the controller sync.
  void UpdateVirtualKeyboardLocked();
  // Rasterizes and uploads the panel when it changed. Returns true when the
  // keyboard swapchain holds a released image ready for a quad layer.
  bool PrepareVirtualKeyboardLayerLocked(VkQueue queue);
  bool UploadVirtualKeyboardVulkan(VkQueue queue, VkImage image);
  void DestroyVirtualKeyboardStaging();
  void DestroyVirtualKeyboardResources();
  bool CaptureEyeEvidence(VkQueue queue, int eye);
  ScriptedPoseSample ComputeScriptedPose(std::uint64_t frame) const;
  void EnsureMirrorForExtent(std::uint32_t width, std::uint32_t height);
  MirrorVkProcs MirrorProcs() const;

  VrGraphicsApi graphics_api_ = VrGraphicsApi::kVulkan;
  GlesTransport *gles_ = nullptr;
  std::uint64_t gl_min_version_ = 0;
  std::uint64_t gl_max_version_ = 0;

  // Held for a whole XR frame cycle and by every path that can destroy the
  // session; always taken before mutex_.
  std::mutex frame_mutex_;
  mutable std::mutex mutex_;
  // The frame cycle's hold on mutex_, so it can release it around
  // xrWaitFrame. Only set while NoteHostPresent runs the cycle.
  std::unique_lock<std::mutex>* state_lock_ = nullptr;
  std::atomic<bool> armed_{false};
  std::atomic<bool> recording_{false};
  std::atomic<bool> eyes_bound_{false};
  std::atomic<bool> session_running_{false};
  std::atomic<std::uint64_t> submitted_frames_{0};
  std::atomic<std::uint64_t> skipped_frames_{0};

  // XR handles are stored as opaque pointers in the header; the implementation
  // casts them back. This keeps vulkan-only consumers free of OpenXR headers.
  void* xr_instance_ = nullptr;
  std::uint64_t xr_system_ = 0;
  void* xr_session_ = nullptr;
  void* xr_local_space_ = nullptr;
  void* xr_view_space_ = nullptr;
  void* xr_stage_space_ = nullptr;
  std::uint32_t xr_eye_width_[2] = {};
  std::uint32_t xr_eye_height_[2] = {};
  EyeSwapchain eye_swapchains_[2];
  VirtualKeyboard keyboard_;
  std::unique_ptr<VirtualKeyboardRasterizer> keyboard_raster_;
  EyeSwapchain keyboard_swapchain_;
  bool keyboard_swapchain_failed_ = false;
  bool keyboard_image_ready_ = false;
  std::uint64_t keyboard_uploaded_revision_ = 0;
  // A new showKeyboard request may open the panel; Close disarms it until
  // Roblox drops that request.
  bool keyboard_armed_ = true;
  pose::Pose keyboard_head_{};
  bool keyboard_head_valid_ = false;
  std::vector<std::uint8_t> keyboard_pixels_;
  VkBuffer keyboard_staging_ = VK_NULL_HANDLE;
  VkDeviceMemory keyboard_staging_memory_ = VK_NULL_HANDLE;
  void* keyboard_staging_mapped_ = nullptr;
  int xr_blend_mode_ = 0;
  // Runtime name captured at xrCreateInstance so the bind-time diagnostics line
  // can report it without re-entering runtime_name() under the held mutex.
  std::string runtime_name_;
  bool xr_instance_used_ = false;
  // Libraries the runtime's first instance loaded; later instances load them
  // with RTLD_DEEPBIND (see EnsureXrInstanceLocked).
  std::vector<std::string> runtime_libraries_;
  std::string user_manifest_;
  std::string runtime_manifest_;

  VkInstance vk_instance_ = VK_NULL_HANDLE;
  VkDevice vk_device_ = VK_NULL_HANDLE;
  VkPhysicalDevice vk_physical_device_ = VK_NULL_HANDLE;
  std::uint32_t vk_queue_family_ = 0;
  VkQueue vk_graphics_queue_ = VK_NULL_HANDLE;
  std::vector<std::uint32_t> device_queue_families_;
  void* vk_loader_ = nullptr;
  struct VkProcs;
  VkProcs* vk_ = nullptr;

  VkCommandPool copy_pool_ = VK_NULL_HANDLE;
  // Synchronous one-off work: keyboard uploads, evidence, present fallback.
  VkCommandBuffer copy_command_ = VK_NULL_HANDLE;
  VkFence copy_fence_ = VK_NULL_HANDLE;
  // Two submissions per frame, so three frames may be in flight.
  static constexpr std::size_t kAsyncSubmissions = 6;
  std::array<AsyncSubmission, kAsyncSubmissions> async_{};
  std::size_t next_async_ = 0;
  MirrorReadback mirror_readback_{};
  // The mirror's signal for this present, which PrepareHostPresent passes on.
  VkSemaphore mirror_ready_ = VK_NULL_HANDLE;
  // Semaphores of destroyed desktop swapchains. A present may still wait on
  // one, so they are destroyed only once the device is idle.
  std::vector<VkSemaphore> retired_semaphores_;
  std::unordered_map<VkFormat, VkFormatFeatureFlags> blit_features_;

  // Optional desktop two-eye mirror (MOCKTAIL_VR_MIRROR_SHM). Publishes the
  // latest complete pair through shared memory; inert when unset.
  VrMirror mirror_;

  std::unordered_map<VkImage, ImageRecord> images_;
  struct DesktopSwapchain {
    VkExtent2D extent;
    VkFormat format;
    std::vector<VkImage> images;
    VkSharingMode sharing_mode = VK_SHARING_MODE_EXCLUSIVE;
    std::vector<std::uint32_t> families;
    bool converted_to_concurrent = false;
    // Two per image, see PresentReadySemaphoreLocked.
    std::vector<VkSemaphore> ready;
  };
  std::unordered_map<VkSwapchainKHR, DesktopSwapchain> desktop_swapchains_;
  std::unordered_map<VkImageView, VkImage> image_views_;
  std::unordered_map<VkFramebuffer, std::vector<VkImageView>> framebuffer_views_;
  std::unordered_map<VkFramebuffer, VkRenderPass> framebuffer_render_pass_;
  std::unordered_map<VkRenderPass, std::vector<VkImageLayout>> render_pass_final_layouts_;
  std::unordered_map<VkQueue, std::uint32_t> queue_families_;
  struct CommandRecord {
    VkCommandPool pool = VK_NULL_HANDLE;
    std::uint32_t family = UINT32_MAX;
    std::vector<std::pair<VkImage, std::uint64_t>> eye_images;
  };
  std::unordered_map<VkCommandPool, std::uint32_t> command_pool_families_;
  std::unordered_map<VkCommandBuffer, CommandRecord> command_records_;
  std::unordered_map<VkImage, VkQueue> eye_render_queues_;
  std::unordered_map<VkImage, std::uint64_t> eye_render_frames_;
  std::uint64_t image_order_ = 0;
  EyeBinding eyes_[2];

  // Frame cycle state (render thread only).
  bool frame_open_ = false;
  bool frame_should_render_ = false;
  // A running session is hidden in SYNCHRONIZED; shouldRender still governs rendering.
  bool visibility_lost_ = false;
  bool session_recovery_pending_ = false;
  std::uint64_t next_session_retry_ns_ = 0;
  // Consecutive failed session recoveries, for backoff and for deciding when
  // the XrInstance itself must be recreated (e.g. after a runtime restart).
  std::uint32_t session_recovery_failures_ = 0;
  bool xr_instance_stale_ = false;
  bool present_sync_failure_logged_ = false;
  bool gles_attach_failed_ = false;
  // Runtime swapchain formats, and the format chosen once the guest eye
  // format is known (VK_FORMAT_UNDEFINED until then).
  std::vector<std::int64_t> xr_swapchain_formats_;
  VkFormat eye_swapchain_format_ = VK_FORMAT_UNDEFINED;
  VkFormat eye_format_checked_ = VK_FORMAT_UNDEFINED;
  std::uint64_t frame_display_time_ = 0;
  // MOCKTAIL_VR_UNPACED benchmark mode: guest presents before this monotonic
  // time leave the open XR frame alone instead of blocking in xrWaitFrame.
  std::uint64_t frame_due_ns_ = 0;
  struct ViewState {
    float position[3];
    float orientation[4];
    float fov[4];  // angleLeft, angleRight, angleUp, angleDown (radians)
    bool valid;
  };
  ViewState frame_views_[2] = {};
  struct PoseStorage {
    float position[3] = {0.f, 0.f, 0.f};
    float orientation[4] = {0.f, 0.f, 0.f, 1.f};
  };
  PoseStorage frame_view_poses_[2];
  bool frame_views_valid_ = false;
  // One-shot guard so a canted-view runtime reports the rejection reason once
  // instead of once per frame per eye.
  bool projection_rejection_logged_ = false;
  mutable ScriptedPoseSample published_pose_;
  std::uint64_t xr_frame_counter_ = 0;
  std::unordered_map<void*, std::uint64_t> applied_poses_;
};

// Process-wide single backend owned by main. Adapter hooks resolve these
// exported C symbols; they are inert while no backend is armed.
OpenXrBackend* ActiveVrBackend();

// Paths of the shared objects mapped into the process.
std::vector<std::string> LoadedSharedObjects();
// Paths mapped now that were not in `before`.
std::vector<std::string> SharedObjectsLoadedSince(
    const std::vector<std::string>& before);

// Holds the runtime's libraries open with RTLD_DEEPBIND while the OpenXR
// loader loads the runtime, so its Vulkan calls bind to its own libvulkan
// dependency instead of the Vulkan adapter's exported hooks. Released after
// the loader holds its own reference, so the runtime still unloads with its
// instance.
class DeepBoundLibraries {
 public:
  explicit DeepBoundLibraries(const std::vector<std::string>& paths);
  ~DeepBoundLibraries();
  DeepBoundLibraries(const DeepBoundLibraries&) = delete;
  DeepBoundLibraries& operator=(const DeepBoundLibraries&) = delete;

 private:
  std::vector<void*> handles_;
};

}  // namespace mocktail::vr

#endif  // MOCKTAIL_VR_OPENXR_BACKEND_H_
