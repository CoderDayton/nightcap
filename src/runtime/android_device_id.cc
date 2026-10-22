#include "runtime/android_device_id.h"

#include <sys/random.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

namespace mocktail::runtime {
namespace {

namespace fs = std::filesystem;

bool IsValid(const std::string& value) {
  if (value.size() != 16 || value == "0000000000000000") return false;
  for (const char c : value) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

std::string ReadSaved(const fs::path& file) {
  std::ifstream input(file);
  std::string value;
  std::getline(input, value);
  return value;
}

std::string Generate() {
  for (;;) {
    std::array<unsigned char, 8> bytes{};
    std::size_t filled = 0;
    while (filled < bytes.size()) {
      const ssize_t count =
          getrandom(bytes.data() + filled, bytes.size() - filled, 0);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return {};
      filled += static_cast<std::size_t>(count);
    }
    std::string value;
    for (const unsigned char byte : bytes) {
      char hex[3];
      std::snprintf(hex, sizeof(hex), "%02x", byte);
      value += hex;
    }
    if (IsValid(value)) return value;
  }
}

bool Save(const fs::path& file, const std::string& value) {
  std::string temporary = file.string() + ".XXXXXX";
  const int descriptor = mkstemp(temporary.data());
  if (descriptor < 0) return false;
  const std::string line = value + "\n";
  bool ok = write(descriptor, line.data(), line.size()) ==
                static_cast<ssize_t>(line.size()) &&
            fsync(descriptor) == 0;
  if (close(descriptor) != 0) ok = false;
  if (ok) ok = rename(temporary.c_str(), file.c_str()) == 0;
  if (!ok) unlink(temporary.c_str());
  return ok;
}

}

AndroidDeviceId LoadOrCreateAndroidDeviceId(const fs::path& file) {
  AndroidDeviceId result;
  result.value = ReadSaved(file);
  if (IsValid(result.value)) return result;

  std::error_code error;
  fs::create_directories(file.parent_path(), error);
  if (error) {
    result.error = "cannot create " + file.parent_path().string() + ": " +
                   error.message();
    return result;
  }
  result.value = Generate();
  if (result.value.empty()) {
    result.error = std::string("cannot read random bytes: ") +
                   std::strerror(errno);
  } else if (!Save(file, result.value)) {
    result.error = "cannot write " + file.string();
    result.value.clear();
  }
  return result;
}

}
