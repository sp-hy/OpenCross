#pragma once
// Element / tech effects in battle, headless part: effect-pack cache and the
// per-cast run. No GPU / audio dependency; BattleView (battle_view_fx*.cpp) owns the glue (texture upload, camera, sound, damage timing).
//
// Convention: the effect scripts address actors by selector:
// 1 / 9 / 0x19 = the CASTER, 2 / 0xD / 0x1A = the TARGET (the one the per-target loop currently serves), 3 / 4 = caster side / target side
// centroid, 5 = everybody, 10-11 = caster-side members 1-2, 0xE-0x15 = target-side members 1-8, 8 / 0xC = the pseudo actors A / B registered by
// emitter ops 3F / 40. "Party" / "enemy" in the PS1 getters are relative to the caster (the camera records use the same 1 = attacker, 2 = first
// target convention), so a monster casting at the party swaps the sides.
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "assets/battle_stage.h"
#include "assets/room_effects.h"
#include "battle/battle.h"
#include "data/tables.h"
#include "game/effects/bespoke/bespoke.h"
#include "game/effects/bespoke/env_mesh.h"
#include "game/effects/effect_slots.h"
#include "game/effects/emitter_system.h"
#include "game/effects/sequence_player.h"
#include "game/battle/battle_seq.h"
#include "import/tim.h"
#include "vfs/vfs.h"

