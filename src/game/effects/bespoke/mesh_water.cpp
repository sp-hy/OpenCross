// ripple: the staged ground scene of a hand rising out of a baked patch of the battle floor, and the registrar of the mesh-deform modules.
//
// The pack's meshes `ring` (the patch of floor) and `hand` (the donor of the hand's packets) are baked from the stage ground (mode 2, no
// offset); `han1`, `h2g` and `g00` (the hand's three shapes) take over the packets of `hand` (every packet type: the group's packet list
// is replaced, the vertex arrays stay). The stock ground is off; every frame it is drawn with the hole {-0x800, 0x800, 0x800, -0x800}
// that the ring fills. The scene is a state machine advanced by op 0x182 (state 0 at init):
//   0 han1 at the block (0, 0, 0), bias 100;   1 han1 shaking (rotation r % 0x28 per axis about (0, -0x800, 0)), bias 100;
//   2 lift: while the speed is negative it grows by 0x7000 per tick, y += speed >> 12; han1 morphs toward h2g with weight (t * 0x55) as s16;
//   3 bob: the speed (0xA000) reverses every 0x28 ticks; h2g morphs toward g00 with weight t * 80;
//   4 flight to target 0's bone 0: the block moves by vx / vz per tick (0x28 ticks to the bone, offset (0x200, 0x200)), y follows an arc of
//     0x5DC from the angle 0xB00 (+0x39 per tick), g00 rolls about z by 0x19 per tick;
//   5 fall at 0x190000 >> 12 per tick, g00 turned a quarter (z 0x400); it falls through into 6 when y > 0;
//   6 g00 rests at (x, 0x400, z).
// Every mesh is drawn with the stage ground's colour. t counts the ticks since the last op 0x182. From state 2 the ring is drawn at the origin (bias 0x8C), the hand with bias 0 (100 for han1).
// Op 0x180 (side = s16 @+8, height = s16 @+10) sets a side x side square of `height` into the 33 x 33 height field at cell (12, 12), clipped
// to the field (rows 12..31, columns 2..31) and switches the field on; 0x181 clears both buffers and switches it off. While on, the field
// takes one step per tick (interior cells: n = ((N + S + W + E) >> 1) - old, stored n - (n >> 3)). Exit: stock ground on.
// The field's picture: while the field is on, the frame is copied (FxModels::capture) and redrawn as a 32 x 32 grid of quads (10 x 7 px
// cells over -160..160 x -112..112) textured with the same cells of the copy, whose corners are projected with the field height as depth
// and shaded 0x80 - (height >> 4).
#include <algorithm>
#include <array>

#include "game/effects/bespoke/ground_base.h"
#include "game/effects/bespoke/mesh_base.h"

namespace oc::effects {
namespace {

using namespace ground_detail;

constexpr int kFieldSide = 33;
constexpr int kStampOrigin = 12;
constexpr int kHandPivotY = -0x800;
// The warp grid: 32 x 32 cells of 10 x 7 px over the 320 x 224 picture; the frame copy goes to VRAM (kCopyX, kCopyY) at ordering-table
// entry kCopySlot and the grid is filed just nearer.
constexpr int kGridCells = 32, kCellW = 10, kCellH = 7;
constexpr int kGridW = kGridCells * kCellW, kGridH = kGridCells * kCellH;
constexpr int kCopyX = 0x1C0, kCopyY = 0x100, kCopySlot = 117, kGridSlot = 114;

// The two int buffers of the height field. `newest` is the buffer a stamp writes and a step reads, `oldest` the one a step writes (the
// two buffers alternate from the first step); the two swap after every step.
struct HeightField {
  std::array<std::array<int, kFieldSide * kFieldSide>, 2> buffer{};
  int newest = 0, oldest = 1;
  bool active = false;

  void stamp(int side, int height) {
    auto& field = buffer[static_cast<size_t>(newest)];
    for (int row = 0; row < side && row < 20; ++row)
      for (int col = 0; col < side; ++col) {
        const int cell_col = col + kStampOrigin;
        if (cell_col > 1 && cell_col < kFieldSide - 1) field[static_cast<size_t>((row + kStampOrigin) * kFieldSide + cell_col)] = height;
      }
    active = true;
  }
  void clear() {
    for (auto& field : buffer) field.fill(0);
    active = false;
  }
  void step() {
    const auto& from = buffer[static_cast<size_t>(newest)];
    auto& into = buffer[static_cast<size_t>(oldest)];
    for (int row = 1; row < kFieldSide - 1; ++row)
      for (int col = 1; col < kFieldSide - 1; ++col) {
        const size_t cell = static_cast<size_t>(row * kFieldSide + col);
        const int sum = from[cell - kFieldSide] + from[cell + kFieldSide] + from[cell - 1] + from[cell + 1];
        const int relaxed = (sum >> 1) - into[cell];
        into[cell] = relaxed - (relaxed >> 3);
      }
    if (newest == 0) oldest = 0, newest = 1;
    else newest = 0, oldest = 1;
  }
};

class Ripple : public GroundFx {
 protected:
  void init() override {
    seed_rng();
    load_meshes();
    set_skip(true);
    ring_ = mesh("ring");
    bake_mesh(ring_, 2, 0, 0);
    PackMesh* donor = mesh("hand");
    if (bake_mesh(donor, 2, 0, 0))
      for (PackMesh* shape : {mesh("han1"), mesh("h2g"), mesh("g00")}) share_packets(*donor, shape);
    rising_ = mesh("han1");
    bobbing_ = mesh("h2g");
    flying_ = mesh("g00");
  }

