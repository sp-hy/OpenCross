#pragma once
// Battle animation-sequence ("anmseq") interpreter.
//
// Every battle model owns two script tables: the model's own S5 block (state animations: 1 idle, 2 approach, 3 return, 5 cast, 6 release, 7 run...)
// and the external 'atk' block of the unit's at0.prd (entry 0 = the attack program: variant 0 = the attacker's script, variant 7 = the
// reaction script run by every target). A script is s16 words: word 0 = op, operands follow; the handler's return value is the
// advance in words, 0 = blocked until the next tick. The VM here runs one script on one unit and reports what it asked the
// engine for as Events; it never touches rendering. Ticks are 60 Hz.
//
// Not interpreted (length-correct no-ops): the ops without a case, such as 0x5f and 0x70; the weapon-trail ops 0x23-0x25 0x46 0x5b 0x85 0x86 are events.
#include <array>
#include <functional>
#include <set>
#include <vector>

#include "core/types.h"

namespace oc::game::bseq {

// ---- tables ------------------------------------------------------------------------------------------------------------
// u32 count; u32 offset[count + 1] (bytes from the block start); entry i = 8 x s16 variant offsets (bytes from the entry start,
// 0 = none) followed by the code. All jumps are relative, so one word array serves the whole block.
class Table {
 public:
  bool parse(ByteSpan block);
  bool empty() const { return words_.empty(); }
  int entries() const { return static_cast<int>(entry_off_.size()); }
  // Index into words() of the first op of (entry, variant), or -1.
  int start(int entry, int variant) const;
  const std::vector<s16>& words() const { return words_; }
  // Adds the tag of every op 0x68 / 0x7D (start an effect sequence) in the table: each variant's code is swept op by op up to the
  // next entry, so branches the VM would skip count too.
  void sequence_tags(std::set<u32>& out) const;
  // Calls `visit(entry, pc)` once for every op the same sweep reaches (each word index once, even where variants share code).
  void each_op(const std::function<void(int entry, int pc)>& visit) const;
  // The unit's own S5 table seen from its atk table: op 0x44 reads the
  // blend table in S5 entry 0. Set by the loader for atk tables.
  void set_model_table(const Table* table) { model_ = table; atk_ = true; }
  const Table* model_table() const { return model_; }
  bool is_atk() const { return atk_; }
  // Word `index` after the 4-byte start of entry 0 (the GetHokanFrame table); 0 when absent.
  // The attached-object hide words the model's S5 entry 8 sets before its first yielding op (at battle start entry 8 runs for the party):
  // bit s set = slot s hidden (ops 0x7A n), bit s clear after a 0x79 n. Cheap prefix scan, no VM run. 0 when absent.
  int initial_weapon_hide() const;
  // Entry 8 (the spawn script) ops 0x21 / 0x22 before its first wait: 1 = sets the model's drop shadow, 0 = clears it, -1 = neither.
  // Retail plays entry 8 for every unit that battle init does not seed off-stage: ki / q_con2 / sakana / fate / tokikui clear it.
  int initial_shadow() const;
  int hokan_word(int index) const {
    if (entry_off_.empty() || index < 0) return 0;
    const size_t word = entry_off_[0] / 2 + 2 + static_cast<size_t>(index);
    return word < words_.size() ? words_[word] : 0;
  }

