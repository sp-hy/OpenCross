#pragma once
// Skinned, textured, alpha-tested 3D mesh drawing with depth test + write.
//
// Skinning: each vertex has two joint-local positions, two joint indices and two weights;
// position = sum(weight[i] * palette[joint[i]] * local[i]). The palette (up to 128 mat4) lives in a per-frame uniform
// buffer together with view-projection, view and model matrices; every draw uses its own slot of that buffer.
// The fragment shader writes depth = camera z / 16 / kDepthRange so meshes sort against background
// groups, which carry their z in the same space. Texel alpha 0 (TIM colour 0) is discarded.
//
// One-sided faces: the PS1 draws a polygon only when its screen-space NCLIP is positive (corners in source order, screen y down); a
// quad when NCLIP(v0,v1,v2) > 0 or NCLIP(v1,v2,v3) <= 0. No shading flag makes a construct double-sided, and the rigid weapon meshes
// test the same way. The test runs in the vertex shader on the projected corners (a culled triangle collapses to a clipped point).
// Semi-transparent constructs (shading entry bit 1, assets::Mesh::trans): only texels with STP set blend with the construct's ABR, the
// others are opaque (import/tim.h kStpAlpha); ABR 0 = (B + F) / 2, 1 = B + F, 2 = B - F, 3 = B + F / 4.
//
// Ordering-table mode (DrawParams::ot_faces; field and every battle model): the PS1 files every
// polygon in ONE ordering-table slot, (average camera z of its corners) >> 4 (AVSZ3 / AVSZ4, ZSF3 = 0x155 then >> 2),
// or the nearest corner's z >> 4 for constructs whose shading-entry flag 0x10 is set, plus the actor's layer bias; a quad's two triangles
// share the slot of all four corners. A face is
// therefore wholly in front of or behind a background layer: no per-pixel cut through a coplanar / ground layer. The meshes are drawn
// unindexed and every vertex carries the skinning inputs of its triangle's other two corners, so the vertex shader computes the face's slot
// and its water state; the fragment depth is that slot plus a sub-slot fraction (the fragment's own z clamped into the slot), which keeps
// the model's self-occlusion within one slot and never crosses a slot boundary. Water: a face with any corner whose WORLD y
// is below the level (y > level, +y down) takes the water colour and its slot is pushed back by `water_bias`.
#include <memory>
#include <span>
#include <vector>

#include "assets/model.h"
#include "render/buffer.h"
#include "render/lights.h"
#include "render/mat4.h"
#include "render/pipeline.h"
#include "render/texture.h"

