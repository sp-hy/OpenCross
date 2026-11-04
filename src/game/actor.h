#pragma once
// A field actor: optional skinned model + skeleton pose + clip playback + pose/movement state on the walk mesh.
//
// Simulation runs at 60 Hz (tick); rendering interpolates position, facing and animation time between the last two
// ticks (alpha). Angles: 4096 = 360 degrees. Joint matrices are rebuilt each render frame from the skeleton hierarchy: palette[j] =
// palette[parent] * (R(rot) | trans), with the keyframe values interpolated at the interpolated time.
//
// An Actor without a model is still a valid pose holder (trigger boxes, invisible script entities).
// Two ways to drive it: tick() (pad movement by heading, used for the player) or the script-driven calls
// begin_tick() / set_position() / set_facing() followed by step_visual() each tick.
#include <cmath>
#include <memory>
#include <vector>

#include <span>

#include "assets/model.h"
#include "game/walk_mesh.h"
#include "render/camera.h"
#include "render/mat4.h"
#include "render/mesh_pipeline.h"
#include "render/sprite_batch.h"

namespace oc::vfs {
class Vfs;
}

namespace oc::game {

// ---- floor shadows ------------------------------------------------------------------------------------------------------------------
// One textured, semi-transparent quad per actor: the 32x31 4bpp blob of map/etc/systim.bin (TIM 0), subtractive, colour = the grey
// strength (128 = full texel). The quad spans +-0x200 in a frame built from the floor plane normal, scaled by the actor scale
// (0x200 -> +-64 units), centred on the floor point. Filed in ONE ordering slot: z of corner 2 >> 4, plus the actor layer; not depth-written.
struct ShadowParams {
  // Floor point.
  float x = 0, y = 0, z = 0;
  // Outer product of the two normalised edges of the floor triangle (4.12, not renormalised).
  float normal[3] = {0, 4096, 0};
  // 0x200 = default.
  float scale_x = 512, scale_z = 512;
  int grey = 64;
  // Actor layer (op 2A), in slots.
  float layer = 0;
  // [Enhancements] Lighting: the actor's tile of `mask` multiplies the floor over +-tile_half units instead of the blob: uv = centre, half span. 0 = the blob.
  float tile_half = 0;
  std::array<float, 3> tile_uv{};
  // The floor's rise per unit of x and z under the tile.
  float tile_slope_x = 0, tile_slope_z = 0;
};

class ActorShadows {
 public:
  // Reads the blob texture from cdrom/map/etc/systim.bin; false (no shadows drawn) when it is missing or not the expected TIM.
  bool init(const vfs::Vfs& files, render::GpuContext& ctx, render::SpriteBatch& sprites);
  void shutdown(render::SpriteBatch& sprites);
  // Draws inside an open SpriteBatch pass (after the background layers): subtractive (a tile: multiplied), depth tested against the layers, not written.
  void draw(render::SpriteBatch& sprites, const render::FieldCamera& camera, const render::Letterbox& lb, float scroll_x, float scroll_y,
            std::span<const ShadowParams> shadows, bool linear, render::TexRef mask = {}) const;
  // The floor-plane normal of walk-mesh triangle `tri` as the engine computes it (+y down; (0, 4096, 0) off the mesh).
  static void floor_normal(const WalkMesh& walk, int tri, float out[3]);

