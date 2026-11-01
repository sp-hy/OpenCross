// Screen-space overlays.
//   muteki   lens flare: op 0x180 tasks draw the 'renz' sheet frames along the line through the screen centre and a full-screen additive
//            tint falling off with the distance to the centre
//   155slot  the slot machine: three reel tasks scroll VRAM windows by DR_MOVEs; the confirm button stops them
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <string>

#include "game/effects/bespoke/mesh_tech.h"
#include "game/effects/fx_math.h"
#include "import/battle_stage.h"
#include "import/container.h"

namespace oc::effects {
namespace {

// ---- muteki ---------------------------------------------------------------------------------------------------------------------------
// Init: caster 0 plays entry 0 of the pack's external anmseq table. 0x180 (life @+4, x @+8, y @+0xA, vx @+0xC, vy @+0xE, colour = the low
// bytes of @+0x10 / +0x12 / +0x14): a task; each tick age += 1, the 'renz' sheet (type 0x15) is drawn at (x, y), then (x, y) += (vx, vy);
// freed when life < age (after that tick's draw: life + 1 draws). Draw: only for 0 <= x <= 0x140, 0 <= y <= 0xE0; d = (0x140 - 2x, 0xE0 - 2y)
// (to the point mirrored through the centre (160, 112)); frame k = 1..N of the sheet at p, p += d 0x1000 / N (20.12, truncated) - its cells
// as FT4 (or SPRT) with flip-x / flip-y (the corners swapped and u / v - 1), colour 0x808080, ABR 1 additive, OT + 1. Then, for a non-zero
// colour, a full-screen F4 at OT + 0, additive, colour c' = c ? f / (0x25400 / c) : 0 with f = 0x25400 - |d|^2 (0x25400 = 320^2 + 224^2).
class Muteki : public MeshTech {
 protected:
  void init() override { start_caster_anim(caster(0)); }
  bool command(int op, const int* args, int count) override {
    if (op != 0x180) return false;
    Flare flare;
    flare.x = arg(args, count, 8), flare.y = arg(args, count, 0xA), flare.vx = arg(args, count, 0xC), flare.vy = arg(args, count, 0xE);
    flare.life = arg(args, count, 4);
    flare.rgb = {arg(args, count, 0x10) & 0xFF, arg(args, count, 0x12) & 0xFF, arg(args, count, 0x14) & 0xFF};
    flares_.push_back(flare);
    return true;
  }
  void frame() override {
    shown_.clear();
    for (Flare& flare : flares_) {
      ++flare.age;
      shown_.push_back({flare.x, flare.y, flare.rgb});
      flare.x = static_cast<s16>(flare.x + flare.vx), flare.y = static_cast<s16>(flare.y + flare.vy);
    }
    flares_.erase(std::remove_if(flares_.begin(), flares_.end(), [](const Flare& flare) { return flare.life < flare.age; }), flares_.end());
  }
  void draw(std::vector<FxPrim>& out) const override {
    const size_t first = out.size();
    u32 order = 0;
    const assets::SpriteSheet* sheet = ctx_.pack_fx ? ctx_.pack_fx->find_sheet("renz") : nullptr;
    for (const Shown& shown : shown_) {
      if (!sheet || shown.x < 0 || shown.x >= 0x141 || shown.y < 0 || shown.y >= 0xE1) continue;
      const int dx = 0x140 - 2 * shown.x, dy = 0xE0 - 2 * shown.y, frame_count = static_cast<int>(sheet->frames.size());
      int x_fixed = shown.x << 12, y_fixed = shown.y << 12;
      for (int k = 1; k <= frame_count; ++k) {
        for (const assets::EffectSprite& sprite : sheet->frames[static_cast<size_t>(k - 1)]) cell(sprite, x_fixed >> 12, y_fixed >> 12, out, order);
        y_fixed += (dy * 0x1000) / frame_count;
        x_fixed += (dx * 0x1000) / frame_count;
      }
      if (shown.rgb[0] | shown.rgb[1] | shown.rgb[2]) {
        const int falloff = 0x25400 - (dx * dx + dy * dy);
        u8 rgb[3];
        for (int k = 0; k < 3; ++k)
          rgb[k] = static_cast<u8>(shown.rgb[static_cast<size_t>(k)] ? falloff / (0x25400 / shown.rgb[static_cast<size_t>(k)]) : 0);
        FxPrim prim;
        prim.kind = FxPrim::Kind::Quad;
        const float xs[4] = {0, 320, 0, 320}, ys[4] = {0, 0, 224, 224};
        for (int k = 0; k < 4; ++k) prim.v[k] = {xs[k], ys[k], rgb[0], rgb[1], rgb[2]};
        prim.blend = 2;
        prim.slot = 0;
        prim.order = order++;
        out.push_back(prim);
      }
    }
    last_prims_ = out.size() - first;
  }

