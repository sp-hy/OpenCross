#include "import/gltf.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>

namespace oc::import::gltf {

namespace {

const Value& null_value() {
  static const Value kNull;
  return kNull;
}

constexpr u32 kGlbMagic = 0x46546C67;
constexpr u32 kChunkJson = 0x4E4F534A;
constexpr u32 kChunkBin = 0x004E4942;

void put_u32(Bytes& out, u32 value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(value >> (8 * i)));
}

u32 get_u32(ByteSpan data, size_t at) {
  u32 value = 0;
  std::memcpy(&value, data.data() + at, 4);
  return value;
}

void write_string(std::string& out, const std::string& text) {
  out += '"';
  for (const char ch : text) {
    const auto code = static_cast<unsigned char>(ch);
    if (ch == '"' || ch == '\\') {
      out += '\\';
      out += ch;
    } else if (code < 0x20) {
      out += std::format("\\u{:04x}", code);
    } else {
      out += ch;
    }
  }
  out += '"';
}

void write_value(std::string& out, const Value& value) {
  switch (value.type()) {
    case Value::Type::Null:
      out += "null";
      break;
    case Value::Type::Bool:
      out += value.flag() ? "true" : "false";
      break;
    case Value::Type::Number: {
      const double number = value.number();
      if (!std::isfinite(number)) out += '0';
      else if (number == std::floor(number) && std::fabs(number) < 9007199254740992.0) out += std::format("{}", static_cast<long long>(number));
      else if (value.single()) out += std::format("{}", static_cast<float>(number));
      else out += std::format("{}", number);
      break;
    }
    case Value::Type::String:
      write_string(out, value.text());
      break;
    case Value::Type::Array: {
      out += '[';
      bool first = true;
      for (const Value& item : value.items()) {
        if (!first) out += ',';
        first = false;
        write_value(out, item);
      }
      out += ']';
      break;
    }
    case Value::Type::Object: {
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

void append_utf8(std::string& out, u32 code) {
  if (code < 0x80) {
    out += static_cast<char>(code);
  } else if (code < 0x800) {
    out += static_cast<char>(0xC0 | (code >> 6));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else if (code < 0x10000) {
    out += static_cast<char>(0xE0 | (code >> 12));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (code >> 18));
    out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code & 0x3F));
  }
}

// Recursive-descent reader over the whole text.
class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  std::optional<Value> document() {
    Value value;
    if (!parse(value, 0)) return std::nullopt;
    skip_space();
    if (pos_ != text_.size()) return std::nullopt;
    return value;
  }

 private:
  static constexpr int kMaxDepth = 64;

  void skip_space() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) ++pos_;
  }

  bool literal(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) return false;
    pos_ += word.size();
    return true;
  }

  bool hex4(u32& code) {
    if (pos_ + 4 > text_.size()) return false;
    code = 0;
    for (int i = 0; i < 4; ++i) {
      const char ch = text_[pos_++];
      code <<= 4;
      if (ch >= '0' && ch <= '9') code |= static_cast<u32>(ch - '0');
      else if (ch >= 'a' && ch <= 'f') code |= static_cast<u32>(ch - 'a' + 10);
      else if (ch >= 'A' && ch <= 'F') code |= static_cast<u32>(ch - 'A' + 10);
      else return false;
    }
    return true;
  }

  bool string(std::string& out) {
    if (pos_ >= text_.size() || text_[pos_] != '"') return false;
    ++pos_;
    while (pos_ < text_.size()) {
      const char ch = text_[pos_++];
      if (ch == '"') return true;
      if (static_cast<unsigned char>(ch) < 0x20) return false;
      if (ch != '\\') {
        out += ch;
        continue;
      }
      if (pos_ >= text_.size()) return false;
      const char escape = text_[pos_++];
      switch (escape) {
        case '"':
        case '/':
        case '\\':
          out += escape;
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'u': {
          u32 code = 0;
          if (!hex4(code)) return false;
          if (code >= 0xD800 && code < 0xDC00) {
            u32 low = 0;
            if (!literal("\\u") || !hex4(low) || low < 0xDC00 || low >= 0xE000) return false;
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          }
          append_utf8(out, code);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  bool number(Value& out) {
    const size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    while (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '.' || text_[pos_] == 'e' ||
                                   text_[pos_] == 'E' || text_[pos_] == '+' || text_[pos_] == '-'))
      ++pos_;
    double number = 0;
    const auto [end, ec] = std::from_chars(text_.data() + start, text_.data() + pos_, number);
    if (ec != std::errc{} || end != text_.data() + pos_ || pos_ == start) return false;
    out = Value(number);
    return true;
  }

  bool parse(Value& out, int depth) {
    if (depth > kMaxDepth) return false;
    skip_space();
    if (pos_ >= text_.size()) return false;
    const char ch = text_[pos_];
    if (ch == '{') {
      ++pos_;
      out = Value::object();
      skip_space();
      if (pos_ < text_.size() && text_[pos_] == '}') return ++pos_, true;
      while (true) {
        skip_space();
        std::string key;
        if (!string(key)) return false;
        skip_space();
        if (pos_ >= text_.size() || text_[pos_++] != ':') return false;
        Value member;
        if (!parse(member, depth + 1)) return false;
        out.set(key, std::move(member));
        skip_space();
        if (pos_ >= text_.size()) return false;
        if (text_[pos_] == ',') {
          ++pos_;
          continue;
        }
        return text_[pos_++] == '}';
      }
    }
    if (ch == '[') {
      ++pos_;
      out = Value::array();
      skip_space();
      if (pos_ < text_.size() && text_[pos_] == ']') return ++pos_, true;
      while (true) {
        Value item;
        if (!parse(item, depth + 1)) return false;
        out.push(std::move(item));
        skip_space();
        if (pos_ >= text_.size()) return false;
        if (text_[pos_] == ',') {
          ++pos_;
          continue;
        }
        return text_[pos_++] == ']';
      }
    }
    if (ch == '"') {
      std::string text;
      if (!string(text)) return false;
      out = Value(std::move(text));
      return true;
    }
    if (literal("true")) return out = Value(true), true;
    if (literal("false")) return out = Value(false), true;
    if (literal("null")) return out = Value(), true;
    return number(out);
  }

  std::string_view text_;
  size_t pos_ = 0;
};

u32 component_size(int component_type) {
  switch (component_type) {
    case kSignedByte:
    case kUnsignedByte:
      return 1;
    case kSignedShort:
    case kUnsignedShort:
      return 2;
    case kUnsignedInt:
    case kFloat:
      return 4;
    default:
      return 0;
  }
}

double read_component(const u8* at, int component_type, bool normalized) {
  switch (component_type) {
    case kSignedByte: {
      const auto value = static_cast<s8>(*at);
      return normalized ? std::max(value / 127.0, -1.0) : value;
    }
    case kUnsignedByte:
      return normalized ? *at / 255.0 : *at;
    case kSignedShort: {
      s16 value = 0;
      std::memcpy(&value, at, 2);
      return normalized ? std::max(value / 32767.0, -1.0) : value;
    }
    case kUnsignedShort: {
      u16 value = 0;
      std::memcpy(&value, at, 2);
      return normalized ? value / 65535.0 : value;
    }
    case kUnsignedInt: {
      u32 value = 0;
      std::memcpy(&value, at, 4);
      return value;
    }
    default: {
      float value = 0;
      std::memcpy(&value, at, 4);
      return value;
    }
  }
}

}  // namespace

Value Value::array() {
  Value value;
  value.type_ = Type::Array;
  return value;
}

Value Value::object() {
  Value value;
  value.type_ = Type::Object;
  return value;
}

size_t Value::index(size_t fallback) const {
  if (type_ != Type::Number || number_ < 0 || number_ != std::floor(number_) || number_ > 4294967295.0) return fallback;
  return static_cast<size_t>(number_);
}

const std::string& Value::text() const {
  static const std::string kEmpty;
  return type_ == Type::String ? text_ : kEmpty;
}

const Value& Value::operator[](size_t i) const { return type_ == Type::Array && i < items_.size() ? items_[i] : null_value(); }

Value& Value::push(Value value) {
  if (type_ != Type::Array) *this = array();
  items_.push_back(std::move(value));
  return items_.back();
}

Value& Value::set(std::string_view key, Value value) {
  if (type_ != Type::Object) *this = object();
  for (Member& member : members_)
    if (member.first == key) return member.second = std::move(value);
  members_.emplace_back(std::string(key), std::move(value));
  return members_.back().second;
}

const Value& Value::get(std::string_view key) const {
  if (type_ == Type::Object)
    for (const Member& member : members_)
      if (member.first == key) return member.second;
  return null_value();
}

std::string write_json(const Value& value) {
  std::string out;
  write_value(out, value);
  return out;
}

std::optional<Value> parse_json(std::string_view text) { return Parser(text).document(); }

Bytes write_glb(const Value& json, ByteSpan bin) {
  std::string text = write_json(json);
  while (text.size() % 4) text += ' ';
  const size_t bin_size = (bin.size() + 3) / 4 * 4;
  const size_t total = 12 + 8 + text.size() + (bin.empty() ? 0 : 8 + bin_size);
  Bytes out;
  out.reserve(total);
  put_u32(out, kGlbMagic);
  put_u32(out, 2);
  put_u32(out, static_cast<u32>(total));
  put_u32(out, static_cast<u32>(text.size()));
  put_u32(out, kChunkJson);
  out.insert(out.end(), text.begin(), text.end());
  if (!bin.empty()) {
    put_u32(out, static_cast<u32>(bin_size));
    put_u32(out, kChunkBin);
    out.insert(out.end(), bin.begin(), bin.end());
    out.resize(total, 0);
  }
  return out;
}

std::optional<Glb> read_glb(ByteSpan data, std::string* err) {
  auto fail = [&](const char* message) -> std::optional<Glb> {
    if (err) *err = message;
    return std::nullopt;
  };
  if (data.size() < 20 || get_u32(data, 0) != kGlbMagic) return fail("not a GLB file");
  if (get_u32(data, 4) != 2) return fail("GLB version is not 2");
  const size_t total = get_u32(data, 8);
  if (total > data.size() || total < 20) return fail("GLB length exceeds the file");
  Glb glb;
  bool have_json = false;
  for (size_t at = 12; at + 8 <= total;) {
    const size_t length = get_u32(data, at), type = get_u32(data, at + 4);
    if (at + 8 + length > total) return fail("GLB chunk runs past the end");
    const ByteSpan chunk = data.subspan(at + 8, length);
    if (type == kChunkJson && !have_json) {
      auto json = parse_json(std::string_view(reinterpret_cast<const char*>(chunk.data()), chunk.size()));
      if (!json || !json->is_object()) return fail("GLB JSON chunk is not a JSON object");
      glb.json = std::move(*json);
      have_json = true;
    } else if (type == kChunkBin && glb.bin.empty()) {
      glb.bin.assign(chunk.begin(), chunk.end());
    }
    at += 8 + (length + 3) / 4 * 4;
  }
  if (!have_json) return fail("GLB has no JSON chunk");
  return glb;
}

u32 type_components(std::string_view type) {
  if (type == "SCALAR") return 1;
  if (type == "VEC2") return 2;
  if (type == "VEC3") return 3;
  if (type == "VEC4" || type == "MAT2") return 4;
  if (type == "MAT3") return 9;
  if (type == "MAT4") return 16;
  return 0;
}

ByteSpan buffer_view(const Glb& glb, size_t view) {
  const Value& entry = glb.json["bufferViews"][view];
  if (!entry.is_object() || entry["buffer"].index(0) != 0) return {};
  const size_t offset = entry["byteOffset"].index(0), length = entry["byteLength"].index(0);
  if (offset > glb.bin.size() || length > glb.bin.size() - offset) return {};
  return ByteSpan(glb.bin).subspan(offset, length);
}

std::optional<std::vector<double>> read_accessor(const Glb& glb, size_t accessor, u32* components) {
  const Value& entry = glb.json["accessors"][accessor];
  if (!entry.is_object()) return std::nullopt;
  const u32 count_per = type_components(entry["type"].text());
  const int component_type = static_cast<int>(entry["componentType"].number());
  const u32 size = component_size(component_type);
  const size_t count = entry["count"].index(0);
  if (!count_per || !size) return std::nullopt;
  if (components) *components = count_per;
  std::vector<double> out(count * count_per, 0.0);
  if (!entry.has("bufferView")) return out;
  const size_t view_index = entry["bufferView"].index();
  const ByteSpan view = buffer_view(glb, view_index);
  if (view.empty() && count) return std::nullopt;
  const size_t element = size_t(size) * count_per;
  const size_t stride = glb.json["bufferViews"][view_index]["byteStride"].index(element);
  const size_t offset = entry["byteOffset"].index(0);
  if (count && (offset > view.size() || (count - 1) * stride + element > view.size() - offset)) return std::nullopt;
  const bool normalized = entry["normalized"].flag();
  for (size_t i = 0; i < count; ++i)
    for (u32 k = 0; k < count_per; ++k) out[i * count_per + k] = read_component(view.data() + offset + i * stride + size_t(k) * size, component_type, normalized);
  return out;
}

Affine operator*(const Affine& lhs, const Affine& rhs) {
  Affine out;
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 4; ++col) {
      double sum = col == 3 ? lhs.at(row, 3) : 0.0;
      for (int k = 0; k < 3; ++k) sum += lhs.at(row, k) * rhs.at(k, col);
      out.at(row, col) = sum;
    }
  return out;
}