 private:
  std::vector<s16> words_;
  // Entry offsets in bytes.
  std::vector<u32> entry_off_;
  const Table* model_ = nullptr;
  bool atk_ = false;
};

// Four-character tag as the engine builds it from two s16 operands (low half first): "_s00", "atk1", "thgh"...
inline u32 tag_of(s16 lo, s16 hi) { return static_cast<u16>(lo) | (static_cast<u32>(static_cast<u16>(hi)) << 16); }
inline std::string tag_str(u32 tag) {
  std::string text;
  for (int i = 0; i < 4; ++i) {
    const char ch = static_cast<char>((tag >> (8 * i)) & 0xFF);
    if (ch) text.push_back(ch);
  }
  return text;
}

// The model's own table: S5 block of the .obj (header u32 offsets at +4, entry 4 = S5, 5 = S6 or end of file).
bool load_model_table(ByteSpan obj, Table& out);
// The external table: the type 0x1A entry "atk" of at0.prd (a drp).
bool load_atk_table(ByteSpan at0_prd, Table& out);

struct Event {
  enum Kind {
    // a = clip id (<0x80 own model clip, >=0x80 at0 clip a-0x80), b = flags/blend (low 13 bits = blend ticks)
    SetAnim,
    Speed,       // a = clip speed (64 = 1.0, 0.25 frame/tick)
    Loop,        // a = 1 loop / 0 hold last frame
    Sfx,         // tag, a = volume, b = 1: positional (op 0x33, pan / volume from the unit's screen position)
    Sequence,    // tag: type 0x10 sequence script (camera / effect timeline) of the acting model's resources
    Camera,      // tag: type 0x0C camera shot
    Damage,      // op 0x41 CallDamage: the damage numbers appear (the hit lands) on this unit
    // op 0x59 / 0x5a: a = target list index, -1 = all targets: they run variant 7 of the attacker's atk table
    React,
    // op 0x0C MoveS: a = target index, b = speed units/tick, c = ticks (from Ctx::move_ticks): walk to the target's approach point
    MoveSpeed,
    MoveTime,    // op 0x0B MoveT: a = target index, b = ticks
    MoveHome,    // op 0x13 RetMoveT: b = ticks, back to the stand position
    MovePos,     // op 0x71 PosMoveT / 0x0D VecMoveT: a,b = x,z world units, c = y (y down: var [v+2]), d = ticks
    Face,        // op 0x12: a = target index, b = ticks: turn to face it
    // op 0x11 RotateT: a = absolute yaw (4096 = turn, vars [v], [v+2], [v+1] = rx, ry, rz & 0xFFF), b = ticks
    Rotate,
    // op 0x1d: emitters named `tag` follow attach point b; a = flags (&0x7F rotation mode, 0x80 follow), off / ang below
    Effect,
    TrailColors, // op 0x23: off = node colour A (r g b), ang = node colour B
    TrailMode,   // op 0x46: a = GP0 semi-transparency mode
    TrailFade,   // op 0x5B: off = per-tick colour change (signed)
    TrailOn,     // op 0x24 / 0x85: a = slot, b / c = attach points sampled
    TrailOff,    // op 0x25 / 0x86: a = slot
    ColorRamp,   // op 0x64 / 0x8E: a = ticks, b = 0 back colour / 1 model colour, off = from, ang = to (bytes)
    LightPos,    // op 0x5C: a = unit list (0 party / 1 enemy), b = attach point, c = light slot
    LightSet,    // op 0x5E: a = slot, off = rgb, b = falloff, c = intensity, d = ramp target, ang[0] = ramp ticks
    ModelShow,   // op 0x19 / 0x1A: a = 1 show / 0 hide
    PartShow,    // op 0x1B / 0x1C: a = 1 show / 0 hide part b
    ModelColor,  // op 0x1F: off = model colour (0x80 = 1.0)
    ModelAbr,    // op 0x20: a = m: 0 semi-transparency off, else on with ABR m - 1
    // op 0x6E JumpT: a,b = x,z, c = y (y down), d = ticks: x / z linear, y a ballistic arc (g = 0x9ccc); tag 1 = op 0x7F (y: vy0 = 2 dy / n, a = -2 dy / n^2), tag 2 = op 0x80 (vy0 = 0, a = 2 dy / n^2)
    Jump,
    WeaponShow,  // op 0x79 / 0x7A: a = 1 show / 0 hide the attached weapon object b (0 / 1; -1 = both)
    // op 0x5D: a = light slot, off = x, y, z (s16 words 2-4, world units): the slot position := these values (5C takes an attach point)
    LightAbs,
    // op 0x72 / 0x73: a = 1 BlinkOn (only for units with party faces) / 0 BlinkOff (always; also shows eye frame 0)
    Blink,
    // op 0x4F: a = eye frame n (0 open, 1 half, 2 closed) of the party face; only for units with party faces
    Eye,
    // op 0x98 LinkEid: a,b = list,idx of the carried unit, c = this unit's attach point, d = the carried unit's, off[0] = ticks (0 = for good)
    Link,
    SetPos,      // op 0x9a: the unit is put at x = a, z = b, y = c (y down) at once
    SetRot,      // op 0x62: the unit's rotation at once: a = yaw (4096 = turn), off = x rot, z rot
    Shadow,      // op 0x21 / 0x22: the drop shadow, a = 1 set / 0 cleared
    SlowK,       // op 0x82: a = the frame-pacing catch-up limit
    ModelScale,  // op 0x83: model scale (4096 = 1.0) ramp to off[0..2] over d ticks (d < 1: at once)
    // op 0x52: a full-screen 320 x 224 flat quad: a = ticks, b = abr mode (0 = opaque, else abr b - 1), off = from rgb, ang = to rgb (bytes)
    ScreenFade,
    // op 0x53: model colour ramps from off to ang over a ticks, b = semi-transparency mode (0 = off, else abr b - 1); no flash draw
    ModelFade,
    PartScale,   // op 0x8F: element a's scale (4096 = 1.0) ramp to off[0..2] over d ticks (d < 1: at once)
    PartScaleStop,  // op 0x90: the scale ramp tasks of element a end
    // op 0x91: alternates parts a and b (a, b = parts, c = ticks a is shown, d = ticks b is shown; starts with b)
    PartBlink,
    PartBlinkStop,  // op 0x92: the blink tasks (a, b) end
    ElemMotion,  // op 0x6A / 0x6B: element a's own keyframe motion off (b = 0) / on (b = 1)
    // op 0x6C: element a's rotation (a = element, off = x, y, z in 4096 = turn) reached over d ticks (d < 1: at once)
    ElemRot,
    // op 0xA0: shading entry a's colour bytes ramp from off to ang over d ticks (d < 1: to at once), b = semi mode (0 = opaque, else ABR b - 1)
    PartColor,
    ElemRotRate, // op 0x6D: element a keeps turning by off (x, y, z) per tick
    // op 0x8C: element 0's z rotation walks to off[2] (4096 = turn) over d ticks, its own motion off, the old z saved
    ElemTilt,
    // op 0x8D: element 0's z rotation walks back to the saved value over d ticks, then its motion is on again
    ElemTiltBack,
    PoseWord,    // op 0xAC: the unit's idle-pose word := a (the idle script's GetPose; 8 = dead)
    // op 0x88: the head turns toward attach point c of list a's entry b, followed every tick (rate 0xE00)
    HeadLook,
    HeadRelease, // op 0x89: the head target becomes straight ahead and the head turn ends once settled
    // op 0xA2 / 0xA3: texture rect A (b 0) / B (b 1) := VRAM x, y, w, h = off[0..2], d (x, w in halfwords)
    TexRect,
    TexDest,     // op 0xA4: the rect copies go to list a's entry b's texture at (c halfwords, d rows) from its origin
    TexFlip,     // op 0xA5: copy A, then B / A in turn every a / b ticks, for c ticks
    Done,        // script ended: a = the op that ended it (0x00 / 0x3E / 0x4C, 0x03 on an empty stack, -1 off the table)
  } kind = Done;
  int a = 0, b = 0, c = 0, d = 0;
  u32 tag = 0;
  // Effect: offsets (words 5-7) and angles (words 8-10).
  s16 off[3] = {0, 0, 0}, ang[3] = {0, 0, 0};
};

// What the scripts ask about their own unit / the running action.
struct Ctx {
  // action_a (op 0x43 first output): 0 weak, 1 medium, 2 heavy for weapons.
  int action_a = 0;
  // action_b (second output): hand toggle (alternates between chained hits).
  int action_b = 0;
  // 0 normal, 1 no damage, 2 miss, 3 heal/absorb.
  int hit_code = 0;
  bool dead = false;
  // GetStatusL / H: bit 0 dead, bit 31 HP <= max/4.
  u32 flags = 0;
  // Merged status words (GetBtStatus0..3).
  u32 status_lo = 0, status_hi = 0;
  // GetAttrib (op 0x58): the colour byte of the running unit's control block, an innate byte (bits 2..7 the colour, bit 1 absorbs its own
  // colour). The hit reactions pick their flash colour and the attack programs their weapon trail colours from it.
  int attrib = 0;
  // GetModelNum.
  int model_num = 0;
  // GetMonBit.
  bool monster = false;
  // GetRot (op 0x61): the unit yaw, 4096 = a turn.
  int yaw = 0;
  // GetStandPos (world units).
  int stand_x = 0, stand_z = 0;
  // GetPose: the clip the idle script holds (1 = stand-by).
  int pose = 1;
  // Op 0x30 Rand; null = std::rand.
  std::function<int()> rand;
  // MoveS: (target index, speed units/tick) -> ticks to walk there; null = 30.
  std::function<int(int, int)> move_ticks;
  // The unit's world position (y down: negative pos_y = airborne); PosMoveT / VecMoveT / the direction query 0F work from it.
  int pos_x = 0, pos_z = 0, pos_y = 0;
  // Op 74 GetPos: list 0 = casters / else targets, index, mode (0 the model origin, 1..4 the origin + the rotated x / z extent of its
  // joints, 5 the approach point (attach 15), 6 the targets' centroid) -> world x, z. False = no such actor.
  std::function<bool(int list, int idx, int mode, int& x, int& z)> get_pos;
  // Op 0x81 GetELPos: world position (x, z, y: y down) of attach point `point` of the posed model of list entry idx (0 = casters, else targets).
  std::function<bool(int list, int idx, int point, int& x, int& z, int& y)> get_attach;
  // Own model's attach point (op 0x8C reads attach 9 of the running unit): world x, z, y (y down).
  std::function<bool(int point, int& x, int& z, int& y)> get_attach_self;
  // Op 0x9F GetCommon: a common word of the unit's control block.
  int common = 0;
  // Op 0x69 GetOption: the option word of the unit's control block, copied from the stat record's u16 at 0x14 (low byte 0 / 1 / 0x70,
  // bits 14-15 the gender). The tech reactions test bits 0x10 / 0x40 / 0x80 of it.
  int option = 0;
  // Op 0xA9 GetBeta: the unit's pending result word (the number its hit shows, -1 = none). Element mode 47 (Beta) writes the field
  // colour bit << 2 into the caster's word, mode 48 (Shuffle) the swapped HP value.
  int result = 0;
  // Op 0xA1 GetNumberTarget: the entries of the target list and the unit's index in it (-1 = not in it).
  int target_count = 0, target_index = -1;
  // Op 74's y (y down) of that actor; null = 0.
  std::function<int(int list, int idx)> get_pos_y;
  // Frame count of clip id (used by the wait-for-end ops); null = 8.
  std::function<int(int)> clip_frames;
};

class Vm {
 public:
  // Starts (table, entry, variant). False if the script does not exist (the VM stays idle).
  bool start(const Table* table, int entry, int variant);
  // Battle start: the previous action number of GetHokanFrame = -1.
  static void reset_battle() { prev_action_ = -1; seq_last_ = 0; }
  // Forgets the last camera timeline name op 0x68 started (compared on its first three bytes): op 0x7E clears it, an action start and the
  // battle start too.
  static void reset_sequence() { seq_last_ = 0; }
  // The state every VM shares (the global words, the previous action, the last timeline name), taken before a script runs outside a
  // battle and put back after, so the next battle starts from what it would have.
  struct Shared {
    std::array<s16, 128> globals{};
    int prev_action = -1;
    u32 seq_last = 0;
  };
  static Shared shared() { return {globals_, prev_action_, seq_last_}; }
  static void restore(const Shared& state) {
    globals_ = state.globals;
    prev_action_ = state.prev_action;
    seq_last_ = state.seq_last;
  }
  // The global anmseq words (operand index byte with bit 0x80): spark's impact helper 7314 reads / clears word 0.
  static int global(int index) { return globals_[static_cast<size_t>(index & 0x7F)]; }
  static void set_global(int index, int value) { globals_[static_cast<size_t>(index & 0x7F)] = static_cast<s16>(value); }
  bool running() const { return running_; }
  void stop() { running_ = false; }
  // One 60 Hz tick: runs ops until one blocks. Events are appended to `out`.
  void tick(const Ctx& ctx, std::vector<Event>& out);

