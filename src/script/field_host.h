#pragma once
// FieldHost: everything the field script VM needs from the rest of the engine.
//
// The VM (field_vm.h) owns script state only: variables, per-entity script slot records, call stacks, entity flag
// words and the plain per-entity values scripts write (Entity in field_vm.h). Anything that touches the world goes
// through this interface, one method per opcode family. Conventions:
//   * `ent` is an entity index of the current room (0..entity_count-1), already resolved from the script's
//     character reference; -1 means "none" and is only passed where documented.
//   * Methods named `*_busy`, `*_active`, `*_playing`, `*_open` are the polls behind the "wait forms" and blocking
//     ops: while they return true the script yields without advancing; the NullHost answers false ("done").
//   * Coordinates are the script's integer units (the high 16 bits of the original 16.16 fixed point); angles are
//     12 bit (4096 = full turn).
//   * Methods are called from inside FieldVM::tick() / load(); a host may call back into FieldVM::set_var(), but
//     must not call tick() or load() from here.
//   * Op references (e.g. "3F") are field script opcodes.
#include <set>
#include <span>
#include <string>
#include <string_view>

#include "core/types.h"

namespace oc::script {

class FieldVM;

struct Vec3 {
  s32 x = 0, y = 0, z = 0;
};

// Walk / move families (start forms; the wait form polls motion_busy()).
enum class MotionKind : u8 {
  // 3F  walk to (x, y, z); mode 0 absolute, 2 relative
  Walk,
  // 40  as Walk plus params[0] (speed/time)
  Walk2,
  // 42  walk to (x, z); params[0]
  WalkXZ,
  // 43  walk to (x, z); params[0] = frames, params[1]
  WalkXZ2,
  // 44  walk towards entity `target` (2D)
  FollowEnt,
  // B8  same, 3D
  FollowEnt3D,
  // 45  as FollowEnt with timeout params[0]
  FollowEntTimed,
  // B9  3D timed follow
  FollowEnt3DTimed,
  // 46  move in direction params[0] (angle) for params[1] frames
  Move,
  // BA  heading params[0], pitch params[1], frames params[2]
  Move3D,
  // 5B  linear tween to (x, y, z) in params[0] frames (mode 0..2 selects how the duration is derived)
  Tween,
  // 5C  jump arc to (x, y, z), params[0] = frames/height
  Jump,
};

struct MotionRequest {
  MotionKind kind = MotionKind::Walk;
  // Operand byte b of the start form.
  u8 mode = 0;
  s32 x = 0, y = 0, z = 0;
  // Entity for the Follow kinds.
  int target = -1;
  s32 params[3] = {0, 0, 0};
};

// The screens the field's launcher opens; the op that requests one is followed by an FE 61 that waits until it has closed.
enum class MenuKind : u8 {
  // FE 5B  character exchange
  PartyScreen,
  // FE 72 (save points)
  MainMenu,
  // FE 5E (arg = shop id)
  ElementShop,
  // FE 68 (arg = shop id)
  WeaponShop,
  // FE 4A (arg = character)
  NameEntry,
  // FE 65
  ChangeDisc,
  // FE 67: sets the trading post row and launcher mode 0x21
  TradingPost,
  // FE 85: the ending's cleared-save prompt (launcher mode 4, save list only)
  Save,
  // FE 84: the New Game+ title list (launcher mode 0x30)
  NewGamePlus,
  // FE 75 (arg = the Grand Slam round 0..2): the monster picker
  MonsterPicker,
};

// FE 38 / FE 39 / FE 3A
enum class LifeOp : u8 { SetClamped, AddCapped, Damage };

// Ops that take a name string: 9B attaches object `name` to an entity; the others are 9C, E7, B6, D3 (PlayEffect), D7 (StopEffect).
enum class NamedOp : u8 {
  AttachToEntity5,
  ModeZero,
  ModeOne,
  Activate,
  PlayEffect,
  StopEffect,
};

// Predicates the original evaluates with unnamed engine routines; true means "jump" for the
// jump ops that use them (95, E5, E6, E8) and "fall through" where noted.
enum class Predicate : u8 {
  Op95,
  OpE5,
  // E6: not recruited
  OpE6,
  OpE8,
  // D0  true: fall through (pc += 3), false: jump
  BoxOutside,
  // D2  true: advance by 4 (skips a 3-byte instruction), false: by 1
  GameStateLt2,
};

// Head-turn family (FE 30 31 32 35 49 53 54 55 56 57). Every form but Reset stores a world target in the model and starts the
// head turn at the entity's turn rate; Reset releases it.
enum class HeadOp : u8 {
  // FE 30 (self) / FE 54: target = (x, y, z)
  Point,
  // FE 31 (self looks at `other`) / FE 55: target = attach point 1 of `other`
  LookAtHead,
  // FE 35 / FE 49 (self), FE 53 / FE 57: target = (rcos(a), y, -rsin(a)) with a = facing + x + 0x400 (x = yaw offset)
  Facing,
  // FE 32 (self) / FE 56
  Reset,
};

// Reads of engine values written to a script variable: FE 28, FE 6E (current song), FE 76, FE 82 (total stars), 65. FE 28 and 65 have no engine source yet.
enum class Query : u8 {
  GlobalDiff,
  CurrentSong,
  Field56,
  StarsTotal,
  ScriptGlobal,
};

// Engine globals that script ops write and the VM does not model itself (63 64 A4 A6 B5, FE 02 05 12 13 15 29 2B 34 4D 59 63 67
// 6A 70 72 75 77 7A). The VM keeps every value (E9, FE 29 and FE 7A read theirs back); the host acts on the ones it models and
// logs the others as stubs.
enum class Global : u8 {
  WindowFrames,        // E9 / FE 7A: owned dialogue-window skins, one bit per frame; lives in the save slot
  ControlYaw,          // 63
  CameraYOffset,       // 64
  SubmergedOtPush,     // A4
  RegisterInvA6,       // A6: 1 when the operand is 0 (purpose unknown)
  PushStrength,        // B5
  ExitTransition,      // FE 02
  FrameVsyncs,         // FE 05
  OverlayGridA0,       // FE 12
  OverlayGridA1,
  OverlayGridA2,
  OverlayGridB0,       // FE 13
  OverlayGridB1,
  OverlayGridB2,
  OverlayOt,           // FE 15
  KeepSfxSlots,        // FE 29: bits 8..15 = C6 slots whose sound survives the next room change
  FootstepVolume,      // FE 2B 10
  FootstepDryType,     // FE 2B 11
  FootstepWaterType,   // FE 2B 12
  FootstepListenerX,   // FE 34 (stored, never read)
  FootstepListenerY,   // FE 34
  FootstepListenerZ,   // FE 34
  FootstepNear,        // FE 34
  FootstepFar,         // FE 34
  Byte4D,              // FE 4D (purpose unknown)
  MenuPreamble3,       // FE 59
  MusicSwapArmed,      // FE 63
  TradingPostRow,      // FE 67
  TradingPostCue,      // FE 67
  Reg6A,               // FE 6A (purpose unknown)
  BattleIntroAnchor,   // FE 70
  SavePointPreamble,   // FE 72
  Selection,           // FE 75
  SelectionFlag,       // FE 75
  PadMask,             // FE 77
  Count,
};
const char* global_name(Global which);

class FieldHost {
 public:
  virtual ~FieldHost() = default;

