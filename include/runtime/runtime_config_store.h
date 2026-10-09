#ifndef MOCKTAIL_RUNTIME_RUNTIME_CONFIG_STORE_H_
#define MOCKTAIL_RUNTIME_RUNTIME_CONFIG_STORE_H_

#include <filesystem>
#include <functional>
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

// Reads a text file that may not exist. A missing file reads as empty text.
// A symlink or anything that is not a regular file is refused.
RuntimeConfigText ReadOptionalTextFile(const std::filesystem::path& file);

// Given the path of a file holding the text about to be saved, returns why it
// must not be saved, or an empty string.
using TextFileCheck =
    std::function<std::string(const std::filesystem::path&)>;

// SaveRuntimeConfigText for another text file, with `check` in place of the
// configuration loader. A missing file counts as holding empty text, and the
// save creates it. Its directory must exist.
RuntimeConfigSaveResult SaveCheckedTextFile(const std::filesystem::path& file,
                                            std::string_view original,
                                            std::string_view text,
                                            const TextFileCheck& check);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_RUNTIME_CONFIG_STORE_H_