namespace oc::game {

// The battle-resident effect data: battle/battle.prd is loaded at battle init and registered like any drp (every entry, the nested drps
// miya / chih / tana / sato included, joins the name registry; its TIMs go to VRAM at once), so
// the element packs name its sheets (b28, 64ke, smok, kona ...) and meshes (maru, wa0, sank, son1 ...) without holding them.
struct FxResident {
  // The nested drps miya, chih, tana, sato: sheets cut from the battle VRAM; emitters registered stopped.
  std::vector<std::unique_ptr<assets::RoomEffects>> packs;
  // sato's meshes, textured from the battle VRAM.
  std::shared_ptr<assets::StageAsset> meshes;
  // The battle VRAM (every TIM of battle.prd) as egfx ctd sections, and the same VRAM as the base for the packs' sprite sheets.
  Bytes vram_ctd;
  import::Vram vram;
};

// One loaded effect pack (slot files of an effect id).
struct FxPack {
  int eid = 0;
  effects::EffectSlot slot;
  assets::RoomEffects fx;
  // Sheet sprites of `fx` with an HD image (EffectSprite::hd_rgba, RDE's 4x sheet pages).
  size_t hd_sprites = 0;
  // RDE's HD-art texels for part of the pack's texture space: 8 bpp indices, kScale finer than the texels of the rect they cover, whose
  // top left is (x halfwords, y rows). They take the colours of the texture they are copied into (anim ops 0xA2 - 0xA5).
  struct HdTexels {
    static constexpr int kScale = 4;
    int x = 0, y = 0;
    import::Image indices;
  };
  std::optional<HdTexels> hd_texels;
  // Raw eins drp: camera / sound entries are registered from it.
  Bytes prd;
  Bytes ctd;
  // The type-0x02 effect meshes (same container as the stage meshes), null without mesh sprites.
  std::shared_ptr<assets::StageAsset> meshes;
  bool has_main = false;
  // Some sequence starts the targets' reaction script (ops 0x44 / 0x46): its CallDamage lands the hit.
  bool has_react_op = false;
  // Some sequence carries op 0x47 / 0x48 (CallDamage of the battle overlay).
  bool has_damage_op = false;
  // A sequence op 0x43 / 0x45 starts a caster script of the pack table whose run reaches React (anmseq 0x59 / 0x5A = CallDamTarget /
  // CallDamTargetAll): the targets' variant 7 (its Damage op) lands the hit there - no generic hit at the pack's start.
  bool caster_script_reacts = false;
  // The targets' reaction (variant 7) is the only place the hit could land and it ends without CallDamage (the only source
  // of the number): the React lands it without a number (e.g. a `Done`-only variant 7).
  bool reaction_lands_no_number = false;
  // Stem of the .prg.
  std::string overlay;
  const effects::BespokeInfo* bespoke = nullptr;
  // The last type-0x1A entry of the pack ('dama', 'main', 'care'...): assigned to every caster
  // and every target as their external anmseq block, so sequence ops 0x43-0x46 can start its entries.
  bseq::Table anim;
  bool has_anim = false;
  // Every type-0x1A entry by tag ('dead', 'dama' ...): overlays assign one by name.
  std::map<std::string, bseq::Table> tables;
  // Every type-0x19 entry (name, cpt of animation clips with the acting model's joint count) in registration order: 'tech' / 'tec1'..'tec5'
  // hang on the caster of that acting-list index, any other name on every caster and target (this replaces the model's external clip set,
  // so the pack's 'main' script plays them as SetAnim 0x80 + k).
  std::vector<std::pair<std::string, Bytes>> clip_packs;
  // Sequence started instead of 'main' / 'own' (the charge packs: "ch0N").
  std::string start_seq;
  // Additional pack searched after this one (unused: battle.prd is `resident`).
  std::shared_ptr<const assets::RoomEffects> extra;
  // battle.prd: searched after the pack (sheets, meshes, sequences, emitters).
  std::shared_ptr<const FxResident> resident;
  // Mesh part `name` (sprite type 14): the pack's own meshes first, then the resident ones. Null if neither holds it.
  const assets::StageAsset* mesh_asset(std::string_view name, size_t& part) const;
  // VRAM while the pack plays: the resident battle.prd TIMs, the egfx sections, the eins TIMs (texture of the sprite type 20 models).
  import::Vram build_vram() const;
};

// Tags of every effect sequence the units' anmseq scripts can start (ops 0x68 / 0x7D): the S5 tables of the battle models' .obj and the
// type-0x1A tables of their at0.prd, their S6 drps and battle.prd. Reads all of them (tools and tests).
std::set<u32> actor_script_tags(const vfs::Vfs& vfs);

// The battle registers each PC's equipped weapon mesh under the PC's two-digit character number, so a type-14 mesh sprite of that name draws
// it: the weapon techs' packs ("02" Guile's wand) and the units' own at0.prd emitters (a thrown boomerang: Mel's "34", Van's "36"). The
// character id a sprite name denotes, -1 for any other name.
int weapon_mesh_character(std::string_view name);

class BattleFxLibrary {
 public:
  // slot table, particle library, bespoke bindings. False if the slot table is missing. `bind_globals` false (the texture dump worker thread, a second
  // library next to the game's own): do not touch the process-wide bespoke slot registry / PS1 exe.
  bool init(const vfs::Vfs& vfs, bool bind_globals = true);
  void clear_cache() { cache_.clear(); }
  // The pack of an effect id; null for ids without files (irregular slots, dummies) or packs that failed to parse. Cached.
  std::shared_ptr<const FxPack> get(int eid);
  // The cast "charge" pack of an element colour (0 green .. 5 blue, data::colour_index_of_bit): battle/battle.prd > nested 'sato' > the LZSS
  // type-0x25 entry "ch0<colour>" (a drp of ~15 emitters + the sequence "ch0<colour>" that picks one of the cameras ch10..ch13).
  // It is instantiated when a cast starts; its op 0x41 releases the element pack. Null without
  // battle.prd or for colour < 0.
  std::shared_ptr<const FxPack> charge_pack(int colour);
  static int element_eid(const data::ElemRec& elem) { return elem.raw[0x10] | (elem.raw[0x11] << 8); }
  // The pack a cast caught in an element trap plays instead of its own: 197 + colour for an element, 203 + colour for a summon (record
  // byte 0 bit 0x40), colour 0..5 as the charge's; 0 for a record without a single colour.
  static int caught_eid(const data::ElemRec& elem) {
    const int colour = data::colour_index_of_bit(elem.colour_bit() & 0xFC);
    if (colour < 0) return 0;
    return ((elem.flags0() & 0x40) ? 203 : 197) + colour;
  }
  const assets::RoomEffects& particle_lib() const { return lib_; }
  size_t slot_count() const { return slots_.size(); }
  const std::vector<effects::EffectSlot>& slots() const { return slots_; }
  const effects::EffectSlot* slot(int eid) const;
  std::shared_ptr<const FxResident> resident() const { return resident_; }
  // The summon motion streams: group-11 local index 0x2D + n of cdrom.fnd (n = 0..2: st_01.mtc,
  // st_03.mtc, st_02.mtc), the files the saraman overlay loads for its 'sara' model (effects::FxModels::PackDrive). "" when unknown.
  const std::string& summon_motion_file(int index) const { return index >= 0 && index < 3 ? motion_files_[static_cast<size_t>(index)] : empty_; }

