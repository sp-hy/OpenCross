#pragma once
// Room backdrop: loads a field room through the VFS, uploads its background layers and draws them in the field picture
// (320 x 224 buffer rows, RDE's framing: render::Letterbox::field) scaled to the window (parallax groups, per-layer visibility, optional RDE HD chip2 set), plus the actor
// pass. It owns the room scene, walk mesh and projection camera but no game state: the scroll
// position, the actors and the player are driven by FieldWorld (game/field_world.h).
#include <future>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "assets/battle_stage.h"
#include "assets/room_scene.h"
#include "game/actor.h"
#include "game/draw_offset.h"
#include "game/room_lighting.h"
#include "game/room_models.h"
#include "game/walk_mesh.h"
#include "import/chip2.h"
#include "render/camera.h"
#include "render/mesh_pipeline.h"
#include "render/sprite_batch.h"
#include "vfs/vfs.h"

namespace oc::game {

class RoomView {
 public:
  RoomView();
  ~RoomView();
  RoomView(const RoomView&) = delete;
  RoomView& operator=(const RoomView&) = delete;

  // Builds the sorted list of rooms (map/mapbin/*.bin that have a .ctd).
  bool init(const vfs::Vfs& files, render::GpuContext& ctx, render::SpriteBatch& sprites, render::MeshPipeline& meshes);
  // Loads the room's background, camera and walk mesh. The scroll starts at (0, 0) with all layers visible. A matching prefetch()
  // hands over its work, so only the GPU uploads are left.
  bool load(std::string_view name);
  void unload() { clear(); }
  // Starts the CPU part of a later load(name) with this .prd path (set_prd_path's form) on a worker thread: file reads, the import,
  // the 1x composition and, in HD art, the 4x one. load() uses it when the room, the .prd, the art mode and the file set still match,
  // else throws it away. A second request for the same room is ignored.
  void prefetch(std::string_view name, std::string prd_path);
  // Waits for a running prefetch and throws it away (the file set is about to change).
  void drop_prefetch();
  // The loaded room's models built with it (by its prefetch or the load itself); index = RoomScene::models. The caller takes them.
  std::vector<std::optional<PreparedRoomModel>> take_prepared_models() { return std::exchange(prepared_models_, {}); }
  // The .prd of the next load() (VFS path): the room's effect file is the disc index record after its .bin, which names another
  // room's .prd in 195 rooms. Empty = "<room>.prd". Consumed by load().
  void set_prd_path(std::string path) { prd_path_ = std::move(path); }

  const std::vector<std::string>& rooms() const { return rooms_; }
  const std::string& room_name() const { return name_; }
  bool loaded() const { return loaded_; }
  const assets::RoomScene* scene() const { return scene_.get(); }
  // The type-0x02 mesh objects of the loaded room's effect .prd (sprite type 14 objects: `elem`, `tori`, ...), or nullopt.
  std::optional<assets::StageAsset> effect_meshes() const;
  WalkMesh& walk() { return walk_; }
  const WalkMesh& walk() const { return walk_; }
  const render::FieldCamera& camera() const { return camera_; }
  size_t layer_count() const { return layers_; }
  // Composed (layer, group) images in the scene.
  size_t group_count() const { return groups_; }
  size_t draw_count() const { return items_.size(); }

  // ---- scroll (layer pixels added to the tile positions; the camera moves, so content moves the other way) --------------
  struct Limits {
    int x_min = 0, x_max = 0, y_min = 0, y_max = 0;
  };
  // The room's pan limits, with the loader's 24 px bias on y_min.
  Limits default_limits() const;
  // Widescreen: pan limits `limits` narrowed by `extra` columns per side so the wider view never shows past the
  // picture: x_max - extra, x_min + extra (both the midpoint when they cross). extra = 0 returns `limits` unchanged.
  static Limits widen(Limits limits, int extra);
  // True while the simulation stands still: draws use the latest tick, not a sweeping alpha.
  void set_interp_frozen(bool frozen) { interp_frozen_ = frozen; }
  // `snap` also resets the interpolation source.
  void set_scroll(int x, int y, bool snap);
  // The one display scroll of a rendered frame (previous -> current tick by alpha; sub-pixel in HD art, floored in original mode).
  // Every drawer of room-space content (layers, actors, shadows, effects) must use this value.
  void scroll_at(float alpha, float& sx, float& sy) const;
  int scroll_x() const { return pan_x_; }
  int scroll_y() const { return pan_y_; }