 private:
  struct Flare {
    int x = 0, y = 0, vx = 0, vy = 0, age = 0, life = 0;
    std::array<int, 3> rgb{};
  };
  struct Shown {
    int x, y;
    std::array<int, 3> rgb;
  };
  // One cell: FT4 (SPRT kind: no flips), additive, colour 0x808080, OT + 1.
  static void cell(const assets::EffectSprite& sprite, int sx, int sy, std::vector<FxPrim>& out, u32& order) {
    float x0 = static_cast<float>(sx + sprite.x), x1 = x0 + sprite.w, y0 = static_cast<float>(sy + sprite.y), y1 = y0 + sprite.h;
    float u0 = sprite.u, u1 = static_cast<float>(sprite.u + sprite.w), v0 = sprite.v, v1 = static_cast<float>(sprite.v + sprite.h);
    const bool sprt = (sprite.flags >> 8) & 8;
    if (!sprt && sprite.mirror_x()) std::swap(x0, x1), u0 -= 1, u1 -= 1;
    if (!sprt && sprite.mirror_y()) std::swap(y0, y1), v0 -= 1, v1 -= 1;
    FxPrim prim;
    prim.kind = FxPrim::Kind::Quad;
    prim.src = FxPrim::TexSrc::Vram;
    prim.tpage = static_cast<u16>(sprite.flags & 0x19F);
    prim.clut = sprite.clut;
    prim.blend = 2;
    prim.slot = 1;
    const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1}, us[4] = {u0, u1, u0, u1}, ws[4] = {v0, v0, v1, v1};
    for (int k = 0; k < 4; ++k) {
      prim.v[k] = {xs[k], ys[k], 0x80, 0x80, 0x80};
      prim.u[k] = us[k];
      prim.w[k] = ws[k];
    }
    prim.order = order++;
    out.push_back(prim);
  }

