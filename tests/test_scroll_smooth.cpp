// ScrollSmoother: a camera stepping whole pixels at a fractional rate is drawn at a near-steady speed, trailing the integer scroll by
// at most 2 px and never passing it; a camera stopping dead at a pan limit is not overshot and the display never moves back; it
// settles on the integer scroll, and a large jump is taken at once.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "game/field_camera.h"

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
}  // namespace

int main() {
  game::ScrollSmoother smooth;
  smooth.reset(100, 50);
  CHECK(smooth.x() == 100.0f && smooth.y() == 50.0f);

  // 0.4 px per tick on x (the integer scroll steps 1 0 1 0 0), still on y.
  const int pattern[] = {1, 0, 1, 0, 0};
  int scroll = 100;
  std::vector<float> steps;
  float last = smooth.x(), max_trail = 0, max_ahead = 0;
  for (int tick = 0; tick < 200; ++tick) {
    scroll += pattern[tick % 5];
    smooth.step(scroll, 50);
    max_trail = std::max(max_trail, static_cast<float>(scroll) - smooth.x());
    max_ahead = std::max(max_ahead, smooth.x() - static_cast<float>(scroll));
    if (tick >= 100) steps.push_back(smooth.x() - last);
    last = smooth.x();
  }
  CHECK(max_trail <= 2.0f && max_ahead <= 0.0f);
  CHECK(smooth.y() == 50.0f);
  const auto [lo, hi] = std::minmax_element(steps.begin(), steps.end());
  CHECK(*lo > 0.3f && *hi < 0.5f);

  // The camera stops: the display comes to rest on the integer scroll.
  for (int tick = 0; tick < 300; ++tick) smooth.step(scroll, 50);
  CHECK(std::abs(smooth.x() - static_cast<float>(scroll)) < 0.01f);

  // 1 px per tick into a pan limit, then stopped dead: no overshoot, no step backwards, at rest on the limit.
  smooth.reset(0, 0);
  bool past = false, back = false;
  last = 0;
  for (int tick = 1; tick <= 120; ++tick) {
    const int limited = std::min(tick, 60);
    smooth.step(limited, 0);
    past = past || smooth.x() > static_cast<float>(limited);
    back = back || smooth.x() < last;
    last = smooth.x();
  }
  CHECK(!past && !back);
  CHECK(std::abs(smooth.x() - 60.0f) < 0.01f);

  // A snap far away is taken at once.
  smooth.step(260, 20);
  CHECK(smooth.x() == 260.0f && smooth.y() == 20.0f);

  if (failures) return 1;
  std::printf("scroll smooth: all passed\n");
  return 0;
}