 private:
  void load_resident();
  const vfs::Vfs* vfs_ = nullptr;
  std::shared_ptr<FxResident> resident_;
  std::vector<effects::EffectSlot> slots_;
  Bytes lib_bytes_;
  assets::RoomEffects lib_;
  std::map<int, std::shared_ptr<FxPack>> cache_;
  Bytes battle_prd_;
  // The nested 'sato' drp (sheets, meshes, ch0N packs, ch1N cameras).
  Bytes sato_;
  std::array<std::shared_ptr<FxPack>, 6> charge_{};
  bool charge_scanned_ = false;
  std::array<std::string, 3> motion_files_{};
  std::string empty_;
};

// The battle objects an effect refers to. World y up (the stage convention), yaw = the model matrix yaw (forward = local -Z).
struct FxUnit {
  bool present = false, pc = false;
  float x = 0, z = 0, yaw = 0, height = 1000;
  // Height above the floor (y up): a unit the anmseq scripts lifted (JumpT, PosMoveT y); the attach points ride with it.
  float y = 0;
  // The actor scale the emitter op 0x39 applies to the scale ops (flag bit 1) and to the basis matrix (bit 2), and the overlays' size
  // field. 4096 = 1.0.
  int fx_scale = 4096, fx_scale_h = 4096;
  // Attach point `point` of the posed model (model units, y down; the model's yaw not applied), for the effect joints >= 2.
  // Unset or false = the 55 % height stand-in.
  std::function<bool(int point, float& x, float& y, float& z)> attach;
  // Joint `index` of the posed model (the joint's world MATRIX translation; model units, y down, the model's own
  // placement not applied). Unset = unavailable.
  std::function<bool(int index, float& x, float& y, float& z)> joint;
  // Rotation (row-major 3 x 3, model space, y down) of the joint attach point `point` names, in the current pose: the joint matrix multiplied
  // into an object with flag bit 7; the model's yaw not applied. Unset / false = none.
  std::function<bool(int point, float (&rot)[9])> attach_rot;
  // The pseudo-actor's SetRot / RotateT z angle (radians, PS1 axes). The model matrix is RotMatrix(x, yaw, z) = Rz * Ry * Rx,
  // so z turns the WORLD frame after the yaw: attach / attach_rot carry Rx * S only, the consumers put Rz in front of the yaw.
  float rot_z = 0;
  // The result of the action on the unit: 0 hit, 1 no damage, 2 miss, 3 heal.
  int hit = 0;
  bool dead = false;
  // A script ghost: a cast target the pack may bring on stage (FxModel::reveal).
  bool offstage = false;
};
// The pack's pseudo-actor (the type-0x14 `act0` model an overlay registers as slot A) stands at this index of unit_at():
// effect selector 8 names it.
constexpr int kPseudoSlot = battle::kUnits;
struct FxScene {
  std::array<FxUnit, battle::kUnits> unit{};
  // present = the pack registered an `act0` model (BattleView::PseudoActor).
  FxUnit pseudo;
  // Acting list entry 0.
  int caster = 0;
  // The whole acting list (e.g. a transformation cast = [4, 3]); empty = {caster}.
  std::vector<int> casters;
  // Unit indices in cast order.
  std::vector<int> targets;
};

struct FxFrame {
  // Sorted.
  std::vector<effects::DrawQuad> quads;
  // Bespoke screen primitives, flattened (blend 5 = invert is dropped).
  std::vector<effects::FlatQuad> flats;
  // Bespoke textured primitives (tex >= 0 = the pack's mesh textures), sorted.
  std::vector<effects::FxPrim> textured;
};

class BattleFxRun {
 public:
  struct Callbacks {
    // op 0x23: shot `name` of the pack.
    std::function<void(std::string_view name)> camera;
    // op 0x4C: AKAO tag looked up in the resource list; volume 0..127, pan8 0x80 = centre.
    std::function<void(std::string_view name, int volume, int pan8)> sound;
    // op 0x47 (index into scene.targets) / 0x48 (-1 = all targets).
    std::function<void(int target_slot)> damage;
    // Every other stock op (visibility, flash, fades, brightness ...).
    std::function<void(const assets::SeqCmd&)> other;
    std::function<void(int ax, int ay, int ticks)> shake;
    // op 0x41: the charge pack hands over to the element pack.
    std::function<void()> ready;
    // op 0x50: stall while the caster's anmseq script is still running.
    std::function<bool()> actor_busy;
    // Op 0x65: ramp the GTE back colour (SetBackColor, the ambient of the lit models) to (r, g, b) over `ticks`; also a
    // pack's type-0x24 entry at the start (set at once, ticks 0).
    std::function<void(int r, int g, int b, int ticks)> ambient;
    // An overlay replaces the battle stage (kamei ops 0x101 / 0x102: the stage pack's objects freed, then the new stage's resources registered):
    // -1 = free the stage's objects (nothing drawn), 0..99 = battle/bg/bg_NN, 100 = battle/bg/kamei.
    std::function<void(int stage)> stage;
    // effects::FxHooks::stage_sound / stage_shot.
    std::function<void()> stage_sound;
    std::function<void(char tag, u32 roll)> stage_shot;
    // An overlay clears the depth-cue flag.
    std::function<void()> fog_off;
    // An overlay's lit draw: the light matrices at a world point and the back colour (effects::FxHooks::lights).
    std::function<bool(const effects::Vec3& pos, effects::LightMatrices& matrices, std::array<float, 3>& back)> lights;
    // hensin ops 0x182 / 0x183: phase 1 = the form change starts (the old model goes), 2 = the unit takes its new form.
    std::function<void(int phase)> form;
    // An overlay writes a unit's idle-pose word (effects::FxHooks::set_pose).
    std::function<void(int unit, int pose)> pose;
    // An overlay changes the music (effects::FxHooks::play_song).
    std::function<void(int song)> song;
  };
  BattleFxRun(std::shared_ptr<const FxPack> pack, const assets::RoomEffects& particle_lib, u32 seed);
  ~BattleFxRun();
  BattleFxRun(const BattleFxRun&) = delete;
  BattleFxRun& operator=(const BattleFxRun&) = delete;

