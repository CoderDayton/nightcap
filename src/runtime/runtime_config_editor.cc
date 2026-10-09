#include "runtime/runtime_config_editor.h"

#include <yaml.h>

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::size_t kIndentStep = 2;
// Separates the names of a flattened key path. It cannot appear in a key the
// editor accepts.
constexpr char kPathSeparator = '\x1f';
constexpr char kEmptyCollection[] = "\x1e";
// Aliases let a small file describe a huge tree. A file that needs more
// visits than this to walk is refused.
constexpr std::size_t kMaxNodeVisits = 100000;

bool IsKeyCharacter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool SplitKey(std::string_view key, std::vector<std::string_view>* path) {
  while (true) {
    const std::size_t dot = key.find('.');
    const std::string_view segment = key.substr(0, dot);
    if (segment.empty()) {
      return false;
    }
    for (const char c : segment) {
      if (!IsKeyCharacter(c)) {
        return false;
      }
    }
    path->push_back(segment);
    if (dot == std::string_view::npos) {
      return true;
    }
    key.remove_prefix(dot + 1);
  }
}

// Length of the line break at `at`, using libyaml's set of breaks, or 0.
std::size_t BreakLength(std::string_view text, std::size_t at) {
  const auto byte = [&text](std::size_t index) {
    return index < text.size() ? static_cast<unsigned char>(text[index]) : 0u;
  };
  if (byte(at) == '\r') {
    return byte(at + 1) == '\n' ? 2 : 1;
  }
  if (byte(at) == '\n') {
    return 1;
  }
  if (byte(at) == 0xc2 && byte(at + 1) == 0x85) {
    return 2;
  }
  if (byte(at) == 0xe2 && byte(at + 1) == 0x80 &&
      (byte(at + 2) == 0xa8 || byte(at + 2) == 0xa9)) {
    return 3;
  }
  return 0;
}

struct Edit {
  std::size_t begin = 0;
  std::size_t end = 0;
  std::string text;
};

// One "# name:" line: a key that is commented out.
struct CommentedKey {
  // Column the key has once uncommented.
  std::size_t indent = 0;
  std::string_view name;
  // Everything after the colon.
  std::string_view rest;
};

bool ParseCommentedKey(std::string_view line, CommentedKey* parsed) {
  const std::size_t hash = line.find_first_not_of(' ');
  if (hash == std::string_view::npos || line[hash] != '#') {
    return false;
  }
  const std::size_t name_at = line.find_first_not_of(' ', hash + 1);
  if (name_at == std::string_view::npos || name_at == hash + 1) {
    return false;
  }
  std::size_t end = name_at;
  while (end < line.size() && IsKeyCharacter(line[end])) {
    ++end;
  }
  if (end == name_at || end == line.size() || line[end] != ':' ||
      (end + 1 < line.size() && line[end + 1] != ' ')) {
    return false;
  }
  // "# key" uncomments to the column of the "#".
  parsed->indent = name_at - 2;
  parsed->name = line.substr(name_at, end - name_at);
  parsed->rest = line.substr(end + 1);
  return true;
}

