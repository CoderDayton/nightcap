// Test-only implicit Vulkan layer that produces GPU-level evidence for the
// experimental native stereo bring-up:
//   - records images only inside the bridge's guest eye initializer;
//   - identifies left/right from the guest eye getter and the subsequent
//     render pass's actual attachments, checking the initializer's owner;
//   - counts vkCmdBeginRenderPass invocations per eye framebuffer (proof that
//     the Roblox renderer issues separate GPU passes into both eyes);
//   - at a configurable present index, reads both eye images back on the
//     presenting queue and writes eye0.ppm / eye1.ppm plus summary.txt into
//     MOCKTAIL_VR_EVIDENCE_DIR, including a pixel-difference verdict that
//     distinguishes real per-eye cameras from a duplicated single image.
//
// The layer is inert unless MOCKTAIL_VR_EVIDENCE_DIR is set. It never aborts
// the application: every evidence operation is best-effort and logged with
// the [vr-evidence-layer] prefix.
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <dlfcn.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__GNUC__)
#define MOCKTAIL_VR_LAYER_EXPORT __attribute__((visibility("default")))
#else
#define MOCKTAIL_VR_LAYER_EXPORT
#endif

namespace {

void Log(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  std::vfprintf(stderr, format, arguments);
  va_end(arguments);
  std::fflush(stderr);
}

std::string EvidenceDirectory() {
  const char *value = std::getenv("MOCKTAIL_VR_EVIDENCE_DIR");
  return value != nullptr ? std::string(value) : std::string();
}

uint32_t EnvUnsigned(const char *name, uint32_t fallback) {
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  const long parsed = std::strtol(value, nullptr, 10);
  return parsed > 0 ? static_cast<uint32_t>(parsed) : fallback;
}

const void *FindInChain(const void *chain, VkStructureType type) {
  // The loader inserts several nodes sharing the same sType (link info,
  // data callbacks, feature flags); only the VK_LAYER_LINK_INFO variant
  // carries the pLayerInfo chain a layer must consume.
  for (const auto *header = static_cast<const VkBaseInStructure *>(chain);
       header != nullptr; header = header->pNext) {
    if (header->sType != type) {
      continue;
    }
    const auto *link =
        reinterpret_cast<const VkLayerInstanceCreateInfo *>(header);
    if (link->function == VK_LAYER_LINK_INFO) {
      return header;
    }
  }
  return nullptr;
}

struct DeviceDispatch {
  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
  PFN_vkDestroyDevice destroy_device = nullptr;
  PFN_vkGetDeviceQueue get_device_queue = nullptr;
  PFN_vkCreateImage create_image = nullptr;
  PFN_vkDestroyImage destroy_image = nullptr;
  PFN_vkDestroyFramebuffer destroy_framebuffer = nullptr;
  PFN_vkCreateImageView create_image_view = nullptr;
  PFN_vkCreateFramebuffer create_framebuffer = nullptr;
  PFN_vkCreateRenderPass create_render_pass = nullptr;
  PFN_vkCmdBeginRenderPass cmd_begin_render_pass = nullptr;
  PFN_vkQueuePresentKHR queue_present = nullptr;
  PFN_vkQueueSubmit queue_submit = nullptr;
  PFN_vkCreateCommandPool create_command_pool = nullptr;
  PFN_vkAllocateCommandBuffers allocate_command_buffers = nullptr;
  PFN_vkBeginCommandBuffer begin_command_buffer = nullptr;
  PFN_vkEndCommandBuffer end_command_buffer = nullptr;
  PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = nullptr;
  PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer = nullptr;
  PFN_vkCreateFence create_fence = nullptr;
  PFN_vkWaitForFences wait_for_fences = nullptr;
  PFN_vkDestroyFence destroy_fence = nullptr;
  PFN_vkCreateBuffer create_buffer = nullptr;
  PFN_vkDestroyBuffer destroy_buffer = nullptr;
  PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = nullptr;
  PFN_vkAllocateMemory allocate_memory = nullptr;
  PFN_vkFreeMemory free_memory = nullptr;
  PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
  PFN_vkMapMemory map_memory = nullptr;
  PFN_vkUnmapMemory unmap_memory = nullptr;
  PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties memory_properties = nullptr;
};

struct ImageRecord {
  VkImage image = VK_NULL_HANDLE;
  DeviceDispatch *dispatch = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageUsageFlags usage = 0;
  void *owner = nullptr;
};

struct EyeTarget {
  bool valid = false;
  ImageRecord image{};
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkImageLayout last_final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  std::uint64_t render_pass_count = 0;
};

bool g_active = false;
uint32_t g_capture_present = 60;
uint32_t g_capture_render_passes = 10;
std::mutex g_mutex;
std::unordered_map<void *, DeviceDispatch> g_dispatches;
std::unordered_map<void *, uint32_t> g_queue_families;
std::unordered_map<VkRenderPass, VkImageLayout> g_render_pass_final_layouts;
std::unordered_map<VkImageView, VkImage> g_image_views;
std::unordered_map<VkImage, ImageRecord> g_images;
std::unordered_map<VkFramebuffer, std::vector<VkImage>> g_framebuffer_images;
using InitializingObjectFn = void *(*)();
using CurrentEyeFn = bool (*)(void **, int *, void **);
using ConsumeEyeFn = void (*)();
InitializingObjectFn g_initializing_object = nullptr;
CurrentEyeFn g_current_eye = nullptr;
ConsumeEyeFn g_consume_eye = nullptr;
EyeTarget g_eyes[2];
std::uint64_t g_present_count = 0;
bool g_capture_attempted = false;
PFN_vkGetInstanceProcAddr g_instance_gipa = nullptr;
VkInstance g_instance = VK_NULL_HANDLE;

void *DispatchKey(void *handle) {
  return handle != nullptr ? *reinterpret_cast<void **>(handle) : nullptr;
}

DeviceDispatch *DispatchFor(void *handle) {
  void *key = DispatchKey(handle);
  if (key == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto entry = g_dispatches.find(key);
  return entry != g_dispatches.end() ? &entry->second : nullptr;
}

bool IsColorFormat(VkFormat format) {
  switch (format) {
  case VK_FORMAT_D16_UNORM:
  case VK_FORMAT_D16_UNORM_S8_UINT:
  case VK_FORMAT_D24_UNORM_S8_UINT:
  case VK_FORMAT_D32_SFLOAT:
  case VK_FORMAT_D32_SFLOAT_S8_UINT:
  case VK_FORMAT_X8_D24_UNORM_PACK32:
    return false;
  default:
    return true;
  }
}

uint32_t BytesPerPixel(VkFormat format) {
  switch (format) {
  case VK_FORMAT_R8G8B8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
  case VK_FORMAT_B8G8R8A8_UNORM:
  case VK_FORMAT_B8G8R8A8_SRGB:
  case VK_FORMAT_R8G8B8A8_UINT:
  case VK_FORMAT_R32_SFLOAT:
    return 4;
  default:
    return 0;
  }
}

bool IsBgra(VkFormat format) {
  return format == VK_FORMAT_B8G8R8A8_UNORM ||
         format == VK_FORMAT_B8G8R8A8_SRGB;
}

bool WritePpm(const std::string &path, const uint8_t *pixels, uint32_t width,
              uint32_t height, bool bgra) {
  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  std::fprintf(file, "P6\n%u %u\n255\n", width, height);
  for (uint32_t row = 0; row < height; ++row) {
    for (uint32_t column = 0; column < width; ++column) {
      const uint8_t *source =
          pixels + (static_cast<size_t>(row) * width + column) * 4;
      const uint8_t red = bgra ? source[2] : source[0];
      const uint8_t green = source[1];
      const uint8_t blue = bgra ? source[0] : source[2];
      const uint8_t rgb[3] = {red, green, blue};
      std::fwrite(rgb, 1, sizeof(rgb), file);
    }
  }
  std::fclose(file);
  return true;
}

int FindMemoryType(const DeviceDispatch &dispatch, uint32_t requirements,
                   VkMemoryPropertyFlags properties) {
  if (dispatch.memory_properties == nullptr) {
    return -1;
  }
  VkPhysicalDeviceMemoryProperties memory{};
  dispatch.memory_properties(dispatch.physical_device, &memory);
  for (uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
    if ((requirements & (1u << index)) != 0 &&
        (memory.memoryTypes[index].propertyFlags & properties) == properties) {
      return static_cast<int>(index);
    }
  }
  return -1;
}

uint32_t QueueFamilyFor(VkQueue queue) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto entry = g_queue_families.find(DispatchKey(queue));
  return entry != g_queue_families.end() ? entry->second : 0;
}

void CaptureEyes(DeviceDispatch &dispatch, VkQueue queue) {
  // Runs on the presenting (render) thread after the frame that filled both
  // eyes was submitted; g_mutex is NOT held here except through helpers.
  EyeTarget eyes[2];
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    eyes[0] = g_eyes[0];
    eyes[1] = g_eyes[1];
  }
  if (!eyes[0].valid || !eyes[1].valid || eyes[0].image.dispatch != &dispatch ||
      eyes[1].image.dispatch != &dispatch ||
      eyes[0].image.owner != eyes[1].image.owner ||
      eyes[0].image.width != eyes[1].image.width ||
      eyes[0].image.height != eyes[1].image.height) {
    Log("  [vr-evidence-layer] capture skipped: eye targets unknown\n");
    return;
  }
  if (BytesPerPixel(eyes[0].image.format) != 4 ||
      BytesPerPixel(eyes[1].image.format) != 4) {
    Log("  [vr-evidence-layer] capture skipped: unsupported eye formats "
        "%d/%d\n",
        static_cast<int>(eyes[0].image.format),
        static_cast<int>(eyes[1].image.format));
    return;
  }
  const uint32_t width = eyes[0].image.width;
  const uint32_t height = eyes[0].image.height;
  const VkDeviceSize buffer_size =
      static_cast<VkDeviceSize>(width) * height * 4;

  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = QueueFamilyFor(queue);
  if (dispatch.create_command_pool(dispatch.device, &pool_info, nullptr,
                                   &pool) != VK_SUCCESS) {
    Log("  [vr-evidence-layer] capture: command pool creation failed\n");
    return;
  }

