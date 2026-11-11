#pragma once
// A small JSON value with a reader and a writer, enough for the multiplayer messages: null, booleans, integers, strings, arrays and
// objects (members kept in insertion order). Numbers with a fraction or exponent read as the nearest integer. Strings are UTF-8;
// \u escapes (surrogate pairs included) are decoded, and the writer escapes quotes, backslashes and control characters and replaces
// invalid UTF-8 with U+FFFD.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oc::net {

class Json {
 public:
  enum class Type { Null, Bool, Int, String, Array, Object };
  using Member = std::pair<std::string, Json>;

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool value) : type_(Type::Bool), int_(value ? 1 : 0) {}
  Json(int value) : type_(Type::Int), int_(value) {}
  Json(std::int64_t value) : type_(Type::Int), int_(value) {}
  Json(std::uint32_t value) : type_(Type::Int), int_(value) {}
  Json(const char* value) : type_(Type::String), str_(value) {}
  Json(std::string value) : type_(Type::String), str_(std::move(value)) {}
  Json(std::string_view value) : type_(Type::String), str_(value) {}

  static Json array() {
    Json value;
    value.type_ = Type::Array;
    return value;
  }
  static Json object() {
    Json value;
    value.type_ = Type::Object;
    return value;
  }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_object() const { return type_ == Type::Object; }

  // Typed reads with a fallback for a value of another type.
  std::int64_t as_int(std::int64_t fallback = 0) const { return type_ == Type::Int || type_ == Type::Bool ? int_ : fallback; }
  bool as_bool(bool fallback = false) const { return type_ == Type::Bool || type_ == Type::Int ? int_ != 0 : fallback; }
  const std::string& as_string() const;

  // Arrays.
  const std::vector<Json>& items() const { return items_; }
  Json& push(Json value);

  // Objects: set() replaces a member of the same name; get() is null for a missing member.
  const std::vector<Member>& members() const { return members_; }
  Json& set(std::string_view key, Json value);
  const Json& get(std::string_view key) const;
  const Json& operator[](std::string_view key) const { return get(key); }
  // Shorthands for a member's typed value.
  std::int64_t int_of(std::string_view key, std::int64_t fallback = 0) const { return get(key).as_int(fallback); }
  const std::string& string_of(std::string_view key) const { return get(key).as_string(); }

 private:
  Type type_ = Type::Null;
  std::int64_t int_ = 0;
  std::string str_;
  std::vector<Json> items_;
  std::vector<Member> members_;
};

// The whole of `text` as one value (white space around it allowed); nullopt when it is not valid JSON or nests deeper than 64 levels.
std::optional<Json> parse_json(std::string_view text);
// Compact JSON text of `value`.
std::string to_json(const Json& value);

// `text` cut to at most `max_points` code points.
std::string utf8_truncate(std::string_view text, size_t max_points);
// Text from a server as the chat, labels and popups show it: control characters, bidirectional overrides and isolates, zero-width
// characters, line / paragraph separators and noncharacters removed, invalid UTF-8 replaced with U+FFFD, white space at the ends
// trimmed, cut to `max_points` code points.
std::string display_text(std::string_view text, size_t max_points);

}  // namespace oc::net