 private:
  render::Texture tex_;
  render::Sampler nearest_, linear_;
  render::TexRef set_nearest_, set_linear_;
};

// Field clip slots of the overworld human models: s4 index.
// 0-based s4 slots: the 3rd clip (index 2) plays as the run cycle, so "1 default, 2 stand-by, 3 run, 4 walk, 5 idle" is 1-based.
enum FieldClip : int { kClipDefault = 0, kClipStandBy = 1, kClipRun = 2, kClipWalk = 3, kClipIdle = 4 };

// Engine constants. Distances in world units, angles in 4096/turn, speeds in units per 60 Hz tick.
// Model units -> world units.
constexpr float kDefaultScale = 512.0f / 4096.0f;
// "No direction" marker of the heading word.
constexpr int kNoHeading = 0x8000;
// Default actor speed; units/tick = speed / 16.
constexpr int kDefaultSpeed = 0x50;
// 0x50 / 16.
constexpr float kWalkUnitsPerTick = 5.0f;
// (2 * S + S / 2) / 16.
constexpr float kRunUnitsPerTick = 12.5f;
// Default turn rate: the engine steps the yaw once per rendered frame by the turn rate * ticks-this-frame,
// i.e. the turn rate per 60 Hz tick: 0x80 = 180 deg in 16 ticks. Per-actor value in Actor::turn_rate.
constexpr int kTurnPerTick = 0x80;
// Model yaw = facing - 0x400.
constexpr int kModelYawBias = 0x400;
// Collision probe distance in front of the move.
constexpr float kProbeAhead = 8.0f;
// The shadow is NOT sized by the radius: +-0x200 x actor scale, ActorShadows.
constexpr float kDefaultRadius = 38.0f;
constexpr float kDefaultHeight = 300.0f;
// Clip switch blend length.
constexpr int kCrossFadeTicks = 0x20;
// 0.25.
constexpr float kKeyframesPerTick = assets::kModelAnimFps / 60.0f;
// Trigger ranges for the script host (circle actors, centre distance squared < (r_p + margin + r_o)^2):
// touch (event 3).
constexpr float kTouchMargin = 16.0f;
// talk (event 2), with the action button and the facing cone.
constexpr float kTalkMargin = 48.0f;
// (facing - (0x800 - atan(dz, dx))) & 0xFFF window, +-61 deg.
constexpr int kTalkConeMin = 0x545, kTalkConeMax = 0xABB;
constexpr bool in_talk_cone(int facing_minus_bearing) {
  const int masked = facing_minus_bearing & 0xFFF;
  return masked >= kTalkConeMin && masked <= kTalkConeMax;
}

struct ActorPose {
  // World units, Y down.
  float x = 0, y = 0, z = 0;
  // Model yaw in 4096/turn, = facing - 0x400; may be fractional when interpolated.
  float yaw = 0;
};

// Model asset + its GPU copy, shared by every actor that uses the same model.
struct ModelHandle {
  assets::ModelAsset asset;
  std::unique_ptr<render::MeshModel> gpu;
};

// Script animation ids (SET_ANIMATION): 0..0x7F = s4 slot, 0x80 + k = extra ("ex") clip k of the model.
constexpr int kExtraAnimBase = 0x80;
// Walk-animation slots set by SET_WALK_ANIMATION (modes 0..8, in the order of the enum below).
enum WalkAnimSlot : int { kSlotStand, kSlotWalk, kSlotRun, kSlotE6, kSlotStairsUp, kSlotIdle2, kSlotStairsDown, kSlotTurnL, kSlotTurnR, kSlotCount };

// A LOAD_ANIMATION pack (fieldobj/ex/*.bin): its clips decoded against the entity's skeleton and its animation-sequence table.
struct ExPack {
  std::vector<assets::AnimationClip> clips;
  assets::AnimSeqTable seq;
};

// Model variables of the animation-sequence interpreter.
constexpr int kModelVarCount = 16;
// Bit 0 -> the anim-done bit, WAIT_ANIMATION.
constexpr int kVarAnimDone = 0;
// Bits 0-3: foot contact at attach points 12 / 13 / 7 / 8; bits 4 / 5: footstep sound.
constexpr int kVarFeet = 1;

class Actor {
 public:
  static std::shared_ptr<ModelHandle> make_model(render::MeshPipeline& pipeline, assets::ModelAsset asset);
  // make_model of an asset whose texture mods are already resolved (render::resolve_model_textures, run on a worker thread), with its
  // uploads and GPU textures optionally made there too (the textures are taken).
  static std::shared_ptr<ModelHandle> make_resolved_model(render::MeshPipeline& pipeline, assets::ModelAsset asset,
                                                          const render::ModelUploads* uploads = nullptr, render::ModelTextures* textures = nullptr);

  // Takes the asset (kept for the skeleton and clips) and uploads it.
  bool load(render::MeshPipeline& pipeline, assets::ModelAsset asset);
  void set_model(std::shared_ptr<ModelHandle> model);
  // Art-mode switch: the same model read from the other file set (cdrom2 mesh / UV atlas / texture). Unlike set_model()
  // the playback state (clip, keyframe, cross-fade, requested script clip, ex pack, idle timers) is kept: the clips are the same data
  // in both sets. False (and the current model kept) when the joint or clip counts differ.
  bool swap_model_art(std::shared_ptr<ModelHandle> model);
  const std::shared_ptr<ModelHandle>& model_handle() const { return model_; }
  bool loaded() const { return model_ != nullptr && model_->gpu != nullptr; }
  void release() { model_.reset(); }