  VkCommandBuffer command = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo allocate_info{};
  allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocate_info.commandPool = pool;
  allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_info.commandBufferCount = 1;
  if (dispatch.allocate_command_buffers(dispatch.device, &allocate_info,
                                        &command) != VK_SUCCESS) {
    dispatch.destroy_command_pool(dispatch.device, pool, nullptr);
    Log("  [vr-evidence-layer] capture: command buffer allocation failed\n");
    return;
  }

  VkBuffer buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDeviceMemory memories[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  void *mapped[2] = {nullptr, nullptr};
  VkFence fence = VK_NULL_HANDLE;
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  bool prepared = dispatch.create_fence(dispatch.device, &fence_info, nullptr,
                                        &fence) == VK_SUCCESS;
  for (int index = 0; prepared && index < 2; ++index) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = buffer_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    prepared = dispatch.create_buffer(dispatch.device, &buffer_info, nullptr,
                                      &buffers[index]) == VK_SUCCESS;
    if (!prepared) {
      break;
    }
    VkMemoryRequirements requirements{};
    dispatch.get_buffer_memory_requirements(dispatch.device, buffers[index],
                                            &requirements);
    const int memory_type =
        FindMemoryType(dispatch, requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type < 0) {
      prepared = false;
      break;
    }
    VkMemoryAllocateInfo memory_info{};
    memory_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memory_info.allocationSize = requirements.size;
    memory_info.memoryTypeIndex = static_cast<uint32_t>(memory_type);
    prepared = dispatch.allocate_memory(dispatch.device, &memory_info, nullptr,
                                        &memories[index]) == VK_SUCCESS;
    if (!prepared) {
      break;
    }
    prepared = dispatch.bind_buffer_memory(dispatch.device, buffers[index],
                                           memories[index], 0) == VK_SUCCESS;
  }