  // ---- system ---------------------------------------------------------------------------------------------------
  // The "system gate": false while a fade/transition/load/other system activity is in progress. Ops
  // that need it yield without advancing until it is true.
  virtual bool system_ready() = 0;
  // Called once at the end of every VM tick (after all entity scripts ran). The original publishes variables
  // 0x12/0x14/0x16 (player X/Z/Y), 0x0A (facing) and 0x0C/0E/10 (party) here; do it with vm.set_var().
  virtual void end_of_tick(FieldVM& vm) = 0;

  // ---- entity references ------------------------------------------------------------------------------------------
  // Entity of party slot 0..2, -1 when empty.
  virtual int party_entity(int slot) = 0;
  // Entity of playable character 0..43 if in the party, else -1.
  virtual int character_entity(int char_id) = 0;
  // 0..2, or -1 when the entity is not a party member.
  virtual int party_slot_of_entity(int ent) = 0;

  // ---- entity setup (0E 0F D1) -------------------------------------------------------------------------------------
  virtual void setup_pc(int ent, int char_id, int flags) = 0;
  virtual void setup_npc(int ent, int model) = 0;
  // D1: room model `model` for the entity (the room's model list, as 0F) placed on floor level 0; returns
  // the room's D1 ordinal = the entity's bit in the room's defeated-monster mask.
  virtual int load_model(int ent, int model) = 0;
  // Bit of the defeated-monster mask (D1 spawns such an entity disabled).
  virtual bool monster_defeated(int ordinal) = 0;
  // Set the defeated bit (D2 after a won battle).
  virtual void set_monster_defeated(int ordinal) = 0;
  // 0A / 0B  per-tick integrate + animate
  virtual void update_entity(int ent) = 0;

