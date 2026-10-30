// Model viewer (tools/model_view.h): catalog, clip matching and shared playback time, bounds, orbit camera, command line.
// Synthetic data only; no game files.
#include <cmath>
#include <cstdio>
#include <limits>

#include "game/battle/battle_pose.h"
#include "model_view.h"

using namespace oc;
namespace mv = oc::tool::model_view;

namespace {

int failures = 0;
#define CHECK(x)                                    \
  do {                                              \
    if (!(x)) {                                     \
      std::printf("FAIL %d: %s\n", __LINE__, #x);   \
      ++failures;                                   \
    }                                               \
  } while (0)

bool near(double first, double second) { return std::fabs(first - second) < 1e-5; }

void test_catalog() {
  const auto entries = mv::catalog({"battle/monster/xxx/xxx", "battle/player/kid/kid", "battle/wepon/sword", "fieldobj/player/selju/selju"});
  CHECK(entries.size() == 3);
  for (const auto& entry : entries) {
    if (entry.stem != "fieldobj/player/selju/selju") continue;
    CHECK(entry.kind == mv::Kind::Field);
    CHECK(mv::matches(entry, 0, "SELJU"));
    CHECK(mv::matches(entry, 1, "selju"));
    CHECK(!mv::matches(entry, 2, "selju"));
    CHECK(!mv::matches(entry, 0, "kid"));
  }
}

// Clips are matched by source and index, and both copies are sampled at the same frame.
void test_clips_and_playback() {
  assets::AnimationClip internal, external;
  internal.name = "s4_00";
  internal.source = "s4";
  internal.index = 0;
  internal.frame_count = 2;
  internal.tracks.resize(1);
  internal.tracks[0].trans = {{{0}, {0, 0, 0}}, {{1}, {10, 0, 0}}};
  external = internal;
  external.name = "ex_00";
  external.source = "ex";
  external.tracks[0].trans = {{{0}, {100, 0, 0}}, {{1}, {200, 0, 0}}};
  CHECK(mv::matching_clip({external, internal}, internal) == 1);
  CHECK(mv::matching_clip({external}, internal) == -1);
  CHECK(mv::matching_clip({internal, internal}, internal) == -1);

  assets::ModelAsset original;
  original.skeleton.joints.emplace_back();
  original.clips = {internal, external};
  assets::ModelAsset modded = original;
  modded.clips = {external, internal};
  const int modded_clip = mv::matching_clip(modded.clips, original.clips[0]);
  game::UnitPose original_pose, modded_pose;
  original_pose.set_model(&original);
  modded_pose.set_model(&modded);
  std::vector<render::Mat4> original_palette, modded_palette;
  for (double seconds : {0.0, 1.0 / 30.0, 1.0 / 15.0, 0.1, 1.0, 100.0}) {
    const auto frame = static_cast<float>(mv::playback_frame(seconds, internal.frame_count, true));
    original_pose.palette_at(0, frame, original_palette);
    modded_pose.palette_at(modded_clip, frame, modded_palette);
    CHECK(original_palette.size() == 1 && modded_palette.size() == 1);
    if (original_palette.size() != 1 || modded_palette.size() != 1) break;
    for (size_t k = 0; k < 16; ++k) CHECK(std::fabs(original_palette[0].m[k] - modded_palette[0].m[k]) < 1e-5f);
  }

  CHECK(near(mv::playback_frame(1.0, 30, true), 15.0));
  CHECK(near(mv::playback_frame(2.0, 30, true), 0.0));
  CHECK(near(mv::playback_frame(99.0, 30, false), 29.0));
  CHECK(near(mv::playback_frame(-1.0, 30, true), 0.0));
  CHECK(near(mv::playback_frame(1.0, 0, true), 0.0));
  CHECK(near(mv::playback_frame(std::numeric_limits<double>::infinity(), 30, true), 0.0));
}

void test_bounds() {
  assets::ModelAsset asset;
  CHECK(!mv::rest_bounds(asset).valid);
  asset.meshes.emplace_back();
  assets::SkinVertex low, high, broken;
  low.bind_pos = {-1, -2, -3};
  high.bind_pos = {3, 4, 5};
  broken.bind_pos = {std::numeric_limits<float>::quiet_NaN(), 0, 0};
  asset.meshes[0].pool = {low, high, broken};
  const mv::Bounds bounds = mv::rest_bounds(asset);
  CHECK(bounds.valid && bounds.min[0] == -1 && bounds.max[2] == 5);
  const mv::Bounds empty;
  CHECK(mv::bounds_union(empty, bounds).min == bounds.min);
  CHECK(mv::bounds_union(bounds, empty).max == bounds.max);
  mv::Bounds other;
  other.valid = true;
  other.min = {-10, -20, -30};
  other.max = {0, 0, 0};
  const mv::Bounds both = mv::bounds_union(bounds, other);
  CHECK(both.min[1] == -20 && both.max[1] == 4);
}

// At zoom 1 every corner of the box is in view, also in a portrait viewport.
void test_camera() {
  mv::Bounds bounds;
  bounds.valid = true;
  bounds.min = {-1, -2, -3};
  bounds.max = {1, 2, 3};
  for (float aspect : {16.0f / 9.0f, 0.5f}) {
    const mv::Camera camera = mv::orbit_camera(bounds, 0, 0, 1, aspect);
    const render::Mat4 view_proj = camera.projection * camera.view;
    for (float x : {-1.0f, 1.0f})
      for (float y : {-2.0f, 2.0f})
        for (float z : {-3.0f, 3.0f}) {
          float clip[4] = {};
          for (int row = 0; row < 4; ++row)
            clip[row] = view_proj.at(row, 0) * x + view_proj.at(row, 1) * y + view_proj.at(row, 2) * z + view_proj.at(row, 3);
          CHECK(clip[3] > 0);
          CHECK(std::fabs(clip[0] / clip[3]) < 1 && std::fabs(clip[1] / clip[3]) < 1);
        }
  }
  const mv::Camera degenerate = mv::orbit_camera({}, 0, 0, 0, 0);
  for (float value : degenerate.projection.m) CHECK(std::isfinite(value));
  for (float value : degenerate.view.m) CHECK(std::isfinite(value));
}

void test_options() {
  const mv::Options options = mv::parse_options({"--sd", "-g", "C:/Game", "--model", "fieldobj/player/selju/selju"});
  CHECK(options.error.empty() && !options.hd && options.game_dir == "C:/Game");
  CHECK(options.model == "fieldobj/player/selju/selju");
  CHECK(!mv::parse_options({"-g"}).error.empty());
  CHECK(!mv::parse_options({"--model", "--sd"}).error.empty());
  CHECK(!mv::parse_options({"--unknown"}).error.empty());
  CHECK(mv::parse_options({"--help"}).help);
}

}  // namespace

int main() {
  test_catalog();
  test_clips_and_playback();
  test_bounds();
  test_camera();
  test_options();
  std::printf("model viewer: %d failure(s)\n", failures);
  return failures ? 1 : 0;
}
