#include "game/effects/emitter_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace oc::effects {

using namespace emitter_detail;

// -- spawn ---------------------------------------------------------------------------------------------
// Returns the object index (objs.back()) or -1 when refused. `parent` = parent frame for child emission.
int EmitterSystem::Impl::spawn(size_t hi, const Basis* parent) {
  if (hi >= headers.size()) return -1;
  if (objs.size() >= cfg.max_objects) {
    ++st.dropped;
    return -1;
  }
  Header& header = headers[hi];
  const assets::Emitter& emitter = *header.def;
  Object obj;
  obj.owner = static_cast<int>(hi);
  const Basis* par = parent;
  // local_par = op 39 result, anchor_par = op 30 result.
  Basis local_par;
  Basis anchor_par;
  int actor_scale = 0;
  bool sprite_set = false;
  auto rotv = [&](int vec[3], int out[3]) {
    if (par) rot_int(*par, vec, out);
    else for (int i = 0; i < 3; ++i) out[i] = vec[i];
  };
  auto scale_by_actor = [&](int value) { return actor_scale ? (value * actor_scale) >> 12 : value; };
  auto xyz3 = [](const assets::FxXyz& xyz, int vec[3]) {
    vec[0] = xyz.x;
    vec[1] = xyz.y;
    vec[2] = xyz.z;
  };
  const s16 hframe = header.frame;

  for (const auto& op : emitter.ops) {
    switch (op.op) {
      case 0x03:
        if (auto* value = std::get_if<assets::FxValue>(&op.arg)) obj.bias = static_cast<s16>(value->value);
        break;
      case 0x04:
        if (auto* value = std::get_if<assets::FxValue>(&op.arg)) obj.anim_ctr = obj.anim_per = static_cast<u8>(value->value);
        break;
      case 0x05:
        if (auto* sprite = std::get_if<assets::FxSprite>(&op.arg)) {
          obj.frame = static_cast<s16>(sprite->frame0);
          obj.stype = sprite->type;
          sprite_set = true;
          // 21: stage lens flare sheet (`lenz`), drawn like type 3.
          if (sprite->type == 3 || sprite->type == 21) {
            obj.sheet = bank.sheet(sprite->name);
            if (!obj.sheet) ++st.unresolved_sprites;
          // 19: the battle stage ground mesh (type 0x12 `ring`); 20: type-0x0B model.
          } else if (sprite->type == 14 || sprite->type == 19 || sprite->type == 20) {
            ++st.mesh_objects;
            obj.mesh_name = &sprite->name;
          } else if (sprite->type == 15) {
            obj.builtin = sprite->name == "flat" ? 1 : sprite->name == "line" ? 2 : 0;
          // A light: no primitive, writes a GTE light slot each tick (the slot is chosen below).
          } else if (sprite->type == 17) {
            obj.light_name = &sprite->name;
          } else if (sprite->type == 0x16 && (obj.flags & 7) > 4 && hooks.akao) {
            hooks.akao(sprite->name);
          }
        }
        break;
      case 0x06:
        if (auto* flags = std::get_if<assets::FxFlags>(&op.arg)) {
          obj.flags = flags->flags;
          if (obj.flags & kFlagFloor) {
            obj.rest = 0xFF;
            obj.floor_y = 0;
          }
        }
        break;
      case 0x08:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
          int vec[3];
          xyz3(*xyz, vec);
          if (const auto* pv = header.patched(0x08))
            for (int i = 0; i < 3; ++i) vec[i] = (*pv)[static_cast<size_t>(i)];
          if (par) {
            if (!(obj.flags & kFlagRelParent)) {
              int rotated[3];
              rot_int(*par, vec, rotated);
              for (int i = 0; i < 3; ++i) vec[i] = rotated[i] + static_cast<int>(par->t[static_cast<size_t>(i)]);
            }
          }
          for (int i = 0; i < 3; ++i) {
            obj.pos[i] = shl32(static_cast<s16>(vec[i]), 16);
            origin[i] = obj.pos[i];
          }
        }
        break;
      case 0x09:
        if (auto* osc = std::get_if<assets::FxOsc>(&op.arg)) {
          int vec[3], rotated[3];
          for (int i = 0; i < 3; ++i) vec[i] = osc_value(osc->axis[static_cast<size_t>(i)], hframe);
          rotv(vec, rotated);
          for (int i = 0; i < 3; ++i) obj.pos[i] = add32(obj.pos[i], rotated[i]);
        }
        break;
      case 0x0A:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
          int vec[3], rotated[3];
          xyz3(*xyz, vec);
          if (const auto* pv = header.patched(0x0A))
            for (int i = 0; i < 3; ++i) vec[i] = (*pv)[static_cast<size_t>(i)];
          rotv(vec, rotated);
          for (int i = 0; i < 3; ++i) obj.vel[i] = shl32(rotated[i], 6);
        }
        break;
      case 0x0B:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
          int vec[3], rotated[3];
          xyz3(*xyz, vec);
          rotv(vec, rotated);
          for (int i = 0; i < 3; ++i)
            if (rotated[i] != 0) obj.vel[i] = add32(obj.vel[i], (rand16() * rotated[i]) >> 10);
        }
        break;
      case 0x0C:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
          int vec[3], rotated[3];
          vec[0] = xyz->x * 8;
          vec[1] = xyz->y * 8;
          vec[2] = xyz->z * 8;
          if (!par) for (int i = 0; i < 3; ++i) rotated[i] = vec[i];
          else {
            for (int i = 0; i < 3; ++i) vec[i] = trunc16(vec[i]);
            rot_int(*par, vec, rotated);
            for (int i = 0; i < 3; ++i) rotated[i] = trunc16(rotated[i]);
          }
          for (int i = 0; i < 3; ++i) obj.acc[i] = rotated[i];
        }
        break;
      case 0x0D:
        if (auto* osc = std::get_if<assets::FxOsc>(&op.arg))
          for (int i = 0; i < 3; ++i) obj.acc[i] = add32(obj.acc[i], osc_value(osc->axis[static_cast<size_t>(i)], hframe) >> 13);
        break;
      case 0x0E:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
          const int vec[3] = {xyz->x, xyz->y, xyz->z};
          for (int i = 0; i < 3; ++i)
            if (vec[i] != 0) obj.acc[i] = add32(obj.acc[i], (rand16() * vec[i]) >> 13);
        }
        break;
      case 0x0F:
        if (auto* value = std::get_if<assets::FxValue>(&op.arg)) obj.life = static_cast<u16>(value->value);
        break;
      case 0x10:
        if (auto* value = std::get_if<assets::FxValue>(&op.arg)) obj.life = static_cast<u16>(obj.life + static_cast<u16>(value->value));
        break;
      case 0x11:
        if (obj.life != 0)
          if (auto* osc = std::get_if<assets::FxOsc>(&op.arg)) {
            const int nv = static_cast<int>(obj.life) + (osc_value(osc->axis[0], hframe) >> 16);
            obj.life = static_cast<u16>(nv);
            if (static_cast<s32>(static_cast<u32>(nv) << 16) < 1) obj.life = 0xFFFF;
          }
        break;
      case 0x12:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
          const int vec[3] = {xyz->x, xyz->y, xyz->z};
          for (int i = 0; i < 3; ++i) obj.scale[i] = static_cast<s16>(actor_scale ? (static_cast<s16>(vec[i]) * actor_scale) >> 12 : vec[i]);
        }
        break;
      case 0x13:
      case 0x18:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
          const int vec[3] = {xyz->x, xyz->y, xyz->z};
          int delta[3];
          for (int i = 0; i < 3; ++i) delta[i] = vec[i] == 0 ? 0 : trunc16((rand16() * vec[i]) >> 16);
          if (actor_scale)
            for (int i = 0; i < 3; ++i) delta[i] = trunc16((delta[i] * actor_scale) >> 12);
          s16* tgt = op.op == 0x13 ? obj.scale : obj.srate;
          for (int i = 0; i < 3; ++i) tgt[i] = add16(tgt[i], delta[i]);
        }
        break;
      case 0x14:
      case 0x16:
        if (auto* osc = std::get_if<assets::FxOsc>(&op.arg)) {
          s16* tgt = op.op == 0x14 ? obj.scale : obj.srate;
          for (int i = 0; i < 3; ++i) {
            int delta = trunc16(osc_value(osc->axis[static_cast<size_t>(i)], hframe) >> 16);
            if (actor_scale) delta = trunc16((delta * actor_scale) >> 12);
            tgt[i] = add16(tgt[i], delta);
          }
        }
        break;
      case 0x15:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
          const int vec[3] = {xyz->x, xyz->y, xyz->z};
          for (int i = 0; i < 3; ++i) obj.srate[i] = static_cast<s16>(scale_by_actor(static_cast<s16>(vec[i])));
        }
        break;
      case 0x19:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
          obj.ang[0] = xyz->x; obj.ang[1] = xyz->y; obj.ang[2] = xyz->z;
          if (const auto* pv = header.patched(0x19))
            for (int i = 0; i < 3; ++i) obj.ang[i] = static_cast<s16>((*pv)[static_cast<size_t>(i)]);
        }
        break;
      case 0x1A:
      case 0x1E:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
          const int vec[3] = {xyz->x, xyz->y, xyz->z};
          s16* tgt = op.op == 0x1A ? obj.ang : obj.arate;
          for (int i = 0; i < 3; ++i)
            if (vec[i] != 0) tgt[i] = add16(tgt[i], (rand16() * vec[i]) >> 16);
        }
        break;
      case 0x1B:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) { obj.arate[0] = xyz->x; obj.arate[1] = xyz->y; obj.arate[2] = xyz->z; }
        break;
      case 0x1C:
      case 0x1F:
        if (auto* osc = std::get_if<assets::FxOsc>(&op.arg)) {
          s16* tgt = op.op == 0x1C ? obj.ang : obj.arate;
          for (int i = 0; i < 3; ++i) tgt[i] = add16(tgt[i], osc_value(osc->axis[static_cast<size_t>(i)], hframe) >> 16);
        }
        break;
      case 0x20:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
          obj.col[0] = static_cast<u16>(xyz->x); obj.col[1] = static_cast<u16>(xyz->y); obj.col[2] = static_cast<u16>(xyz->z);
        }
        break;
      case 0x21:
      case 0x23:
        if (auto* osc = std::get_if<assets::FxOsc>(&op.arg)) {
          u16* tgt = op.op == 0x21 ? obj.col : obj.crate;
          for (int i = 0; i < 3; ++i) tgt[i] = static_cast<u16>(tgt[i] + (osc_value(osc->axis[static_cast<size_t>(i)], hframe) >> 16));
        }
        break;
      case 0x22:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
          obj.crate[0] = static_cast<u16>(xyz->x); obj.crate[1] = static_cast<u16>(xyz->y); obj.crate[2] = static_cast<u16>(xyz->z);
        }
        break;
      case 0x25:
      case 0x26:
        if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
          const int vec[3] = {xyz->x, xyz->y, xyz->z};
          u16* tgt = op.op == 0x25 ? obj.col : obj.crate;
          for (int i = 0; i < 3; ++i)
            if (vec[i] != 0) tgt[i] = static_cast<u16>(tgt[i] + ((rand16() * vec[i]) >> 16));
        }
        break;
      case 0x27:
        if (auto* blob = std::get_if<assets::FxBlob>(&op.arg)) {
          const int base_len = blob->base + (blob->base_grow ? (static_cast<int>(hframe) * blob->base_grow) / 2048 : 0);
          const int span_len = blob->span + (blob->span_grow ? (static_cast<int>(hframe) * blob->span_grow) / 2048 : 0);
          // Shifted by pos_shift (5 in battle, 3 in the field).
          const int radius = shl32(base_len + (((rng.next() & 0x7FFF) * span_len) >> 15), cfg.pos_shift);
          const int ra = rng.next() & 0xFFFF, rb = rng.next() & 0xFFFF;
          const Mat3 rot = Mat3::from_angles(ra, rb, 0).scaled_rows(blob->sx / 4096.0f, blob->sy / 4096.0f, blob->sz / 4096.0f);
          int point[3] = {static_cast<int>(rot.m[0] * static_cast<float>(radius)), static_cast<int>(rot.m[3] * static_cast<float>(radius)),
                          static_cast<int>(rot.m[6] * static_cast<float>(radius))};
          int rotated[3];
          rotv(point, rotated);
          for (int i = 0; i < 3; ++i) obj.pos[i] = add32(obj.pos[i], shl32(rotated[i], 16));
        }
        break;
      case 0x2F:
        if (auto* osc = std::get_if<assets::FxOsc>(&op.arg)) {
          int vec[3], rotated[3];
          for (int i = 0; i < 3; ++i) vec[i] = osc_value(osc->axis[static_cast<size_t>(i)], hframe) >> 10;
          rotv(vec, rotated);
          for (int i = 0; i < 3; ++i) obj.vel[i] = add32(obj.vel[i], rotated[i]);
        }
        break;
      case 0x30:
        if (header.anchor >= 0 && hooks.anchor_basis && hooks.anchor_basis(header.anchor, anchor_par)) par = &anchor_par;
        break;
      case 0x31:
        if (auto* value = std::get_if<assets::FxValue>(&op.arg)) obj.rest = static_cast<u8>(value->value);
        break;
      case 0x32:
        if (auto* value = std::get_if<assets::FxValue>(&op.arg)) obj.floor_y = static_cast<u16>(value->value);
        break;
      case 0x33:
        if (auto* named = std::get_if<assets::FxNamed>(&op.arg)) obj.death = named->name.empty() ? -1 : find_header(named->name);
        break;
      case 0x34:
        if (auto* value = std::get_if<assets::FxValue>(&op.arg)) {
          const double dx = static_cast<double>((obj.pos[0] - origin[0]) >> 16), dy = static_cast<double>((obj.pos[1] - origin[1]) >> 16),
                       dz = static_cast<double>((obj.pos[2] - origin[2]) >> 16);
          const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
          if (len > 0) {
            // GTE GPF with sf = 1 (command 0x198003D): k * unit >> 12, i.e. k times the unit direction, then << 3 into the raw velocity
            // (about k / 1024 world units per tick; the old 4096 factor sent 'away' objects off screen in one tick).
            const double k = 8.0 * value->value / len;
            obj.vel[0] = add32(obj.vel[0], static_cast<s32>(dx * k));
            obj.vel[1] = add32(obj.vel[1], static_cast<s32>(dy * k));
            obj.vel[2] = add32(obj.vel[2], static_cast<s32>(dz * k));
          }
        }
        break;
      case 0x35:
        if (header.big)
          if (auto* ext35 = std::get_if<assets::FxExt35>(&op.arg)) {
            obj.ext = ext35->ext & ~3u;
            obj.flags |= kFlagExt;
          }
        break;
      case 0x36:
        if (header.big)
          if (auto* uv_scroll = std::get_if<assets::FxUvScroll>(&op.arg)) {
            obj.uv_phase[0] = obj.uv_phase[1] = obj.uv_phase_prev[0] = obj.uv_phase_prev[1] = 0;
            obj.uv_rate[0] = uv_scroll->rate_u;
            obj.uv_rate[1] = uv_scroll->rate_v;
            obj.ext |= 1;
          }
        break;
      case 0x37:
        if (header.big)
          if (auto* morph = std::get_if<assets::FxMorph>(&op.arg)) {
            obj.morph_phase = obj.morph_phase_prev = 0;
            obj.morph_rate = morph->rate;
            obj.morph_name = morph->name.empty() ? nullptr : &morph->name;
            obj.ext = (obj.ext & 0xFFFF1FFFu) | ((morph->mode & 7u) << 13);
            if (!morph->name.empty()) obj.ext |= 2;
          }
        break;
      case 0x38: {
        const int steps = static_cast<s16>(obj.life);
        for (int k = 0; k < steps; ++k) integrate_step(obj, cfg.pos_shift);
        if (obj.drag != 0) obj.drag = static_cast<s16>(0x1000000 / obj.drag);
        for (int i = 0; i < 3; ++i) obj.vel[i] = -obj.vel[i];
        break;
      }
      case 0x39:
        if (auto* basis_arg = std::get_if<assets::FxBasis>(&op.arg)) {
          int sc = 0;
          bool found = false;
          const std::array<int, 4> sel = header.patched(0x39) ? *header.patched(0x39) : std::array<int, 4>{basis_arg->a, basis_arg->b, basis_arg->c, basis_arg->d};
          local_par = actor_basis(sel[0], sel[1], sel[2], sel[3], sc, found);
          par = &local_par;
          if (found) {
            if (basis_arg->flags & 2) actor_scale = sc;
            if (basis_arg->flags & 4) local_par.m = local_par.m.scaled_rows(sc / 4096.0f, sc / 4096.0f, sc / 4096.0f);
          }
        }
        break;
      case 0x3A:
        if (auto* drag = std::get_if<assets::FxDrag>(&op.arg)) {
          obj.drag = static_cast<s16>(drag->base);
          if (drag->rand != 0) obj.drag = add16(obj.drag, rng.next() % static_cast<s16>(drag->rand));
        }
        break;
      case 0x3B:
      case 0x46:
        if (par) {
          const Mat3& matrix = par->m;
          obj.ang[1] = add16(obj.ang[1], ratan2(static_cast<int>(matrix.m[8] * 4096), static_cast<int>(matrix.m[6] * 4096)));
          if (op.op == 0x46) obj.ang[2] = add16(obj.ang[2], -ratan2(static_cast<int>(matrix.m[3] * 4096), static_cast<int>(matrix.m[4] * 4096)));
        }
        break;
      case 0x42:
        if (par) {
          if (!header.has_basis_op) {
            const Mat3& matrix = par->m;
            obj.ang[1] = add16(obj.ang[1], ratan2(static_cast<int>(matrix.m[8] * 4096), static_cast<int>(matrix.m[6] * 4096)));
            obj.ang[2] = add16(obj.ang[2], -ratan2(static_cast<int>(matrix.m[3] * 4096), static_cast<int>(matrix.m[4] * 4096)));
          } else if (header.basis_sel[2] == 0) {
            const Mat3& matrix = par->m;
            obj.ang[1] = add16(obj.ang[1], ratan2(static_cast<int>(matrix.m[8] * 4096), static_cast<int>(matrix.m[6] * 4096)));
          } else {
            int s0 = 0, s1 = 0;
            bool f0 = false, f1 = false;
            const Basis p0 = actor_basis(header.basis_sel[0], header.basis_sel[1], 0, 0, s0, f0);
            const Basis p1 = actor_basis(header.basis_sel[2], header.basis_sel[3], 0, 0, s1, f1);
            const int dx = static_cast<int>(p1.t[0] - p0.t[0]), dy = static_cast<int>(p1.t[1] - p0.t[1]),
                      dz = static_cast<int>(p1.t[2] - p0.t[2]);
            obj.ang[1] = add16(obj.ang[1], -ratan2(dz, dx));
            const int hd = static_cast<int>(std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dz) * dz));
            obj.ang[2] = add16(obj.ang[2], ratan2(dy, hd));
          }
        }
        break;
      case 0x3C:
        if (auto* named = std::get_if<assets::FxNamed>(&op.arg)) {
          obj.periodic = named->name.empty() ? -1 : find_header(named->name);
          // Dev trace: which emitter owns which periodic child.
          if (static const bool trace = std::getenv("OC_FX_START") != nullptr; trace)
            std::fprintf(stderr, "fx periodic %s -> %s (header %d)\n", header.def->name.c_str(), named->name.c_str(), obj.periodic);
        }
        break;
      case 0x3D:
        if (auto* path_arg = std::get_if<assets::FxPath>(&op.arg)) {
          const Curve* curve = path_arg->name.empty() ? nullptr : bank.curve(path_arg->name);
          if (curve && emitter.duration != 0) {
            Basis basis;
            if (par) basis = *par;
            else {
              int sc = 0;
              bool found = false;
              basis = actor_basis(curve->basis_sel[0], curve->basis_sel[1], curve->basis_sel[2], curve->basis_sel[3], sc, found);
            }
            const int eased = ease(((static_cast<int>(hframe) - emitter.start) * 0x1000) / emitter.duration, path_arg->mode & 7);
            const Vec3 point = curve->sample(eased);
            const Vec3 world = basis.m.apply(point);
            for (int i = 0; i < 3; ++i) obj.pos[i] = shl32(static_cast<s16>(static_cast<int>(world[static_cast<size_t>(i)] + basis.t[static_cast<size_t>(i)])), 16);
          }
        }
        break;
      case 0x3E:
        if (header.huge)
          if (auto* path_arg = std::get_if<assets::FxPath>(&op.arg)) {
            const Curve* curve = path_arg->name.empty() ? nullptr : bank.curve(path_arg->name);
            if (curve) {
              obj.path = curve;
              obj.path_sel = curve->basis_sel;
              obj.path_live = (curve->flags & 1) != 0;
              if (par) obj.path_basis = *par;
              else {
                int sc = 0;
                bool found = false;
                obj.path_basis = actor_basis(curve->basis_sel[0], curve->basis_sel[1], curve->basis_sel[2], curve->basis_sel[3], sc, found);
              }
              obj.path_mode = path_arg->mode;
              obj.path_l0 = obj.life;
              const Vec3 point = obj.path_basis.m.apply(curve->sample(0));
              for (int i = 0; i < 3; ++i) obj.pos[i] = shl32(static_cast<s16>(static_cast<int>(point[static_cast<size_t>(i)] + obj.path_basis.t[static_cast<size_t>(i)])), 16);
            }
          }
        break;
      case 0x3F:
      case 0x40:
        if (sprite_set) {
          obj.slot_reg = op.op == 0x3F ? 0 : 1;
          // A particle without a model is bound itself and clears the slot when it is freed.
          if (obj.stype != 20) obj.flags |= 0x80000000u;
        }
        break;
      case 0x41:
        if (auto* rand_dir = std::get_if<assets::FxRandDir>(&op.arg)) {
          int ang[3];
          for (int i = 0; i < 3; ++i) {
            const int spread = rand_dir->spread[static_cast<size_t>(i)];
            ang[i] = static_cast<int>(rand_dir->base[static_cast<size_t>(i)]) + (spread + 1 != 0 ? rng.next() % (spread + 1) : 0) - spread / 2;
          }
          const double sp = std::sqrt(std::pow(obj.vel[0] >> 12, 2.0) + std::pow(obj.vel[1] >> 12, 2.0) + std::pow(obj.vel[2] >> 12, 2.0));
          const Mat3 rot = Mat3::from_angles(ang[0], ang[1], ang[2]);
          Vec3 vec = rot.apply({static_cast<float>(sp), 0, 0});
          if (par) vec = par->m.apply(vec);
          for (int i = 0; i < 3; ++i) obj.vel[i] = shl32(static_cast<s32>(vec[static_cast<size_t>(i)]), 12);
        }
        break;
      case 0x43:
        if (header.big)
          if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
            for (int i = 0; i < 3; ++i) obj.sub_s[i] = 0;
            int vec[3] = {xyz->x, xyz->y, xyz->z};
            if (actor_scale)
              for (int i = 0; i < 3; ++i) vec[i] = (vec[i] * actor_scale) >> 12;
            for (int i = 0; i < 3; ++i) obj.acc_s[i] = static_cast<s16>(vec[i]);
          }
        break;
      case 0x44:
        if (header.big)
          if (auto* xyz = std::get_if<assets::FxXyz>(&op.arg)) {
            for (int i = 0; i < 3; ++i) obj.sub_a[i] = 0;
            obj.acc_a[0] = xyz->x; obj.acc_a[1] = xyz->y; obj.acc_a[2] = xyz->z;
          }
        break;
      case 0x45:
        if (auto* named = std::get_if<assets::FxNamed>(&op.arg)) pending_child = named->name.empty() ? -1 : find_header(named->name);
        break;
      // Model body: u16 @+6 = motion advance per tick in 1/256 keyframe; op 04's byte selects the motion set that is started;
      // byte @+2 = loop.
      case 0x49:
        if (auto* model_body = std::get_if<assets::FxModelBody>(&op.arg)) {
          obj.motion_rate = model_body->rate;
          obj.model_clip = static_cast<s16>(obj.anim_per);
          obj.model_loop = model_body->show != 0;
          obj.motion = obj.motion_prev = 0;
        }
        break;
      // 01, 17, 1D, 24, 48, 49 and unknown: nothing to do for the sprite player.
      default: break;
    }
  }
  // After the stream: matrix, bits, persistent bookkeeping.
  build_matrix(obj);
  if (obj.stype == 17) {
    // A light object takes the first light slot whose intensity is 0, else slot 2, and an older live
    // object of the same sprite record holding that slot is freed. Without a table every
    // slot reads as free.
    int slot = 0;
    while (lights && slot < 3 && lights->slot[static_cast<size_t>(slot)].intensity != 0) ++slot;
    obj.light_slot = static_cast<s8>(slot == 3 ? 2 : slot);
    for (Object& other : objs)
      if (other.alive && other.stype == 17 && other.light_slot == obj.light_slot && other.light_name && obj.light_name && *other.light_name == *obj.light_name)
        other.alive = false;
  }
  if (obj.life == 0) {
    // A persistent object replaces the header's earlier objects, and the header leaves the firing list
    // (stopped = 1) until a script starts it again; the object keeps the link for the per-tick replay,
    // which also advances the header frame. Firing on would re-spawn it every period (resetting its UV-scroll phase and state).
    // Compaction is lazy; the dead ones are removed at the end of tick().
    kill_objects(static_cast<int>(hi));
    header.linked = false;
    header.stopped = true;
  }
  for (int i = 0; i < 3; ++i) {
    obj.ppos[i] = obj.pos[i];
    obj.pscale[i] = obj.scale[i];
    obj.pang[i] = obj.ang[i];
    obj.pcol[i] = obj.col[i];
  }
  if (header.has_anchor_op && header.anchor >= 0 && hooks.anchor_basis_at && !(obj.flags & kFlagRelParent)) {
    // Born at the anchor entity's tick position (a world-space object, op 0x08 + par->t above): the entity itself is drawn at its
    // interpolated pose, so the new object starts from where the entity was one tick ago and glides with it. Without this the
    // newest piece of a boat's wake sat a step ahead of the hull at every render between two ticks.
    Basis b0, b1;
    if (hooks.anchor_basis_at(header.anchor, 0.0f, b0) && hooks.anchor_basis_at(header.anchor, 1.0f, b1))
      for (size_t i = 0; i < 3; ++i) obj.born_shift[i] = static_cast<s32>(std::lround((b0.t[i] - b1.t[i]) * 65536.0f));
  }
  ++st.spawned;
  ++st.type_spawns[static_cast<size_t>(std::clamp<int>(obj.stype, 0, 31))];
  objs.push_back(std::move(obj));
  return static_cast<int>(objs.size()) - 1;
}

