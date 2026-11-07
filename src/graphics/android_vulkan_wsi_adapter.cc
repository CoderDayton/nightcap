#include "mocktail/graphics/android_vulkan_wsi_adapter.h"

#include <algorithm>

namespace mocktail {
namespace graphics {
namespace {

bool Contains(const std::vector<std::string>& values,
              const std::string& candidate) {
  return std::find(values.begin(), values.end(), candidate) != values.end();
}

Status AppendUniqueExtension(const std::string& extension,
                             std::vector<std::string>* output) {
  if (extension.empty()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "Vulkan extension names must not be empty");
  }
  if (!Contains(*output, extension)) {
    output->push_back(extension);
  }
  return Status::Ok();
}

}  // namespace

VkResult NormalizeAndroidSwapchainResult(VkResult result) {
  return result == VK_SUBOPTIMAL_KHR ? VK_SUCCESS : result;
}

Status TranslateAndroidVulkanInstanceExtensions(
    const std::vector<std::string>& android_extensions,
    const std::vector<std::string>& host_wsi_extensions,
    std::vector<std::string>* host_extensions) {
  if (host_extensions == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "Vulkan extension output is required");
  }

  std::vector<std::string> translated;
  translated.reserve(android_extensions.size() + host_wsi_extensions.size());
  bool requested_android_surface = false;
  for (const std::string& extension : android_extensions) {
    if (extension == kAndroidSurfaceExtension) {
      requested_android_surface = true;
      continue;
    }
    Status status = AppendUniqueExtension(extension, &translated);
    if (!status.ok()) {
      return status;
    }
  }

  if (requested_android_surface) {
    if (host_wsi_extensions.empty()) {
      return Status::Error(
          StatusCode::kFailedPrecondition,
          "Android Vulkan surface requested without host WSI extensions");
    }
    for (const std::string& extension : host_wsi_extensions) {
      Status status = AppendUniqueExtension(extension, &translated);
      if (!status.ok()) {
        return status;
      }
    }
  }

  *host_extensions = std::move(translated);
  return Status::Ok();
}

Status AndroidVulkanWsiAdapter::Initialize(SdlVulkanWsi* host_wsi) {
  if (host_wsi_ != nullptr) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "Android Vulkan WSI adapter is already initialized");
  }
  if (host_wsi == nullptr || !host_wsi->IsInitialized()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "an initialized SDL Vulkan WSI is required");
  }
  if (host_wsi->GetInstanceProcAddress() == nullptr) {
    return Status::Error(StatusCode::kUnavailable,
                         "host vkGetInstanceProcAddr is unavailable");
  }
  host_wsi_ = host_wsi;
  return Status::Ok();
}

void AndroidVulkanWsiAdapter::Shutdown() { host_wsi_ = nullptr; }

Status AndroidVulkanWsiAdapter::TranslateInstanceExtensions(
    const std::vector<std::string>& android_extensions,
    std::vector<std::string>* host_extensions) const {
  if (host_wsi_ == nullptr) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "Android Vulkan WSI adapter is not initialized");
  }
  return TranslateAndroidVulkanInstanceExtensions(
      android_extensions, host_wsi_->required_instance_extensions(),
      host_extensions);
}

AndroidVulkanResult AndroidVulkanWsiAdapter::CreateAndroidSurface(
    VkInstance instance, const void* android_create_info,
    const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) const {
  static_cast<void>(android_create_info);
  if (surface != nullptr) {
    *surface = VkSurfaceKHR{};
  }
  if (host_wsi_ == nullptr) {
    return kAndroidVulkanErrorInitializationFailed;
  }
  Status status = host_wsi_->CreateSurface(instance, allocator, surface);
  return status.ok() ? kAndroidVulkanSuccess
                     : kAndroidVulkanErrorInitializationFailed;
}

void AndroidVulkanWsiAdapter::DestroySurface(
    VkInstance instance, VkSurfaceKHR surface,
    const VkAllocationCallbacks* allocator) const {
  if (host_wsi_ != nullptr) {
    host_wsi_->DestroySurface(instance, surface, allocator);
  }
}

}  // namespace graphics
}  // namespace mocktail