  // ---- position, facing, animation -----------------------------------------------------------------------------------
  // 25 (Y snaps to the ground)
  virtual void set_position_xz(int ent, s32 x, s32 z, int param) = 0;
  // 26
  virtual void set_position_xyz(int ent, s32 x, s32 y, s32 z) = 0;
  // 48 4E 4F 79 ...
  virtual Vec3 get_position(int ent) = 0;
  // 2F 30 31 32
  virtual void set_facing(int ent, int angle) = 0;
  // 49 4A 32
  virtual int get_facing(int ent) = 0;
  // 2E 33
  virtual void turn_to_entity(int ent, int target) = 0;
  // CA CB CC
  virtual bool rotation_busy(int ent) = 0;
  // CA: facing = bearing to (x, z), gated by the slot flag / turn rate
  virtual void face_point(int ent, s32 x, s32 z) = 0;
  // 3C
  virtual void set_animation(int ent, int anim) = 0;
  // A9
  virtual void play_animation(int ent, int anim) = 0;
  // 3D (mode 0..8)
  virtual void set_walk_animation(int ent, int mode, int anim) = 0;
  // C4
  virtual bool animation_busy(int ent) = 0;
  // 41
  virtual void set_move_speed(int ent, int speed) = 0;
  // 3F 40 42-46 5B 5C B8 B9 BA (start forms)
  virtual void start_motion(int ent, const MotionRequest& req) = 0;
  // ... and their wait forms
  virtual bool motion_busy(int ent) = 0;
  // Wait form of 3F / 40 / 42 / 43: the engine's wait form performs the step itself and, when the target is within one step, snaps to it
  // and advances in the same tick. A host that can do that arrival test overrides this.
  virtual bool motion_wait(int ent) { return motion_busy(ent); }
  // D8
  virtual bool near_and_facing(int ent, int target, int dist, int tolerance) = 0;
  // 55 56: entity inside the region
  virtual bool region_test(int ent, int region, bool band) = 0;
  // 7A  record byte compared with U
  virtual int entity_state(int ent) = 0;
  // Head turns (HeadOp): `ent` is the entity whose head turns, `other` the looked-at entity (LookAtHead), (x, y, z) the operands as
  // documented per kind, `rate` the entity's head-turn rate (FE 54: the EXECUTING entity's).
  virtual void head_op(HeadOp op, int ent, int other, s32 x, s32 y, s32 z, int rate) = 0;
  // FE 48: world position of attach point `point` of the entity's model; false when it has none.
  virtual bool attach_point(int ent, int point, Vec3& out) = 0;
  // 82 9D A0 A1 AC C5 C8 FE 03 FE 43 ...
  virtual void entity_effect(u16 key, int ent, std::span<const s32> args) = 0;
  // D6 (target -1 detaches)
  virtual void attach_entity(int ent, int target, int offset) = 0;
  // BF C0 C1 (start/stop of the extra animation of entity `ent`; C1 arg = pack U)
  virtual void extra_animation(u16 key, bool start, int ent, int arg) = 0;
  virtual bool extra_animation_busy() = 0;

  // ---- camera ---------------------------------------------------------------------------------------------------
  // 5D
  virtual void camera_set_position(s32 x, s32 y, int frames) = 0;
  // 5E (centres on the controlled entity)
  virtual void camera_to_actor(int frames) = 0;
  // 61
  virtual void camera_follow(int ent, int frames) = 0;
  virtual bool camera_busy() = 0;
  // 83
  virtual void camera_limits(s32 x0, s32 x1, s32 y0, s32 y1) = 0;
  // 86 (mode != 1)
  virtual void camera_auto_follow(bool on) = 0;
  // C3
  virtual std::pair<s32, s32> camera_position() = 0;
  // FE 50
  virtual void set_camera_origin(s32 x, s32 y) = 0;
  // 5F 60 FE 4C (-1 = clear)
  virtual void set_controlled_entity(int ent) = 0;
  // FE 21
  virtual void shake(int x, int y, int time) = 0;
  // 84 DF set up the party formation
  virtual void formation(u16 key, std::span<const s32> args) = 0;
  // 85 86 BE waits
  virtual bool formation_done() = 0;
  // BE: holds the no-follow rule until the party stands or 240 ticks passed
  virtual bool follow_wait() = 0;

