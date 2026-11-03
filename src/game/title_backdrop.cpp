#include "game/title_backdrop.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <variant>

#include "core/log.h"
#include "game/free_cam.h"
#include "assets/room_effects.h"
#include "import/battle_stage.h"
#include "import/container.h"
#include "import/model.h"
#include "import/room_effects.h"
#include "render/mat4.h"
#include "render/stage_tiles.h"
#include "render/texture_registry.h"

namespace oc::game {

namespace {

constexpr float kPi = 3.14159265358979f;
using V3 = std::array<float, 3>;
V3 cross3(const V3& lhs, const V3& rhs) {
  return {lhs[1] * rhs[2] - lhs[2] * rhs[1], lhs[2] * rhs[0] - lhs[0] * rhs[2], lhs[0] * rhs[1] - lhs[1] * rhs[0]};
}
float dot3(const V3& lhs, const V3& rhs) { return lhs[0] * rhs[0] + lhs[1] * rhs[1] + lhs[2] * rhs[2]; }
V3 norm3(V3 vec, const V3& fb) {
  const float len = std::sqrt(dot3(vec, vec));
  if (len < 1e-6f) return fb;
  return {vec[0] / len, vec[1] / len, vec[2] / len};
}

void put16(Bytes& bytes, size_t offset, u32 value) {
  bytes[offset] = static_cast<u8>(value);
  bytes[offset + 1] = static_cast<u8>(value >> 8);
}
void put32(Bytes& bytes, size_t offset, u32 value) {
  put16(bytes, offset, value & 0xFFFF);
  put16(bytes, offset + 2, value >> 16);
}

// title.ltd = cpt of VRAM uploads {u32 type 0x1101 CLUT / 0x1100 image, u16 x, u16 y, u16 dx, u16 dy, u16 w (halfwords), u16 h, data}:
// one CLUT block (256 x 12 at (256, 228)) and four image blocks, all keyed to the page origin (192, 256) with the block's own
// offset in dx / dy: the 512 x 256 main page and three smaller blocks at (704, 256), (704, 352) and (704, 384) that hold the
// fish / manta / tree textures (tpage 704) and the small sprites. Re-wrapped as the sector .ctd the stage importer reads
// (types 0x1201 / 0x1200, x = base + dx, y = base + dy) so the atlas spans every block.
Bytes ltd_to_ctd(ByteSpan ltd) {
  Bytes out;
  const auto cpt = import::parse_cpt(ltd);
  if (!cpt) return out;
  std::vector<ByteSpan> secs;
  for (const auto& entry : cpt->entries) {
    if (entry.data.size() < 16) continue;
    const u32 type = entry.data[0] | (entry.data[1] << 8) | (entry.data[2] << 16) | (static_cast<u32>(entry.data[3]) << 24);
    if (type != 0x1101 && type != 0x1100) continue;
    const u32 w = entry.data[12] | (entry.data[13] << 8), h = entry.data[14] | (entry.data[15] << 8);
    if (w == 0 || h == 0 || entry.data.size() < 16 + 2ull * w * h) continue;
    secs.push_back(entry.data);
  }
  for (ByteSpan block : secs) {
    const u32 type = block[0] | (block[1] << 8), x = (block[4] | (block[5] << 8)) + (block[8] | (block[9] << 8)),
              y = (block[6] | (block[7] << 8)) + (block[10] | (block[11] << 8));
    const u32 w = block[12] | (block[13] << 8), h = block[14] | (block[15] << 8);
    const u32 row_bytes = w * 2, rows_per = std::max<u32>(1, 2048 / std::max<u32>(2, row_bytes));
    const u32 nsec = (h + rows_per - 1) / rows_per;
    const size_t base = out.size();
    out.resize(base + 2048ull * (1 + nsec), 0);
    put32(out, base, type + 0x100);
    put16(out, base + 4, x);
    put16(out, base + 6, y);
    put16(out, base + 12, w);
    put32(out, base + 20, static_cast<u32>(secs.size()));
    put32(out, base + 24, nsec);
    for (u32 sector = 0; sector < nsec; ++sector) {
      const u32 rows = std::min(rows_per, h - sector * rows_per);
      put16(out, base + 28 + 2ull * sector, rows);
      std::memcpy(&out[base + 2048ull * (1 + sector)], block.data() + 16 + static_cast<size_t>(sector) * rows_per * row_bytes,
                  static_cast<size_t>(rows) * row_bytes);
    }
  }
  return out;
}

}  // namespace

std::optional<assets::StageAsset> load_title_stage(const vfs::Vfs& vfs) {
  const auto prd = vfs.read("cdrom/title.prd");
  const auto ltd = vfs.read("cdrom/title.ltd");
  if (!prd || !ltd) return std::nullopt;
  const Bytes ctd = ltd_to_ctd(*ltd);
  if (ctd.empty()) return std::nullopt;
  auto st = import::load_stage(*prd, ctd);
  if (st) {
    st->tex_group = "title";
    render::bake_object_textures(*st);
  }
  return st;
}

bool TitleBackdrop::init(const vfs::Vfs& vfs, render::GpuContext& ctx, render::SpriteBatch& sprites, render::MeshPipeline& meshes) {
  shutdown();
  ctx_ = &ctx;
  sprites_ = &sprites;
  mesh_pipe_ = &meshes;
  auto prd = vfs.read("cdrom/title.prd");
  auto ltd = vfs.read("cdrom/title.ltd");
  if (!prd || !ltd) {
    log::warn("title backdrop: cdrom/title.prd or title.ltd unavailable");
    return false;
  }
  prd_ = std::move(*prd);
  if (auto particle = vfs.read("cdrom/effect/particle.prd")) {
    lib_bytes_ = std::move(*particle);
    import::Vram empty;
    lib_ = import::load_room_effects(lib_bytes_, empty);
  }
  const Bytes ctd = ltd_to_ctd(*ltd);
  ltd_blocks_.clear();
  if (auto cpt = import::parse_cpt(*ltd))
    for (const auto& entry : cpt->entries) {
      if (entry.data.size() < 16 || (entry.data[0] | (entry.data[1] << 8)) != 0x1100) continue;
      ltd_blocks_.push_back({(entry.data[4] | (entry.data[5] << 8)) + (entry.data[8] | (entry.data[9] << 8)),
                             (entry.data[6] | (entry.data[7] << 8)) + (entry.data[10] | (entry.data[11] << 8)),
                             entry.data[12] | (entry.data[13] << 8), entry.data[14] | (entry.data[15] << 8)});
    }
  if (ctd.empty()) {
    log::warn("title backdrop: title.ltd is not a cpt of VRAM uploads");
    return false;
  }
  import::Vram empty;
  fx_ = import::load_battle_effects(prd_, {}, empty, lib_bytes_);
  for (const auto& error : fx_.errors) log::debug("title backdrop: {}", error);
  std::string err;
  if (auto st = import::load_stage(prd_, ctd, &err)) meshes_ = std::make_shared<assets::StageAsset>(std::move(*st));
  else log::warn("title backdrop: meshes: {}", err);
  if (meshes_) {
    // Texture dump / replace: title/obj_NN_<kind>_<name>.png (baked per-object textures).
    meshes_->tex_group = "title";
    render::bake_object_textures(*meshes_);
    render::apply_tile_textures(*meshes_, "title");
    tile_gen_ = render::texture_registry::generation();
  }
  cams_.add_drp(prd_);
  camera_.set_bank(&cams_);

  effects::EffectBank bank;
  bank.add(&fx_);
  bank.add(&lib_);
  if (!gpu_.init(ctx, sprites) || !gpu_.load(bank) || !particles_.init(ctx) || !stage_pipe_.init(ctx)) {
    log::warn("title backdrop: GPU setup failed");
    return false;
  }
  if (meshes_) {
    // The meshes are drawn exactly like a battle stage (StageScene): StagePipeline instances with the object's matrix, colour,
    // blend and OT bias; the atlases get the transparent-texel bleed and a bilinear set for HD mode.
    for (size_t i = 0; i < meshes_->parts.size(); ++i) parts_.emplace(meshes_->parts[i].name, static_cast<int>(i));
    stage_mesh_ = stage_pipe_.create(*meshes_);
    if (!stage_mesh_) log::warn("title backdrop: mesh upload failed");
    load_models(meshes);
  }
  ready_ = true;
  log::info("title backdrop: {} emitters, {} sequences, {} meshes, {} cameras, {} textures", fx_.emitters.size(), fx_.sequences.size(),
            meshes_ ? meshes_->parts.size() : 0, cams_.cameras(), meshes_ ? meshes_->textures.size() : 0);
  return true;
}

// Sprite type 20 = a type-0x0B record = an Mdl model used as the particle body (the object's draw colour, position, angles and
// scale are copied into the model each tick and its motion advances). The record carries no texture words: the *record name* is
// decoded as hex digits - chars 0-1 = CLUT row (`ed9b` -> 0xED = 237), char 2 = a nibble whose bit 0 is
// the tpage y (1 -> 256) and bit 3 the CLUT x (8 -> 256), char 3 = tpage x / 64 (`b` -> 704, `a` -> 640) - and bound to the model.
std::vector<TitleModel> load_title_models(ByteSpan prd, const assets::StageAsset& stage) {
  std::vector<TitleModel> out;
  const auto drp = import::parse_drp(prd);
  if (!drp) return out;
  auto hexval = [](char digit) -> int {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    return -1;
  };
  for (const import::DrpEntry& entry : drp->entries) {
    if (entry.type != 0x0B) continue;
    std::string err;
    if (entry.name.size() < 4 || hexval(entry.name[0]) < 0 || hexval(entry.name[1]) < 0 || hexval(entry.name[2]) < 0 || hexval(entry.name[3]) < 0) {
      log::warn("title backdrop: model {}: name is not a texture binding", entry.name);
      continue;
    }
    const int clut_y = hexval(entry.name[0]) * 16 + hexval(entry.name[1]);
    const int nib = hexval(entry.name[2]);
    const int tpage_y = (nib & 1) ? 256 : 0, clut_x = (nib & 8) ? 256 : 0;
    const int tpx = hexval(entry.name[3]) * 64;
    // The title's pages are 8bpp; a 4bpp CLUT would be a 16-entry row.
    int tex = stage.find_texture(1, clut_x, clut_y);
    if (tex < 0) tex = stage.find_texture(0, clut_x, clut_y);
    if (tex < 0 || tpage_y != 256) {
      log::warn("title backdrop: model {}: texture (tpage {},{} CLUT {},{}) not in the title VRAM atlas", entry.name, tpx, tpage_y, clut_x, clut_y);
      continue;
    }
    const assets::StageTexture& texture = stage.textures[static_cast<size_t>(tex)];
    const int ppw = texture.mode == 0 ? 4 : 2;
    const int x0 = (tpx - stage.atlas_x0_hw) * ppw;
    import::Image page;
    page.width = 128;
    page.height = 256;
    page.pixels.assign(static_cast<size_t>(128) * 256 * 4, 0);
    for (u32 y = 0; y < 256 && y < texture.image.height; ++y)
      for (u32 x = 0; x < 128; ++x) {
        const int sx = x0 + static_cast<int>(x);
        if (sx < 0 || static_cast<u32>(sx) >= texture.image.width) continue;
        std::memcpy(&page.pixels[(static_cast<size_t>(y) * 128 + x) * 4],
                    &texture.image.pixels[(static_cast<size_t>(y) * texture.image.width + static_cast<u32>(sx)) * 4], 4);
      }
    auto asset = import::load_room_model(entry.data, page, &err);
    if (!asset) {
      log::warn("title backdrop: model {}: {}", entry.name, err);
      continue;
    }
    asset->source = "@room_model";
    out.push_back({entry.name, tpx, tpage_y, clut_x, clut_y, std::move(*asset)});
  }
  return out;
}

bool TitleBackdrop::load_models(render::MeshPipeline& meshes) {
  models_.clear();
  if (!meshes_) return false;
  int loaded = 0;
  for (TitleModel& model : load_title_models(prd_, *meshes_)) {
    auto res = std::make_unique<ModelRes>();
    res->asset = std::move(model.asset);
    render::resolve_model_textures(res->asset);
    res->gpu = meshes.create_model(res->asset);
    if (!res->gpu) {
      log::warn("title backdrop: model {}: GPU upload failed", model.name);
      continue;
    }
    res->pose.set_model(&res->asset);
    // Motion: the clip with the most keyframes (the single-frame clip is the rest pose) [I: op 05 frame0 = 1 may be the motion index].
    for (size_t i = 0; i < res->asset.clips.size(); ++i)
      if (res->clip < 0 || res->asset.clips[i].frame_count > res->asset.clips[static_cast<size_t>(res->clip)].frame_count) res->clip = static_cast<int>(i);
    log::info("title backdrop: model {}: {} joints, {} clips (motion {} x {} keyframes), tpage {},{} / CLUT ({},{}) from the name", model.name,
              res->asset.skeleton.joints.size(), res->asset.clips.size(), res->clip,
              res->clip >= 0 ? res->asset.clips[static_cast<size_t>(res->clip)].frame_count : 0, model.tpage_x, model.tpage_y, model.clut_x,
              model.clut_y);
    models_.emplace(model.name, std::move(res));
    ++loaded;
  }
  return loaded > 0;
}

// The models' pristine pages stay in the asset; only the replacement images are dropped and resolved again, and the GPU copy rebuilt.
// The motion lives in the emitter objects, so the swim carries on.
void TitleBackdrop::reload_model_textures() {
  if (!mesh_pipe_) return;
  for (auto& [name, res] : models_) {
    res->asset.mod_textures.clear();
    render::resolve_model_textures(res->asset);
    if (auto gpu = mesh_pipe_->create_model(res->asset)) res->gpu = std::move(gpu);
    else log::warn("title backdrop: model {}: GPU upload failed", name);
  }
}

void TitleBackdrop::shutdown() {
  seq_.reset();
  em_.reset();
  models_.clear();
  mesh_pipe_ = nullptr;
  stage_mesh_.reset();
  stage_pipe_.destroy();
  inst_.clear();
  parts_.clear();
  gpu_.clear();
  particles_.destroy();
  meshes_.reset();
  ready_ = running_ = false;
  ctx_ = nullptr;
  sprites_ = nullptr;
}

bool TitleBackdrop::build_run(std::string_view first) {
  if (!fx_.find_sequence(first)) return false;
  bank_ = effects::EffectBank{};
  bank_.add(&fx_);
  bank_.add(&lib_);
  effects::EmitterSystem::Config cfg;
  cfg.seed = 0x7171u + static_cast<u32>(first.back());
  // The whole sea at all times, not per-shot scenery sets.
  cfg.keep_on_stop_expire = true;
  seq_.reset();
  em_ = std::make_unique<effects::EmitterSystem>(bank_, cfg);
  effects::ActorHooks ah;
  ah.anchor_basis = [](int, effects::Basis& out) {
    out = effects::Basis{};
    return true;
  };
  em_->set_hooks(std::move(ah));
  em_->register_all();
  seq_ = std::make_unique<effects::SequencePlayer>(bank_, *em_, cfg.seed);
  effects::SequenceHooks sh;
  sh.camera = [this](std::string_view shot, const assets::EffectBlob*) {
    if (!camera_.select(std::string(shot))) log::debug("title backdrop: camera shot '{}' missing", shot);
  };
  sh.actor_busy = [] { return false; };
  seq_->set_hooks(std::move(sh));
  return true;
}

bool TitleBackdrop::play(std::string_view name) {
  // Debug: force a sequence (tit0..tit3) instead of the script's RAND & 3 pick.
  static const char* seq_dbg = std::getenv("OC_TITLE_SEQ");
  if (seq_dbg && *seq_dbg) name = seq_dbg;
  if (!ready_ || !build_run(name)) return false;
  ticks_ = 0;
  models_logged_ = false;
  running_ = true;
  seq_->start(name);
  log::info("title backdrop: sequence {}", name);
  return true;
}

void TitleBackdrop::stop() {
  running_ = false;
  seq_.reset();
  em_.reset();
}

void TitleBackdrop::tick() {
  if (!running_ || !em_) return;
  seq_->tick();
  em_->tick();
  CamEvents ev;
  camera_.tick(ev);
  ++ticks_;
}

u32 TitleBackdrop::live_objects() const { return em_ ? em_->stats().live : 0; }

namespace {
// Debug tab free camera: the sea is looked at from the free pose (H stays 512, FovDeg applies in view()). Used by BOTH the sprite view and the
// model (sprite type 20: manta, turtle, fish) view matrix below.
CamPose free_or_shot_pose(const BattleCamera& camera, float alpha) {
  CamPose pose = camera.pose(alpha);
  if (FreeCam& fc = free_cam(); fc.on()) {
    if (fc.needs_init()) fc.init_from(pose.eye, pose.target);
    pose.eye = fc.eye();
    pose.target = fc.target();
    pose.h = 512.0f;
    pose.roll_deg = 0.0f;
  }
  return pose;
}
}  // namespace

// CamPose -> view (the battle camera's basis: y down, roll about the view axis).
effects::ViewParams TitleBackdrop::view(float alpha) const {
  const CamPose pose = free_or_shot_pose(camera_, alpha);
  const V3 forward = norm3({pose.target[0] - pose.eye[0], pose.target[1] - pose.eye[1], pose.target[2] - pose.eye[2]}, {0, 0, 1});
  V3 right = norm3(cross3({0, 1, 0}, forward), {1, 0, 0});
  V3 down = cross3(forward, right);
  for (float& comp : down) comp = -comp;
  // A positive roll turns the picture clockwise.
  const float cr = std::cos(pose.roll_deg * kPi / 180.0f), sr = std::sin(pose.roll_deg * kPi / 180.0f);
  for (size_t i = 0; i < 3; ++i) {
    const float ri = right[i] * cr - down[i] * sr, di = right[i] * sr + down[i] * cr;
    right[i] = ri;
    down[i] = di;
  }
  effects::ViewParams params;
  const V3* rows[3] = {&right, &down, &forward};
  for (size_t k = 0; k < 3; ++k) {
    const V3& row = *rows[k];
    params.rot.m[k * 3 + 0] = row[0];
    params.rot.m[k * 3 + 1] = -row[1];
    params.rot.m[k * 3 + 2] = row[2];
    params.trans[k] = -dot3(row, pose.eye);
  }
  params.h = pose.h * render::fov_scale();
  // The title overlay's geometry offset is (160, 112), not the field's 120.
  params.oy = 112.0f;
  return params;
}

// The sea's draw area: every visible column of `lb` (widescreen: the widened frame), its 240 rows, clamped to the frame.
namespace {
render::Rect sea_scissor(const render::Letterbox& lb) {
  // Edges rounded to the pixels a 2D quad from the same fractional edge covers (pixel centres inside it): truncating the left edge
  // put the sea one column further left than the 4:3 title picture over it (a cyan sliver beside the pillarbox).
  const float x0 = std::max(0.0f, std::round(lb.left())), y0 = std::max(0.0f, std::round(lb.oy));
  const float x1 = std::min(lb.vw, std::round(lb.left() + lb.width())), y1 = std::min(lb.vh, std::round(lb.oy + render::kLayerH * lb.scale_y));
  return {static_cast<int32_t>(x0), static_cast<int32_t>(y0),
          static_cast<u32>(std::max(0.0f, x1 - x0)), static_cast<u32>(std::max(0.0f, y1 - y0))};
}
}  // namespace

void TitleBackdrop::draw(const render::Frame& frame, float view_w, float view_h, float alpha, bool linear) {
  if (!running_ || !em_) return;
  const effects::ViewParams vp = view(alpha);
  // The 240-row 4:3 fit, widened in widescreen by the window's extra columns: the same H and projection
  // centre (160, 112) over a wider viewport = a wider view of the 3D sea, nothing clamped (as the battle 3D). The title room's 4:3
  // layers are composed with wide_extra() forced to 0, so this reads screen_extra().
  const render::Letterbox lb = render::Letterbox::wide(view_w, view_h, render::screen_extra());

  // Mesh objects (sprite type 14): one StageInstance per live object, as StageScene::draw builds them for a battle stage. The
  // pipeline clips at the near plane, writes per-pixel depth (camera z / 16 + OT bias, the particle batch's slot units) and
  // draws the opaque faces first, then the blended ones back to front.
  // The mod overlay changed: re-apply the tile replacements and the models' texture replacements live.
  if (meshes_ && tile_gen_ != render::texture_registry::generation()) {
    tile_gen_ = render::texture_registry::generation();
    if (stage_mesh_) {
      render::apply_tile_textures(*meshes_, "title");
      stage_pipe_.reload_textures(*stage_mesh_, *meshes_);
    }
    reload_model_textures();
  }
  if (stage_mesh_) {
    std::vector<effects::EmitterSystem::MeshInstance> live;
    em_->collect_meshes(vp, alpha, live);
    inst_.clear();
    inst_.reserve(live.size());
    // Debug: comma list of mesh names to draw.
    static const char* only_dbg = std::getenv("OC_STAGE_ONLY");
    for (const auto& mi : live) {
      // Model objects are drawn below with the mesh pipeline.
      if (mi.sprite_type == 20) continue;
      const auto pit = parts_.find(*mi.name);
      if (pit == parts_.end()) continue;
      if (mi.off_screen(meshes_->parts[static_cast<size_t>(pit->second)].cull_points, vp)) continue;
      if (only_dbg && std::string_view(only_dbg).find(*mi.name) == std::string_view::npos) continue;
      // Debug: comma list of mesh names not to draw.
      static const char* skip_dbg = std::getenv("OC_STAGE_SKIP");
      if (skip_dbg && std::string_view(skip_dbg).find(*mi.name) != std::string_view::npos) continue;
      render::StageInstance in;
      in.part = pit->second;
      for (size_t k = 0; k < 9; ++k) in.cam[k] = mi.mat.m[k];
      for (size_t k = 0; k < 3; ++k) in.cam[9 + k] = mi.centre[k];
      for (size_t k = 0; k < 3; ++k) in.rgb[k] = mi.rgb[k];
      in.blend = mi.blend;
      in.ext = mi.ext_path;
      in.ext_morph = mi.morph_name != nullptr;
      in.bias = mi.bias;
      in.fixed_slot = mi.fixed_slot;
      // Op 37 morph (fish fis1 -> fish, jellyfish hugu -> hug1, eel utub -> utu1, kura -> kur1).
      if (mi.morph_name) {
        const auto mt = parts_.find(*mi.morph_name);
        if (mt != parts_.end()) {
          in.morph_part = mt->second;
          in.morph_t = mi.morph_t_prev + (mi.morph_t - mi.morph_t_prev) * alpha;
        }
      }
      // Extended objects (op 35): ext bit 4 reverses the cull side, bit 5 draws both sides; bit 0 = op-36 UV scroll.
      if (mi.ext & 0x20) in.cull = 2;
      else if (mi.ext & 0x10) in.cull = 1;
      // Debug: swap | none overrides the cull side.
      static const char* cull_dbg = std::getenv("OC_STAGE_CULL");
      if (cull_dbg) in.cull = std::string_view(cull_dbg) == "swap" ? 1 : std::string_view(cull_dbg) == "none" ? 2 : 0;
      if (mi.ext & 1) {
        in.uv_scroll = true;
        in.uv_phase = mi.uv_phase;
        if (mi.owner >= 0 && static_cast<size_t>(mi.owner) < em_->header_count())
          for (const auto& op : em_->header_def(static_cast<size_t>(mi.owner)).ops)
            if (op.op == 0x36)
              if (auto* scroll = std::get_if<assets::FxUvScroll>(&op.arg)) in.uv_window = {scroll->off_u, scroll->off_v, scroll->win_w, scroll->win_h};
      }
      inst_.push_back(in);
    }
    if (!inst_.empty()) {
      const render::Rect scissor = sea_scissor(lb);
      const render::Mat4 proj = render::StagePipeline::projection(lb, vp.h, vp.ox, vp.oy);
      stage_pipe_.begin(frame, scissor);
      stage_pipe_.draw(*stage_mesh_, proj, inst_, linear, meshes_.get());
      stage_pipe_.end();
    }
  }

  // Model objects (sprite type 20): the skinned mesh pipeline with the same camera and depth units (camera z / 16), after the
  // scenery and before the sprites. Model space is y down like every Mdl; the world of the mesh pipeline is y up, so the
  // object's PS1 position / matrix are mirrored: model = T(x, -y, z) * F * (R * S), F = diag(1, -1, 1).
  if (mesh_pipe_ && !models_.empty()) {
    std::vector<effects::EmitterSystem::MeshInstance> live;
    em_->collect_meshes(vp, alpha, live);
    const CamPose pose = free_or_shot_pose(camera_, alpha);
    const V3 forward = norm3({pose.target[0] - pose.eye[0], pose.target[1] - pose.eye[1], pose.target[2] - pose.eye[2]}, {0, 0, 1});
    V3 right = norm3(cross3({0, 1, 0}, forward), {1, 0, 0});
    V3 down = cross3(forward, right);
    for (float& comp : down) comp = -comp;
    const float cr = std::cos(pose.roll_deg * kPi / 180.0f), sr = std::sin(pose.roll_deg * kPi / 180.0f);
    for (size_t i = 0; i < 3; ++i) {
      const float ri = right[i] * cr + down[i] * sr, di = -right[i] * sr + down[i] * cr;
      right[i] = ri;
      down[i] = di;
    }
    render::Mat4 view;
    for (int col = 0; col < 3; ++col) {
      view.at(0, col) = right[static_cast<size_t>(col)];
      view.at(1, col) = down[static_cast<size_t>(col)];
      view.at(2, col) = forward[static_cast<size_t>(col)];
    }
    view.at(0, 3) = -dot3(right, pose.eye);
    view.at(1, 3) = -dot3(down, pose.eye);
    view.at(2, 3) = -dot3(forward, pose.eye);
    const render::Mat4 proj = render::StagePipeline::projection(lb, vp.h, vp.ox, vp.oy);
    const render::Mat4 view_proj = proj * view;
    const render::Rect scissor = sea_scissor(lb);
    bool open = false;
    u32 drawn = 0;
    for (const auto& mi : live) {
      if (mi.sprite_type != 20 || mi.off_screen(vp)) continue;
      const auto it = models_.find(*mi.name);
      if (it == models_.end()) continue;
      ModelRes& res = *it->second;
      render::Mat4 model = render::mat4_translation(mi.world_pos[0], -mi.world_pos[1], mi.world_pos[2]);
      render::Mat4 rs;
      for (int rr = 0; rr < 3; ++rr)
        // F * (R S)
        for (int col = 0; col < 3; ++col) rs.at(rr, col) = mi.world_mat.m[static_cast<size_t>(rr * 3 + col)] * (rr == 1 ? -1.0f : 1.0f);
      model = model * rs;
      // Debug: rest pose instead of the motion clip.
      static const bool rest_dbg = std::getenv("OC_TITLE_MODEL_REST") != nullptr;
      res.pose.palette_at(rest_dbg ? -1 : res.clip, mi.motion_prev + (mi.motion - mi.motion_prev) * alpha, palette_);
      if (!open) {
        mesh_pipe_->begin(frame, scissor);
        open = true;
      }
      render::MeshPipeline::DrawParams dp;
      dp.view_proj = view_proj;
      dp.view = view;
      dp.model = model;
      dp.palette = palette_;
      dp.linear = linear;
      dp.hd = false;
      for (size_t ch = 0; ch < 3; ++ch) dp.tint[ch] = static_cast<float>(mi.rgb[ch]) / 128.0f;
      mesh_pipe_->draw(*res.gpu, dp);
      ++drawn;
    }
    if (open) mesh_pipe_->end();
    if (drawn && !models_logged_) {
      models_logged_ = true;
      log::info("title backdrop: first model object drawn at tick {} ({} live)", ticks_, drawn);
    }
  }

  // Sprite objects (type 3): ordering-table order, depth-tested against the meshes.
  std::vector<effects::DrawQuad> quads;
  em_->collect_quads(vp, alpha, quads);
  if (quads.empty()) return;
  effects::sort_draw_order(quads);
  particles_.begin(frame, view_w, view_h);
  for (const effects::DrawQuad& quad : quads) gpu_.draw_quad(particles_, quad, lb);
  particles_.end();
}

}  // namespace oc::game