  bool command(int op, const int* args, int arg_count) override {
    if (op == 0x180) field_.stamp(static_cast<s16>(arg(args, arg_count, 8)), static_cast<s16>(arg(args, arg_count, 10)));
    else if (op == 0x181) field_.clear();
    else if (op == 0x182) advance();
    else return false;
    return true;
  }

  void frame() override {
    ++clock_;
    ++total_;
    if (const FxGround* stage_ground = fx_ground())
      for (int channel = 0; channel < 3; ++channel) ground_rgb_[channel] = stage_ground->rgb[channel];
    const std::array<int, 3> pivot{0, kHandPivotY, 0};
    int rot[3] = {0, 0, 0};
    Vec3 hand_at{0, 0, 0}, ring_at{0, 0, 0};
    const PackMesh* hand = nullptr;
    const PackMesh* morph_to = nullptr;
    int hand_bias = 0, ring_bias = 0x8C, weight = 0;
    switch (state_) {
      case 0:
      case 1:
        if (state_ == 1)
          for (int& angle : rot) angle = rand() % 0x28;
        hand = rising_;
        hand_bias = ring_bias = 100;
        hand_at = ring_at = {static_cast<float>(x_), static_cast<float>(y_), static_cast<float>(z_)};
        break;
      case 2:
        if (lift_ < 0) lift_ += lift_accel_;
        y_ = s16c(y_ + (lift_ >> 12));
        hand = rising_;
        hand_bias = 100;
        morph_to = bobbing_;
        weight = s16c(total_ * 0x55);
        hand_at = {static_cast<float>(x_), static_cast<float>(y_ + 0x400), static_cast<float>(z_)};
        break;
      case 3:
        if (clock_ > 0x28) clock_ = 0, lift_ = -lift_;
        y_ = s16c(y_ + (lift_ >> 12));
        hand = bobbing_;
        morph_to = flying_;
        weight = static_cast<int>(static_cast<u32>(total_) * 0x500000u) >> 16;
        hand_at = {static_cast<float>(x_), static_cast<float>(y_ + 0x400), static_cast<float>(z_)};
        break;
      case 4:
        rot[2] = s16c(0x19 * s16c(clock_));
        lift_ += arc_step_;
        x_ = s16c(x_ + (vx_ >> 12));
        z_ = s16c(z_ + (vz_ >> 12));
        hand = flying_;
        hand_at = {static_cast<float>(x_), static_cast<float>(y_ + ((rsin(lift_) * -0x5DC) >> 12) + 0x400), static_cast<float>(z_)};
        break;
      case 5:
        // The fall is drawn, then the resting pose of state 6 follows in the same frame.
        y_ = s16c(y_ + (lift_ >> 12));
        if (y_ > 0) advance();
        rot[2] = 0x400;
        draw_hand(flying_, nullptr, 0, rot, pivot, {static_cast<float>(x_), static_cast<float>(y_ + 0x400), static_cast<float>(z_)}, 0);
        draw_ring(ring_at, ring_bias);
        [[fallthrough]];
      default:
        y_ = 0x400;
        rot[2] = 0x400;
        hand = flying_;
        hand_at = {static_cast<float>(x_), 0x400, static_cast<float>(z_)};
        break;
    }
    draw_hand(hand, morph_to, weight, rot, pivot, hand_at, hand_bias);
    draw_ring(ring_at, ring_bias);
    draw_ground(-0x800, 0x800, 0x800, -0x800);
    if (field_.active) field_.step();
    if (ctx_.models) ctx_.models->capture = field_.active ? FxModels::Capture{kCopyX, kCopyY, kCopySlot} : FxModels::Capture{};
  }

  // The 32 x 32 grid of the field: every cell is a quad textured with the same cell of the frame copy, its corners moved toward (or away
  // from) the screen centre by the field height as depth, and shaded darker for a positive height, brighter for a negative one.
  void draw(std::vector<FxPrim>& out) const override {
    if (!field_.active) return;
    const auto& heights = field_.buffer[static_cast<size_t>(field_.oldest)];
    const ViewParams& view = ctx_.view;
    u32 order = 0;
    for (int row = 0; row < kGridCells; ++row)
      for (int col = 0; col < kGridCells; ++col) {
        FxPrim prim;
        prim.kind = FxPrim::Kind::Quad;
        prim.src = FxPrim::TexSrc::Capture;
        prim.slot = kGridSlot;
        prim.order = order++;
        for (int corner = 0; corner < 4; ++corner) {
          const int cell_col = col + (corner & 1), cell_row = row + (corner >> 1);
          const int height = static_cast<s16>(heights[static_cast<size_t>(cell_col * kFieldSide + cell_row)]);
          const float depth = view.h + static_cast<float>(height);
          const float scale = depth > view.h * 0.5f ? view.h / depth : 2.0f;
          const float grid_x = static_cast<float>(cell_col * kCellW - kGridW / 2), grid_y = static_cast<float>(cell_row * kCellH - kGridH / 2);
          const u8 shade = static_cast<u8>(0x80 - static_cast<s8>(height >> 4));
          prim.v[corner] = {view.ox + grid_x * scale, view.oy + grid_y * scale, shade, shade, shade};
          prim.u[corner] = static_cast<float>(cell_col * kCellW);
          prim.w[corner] = static_cast<float>(cell_row * kCellH);
        }
        out.push_back(prim);
      }
  }