  // Debug hook: called before every op with its word index and the op's words.
  std::function<void(int pc, const s16* words, int len)> trace;

  int loop_flag() const { return loop_; }
  int var(int index) const { return vars_[static_cast<size_t>(index & 0x7F)]; }
  static constexpr int kMaxOpsPerTick = 4096;

 private:
  int rd(int idx) const;
  void wr(int idx, int value);
  bool wait_ticks(int ticks);
  // Waits until `total - param` ticks of this animation / move have passed.
  bool wait_total(int total, int param);
  int step(const Ctx& ctx, std::vector<Event>& out);

  const Table* table_ = nullptr;
  int pc_ = 0;
  bool running_ = false;
  // Wait counter.
  int counter_ = 0;
  int anim_frames_ = 8;
  int move_ticks_ = 0;
  // OvalMoveS task: the unit orbits an ellipse between its position and a target point.
  // cx..az: centre, semi-axes, unit vector of the major axis (towards the target).
  // step: angle (4096 = turn) per tick; left: ticks left; angle: current angle; out: the variable it is mirrored to.
  struct Oval {
    bool on = false;
    double cx = 0, cz = 0, rx = 0, rz = 0, ax = 1, az = 0;
    int step = 0, left = 0, angle = 0, out = 0;
  } oval_;
  // The tick's working copy of the Ctx: SetPos (which writes the model at once) updates its position for the ops after it.
  Ctx* live_ = nullptr;
  // The oval task ended this tick: its velocity is cleared once the script has run.
  bool zero_vel_ = false;
  // Velocity (20.12 units per tick) of the last PosMoveT / VecMoveT: op 0F reads the direction.
  int vel_x_ = 0, vel_z_ = 0;
  int loop_ = 0;
  std::array<s16, 128> vars_{};
  std::array<int, 8> stack_{};
  int sp_ = 0;
  static std::array<s16, 128> globals_;
  // Previous action number (GetHokanFrame).
  static int prev_action_;
  // Last camera timeline name started by op 0x68.
  static u32 seq_last_;
  int variant_ = 0;
};

}  // namespace oc::game::bseq
