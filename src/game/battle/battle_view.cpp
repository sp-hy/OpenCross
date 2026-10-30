#include "game/battle/battle_view.h"
#include "game/cheats.h"
#include "game/battle/battle_view_op.h"
#include "battle/dopple_table.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>

#include "core/log.h"
namespace oc::game {


namespace {
// Time budget for the stage scene's load phases per call (a phase itself is never split).
constexpr double kStageBudgetMs = 2.0;

void put16(Bytes& bytes, int value) {
  bytes.push_back(static_cast<u8>(value & 0xFF));
  bytes.push_back(static_cast<u8>((value >> 8) & 0xFF));
}
void put32(Bytes& bytes, u32 value) {
  put16(bytes, static_cast<int>(value & 0xFFFF));
  put16(bytes, static_cast<int>(value >> 16));
}

// A minimal "atk" block for models that have none: variant 0 = wait, hit every target, wait; variant 7 = hit pose + damage.
Bytes make_fallback_block() {
  const std::vector<int> v0 = {0x09, 16, 0x5A, 0x09, 14, 0x00};
  const std::vector<int> v7 = {0x15, 5, 4, 0x41, 0x09, 20, 0x00};
  Bytes bytes;
  const u32 entry = 12, size = entry + 16 + 2 * static_cast<u32>(v0.size() + v7.size());
  put32(bytes, 1);
  put32(bytes, entry);
  put32(bytes, size);
  put16(bytes, 16);
  for (int i = 0; i < 6; ++i) put16(bytes, 0);
  put16(bytes, 16 + 2 * static_cast<int>(v0.size()));
  for (int word : v0) put16(bytes, word);
  for (int word : v7) put16(bytes, word);
  return bytes;
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------ setup
BattleView::BattleView() = default;
BattleView::~BattleView() { shutdown(); }

bool BattleView::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::MeshPipeline& meshes,
                      const data::GameTables& tables, audio::Mixer* mixer) {
  shutdown();
  ctx_ = &ctx;
  vfs_ = &vfs;
  // Enemy AI = the scene's battle script (battle/event/evtNNN.pac, else the shared evt0000.pac):
  // slot 2 plans each enemy turn, slot 3 / 4 react to deaths / hits.
  brain_.set_loader([this](int scene) -> std::optional<Bytes> {
    if (auto bytes = vfs_->read(std::format("cdrom/battle/event/evt{:03d}.pac", scene))) return bytes;
    return vfs_->read("cdrom/battle/event/evt0000.pac");
  });
  lang_tables_.init(&vfs);
  sprites_ = &sprites;
  meshes_ = &meshes;
  tables_ = &tables;
  mixer_ = mixer;
  audio_.init(mixer, &vfs);
  model_cache_.fallback_atk.parse(make_fallback_block());
  if (!stage_pipe_.init(ctx)) return false;
  if (!linear_.create(ctx, true) || !nearest_.create(ctx, false)) return false;
  if (!text_.init(ctx, sprites)) return false;
  if (!particles_.init(ctx)) return false;

  // Fonts: RDE's font slots through ui::font::FontSet; original art loads the small_font_* files of the same slots.
  fonts_.init(vfs, text_);
  fonts_.set_classic(!hd_);
  if (fonts_.face(ui::font::kBattle) == render::kNoFace) {
    log::error("battle: no usable font");
    return false;
  }
  fonts_.prepare(ui::font::kBattle, U" !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]abcdefghijklmnopqrstuvwxyz");

  auto load_tex = [&](Tex& tex, const char* path) {
    auto png = vfs.read(path);
    render::DecodedImage img;
    if (!png || !render::decode_image(*png, img) || !tex.tex.create(ctx, img.width, img.height, render::TextureFormat::Rgba8, img.rgba.data())) return false;
    tex.set = render::TexRef(tex.tex, linear_);
    tex.w = static_cast<float>(img.width);
    tex.h = static_cast<float>(img.height);
    return static_cast<bool>(tex.set);
  };
  const u8 px[4] = {255, 255, 255, 255};
  if (!white_.tex.create(ctx, 1, 1, render::TextureFormat::Rgba8, px)) return false;
  white_.set = render::TexRef(white_.tex, nearest_);
  if (!load_tex(win_base_, "hd/battle/shapes/battle-win4-large-base.png") || !load_tex(win_frame_, "hd/battle/shapes/battle-win4-large-frame.png"))
    log::warn("battle: HD panel PNGs missing, HD mode uses plain boxes");
  for (size_t i = 0; i < plates_.size(); ++i)
    if (!load_tex(plates_[i], std::format("hd/menu/menuelem/menuelem_{}.png", i + 1).c_str())) log::warn("battle: element plate {} missing", i + 1);
  load_hud_art(vfs);

  ready_ = true;
  return true;
}

void BattleView::unload_scene() {
  for (Visual& visual : vis_) visual = Visual{};
  showcase_.clear();
  entry_shown_.fill(false);
  ghost_view_.fill(false);
  fx_conceal_.fill(false);
  hud_off_ = false;
  hud_script_off_ = false;
  colour_row_ = {};
  // Everything the script presentation keeps between frames starts clean in a new battle (a fight left by running away mid-window
  // must not leave a half-open window / pending position / engaged PC / menu-facing memory in the next scripted fight).
  smsg_ = ScriptMsg{};
  smsg_pos_set_ = false;
  smsg_pos_x_ = smsg_pos_y_ = 0;
  script_actor_ = -1;
  engaged_ = engage_arrive_ = engaged_after_cast_ = false;
  engaged_target_ = -1;
  engage_interrupted_ = false;
  menu_face_ui_ = Ui::Intro;
  menu_face_pc_ = tgt_face_unit_ = -2;
  last_actor_ = -1;
  last_kind_ = battle::Command::Kind::Attack;
  script_stream_.clear();
  script_prev_vt_ = 0;
  script_gap_ = -1;
  script_passed_ = 0;
  script_stall_ = 0;
  fx_reset_scene();
  anm_reset();
  model_cache_.clear();
  stage_scene_.clear();
  battle_.reset();
  pops_.clear();
  pop_rects_ = {};
  log_.clear();
  ops_.clear();
  queue_.clear();
  pending_.clear();
  act_ = Act{};
  finish_ = Finish::None;
  victory_song_ = -1;
}

void BattleView::shutdown() {
  if (!ctx_) return;
  free_transition();
  unload_scene();
  for (Tex& tex : hud_) {
    tex.tex.destroy();
    tex.set = {};
  }
  for (Tex* tex : {&white_, &win_base_, &win_frame_}) {
    tex->tex.destroy();
    tex->set = {};
  }
  for (Tex& tex : plates_) {
    tex.tex.destroy();
    tex.set = {};
  }
  text_.destroy();
  // The effect screen warp target.
  fx_warp_set_ = {};
  fx_warp_rt_.destroy();
  for (int i = 0; i < 2; ++i) {
    fx_acc_set_[i] = {};
    fx_acc_rt_[i].destroy();
  }
  fx_copy_set_ = {};
  fx_copy_rt_.destroy();
  particles_.destroy();
  stage_pipe_.destroy();
  linear_.destroy();
  nearest_.destroy();
  ctx_ = nullptr;
  ready_ = false;
}

void BattleView::set_art_mode(bool hd, bool force) {
  const bool changed = hd_ != hd || force;
  hd_ = hd;
  // HUD text: small_ / large_ font files of the same slots; the HUD art is the same in both modes.
  fonts_.set_classic(!hd);
  if (!changed || !ready_ || model_cache_.models.empty()) return;
  // Art-mode switch during a battle = a pure rendering swap: the unit models are read again through the VFS, which now
  // resolves battle/player|monster through the other file set (cdrom2 = other mesh / UV atlas / .ltc; the clips are the same data),
  // and their mesh + GPU copy replace the old ones *inside* the shared ModelHandle, so UnitPose (which points at
  // ModelHandle::asset), the scripts, the battle state and every unit's pose / position stay. Stages (battle/bg) and the effect packs have no
  // cdrom2 variant; the weapons have none either but are rebuilt the same way for the texture mods (a mod switched on / off).
  // Pack models that took their battle model's cdrom2 mesh + texture (fx_hd_counterpart) are rebuilt at the pack's next cast; the
  // pack playing now keeps its copy to its end (the pseudo-actor points into it).
  for (auto it = fx_gpu_.begin(); it != fx_gpu_.end();)
    it = it->second && it->second->art_dependent && !(fx_pack_ && fx_pack_->eid == it->first) ? fx_gpu_.erase(it) : std::next(it);
  std::vector<std::string> stems;
  for (const auto& [stem, handle] : model_cache_.models)
    if (handle) stems.push_back(stem);
  int swapped = 0, kept = 0;
  for (const std::string& stem : stems) {
    const std::shared_ptr<ModelHandle> live = model_cache_.models[stem];
    model_cache_.models.erase(stem);
    const std::shared_ptr<ModelHandle> fresh = load_model_for(stem);
    model_cache_.models[stem] = live;
    if (fresh && fresh->gpu && fresh->asset.skeleton.joints.size() == live->asset.skeleton.joints.size() &&
        fresh->asset.clips.size() == live->asset.clips.size()) {
      live->asset = std::move(fresh->asset);
      live->gpu = std::move(fresh->gpu);
      ++swapped;
    } else {
      ++kept;
    }
  }
  if (vfs_ && meshes_) model_cache_.reload_weapons(*vfs_, *meshes_);
  log::info("battle: art mode {} swapped in place: {} unit models rebuilt, {} kept; battle state untouched", hd ? "hd" : "original", swapped, kept);
}

bool BattleView::start(int scene_id, std::vector<battle::PartyMember> party) { return start(scene_id, std::move(party), StartOptions{}); }

bool BattleView::start(int scene_id, std::vector<battle::PartyMember> party, const StartOptions& opt) {
  if (!ready_) return false;
  // The host may have run the load in slices behind the field's swirl / hold (begin_load + load_step); a plain start() does it all at once.
  if (!(load_ && load_->scene == scene_id)) begin_load(scene_id, std::move(party), opt);
  load_step(1e9);
  return finish_start();
}

bool BattleView::begin_load(int scene_id, std::vector<battle::PartyMember> party, const StartOptions& opt) {
  if (!ready_) return false;
  load_ = std::make_unique<LoadJob>();
  load_->scene = scene_id;
  load_->party = std::move(party);
  load_->opt = opt;
  load_->t0 = std::chrono::steady_clock::now();
  // The lazy fallback: a no-op when the host already warmed the view at title / room load.
  warm_up();
  return true;
}

// The one-time work of the first battle of a session - the effects library init (~16 ms) and the two ~190 MB JP HUD
// fonts - started by the host at title / first room load so the swirl and the white pay nothing. Idempotent; begin_load calls it again as the fallback.
void BattleView::warm_up() {
  if (!ready_) return;
  prefetch_fonts();
  if (!fxlib_ready_ && !fxlib_job_.valid()) fxlib_job_ = std::async(std::launch::async, [this] { return fxlib_.init(*vfs_); });
}

// The battle load in slices. Each step is one piece of the load: rules, stage files,
// stage scene, the shared battle.prd, one unit model each, the HUD fonts. Steps run until `budget_ms` of wall time is used (at least one per
// call). Returns true when the load is complete (or failed).
bool BattleView::load_step(double budget_ms) {
  if (!ready_ || !load_ || load_->done) return true;
  const auto t_call = std::chrono::steady_clock::now();
  load_->block = budget_ms >= 1e6;
  while (!load_->done) {
    const auto t_step = std::chrono::steady_clock::now();
    const int step = load_->step;
    run_load_step(*load_);
    // Not ready (the JP font worker): try again next frame.
    if (load_->wait) {
      load_->wait = false;
      break;
    }
    // A multi-call step (the stage scene's phases) has more to do: same step, budget check below.
    if (load_->again) {
      load_->again = false;
      const auto t_a = std::chrono::steady_clock::now();
      const double ms_a = std::chrono::duration<double, std::milli>(t_a - t_step).count();
      load_->ms += ms_a;
      log::debug("timing: battle load step {} (part): {:.1f} ms", step, ms_a);
      if (std::chrono::duration<double, std::milli>(t_a - t_call).count() >= budget_ms) break;
      continue;
    }
    ++load_->step;
    const auto t_end = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t_end - t_step).count();
    load_->ms += ms;
    log::debug("timing: battle load step {}: {:.1f} ms", step, ms);
    if (std::chrono::duration<double, std::milli>(t_end - t_call).count() >= budget_ms) break;
  }
  return load_->done;
}

void BattleView::prefetch_fonts() {
  // kb_70 / kt_70 are ~190 MB each: read on a worker thread, behind the swirl.
  fonts_.prefetch(ui::font::kKb);
  fonts_.prefetch(ui::font::kBattleJp);
  fonts_.prefetch(ui::font::kCh);
}

void BattleView::run_load_step(LoadJob& job) {
  const int scene_id = job.scene;
  const StartOptions& opt = job.opt;
  // The start flags are read by the theme (flags 1 and 0x40) and the victory song (flag 0x10).
  start_flags_ = opt.flags;
  auto fail = [&] {
    job.failed = true;
    job.done = true;
  };
  enum { kRules, kStageRead, kStageScene, kCommon, kUnits0, kFonts = kUnits0 + battle::kUnits };
  // (kFonts is followed by two more steps: the JP face and the headline face)
  if (job.step == kRules) {
  unload_scene();
  // Reset the GetHokanFrame chain state and the weapon hand toggle.
  bseq::Vm::reset_battle();
  hand_toggle_ = 1;
  finished_ = false;
  route_events_.clear();
  scene_id_ = scene_id;
  if (scene_id < 0 || static_cast<size_t>(scene_id) >= tables_->scenes.size()) {
    log::error("battle: scene {} out of range", scene_id);
    return fail();
  }
  if (job.party.empty())
    for (int id : {0, 1, 22}) job.party.push_back(battle::make_party_member(*tables_, id, 0, "char"));

  u32 seed = static_cast<u32>(std::chrono::steady_clock::now().time_since_epoch().count());
  // Dev: OC_BATTLE_SEED makes fights repeatable.
  if (const char* env = std::getenv("OC_BATTLE_SEED")) seed = static_cast<u32>(std::strtoul(env, nullptr, 10));
  // The random camera picks (intro / party intro timelines, their shot groups, the approach shots) differ from battle to battle.
  std::srand(seed);
  rng_ = std::make_unique<battle::GameRng>(seed, tables_->rng_table);
  spawn_rng_.seed(seed);
  battle_ = std::make_unique<battle::Battle>(*tables_, *rng_, brain_);
  battle_->set_invincible(dbg_.inv_party, dbg_.inv_enemies);
  battle_->set_cheats(battle_cheats());
  brain_.set_enabled(!dbg_.no_script);
  battle::BattleOptions bopt;
  bopt.ng_plus = opt.ng_plus;
  bopt.gold_bonus = opt.gold_bonus;
  bopt.battlefield = opt.battlefield;
  bopt.growth = opt.growth;
  bopt.bench = opt.bench;
  bopt.dopple_mask = opt.dopple_mask;
  bopt.piggy_bank = opt.piggy_bank;
  bopt.monster_party = opt.monster_party;
  // Dev: OC_DOPPLE_SLOTS sets the Dopplegang pool slots Big Catch picks from ("26,106"; "all" = all 256 bits, every save var 0x3E0..0x3FE 0xFFFF,
  // Big Catch skips bits past the 220-row table; "debug" = the debug list: every table row whose second halfword (`val`) is non-zero, pool bits ignored).
  if (const char* env = std::getenv("OC_DOPPLE_SLOTS")) {
    const std::string list = env;
    if (list == "all")
      bopt.dopple_mask.fill(0xFFFF);
    if (list == "debug")
      for (size_t row = 0; row < battle::kDoppleTable.size(); ++row)
        if (battle::kDoppleTable[row].val != 0) bopt.dopple_mask[row >> 4] |= static_cast<u16>(1u << (row & 15));
    for (size_t pos = 0; list != "all" && list != "debug" && pos < list.size();) {
      const int slot = std::atoi(list.c_str() + pos);
      if (slot >= 0 && slot < 256) bopt.dopple_mask[static_cast<size_t>(slot >> 4)] |= static_cast<u16>(1u << (slot & 15));
      const size_t comma = list.find(',', pos);
      pos = comma == std::string::npos ? list.size() : comma + 1;
    }
  }
  if (!battle_->start(std::move(job.party), scene_id, bopt)) {
    log::error("battle: Battle::start failed for scene {}", scene_id);
    battle_.reset();
    return fail();
  }
  boss_scene_ = tables_->scenes[static_cast<size_t>(scene_id)].boss();
  direct_id_ = opt.battle_id < 0 || opt.battle_id > 0x1FF;

  // Camera / sound banks: the stage first (its own cameras win), then the shared battle.prd (zen0, apr*, atk* shots, "fade", chih sounds).
  bank_ = CamBank{};
  audio_.begin_battle();
  camera_ = BattleCamera{};
  camera_.set_bank(&bank_);
  camera_.set_anchor([this](int unit, int point) { return anchor_frame(unit, point); });
  // Timeline op 0x22: the emitter of that name in the loaded resources: the stage's, else the acting unit's own (a fierce attack's at0.prd timelines).
  camera_.set_emitter_hook([this](const std::string& name) {
    if (stage_scene_.start_emitter(name) || act_.actor < 0) return;
    bseq::Event effect;
    effect.kind = bseq::Event::Effect;
    for (size_t i = 0; i < name.size() && i < 4; ++i) effect.tag |= static_cast<u32>(static_cast<u8>(name[i])) << (8 * i);
    anm_effect(act_.actor, effect);
  });
  // Timeline ops 0x3E-0x40: the same screen copy warp as the effect packs' (one global state).
  camera_.set_warp_hook([this](const CamCmd& cmd) {
    assets::SeqCmd seq_cmd;
    seq_cmd.op = cmd.op;
    seq_cmd.data = cmd.data;
    fx_warp_op(seq_cmd);
  });
  // Timeline ops 0x2B / 0x2C: the screen colour layers.
  camera_.set_screen_hook([this](const CamCmd& cmd) {
    assets::SeqCmd seq_cmd;
    seq_cmd.op = cmd.op;
    seq_cmd.data = cmd.data;
    fx_screen_colour(cmd.op == 0x2C ? 1 : 0, seq_cmd);
  });
  // Timeline op 0x50 waits while the acting unit's script runs; a per-target timeline repeats while the action has a next target.
  camera_.set_actor_busy_hook([this] { return fx_actor_busy(); });
  camera_.set_target_hook([this](int index) {
    std::vector<int> targets = act_.targets;
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return index >= 0 && index < static_cast<int>(targets.size());
  });
  const int scene_bg = tables_->scenes[static_cast<size_t>(scene_id)].bg();
  int bg = (opt.battlefield >= 0 && opt.battlefield < 100) ? opt.battlefield : scene_bg;
  // Dev: OC_BATTLE_FIELD forces the stage.
  if (const char* env = std::getenv("OC_BATTLE_FIELD"); env && *env) bg = std::atoi(env);
  const std::string base = std::format("battle/bg/bg_{:02d}", bg);
  log::info("battle: scene {} stage bg_{:02d} (operand {}, scene default bg_{:02d})", scene_id, bg, opt.battlefield, scene_bg);
    prefetch_unit_models();
    job.base = base;
    return;
  }
  if (job.step == kStageRead) {
    // The worker's library init (started by begin_load) is never joined while it runs, unless the whole load is synchronous.
    if (fxlib_job_.valid()) {
      if (!job.block && fxlib_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        job.wait = true;
        return;
      }
      fxlib_ready_ = fxlib_job_.get();
    }
    const std::string& base = job.base;
    if (auto prd = vfs_->read("cdrom/" + base + ".prd")) {
      job.prd = std::move(*prd);
      job.ctd = vfs_->read("cdrom/" + base + ".ctd").value_or(Bytes{});
      job.has_prd = true;
    }
    // Only when the worker failed.
    if (!fxlib_ready_) fxlib_ready_ = fxlib_.init(*vfs_);
    return;
  }
  if (job.step == kStageScene) {
    const std::string& base = job.base;
    if (job.has_prd) {
      const Bytes* prd = &job.prd;
      const Bytes& ctd = job.ctd;
      std::string err;
      if (!job.stage_begun) {
        std::vector<const assets::RoomEffects*> sheet_libs;
        if (fxlib_ready_) {
          if (const auto res = fxlib_.resident())
            for (const auto& resident_pack : res->packs) sheet_libs.push_back(resident_pack.get());
          sheet_libs.push_back(&fxlib_.particle_lib());
        }
        // Every emitter in battle uses position shift 5 (set before the load: the emitters read it there); the scene group names the texture
        // dump / replacement (bg_NN).
        stage_scene_.set_pos_shift(5);
        stage_pipe_.set_scene_group("battle_stage/" + std::filesystem::path(base).filename().string());
        stage_scene_.load_begin(*ctx_, stage_pipe_, *prd, ctd, sprites_, std::move(sheet_libs));
        job.stage_begun = true;
      }
    const auto result = stage_scene_.load_step(job.block ? 1e9 : kStageBudgetMs, &err);
    // The stage decode is still running on its worker: the same step on a later frame.
    if (result == StageScene::LoadResult::Wait) {
      job.wait = true;
      return;
    }
    // The same step again next call (the loop in load_step() decides by the frame budget).
    if (result == StageScene::LoadResult::More) {
      job.again = true;
      return;
    }
    if (result == StageScene::LoadResult::Failed) log::warn("battle: stage {}: {}", base, err);
    bank_.add_drp(*prd);
    audio_.load_group("stage", *prd, -1);
    } else {
      log::warn("battle: stage {} not found", base);
    }
    job.prd = Bytes{};
    job.ctd = Bytes{};
    return;
  }
  if (job.step == kCommon) {
  if (auto common = vfs_->read("cdrom/battle/battle.prd")) {
    bank_.add_drp(*common);
    audio_.load_group("battle", *common, -1);
  }
  audio_.activate("stage");
  audio_.activate("battle");
  log::info("battle: camera bank {} shots / {} timelines", bank_.cameras(), bank_.sequences());
  if (!camera_.select("zen0")) log::warn("battle: no zen0 camera");

    return;
  }
  if (job.step >= kUnits0 && job.step < kFonts) {
    if (!job.block && !model_cache_.prefetch_ready()) {
      job.wait = true;
      return;
    }
    const int unit_id = job.step - kUnits0;
    const battle::Unit& unit = battle_->unit(unit_id);
    Visual& visual = vis_[static_cast<size_t>(unit_id)];
    visual.hp_disp = static_cast<float>(unit.hp);
    visual.hp_shown = unit.hp;
    if (!unit.present) return;
    bind_unit_model(unit_id);
    if (unit.pc && unit.form < 0 && visual.weapon)
      if (const data::GearRec* gear = tables_->gear_rec(unit.eq.weapon_id)) {
        visual.weapon_prism = gear->tier() == 5;
        log::debug("battle: unit {} weapon {:#x} tier byte {:#x}{}", unit_id, unit.eq.weapon_id, gear->tier(), visual.weapon_prism ? " (prism lights)" : "");
      }
    return;
  }
  // kFonts .. kFonts + 2: the HUD faces read by prefetch_fonts (kb_70, kt_70, the ch_100 command headline); one face per call (each builds its
  // first glyph atlas: ~10 ms in Release), retried on a later frame while the worker is still reading the file.
  const int face = job.step - kFonts;
  const int slot = face == 0 ? ui::font::kKb : face == 1 ? ui::font::kBattleJp : ui::font::kCh;
  if (!job.block && !fonts_.ready(slot)) {
    job.wait = true;
    return;
  }
  fonts_.prepare(slot, face == 0 ? U"●" : face == 1 ? U"♂♀" : U"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyzÉé ");
  if (face == 2) job.done = true;
}

// The models and weapons bind_unit_model will load for the battle's units, read and decoded on a worker while the stage loads.
void BattleView::prefetch_unit_models() {
  std::vector<std::string> stems;
  std::vector<std::pair<std::string, int>> weapon_parts;
  for (int unit_id = 0; unit_id < battle::kUnits; ++unit_id) {
    const battle::Unit& unit = battle_->unit(unit_id);
    if (!unit.present) continue;
    stems.push_back(unit_model_stem(unit));
    if (unit.pc && unit.form < 0) {
      if (const data::GearRec* gear = tables_->gear_rec(unit.eq.weapon_id)) weapon_parts.emplace_back(ModelCache::player_weapon_bin(unit.src_id), gear->tier() & 0xF);
      continue;
    }
    const data::StatRec& er = tables_->enemies[static_cast<size_t>(unit.form >= 0 ? unit.form : unit.src_id)];
    if (er.weapon() != 0 && er.weapon_model() != 0) {
      const data::GearRec* wg = tables_->gear_rec(er.weapon());
      weapon_parts.emplace_back(ModelCache::enemy_weapon_bin(static_cast<int>(er.model())), wg ? (wg->tier() & 0x7F) : 0);
    }
  }
  model_cache_.prefetch(*vfs_, std::move(stems), std::move(weapon_parts));
}

// One unit's model, scripts, sound group, weapon mesh and extents from its Unit record: the load step, and a Dopplegang form change.
void BattleView::bind_unit_model(int unit_id) {
  const battle::Unit& unit = battle_->unit(unit_id);
  Visual& visual = vis_[static_cast<size_t>(unit_id)];
  // A PC in a creature form uses the creature's model, scripts and weapon.
  const bool pcm = unit.pc && unit.form < 0;
  const int mon = unit.form >= 0 ? unit.form : unit.src_id;
  const std::string stem = unit_model_stem(unit);
  visual.weapon = nullptr, visual.weapon_attach = -1, visual.weapon_prism = false;
  visual.attrib = unit.innate;
  visual.spawn_joints = 0;
  visual.tech_clips.clear();
  visual.pose.set_ext_clips(nullptr);
  if (!pcm) log::info("battle: unit {} {} monster {} model {} -> {} (AP {} countdown {} HP {})", unit_id, unit.name, mon, tables_->enemies[static_cast<size_t>(mon)].model(), stem, unit.ap, unit.countdown, unit.max_hp);
  if (auto handle = load_model_for(stem)) {
    visual.model = handle;
    visual.has_model = true;
    visual.pose.set_model(&handle->asset);
    visual.scripts = load_scripts_for(stem);
    {
      // S5 entry 8 is played for a unit that is not seeded off-stage, and its op 0x22 clears the shadow bit for the whole battle
      // (ki / q_con2 / sakana / fate / tokikui). A seeded unit runs entry 8 when the battle script brings it on.
      const int sh = visual.scripts && visual.scripts->has_s5 ? visual.scripts->s5.initial_shadow() : -1;
      shadow_off_[static_cast<size_t>(unit_id)] = sh == 0 && !brain_.hidden(unit_id);
    }
    // Its first tick's part hides / blinks (sakana's fins).
    anm_spawn_parts(unit_id);
    visual.sound_key = stem + "#" + std::to_string(std::max(0, weapon_digit(unit)));
    if (visual.scripts && !visual.scripts->at0.empty()) audio_.load_group(visual.sound_key, visual.scripts->at0, pcm ? weapon_digit(unit) : 0);
    float ext = 0;
    for (const assets::Mesh& mesh : handle->asset.meshes)
      for (const assets::SkinVertex& sv : mesh.pool) ext = std::max(ext, std::fabs(sv.bind_pos[1]));
    visual.height = ext > 50 ? ext : 1000.0f;
    float ax = 0, ay = 0, az = 0;
    if (pcm) log::info("battle: unit {} model extent {:.0f}", unit_id, ext);
    if (visual.pose.attach_rest(15, ax, ay, az)) visual.reach15 = std::hypot(ax, az);
    if (visual.pose.attach_rest(16, ax, ay, az)) visual.reach16 = std::hypot(ax, az);
    // Weapon mesh on the model's mount record 0 (attach point + offset + rotation).
    if (pcm) {
      const auto& pts = handle->asset.attach.points;
      const assets::MountPoint& mount = handle->asset.attach.mounts[0];
      if (const data::GearRec* gear = tables_->gear_rec(unit.eq.weapon_id)) {
        visual.weapon = load_weapon(unit.src_id, gear->tier() & 0xF);
        // An all-zero record (Viper, Zoah, Poshul ...: no mount object) uses attach 0x12 (grip) when the model
        // has it, else the right hand (07), else 15 (status screen rule).
        visual.weapon_attach = mount.used ? mount.attach : pts[0x12].used ? 0x12 : pts[7].used ? 7 : 15;
        log::debug("battle: unit {} weapon mount: attach {} (joint {}) offset {:.0f} {:.0f} {:.0f} rot {:.0f} {:.0f} {:.0f}{}", unit_id, visual.weapon_attach,
                   pts[static_cast<size_t>(visual.weapon_attach)].joint, mount.pos[0], mount.pos[1], mount.pos[2], mount.rot[0], mount.rot[1], mount.rot[2],
                   mount.used ? "" : " (no mount record: attach rule)");
      }
    }
    // Monster weapon: weapon item and weapon model both non-zero.
    if (!pcm) {
      const data::StatRec& er = tables_->enemies[static_cast<size_t>(mon)];
      if (er.weapon() != 0 && er.weapon_model() != 0) {
        const auto& pts = handle->asset.attach.points;
        const assets::MountPoint& mount = handle->asset.attach.mounts[0];
        const data::GearRec* wg = tables_->gear_rec(er.weapon());
        visual.weapon = load_enemy_weapon(static_cast<int>(er.model()), wg ? (wg->tier() & 0x7F) : 0);
        visual.weapon_attach = mount.used ? mount.attach : pts[0x12].used ? 0x12 : pts[7].used ? 7 : 15;
        log::info("battle: unit {} {} weapon_model {} -> {} attach {} (joint {}){} slot1 {} entry-8 hide mask {}", unit_id, unit.name, er.weapon_model(),
                  visual.weapon ? "loaded" : "FAILED", visual.weapon_attach, pts[static_cast<size_t>(visual.weapon_attach)].joint,
                  mount.used ? "" : " (no mount record: attach rule)", handle->asset.attach.mounts[1].used ? "record" : "none",
                  visual.scripts && visual.scripts->has_s5 ? visual.scripts->s5.initial_weapon_hide() : 0);
      }
    }
  } else {
    log::warn("battle: unit {} ({}) has no model", unit_id, unit.name);
  }
}

bool BattleView::finish_start() {
  if (!load_ || load_->failed || !battle_) {
    load_.reset();
    return false;
  }
  const int scene_id = scene_id_;
  place_units();
  for (int i = 0; i < battle::kUnits; ++i)
    if (battle_->unit(i).present && vis_[static_cast<size_t>(i)].has_model) {
      vis_[static_cast<size_t>(i)].pose.play(1, 0);
      // The spawn script, not the idle (an off-stage seeded enemy: the idle; it plays entry 8 when brought on).
      start_spawn(i);
      // The height word = y of attach point 14 in the pose just built = the idle clip's first keyframe (Serge 1105, Sand Squirt 931,
      // Komodo Pup 945; the bind pose gives 1160 for Serge); y is down.
      float hx = 0, hy = 0, hz = 0;
      if (vis_[static_cast<size_t>(i)].pose.attach_now(14, 1.0f, hx, hy, hz) && -hy > 50.0f) vis_[static_cast<size_t>(i)].height = -hy;
      if (i < battle::kPcSlots) log::info("battle: unit {} height word {:.0f}", i, vis_[static_cast<size_t>(i)].height);
    }
  if (showcase_on_) showcase_load();

  ui_ = Ui::Intro;
  // Consumed by the first idle command-camera pass.
  fx_screen_menu_clear_ = true;
  unplaced_.fill(false);
  active_pc_ = -1;
  menu_chime_ = true;
  top_cursor_ = chain_cursor_ = 0;
  // The per-unit memory bytes start clear (target memory 3) in a new battle.
  std::fill(std::begin(top_mem_), std::end(top_mem_), 0);
  std::fill(std::begin(tgt_mem_), std::end(tgt_mem_), 3);
  // Dev: OC_DEV_TGTMEM=<unit> starts every PC's remembered target there (scene 266: 4 = the
  // hidden Bomber, so `--auto` keeps hitting it and the whole swell sequence plays without moving the cursor)
  if (const char* tgt_mem = std::getenv("OC_DEV_TGTMEM"); tgt_mem && *tgt_mem) std::fill(std::begin(tgt_mem_), std::end(tgt_mem_), std::atoi(tgt_mem));
  elem_shown_ = -1, elem_state_ = 0;
  chain_.clear();
  time_ = 0;
  intro_ticks_ = 0;
  intro_layer_cut_ = false;
  orbit_pc_ = dyn_target_ = -1;
  menu_glide_ = false;
  orbit_blocked_ = orbit_block_next_ = false;
  auto_delay_ = 0;
  auto_pc_ = auto_target_ = -1;
  auto_step_ = 0;

  // Intro: the stage's "bti*" timeline (a random one of those present; "boss" for forced boss scenes), plus "fade" for field battles.
  camera_.reset_fade();
  field_battle_ = true;
  std::vector<std::string> intros;
  for (int k = 0; k < 8; ++k) {
    const std::string name = std::format("bti{}", k);
    if (bank_.sequence(name)) intros.push_back(name);
  }
  std::string pick;
  // 'boss' when the battle id is > 0x1FF (a direct scene) and the stage has one, else a random bti*.
  if (scene_id >= 0 && direct_id_ && bank_.sequence("boss")) pick = "boss";
  else if (!intros.empty()) pick = intros[static_cast<size_t>(std::rand()) % intros.size()];
  // Dev: OC_BATTLE_INTRO forces the intro timeline.
  if (const char* env = std::getenv("OC_BATTLE_INTRO"); env && bank_.sequence(env)) pick = env;
  // The stage intro and 'fade' start in tick_camera (start_stage_intro): at once without the RDE hand-over, when the RDE load reaches them under it.
  intro_pending_ = pick;
  intro_started_ = false;
  // Party intro: a random one of the stage's `pli0..pli7`, when the load ends (tick_camera). In a field battle it cuts the stage intro then
  // (every camera shot torn down, every timeline but 'loop' ended, the screen-copy warp cleared); when the intro holds (op 0x73) it waits for the
  // `bti*` timeline and its shot. The party is placed in ascending character id: the first one when the load ends, the others with the party intro.
  std::vector<std::string> party_intros;
  for (int k = 0; k < 8; ++k) {
    const std::string name = std::format("pli{}", k);
    if (bank_.sequence(name)) party_intros.push_back(name);
  }
  pli_pending_.clear();
  pli_after_load_ = false;
  if (!party_intros.empty()) {
    pli_pending_ = party_intros[static_cast<size_t>(std::rand()) % party_intros.size()];
    pli_after_load_ = true;
    for (int i = 0; i < battle::kPcSlots; ++i) unplaced_[static_cast<size_t>(i)] = battle_->unit(i).present;
  }
  audio_.start_battle_theme(start_flags_, field_battle_ && (start_flags_ & 1));

  for (const std::string& line : battle_->drain_log()) log::debug("battle: {}", line);
  // Enemies that act first.
  collect_events();
  // The target window's glyphs that need the big JP faces (loaded here, not on the first target selection): the innate "●" (style 0xB =
  // slot 0, kb_70) and the sex glyph (XLAT(♂ / ♀) has no translation: it keeps the JP font = slot 3, kt_70).
  log::info("battle: scene {} ready in {} ms (load steps {:.0f} ms)", scene_id_, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - load_->t0).count(), load_->ms);
  load_.reset();
  return true;
}