  // ---- dialogue ---------------------------------------------------------------------------------------------------
  // 62 (x, y, w, h, flags)
  virtual void setup_dialog_window(int ent, std::span<const s32> v5) = 0;
  // 9E / 9F (-1 clears)
  virtual void set_dialog_face(int face) = 0;
  // E2 wait: the running entity's own window is open
  virtual bool dialogue_open() = 0;
  // E0 / E3 gate: own window open or all 4 windows in use
  virtual bool dialogue_blocked() = 0;
  // E0 (speaker -1) E3 E4
  virtual void open_dialogue(int speaker, int message, int flags) = 0;
  // E1
  virtual void dialogue_choice(int first, int last) = 0;
  // E2: confirmed choice line, -1 none
  virtual int dialogue_result() = 0;

  // ---- screen -------------------------------------------------------------------------------------------------------
  // 80 81
  virtual void bg_layer(int layer, bool on) = 0;
  // C2
  virtual void set_layer_opacity(int layer, int r, int g, int b) = 0;
  // 90: returns true when the op may advance
  virtual bool bg_wave(int mode, int arg) = 0;
  // 8F: the 8 U operands
  virtual void bg_wave_start(std::span<const s32> p8) = 0;
  // 67 (16 entries of 4 bytes)
  virtual void set_shadow_colors(std::span<const u8> rgbx16) = 0;
  // 68
  virtual void set_screen_rgb(int r, int g, int b, int a, int b2) = 0;
  // 87
  virtual void set_underwater_rgb(int r, int g, int b) = 0;
  // A8
  virtual void fade(int mode, int arg) = 0;
  // FE 44
  virtual void draw_sync() = 0;
  // A1
  virtual void set_entity_rgb(int ent, int r, int g, int b) = 0;

  // ---- audio / movies -----------------------------------------------------------------------------------------------
  // 88 (0xFF = no change); false = refused, the op retries next tick
  virtual bool play_song(int song) = 0;
  // FE 78
  virtual bool play_song_ex(int song, int volume) = 0;
  // 8A wait
  virtual bool song_changing() = 0;
  // C6
  virtual void play_sfx(int mode, int id, int volume, int pan, int extra) = 0;
  // FE 22: slot, frames, volume (the sounds C6 started on the slot slide to it)
  virtual void sfx_volume(int slot, int frames, int volume) = 0;
  // FE 23: the same for the pan (64 = centre)
  virtual void sfx_pan(int /*slot*/, int /*frames*/, int /*pan*/) {}
  // FE 1D
  virtual void fade_master_volume(int frames, int volume) = 0;
  virtual void named_effect(NamedOp op, std::string_view name, int ent) = 0;
  // 7E D4
  virtual void play_movie(int index, int variant, bool skippable) = 0;
  // 7F wait
  virtual bool movie_playing() = 0;

  // ---- flow into other game modes -------------------------------------------------------------------------------------
  // A3 / C9 (C9 passes id + 0x200)
  virtual void request_battle(int battle_id, int arg, int flags) = 0;
  // A3 C9 FE 10 wait; result goes to var 0x32
  virtual bool battle_active() = 0;
  // Cheats "No encounters": true = this A3 / C9 battle (`arg` = the op's raw battle id operand, `ent` = the executing
  // entity) is won without being fought; the VM then runs "09 00 02" (entity 0's slot 2, waited for) instead of the battle.
  virtual bool skip_battle(int /*arg*/, bool /*c9*/, int /*ent*/) { return false; }
  // Cheats "No encounters": true = op D9 is skipped (the PC advances by 4).
  virtual bool skip_model_swap(int /*ent*/) { return false; }
  // 5A / DC
  virtual void request_map_jump(int room, int param) = 0;
  virtual void request_menu(MenuKind kind, int arg) = 0;
  // The menu/shop/save request is still open.
  virtual bool menu_active() = 0;
  // FE 2F
  virtual void field_reset() = 0;