void EmitterSystem::Impl::build_matrix(Object& obj) {
  const bool zyx = obj.flags & kFlagZyx;
  if (obj.ang[0] == 0 && obj.ang[1] == 0 && obj.ang[2] == 0) obj.mat = Mat3::identity();
  else obj.mat = Mat3::from_angles(obj.ang[0], obj.ang[1], obj.ang[2], zyx);
  // Flag bit 22 (set when the scale is not the default) -> ScaleMatrix every tick whatever the values, so a
  // scale of (0, 0, 0) zeroes the matrix (a never-visible anchor particle).
    // ScaleMatrix: m[i][j] *= s[j], i.e. R * diag(s) - the sprite's own axes are scaled before the rotation
    // (a flat ring = angle X 1024 with scale z 0, the wake emitter mz01; scaling the rows instead collapsed it to a line).
    obj.mat = obj.mat.scaled_columns(obj.scale[0] / 4096.0f, obj.scale[1] / 4096.0f, obj.scale[2] / 4096.0f);
  // Face the velocity (yaw, and pitch unless flag bit 14 is set).
  if (obj.flags & kFlagFaceVelMask) {
    const int vx = obj.vel[0] >> 8, vy = obj.vel[1] >> 8, vz = obj.vel[2] >> 8;
    const int hd = static_cast<int>(std::sqrt(static_cast<double>(vx) * vx + static_cast<double>(vz) * vz));
    const int yaw = -ratan2(vz, vx);
    const int pitch = (obj.flags & (2u << 13)) == 0 ? ratan2(vy, hd) : 0;
    obj.mat = Mat3::from_angles(0, yaw, pitch) * obj.mat;
  }
}