  std::vector<Flare> flares_;
  std::vector<Shown> shown_;
};

// ---- 155slot --------------------------------------------------------------------------------------------------------------------------
// Init: sequence 'init' started, three reels made and stepped once: reel i shows the VRAM window (0x1C0 + 0x20 i, 0x100) 0x20 x 0xC0
// halfwords; speed 6.0 lines per tick (0x00060000, 16.16), pos 0. A step at line pos: idx = (pos >> 6) % 21, symbol s = reel i's order[idx],
// row = pos & 63; DR_MOVE the window up one line (src (x, 0x101) 0x20 x 0xBF -> (x, 0x100)), then DR_MOVE row `row` of symbol s's 0x20 x 0x40
// image to the bottom line (x, 0x1BF); pos += 1. Spinning: acc += speed per tick, one step per whole line passed. 0x100 enables the buttons;
// each new press of the confirm button switches the next reel (left to right) to the stopping task: speed -= 0x600 per tick down to 1.0,
// freed when pos % 64 == 0 at 1.0. The third press starts 'main'. 0x101 restarts 'dr@1' 'dr@2' ('dr@0' is not in the pack: entry 5 is 'dr`0')
// and kills 'dra0'..'dra2'. The retail exit reports the three symbols to the battle script (not tracked by OpenCross). The reels are seen
// through the pack meshes drm1..3 (emitters dra0..2 while they spin, dr@1 / dr@2 / dr`0 after): the stock mesh draw samples the live VRAM,
// where the host cuts a pack mesh's textures once, so the overlay draws those emitters' objects itself: each packet projected through the
// object's place (the NCLIP > 0 faces) as a TexSrc::Vram primitive with the packet's own tpage / CLUT word and texels, so the scrolling
// window shows.
class Slot155 : public MeshTech {
 protected:
  void init() override {
    load_drums();
    if (ctx_.models) ctx_.models->suppress_main = true;
    if (ctx_.sequences) ctx_.sequences->start("init");
    for (int i = 0; i < 3; ++i) {
      reels_[static_cast<size_t>(i)].index = i;
      advance(reels_[static_cast<size_t>(i)]);
    }
  }
  bool command(int op, const int* args, int count) override {
    (void)args;
    (void)count;
    if (op == 0x100) {
      enabled_ = true;
      return true;
    }
    if (op != 0x101) return false;
    if (em()) {
      for (const char* name : {"dr@0", "dr@1", "dr@2"}) em()->start(name);
      for (const char* name : {"dra0", "dra1", "dra2"}) em()->kill(name);
    }
    return true;
  }
  void frame() override {
    for (Reel& reel : reels_)
      if (reel.state == 1) advance(reel);
      else if (reel.state == 2) stopping(reel);
    const bool press = ctx_.models && (ctx_.models->pad_edge & 0x20);
    if (enabled_ && stops_ < 3 && (press || (ctx_.auto_input && ++auto_t_ % 60 == 0))) {
      reels_[static_cast<size_t>(stops_)].state = 2;
      if (++stops_ == 3 && ctx_.sequences) ctx_.sequences->start_main();
    }
  }
  bool done() const override { return stops_ >= 3 && std::all_of(reels_.begin(), reels_.end(), [](const Reel& reel) { return reel.state == 0; }); }
  void draw(std::vector<FxPrim>& out) const override {
    if (!em() || drums_.empty()) return;
    std::vector<EmitterSystem::MeshInstance> instances;
    em()->collect_meshes(ctx_.view, 1.0f, instances, true);
    u32 order = 0;
    const size_t first = out.size();
    for (const auto& instance : instances) {
      if (instance.sprite_type != 14 || !instance.name || instance.owner < 0 || !own_header(static_cast<size_t>(instance.owner))) continue;
      const auto it = drums_.find(*instance.name);
      if (it == drums_.end()) continue;
      const Drum& drum = it->second;
      for (const import::StageFace& face : drum.faces) {
        if (!face.textured) continue;
        FxPrim prim;
        prim.kind = face.n == 3 ? FxPrim::Kind::Tri : FxPrim::Kind::Quad;
        prim.src = FxPrim::TexSrc::Vram;
        prim.tpage = face.tpage;
        prim.clut = face.clut;
        float depth_sum = 0;
        bool ok = true;
        float sx[4], sy[4];
        for (int k = 0; k < face.n && ok; ++k) {
          const auto& vertex = drum.verts[face.idx[static_cast<size_t>(k)]];
          Vec3 camera = instance.mat.apply({static_cast<float>(vertex[0]), static_cast<float>(vertex[1]), static_cast<float>(vertex[2])});
          for (size_t axis = 0; axis < 3; ++axis) camera[axis] += instance.centre[axis];
          if (camera[2] < 32.0f) {
            ok = false;
            break;
          }
          sx[k] = ctx_.view.ox + ctx_.view.h * camera[0] / camera[2] + ctx_.view.scroll_x;
          sy[k] = ctx_.view.oy + ctx_.view.h * camera[1] / camera[2] + ctx_.view.scroll_y;
          if (face.n == 3 || k > 0) depth_sum += camera[2];
          // Gouraud packets keep a colour per corner, the others repeat colour 0.
          const int colour_corner = (face.code & 0x10) ? k : 0;
          for (int ch = 0; ch < 3; ++ch) {
            const int rgb = face.rgb[static_cast<size_t>(colour_corner)][static_cast<size_t>(ch)] * instance.rgb[ch] / 128;
            (ch == 0 ? prim.v[k].r : ch == 1 ? prim.v[k].g : prim.v[k].b) = static_cast<u8>(std::min(rgb, 255));
          }
          prim.v[k].x = sx[k];
          prim.v[k].y = sy[k];
          prim.u[k] = face.uv[static_cast<size_t>(k)][0];
          prim.w[k] = face.uv[static_cast<size_t>(k)][1];
        }
        if (!ok) continue;
        auto nclip = [&](int i0, int i1, int i2) { return (sx[i1] - sx[i0]) * (sy[i2] - sy[i0]) - (sx[i2] - sx[i0]) * (sy[i1] - sy[i0]); };
        if (face.n == 3 ? !(nclip(0, 1, 2) > 0.0f) : !(nclip(0, 1, 2) > 0.0f || nclip(1, 2, 3) < 0.0f)) continue;
        const int blend = instance.blend >= 5 ? 2 : instance.blend;
        prim.blend = blend != 0 ? blend : (face.semi ? ((face.tpage >> 5) & 3) + 1 : 0);
        prim.slot = std::clamp(fx_slot_of_depth(depth_sum / 3.0f) + instance.bias, 1, 0xFFF);
        prim.order = order++;
        out.push_back(prim);
      }
    }
    last_prims_ = out.size() - first;
  }

