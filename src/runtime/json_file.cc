#include "runtime/json_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <utility>

#include "mocktail/scoped_file_descriptor.h"
#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr std::uintmax_t kMaximumJsonBytes = 16U * 1024U * 1024U;
std::atomic<std::uint64_t> g_temporary_sequence{0};

bool WriteAll(int descriptor, std::string_view bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written =
        write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

}  // namespace

JsonFileReadResult ReadJsonObjectFile(const std::filesystem::path& path,
                                      std::string_view label) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      return {};
    }
    return {JsonFileStatus::kInvalid,
            {},
            "cannot safely open " + std::string(label)};
  }
  const ScopedFileDescriptor file(descriptor);
  struct stat status{};
  if (fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 ||
      static_cast<std::uintmax_t>(status.st_size) > kMaximumJsonBytes) {
    return {JsonFileStatus::kInvalid,
            {},
            std::string(label) + " is not a bounded regular file"};
  }

  std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count =
        read(file.get(), bytes.data() + offset, bytes.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return {JsonFileStatus::kInvalid,
              {},
              "cannot read " + std::string(label)};
    }
    if (count == 0) {
      return {JsonFileStatus::kInvalid,
              {},
              std::string(label) + " changed while being read"};
    }
    offset += static_cast<std::size_t>(count);
  }
  nlohmann::json parsed = nlohmann::json::parse(bytes, nullptr, false, true);
  std::fill(bytes.begin(), bytes.end(), '\0');
  if (parsed.is_discarded() || !parsed.is_object()) {
    return {JsonFileStatus::kInvalid,
            {},
            std::string(label) + " is not a JSON object"};
  }
  return {JsonFileStatus::kReady, std::move(parsed), {}};
}

bool WriteJsonFileAtomically(const std::filesystem::path& path,
                             const nlohmann::json& value,
                             const JsonFileWriteNames& names,
                             std::string* error) {
  const std::string label(names.label);
  std::error_code filesystem_error;
  if (!RuntimePaths::EnsureDirectory(path.parent_path(), &filesystem_error)) {
    *error = "cannot create " + label + " directory";
    return false;
  }
  const std::filesystem::file_status target_status =
      std::filesystem::symlink_status(path, filesystem_error);
  if (!filesystem_error && std::filesystem::is_symlink(target_status)) {
    *error = std::string(names.target) + " is a symlink";
    return false;
  }
  if (filesystem_error != std::errc::no_such_file_or_directory &&
      filesystem_error) {
    *error = "cannot inspect " + std::string(names.target);
    return false;
  }

  const std::filesystem::path temporary =
      path.parent_path() /
      (std::string(names.temporary_prefix) + std::to_string(getpid()) + "." +
       std::to_string(g_temporary_sequence.fetch_add(1)));
  const int descriptor =
      open(temporary.c_str(),
           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0) {
    *error = "cannot create temporary " + label;
    return false;
  }
  bool stored = false;
  {
    const ScopedFileDescriptor file(descriptor);
    const std::string bytes = value.dump();
    stored = fchmod(file.get(), S_IRUSR | S_IWUSR) == 0 &&
             WriteAll(file.get(), bytes) && fsync(file.get()) == 0;
  }
  if (!stored || rename(temporary.c_str(), path.c_str()) != 0) {
    const int saved_errno = errno;
    (void)unlink(temporary.c_str());
    errno = saved_errno;
    *error = "cannot atomically store " + label;
    return false;
  }
  const int directory_descriptor =
      open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (directory_descriptor < 0) {
    *error = "cannot open " + label + " directory for sync";
    return false;
  }
  const ScopedFileDescriptor directory(directory_descriptor);
  if (fsync(directory.get()) != 0) {
    *error = "cannot sync " + label + " directory";
    return false;
  }
  return true;
}

}  // namespace runtime
}  // namespace mocktail
