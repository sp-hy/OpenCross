#include "import/model.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <stdexcept>
#include <tuple>

#include "core/span_reader.h"
#include "import/container.h"
#include "import/tim.h"

namespace oc::import {
namespace {

using namespace assets;

// 12-bit angles: 4096 = 360 degrees.
constexpr double kAngleUnit = 360.0 / 4096.0;
constexpr size_t kS3Size = 0x160;
constexpr double kSkinOne = 4096.0;

// PS1 GPU primitive codes (without the colour byte) used as UV-map entry types.
constexpr u16 kFlatTri = 0x20, kTexTri = 0x24, kFlatQuad = 0x28, kTexQuad = 0x2C;

struct Fail {
  std::string msg;
};
[[noreturn]] void fail(std::string message) { throw Fail{std::move(message)}; }
template <typename... Args>
[[noreturn]] void failf(std::format_string<Args...> format, Args&&... args) {
  throw Fail{std::format(format, std::forward<Args>(args)...)};
}

// Soft findings collected while loading: errors/warnings that validate_model reports but load_* tolerates.
struct Notes {
  std::vector<std::string> errors, warnings;
  u32 faces = 0, vertices = 0;
};

int floor_div(int dividend, int divisor) {
  int quotient = dividend / divisor;
  if ((dividend % divisor != 0) && ((dividend < 0) != (divisor < 0))) --quotient;
  return quotient;
}
int floor_mod(int dividend, int divisor) { return dividend - floor_div(dividend, divisor) * divisor; }

// ---------------------------------------------------------------- textures
struct Atim {
  u32 clut_w = 0, clut_h = 0;
  // clut_h * clut_w
  std::vector<u16> clut;
  struct Img {
    // pixels (8bpp)
    u32 w = 0, h = 0;
    ByteSpan px;
  };
  std::vector<Img> images;
};

Atim parse_atim(ByteSpan data) {
  auto cpt = parse_cpt(data);
  if (!cpt || cpt->entries.size() < 2) fail("atim: not a cpt with >= 2 objects");
  Atim atim;
  {
    SpanReader reader(cpt->entries[0].data);
    if (reader.at<u32>(0) != 0x1101) failf("atim: CLUT object type {:#x}", reader.at<u32>(0));
    atim.clut_w = reader.at<u16>(12);
    atim.clut_h = reader.at<u16>(14);
    if (atim.clut_w == 0 || atim.clut_h == 0 || reader.size() != 16 + 2ull * atim.clut_w * atim.clut_h) fail("atim: CLUT size");
    atim.clut.resize(static_cast<size_t>(atim.clut_w) * atim.clut_h);
    for (size_t i = 0; i < atim.clut.size(); ++i) atim.clut[i] = reader.at<u16>(16 + 2 * i);
  }
  for (size_t i = 1; i < cpt->entries.size(); ++i) {
    SpanReader reader(cpt->entries[i].data);
    if (reader.at<u32>(0) != 0x1100) failf("atim: image object type {:#x}", reader.at<u32>(0));
    Atim::Img image;
    image.w = reader.at<u16>(12) * 2u;
    image.h = reader.at<u16>(14);
    if (image.w == 0 || image.h == 0 || reader.size() != 16ull + static_cast<size_t>(image.w) * image.h) fail("atim: image size");
    image.px = cpt->entries[i].data.subspan(16);
    atim.images.push_back(image);
  }
  return atim;
}

// .ltd = one atim; .ltc = cpt of atims (main texture, overlay strip).
std::vector<Atim> parse_atim_file(ByteSpan data) {
  auto cpt = parse_cpt(data);
  if (!cpt) fail("atim: not a cpt");
  std::vector<Atim> out;
  if (cpt->entries[0].data.size() >= 4 && SpanReader(cpt->entries[0].data).at<u32>(0) == 0x1101) {
    out.push_back(parse_atim(data));
  } else {
    for (const auto& entry : cpt->entries) out.push_back(parse_atim(entry.data));
  }
  return out;
}

Image make_rgba(u32 w, u32 h) {
  Image image;
  image.width = w;
  image.height = h;
  image.pixels.assign(static_cast<size_t>(w) * h * 4, 0);
  return image;
}

// Images of one atim stacked vertically, coloured with CLUT row `row`.
Image atim_texture(const Atim& atim, u32 row) {
  u32 w = 0, h = 0;
  for (const auto& image : atim.images) {
    w = std::max(w, image.w);
    h += image.h;
  }
  Image out = make_rgba(w, h);
  const size_t base = static_cast<size_t>(std::min(row, atim.clut_h - 1)) * atim.clut_w;
  u32 y0 = 0;
  for (const auto& image : atim.images) {
    for (u32 y = 0; y < image.h; ++y) {
      for (u32 x = 0; x < image.w; ++x) {
        const u8 idx = image.px[static_cast<size_t>(y) * image.w + x];
        const u32 colour = idx < atim.clut_w ? bgr555_to_rgba_stp(atim.clut[base + idx]) : 0u;
        u8* dst = &out.pixels[(static_cast<size_t>(y0 + y) * w + x) * 4];
        for (int k = 0; k < 4; ++k) dst[k] = static_cast<u8>(colour >> (8 * k));
      }
    }
    y0 += image.h;
  }
  return out;
}

struct TexInfo {
  std::vector<Image> textures;
  u32 main_count = 0;
  // main texture size (defaults when absent)
  u32 width = 128, height = 256;
  // images stacked in the main texture
  u32 images = 1;
  bool have = false;
};

TexInfo build_atim_textures(ByteSpan tex) {
  TexInfo info;
  if (tex.empty()) return info;
  const auto atims = parse_atim_file(tex);
  const Atim& a0 = atims[0];
  for (u32 row = 0; row < a0.clut_h; ++row) info.textures.push_back(atim_texture(a0, row));
  info.main_count = a0.clut_h;
  for (size_t i = 1; i < atims.size(); ++i) info.textures.push_back(atim_texture(atims[i], 0));
  info.width = info.textures[0].width;
  info.height = info.textures[0].height;
  info.images = static_cast<u32>(a0.images.size());
  info.have = true;
  return info;
}

Image tim_rgba(ByteSpan tim_data) {
  auto tim = parse_tim(tim_data);
  if (!tim) fail("TIM: malformed");
  // Keep STP (tim.h kStpAlpha).
  auto img = decode_tim(*tim, 0, true);
  if (!img) fail("TIM: cannot decode");
  return img->indexed ? to_rgba(*img) : *img;
}

TexInfo build_tim_texture(ByteSpan tim_data) {
  TexInfo info;
  if (tim_data.empty()) return info;
  info.textures.push_back(tim_rgba(tim_data));
  info.main_count = 1;
  info.width = info.textures[0].width;
  info.height = info.textures[0].height;
  info.have = true;
  return info;
}

// ---------------------------------------------------------------- skeleton
struct RawJoint {
  int parent = -1;
  std::array<int, 3> rot{}, trans{};
  u32 bone_ref = 0;
};

struct World {
  std::array<double, 9> matrix{};
  std::array<double, 3> offset{};
};

std::array<double, 9> rot_matrix(const std::array<int, 3>& angles) {
  const double radians_per_unit = 2 * 3.14159265358979323846 / 4096;
  const double cx = std::cos(angles[0] * radians_per_unit), sx = std::sin(angles[0] * radians_per_unit);
  const double cy = std::cos(angles[1] * radians_per_unit), sy = std::sin(angles[1] * radians_per_unit);
  const double cz = std::cos(angles[2] * radians_per_unit), sz = std::sin(angles[2] * radians_per_unit);
  return {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,  //
          sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,  //
          -sy,     cy * sx,                cy * cx};
}

std::vector<World> rest_world(const std::vector<RawJoint>& joints) {
  std::vector<World> world(joints.size());
  for (size_t i = 0; i < joints.size(); ++i) {
    const auto rotation = rot_matrix(joints[i].rot);
    const std::array<double, 3> trans{static_cast<double>(joints[i].trans[0]), static_cast<double>(joints[i].trans[1]),
                                      static_cast<double>(joints[i].trans[2])};
    if (joints[i].parent < 0) {
      world[i].matrix = rotation;
      world[i].offset = trans;
    } else {
      const World& parent = world[static_cast<size_t>(joints[i].parent)];
      for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
          double sum = 0;
          for (int k = 0; k < 3; ++k) sum += parent.matrix[row * 3 + k] * rotation[k * 3 + col];
          world[i].matrix[row * 3 + col] = sum;
        }
        world[i].offset[row] = parent.matrix[row * 3] * trans[0] + parent.matrix[row * 3 + 1] * trans[1] + parent.matrix[row * 3 + 2] * trans[2] + parent.offset[row];
      }
    }
  }
  return world;
}

std::vector<RawJoint> parse_skeleton(const SpanReader& reader, size_t offset, size_t end) {
  const u32 count = reader.at<u32>(offset);
  if (offset + 4 + 20ull * count != end) failf("S2 size mismatch (N={})", count);
  std::vector<RawJoint> out(count);
  for (u32 i = 0; i < count; ++i) {
    const size_t pos = offset + 4 + 20ull * i;
    RawJoint& joint = out[i];
    joint.parent = static_cast<s32>(reader.at<u32>(pos));
    if (!(joint.parent == -1 || (joint.parent >= 0 && static_cast<u32>(joint.parent) < i))) failf("joint {} parent {}", i, joint.parent);
    for (int k = 0; k < 3; ++k) {
      joint.rot[k] = static_cast<s16>(reader.at<u16>(pos + 4 + 2 * k));
      joint.trans[k] = static_cast<s16>(reader.at<u16>(pos + 10 + 2 * k));
    }
    joint.bone_ref = reader.at<u32>(pos + 16);
  }
  return out;
}

