#include "game/battle/stage_key_light.h"

#include <algorithm>
#include <cmath>

namespace oc::game {

namespace {

constexpr float kPi = 3.14159265f;
// A lower sun stretches a unit's shadow past its own height, into thin strips.
constexpr float kMinElevation = 45.0f;

using Vec3 = std::array<float, 3>;

Vec3 normalised(const Vec3& v) {
  const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  return len > 0.0f ? Vec3{v[0] / len, v[1] / len, v[2] / len} : Vec3{0, 1, 0};
}

// The elevation limit, the azimuth kept.
Vec3 raised(const Vec3& dir) {
  const float horizontal = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
  if (horizontal <= 0.0f || std::atan2(dir[1], horizontal) * 180.0f / kPi >= kMinElevation) return dir;
  const float elevation = kMinElevation * kPi / 180.0f;
  return {dir[0] / horizontal * std::cos(elevation), std::sin(elevation), dir[2] / horizontal * std::cos(elevation)};
}

// The direction of mesh.vert's key light, with the stage's first camera as the view.
Vec3 camera_key(const assets::StageAsset& stage) {
  Vec3 forward{0, 0, 1};
  for (const assets::StageCamera& camera : stage.cameras)
    if (camera.name == "zen0" && !camera.keys.empty()) {
      const assets::StageCameraKey& key = camera.keys.front();
      const float dx = key.target[0] - key.eye[0], dz = key.target[2] - key.eye[2];
      if (dx != 0.0f || dz != 0.0f) forward = normalised({dx, 0.0f, dz});
      break;
    }
  const Vec3 right{forward[2], 0.0f, -forward[0]};
  return normalised({-0.55f * forward[0] - 0.35f * right[0], 0.70f, -0.55f * forward[2] - 0.35f * right[2]});
}

}  // namespace

KeyLight key_light(const assets::StageAsset& stage) {
  const auto from_object = [](KeyLightSource source, const assets::StageEmitter& emitter) {
    return KeyLight{source, raised(normalised({static_cast<float>(emitter.pos[0]), -static_cast<float>(emitter.pos[1]), static_cast<float>(emitter.pos[2])}))};
  };
  for (const assets::StageEmitter& emitter : stage.emitters)
    if (emitter.sprite == "sun0" && emitter.placed) return from_object(KeyLightSource::Sun, emitter);
  for (const assets::StageEmitter& emitter : stage.emitters)
    if (emitter.sprite_type == 21 && emitter.placed) return from_object(KeyLightSource::Flare, emitter);
  return {KeyLightSource::Camera, camera_key(stage)};
}

}  // namespace oc::game
