#include "compat/file_digest_record.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

namespace mocktail::compat {
namespace {

struct FileIdentity {
  std::string key;
  std::string metadata;
  bool operator==(const FileIdentity& other) const {
    return key == other.key && metadata == other.metadata;
  }
};

std::optional<FileIdentity> Identity(const std::filesystem::path& path) {
  struct stat metadata = {};
  if (lstat(path.c_str(), &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
    return std::nullopt;
  }
  std::ostringstream identity;
  identity << "v1 " << metadata.st_size << ' ' << metadata.st_mtim.tv_sec
           << ' ' << metadata.st_mtim.tv_nsec << ' '
           << metadata.st_ctim.tv_sec << ' ' << metadata.st_ctim.tv_nsec;
  return FileIdentity{
      std::to_string(metadata.st_dev) + "-" + std::to_string(metadata.st_ino),
      identity.str()};
}

bool IsPrivateDirectory(const std::filesystem::path& directory) {
  struct stat metadata = {};
  return lstat(directory.c_str(), &metadata) == 0 &&
         S_ISDIR(metadata.st_mode) && metadata.st_uid == geteuid() &&
         (metadata.st_mode & 077) == 0;
}

bool IsDigest(const std::string& digest) {
  return !digest.empty() &&
         digest.find_first_of(" \t\r\n") == std::string::npos;
}

bool EnsurePrivateDirectory(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  struct stat metadata = {};
  return lstat(directory.c_str(), &metadata) == 0 &&
         S_ISDIR(metadata.st_mode) && metadata.st_uid == geteuid() &&
         chmod(directory.c_str(), 0700) == 0;
}

void WriteRecord(const std::filesystem::path& record,
                 const std::string& contents) {
  const std::filesystem::path temporary =
      record.string() + ".tmp-" + std::to_string(getpid());
  const int descriptor =
      open(temporary.c_str(),
           O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0) {
    return;
  }
  const bool written =
      write(descriptor, contents.data(), contents.size()) ==
      static_cast<ssize_t>(contents.size());
  close(descriptor);
  if (!written || rename(temporary.c_str(), record.c_str()) != 0) {
    unlink(temporary.c_str());
  }
}

}

std::string RememberedFileDigest(const std::filesystem::path& record_directory,
                                 const std::filesystem::path& path,
                                 const std::function<std::string()>& hash) {
  if (record_directory.empty()) {
    return hash();
  }
  const std::optional<FileIdentity> before = Identity(path);
  if (!before.has_value()) {
    return hash();
  }
  const std::filesystem::path record = record_directory / before->key;

  if (IsPrivateDirectory(record_directory)) {
    std::ifstream input(record);
    std::string stored_identity;
    std::string stored_digest;
    if (input && std::getline(input, stored_identity) &&
        std::getline(input, stored_digest) &&
        stored_identity == before->metadata && IsDigest(stored_digest)) {
      return stored_digest;
    }
  }

  const std::string digest = hash();
  if (IsDigest(digest) && Identity(path) == before &&
      EnsurePrivateDirectory(record_directory)) {
    WriteRecord(record, before->metadata + "\n" + digest + "\n");
  }
  return digest;
}

}