// ---------------------------------------------------------------- geometry
struct Face {
  int corners = 3;
  bool textured = false;
  // global vertex indices in UV-record order
  std::array<u32, 4> vertex_ids{};
  std::array<std::array<u8, 2>, 4> uv{};
  std::array<u8, 3> color{};
  // Texture-select bits of a textured face:
  // bit 0 of the first pointer (UV-record order) adds one CLUT row (+0x40 to the CLUT word), bit 0 of the second adds one texture
  // page (+1 to the tpage word = 64 halfwords = 128 texels at 8bpp to the right).
  bool clut_bit = false, page_bit = false;
};

struct RawVertex {
  u32 index = 0;
  std::array<u16, 2> joint{0, 0};
  std::array<int, 2> weight{4096, 0};
  std::array<std::array<int, 3>, 2> local{};
  int influences = 1;
  // VDDM record: own record (8-byte vertex), rec0's (16-byte vertex; rotated by J2).
  std::array<s8, 3> normal{0, 0, 0};
};

struct Construct {
  std::vector<RawVertex> verts;
  std::vector<Face> faces;
  u32 first_index = 0;
};

Construct parse_construct(const SpanReader& reader, size_t s1, size_t construct_off, u32 base_index, size_t njoints, bool last,
                          size_t next_off) {
  Construct construct;
  construct.first_index = base_index;
  size_t cursor = s1 + construct_off;
  const u32 um = reader.at<u32>(cursor), vp = reader.at<u32>(cursor + 4), vd = reader.at<u32>(cursor + 8);
  cursor += 12;
  const u32 na = reader.at<u32>(cursor), vo8 = reader.at<u32>(cursor + 4);
  cursor += 8;
  struct A8 {
    u16 nv, joint;
  };
  std::vector<A8> a8;
  for (u32 i = 0; i < na; ++i) {
    a8.push_back({reader.at<u16>(cursor), reader.at<u16>(cursor + 2)});
    cursor += 4;
  }
  const u32 nb = reader.at<u32>(cursor), vo16 = reader.at<u32>(cursor + 4);
  cursor += 8;
  struct A16 {
    u16 nv, j1, w1, j2, w2;
  };
  std::vector<A16> a16;
  for (u32 i = 0; i < nb; ++i) {
    a16.push_back({reader.at<u16>(cursor), reader.at<u16>(cursor + 4), reader.at<u16>(cursor + 6), reader.at<u16>(cursor + 8), reader.at<u16>(cursor + 10)});
    cursor += 12;
  }
  const u32 ne = reader.at<u32>(cursor);
  cursor += 4;
  struct Ent {
    u16 type, count;
    u32 off;
  };
  std::vector<Ent> ents;
  for (u32 i = 0; i < ne; ++i) {
    ents.push_back({reader.at<u16>(cursor), reader.at<u16>(cursor + 2), reader.at<u32>(cursor + 4)});
    cursor += 8;
  }
  if (!last && s1 + next_off != cursor) failf("construct footer end {:#x} != next {:#x}", cursor, s1 + next_off);

  u32 n8 = 0;
  for (const auto& group : a8) n8 += group.nv;
  if (vo8 != 0 || vo16 != 8ull * n8) failf("pool offsets {:#x}/{:#x} (n8={})", vo8, vo16, n8);
  size_t pos = s1 + vp + vo8;
  for (const auto& group : a8) {
    for (u16 i = 0; i < group.nv; ++i) {
      RawVertex vertex;
      for (int k = 0; k < 3; ++k) vertex.local[0][k] = static_cast<s16>(reader.at<u16>(pos + 2 * k));
      vertex.index = reader.at<u16>(pos + 6);
      pos += 8;
      if (group.joint >= njoints) failf("vertex joint {} >= {}", group.joint, njoints);
      vertex.joint[0] = group.joint;
      construct.verts.push_back(vertex);
    }
  }
  // VDDM: one 4-byte record per position record, 8-byte area first. The skinner reads the record of
  // an 8-byte vertex and the first record (rec0's) of a 16-byte vertex pair.
  const size_t nd = s1 + vd;
  size_t n16 = 0;
  for (const auto& group : a16) n16 += group.nv;
  const bool has_vd = vd != 0 && nd + 4 * (n8 + 2 * n16) <= reader.size();
  for (size_t i = 0; has_vd && i < construct.verts.size(); ++i)
    for (size_t k = 0; k < 3; ++k) construct.verts[i].normal[k] = static_cast<s8>(reader.at<u8>(nd + 4 * i + k));
  pos = s1 + vp + vo16;
  size_t i16 = 0;
  for (const auto& group : a16) {
    for (u16 i = 0; i < group.nv; ++i) {
      RawVertex vertex;
      const u16 ia = reader.at<u16>(pos + 6), ib = reader.at<u16>(pos + 14);
      if (ia != ib) fail("16-byte vertex idx mismatch");
      vertex.index = ia;
      // record order is (position rel. J2, position rel. J1)
      for (int k = 0; k < 3; ++k) {
        // rec1 is relative to J1, rec0 to J2.
        vertex.local[0][k] = static_cast<s16>(reader.at<u16>(pos + 8 + 2 * k));
        vertex.local[1][k] = static_cast<s16>(reader.at<u16>(pos + 2 * k));
      }
      pos += 16;
      const int wsum = group.w1 + group.w2;
      if (group.j1 >= njoints || group.j2 >= njoints || wsum < 4094 || wsum > 4096)
        failf("16-byte vertex joints/weights {},{} {}+{}", group.j1, group.j2, group.w1, group.w2);
      vertex.joint = {group.j1, group.j2};
      vertex.weight = {group.w1, group.w2};
      vertex.influences = 2;
      if (has_vd)
        for (size_t k = 0; k < 3; ++k) vertex.normal[k] = static_cast<s8>(reader.at<u8>(nd + 4 * (n8 + 2 * i16) + k));
      ++i16;
      construct.verts.push_back(vertex);
    }
  }

  // Faces: textured/flat packets with the pointer -> vertex rule.
  // -1 = dummy slot.
  std::vector<int64_t> slots;
  for (const auto& ent : ents) {
    size_t sz, corners;
    bool textured;
    switch (ent.type) {
      case kFlatTri: sz = 12; corners = 3; textured = false; break;
      case kTexTri: sz = 12; corners = 3; textured = true; break;
      case kFlatQuad: sz = 12; corners = 4; textured = false; break;
      case kTexQuad: sz = 16; corners = 4; textured = true; break;
      default: failf("unknown UM type {:#x}", ent.type);
    }
    for (u32 i = 0; i < ent.count; ++i) {
      const size_t rec = s1 + um + ent.off + i * sz;
      Face face;
      face.corners = static_cast<int>(corners);
      face.textured = textured;
      size_t ptr_o;
      if (textured) {
        for (size_t k = 0; k < corners; ++k) face.uv[k] = {reader.at<u8>(rec + 2 * k), reader.at<u8>(rec + 2 * k + 1)};
        ptr_o = rec + 2 * corners;
      } else {
        face.color = {reader.at<u8>(rec), reader.at<u8>(rec + 1), reader.at<u8>(rec + 2)};
        ptr_o = rec + 4;
      }
      std::array<int, 4> ptrs{};
      for (size_t k = 0; k < corners; ++k) ptrs[k] = static_cast<s16>(reader.at<u16>(ptr_o + 2 * k));
      std::array<int, 4> ordered = ptrs;
      // Textured triangles store rotated pointers.
      if (corners == 3 && textured) ordered = {ptrs[1], ptrs[2], ptrs[0], 0};
      for (size_t k = 0; k < corners; ++k) {
        const int unit = (k < 2 && textured) ? 16 : 8;
        const int pointer = ordered[k];
        if (textured && k == 0) face.clut_bit = (floor_mod(pointer, unit) & 1) != 0;
        if (textured && k == 1) face.page_bit = (floor_mod(pointer, unit) & 1) != 0;
        int64_t slot;
        if (pointer >= 0) {
          slot = pointer / unit;
        } else {
          const int64_t ref = static_cast<int64_t>(slots.size()) + floor_div(pointer, unit);
          if (ref < 0 || slots[static_cast<size_t>(ref)] < 0) failf("bad back-reference {} at slot {}", pointer, slots.size());
          slot = slots[static_cast<size_t>(ref)];
        }
        face.vertex_ids[k] = static_cast<u32>(slot);
        slots.push_back(slot);
      }
      if (corners == 3) slots.push_back(-1);
      construct.faces.push_back(face);
    }
  }
  return construct;
}

