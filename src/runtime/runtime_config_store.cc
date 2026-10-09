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

// Reads `path` as text. When `missing` is not null, a file that does not
// exist reads as empty text and sets it.
RuntimeConfigText ReadText(const std::filesystem::path& path, bool* missing) {
  RuntimeConfigText result;
  // Errors name the file, since more than one file is read this way.
  const std::string name = path.filename().string() + ": ";
  if (missing != nullptr) {
    *missing = false;
  }
  const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (file < 0) {
    if (errno == ENOENT && missing != nullptr) {
      *missing = true;
      return result;
    }
    result.error =
        name + (errno == ELOOP ? "refusing to read through a symlink"
                               : ErrorMessage("cannot open the file", errno));
    return result;
  }
  struct stat metadata = {};
  if (fstat(file, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
    result.error = name + "the file must be a regular file";
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
      result.error = name + ErrorMessage("cannot read the file", errno);
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

// Publishes `text` as `config_file` once `check` accepts it. `may_be_missing`
// lets the save create the file.
RuntimeConfigSaveResult Save(const std::filesystem::path& config_file,
                             std::string_view original, std::string_view text,
                             bool may_be_missing, const TextFileCheck& check) {
  RuntimeConfigSaveResult result;
  if (config_file.empty() || config_file.filename().empty()) {
    result.error = "the path must name a file";
    return result;
  }
  const std::string name = config_file.filename().string() + ": ";
  bool missing = false;
  const RuntimeConfigText current =
      ReadText(config_file, may_be_missing ? &missing : nullptr);
  if (!current) {
    result.error = current.error;
    return result;
  }
  if (current.text != original) {
    result.error = name + "the file changed on disk since it was read";
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
    result.error =
        name + ErrorMessage("cannot create a temporary file", errno);
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
    result.error = name + ErrorMessage("cannot write the temporary file",
                                       written ? errno : write_error);
    discard_temporary();
    return result;
  }

  // The bytes that would be published are the bytes that get checked.
  result.error = check(temporary_path.data());
  if (!result.error.empty()) {
    discard_temporary();
    return result;
  }

  if (rename(temporary_path.data(), config_file.c_str()) != 0) {
    result.error = name + ErrorMessage("cannot replace the file", errno);
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

}  // namespace

RuntimeConfigText ReadRuntimeConfigText(
    const std::filesystem::path& config_file) {
  return ReadText(config_file, nullptr);
}

RuntimeConfigSaveResult SaveRuntimeConfigText(
    const std::filesystem::path& config_file, std::string_view original,
    std::string_view text) {
  return Save(config_file, original, text, false,
              [](const std::filesystem::path& candidate) {
                return LoadRuntimeConfig(NoEnvironment(), candidate).error;
              });
}

RuntimeConfigText ReadOptionalTextFile(const std::filesystem::path& file) {
  bool missing = false;
  return ReadText(file, &missing);
}

RuntimeConfigSaveResult SaveCheckedTextFile(const std::filesystem::path& file,
                                            std::string_view original,
                                            std::string_view text,
                                            const TextFileCheck& check) {
  return Save(file, original, text, true, check);
}

}  // namespace runtime
}  // namespace mocktail