namespace oc::render {

// Gives every fully transparent texel of an RGBA8 image the mean colour of its opaque neighbours (two passes; alpha untouched),
// so bilinear filtering does not bleed the (black / white) colour of cut-out texels into the opaque edge. Shared by the
// skinned-model and battle-stage textures.
std::vector<u8> bleed_transparent(const std::vector<u8>& src, u32 w, u32 h);

// One corner of an unindexed triangle. Every vertex carries the skinning inputs of its triangle's other two corners and, for a half of a
// source quad, of the quad's fourth corner, so the vertex shader can compute per-face values: the PS1 back-face test (NCLIP, a quad
// culled as a whole), the field ordering-table slot and the water test. Corner c: 0 = this vertex, 1 / 2 = the triangle's next corners
// (k + 1, k + 2), 3 = the quad partner (the corner across the shared diagonal; unused for triangles).
struct SkinnedVertex {
  // Corner c, influence i: joint-local position.
  float pos[4][2][3];
  u16 joint[4][2];
  float weight[4][2];
  float uv[2];
  // 0 triangle, 1 / 2 = first (v0,v1,v2) / second (v2,v1,v3) half of a quad (assets::Mesh::tri_quad).
  float quad = 0;
  // k: this vertex's position in its triangle (0..2), so every corner rebuilds the triangle in source order.
  float corner = 0;
  // PS1 lit path: the VDDM normal of corner 0 as n / 128 (the skinner uses n << 5 in 4.12 = the same value) and,
  // in w, the index of the joint whose matrix rotates it (the vertex's own joint, or J2 = joint[1] for a 16-byte vertex). Zero for rigid meshes.
  float normal[4] = {0, 0, 0, 0};
  // The corner's packet colour / 128 (assets::MeshVertex::rgb; 1 = neutral), w unused.
  float rgb[4] = {1, 1, 1, 0};
  // Rigid meshes ([Enhancements] Lighting's per-pixel weapon glint): this corner's smoothed model-space normal = the area-weighted face normals
  // at its position whose faces lie within kHardEdgeDegrees of this face (sharp rims stay sharp); w = 1 when present.
  float smooth[4] = {0, 0, 0, 0};
};

// GPU copy of one assets::ModelAsset (all meshes, textures). Created by MeshPipeline::create_model; must be destroyed
// before the pipeline and only while the GPU no longer uses it.
struct MeshModel {
  struct Draw {
    u32 first_index = 0, index_count = 0;
    // Index into sets_*, -1 = flat colour.
    int texture = -1;
    // assets::Mesh::trans.
    u8 trans = 0;
    // Index of the asset mesh (= construct = S3 shading entry).
    u16 mesh = 0;
    // PS1 back-face culled: Mdl constructs and rigid weapon meshes.
    bool one_sided = true;
    float color[3] = {1, 1, 1};
  };
  Buffer vertices, indices;
  std::vector<Draw> draws;
  std::vector<std::unique_ptr<Texture>> textures;
  std::vector<TexRef> sets_nearest, sets_linear;
  // Optional RDE 4x texture per texture index (a null TexRef = none); drawn instead when DrawParams::hd is set.
  std::vector<std::unique_ptr<Texture>> hd_textures;
  std::vector<TexRef> hd_sets_nearest, hd_sets_linear;
  // Eye-blink variants (assets::ModelAsset::eye): textures / sets [eye_base + k - 1] hold texture 0 with eye frame k (1, 2) pasted
  // in (the 1x image or the replacement, and the 4x image where there is one). DrawParams::eye_frame picks them for the draws of texture 0.
  int eye_base = -1;
  MeshModel() = default;
  MeshModel(const MeshModel&) = delete;
  MeshModel& operator=(const MeshModel&) = delete;
  u32 joint_count = 0;
};

// Texture dump / replace of a model with a source (texture_registry.h: model/<member stem>/clut<row>.png | overlay<k>.png; room models
// "@room_model": render/model_tiles.h): dumps each texture's best image (the 4x one when there is one) and fills assets::ModelAsset::mod_textures
// from the active texture mods, so every user of the asset's pixels (the GPU copy, eye blinks, effect fades) draws the replacement. Call before create_model.
void resolve_model_textures(assets::ModelAsset& asset);

// The images create_model uploads, their transparent texels bled (bleed_transparent): per asset texture the drawn image and the 4x one
// (empty without), then the two eye-blink frames and their 4x ones (empty without). CPU only: any thread, after resolve_model_textures.
struct ModelUploads {
  std::vector<std::vector<u8>> main, hd, eye, hd_eye;
};
ModelUploads prepare_model_uploads(const assets::ModelAsset& asset);

class MeshPipeline {
 public:
  static constexpr u32 kMaxJoints = 128;
  // Distinct model draws (actors) per frame.
  static constexpr u32 kMaxDrawsPerFrame = 96;

  MeshPipeline() = default;
  ~MeshPipeline() { destroy(); }
  MeshPipeline(const MeshPipeline&) = delete;
  MeshPipeline& operator=(const MeshPipeline&) = delete;

  bool init(GpuContext& ctx);
  void destroy();

  // `uploads`: prepare_model_uploads(asset) made ahead (on a worker); null = made here.
  std::unique_ptr<MeshModel> create_model(const assets::ModelAsset& asset, const ModelUploads* uploads = nullptr);