Mesh build_mesh(const Construct& construct, const std::vector<World>& world, const TexInfo& tex, Notes& notes, size_t ci) {
  Mesh mesh;
  const u32 lo = construct.first_index, hi = lo + static_cast<u32>(construct.verts.size());
  mesh.pool.resize(construct.verts.size());
  std::vector<u8> seen(construct.verts.size(), 0);
  for (const auto& vertex : construct.verts) {
    if (vertex.index < lo || vertex.index >= hi || seen[vertex.index - lo])
      failf("construct {}: vertex index {} outside {}..{} or duplicated", ci, vertex.index, lo, hi);
    seen[vertex.index - lo] = 1;
    SkinVertex& skin = mesh.pool[vertex.index - lo];
    skin.source_index = vertex.index;
    const double wsum = vertex.weight[0] + vertex.weight[1];
    double acc[3] = {0, 0, 0};
    for (int i = 0; i < vertex.influences; ++i) {
      const World& joint_world = world[vertex.joint[static_cast<size_t>(i)]];
      const double wt = vertex.weight[static_cast<size_t>(i)] / wsum;
      const auto& local = vertex.local[static_cast<size_t>(i)];
      for (int row = 0; row < 3; ++row)
        acc[row] += (joint_world.matrix[row * 3] * local[0] + joint_world.matrix[row * 3 + 1] * local[1] + joint_world.matrix[row * 3 + 2] * local[2] + joint_world.offset[row]) * wt;
      skin.joint[static_cast<size_t>(i)] = vertex.joint[static_cast<size_t>(i)];
      skin.weight[static_cast<size_t>(i)] = static_cast<float>(wt);
      for (int axis = 0; axis < 3; ++axis) skin.local[static_cast<size_t>(i)][static_cast<size_t>(axis)] = static_cast<float>(local[static_cast<size_t>(axis)]);
    }
    if (vertex.influences == 1) skin.weight = {1, 0};
    skin.normal = vertex.normal;
    skin.normal_joint = vertex.influences == 2 ? 1 : 0;
    skin.bind_pos = {static_cast<float>(acc[0]), static_cast<float>(acc[1]), static_cast<float>(acc[2])};
  }

  std::map<std::tuple<u32, float, float, u32>, u32> dedupe;
  bool page_warned = false, clut_warned = false;
  for (const auto& face : construct.faces) {
    for (int k = 0; k < face.corners; ++k)
      if (face.vertex_ids[static_cast<size_t>(k)] < lo || face.vertex_ids[static_cast<size_t>(k)] >= hi)
        failf("construct {}: face references vertex {} outside its construct", ci, face.vertex_ids[static_cast<size_t>(k)]);
    // Flat faces (UM 0x20 / 0x28, 92 in the whole data) are decoded (their pointer slots feed the back-references) but never drawn: every
    // construct loop dispatches 0x24 / 0x2C only and steps over other entries.
    if (!face.textured) {
      ++notes.faces;
      continue;
    }
    Material mat;
    u32 u_off = 0;
    if (face.textured) {
      mat.texture = tex.have ? 0 : -1;
      // CLUT bit -> the next CLUT row (textures[1]: an Atim's second palette row, a room model's CLUT row + 1); page bit -> the texture
      // page to the right (u + 128: the right half of a 256-px Atim image, a room model's neighbouring VRAM page). Independent bits.
      if (face.clut_bit && tex.have) {
        if (tex.main_count >= 2) mat.texture = 1;
        else if (!clut_warned) {
          notes.warnings.push_back(std::format("construct {}: CLUT bit on a texture with one CLUT row", ci));
          clut_warned = true;
        }
      }
      if (face.page_bit) {
        u_off = 128;
        if (tex.have && tex.width < 256 && !page_warned) {
          notes.warnings.push_back(std::format("construct {}: page bit on a {}-px image", ci, tex.width));
          page_warned = true;
        }
      }
    } else {
      mat.color = face.color;
    }
    u32 mi = 0;
    for (; mi < mesh.materials.size(); ++mi)
      if (mesh.materials[mi] == mat) break;
    if (mi == mesh.materials.size()) mesh.materials.push_back(mat);

    std::array<u32, 4> corner{};
    for (int k = 0; k < face.corners; ++k) {
      float u = 0, v = 0;
      if (face.textured) {
        const u32 pu = face.uv[static_cast<size_t>(k)][0] + u_off, pv = face.uv[static_cast<size_t>(k)][1];
        if (tex.have && pv >= tex.height) {
          // Multi-image atims (kurage, manet) overrun the stacked height by a row; the Python reference only checks
          // single-image textures, so this is a warning there.
          auto& list = tex.images > 1 ? notes.warnings : notes.errors;
          list.push_back(std::format("UV v={} beyond image height {}", pv, tex.height));
        }
        u = (static_cast<float>(pu) + 0.5f) / static_cast<float>(tex.width);
        v = (static_cast<float>(pv) + 0.5f) / static_cast<float>(tex.height);
      }
      const u32 pool = face.vertex_ids[static_cast<size_t>(k)] - lo;
      const auto key = std::make_tuple(pool, u, v, mi);
      auto it = dedupe.find(key);
      if (it == dedupe.end()) {
        it = dedupe.emplace(key, static_cast<u32>(mesh.vertices.size())).first;
        mesh.vertices.push_back({pool, {u, v}});
      }
      corner[static_cast<size_t>(k)] = it->second;
    }
    u32 first = static_cast<u32>(mesh.indices.size());
    // Quad vertices are in GPU Z-order: triangles (0,1,2) and (2,1,3).
    mesh.indices.insert(mesh.indices.end(), {corner[0], corner[1], corner[2]});
    mesh.tri_quad.push_back(face.corners == 4 ? u8{1} : u8{0});
    if (face.corners == 4) {
      mesh.indices.insert(mesh.indices.end(), {corner[2], corner[1], corner[3]});
      mesh.tri_quad.push_back(2);
      ++mesh.quad_count;
    }
    const u32 cnt = static_cast<u32>(mesh.indices.size()) - first;
    if (mesh.submeshes.empty() || mesh.submeshes.back().material != mi) mesh.submeshes.push_back({mi, first, 0});
    mesh.submeshes.back().index_count += cnt;
    ++notes.faces;
  }
  notes.vertices += static_cast<u32>(mesh.pool.size());
  return mesh;
}

// ---------------------------------------------------------------- animations
struct RawAnim {
  u32 frames = 0, joints = 0;
  std::vector<u8> rot_flag, trans_flag;
  // Layout [frame][joint][3]; unflagged joints repeat frame 0.
  std::vector<s16> rot, trans;
};

u32 infer_joint_count(const SpanReader& reader) {
  const u32 frames = reader.at<u32>(0);
  const size_t size = reader.size();
  for (u32 nw = 1; nw < 6; ++nw) {
    const size_t p0 = 4 + 4ull * nw + 4ull * frames;
    if (p0 > size) break;
    const size_t nxt = frames > 1 ? reader.at<u32>(4 + 4ull * nw + 4) : size;
    if (nxt <= p0) continue;
    const size_t sz = nxt - p0;
    if (sz % 12 == 0 && reader.at<u32>(4 + 4ull * nw) == p0) {
      const u32 joints = static_cast<u32>(sz / 12);
      if ((2 * joints + 31) / 32 == nw) return joints;
    }
  }
  fail("cannot infer anim joint count");
}

RawAnim parse_anim(ByteSpan data, u32 nj) {
  SpanReader reader(data);
  RawAnim anim;
  anim.frames = reader.at<u32>(0);
  if (anim.frames == 0 || anim.frames > 4000) failf("anim frame count {}", anim.frames);
  if (nj == 0) nj = infer_joint_count(reader);
  anim.joints = nj;
  const size_t nflag = ((2ull * nj + 31) / 32) * 4;
  anim.rot_flag.resize(nj);
  anim.trans_flag.resize(nj);
  for (u32 j = 0; j < nj; ++j) {
    const u32 word = reader.at<u32>(4 + 4ull * ((2 * j) / 32));
    anim.rot_flag[j] = (word >> ((2 * j) % 32)) & 1;
    anim.trans_flag[j] = (word >> ((2 * j) % 32 + 1)) & 1;
  }
  if ((2 * nj) % 32 != 0) {
    const u32 last = reader.at<u32>(4 + 4ull * ((2 * nj) / 32));
    if (last >> ((2 * nj) % 32)) fail("flag bits beyond joint count");
  }
  const size_t ptr_o = 4 + nflag;
  std::vector<size_t> bounds(anim.frames + 1);
  for (u32 k = 0; k < anim.frames; ++k) bounds[k] = reader.at<u32>(ptr_o + 4ull * k);
  bounds[anim.frames] = data.size();
  if (bounds[0] != 4 + nflag + 4ull * anim.frames) failf("frame 0 pointer {:#x}", bounds[0]);
  size_t ncmd = 0;
  for (u32 j = 0; j < nj; ++j) ncmd += anim.rot_flag[j] + anim.trans_flag[j];
  anim.rot.assign(static_cast<size_t>(anim.frames) * nj * 3, 0);
  anim.trans.assign(anim.rot.size(), 0);
  for (u32 k = 0; k < anim.frames; ++k) {
    const size_t start = bounds[k], end = bounds[k + 1];
    if (end < start || end > data.size()) fail("frame pointers not monotonic");
    s16* rot = &anim.rot[static_cast<size_t>(k) * nj * 3];
    s16* trn = &anim.trans[static_cast<size_t>(k) * nj * 3];
    if (k == 0) {
      if (end - start != 12ull * nj) failf("frame 0 size {} != {}", end - start, 12ull * nj);
      for (u32 j = 0; j < nj; ++j)
        for (int axis = 0; axis < 3; ++axis) {
          rot[j * 3 + axis] = static_cast<s16>(reader.at<u16>(start + 12ull * j + 2 * axis));
          trn[j * 3 + axis] = static_cast<s16>(reader.at<u16>(start + 12ull * j + 6 + 2 * axis));
        }
    } else {
      const size_t want = 6 * ncmd;
      if (end - start < want) failf("frame {} size {} != {}", k, end - start, want);
      const size_t pad = end - start - want;
      if (!(pad == 0 || (k == anim.frames - 1 && pad < 4))) failf("frame {} size {} != {}", k, end - start, want);
      size_t cursor = start;
      for (u32 j = 0; j < nj; ++j) {
        if (anim.rot_flag[j]) {
          for (int axis = 0; axis < 3; ++axis) rot[j * 3 + axis] = static_cast<s16>(reader.at<u16>(cursor + 2 * axis));
          cursor += 6;
        } else {
          for (int axis = 0; axis < 3; ++axis) rot[j * 3 + axis] = anim.rot[static_cast<size_t>(j) * 3 + axis];
        }
        if (anim.trans_flag[j]) {
          for (int axis = 0; axis < 3; ++axis) trn[j * 3 + axis] = static_cast<s16>(reader.at<u16>(cursor + 2 * axis));
          cursor += 6;
        } else {
          for (int axis = 0; axis < 3; ++axis) trn[j * 3 + axis] = anim.trans[static_cast<size_t>(j) * 3 + axis];
        }
      }
    }
  }
  return anim;
}

