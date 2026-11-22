#include "ui/slot_files.h"

#include <unistd.h>

#include <string>

namespace mocktail {
namespace ui {
namespace {

std::filesystem::path Beside(const std::filesystem::path& target,
                             const char* suffix) {
  return target.parent_path() / ("." + target.filename().string() + "." +
                                 std::to_string(getpid()) + suffix);
}

}

bool ReplaceAllWith(const std::filesystem::path& source,
                    const std::vector<std::filesystem::path>& targets,
                    std::error_code* failure) {
  failure->clear();
  std::error_code ignored;
  for (const std::filesystem::path& target : targets) {
    if (*failure) break;
    std::filesystem::create_directories(target.parent_path(), *failure);
    if (*failure) break;
    std::filesystem::copy_file(
        source, Beside(target, ".new"),
        std::filesystem::copy_options::overwrite_existing, *failure);
  }
  std::size_t replaced = 0;
  for (; !*failure && replaced < targets.size(); ++replaced) {
    const std::filesystem::path& target = targets[replaced];
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(target, ignored);
    if (std::filesystem::exists(status)) {
      if (!std::filesystem::is_regular_file(status) &&
          !std::filesystem::is_symlink(status)) {
        *failure = std::make_error_code(std::errc::file_exists);
        break;
      }
      std::filesystem::rename(target, Beside(target, ".old"), *failure);
      if (*failure) break;
    }
    std::filesystem::rename(Beside(target, ".new"), target, *failure);
    if (*failure) {
      std::filesystem::rename(Beside(target, ".old"), target, ignored);
      break;
    }
  }
  if (*failure) {
    for (std::size_t index = 0; index < replaced; ++index) {
      const std::filesystem::path& target = targets[index];
      std::filesystem::remove(target, ignored);
      std::filesystem::rename(Beside(target, ".old"), target, ignored);
    }
  }
  for (const std::filesystem::path& target : targets) {
    std::filesystem::remove(Beside(target, ".new"), ignored);
    std::filesystem::remove(Beside(target, ".old"), ignored);
  }
  return !*failure;
}

std::filesystem::path FirstRegularFile(
    const std::vector<std::filesystem::path>& files) {
  std::error_code error;
  for (const std::filesystem::path& file : files) {
    if (std::filesystem::is_regular_file(file, error)) return file;
  }
  return {};
}

bool RemoveAll(const std::vector<std::filesystem::path>& files,
               std::error_code* failure) {
  failure->clear();
  for (const std::filesystem::path& file : files) {
    std::filesystem::remove(file, *failure);
    if (*failure) return false;
  }
  return true;
}

}
}