  // Puts the actor on triangle `tri` at (x, z); the height comes from the plane. `facing` in 4096/turn.
  void place(const WalkMesh& walk, int tri, float x, float z, int facing);

  // One 60 Hz tick. `heading` = final movement heading (pad heading + control yaw, 0..0xFFF) or kNoHeading. Moves at
  // walk/run speed with probe + wall slide, turns the model toward the facing at kTurnPerTick, advances the animation
  // (stand 1 / walk 3 / run 2, 0x20-tick cross-fade on switches).
  // Cheats "No clip": tick() moves by the full step whatever the walk mesh says (the triangle follows when one is under the actor).
  bool noclip = false;
  void tick(const WalkMesh& walk, int heading, bool run);
  // The pad velocity of the last tick() before the walk-mesh probe, wall slide and slope rebuild (0 when standing): what a pushed
  // actor takes over.
  float pad_step_x() const { return pad_step_[0]; }
  float pad_step_z() const { return pad_step_[1]; }

  // ---- script-driven use -----------------------------------------------------------------------------------------
  // Call before anything moves the actor this tick.
  void begin_tick() { prev_ = cur_; }
  // The off-screen cull: the animation sequence, the clip playback and the head callback do not run.
  void set_culled(bool culled) { culled_ = culled; }
  bool culled() const { return culled_; }
  // An inactive entity (SET_VISIBLE 0): no turn and no new clip; the clip it already plays keeps running.
  void set_inactive(bool inactive) { inactive_ = inactive; }
  // Call once after everything moved the actor this tick (FieldWorld::sim_tick). Display smoothing: a script that steps an actor
  // (or turns its model) only every 2nd-4th tick (the circling child: 9-instruction UPDATE, one step per 2 ticks; the yaw chases
  // every 2nd tick) would otherwise be drawn as move-one-tick / stand-one-tick at a render rate above the tick rate. Position and
  // yaw each keep a segment (start, end, span): a change spreads over the gap since the previous change (1..4 ticks), so the
  // display runs continuously and an every-tick mover is unchanged. Warps (place, teleport, > 150 units) restart the segment.
  void end_tick();
  // x/z move; y comes from the walk-mesh plane when (x, z) is inside a triangle and `free_y` is false. `teleport` also
  // resets the interpolation source so the actor does not glide.
  void set_position(const WalkMesh& walk, float x, float z, bool teleport, bool free_y = false);
  void set_height(float y, bool teleport);
  // Direct position write with a known triangle: the party followers replay the leader's ring (copying the ring entry's tile;
  // followers never go through the mover's mesh test). tri -1 keeps the current one.
  void move_free(float x, float y, float z, int tri) {
    cur_.x = x;
    cur_.y = y;
    cur_.z = z;
    if (tri >= 0) tri_ = tri;
  }
  // A script/AI move by (dx, dz) with the leader's probe + wall-slide rule (WalkMesh::move_actor); never leaves the mesh.
  // Returns false when nothing moved. Falls back to set_position when the actor has no triangle yet.
  bool move_on_mesh(const WalkMesh& walk, float dx, float dz);
  // Stairs / ladders (once per tick for every actor): `tile_flags` = the walk flags of the actor's triangle. A tile with
  // 0x4000 / 0x8000 turns the actor toward the plane normal's horizontal direction (uphill, facing the ladder) and selects the stair clip
  // in step_visual, whether it moves or stands; tiles with 0xC800 take away the run speed.
  void set_tile(const WalkMesh& walk, u32 tile_flags);
  // Ground-aligned orientation (script FE 00 0 / FE 00 1): the model's up axis
  // follows the plane normal of the tile it stands on. The tilt matrix steps toward its target by at most 0xAA / 0x1000 per element per tick
  // unless `snap` (which the engine clears after the frame it was used); the yaw is applied inside it.
  void update_ground_tilt(const WalkMesh& walk, bool enabled, bool snap);
  u32 tile_flags() const { return tile_flags_; }
  // Position snapshot for undoing a move that ended inside a solid entity.
  struct Spot {
    float x = 0, y = 0, z = 0;
    int tri = -1;
  };
  Spot spot() const { return {cur_.x, cur_.y, cur_.z, tri_}; }
  void restore(const Spot& spot) {
    cur_.x = spot.x;
    cur_.y = spot.y;
    cur_.z = spot.z;
    tri_ = spot.tri;
  }
  // Sets the target facing; the model turns at turn_rate per tick.
  void set_facing(int facing) { facing_ = facing & 0xFFF; }
  void snap_facing() { smooth_warp_ = true; cur_.yaw = prev_.yaw = static_cast<float>((facing_ - kModelYawBias) & 0xFFF); }
  // Sets the model yaw so the model shows `facing` this tick (still interpolated from the previous tick): the curved walks
  // write the yaw directly.
  void set_shown_facing(int facing) { cur_.yaw = static_cast<float>((facing - kModelYawBias) & 0xFFF); }
  int shown_facing() const { return (static_cast<int>(std::lround(cur_.yaw)) + kModelYawBias) & 0xFFF; }
  // Model yaw has not reached the target facing (WAIT_ROTATION CB / CC: (yaw + 0x400) & 0xFFF != facing & 0xFFF). A pure angle test:
  // with a turn rate of 0 it stays true (the engine waits for ever; FieldWorld::rotation_busy decides).
  bool turning() const;
  // Turns toward the facing and advances the animation. `moving`/`run` pick the automatic clip (stand / walk / run)
  // unless a script clip is requested; moving always overrides a requested clip. A model with an animation-sequence table
  // (field characters) runs its script for the submitted id: the script starts the clips, sets the loop flag, the
  // anim-done variable and the foot events.
  void step_visual(bool moving, bool run);