// Frame 0 holds 12 bytes per joint right behind the pointer table, so the words tell whether an animation was authored for `nj` joints.
bool anim_fits(ByteSpan data, u32 nj) {
  if (data.size() < 4) return false;
  SpanReader reader(data);
  const u32 frames = reader.at<u32>(0);
  const size_t ptr_o = 4 + ((2ull * nj + 31) / 32) * 4, frame0 = ptr_o + 4ull * frames;
  if (frames == 0 || frame0 + 12ull * nj > data.size()) return false;
  const size_t next = frames > 1 ? reader.at<u32>(ptr_o + 4) : data.size();
  return reader.at<u32>(ptr_o) == frame0 && next - frame0 == 12ull * nj;
}

// Anim cpt -> raw anims (joint count inferred when 0). Returns nullopt-by-throw semantics via Fail.
std::vector<RawAnim> parse_anim_pack(ByteSpan pack, u32 nj) {
  auto cpt = parse_cpt(pack);
  if (!cpt) fail("anim pack: not a cpt");
  std::vector<RawAnim> out;
  for (const auto& entry : cpt->entries) out.push_back(parse_anim(entry.data, nj));
  return out;
}

AnimationClip make_clip(const RawAnim& anim, const std::vector<RawJoint>& rest, std::string source, u32 index) {
  AnimationClip clip;
  clip.source = source;
  clip.index = index;
  clip.name = std::format("{}_{:02}", source, index);
  clip.frame_count = anim.frames;
  clip.duration = static_cast<float>(anim.frames) / kModelAnimFps;
  clip.rot_animated = anim.rot_flag;
  clip.trans_animated = anim.trans_flag;
  clip.tracks.resize(anim.joints);
  const size_t nj = anim.joints;
  for (size_t j = 0; j < nj; ++j) {
    JointTrack& track = clip.tracks[j];
    const u32 nr = anim.rot_flag[j] ? anim.frames : 1, nt = anim.trans_flag[j] ? anim.frames : 1;
    for (u32 k = 0; k < std::max(nr, nt); ++k) {
      const float time = static_cast<float>(k) / kModelAnimFps;
      if (k < nr) {
        Key3 key;
        key.time = time;
        for (size_t x = 0; x < 3; ++x)
          key.v[x] = static_cast<float>((rest[j].rot[x] + anim.rot[(static_cast<size_t>(k) * nj + j) * 3 + x]) * kAngleUnit);
        track.rot.push_back(key);
      }
      if (k < nt) {
        Key3 key;
        key.time = time;
        for (size_t x = 0; x < 3; ++x)
          key.v[x] = static_cast<float>(rest[j].trans[x] + anim.trans[(static_cast<size_t>(k) * nj + j) * 3 + x]);
        track.trans.push_back(key);
      }
    }
  }
  return clip;
}

// ---------------------------------------------------------------- Mdl
ByteSpan at0_pack_or_empty(ByteSpan prd) {
  auto drp = parse_drp(prd);
  if (!drp) return {};
  for (const auto& entry : drp->entries)
    if (entry.type == 0x19) return entry.data;
  return {};
}

// Share of the 32 x 32 texels at (x, y) of `main` that match strip half `half` of eye frame 1 (the half-shut eye: the mask / skin / hair
// around the eye are the open texture's); 0 when the rect leaves the image.
double eye_fit(const Image& main, const Image& strip, size_t half, u32 x, u32 y) {
  if (x + 32 > main.width || y + 32 > main.height) return 0.0;
  u32 same = 0;
  for (u32 row = 0; row < 32; ++row)
    for (u32 col = 0; col < 32; ++col) {
      const u8* a = &main.pixels[(size_t(y + row) * main.width + x + col) * 4];
      const u8* b = &strip.pixels[(size_t(row) * strip.width + half * 32 + col) * 4];
      if (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) < 24) ++same;
    }
  return same / 1024.0;
}

// The eye rects of a texture whose atlas does not keep them where S5 says: the HD file set re-lays some textures but keeps the PS1 word
// (Guile: rect (52, 28) is braid in the HD texture, the eye is at (2, 28)), so a blink would paste the eye over another part. A rect the
// strip does not fit at all moves to where it fits best; the PS1 data's own loose fits (the shut frame differs around the eye) stay.
void fit_eye_rects(ModelAsset& asset) {
  const Image& main = asset.textures[0];
  const Image& strip = asset.textures[asset.main_texture_count];
  if (main.indexed || strip.indexed || strip.width < 128 || strip.height < 32 || main.width < 32 || main.height < 32) return;
  for (size_t half = 0; half < 2; ++half) {
    if (eye_fit(main, strip, half, asset.eye.x[half], asset.eye.y[half]) >= 0.1) continue;
    double best = 0.0;
    u32 best_x = 0, best_y = 0;
    for (u32 y = 0; y + 32 <= main.height; ++y)
      for (u32 x = 0; x + 32 <= main.width; x += 2)
        if (const double fit = eye_fit(main, strip, half, x, y); fit > best) best = fit, best_x = x, best_y = y;
    if (best < 0.5) continue;
    asset.eye.x[half] = static_cast<u16>(best_x);
    asset.eye.y[half] = static_cast<u16>(best_y);
  }
}

