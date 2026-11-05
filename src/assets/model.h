#pragma once
// Engine-side description of one character / monster / field-object model. Plain float data:
// no PS1 types, no VRAM. Produced by import::load_model() / load_field_model(); consumed by the render layer.
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "core/types.h"
#include "import/tim.h"  // import::Image only (plain struct)

namespace oc::assets {

// Keyframe rate used to convert animation frame numbers to seconds: the engine advances
// 0x40 of 0x100 per keyframe per 60 Hz tick = 0.25 keyframe/tick = 15 keyframes/s. Playback (Actor) works in keyframes
// and does not use this; it only fixes the asset's `time`/`duration` seconds.
constexpr float kModelAnimFps = 15.0f;

using Vec3 = std::array<float, 3>;
// Row-major 3x3 matrix.
using Mat3 = std::array<float, 9>;

// R = Rz(rz) * Ry(ry) * Rx(rx), angles in degrees (x applied first, right-handed, +y down).
inline Mat3 rotation_zyx(const Vec3& deg) {
  constexpr double kRad = 3.14159265358979323846 / 180.0;
  const double cx = std::cos(deg[0] * kRad), sx = std::sin(deg[0] * kRad);
  const double cy = std::cos(deg[1] * kRad), sy = std::sin(deg[1] * kRad);
  const double cz = std::cos(deg[2] * kRad), sz = std::sin(deg[2] * kRad);
  return {static_cast<float>(cz * cy), static_cast<float>(cz * sy * sx - sz * cx), static_cast<float>(cz * sy * cx + sz * sx),
          static_cast<float>(sz * cy), static_cast<float>(sz * sy * sx + cz * cx), static_cast<float>(sz * sy * cx - cz * sx),
          static_cast<float>(-sy),     static_cast<float>(cy * sx),                 static_cast<float>(cy * cx)};
}

// Unit quaternion (x, y, z, w) of rotation_zyx(deg).
inline std::array<float, 4> quat_zyx(const Vec3& deg) {
  const Mat3 rot = rotation_zyx(deg);
  const double trace = static_cast<double>(rot[0]) + rot[4] + rot[8];
  double x, y, z, w;
  if (trace > 0) {
    const double scale = std::sqrt(trace + 1.0) * 2;
    w = 0.25 * scale;
    x = (rot[7] - rot[5]) / scale;
    y = (rot[2] - rot[6]) / scale;
    z = (rot[3] - rot[1]) / scale;
  } else if (rot[0] > rot[4] && rot[0] > rot[8]) {
    const double scale = std::sqrt(1.0 + rot[0] - rot[4] - rot[8]) * 2;
    w = (rot[7] - rot[5]) / scale;
    x = 0.25 * scale;
    y = (rot[1] + rot[3]) / scale;
    z = (rot[2] + rot[6]) / scale;
  } else if (rot[4] > rot[8]) {
    const double scale = std::sqrt(1.0 + rot[4] - rot[0] - rot[8]) * 2;
    w = (rot[2] - rot[6]) / scale;
    x = (rot[1] + rot[3]) / scale;
    y = 0.25 * scale;
    z = (rot[5] + rot[7]) / scale;
  } else {
    const double scale = std::sqrt(1.0 + rot[8] - rot[0] - rot[4]) * 2;
    w = (rot[3] - rot[1]) / scale;
    x = (rot[2] + rot[6]) / scale;
    y = (rot[5] + rot[7]) / scale;
    z = 0.25 * scale;
  }
  return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w)};
}

// parent is always < the joint's own index; rest_rot_deg is (rx, ry, rz) degrees, composed as Rz*Ry*Rx; rest_trans is in model units,
// relative to the parent; bone is the "real bone" number (boneRef >> 16) or -1 for helper joints.
struct Joint {
  int parent = -1;
  Vec3 rest_rot_deg{};
  std::array<float, 4> rest_quat{0, 0, 0, 1};
  Vec3 rest_trans{};
  int bone = -1;
};

struct Skeleton {
  std::vector<Joint> joints;
};