  if (prepared) {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    prepared =
        dispatch.begin_command_buffer(command, &begin_info) == VK_SUCCESS;
  }
  if (prepared) {
    for (int index = 0; index < 2; ++index) {
      VkImageMemoryBarrier to_source{};
      to_source.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      to_source.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      to_source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      to_source.oldLayout = eyes[index].last_final_layout;
      to_source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      to_source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_source.image = eyes[index].image.image;
      to_source.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      dispatch.cmd_pipeline_barrier(
          command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
          &to_source);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {width, height, 1};
      dispatch.cmd_copy_image_to_buffer(command, eyes[index].image.image,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        buffers[index], 1, &region);
      VkImageMemoryBarrier restore = to_source;
      restore.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      restore.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      restore.newLayout = eyes[index].last_final_layout;
      dispatch.cmd_pipeline_barrier(
          command, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
          nullptr, 1, &restore);
    }
    prepared = dispatch.end_command_buffer(command) == VK_SUCCESS;
  }
  if (prepared) {
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    prepared = dispatch.queue_submit(queue, 1, &submit, fence) == VK_SUCCESS &&
               dispatch.wait_for_fences(dispatch.device, 1, &fence, VK_TRUE,
                                        2000000000ULL) == VK_SUCCESS;
  }
  if (prepared) {
    for (int index = 0; index < 2; ++index) {
      prepared =
          dispatch.map_memory(dispatch.device, memories[index], 0, buffer_size,
                              0, &mapped[index]) == VK_SUCCESS;
      if (!prepared) {
        break;
      }
    }
  }

