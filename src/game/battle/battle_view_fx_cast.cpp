// BattleView: element / tech effect cast lifecycle: start and end hooks, charge, fog, hit queueing, damage and unit hiding.
//
// Timing: kCastLead ticks after the confirm the cast's effect start runs:
// the casters play S5 entry 5, the chant "sing" / "msin" sounds and the colour's charge pack "ch0N" (battle.prd > sato) plays on the caster with
// one of its cameras ch10..ch13. Its op 0x41 marks the element pack ready; the charge is then killed, the
// depth cue reset and the pack's 'main' / 'own' started. The pack's ops drive the rest: 0x4F (caster S5 entry, 6 = release), 0x50 (wait for it),
// 0x23 cameras, 0x2D fog ramps, 0x46 / 0x44 (target reaction from the pack's 0x1A table, variant 7), 0x47 / 0x48 CallDamage (the number, the
// HP and the tint flash); hits it never lands are released when the effect ends. Packs without files, consumables and traps use the generic
// path (entry 6 + the 4 + 6k stagger). Camera tracks play on BattleCamera through a per-effect bank, AKAO sounds (op 0x4C) through BattleAudio,
// bespoke overlays through the BespokeRegistry.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <set>
#include <string_view>

#include "core/log.h"
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"

namespace oc::game {

using namespace bv;

namespace {

// Op 0x4C: looks the named sound up over the whole resource list and plays it with the mask, pan and volume;
// a name nobody holds plays nothing.
void fx_play_sound(BattleAudio& audio, std::string_view name, int volume, int pan8) {
  if (!audio.play(std::string(name), volume, pan8 / 2)) log::debug("battle audio: no sound '{}' (op 0x4C)", name);
}

}  // namespace

// The effect start saves each listed unit's shadow-off flag; the effect end writes it back.
// The hide bit survives a pack end: the idle restart shows a living unit again; a dead one has no idle and stays hidden through the next pack.
void BattleView::fx_show_living() {
  for (int i = 0; i < battle::kUnits; ++i) {
    if (battle_->unit(i).present && !battle_->unit(i).in_turn_order()) continue;
    fx_hidden_[static_cast<size_t>(i)] = false;
    fx_hide_bits_[static_cast<size_t>(i)] = 0;
  }
}

// A unit's .obj and .ltc read back to back.
int BattleView::fx_model_read_ticks(int unit) const {
  const std::string stem = unit_model_stem(battle_->unit(unit));
  size_t bytes = 0;
  for (const char* extension : {".obj", ".ltc"})
    if (const auto file = vfs_->read("cdrom/" + stem + extension)) bytes += (file->size() + 2047) / 2048 * 2048;
  return effects::disc_read_ticks(bytes);
}

void BattleView::fx_hook_start() { shadow_saved_off_ = shadow_off_; }
void BattleView::fx_hook_end() { shadow_off_ = shadow_saved_off_; }

void BattleView::fx_reset_scene() {
  fx_slow_n_ = fx_slow_x_ = fx_slow_phase_ = 0;
  // Screen colours, depth cue off, the warp bracket off.
  fx_screen_ = {};
  fx_warp_ = {};
  fx_unit_ = {};
  fx_ambient_stage_reset();
  // Shadows start on at the model init; the saved flags are zero until the first start hook.
  shadow_off_.fill(false);
  shadow_saved_off_.fill(true);
  fx_fog_on_ = false;
  fx_lens_wait_ = 0;
  fx_unit_fog_.fill(false);
  fx_.reset();
  // The ground overlay is freed.
  stage_scene_.ground_overlay_end();
  fx_pack_.reset();
  fx_gpu_.clear();
  fx_lib_gpu_.reset();
  fx_lib_key_ = nullptr;
  fxlib_.clear_cache();
  fx_hidden_.fill(false);
  fx_hide_bits_.fill(0);
  fx_lit_.fill(false);
  fx_fired_.clear();
  fx_textures_.reset(*sprites_);
  fx_shake_x_ = fx_shake_y_ = 0;
  fx_faded_ = false;
  fx_cam_.reset();
  fx_cam_old_.reset();
  fx_prev_bank_ = nullptr;
  fx_charge_ = fx_ready_ = false;
  fx_fog_reset();
  fog_left_ = fog_total_ = 0;
}

// ---- charge ---------------------------------------------------------------------------------------------------------------------------
// The action's effect start: instantiates the type-0x25 "ch0<colour>" of battle.prd > sato
// (colour = the first set bit of the action's colour byte, i.e. data::colour_index_of_bit) and plays "sing" (a PC) / "msin"
// (a monster); starts the sequence "ch0<colour>", runs S5 entry 5 on every caster and
// queues the element pack load. The charge sequence's op 0x41 (about tick 108, +5 wait) hands over to the pack.
void BattleView::fx_charge_begin() {
  if (act_.ev.empty() || act_.actor < 0) return;
  if (act_.turn_targets_at_charge) {
    act_.turn_targets_at_charge = false;
    face_cast_targets(true);
  }
  // An element with no colour, or with all six, opens no charge file: no group facing, no cast pose (the casters stay in their idle), no chant.
  const int cast_element = act_.ev.front().element;
  const bool known_element = cast_element > 0 && static_cast<size_t>(cast_element) < tables_->elements.size();
  const int cast_colours = known_element ? tables_->elements[static_cast<size_t>(cast_element)].colour_bit() >> 2 : 1;
  const bool charged = cast_colours != 0 && cast_colours != 0x3F;
  if (charged) face_group_cast();
  // The group facing of face_cast_targets follows the charge camera's select, whatever the exit.
  struct GroupFace {
    BattleView& view;
    ~GroupFace() {
      if (!view.face_pending_) return;
      view.face_pending_ = false;
      view.face_point(view.act_.actor, view.face_px_, view.face_pz_);
    }
  } group_face{*this};
  Visual& actor_visual = vis_[static_cast<size_t>(act_.actor)];
  fx_charge_ = true;
  fx_ready_ = false;
  fx_hand_wait_ = 0;
  // Set once the charge instance exists: only then does S5 entry 5 run, whose 'mag<k>' is the monster's own camera.
  model_cam_ = false;
  if (charged && actor_visual.scripts && actor_visual.scripts->has_s5) start_script(act_.actor, &actor_visual.scripts->s5, 5, 0, kCast);
  else actor_visual.script_done = true;
  // S5 entry 5 on EVERY acting-list entry - a group cast's extra casters take the cast pose too.
  for (size_t i = 1; charged && i < act_.casters.size(); ++i) {
    const int cu = act_.casters[i];
    if (cu < 0 || cu >= battle::kUnits || cu == act_.actor) continue;
    Visual& cv = vis_[static_cast<size_t>(cu)];
    if (cv.has_model && cv.scripts && cv.scripts->has_s5) start_script(cu, &cv.scripts->s5, 5, 0, kCast);
  }
  // Chant: mask 0x100, pan 0x80, volume 0x7F; the mask makes the next chant end this one.
  if (const std::string chant = battle_->unit(act_.actor).pc ? "sing" : "msin"; charged && !audio_.play(chant, 127, 64, 0x100)) log::debug("battle audio: no sound '{}'", chant);
  // The theme fades to 0x66 over 0x6D frames while the spell runs.
  audio_.duck_cast();
  if (!dbg_.effects || fx_) return;
  const battle::ActionEvent& e0 = act_.ev.front();
  if (e0.element <= 0 || static_cast<size_t>(e0.element) >= tables_->elements.size()) return;
  if (!fxlib_ready_) fxlib_ready_ = fxlib_.init(*vfs_);
  if (!fxlib_ready_) return;
  const int colour = data::colour_index_of_bit(tables_->elements[static_cast<size_t>(e0.element)].colour_bit());
  std::shared_ptr<const FxPack> pack = fxlib_.charge_pack(colour);
  if (!pack || !fx_gpu_for(pack)) {
    log::debug("battle fx: no charge pack for colour {}", colour);
    return;
  }
  fx_pack_ = pack;
  model_cam_ = true;
  fx_ = std::make_unique<BattleFxRun>(pack, fxlib_.particle_lib(), std::getenv("OC_FX_SEED") ? static_cast<u32>(std::strtoul(std::getenv("OC_FX_SEED"), nullptr, 10)) : 1u + static_cast<u32>(fx_casts_));
  ++fx_casts_;
  BattleFxRun::Callbacks cb;
  // ch10..ch13 live in battle.prd > sato, i.e. in the stage / battle bank every unit bank falls back to.
  cb.camera = [this](std::string_view name) {
    const bool ok = camera_.select(std::string(name));
    log::debug("battle fx: charge camera '{}' {}", name, ok ? "selected" : "missing");
  };
  cb.sound = [this](std::string_view name, int vol, int pan8) { fx_play_sound(audio_, name, vol, pan8); };
  cb.ready = [this] { fx_ready_ = true; };
  cb.other = [this](const assets::SeqCmd& cmd) { fx_other(cmd); };
  cb.ambient = [this](int r, int g, int b, int ticks) { fx_ambient_set(r, g, b, ticks); };
  cb.stage = [this](int stage) { fx_swap_stage(stage); };
  cb.fog_off = [this] {
    fx_fog_on_ = false;
    fx_unit_fog_.fill(false);
  };
  fx_->set_callbacks(std::move(cb));
  fx_->set_sfx(audio_.sfx());
  fx_->set_lights(&fx_lights_);
  fx_spell_run_begin();
  fx_fired_.assign(act_.ev.size(), false);
  fx_show_living();
  fx_lit_.fill(false);
  // The charge instance is an effect context of its own.
  fx_hook_start();
  // The sequence's camera pool picks one of ch10..ch13.
  fx_->start(fx_scene(), fx_view());
  log::debug("battle fx: charge {} (colour {}) for element {}, caster fx_scale {} / {} height {}", pack->start_seq, colour, e0.element,
             fx_scene().unit[static_cast<size_t>(act_.actor)].fx_scale, fx_scene().unit[static_cast<size_t>(act_.actor)].fx_scale_h,
             fx_scene().unit[static_cast<size_t>(act_.actor)].height);
  if (const auto& visual = vis_[static_cast<size_t>(act_.actor)]; visual.has_model && visual.model)
    log::debug("battle fx: caster model S1 extent {} {} {}", visual.model->asset.extent[0], visual.model->asset.extent[1], visual.model->asset.extent[2]);
}

bool BattleView::fx_charge_done() const {
  if (!fx_charge_) return true;
  if (fx_) return (fx_ready_ && fx_hand_wait_ > fx_hand_delay()) || fx_->done();
  // No charge pack: the cast script's own 28 ticks.
  return vis_[static_cast<size_t>(std::max(0, act_.actor))].script_done;
}

// The pack is loaded and the charge released it: the charge instance is freed, the SFX volume restored, the far colour
// (0x80 grey) and the fog range (0x4000..0x5000) reset, then the overlay's init starts 'main' / 'own'.
// The pass delta a task born in a pack command gets on top of its normal ticks: a new task runs at
// once with the pass delta. The hand-over pass has delta 4 (its first 4 pack ticks), the pack's 30 Hz passes 2. Colour ramps (op 0x58) and the
// depth-cue task (0x2D, 2 steps) show it; sequences / emitters do not. The charge's own passes (delta 3) are not credited.
// OC_FX_DT is a dev harness only; the product runs with dt 1 = no hand-over delay, no birth credit, no pass quantisation.
bool BattleView::fx_dt_emulation() {
  static const bool on = [] { const char* env = std::getenv("OC_FX_DT"); return env && *env; }();
  return on;
}

int BattleView::fx_hand_delay() const { return fx_dt_emulation() ? kHandOverDelay : 0; }

int BattleView::fx_task_credit() const {
  if (!fx_ || fx_charge_ || !fx_dt_emulation()) return 0;
  return fx_->ticks() < 4 ? 4 : 2;
}

void BattleView::fx_charge_end() {
  if (fx_charge_ && fx_ && fx_pack_ && !fx_pack_->start_seq.empty()) {
    // The charge camera's task dies, the pose stays.
    camera_.kill_shot();
    fx_.reset();
    fx_pack_.reset();
    fx_hook_end();
  }
  fx_charge_ = false;
  fx_ready_ = false;
  fx_hand_wait_ = 0;
  fx_fog_reset();
}

// ---- fog (GTE depth cue) -----------------------------------------------------------------------------------------------------------
void BattleView::fx_fog_reset() {
  fog_cur_ = Fog{};
  fog_raw_ = Fog{};
  fog_left_ = fog_total_ = 0;
}

// The op 0x2D task: every frame value = start + (target - start) * elapsed / frames, applied as the fog near / far and far colour.
void BattleView::fx_fog_tick() {
  if (fog_left_ <= 0) return;
  --fog_left_;
  const float blend = fog_total_ > 0 ? 1.0f - static_cast<float>(fog_left_) / static_cast<float>(fog_total_) : 1.0f;
  fog_raw_.r = lerp(fog_from_.r, fog_to_.r, blend);
  fog_raw_.g = lerp(fog_from_.g, fog_to_.g, blend);
  fog_raw_.b = lerp(fog_from_.b, fog_to_.b, blend);
  fog_raw_.near = lerp(fog_from_.near, fog_to_.near, blend);
  fog_raw_.far = lerp(fog_from_.far, fog_to_.far, blend);
  // The far colour has no guard.
  fog_cur_.r = fog_raw_.r;
  fog_cur_.g = fog_raw_.g;
  fog_cur_.b = fog_raw_.b;
  // The fog range is only applied when far - near > 99 (as libgte's SetFogNearFar); otherwise nothing changes.
  if (fog_raw_.far - fog_raw_.near >= 100.0f) {
    fog_cur_.near = fog_raw_.near;
    fog_cur_.far = fog_raw_.far;
  }
}

// The depth cue over what the mesh pipeline does not draw: the backdrop (a flat black plane in the PS1 scene turns into p * far colour) and
// the stage (one multiply by mix(1, far / 128, p) at the camera target's depth; the PS1 fogs the stage per vertex).
// p(z) = far * (z - near) / ((far - near) * z) (the GTE fog interpolation, clamped to 0..1).
void BattleView::fx_draw_fog(const render::Frame& frame, const render::Letterbox& lb, bool before_stage, bool resume) {
  if (before_stage) fog_pass_ = false;
  // The depth cue shows only while switched on (op 0x24 .. 0x6C).
  if (!fx_fog_on_) return;
  // The GTE depth cue only touches drawn primitives: the clear colour (black) is never fogged, so the uncovered backdrop stays black.
  // The stage meshes carry their own per-packet cue (StageScene::set_depth_cue).
  if (before_stage) return;
  if (fog_cur_.far <= fog_cur_.near + 1.0f || !white_.set) return;
  auto p_at = [&](float z) {
    z = std::max(z, 1.0f);
    return std::clamp(fog_cur_.far * (z - fog_cur_.near) / ((fog_cur_.far - fog_cur_.near) * z), 0.0f, 1.0f);
  };
  // The 320 x 224 battle buffer (widescreen: its visible columns).
  const float lw = lb.width(), lh = kBufferRows * lb.scale_y;
  u32 color = 0;
  if (before_stage) {
    // The backdrop plane's depth.
    const float density = p_at(12000.0f);
    if (density < 0.004f) return;
    color = render::pack_rgba(static_cast<u8>(std::clamp(fog_cur_.r, 0.0f, 255.0f)), static_cast<u8>(std::clamp(fog_cur_.g, 0.0f, 255.0f)),
                              static_cast<u8>(std::clamp(fog_cur_.b, 0.0f, 255.0f)), static_cast<u8>(255.0f * density));
  } else {
    const CamPose cp = camera_.pose(alpha_);
    const float z = dot(cam_.fwd, {cp.target[0] - cam_.eye[0], cp.target[1] - cam_.eye[1], cp.target[2] - cam_.eye[2]});
    const float density = p_at(z);
    if (density < 0.004f) return;
    auto ch = [&](float channel) { return static_cast<u8>(255.0f * std::clamp(lerp(1.0f, channel / 128.0f, density), 0.0f, 1.0f)); };
    color = render::pack_rgba(ch(fog_cur_.r), ch(fog_cur_.g), ch(fog_cur_.b), 255);
  }
  sprites_->begin(frame, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height), resume || fog_pass_);
  sprites_->set_blend(before_stage ? render::BlendMode::Alpha : render::BlendMode::Multiply);
  sprites_->draw(white_.set, lb.left(), lb.oy, lw, lh, 0, 0, 1, 1, color);
  sprites_->set_blend(render::BlendMode::Alpha);
  sprites_->end();
  fog_pass_ = true;
}