  // Binds the pipeline and restricts drawing to a scissor rectangle (the 4:3 game area); end() restores the full window.
  void begin(const Frame& frame, Rect scissor);
  struct DrawParams {
    Mat4 view_proj, view, model;
    // Per-part state of battle animation: the shading entry's rgb (0x80 = neutral) multiplies the part's colour and its flag
    // byte bit 1 / bits 2-3 make the part semi-transparent with ABR mode - 1. part_mask bit k = the rgb of part k is in force.
    u32 part_mask = 0, part_semi = 0;
    float part_tint[32][3] = {};
    int part_abr[32] = {};
    // Joint world matrices; the vertices carry joint-local positions.
    std::span<const Mat4> palette;
    // Bilinear instead of nearest texture sampling.
    bool linear = false;
    // Use the 4x texture where the model has one.
    bool hd = false;
    // The model matrix has a negated axis (a reflected copy): the back-face test is inverted (`NCLIP < 0`).
    bool mirror = false;
    // Party eye blink: 0 open (the model's texture), 1 / 2 = the .ltc strip frame pasted over the eye rects (model.eye).
    int eye_frame = 0;
    // Flat light colour multiplier: room light zone (RoomLighting) x entity RGB, value / 128.
    float tint[3] = {1, 1, 1};
    // Water: fragments whose model-space y (+y down, feet = 0) exceeds water_level use water_tint
    // (RGB / 128) instead of `tint`. water_level >= 1e8 = dry.
    // In ot_faces mode the level is a WORLD y (the PS1 compares the world-space vertex buffer).
    float water_level = 1e9f;
    float water_tint[3] = {0.25f, 0.25f, 1.4f};
    // Field ordering-table mode (see the file comment). ot_bias = the actor's layer bias in slots; water_bias = the slot push of submerged
    // faces (layer 0's z at room load). Submerged faces are drawn after the rest: first depth only, then added to the
    // frame (PS1: drawn first, far back, and the semi-transparent water layer added over them; the sum is the same).
    bool ot_faces = false;
    float ot_bias = 0, water_bias = 0;
    // bit k: mesh k not drawn (shading entry k flag bit 0).
    u32 hidden_meshes = 0;
    // bit k: mesh k sorts by its nearest corner (shading entry k flag 0x10); ot_faces only.
    u32 near_meshes = 0;
    // bit k: mesh k is env-mapped with its own texture (shading entry flag 0x80 with 0x20 / 0x40):
    // every corner's UV comes from its skinned normal bytes, u = (nx >> 1) + 0x40, v = (ny >> 1) + 0xC0, instead of the face's own UV.
    u32 env_meshes = 0;
    // When set, the env meshes sample this texture (a 256 x 256 window of the effect VRAM: kamei's env page override) instead of their own.
    TexRef env_set;
    // STOCK env page (shading flag 0x20 / 0x40 WITHOUT 0x80; UV = (nx >> 1) + 0x40 / (ny >> 1) + 0xC0 unshifted): stock_env_set = the stage's page (320, 256) through CLUT row (0, 239), 256 x 256. Bit k of stock_env_replace: mesh k
    // (flag 0x20 only) is drawn with that page and those UVs instead of its own texture; bit k of stock_env_over (flag 0x40): mesh k also gets a second,
    // semi-transparent primitive over its model primitive, ABR = stock_env_abr[k] (flag bits 2-3).
    TexRef stock_env_set;
    u32 stock_env_replace = 0, stock_env_over = 0;
    u8 stock_env_abr[32] = {};
    // GTE depth cue (far colour + near/far fog distances): the light multiplier is mixed toward `fog_color` (far colour / 128,
    // 1 = neutral) by p(z) = fog_far * (z - fog_near) / ((fog_far - fog_near) * z), z = view depth in world units. fog_far <= fog_near = off.
    float fog_color[3] = {1, 1, 1};
    float fog_near = 0, fog_far = 0;
    // Submerged faces in ot_faces mode with water: Mixed = the dry faces, then the submerged ones added over the
    // frame (an approximation that is exact only under an additive water layer with nothing behind it); DryOnly / WetOnly split the model so
    // the caller can draw the submerged faces BEFORE the background layers (opaque, depth written: farther layers are then hidden by them and
    // the semi-transparent water layer blends over them with its own ABR - the PS1 order) and the dry faces after the layers as usual.
    enum class WetFaces : u8 { Mixed, DryOnly, WetOnly };
    WetFaces wet_faces = WetFaces::Mixed;
    // Whole-model semi-transparency (ABR = n - 1): every
    // construct is drawn with the semi-transparency bit and THIS ABR (it replaces the constructs' own); as always on the PS1 only the texels
    // with STP set blend, the others stay opaque. -1 = off (each construct keeps its own shading-entry blend).
    int model_abr = -1;
    // Flash draw: every polygon twice,
    // both semi-transparent with the usual light colour: first with the white CLUT (0x7FFF ORed into every non-zero entry, STP
    // kept) and ABR 2 (B - F), then with the model's own CLUT and ABR 1 (B + F). Texels without STP are opaque in both, so they show the
    // normal colour; STP texels end as B - light + texel * light, of the nearest blended surface only (a depth pass comes first).
    bool flash = false;
    // Ordering-table slab (ot_faces only; the 8F screen ripple's split at its OT index, see render_target.h build_screen_warp): a face is drawn only
    // when slot_min < its slot (face slot + ot_bias, + water_bias when submerged) <= slot_max.
    float slot_min = -1e9f, slot_max = 1e9f;
    // PS1 lit model path: a model flagged for lighting is skinned with world-space normals and its faces are Gouraud
    // triangles (GT3) whose corner colours come from the GTE NCCT: I_i = max(0, LLM_i . N), c = max(0, BK + LCM . I), colour = min(RGBC * c, 255)
    // (no depth cue on this path). `dir[i]` = light matrix row i in THIS world space = normalise(light_i - model) * k_i / 4096 (k_i = intensity -
    // distance * falloff >> 12 clamped to 0..0x7FFF), `rgb[i]` = colour matrix column i = light colour bytes / 256, `bk` = back
    // colour bytes / 256. With `on` false the flat path below is untouched.
    struct Light {
      bool on = false;
      float bk[3] = {0, 0, 0};
      float dir[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
      float rgb[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
      // Weapon specular (evaluated per corner): the light
      // direction `spec_dir` (unit vector from the unit toward the stage light) reflected about
      // the face normal, dotted with the unit vector from the vertex to the eye, ^9, x 256, clamped 255, ADDED to the corner
      // colour byte. World space (the engine's y-up world), face normal from the triangle.
      bool spec = false;
      float spec_dir[3] = {0, 1, 0};
      float spec_eye[3] = {0, 0, 0};
    };
    Light light;
    // [Enhancements] SpellLights: point lights (render world) added to this model's colour multiplier; null / empty = none (the PS1 look).
    const LightSet* lights = nullptr;
    // Every visible face as an opaque, untextured, unlit polygon of `solid_rgb` (a flat shadow of the model).
    bool solid = false;
    float solid_rgb[3] = {0, 0, 0};
  };
  void draw(const MeshModel& model, const DrawParams& params);
  void end();