// The comment that follows a value, with the spaces before it, or empty.
std::string_view TrailingComment(std::string_view rest) {
  char quote = '\0';
  for (std::size_t index = 0; index < rest.size(); ++index) {
    const char c = rest[index];
    if (quote != '\0') {
      if (c == '\\' && quote == '"') {
        ++index;
      } else if (c == quote) {
        quote = '\0';
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '#' && index > 0 && rest[index - 1] == ' ') {
      std::size_t start = index;
      while (start > 0 && rest[start - 1] == ' ') {
        --start;
      }
      const std::size_t value = rest.find_first_not_of(' ');
      return rest.substr(value < start ? start : index - 1);
    }
  }
  return {};
}

void AppendHexEscape(unsigned char byte, std::string* scalar) {
  constexpr char kDigits[] = "0123456789ABCDEF";
  scalar->append("\\x");
  scalar->push_back(kDigits[byte >> 4]);
  scalar->push_back(kDigits[byte & 0xf]);
}

// The YAML text of a string value: plain when that reads back unchanged,
// otherwise double quoted with line breaks and control characters escaped.
std::string QuoteString(std::string_view value) {
  bool plain = !value.empty() && value.front() != ' ' && value.back() != ' ' &&
               value.front() != '-';
  for (const char c : value) {
    if (!(IsKeyCharacter(c) || c == '.' || c == '/' || c == ' ')) {
      plain = false;
    }
  }
  if (plain) {
    return std::string(value);
  }
  const auto byte = [&value](std::size_t index) {
    return index < value.size() ? static_cast<unsigned char>(value[index])
                                : 0u;
  };
  std::string scalar(1, '"');
  for (std::size_t index = 0; index < value.size(); ++index) {
    const unsigned c = byte(index);
    if (c == '"' || c == '\\') {
      scalar.push_back('\\');
      scalar.push_back(static_cast<char>(c));
    } else if (c < 0x20 || c == 0x7f) {
      AppendHexEscape(static_cast<unsigned char>(c), &scalar);
    } else if (c == 0xc2 && byte(index + 1) >= 0x80 &&
               byte(index + 1) <= 0x9f) {
      // U+0080 to U+009F, which includes the next-line break U+0085.
      AppendHexEscape(static_cast<unsigned char>(byte(index + 1)), &scalar);
      ++index;
    } else if (c == 0xe2 && byte(index + 1) == 0x80 &&
               (byte(index + 2) == 0xa8 || byte(index + 2) == 0xa9)) {
      scalar.append(byte(index + 2) == 0xa8 ? "\\L" : "\\P");
      index += 2;
    } else if (c == 0xef && byte(index + 1) == 0xbb &&
               byte(index + 2) == 0xbf) {
      scalar.append("\\uFEFF");
      index += 2;
    } else {
      scalar.push_back(static_cast<char>(c));
    }
  }
  scalar.push_back('"');
  return scalar;
}

std::string KeyText(std::size_t indent, std::string_view name) {
  std::string text(indent, ' ');
  text.append(name);
  text.push_back(':');
  return text;
}

std::string_view ScalarValue(const yaml_node_t* node) {
  return {reinterpret_cast<const char*>(node->data.scalar.value),
          node->data.scalar.length};
}

bool IsNull(const yaml_node_t* node) {
  return node->type == YAML_SCALAR_NODE &&
         node->start_mark.index == node->end_mark.index;
}

bool IsBlockCollection(const yaml_node_t* node) {
  return (node->type == YAML_MAPPING_NODE &&
          node->data.mapping.style != YAML_FLOW_MAPPING_STYLE) ||
         (node->type == YAML_SEQUENCE_NODE &&
          node->data.sequence.style != YAML_FLOW_SEQUENCE_STYLE);
}

struct Pair {
  yaml_node_t* mapping = nullptr;
  yaml_node_t* key = nullptr;
  yaml_node_t* value = nullptr;
  // The value is written as "*name". `value` is then the node the alias
  // names, whose text is elsewhere; the alias itself is at these bytes.
  bool alias = false;
  std::size_t alias_begin = 0;
  std::size_t alias_end = 0;
};

struct LineInfo {
  std::size_t begin = 0;
  // End of the line's text, before its break.
  std::size_t end = 0;
  // Inside a value that spans lines, so a "#" here is not a comment.
  bool content = false;
  // Starts with a block-mapping key.
  bool key = false;
  std::size_t indent = 0;
  std::string_view name;
};

// A parsed config file with the byte position of every node.
class Source {
 public:
  explicit Source(std::string_view text) : text_(text) {}
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;
  ~Source() {
    if (loaded_) {
      yaml_document_delete(&document_);
    }
  }

  bool Parse(std::string* error);

  std::string_view text() const { return text_; }
  const std::string& eol() const { return eol_; }
  std::vector<LineInfo>& lines() { return lines_; }
  yaml_node_t* root() { return yaml_document_get_root_node(&document_); }
  yaml_node_t* Node(int index) {
    return yaml_document_get_node(&document_, index);
  }
  // The block scalar whose text runs to the end of a file that has no final
  // line break, or null.
  yaml_node_t* open_block_scalar() { return open_block_scalar_; }

  // libyaml marks count characters and skip a byte order mark.
  std::size_t Byte(const yaml_mark_t& mark) const {
    return char_to_byte_[std::min(mark.index, char_to_byte_.size() - 1)];
  }
  std::size_t LineAt(std::size_t byte) const {
    std::size_t low = 0;
    std::size_t high = lines_.size();
    while (high - low > 1) {
      const std::size_t middle = low + (high - low) / 2;
      (lines_[middle].begin <= byte ? low : high) = middle;
    }
    return low;
  }
  // Byte of a column that only spaces precede.
  std::size_t ColumnByte(std::size_t line, std::size_t column) const {
    return lines_[line].begin + (line == 0 ? bom_ : 0) + column;
  }

  // One past the last byte the node's text occupies. libyaml ends a block
  // collection at the next token and a block scalar after the blank lines
  // that follow it; this stops at the node's own last character.
  std::size_t LastByte(yaml_node_t* node);
  // First byte after the key's colon, or the key's end when it has none.
  std::size_t AfterColon(const Pair& pair, bool* has_colon = nullptr);
  // One past the last byte of the pair's value, or of its colon when the
  // value is empty.
  std::size_t ValueEnd(const Pair& pair) {
    return pair.alias ? pair.alias_end
                      : std::max(LastByte(pair.value), AfterColon(pair));
  }
  // End of the anchor and tag that begin at `from` or after spaces that
  // follow it, or `from` when there are none.
  std::size_t PropertiesEnd(std::size_t from) const;

  Pair MakePair(yaml_node_t* mapping, const yaml_node_pair_t* raw);
  Pair FindPair(yaml_node_t* mapping, std::string_view name);
  void Flatten(yaml_node_t* node, const std::string& prefix,
               std::map<std::string, std::string>* leaves);

 private:
  bool Check(yaml_node_t* node, std::vector<char>* open, std::size_t* visits,
             std::string* error);
  void IndexNode(yaml_node_t* node);

  std::string_view text_;
  std::size_t bom_ = 0;
  std::string eol_ = "\n";
  std::vector<std::size_t> char_to_byte_;
  std::vector<LineInfo> lines_;
  yaml_document_t document_;
  bool loaded_ = false;
  yaml_node_t* open_block_scalar_ = nullptr;
};

bool Source::Parse(std::string* error) {
  const auto byte = [this](std::size_t index) {
    return index < text_.size() ? static_cast<unsigned char>(text_[index])
                                : 0u;
  };
  if ((byte(0) == 0xff && byte(1) == 0xfe) ||
      (byte(0) == 0xfe && byte(1) == 0xff)) {
    *error = "the file is not UTF-8";
    return false;
  }
  bom_ = byte(0) == 0xef && byte(1) == 0xbb && byte(2) == 0xbf ? 3 : 0;
  for (std::size_t at = bom_; at < text_.size();) {
    char_to_byte_.push_back(at);
    const unsigned lead = byte(at);
    at += lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
  }
  char_to_byte_.push_back(text_.size());

  bool eol_found = false;
  LineInfo line;
  for (std::size_t at = 0; at < text_.size();) {
    const std::size_t length = BreakLength(text_, at);
    if (length == 0) {
      ++at;
      continue;
    }
    if (!eol_found) {
      eol_found = true;
      eol_ = std::string(text_.substr(at, length));
    }
    line.end = at;
    lines_.push_back(line);
    at += length;
    line.begin = at;
  }
  line.end = text_.size();
  lines_.push_back(line);

  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) {
    *error = "out of memory";
    return false;
  }
  yaml_parser_set_input_string(
      &parser, reinterpret_cast<const unsigned char*>(text_.data()),
      text_.size());
  loaded_ = yaml_parser_load(&parser, &document_) != 0;
  if (!loaded_) {
    *error = "the file is not valid YAML";
    if (parser.problem != nullptr) {
      *error += std::string(": ") + parser.problem + " on line " +
                std::to_string(parser.problem_mark.line + 1);
    }
  }
  yaml_parser_delete(&parser);
  if (!loaded_) {
    return false;
  }
  if (root() != nullptr) {
    std::vector<char> open(static_cast<std::size_t>(document_.nodes.top -
                                                    document_.nodes.start));
    std::size_t visits = 0;
    if (!Check(root(), &open, &visits, error)) {
      return false;
    }
    IndexNode(root());
  }
  return true;
}