Vec3d apply_point(const Affine& xf, const Vec3d& point) {
  Vec3d out{};
  for (int row = 0; row < 3; ++row)
    out[static_cast<size_t>(row)] = xf.at(row, 0) * point[0] + xf.at(row, 1) * point[1] + xf.at(row, 2) * point[2] + xf.at(row, 3);
  return out;
}

Vec3d apply_vector(const Affine& xf, const Vec3d& v) {
  Vec3d out{};
  for (int row = 0; row < 3; ++row) out[static_cast<size_t>(row)] = xf.at(row, 0) * v[0] + xf.at(row, 1) * v[1] + xf.at(row, 2) * v[2];
  return out;
}

Affine inverse(const Affine& xf) {
  // The 3 x 3 part by its cofactors (adjugate / determinant), then the translation.
  Affine out;
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) {
      const int r0 = (col + 1) % 3, r1 = (col + 2) % 3, c0 = (row + 1) % 3, c1 = (row + 2) % 3;
      out.at(row, col) = xf.at(r0, c0) * xf.at(r1, c1) - xf.at(r0, c1) * xf.at(r1, c0);
    }
  const double det = xf.at(0, 0) * out.at(0, 0) + xf.at(0, 1) * out.at(1, 0) + xf.at(0, 2) * out.at(2, 0);
  const double scale = det != 0 ? 1.0 / det : 0.0;
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) out.at(row, col) *= scale;
  for (int row = 0; row < 3; ++row) out.at(row, 3) = -(out.at(row, 0) * xf.at(0, 3) + out.at(row, 1) * xf.at(1, 3) + out.at(row, 2) * xf.at(2, 3));
  return out;
}