namespace {

// Does the pack's host script (entry 0, variants 0 = the caster's run and 7 = a target's reaction) ever reach CallDamage (op 0x41) or a target reaction
// (0x59 / 0x5a), for a landed or a failed hit? The damage numbers of a host-script pack come from those ops only; a script without them (Boxer Boys'
// "presents": the chest walks to the PC, opens, and no number shows at all) lands nothing, so the end-of-pack release must not pop one either.
bool host_script_lands_hits(const bseq::Table& table) {
  for (const int variant : {0, 7})
    for (const int hit : {0, 2}) {
      bseq::Vm vm;
      if (!vm.start(&table, 0, variant)) continue;
      bseq::Ctx ctx;
      ctx.clip_frames = [](int) { return 8; };
      ctx.hit_code = hit;
      std::vector<bseq::Event> ev;
      for (int tick = 0; tick < 2000 && vm.running(); ++tick) {
        ev.clear();
        vm.tick(ctx, ev);
        for (const bseq::Event& event : ev)
          if (event.kind == bseq::Event::Damage || event.kind == bseq::Event::React) return true;
      }
    }
  return false;
}

}  // namespace

bool BattleView::fx_begin() {
  if (!dbg_.effects || act_.ev.empty() || fx_) return false;
  const battle::ActionEvent& e0 = act_.ev.front();
  if (e0.kind != battle::ActionEvent::Kind::Element) return false;
  if (e0.element <= 0 || static_cast<size_t>(e0.element) >= tables_->elements.size()) return false;
  const data::ElemRec& rec = tables_->elements[static_cast<size_t>(e0.element)];
  if (!fxlib_ready_) fxlib_ready_ = fxlib_.init(*vfs_);
  if (!fxlib_ready_) return false;
  int eid = e0.caught ? BattleFxLibrary::caught_eid(rec) : e0.fx_eid >= 0 ? e0.fx_eid : BattleFxLibrary::element_eid(rec);
  // A MONSTER that casts a trap element runs montrap.prd (eid 397: `main` = camera
  // `cam` + ENEMY_B_FN + per-target `tgef`, emitters cr1 / clr1 / iz32 ...), not the party's etrap (396) / sumtrap (395) pack (`camr`, `atk5`, pt / pt1).
  // The eid is chosen by the monster's battle script, not the element table; the three trap eids are the whole set.
  if ((eid == 395 || eid == 396) && !battle_->unit(std::max(0, act_.actor)).pc) eid = 397;
  if (eid <= 1) return false;
  std::shared_ptr<const FxPack> pack = fxlib_.get(eid);
  if (!pack || !fx_gpu_for(pack)) return false;
  fx_state_log("pack start");
  // The effect start switches the depth cue off.
  fx_fog_on_ = false;
  fx_unit_fog_.fill(false);

  Visual& actor_visual = vis_[static_cast<size_t>(act_.actor)];
  fx_pack_ = pack;
  fx_ = std::make_unique<BattleFxRun>(pack, fxlib_.particle_lib(), std::getenv("OC_FX_SEED") ? static_cast<u32>(std::strtoul(std::getenv("OC_FX_SEED"), nullptr, 10)) : 1u + static_cast<u32>(fx_casts_));
  ++fx_casts_;
  BattleFxRun::Callbacks cb;
  cb.camera = [this](std::string_view name) {
    const bool ok = camera_.select(std::string(name));
    // 0 unless OC_FX_DT: the new camera task runs at once with the pass delta.
    if (ok) camera_.credit_ticks(fx_task_credit());
    log::debug("battle fx: camera shot '{}' {} at tick {}", name, ok ? "selected" : "missing", tick_count_);
  };
  cb.sound = [this](std::string_view name, int vol, int pan8) { fx_play_sound(audio_, name, vol, pan8); };
  cb.damage = [this](int slot) { fx_damage(slot); };
  cb.other = [this](const assets::SeqCmd& cmd) { fx_other(cmd); };
  cb.actor_busy = [this] { return fx_actor_busy(); };
  cb.ambient = [this](int r, int g, int b, int ticks) { fx_ambient_set(r, g, b, ticks); };
  cb.stage = [this](int stage) { fx_swap_stage(stage); };
  // The swapped-in stage's first sound entry, played as the engine plays a pointer-played sound file: mask 0x100, pan 0x80, volume 0x7F.
  cb.stage_sound = [this] {
    if (!fx_stage_sound_.empty()) audio_.play(fx_stage_sound_, 0x7F, 0x80 / 2, 0x100);
  };
  cb.stage_shot = [this](char tag, u32 roll) {
    std::vector<std::string> shots;
    for (const std::string& shot : fx_stage_shots_)
      if (!shot.empty() && shot.front() == tag) shots.push_back(shot);
    if (!shots.empty()) camera_.select(shots[roll % shots.size()]);
  };
  cb.form = [this](int ph) { fx_form_phase(ph); };
  cb.pose = [this](int unit, int pose) { fx_set_pose(unit, pose); };
  cb.song = [this](int song) { audio_.play_song(song); };
  cb.fog_off = [this] {
    fx_fog_on_ = false;
    fx_unit_fog_.fill(false);
  };
  cb.lights = [this](const effects::Vec3& pos, effects::LightMatrices& matrices, std::array<float, 3>& back) {
    matrices = effects::light_matrices(fx_lights_, pos, false);
    for (size_t k = 0; k < 3; ++k) back[k] = static_cast<float>(fx_ambient_.cur[k]) / 256.0f;
    return true;
  };
  fx_->set_callbacks(std::move(cb));
  fx_->set_sfx(audio_.sfx());
  fx_->set_lights(&fx_lights_);
  fx_spell_run_begin();

  // Camera shots of the pack, falling back to the actor's bank (zen0, elm0 ...).
  fx_cam_old_ = std::move(fx_cam_);
  fx_cam_ = std::make_unique<CamBank>();
  fx_cam_->add_drp(pack->prd);
  fx_prev_bank_ = actor_visual.scripts ? &actor_visual.scripts->cam : &bank_;
  fx_cam_->set_parent(fx_prev_bank_);
  camera_.set_bank(fx_cam_.get());
  // The pack's first shot ends the camera tasks of the earlier actions (a death cast right after the killing blow: the attack's shot is not
  // resumed when the pack's shorter one ends).
  camera_.retire_tasks();
  fx_sound_key_ = std::format("fx{}", eid);
  // The pack registers at the head of the resource list.
  audio_.load_group(fx_sound_key_, pack->prd, -1, BattleAudio::Kind::Pack);
  audio_.activate(fx_sound_key_);
  // The chant fades out (0x3C frames) when the pack opens.
  audio_.fade_chant();

  fx_fired_.assign(act_.ev.size(), false);
  fx_show_living();
  fx_lit_.fill(false);
  fx_faded_ = false;
  // A pack start ends the warp bracket and the slow motion.
  fx_warp_.on = false;
  fx_slow_n_ = fx_slow_x_ = fx_slow_phase_ = 0;
  // The stage's lens flare (sprite type 21) is gone from the element pack's start
  // until a fresh one returns at the end - the full-screen additive flash of a sun in view is the 10-frame "white wash" of Turn White's last shot.
  if (effects::EmitterSystem* se = stage_scene_.emitters()) log::debug("battle fx: stage lens flare off ({} emitter(s))", se->lens_off());
  fx_armed_at_start_.clear();
  // The pack's own clips replace the casters' set before any script runs (before the main sequence's tick-0 ops: kamedeth's op 0x59 SetAnim 0x80 reads them).
  fx_hang_clip_packs(*pack);
  fx_starting_ = true;
  fx_hook_start();
  fx_->start(fx_scene(), fx_view());
  fx_starting_ = false;
  // The pack shows kHandOverDelay ticks after the charge's op 0x41 but its timeline is the
  // one that started at the op (`main`'s emitters, children and camera sit exactly where an op-41 start puts them): the ticks the charge
  // kept the screen are run here at once, in the order of a host tick.
  if (!fx_->bespoke() && fx_hand_delay() > 0) {
    for (int j = 0; j < fx_hand_delay(); ++j) {
      fx_fog_tick();
      fx_screen_tick();
      CamEvents cam_events;
      camera_.tick(cam_events);
      for (const std::string& tag : cam_events.sounds) play_tag_sound(tag, 127);
      fx_->catch_up(fx_scene(), 1);
    }
    log::debug("battle fx: pack birth credit {} at frame {}", fx_hand_delay(), frame_count_);
  }
  // The stock tech hosts' init tail: when the pack registered a type-0x1A 'main', every acting model starts entry 0 of the pack's own atk
  // table, variant 0 (the caster's run, the pack timeline and the targets' reactions are started from that script). stdmag0 / stdtech1 have
  // no such block. The same block is the init tail of the packs listed in kMainTail (stdtech0, soidash, 3mix, 3voice, cook, ghost1, hip,
  // shine, shock, swordbox, uzumaki, xgiri, sky, jigoku, canon): the Boxer Boys' "presents" (element 379 / 380, `shock`) is a caster script that
  // walks / opens the chest model, and jigoku (207 Dinner Guest) starts its cameras from the same `main` loop.
  static const std::set<std::string_view> kMainTail = {"stdtech0", "soidash", "shock", "3mix", "3voice", "cook", "ghost1", "hip", "shine", "swordbox", "uzumaki", "xgiri", "sky", "jigoku", "canon", "godhand"};
  bool host_main = false;
  if (kMainTail.count(pack->overlay) && pack->has_anim && pack->tables.count("main") && act_.actor >= 0) {
    // The init tail walks the whole acting list: every caster runs the pack's atk entry with its list index as the variant.
    for (size_t i = 0; i < act_.casters.size(); ++i) {
      // Arming only RE-ARMS the model's script and the script's first step runs later in the unit pass. The host init arms before the pack's
      // `main` sequence starts; a t0 op 0x45 / 0x43 re-arms the same script, so the script RUNS ONCE (running it twice would SetAnim twice, the
      // second blend starting from the first's pose and erasing the cross-fade).
      if (std::find(fx_armed_at_start_.begin(), fx_armed_at_start_.end(), act_.casters[i]) != fx_armed_at_start_.end()) {
        if (i == 0) host_main = true;
        log::debug("battle fx: host init: unit {} (list {}) already armed by the pack's t0 op", act_.casters[i], i);
        continue;
      }
      const bool ok = start_script(act_.casters[i], &pack->anim, 0, static_cast<int>(i), kRelease);
      if (i == 0) host_main = ok;
      log::debug("battle fx: host init: unit {} (list {}) runs the pack's main atk entry {}", act_.casters[i], i, ok ? "started" : "missing");
    }
    // Nothing in the script lands a hit: no number (see host_script_lands_hits).
    if (host_main && !host_script_lands_hits(pack->anim)) std::fill(fx_fired_.begin(), fx_fired_.end(), true);
  }
  // The pack's pseudo-actor: ghost1, oomono2 and jigoku set up the type-0x14 `act0` model and
  // run its entry 0; the oomono2 script's `Sequence zmn0` etc. reach the pack through pa_tick (battle_view_pseudo.cpp).
  pa_begin(pack);
  // A bespoke overlay whose init starts the casters' run of the pack's table (entry 0x80 = `main` entry 0: oomono2, ghost2, jakotu ...) leaves
  // the hit to that script: its `React(-1)` (CallDamTargetAll; Big Catch at script tick 192 = the pseudo-actor's `Sequence zmn0`, Shadow Cat's
  // Shadow at 376) runs variant 7 of the same table on every target = Damage (0x41), so the number lands there, not at the pack's start
  // (the generic 4 + 6 k ticks).
  bool overlay_main = false;
  if (const effects::FxModels* ms = fx_->bespoke_models(); ms && pack->has_anim)
    for (const int unit : act_.casters)
      if (unit >= 0 && static_cast<size_t>(unit) < ms->units.size() && ms->units[static_cast<size_t>(unit)].script_entry == 0x80 &&
          ms->units[static_cast<size_t>(unit)].script_table.empty())
        overlay_main = true;
  const bool script_hits = overlay_main && host_script_lands_hits(pack->anim);
  // The init's own requests (the casters' entry 0x80 run, SetAnim) apply in this pass.
  fx_model_requests(true);
  // The same holds for a caster script a SEQUENCE starts (op 0x43 / 0x45, pack table): Bomber's Suicide Bomb (348, bombom.prd, stdmag0)
  // has no damage / reaction op in its sequences; `main` t0 op 0x45 runs the Bomber's entry 0, whose t290 `React(-1)` starts Serge's
  // variant 7 = Damage next to `Sequence bomb`, so the number spawns about 290 ticks into the pack, not at its start.
  if (!pack->has_damage_op && !pack->has_react_op && !host_main && !script_hits && !pack->caster_script_reacts) fx_queue_hits();
  // Dev: log every emitter name the running pack registers.
  if (std::getenv("OC_ANM_TRACE")) {
    std::string names;
    for (const assets::Emitter& em : pack->fx.emitters) names += em.name + " ";
    log::info("fx pack emitters: {}", names);
  }
  log::debug("battle fx: element {} eid {} {} overlay '{}'{} damage-op {}", e0.element, eid, pack->slot.prd, pack->overlay,
             pack->bespoke ? " (bespoke module)" : "", pack->has_damage_op);
  return true;
}