// One skinned vertex of the shared pool of a mesh. Position = sum(weight[i] * M(joint[i]) * local[i]).
// source_index = global vertex number in the file; bind_pos = rest-pose model-space position; weight is normalised (sum 1); local =
// position relative to each influencing joint; normal_joint indexes `joint`.
struct SkinVertex {
  u32 source_index = 0;
  Vec3 bind_pos{};
  std::array<u16, 2> joint{0, 0};
  std::array<float, 2> weight{1, 0};
  std::array<Vec3, 2> local{};
  // The normal record the PS1 skinner rotates with the joint matrix: the vertex's own record for an 8-byte
  // vertex (joint[0]), the record of rec0 (relative to J2 = joint[1]) for a 16-byte vertex. s8 per axis, the skinner uses it as n << 5.
  std::array<s8, 3> normal{0, 0, 0};
  u8 normal_joint = 0;
};

// A renderable corner: a pool vertex plus the texture coordinate of the face that uses it. pool indexes Mesh::pool; uv is 0..1,
// origin top-left, v down. rgb = the corner's packet colour (0x80 neutral): the textured Gouraud faces of the weapon meshes carry one per
// corner and the GPU multiplies the texel by it.
struct MeshVertex {
  u32 pool = 0;
  std::array<float, 2> uv{};
  std::array<u8, 3> rgb{128, 128, 128};
};

// texture indexes ModelAsset::textures (-1 = flat colour); color is for flat-shaded faces.
struct Material {
  int texture = -1;
  std::array<u8, 3> color{255, 255, 255};
  bool operator==(const Material&) const = default;
};

// material indexes Mesh::materials.
struct SubMesh {
  u32 material = 0;
  u32 first_index = 0, index_count = 0;
};

// indices are triangles (quads split as v0,v1,v2 / v2,v1,v3), wound like the source; skinned is false for rigid meshes (weapons),
// which use the pool bind_pos only; trans is 0 solid, 1..4 semi-transparent with PS1 ABR 0..3 (+1), 0xFF invisible (set by the
// room-model loader from S3 shading); quad_count is a statistic.
struct Mesh {
  std::vector<SkinVertex> pool;
  std::vector<MeshVertex> vertices;
  std::vector<u32> indices;
  // Per triangle (indices / 3): 0 = a source triangle, 1 / 2 = the first (v0,v1,v2) / second (v2,v1,v3) half of a source quad (the two
  // halves are consecutive). The PS1 culls a quad as a whole.
  std::vector<u8> tri_quad;
  std::vector<Material> materials;
  std::vector<SubMesh> submeshes;
  bool skinned = true;
  u8 trans = 0;
  u32 quad_count = 0;
};

// time is in seconds; v is an absolute Euler rotation in degrees (rest + delta) or an absolute translation in units.
struct Key3 {
  float time = 0;
  Vec3 v{};
};

// rot holds 1 key when the joint is not animated by the clip, else frame_count keys.
struct JointTrack {
  std::vector<Key3> rot;
  std::vector<Key3> trans;
};

// name is e.g. "s4_03", "at0_12", "ex_01"; source is "s4", "at0" or "ex"; index is within its source pack; duration is in seconds
// (frame_count / kModelAnimFps); tracks has one per skeleton joint; rot_animated / trans_animated are the file flags per joint.
struct AnimationClip {
  std::string name;
  std::string source;
  u32 index = 0;
  u32 frame_count = 0;
  float duration = 0;
  std::vector<JointTrack> tracks;
  std::vector<u8> rot_animated, trans_animated;
};

// pos is in the joint's space; unused slots are all zero in the file.
struct AttachPoint {
  Vec3 pos{};
  u16 joint = 0;
  bool used = false;
};

// flags: the high nybble is the env-map mode, the low nybble the transparency mode.
struct ShadingEntry {
  u8 r = 128, g = 128, b = 128, flags = 0;
};

