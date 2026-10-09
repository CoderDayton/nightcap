#ifndef MOCKTAIL_RUNTIME_RUNTIME_CONFIG_STORE_H_
#define MOCKTAIL_RUNTIME_RUNTIME_CONFIG_STORE_H_

#include <filesystem>
#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {

struct RuntimeConfigText {
  std::string text;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

struct RuntimeConfigSaveResult {
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

// Reads config.yaml as text. The file must be a regular file; a symlink is
// refused.
RuntimeConfigText ReadRuntimeConfigText(
    const std::filesystem::path& config_file);

// Replaces config.yaml with `text` in one step, with mode 0600. On any error
// the file on disk is left byte for byte as it was.
//
// `text` is first written beside the file and loaded the way Nightcap loads
// its configuration, with no environment overrides, so a value the loader
// rejects is never saved. `original` is the text the caller read and edited:
// when the file no longer holds it, something else changed the file and the
// save is refused. The file must exist and be a regular file.
RuntimeConfigSaveResult SaveRuntimeConfigText(
    const std::filesystem::path& config_file, std::string_view original,
    std::string_view text);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_RUNTIME_CONFIG_STORE_H_
