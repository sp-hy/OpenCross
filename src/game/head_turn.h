#pragma once
// The head turn of a model: a callback on the head joint (the joint of attach point 1) that turns it toward a target point, shared by
// the field (FE 30..57) and the battle (anim ops 0x88 / 0x89). The owner keeps the target, builds the head frame (the joint's parent
// composite x its rest local, scale removed) and hands the direction over in that frame; this state turns it a little per tick and
// gives the rotation the joint's local matrix is built with (RotMatrix({ax, 0, az}) x the rest rotation, the rest translation kept).
#include <algorithm>
#include <cmath>

namespace oc::game {

struct HeadTurn {
  // The callback is installed.
  bool active = false;
  // The direction still changed by >= 0x40 / 4096 on an axis this tick.
  bool moving = false;
  // Set by a release: the callback is removed once the head settles.
  bool returning = false;
  // Weight of the current direction per tick (average of cur and target with weights rate and 0x1000 - rate).
  int rate = 0;
  // Unit direction in the head frame.
  float cur[3]{0, 1, 0};
  // RotMatrix({ax, 0, az}) this tick / last tick (render interpolation), 4096 per turn.
  float ax = 0, az = 0, pax = 0, paz = 0;

  static constexpr float kPi = 3.14159265358979323846f;

  static void normalise(float vec[3]) {
    const float length = std::sqrt(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2]);
    if (length > 0.0f)
      for (int i = 0; i < 3; ++i) vec[i] /= length;
  }
  // The angles {ax, 0, az} of RotMatrix that turn +y into the unit vector dir: ax = ratan2(dir.z, dir.y), then with
  // w = RotMatrix({-ax, 0, 0}) dir = (dir.x, |(dir.y, dir.z)|, 0): az = -ratan2(w.x, w.y). 4096 per turn.
  static void angles(const float dir[3], float& out_ax, float& out_az) {
    constexpr float k = 4096.0f / (2.0f * kPi);
    out_ax = std::atan2(dir[2], dir[1]) * k;
    out_az = -std::atan2(dir[0], std::sqrt(dir[1] * dir[1] + dir[2] * dir[2])) * k;
  }
  // RotMatrix({ax, 0, az}) = Rx(ax) * Rz(az) (z applied first), row-major.
  static void rotation(float angle_x, float angle_z, float out[9]) {
    constexpr float k = 2.0f * kPi / 4096.0f;
    const float cx = std::cos(angle_x * k), sx = std::sin(angle_x * k);
    const float cz = std::cos(angle_z * k), sz = std::sin(angle_z * k);
    const float matrix[9] = {cz, -sz, 0, cx * sz, cx * cz, -sx, sx * sz, sx * cz, cx};
    std::copy(matrix, matrix + 9, out);
  }
  // Wraps a difference of two 4096/turn angles into [-2048, 2048).
  static float wrap(float angle) {
    angle = std::fmod(angle + 2048.0f, 4096.0f);
    if (angle < 0) angle += 4096.0f;
    return angle - 2048.0f;
  }

  // Installs the callback: the start direction is the model's forward axis taken into the head frame (normalised here).
  void start(const float forward_in_head[3]) {
    std::copy(forward_in_head, forward_in_head + 3, cur);
    normalise(cur);
    angles(cur, ax, az);
    pax = ax;
    paz = az;
    active = true;
    returning = false;
    moving = true;
  }
  // Once per tick before step: the angles of the last tick become the interpolation start.
  void begin_tick() {
    pax = ax;
    paz = az;
  }
  // One tick of the callback with head_dir = normalise(attach point 1 - target) in the head frame:
  //   (tx, tz) = angles(head_dir), (cx, cz) = angles(cur); z range = [max(min(tz, cz), -0x180), min(max(tz, cz), 0x200)];
  //   cur = normalise(cur * rate + head_dir * (0x1000 - rate)); (ax, az) = angles(cur), ax clamped to +-0x200, az to the z range;
  //   cur = RotMatrix({ax, 0, az}) (0, 0x1000, 0). A change below 0x40 / 0x1000 on x and z clears the moving bit; with the returning bit
  //   set the callback is then removed (the head follows the animation again).
  void step(const float head_dir[3]) {
    float tx, tz, cx, cz;
    angles(head_dir, tx, tz);
    angles(cur, cx, cz);
    const float zlo = std::max(std::min(tz, cz), -384.0f), zhi = std::min(std::max(tz, cz), 512.0f);
    const float old[3] = {cur[0], cur[1], cur[2]};
    const float kc = static_cast<float>(rate) / 4096.0f;
    float blended[3];
    for (int i = 0; i < 3; ++i) blended[i] = cur[i] * kc + head_dir[i] * (1.0f - kc);
    normalise(blended);
    float next_x, next_z;
    angles(blended, next_x, next_z);
    next_x = std::clamp(next_x, -512.0f, 512.0f);
    // The engine tests the upper bound first.
    if (next_z > zhi) next_z = zhi;
    else if (next_z < zlo) next_z = zlo;
    float rm[9];
    rotation(next_x, next_z, rm);
    cur[0] = rm[1];
    cur[1] = rm[4];
    cur[2] = rm[7];
    ax = next_x;
    az = next_z;
    constexpr float kSettle = 64.0f / 4096.0f;
    if (std::fabs(cur[0] - old[0]) < kSettle && std::fabs(cur[2] - old[2]) < kSettle) {
      moving = false;
      if (returning) *this = HeadTurn{.rate = rate};
    } else {
      moving = true;
    }
  }
  // The head rotation at `alpha` between the last two ticks.
  void rotation_at(float alpha, float out[9]) const { rotation(pax + wrap(ax - pax) * alpha, paz + wrap(az - paz) * alpha, out); }
};

}  // namespace oc::game
