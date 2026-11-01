#pragma once
// Fixed-point helpers of the PS1 effect engine (angles 4096 = one turn, trig values 4096 = 1.0) plus the small float
// matrix type the effect player uses for object / parent / camera frames.
#include <array>
#include <cstddef>

#include "core/types.h"

namespace oc::effects {

using Vec3 = std::array<float, 3>;

// A file read from the disc: 2048-byte sectors at 2.5 sectors per tick, in whole ticks.
constexpr int disc_read_ticks(size_t bytes) { return static_cast<int>(((bytes + 2047) / 2048 * 2 + 4) / 5); }

// PsyQ rsin / rcos: argument masked to 12 bits, result in -4096..4096.
int rsin(int angle);
int rcos(int angle);
// PsyQ ratan2(y, x) in 4096 units (signed).
int ratan2(int y, int x);

// Easing used by emitter ops 37 / 3D / 3E. `phase`: 0..4096 (reflected at bit 12), mode 0..4 (5..7 = linear).
int ease(int phase, int mode);

// Pseudo-random source standing in for the C rand(): 15-bit result like the PS1 libc.
struct Rng {
  u32 state = 1;
  int next() {
    state = state * 1103515245u + 12345u;
    return static_cast<int>((state >> 16) & 0x7FFF);
  }
};

struct Mat3 {
  // Row-major.
  std::array<float, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};

  static Mat3 identity() { return {}; }
  // PsyQ RotMatrix from three angles (4096 = turn): R = Rx*Ry*Rz (z applied first: m[0][2] = sy, m[1][2] = -cy*sx); `zyx` gives
  // RotMatrixZYX = Rz*Ry*Rx (m[2][0] = -sy), which objects with flag bit 16 use.
  static Mat3 from_angles(int ax, int ay, int az, bool zyx = false);
  Vec3 apply(const Vec3& vec) const {
    return {m[0] * vec[0] + m[1] * vec[1] + m[2] * vec[2], m[3] * vec[0] + m[4] * vec[1] + m[5] * vec[2],
            m[6] * vec[0] + m[7] * vec[1] + m[8] * vec[2]};
  }
  Mat3 operator*(const Mat3& other) const;
  Mat3 scaled_columns(float sx, float sy, float sz) const;
  Mat3 scaled_rows(float sx, float sy, float sz) const;
};

// A parent frame: rotation + world translation (the PS1 MATRIX with its t[] vector). `joint` is the attach joint's own world rotation
// when the selector names one actor's joint (b >= 2, no 2nd point): an object with flag bit 7 multiplies it in front of its own matrix,
// while `m` itself stays the actor's yaw.
struct Basis {
  Mat3 m;
  Vec3 t{0, 0, 0};
  bool has_joint = false;
  Mat3 joint;
};

// Colour-blend arithmetic of the GPU for the emitter blend modes 0..4, per 8-bit channel: 1 = (B + F) / 2, 2 = B + F, 3 = B - F,
// 4 = B + F / 4. `back` = framebuffer, `front` = texel * colour / 128 (already modulated, 0..255). Mode 0 = opaque.
int blend_channel(int mode, int back, int front);
// texel * colour / 0x80, saturated to 255.
int modulate(int texel, int colour);

}  // namespace oc::effects