  // SET_ANIMATION (3C / A9): the script animation id in force; the anim-done bit and model var 0 are cleared. The same id
  // as the one submitted does not restart; 0xFF re-submits the current one. Without a sequence
  // table: looping slots 1-4, others play once and hold the last frame.
  void play(int script_anim);
  // A clip the engine itself switches to (the JUMP_MOVE fall clip; the scripted-turn clip, which also
  // forces `restart`): like play(), but the anim-done bit is set with it, so a pending WAIT_ANIMATION ends.
  void play_engine_clip(int script_anim, bool restart = false) {
    play(script_anim);
    if (restart) submitted_ = -1;
    busy_ = false;
  }
  // The model has a clip for this script id (an s4 / ex clip, not the stand-in fallback). The engine refuses ids past the table.
  bool has_script_clip(int script_anim) const;
  // LOAD_ANIMATION (C1) / FREE_EXTRA_ANIMATION (C0): replaces / restores this actor's ex clips (SET_ANIMATION 0x80 + k) and the ex
  // animation-sequence table.
  void set_ex_clips(std::vector<assets::AnimationClip> clips, assets::AnimSeqTable seq = {});
  void clear_ex_clips();
  void clear_play() { requested_ = -1; busy_ = false; }
  // True while the anim-done bit is clear.
  bool anim_busy() const { return busy_; }
  int requested_anim() const { return requested_; }
  // The id whose clip / sequence was last started.
  int submitted_anim() const { return submitted_; }
  bool has_anim_seq() const { return model_ && !model_->asset.anim_seq.empty(); }
  int model_var(int k) const { return k >= 0 && k < kModelVarCount ? vars_[static_cast<size_t>(k)] : 0; }
  // Reads model var 1 once per frame and clears it: the foot events of this tick (bits 0-3 = attach points 12, 13, 7, 8).
  int take_foot_events() {
    const int events = vars_[kVarFeet];
    vars_[kVarFeet] = 0;
    return events;
  }
  // World position of attach point `point` at the current tick (joint matrix * offset + translation).
  bool attach_world(int point, float out[3]) const;
  // Attach-point frame for an emitter follower (anim-sequence op 1D): world rotation part of the joint of `point`
  // (model scale removed, row-major 3x3) and the attach point's world position. False without a model / joint.
  bool attach_frame(int point, float rot[9], float pos[3]) const;
  // Model rotation without scale (row-major 3x3): the entity matrix x the model rotation of the engine.
  void model_rotation(float rot[9]) const;
  // World matrix of the joint of attach point `point` (model_matrix x joint palette at render fraction `alpha`, scale included) and the
  // point's joint-space offset (attach table entry + its joint's composite matrix). False without a model / joint.
  bool attach_joint(int point, float alpha, render::Mat4& joint_world, float offset[3]) const;
  // Attached display (D6 + FE 46 / FE 47): the model is drawn with the parent's joint matrix of attach point `point` x `local` (the
  // joint-space translation, rotation and scale) instead of the actor pose. The parent's joint is taken at the same render fraction
  // as the drawn frame, so the model stays rigidly on the parent however the parent's pose is interpolated. clear_attached() ends it.
  void set_attached(const Actor* parent, int point, const render::Mat4& local);
  void clear_attached() {
    attached_ = false;
    attach_parent_ = nullptr;
  }
  bool attached() const { return attached_; }