// The generic element hit: CallDamage on each target, one after the other (4 + 6 k ticks from now).
void BattleView::fx_queue_hits() {
  int target_index = 0;
  for (size_t i = 0; i < act_.ev.size(); ++i) {
    if (act_.ev[i].target < 0 || fx_fired_[i]) continue;
    fx_fired_[i] = true;
    pending_.push_back({act_.ev[i].target, 4 + 6 * target_index, act_.ev[i]});
    ++target_index;
  }
}

void BattleView::fx_fire(size_t i, bool number) {
  if (i >= fx_fired_.size() || fx_fired_[i]) return;
  fx_fired_[i] = true;
  const battle::ActionEvent& hit_event = act_.ev[i];
  // Mode 50 (kamei, death casts) shows no number.
  if (hit_event.target < 0 || hit_event.silent) return;
  // Pack damage ops 0x47 / 0x48 only call CallDamage: the number, the HP and a tint flash. The target runs
  // no reaction script (it keeps its pose; the number appears at the burst's end).
  vis_[static_cast<size_t>(hit_event.target)].react_ev = hit_event;
  on_damage(hit_event.target, number);
}

void BattleView::fx_react(int idx, int caller) {
  std::vector<int> units;
  if (idx < 0) units = act_.targets;
  else if (const int target = target_unit(idx); target >= 0) units.push_back(target);
  for (int unit : units) {
    // CallDamTarget / CallDamTargetAll: the model running the host script is never
    // given the variant-7 reaction, so a cast on its own caster (the death casts: Bomber's Suicide Bomb 348, Potpourri 387 - the target IS the dying unit)
    // leaves the caster's script running.
    if (unit == caller) continue;
    Visual& visual = vis_[static_cast<size_t>(unit)];
    for (const battle::ActionEvent& event : act_.ev)
      if (event.target == unit) {
        visual.react_ev = event;
        visual.has_react = true;
        break;
      }
    const bseq::Table* table = fx_pack_ && fx_pack_->has_anim ? &fx_pack_->anim : nullptr;
    // No miss special case: nothing skips a target's reaction on a miss. CallDamTargetAll always runs
    // entry N variant 7 on every target; whether the target flinches is that script's own GetHitType test (ctx.hit_code). Big Catch's broken line
    // only looks like a skip: the pack's variant 7 is Damage + stand-by clip 1, it never plays a flinch on a hit or a miss; the "miss!"
    // number is its Damage op (code 2 -> the miss glyphs in on_damage).
    if (!(table && start_script(unit, table, 0, 7, kReact))) {
      // No script: the hit itself.
      fx_land(unit);
    } else if (fx_pack_->reaction_lands_no_number && visual.react_ev.code == 1) {
      // A "no damage" result (code 1: mode 40's zero roll) whose reaction ends without CallDamage, nothing else in the
      // pack calling it (Toxic Burst 387, jibaku.prd: a `Done`-only variant 7), shows no number. The hit lands
      // here, at the reaction, without it (HP / KO only). Code 0 hits on such packs (Multistab 460: four React onto a `Done` variant 7)
      // keep the normal path.
      for (size_t i = 0; i < act_.ev.size(); ++i)
        if (act_.ev[i].target == unit && !fx_fired_[i]) fx_fire(i, false);
    }
  }
}

