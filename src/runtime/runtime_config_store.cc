#include "runtime/runtime_config_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "runtime/environment.h"
#include "runtime/runtime_config_file.h"

namespace mocktail {
namespace runtime {
namespace {

// Validation reads the file alone. With the process environment, a variable
// such as MOCKTAIL_GRAPHICS_BACKEND would replace the file's value and hide
// a bad one.
class NoEnvironment final : public Environment {
 public:
  std::optional<std::string> Get(std::string_view) const override {
    return std::nullopt;
  }
};

std::string ErrorMessage(std::string_view action, int error_number) {
  return std::string(action) + ": " + std::strerror(error_number);
}

bool WriteAll(int file, std::string_view contents) {
  const char* next = contents.data();
  std::size_t remaining = contents.size();
  while (remaining != 0) {
    const ssize_t written = write(file, next, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    next += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

}  // namespace

RuntimeConfigText ReadRuntimeConfigText(
    const std::filesystem::path& config_file) {
  RuntimeConfigText result;
  const int file =
      open(config_file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (file < 0) {
    result.error = errno == ELOOP
                       ? "refusing symlink runtime configuration"
                       : ErrorMessage("cannot open configuration", errno);
    return result;
  }
  struct stat metadata = {};
  if (fstat(file, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
    result.error = "runtime configuration must be a regular file";
    (void)close(file);
    return result;
  }
  char buffer[4096];
  while (true) {
    const ssize_t count = read(file, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      result.error = ErrorMessage("cannot read configuration", errno);
      result.text.clear();
      break;
    }
    if (count == 0) {
      break;
    }
    result.text.append(buffer, static_cast<std::size_t>(count));
  }
  (void)close(file);
  return result;
}

RuntimeConfigSaveResult SaveRuntimeConfigText(
    const std::filesystem::path& config_file, std::string_view original,
    std::string_view text) {
  RuntimeConfigSaveResult result;
  if (config_file.empty() || config_file.filename().empty()) {
    result.error = "configuration path must name a file";
    return result;
  }
  const RuntimeConfigText current = ReadRuntimeConfigText(config_file);
  if (!current) {
    result.error = current.error;
    return result;
  }
  if (current.text != original) {
    result.error = "the configuration file changed on disk since it was read";
    return result;
  }

  std::filesystem::path parent = config_file.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  const std::string temporary_pattern =
      (parent / ("." + config_file.filename().string() + ".tmp.XXXXXX"))
          .string();
  std::vector<char> temporary_path(temporary_pattern.begin(),
                                   temporary_pattern.end());
  temporary_path.push_back('\0');
  const int temporary_file = mkstemp(temporary_path.data());
  if (temporary_file < 0) {
    result.error = ErrorMessage("cannot create temporary configuration", errno);
    return result;
  }
  const auto discard_temporary = [&temporary_path]() {
    (void)unlink(temporary_path.data());
  };

  const bool written = fcntl(temporary_file, F_SETFD, FD_CLOEXEC) == 0 &&
                       fchmod(temporary_file, S_IRUSR | S_IWUSR) == 0 &&
                       WriteAll(temporary_file, text) &&
                       fsync(temporary_file) == 0;
  const int write_error = errno;
  if (close(temporary_file) != 0 || !written) {
    result.error = ErrorMessage("cannot write temporary configuration",
                                written ? errno : write_error);
    discard_temporary();
    return result;
  }

  // The bytes that would be published are the bytes that get checked.
  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(NoEnvironment(), temporary_path.data());
  if (!loaded) {
    result.error = loaded.error;
    discard_temporary();
    return result;
  }

  if (rename(temporary_path.data(), config_file.c_str()) != 0) {
    result.error = ErrorMessage("cannot replace configuration", errno);
    discard_temporary();
    return result;
  }
  // The rename is what a reader sees. Flushing the directory only makes it
  // survive a power cut, so a failure here does not undo the save.
  const int directory =
      open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (directory >= 0) {
    (void)fsync(directory);
    (void)close(directory);
  }
  return result;
}

}  // namespace runtime
}  // namespace mocktail