// Refuses a tree the other walks cannot finish or cannot compare: one that
// loops through an alias, one that aliases blow up, or one with a repeated
// key.
bool Source::Check(yaml_node_t* node, std::vector<char>* open,
                   std::size_t* visits, std::string* error) {
  if (++*visits > kMaxNodeVisits) {
    *error = "the file's aliases repeat too much of it";
    return false;
  }
  if (node->type == YAML_SCALAR_NODE) {
    return true;
  }
  char& is_open = (*open)[static_cast<std::size_t>(node - document_.nodes.start)];
  if (is_open) {
    *error = "an alias in the file names the section it is in";
    return false;
  }
  is_open = 1;
  if (node->type == YAML_MAPPING_NODE) {
    std::set<std::string_view> names;
    for (yaml_node_pair_t* pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top; ++pair) {
      yaml_node_t* key = Node(pair->key);
      if (key->type == YAML_SCALAR_NODE &&
          !names.insert(ScalarValue(key)).second) {
        *error = "the key " + std::string(ScalarValue(key)) +
                 " is set twice, again on line " +
                 std::to_string(key->start_mark.line + 1);
        return false;
      }
      if (!Check(Node(pair->value), open, visits, error)) {
        return false;
      }
    }
  } else {
    for (yaml_node_item_t* item = node->data.sequence.items.start;
         item < node->data.sequence.items.top; ++item) {
      if (!Check(Node(*item), open, visits, error)) {
        return false;
      }
    }
  }
  is_open = 0;
  return true;
}

std::size_t Source::LastByte(yaml_node_t* node) {
  if (node->type == YAML_SCALAR_NODE) {
    const std::size_t begin = Byte(node->start_mark);
    std::size_t end = Byte(node->end_mark);
    if (node->data.scalar.style == YAML_LITERAL_SCALAR_STYLE ||
        node->data.scalar.style == YAML_FOLDED_SCALAR_STYLE) {
      // Step back over the breaks and blank lines libyaml took with it.
      while (end > begin + 1) {
        const std::size_t line = LineAt(end - 1);
        if (end > lines_[line].end) {
          end = lines_[line].end;
        } else if (text_[end - 1] == ' ') {
          --end;
        } else {
          break;
        }
      }
    }
    return end;
  }
  if (!IsBlockCollection(node)) {
    return Byte(node->end_mark);
  }
  if (node->type == YAML_MAPPING_NODE) {
    if (node->data.mapping.pairs.start == node->data.mapping.pairs.top) {
      return Byte(node->end_mark);
    }
    const yaml_node_pair_t* last = node->data.mapping.pairs.top - 1;
    return std::max(Byte(Node(last->key)->end_mark),
                    LastByte(Node(last->value)));
  }
  if (node->data.sequence.items.start == node->data.sequence.items.top) {
    return Byte(node->end_mark);
  }
  return LastByte(Node(*(node->data.sequence.items.top - 1)));
}

std::size_t Source::AfterColon(const Pair& pair, bool* has_colon) {
  std::size_t at = Byte(pair.key->end_mark);
  const std::size_t colon = text_.find_first_not_of(' ', at);
  const bool found = colon != std::string_view::npos && text_[colon] == ':';
  if (has_colon != nullptr) {
    *has_colon = found;
  }
  return found ? colon + 1 : at;
}

Pair Source::FindPair(yaml_node_t* mapping, std::string_view name) {
  if (mapping == nullptr || mapping->type != YAML_MAPPING_NODE) {
    return {};
  }
  for (yaml_node_pair_t* pair = mapping->data.mapping.pairs.start;
       pair < mapping->data.mapping.pairs.top; ++pair) {
    yaml_node_t* key = Node(pair->key);
    if (key->type == YAML_SCALAR_NODE && ScalarValue(key) == name) {
      return MakePair(mapping, pair);
    }
  }
  return {};
}