bool BattleView::fx_land(int unit) {
  if (!fx_ || fx_fired_.size() != act_.ev.size()) return false;
  bool any = false;
  for (size_t i = 0; i < act_.ev.size(); ++i) {
    if (act_.ev[i].target != unit) continue;
    any = true;
    if (!fx_fired_[i]) {
      fx_fire(i);
      return true;
    }
  }
  // Already landed by op 0x47 / 0x48: the script's CallDamage shows nothing new.
  return any;
}

void BattleView::fx_damage(int slot) {
  if (slot < 0) {
    for (size_t i = 0; i < act_.ev.size(); ++i) fx_fire(i);
    return;
  }
  if (slot >= static_cast<int>(act_.targets.size())) return;
  const int unit = act_.targets[static_cast<size_t>(slot)];
  for (size_t i = 0; i < act_.ev.size(); ++i)
    if (act_.ev[i].target == unit && !fx_fired_[i]) {
      fx_fire(i);
      return;
    }
}

// Sequence ops that start anmseq scripts: 0x43 every caster / 0x45 caster i: entry s16@8 + 0x80 = the pack's external 0x1A
// table (variant = the caster's list index, 0); 0x44 every target / 0x46 target i + loop cursor: the same table, variant 7 (the reaction); 0x4F
// caster i / 0x57 target i: the model's own S5 entry s16@8 (6 = release). Operands: entry at data +4, actor index at data +6.
void BattleView::fx_actor_script(const assets::SeqCmd& cmd) {
  if (cmd.data.size() < 8 || act_.actor < 0) return;
  const int entry = static_cast<s16>(cmd.data[4] | (cmd.data[5] << 8)), idx = static_cast<s16>(cmd.data[6] | (cmd.data[7] << 8));
  const bool pack_table = cmd.op == 0x43 || cmd.op == 0x44 || cmd.op == 0x45 || cmd.op == 0x46;
  const bool on_targets = cmd.op == 0x44 || cmd.op == 0x46 || cmd.op == 0x57;
  const bool all = cmd.op == 0x43 || cmd.op == 0x44;
  std::vector<int> units;
  if (on_targets) {
    if (all) units = act_.targets;
    else {
      const int target_index = idx + (cmd.op == 0x46 && fx_ ? fx_->cursor() : 0);
      if (target_index >= 0 && static_cast<size_t>(target_index) < act_.targets.size()) units.push_back(act_.targets[static_cast<size_t>(target_index)]);
    }
  } else {
    // 0x43 every acting model; 0x45 / 0x4F caster idx of the acting list.
    if (all) units = act_.casters;
    else if (idx >= 0 && static_cast<size_t>(idx) < act_.casters.size()) units.push_back(act_.casters[static_cast<size_t>(idx)]);
  }
  for (int unit : units) {
    Visual& visual = vis_[static_cast<size_t>(unit)];
    const bseq::Table* table = pack_table ? (fx_pack_ && fx_pack_->has_anim ? &fx_pack_->anim : nullptr) : (visual.scripts && visual.scripts->has_s5 ? &visual.scripts->s5 : nullptr);
    if (!table) continue;
    if (on_targets) {
      for (const battle::ActionEvent& hit_event : act_.ev)
        if (hit_event.target == unit) {
          visual.react_ev = hit_event;
          visual.has_react = true;
          break;
        }
    }
    int variant = on_targets && pack_table ? 7 : 0;
    // The caster's variant = its place in the acting list.
    if (!on_targets && pack_table)
      for (size_t k = 0; k < act_.casters.size(); ++k)
        if (act_.casters[k] == unit) variant = static_cast<int>(k);
    const bool ok = start_script(unit, table, entry, variant, on_targets ? kReact : kRelease);
    if (ok && fx_starting_ && pack_table && !on_targets) fx_armed_at_start_.push_back(unit);
    if (ok) {
      // Arming the model's script runs its first step in the same frame (stepping it a frame later, with
      // the next tick_units, would make op 0x50 pass one tick late and every later pack event trail by 1).
      bseq::Ctx ctx;
      fill_ctx(unit, ctx);
      std::vector<bseq::Event> sev;
      visual.vm.tick(ctx, sev);
      for (const bseq::Event& event : sev) on_script_event(unit, event);
      if (!visual.vm.running()) visual.script_done = true;
    }
    log::debug("battle fx: tick {} op {:02X} unit {} {} entry {} variant {} {}", tick_count_, cmd.op, unit, pack_table ? "pack" : "S5", entry, on_targets && pack_table ? 7 : 0,
               ok ? "started" : "missing");
  }
}