 private:
  // The shapes draw the packets of the donor: per packet type the mesh's list is replaced by the donor's (as many as the mesh has).
  static void share_packets(const PackMesh& donor, PackMesh* shape) {
    if (!shape) return;
    for (int code : {0x24, 0x2C, 0x34, 0x3C}) {
      size_t next = 0;
      for (PackPrim& prim : shape->prims) {
        if (prim.code != code) continue;
        while (next < donor.prims.size() && donor.prims[next].code != code) ++next;
        if (next >= donor.prims.size()) break;
        const PackPrim& source = donor.prims[next++];
        const bool in_range = std::all_of(source.idx.begin(), source.idx.begin() + source.corners, [&](u16 index) { return index < shape->verts.size(); });
        if (in_range) prim = source;
      }
    }
  }

  // Op 0x182: the next state, the state timers restart; states 1 to 4 set up the motion of the state that follows.
  void advance() {
    total_ = 0;
    const int previous = state_;
    ++state_;
    switch (previous) {
      case 1:
        vx_ = vz_ = 0;
        lift_ = -0xC8000;
        lift_accel_ = 0x7000;
        break;
      case 2:
        vx_ = vz_ = 0;
        lift_ = 0xA000;
        break;
      case 3:
        if (const FxModel* target_model = target(0)) {
          // Bone 0 of the target's posed model; the waist stands in for an unposed model.
          Vec3 bone{target_model->pos[0], target_model->pos[1] - target_model->height * 0.5f, target_model->pos[2]};
          joint_world(target_model, 0, bone);
          vx_ = trunc_div((x_ - static_cast<int>(bone[0]) - 0x200) * -0x1000, 0x28);
          vz_ = trunc_div((z_ - static_cast<int>(bone[2]) + 0x200) * -0x1000, 0x28);
          arc_step_ = 0x39;
          lift_ = 0xB00;
          y_ = s16c(y_ + ((rsin(0xB00) * 0x5DC) >> 12));
        }
        break;
      case 4:
        y_ = s16c(y_ + ((rsin(lift_) * -0x5DC) >> 12));
        lift_ = 0x190000;
        arc_step_ = 600;
        break;
      default: break;
    }
    clock_ = 0;
  }

  // The hand mesh `hand` at `offset`, turned by `rot` about `pivot`, in the ground colour; a morph draw (weight toward `morph_to`) forces
  // opaque packets.
  void draw_hand(const PackMesh* hand, const PackMesh* morph_to, int weight, const int rot[3], const std::array<int, 3>& pivot, const Vec3& offset,
                 int bias) {
    if (!hand) return;
    MeshDraw draw;
    draw.centre = pivot;
    for (int k = 0; k < 3; ++k) {
      draw.rot[k] = rot[k];
      draw.rgb[k] = ground_rgb_[k];
    }
    draw.offset = offset;
    draw.bias = bias;
    if (morph_to) {
      draw.morph = morph_to;
      draw.morph_w = weight;
      draw.opaque = true;
    }
    draw_mesh(*hand, draw);
  }

  void draw_ring(const Vec3& offset, int bias) {
    if (!ring_) return;
    MeshDraw draw;
    draw.offset = offset;
    draw.bias = bias;
    for (int k = 0; k < 3; ++k) draw.rgb[k] = ground_rgb_[k];
    draw_mesh(*ring_, draw);
  }

  PackMesh *ring_ = nullptr, *rising_ = nullptr, *bobbing_ = nullptr, *flying_ = nullptr;
  HeightField field_;
  u8 ground_rgb_[3] = {128, 128, 128};
  int state_ = 0, clock_ = 0, total_ = 0;
  int x_ = 0, y_ = 0, z_ = 0;
  int vx_ = 0, vz_ = 0, lift_ = 0, lift_accel_ = 0, arc_step_ = 0;
};

}  // namespace

void register_mesh_water(BespokeRegistry& registry) {
  registry.add({"ripple", "ground", "Ripple", [] { return std::make_unique<Ripple>(); }});
}

void register_mesh_deforms(BespokeRegistry& registry) {
  register_mesh_sweeps(registry);
  register_mesh_flights(registry);
  register_mesh_water(registry);
}

}  // namespace oc::effects