Pair Source::MakePair(yaml_node_t* mapping, const yaml_node_pair_t* raw) {
  Pair pair;
  pair.mapping = mapping;
  pair.key = Node(raw->key);
  pair.value = Node(raw->value);
  bool has_colon = false;
  const std::size_t star =
      text_.find_first_not_of(' ', AfterColon(pair, &has_colon));
  // Only an alias can start with "*".
  if (has_colon && star != std::string_view::npos && text_[star] == '*') {
    const bool flow = mapping->data.mapping.style == YAML_FLOW_MAPPING_STYLE;
    std::size_t end = star + 1;
    while (end < text_.size() && text_[end] != ' ' &&
           BreakLength(text_, end) == 0 &&
           !(flow && (text_[end] == ',' || text_[end] == '}'))) {
      ++end;
    }
    pair.alias = true;
    pair.alias_begin = star;
    pair.alias_end = end;
  }
  return pair;
}

std::size_t Source::PropertiesEnd(std::size_t from) const {
  std::size_t end = from;
  while (true) {
    const std::size_t at = text_.find_first_not_of(' ', end);
    if (at == std::string_view::npos ||
        (text_[at] != '&' && text_[at] != '!')) {
      return end;
    }
    end = at;
    while (end < text_.size() && text_[end] != ' ' &&
           BreakLength(text_, end) == 0) {
      ++end;
    }
  }
}

void Source::Flatten(yaml_node_t* node, const std::string& prefix,
                     std::map<std::string, std::string>* leaves) {
  if (node->type == YAML_SCALAR_NODE) {
    (*leaves)[prefix] = std::string(ScalarValue(node));
    return;
  }
  bool empty = true;
  if (node->type == YAML_MAPPING_NODE) {
    for (yaml_node_pair_t* pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top; ++pair) {
      empty = false;
      yaml_node_t* key = Node(pair->key);
      const std::string name = key->type == YAML_SCALAR_NODE
                                   ? std::string(ScalarValue(key))
                                   : std::string("?");
      Flatten(Node(pair->value),
              prefix.empty() ? name : prefix + kPathSeparator + name, leaves);
    }
  } else {
    std::size_t index = 0;
    for (yaml_node_item_t* item = node->data.sequence.items.start;
         item < node->data.sequence.items.top; ++item, ++index) {
      empty = false;
      Flatten(Node(*item), prefix + kPathSeparator + std::to_string(index),
              leaves);
    }
  }
  if (empty) {
    (*leaves)[prefix] = kEmptyCollection;
  }
}

void Source::IndexNode(yaml_node_t* node) {
  const std::size_t first = LineAt(Byte(node->start_mark));
  const std::size_t last_byte = LastByte(node);
  const std::size_t last = LineAt(last_byte > 0 ? last_byte - 1 : 0);
  if (!IsBlockCollection(node)) {
    for (std::size_t line = first + 1; line <= last; ++line) {
      lines_[line].content = true;
    }
    if (node->type == YAML_SCALAR_NODE && last > first &&
        last_byte == text_.size() &&
        (node->data.scalar.style == YAML_LITERAL_SCALAR_STYLE ||
         node->data.scalar.style == YAML_FOLDED_SCALAR_STYLE)) {
      open_block_scalar_ = node;
    }
    return;
  }
  if (node->type == YAML_SEQUENCE_NODE) {
    for (yaml_node_item_t* item = node->data.sequence.items.start;
         item < node->data.sequence.items.top; ++item) {
      IndexNode(Node(*item));
    }
    return;
  }
  for (yaml_node_pair_t* pair = node->data.mapping.pairs.start;
       pair < node->data.mapping.pairs.top; ++pair) {
    yaml_node_t* key = Node(pair->key);
    const std::size_t key_byte = Byte(key->start_mark);
    LineInfo& line = lines_[LineAt(key_byte)];
    if (key->type == YAML_SCALAR_NODE &&
        ColumnByte(LineAt(key_byte), key->start_mark.column) == key_byte &&
        text_.substr(line.begin, key_byte - line.begin)
                .find_first_not_of(" \xef\xbb\xbf") ==
            std::string_view::npos) {
      line.key = true;
      line.indent = key->start_mark.column;
      line.name = ScalarValue(key);
    }
    IndexNode(Node(pair->value));
  }
}

// First byte of a line's text that is not a space, or its end.
std::size_t FirstText(Source& source, std::size_t line) {
  const LineInfo& info = source.lines()[line];
  const std::size_t at =
      source.text().find_first_not_of(' ', source.ColumnByte(line, 0));
  return at == std::string_view::npos || at > info.end ? info.end : at;
}

// Comments out every line of text in [first, last], putting the "#" at
// `column` or at the line's own indentation when that is shallower.
void CommentOut(Source& source, std::size_t first, std::size_t last,
                std::size_t column, std::vector<Edit>* edits) {
  for (std::size_t line = first; line <= last; ++line) {
    const LineInfo& info = source.lines()[line];
    const std::size_t text = FirstText(source, line);
    if (text == info.end ||
        (source.text()[text] == '#' && !info.content)) {
      continue;
    }
    const std::size_t at = std::min(source.ColumnByte(line, column), text);
    edits->push_back({at, at, "# "});
  }
}

// Last line of the section that ends at `anchor`, counting the comments below
// it that are indented to at least `min_column`. Blank lines and shallower
// comments in between are passed over.
std::size_t ExtendOverComments(Source& source, std::size_t anchor,
                               std::size_t min_column) {
  for (std::size_t line = anchor + 1; line < source.lines().size(); ++line) {
    const LineInfo& info = source.lines()[line];
    const std::size_t text = FirstText(source, line);
    if (text == info.end) {
      continue;
    }
    if (info.key || info.content || source.text()[text] != '#') {
      break;
    }
    if (text - source.ColumnByte(line, 0) >= min_column) {
      anchor = line;
    }
  }
  return anchor;
}