  // ---- layers and art ---------------------------------------------------------------------------------------------
  // Bit n = background tile group n visible (BG_LAYER_ON / OFF address groups). Rebuilds the layer textures when it changes.
  void set_layer_mask(u32 mask);
  // Some layer of the room has tiles in this tile group.
  bool has_group(u32 group) const;
  // (layer, group) images whose group bit is set in the mask.
  size_t visible_group_images() const;
  u32 layer_mask() const { return layer_mask_; }
  // Host override: groups drawn as if BG_LAYER_OFF whatever the script says (the HD title hides the PS1 logo / prompt / (c) layers).
  void set_hidden_groups(u32 mask) { hidden_groups_ = mask; }
  // Groups with parallax scale 0: fixed to the screen (the world maps' "Home / Another World" name).
  u32 fixed_groups() const;
  // Widescreen: the screen-fixed groups keep to the left edge of the visible columns instead of the 4:3 area's (the world name).
  void set_fixed_to_edge(bool on) { fixed_to_edge_ = on; }
  // C2 SET_LAYER_OPACITY: the colour the group's tiles are modulated with (bytes, 128 = neutral). Cleared by load().
  void set_group_tint(size_t group, int r, int g, int b);
  // Debug: one line per drawn texture (group, z, rectangle, alpha).
  std::string describe_items() const;
  // Selects the 1x or the 4x (RDE .chip2) background set; the HD set is built on first use. Rooms with no chip2 stay 1x.
  void set_art_mode(bool hd);
  // Art-mode switch of a loaded room as a pure rendering swap: re-reads the room's .bin / .ctd / .prd
  // through the VFS (which now resolves cdrom/ through the other file set) and replaces only the art of the scene - layer images,
  // room-model meshes / textures, effect sprite pixels (in place: the effect players keep their pointers) - then rebuilds the layer
  // textures (1x, and 4x when `hd`). Walk mesh (incl. the script-edited flag table), camera, scroll, layer mask, tints, script and
  // zones are untouched. False (old art kept, `hd` still selects the layer set) when the files cannot be re-read or do not match.
  bool reload_art(bool hd);
  // True when the HD set is the one on screen.
  bool art_mode_hd() const { return hd_ && hd_ready_; }
  // Bilinear actor texture sampling.
  bool linear_filter = false;
  // [Video] FilterRooms: bilinear background tiles (the sampler of the sets is nearest; draw() overrides it).
  bool bg_linear = false;
  // [Video] WideEdgeFade: widescreen, a room narrower than the window: the cut edge fades to black over this many layer px.
  float edge_fade = 0.0f;

  // ---- drawing ----------------------------------------------------------------------------------------------------
  // `alpha` (0..1) interpolates between the last two simulated ticks. Background first (call between SpriteBatch begin()
  // and end()), then the actors (3D pass, outside SpriteBatch begin/end).
  void draw(render::SpriteBatch& sprites, float view_w, float view_h, float alpha) const;
  // `looks` (optional, parallel to `actors`) = per-actor light colour + water rule (room_lighting.h); without it every
  // actor uses Actor::tint and is dry.
  void draw_actors(render::MeshPipeline& meshes, const render::Frame& frame, float alpha, std::span<const Actor* const> actors,
                   std::span<const ActorLook> looks = {}) const;
  // Window area of the field picture: buffer rows 0..224 in RDE's cropped 4:3 viewport (render::Letterbox::field), or 1:1 while the
  // field is drawn into the PS1-resolution target (set_target_pass). Every drawer of room-space content must use this mapping.
  render::Letterbox letterbox(float view_w, float view_h) const {
    return target_pass_ ? render::Letterbox::identity(view_w, view_h, target_scale_) : render::Letterbox::field(view_w, view_h);
  }
  // Layer columns of the field picture in `lb` at scroll x `scroll_x` (scroll_at): the visible columns [lb.x0, lb.x0 + lb.w]; in widescreen
  // cut to the room picture's own columns (black where the picture ends).
  void picture_columns(const render::Letterbox& lb, float scroll_x, float& c0, float& c1) const;
  // The window rectangle of those columns x buffer rows 0..224 (the scissor of every field pass): `rounded` = rounded edges clamped to
  // fw x fh (the host's pass clip), else floor of the origin / ceil of the size (the actor passes). 4:3 = exactly the 320 x 224 area.
  render::Rect clip_rect(const render::Letterbox& lb, float scroll_x, float fw, float fh, bool rounded = false) const;
  // True while the host records the field into a (kLayerW x kFieldRows) * `scale` render target (classic art, "PS1 resolution 3D";
  // scale = [Video] RenderScale: target pixels per buffer pixel).
  void set_target_pass(bool on, float scale = 1.0f) {
    target_pass_ = on;
    target_scale_ = scale;
  }
  // Ordering-table slab of the current pass (FieldWorld::set_draw_slab, the 8F ripple split): draw() keeps the layers whose OT index
  // (the group z, minimum 8 as in the depth) satisfies lo < z <= hi.
  void set_slab(float lo, float hi) {
    slab_lo_ = lo;
    slab_hi_ = hi;
  }
  // Late layers (FieldWorld::draw_background_late): semi-transparent items (a blend mode) whose OT index is below `z` - nearer than every
  // actor - are skipped by the normal draw() and drawn by a second call with late_pass = true after the actors and effects: the PS1 files
  // them nearer than the models, so they blend over the models (the water layer of the Hydra Marshes). z <= 0 = no late layers.
  // How fast what the ordering table draws at `slot` scrolls, as a factor of the global scroll: an effect filed under a layer's draw offset
  // moves with that layer (game/draw_offset.h).
  float slot_parallax(int slot) const;
  void set_late(float z, bool late_pass) {
    late_z_ = z;
    late_pass_ = late_pass;
  }