  const std::string directory = EvidenceDirectory();
  bool wrote_images = false;
  if (prepared && mapped[0] != nullptr && mapped[1] != nullptr) {
    const auto *pixels0 = static_cast<const uint8_t *>(mapped[0]);
    const auto *pixels1 = static_cast<const uint8_t *>(mapped[1]);
    wrote_images = WritePpm(directory + "/eye0.ppm", pixels0, width, height,
                            IsBgra(eyes[0].image.format)) &&
                   WritePpm(directory + "/eye1.ppm", pixels1, width, height,
                            IsBgra(eyes[1].image.format));
    std::size_t differing = 0;
    std::uint64_t difference_sum = 0;
    uint32_t maximum_difference = 0;
    const std::size_t total = static_cast<std::size_t>(width) * height;
    for (std::size_t pixel = 0; pixel < total; ++pixel) {
      const uint8_t *a = pixels0 + pixel * 4;
      const uint8_t *b = pixels1 + pixel * 4;
      uint32_t largest = 0;
      for (int channel = 0; channel < 3; ++channel) {
        const uint32_t difference = a[channel] > b[channel]
                                        ? a[channel] - b[channel]
                                        : b[channel] - a[channel];
        largest = difference > largest ? difference : largest;
      }
      if (largest > 0) {
        ++differing;
        difference_sum += largest;
        maximum_difference =
            largest > maximum_difference ? largest : maximum_difference;
      }
    }
    std::FILE *summary = std::fopen((directory + "/summary.txt").c_str(), "w");
    if (summary != nullptr) {
      std::fprintf(summary,
                   "provenance=guest_initializer_and_eye_getter owner=%p\n",
                   eyes[0].image.owner);
      std::fprintf(summary, "capture_present=%llu\n",
                   static_cast<unsigned long long>(g_present_count));
      for (int index = 0; index < 2; ++index) {
        const EyeTarget &eye = eyes[index];
        std::fprintf(
            summary,
            "eye%d_image=%p eye%d_format=%d eye%d_width=%u eye%d_height=%u "
            "eye%d_usage=0x%x eye%d_framebuffer=%p "
            "eye%d_render_pass_count=%llu eye%d_final_layout=%d\n",
            index, static_cast<void *>(eye.image.image), index,
            static_cast<int>(eye.image.format), index, eye.image.width, index,
            eye.image.height, index, eye.image.usage, index,
            static_cast<void *>(eye.framebuffer), index,
            static_cast<unsigned long long>(eye.render_pass_count), index,
            static_cast<int>(eye.last_final_layout));
      }
      std::fprintf(summary,
                   "pixels_total=%zu pixels_differing=%zu max_abs_diff=%u "
                   "mean_abs_diff=%.4f\n",
                   total, differing, maximum_difference,
                   differing > 0 ? static_cast<double>(difference_sum) /
                                       static_cast<double>(differing)
                                 : 0.0);
      std::fprintf(summary, "EYES_DIFFER=%s\n", differing > 0 ? "yes" : "no");
      std::fclose(summary);
    }
    Log("  [vr-evidence-layer] capture done: differing=%zu/%u max_diff=%u "
        "eye0_rp=%llu eye1_rp=%llu wrote_ppm=%d\n",
        differing, static_cast<uint32_t>(total), maximum_difference,
        static_cast<unsigned long long>(eyes[0].render_pass_count),
        static_cast<unsigned long long>(eyes[1].render_pass_count),
        wrote_images ? 1 : 0);
  } else {
    Log("  [vr-evidence-layer] capture FAILED (prepared=%d)\n",
        prepared ? 1 : 0);
  }

  for (int index = 0; index < 2; ++index) {
    if (mapped[index] != nullptr) {
      dispatch.unmap_memory(dispatch.device, memories[index]);
    }
    if (buffers[index] != VK_NULL_HANDLE) {
      dispatch.destroy_buffer(dispatch.device, buffers[index], nullptr);
    }
    if (memories[index] != VK_NULL_HANDLE) {
      dispatch.free_memory(dispatch.device, memories[index], nullptr);
    }
  }
  if (fence != VK_NULL_HANDLE) {
    dispatch.destroy_fence(dispatch.device, fence, nullptr);
  }
  dispatch.destroy_command_pool(dispatch.device, pool, nullptr);
}

VKAPI_ATTR VkResult VKAPI_CALL
LayerQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
  DeviceDispatch *dispatch = DispatchFor(queue);
  if (dispatch == nullptr || dispatch->queue_present == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (!g_active) {
    return dispatch->queue_present(queue, info);
  }
  std::uint64_t presents = 0;
  bool should_capture = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    presents = ++g_present_count;
    // Only capture once BOTH eyes have actually been rendered into at least
    // g_capture_render_passes times; otherwise the readback could show
    // uninitialized contents and a bogus pixel difference.
    should_capture = !g_capture_attempted && presents >= g_capture_present &&
                     g_eyes[0].valid && g_eyes[1].valid &&
                     g_eyes[0].image.dispatch == dispatch &&
                     g_eyes[1].image.dispatch == dispatch &&
                     g_eyes[0].framebuffer != VK_NULL_HANDLE &&
                     g_eyes[1].framebuffer != VK_NULL_HANDLE &&
                     g_eyes[0].render_pass_count >= g_capture_render_passes &&
                     g_eyes[1].render_pass_count >= g_capture_render_passes;
    if (should_capture) {
      g_capture_attempted = true;
    }
  }
  if (should_capture) {
    CaptureEyes(*dispatch, queue);
  }
  if (presents == 1 || presents % 120 == 0) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Log("  [vr-evidence-layer] presents=%llu eye0_rp=%llu eye1_rp=%llu\n",
        static_cast<unsigned long long>(presents),
        static_cast<unsigned long long>(g_eyes[0].render_pass_count),
        static_cast<unsigned long long>(g_eyes[1].render_pass_count));
  }
  return dispatch->queue_present(queue, info);
}

