// [Enhancements] Lighting: which object gives a stage its sunlight, the elevation limit, and the camera-relative fallback (src/game/battle/stage_key_light.*).
#include <cmath>
#include <cstdio>

#include "game/battle/stage_key_light.h"

using namespace oc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

assets::StageEmitter light_object(const char* sprite, int type, int x, int y, int z) {
  assets::StageEmitter emitter;
  emitter.sprite = sprite;
  emitter.sprite_type = type;
  emitter.placed = true;
  emitter.pos = {x, y, z};
  return emitter;
}

float elevation(const std::array<float, 3>& dir) { return std::asin(dir[1]) * 180.0f / 3.14159265f; }
float length(const std::array<float, 3>& dir) { return std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]); }
}  // namespace

int main() {
  // The sun object wins over a flare; file y is down.
  {
    assets::StageAsset stage;
    stage.emitters.push_back(light_object("lenz", 21, 1000, -1000, 0));
    stage.emitters.push_back(light_object("sun0", 15, 0, -2000, 2000));
    const game::KeyLight light = game::key_light(stage);
    CHECK(light.source == game::KeyLightSource::Sun);
    CHECK(std::fabs(elevation(light.dir) - 45.0f) < 0.01f);
    CHECK(light.dir[2] > 0.0f && std::fabs(light.dir[0]) < 1e-5f);
  }
  // A flare alone; an unplaced sun object does not count.
  {
    assets::StageAsset stage;
    assets::StageEmitter unplaced = light_object("sun0", 15, 0, -2000, 2000);
    unplaced.placed = false;
    stage.emitters.push_back(unplaced);
    stage.emitters.push_back(light_object("lenz", 21, -3000, -3000, 0));
    const game::KeyLight light = game::key_light(stage);
    CHECK(light.source == game::KeyLightSource::Flare);
    CHECK(light.dir[0] < 0.0f);
  }
  // A sun on the horizon keeps its azimuth and rises to the elevation limit.
  {
    assets::StageAsset stage;
    stage.emitters.push_back(light_object("sun0", 15, -23648, 0, 0));
    const game::KeyLight light = game::key_light(stage);
    CHECK(std::fabs(elevation(light.dir) - 45.0f) < 0.01f);
    CHECK(light.dir[0] < 0.0f && std::fabs(light.dir[2]) < 1e-5f);
    CHECK(std::fabs(length(light.dir) - 1.0f) < 1e-5f);
  }
  // A sun straight above stays there.
  {
    assets::StageAsset stage;
    stage.emitters.push_back(light_object("sun0", 15, 0, -11936, 0));
    CHECK(game::key_light(stage).dir[1] > 0.9999f);
  }
  // No light object: the key light as seen from the main camera, behind it and to its left, above the elevation limit.
  {
    assets::StageAsset stage;
    assets::StageCamera camera;
    camera.name = "zen0";
    assets::StageCameraKey key;
    key.eye = {0, 1000, -4000};
    key.target = {0, 500, 0};
    camera.keys.push_back(key);
    stage.cameras.push_back(camera);
    const game::KeyLight light = game::key_light(stage);
    CHECK(light.source == game::KeyLightSource::Camera);
    CHECK(light.dir[2] < 0.0f && light.dir[0] < 0.0f);
    CHECK(elevation(light.dir) >= 45.0f);
    CHECK(std::fabs(length(light.dir) - 1.0f) < 1e-5f);
  }
  if (failures == 0) std::printf("ok\n");
  return failures ? 1 : 0;
}
