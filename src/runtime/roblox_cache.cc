#include "runtime/roblox_cache.h"

#include <array>
#include <system_error>
#include <thread>
#include <vector>

namespace mocktail::runtime {
namespace {

namespace fs = std::filesystem;

constexpr std::array<const char*, 4> kIndexFiles = {
    "rbx-storage.db", "rbx-storage.db-wal", "rbx-storage.db-shm",
    "rbx-storage.db-journal"};

fs::path Storage(const fs::path& root) { return root / "rbx-storage"; }
fs::path Index(const fs::path& root, const char* name) {
  return root / "files/appData" / name;
}

std::uintmax_t RegularFileBytes(const fs::path& path) {
  std::error_code error;
  if (!fs::is_regular_file(fs::symlink_status(path, error))) return 0;
  const std::uintmax_t size = fs::file_size(path, error);
  return error ? 0 : size;
}

std::uintmax_t FolderBytes(const fs::path& folder) {
  std::uintmax_t bytes = 0;
  std::error_code error;
  if (!fs::is_directory(fs::symlink_status(folder, error))) return 0;
  for (fs::recursive_directory_iterator
           it(folder, fs::directory_options::skip_permission_denied, error),
       end;
       !error && it != end; it.increment(error)) {
    bytes += RegularFileBytes(it->path());
  }
  return bytes;
}

std::vector<fs::path> ContentProviderFolders(const fs::path& root) {
  std::vector<fs::path> folders;
  std::error_code error;
  for (fs::directory_iterator it(root, error), end; !error && it != end;
       it.increment(error)) {
    std::error_code status_error;
    if (it->path().filename().string().rfind("ContentProvider_", 0) == 0 &&
        fs::is_directory(fs::symlink_status(it->path(), status_error))) {
      folders.push_back(it->path());
    }
  }
  return folders;
}

}

std::uintmax_t RobloxCacheBytes(const fs::path& android_data_root) {
  std::uintmax_t bytes = FolderBytes(Storage(android_data_root));
  for (const fs::path& folder : ContentProviderFolders(android_data_root)) {
    bytes += FolderBytes(folder);
  }
  for (const char* name : kIndexFiles) {
    bytes += RegularFileBytes(Index(android_data_root, name));
  }
  return bytes;
}

RobloxCacheClear ClearRobloxCache(const fs::path& android_data_root) {
  RobloxCacheClear result;
  std::error_code error;
  const fs::path storage = Storage(android_data_root);
  const fs::file_status status = fs::symlink_status(storage, error);
  if (fs::is_symlink(status)) {
    result.error = storage.string() + " is a link, so it was not cleared";
    return result;
  }
  result.bytes = RobloxCacheBytes(android_data_root);
  if (fs::is_directory(status)) {
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(storage, error), end;
         !error && it != end; it.increment(error)) {
      entries.push_back(it->path());
    }
    if (error) {
      result.error = "cannot list " + storage.string() + ": " + error.message();
      return result;
    }
    for (const fs::path& entry : entries) {
      fs::remove_all(entry, error);
      if (error) {
        result.error = "cannot remove " + entry.string() + ": " + error.message();
        return result;
      }
    }
  }
  for (const fs::path& folder : ContentProviderFolders(android_data_root)) {
    fs::remove_all(folder, error);
    if (error) {
      result.error = "cannot remove " + folder.string() + ": " + error.message();
      return result;
    }
  }
  for (const char* name : kIndexFiles) {
    const fs::path file = Index(android_data_root, name);
    fs::remove(file, error);
    if (error) {
      result.error = "cannot remove " + file.string() + ": " + error.message();
      return result;
    }
  }
  return result;
}

SingleInstanceLock AcquireRobloxDataLock(const fs::path& lock,
                                         std::chrono::milliseconds wait) {
  const auto deadline = std::chrono::steady_clock::now() + wait;
  for (;;) {
    SingleInstanceLock acquired = SingleInstanceLock::Acquire(lock);
    if (!acquired.already_running() ||
        std::chrono::steady_clock::now() >= deadline) {
      return acquired;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

fs::path RobloxDataLockPath(const RuntimePaths& paths) {
  return paths.android_runtime_root() / "in-use.lock";
}

}
