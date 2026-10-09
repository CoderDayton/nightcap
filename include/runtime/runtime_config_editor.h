#ifndef MOCKTAIL_RUNTIME_RUNTIME_CONFIG_EDITOR_H_
#define MOCKTAIL_RUNTIME_RUNTIME_CONFIG_EDITOR_H_

#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {

struct RuntimeConfigEdit {
  std::string text;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

// Returns `text`, the contents of a config.yaml, with the value at `key` set.
// `key` is a dotted path such as "graphics.vsync". Only the bytes that carry
// the key change: comments, order, line endings, and unrelated keys are kept.
//
// - A key with a value has that value replaced, whatever its YAML style.
// - A key that is present only as a comment ("# vsync: off") is uncommented
//   in place, together with any commented section above it.
// - A key that is absent is added at the end of its section, and a missing
//   section is added at the end of the file.
// - A key that holds a section becomes a single value; the section's lines
//   stay in the file as comments.
// - A value that sits where a section is needed makes way for the section.
//
// `text` must be valid UTF-8 YAML whose top level is a mapping or empty. Every
// result is parsed again and compared with `text`; an edit that would change
// any other setting is refused with an error and no text.
RuntimeConfigEdit SetRuntimeConfigBool(std::string_view text,
                                       std::string_view key, bool value);
RuntimeConfigEdit SetRuntimeConfigInteger(std::string_view text,
                                          std::string_view key,
                                          long long value);
RuntimeConfigEdit SetRuntimeConfigString(std::string_view text,
                                         std::string_view key,
                                         std::string_view value);

// Returns `text` without `key`. The key's lines stay as comments, in the form
// the Set functions uncomment, and so does a section it leaves with no keys.
// A key the file does not have leaves `text` unchanged.
RuntimeConfigEdit UnsetRuntimeConfigValue(std::string_view text,
                                          std::string_view key);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_RUNTIME_CONFIG_EDITOR_H_