ModelAsset load_mdl(ByteSpan obj, const TexInfo& tex, ByteSpan ext_pack, const char* ext_source, bool field, Notes& notes,
                    ByteSpan hd_tim = {}) {
  // HD-mode (cdrom2) field models carry a trailing block (~0.2-0.5 KB, a second 'drp' table) after the EOF pointer.
  if (obj.size() > 32) {
    u32 eof = 0;
    std::memcpy(&eof, obj.data() + 28, 4);
    if (eof >= 32 && eof < obj.size()) obj = obj.first(eof);
  }
  SpanReader reader(obj);
  if (reader.at<u32>(0) != 6) failf("header count {} != 6", reader.at<u32>(0));
  std::array<u32, 6> sec{};
  for (size_t i = 0; i < 6; ++i) sec[i] = reader.at<u32>(4 + 4 * i);
  if (reader.at<u32>(28) != obj.size()) failf("EOF pointer {:#x} != size {:#x}", reader.at<u32>(28), obj.size());
  const u32 s1 = sec[0], s2 = sec[1], s3 = sec[2], s4 = sec[3], s6 = sec[5];
  // Room models leave S5 = 0 (no anim-related blocks); the anim pack then runs to S6 / the end.
  const u32 end = s6 ? s6 : static_cast<u32>(obj.size());
  const u32 s5 = sec[4] ? sec[4] : end;
  if (!(s1 < s2 && s2 < s3 && s3 < s4 && s4 < s5 && s5 <= end && end <= obj.size()))
    fail("section pointers not monotonic");

  ModelAsset asset;
  asset.kind = field ? "field" : "battle";
  const auto raw = parse_skeleton(reader, s2, s3);
  const size_t nj = raw.size();
  asset.skeleton.joints.resize(nj);
  for (size_t i = 0; i < nj; ++i) {
    Joint& joint = asset.skeleton.joints[i];
    joint.parent = raw[i].parent;
    joint.rest_rot_deg = {static_cast<float>(raw[i].rot[0] * kAngleUnit), static_cast<float>(raw[i].rot[1] * kAngleUnit),
                          static_cast<float>(raw[i].rot[2] * kAngleUnit)};
    joint.rest_quat = quat_zyx(joint.rest_rot_deg);
    joint.rest_trans = {static_cast<float>(raw[i].trans[0]), static_cast<float>(raw[i].trans[1]), static_cast<float>(raw[i].trans[2])};
    joint.bone = raw[i].bone_ref == 0xFFFFFFFFu ? -1 : static_cast<int>(raw[i].bone_ref >> 16);
  }

  asset.textures = tex.textures;
  if (!hd_tim.empty() && tex.have) {
    if (auto hd = load_hd_tim(hd_tim)) {
      const Image& sd = tex.textures[0];
      if (sd.width && sd.height && hd->width % sd.width == 0 && hd->height % sd.height == 0 &&
          hd->width / sd.width == hd->height / sd.height && hd->width > sd.width) {
        asset.hd_scale = hd->width / sd.width;
        asset.hd_textures.resize(asset.textures.size());
        asset.hd_textures[0] = std::move(*hd);
      }
    }
  }
  asset.main_texture_count = tex.main_count;

  // S1 geometry
  const auto world = rest_world(raw);
  const u32 nc = reader.at<u32>(s1);
  if (nc == 0 || nc > 256) failf("construct count {}", nc);
  std::vector<u32> offs(nc);
  for (u32 i = 0; i < nc; ++i) offs[i] = reader.at<u32>(s1 + 4 + 4ull * i);
  // the two words after the table: s16 extent x, y, z (the model's effect scales)
  for (size_t k = 0; k < 3; ++k) asset.extent[k] = static_cast<s16>(reader.at<u16>(s1 + 4 + 4ull * nc + 2 * k));
  u32 base = 0;
  for (u32 ci = 0; ci < nc; ++ci) {
    Construct construct = parse_construct(reader, s1, offs[ci], base, nj, ci + 1 == nc, ci + 1 < nc ? offs[ci + 1] : 0);
    asset.meshes.push_back(build_mesh(construct, world, tex, notes, ci));
    base += static_cast<u32>(construct.verts.size());
  }

  // S3
  if (s4 - s3 != kS3Size) failf("S3 size {:#x}", s4 - s3);
  for (size_t i = 0; i < 32; ++i) {
    const size_t pos = s3 + 8 * i;
    AttachPoint& point = asset.attach.points[i];
    point.pos = {static_cast<float>(static_cast<s16>(reader.at<u16>(pos))), static_cast<float>(static_cast<s16>(reader.at<u16>(pos + 2))),
                 static_cast<float>(static_cast<s16>(reader.at<u16>(pos + 4)))};
    point.joint = reader.at<u16>(pos + 6);
    point.used = point.pos[0] != 0 || point.pos[1] != 0 || point.pos[2] != 0 || point.joint != 0;
  }
  for (size_t i = 0; i < 16; ++i) {
    const size_t pos = s3 + 0x100 + 4 * i;
    asset.shading[i] = {reader.at<u8>(pos), reader.at<u8>(pos + 1), reader.at<u8>(pos + 2), reader.at<u8>(pos + 3)};
  }
  // S3 +0x140: the attached-object mounts.
  for (size_t i = 0; i < 2; ++i) {
    const size_t pos = s3 + 0x140 + 16 * i;
    MountPoint& mount = asset.attach.mounts[i];
    for (size_t k = 0; k < 3; ++k) {
      mount.rot[k] = static_cast<float>(static_cast<s16>(reader.at<u16>(pos + 2 * k)));
      mount.pos[k] = static_cast<float>(static_cast<s16>(reader.at<u16>(pos + 6 + 2 * k)));
    }
    mount.attach = static_cast<u16>(reader.at<u16>(pos + 12) & 31);
    mount.used = mount.attach != 0 || mount.rot[0] != 0 || mount.rot[1] != 0 || mount.rot[2] != 0 || mount.pos[0] != 0 || mount.pos[1] != 0 || mount.pos[2] != 0;
  }
  // Shading entry k bit 1 = construct k semi-transparent with ABR (flags >> 2) & 3 (the packet code is
  // 0x34 | (flags & 2) and the tpage ABR (flags & 0xC) << 3). Only the texels
  // with STP set blend (tim.h kStpAlpha). Bit 0 (not drawn) is an instance state (ops 1B / 1C) and stays with the caller.
  for (size_t k = 0; k < asset.meshes.size() && k < asset.shading.size(); ++k) {
    const u8 flags = asset.shading[k].flags;
    asset.meshes[k].trans = (flags & 2) ? static_cast<u8>(1 + ((flags >> 2) & 3)) : u8{0};
  }

  // S4 internal animations
  {
    const auto raws = parse_anim_pack(obj.subspan(s4, s5 - s4), static_cast<u32>(nj));
    for (size_t i = 0; i < raws.size(); ++i) asset.clips.push_back(make_clip(raws[i], raw, "s4", static_cast<u32>(i)));
  }
  // External pack
  if (!ext_pack.empty()) {
    const auto raws = parse_anim_pack(ext_pack, 0);
    for (size_t i = 0; i < raws.size(); ++i) {
      if (raws[i].joints != nj) failf("external anim J={} != model J={}", raws[i].joints, nj);
      asset.clips.push_back(make_clip(raws[i], raw, ext_source, static_cast<u32>(i)));
    }
  }
  if (s6) {
    if (auto drp = parse_drp(obj.subspan(s6))) asset.drp_entries = static_cast<u32>(drp->entries.size());
  }
  // S5: the animation-sequence table. Room models leave S5 = 0.
  if (sec[4]) asset.anim_seq = parse_anim_seq_table(obj.subspan(s5, end - s5));
  // The party members' eye blink: the word at +0x80 of S5 entry 0 (an
  // entry with no code) holds the two eye rects of the main texture as bytes x1 y1 x2 y2 (x in texels, stored as halfwords: x & ~1); the
  // closed-eye frames are the .ltc overlay strip. Only models that carry the strip (the 52 battle players).
  if (!field && sec[4] && asset.textures.size() > asset.main_texture_count && asset.main_texture_count > 0 && s5 + 8 <= end) {
    const u32 n5 = reader.at<u32>(s5);
    const u32 e0 = n5 ? reader.at<u32>(s5 + 4) : 0;
    if (n5 && e0 && s5 + e0 + 0x84 <= end) {
      const u32 eye_word = reader.at<u32>(s5 + e0 + 0x80);
      asset.eye.valid = true;
      asset.eye.x = {static_cast<u16>((eye_word & 0xFF) & ~1u), static_cast<u16>(((eye_word >> 16) & 0xFF) & ~1u)};
      asset.eye.y = {static_cast<u16>((eye_word >> 8) & 0xFF), static_cast<u16>((eye_word >> 24) & 0xFF)};
      fit_eye_rects(asset);
    }
  }
  return asset;
}

template <typename Fn>
auto guarded(std::string* err, Fn&& fn) -> std::optional<decltype(fn())> {
  try {
    return fn();
  } catch (const Fail& failure) {
    if (err) *err = failure.msg;
  } catch (const std::out_of_range& range_error) {
    if (err) *err = std::string("truncated or out-of-range data: ") + range_error.what();
  } catch (const std::bad_alloc&) {
    if (err) *err = "out of memory (corrupt counts)";
  }
  return std::nullopt;
}

ByteSpan ex_pack_span(ByteSpan bin) {
  auto cpt = parse_cpt(bin);
  if (!cpt || cpt->entries.size() != 2) return {};
  return cpt->entries[0].data;
}

ByteSpan ex_seq_span(ByteSpan bin) {
  auto cpt = parse_cpt(bin);
  if (!cpt || cpt->entries.size() != 2) return {};
  return cpt->entries[1].data;
}

