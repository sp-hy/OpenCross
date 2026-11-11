#include "net/json.h"

#include <cmath>
#include <cstdlib>

namespace oc::net {

namespace {

const std::string kEmptyString;
const Json kNull;
constexpr int kMaxDepth = 64;
// Members one object may hold: no message comes close, and Json::set's duplicate check makes a huge object slow to build.
constexpr size_t kMaxMembers = 256;
constexpr char32_t kReplacement = 0xFFFD;

void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// One code point from `text` at `pos` (advanced past it); malformed or overlong sequences and surrogates read as U+FFFD, one byte long.
char32_t next_code_point(std::string_view text, size_t& pos) {
  const auto byte = [&](size_t at) { return static_cast<unsigned char>(text[at]); };
  const unsigned char lead = byte(pos);
  if (lead < 0x80) {
    ++pos;
    return lead;
  }
  int extra = 0;
  char32_t cp = 0, min = 0;
  if ((lead & 0xE0) == 0xC0) extra = 1, cp = lead & 0x1F, min = 0x80;
  else if ((lead & 0xF0) == 0xE0) extra = 2, cp = lead & 0x0F, min = 0x800;
  else if ((lead & 0xF8) == 0xF0) extra = 3, cp = lead & 0x07, min = 0x10000;
  else {
    ++pos;
    return kReplacement;
  }
  if (pos + static_cast<size_t>(extra) >= text.size()) {
    ++pos;
    return kReplacement;
  }
  for (int i = 1; i <= extra; ++i) {
    const unsigned char next = byte(pos + static_cast<size_t>(i));
    if ((next & 0xC0) != 0x80) {
      ++pos;
      return kReplacement;
    }
    cp = (cp << 6) | (next & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) {
    ++pos;
    return kReplacement;
  }
  pos += static_cast<size_t>(extra) + 1;
  return cp;
}

void write_string(std::string& out, std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  out += '"';
  size_t pos = 0;
  while (pos < text.size()) {
    const char32_t cp = next_code_point(text, pos);
    switch (cp) {
      case '\b': out += "\\b"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\f': out += "\\f"; break;
      case '\r': out += "\\r"; break;
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      default:
        if (cp < 0x20) {
          out += "\\u00";
          out += kHex[cp >> 4];
          out += kHex[cp & 0xF];
        } else {
          append_utf8(out, cp);
        }
    }
  }
  out += '"';
}

void write_value(std::string& out, const Json& value) {
  switch (value.type()) {
    case Json::Type::Null: out += "null"; break;
    case Json::Type::Bool: out += value.as_bool() ? "true" : "false"; break;
    case Json::Type::Int: out += std::to_string(value.as_int()); break;
    case Json::Type::String: write_string(out, value.as_string()); break;
    case Json::Type::Array: {
      out += '[';
      bool first = true;
      for (const Json& item : value.items()) {
        if (!first) out += ',';
        first = false;
        write_value(out, item);
      }
      out += ']';
      break;
    }
    case Json::Type::Object: {
      out += '{';
      bool first = true;
      for (const auto& [key, member] : value.members()) {
        if (!first) out += ',';
        first = false;
        write_string(out, key);
        out += ':';
        write_value(out, member);
      }
      out += '}';
      break;
    }
  }
}

// Recursive-descent reader over the whole text.
class Reader {
 public:
  explicit Reader(std::string_view text) : text_(text) {}

  std::optional<Json> document() {
    Json value;
    if (!parse_value(value, 0)) return std::nullopt;
    skip_space();
    if (pos_ != text_.size()) return std::nullopt;
    return value;
  }

 private:
  void skip_space() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) ++pos_;
  }

  bool literal(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) return false;
    pos_ += word.size();
    return true;
  }

  bool parse_value(Json& out, int depth) {
    if (depth >= kMaxDepth) return false;
    skip_space();
    if (pos_ >= text_.size()) return false;
    switch (text_[pos_]) {
      case '"': {
        std::string text;
        if (!parse_string(text)) return false;
        out = Json(std::move(text));
        return true;
      }
      case '[': return parse_array(out, depth);
      case 'f':
        out = Json(false);
        return literal("false");
      case 'n':
        out = Json();
        return literal("null");
      case 't':
        out = Json(true);
        return literal("true");
      case '{': return parse_object(out, depth);
      default: return parse_number(out);
    }
  }

