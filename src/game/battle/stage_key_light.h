#pragma once
// [Enhancements] Lighting: where the sunlight of a battle stage comes from. The stage's own light objects win: the `sun0` object, then a lens
// flare. A stage with neither takes the key light of [Enhancements] Lighting as seen from its main battle camera, so the shadows agree with the
// shading of the units.
#include <array>

#include "assets/battle_stage.h"

namespace oc::game {

enum class KeyLightSource { Camera, Flare, Sun };

struct KeyLight {
  KeyLightSource source = KeyLightSource::Camera;
  // Unit vector toward the light (y up), raised to the lowest elevation that keeps a shadow inside the arena.
  std::array<float, 3> dir{0, 1, 0};
};

KeyLight key_light(const assets::StageAsset& stage);

}  // namespace oc::game