  void set_callbacks(Callbacks cb) { cb_ = std::move(cb); }
  void set_sfx(audio::Sfx* sfx);
  // The GTE light slots the pack's type-17 light objects write; owned by the view (they outlive the pack).
  void set_lights(effects::LightTable* table);
  // Registers the emitters and starts the 'main' (or 'own') sequence; `view` = the camera at this moment (used by the screen-space modules).
  void start(const FxScene& scene, const effects::ViewParams& view);
  // anmseq ops 0x68 / 0x7D: start the pack's sequence `tag` (the scripts drive the packs without
  // 'main'). False if neither the pack nor the resident library holds it (the caller then plays its camera timeline).
  bool start_sequence(std::string_view tag);
  // Tools / tests without actor scripts: starts the root sequences that 'main' / 'own' does not reach (none starts it with op 0x2F), as
  // the anmseq scripts would over the action. A pack with 'main' and no caster scripts or clips of its own (types 0x1A / 0x19) gets only
  // those whose tag a unit's script names (`actor_tags`, actor_script_tags): its other root sequences never run. Returns the number started.
  int start_script_sequences(const std::set<u32>& actor_tags);
  // One 60 Hz tick.
  void tick(const FxScene& scene);
  // The birth credit of the pack's `main`: sequence tasks + emitters only, `ticks` ticks (no overlay call).
  void catch_up(const FxScene& scene, int ticks);
  // The overlay pacing: the PS1 loop iteration length dt in vsyncs, as "N" or a schedule "t:N,t:N" (pack
  // tick t on). 1 = every 60 Hz tick is a call (the default); dt >= 2: an overlay's per-call emitter fires fire once per dt
  // ticks (EmitterSystem::begin_overlay_tick). Env OC_FX_DT feeds it (dev tools).
  void set_pace(std::string_view spec);
  // A constant dt the environment does not override (the charge pack's passes are 3 vsyncs).
  void set_fixed_pace(int dt) {
    pace_.clear();
    pace_.emplace_back(0, dt);
    pace_fixed_ = true;
  }
  int pace_dt() const {
    int dt = 1;
    for (const auto& entry : pace_)
      if (entry.first <= ticks_) dt = entry.second;
    return dt;
  }
  bool done() const;
  int ticks() const { return ticks_; }
  void collect(const effects::ViewParams& view, float alpha, FxFrame& out) const;
  effects::EnvMeshes& env_meshes() { return env_meshes_; }
  void collect_meshes(const effects::ViewParams& view, float alpha, std::vector<effects::EmitterSystem::MeshInstance>& out) const {
    em_->collect_meshes(view, alpha, out);
  }