 private:
  // One texture drawn as one quad: all (layer, group) images sharing the same group and z.
  struct Item;
  // The CPU half of a load (prepare), made on the calling thread or by prefetch().
  struct Prepared;
  using Chip2Result = std::pair<std::optional<import::Chip2>, std::string>;

  // CPU only, thread-safe. Builds the room's models in parallel; `ahead` (a prefetch) also composes the 4x images (within a memory
  // budget); without it the chip2 read is left running in the result for ensure_hd.
  static std::unique_ptr<Prepared> prepare(const vfs::Vfs& files, render::GpuContext* ctx, const std::string& name, const std::string& prd_request,
                                           bool ahead, bool hd);
  // The scene's (layer, group) images merged into items, back to front, without textures.
  static std::vector<std::unique_ptr<Item>> plan_items(const assets::RoomScene& scene);
  static std::vector<std::vector<u8>> compose_1x(const assets::RoomScene& scene, const std::vector<std::unique_ptr<Item>>& items);
  static std::vector<u8> compose_pixels(const assets::RoomScene& scene, const import::Chip2* chip2, const Item& item, bool hd);
  // Uploads the 1x textures of items_ from `px` (one image per item) and records the painted columns.
  bool upload_items(const std::vector<std::vector<u8>>& px);
  bool upload_set(Item& item, bool hd, const std::vector<u8>& px);
  static std::optional<import::Chip2> read_chip2(const vfs::Vfs& files, const std::string& room, std::string* err);
  void ensure_hd();
  // Phase accumulator for the debug timing log (set only while ensure_hd runs).
  double* hd_upload_ms_ = nullptr;
  std::future<Chip2Result> chip2_future_;
  // The running prefetch and what it was started for (room, .prd request, art mode, file set generation).
  std::future<std::unique_ptr<Prepared>> prefetch_;
  std::string prefetch_name_, prefetch_prd_;
  bool prefetch_hd_ = false;
  u32 prefetch_generation_ = 0;
  void clear();

  const vfs::Vfs* files_ = nullptr;
  render::GpuContext* ctx_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  render::MeshPipeline* meshes_ = nullptr;
  render::Sampler sampler_;
  render::Sampler fade_sampler_;
  // 128 x 1 black, alpha 255 -> 0 (the widescreen edge fade ramp).
  render::Texture fade_tex_;
  render::TexRef fade_set_;
  WalkMesh walk_;
  render::FieldCamera camera_;
  std::vector<std::string> rooms_;
  // Back to front.
  std::vector<std::unique_ptr<Item>> items_;
  std::vector<u32> group_scale_;
  // Scroll factor of a group: 4096 / scale, 0 for a group fixed to the screen.
  float group_parallax(u32 group) const;
  // Only for rooms with a group that does not scroll 1:1; empty otherwise.
  std::vector<draw_offset::LayerDepth> layer_depths_;
  std::string name_;
  std::string prd_path_;
  // The .prd the loaded room was imported with (reload_art re-reads it; empty = none).
  std::string loaded_prd_path_;
  size_t layers_ = 0, groups_ = 0;
  Limits limits_;
  // The painted extent in layer columns: the union of the layer groups' tile boxes (upload_items). Widescreen shows the picture out to it
  // rather than to the camera block's pan limits: several rooms carry art beyond what 4:3 panning can reach (man_bo1f), and the frame
  // beyond the tiles stays black.
  int art_x0_ = 0, art_x1_ = 320;
  // See set_interp_frozen.
  bool interp_frozen_ = false;
  // Scroll S (layer pixels, added to tile positions), latest tick; then the previous tick, for render interpolation.
  int pan_x_ = 0, pan_y_ = 0;
  int prev_pan_x_ = 0, prev_pan_y_ = 0;
  u32 layer_mask_ = 0xFFFFFFFFu;
  u32 hidden_groups_ = 0;
  bool fixed_to_edge_ = false;
  // Per group, 1 = neutral.
  std::vector<std::array<float, 3>> group_tint_;
  bool loaded_ = false;
  bool target_pass_ = false;
  float target_scale_ = 1.0f;
  float slab_lo_ = -1e9f, slab_hi_ = 1e9f;
  float late_z_ = -1e9f;
  bool late_pass_ = false;
  bool hd_ = true, hd_ready_ = false, hd_failed_ = false;
  std::unique_ptr<assets::RoomScene> scene_;
  std::vector<std::optional<PreparedRoomModel>> prepared_models_;
  std::optional<import::Chip2> chip2_;
};

}  // namespace oc::game
