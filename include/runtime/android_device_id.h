#ifndef MOCKTAIL_RUNTIME_ANDROID_DEVICE_ID_H_
#define MOCKTAIL_RUNTIME_ANDROID_DEVICE_ID_H_

#include <filesystem>
#include <string>

namespace mocktail::runtime {

struct AndroidDeviceId {
  std::string value;
  std::string error;
  explicit operator bool() const { return error.empty(); }
};

AndroidDeviceId LoadOrCreateAndroidDeviceId(const std::filesystem::path& file);

}

#endif
