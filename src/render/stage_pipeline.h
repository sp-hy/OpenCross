#pragma once
// Battle stage drawing: the packet meshes of assets::StageAsset drawn as instances placed by the stage's object scripts
// (the PS1 draws every stage mesh through a type 0x0A object).
//
// Per instance (StageInstance): camera-space transform of the mesh's PS1 file space, object colour (modulates every
// corner, 0x80 = neutral), blend mode (op 06 bits 0-2, replaces the packet tpage ABR), OT bias (sort depth offset of
// bias * 16 world units, or the absolute slot with the fixed-slot flag), cull side (ext bits 4 / 5) and the op-36
// texture-window scroll. Faces are single-sided: the GPU draws a packet only when NCLIP(0,1,2) > 0; the
// importer keeps the packet corner order and the pipelines cull the other side.
// The fragment shader writes depth = ((average camera z of the packet) / 16 + bias) / kDepthRange: one ordering-table slot
// per packet like the PS1 (StageVertex::centre), so coplanar floors with different biases resolve per packet, and stage
// geometry still sorts against the skinned actors (MeshPipeline writes camera z / 16 / kDepthRange per fragment).
// Texel alpha 0 (TIM colour 0) is discarded; vertex colour 1.0 = PS1 0x80 (neutral), so out = texel * colour * object.
// Sampling: nearest in original art mode, bilinear in HD mode (RDE samples every PS1 texture page linearly in HD mode;
// the atlases get their transparent texels bled so the filter does not darken cut-out edges).
// draw_dynamic(): per-frame geometry in the same vertex format (the unit drop shadows: Gouraud
// hulls drawn subtractively), streamed through a host-visible ring buffer.
#include <array>
#include <bitset>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "assets/battle_stage.h"
#include "render/buffer.h"
#include "render/camera.h"
#include "render/lights.h"
#include "render/mat4.h"
#include "render/pipeline.h"
#include "render/texture.h"