  // Anim-sequence op 1D: `fl pt name name ox oy oz rx ry rz` = start the room emitters named `name` with a follower on
  // attach point pt (fl & 0x7F = rotation mode 0 / 1 / 2, fl & 0x80 = follow the point every tick), offsets (ox, oy, oz), angles r.
  struct BoneEmitterRequest {
    std::string name;
    int flags = 0, point = 0;
    std::array<s16, 3> off{}, ang{};
  };
  std::vector<BoneEmitterRequest> take_bone_emitters() { return std::exchange(bone_emitters_, {}); }

  // ---- head turns -----------------------------------------------------------------------------------------------------------------
  // The head joint is the joint of attach point 1. head_look_at is called after the caller
  // stored the world target: `rate` (1..0xFFF, else the old value is kept) becomes the head turn rate; a head
  // that is not turning yet starts from the model's forward axis. head_reset: a turning head gets the target straight
  // ahead of attach point 1 and is released (back to the animation) once it has settled.
  void head_look_at(const float target_world[3], int rate);
  void head_reset();
  bool head_active() const { return head_.active; }
  bool head_moving() const { return head_.moving; }
  // Current head-turn angles (4096 per turn): ax = yaw about the head bone x, az = nod.
  float head_ax() const { return head_.ax; }
  float head_az() const { return head_.az; }
  int head_joint() const;
  // Shading-entry flags of this model instance (copied from the file's S3 at model init):
  // bit k of hidden_parts = entry k flag bit 0 (construct k not drawn; ops 1B clear / 1C set), bit k of near_parts = flag 0x10 (construct k
  // files its faces at the nearest corner's z instead of the average; op 93 sets / clears it).
  u16 hidden_parts() const { return hidden_parts_; }
  u16 near_parts() const { return near_parts_; }
  void set_anim_slot(int slot, int script_anim) { if (slot >= 0 && slot < kSlotCount) slot_anim_[slot] = script_anim; }
  // SETUP_NPC / D1 of a model without an animation-sequence table: the stand slot and the current id become 0, and clip 0 is
  // started at once (looping, no blend). It stays until the entity walks or a script asks for another clip.
  void stand_on_clip_zero();
  int anim_slot(int slot) const { return slot_anim_[slot]; }

  // Interpolated pose.
  ActorPose pose(float alpha) const;
  // Latest simulated state.
  const ActorPose& current() const { return cur_; }
  int triangle() const { return tri_; }
  int facing() const { return facing_; }
  bool moving() const { return moving_; }
  const std::string& clip_name() const;
  int cur_clip() const { return clip_; }
  // Ends a running clip cross-fade at once (the room-entry cut, FieldWorld::step_visuals).
  void finish_fade() {
    from_clip_ = -1;
    fade_prev_ = fade_cur_ = fade_len_;
  }
  // Route trace hook.
  int fade_source() const { return from_clip_ >= 0 && fade_cur_ < fade_len_ ? (from_clip_ << 12) | (static_cast<int>(from_cur_ * 4.0f + 0.5f) & 0xFFF) : -1; }
  // Keyframe position in the current clip (route trace hook, debug dump).
  float anim_time() const { return t_cur_; }
  size_t clip_count() const { return model_ ? model_->asset.clips.size() : 0; }

  // World matrices of all joints at the interpolated animation time (blended during a cross-fade).
  void palette(float alpha, std::vector<render::Mat4>& out) const;
  // Model -> world: translation * yaw * uniform scale (model units to world units).
  render::Mat4 model_matrix(float alpha) const;
  const render::MeshModel* gpu() const { return model_ ? model_->gpu.get() : nullptr; }
  const assets::ModelAsset* asset() const { return model_ ? &model_->asset : nullptr; }

  // Idle clip: a party member standing still for (rand & 0x3FF) + 0x200 ticks switches to
  // the idle2 clip; from then on every (rand & 0x3FF) + 0x100 ticks the clip is re-picked at random (stand / idle2).
  // Set by FieldWorld for party members each tick; any movement restarts the delay.
  bool auto_idle = false;
  void seed_rng(u32 seed) { rng_ = seed * 2654435761u + 12345u; idle_timer_ = idle_alt_ = rand_(0xFF) + 0xFF; }

