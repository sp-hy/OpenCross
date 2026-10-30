#pragma once
// The parts of `opencross modelview` that need no window: the catalog of character models, clip matching between the original
// and the modded copy, playback time, framing bounds, the orbit camera and the command line.
#include <string>
#include <vector>

#include "assets/model.h"
#include "render/mat4.h"

namespace oc::tool::model_view {

enum class Kind { Field, BattlePlayer, BattleMonster };

struct Entry {
  std::string stem;
  Kind kind = Kind::Field;
};

// One entry per stem of game::model_stems(), its kind taken from the folder.
std::vector<Entry> catalog(const std::vector<std::string>& stems);
// `group`: 0 = all, else the Kind + 1. `query` matches anywhere in the stem, case-insensitively.
bool matches(const Entry& entry, int group, const std::string& query);

// The clip of `clips` with the reference clip's source and index (not its position: the two copies may order their clips
// differently). -1 when there is none or more than one.
int matching_clip(const std::vector<assets::AnimationClip>& clips, const assets::AnimationClip& reference);
// The clip frame `seconds` into playback: wraps when looping, else holds the last frame.
double playback_frame(double seconds, u32 frame_count, bool loop);

// Axis-aligned box of the rest-pose vertices.
struct Bounds {
  assets::Vec3 min{}, max{};
  bool valid = false;
};
Bounds rest_bounds(const assets::ModelAsset& asset);
Bounds bounds_union(const Bounds& first, const Bounds& second);

// Zoom multiplies the fitted distance; pitch is in radians.
constexpr float kMinZoom = 0.35f, kMaxZoom = 10.0f, kMaxPitch = 1.4f;

struct Camera {
  render::Mat4 view, projection;
};
// Orbit camera that fits `bounds` in the view at zoom 1. Model space has y down and camera depth positive, as the mesh pipeline
// expects.
Camera orbit_camera(const Bounds& bounds, float yaw, float pitch, float zoom, float aspect);

struct Options {
  std::string game_dir, model, error;
  bool hd = true, help = false;
};
// -g <dir>, --model <stem>, --sd, --help / -h.
Options parse_options(const std::vector<std::string>& args);

}  // namespace oc::tool::model_view
