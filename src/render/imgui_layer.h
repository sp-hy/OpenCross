#pragma once
// Dear ImGui over SDL3 + SDL_GPU. Draws inside the frame's rendering scope, after the scene.
//
// The Esc settings overlay is one tabbed window: General and Graphics are built in; other tabs (Mods, Debug) are registered by the
// modules that own them with add_tab() and drawn in `order` (General 0, Graphics 10, Mods 30, Debug 90).
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/config.h"
#include "render/gpu_context.h"

union SDL_Event;

namespace oc::render {

// What the options window changed this frame (the config has already been updated and saved).
struct OptionsChange {
  bool vsync = false;
  bool art_mode = false;
  bool audio = false;
  // Each flag names the config key that changed. `window` covers WindowMode / Width / Height / Display (needs Window::apply_video and a
  // swapchain rebuild); `other` is anything else that was saved (FrameCap, a registered tab's mark_changed).
  bool portraits = false;
  bool bgm_source = false;
  bool language = false;
  bool ps1_res = false;
  bool window = false;
  bool filter = false;
  bool msaa = false;
  bool anisotropy = false;
  bool widescreen = false;
  bool enhancements = false;
  bool other = false;
  // The General tab's Quit and Restart buttons.
  bool quit = false;
  bool restart_game = false;
  bool any() const {
    return vsync || art_mode || audio || portraits || bgm_source || language || ps1_res || window || filter || msaa || anisotropy ||
           widescreen || enhancements || other || quit || restart_game;
  }
};

// A tab of the settings window registered by the code that owns its content. `draw` runs inside the tab's scrolling child each frame the tab is shown;
// it may call ImGuiLayer::mark_changed() to have the config saved.
struct SettingsTab {
  std::string name;
  std::function<void()> draw;
  int order = 50;
};

class ImGuiLayer {
 public:
  ImGuiLayer() = default;
  ~ImGuiLayer() { shutdown(); }
  ImGuiLayer(const ImGuiLayer&) = delete;
  ImGuiLayer& operator=(const ImGuiLayer&) = delete;

  bool init(GpuContext& ctx, SDL_Window* window);
  void shutdown();

  // Window::set_event_hook-compatible; `user` is the ImGuiLayer.
  static void event_hook(const SDL_Event& event, void* user);

  void new_frame();

  // Registers (or replaces, by name) a settings tab. One line for a new tab:
  //   ui.add_tab("Mods", [&] { draw_mods_tab(config); }, 30);
  void add_tab(std::string name, std::function<void()> draw, int order = 50);
  // Inside a registered tab's draw: the config changed and must be saved (reported as OptionsChange::other).
  void mark_changed() { tab_changed_ = true; }

  // Opens the overlay; a non-empty `tab` selects that tab (F8 opens "Debug").
  void open(std::string_view tab = {});
  void toggle(std::string_view tab = {});
  // The tab shown last ("General", "Graphics", ...).
  const std::string& current_tab() const { return current_tab_; }

  // The settings window. Edits `config` in place and writes it to `config_file` on any change.
  OptionsChange options_window(platform::Config& config, const std::filesystem::path& config_file);
  void render(const Frame& frame);

  bool options_open = false;
  // Called by the Graphics tab's "Restart now" button (after a graphics backend change).
  std::function<void()> on_restart;

  // A small fading notification in the top-right corner ("Art: HD"), drawn over everything in render(). A toast with the same text up to
  // the first ':' replaces the earlier one. No-op while [Video] Toasts is off (the caller checks the config).
  void toast(std::string text);

  // Shared widgets for tab bodies (consistent look across tabs).
  // section: a header line with a rule. begin_panes: side-by-side panes (TableNextColumn per pane; one column when narrow), EndTable()
  // if it returned true. begin_rows: a two-column label / control table, end_rows() if it returned true. row: next row, label cell then
  // the control cell (full width). tooltip: a hover description, word-wrapped at ~36 em.
  static void section(const char* title);
  static bool begin_panes(const char* id);
  static bool begin_rows(const char* id);
  static void end_rows();
  static void row(const char* label, const char* help = nullptr);
  static void tooltip(const char* text);

 private:
  void apply_style();
  OptionsChange general_tab(platform::Config& cfg);
  OptionsChange graphics_tab(platform::Config& cfg);

  bool initialised_ = false;
  // The GPU driver in use, and the [Video] Driver value when the Graphics tab was first drawn (a different one now needs a restart).
  std::string active_driver_;
  std::optional<std::string> startup_driver_;
  struct Toast {
    std::string text;
    double born = 0.0;
  };
  std::vector<Toast> toasts_;
  void draw_toasts();
  std::vector<SettingsTab> tabs_;
  // select_tab_: tab to select on the next frame. geom_dirty_: window moved / resized, saved once the mouse button is up. opts_scale_ /
  // opts_disp_*: UI scale / display size seen by the last options_window frame (0 = closed); a change re-applies the saved size.
  // tab_restored_: the saved tab was applied at the first open.
  std::string select_tab_;
  std::string current_tab_ = "General";
  bool geom_dirty_ = false;
  float opts_scale_ = 0.0f;
  float opts_disp_w_ = 0.0f, opts_disp_h_ = 0.0f;
  bool tab_restored_ = false;
  bool tab_changed_ = false;
  // Style scale currently applied (0 = not yet).
  float ui_scale_ = 0.0f;
  bool custom_font_ = false;
};

}  // namespace oc::render