  // Animation rate (SET_SELF_10E / 74); negative plays the clip backwards.
  float anim_rate = 1.0f;
  // Yaw step toward the facing per tick (A5 / CF); 0 = no turning.
  int turn_rate = kTurnPerTick;
  // Per-actor override of the model scale.
  float scale = kDefaultScale;
  // Per-axis model scale (op 82 AXIS_SCALE), applied as R * diag(s), i.e. in model space. Multiplies `scale` per model axis; {1,1,1} = uniform.
  float axis_scale[3] = {1.0f, 1.0f, 1.0f};
  // Run speed = 2.5x.
  int speed = kDefaultSpeed;
  // For NPC collision / triggers.
  float radius = kDefaultRadius;
  float height = kDefaultHeight;
  // Which attr walls block this actor (bits 0x200000 / 0x400000 of the entity flags).
  u32 class_bits = 0;
  // Draw the model's 4x texture where it has one (set by FieldWorld per frame).
  bool use_hd = false;
  // Op AD: pitch / roll compose in the world frame, after the yaw (world-map party); else model frame.
  bool tilt_world = false;
  // World-y added to the model translation only (ops B0 / B1); the shadow keeps the floor y.
  float y_offset = 0;
  // Extra model rotation about X / Y / Z (script ops 27 / 28 / 29 / AA), 4096/turn.
  float pitch = 0, local_yaw = 0, roll = 0;
  // ENTITY_RGB colour multiplier (value / 128).
  float tint[3] = {1, 1, 1};

 private:
  struct Local {
    assets::Vec3 rot, trans;
  };
  void set_clip(int clip_index);
  // Restart from keyframe 0; `fade` = the blend word (ticks & 0x1FFF, 0 = cut; 0x8000 = source keeps running).
  void start_clip(int clip_index, int fade);
  // Model init + room start: no sequence, vars 0, the stand id submitted.
  void reset_playback();
  // Submission: the sequence of the id, or the clip itself.
  void submit(int script_anim);
  // One tick of the animation-sequence interpreter.
  void run_seq();
  const std::vector<s16>* seq_entry(int script_anim) const;
  // Op 15 clip id -> clip index, -1 when the model has none (no fallback).
  int clip_for_seq(int clip_id) const;
  int find_clip(int s4_index) const;
  // An idx past the model's clips = the loaded ex pack.
  const assets::AnimationClip& clip_at(int idx) const;
  // -1 when the model has no such clip.
  int clip_for_script(int script_anim) const;
  void sample_local(const assets::AnimationClip& clip, float keyframe, std::vector<Local>& out) const;
  // Animated local pose (cross-fade included) at the interpolated time.
  void locals(float alpha, std::vector<Local>& out) const;
  // Joint matrices from `local`; `head` applies the head-turn rotation to the head joint (interpolated with `alpha`).
  void compose(const std::vector<Local>& local, float alpha, bool head, std::vector<render::Mat4>& out) const;
  // One tick of the head turn.
  void head_step();
  // The head code works in the ENGINE frame: script targets (FE 30..57) are engine world points, and the engine's model matrix is Ry(yaw) x
  // the model rotation, while the renderer's model_matrix() turns the model a further half turn (yaw + 0x800). The engine frame is the render matrix
  // with the x and z rows of its rotation negated (translation unchanged).
  render::Mat4 engine_matrix() const;
  float clip_length(int clip) const;
  void turn_step();
  // The turn and the clip choice of step_visual: moving / requested / stand (idle) / stair slot, submitted when it changes.
  void pick_clip(bool moving, bool run);
  void advance_anim(bool loop_current);
  // Logs joints whose rotation turns > 60 deg between two ticks.
  void pose_trace();
  std::vector<Local> trace_prev_;

  std::shared_ptr<ModelHandle> model_;
  std::vector<assets::AnimationClip> ex_override_;
  assets::AnimSeqTable ex_seq_override_;
  std::string empty_;