// pos += vel << shift (`shift`: 3 field / title, 5 battle), the floor at floor_y << shift.
void EmitterSystem::Impl::integrate_step(Object& obj, int shift) {
  for (int i = 0; i < 3; ++i) obj.pos[i] = add32(obj.pos[i], shl32(obj.vel[i], shift));
  for (int i = 0; i < 3; ++i) obj.vel[i] = add32(obj.vel[i], obj.acc[i]);
  if (obj.drag != 0)
    for (int i = 0; i < 3; ++i) obj.vel[i] = ((obj.vel[i] >> 6) * obj.drag) >> 6;
  if ((obj.flags & kFlagFloor) && shl32(static_cast<s32>(obj.floor_y) << shift, 16) < obj.pos[1]) {
    // Handled by the caller (die).
    if (obj.flags & kFlagNoDeathSpawn) return;
    obj.vel[1] = (-obj.vel[1] * static_cast<int>(obj.rest) + 0xFF) >> 8;
    obj.pos[1] = shl32(static_cast<s32>(obj.floor_y) << shift, 16);
  }
}

// -- header fire ------------------------------------------------------------------------------------------------------
// Spawn one object, then the optional op-45 child emission.
// Returns the main object's index (-1 when refused).
int EmitterSystem::Impl::fire(size_t hi, const Object* parent_obj) {
  if (depth >= kMaxDepth) return -1;
  ++depth;
  pending_child = -1;
  const Basis* pb = nullptr;
  Basis pbs;
  if (parent_obj) {
    pbs = heading_basis(*parent_obj);
    pb = &pbs;
  }
  const int oi = spawn(hi, pb);
  if (oi >= 0 && pending_child >= 0) {
    const int child = pending_child;
    pending_child = -1;
    const Object parent = objs[static_cast<size_t>(oi)];
    Basis basis;
    const Header& header = headers[hi];
    bool found = false;
    int sc = 0;
    if (header.has_basis_op) basis = actor_basis(header.basis_sel[0], header.basis_sel[1], header.basis_sel[2], header.basis_sel[3], sc, found);
    else basis.m = Mat3::from_angles(0, -ratan2(parent.vel[2] >> 4, parent.vel[0] >> 4), 0);
    basis.t = {static_cast<float>(parent.pos[0] >> 16), static_cast<float>(parent.pos[1] >> 16), static_cast<float>(parent.pos[2] >> 16)};
    const int reps = headers[static_cast<size_t>(child)].def->repeat;
    for (int k = 0; k <= reps && reps >= 0; ++k) {
      const int ci = spawn(static_cast<size_t>(child), &basis);
      if (ci < 0) break;
      Object& child_obj = objs[static_cast<size_t>(ci)];
      if (child_obj.life == 0) {
        child_obj.alive = false;
      } else {
        if (parent.flags & kFlagInheritAngle) for (int i = 0; i < 3; ++i) child_obj.ang[i] = parent.ang[i];
        if (parent.flags & kFlagInheritVel) for (int i = 0; i < 3; ++i) child_obj.vel[i] = parent.vel[i];
        if (parent.flags & kFlagInheritScale) for (int i = 0; i < 3; ++i) child_obj.scale[i] = parent.scale[i];
        if (parent.flags & kFlagInheritColor) for (int i = 0; i < 3; ++i) child_obj.col[i] = parent.col[i];
        build_matrix(child_obj);
        for (int i = 0; i < 3; ++i) { child_obj.ppos[i] = child_obj.pos[i]; child_obj.pang[i] = child_obj.ang[i]; child_obj.pscale[i] = child_obj.scale[i]; child_obj.pcol[i] = child_obj.col[i]; }
      }
    }
  }
  --depth;
  return oi;
}