VKAPI_ATTR void VKAPI_CALL LayerCmdBeginRenderPass(
    VkCommandBuffer command, const VkRenderPassBeginInfo *info,
    VkSubpassContents contents) {
  DeviceDispatch *dispatch = DispatchFor(command);
  if (dispatch == nullptr || dispatch->cmd_begin_render_pass == nullptr) {
    return;
  }
  if (g_active && info != nullptr) {
    std::lock_guard<std::mutex> lock(g_mutex);
    void *owner = nullptr;
    void *guest_framebuffer = nullptr;
    int index = -1;
    const bool requested = g_current_eye != nullptr &&
                           g_current_eye(&owner, &index, &guest_framebuffer) &&
                           index >= 0 && index < 2;
    const auto attachments = g_framebuffer_images.find(info->framebuffer);
    if (requested && attachments != g_framebuffer_images.end()) {
      const ImageRecord *target = nullptr;
      bool ambiguous = false;
      for (VkImage image : attachments->second) {
        const auto found = g_images.find(image);
        if (found != g_images.end() && found->second.owner == owner &&
            found->second.dispatch == dispatch &&
            IsColorFormat(found->second.format) &&
            (found->second.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0) {
          if (target != nullptr)
            ambiguous = true;
          target = &found->second;
        }
      }
      if (target != nullptr && !ambiguous) {
        if ((g_eyes[0].valid && g_eyes[0].image.owner != owner) ||
            (g_eyes[1].valid && g_eyes[1].image.owner != owner)) {
          g_eyes[0] = {};
          g_eyes[1] = {};
          g_capture_attempted = false;
        }
        EyeTarget &eye = g_eyes[index];
        if (!eye.valid || eye.image.image != target->image ||
            eye.framebuffer != info->framebuffer) {
          eye = {};
          eye.valid = true;
          eye.image = *target;
          eye.framebuffer = info->framebuffer;
          Log("  [vr-evidence-layer] eye framebuffer bound: eye=%d owner=%p "
              "guest_fb=%p fb=%p image=%p "
              "provenance=guest_initializer_and_eye_getter\n",
              index, owner, guest_framebuffer,
              static_cast<void *>(info->framebuffer),
              static_cast<void *>(target->image));
        }
        ++eye.render_pass_count;
        const auto layout = g_render_pass_final_layouts.find(info->renderPass);
        if (layout != g_render_pass_final_layouts.end())
          eye.last_final_layout = layout->second;
        if (g_consume_eye != nullptr)
          g_consume_eye();
      }
    }
  }
  dispatch->cmd_begin_render_pass(command, info, contents);
}

VKAPI_ATTR VkResult VKAPI_CALL
LayerCreateImage(VkDevice device, const VkImageCreateInfo *info,
                 const VkAllocationCallbacks *allocator, VkImage *image) {
  DeviceDispatch *dispatch = DispatchFor(device);
  if (dispatch == nullptr || dispatch->create_image == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      dispatch->create_image(device, info, allocator, image);
  void *owner =
      g_initializing_object != nullptr ? g_initializing_object() : nullptr;
  if (g_active && owner != nullptr && result == VK_SUCCESS && info != nullptr &&
      image != nullptr) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ImageRecord record{};
    record.image = *image;
    record.dispatch = dispatch;
    record.width = info->extent.width;
    record.height = info->extent.height;
    record.format = info->format;
    record.usage = info->usage;
    record.owner = owner;
    g_images[*image] = record;
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateImageView(
    VkDevice device, const VkImageViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkImageView *view) {
  DeviceDispatch *dispatch = DispatchFor(device);
  if (dispatch == nullptr || dispatch->create_image_view == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      dispatch->create_image_view(device, info, allocator, view);
  if (g_active && result == VK_SUCCESS && info != nullptr && view != nullptr) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_image_views[*view] = info->image;
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateFramebuffer(
    VkDevice device, const VkFramebufferCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkFramebuffer *framebuffer) {
  DeviceDispatch *dispatch = DispatchFor(device);
  if (dispatch == nullptr || dispatch->create_framebuffer == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      dispatch->create_framebuffer(device, info, allocator, framebuffer);
  if (g_active && result == VK_SUCCESS && info != nullptr &&
      framebuffer != nullptr) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto &images = g_framebuffer_images[*framebuffer];
    images.clear();
    for (uint32_t index = 0; index < info->attachmentCount; ++index) {
      const auto source = g_image_views.find(info->pAttachments[index]);
      if (source != g_image_views.end())
        images.push_back(source->second);
    }
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL LayerDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks *allocator) {
  auto *dispatch = DispatchFor(device);
  if (dispatch == nullptr)
    return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_images.erase(image);
    for (auto it = g_image_views.begin(); it != g_image_views.end();) {
      if (it->second == image)
        it = g_image_views.erase(it);
      else
        ++it;
    }
    if (g_eyes[0].image.image == image || g_eyes[1].image.image == image) {
      g_eyes[0] = {};
      g_eyes[1] = {};
      g_capture_attempted = false;
    }
  }
  dispatch->destroy_image(device, image, allocator);
}

VKAPI_ATTR void VKAPI_CALL
LayerDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer,
                        const VkAllocationCallbacks *allocator) {
  auto *dispatch = DispatchFor(device);
  if (dispatch == nullptr)
    return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_framebuffer_images.erase(framebuffer);
    if (g_eyes[0].framebuffer == framebuffer ||
        g_eyes[1].framebuffer == framebuffer) {
      g_eyes[0] = {};
      g_eyes[1] = {};
      g_capture_attempted = false;
    }
  }
  dispatch->destroy_framebuffer(device, framebuffer, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateRenderPass(
    VkDevice device, const VkRenderPassCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkRenderPass *render_pass) {
  DeviceDispatch *dispatch = DispatchFor(device);
  if (dispatch == nullptr || dispatch->create_render_pass == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      dispatch->create_render_pass(device, info, allocator, render_pass);
  if (g_active && result == VK_SUCCESS && info != nullptr &&
      render_pass != nullptr && info->attachmentCount > 0) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_render_pass_final_layouts[*render_pass] =
        info->pAttachments[0].finalLayout;
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL LayerGetDeviceQueue(VkDevice device, uint32_t family,
                                               uint32_t index, VkQueue *queue) {
  DeviceDispatch *dispatch = DispatchFor(device);
  if (dispatch == nullptr || dispatch->get_device_queue == nullptr) {
    return;
  }
  dispatch->get_device_queue(device, family, index, queue);
  if (g_active && queue != nullptr && *queue != VK_NULL_HANDLE) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue_families[DispatchKey(*queue)] = family;
  }
}

VKAPI_ATTR void VKAPI_CALL
LayerDestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator) {
  DeviceDispatch *dispatch = DispatchFor(device);
  if (dispatch == nullptr) {
    return;
  }
  PFN_vkDestroyDevice destroy = dispatch->destroy_device;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_dispatches.erase(DispatchKey(device));
  }
  if (destroy != nullptr) {
    destroy(device, allocator);
  }
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
LayerGetDeviceProcAddr(VkDevice device, const char *name) {
  if (name == nullptr) {
    return nullptr;
  }
  if (g_active) {
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
      return reinterpret_cast<PFN_vkVoidFunction>(LayerGetDeviceProcAddr);
    }
    if (std::strcmp(name, "vkDestroyImage") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(LayerDestroyImage);
    if (std::strcmp(name, "vkDestroyFramebuffer") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(LayerDestroyFramebuffer);
    if (std::strcmp(name, "vkDestroyDevice") == 0) {
      return reinterpret_cast<PFN_vkVoidFunction>(LayerDestroyDevice);
    }
    if (std::strcmp(name, "vkGetDeviceQueue") == 0) {
      return reinterpret_cast<PFN_vkVoidFunction>(LayerGetDeviceQueue);
    }
    if (std::strcmp(name, "vkCreateImage") == 0) {
      return reinterpret_cast<PFN_vkVoidFunction>(LayerCreateImage);
    }
    if (std::strcmp(name, "vkCreateImageView") == 0) {
      return reinterpret_cast<PFN_vkVoidFunction>(LayerCreateImageView);
    }
    if (std::strcmp(name, "vkCreateFramebuffer") == 0) {
      return reinterpret_cast<PFN_vkVoidFunction>(LayerCreateFramebuffer);
    }
    if (std::strcmp(name, "vkCreateRenderPass") == 0) {
      return reinterpret_cast<PFN_vkVoidFunction>(LayerCreateRenderPass);
    }
    if (std::strcmp(name, "vkCmdBeginRenderPass") == 0) {
      return reinterpret_cast<PFN_vkVoidFunction>(LayerCmdBeginRenderPass);
    }
    if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
      return reinterpret_cast<PFN_vkVoidFunction>(LayerQueuePresentKHR);
    }
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto entry = g_dispatches.find(DispatchKey(device));
  if (entry == g_dispatches.end() ||
      entry->second.get_device_proc_addr == nullptr) {
    return nullptr;
  }
  return entry->second.get_device_proc_addr(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
LayerGetInstanceProcAddr(VkInstance instance, const char *name);

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateDevice(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDevice *device) {
  auto *link = const_cast<VkLayerDeviceCreateInfo *>(
      static_cast<const VkLayerDeviceCreateInfo *>(
          FindInChain(info != nullptr ? info->pNext : nullptr,
                      VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)));
  if (link == nullptr || link->u.pLayerInfo == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkLayerDeviceLink *device_link = link->u.pLayerInfo;
  PFN_vkGetInstanceProcAddr next_gipa = device_link->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr next_gdpa = device_link->pfnNextGetDeviceProcAddr;
  PFN_vkCreateDevice next_create = reinterpret_cast<PFN_vkCreateDevice>(
      next_gipa(VK_NULL_HANDLE, "vkCreateDevice"));
  if (next_create == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;
  const VkResult result = next_create(physical_device, info, allocator, device);
  if (result != VK_SUCCESS || device == nullptr) {
    return result;
  }
  DeviceDispatch dispatch{};
  dispatch.device = *device;
  dispatch.physical_device = physical_device;
  dispatch.get_device_proc_addr = next_gdpa;
#define MOCKTAIL_LAYER_DEVICE_FN(field, name)                                  \
  dispatch.field = reinterpret_cast<PFN_##name>(next_gdpa(*device, #name));
  MOCKTAIL_LAYER_DEVICE_FN(destroy_device, vkDestroyDevice)
  MOCKTAIL_LAYER_DEVICE_FN(get_device_queue, vkGetDeviceQueue)
  MOCKTAIL_LAYER_DEVICE_FN(create_image, vkCreateImage)
  MOCKTAIL_LAYER_DEVICE_FN(destroy_image, vkDestroyImage)
  MOCKTAIL_LAYER_DEVICE_FN(destroy_framebuffer, vkDestroyFramebuffer)
  MOCKTAIL_LAYER_DEVICE_FN(create_image_view, vkCreateImageView)
  MOCKTAIL_LAYER_DEVICE_FN(create_framebuffer, vkCreateFramebuffer)
  MOCKTAIL_LAYER_DEVICE_FN(create_render_pass, vkCreateRenderPass)
  MOCKTAIL_LAYER_DEVICE_FN(cmd_begin_render_pass, vkCmdBeginRenderPass)
  MOCKTAIL_LAYER_DEVICE_FN(queue_present, vkQueuePresentKHR)
  MOCKTAIL_LAYER_DEVICE_FN(queue_submit, vkQueueSubmit)
  MOCKTAIL_LAYER_DEVICE_FN(create_command_pool, vkCreateCommandPool)
  MOCKTAIL_LAYER_DEVICE_FN(allocate_command_buffers, vkAllocateCommandBuffers)
  MOCKTAIL_LAYER_DEVICE_FN(begin_command_buffer, vkBeginCommandBuffer)
  MOCKTAIL_LAYER_DEVICE_FN(end_command_buffer, vkEndCommandBuffer)
  MOCKTAIL_LAYER_DEVICE_FN(cmd_pipeline_barrier, vkCmdPipelineBarrier)
  MOCKTAIL_LAYER_DEVICE_FN(cmd_copy_image_to_buffer, vkCmdCopyImageToBuffer)
  MOCKTAIL_LAYER_DEVICE_FN(create_fence, vkCreateFence)
  MOCKTAIL_LAYER_DEVICE_FN(wait_for_fences, vkWaitForFences)
  MOCKTAIL_LAYER_DEVICE_FN(destroy_fence, vkDestroyFence)
  MOCKTAIL_LAYER_DEVICE_FN(create_buffer, vkCreateBuffer)
  MOCKTAIL_LAYER_DEVICE_FN(destroy_buffer, vkDestroyBuffer)
  MOCKTAIL_LAYER_DEVICE_FN(get_buffer_memory_requirements,
                           vkGetBufferMemoryRequirements)
  MOCKTAIL_LAYER_DEVICE_FN(allocate_memory, vkAllocateMemory)
  MOCKTAIL_LAYER_DEVICE_FN(free_memory, vkFreeMemory)
  MOCKTAIL_LAYER_DEVICE_FN(bind_buffer_memory, vkBindBufferMemory)
  MOCKTAIL_LAYER_DEVICE_FN(map_memory, vkMapMemory)
  MOCKTAIL_LAYER_DEVICE_FN(unmap_memory, vkUnmapMemory)
  MOCKTAIL_LAYER_DEVICE_FN(destroy_command_pool, vkDestroyCommandPool)
#undef MOCKTAIL_LAYER_DEVICE_FN
  dispatch.memory_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          next_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties"));
  if (g_active) {
    Log("  [vr-evidence-layer] CreateDevice: g_instance=%p chain-gipa "
        "memory-props=%p physdev-props=%p enumerate-pd=%p pd=%p\n",
        static_cast<void *>(g_instance),
        reinterpret_cast<void *>(dispatch.memory_properties),
        reinterpret_cast<void *>(
            next_gipa(g_instance, "vkGetPhysicalDeviceProperties")),
        reinterpret_cast<void *>(
            next_gipa(g_instance, "vkEnumeratePhysicalDevices")),
        static_cast<void *>(physical_device));
  }
  if (dispatch.memory_properties == nullptr) {
    // NOTE: dlsym(RTLD_DEFAULT) returns the loader's top-level trampoline,
    // which rejects below-chain physical device handles; do not use it here.
  }
  if (dispatch.memory_properties == nullptr) {
    Log("  [vr-evidence-layer] warning: memory-properties entry unresolved; "
        "pixel readback will be unavailable\n");
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_dispatches[DispatchKey(*device)] = dispatch;
  }
  if (g_active) {
    Log("  [vr-evidence-layer] device dispatch registered (%p)\n",
        static_cast<void *>(*device));
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateInstance(
    const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
    VkInstance *instance) {
  auto *link = const_cast<VkLayerInstanceCreateInfo *>(
      static_cast<const VkLayerInstanceCreateInfo *>(
          FindInChain(info != nullptr ? info->pNext : nullptr,
                      VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO)));
  if (link == nullptr || link->u.pLayerInfo == nullptr) {
    if (g_active) {
      Log("  [vr-evidence-layer] CreateInstance: loader chain info missing "
          "(link=%p)\n",
          static_cast<const void *>(link));
    }
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkLayerInstanceLink *instance_link = link->u.pLayerInfo;
  PFN_vkGetInstanceProcAddr next_gipa =
      instance_link->pfnNextGetInstanceProcAddr;
  PFN_vkCreateInstance next_create = reinterpret_cast<PFN_vkCreateInstance>(
      next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
  if (next_create == nullptr) {
    Log("  [vr-evidence-layer] CreateInstance: next chain entry is null\n");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;
  const VkResult result = next_create(info, allocator, instance);
  if (g_active) {
    Log("  [vr-evidence-layer] CreateInstance result=%d api=%u\n",
        static_cast<int>(result),
        info != nullptr && info->pApplicationInfo != nullptr
            ? info->pApplicationInfo->apiVersion
            : 0u);
  }
  if (result == VK_SUCCESS) {
    g_instance_gipa = next_gipa;
    if (instance != nullptr) {
      g_instance = *instance;
    }
  }
  return result;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
LayerGetInstanceProcAddr(VkInstance instance, const char *name) {
  if (name == nullptr) {
    return nullptr;
  }
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(LayerGetInstanceProcAddr);
  }
  if (std::strcmp(name, "vkCreateInstance") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(LayerCreateInstance);
  }
  if (std::strcmp(name, "vkCreateDevice") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(LayerCreateDevice);
  }
  if (g_instance_gipa != nullptr) {
    return g_instance_gipa(instance, name);
  }
  return nullptr;
}

} // namespace

extern "C" {

MOCKTAIL_VR_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name) {
  return LayerGetInstanceProcAddr(instance, name);
}

MOCKTAIL_VR_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name) {
  return LayerGetDeviceProcAddr(device, name);
}

MOCKTAIL_VR_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *version) {
  if (version == nullptr ||
      version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (!g_active) {
    const std::string directory = EvidenceDirectory();
    if (!directory.empty()) {
      g_active = true;
      g_initializing_object = reinterpret_cast<InitializingObjectFn>(
          dlsym(RTLD_DEFAULT, "mocktail_vr_initializing_object"));
      g_current_eye = reinterpret_cast<CurrentEyeFn>(
          dlsym(RTLD_DEFAULT, "mocktail_vr_current_eye"));
      g_consume_eye = reinterpret_cast<ConsumeEyeFn>(
          dlsym(RTLD_DEFAULT, "mocktail_vr_consume_eye"));
      g_capture_present =
          EnvUnsigned("MOCKTAIL_VR_EVIDENCE_CAPTURE_PRESENT", 60);
      g_capture_render_passes =
          EnvUnsigned("MOCKTAIL_VR_EVIDENCE_CAPTURE_RENDER_PASSES", 10);
      Log("  [vr-evidence-layer] activated: dir=%s "
          "capture_present=%u capture_render_passes=%u\n",
          directory.c_str(), g_capture_present, g_capture_render_passes);
    }
  }
  if (version->loaderLayerInterfaceVersion >= 2) {
    version->pfnGetInstanceProcAddr = LayerGetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = LayerGetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = nullptr;
    version->loaderLayerInterfaceVersion = 2;
  }
  return VK_SUCCESS;
}

} // extern "C"