// Op 0x50: the sequence stalls while the first caster's anmseq is still running.
bool BattleView::fx_actor_busy() const {
  if (act_.actor < 0) return false;
  const Visual& actor_visual = vis_[static_cast<size_t>(act_.actor)];
  // The caster's own `main` started by a bespoke overlay (fx_model_requests) runs as kReact without a reaction event: the same effect script.
  return actor_visual.vm.running() && !actor_visual.script_done && (actor_visual.script_kind == kRelease || actor_visual.script_kind == kCast || (actor_visual.script_kind == kReact && !actor_visual.has_react));
}

// Visibility: ops set / clear a hide bit of the model: bit 0x1 for 0x30 / 0x31 (everyone), 0x32 / 0x33 (show / hide the caster's side: the
// actor's entry flag 0x8000 equals caster 0's), 0x34 / 0x35 (the other side), 0x5B / 0x5C (targets), 0x5D / 0x5E (casters). A model with
// bit 0 set is not drawn. The same ops' bit 0x100 (0x61 / 0x62 targets, 0x63 / 0x64 casters, 0x68 / 0x69 everyone) is the LIT flag
// (fx_lit_), not a second hide bit.
void BattleView::fx_set_hidden(bool caster_side, bool hidden) {
  const bool cpc = battle_->unit(std::max(0, act_.actor)).pc;
  std::vector<int> us;
  for (int i = 0; i < battle::kUnits; ++i)
    if (battle_->unit(i).present && ((battle_->unit(i).pc == cpc) == caster_side)) us.push_back(i);
  fx_hide(us, 1, hidden);
}

