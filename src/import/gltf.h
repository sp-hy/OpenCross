#pragma once
// glTF 2.0 binary (.glb) container: a JSON value type with numbers as doubles (reader and writer), the GLB chunk layout, and typed reads of
// accessors. Knows nothing about the game's models (game/model_export.h builds the documents).
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/types.h"

namespace oc::import::gltf {

// Accessor component types.
constexpr int kSignedByte = 5120, kUnsignedByte = 5121, kSignedShort = 5122, kUnsignedShort = 5123, kUnsignedInt = 5125, kFloat = 5126;

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };
  using Member = std::pair<std::string, Value>;

  Value() = default;
  Value(bool flag) : type_(Type::Bool), number_(flag ? 1 : 0) {}
  Value(int number) : type_(Type::Number), number_(number) {}
  Value(unsigned number) : type_(Type::Number), number_(number) {}
  Value(long number) : type_(Type::Number), number_(static_cast<double>(number)) {}
  Value(unsigned long number) : type_(Type::Number), number_(static_cast<double>(number)) {}
  Value(long long number) : type_(Type::Number), number_(static_cast<double>(number)) {}
  Value(unsigned long long number) : type_(Type::Number), number_(static_cast<double>(number)) {}
  Value(double number) : type_(Type::Number), number_(number) {}
  // Written with the shortest text that reads back as the same float.
  Value(float number) : type_(Type::Number), number_(number), single_(true) {}
  Value(const char* text) : type_(Type::String), text_(text) {}
  Value(std::string text) : type_(Type::String), text_(std::move(text)) {}
  Value(std::string_view text) : type_(Type::String), text_(text) {}

  static Value array();
  static Value object();

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_object() const { return type_ == Type::Object; }
  double number(double fallback = 0) const { return type_ == Type::Number || type_ == Type::Bool ? number_ : fallback; }
  // The number as an index; `fallback` for anything that is not a non-negative integer.
  size_t index(size_t fallback = static_cast<size_t>(-1)) const;
  bool flag() const { return type_ == Type::Bool && number_ != 0; }
  bool single() const { return single_; }
  const std::string& text() const;

  const std::vector<Value>& items() const { return items_; }
  size_t size() const { return type_ == Type::Array ? items_.size() : members_.size(); }
  const Value& operator[](size_t i) const;
  Value& push(Value value);

  const std::vector<Member>& members() const { return members_; }
  // Replaces a member of the same name.
  Value& set(std::string_view key, Value value);
  // Null for a missing member.
  const Value& get(std::string_view key) const;
  const Value& operator[](std::string_view key) const { return get(key); }
  bool has(std::string_view key) const { return !get(key).is_null(); }

 private:
  Type type_ = Type::Null;
  double number_ = 0;
  bool single_ = false;
  std::string text_;
  std::vector<Value> items_;
  std::vector<Member> members_;
};

// Compact JSON text. Non-finite numbers are written as 0.
std::string write_json(const Value& value);
// nullopt when `text` is not one valid JSON value or nests deeper than 64 levels.
std::optional<Value> parse_json(std::string_view text);

// The GLB file of a JSON chunk and a binary chunk (both padded to 4 bytes: spaces / zeros); an empty `bin` writes no BIN chunk.
Bytes write_glb(const Value& json, ByteSpan bin);

struct Glb {
  Value json;
  Bytes bin;
};
// nullopt (and a message) when the header, the chunk layout or the JSON is malformed.
std::optional<Glb> read_glb(ByteSpan data, std::string* err = nullptr);

// Component count of an accessor type ("SCALAR" 1 ... "MAT4" 16), 0 when unknown.
u32 type_components(std::string_view type);
// Accessor `accessor` of the GLB's buffer 0, element by element (count x components values), byteStride honoured; normalized integer
// components become 0..1 (signed: -1..1). nullopt when the accessor or its view is out of range. `components` receives the component count.
std::optional<std::vector<double>> read_accessor(const Glb& glb, size_t accessor, u32* components = nullptr);
// The bytes of buffer view `view` (images), empty when out of range.
ByteSpan buffer_view(const Glb& glb, size_t view);

using Vec3d = std::array<double, 3>;
// (x, y, z, w).
using Quat = std::array<double, 4>;

// Row-major 3 x 4 affine transform.
struct Affine {
  std::array<double, 12> cells{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  double& at(int row, int col) { return cells[static_cast<size_t>(row * 4 + col)]; }
  double at(int row, int col) const { return cells[static_cast<size_t>(row * 4 + col)]; }
};
Affine operator*(const Affine& lhs, const Affine& rhs);
Vec3d apply_point(const Affine& xf, const Vec3d& point);
Vec3d apply_vector(const Affine& xf, const Vec3d& v);
// A singular transform gives a zero 3 x 3 part.
Affine inverse(const Affine& xf);
// A node transform T * R * S (the quaternion is normalised; scale per axis).
Affine affine_trs(const Vec3d& trans, const Quat& quat, const Vec3d& scale);
// A node's local transform: its "matrix" (column-major) when it has one, else its translation, rotation and scale.
Affine node_transform(const Value& node);

}  // namespace oc::import::gltf