void InsertLinesAfter(Source& source, std::size_t line,
                      const std::vector<std::string>& added,
                      std::vector<Edit>* edits) {
  Edit edit;
  if (line + 1 < source.lines().size()) {
    edit.begin = edit.end = source.lines()[line + 1].begin;
    for (const std::string& text : added) {
      edit.text += text + source.eol();
    }
  } else {
    edit.begin = edit.end = source.text().size();
    for (const std::string& text : added) {
      edit.text += source.eol() + text;
    }
    edit.text += source.eol();
    if (yaml_node_t* scalar = source.open_block_scalar()) {
      // The line break this adds would become part of the scalar. Stripping
      // its final breaks keeps the value it has now.
      const std::size_t header = source.Byte(scalar->start_mark);
      const std::size_t header_end =
          source.lines()[source.LineAt(header)].end;
      std::string_view indicators =
          source.text().substr(header, header_end - header);
      indicators = indicators.substr(0, indicators.find_first_of(" #"));
      const std::size_t keep = indicators.find('+');
      if (keep != std::string_view::npos) {
        edits->push_back({header + keep, header + keep + 1, "-"});
      } else if (indicators.find('-') == std::string_view::npos) {
        edits->push_back({header + 1, header + 1, "-"});
      }
    }
  }
  edits->push_back(std::move(edit));
}

void AppendLines(Source& source, const std::vector<std::string>& added,
                 std::vector<Edit>* edits) {
  const std::size_t last = source.lines().size() - 1;
  if (source.lines()[last].begin != source.lines()[last].end) {
    InsertLinesAfter(source, last, added, edits);
    return;
  }
  Edit edit;
  edit.begin = edit.end = source.text().size();
  for (const std::string& text : added) {
    edit.text += text + source.eol();
  }
  edits->push_back(std::move(edit));
}

std::string Apply(std::string_view text, std::vector<Edit> edits) {
  std::stable_sort(edits.begin(), edits.end(),
                   [](const Edit& left, const Edit& right) {
                     return left.begin < right.begin;
                   });
  std::string result;
  std::size_t at = 0;
  for (const Edit& edit : edits) {
    if (edit.begin < at) {
      // Inside text an earlier edit already replaced.
      continue;
    }
    result.append(text.substr(at, edit.begin - at));
    result.append(edit.text);
    at = std::max(at, edit.end);
  }
  result.append(text.substr(at));
  return result;
}

struct StackEntry {
  std::size_t indent = 0;
  std::string_view name;
  std::size_t line = 0;
};

void PopToParentOf(std::size_t indent, std::vector<StackEntry>* entries) {
  while (!entries->empty() && entries->back().indent >= indent) {
    entries->pop_back();
  }
}

// Whether `entries` followed by `name` is a prefix of `path`, or `path`.
bool OnPath(const std::vector<StackEntry>& entries, std::string_view name,
            const std::vector<std::string_view>& path) {
  if (entries.size() >= path.size() || path[entries.size()] != name) {
    return false;
  }
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (entries[index].name != path[index]) {
      return false;
    }
  }
  return true;
}

// Lines of the commented-out keys that lead from the section holding the
// first `depth` names of `path` down to `path` itself, or empty. Commented
// keys above that section are not used: uncommenting one would repeat a key
// the file already has. Nor is a chain used when the next line of settings
// is indented past `column`, the column the chain's first key would take:
// uncommented there, the chain would cut that line off from its section.
std::vector<std::size_t> FindCommentedChain(
    Source& source, const std::vector<std::string_view>& path,
    std::size_t depth, std::size_t column) {
  std::vector<StackEntry> active;
  std::vector<StackEntry> overlay;
  std::size_t overlay_base = 0;
  // Indentation of the last commented key that is off the path. Commented
  // keys indented past it belong to it.
  bool off_path = false;
  std::size_t off_path_indent = 0;
  for (std::size_t index = 0; index < source.lines().size(); ++index) {
    const LineInfo& info = source.lines()[index];
    if (info.key) {
      off_path = false;
      overlay.clear();
      PopToParentOf(info.indent, &active);
      active.push_back({info.indent, info.name, index});
      continue;
    }
    if (info.content) {
      continue;
    }
    const std::size_t begin = source.ColumnByte(index, 0);
    CommentedKey parsed;
    if (begin > info.end ||
        !ParseCommentedKey(source.text().substr(begin, info.end - begin),
                           &parsed)) {
      continue;
    }
    if (off_path && parsed.indent > off_path_indent) {
      continue;
    }
    off_path = false;
    std::vector<StackEntry> chain = active;
    if (!overlay.empty()) {
      chain.resize(overlay_base);
      chain.insert(chain.end(), overlay.begin(), overlay.end());
    }
    PopToParentOf(parsed.indent, &chain);
    if (!OnPath(chain, parsed.name, path)) {
      // A commented key off the path still ends the commented sections that
      // are indented at least as far.
      PopToParentOf(parsed.indent, &overlay);
      off_path = true;
      off_path_indent = parsed.indent;
      continue;
    }
    if (overlay.empty() || chain.size() < overlay_base) {
      overlay_base = chain.size();
    }
    overlay.assign(chain.begin() + static_cast<std::ptrdiff_t>(overlay_base),
                   chain.end());
    overlay.push_back({parsed.indent, parsed.name, index});
    if (chain.size() + 1 != path.size() || overlay_base != depth) {
      continue;
    }
    bool cuts_a_section = false;
    for (std::size_t next = index + 1; next < source.lines().size(); ++next) {
      const LineInfo& following = source.lines()[next];
      const std::size_t text = FirstText(source, next);
      if (text == following.end ||
          (source.text()[text] == '#' && !following.content)) {
        continue;
      }
      cuts_a_section = following.content ||
                       text - source.ColumnByte(next, 0) > column;
      break;
    }
    if (!cuts_a_section) {
      std::vector<std::size_t> lines;
      for (const StackEntry& entry : overlay) {
        lines.push_back(entry.line);
      }
      return lines;
    }
  }
  return {};
}