  const FxPack& pack() const { return *pack_; }
  const effects::EmitterSystem& emitters() const { return *em_; }
  // The particle the pack registered as pseudo-actor slot A (0, op 0x3F) / B (1, op 0x40): position and velocity (PS1 world, y down).
  bool slot_object(int slot, effects::Vec3& pos, effects::Vec3& vel) const { return em_ && em_->slot_object(slot, pos, vel); }
  bool slot_model_yaw(int slot, int& yaw) const { return em_ && em_->slot_model_yaw(slot, yaw); }
  const effects::SequenceStats& sequence_stats() const { return seq_->stats(); }
  const effects::SequencePlayer& sequences() const { return *seq_; }
  // done() only because of the hard cap.
  bool capped() const { return ticks_ > kTickCap; }
  const effects::BespokeEffect* bespoke() const { return bespoke_.get(); }
  // Overlay-private ops (>= 0x100) met, and those no bespoke module acted on (none running, or the module ignored the op), per op.
  u32 private_ops() const { return private_ops_; }
  u32 private_ops_unhandled() const {
    u32 total = 0;
    for (const auto& entry : private_ignored_) total += entry.second;
    return total;
  }
  const std::map<int, u32>& private_ops_ignored() const { return private_ignored_; }
  // Sequence ops the battle presentation carries out (BattleFxRun itself, SequencePlayer, BattleView::fx_other); the audit counts every other
  // stock op it meets as "not presented".
  enum class OpClass { Presented, Missing, NonVisual, Private };
  static OpClass op_class(int op, int len);
  static bool presented(int op, int len) { return op_class(op, len) == OpClass::Presented; }
  // A safety net only: above the longest shipped chain of sequences (lasdes2: 'main' 3995 ticks, its child 'lost' from 3230, that one's 'kid!'
  // from 4089 to 5605; lasdes3 'main' 2240).
  static constexpr int kTickCap = 6144;
  size_t sequences_active() const { return seq_->active(); }
  int shake_x() const;
  int shake_y() const;
  bool damage_pending() const { return !damage_seen_; }
  // Target the 0x4D per-target loop serves (ops 0x46 / 0x47 add it to their index).
  int cursor() const { return cursor_; }
  bool ready_seen() const { return ready_seen_; }
  // The battle models a bespoke overlay steers (position, rotation, scale, colour, visibility, clip / script requests).
  // Null without a bespoke module. The host draws steered models from it and consumes the requests after each tick.
  effects::FxModels* bespoke_models() { return bespoke_ ? &models_ : nullptr; }
  const effects::FxModels* bespoke_models() const { return bespoke_ ? &models_ : nullptr; }

 private:
  bool actor_basis(int sel, int joint, int second_sel, int second_joint, effects::Basis& out, int& scale) const;
  bool point_of(int sel, int joint, effects::Vec3& pos, float& yaw) const;
  const FxUnit& unit_at(int unit) const;
  // FxModels caster / target overrides (an overlay replaced the acting / target list) -> scene_.
  void apply_list_overrides();
  void on_other(const assets::SeqCmd& original);
  effects::FxContext make_context(const effects::ViewParams& view) const;
  void init_models();
  // The models no overlay steers take the units' live position.
  void sync_unsteered();

  std::shared_ptr<const FxPack> pack_;
  effects::EffectBank bank_;
  std::unique_ptr<effects::EmitterSystem> em_;
  std::unique_ptr<effects::SequencePlayer> seq_;
  std::unique_ptr<effects::BespokeEffect> bespoke_;
  // Behind bespoke_models().
  effects::FxModels models_;
  effects::EnvMeshes env_meshes_;
  Callbacks cb_;
  FxScene scene_;
  // Target the per-target loop serves.
  int cursor_ = 0;
  int ticks_ = 0, idle_ = 0;
  // (pack tick, dt) schedule, ascending.
  std::vector<std::pair<int, int>> pace_;
  bool pace_fixed_ = false;
  // Ticks left in the current pacing window.
  int pace_left_ = 0;
  u32 private_ops_ = 0;
  std::map<int, u32> private_ignored_;
  // Model copies of ops 0x6F / 0x71 / 0x77.
  std::array<std::optional<FxUnit>, battle::kUnits> frozen_{};
  bool damage_seen_ = false, ready_seen_ = false;
  mutable int shake_x_ = 0, shake_y_ = 0;
};

}  // namespace oc::game
