#include "game/battle/stage_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <variant>
#include <chrono>

#include "core/log.h"
#include "import/battle_stage.h"
#include "import/container.h"
#include "import/room_effects.h"
#include "render/stage_tiles.h"
#include "render/texture_registry.h"

namespace oc::game {

bool StageScene::load(render::GpuContext& ctx, render::StagePipeline& pipe, ByteSpan prd, ByteSpan ctd, std::string* err, render::SpriteBatch* sprites,
                      std::vector<const assets::RoomEffects*> libs) {
  load_begin(ctx, pipe, prd, ctd, sprites, std::move(libs));
  for (;;) {
    const LoadResult result = load_step(1e9, err);
    if (result != LoadResult::More) return result == LoadResult::Done;
  }
}

// Phase 0 of the load, on a worker thread: decode, tile library + per-object textures, the mods' tile replacements (the object images re-composed), then
// the GPU textures, created and uploaded.
StageScene::Prepared StageScene::prepare(render::GpuContext& ctx, const Bytes& prd, const Bytes& ctd, const std::string& group) {
  Prepared out;
  out.generation = render::texture_registry::generation();
  auto st = import::load_stage(ByteSpan(prd), ByteSpan(ctd), &out.error);
  if (!st) return out;
  // Debug: OC_STAGE_ONLY keeps only the parts named in the comma list.
  if (const char* only = std::getenv("OC_STAGE_ONLY")) {
    const std::string keep = std::string(",") + only + ",";
    std::erase_if(st->parts, [&](const assets::StagePart& part) { return keep.find("," + part.name + ",") == std::string::npos; });
  }
  // Debug: OC_NO_BAKE skips the per-object bake (draws the raw atlases; for before / after pixel comparisons).
  static const bool no_bake = std::getenv("OC_NO_BAKE") != nullptr;
  if (!no_bake) {
    render::bake_object_textures(*st);
    render::apply_tile_textures(*st, group);
    out.baked = true;
  }
  out.textures = render::prepare_stage_textures(ctx, *st);
  out.stage = std::move(*st);
  return out;
}

// The load in phases: 0 decode the stage (worker thread), 1 mesh textures (a few per call), 2 mesh buffers + ground,
// 3 object scripts + sheet atlas start, 4 sheet atlases (a few per call), 5 emitters + pre-roll. `prd` / `ctd` must stay valid until load_step() stops returning More.
void StageScene::load_begin(render::GpuContext& ctx, render::StagePipeline& pipe, ByteSpan prd, ByteSpan ctd, render::SpriteBatch* sprites,
                            std::vector<const assets::RoomEffects*> libs) {
  clear();
  ctx_ = &ctx;
  ld_ = LoadState{};
  ld_.on = true;
  ld_.pipe = &pipe;
  ld_.prd = prd;
  ld_.ctd = ctd;
  ld_.sprites = sprites;
  ld_.libs = std::move(libs);
}

StageScene::LoadResult StageScene::load_step(double budget_ms, std::string* err) {
  if (!ld_.on) return LoadResult::Done;
  const auto t0 = std::chrono::steady_clock::now();
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    clear();
    return LoadResult::Failed;
  };
  const auto spent = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
  const ByteSpan prd = ld_.prd, ctd = ld_.ctd;
  render::SpriteBatch* sprites = ld_.sprites;
  if (ld_.phase == 0) {
  // The worker owns copies of the files: a load abandoned mid-way may free the caller's.
  if (!ld_.prepared.valid())
    ld_.prepared = std::async(std::launch::async, [ctx = ctx_, prd_copy = Bytes(prd.begin(), prd.end()), ctd_copy = Bytes(ctd.begin(), ctd.end()), group = ld_.pipe->scene_group()] {
      return prepare(*ctx, prd_copy, ctd_copy, group);
    });
  if (budget_ms < 1e6 && ld_.prepared.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return LoadResult::Wait;
  Prepared prepared = ld_.prepared.get();
  if (!prepared.stage) return fail(prepared.error);
  if (prepared.baked) {
    tile_group_ = ld_.pipe->scene_group();
    tile_gen_ = prepared.generation;
  }
  ld_.textures = std::move(prepared.textures);
  asset_ = std::make_shared<assets::StageAsset>(std::move(*prepared.stage));
  // Stage light for the unit shadows: the `sun0` object's position; without one the default stays:
  // (0, -0xC00, 0) in PS1 space = 3072 units above the arena centre.
  light_ = {0.0f, 3072.0f, 0.0f};
  for (const assets::StageEmitter& em : asset_->emitters)
    if (em.sprite == "sun0" && em.placed) {
      light_ = {static_cast<float>(em.pos[0]), -static_cast<float>(em.pos[1]), static_cast<float>(em.pos[2])};
      log::info("stage: light from sun0 at ({}, {}, {})", light_[0], light_[1], light_[2]);
      break;
    }
  key_light_ = game::key_light(*asset_);
  // Names are not unique across kinds (bg_34 has a scenery `bg34` and the ground `bg34`): the PS1 registry keys them by list type.
  for (size_t i = 0; i < asset_->parts.size(); ++i) (asset_->parts[i].ground ? ground_parts_ : parts_).emplace(asset_->parts[i].name, static_cast<int>(i));
    ld_.phase = 1;
    if (spent() >= budget_ms) return LoadResult::More;
  }
  // Phase 1: the mesh's textures, a few per call.
  if (ld_.phase == 1) {
    if (!ld_.mesh_job) {
      ld_.mesh_job = std::make_unique<render::StageCreateJob>(ld_.pipe->create_begin());
      ld_.mesh_job->prepared = std::move(ld_.textures);
    }
    if (!ld_.pipe->create_textures(*ld_.mesh_job, *asset_, std::max(0.5, budget_ms - spent()))) return LoadResult::More;
    ld_.phase = 2;
    if (spent() >= budget_ms) return LoadResult::More;
  }
  if (ld_.phase == 2) {
  mesh_ = ld_.pipe->create_finish(*ld_.mesh_job, *asset_);
  ld_.mesh_job.reset();
  if (!mesh_) return fail("no geometry");
  // The ground object, built from the drp's first type-0x12 entry: every FT4 / GT4 packet of the
  // ring becomes the cell record at its corner average (uv words, CLUT, tpage, four colours; FT4 repeats its colour).
  ground_ = effects::FxGround{};
  ground_part_ = -1;
  for (size_t i = 0; i < asset_->parts.size() && ground_part_ < 0; ++i)
    if (asset_->parts[i].ground) ground_part_ = static_cast<int>(i);
  if (ground_part_ >= 0)
    if (auto drp = import::parse_drp(prd); drp && asset_->parts[static_cast<size_t>(ground_part_)].entry < drp->entries.size())
      if (auto ground_mesh = import::parse_stage_mesh(drp->entries[asset_->parts[static_cast<size_t>(ground_part_)].entry].data)) {
        int cells = 0;
        std::vector<effects::FxGroundTri> gt3;
        for (const import::StageFace& face : ground_mesh->faces) {
          // The rim: FT3 and GT3 triangles go to separate lists (file order inside each list).
          if (face.n == 3 && face.textured) {
            effects::FxGroundTri tri;
            for (size_t k = 0; k < 3; ++k) {
              tri.v[k] = ground_mesh->verts[face.idx[k]];
              tri.uv[k][0] = face.uv[k][0];
              tri.uv[k][1] = face.uv[k][1];
              for (size_t j = 0; j < 3; ++j) tri.rgb[k][j] = face.rgb[k][j];
            }
            tri.clut = face.clut;
            tri.tpage = face.tpage;
            tri.gouraud = face.code == 0x34;
            (tri.gouraud ? gt3 : ground_.rim).push_back(tri);
            continue;
          }
          if (face.n != 4 || !face.textured) continue;
          int sx = 0, sz = 0;
          for (size_t k = 0; k < 4; ++k) {
            sx += static_cast<s16>(ground_mesh->verts[face.idx[k]][0] + 0x1400);
            sz += static_cast<s16>(ground_mesh->verts[face.idx[k]][2] - 0x1400);
          }
          int ax = sx >> 2, az = sz >> 2;
          if (ax < 0) ax += 0x3FF;
          if (az < 0) az += 0x3FF;
          const int idx = (ax >> 10) - 10 * (az >> 10);
          // The PS1 would write out of bounds; no stage does it.
          if (idx < 0 || idx >= 100) continue;
          effects::FxGroundCell& ground_cell = ground_.cells[static_cast<size_t>(idx)];
          ground_cell.present = true;
          ground_cell.clut = face.clut;
          ground_cell.tpage = face.tpage;
          for (size_t k = 0; k < 4; ++k) {
            ground_cell.uv[k][0] = face.uv[k][0];
            ground_cell.uv[k][1] = face.uv[k][1];
            for (size_t j = 0; j < 3; ++j) ground_cell.rgb[k][j] = face.rgb[k][j];
          }
          ++cells;
        }
        ground_.rim.insert(ground_.rim.end(), gt3.begin(), gt3.end());
        ground_.present = cells > 0;
        log::info("stage: ground '{}': {} cells", asset_->parts[static_cast<size_t>(ground_part_)].name, cells);
      }
  effects::set_fx_ground(&ground_);
    ld_.phase = 3;
    if (spent() >= budget_ms) return LoadResult::More;
  }
  if (ld_.phase == 3) {
  // The object scripts (type 0x0A) and sequences of the same drp; the sprite sheets they name (smoke / dust: battle.prd's resident packs and
  // effect/particle.prd) come from the libraries the caller attaches.
  import::Vram empty;
  fx_ = import::load_battle_effects(prd, ctd, empty, {});
  for (const auto& message : fx_.errors) log::debug("stage: {}", message);
  bank_ = effects::EffectBank{};
  bank_.add(&fx_);
  for (const assets::RoomEffects* lib : ld_.libs) bank_.add_library(lib);
  sprites_ok_ = false;
  if (sprites) {
    gpu_.clear();
    sprites_ok_ = gpu_.init(*ctx_, *sprites) && gpu_.load_begin();
  }
    ld_.phase = sprites_ok_ ? 4 : 5;
    if (spent() >= budget_ms) return LoadResult::More;
  }
  // Phase 4: the sheet atlases (hundreds, shared with the effect library), as many as the budget allows.
  if (ld_.phase == 4) {
    bool done = false;
    if (!gpu_.load_more(bank_, effects::GpuEffects::Which::All, std::max(0.5, budget_ms - spent()), done)) {
      sprites_ok_ = false;
      done = true;
    }
    if (!done) return LoadResult::More;
    ld_.phase = 5;
    if (spent() >= budget_ms) return LoadResult::More;
  }
  if (sprites && sprites_ok_ && !particles_ready_) particles_ready_ = particles_.init(*ctx_);
  if (sprites && !(sprites_ok_ && particles_ready_)) {
    sprites_ok_ = false;
    log::warn("stage: sprite sheets unavailable");
  }
  effects::EmitterSystem::Config cfg;
  cfg.seed = 0x5347u;
  cfg.pos_shift = pos_shift_;
  em_ = std::make_unique<effects::EmitterSystem>(bank_, cfg);
  effects::ActorHooks ah;
  ah.anchor_basis = [](int, effects::Basis& out) {
    out = effects::Basis{};
    return true;
  };
  em_->set_hooks(std::move(ah));
  em_->register_all();
  // The 'ring' emitter's COLOR record (op 0x20, 12-bit): tekken tints its rocks with it.
  for (size_t i = 0; i < em_->header_count(); ++i)
    if (em_->header_def(i).name == "ring") {
      for (const auto& op : em_->header_def(i).ops)
        if (op.op == 0x20)
          if (auto* colour = std::get_if<assets::FxXyz>(&op.arg)) {
            ground_.ring_colour[0] = static_cast<u8>(static_cast<u16>(colour->x) >> 4);
            ground_.ring_colour[1] = static_cast<u8>(static_cast<u16>(colour->y) >> 4);
            ground_.ring_colour[2] = static_cast<u8>(static_cast<u16>(colour->z) >> 4);
            break;
          }
      break;
    }
  // A steady emitter fires `period + 1` ticks after registration; the PS1 loads the stage behind the
  // white transition, so the scenery is spawned here before the first frame.
  int pre = 2;
  for (const auto& em : fx_.emitters)
    if (!em.start_stopped()) pre = std::max(pre, static_cast<int>(em.period) + 2 + std::max<int>(0, em.start));
  for (int i = 0; i < std::min(pre, 256); ++i) em_->tick();
  const effects::EmitterStats stats = em_->stats();
  log::info("stage: {} parts, {} textures, {} emitters ({} mesh objects, {} live, {} sheet objects spawned, {} unresolved sprites)", asset_->parts.size(),
            asset_->textures.size(), fx_.emitters.size(), stats.mesh_objects, stats.live, stats.type_spawns[3], stats.unresolved_sprites);
  ld_ = LoadState{};
  return LoadResult::Done;
}

// ---- the battle ground -------------------------------------------------------------------------------------
int StageScene::ground_cell_texture(int cell) const {
  if (!asset_ || cell < 0 || cell >= 100 || !ground_.cells[static_cast<size_t>(cell)].present) return -1;
  const effects::FxGroundCell& ground_cell = ground_.cells[static_cast<size_t>(cell)];
  const auto cl = assets::decode_clut(ground_cell.clut);
  return asset_->find_texture(assets::decode_tpage(ground_cell.tpage).mode, cl[0], cl[1]);
}

// One step of the bias ramp (dt = 1): value =from + (to - from) * elapsed / ticks while ticks != 0, elapsed += 1; past `ticks` the target and the task ends.
void StageScene::ramp_step() {
  if (!ramp_.on) return;
  int bias = ground_.bias;
  if (ramp_.ticks != 0) bias = ramp_.from + (ramp_.to - ramp_.from) * ramp_.elapsed / ramp_.ticks;
  ramp_.elapsed += 1;
  if (ramp_.ticks < ramp_.elapsed) {
    bias = ramp_.to;
    ramp_.on = false;
  }
  ground_.bias = bias;
}

void StageScene::ground_bias_ramp(int ticks, int target) {
  // A new ramp replaces a running one.
  ramp_ = BiasRamp{true, 0, static_cast<s16>(ticks), ground_.bias, static_cast<s16>(target)};
  ramp_step();
}

void StageScene::ground_overlay_end() {
  ramp_ = BiasRamp{};
  ld_ = LoadState{};
  ground_.bias = 100;
  ground_.skip_stock = false;
  clear_ground_frame();
}

void StageScene::clear() {
  if (effects::fx_ground() == &ground_) effects::set_fx_ground(nullptr);
  ground_ = effects::FxGround{};
  ground_part_ = -1;
  ramp_ = BiasRamp{};
  // An abandoned load waits here for its worker (the future's destructor).
  ld_ = LoadState{};
  em_.reset();
  gpu_.clear();
  sprites_ok_ = false;
  bank_ = effects::EffectBank{};
  fx_ = assets::RoomEffects{};
  mesh_.reset();
  parts_.clear();
  ground_parts_.clear();
  inst_.clear();
  casters_.clear();
  footprints_.clear();
  shadow_verts_.clear();
  shadow_idx_.clear();
  light_ = {0.0f, 3072.0f, 0.0f};
  key_light_ = KeyLight{};
  floor_shadow_ = nullptr;
  asset_.reset();
  ctx_ = nullptr;
  drawn_ = 0;
}

void StageScene::tick() {
  if (em_) em_->tick();
  // The op 0x6D / 0x6E task.
  ramp_step();
}

bool StageScene::start_emitter(std::string_view name) { return em_ && em_->start(name); }

void StageScene::draw_sprites(const render::Frame& frame, render::Rect scissor, const effects::ViewParams& view, const render::Letterbox& lb, float alpha) {
  sprites_drawn_ = 0;
  if (!em_ || !sprites_ok_) return;
  // Debug logging (OC_STAGE_REJECT_LOG).
  static const bool log_sprites = std::getenv("OC_STAGE_REJECT_LOG") != nullptr;
  if (log_sprites) {
    const effects::EmitterStats st = em_->stats();
    log::info("stage: sprites: {} spawned, {} live, {} unresolved", st.type_spawns[3], st.live, st.unresolved_sprites);
  }
  std::vector<effects::DrawQuad> quads;
  em_->collect_quads(view, alpha, quads);
  if (quads.empty()) return;
  effects::sort_draw_order(quads);
  frame.set_scissor(scissor);
  particles_.begin(frame, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
  for (const effects::DrawQuad& quad : quads) gpu_.draw_quad(particles_, quad, lb);
  particles_.end();
  sprites_drawn_ = quads.size();
  if (log_sprites) log::info("stage: {} sprite quads", quads.size());
}

const StageScene::Footprint& StageScene::footprint(const assets::ModelAsset* model) {
  auto it = footprints_.find(model);
  if (it != footprints_.end()) return it->second;
  // The x/z extents of the model's joint positions; fallback from the bind pose
  // (rest translations accumulated down the parent chain, rotations ignored) when the caller has no posed palette.
  Footprint fp;
  bool any = false;
  std::vector<assets::Vec3> pos;
  if (model)
    for (size_t j = 0; j < model->skeleton.joints.size(); ++j) {
      const assets::Joint& jt = model->skeleton.joints[j];
      assets::Vec3 joint_pos = jt.rest_trans;
      if (jt.parent >= 0 && static_cast<size_t>(jt.parent) < pos.size())
        for (size_t k = 0; k < 3; ++k) joint_pos[k] += pos[static_cast<size_t>(jt.parent)][k];
      pos.push_back(joint_pos);
      if (!any) {
        fp.min_x = fp.max_x = joint_pos[0];
        fp.min_z = fp.max_z = joint_pos[2];
        any = true;
      }
      fp.min_x = std::min(fp.min_x, joint_pos[0]);
      fp.max_x = std::max(fp.max_x, joint_pos[0]);
      fp.min_z = std::min(fp.min_z, joint_pos[2]);
      fp.max_z = std::max(fp.max_z, joint_pos[2]);
    }
  return footprints_.emplace(model, fp).first->second;
}

// Builds the drop-shadow geometry: hull, light projection, rounding, rings and colours (steps below).
void StageScene::build_shadow(const ShadowCaster& caster, const Footprint& fp, const std::array<float, 3>& light, std::vector<assets::StageVertex>& verts,
                              std::vector<u32>& indices, float grey_scale) {
  // Model-local x / z on the floor.
  using Point2 = std::array<float, 2>;
  // 1. Footprint rectangle padded by 0x80.
  const float pad = 128.0f;
  std::vector<Point2> pts = {{fp.min_x - pad, fp.min_z - pad}, {fp.max_x + pad, fp.min_z - pad}, {fp.max_x + pad, fp.max_z + pad}, {fp.min_x - pad, fp.max_z + pad}};
  // 2. The model's top (unit position, height above the floor) cast along the light onto the floor: the
  //    horizontal offset is h * d / (H - h) along the light -> top direction, 0 for a light straight above, 0xC00 at most
  //    (or when the light is not above the top); |components| > 0x4000 fall back to straight down.
  {
    const float top[3] = {caster.x, caster.height + caster.y, caster.z};
    float dir[3] = {top[0] - light[0], top[1] - light[1], top[2] - light[2]};
    if (std::fabs(dir[0]) > 16384.0f || std::fabs(dir[1]) > 16384.0f || std::fabs(dir[2]) > 16384.0f) dir[0] = 0, dir[1] = -4096.0f, dir[2] = 0;
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    const float hd = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
    if (len > 0.0f) {
      float ext = 0;
      // ny < 0: the light is above the top.
      const float ny = dir[1] / len;
      if (ny >= -1.0f / 4096.0f) ext = 3072.0f;
      else if (hd < 33.0f) ext = 0;
      else {
        const float dy = light[1] - top[1];
        ext = dy > 0 ? top[1] * hd / dy : 3072.0f;
        ext = std::min(ext, 3072.0f);
      }
      // World offset from the unit.
      const float px = top[0] + dir[0] / len * ext - caster.x, pz = top[2] + dir[2] / len * ext - caster.z;
      // Into model-local space: the model is drawn with rotation_y(yaw) (x' = c x + s z, z' = -s x + c z); invert it.
      const float cs = std::cos(caster.yaw), sn = std::sin(caster.yaw);
      const Point2 local{cs * px - sn * pz, sn * px + cs * pz};
      const bool inside = local[0] >= pts[0][0] && local[0] <= pts[1][0] && local[1] >= pts[0][1] && local[1] <= pts[2][1];
      if (!inside) pts.push_back(local);
    }
  }
  // 3. Convex hull (the PS1 picks it from an outcode table), counter-clockwise, as a plain monotone chain.
  std::sort(pts.begin(), pts.end());
  std::vector<Point2> hull;
  auto cross = [](const Point2& origin, const Point2& lhs, const Point2& rhs) { return (lhs[0] - origin[0]) * (rhs[1] - origin[1]) - (lhs[1] - origin[1]) * (rhs[0] - origin[0]); };
  for (const Point2& point : pts) {
    while (hull.size() >= 2 && cross(hull[hull.size() - 2], hull.back(), point) <= 0) hull.pop_back();
    hull.push_back(point);
  }
  const size_t lower = hull.size() + 1;
  for (size_t i = pts.size() - 1; i-- > 0;) {
    while (hull.size() >= lower && cross(hull[hull.size() - 2], hull.back(), pts[i]) <= 0) hull.pop_back();
    hull.push_back(pts[i]);
  }
  hull.pop_back();
  if (hull.size() < 3) return;
  // 4. Rounded outline: for every corner B with neighbours A, C the PS1 walks t = 0..1 in quarters along
  //    (quadratic Bezier(A, B, C) + B) / 2 (weights 8(1-t)^2, 16t(1-t) + 8, 8t^2 over 16), i.e. from the midpoint of AB to
  //    the midpoint of BC bulging toward B; the pieces join at the edge midpoints.
  std::vector<Point2> rim;
  const size_t hull_count = hull.size();
  for (size_t i = 0; i < hull_count; ++i) {
    const Point2& corner_a = hull[(i + hull_count - 1) % hull_count];
    const Point2& corner_b = hull[i];
    const Point2& corner_c = hull[(i + 1) % hull_count];
    for (int k = 0; k < 4; ++k) {
      const float along = static_cast<float>(k) / 4.0f;
      const float weight_a = 8.0f * (1 - along) * (1 - along), weight_b = 16.0f * along * (1 - along) + 8.0f, weight_c = 8.0f * along * along;
      rim.push_back({(weight_a * corner_a[0] + weight_b * corner_b[0] + weight_c * corner_c[0]) / 16.0f, (weight_a * corner_a[1] + weight_b * corner_b[1] + weight_c * corner_c[1]) / 16.0f});
    }
  }
  // 5. Grey level (0xFF) fades as the unit rises: grey -= y * grey / 1024, floor 8.
  float grey = 255.0f;
  if (caster.y > 0) grey = std::max(8.0f, 255.0f - caster.y * 255.0f / 1024.0f);
  grey *= grey_scale;
  float radius = 0;
  for (const Point2& point : rim) radius = std::max(radius, std::sqrt(point[0] * point[0] + point[1] * point[1]));
  // Near hulls (radius < 0x400): a fan from the centre, centre colour grey, rim 0. Far hulls: the
  // outline shrunk toward the centre in quarters, four bands coloured 3q / 2q / q / 0 outward with q = grey / 4.
  const bool far = radius >= 1024.0f;
  const int bands = far ? 4 : 1;
  float level[5] = {grey, 0, 0, 0, 0};
  if (far) {
    const float quarter = std::floor(grey / 4.0f);
    // level[0]: centre disc and the inner edge of band 1.
    level[0] = 3 * quarter;
    level[1] = 3 * quarter;
    level[2] = 2 * quarter;
    level[3] = quarter;
    level[4] = 0;
  }
  // Model-local -> world (rotation_y(yaw) then translate), y = 0 on the floor.
  const float cs = std::cos(caster.yaw), sn = std::sin(caster.yaw);
  auto emit = [&](const Point2& point, float lvl) {
    assets::StageVertex vertex;
    vertex.pos = {caster.x + cs * point[0] + sn * point[1], 0.0f, caster.z - sn * point[0] + cs * point[1]};
    const float col = std::clamp(lvl / 255.0f, 0.0f, 1.0f);
    vertex.color = {col, col, col};
    vertex.uv = {0.5f, 0.5f};
    vertex.uv_raw = {0, 0};
    vertex.uv_scale = {0, 0};
    verts.push_back(vertex);
    return static_cast<u32>(verts.size() - 1);
  };
  const size_t rim_count = rim.size();
  const u32 centre = emit({0, 0}, level[0]);
  // Ring vertices from the inside out: ring `band` at fraction band / bands of the rim.
  // prev: the previous (innermost first) ring, fraction 1 / bands.
  std::vector<u32> prev;
  for (int band = 1; band <= bands; ++band) {
    const float fraction = static_cast<float>(band) / static_cast<float>(bands);
    const float lvl = far ? level[band] : 0.0f;
    std::vector<u32> ring;
    ring.reserve(rim_count);
    for (const Point2& point : rim) ring.push_back(emit({point[0] * fraction, point[1] * fraction}, lvl));
    if (band == 1) {
      for (size_t i = 0; i < rim_count; ++i) {
        indices.push_back(centre);
        indices.push_back(ring[i]);
        indices.push_back(ring[(i + 1) % rim_count]);
      }
    } else {
      for (size_t i = 0; i < rim_count; ++i) {
        const u32 inner_a = prev[i], inner_b = prev[(i + 1) % rim_count], outer_a = ring[i], outer_b = ring[(i + 1) % rim_count];
        indices.push_back(inner_a);
        indices.push_back(outer_a);
        indices.push_back(outer_b);
        indices.push_back(inner_a);
        indices.push_back(outer_b);
        indices.push_back(inner_b);
      }
    }
    prev.swap(ring);
  }
}

namespace {
constexpr float kContactShade = 0.3f;
}

void StageScene::draw(render::StagePipeline& pipe, const render::Frame& frame, render::Rect scissor, const effects::ViewParams& view,
                      const render::Letterbox& lb, float alpha, bool linear) {
  drawn_ = 0;
  if (!mesh_ || !em_) return;
  // When the mod overlay changed, re-apply the tile replacements live.
  if (asset_ && !tile_group_.empty() && tile_gen_ != render::texture_registry::generation()) {
    tile_gen_ = render::texture_registry::generation();
    render::apply_tile_textures(*asset_, tile_group_);
    pipe.reload_textures(*mesh_, *asset_);
  }
  std::vector<effects::EmitterSystem::MeshInstance> live;
  em_->collect_meshes(view, alpha, live);
  warp_fn_ = nullptr;
  const bool mark_warp_objects = static_cast<bool>(ground_.warp) && ground_.warp_quarter_add;
  if (mark_warp_objects != warp_objects_marked_) {
    for (const std::string& name : mark_warp_objects ? ground_.warp_emitters : marked_emitters_) em_->mark_mesh_objects(name, mark_warp_objects);
    marked_emitters_ = mark_warp_objects ? ground_.warp_emitters : std::vector<std::string>{};
    warp_objects_marked_ = mark_warp_objects;
  }
  if (ground_.warp)
    warp_fn_ = [this](std::array<float, 3>& point) {
      effects::Vec3 warped{point[0], point[1], point[2]};
      ground_.warp(warped);
      point = {warped[0], warped[1], warped[2]};
    };
  inst_.clear();
  inst_.reserve(live.size());
  for (const auto& mi : live) {
    // A zero name word takes the first registered entry of the type: for the ground, the stage's one ground object.
    const auto& table = mi.sprite_type == 19 ? ground_parts_ : parts_;
    const auto pit = table.find(*mi.name);
    int part = pit != table.end() ? pit->second : -1;
    if (part < 0 && mi.sprite_type == 19 && mi.name->empty()) part = ground_part_;
    if (part < 0) continue;
    if (mi.off_screen(asset_->parts[static_cast<size_t>(part)].cull_points, view)) continue;
    // The ground takes the object's colour.
    if (mi.sprite_type == 19) {
      ground_rgb_ = {mi.rgb[0], mi.rgb[1], mi.rgb[2]};
      for (size_t k = 0; k < 3; ++k) ground_.rgb[k] = mi.rgb[k];
    }
    // With skip_stock set the stock draw leaves the ground out.
    if (mi.sprite_type == 19 && ground_.skip_stock) continue;
    render::StageInstance in;
    in.part = part;
    for (size_t k = 0; k < 9; ++k) in.cam[k] = mi.mat.m[k];
    for (size_t k = 0; k < 3; ++k) in.cam[9 + k] = mi.centre[k];
    for (size_t k = 0; k < 3; ++k) in.rgb[k] = mi.rgb[k];
    in.blend = mi.blend;
    in.ext = mi.ext_path;
    in.ext_morph = mi.morph_name != nullptr;
    in.bias = mi.bias;
    in.fixed_slot = mi.fixed_slot;
    // Op 37: the morph target is a type-0x02 mesh of the same drp.
    if (mi.morph_name) {
      const auto mt = parts_.find(*mi.morph_name);
      if (mt != parts_.end()) {
        in.morph_part = mt->second;
        in.morph_t = mi.morph_t_prev + (mi.morph_t - mi.morph_t_prev) * alpha;
      }
    }
    // Extended objects (op 35): ext bit 4 reverses the cull side, bit 5 draws both sides; bit 0 = op-36 UV scroll
    // with the phase in 24.8 texels masked to 0..127.
    if (mi.ext & 0x20) in.cull = 2;
    else if (mi.ext & 0x10) in.cull = 1;
    // Debug: OC_STAGE_CULL = swap | none overrides the cull side of every object.
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
    // Op 06 flag bit 17: depth-cue the colour.
    if (mi.owner >= 0 && static_cast<size_t>(mi.owner) < em_->header_count())
      for (const auto& op : em_->header_def(static_cast<size_t>(mi.owner)).ops)
        if (op.op == 0x06)
          if (auto* flags = std::get_if<assets::FxFlags>(&op.arg)) in.depth_cue = (flags->flags >> 17) & 1;
    if (ground_.warp && (!ground_.warp_quarter_add || mi.marked) && mi.sprite_type == 14 && mi.owner >= 0 && static_cast<size_t>(mi.owner) < em_->header_count() &&
        std::find(ground_.warp_emitters.begin(), ground_.warp_emitters.end(), em_->header_def(static_cast<size_t>(mi.owner)).name) !=
            ground_.warp_emitters.end()) {
      in.warp = &warp_fn_;
      if (ground_.warp_quarter_add && in.blend != 0) in.blend = 4;
      for (size_t k = 0; k < 9; ++k) in.warp_view[k] = view.rot.m[k];
      for (size_t k = 0; k < 3; ++k) in.warp_view[9 + k] = view.trans[k];
    }
    // The ground object: its own bias, cued whenever the fog flag is up. The extended-object bits belong to the mesh draw only: the cells
    // and rim keep the default cull side and never scroll.
    if (mi.sprite_type == 19) {
      in.ground = true;
      in.cull = 0;
      in.uv_scroll = false;
      in.bias = ground_.bias;
      in.fixed_slot = false;
      in.depth_cue = true;
      in.blend = mi.blend != 0 ? 1 : 0;
    }
    inst_.push_back(in);
  }
  // The overlays' own ground draws this tick (with the view matrix): one draw shows every cell that any of the rectangles keeps.
  if (!ground_.draws.empty() && ground_part_ >= 0 && !(!ground_.skip_stock && std::any_of(inst_.begin(), inst_.end(), [](const auto& instance) { return instance.ground; }))) {
    render::StageInstance in;
    in.part = ground_part_;
    for (size_t k = 0; k < 9; ++k) in.cam[k] = view.rot.m[k];
    for (size_t k = 0; k < 3; ++k) in.cam[9 + k] = view.trans[k];
    in.rgb = ground_rgb_;
    in.ground = true;
    in.bias = ground_.bias + ground_.draw_bias_add;
    in.depth_cue = true;
    // The 46black effect's own ground copy: every vertex through the warp, in world space (the instance is already the view).
    if (ground_.warp) {
      in.warp = &warp_fn_;
      in.warp_view = in.cam;
    }
    for (int cell = 0; cell < 100; ++cell) {
      const int x = -0x1400 + 0x400 * (cell % 10), z = 0x1400 - 0x400 * (cell / 10);
      bool hidden = true;
      for (const auto& rect : ground_.draws)
        if (!(x >= rect[0] && x < rect[2] && z > rect[3] && z <= rect[1])) hidden = false;
      in.hide.set(static_cast<size_t>(cell), hidden);
    }
    inst_.push_back(in);
  }
  const render::Mat4 proj = render::StagePipeline::projection(lb, view.h, view.ox, view.oy);
  pipe.begin(frame, scissor);
  // The PS1 packet builders' rejection rules.
  pipe.set_gte(view.h, view.ox, view.oy);
  pipe.set_depth_cue(cue_);
  // [Enhancements] Lighting: the view's PS1 world -> camera transform (the stage objects take the same cam).
  if (spell_lights_.active()) {
    std::array<float, 12> vcam;
    for (size_t k = 0; k < 9; ++k) vcam[k] = view.rot.m[k];
    for (size_t k = 0; k < 3; ++k) vcam[9 + k] = view.trans[k];
    pipe.set_lights(spell_lights_, vcam);
  }
  if (!inst_.empty()) {
    drawn_ = inst_.size();
    pipe.draw(*mesh_, proj, inst_, linear, asset_.get());
  }
  // The overlays' own pack meshes baked from the ground (FxGroundPrim), drawn with the stage atlases through the view.
  if (!ground_.prims.empty()) {
    std::array<float, 12> cam;
    for (size_t k = 0; k < 9; ++k) cam[k] = view.rot.m[k];
    for (size_t k = 0; k < 3; ++k) cam[9 + k] = view.trans[k];
    for (const bool camera_space : {false, true}) {
    prims_.clear();
    for (const effects::FxGroundPrim& ground_prim : ground_.prims) {
      if (ground_prim.camera_space != camera_space) continue;
      render::StagePrim prim;
      prim.n = ground_prim.n;
      for (size_t k = 0; k < 4; ++k) {
        prim.pos[k] = {ground_prim.p[k][0], ground_prim.p[k][1], ground_prim.p[k][2]};
        prim.uv[k] = {ground_prim.uv[k][0], ground_prim.uv[k][1]};
        prim.rgb[k] = {ground_prim.rgb[k][0], ground_prim.rgb[k][1], ground_prim.rgb[k][2]};
      }
      prim.clut = ground_prim.clut;
      prim.tpage = ground_prim.tpage;
      prim.textured = ground_prim.textured;
      prim.blend = ground_prim.blend;
      prim.bias = ground_prim.bias;
      prim.two_sided = ground_prim.two_sided;
      prims_.push_back(prim);
    }
    static const std::array<float, 12> kIdentity{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    pipe.draw_prims(*mesh_, *asset_, proj, camera_space ? kIdentity : cam, prims_, linear);
    }
  }
  rejected_ = pipe.rejected_packets();
  // Debug: OC_BESPOKE_TRACE logs the overlays' ground state every 60 frames.
  static const bool trace = std::getenv("OC_BESPOKE_TRACE") != nullptr;
  static u32 trace_count = 0;
  if (trace && ground_.present && (trace_count++ % 60) == 0) {
    size_t warped = 0, grounds = 0, hidden = 0;
    for (const auto& instance : inst_) {
      warped += instance.warp != nullptr;
      if (instance.ground) ++grounds, hidden += instance.hide.count();
    }
    log::debug("stage ground: skip {} bias {} draws {} prims {} ground instances {} hidden cells {} warped instances {}", ground_.skip_stock, ground_.bias,
               ground_.draws.size(), ground_.prims.size(), grounds, hidden, warped);
  }
  // Debug: OC_STAGE_REJECT_LOG logs the packets dropped per frame.
  static const bool log_reject = std::getenv("OC_STAGE_REJECT_LOG") != nullptr;
  if (log_reject) log::info("stage: {} objects, {} packets rejected", inst_.size(), rejected_);
  std::array<float, 12> floor_cam;
  for (size_t k = 0; k < 9; ++k) floor_cam[k] = view.rot.m[k];
  for (size_t k = 0; k < 3; ++k) floor_cam[9 + k] = view.trans[k];
  // [Enhancements] Lighting: the real shadows on the floor; the sun of a light object throws a harder one than the stand-in key light.
  if (floor_shadow_) {
    render::ShadowPipeline::Floor floor;
    floor.clip = proj * render::mat4_from_rows(floor_cam);
    floor.rect = floor_rect_;
    floor.strength = key_light_.source == KeyLightSource::Camera ? 0.42f : 0.5f;
    floor.lights = &spell_lights_;
    floor_shadow_->draw_floor(floor);
  }
  // Unit drop shadows after the scenery: subtractive Gouraud hulls on the floor, depth tested one slot in front of the floor
  // they lie on (the PS1 sorts them 0x19 slots behind the unit; the units are drawn afterwards and win the depth test).
  shadow_verts_.clear();
  shadow_idx_.clear();
  // Debug: OC_STAGE_NOSHADOW leaves the unit drop shadows out (reference comparisons).
  static const bool no_shadow = std::getenv("OC_STAGE_NOSHADOW") != nullptr;
  if (!no_shadow)
    for (const ShadowCaster& caster : casters_) {
      const Footprint& fp = caster.has_fp ? caster.fp : footprint(caster.model);
      // Under real shadows the hull is only the contact shade of the feet: faint, and straight below the unit.
      if (floor_shadow_) build_shadow(caster, fp, {caster.x, 3072.0f, caster.z}, shadow_verts_, shadow_idx_, kContactShade);
      else build_shadow(caster, fp, light_, shadow_verts_, shadow_idx_);
    }
  if (!shadow_idx_.empty()) pipe.draw_dynamic(proj, floor_cam, shadow_verts_, shadow_idx_, 3, -1);
  pipe.end();
}

}  // namespace oc::game
