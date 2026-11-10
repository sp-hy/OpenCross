// FieldWorld: construction, room loading and the session (art mode, language, party spawn, tick end), the per-room defeated-monster
// flags and the disc / map-id lookups.
#include "game/field_world.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

#include "audio/voice.h"
#include "core/log.h"
#include "core/timer.h"
#include "game/battle/model_table.h"
#include "game/field_world_internal.h"
#include "game/model_files.h"
#include "game/new_game.h"
#include "game/room_models.h"
#include "import/battle_stage.h"
#include "import/room_effects.h"
#include "import/tim.h"
#include "render/texture.h"
#include "render/texture_registry.h"

namespace oc::game {

using namespace field_internal;

FieldWorld::FieldWorld() : vm_(*this) {}
FieldWorld::~FieldWorld() { shutdown(); }

bool FieldWorld::init(const vfs::Vfs& files, render::GpuContext& ctx, render::SpriteBatch& sprites, render::MeshPipeline& meshes,
                      import::Lang lang) {
  files_ = &files;
  if (const char* env = std::getenv("OC_STUCK_TICKS"); env && std::atoi(env) > 0) watchdog_.stuck_limit = static_cast<u32>(std::atoi(env));
  install_name_hooks();
  discs_.load(files);
  if (auto exe = files.read("cdrom/prog/slps_023.64"); !exe || !exe_strings_.load(*exe)) log::warn("field: executable strings unavailable");
  ctx_ = &ctx;
  sprites_ = &sprites;
  meshes_ = &meshes;
  lang_ = lang;
  if (!view_.init(files, ctx, sprites, meshes)) return false;
  if (!dialogue_.init(ctx, files, sprites, lang)) log::warn("field: dialogue box unavailable (no usable font)");
  dialogue_.show_status(false);
  const u32 px = 0xFFFFFFFFu;
  if (white_.create(ctx, 1, 1, render::TextureFormat::Rgba8, &px) && white_sampler_.create(ctx, false))
    white_set_ = render::TexRef(white_, white_sampler_);
  // Room effects: the shared particle library and the GPU glue (a failure only disables the effects).
  if (auto lib = files.read("cdrom/effect/particle.prd")) {
    import::Vram empty;
    lib_fx_ = import::load_room_effects(*lib, empty);
    lib_loaded_ = true;
    // Nested pack 'fenc' (the encounter effect): its sequences carry battle ops (2B, 3E, 40), so the
    // lenient battle framing; its two meshes are packet meshes like the battle effect meshes (import::load_stage).
    if (const Bytes fenc = import::nested_pack(*lib, "fenc"); !fenc.empty()) {
      fenc_fx_ = import::load_battle_effects(fenc, {}, empty, *lib);
      std::string err;
      fenc_meshes_ = import::load_stage(fenc, {}, &err);
      if (!fenc_meshes_) log::warn("field: particle.prd > fenc meshes: {}", err);
      for (const auto& error : fenc_fx_.errors) log::warn("field: particle.prd > fenc: {}", error);
    } else {
      log::warn("field: effect/particle.prd has no nested pack 'fenc' (no encounter effect)");
    }
  }
  particles_ok_ = gpu_fx_.init(ctx, sprites) && particles_.init(ctx);
  if (!particles_ok_) log::warn("field: effect renderer unavailable; room effects are not drawn");
  // The blob shadow texture is TIM 0 of map/etc/systim.bin.
  shadows_ok_ = shadows_.init(files, ctx, sprites);
  initialised_ = true;
  return true;
}

void FieldWorld::shutdown() {
  if (!initialised_) return;
  initialised_ = false;
  clear_effects();
  gpu_fx_.clear();
  particles_.destroy();
  ents_.clear();
  room_models_.clear();
  char_models_.clear();
  sourced_models_.clear();
  drop_character_prefetches();
  free_status_icons();
  free_group_sprites();
  white_set_ = {};
  capture_.set = {};
  capture_.tex.destroy();
  if (sprites_) shadows_.shutdown(*sprites_);
  shadow_tiles_.destroy();
  dialogue_.shutdown();
}

void FieldWorld::set_art_mode(bool hd, bool force) {
  const bool changed = hd_ != hd || force;
  hd_ = hd;
  // RDE reads cdrom2/ variants (mesh + UV atlas) in HD mode; keep mesh and texture on the same layer.
  if (files_) const_cast<vfs::Vfs*>(files_)->set_hd_mode(hd);
  // A pure rendering swap: the room is NOT reloaded (that would re-run INIT / the entrance and could change story flags). Only
  // the art is rebuilt from the other file set; script VM, entities, poses, clips, variables, timers, camera, scroll and effect state stay.
  if (changed && initialised_) {
    // A worker reads the file set of the mode it started in.
    drop_character_prefetches();
    if (view_.loaded()) {
      swap_art();
    } else {
      char_models_.clear();
      sourced_models_.clear();
    }
  }
  if (!changed || !initialised_ || !view_.loaded()) view_.set_art_mode(hd);
  const ui::ArtMode mode = hd ? ui::ArtMode::Hd : ui::ArtMode::Original;
  // Art-only swap: skin, portrait and fonts change live; an open window keeps its message, typewriter position and choice.
  dialogue_mode_pending_ = false;
  if (initialised_) {
    dialogue_.set_art_mode(mode);
    for (auto& window : extra_win_)
      if (window.box) window.box->set_art_mode(mode);
    for (auto& window : mod_ui_win_)
      if (window.box) window.box->set_art_mode(mode);
  }
}

void FieldWorld::swap_art() {
  // Room art (layers, room-model meshes / pages, effect sprite pixels) from the other file set, in place.
  view_.reload_art(hd_);
  room_prepared_.clear();
  // Models: rebuild every one an entity shows and hand it over without touching the playback state. Unused cache entries are dropped
  // (built again on first use).
  const auto old_room = std::move(room_models_);
  const auto old_chars = std::move(char_models_);
  char_models_.clear();
  const auto old_sourced = std::move(sourced_models_);
  sourced_models_.clear();
  const size_t n_room = view_.scene() ? view_.scene()->models.size() : 0;
  room_models_.assign(n_room, nullptr);
  room_model_tried_.assign(n_room, false);
  // Every model an entity shows is rebuilt on its own worker at once (reading, decoding and composing replacement art is the slow
  // part), then handed over below in entity order.
  std::vector<std::future<std::optional<PreparedRoomModel>>> room_jobs(n_room);
  for (const Ent& ent_state : ents_) {
    const auto& cur = ent_state.actor.model_handle();
    if (!cur) continue;
    for (size_t i = 0; i < old_room.size() && i < n_room; ++i)
      if (old_room[i] == cur && !room_jobs[i].valid())
        room_jobs[i] = std::async(std::launch::async, [scene = view_.scene(), i, ctx = ctx_] { return prepare_room_model(*scene, i, ctx); });
    for (const auto& [id, handle] : old_chars)
      if (handle == cur) prefetch_character_model(id);
    for (const auto& [key, handle] : old_sourced)
      if (handle == cur) prefetch_sourced_model(key);
  }
  room_prepared_.resize(n_room);
  for (size_t i = 0; i < n_room; ++i)
    if (room_jobs[i].valid()) room_prepared_[i] = room_jobs[i].get();
  int swapped = 0, kept = 0;
  for (Ent& ent_state : ents_) {
    const auto& cur = ent_state.actor.model_handle();
    if (!cur) continue;
    std::shared_ptr<ModelHandle> fresh;
    for (size_t i = 0; i < old_room.size() && !fresh; ++i)
      if (old_room[i] == cur) fresh = own_room_model(static_cast<int>(i));
    for (const auto& [id, handle] : old_chars)
      if (!fresh && handle == cur) fresh = character_model(id);
    for (const auto& [key, handle] : old_sourced)
      if (!fresh && handle == cur) fresh = sourced_model(key);
    if (!fresh) continue;
    if (ent_state.actor.swap_model_art(fresh)) ++swapped;
    else ++kept;
  }
  if (kept) log::warn("field: art swap: {} actors keep their old model (skeleton / clip count differs between the file sets)", kept);
  // The party's own models went with the unused entries: made ready again for a switch back.
  for (const Ent& ent_state : ents_)
    if (ent_state.char_id >= 0 && ent_state.char_model != ent_state.char_id) prefetch_character_model(ent_state.char_id);
  // Effect atlases from the swapped sprite pixels.
  if (particles_ok_ && effects_gpu_) gpu_fx_.load(bank_);
  ++art_swaps_;
  log::info("field: {} art mode {} swapped in place: {} actor models rebuilt, script / entities / camera untouched", view_.room_name(),
            hd_ ? "hd" : "original", swapped);
}

void FieldWorld::set_language(import::Lang lang) {
  lang_ = lang;
  dialogue_.set_language(lang);
  for (auto& window : extra_win_)
    if (window.box) window.box->set_language(lang);
  // The mod UI's text windows show their messages again in the new language.
  for (auto& window : mod_ui_win_) {
    if (window.box) window.box->set_language(lang);
    window.element = -1;
  }
  close_windows();
}

void FieldWorld::stub(const char* what) {
  if (++stub_hits_[what] == 1) log::debug("field: stubbed host call {}", what);
}

int FieldWorld::bearing(float dx, float dz) const {
  return static_cast<int>(std::lround(std::atan2(dz, -dx) * kUnitsPerRad)) & 0xFFF;
}

int FieldWorld::player_entity() const {
  if (controlled_override_ >= 0 && controlled_override_ < static_cast<int>(ents_.size())) return controlled_override_;
  return party_ent_[0];
}

// ------------------------------------------------------------------------------------------------ models
std::shared_ptr<ModelHandle> FieldWorld::room_model(int index) {
  // FE D0: a model from elsewhere under this number (also past the room's own models). Characters share the party's cache.
  if (auto it = model_sources_.find(index); it != model_sources_.end()) {
    const ModelSource& source = it->second;
    if (source.kind == ModelSource::kCharacter) return character_model(model_byte(source.id));
    if (source.kind == ModelSource::kFieldModel) return character_model(source.id);
    if (source.kind == ModelSource::kRoomModel && room_stem_for_id(source.id) == view_.room_name()) return own_room_model(source.sub);
    return sourced_model(source_key(source));
  }
  return own_room_model(index);
}

std::shared_ptr<ModelHandle> FieldWorld::own_room_model(int index) {
  const assets::RoomScene* scene = view_.scene();
  if (!scene || index < 0 || index >= static_cast<int>(scene->models.size())) return nullptr;
  const size_t i = static_cast<size_t>(index);
  if (room_model_tried_[i]) return room_models_[i];
  room_model_tried_[i] = true;
  if (i < room_prepared_.size() && room_prepared_[i] && room_prepared_[i]->generation == render::texture_registry::generation()) {
    ScopedTimer model_timer("field: room model load (prepared ahead)");
    PreparedRoomModel prepared = std::move(*room_prepared_[i]);
    room_prepared_[i].reset();
    room_models_[i] = Actor::make_resolved_model(*meshes_, std::move(prepared.asset), &prepared.uploads, &prepared.textures);
    return room_models_[i];
  }
  ScopedTimer model_timer("field: room model load");
  std::string err;
  auto asset = build_room_model(*scene, i, &err);
  if (!asset) {
    log::warn("room {}: model {}: {}", view_.room_name(), index, err);
    return nullptr;
  }
  room_models_[i] = Actor::make_model(*meshes_, std::move(*asset));
  return room_models_[i];
}

namespace {

// The field model of a player folder (fieldobj/player/<folder>), its texture mods resolved. Any thread.
std::optional<assets::ModelAsset> read_character_model(const vfs::Vfs& files, const std::string& folder) {
  std::string err;
  auto asset = read_model_files(files, "fieldobj/player/" + folder + "/" + folder, &err);
  if (!asset) {
    log::warn("player model {}: {}", folder, err);
    return std::nullopt;
  }
  std::string list;
  for (const auto& clip : asset->clips) list += std::format(" {}:{}", clip.name, clip.frame_count);
  log::debug("field: character model {} texture {}x{} hd x{} clips{}", folder, asset->textures.empty() ? 0 : asset->textures[0].width,
             asset->textures.empty() ? 0 : asset->textures[0].height, asset->hd_textures.empty() ? 0 : asset->hd_scale, list);
  render::resolve_model_textures(*asset);
  return asset;
}

}  // namespace

// `char_id` is a character MODEL BYTE (see model_byte(); normally the character id).
std::shared_ptr<ModelHandle> FieldWorld::character_model(int char_id) {
  if (char_id < 0 || char_id >= 0x100) return nullptr;
  if (auto it = char_models_.find(char_id); it != char_models_.end()) return it->second;
  std::shared_ptr<ModelHandle> handle;
  if (auto it = char_prefetch_.find(char_id); it != char_prefetch_.end()) {
    ScopedTimer model_timer("field: character model load (prefetched)");
    std::optional<PrefetchedCharacter> prefetched = it->second.get();
    char_prefetch_.erase(it);
    if (prefetched && prefetched->generation == render::texture_registry::generation())
      handle = Actor::make_resolved_model(*meshes_, std::move(prefetched->asset), &prefetched->uploads, &prefetched->textures);
  }
  if (!handle) {
    ScopedTimer model_timer("field: character model load (first use)");
    const std::string folder = model_folder(char_id);
    if (folder.empty()) {
      char_models_[char_id] = nullptr;
      return nullptr;
    }
    if (std::optional<assets::ModelAsset> asset = read_character_model(*files_, folder)) handle = Actor::make_resolved_model(*meshes_, std::move(*asset));
  }
  char_models_[char_id] = handle;
  return handle;
}

void FieldWorld::prefetch_character_model(int model_byte) {
  if (model_byte < 0 || model_byte >= 0x100 || !files_ || char_models_.contains(model_byte) || char_prefetch_.contains(model_byte)) return;
  std::string folder = model_folder(model_byte);
  if (folder.empty()) return;
  char_prefetch_[model_byte] = std::async(std::launch::async, [files = files_, ctx = ctx_, folder = std::move(folder)]() -> std::optional<PrefetchedCharacter> {
    const unsigned generation = render::texture_registry::generation();
    std::optional<assets::ModelAsset> asset = read_character_model(*files, folder);
    if (!asset) return std::nullopt;
    render::ModelUploads uploads = render::prepare_model_uploads(*asset);
    render::ModelTextures textures = ctx ? render::prepare_model_textures(*ctx, *asset, uploads) : render::ModelTextures{};
    return PrefetchedCharacter{std::move(*asset), std::move(uploads), std::move(textures), generation};
  });
}

void FieldWorld::drop_character_prefetches() {
  for (auto& [model_byte, job] : char_prefetch_)
    if (job.valid()) job.wait();
  char_prefetch_.clear();
  for (auto& [key, job] : sourced_prefetch_)
    if (job.valid()) job.wait();
  sourced_prefetch_.clear();
}

namespace {

// The CPU half of a sourced model ("room:<stem>:<k>" or "battle:<stem>"), its texture mods resolved. Any thread.
std::optional<assets::ModelAsset> read_sourced_model(const vfs::Vfs& files, const std::string& key, bool hd) {
  std::string err;
  std::optional<assets::ModelAsset> asset;
  if (key.starts_with("room:")) {
    const size_t colon = key.rfind(':');
    asset = read_room_model(files, key.substr(5, colon - 5), std::atoi(key.c_str() + colon + 1), &err);
  } else if (key.starts_with("battle:")) {
    asset = read_battle_model_for_field(files, key.substr(7), hd, &err);
  }
  if (!asset) {
    log::warn("field: model {}: {}", key, err);
    return std::nullopt;
  }
  render::resolve_model_textures(*asset);
  return asset;
}

}  // namespace

std::string FieldWorld::source_key(const ModelSource& source) const {
  switch (source.kind) {
    case ModelSource::kRoomModel: {
      const std::string room = room_stem_for_id(source.id);
      return room.empty() || source.sub < 0 ? std::string() : std::format("room:{}:{}", room, source.sub);
    }
    case ModelSource::kBattleModel:
      if (source.id < 0 || source.id >= static_cast<int>(kBattleModels.size()) || kBattleModels[static_cast<size_t>(source.id)].empty()) return {};
      return "battle:" + std::string(kBattleModels[static_cast<size_t>(source.id)]);
    case ModelSource::kCharacterBattle: {
      const std::string stem = files_ ? character_battle_stem(*files_, source.id) : std::string();
      return stem.empty() ? std::string() : "battle:" + stem;
    }
    default: return {};
  }
}

std::shared_ptr<ModelHandle> FieldWorld::sourced_model(const std::string& key) {
  if (key.empty() || !files_) return nullptr;
  if (auto it = sourced_models_.find(key); it != sourced_models_.end()) return it->second;
  std::shared_ptr<ModelHandle> handle;
  if (auto it = sourced_prefetch_.find(key); it != sourced_prefetch_.end()) {
    ScopedTimer model_timer("field: sourced model load (prefetched)");
    std::optional<PrefetchedCharacter> prefetched = it->second.get();
    sourced_prefetch_.erase(it);
    if (prefetched && prefetched->generation == render::texture_registry::generation())
      handle = Actor::make_resolved_model(*meshes_, std::move(prefetched->asset), &prefetched->uploads, &prefetched->textures);
  }
  if (!handle) {
    ScopedTimer model_timer("field: sourced model load (first use)");
    if (std::optional<assets::ModelAsset> asset = read_sourced_model(*files_, key, hd_)) handle = Actor::make_resolved_model(*meshes_, std::move(*asset));
  }
  if (handle && key.starts_with("battle:")) handle->field_scale = kBattleModelFieldScale;
  if (handle)
    log::debug("field: model {} ({} clips, extent {} {} {})", key, handle->asset.clips.size(), handle->asset.extent[0], handle->asset.extent[1],
               handle->asset.extent[2]);
  sourced_models_[key] = handle;
  return handle;
}

void FieldWorld::prefetch_sourced_model(const std::string& key) {
  if (key.empty() || !files_ || sourced_models_.contains(key) || sourced_prefetch_.contains(key)) return;
  sourced_prefetch_[key] = std::async(std::launch::async, [files = files_, ctx = ctx_, key, hd = hd_]() -> std::optional<PrefetchedCharacter> {
    const unsigned generation = render::texture_registry::generation();
    std::optional<assets::ModelAsset> asset = read_sourced_model(*files, key, hd);
    if (!asset) return std::nullopt;
    render::ModelUploads uploads = render::prepare_model_uploads(*asset);
    render::ModelTextures textures = ctx ? render::prepare_model_textures(*ctx, *asset, uploads) : render::ModelTextures{};
    return PrefetchedCharacter{std::move(*asset), std::move(uploads), std::move(textures), generation};
  });
}

// ------------------------------------------------------------------------------------------------ room loading
void FieldWorld::clear_room() {
  clear_effects();
  lighting_.clear();
  watchdog_.reset();
  sfx_vol_.fill(SfxSlide{});
  sfx_pan_.fill(SfxSlide{64, 64, 0, 0});
  song_cue_ = {};
  ents_.clear();
  room_models_.clear();
  room_model_tried_.clear();
  room_prepared_.clear();
  model_sources_.clear();
  // Room entries are decoded against this room's models.
  std::erase_if(ex_pack_cache_, [](const auto& kv) { return kv.first.starts_with("room:"); });
  party_ent_ = {-1, -1, -1};
  controlled_override_ = -1;
  camera_target_ = -1;
  // A room entry drops every pending request of the room before: a running exit (a direct load during an exit fade), its frame
  // capture, and a script screen still Requested or Open (a Load picked from a save point's menu).
  jump_ = {};
  movie_jump_ = {};
  trans_ = Trans::None;
  capture_.want = capture_.active = false;
  menu_state_ = MenuState::None;
  room_lost_ = false;
  camera_.reset();
  shake_ = Shake{};
  close_windows();
  choice_pending_ = false;
  label_ = false;
  handoff_.reset();
  dialogue_face_ = -1;
  auto_target_ = -1;
  auto_teleported_ = false;
  follow_hold_ = false;
  input_.reset();
  fog_ = {};
  // Default underwater tint; water_bias_ is layer 0's z at room load until op A4 sets it; sound registers get the footstep
  // volume / distance band defaults.
  underwater_rgb_ = {32, 32, 180};
  water_bias_ = -1;
  reset_room_sound_regs();
  fstat_.jolt = 0;
  // Room-init values of the registers behind B5 / FE 77 / FE 70 / FE 02, the 8F wave, the C5 sound
  // records and the D1 counter; op 1D followers and B2's override do not survive the room.
  push_strength_ = 5;
  exit_kind_ = 0;
  wave_ = BgWave{};
  bg_group_abe_.clear();
  sound_emitters_.clear();
  bone_followers_.clear();
  model_swap_slot_ = -1;
  d1_counter_ = 0;
}

// The music is not touched by a room change: the original only suspends / restores music channels for movies, song swaps and the
// debug pause screen - never on a room load; a PLAY_SONG of the current song is ignored, so the BGM plays on without a gap.
bool FieldWorld::load_room(std::string_view name, int param) {
  ScopedTimer total_timer("field: load_room TOTAL");
  ++room_loads_;
  if (files_) const_cast<vfs::Vfs*>(files_)->set_hd_mode(hd_);
  {
    ScopedTimer timer("field: clear_room (incl. GPU idle wait)");
    clear_room();
  }
  // The party's models build on workers while the room loads; INIT's party binds take them.
  prefetch_party_models(gs_->slot);
  bool view_ok;
  {
    ScopedTimer timer("field: view_.load");
    view_.set_prd_path(prd_path_for(name));
    view_ok = view_.load(name);
    room_prepared_ = view_.take_prepared_models();
  }
  if (!view_ok) {
    status_ = std::format("room {}: load failed", name);
    return false;
  }
  const assets::RoomScene* scene = view_.scene();
  std::string err;
  std::optional<script::FieldScriptModule> mod;
  {
    ScopedTimer timer("field: script parse");
    mod = script::FieldScriptModule::parse(scene->script_bytes, &err);
  }
  if (!mod) {
    log::error("room {}: field script: {}", name, err);
    status_ = std::format("room {}: bad script", name);
    return false;
  }
  if (sfx_) {
    // Every sound ends except the FE 29 slots playing a global sound; the keep mask is cleared for the new room.
    sfx_->leave_room(sfx_keep_slots_);
    sfx_keep_slots_ = 0;
    ScopedTimer timer("field: sfx set_room");
    // C6 plays this room's own sounds (ids below 256).
    sfx_->set_room(scene->yos0_bytes, scene->yos1_bytes);
  }
  room_id_ = import::room_id_from_name(name);
  map_id_ = map_id_for(name);
  load_group_sprites(name);
  // A room a mod adds (not shipped, so not in the lang.dat room table) reads its text rows under its map id.
  if (room_id_ < 0 && std::find(view_.rooms().begin(), view_.rooms().end(), std::string(name)) == view_.rooms().end()) room_id_ = map_id_;
  // The defeated-monster record is set up before INIT runs its D1 spawns.
  defeated_enter_room();
  entrance_param_ = param;
  ents_.clear();
  ents_.resize(mod->entity_count());
  // Every entity starts on the centroid of walk triangle 0 (scripts and the entrance record move it from there) at engine heading
  // 0 (= world +X); after INIT the shown facing is that minus 0x400. An entity INIT never turns (tel_b23e's fisherman next to
  // Korcha's boat, ent 10) keeps that heading: internal 0x800, not internal 0.
  const WalkMesh& walk = view_.walk();
  const auto start = walk.empty() ? std::array<float, 2>{} : walk.centroid(0);
  for (Ent& ent_state : ents_) {
    if (!walk.empty()) ent_state.actor.place(walk, 0, start[0], start[1], to_internal(0));
    ent_state.actor.set_facing(to_internal(0));
  }
  room_models_.assign(scene->models.size(), nullptr);
  room_model_tried_.assign(scene->models.size(), false);

  // The control yaw (op 63) and the portrait yaw (FE 6A) are 0 at every room load.
  control_yaw_ = 0;
  portrait_yaw_ = 0;
  const RoomView::Limits lim = view_.default_limits();
  camera_.cam.limits = lim;

  vm_.set_var(script::var::kRoomParam, param);
  vm_.set_var(script::var::kMapId, map_id_ < 0 ? 0 : map_id_);
  room_card_ = import::room_card(map_id_, param);
  // Var 0x3C is cleared at room load; scripts that allow saving here set it to 1. Var 0x3A = ROOM_PARAM (the entrance a later Load
  // re-enters with); var 0x50 = 0; var 0x34 (debug mode) = 1.
  vm_.set_var(0x3C, 0);
  vm_.set_var(0x3A, param);
  vm_.set_var(0x50, 0);
  vm_.set_var(0x34, 1);
  sync_party();
  {
    ScopedTimer timer("field: build_effects");
    build_effects();
  }
  in_init_ = true;
  {
    ScopedTimer timer("field: script INIT (incl. model loads)");
    const auto& af = scene->attr_flags;
    // E9 / FE 7A: the owned window frames live in the save.
    if (gs_) vm_.seed_global(script::Global::WindowFrames, gs_->slot.frames);
    vm_.load(std::move(*mod), std::span<const u32>(af.data(), std::min<size_t>(af.size(), 256)));
  }
  flag_writes_seen_ = 0;
  sync_flag_table();
  in_init_ = false;
  // Continue / Load, debug and route loads come in like a room change of kind 0 (step_transition picks the kind of a room change).
  begin_entry_fade(0);

  // The leader goes first: the followers' V spots are relative to it.
  for (int pass = 0; pass < 2; ++pass)
    for (size_t i = 0; i < ents_.size(); ++i) {
      Ent& ent_state = ents_[i];
      if (pass == 0) ent_state.actor.snap_facing();
      if (ent_state.pc_slot >= 0 && !ent_state.placed && (ent_state.pc_slot == 0) == (pass == 0)) place_party_spawn(ent_state.pc_slot);
    }
  reset_trails();
  settle_spawned_followers();
  update_input_lock();
  // A world-map Continue eases on from the scroll FE 50 restored; every other entry starts from 0.
  snap_camera_to_player(vm_.world_continue());
  entry_cut_ = 4;
  entry_window_ = 4;
  pick_auto_target();
  status_ = std::format("{} (room id {}, entrance {})", name, room_id_, param);
  log::info("field: {} loaded, {} entities, {} models, {} zones, auto target {}", name, ents_.size(), scene->models.size(),
            scene->zones.size(), auto_target_);
  return true;
}

// The party V (formation op DF): behind a leader at (lx, lz) with engine heading h, slot 1 stands 128 units away at
// heading h + 0x200 from straight behind on one side, slot 2 (h - 0x200) on the other: (x - cos(a) * 128, z + sin(a) * 128), a = h +- 0x200.
void FieldWorld::party_v_spot(int slot, float lx, float lz, int heading, float& x, float& z) const {
  const float angle = static_cast<float>((heading + (slot == 1 ? 0x200 : -0x200)) & 0xFFF) * kUnitsToRad;
  x = lx - std::cos(angle) * 128.0f;
  z = lz + std::sin(angle) * 128.0f;
}

void FieldWorld::place_party_spawn(int slot) {
  const int ent = party_ent_[static_cast<size_t>(slot)];
  if (ent < 0) return;
  Ent& ent_state = ents_[static_cast<size_t>(ent)];
  const WalkMesh& walk = view_.walk();
  float x = 0, z = 0;
  int facing = 0;
  bool have = false;
  // `level`: start-triangle index of the entrance record (-1 = none); `v_tri`: for a follower, the triangle of its V spot (or the leader's).
  int level = -1;
  int v_tri = -1;
  const auto& pos = vm_.module().positions;
  // A room without an entrance table (the cutscene rooms) places nobody: every party member stays where the room load put each
  // entity, on the centroid of walk triangle 0, until a script moves it.
  if (pos.empty() && !walk.empty()) {
    ent_state.placed = true;
    if (slot == 0) log::debug("field: {} has no entrance table, the party stays on walk triangle 0", view_.room_name());
    return;
  }
  if (slot > 0 && party_ent_[0] >= 0 && ents_[static_cast<size_t>(party_ent_[0])].placed) {
    const ActorPose& lp = ents_[static_cast<size_t>(party_ent_[0])].actor.current();
    // Followers spawn in the party V behind the leader (the geometry of the formation op DF): with heading h the
    // leader faces, slot 1 stands 128 units away at heading h + 0x200 from straight behind on one side, slot 2 on the other
    // (x - cos(h +- 0x200) * 128, z + sin(h +- 0x200) * 128). In the lift of opn_a01i this is what puts the third member left and Kid right.
    facing = ents_[static_cast<size_t>(party_ent_[0])].actor.facing();
    // The followers appear in this V at once, facing the leader's way, and the spot is set without any walk test (the opening
    // lift: Serge (0, 1934), followers (+-90, 2024) = the full V, across the lift's blocked edge); only a spot with no floor of its
    // own, or one on another floor (a ledge below), falls back to the half-size V, else the leader's own spot (settled out by
    // settle_spawned_followers). A half V (64 units) would put the follower under follow rule 1's 0x50 and back it off / turn it.
    const int ltri = ents_[static_cast<size_t>(party_ent_[0])].actor.triangle();
    x = lp.x;
    z = lp.z;
    v_tri = ltri;
    for (const float scale : {1.0f, 0.5f}) {
      float vx = 0, vz = 0;
      party_v_spot(slot, lp.x, lp.z, (facing - 0x800) & 0xFFF, vx, vz);
      vx = lp.x + (vx - lp.x) * scale;
      vz = lp.z + (vz - lp.z) * scale;
      const int st = walk.find_triangle(vx, vz, lp.y);
      log::debug("field: follower {} V spot k {:.1f} ({:.0f}, {:.0f}) leader tri {} -> tri {} dy {:.0f}", slot, static_cast<double>(scale),static_cast<double>(vx), static_cast<double>(vz), ltri, st,
                 st >= 0 ? static_cast<double>(walk.height(st, vx, vz) - lp.y) : 0.0);
      if (ltri < 0 || st < 0 || std::fabs(walk.height(st, vx, vz) - lp.y) > 64.0f) continue;
      x = vx;
      z = vz;
      v_tri = st;
      break;
    }
    have = true;
  } else if (entrance_param_ >= 0 && static_cast<size_t>(entrance_param_) < pos.size()) {
    // Pre-script table record = entrance (x, z, facing) selected by ROOM_PARAM (4096/turn).
    const auto& entrance = pos[static_cast<size_t>(entrance_param_)];
    x = entrance.a;
    z = entrance.b;
    facing = to_internal(entrance.c);
    level = entrance.level;
    have = true;
  }
  int tri = -1;
  if (have) {
    // Followers stand on the leader's floor; the leader on floor `level` of the entrance record.
    if (level >= 0) tri = walk.find_triangle_level(x, z, level);
    else if (party_ent_[0] >= 0) {
      const Actor& la = ents_[static_cast<size_t>(party_ent_[0])].actor;
      // The triangle reached from the leader.
      tri = slot > 0 && v_tri >= 0 ? v_tri : walk.find_triangle(x, z, la.current().y);
    }
    else tri = walk.find_triangle(x, z, 0.0f);
  }
  if (have && slot == 0) log::debug("field: spawn ({:.0f}, {:.0f}) level {} -> tri {} y {:.0f}", static_cast<double>(x), static_cast<double>(z), level, tri, tri >= 0 ? static_cast<double>(walk.height(tri, x, z)) : 0.0);
  // The V spot is off the mesh: stand on the leader instead.
  if (tri < 0 && have && slot > 0 && party_ent_[0] >= 0) {
    const ActorPose& lp = ents_[static_cast<size_t>(party_ent_[0])].actor.current();
    x = lp.x;
    z = lp.z;
    tri = walk.find_triangle(x, z, 0.0f);
  }
  // An entrance record is written as it stands, even where the walk mesh has no floor (a party that walks in from outside the mesh).
  if (tri < 0 && !have && !walk.empty()) {
    // No entrance record: the walk-mesh vertex nearest the mesh centre.
    float cx = 0, cz = 0;
    const auto& verts = walk.mesh().vertices;
    for (const auto& vertex : verts) {
      cx += vertex[0];
      cz += vertex[2];
    }
    cx /= static_cast<float>(std::max<size_t>(verts.size(), 1));
    cz /= static_cast<float>(std::max<size_t>(verts.size(), 1));
    if (walk.nearest_vertex(cx, cz, 0, &tri) >= 0 && tri >= 0) {
      const auto centroid = walk.centroid(tri);
      x = centroid[0];
      z = centroid[1];
    }
    // Once per load, not once per party slot.
    if (slot == 0) log::warn("field: no entrance record {} in {}, spawning at the mesh centre", entrance_param_, view_.room_name());
  }
  ent_state.actor.place(walk, tri, x, z, facing);
  ent_state.placed = true;
  ent_state.spawned_on_leader = slot > 0 && have && party_ent_[0] >= 0 &&
                        std::hypot(x - ents_[static_cast<size_t>(party_ent_[0])].actor.current().x, z - ents_[static_cast<size_t>(party_ent_[0])].actor.current().z) <= 1.0f;
}

// ------------------------------------------------------------------------------------------------ host: entities
// A screen fade alone does not hold the gate: the scripts of a battle return, a menu close or a room entry run under the fade-in.
// A battle holds it from its request through the encounter effect to its end, so a second A3 / C9 waits for its own fight. A screen
// (menu, shop, name entry, party screen, save list) or a movie holds it from its request until it has closed, and a party model swap
// from D9 until its DA: a second screen op waits and opens its screen after the first one. The full gate also holds while a song
// change runs (the WAIT_SONG condition); RUN_SCRIPT tests the gate without that term.
bool FieldWorld::busy_clear() {
  return in_init_ || (trans_ == Trans::None && !jump_.active && handoff_.battle_state == BattleState::None && menu_state_ == MenuState::None &&
                      movie_state_ == MovieState::None && !model_swap_pending());
}

bool FieldWorld::system_ready() { return busy_clear() && (in_init_ || !song_changing()); }

// Ops 75 / 76 / 78 edit the attr -> flag table that the walk mesh blocks by.
void FieldWorld::sync_flag_table() {
  if (vm_.flag_table_writes() == flag_writes_seen_) return;
  flag_writes_seen_ = vm_.flag_table_writes();
  const auto& table = vm_.flag_table();
  view_.walk().set_attr_flags(std::span<const u32>(table.data(),std::max<size_t>(view_.scene() ? view_.scene()->attr_flags.size() : 0, 0)));
}

void FieldWorld::end_of_tick(script::FieldVM& vm) {
  sync_flag_table();
  step_song_change();
  if (pending_song_ >= 0 && (!mixer_ || mixer_->current_song() == pending_song_ || ++pending_ticks_ > 30)) pending_song_ = -1;
  const int player = player_entity();
  if (player >= 0) {
    const ActorPose& pose = ents_[static_cast<size_t>(player)].actor.current();
    vm.set_var(0x12, static_cast<s32>(std::lround(pose.x)));
    vm.set_var(0x14, static_cast<s32>(std::lround(pose.z)));
    vm.set_var(0x16, static_cast<s32>(std::lround(pose.y)));
    vm.set_var(0x0A, to_script(ents_[static_cast<size_t>(player)].actor.facing()));
  }
  for (int slot = 0; slot < 3; ++slot)
    vm.set_var(static_cast<u16>(0x0C + 2 * slot), slot < static_cast<int>(party_chars_.size()) ? party_chars_[static_cast<size_t>(slot)] : 0xFF);
  // Also published every frame: the FE 4F slot entities' X / Z / facing (variables 0x300 + 6k, +2, +4) and the scroll (0x330 /
  // 0x332). They are saved with the variable block; FE 50 reads them back on a world-map Continue.
  for (size_t k = 0; k < 8; ++k) {
    const int se = vm.slot_entity(k);
    if (se < 0 || se >= static_cast<int>(ents_.size())) continue;
    const ActorPose& sp = ents_[static_cast<size_t>(se)].actor.current();
    const u16 var = static_cast<u16>(0x300 + 6 * k);
    vm.set_var(var, static_cast<s32>(std::lround(sp.x)));
    vm.set_var(static_cast<u16>(var + 2), static_cast<s32>(std::lround(sp.z)));
    vm.set_var(static_cast<u16>(var + 4), to_script(ents_[static_cast<size_t>(se)].actor.facing()));
  }
  vm.set_var(0x330, camera_.cam.x);
  vm.set_var(0x332, camera_.cam.y);
  // Every frame: play time as hours / minutes / seconds in variables 0x2C / 0x2E / 0x30.
  if (gs_) {
    const u32 frames = gs_->slot.playtime_frames();
    vm.set_var(0x2C, static_cast<s32>(frames / 216000u));
    vm.set_var(0x2E, static_cast<s32>(frames % 216000u / 3600u));
    vm.set_var(0x30, static_cast<s32>(frames % 3600u / 60u));
  }
  update_input_lock();
}


bool FieldWorld::monster_defeated(int ordinal) { return (defeated_[0].mask >> (ordinal & 31)) & 1u; }
void FieldWorld::set_monster_defeated(int ordinal) {
  defeated_[0].mask |= 1u << (ordinal & 31);
  log::debug("field: tick {} room {} monster {} defeated (mask {:#x})", tick_count_, map_id_, ordinal, defeated_[0].mask);
}

// Room load: the record of the room moves to the front; a room without one gets a fresh record (mask 0) and the oldest record
// (record 2) is dropped - order [current, old 0, old 1].
void FieldWorld::defeated_enter_room() {
  const int room = map_id_;
  std::array<DefeatedRec, 3> out{};
  bool found = false;
  for (auto& record : defeated_)
    if (record.room == room && record.room >= 0) {
      out[0] = record;
      record.room = -1;
      found = true;
      break;
    }
  if (!found) {
    defeated_[2].room = -1;
    out[0] = {room, 0};
  }
  size_t next = 1;
  for (const auto& record : defeated_)
    if (record.room >= 0 && next < 3) out[next++] = record;
  defeated_ = out;
  d1_counter_ = 0;
}

// 0A / 0B: the per-tick entity update, which in the original includes the party-follow logic. A party
// member whose script is busy (inside WALK_TO / MOVE / a wait, or preempted by a script of a higher class) therefore does not
// follow; step_followers() only moves members that ran this op during the tick.

// FE 2F: the boot state. Variable 0x34 (debug) survives; variable 0x42 names the disc in use, which stays.
void FieldWorld::field_reset() {
  const u16 debug = static_cast<u16>(vm_.var(0x34));
  const u16 disc_var = static_cast<u16>(disc_ - 1);
  if (gs_) {
    gs_->slot = game::newgame::boot_state(gs_->rules ? gs_->rules->tables : nullptr, static_cast<u32>(tick_count_) * 2654435761u | 1u);
    gs_->slot.set_var(0x34, debug);
    gs_->slot.set_var(0x42, disc_var);
  }
  for (u16 id = 0; id < 0x3D0; id += 2) vm_.set_var(id, 0);
  const auto globals = game::newgame::globals();
  for (size_t i = 0; i < globals.size(); ++i) vm_.set_var(static_cast<u16>(0x3D0 + 2 * i), globals[i]);
  vm_.set_var(0x34, debug);
  vm_.set_var(0x42, disc_var);
  defeated_ = {};
  game_reset_ = true;
}

// ------------------------------------------------------------------------------------------------ host: party and items

void FieldWorld::set_disc(int disc) {
  if (disc < 1 || disc > 2 || disc == disc_) return;
  log::info("field: disc {} -> {} (file index cdrom{}.fnd)", disc_, disc, disc == 2 ? "2" : "");
  disc_ = disc;
  stem_ids_.clear();
}

// The room's .prd is disc index record 368 + 3 * id + 1 (bin, prd, ctd), which names another room's .prd in 195 rooms (dem_05 ->
// hid_a02f, alu_b01f -> alu_a01f, opn_a01i -> ryu_a24i, ...). title.prd (outside map/mapbin, the opening room's record) is the title
// overlay's scene, run by TitleBackdrop. Empty = the room's own <room>.prd.
std::string FieldWorld::prd_path_for(std::string_view name) {
  const int prd_id = map_id_for(name);
  const std::string& prd_rec = prd_id < 0 ? std::string() : discs_.record(disc_, DiscIndex::kMapBase + 3 * prd_id + 1);
  const size_t prd_at = prd_rec.find("/map/mapbin/");
  return prd_at != std::string::npos && prd_rec.ends_with(".prd") ? "cdrom" + prd_rec.substr(prd_at) : std::string();
}

void FieldWorld::prefetch_room(int room_id, int disc) {
  if (disc != disc_) return;
  const std::string stem = room_stem_for_id(room_id);
  const auto& rooms = view_.rooms();
  if (stem.empty() || std::find(rooms.begin(), rooms.end(), stem) == rooms.end()) return;
  view_.prefetch(stem, prd_path_for(stem));
}

void FieldWorld::select_disc_for_room(std::string_view stem) {
  if (!discs_.loaded() || discs_.has_room(disc_, stem)) return;
  const int other = disc_ == 1 ? 2 : 1;
  if (!discs_.has_room(other, stem)) return;
  set_disc(other);
  vm_.set_var(0x42, other - 1);
}

std::string FieldWorld::room_stem_for_id(int room_id) const {
  if (std::string stem = discs_.room_stem(disc_, room_id); !stem.empty()) return stem;
  // The disc's record is a placeholder (disc-1-only rooms are dummy.dat on disc 2): no room, as on the PS1, which cannot load one.
  if (!discs_.record(disc_, DiscIndex::kMapBase + 3 * room_id).empty()) return {};
  const char* name = import::room_name_for_id(room_id);
  return name ? name : std::string();
}

bool FieldWorld::script_map_jump(int& room_id, int& param) const {
  const script::FieldScriptModule& module = vm_.module();
  std::vector<u32> todo;
  for (size_t entity = 0; entity < module.entity_count(); ++entity)
    for (size_t slot = 0; slot < static_cast<size_t>(script::kScriptSlots); ++slot)
      if (const u16 ptr = module.script_ptr(entity, slot)) todo.push_back(ptr);
  std::vector<bool> seen(module.code.size());
  std::optional<std::pair<int, int>> found;
  while (!todo.empty()) {
    u32 off = todo.back();
    todo.pop_back();
    while (off < module.code.size() && !seen[off]) {
      seen[off] = true;
      const std::optional<script::Insn> insn = script::decode_insn(module.code, off);
      if (!insn) break;
      if (insn->key == 0xDC && insn->ops.size() == 2 && insn->ops[0].kind == script::OperandKind::Imm &&
          insn->ops[1].kind == script::OperandKind::Imm) {
        const std::pair<int, int> jump{insn->ops[0].value, insn->ops[1].value};
        if (found && *found != jump) return false;
        found = jump;
      }
      for (const auto& target : insn->targets) todo.push_back(target.first);
      if (insn->flow != script::Flow::Next) break;
      off += insn->length;
    }
  }
  if (!found) return false;
  room_id = found->first;
  param = found->second;
  return true;
}

int FieldWorld::map_id_for(std::string_view stem) {
  if (stem_ids_.empty() && discs_.loaded())
    for (int id = 0; id < 2000; ++id)
      // The lowest id wins.
      if (std::string room_stem = discs_.room_stem(disc_, id); !room_stem.empty()) stem_ids_.emplace(std::move(room_stem), id);
  const auto it = stem_ids_.find(std::string(stem));
  if (it != stem_ids_.end()) return it->second;
  return import::room_id_from_name(stem);
}

}  // namespace oc::game