// The two attached-object mount records of a model (16 bytes each). The weapon renderer draws slot k at
// JointMatrix(points[attach].joint) * [RotMatrixZYX(rot) | points[attach].pos + pos] (the offset is in the joint's space, not turned
// by rot). Slot 0 = the weapon (Serge: attach 18 + (0, 0, -96); Kid: attach 7, the hand,
// turned (3072, 3072, 4064); Guile / Doc / Fargo / Glenn / Pierre ...: attach 31 = a dedicated weapon bone the tech clips animate), slot 1 =
// a second object (Kid's other hand). All-zero records (Viper, Zoah, Poshul ...) = no mount: the character has no separate weapon object.
// rot: 4096 = a turn, per axis (RotMatrixZYX order); pos: model units in the joint's space (PS1 y down); attach: attach point index;
// used: any field non-zero.
struct MountPoint {
  Vec3 rot{};
  Vec3 pos{};
  u16 attach = 0;
  bool used = false;
};

struct AttachPoints {
  std::array<AttachPoint, 32> points{};
  std::array<MountPoint, 2> mounts{};
};

// Animation-sequence ("anmseq") table of a field model: Mdl S5 for script animation ids 0..0x7F, the second
// entry of a fieldobj/ex/*.bin for ids 0x80+. One entry per id: the raw halfwords of the entry, starting with its 8-short header
// (header[k] = byte offset of sub-sequence k from the entry start; the field always runs sub-sequence 0). Empty entry = none.
using AnimSeqTable = std::vector<std::vector<s16>>;

struct ModelAsset {
  // "battle" or "field".
  std::string kind;
  // Texture dump / replace name (render/texture_registry.h model_texture_key): the member stem ("battle/player/selju/selju",
  // "fieldobj/player/cl_bort/cl_bort"), a weapon pack part ("battle/wepon/sword/part3") or "@room_model" for models whose texture is cut
  // from a VRAM page. Empty = not dumped.
  std::string source;
  // One mesh per construct.
  std::vector<Mesh> meshes;
  Skeleton skeleton;
  std::vector<AnimationClip> clips;
  AttachPoints attach;
  std::array<ShadingEntry, 16> shading{};
  // Textures as RGBA8. One per CLUT row of the main image (alternate palettes), then the overlay strips of an
  // .ltc. Multi-image atims are stacked vertically into one image.
  std::vector<import::Image> textures;
  // main_texture_count: the CLUT-row textures (the rest are overlays). drp_entries: S6 entries (not decoded).
  u32 main_texture_count = 0;
  u32 drp_entries = 0;
  // The two words after the construct table = s16 extent x, y, z (+ pad), used for the model's effect scales. Zero when absent.
  std::array<s16, 3> extent{0, 0, 0};
  // anim_seq: S5 (empty for room models, S5 = 0), the per-id animation scripts. ex_anim_seq: the ex pack's table (load_field_model
  // with an ex .bin), ids 0x80 + k.
  AnimSeqTable anim_seq;
  AnimSeqTable ex_anim_seq;
  // The eye-blink regions of a party member: two 32 x 32 texel rects of the MAIN texture (1x pixels,
  // x even: S5 entry 0 +0x80 = u8 x1, y1, x2, y2), replaced by the left / right halves of frame f (1, 2) of the .ltc strip (textures[main_texture_count],
  // 128 x 32 = two 64-wide frames, each a left and a right 32 x 32 half). Frame 0 = the main texture itself. `valid` only for models with a strip.
  struct EyeSwap {
    bool valid = false;
    std::array<u16, 2> x{}, y{};
  } eye;
  // Optional RDE HD override, parallel to `textures` (empty vector = none; an entry with width 0 = no HD for that
  // texture). Same UV space: texel (u,v) of textures[i] is texel (u*hd_scale, v*hd_scale) of hd_textures[i].
  // Only textures[0] gets an HD image (the HD TIM carries its own single CLUT), plus the eye strip of a battle player drawn on its
  // Status-screen TIM (import::apply_battle_hd_tim).
  std::vector<import::Image> hd_textures;
  u32 hd_scale = 4;
  // A replacement image for textures[i] (empty vector / width 0 = none), any size, STP kept: a texture mod's (render::resolve_model_textures)
  // or, on an effect pack's copy of a battle unit, the unit's drawn art cut into the page. Drawn in place of textures[i] AND hd_textures[i].
  std::vector<import::Image> mod_textures;