Affine affine_trs(const Vec3d& trans, const Quat& quat, const Vec3d& scale) {
  const double len = std::sqrt(quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2] + quat[3] * quat[3]);
  const double x = len > 0 ? quat[0] / len : 0, y = len > 0 ? quat[1] / len : 0, z = len > 0 ? quat[2] / len : 0, w = len > 0 ? quat[3] / len : 1;
  const double rot[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                            {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                            {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
  Affine out;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) out.at(row, col) = rot[row][col] * scale[static_cast<size_t>(col)];
    out.at(row, 3) = trans[static_cast<size_t>(row)];
  }
  return out;
}

Affine node_transform(const Value& node) {
  const Value& matrix = node["matrix"];
  if (matrix.size() == 16) {
    Affine out;
    for (int col = 0; col < 4; ++col)
      for (int row = 0; row < 3; ++row) out.at(row, col) = matrix[static_cast<size_t>(col * 4 + row)].number();
    return out;
  }
  auto numbers = [&](std::string_view key, Quat fallback) {
    const Value& list = node[key];
    for (size_t i = 0; i < list.size() && i < fallback.size(); ++i) fallback[i] = list[i].number(fallback[i]);
    return fallback;
  };
  const Quat trans = numbers("translation", {0, 0, 0, 0}), rot = numbers("rotation", {0, 0, 0, 1}), scale = numbers("scale", {1, 1, 1, 0});
  return affine_trs({trans[0], trans[1], trans[2]}, rot, {scale[0], scale[1], scale[2]});
}

}  // namespace oc::import::gltf