 private:
  struct Drum {
    std::vector<std::array<s16, 3>> verts;
    std::vector<import::StageFace> faces;
  };
  void load_drums() {
    if (ctx_.pack_prd.empty()) return;
    auto drp = import::parse_drp(ctx_.pack_prd);
    if (!drp) return;
    for (const auto& entry : drp->entries) {
      if (entry.type != 0x02 || entry.name.rfind("drm", 0) != 0 || drums_.count(entry.name)) continue;
      if (auto mesh = import::parse_stage_mesh(entry.data)) drums_[entry.name] = Drum{mesh->verts, mesh->faces};
    }
    if (!em() || drums_.empty()) return;
    for (size_t i = 0; i < em()->header_count(); ++i) {
      if (!own_header(i)) continue;
      for (const auto& op : em()->header_def(i).ops)
        if (op.op == 0x05)
          if (auto* sprite = std::get_if<assets::FxSprite>(&op.arg); sprite && sprite->type == 14 && drums_.count(sprite->name))
            em()->set_draw_suppressed(em()->header_def(i).name, true);
    }
  }
  std::map<std::string, Drum> drums_;
  struct Reel {
    int index = 0, state = 1, pos = 0;
    u32 speed = 0x00060000u, acc = 0;
  };
  // One line of scroll: the window moves up one row and row (pos & 63) of the symbol at reel position pos >> 6 fills the bottom line.
  void step(Reel& reel) {
    static constexpr u8 kOrder[3][21] = {{0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6},
                                         {3, 4, 5, 6, 0, 1, 2, 0, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6},
                                         {6, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6}};
    static constexpr int kSym[7][2] = {{0x220, 0x100}, {0x220, 0x140}, {0x220, 0x180}, {0x220, 0x1C0}, {0x200, 0x1C0}, {0x1E0, 0x1C0}, {0x1C0, 0x1C0}};
    const int pos = static_cast<s16>(reel.pos);
    const int idx64 = (pos < 0 ? pos + 63 : pos) >> 6;
    const int idx = static_cast<s16>(idx64 % 21);
    const int sym = kOrder[reel.index][std::clamp(idx, 0, 20)];
    const int row = pos - (idx64 << 6), dx = 0x1C0 + 0x20 * reel.index, dy = 0x100;
    if (ctx_.models) {
      ctx_.models->vram_moves.push_back({dx, dy + 1, 0x20, 0xBF, dx, dy, false});
      ctx_.models->vram_moves.push_back({kSym[sym][0], kSym[sym][1] + row, 0x20, 1, dx, dy + 0xBF, false});
    }
    reel.pos = static_cast<s16>(reel.pos + 1);
  }
  void advance(Reel& reel) {
    const u32 old = reel.acc >> 16;
    reel.acc += reel.speed;
    for (u32 j = 0; j < (reel.acc >> 16) - old; ++j) step(reel);
  }
  void stopping(Reel& reel) {
    reel.speed -= 0x600;
    if (reel.speed < 0x10000u) reel.speed = 0x10000u;
    advance(reel);
    const int line_in_symbol = static_cast<s16>(reel.pos - ((static_cast<s16>(reel.pos) >> 6) << 6));
    if ((line_in_symbol > 0x40 - 1 || line_in_symbol < 1) && reel.speed == 0x10000u) reel.state = 0;
  }

  std::array<Reel, 3> reels_{};
  bool enabled_ = false;
  int stops_ = 0, auto_t_ = 0;
};

}  // namespace

void register_screen_ports(BespokeRegistry& registry) {
  registry.add({"muteki", "band-wipe", "Muteki", [] { return std::make_unique<Muteki>(); }});
  registry.add({"155slot", "band-wipe", "Slot155", [] { return std::make_unique<Slot155>(); }});
}

}  // namespace oc::effects