// "a: {b: {c: value}}" for the names of `path` from `depth` on.
std::string FlowText(const std::vector<std::string_view>& path,
                     std::size_t depth, const std::string& scalar) {
  std::string text;
  for (std::size_t index = depth; index < path.size(); ++index) {
    text += std::string(path[index]) + ": ";
    if (index + 1 < path.size()) {
      text += "{";
    }
  }
  text += scalar;
  text.append(path.size() - depth - 1, '}');
  return text;
}

std::string JoinPath(const std::vector<std::string_view>& path,
                     std::size_t count) {
  std::string joined;
  for (std::size_t index = 0; index < count; ++index) {
    if (index > 0) {
      joined.push_back(kPathSeparator);
    }
    joined.append(path[index]);
  }
  return joined;
}

void EraseSubtree(const std::string& prefix,
                  std::map<std::string, std::string>* leaves) {
  leaves->erase(prefix);
  const std::string children = prefix + kPathSeparator;
  auto it = leaves->lower_bound(children);
  while (it != leaves->end() &&
         it->first.compare(0, children.size(), children) == 0) {
    it = leaves->erase(it);
  }
}

RuntimeConfigEdit Fail(std::string_view key, const std::string& reason) {
  RuntimeConfigEdit edit;
  edit.error = "cannot change " + std::string(key) + ": " + reason;
  return edit;
}

bool IsFlowMapping(const yaml_node_t* node) {
  return node->type == YAML_MAPPING_NODE &&
         node->data.mapping.style == YAML_FLOW_MAPPING_STYLE;
}

std::size_t PairCount(const yaml_node_t* mapping) {
  return static_cast<std::size_t>(mapping->data.mapping.pairs.top -
                                  mapping->data.mapping.pairs.start);
}

// Writes `text` as the value of a key that has none yet.
void FillEmptyValue(Source& source, const Pair& pair, const std::string& text,
                    std::vector<Edit>* edits) {
  bool has_colon = false;
  const std::size_t at = source.AfterColon(pair, &has_colon);
  edits->push_back({at, at, (has_colon ? " " : ": ") + text});
}

// Comments out the lines of a block section or list below its key. An
// anchor or tag can put the value's start on the key's own line; that line
// stays.
void CommentOutBlockValue(Source& source, const Pair& pair,
                          std::vector<Edit>* edits) {
  const std::size_t key_line =
      source.LineAt(source.Byte(pair.key->start_mark));
  CommentOut(source,
             std::max(source.LineAt(source.Byte(pair.value->start_mark)),
                      key_line + 1),
             source.LineAt(source.LastByte(pair.value) - 1),
             pair.key->start_mark.column, edits);
}

// Last line a block section owns. A block value that keeps its final line
// breaks ("|+") owns the blank lines after it.
std::size_t LastLine(Source& source, yaml_node_t* node) {
  std::size_t line = source.LineAt(source.LastByte(node) - 1);
  yaml_node_t* leaf = node;
  while (IsBlockCollection(leaf)) {
    if (leaf->type == YAML_MAPPING_NODE) {
      if (PairCount(leaf) == 0) {
        return line;
      }
      leaf = source.Node((leaf->data.mapping.pairs.top - 1)->value);
    } else {
      if (leaf->data.sequence.items.start == leaf->data.sequence.items.top) {
        return line;
      }
      leaf = source.Node(*(leaf->data.sequence.items.top - 1));
    }
  }
  if (leaf->type != YAML_SCALAR_NODE ||
      (leaf->data.scalar.style != YAML_LITERAL_SCALAR_STYLE &&
       leaf->data.scalar.style != YAML_FOLDED_SCALAR_STYLE) ||
      source.LineAt(source.LastByte(leaf) - 1) != line) {
    return line;
  }
  const std::size_t header = source.Byte(leaf->start_mark);
  std::string_view indicators = source.text().substr(
      header, source.lines()[source.LineAt(header)].end - header);
  indicators = indicators.substr(0, indicators.find_first_of(" #"));
  if (indicators.find('+') == std::string_view::npos) {
    return line;
  }
  // The last entry of lines() is the text after the final line break, which
  // the value does not own.
  while (line + 2 < source.lines().size() &&
         FirstText(source, line + 1) == source.lines()[line + 1].end) {
    ++line;
  }
  return line;
}

void SetExisting(Source& source, const Pair& pair, const std::string& scalar,
                 std::vector<Edit>* edits) {
  yaml_node_t* value = pair.value;
  if (pair.alias) {
    edits->push_back({pair.alias_begin, pair.alias_end, scalar});
  } else if (IsNull(value)) {
    FillEmptyValue(source, pair, scalar, edits);
  } else if (IsBlockCollection(value)) {
    // A section becomes a single value: its lines stay as comments.
    CommentOutBlockValue(source, pair, edits);
    const std::size_t at = source.PropertiesEnd(source.AfterColon(pair));
    edits->push_back({at, at, " " + scalar});
  } else {
    // An anchor or tag in front of the value stays.
    const std::size_t begin = source.Byte(value->start_mark);
    const std::size_t end = source.LastByte(value);
    const std::size_t properties = source.PropertiesEnd(begin);
    const std::size_t content =
        source.text().find_first_not_of(" \r\n", properties);
    if (properties == begin) {
      edits->push_back({begin, end, scalar});
    } else if (content == std::string_view::npos || content >= end) {
      edits->push_back({properties, properties, " " + scalar});
    } else {
      edits->push_back({content, end, scalar});
    }
  }
}