namespace oc::render {

// [Enhancements] EdgeBlend: the host sets it; StagePipeline::draw() / draw_cpu() draw the parts' seam
// strips of the OPAQUE instances right after their faces: the packet again with the neighbour's texture mirrored across the shared edge, alpha 1/2 on the edge
// fading to 0 over the band (stage.frag, params.y + 16), through the per-frame ring, so the strips follow the packets kept this frame, the ground cells' depth
// and hiding, warps and morphs. Every baked part has seams (any art mode).
inline bool g_edge_blend = false;
inline void set_edge_blend(bool on) { g_edge_blend = on; }
inline bool edge_blend() { return g_edge_blend; }

// GPU copy of one StageAsset. Destroy before the pipeline and only while the GPU no longer uses it.
struct StageMesh {
  struct Sub {
    u32 first_index = 0, index_count = 0;
    // Index into sets (always valid: untextured faces use the white texture).
    int texture = -1;
    // Packet code bit 1.
    bool semi = false;
    // Tpage blend of the faces.
    int abr = 0;
    // The packets (StageMesh::packets) whose indices this run covers.
    u32 first_packet = 0, packet_count = 0;
  };
  struct Part {
    u32 first_sub = 0, sub_count = 0;
    // The part's range of the vertex buffer (StageAsset::parts[i].vertices).
    u32 first_vertex = 0, vertex_count = 0;
    // drp type 0x12 'ring' (StageInstance::ground).
    bool ground = false;
  };
  // One GPU packet of the source mesh (a triangle, or a quad = two triangles (0,1,2) (2,1,3)): the PS1 builders reject or draw it as a
  // whole, so the CPU keeps the corner list to test it per frame.
  struct Packet {
    // Into cpu_indices; 3 (triangle) or 6 (quad) entries.
    u32 first_index = 0;
    // Global vertex indices of the corners (cpu_pos); the 4th is unused for a triangle.
    u32 corner[4] = {};
    u8 corners = 3;
    // Ground parts: the quad's grid cell (cx - 10 * cz of the corner average), -1 = a rim triangle.
    s8 cell = -1;
    // Ground parts: an untextured packet (F3 / G3), which the ground object does not copy.
    bool ground_drop = false;
  };
  Buffer vertices, indices;
  std::vector<Packet> packets;
  // The index buffer content (global vertex indices).
  std::vector<u32> cpu_indices;
  // Vertex positions in PS1 file space (x, y down, z), per global vertex.
  std::vector<std::array<float, 3>> cpu_pos;
  std::vector<Sub> subs;
  // parts[i] = StageAsset::parts[i].
  std::vector<Part> parts;
  std::vector<std::unique_ptr<Texture>> textures;
  // One per texture (nearest sampler), the last one is the 1x1 white texture.
  std::vector<TexRef> sets;
  // The same textures with the bilinear sampler (HD art mode).
  std::vector<TexRef> sets_linear;
  std::unique_ptr<Texture> white;
  StageMesh() = default;
  StageMesh(const StageMesh&) = delete;
  StageMesh& operator=(const StageMesh&) = delete;
};

// One placed mesh for a frame. `cam` is the row-major 3x4 transform from the mesh's PS1 file space (x, y down, z) to
// camera space (x right, y down, z forward): cam_pos = M * v + t with M = cam[0..8], t = cam[9..11].
struct StageInstance {
  int part = -1;
  std::array<float, 12> cam{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
  std::array<u8, 3> rgb{128, 128, 128};
  // op 06 bits 0-2: 0 opaque, 1 B/2+F/2, 2 B+F, 3 B-F, 4 B+F/4, 5-7 add (+ a subtract pass not drawn).
  int blend = 0;
  // Extended object (op 35 flag bit 29): uses the extended packet builders; a scrolling (op 36) blend-2 object adds a quarter.
  bool ext = false;
  // ... while a morphing (op 37) ext object honours its own blend word (cfis blend 0 solid, afi1 blend 2 additive).
  bool ext_morph = false;
  // OT bias (slot units of 16 world z units).
  int bias = 0;
  // Flag bit 23: depth = bias alone.
  bool fixed_slot = false;
  // Object flag bit 17: the colour is depth-cued toward the far colour while the fog flag is set (StagePipeline::set_depth_cue).
  bool depth_cue = false;
  // 0 PS1 rule: NCLIP > 0 faces only (default), 1 reversed (ext bit 4), 2 both sides (ext bit 5).
  int cull = 0;
  // op 36 (ext bit 0): u += phase.u, v += phase.v inside the window.
  bool uv_scroll = false;
  std::array<float, 2> uv_phase{0, 0};
  // off_u, off_v, win_w, win_h (texels); 0 width / height = whole page.
  std::array<int, 4> uv_window{0, 0, 0, 0};
  // Op 37 mesh morph: draw with the vertices lerped toward `asset.parts[morph_part]` by morph_t (the two parts have the same
  // packets; only positions differ). Needs the asset passed to draw(); -1 = none.
  int morph_part = -1;
  float morph_t = 0;
  // The battle ground (the type-0x13 object; needs the asset passed to draw()): the part's quads are the
  // 10 x 10 grid cells (OT slot = the FARTHEST corner's camera z >> 4 + bias, no GTE flag test, dropped when that slot
  // leaves 0..4095), its textured triangles the rim (average z, the flag test), its untextured packets are
  // not drawn. `hide` = cells skipped this frame (a rectangle of cells). `bias` is the ground bias, `blend` 0 or 1 (the
  // ground is semi-transparent with ABR 0 when its object is; the packets' own semi bits are ignored).
  bool ground = false;
  std::bitset<100> hide;
  // Per-vertex warp in world space (the ground copy of the 46black stage and its 'near' / 'far' draw): every vertex goes to camera
  // space by `cam`, back to world space by the inverse rotation of `warp_view` (the view: world -> camera), through `warp` (PS1 axes, y down),
  // and is drawn through `warp_view`. Null / empty = none.
  const std::function<void(std::array<float, 3>&)>* warp = nullptr;
  std::array<float, 12> warp_view{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
  // Screen copy (needs the asset passed to draw()): a copy of the picture placed in VRAM at `screen_origin` (x halfwords | y rows << 16),
  // `screen_size` PS1 pixels. The sub-meshes whose direct-colour page (StageSubMesh::direct_page) lies inside it sample `screen` instead of
  // their own texels: packet texel (u, v) of a page at offset (dx, dy) is copy pixel (dx + u, dy + v), sampled at
  // ((dx + u) * screen_uv[0] + screen_uv[2], (dy + v) * screen_uv[1] + screen_uv[3]) (normalised). Null = none.
  TexRef screen;
  int screen_origin = -1;
  std::array<int, 2> screen_size{320, 224};
  std::array<float, 4> screen_uv{0, 0, 0, 0};
  // The copy's black pixels are opaque texels (a copy made with the mask bit set); otherwise they are transparent.
  bool screen_opaque = false;
  // The offset of a direct-colour page inside the screen copy; false when there is no copy or the page lies outside it.
  bool screen_offset(int direct_page, int& dx, int& dy) const {
    if (!screen || screen_origin < 0 || direct_page < 0) return false;
    dx = (direct_page & 0xFFFF) - (screen_origin & 0xFFFF);
    dy = (direct_page >> 16) - (screen_origin >> 16);
    return dx >= 0 && dy >= 0 && dx < screen_size[0] && dy < screen_size[1];
  }
};

// One polygon of an effect overlay drawn with the stage's textures (effects::FxGroundPrim): PS1 world positions
// (y down), packet texel bytes + CLUT / tpage words (the atlas is found by mode + CLUT), corner colours (textured 0x80 = neutral,
// untextured raw), blend 0 opaque / 1 B/2+F/2 / 2 B+F / 3 B-F / 4 B+F/4, OT bias; slot = average camera z >> 4 + bias.
struct StagePrim {
  // 3 or 4 (quad triangles (0,1,2) (2,1,3)).
  u8 n = 3;
  std::array<std::array<float, 3>, 4> pos{};
  std::array<std::array<u8, 2>, 4> uv{};
  u16 clut = 0, tpage = 0;
  bool textured = true;
  std::array<std::array<u8, 3>, 4> rgb{};
  int blend = 0, bias = 0;
  bool two_sided = false;
  // The corner colours take the depth cue, each at its own depth.
  bool depth_cue = false;
};

// The atlas and object textures of a baked stage made on a worker thread, in StageMesh::sets order (atlases, then objects); null = made at upload.
using StageTextures = std::vector<std::unique_ptr<Texture>>;
// For a worker thread: fills transparent texels (bleed_transparent, concurrently), then creates and uploads every texture of a baked stage
// (Texture::create_blocking). Empty for an asset without object textures (its atlases go through the texture registry at upload).
StageTextures prepare_stage_textures(GpuContext& ctx, const assets::StageAsset& stage);

// State of a sliced StagePipeline::create.
struct StageCreateJob {
  std::unique_ptr<StageMesh> mesh;
  // Next stage texture to upload.
  size_t next = 0;
  // Texture dump / replace group when the asset has none (StagePipeline::set_scene_group).
  std::string group;
  // Textures made off the render thread (prepare_stage_textures), taken by create_textures.
  StageTextures prepared;
};

class StagePipeline {
 public:
  StagePipeline() = default;
  ~StagePipeline() { destroy(); }
  StagePipeline(const StagePipeline&) = delete;
  StagePipeline& operator=(const StagePipeline&) = delete;

  bool init(GpuContext& ctx);
  void destroy();

  std::unique_ptr<StageMesh> create(const assets::StageAsset& stage);
  // Sliced creation: create_begin(); create_textures() every call until it returns true; create_finish() (nullptr = failed).
  StageCreateJob create_begin();
  // Texture group ("battle_stage/bg_07") given to the atlases of assets that have no StageAsset::tex_group and are created through
  // create_begin() (the battle stage, StageScene). create() never uses it (the effect meshes / title name their own assets).
  void set_scene_group(std::string group) { scene_group_ = std::move(group); }
  const std::string& scene_group() const { return scene_group_; }
  bool create_textures(StageCreateJob& job, const assets::StageAsset& stage, double budget_ms);
  std::unique_ptr<StageMesh> create_finish(StageCreateJob& job, const assets::StageAsset& stage);
  // Live tile replacement (render/stage_tiles.h): after the asset's object images / UVs were re-composed (apply_tile_textures), re-creates the object textures
  // (+ both sampling references) and re-uploads the vertex buffer of an existing mesh. The atlases are untouched. False when the mesh does not match the asset.
  bool reload_textures(StageMesh& mesh, const assets::StageAsset& stage);

  // Camera space -> clip, the PS1 projection H * x / z about (ox, oy) in the 320 x 240 layer mapped onto the letterbox
  // (the same mapping as BattleView::Cam::view_proj; depth comes from the fragment shader).
  static Mat4 projection(const Letterbox& lb, float proj_h, float ox = 160.0f, float oy = 120.0f);

  // Restricts drawing to `scissor` for the pass (end() restores the full window). Resets set_gte() / set_depth_cue().
  void begin(const Frame& frame, Rect scissor);
  // GTE state of the pass: with it, draw() applies the PS1 packet builders' rejection rules per packet:
  // a packet is dropped whole when a corner's camera x / y leaves +-32767 (IR1 / IR2 saturation), z leaves 0..65535 or is <= H / 2
  // (division overflow), a screen coordinate leaves -1024..1023 (SX2 / SY2 saturation); a triangle whose screen extent exceeds
  // 1023 x 511 is not drawn by the GPU. `proj_h`, `ox`, `oy` = the projection distance and screen offset of the view. Off by default (title).
  void set_gte(float proj_h, float ox, float oy);
  // GTE depth cue (battle sequence op 0x24 sets the fog flag, op 0x2D ramps the far colour / range): instances with `depth_cue` blend their
  // colour toward the far colour `rgb` (colour bytes, 0x80 = neutral modulation) by p(z) = far * (z - near) / ((far - near) * z), z = the
  // packet's average camera z. `active` = the fog flag.
  struct DepthCue {
    bool active = false;
    float r = 128, g = 128, b = 128, near = 16384, far = 20480;
  };
  void set_depth_cue(const DepthCue& cue) { cue_ = cue; }
  // [Enhancements] Lighting: the depth cue per vertex (smooth floor darkening) instead of per packet (PS1 stepping); independent of art mode.
  void set_smooth_fog(bool on) { smooth_fog_ = on; }
  // [Enhancements] SpellLights: point lights (the strongest kLights of `s`, render world y up) lighting the OPAQUE scenery drawn until the next begin()
  // (per fragment; the face normal from the screen derivatives; additive on the PS1 colour multiplier). `cam` = StageInstance::cam of the view.
  // Not called = no lights = the PS1 look. Effect meshes, shadows and the semi-transparent layers are never lit.
  static constexpr int kLights = 3;
  void set_lights(const LightSet& light_set, const std::array<float, 12>& cam);
  struct SceneLights {
    int count = 0;
    // Camera space (PS1 axes) + radius.
    float pos[kLights][4] = {};
    // Colour x intensity.
    float col[kLights][4] = {};
    // Scene ambient multiplier.
    float amb[3] = {1, 1, 1};
    bool amb_on = false;
  };
  // Packets dropped by the last draw() (debug / tests).
  size_t rejected_packets() const { return rejected_; }
  // Draws every instance: opaque faces first (depth written), then semi-transparent ones back to front (depth tested).
  // `linear`: bilinear texture sampling (HD art mode).
  // `asset` (optional) is needed for instances with a morph_part or a screen copy: their geometry is rebuilt per frame through the dynamic ring buffer.
  void draw(const StageMesh& mesh, const Mat4& proj, std::span<const StageInstance> instances, bool linear = false,
            const assets::StageAsset* asset = nullptr);
  // Draws per-frame geometry (untextured: the vertex colours are the raw PS1 colour / 255) with the given transform,
  // blend (0 opaque, 1 average, 2 add, 3 subtract; 1-3 are depth tested, not written), both sides, and OT bias in slots.
  // Capacity per frame: kDynVertices / kDynIndices; calls beyond it are dropped with a warning.
  void draw_dynamic(const Mat4& proj, const std::array<float, 12>& cam, std::span<const assets::StageVertex> vertices,
                    std::span<const u32> indices, int blend, int bias);
  // Draws effect polygons with the stage's atlases (`cam` = world -> camera as StageInstance::cam, the PS1 packet rejection of set_gte()
  // applied per polygon): opaque ones first, then the semi-transparent ones back to front.
  void draw_prims(const StageMesh& mesh, const assets::StageAsset& asset, const Mat4& proj, const std::array<float, 12>& cam,
                  std::span<const StagePrim> prims, bool linear = false);
  void end();

  // 65536 vertices: the seam strips of a big scenery part go through the ring too.
  static constexpr u32 kDynVertices = 65536, kDynIndices = 262144;

 private:
  TexRef make_stage_set(const Texture& tex, bool linear);
  // Appends to the dynamic ring at vertex / index position `first` (the frame's mapped memory) and marks the range for the copy to the GPU.
  void write_vertices(const void* data, u32 count, u32 first);
  void write_indices(const u32* data, u32 count, u32 first);
  void draw_cpu(const StageMesh& mesh, const assets::StageAsset& asset, const Mat4& proj, const StageInstance& in,
                const std::vector<TexRef>& sets);
  // The seam strips of one instance of `part` (render::edge_blend()): `vv` = the part's vertices as drawn this frame (positions / colours / centres;
  // part-local indices), `kept` = per packet of the part (packet index - the part's first packet) whether it was drawn, null = all. Binds the ring's vertex
  // buffer; the caller restores its own bindings. Nothing for semi-transparent sub-meshes / grounds.
  void draw_seams(const StageMesh& mesh, const assets::StagePart& part, const StageMesh::Part& mp, const Mat4& proj, const Mat4& cam, const StageInstance& in,
                  const std::vector<TexRef>& sets, const std::vector<assets::StageVertex>& vv, const std::vector<u8>* kept, bool affine);
  std::string scene_group_;
  // Effect polygons that fell back to the raw atlases (draw_prims; logged at powers of two).
  u32 raw_prims_ = 0;
  GpuContext* ctx_ = nullptr;
  SDL_GPUShader* vs_ = nullptr;
  SDL_GPUShader* fs_ = nullptr;
  // The same shaders with screen-linear (PS1 affine) interpolation.
  SDL_GPUShader* vs_affine_ = nullptr;
  SDL_GPUShader* fs_affine_ = nullptr;
  // [cull 0..2][blend 0 opaque, 1 average, 2 add, 3 subtract]
  Pipeline pipes_[3][4];
  // [Video] Ps1Jitter: affine texture / colour interpolation like the PS1 GPU.
  Pipeline pipes_affine_[3][4];
  Sampler nearest_, linear_;
  bool active_ = false;
  Extent extent_{};
  u32 frame_index_ = 0;
  StreamBuffer dyn_vertices_, dyn_indices_;
  u8* dyn_vertex_memory_ = nullptr;
  u8* dyn_index_memory_ = nullptr;
  u32 dyn_vused_ = 0, dyn_iused_ = 0;
  bool gte_ = false;
  float gte_h_ = 512, gte_ox_ = 160, gte_oy_ = 120;
  CullFrame gte_cf_;
  DepthCue cue_;
  // [Enhancements] Lighting: depth cue per vertex instead of per packet.
  bool smooth_fog_ = false;
  // The current draw() samples the bilinear texture sets.
  bool linear_draw_ = false;
  size_t rejected_ = 0;
  SceneLights lights_;
  std::unique_ptr<Texture> dyn_white_;
  TexRef dyn_white_set_;
};

}  // namespace oc::render
