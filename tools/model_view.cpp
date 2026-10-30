#include "model_view.h"

#include <algorithm>
#include <cmath>

#include "tool_util.h"

namespace oc::tool::model_view {

namespace {

constexpr float kHalfFov = 0.35f;

bool finite(const assets::Vec3& point) { return std::isfinite(point[0]) && std::isfinite(point[1]) && std::isfinite(point[2]); }

assets::Vec3 center(const Bounds& bounds) {
  if (!bounds.valid) return {};
  assets::Vec3 middle;
  for (size_t k = 0; k < 3; ++k) middle[k] = bounds.min[k] * 0.5f + bounds.max[k] * 0.5f;
  return middle;
}

// Half the box diagonal (at least 1).
float extent(const Bounds& bounds) {
  if (!bounds.valid) return 1.0f;
  double squared = 0;
  for (size_t k = 0; k < 3; ++k) {
    const double half = (static_cast<double>(bounds.max[k]) - bounds.min[k]) * 0.5;
    squared += half * half;
  }
  return static_cast<float>(std::max(1.0, std::sqrt(squared)));
}

}  // namespace

std::vector<Entry> catalog(const std::vector<std::string>& stems) {
  std::vector<Entry> entries;
  for (const std::string& stem : stems) {
    if (stem.starts_with("fieldobj/player/")) entries.push_back({stem, Kind::Field});
    else if (stem.starts_with("battle/player/")) entries.push_back({stem, Kind::BattlePlayer});
    else if (stem.starts_with("battle/monster/")) entries.push_back({stem, Kind::BattleMonster});
  }
  return entries;
}

bool matches(const Entry& entry, int group, const std::string& query) {
  if (group != 0 && group != static_cast<int>(entry.kind) + 1) return false;
  return lower_ascii(entry.stem).find(lower_ascii(query)) != std::string::npos;
}

int matching_clip(const std::vector<assets::AnimationClip>& clips, const assets::AnimationClip& reference) {
  int match = -1;
  for (size_t i = 0; i < clips.size(); ++i) {
    if (clips[i].source != reference.source || clips[i].index != reference.index) continue;
    if (match >= 0) return -1;
    match = static_cast<int>(i);
  }
  return match;
}

double playback_frame(double seconds, u32 frame_count, bool loop) {
  if (!std::isfinite(seconds) || seconds <= 0 || frame_count == 0) return 0;
  const double frame = seconds * assets::kModelAnimFps;
  if (!std::isfinite(frame)) return 0;
  if (loop) return std::fmod(frame, static_cast<double>(frame_count));
  return std::min(frame, static_cast<double>(frame_count - 1));
}

Bounds rest_bounds(const assets::ModelAsset& asset) {
  Bounds bounds;
  for (const auto& mesh : asset.meshes)
    for (const auto& vertex : mesh.pool) {
      if (!finite(vertex.bind_pos)) continue;
      if (!bounds.valid) {
        bounds.min = bounds.max = vertex.bind_pos;
        bounds.valid = true;
        continue;
      }
      for (size_t k = 0; k < 3; ++k) {
        bounds.min[k] = std::min(bounds.min[k], vertex.bind_pos[k]);
        bounds.max[k] = std::max(bounds.max[k], vertex.bind_pos[k]);
      }
    }
  return bounds;
}

Bounds bounds_union(const Bounds& first, const Bounds& second) {
  if (!first.valid) return second;
  if (!second.valid) return first;
  Bounds both = first;
  for (size_t k = 0; k < 3; ++k) {
    both.min[k] = std::min(first.min[k], second.min[k]);
    both.max[k] = std::max(first.max[k], second.max[k]);
  }
  return both;
}

Camera orbit_camera(const Bounds& bounds, float yaw, float pitch, float zoom, float aspect) {
  if (!std::isfinite(yaw)) yaw = 0;
  if (!std::isfinite(pitch)) pitch = 0;
  if (!std::isfinite(zoom)) zoom = 1;
  if (!std::isfinite(aspect) || aspect <= 0) aspect = 1;
  pitch = std::clamp(pitch, -kMaxPitch, kMaxPitch);
  zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
  const assets::Vec3 target = center(bounds);
  const float size = extent(bounds);
  // Fit the bounding sphere into the narrower of the horizontal and vertical fields of view, with a 10% margin.
  const float fit_fov = std::atan(std::tan(kHalfFov) * std::min(aspect, 1.0f));
  const float distance = size / std::sin(fit_fov) * 1.1f * zoom;
  const assets::Vec3 offset{std::sin(yaw) * std::cos(pitch) * distance, -std::sin(pitch) * distance,
                            -std::cos(yaw) * std::cos(pitch) * distance};
  const assets::Vec3 forward{-offset[0] / distance, -offset[1] / distance, -offset[2] / distance};
  const assets::Vec3 right{std::cos(yaw), 0, std::sin(yaw)};
  const assets::Vec3 down{forward[1] * right[2] - forward[2] * right[1], forward[2] * right[0] - forward[0] * right[2],
                          forward[0] * right[1] - forward[1] * right[0]};
  Camera camera;
  const assets::Vec3 axes[] = {right, down, forward};
  for (int row = 0; row < 3; ++row) {
    const assets::Vec3& axis = axes[row];
    camera.view.at(row, 3) = 0;
    for (int col = 0; col < 3; ++col) {
      const size_t k = static_cast<size_t>(col);
      camera.view.at(row, col) = axis[k];
      camera.view.at(row, 3) -= axis[k] * (target[k] + offset[k]);
    }
  }
  const float near_plane = std::max(0.01f, size * 0.01f), far_plane = std::max(near_plane + 1, distance + size * 20);
  const float focal = 1.0f / std::tan(kHalfFov);
  camera.projection.m.fill(0);
  camera.projection.at(0, 0) = focal / aspect;
  camera.projection.at(1, 1) = focal;
  camera.projection.at(2, 2) = far_plane / (far_plane - near_plane);
  camera.projection.at(2, 3) = -far_plane * near_plane / (far_plane - near_plane);
  camera.projection.at(3, 2) = 1;
  return camera;
}

Options parse_options(const std::vector<std::string>& args) {
  Options options;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--help" || arg == "-h") {
      options.help = true;
    } else if (arg == "--sd") {
      options.hd = false;
    } else if (arg == "-g" || arg == "--model") {
      if (i + 1 >= args.size() || args[i + 1].starts_with("-")) {
        options.error = "missing value for " + arg;
        return options;
      }
      (arg == "-g" ? options.game_dir : options.model) = args[++i];
    } else {
      options.error = "unknown argument: " + arg;
      return options;
    }
  }
  return options;
}

}  // namespace oc::tool::model_view