// Adds a key that the file does not have. `chain` holds the sections of
// `path` that do exist. Returns whether the innermost one held a value that
// had to make way for a section.
bool SetMissing(Source& source, yaml_node_t* root,
                const std::vector<Pair>& chain,
                const std::vector<std::string_view>& path,
                const std::string& scalar, std::vector<Edit>* edits) {
  const std::size_t depth = chain.size();
  yaml_node_t* container = depth > 0 ? chain.back().value : root;
  const bool is_mapping =
      container != nullptr && container->type == YAML_MAPPING_NODE;

  if (depth > 0 && !is_mapping && IsFlowMapping(chain.back().mapping)) {
    const std::string text = "{" + FlowText(path, depth, scalar) + "}";
    if (IsNull(container)) {
      FillEmptyValue(source, chain.back(), text, edits);
    } else {
      edits->push_back({source.Byte(container->start_mark),
                        source.LastByte(container), text});
    }
    return true;
  }
  if (is_mapping && IsFlowMapping(container)) {
    if (PairCount(container) == 0) {
      const std::size_t at =
          source.text().find('{', source.Byte(container->start_mark)) + 1;
      edits->push_back({at, at, FlowText(path, depth, scalar)});
    } else {
      const std::size_t at = source.ValueEnd(source.MakePair(
          container, container->data.mapping.pairs.top - 1));
      edits->push_back({at, at, ", " + FlowText(path, depth, scalar)});
    }
    return false;
  }

  std::size_t indent = 0;
  std::size_t anchor = 0;
  if (depth > 0 && !is_mapping) {
    const Pair& parent = chain.back();
    const std::size_t column = parent.key->start_mark.column;
    const std::size_t after = source.AfterColon(parent);
    if (IsBlockCollection(container)) {
      CommentOutBlockValue(source, parent, edits);
    } else if (!IsNull(container)) {
      edits->push_back({after, source.LastByte(container), ""});
    }
    indent = column + kIndentStep;
    anchor = ExtendOverComments(
        source,
        source.LineAt(std::max(source.LastByte(container), after) - 1),
        column + 1);
  } else if (container != nullptr) {
    indent = source.Node(container->data.mapping.pairs.start->key)
                 ->start_mark.column;
    anchor = ExtendOverComments(
        source, LastLine(source, container),
        depth > 0 ? chain.back().key->start_mark.column + 1 : 0);
  }

  const std::vector<std::size_t> commented =
      FindCommentedChain(source, path, depth, indent);
  if (!commented.empty()) {
    // The first key takes the column its section's keys use, which need not
    // be the column it was commented out at. The rest keep their offset.
    std::size_t first_indent = 0;
    for (const std::size_t line : commented) {
      const LineInfo& info = source.lines()[line];
      const std::size_t begin = source.ColumnByte(line, 0);
      CommentedKey parsed;
      ParseCommentedKey(source.text().substr(begin, info.end - begin),
                        &parsed);
      if (line == commented.front()) {
        first_indent = parsed.indent;
      }
      std::string text =
          KeyText(indent + (parsed.indent - first_indent), parsed.name);
      // A commented key on the way down becomes a section, so only the last
      // one keeps a value.
      if (line == commented.back()) {
        text += " " + scalar;
      }
      text += std::string(TrailingComment(parsed.rest));
      edits->push_back({begin, info.end, std::move(text)});
    }
  } else {
    std::vector<std::string> added;
    for (std::size_t index = depth; index < path.size(); ++index) {
      added.push_back(KeyText(indent, path[index]));
      indent += kIndentStep;
    }
    added.back() += " " + scalar;
    if (container == nullptr) {
      AppendLines(source, added, edits);
    } else {
      InsertLinesAfter(source, anchor, added, edits);
    }
  }
  return depth > 0 && !is_mapping;
}

void Unset(Source& source, const std::vector<Pair>& chain,
           std::vector<Edit>* edits) {
  const Pair& pair = chain.back();
  const std::size_t key_begin = source.Byte(pair.key->start_mark);
  const std::size_t value_end = source.ValueEnd(pair);
  if (IsFlowMapping(pair.mapping)) {
    yaml_node_pair_t* raw = pair.mapping->data.mapping.pairs.start;
    while (source.Node(raw->key) != pair.key) {
      ++raw;
    }
    if (raw + 1 < pair.mapping->data.mapping.pairs.top) {
      edits->push_back(
          {key_begin, source.Byte(source.Node((raw + 1)->key)->start_mark),
           ""});
    } else if (raw > pair.mapping->data.mapping.pairs.start) {
      edits->push_back(
          {source.ValueEnd(source.MakePair(pair.mapping, raw - 1)), value_end,
           ""});
    } else {
      // The only pair: clear everything between the braces, which takes a
      // trailing comma with it.
      edits->push_back(
          {source.text().find('{', source.Byte(pair.mapping->start_mark)) + 1,
           source.Byte(pair.mapping->end_mark) - 1, ""});
    }
    return;
  }
  CommentOut(source, source.LineAt(key_begin), source.LineAt(value_end - 1),
             pair.key->start_mark.column, edits);
  // A section left without a key is commented out too, so the file never
  // holds a section with nothing in it.
  for (std::size_t depth = chain.size(); depth >= 2; --depth) {
    const Pair& parent = chain[depth - 2];
    if (PairCount(chain[depth - 1].mapping) != 1 ||
        IsFlowMapping(parent.mapping)) {
      break;
    }
    const std::size_t line =
        source.LineAt(source.Byte(parent.key->start_mark));
    CommentOut(source, line, line, parent.key->start_mark.column, edits);
  }
}