  bool parse_number(Json& out) {
    const size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    const size_t digits = pos_;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    if (pos_ == digits) return false;
    bool integral = true;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      integral = false;
      const size_t frac = ++pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
      if (pos_ == frac) return false;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      integral = false;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      const size_t exp = pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
      if (pos_ == exp) return false;
    }
    const std::string number(text_.substr(start, pos_ - start));
    if (integral) {
      out = Json(static_cast<std::int64_t>(std::strtoll(number.c_str(), nullptr, 10)));
    } else {
      const double value = std::strtod(number.c_str(), nullptr);
      out = Json(std::isfinite(value) && std::fabs(value) < 9.2e18 ? static_cast<std::int64_t>(std::llround(value)) : std::int64_t{0});
    }
    return true;
  }

  bool hex4(char32_t& out) {
    if (pos_ + 4 > text_.size()) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char letter = text_[pos_++];
      out <<= 4;
      if (letter >= '0' && letter <= '9') out |= static_cast<char32_t>(letter - '0');
      else if (letter >= 'a' && letter <= 'f') out |= static_cast<char32_t>(letter - 'a' + 10);
      else if (letter >= 'A' && letter <= 'F') out |= static_cast<char32_t>(letter - 'A' + 10);
      else return false;
    }
    return true;
  }

  // A \u escape after its backslash and 'u'; a high surrogate takes the low one that must follow, a lone surrogate reads as U+FFFD.
  bool unicode_escape(std::string& out) {
    char32_t cp = 0;
    if (!hex4(cp)) return false;
    if (cp >= 0xD800 && cp < 0xDC00) {
      char32_t low = 0;
      if (text_.substr(pos_, 2) == "\\u") {
        const size_t back = pos_;
        pos_ += 2;
        if (!hex4(low)) return false;
        if (low >= 0xDC00 && low < 0xE000) {
          append_utf8(out, 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00));
          return true;
        }
        pos_ = back;
      }
      append_utf8(out, kReplacement);
      return true;
    }
    append_utf8(out, cp >= 0xDC00 && cp < 0xE000 ? kReplacement : cp);
    return true;
  }

  bool parse_string(std::string& out) {
    ++pos_;
    while (pos_ < text_.size()) {
      const char letter = text_[pos_];
      if (letter == '"') {
        ++pos_;
        return true;
      }
      if (static_cast<unsigned char>(letter) < 0x20) return false;
      if (letter != '\\') {
        out += letter;
        ++pos_;
        continue;
      }
      if (++pos_ >= text_.size()) return false;
      const char escape = text_[pos_++];
      switch (escape) {
        case '"': out += '"'; break;
        case '/': out += '/'; break;
        case '\\': out += '\\'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u':
          if (!unicode_escape(out)) return false;
          break;
        default: return false;
      }
    }
    return false;
  }

  bool parse_array(Json& out, int depth) {
    ++pos_;
    out = Json::array();
    skip_space();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      Json item;
      if (!parse_value(item, depth + 1)) return false;
      out.push(std::move(item));
      skip_space();
      if (pos_ >= text_.size()) return false;
      if (text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      if (text_[pos_++] != ',') return false;
    }
  }

  bool parse_object(Json& out, int depth) {
    ++pos_;
    out = Json::object();
    skip_space();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      skip_space();
      std::string key;
      if (pos_ >= text_.size() || text_[pos_] != '"' || !parse_string(key)) return false;
      skip_space();
      if (pos_ >= text_.size() || text_[pos_++] != ':') return false;
      Json member;
      if (!parse_value(member, depth + 1)) return false;
      out.set(key, std::move(member));
      if (out.members().size() > kMaxMembers) return false;
      skip_space();
      if (pos_ >= text_.size()) return false;
      if (text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      if (text_[pos_++] != ',') return false;
    }
  }

  std::string_view text_;
  size_t pos_ = 0;
};

}  // namespace

const std::string& Json::as_string() const { return type_ == Type::String ? str_ : kEmptyString; }

Json& Json::push(Json value) {
  if (type_ != Type::Array) *this = array();
  items_.push_back(std::move(value));
  return *this;
}

Json& Json::set(std::string_view key, Json value) {
  if (type_ != Type::Object) *this = object();
  for (Member& member : members_)
    if (member.first == key) {
      member.second = std::move(value);
      return *this;
    }
  members_.emplace_back(std::string(key), std::move(value));
  return *this;
}

const Json& Json::get(std::string_view key) const {
  for (const Member& member : members_)
    if (member.first == key) return member.second;
  return kNull;
}

std::optional<Json> parse_json(std::string_view text) { return Reader(text).document(); }

std::string to_json(const Json& value) {
  std::string out;
  write_value(out, value);
  return out;
}

std::string utf8_truncate(std::string_view text, size_t max_points) {
  size_t count = 0, pos = 0;
  while (pos < text.size() && count < max_points) {
    next_code_point(text, pos);
    ++count;
  }
  return std::string(text.substr(0, pos));
}

namespace {

bool hidden_code_point(char32_t cp) {
  return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E) ||
         (cp >= 0x2060 && cp <= 0x206F) || cp == 0xFEFF || (cp >= 0xFFF9 && cp <= 0xFFFB) || (cp >= 0xFDD0 && cp <= 0xFDEF) ||
         (cp & 0xFFFE) == 0xFFFE;
}

}  // namespace

std::string display_text(std::string_view text, size_t max_points) {
  std::string out;
  size_t count = 0, pos = 0;
  while (pos < text.size() && count < max_points) {
    const char32_t cp = next_code_point(text, pos);
    if (hidden_code_point(cp) || (count == 0 && cp == ' ')) continue;
    append_utf8(out, cp);
    ++count;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

}  // namespace oc::net