  // ---- party, roster, items, money, characters -----------------------------------------------------------------------
  // Character id in party slot 0..2, -1 empty.
  virtual int party_member(int slot) = 0;
  // 94
  virtual bool in_party(int char_id) = 0;
  // 7B: false = not added (id >= 0x2C, already in the party or no free slot); the engine then skips the next byte (the 7C).
  virtual bool party_add(int char_id) = 0;
  // 7D: character reference (FD / FE / FF = the character in slot 0 / 1 / 2, else a character id);
  // its first slot is removed and the members behind it move up.
  virtual void party_remove(int char_ref) = 0;
  // B2
  virtual void party_override(bool enter, int key) = 0;
  // FE 52
  virtual void swap_leader(int char_id) = 0;
  // 96
  virtual void roster_add(int char_id) = 0;
  // 97
  virtual void roster_remove(int char_id) = 0;
  // B4 D5 (id: <0x200 items, <0x400 elements, 0x400.. key items)
  virtual int item_count(int id) = 0;
  // BB b == 0
  virtual void give_item(int id) = 0;
  // 71: the variable bit `bit_ref` was set
  virtual void var_bit_set(u32) {}
  // BB b != 0
  virtual void take_item(int id) = 0;
  // BC
  virtual s32 money() = 0;
  // BD (already clamped to 0..9999999)
  virtual void set_money(s32 amount) = 0;
  // FE 37
  virtual int char_life(int char_id) = 0;
  virtual void change_char_life(LifeOp op, int char_id, s32 value) = 0;
  // FE 3B
  virtual int char_status(int char_id) = 0;
  // FE 36
  virtual void heal_group(int group) = 0;
  // FE 3D (set = true: status |= mask) / FE 3E (status &= ~(mask & 0xFF)); char_ref as FE 36.
  virtual void change_status(int char_ref, int mask, bool set) = 0;
  // FE 73 74 79
  virtual void learn_limit(int level, int char_id, int value) = 0;
  // FE 7B
  virtual void set_limit_mode(int mode) = 0;
  // D9, char_ref as for 7D: 0 = done (pc += 6, skips the DA), 1 = a party member's model is being replaced
  // (pc += 5 onto the DA, yield), -1 = another replacement is pending (retry next tick).
  virtual int set_character_model(int char_ref, int value) = 0;
  // DA wait
  virtual bool model_swap_busy() = 0;
  // DA
  virtual void model_swap_commit() = 0;
  // DB
  virtual int character_model_byte(int char_id) = 0;
  // 7C
  virtual void refresh_party_models() = 0;
  // FE 5D / FE 60 bitmaps, FE 69, FE 6F, FE 7C, FE 7D, C7
  virtual void table_op(u16 key, int arg, int index) = 0;

  // ---- input -----------------------------------------------------------------------------------------------------------
  // 50
  virtual u16 keys_held() = 0;
  // 51
  virtual u16 key_latch() = 0;
  // 54
  virtual void clear_key_latch() = 0;

  // ---- catch-alls (ops whose effect the VM does not model) ---------------------------------------------------------------
  virtual bool predicate(Predicate which, int arg) = 0;
  virtual s32 query(Query which) = 0;
  // A global written by a script op (the `Global` list); the VM has already stored the value.
  virtual void set_global(Global which, s32 value) = 0;
  // Raw operand bytes after the opcode.
  virtual void opaque_op(u16 key, std::span<const u8> operands) = 0;
};

// Logs the first call of each method and answers "done / nothing": gates open, waits finished, queries zero.
class NullHost : public FieldHost {
 public:
  explicit NullHost(bool verbose = false) : verbose_(verbose) {}
  // Distinct methods called so far (for tests / the soak report).
  const std::set<std::string>& called() const { return called_; }
  size_t calls() const { return calls_; }