// The file's settings: its top-level node, or null when it has none. A file
// that holds only a "---" marker has an empty value there.
yaml_node_t* Settings(Source& source) {
  yaml_node_t* root = source.root();
  return root != nullptr && IsNull(root) ? nullptr : root;
}

// Sets `key` to `scalar`, whose parsed value is `raw`, or unsets it when
// `scalar` is null. The result is parsed again and refused unless it differs
// from `text` in that key alone.
RuntimeConfigEdit Change(std::string_view text, std::string_view key,
                         const std::string* scalar, std::string_view raw) {
  std::vector<std::string_view> path;
  if (!SplitKey(key, &path)) {
    return Fail(key, "the key is not a dotted list of names");
  }
  Source source(text);
  std::string error;
  if (!source.Parse(&error)) {
    return Fail(key, error);
  }
  yaml_node_t* root = Settings(source);
  if (root != nullptr && root->type != YAML_MAPPING_NODE) {
    return Fail(key, "the top level of the file is not a list of settings");
  }

  std::vector<Pair> chain;
  for (yaml_node_t* node = root;
       node != nullptr && chain.size() < path.size();) {
    const Pair pair = source.FindPair(node, path[chain.size()]);
    if (pair.key == nullptr) {
      break;
    }
    chain.push_back(pair);
    if (pair.alias && chain.size() < path.size()) {
      // The keys below an alias are written in another section's lines, so
      // changing one would change that section too.
      return Fail(key, std::string(path[chain.size() - 1]) +
                           " is a YAML alias, and a key below an alias "
                           "cannot be changed on its own");
    }
    node = pair.value;
  }
  const std::size_t depth = chain.size();
  const bool found = depth == path.size();

  std::map<std::string, std::string> expected;
  if (root != nullptr) {
    source.Flatten(root, "", &expected);
  }
  std::vector<Edit> edits;
  bool container_replaced = false;
  if (scalar == nullptr) {
    if (found) {
      Unset(source, chain, &edits);
    }
  } else if (found) {
    SetExisting(source, chain.back(), *scalar, &edits);
  } else {
    container_replaced = SetMissing(source, root, chain, path, *scalar, &edits);
  }

  RuntimeConfigEdit edit;
  edit.text = Apply(text, std::move(edits));

  Source result(edit.text);
  if (!result.Parse(&error)) {
    return Fail(key, "the edit would leave the file unreadable");
  }
  std::map<std::string, std::string> actual;
  if (yaml_node_t* result_root = Settings(result)) {
    result.Flatten(result_root, "", &actual);
  }
  EraseSubtree(JoinPath(path, path.size()), &expected);
  if (container_replaced) {
    EraseSubtree(JoinPath(path, depth), &expected);
  }
  for (std::size_t count = 1; count < path.size(); ++count) {
    // A section on the way is a value before it has keys, and again once its
    // last key is gone.
    expected.erase(JoinPath(path, count));
    if (scalar == nullptr) {
      actual.erase(JoinPath(path, count));
    }
  }
  if (scalar != nullptr) {
    expected[JoinPath(path, path.size())] = std::string(raw);
  }
  if (actual != expected) {
    return Fail(key, "the edit would change other settings");
  }
  return edit;
}

}  // namespace

RuntimeConfigEdit SetRuntimeConfigBool(std::string_view text,
                                       std::string_view key, bool value) {
  const std::string scalar = value ? "true" : "false";
  return Change(text, key, &scalar, scalar);
}

RuntimeConfigEdit SetRuntimeConfigInteger(std::string_view text,
                                          std::string_view key,
                                          long long value) {
  const std::string scalar = std::to_string(value);
  return Change(text, key, &scalar, scalar);
}

RuntimeConfigEdit SetRuntimeConfigString(std::string_view text,
                                         std::string_view key,
                                         std::string_view value) {
  const std::string scalar = QuoteString(value);
  return Change(text, key, &scalar, value);
}

RuntimeConfigEdit UnsetRuntimeConfigValue(std::string_view text,
                                          std::string_view key) {
  return Change(text, key, nullptr, {});
}

RuntimeConfigValue GetRuntimeConfigValue(std::string_view text,
                                         std::string_view key) {
  RuntimeConfigValue result;
  std::vector<std::string_view> path;
  if (!SplitKey(key, &path)) {
    result.error = "cannot read " + std::string(key) +
                   ": the key is not a dotted list of names";
    return result;
  }
  Source source(text);
  if (!source.Parse(&result.error)) {
    result.error = "cannot read " + std::string(key) + ": " + result.error;
    return result;
  }
  yaml_node_t* node = Settings(source);
  for (const std::string_view name : path) {
    node = source.FindPair(node, name).value;
    if (node == nullptr) {
      return result;
    }
  }
  if (node->type == YAML_SCALAR_NODE && !IsNull(node)) {
    result.found = true;
    result.value = std::string(ScalarValue(node));
  }
  return result;
}

}  // namespace runtime
}  // namespace mocktail