// Rigid mesh packets.
Mesh parse_mesh(ByteSpan data, float tw, float th) {
  SpanReader reader(data);
  if (reader.at<u32>(0) != 0x80000001u) failf("mesh magic {:#x}", reader.at<u32>(0));
  const u16 nv = reader.at<u16>(4), nf = reader.at<u16>(6);
  const u32 vo = reader.at<u32>(8), go = reader.at<u32>(16);
  Mesh mesh;
  mesh.skinned = false;
  mesh.pool.resize(nv);
  for (u16 i = 0; i < nv; ++i) {
    SkinVertex& skin = mesh.pool[i];
    skin.source_index = i;
    for (size_t k = 0; k < 3; ++k) skin.bind_pos[k] = static_cast<float>(static_cast<s16>(reader.at<u16>(vo + 8ull * i + 2 * k)));
    skin.local[0] = skin.bind_pos;
  }
  std::map<std::tuple<u32, float, float, u32, u32>, u32> dedupe;
  u32 faces = 0;
  const u32 ng = reader.at<u32>(go);
  if (ng > 4096) fail("mesh group count");
  for (u32 group = 0; group < ng; ++group) {
    const u16 typ = reader.at<u16>(go + 4 + 8ull * group), cnt = reader.at<u16>(go + 6 + 8ull * group);
    const u32 off = reader.at<u32>(go + 8 + 8ull * group);
    const bool quad = typ & 8, tex = typ & 4, gour = typ & 0x10;
    const size_t nvv = quad ? 4 : 3, ncol = gour ? nvv : 1;
    const size_t uvb = tex ? (quad ? 12 : 10) : 0;
    const size_t size = 4 * ncol + uvb + 2 * nvv + ((tex || quad) ? 0 : 2);
    // Untextured gouraud groups (G3 0x30 / G4 0x38) are drawn by neither weapon dispatcher.
    const bool drawn = tex || !gour;
    for (u16 i = 0; i < cnt; ++i) {
      if (!drawn) {
        ++faces;
        continue;
      }
      const size_t rec = off + static_cast<size_t>(i) * size;
      const size_t idx_pos = rec + 4 * ncol + uvb;
      Material mat;
      if (tex) mat.texture = 0;
      else mat.color = {reader.at<u8>(rec), reader.at<u8>(rec + 1), reader.at<u8>(rec + 2)};
      u32 mi = 0;
      for (; mi < mesh.materials.size(); ++mi)
        if (mesh.materials[mi] == mat) break;
      if (mi == mesh.materials.size()) mesh.materials.push_back(mat);
      static constexpr size_t uv_off[4] = {0, 4, 8, 10};
      // Textured triangles (FT3 0x24 / GT3 0x34): the weapon renderers load the vertices
      // as idx1, idx2, idx0 and attach the packet's corner data (uv, colour) in packet order, so corner k belongs to idx[(k + 1) % 3] - the
      // battle-stage rule of the same packet format. The cyclic order, and with it the NCLIP side, is unchanged.
      const bool rotated = tex && !quad;
      std::array<u32, 4> corner{};
      for (size_t k = 0; k < nvv; ++k) {
        const u16 idx = reader.at<u16>(idx_pos + 2 * (rotated ? (k + 1) % 3 : k));
        if (idx >= nv) fail("mesh index out of range");
        float u = 0, v = 0;
        std::array<u8, 3> rgb{128, 128, 128};
        if (tex) {
          u = (static_cast<float>(reader.at<u8>(rec + 4 * ncol + uv_off[k])) + 0.5f) / tw;
          v = (static_cast<float>(reader.at<u8>(rec + 4 * ncol + uv_off[k] + 1)) + 0.5f) / th;
          // The texel is modulated by the packet colour: one per corner on a Gouraud face (GT3 / GT4), the face's own on FT3 / FT4.
          const size_t colour = rec + 4 * (gour ? k : 0);
          rgb = {reader.at<u8>(colour), reader.at<u8>(colour + 1), reader.at<u8>(colour + 2)};
        }
        const auto key = std::make_tuple(static_cast<u32>(idx), u, v, mi, static_cast<u32>(rgb[0] | rgb[1] << 8 | rgb[2] << 16));
        auto it = dedupe.find(key);
        if (it == dedupe.end()) {
          it = dedupe.emplace(key, static_cast<u32>(mesh.vertices.size())).first;
          mesh.vertices.push_back({idx, {u, v}, rgb});
        }
        corner[k] = it->second;
      }
      const u32 first = static_cast<u32>(mesh.indices.size());
      mesh.indices.insert(mesh.indices.end(), {corner[0], corner[1], corner[2]});
      mesh.tri_quad.push_back(quad ? u8{1} : u8{0});
      if (quad) {
        mesh.indices.insert(mesh.indices.end(), {corner[2], corner[1], corner[3]});
        mesh.tri_quad.push_back(2);
        ++mesh.quad_count;
      }
      const bool not_gouraud_textured = !(tex && gour);
      if (mesh.submeshes.empty() || mesh.submeshes.back().material != mi || mesh.submeshes.back().not_gouraud_textured != not_gouraud_textured)
        mesh.submeshes.push_back({mi, first, 0, not_gouraud_textured});
      mesh.submeshes.back().index_count += static_cast<u32>(mesh.indices.size()) - first;
      ++faces;
    }
  }
  if (faces != nf) failf("mesh face count {} != {}", faces, nf);
  // Vertex normals, built the way the loader builds them for a lit weapon: every face of every group in packet order gets the unit normal
  // (b - c) x (a - c) of its indices a b c (quads a b c d: (a - b) x (d - b)); each corner adds it to its vertex normal and normalises again.
  std::vector<std::array<float, 3>> normals(nv, {0.0f, 0.0f, 0.0f});
  for (u32 group = 0; group < ng; ++group) {
    const u16 typ = reader.at<u16>(go + 4 + 8ull * group), cnt = reader.at<u16>(go + 6 + 8ull * group);
    const u32 off = reader.at<u32>(go + 8 + 8ull * group);
    const bool quad = typ & 8, tex = typ & 4, gour = typ & 0x10;
    const size_t nvv = quad ? 4 : 3, ncol = gour ? nvv : 1;
    const size_t size = 4 * ncol + (tex ? (quad ? 12 : 10) : 0) + 2 * nvv + ((tex || quad) ? 0 : 2);
    for (u16 i = 0; i < cnt; ++i) {
      const size_t idx_pos = off + static_cast<size_t>(i) * size + 4 * ncol + (tex ? (quad ? 12 : 10) : 0);
      std::array<u16, 4> idx{};
      bool valid = true;
      for (size_t k = 0; k < nvv; ++k) valid = (idx[k] = reader.at<u16>(idx_pos + 2 * k)) < nv && valid;
      if (!valid) continue;
      const Vec3& origin = mesh.pool[quad ? idx[1] : idx[2]].bind_pos;
      const Vec3& p1 = mesh.pool[quad ? idx[0] : idx[1]].bind_pos;
      const Vec3& p2 = mesh.pool[quad ? idx[3] : idx[0]].bind_pos;
      const float e1[3] = {p1[0] - origin[0], p1[1] - origin[1], p1[2] - origin[2]};
      const float e2[3] = {p2[0] - origin[0], p2[1] - origin[1], p2[2] - origin[2]};
      std::array<float, 3> face = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
      const float face_len = std::sqrt(face[0] * face[0] + face[1] * face[1] + face[2] * face[2]);
      if (face_len <= 0.0f) continue;
      for (float& component : face) component /= face_len;
      for (size_t k = 0; k < nvv; ++k) {
        std::array<float, 3>& normal = normals[idx[k]];
        for (size_t c = 0; c < 3; ++c) normal[c] += face[c];
        const float len = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        if (len > 0.0f)
          for (float& component : normal) component /= len;
      }
    }
  }
  for (u16 i = 0; i < nv; ++i)
    for (size_t k = 0; k < 3; ++k) mesh.pool[i].normal[k] = static_cast<s8>(std::clamp(std::lround(normals[i][k] * 128.0f), -128L, 127L));
  return mesh;
}

}  // namespace

// ---------------------------------------------------------------- public API
std::optional<Image> load_hd_tim(ByteSpan bytes) {
  auto tim = parse_tim(bytes);
  if (!tim) return std::nullopt;
  // The HD CLUT carries STP like the 1x one (all non-zero entries of the field players).
  auto img = decode_tim(*tim, 0, true);
  if (!img || img->width == 0 || img->height == 0) return std::nullopt;
  return img->indexed ? to_rgba(*img) : std::move(*img);
}

bool apply_battle_hd_tim(ModelAsset& asset, ByteSpan tim) {
  if (asset.textures.empty() || asset.main_texture_count == 0) return false;
  const Image& page = asset.textures[0];
  if (page.indexed || !page.width || !page.height) return false;
  const auto parsed = parse_tim(tim);
  auto decoded = parsed ? decode_tim(*parsed, 0, true) : std::nullopt;
  if (!decoded || !decoded->width || !decoded->height) return false;
  std::vector<u32> palette = decoded->indexed ? decoded->palette : std::vector<u32>{};
  const std::optional<Image> full = decoded->indexed ? to_rgba(*decoded) : std::move(*decoded);
  const u32 half_width = full->width / 2;
  if (half_width % page.width || full->height % page.height) return false;
  const u32 scale = half_width / page.width;
  if (scale < 2 || full->height / page.height != scale) return false;
  auto copy_rect = [&](Image& to, u32 to_x, u32 to_y, u32 from_x, u32 from_y, u32 w, u32 h) {
    for (u32 y = 0; y < h; ++y)
      std::memcpy(&to.pixels[(static_cast<size_t>(to_y + y) * to.width + to_x) * 4],
                  &full->pixels[(static_cast<size_t>(from_y + y) * full->width + from_x) * 4], static_cast<size_t>(w) * 4);
  };
  Image open;
  open.width = half_width, open.height = full->height;
  open.pixels.resize(static_cast<size_t>(open.width) * open.height * 4);
  copy_rect(open, 0, 0, 0, 0, open.width, open.height);
  asset.hd_scale = scale;
  asset.hd_palette = std::move(palette);
  asset.hd_textures.assign(asset.textures.size(), Image{});
  asset.hd_textures[0] = std::move(open);
  const size_t strip_index = asset.main_texture_count;
  if (!asset.eye.valid || strip_index >= asset.textures.size()) return true;
  const Image& strip = asset.textures[strip_index];
  constexpr u32 kHalf = 32, kShutFrameX = 64;
  if (strip.indexed || strip.width < kShutFrameX + 2 * kHalf || strip.height < kHalf) return true;
  for (size_t half = 0; half < 2; ++half)
    if (asset.eye.x[half] + kHalf > page.width || asset.eye.y[half] + kHalf > page.height) return true;
  Image shut;
  shut.width = strip.width * scale, shut.height = strip.height * scale;
  shut.pixels.resize(static_cast<size_t>(shut.width) * shut.height * 4);
  for (u32 y = 0; y < shut.height; ++y)
    for (u32 x = 0; x < shut.width; ++x)
      std::memcpy(&shut.pixels[(static_cast<size_t>(y) * shut.width + x) * 4], &strip.pixels[(static_cast<size_t>(y / scale) * strip.width + x / scale) * 4], 4);
  for (size_t half = 0; half < 2; ++half)
    copy_rect(shut, (kShutFrameX + kHalf * static_cast<u32>(half)) * scale, 0, half_width + asset.eye.x[half] * scale, asset.eye.y[half] * scale,
              kHalf * scale, kHalf * scale);
  asset.hd_textures[strip_index] = std::move(shut);
  return true;
}

namespace {
// Header of a 6-section Mdl truncated at its EOF word (load_mdl's rule): section offsets S1..S6 + EOF, or false.
bool mdl_header(ByteSpan& mdl, std::array<u32, 7>& header) {
  if (mdl.size() < 32) return false;
  u32 words[8];
  std::memcpy(words, mdl.data(), 32);
  if (words[0] != 6 || words[1] != 32 || words[7] < 32 || words[7] > mdl.size()) return false;
  mdl = mdl.first(words[7]);
  for (size_t i = 0; i < 7; ++i) header[i] = words[i + 1];
  return true;
}
}  // namespace

ByteSpan mdl_section(ByteSpan mdl, int index) {
  std::array<u32, 7> header{};
  if (index < 0 || index > 5 || !mdl_header(mdl, header) || header[static_cast<size_t>(index)] == 0) return {};
  u32 end = header[6];
  for (size_t j = static_cast<size_t>(index) + 1; j < 6; ++j)
    if (header[j] != 0) {
      end = header[j];
      break;
    }
  const u32 start = header[static_cast<size_t>(index)];
  if (start > end || end > mdl.size()) return {};
  return mdl.subspan(start, end - start);
}