  // [Enhancements] SmoothWaterLine: in field ordering-table mode a face straddling the water level is split per fragment at the exact
  // plane (default off = the PS1's whole-face test).
  void set_smooth_water_line(bool on) { smooth_water_line_ = on; }
  // [Enhancements] EnhancedLighting: a soft key-light shading from the vertex normals, added to the PS1 colour multiplier (field and battle models).
  // Default off = the PS1 look.
  void set_enhanced_lighting(bool on) { enhanced_ = on; }
  // [Enhancements] SmoothReflections: the env-mapped parts' corner UVs (mesh.vert) keep the sub-texel fraction of the normal
  // instead of the PS1's whole texel, so the reflection slides with the pose. Default off = the PS1's one-texel steps.
  void set_smooth_reflections(bool on) { smooth_reflections_ = on; }

 private:
  GpuContext* ctx_ = nullptr;
  SDL_GPUShader* vs_ = nullptr;
  SDL_GPUShader* fs_ = nullptr;
  // Solid; semi-transparent additive / subtractive (depth tested, not written).
  Pipeline pipeline_, pipeline_add_, pipeline_sub_;
  // Semi-transparent ABR 0, (B + F) / 2: alpha blend at 0.5 (depth tested, not written).
  Pipeline pipeline_avg_;
  // Depth written, colour kept (alpha-0 output): the submerged faces' first pass.
  Pipeline pipeline_depth_only_;
  Sampler nearest_, linear_;
  bool smooth_water_line_ = false;
  bool enhanced_ = false;
  bool smooth_reflections_ = false;
  // 1 PS1 back-face test, 0 off, -1 inverted (debug: env OC_MESH_CULL=0 / -1).
  float cull_ = 1.0f;
  // One FrameUniforms per draw, read by the vertex shader as a storage buffer.
  StreamBuffer slots_;
  void* slots_memory_ = nullptr;
  bool active_ = false;
  u32 slot_ = 0;
  // Frame::serial of the frame slot_ counts in.
  u64 serial_ = 0;
  Extent extent_{};
};

}  // namespace oc::render