void BattleView::fx_hide(const std::vector<int>& units, u16 bit, bool on) {
  for (int unit : units) {
    if (unit < 0 || unit >= battle::kUnits) continue;
    u16& bits = fx_hide_bits_[static_cast<size_t>(unit)];
    bits = on ? static_cast<u16>(bits | bit) : static_cast<u16>(bits & ~bit);
    fx_hidden_[static_cast<size_t>(unit)] = bits != 0;
    // The stock ops, the model scripts and the overlays write one hide bit, so a show also ends an overlay's hide; a model whose block was given
    // up stays off the stage until its reload.
    if (effects::FxModels* overlay_models = fx_ ? fx_->bespoke_models() : nullptr; !on && bit == 1 && overlay_models && static_cast<size_t>(unit) < overlay_models->units.size() && !overlay_models->units[static_cast<size_t>(unit)].unloaded)
      overlay_models->units[static_cast<size_t>(unit)].hidden = false;
  }
}

// Units by selector: 0 everyone, 1 the targets, 2 the casters.
std::vector<int> BattleView::fx_units(int which) const {
  std::vector<int> us;
  if (which == 1) return act_.targets;
  if (which == 2) {
    if (!act_.casters.empty()) return act_.casters;
    if (act_.actor >= 0) us.push_back(act_.actor);
    return us;
  }
  for (int i = 0; i < battle::kUnits; ++i)
    if (battle_->unit(i).present) us.push_back(i);
  return us;
}
}  // namespace oc::game