// The dying object runs its death-child emitter with itself as parent. `spawn_children` is false for a
// natural expiry of an object with flag bit27 (no death spawn).
void EmitterSystem::Impl::die(size_t idx, bool spawn_children) {
  Object dying = objs[idx];
  objs[idx].alive = false;
  ++st.died;
  if (!spawn_children || dying.death < 0 || depth >= kMaxDepth) return;
  // The death child goes through fire(): its op 45 (SPAWN_WITH) emission runs too.
  const int reps = headers[static_cast<size_t>(dying.death)].def->repeat;
  const bool pos_replace = !(dying.flags & (1u << 18));
  for (int k = 0; k <= reps && reps >= 0; ++k) {
    const int ci = fire(static_cast<size_t>(dying.death), nullptr);
    if (ci < 0) break;
    Object& child_obj = objs[static_cast<size_t>(ci)];
    if (child_obj.life == 0) {
      child_obj.alive = false;
      continue;
    }
    if (pos_replace) {
      if (!(dying.flags & (1u << 19))) for (int i = 0; i < 3; ++i) child_obj.pos[i] = dying.pos[i];
      else for (int i = 0; i < 3; ++i) child_obj.pos[i] = add32(child_obj.pos[i], dying.pos[i]);
    }
    if (dying.flags & kFlagInheritAngle) for (int i = 0; i < 3; ++i) child_obj.ang[i] = dying.ang[i];
    if (dying.flags & kFlagInheritVel) for (int i = 0; i < 3; ++i) child_obj.vel[i] = dying.vel[i];
    if (dying.flags & kFlagInheritScale) for (int i = 0; i < 3; ++i) child_obj.scale[i] = dying.scale[i];
    if (dying.flags & kFlagInheritColor) for (int i = 0; i < 3; ++i) child_obj.col[i] = dying.col[i];
    build_matrix(child_obj);
    for (int i = 0; i < 3; ++i) { child_obj.ppos[i] = child_obj.pos[i]; child_obj.pang[i] = child_obj.ang[i]; child_obj.pscale[i] = child_obj.scale[i]; child_obj.pcol[i] = child_obj.col[i]; }
  }
}

}  // namespace oc::effects
