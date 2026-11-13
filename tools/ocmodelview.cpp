// `opencross modelview [-g dir] [--model stem] [--sd]`: a window that shows one character model (field character, battle player,
// monster, or a room's own model such as an NPC's) with an orbit camera, raw animation clips and an original / modded comparison. The original copy reads a Vfs
// without mods and skips texture replacement; the modded copy uses the mods enabled in opencross.ini. A playable field character can be
// shown as [Enhancements] MenuModelsInField draws it: its menu model posed by the field clip (game/menu_rig.h). Writes nothing: no save,
// no config, no mod settings.
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <format>
#include <iterator>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "game/battle/battle_pose.h"
#include "game/disc_index.h"
#include "game/menu_rig.h"
#include "game/model_files.h"
#include "game/room_models.h"
#include "import/room.h"
#include "model_view.h"
#include "platform/config.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/gpu_context.h"
#include "render/imgui_layer.h"
#include "render/imgui_mods_tab.h"
#include "render/mesh_pipeline.h"
#include "render/texture_registry.h"
#include "tool_util.h"
#include "vfs/mods.h"
#include "vfs/vfs.h"

namespace oc::tool {

namespace {

namespace mv = model_view;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kRestPitch = 0.12f;
constexpr float kOrbitSpeed = 0.008f, kWheelZoom = 0.12f;
constexpr u32 kMaxHiddenMeshes = 32;

// The texture registry is global; bind it to the modded Vfs only while the modded copy resolves its replacements.
struct TextureRegistryScope {
  const vfs::Vfs* previous = render::texture_registry::bound_vfs();
  render::texture_registry::Settings settings = render::texture_registry::settings();
  explicit TextureRegistryScope(const vfs::Vfs& files) {
    render::texture_registry::bind(&files);
    render::texture_registry::configure({.dump = false, .replace = true});
  }
  ~TextureRegistryScope() {
    render::texture_registry::bind(previous);
    render::texture_registry::configure(settings);
  }
};

// One copy of the model: original or modded; for a playable field character or a story NPC also its detailed body (menu model or battle
// model, game::read_body_model) and the rig that poses it.
struct Side {
  std::optional<assets::ModelAsset> asset;
  std::unique_ptr<render::MeshModel> gpu;
  game::UnitPose pose;
  std::string error;
  std::optional<assets::ModelAsset> menu;
  std::unique_ptr<render::MeshModel> menu_gpu;
  game::MenuRig rig;
  std::string menu_error;
  // The detailed body's battle model number, -1 = none.
  int body = -1;
  // For a room entry: how many models the room has.
  size_t room_models = 0;
};

// The room models of `map/mapbin/<room>`, empty with `err` set when the room does not load.
std::vector<assets::RoomModelHeader> read_room_models(const vfs::Vfs& files, const std::string& stem, std::string* err) {
  const auto bin = files.read("cdrom/" + stem + ".bin");
  const auto ctd = files.read("cdrom/" + stem + ".ctd");
  if (!bin || !ctd) {
    *err = "room files missing";
    return {};
  }
  auto models = import::load_room_models(*bin, *ctd, err);
  return models ? std::move(*models) : std::vector<assets::RoomModelHeader>{};
}

// `menu_byte`: the character model byte of a playable field character (its menu model), -1 for any other model, which gets a story NPC's
// battle model when game::npc_body knows it. `room_model`: the model index of a room entry.
void load_side(Side& side, const vfs::Vfs& files, const mv::Entry& entry, int room_model, bool modded, bool hd, int menu_byte,
               render::MeshPipeline& meshes) {
  side.pose.set_model(nullptr);
  side.gpu.reset();
  side.menu_gpu.reset();
  side.menu.reset();
  side.rig = {};
  side.error.clear();
  side.menu_error.clear();
  side.room_models = 0;
  side.body = -1;
  if (entry.kind == mv::Kind::Room) {
    side.asset.reset();
    const auto models = read_room_models(files, entry.stem, &side.error);
    side.room_models = models.size();
    if (room_model >= 0 && static_cast<size_t>(room_model) < models.size())
      side.asset = game::build_room_model(models[static_cast<size_t>(room_model)], &side.error);
    else if (side.error.empty())
      side.error = "the room has no model " + std::to_string(room_model);
  } else {
    side.asset = game::read_model_files(files, entry.stem, &side.error);
  }
  if (!side.asset) return;
  const int body = menu_byte >= 0 ? menu_byte : game::npc_body(game::body_fingerprint(*side.asset));
  std::optional<TextureRegistryScope> scope;
  if (modded) {
    scope.emplace(files);
    render::resolve_model_textures(*side.asset);
  }
  side.gpu = meshes.create_model(*side.asset);
  if (!side.gpu) side.error = "GPU upload failed";
  side.pose.set_model(&*side.asset);
  if (body < 0) return;
  side.body = body;
  side.menu = game::read_body_model(files, body, hd, &side.menu_error);
  if (!side.menu) return;
  if (modded) render::resolve_model_textures(*side.menu);
  side.rig = game::match_body_rig(*side.asset, *side.menu, body);
  side.menu_gpu = meshes.create_model(*side.menu);
  if (!side.menu_gpu) side.menu_error = "GPU upload failed";
}

// The character model byte (up to the last menu model) whose field model is `stem`, -1 when it is none.
int field_model_byte(const game::DiscIndex& discs, const std::string& stem) {
  for (int byte = 0; byte <= game::kLastMenuModel; ++byte)
    if (const std::string field = discs.field_model_stem(1, byte); !field.empty() && "fieldobj/player/" + field == stem) return byte;
  return -1;
}

void show_stats(const char* label, const Side& side) {
  ImGui::SeparatorText(label);
  if (!side.error.empty()) ImGui::TextWrapped("%s", side.error.c_str());
  if (!side.asset) return;
  size_t vertices = 0, triangles = 0;
  for (const auto& mesh : side.asset->meshes) {
    vertices += mesh.pool.size();
    triangles += mesh.indices.size() / 3;
  }
  ImGui::Text("%zu vertices / %zu triangles", vertices, triangles);
  ImGui::Text("%zu parts / %zu joints / %zu clips", side.asset->meshes.size(), side.asset->skeleton.joints.size(), side.asset->clips.size());
  if (side.asset->skeleton.joints.size() > render::MeshPipeline::kMaxJoints)
    ImGui::TextWrapped("More joints than the renderer's %u (the game draws it the same way).", render::MeshPipeline::kMaxJoints);
}

// Contact sheets of a playable field character for reviewing [Enhancements] MenuModelsInField: one row per clip (the field model's own,
// then its event pack's; a one-keyframe pose repeats across the row), kSheetFrames keyframes across each row, and per keyframe the field
// model and the menu model posed from it, from the front and from the side. kSheetRows rows a page, written to <sheet>_p<N>.png;
// unmodded files, the chosen art set.
constexpr int kSheetRows = 6, kSheetFrames = 3, kSheetView = 110, kSheetRowHeight = 170, kSheetLabel = 18;
// Closer than the viewer's fitted framing: a cell is narrow and the model stands upright in it.
constexpr float kSheetZoom = 0.62f;

int run_sheet(const mv::Options& options) {
  platform::Config config;
  config.load(platform::exe_dir() / "opencross.ini");
  const std::string requested = !options.game_dir.empty() ? options.game_dir : !env_game_dir().empty() ? env_game_dir() : config.game_dir;
  const auto game_dir = platform::find_game_dir(requested);
  vfs::Vfs files;
  if (game_dir.empty() || !files.open(game_dir)) {
    std::fprintf(stderr, "game data not found; use -g <dir>\n");
    return 1;
  }
  files.set_hd_mode(options.hd);
  game::DiscIndex discs;
  discs.load(files);
  const std::string stem = lower_ascii(options.model);
  const auto entries = mv::catalog({stem});
  const mv::Entry entry = entries.empty() ? mv::Entry{stem, mv::Kind::Field} : entries[0];
  const int menu_byte = entry.kind == mv::Kind::Field ? field_model_byte(discs, stem) : -1;
  constexpr int kCell = 4 * kSheetView, kWidth = kSheetFrames * kCell, kHeight = kSheetRows * kSheetRowHeight;
  platform::Window window;
  if (!window.create("OpenCross model sheet", kWidth, kHeight, false)) return 1;
  render::GpuContext context;
  if (!context.init(window.handle(), true, config.gpu_driver)) return 1;
  context.set_clear_color(0.18f, 0.20f, 0.24f);
  render::MeshPipeline meshes;
  if (!meshes.init(context)) return 1;
  render::ImGuiLayer ui;
  if (!ui.init(context, window.handle())) return 1;
  Side side;
  struct WaitBeforeRelease {
    render::GpuContext& gpu;
    ~WaitBeforeRelease() { gpu.wait_idle(); }
  } wait_before_release{context};
  load_side(side, files, entry, options.index, false, options.hd, menu_byte, meshes);
  if (side.asset && side.body < 0) {
    std::fprintf(stderr, "--sheet needs a playable field character or a story NPC with a detailed body (--model fieldobj/player/<folder>/<file> "
                         "or map/mapbin/<room> --index <model>)\n");
    return 2;
  }
  if (!side.gpu || !side.menu_gpu) {
    std::fprintf(stderr, "sheet: %s %s\n", side.error.c_str(), side.menu_error.c_str());
    return 1;
  }
  std::vector<size_t> clips;
  for (size_t i = 0; i < side.asset->clips.size(); ++i)
    if (side.asset->clips[i].frame_count > 0) clips.push_back(i);
  const mv::Bounds bounds = mv::rest_bounds(*side.asset);
  std::vector<render::Mat4> palette, menu_palette;
  platform::Input input;
  int pages = 0;
  for (size_t first = 0; first < clips.size(); first += kSheetRows) {
    if (!window.pump(input)) break;
    ui.new_frame();
    render::Frame frame;
    if (!context.begin_frame(frame)) {
      ImGui::EndFrame();
      continue;
    }
    const float to_pixels = static_cast<float>(frame.extent.width) / static_cast<float>(kWidth);
    const ImGuiIO& io = ImGui::GetIO();
    const float to_ui = io.DisplaySize.x / static_cast<float>(kWidth);
    ImDrawList* labels = ImGui::GetForegroundDrawList();
    for (int row = 0; row < kSheetRows && first + static_cast<size_t>(row) < clips.size(); ++row) {
      const assets::AnimationClip& clip = side.asset->clips[clips[first + static_cast<size_t>(row)]];
      const std::string label = std::format("{} ({} keys)", clip.name, clip.frame_count);
      labels->AddText({4.0f * to_ui, static_cast<float>(row * kSheetRowHeight + 2) * to_ui}, IM_COL32(255, 255, 200, 255), label.c_str());
      for (int column = 0; column < kSheetFrames; ++column) {
        const float key = static_cast<float>(column) * static_cast<float>(clip.frame_count - 1) / static_cast<float>(kSheetFrames);
        side.pose.palette_at(static_cast<int>(clips[first + static_cast<size_t>(row)]), key, palette);
        game::menu_rig_palette(side.rig, palette, menu_palette);
        for (int view = 0; view < 4; ++view) {
          const bool menu = view % 2 == 1;
          const float yaw = view < 2 ? 0.0f : kPi * 0.5f;
          const float x = static_cast<float>(column * kCell + view * kSheetView) * to_pixels;
          const float y = static_cast<float>(row * kSheetRowHeight + kSheetLabel) * to_pixels;
          const float w = static_cast<float>(kSheetView) * to_pixels, h = static_cast<float>(kSheetRowHeight - kSheetLabel) * to_pixels;
          const mv::Camera camera = mv::orbit_camera(bounds, yaw, 0.0f, kSheetZoom, w / h);
          render::MeshPipeline::DrawParams params;
          params.view = camera.view;
          params.view_proj = camera.projection * camera.view;
          params.palette = menu ? menu_palette : palette;
          params.hd = options.hd;
          params.linear = true;
          if (menu) {
            params.model = render::mat4_scale(side.rig.scale);
            game::menu_model_parts(*side.menu, params);
          } else {
            for (size_t k = 0; k < side.asset->meshes.size() && k < side.asset->shading.size() && k < kMaxHiddenMeshes; ++k)
              if (side.asset->shading[k].flags & 1) params.hidden_meshes |= 1u << k;
          }
          const SDL_GPUViewport viewport{x, y, w, h, 0, 1};
          SDL_SetGPUViewport(context.pass(), &viewport);
          meshes.begin(frame, {static_cast<s32>(x), static_cast<s32>(y), static_cast<u32>(w), static_cast<u32>(h)});
          meshes.draw(menu ? *side.menu_gpu : *side.gpu, params);
          meshes.end();
        }
      }
    }
    const SDL_GPUViewport full{0, 0, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height), 0, 1};
    SDL_SetGPUViewport(context.pass(), &full);
    ui.render(frame);
    const std::string path = std::format("{}_p{}.png", options.sheet, pages);
    context.request_capture([path](std::vector<u8>&& pixels, u32 width, u32 height) {
      if (!write_png(path, width, height, pixels)) std::fprintf(stderr, "sheet: could not write %s\n", path.c_str());
    });
    context.end_frame();
    ++pages;
  }
  std::printf("sheet: %s, %d page(s), %zu clips, rig %s, scale %.2f\n", game::body_model_stem(side.body).c_str(), pages, clips.size(),
              side.rig.plan == game::MenuRigPlan::Map ? "hand-made map" : "automatic", side.rig.scale);
  return 0;
}

int run_viewer(const mv::Options& options) {
  platform::Config config;
  config.load(platform::exe_dir() / "opencross.ini");
  const std::string requested = !options.game_dir.empty() ? options.game_dir : !env_game_dir().empty() ? env_game_dir() : config.game_dir;
  const auto game_dir = platform::find_game_dir(requested);
  vfs::Vfs original_files, modded_files;
  if (game_dir.empty() || !original_files.open(game_dir) || !modded_files.open(game_dir)) {
    std::fprintf(stderr, "game data not found; use -g <dir>\n");
    return 1;
  }
  std::vector<std::string> stems = game::model_stems(original_files);
  std::ranges::move(mv::room_stems(original_files.list("cdrom")), std::back_inserter(stems));
  const std::vector<mv::Entry> entries = mv::catalog(stems);
  game::DiscIndex discs;
  discs.load(original_files);
  if (entries.empty()) {
    std::fprintf(stderr, "no character models in the game data\n");
    return 1;
  }
  const std::string initial = options.model.empty() ? "fieldobj/player/selju/selju" : lower_ascii(options.model);
  const auto found = std::ranges::find(entries, initial, &mv::Entry::stem);
  if (found == entries.end() && !options.model.empty()) {
    std::fprintf(stderr, "model not found: %s\n", options.model.c_str());
    return 2;
  }
  size_t selected = found == entries.end() ? 0 : static_cast<size_t>(found - entries.begin());

  platform::Window window;
  if (!window.create("OpenCross model viewer", 1440, 960, false)) return 1;
  render::GpuContext context;
  if (!context.init(window.handle(), true, config.gpu_driver)) return 1;
  context.set_clear_color(0.07f, 0.09f, 0.12f);
  render::MeshPipeline meshes;
  if (!meshes.init(context)) return 1;
  render::ImGuiLayer ui;
  if (!ui.init(context, window.handle())) return 1;
  window.set_event_hook(render::ImGuiLayer::event_hook, &ui);
  // Space / Tab / R are the viewer's own keys: no keyboard navigation of the panel.
  ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
  Side original, modded;
  // Declared after the sides: its destructor runs first and lets the GPU finish with their models, also on an exception.
  struct WaitBeforeRelease {
    render::GpuContext& gpu;
    ~WaitBeforeRelease() { gpu.wait_idle(); }
  } wait_before_release{context};

  vfs::ModManager mods;
  bool hd = options.hd, show_modded = true, reload = true, scroll_to_selected = true;
  bool playing = false, loop = true, lighting = false, linear = true, show_hidden = false, menu_model = false;
  int menu_byte = -1;
  int group = 0, clip = -1, room_model = options.index;
  char search[128] = {};
  double seconds = 0;
  float speed = 1, yaw = 0, pitch = kRestPitch, zoom = 1;
  float ambient = 0.55f, key_strength = 0.65f;
  mv::Bounds bounds;
  platform::Input input;
  platform::FrameTimer timer;
  std::vector<render::Mat4> palette, menu_palette;

  auto reload_models = [&] {
    context.wait_idle();
    original_files.set_hd_mode(hd);
    modded_files.set_hd_mode(hd);
    mods.scan(original_files, game_dir / "mods", render::mod_prefs(config));
    mods.apply(modded_files, hd ? vfs::ModArt::Modded : vfs::ModArt::Sd);
    menu_byte = entries[selected].kind == mv::Kind::Field ? field_model_byte(discs, entries[selected].stem) : -1;
    load_side(original, original_files, entries[selected], room_model, false, hd, menu_byte, meshes);
    load_side(modded, modded_files, entries[selected], room_model, true, hd, menu_byte, meshes);
    bounds = mv::bounds_union(original.asset ? mv::rest_bounds(*original.asset) : mv::Bounds{},
                              modded.asset ? mv::rest_bounds(*modded.asset) : mv::Bounds{});
    clip = -1;
    seconds = 0;
    playing = false;
    reload = false;
  };

  while (window.pump(input)) {
    const double dt = std::clamp(timer.tick(), 0.0, 0.1);
    if (reload) reload_models();
    ui.new_frame();
    const ImGuiIO& io = ImGui::GetIO();
    if (input.key_pressed[SDL_SCANCODE_ESCAPE] && !io.WantTextInput) {
      ImGui::EndFrame();
      break;
    }
    const float panel_width = std::min(390.0f, io.DisplaySize.x * 0.42f);
    if (!io.WantTextInput) {
      if (input.key_pressed[SDL_SCANCODE_SPACE]) playing = !playing;
      if (input.key_pressed[SDL_SCANCODE_TAB]) show_modded = !show_modded;
      if (input.key_pressed[SDL_SCANCODE_R]) reload = true;
    }
    if (!io.WantCaptureMouse && io.MousePos.x > panel_width) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        yaw += io.MouseDelta.x * kOrbitSpeed;
        pitch = std::clamp(pitch + io.MouseDelta.y * kOrbitSpeed, -mv::kMaxPitch, mv::kMaxPitch);
      }
      zoom = std::clamp(zoom * std::exp(-io.MouseWheel * kWheelZoom), mv::kMinZoom, mv::kMaxZoom);
    }

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({panel_width, io.DisplaySize.y});
    ImGui::Begin("Model viewer", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::TextWrapped("%s", entries[selected].stem.c_str());
    ImGui::TextDisabled("Drag to orbit, wheel to zoom");
    ImGui::InputTextWithHint("##search", "Search model path", search, sizeof search);
    ImGui::Combo("Group", &group, "All\0Field characters\0Battle players\0Battle monsters\0Room models\0");
    if (ImGui::BeginChild("catalog", {0, 190}, ImGuiChildFlags_Borders)) {
      for (size_t i = 0; i < entries.size(); ++i) {
        if (!mv::matches(entries[i], group, search)) continue;
        const bool clicked = ImGui::Selectable(entries[i].stem.c_str(), i == selected);
        if (i == selected && scroll_to_selected) {
          ImGui::SetScrollHereY();
          scroll_to_selected = false;
        }
        if (clicked && i != selected) {
          selected = i;
          room_model = 0;
          reload = true;
          yaw = 0;
          pitch = kRestPitch;
          zoom = 1;
        }
      }
    }
    ImGui::EndChild();
    if (entries[selected].kind == mv::Kind::Room) {
      const size_t count = std::max(original.room_models, modded.room_models);
      if (count > 0) {
        const int last = static_cast<int>(count) - 1;
        const int before = room_model;
        ImGui::SliderInt("Room model", &room_model, 0, last);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##previous", ImGuiDir_Left)) room_model = std::max(0, room_model - 1);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##next", ImGuiDir_Right)) room_model = std::min(last, room_model + 1);
        if (room_model != before) reload = true;
      }
      ImGui::TextWrapped("The room's own models in the order of its model list (the field script's model numbers): NPCs and objects.");
    }
    if (ImGui::Checkbox("HD art", &hd)) reload = true;
    ImGui::SameLine();
    ImGui::Checkbox("Modded", &show_modded);
    if (ImGui::Button("Reload models and mods (R)")) reload = true;
    ImGui::TextWrapped("Original ignores every mod. Modded uses the mods enabled in the Mods tab; change them there and reload.");
    if (original.body >= 0 || modded.body >= 0) {
      ImGui::Checkbox("Override with detailed body", &menu_model);
      ImGui::TextWrapped("As the HD models in fields enhancement draws this character: the Status screen's model (a story NPC: the battle "
                         "model), on the battle skeleton, posed by the field clip.");
      const Side& shown = show_modded ? modded : original;
      if (menu_model && shown.menu) {
        constexpr const char* kPlans[4] = {"automatic (under)", "automatic (beside)", "automatic (beside when full)", "hand-made map"};
        ImGui::Text("Rig: %s, %s", kPlans[static_cast<int>(shown.rig.plan)], shown.rig.usable ? "used in the field" : "NOT used in the field");
        ImGui::Text("Bones %.0f / %.0f deg, unmatched %d (%d with children)", shown.rig.bone_p90, shown.rig.bone_max, shown.rig.unmatched,
                    shown.rig.unmatched_inner);
      }
    }

    ImGui::SeparatorText("Animation");
    const assets::ModelAsset* reference = original.asset ? &*original.asset : modded.asset ? &*modded.asset : nullptr;
    const bool clip_valid = reference && clip >= 0 && static_cast<size_t>(clip) < reference->clips.size();
    if (reference) {
      const char* title = clip_valid ? reference->clips[static_cast<size_t>(clip)].name.c_str() : "Rest pose";
      if (ImGui::BeginCombo("Clip", title)) {
        if (ImGui::Selectable("Rest pose", clip < 0)) {
          clip = -1;
          seconds = 0;
          playing = false;
        }
        for (size_t i = 0; i < reference->clips.size(); ++i) {
          const auto& candidate = reference->clips[i];
          const std::string label = candidate.source + "[" + std::to_string(candidate.index) + "] " + candidate.name;
          if (ImGui::Selectable(label.c_str(), clip == static_cast<int>(i))) {
            clip = static_cast<int>(i);
            seconds = 0;
            playing = true;
          }
        }
        ImGui::EndCombo();
      }
      if (clip_valid) {
        const u32 frames = reference->clips[static_cast<size_t>(clip)].frame_count;
        const float last_frame = static_cast<float>(frames > 0 ? frames - 1 : 0);
        ImGui::Checkbox("Play (Space)", &playing);
        ImGui::SameLine();
        ImGui::Checkbox("Loop", &loop);
        ImGui::SliderFloat("Speed", &speed, 0.1f, 3.0f, "%.2fx");
        float shown = static_cast<float>(mv::playback_frame(seconds, frames, loop));
        if (ImGui::SliderFloat("Frame", &shown, 0.0f, last_frame, "%.2f")) {
          seconds = shown / assets::kModelAnimFps;
          playing = false;
        }
        if (playing) {
          seconds += dt * speed;
          if (!loop && seconds * assets::kModelAnimFps >= last_frame) playing = false;
        }
      }
    }

    ImGui::SeparatorText("Camera and light");
    if (ImGui::Button("Front")) yaw = 0, pitch = 0;
    ImGui::SameLine();
    if (ImGui::Button("Side")) yaw = kPi * 0.5f, pitch = 0;
    ImGui::SameLine();
    if (ImGui::Button("Back")) yaw = kPi, pitch = 0;
    if (ImGui::Button("Reset framing")) yaw = 0, pitch = kRestPitch, zoom = 1;
    ImGui::SliderFloat("Zoom", &zoom, mv::kMinZoom, mv::kMaxZoom, "%.2f");
    ImGui::Checkbox("Inspection light", &lighting);
    if (lighting) {
      ImGui::SliderFloat("Ambient", &ambient, 0.0f, 1.0f);
      ImGui::SliderFloat("Key strength", &key_strength, 0.0f, 1.5f);
    }
    ImGui::Checkbox("Bilinear texture filter", &linear);
    ImGui::Checkbox("Show initially hidden parts", &show_hidden);
    show_stats("Original", original);
    show_stats("Modded", modded);
    ImGui::End();

    Side& side = show_modded ? modded : original;
    int side_clip = -1;
    const char* mismatch = nullptr;
    if (clip_valid && side.asset) {
      const auto& wanted = reference->clips[static_cast<size_t>(clip)];
      side_clip = mv::matching_clip(side.asset->clips, wanted);
      if (side_clip < 0) mismatch = "This copy has no single clip with that source and index; showing the rest pose.";
      else if (side.asset->clips[static_cast<size_t>(side_clip)].frame_count != wanted.frame_count)
        mismatch = "The clip lengths differ; both copies play at the same time.";
    }
    ImGui::SetNextWindowPos({panel_width + 15, 15});
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::Begin("##viewer_status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
    ImGui::Text("%s / %s", show_modded ? "MODDED" : "ORIGINAL", hd ? "HD" : "SD");
    if (!side.error.empty()) ImGui::TextWrapped("%s", side.error.c_str());
    if (menu_model && side.body >= 0) {
      ImGui::Text("DETAILED BODY %s", game::body_model_stem(side.body).c_str());
      if (!side.menu_error.empty()) ImGui::TextWrapped("%s", side.menu_error.c_str());
    }
    if (mismatch) ImGui::TextWrapped("%s", mismatch);
    ImGui::End();

    if (reload) {
      ImGui::EndFrame();
      continue;
    }
    render::Frame frame;
    if (!context.begin_frame(frame)) {
      ImGui::EndFrame();
      SDL_Delay(10);
      continue;
    }
    const float pixel_ratio = io.DisplaySize.x > 0 ? static_cast<float>(frame.extent.width) / io.DisplaySize.x : 1;
    const auto left = static_cast<u32>(panel_width * pixel_ratio);
    const u32 width = frame.extent.width > left ? frame.extent.width - left : 1u;
    if (side.gpu && side.asset) {
      const float aspect = static_cast<float>(width) / static_cast<float>(std::max(1u, frame.extent.height));
      const mv::Camera camera = mv::orbit_camera(bounds, yaw, pitch, zoom, aspect);
      double sample_frame = 0;
      if (side_clip >= 0) sample_frame = mv::playback_frame(seconds, side.asset->clips[static_cast<size_t>(side_clip)].frame_count, loop);
      side.pose.palette_at(side_clip, static_cast<float>(sample_frame), palette);
      const bool dressed = menu_model && side.menu_gpu && side.menu;
      if (dressed) {
        game::menu_rig_palette(side.rig, palette, menu_palette);
        palette.swap(menu_palette);
      }
      render::MeshPipeline::DrawParams params;
      params.view = camera.view;
      params.view_proj = camera.projection * camera.view;
      if (dressed) params.model = render::mat4_scale(side.rig.scale);
      params.palette = palette;
      params.hd = hd;
      params.linear = linear;
      // S3 shading bit 0 = the part starts hidden (the game's scripts show it).
      if (dressed) {
        game::menu_model_parts(*side.menu, params);
        if (show_hidden) params.hidden_meshes = 0;
      } else if (!show_hidden) {
        for (size_t k = 0; k < side.asset->meshes.size() && k < side.asset->shading.size() && k < kMaxHiddenMeshes; ++k)
          if (side.asset->shading[k].flags & 1) params.hidden_meshes |= 1u << k;
      }
      // A fixed model-space key light plus ambient: orbiting moves the camera, not the light.
      params.light.on = lighting;
      for (size_t channel = 0; channel < 3; ++channel) {
        params.light.bk[channel] = ambient;
        params.light.rgb[0][channel] = key_strength;
      }
      params.light.dir[0][0] = -0.35f;
      params.light.dir[0][1] = -0.70f;
      params.light.dir[0][2] = -0.62f;
      const SDL_GPUViewport viewport{static_cast<float>(left), 0, static_cast<float>(width), static_cast<float>(frame.extent.height), 0, 1};
      SDL_SetGPUViewport(context.pass(), &viewport);
      meshes.begin(frame, {static_cast<s32>(left), 0, width, frame.extent.height});
      meshes.draw(dressed ? *side.menu_gpu : *side.gpu, params);
      meshes.end();
    }
    const SDL_GPUViewport full{0, 0, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height), 0, 1};
    SDL_SetGPUViewport(context.pass(), &full);
    ui.render(frame);
    context.end_frame();
  }
  window.set_event_hook(nullptr, nullptr);
  return 0;
}

}  // namespace

}  // namespace oc::tool

int ocmodelview_main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  const auto options = oc::tool::model_view::parse_options(args);
  if (options.help || !options.error.empty()) {
    if (!options.error.empty()) std::fprintf(stderr, "%s\n", options.error.c_str());
    std::fprintf(options.error.empty() ? stdout : stderr,
                 "usage: opencross modelview [-g <dir>] [--model <stem>] [--index <room model>] [--sd] [--sheet <png base>]\n"
                 "  <stem> as listed in the viewer, e.g. fieldobj/player/selju/selju, or map/mapbin/obo_b01f with --index\n"
                 "  --sheet: contact sheets of a playable field character or a story NPC, field model and detailed body per clip,\n"
                 "  to <base>_pN.png\n");
    return options.error.empty() ? 0 : 2;
  }
  try {
    return options.sheet.empty() ? oc::tool::run_viewer(options) : oc::tool::run_sheet(options);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "modelview: %s\n", error.what());
    return 1;
  }
}