  bool system_ready() override;
  void end_of_tick(FieldVM& vm) override;
  int party_entity(int slot) override;
  int character_entity(int char_id) override;
  int party_slot_of_entity(int ent) override;
  void setup_pc(int ent, int char_id, int flags) override;
  void setup_npc(int ent, int model) override;
  int load_model(int ent, int model) override;
  bool monster_defeated(int ordinal) override;
  void set_monster_defeated(int ordinal) override;
  void update_entity(int ent) override;
  void set_position_xz(int ent, s32 x, s32 z, int param) override;
  void set_position_xyz(int ent, s32 x, s32 y, s32 z) override;
  Vec3 get_position(int ent) override;
  void set_facing(int ent, int angle) override;
  int get_facing(int ent) override;
  void turn_to_entity(int ent, int target) override;
  bool rotation_busy(int ent) override;
  void face_point(int ent, s32 x, s32 z) override;
  void set_animation(int ent, int anim) override;
  void play_animation(int ent, int anim) override;
  void set_walk_animation(int ent, int mode, int anim) override;
  bool animation_busy(int ent) override;
  void set_move_speed(int ent, int speed) override;
  void start_motion(int ent, const MotionRequest& req) override;
  bool motion_busy(int ent) override;
  bool near_and_facing(int ent, int target, int dist, int tolerance) override;
  bool region_test(int ent, int region, bool band) override;
  int entity_state(int ent) override;
  void head_op(HeadOp op, int ent, int other, s32 x, s32 y, s32 z, int rate) override;
  bool attach_point(int ent, int point, Vec3& out) override;
  void entity_effect(u16 key, int ent, std::span<const s32> args) override;
  void attach_entity(int ent, int target, int offset) override;
  void extra_animation(u16 key, bool start, int ent, int arg) override;
  bool extra_animation_busy() override;
  void camera_set_position(s32 x, s32 y, int frames) override;
  void camera_to_actor(int frames) override;
  void camera_follow(int ent, int frames) override;
  bool camera_busy() override;
  void camera_limits(s32 x0, s32 x1, s32 y0, s32 y1) override;
  void camera_auto_follow(bool on) override;
  std::pair<s32, s32> camera_position() override;
  void set_camera_origin(s32 x, s32 y) override;
  void set_controlled_entity(int ent) override;
  void shake(int x, int y, int time) override;
  void formation(u16 key, std::span<const s32> args) override;
  bool formation_done() override;
  bool follow_wait() override;
  void setup_dialog_window(int ent, std::span<const s32> v5) override;
  void set_dialog_face(int face) override;
  bool dialogue_open() override;
  bool dialogue_blocked() override;
  void open_dialogue(int speaker, int message, int flags) override;
  void dialogue_choice(int first, int last) override;
  int dialogue_result() override;
  void bg_layer(int layer, bool on) override;
  void set_layer_opacity(int layer, int r, int g, int b) override;
  bool bg_wave(int mode, int arg) override;
  void bg_wave_start(std::span<const s32> p8) override;
  void set_shadow_colors(std::span<const u8> rgbx16) override;
  void set_screen_rgb(int r, int g, int b, int a, int b2) override;
  void set_underwater_rgb(int r, int g, int b) override;
  void fade(int mode, int arg) override;
  void draw_sync() override;
  void set_entity_rgb(int ent, int r, int g, int b) override;
  bool play_song(int song) override;
  bool play_song_ex(int song, int volume) override;
  bool song_changing() override;
  void play_sfx(int mode, int id, int volume, int pan, int extra) override;
  void sfx_volume(int slot, int frames, int volume) override;
  void fade_master_volume(int frames, int volume) override;
  void named_effect(NamedOp op, std::string_view name, int ent) override;
  void play_movie(int index, int variant, bool skippable) override;
  bool movie_playing() override;
  void request_battle(int battle_id, int arg, int flags) override;
  bool battle_active() override;
  void request_map_jump(int room, int param) override;
  void request_menu(MenuKind kind, int arg) override;
  bool menu_active() override;
  void field_reset() override;
  int party_member(int slot) override;
  bool in_party(int char_id) override;
  bool party_add(int char_id) override;
  void party_remove(int char_ref) override;
  void party_override(bool enter, int key) override;
  void swap_leader(int char_id) override;
  void roster_add(int char_id) override;
  void roster_remove(int char_id) override;
  int item_count(int id) override;
  void give_item(int id) override;
  void take_item(int id) override;
  s32 money() override;
  void set_money(s32 amount) override;
  int char_life(int char_id) override;
  void change_char_life(LifeOp op, int char_id, s32 value) override;
  int char_status(int char_id) override;
  void heal_group(int group) override;
  void change_status(int char_ref, int mask, bool set) override;
  void learn_limit(int level, int char_id, int value) override;
  void set_limit_mode(int mode) override;
  int set_character_model(int char_ref, int value) override;
  bool model_swap_busy() override;
  void model_swap_commit() override;
  int character_model_byte(int char_id) override;
  void refresh_party_models() override;
  void table_op(u16 key, int arg, int index) override;
  u16 keys_held() override;
  u16 key_latch() override;
  void clear_key_latch() override;
  bool predicate(Predicate which, int arg) override;
  s32 query(Query which) override;
  void set_global(Global which, s32 value) override;
  void opaque_op(u16 key, std::span<const u8> operands) override;

 private:
  void note(const char* method);
  bool verbose_;
  std::set<std::string> called_;
  size_t calls_ = 0;
};

}  // namespace oc::script