// Formation: the scene record's bytes 6..14 give one position byte per unit slot (3 PCs, then up to 6
// enemy instances) and bytes 0x11..0x19 one facing byte per slot. The position is
// x = (2 * (b & 15) - 15) * 256, z = (15 - 2 * (b >> 4)) * 256 in world units, and the yaw word is byte << 9 (4096 = 360 deg).
// World x/z are the stage's x/z.
void BattleView::place_units() {
  const auto& raw = tables_->scenes[static_cast<size_t>(scene_id_)].raw;
  for (int i = 0; i < battle::kUnits; ++i) {
    Visual& visual = vis_[static_cast<size_t>(i)];
    const u8 pos = raw[static_cast<size_t>(6 + i)];
    visual.x = static_cast<float>((2 * (pos & 15) - 15) * 256);
    visual.z = static_cast<float>((15 - 2 * (pos >> 4)) * 256);
    const float yaw = static_cast<float>(raw[static_cast<size_t>(0x11 + i)]) * (3.14159265f / 4.0f);
    // Yaw 90 deg (PCs) looks toward -x, 270 deg (enemies) toward +x (world frame, forward = (-sin yaw, -cos yaw) like the model matrix; the view
    // mirrors x, see Cam::view_proj).
    visual.face_x = -std::sin(yaw);
    visual.face_z = -std::cos(yaw);
    visual.px = visual.x;
    visual.pz = visual.z;
    visual.pface_x = visual.face_x;
    visual.pface_z = visual.face_z;
    visual.home_x = visual.x;
    visual.home_z = visual.z;
    visual.home_fx = visual.face_x;
    visual.home_fz = visual.face_z;
  }
}

}  // namespace oc::game