  ActorPose prev_, cur_;
  // Display segment of one channel (position x, y, z / yaw).
  struct Chan {
    float from[3]{}, to[3]{}, ref[3]{};
    int span = 1, age = 0, gap = 1000;
  };
  Chan smooth_pos_, smooth_yaw_;
  // end_tick() has run at least once: pose() uses the segments.
  bool smooth_on_ = false;
  // A teleport / placement since the last end_tick(): the next end_tick restarts the segments.
  bool smooth_warp_ = true;
  void reset_smooth() { smooth_warp_ = true; }
  int tri_ = -1;
  // Target facing, 0..0xFFF.
  int facing_ = 0;
  bool moving_ = false;

  int clip_ = -1;
  // Keyframes; t_prev_ may be negative right after a loop wrap.
  float t_prev_ = 0, t_cur_ = 0;
  // Cross-fade source: FROZEN at its switch time unless the blend word had bit 0x8000.
  int from_clip_ = -1;
  // Blend word bit 0x8000: the source keeps playing (looping) during the fade.
  bool from_runs_ = false;
  float from_prev_ = 0, from_cur_ = 0;
  // Elapsed fade ticks.
  int fade_prev_ = kCrossFadeTicks, fade_cur_ = kCrossFadeTicks;
  // Length of the running fade (op 15 blend / 0x20).
  int fade_len_ = kCrossFadeTicks;
  // See set_culled.
  bool culled_ = false;
  // See set_inactive.
  bool inactive_ = false;
  // See pad_step_x.
  float pad_step_[2] = {0.0f, 0.0f};
  // The playing clip loops (picked with it by pick_clip).
  bool clip_loops_ = true;
  // Blend word bits 0x6000: 1 = exponential weight, else linear.
  int fade_curve_ = 0;

  // Animation-sequence interpreter state.
  struct Seq {
    // Script animation id whose entry runs (selects the S5 / ex table).
    int id = -1;
    // Halfword index into the entry, -1 = stopped.
    int pc = -1;
    // Wait counter.
    int counter = 0;
    // Call depth.
    int sp = 0;
    // Return addresses.
    std::array<int, 4> stack{};
  } seq_;
  std::array<s16, kModelVarCount> vars_{};
  // Loop flag (ops 17 / 18); starts at 1.
  bool clip_loop_ = true;
  u16 hidden_parts_ = 0;
  u16 near_parts_ = 0;
  // The id whose clip / sequence was last started.
  int submitted_ = -1;
  // The submitted id runs a sequence (loop flag and anim-done come from it).
  bool seq_mode_ = false;
  bool seq_warned_ = false;

  u32 rng_ = 0x1234567u;
  int rand_(int mask) {
    rng_ = rng_ * 1664525u + 1013904223u;
    return static_cast<int>((rng_ >> 16) & static_cast<u32>(mask));
  }
  // Idle timers, in ticks.
  int idle_timer_ = 0x1FF, idle_alt_ = 0x1FF;
  // Slot of the idle clip in force.
  int idle_pick_ = kSlotIdle2;

  // Head-turn state of the model.
  struct Head {
    // The head-turn callback is installed.
    bool active = false;
    // The direction still changed by >= 0x40 / 4096 on an axis this tick.
    bool moving = false;
    // Set by head_reset; the callback is removed once the head settles.
    bool returning = false;
    // Weight of the current direction per tick (average of cur and target with weights rate and 0x1000 - rate).
    int rate = 0;
    // World point the head turns away from (d = head - target).
    float target[3]{};
    // Unit direction in the frame of (parent composite x animated local) of the head joint.
    float cur[3]{0, 1, 0};
    // RotMatrix({ax, 0, az}) this tick / last tick (render interpolation), 4096 per turn.
    float ax = 0, az = 0, pax = 0, paz = 0;
  } head_;
  std::vector<BoneEmitterRequest> bone_emitters_;
  bool attached_ = false;
  const Actor* attach_parent_ = nullptr;
  int attach_point_ = 0;
  render::Mat4 attach_local_;

  // The move keeps its 3D length on a slope.
  bool slope_step(const WalkMesh& walk, float dx, float dz);
  // Ground tilt matrix (row-major), identity when off.
  float gt_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  bool gt_on_ = false;
  // Walk flags of the current triangle, as last set by tick() / set_tile().
  u32 tile_flags_ = 0;
  // Script animation id in force, -1 = automatic.
  int requested_ = -1;
  bool requested_loops_ = true;
  // A one-shot requested clip has not finished.
  bool busy_ = false;
  // Engine defaults per walk-animation slot.
  int slot_anim_[kSlotCount] = {1, 3, 2, 0, 5, 4, 3, 6, 7};
};

}  // namespace oc::game