  // Pixels of the main texture `base` (RGBA8, w x h; the 1x image, the 4x one or a replacement of any size) with frame `frame` (1 or 2)
  // of the eye strip pasted over the two eye rects (nearest). `strip` = a replacement of the strip (any size) instead of the model's own.
  // Empty when the model has no eye swap or no strip.
  std::vector<u8> eye_frame_pixels(const std::vector<u8>& base, u32 w, u32 h, int frame, const import::Image* strip = nullptr) const {
    std::vector<u8> out;
    if (!eye.valid || frame < 1 || frame > 2 || main_texture_count >= textures.size() || textures.empty()) return out;
    const import::Image& one_strip = textures[main_texture_count];
    const import::Image& one = textures[0];
    if (!strip) strip = &one_strip;
    if (one_strip.width < 128 || one_strip.height < 32 || strip->indexed || !strip->width || !strip->height ||
        strip->pixels.size() < size_t(strip->width) * strip->height * 4 || one.width == 0 || one.height == 0 || base.size() < size_t(w) * h * 4)
      return out;
    // `base` texels per 1x texel, and strip texels per 1x strip texel.
    const double scale_x = double(w) / one.width, scale_y = double(h) / one.height;
    const double strip_x = double(strip->width) / one_strip.width, strip_y = double(strip->height) / one_strip.height;
    out = base;
    for (size_t half = 0; half < 2; ++half) {
      const u32 x0 = u32(eye.x[half] * scale_x), x1 = std::min(w, u32((eye.x[half] + 32) * scale_x));
      const u32 y0 = u32(eye.y[half] * scale_y), y1 = std::min(h, u32((eye.y[half] + 32) * scale_y));
      for (u32 py = y0; py < y1; ++py)
        for (u32 px = x0; px < x1; ++px) {
          const double u = (frame - 1) * 64 + double(half) * 32 + ((px + 0.5) / scale_x - eye.x[half]);
          const double v = (py + 0.5) / scale_y - eye.y[half];
          const u32 sx = std::min(strip->width - 1, u32(u * strip_x)), sy = std::min(strip->height - 1, u32(v * strip_y));
          const u8* src = &strip->pixels[(size_t(sy) * strip->width + sx) * 4];
          u8* dst = &out[(size_t(py) * w + px) * 4];
          dst[0] = src[0], dst[1] = src[1], dst[2] = src[2], dst[3] = src[3];
        }
    }
    return out;
  }
  bool has_hd(size_t i = 0) const { return i < hd_textures.size() && hd_textures[i].width != 0; }
  // Texture i for the requested quality; falls back to the original when no HD image exists. UVs are normalised, so
  // the same mesh UVs work for both.
  const import::Image& texture_for(size_t i, bool hd) const { return hd && has_hd(i) ? hd_textures[i] : textures[i]; }
  bool has_mod(size_t i) const { return i < mod_textures.size() && mod_textures[i].width != 0; }
  // The image texture i is drawn with: the mod's, else texture_for(i, hd).
  const import::Image& drawn_texture(size_t i, bool hd) const { return has_mod(i) ? mod_textures[i] : texture_for(i, hd); }
  // drawn_texture(0, hd) with eye frame `frame` pasted from the drawn strip (the mod's, else the strip's HD image when `hd`, else the
  // model's own). Empty as eye_frame_pixels.
  std::vector<u8> drawn_eye_frame(int frame, bool hd) const {
    if (textures.empty()) return {};
    const import::Image& base = drawn_texture(0, hd);
    const size_t strip = main_texture_count;
    return eye_frame_pixels(base.pixels, base.width, base.height, frame, has_mod(strip) ? &mod_textures[strip] : hd && has_hd(strip) ? &hd_textures[strip] : nullptr);
  }
};

// Weapon part: TIM texture (RGBA8) + rigid mesh.
struct WeaponPart {
  import::Image texture;
  Mesh mesh;
};
struct WeaponPack {
  std::vector<WeaponPart> parts;
};

}  // namespace oc::assets