std::optional<Bytes> mdl_with_mesh_of(ByteSpan mdl, ByteSpan donor) {
  std::array<u32, 7> header{}, donor_header{};
  if (!mdl_header(mdl, header) || !mdl_header(donor, donor_header)) return std::nullopt;
  const ByteSpan sk = mdl_section(mdl, 1), skd = mdl_section(donor, 1), s1d = mdl_section(donor, 0);
  if (sk.empty() || sk.size() != skd.size() || std::memcmp(sk.data(), skd.data(), sk.size()) != 0 || s1d.empty()) return std::nullopt;
  // S2 follows S1 (sections are monotonic).
  const u32 old_s1 = header[1] - header[0];
  const s64 delta = static_cast<s64>(s1d.size()) - static_cast<s64>(old_s1);
  Bytes out(mdl.begin(), mdl.begin() + 32);
  out.insert(out.end(), s1d.begin(), s1d.end());
  out.insert(out.end(), mdl.begin() + header[1], mdl.end());
  for (size_t i = 1; i < 7; ++i) {
    if (header[i] == 0) continue;
    const u32 shifted = static_cast<u32>(static_cast<s64>(header[i]) + delta);
    std::memcpy(out.data() + 4 + 4 * i, &shifted, 4);
  }
  return out;
}

std::optional<ModelAsset> load_menu_model(ByteSpan ome, ByteSpan hd_tim, std::string* err) {
  const auto cpt = parse_cpt(ome);
  if (!cpt || cpt->entries.size() < 6) {
    if (err) *err = "menu model: not a 6-entry cpt";
    return std::nullopt;
  }
  // The disc file has 5 sections (S1 at 0x1C), the HD art set's 4 (S1 at 0x18, no S5; its "EOF" word equals S4, the anim pack that
  // follows it, so the word cannot be trusted). The standard header has 6 sections (S1 at 0x20, missing sections 0, EOF = the real size).
  Bytes mdl(cpt->entries[3].data.begin(), cpt->entries[3].data.end());
  if (mdl.size() > 0x20 && (mdl[0] == 4 || mdl[0] == 5)) {
    const size_t sections = mdl[0], old_header = 4 * (sections + 2), shift = 0x20 - old_header;
    Bytes fixed(mdl.size() + shift, 0);
    auto word = [&](size_t offset) {
      u32 value;
      std::memcpy(&value, mdl.data() + offset, 4);
      return value;
    };
    auto put = [&](size_t offset, u32 value) { std::memcpy(fixed.data() + offset, &value, 4); };
    put(0, 6);
    for (size_t k = 0; k < sections; ++k)
      if (const u32 offset = word(4 + 4 * k)) put(4 + 4 * k, offset + static_cast<u32>(shift));
    put(28, static_cast<u32>(fixed.size()));
    std::copy(mdl.begin() + static_cast<std::ptrdiff_t>(old_header), mdl.end(), fixed.begin() + 0x20);
    mdl = std::move(fixed);
  }
  return load_model(mdl, cpt->entries[5].data, ByteSpan{}, err, hd_tim);
}

std::optional<AtimMain> load_atim_main(ByteSpan tex) {
  return guarded(nullptr, [&] {
    const auto atims = parse_atim_file(tex);
    const Atim& atim = atims.at(0);
    if (atim.images.empty()) fail("atim: no image");
    AtimMain result;
    result.width = atim.images[0].w;
    result.height = atim.images[0].h;
    result.indices.assign(atim.images[0].px.begin(), atim.images[0].px.end());
    result.clut.assign(atim.clut.begin(), atim.clut.begin() + atim.clut_w);
    return result;
  });
}

ByteSpan at0_pack(ByteSpan prd) { return at0_pack_or_empty(prd); }
ByteSpan ex_pack(ByteSpan ex_bin) { return ex_pack_span(ex_bin); }

std::optional<ModelAsset> load_model(ByteSpan obj, ByteSpan tex, ByteSpan at0_prd, std::string* err, ByteSpan hd_tim) {
  return guarded(err, [&] {
    Notes notes;
    ByteSpan ext;
    if (!at0_prd.empty()) {
      ext = at0_pack_or_empty(at0_prd);
      if (ext.empty()) fail("at0.prd: no type 0x19 anim pack");
    }
    TexInfo ti = build_atim_textures(tex);
    // cdrom2 menu models (menu/player/*.ome in HD art mode) carry no Atim at all: the hd.dat TIM (painted on the cdrom2 UV atlas, exactly 4x)
    // is their only texture. Stand in a 1x main texture of hd / 4 (every 4th texel) so the UVs normalise and the HD image binds.
    if (!ti.have && !hd_tim.empty()) {
      if (auto hd = load_hd_tim(hd_tim); hd && !hd->indexed && hd->width >= 4 && hd->height >= 4) {
        Image sd;
        sd.width = hd->width / 4, sd.height = hd->height / 4;
        sd.pixels.resize(static_cast<size_t>(sd.width) * sd.height * 4);
        for (u32 y = 0; y < sd.height; ++y)
          for (u32 x = 0; x < sd.width; ++x)
            std::memcpy(&sd.pixels[(static_cast<size_t>(y) * sd.width + x) * 4], &hd->pixels[(static_cast<size_t>(y * 4) * hd->width + x * 4) * 4], 4);
        ti.textures.push_back(std::move(sd));
        ti.main_count = 1;
        ti.width = ti.textures[0].width;
        ti.height = ti.textures[0].height;
        ti.have = true;
      }
    }
    return load_mdl(obj, ti, ext, "at0", false, notes, hd_tim);
  });
}

std::optional<ModelAsset> load_field_model(ByteSpan obj, ByteSpan tim, ByteSpan ex_bin, std::string* err, ByteSpan hd_tim) {
  return guarded(err, [&] {
    Notes notes;
    ByteSpan ext;
    if (!ex_bin.empty()) {
      ext = ex_pack_span(ex_bin);
      if (ext.empty()) fail("ex bin: not a 2-entry cpt");
    }
    TexInfo ti = build_tim_texture(tim);
    // A mod can replace the 1x TIM (cdrom/fieldobj/player/<n>/<n>.tim) with a 4x one (HD Boat: adk_sp, neko*, cl_bort, 512 x 512). The
    // mesh UVs are texel coordinates of the 128 x 128 atlas, so normalising them by the 512 wide image sampled its top-left quarter
    // (the Viper Manor disguise: white heads, smeared skirts). Keep every k-th texel as the 1x texture and use the big image as the HD one.
    ByteSpan hd = hd_tim;
    if (ti.have && hd_tim.empty()) {
      const Image& big = ti.textures[0];
      // fieldobj/player TIMs are 128 x 128 at 1x.
      constexpr u32 kAtlas = 128;
      if (big.width > kAtlas && big.width % kAtlas == 0 && big.height == big.width && !big.indexed) {
        const u32 step = big.width / kAtlas;
        Image sd;
        sd.width = kAtlas, sd.height = kAtlas;
        sd.pixels.resize(static_cast<size_t>(kAtlas) * kAtlas * 4);
        for (u32 y = 0; y < kAtlas; ++y)
          for (u32 x = 0; x < kAtlas; ++x)
            std::memcpy(&sd.pixels[(static_cast<size_t>(y) * kAtlas + x) * 4], &big.pixels[(static_cast<size_t>(y * step) * big.width + x * step) * 4], 4);
        ti.textures[0] = std::move(sd);
        ti.width = kAtlas, ti.height = kAtlas;
        hd = tim;
      }
    }
    ModelAsset asset = load_mdl(obj, ti, ext, "ex", true, notes, hd);
    if (!ex_bin.empty()) asset.ex_anim_seq = parse_anim_seq_table(ex_seq_span(ex_bin));
    return asset;
  });
}

AnimSeqTable parse_anim_seq_table(ByteSpan table) {
  // u32 n; u32 off[n] (relative to the table, 0 = none); entry i runs to the next higher offset. The 8-short
  // header of an entry holds the byte offsets of its sub-sequences (pc = entry + header[k]).
  AnimSeqTable out;
  if (table.size() < 4) return out;
  u32 count = 0;
  std::memcpy(&count, table.data(), 4);
  if (count == 0 || count > 256 || 4 + 4ull * count > table.size()) return out;
  std::vector<u32> off(count);
  std::memcpy(off.data(), table.data() + 4, 4ull * count);
  out.resize(count);
  for (u32 i = 0; i < count; ++i) {
    const u32 start = off[i];
    if (start == 0 || (start & 1) || start + 16 > table.size()) continue;
    u32 stop = static_cast<u32>(table.size());
    for (u32 j = 0; j < count; ++j)
      if (off[j] > start && off[j] < stop) stop = off[j];
    std::vector<s16>& seq = out[i];
    seq.resize((stop - start) / 2);
    std::memcpy(seq.data(), table.data() + start, seq.size() * 2);
    if (seq[0] < 16 || (seq[0] & 1) || static_cast<size_t>(seq[0] / 2) >= seq.size()) seq.clear();
  }
  return out;
}

AnimSeqTable ex_anim_seq(ByteSpan ex_bin) { return parse_anim_seq_table(ex_seq_span(ex_bin)); }

std::optional<ModelAsset> load_room_model(ByteSpan mdl, const Image& texture, std::string* err) {
  return guarded(err, [&] {
    Notes notes;
    TexInfo info;
    if (!texture.pixels.empty()) {
      info.textures.push_back(texture);
      info.main_count = 1;
      info.width = texture.width;
      info.height = texture.height;
      info.have = true;
    }
    return load_mdl(mdl, info, {}, "ex", true, notes);
  });
}

std::vector<Image> vram_page_textures(const Vram& vram, u16 img_x, u16 img_y, u16 clut_x, u16 clut_y, u32 rows) {
  std::vector<Image> out;
  // The model's 8bpp page (128 texels) plus the next page (page bit).
  const u32 w = 256;
  const u32 h = std::min<u32>(256, static_cast<u32>(Vram::kHeight) - std::min<u32>(img_y, Vram::kHeight - 1));
  for (u32 row = 0; row < rows; ++row) {
    const std::vector<u16> pal = vram.read_clut(clut_x, static_cast<int>(clut_y + row), 256);
    Image image = make_rgba(w, h);
    for (u32 y = 0; y < h; ++y)
      for (u32 x = 0; x < w; ++x) {
        const u32 colour = bgr555_to_rgba_stp(vram.texel(img_x * 2 + static_cast<int>(x), img_y + static_cast<int>(y), 8, pal));
        u8* dst = &image.pixels[(static_cast<size_t>(y) * w + x) * 4];
        for (int k = 0; k < 4; ++k) dst[k] = static_cast<u8>(colour >> (8 * k));
      }
    out.push_back(std::move(image));
  }
  return out;
}

std::optional<ModelAsset> load_room_model(ByteSpan mdl, const std::vector<Image>& clut_rows, std::string* err) {
  return guarded(err, [&] {
    Notes notes;
    TexInfo info;
    for (const Image& image : clut_rows)
      if (!image.pixels.empty()) info.textures.push_back(image);
    if (!info.textures.empty()) {
      info.main_count = static_cast<u32>(info.textures.size());
      info.width = info.textures[0].width;
      info.height = info.textures[0].height;
      info.have = true;
    }
    return load_mdl(mdl, info, {}, "ex", true, notes);
  });
}

std::optional<std::vector<AnimationClip>> load_ex_clips(ByteSpan obj, ByteSpan ex_bin, std::string* err) {
  return guarded(err, [&] {
    // Same HD trailing-block trim as load_mdl.
    if (obj.size() > 32) {
      u32 eof = 0;
      std::memcpy(&eof, obj.data() + 28, 4);
      if (eof >= 32 && eof < obj.size()) obj = obj.first(eof);
    }
    SpanReader reader(obj);
    if (reader.at<u32>(0) != 6) failf("header count {} != 6", reader.at<u32>(0));
    const u32 s2 = reader.at<u32>(8), s3 = reader.at<u32>(12);
    if (!(s2 < s3 && s3 <= obj.size())) fail("skeleton section pointers");
    const auto raw = parse_skeleton(reader, s2, s3);
    const ByteSpan ext = ex_pack_span(ex_bin);
    if (ext.empty()) fail("ex bin: not a 2-entry cpt");
    const auto raws = parse_anim_pack(ext, 0);
    std::vector<AnimationClip> out;
    for (size_t i = 0; i < raws.size(); ++i) {
      if (raws[i].joints != raw.size()) failf("ex anim J={} != model J={}", raws[i].joints, raw.size());
      out.push_back(make_clip(raws[i], raw, "ex", static_cast<u32>(i)));
    }
    return out;
  });
}

std::optional<Skeleton> load_mdl_skeleton(ByteSpan mdl, std::string* err) {
  return guarded(err, [&] {
    SpanReader reader(mdl);
    if (mdl.size() < 32 || reader.at<u32>(0) != 6) fail("not a 6-section Mdl");
    const u32 s2 = reader.at<u32>(8), s3 = reader.at<u32>(12);
    if (!(s2 < s3 && s3 <= mdl.size())) fail("skeleton section pointers");
    const auto raw = parse_skeleton(reader, s2, s3);
    Skeleton skeleton;
    skeleton.joints.resize(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
      Joint& joint = skeleton.joints[i];
      joint.parent = raw[i].parent;
      joint.rest_rot_deg = {static_cast<float>(raw[i].rot[0] * kAngleUnit), static_cast<float>(raw[i].rot[1] * kAngleUnit),
                            static_cast<float>(raw[i].rot[2] * kAngleUnit)};
      joint.rest_quat = quat_zyx(joint.rest_rot_deg);
      joint.rest_trans = {static_cast<float>(raw[i].trans[0]), static_cast<float>(raw[i].trans[1]), static_cast<float>(raw[i].trans[2])};
      joint.bone = raw[i].bone_ref == 0xFFFFFFFFu ? -1 : static_cast<int>(raw[i].bone_ref >> 16);
    }
    return skeleton;
  });
}

std::optional<std::vector<AnimationClip>> load_mdl_clips(ByteSpan mdl, ByteSpan pack, const char* source, std::string* err) {
  return guarded(err, [&] {
    // Same HD trailing-block trim as load_mdl.
    if (mdl.size() > 32) {
      u32 eof = 0;
      std::memcpy(&eof, mdl.data() + 28, 4);
      if (eof >= 32 && eof < mdl.size()) mdl = mdl.first(eof);
    }
    SpanReader reader(mdl);
    if (reader.at<u32>(0) != 6) failf("header count {} != 6", reader.at<u32>(0));
    const u32 s2 = reader.at<u32>(8), s3 = reader.at<u32>(12);
    if (!(s2 < s3 && s3 <= mdl.size())) fail("skeleton section pointers");
    const auto raw = parse_skeleton(reader, s2, s3);
    const auto cpt = parse_cpt(pack);
    if (!cpt) fail("anim pack: not a cpt");
    const u32 joints = static_cast<u32>(raw.size());
    std::vector<AnimationClip> out;
    bool any_fits = false;
    for (size_t i = 0; i < cpt->entries.size(); ++i) {
      const ByteSpan data = cpt->entries[i].data;
      if (anim_fits(data, joints)) {
        out.push_back(make_clip(parse_anim(data, joints), raw, source ? source : "pack", static_cast<u32>(i)));
        any_fits = true;
      } else {
        AnimationClip slot;
        slot.index = static_cast<u32>(i);
        out.push_back(std::move(slot));
      }
    }
    if (!any_fits) failf("no pack anim fits the model's {} joints", joints);
    return out;
  });
}

std::optional<Mesh> load_kmd(ByteSpan data, std::string* err, float tex_w, float tex_h) {
  return guarded(err, [&] { return parse_mesh(data, tex_w, tex_h); });
}

std::optional<WeaponPack> load_weapon_pack(ByteSpan data, std::string* err) {
  return guarded(err, [&] {
    auto cpt = parse_cpt(data);
    if (!cpt) fail("weapon pack: not a cpt");
    WeaponPack pack;
    Image tex;
    for (const auto& entry : cpt->entries) {
      if (entry.size <= 4) continue;
      if (SpanReader(entry.data).at<u32>(0) == 0x10) {
        tex = tim_rgba(entry.data);
      } else {
        WeaponPart part;
        part.texture = tex;
        part.mesh = parse_mesh(entry.data, tex.width ? static_cast<float>(tex.width) : 256.f,
                               tex.height ? static_cast<float>(tex.height) : 256.f);
        pack.parts.push_back(std::move(part));
      }
    }
    return pack;
  });
}

std::optional<AnimPackInfo> inspect_anim_pack(ByteSpan pack, std::string* err, u32 joints) {
  return guarded(err, [&] {
    AnimPackInfo info;
    const auto raws = parse_anim_pack(pack, joints);
    info.clips = static_cast<u32>(raws.size());
    for (const auto& anim : raws) {
      if (info.joints && anim.joints != info.joints) failf("anim pack joint counts differ ({} vs {})", info.joints, anim.joints);
      info.joints = anim.joints;
      info.frames += anim.frames;
    }
    return info;
  });
}

ModelValidation validate_model(ByteSpan obj, ByteSpan tex, ByteSpan ext_anims, bool field) {
  ModelValidation validation;
  std::string err;
  auto run = guarded(&err, [&] {
    Notes notes;
    ByteSpan ext;
    if (!ext_anims.empty()) {
      ext = field ? ex_pack_span(ext_anims) : at0_pack_or_empty(ext_anims);
      if (ext.empty()) fail(field ? "ex bin: not a 2-entry cpt" : "at0.prd: no type 0x19 anim pack");
    }
    const TexInfo ti = field ? build_tim_texture(tex) : build_atim_textures(tex);
    if (!ti.have) notes.warnings.push_back("no texture sibling");
    ModelAsset asset = load_mdl(obj, ti, ext, field ? "ex" : "at0", field, notes);
    validation.joints = static_cast<u32>(asset.skeleton.joints.size());
    validation.constructs = static_cast<u32>(asset.meshes.size());
    validation.vertices = notes.vertices;
    validation.faces = notes.faces;
    validation.clips = static_cast<u32>(asset.clips.size());
    // Sanity on the converted data.
    for (const auto& clip : asset.clips)
      if (clip.tracks.size() != asset.skeleton.joints.size()) fail("clip track count != joint count");
    for (const auto& mesh : asset.meshes)
      for (u32 i : mesh.indices)
        if (i >= mesh.vertices.size()) fail("index out of range");
    // dedupe repeated UV errors
    std::sort(notes.errors.begin(), notes.errors.end());
    notes.errors.erase(std::unique(notes.errors.begin(), notes.errors.end()), notes.errors.end());
    for (auto& error : notes.errors) validation.errors.push_back(std::move(error));
    for (auto& warning : notes.warnings) validation.warnings.push_back(std::move(warning));
    return 0;
  });
  if (!run) validation.errors.push_back(err);
  return validation;
}

}  // namespace oc::import
