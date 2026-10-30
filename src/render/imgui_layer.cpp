#include "render/imgui_layer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <system_error>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "core/log.h"
#include "platform/window.h"

namespace oc::render {

namespace {

// The UI font is rasterised once at this size and drawn at kFontPx * scale (FontGlobalScale), so it stays sharp when scaled down.
constexpr float kFontRaster = 32.0f;
constexpr float kFontPx = 17.0f;
// Settings window size at scale 1 (the window is resizable by the user; this is the first-open size).
constexpr float kWindowW = 640.0f, kWindowH = 560.0f;
// Width of the label column of the label / control tables, at scale 1.
constexpr float kLabelW = 210.0f;

ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
  return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a);
}

// Dark, low-contrast theme: near-black blue-grey panels, one muted teal accent.
void build_style(ImGuiStyle& style) {
  style = ImGuiStyle();
  ImGui::StyleColorsDark(&style);
  style.WindowPadding = ImVec2(16, 14);
  style.FramePadding = ImVec2(10, 5);
  style.ItemSpacing = ImVec2(10, 8);
  style.ItemInnerSpacing = ImVec2(8, 6);
  style.CellPadding = ImVec2(8, 5);
  style.IndentSpacing = 18;
  style.ScrollbarSize = 12;
  style.GrabMinSize = 10;
  style.WindowRounding = 6;
  style.ChildRounding = 4;
  style.FrameRounding = 4;
  style.PopupRounding = 4;
  style.ScrollbarRounding = 6;
  style.GrabRounding = 3;
  style.TabRounding = 4;
  style.WindowBorderSize = 1;
  style.FrameBorderSize = 0;
  style.TabBorderSize = 0;
  style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
  style.SeparatorTextBorderSize = 1;
  style.SeparatorTextPadding = ImVec2(0, 4);

  const ImVec4 bg = rgb(20, 22, 27, 0.97f), panel = rgb(28, 31, 38), frame = rgb(38, 42, 51), frame_hi = rgb(48, 53, 64),
               frame_act = rgb(56, 62, 75), accent = rgb(86, 156, 160), accent_hi = rgb(104, 180, 184), text = rgb(218, 222, 230),
               dim = rgb(128, 134, 146), border = rgb(52, 57, 68);
  ImVec4* colors = style.Colors;
  colors[ImGuiCol_Text] = text;
  colors[ImGuiCol_TextDisabled] = dim;
  colors[ImGuiCol_WindowBg] = bg;
  colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_PopupBg] = rgb(24, 27, 33, 0.98f);
  colors[ImGuiCol_Border] = border;
  colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_FrameBg] = frame;
  colors[ImGuiCol_FrameBgHovered] = frame_hi;
  colors[ImGuiCol_FrameBgActive] = frame_act;
  colors[ImGuiCol_TitleBg] = panel;
  colors[ImGuiCol_TitleBgActive] = panel;
  colors[ImGuiCol_TitleBgCollapsed] = panel;
  colors[ImGuiCol_MenuBarBg] = panel;
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_ScrollbarGrab] = frame_hi;
  colors[ImGuiCol_ScrollbarGrabHovered] = frame_act;
  colors[ImGuiCol_ScrollbarGrabActive] = accent;
  colors[ImGuiCol_CheckMark] = accent_hi;
  colors[ImGuiCol_SliderGrab] = accent;
  colors[ImGuiCol_SliderGrabActive] = accent_hi;
  colors[ImGuiCol_Button] = frame;
  colors[ImGuiCol_ButtonHovered] = frame_hi;
  colors[ImGuiCol_ButtonActive] = frame_act;
  colors[ImGuiCol_Header] = rgb(46, 66, 70);
  colors[ImGuiCol_HeaderHovered] = rgb(56, 82, 86);
  colors[ImGuiCol_HeaderActive] = rgb(64, 96, 100);
  colors[ImGuiCol_Separator] = border;
  colors[ImGuiCol_SeparatorHovered] = accent;
  colors[ImGuiCol_SeparatorActive] = accent_hi;
  colors[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_ResizeGripHovered] = accent;
  colors[ImGuiCol_ResizeGripActive] = accent_hi;
  colors[ImGuiCol_Tab] = panel;
  colors[ImGuiCol_TabHovered] = frame_hi;
  colors[ImGuiCol_TabSelected] = frame;
  colors[ImGuiCol_TabSelectedOverline] = accent;
  colors[ImGuiCol_TabDimmed] = panel;
  colors[ImGuiCol_TabDimmedSelected] = frame;
  colors[ImGuiCol_TabDimmedSelectedOverline] = border;
  colors[ImGuiCol_TableHeaderBg] = panel;
  colors[ImGuiCol_TableBorderStrong] = border;
  colors[ImGuiCol_TableBorderLight] = rgb(40, 44, 53);
  colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  colors[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.025f);
  colors[ImGuiCol_NavCursor] = accent;
  colors[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.5f);
}

// A combo over a list of labels; returns true when the selection changed.
bool combo(const char* id, int& index, const std::vector<std::string>& items) {
  bool changed = false;
  const char* preview = index >= 0 && static_cast<size_t>(index) < items.size() ? items[static_cast<size_t>(index)].c_str() : "-";
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::BeginCombo(id, preview)) {
    for (size_t i = 0; i < items.size(); ++i) {
      const bool sel = static_cast<int>(i) == index;
      if (ImGui::Selectable(items[i].c_str(), sel)) {
        changed = static_cast<int>(i) != index;
        index = static_cast<int>(i);
      }
      if (sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

std::string driver_label(const std::string& driver) {
  if (driver == "vulkan") return "Vulkan";
  if (driver == "direct3d12") return "Direct3D 12";
  if (driver == "metal") return "Metal";
  return driver;
}

// Art modes offered in the Graphics tab (label, mode). A new art mode is one line here.
struct ArtModeEntry {
  const char* label;
  platform::ArtMode mode;
};
constexpr ArtModeEntry kArtModes[] = {
    {"HD (Radical Dreamers Edition)", platform::ArtMode::Hd},
    {"Original (PS1 / SD)", platform::ArtMode::Original},
    {"Modded (HD + mods)", platform::ArtMode::Modded},
};

bool filter_row(const char* label, platform::TexFilter& filter,
                const char* help = "Sampling of 3D model textures: nearest keeps texels square, bilinear smooths them.") {
  ImGuiLayer::row(label, help);
  int selected = filter == platform::TexFilter::Bilinear ? 1 : 0;
  ImGui::PushID(label);
  bool changed = false;
  if (ImGui::RadioButton("Nearest", &selected, 0)) changed = true;
  ImGui::SameLine(0, 24);
  if (ImGui::RadioButton("Bilinear", &selected, 1)) changed = true;
  ImGui::PopID();
  if (changed) filter = selected == 1 ? platform::TexFilter::Bilinear : platform::TexFilter::Nearest;
  return changed;
}

// Two side-by-side columns when the tab body is wide enough, else the two stacked. Each column is a callable that draws sections.
// kTwoColumnW is in pixels at UI scale 1.
constexpr float kTwoColumnW = 820.0f;
template <class L, class R>
void two_columns(float scale, L&& left, R&& right) {
  if (ImGui::GetContentRegionAvail().x >= kTwoColumnW * scale &&
      ImGui::BeginTable("cols", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID("L");
    left();
    ImGui::PopID();
    ImGui::TableNextColumn();
    ImGui::PushID("R");
    right();
    ImGui::PopID();
    ImGui::EndTable();
  } else {
    left();
    right();
  }
}

}  // namespace

bool ImGuiLayer::init(GpuContext& ctx, SDL_Window* window) {
  active_driver_ = ctx.driver();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;

  // A readable UI font: Segoe UI from Windows when present (ImGui's built-in 13 px bitmap font otherwise).
  if (const char* windir = std::getenv("WINDIR")) {
    const std::filesystem::path font = std::filesystem::path(windir) / "Fonts" / "segoeui.ttf";
    std::error_code ec;
    if (std::filesystem::exists(font, ec)) custom_font_ = io.Fonts->AddFontFromFileTTF(font.string().c_str(), kFontRaster) != nullptr;
  }
  if (!custom_font_) io.Fonts->AddFontDefault();

  if (!ImGui_ImplSDL3_InitForSDLGPU(window)) {
    log::error("ImGui_ImplSDL3_InitForSDLGPU failed");
    return false;
  }

  ImGui_ImplSDLGPU3_InitInfo info{};
  info.Device = ctx.device();
  info.ColorTargetFormat = ctx.color_format();
  info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
  if (!ImGui_ImplSDLGPU3_Init(&info)) {
    log::error("ImGui_ImplSDLGPU3_Init failed");
    return false;
  }
  initialised_ = true;
  apply_style();
  return true;
}

void ImGuiLayer::shutdown() {
  if (!initialised_) return;
  ImGui_ImplSDLGPU3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  initialised_ = false;
}

void ImGuiLayer::event_hook(const SDL_Event& event, void* user) {
  if (static_cast<ImGuiLayer*>(user)->initialised_) ImGui_ImplSDL3_ProcessEvent(&event);
}

// Style and font size follow the window height (1.0 at 900 px and below, in 0.25 steps up to 2.5).
void ImGuiLayer::apply_style() {
  ImGuiIO& io = ImGui::GetIO();
  const float h = io.DisplaySize.y > 0 ? io.DisplaySize.y : 900.0f;
  const float scale = std::clamp(std::round(h / 900.0f * 4.0f) / 4.0f, 1.0f, 2.5f);
  if (scale == ui_scale_) return;
  ui_scale_ = scale;
  ImGuiStyle& style = ImGui::GetStyle();
  build_style(style);
  style.ScaleAllSizes(scale);
  io.FontGlobalScale = custom_font_ ? kFontPx * scale / kFontRaster : scale;
}

void ImGuiLayer::new_frame() {
  ImGui_ImplSDLGPU3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  apply_style();
  ImGui::NewFrame();
}

void ImGuiLayer::add_tab(std::string name, std::function<void()> draw, int order) {
  for (SettingsTab& tab : tabs_) {
    if (tab.name == name) {
      tab.draw = std::move(draw);
      tab.order = order;
      return;
    }
  }
  tabs_.push_back({std::move(name), std::move(draw), order});
}

void ImGuiLayer::open(std::string_view tab) {
  options_open = true;
  if (!tab.empty()) select_tab_ = std::string(tab);
}

void ImGuiLayer::toggle(std::string_view tab) {
  if (options_open && (tab.empty() || current_tab_ == tab)) {
    options_open = false;
    return;
  }
  open(tab);
}

void ImGuiLayer::section(const char* title) {
  ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y * 0.5f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_SliderGrabActive]);
  ImGui::SeparatorText(title);
  ImGui::PopStyleColor();
}

bool ImGuiLayer::begin_panes(const char* id) {
  const float scale = ImGui::GetStyle().FramePadding.y / 5.0f;
  // Narrow: the panes stack into one column.
  const int cols = ImGui::GetContentRegionAvail().x >= 560.0f * scale ? 2 : 1;
  return ImGui::BeginTable(id, cols, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings);
}

bool ImGuiLayer::begin_rows(const char* id) {
  if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) return false;
  // The style scale (FramePadding.y is 5 at scale 1).
  const float scale = ImGui::GetStyle().FramePadding.y / 5.0f;
  ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, kLabelW * scale);
  ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch);
  return true;
}

void ImGuiLayer::end_rows() { ImGui::EndTable(); }

void ImGuiLayer::row(const char* label, const char* help) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (help && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) tooltip(help);
  ImGui::TableNextColumn();
}

void ImGuiLayer::tooltip(const char* text) {
  if (!text || !*text || !ImGui::BeginTooltip()) return;
  // Wrap long descriptions instead of one line across the screen.
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

OptionsChange ImGuiLayer::general_tab(platform::Config& cfg) {
  OptionsChange change;
  const float scale = ui_scale_ > 0 ? ui_scale_ : 1.0f;
  auto sec_game = [&] {
  section("Game");
  if (begin_rows("game")) {
    row("Language", "Dialogue and menu text language.");
    static const char* const kLangs[] = {"en", "fr", "it", "de", "es", "jp"};
    static const std::vector<std::string> kLangNames = {"English", "Francais", "Italiano", "Deutsch", "Espanol", "Japanese"};
    int li = 0;
    for (int i = 0; i < 6; ++i)
      if (cfg.language == kLangs[i]) li = i;
    if (combo("##lang", li, kLangNames)) {
      cfg.language = kLangs[li];
      change.language = true;
    }
    row("Auto-save", "RDE's auto-save (card 1, slot 0) on every location change.");
    if (ImGui::Checkbox("##autosave", &cfg.auto_save)) change.other = true;
    row("Steam Connection", "Connects to Steam as the Radical Dreamers Edition, so Steam shows you playing it and its achievements unlock. Needs Steam running. "
                 "Turning it off stops achievements at once; Steam keeps showing you playing until OpenCross closes.");
    if (ImGui::Checkbox("##steam", &cfg.steam)) change.other = true;
    row("Session", "Restart goes back to the start of the game without closing OpenCross. Quit closes OpenCross. Either way, progress since your "
                   "last save is lost.");
    if (ImGui::Button("Restart##session")) change.restart_game = true;
    ImGui::SameLine();
    if (ImGui::Button("Quit##session")) change.quit = true;
    end_rows();
  }

  };
  auto sec_audio = [&] {
  section("Audio");
  if (begin_rows("audio")) {
    auto volume = [&](const char* label, float& value, const char* help) {
      row(label, help);
      ImGui::PushID(label);
      ImGui::SetNextItemWidth(-FLT_MIN);
      int pct = static_cast<int>(std::lround(value * 100.0f));
      if (ImGui::SliderInt("##v", &pct, 0, 100, "%d%%")) {
        value = static_cast<float>(pct) / 100.0f;
        change.audio = true;
      }
      ImGui::PopID();
    };
    volume("Master volume", cfg.master, "Overall loudness of everything: music, sound effects and voices.");
    volume("Music volume", cfg.music, "Loudness of the music only.");
    volume("Sound effects volume", cfg.sfx, "Loudness of the sound effects only (footsteps, menus, battle).");
    row("Voices", "Plays the voice lines of the voice packs in your mods folders (CrossFix layout: mods/<pack>/voices/<scene>/<dialog>-<page>-<name>.mp3). "
                  "Battle lines: voices/battle/<battle>/<msg>-<page>-<unit>.mp3.");
    if (ImGui::Checkbox("##voices", &cfg.voices)) change.audio = true;
    row("Voice volume", "Loudness of the voice lines, on top of the master volume.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##voicevol", &cfg.voice_volume, 0, 100, "%d%%")) change.audio = true;
    row("Music source", "Remastered: RDE's arranged soundtrack. Original: the PS1 sequences played by the built-in synthesiser.");
    static const std::vector<std::string> kBgm = {"Remastered", "Original (PS1)"};
    int bgm = cfg.bgm_source == "original" ? 1 : 0;
    if (combo("##bgm", bgm, kBgm)) {
      cfg.bgm_source = bgm == 1 ? "original" : "remastered";
      change.bgm_source = true;
    }
    end_rows();
  }

  };
  auto sec_controls = [&] {
  section("Controls");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    tooltip("The keyboard shortcuts the game itself listens to (pad buttons follow the RDE layout). Esc (or holding the pad's Start) opens "
            "this window; the others work in the field and in battle.");
  // One key per row.
  if (begin_rows("controls")) {
    const char* keys[][2] = {{"Esc", "Settings (pad: hold Start)"},      {"Space", "Start: pause (pad: Start)"},
                             {"K", "Select: change the lead character; held in battle, hide the overlays (pad: Back / View)"},
                             {"F1 / F2", "Slow / Fast mode (pad: LT / RT)"},
                             {"F3", "No encounters (pad: L3)"},  {"F4", "Battle boost (pad: R3)"},
                             {"F5", "Auto battle (pad: L3 + R3)"}, {"F6", "Art mode"},
                             {"F7", "Widescreen"},                {"F8", "Debug tab"},
                             {"F9", "Battle camera overlay"}};
    for (const auto& shortcut : keys) {
      row(shortcut[0], "Keyboard shortcut (and pad button).");
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("%s", shortcut[1]);
    }
    end_rows();
  }
  };
  two_columns(
      scale, [&] { sec_game(); sec_audio(); }, [&] { sec_controls(); });
  return change;
}

OptionsChange ImGuiLayer::graphics_tab(platform::Config& cfg) {
  OptionsChange change;
  if (!startup_driver_) startup_driver_ = cfg.gpu_driver;
  // Monitor / resolution lists are queried from SDL a few times a second, not every frame.
  static std::vector<std::string> displays;
  // sys_modes: the display's own modes (aspect "System").
  static std::vector<platform::Window::Resolution> modes, sys_modes;
  static int cached_display = -1, cached_mode = -1, refresh = 0;
  const int mode = static_cast<int>(cfg.window_mode);
  if (cached_display != cfg.display || cached_mode != mode || --refresh <= 0) {
    displays = platform::Window::display_names();
    modes = platform::Window::resolutions(cfg.display, cfg.window_mode == platform::WindowMode::Exclusive);
    sys_modes = platform::Window::resolutions(cfg.display, true);
    cached_display = cfg.display;
    cached_mode = mode;
    refresh = 120;
  }

  const float scale = ui_scale_ > 0 ? ui_scale_ : 1.0f;
  auto sec_display = [&] {
  section("Display");
  if (begin_rows("display")) {
    row("Window mode", "Windowed: a fixed-size window (the size below). Borderless: fullscreen at the desktop resolution. "
                       "Exclusive: fullscreen with its own display mode (the size below).");
    static const std::vector<std::string> kModes = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
    int wm = mode;
    if (combo("##wmode", wm, kModes)) {
      cfg.window_mode = static_cast<platform::WindowMode>(wm);
      change.window = true;
    }

    row("Monitor", "The display the game window is shown on (fullscreen modes use this display).");
    int di = std::clamp(cfg.display, 0, static_cast<int>(displays.size()) - 1);
    if (combo("##display", di, displays)) {
      cfg.display = di;
      change.window = true;
    }

    row("Aspect ratio", "Filters the Resolution list below to sizes of this shape (16:10 includes 8:5; 21:9 includes the 2560 x 1080 / 3440 x 1440 "
                        "ultrawides). System: every mode the display itself offers, whatever its shape (phones, TVs, streaming devices). "
                        "Picking a shape the current size does not match switches to the nearest listed size.");
    {
      static const std::vector<std::string> kAspectNames(std::begin(platform::kAspects), std::end(platform::kAspects));
      int ai = 0;
      for (int i = 0; i < static_cast<int>(kAspectNames.size()); ++i)
        if (cfg.aspect == kAspectNames[static_cast<size_t>(i)]) ai = i;
      if (combo("##aspect", ai, kAspectNames)) {
        cfg.aspect = kAspectNames[static_cast<size_t>(ai)];
        if (!platform::aspect_matches(cfg.aspect, cfg.width, cfg.height) && cfg.window_mode != platform::WindowMode::Borderless) {
          // The listed size of that shape nearest in pixel count.
          long best = -1;
          for (const auto& resolution : cfg.aspect == "System" ? sys_modes : modes)
            if (platform::aspect_matches(cfg.aspect, resolution.w, resolution.h)) {
              const long diff = std::labs(static_cast<long>(resolution.w) * resolution.h - static_cast<long>(cfg.width) * cfg.height);
              if (best < 0 || diff < best) best = diff, cfg.width = resolution.w, cfg.height = resolution.h;
            }
          if (best >= 0) change.window = true;
        }
        change.other = true;
      }
    }
    row("Resolution", "Window size (windowed) or display mode (exclusive) of the aspect ratio above. Borderless always uses the desktop resolution.");
    if (cfg.window_mode == platform::WindowMode::Borderless) {
      const platform::Window::Resolution desktop = platform::Window::desktop_size(cfg.display);
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("%d x %d (desktop)", desktop.w, desktop.h);
    } else {
      std::vector<std::string> labels;
      int sel = -1;
      // The modes of the chosen aspect ratio.
      std::vector<platform::Window::Resolution> shown;
      for (const auto& resolution : cfg.aspect == "System" ? sys_modes : modes)
        if (platform::aspect_matches(cfg.aspect, resolution.w, resolution.h)) shown.push_back(resolution);
      // (a local list: the cached `modes` must stay complete - an earlier version swapped it and the next aspect found no sizes)
      for (size_t i = 0; i < shown.size(); ++i) {
        char buf[48];
        const int w = shown[i].w, h = shown[i].h;
        const int gcd = [](int lhs, int rhs) { while (rhs) { const int rem = lhs % rhs; lhs = rhs; rhs = rem; } return lhs; }(w, h);
        const int aw = gcd ? w / gcd : 0, ah = gcd ? h / gcd : 0;
        if (aw == 8 && ah == 5) std::snprintf(buf, sizeof buf, "%d x %d  (16:10)", w, h);
        else if (aw <= 21 && ah <= 9) std::snprintf(buf, sizeof buf, "%d x %d  (%d:%d)", w, h, aw, ah);
        else std::snprintf(buf, sizeof buf, "%d x %d", w, h);
        labels.emplace_back(buf);
        if (w == cfg.width && h == cfg.height) sel = static_cast<int>(i);
      }
      // A size from the ini that is not in the list.
      if (sel < 0) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%d x %d  (custom)", cfg.width, cfg.height);
        labels.insert(labels.begin(), buf);
        shown.insert(shown.begin(), {cfg.width, cfg.height});
        sel = 0;
      }
      if (combo("##res", sel, labels)) {
        cfg.width = shown[static_cast<size_t>(sel)].w;
        cfg.height = shown[static_cast<size_t>(sel)].h;
        change.window = true;
      }
    }

    row("Render scale", "Resolution of the 3D pictures (field, battle, title sea, world map) as a multiple of the window's: above 1x "
                        "supersampled and filtered down (smoother edges, more GPU time), below 1x drawn smaller and scaled up. Menus, "
                        "text and the HUD stay at the window's resolution. With PS1-resolution 3D (Retro look): the multiple of the PS1 "
                        "picture instead (1x = 320 x 224, 2x = 640 x 448, ...).");
    static const std::vector<std::string> kScaleNames = {"0.5x", "0.75x", "1x", "1.5x", "2x", "3x", "4x"};
    int si = 2;
    for (int i = 0; i < static_cast<int>(std::size(platform::Config::kRenderScales)); ++i)
      if (cfg.render_scale == platform::Config::kRenderScales[i]) si = i;
    if (combo("##rscale", si, kScaleNames)) {
      cfg.render_scale = platform::Config::kRenderScales[si];
      change.other = true;
    }

    row("Antialiasing", "Multisample antialiasing (MSAA) of the 3D pictures and the 2D drawn over them: smooths polygon edges, more "
                        "samples = smoother, more GPU time and memory. The PS1-resolution pictures (Retro look) are not antialiased. A "
                        "high render scale already supersamples the 3D: the samples are lowered there (at 4x, to none) to stay within "
                        "the memory of an 8x 4K picture.");
    {
      static const std::vector<std::string> kMsaaNames = {"Off", "2x MSAA", "4x MSAA", "8x MSAA"};
      int mi = 0;
      for (int i = 0; i < static_cast<int>(std::size(platform::Config::kMsaaLevels)); ++i)
        if (cfg.msaa == platform::Config::kMsaaLevels[i]) mi = i;
      if (combo("##msaa", mi, kMsaaNames)) {
        cfg.msaa = platform::Config::kMsaaLevels[mi];
        change.msaa = true;
      }
    }

    row("Frame rate limit", "Upper bound on rendered frames per second (the game itself always runs at 60 ticks per second; "
                            "frames in between are interpolated).");
    static const int kCaps[] = {0, 30, 60, 75, 120, 144, 165, 240};
    static const std::vector<std::string> kCapNames = {"Unlimited", "30", "60", "75", "120", "144", "165", "240"};
    int ci = 0;
    for (int i = 0; i < 8; ++i)
      if (cfg.frame_cap == kCaps[i]) ci = i;
    if (combo("##cap", ci, kCapNames)) {
      cfg.frame_cap = kCaps[ci];
      change.other = true;
    }
    row("VSync", "Wait for the display refresh: no tearing.");
    if (ImGui::Checkbox("##vsync", &cfg.vsync)) change.vsync = true;
    row("Backend", "The graphics API the game renders with. Auto lets the game choose. A change applies after a restart.");
    {
      static const std::vector<std::string> kDrivers = GpuContext::available_drivers();
      std::vector<std::string> labels = {"Auto"};
      int selected = 0;
      for (size_t i = 0; i < kDrivers.size(); ++i) {
        labels.push_back(driver_label(kDrivers[i]));
        if (kDrivers[i] == cfg.gpu_driver) selected = static_cast<int>(i) + 1;
      }
      if (combo("##driver", selected, labels)) {
        cfg.gpu_driver = selected == 0 ? std::string() : kDrivers[static_cast<size_t>(selected) - 1];
        change.other = true;
      }
      ImGui::TextDisabled("In use: %s", driver_label(active_driver_).c_str());
      if (cfg.gpu_driver != *startup_driver_) {
        ImGui::SameLine();
        if (ImGui::Button("Restart now") && on_restart) on_restart();
      }
    }
    end_rows();
  }

  };
  auto sec_wide = [&] {
  section("Widescreen");
  if (begin_rows("wide")) {
    row("Widescreen", "Fills the window's width with more of the field, world map, battle and title sea (F7 toggles). Off = 4:3 with "
                      "black bars. Menus, dialogue, the title logo / menu and movies stay 4:3.");
    if (ImGui::Checkbox("##wide", &cfg.widescreen)) change.widescreen = true;
    row("Edge fade", "Widescreen only: a room narrower than the window ends in a hard cut to black. This fades the room's edge into "
                     "black over the given number of picture pixels (0 = off, the straight cut).");
    ImGui::BeginDisabled(!cfg.widescreen);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##edgefade", &cfg.wide_edge_fade, 0, 96, cfg.wide_edge_fade == 0 ? "Off" : "%d px")) change.other = true;
    ImGui::EndDisabled();
    row("Field of view", "HORIZONTAL field of view in degrees of the 4:3 picture, for the battle and the title's sea; larger shows more space "
                         "(35 = the PS1 view). Field rooms keep the PS1 view: their painted backgrounds are fixed to it.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##fov", &cfg.fov_deg, 30, 110, "%d deg")) change.other = true;
    row("Wide Menus/HUD", "Widescreen only. 4:3: the battle HUD and the main menu layouts stay on the centred 4:3 canvas. 16:9: the HUD's left "
                            "group (field effect, command / element menus) and the party panels, and the main menu screens, spread out to a "
                            "16:9 frame's width - on an ultrawide window that keeps them closer to the middle. Full (default): out to the window's edges.");
    ImGui::BeginDisabled(!cfg.widescreen);
    {
      static const std::vector<std::string> kWideHud = {"4:3", "16:9", "Full"};
      int wh = std::clamp(cfg.wide_hud, 0, 2);
      if (combo("##widehud", wh, kWideHud)) {
        cfg.wide_hud = wh;
        change.other = true;
      }
    }
    ImGui::EndDisabled();
    end_rows();
  }

  };
  auto sec_filters = [&] {
  section("Texture filtering");
  if (begin_rows("filters")) {
    if (filter_row("Room backgrounds", cfg.filter_rooms,
                   "Sampling of the room's background tiles: nearest keeps the pixel art sharp, bilinear smooths it."))
      change.other = true;
    if (filter_row("Effects", cfg.filter_effects,
                   "Sampling of the effect sprites of the field and of battles (clouds, splashes, particles, spell sheets): bilinear keeps them soft while the "
                   "backgrounds stay nearest."))
      change.other = true;
    if (filter_row("3D models", cfg.filter)) change.filter = true;
    row("Anisotropic filtering", "Keeps the bilinear textures of the 3D models, battle stages and title sea sharp on surfaces seen at a "
                                 "steep angle (floors, walls receding into the distance). No effect on nearest-filtered textures.");
    {
      static const std::vector<std::string> kAnisotropyNames = {"Off", "2x", "4x", "8x", "16x"};
      int ai = 0;
      for (int i = 0; i < static_cast<int>(std::size(platform::Config::kAnisotropyLevels)); ++i)
        if (cfg.anisotropy == platform::Config::kAnisotropyLevels[i]) ai = i;
      if (combo("##aniso", ai, kAnisotropyNames)) {
        cfg.anisotropy = platform::Config::kAnisotropyLevels[ai];
        change.anisotropy = true;
      }
    }
    end_rows();
  }

  };
  auto sec_art = [&] {
  section("Art");
  if (begin_rows("art")) {
    row("Art mode", "Switches the art live (F6 cycles the art modes); nothing is reloaded or restarted.");
    std::vector<std::string> names;
    int ai = 0;
    for (size_t i = 0; i < std::size(kArtModes); ++i) {
      names.emplace_back(kArtModes[i].label);
      if (kArtModes[i].mode == cfg.art_mode) ai = static_cast<int>(i);
    }
    if (combo("##art", ai, names)) {
      cfg.art_mode = kArtModes[ai].mode;
      change.art_mode = true;
    }
    row("Portraits", "RDE face art set.");
    static const std::vector<std::string> kPortraits = {"2021 (Radical Dreamers Edition)", "1999 (original)"};
    int pt = cfg.portraits == "1999" ? 1 : 0;
    if (combo("##portraits", pt, kPortraits)) {
      cfg.portraits = pt == 1 ? "1999" : "2021";
      change.portraits = true;
    }
    end_rows();
  }

  };
  // Retro look / Enhancements: labelled check boxes in a 2 x 2 grid, the description on hover only.
  auto check_cell = [&](const char* label, const char* help, bool& value) {
    ImGui::TableNextColumn();
    const bool clicked = ImGui::Checkbox(label, &value);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) tooltip(help);
    return clicked;
  };
  auto sec_retro = [&] {
    section("Retro look");
    if (ImGui::BeginTable("retro", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
      if (check_cell("PS1 jitter",
                     "Simulates the PS1's rendering artefacts in the 3D pictures (field models, battle stage and models, effects): projected "
                     "vertices are snapped to whole pixels of the 320 x 224 PS1 picture, so models and floors wobble, and textures are mapped "
                     "without perspective correction, so they swim on large polygons.",
                     cfg.ps1_jitter))
        change.other = true;
      if (check_cell("PS1 resolution",
                     "Everything is drawn at the PS1's resolution and scaled up with sharp pixels, in every art mode: the field and battle 3D "
                     "at 320 x 224 (times the render scale); the menus, shops, title menu, name entry, Continue and party screens at their PS1 "
                     "display size; the battle HUD, battle menus, results, pause screen and the dialogue windows in the 320 x 224 picture. "
                     "Original art uses the PS1's own disc textures, HD / modded art point-sample RDE's art (small text gets rough). Off: the "
                     "window's resolution.",
                     cfg.ps1_res))
        change.ps1_res = true;
      if (check_cell("CRT filter",
                     "A final pass over the whole picture before the settings window and notifications: scanlines, a faint colour mask and a "
                     "soft vignette.",
                     cfg.crt))
        change.other = true;
      ImGui::EndTable();
    }
  };
  // Optional improvements over the PS1 look, independent of the art mode.
  auto sec_enh = [&] {
    section("Enhancements");
    if (ImGui::BeginTable("enh", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
      if (check_cell("Enhanced battle shadows",
                     "Darkens the battle floor and stage objects smoothly with distance (the PS1 depth cue evaluated per vertex), instead of "
                     "one flat step per floor tile. Off: the PS1's stepped look.",
                     cfg.battle_shadows))
        change.enhancements = true;
      if (check_cell("Smooth effect rings",
                     "The blue contact rings of an encounter and the ring / tube shaped spell effects are 32-sided polygons on the PS1 "
                     "(invisible at 320x224). Each segment is split along a smooth curve through the original corners.",
                     cfg.smooth_effect_rings))
        change.enhancements = true;
      if (check_cell("Smooth water line",
                     "Characters wading in water are tinted per triangle on the PS1, so the waterline is a staircase. This cuts the tint at "
                     "the exact water plane instead.",
                     cfg.smooth_water_line))
        change.enhancements = true;
      if (check_cell("Smooth reflections",
                     "Mirror-like model parts (FATE, Lavos, Zoah's armour ...) take their reflection from the vertex normals in whole "
                     "texture texels on the PS1, so a slowly turning body moves its reflection in visible steps. This keeps the fraction, "
                     "so the reflection slides smoothly with the movement. Off: the PS1's stepped look.",
                     cfg.smooth_reflections))
        change.enhancements = true;
      if (check_cell("Enhanced lighting",
                     "Real lighting on top of the PS1's flat colours. Characters (field and battle) get a key light from above and in front "
                     "of the camera, so faces turning away are clearly darker and faces turning toward it brighter. In battle, bright spells "
                     "(fire, beams, flashes) glow: they light up the characters, the floor and the scenery around them in the spell's colour "
                     "while they play. Off: the PS1's flat lighting.",
                     cfg.lighting))
        change.enhancements = true;
      if (check_cell("Edge blending",
                     "Blends the edges where textures meet on the battle stages and the title scene. The hard lines between them are barely "
                     "visible at the PS1's resolution but stand out at higher resolutions and with upscaled textures.",
                     cfg.edge_blend))
        change.enhancements = true;
      if (check_cell("Hide menus during casts",
                     "Hides the battle HUD, menus, field effect ring and message windows while an element, tech or summon plays, as holding "
                     "the pad's Select does. The damage numbers still show.",
                     cfg.autohide_cast_hud))
        change.enhancements = true;
      if (check_cell("Retain battle scenery",
                     "Some battlefields show extra scenery while the camera pans in (the sea, fish and birds on the beach, drifting dust, "
                     "smoke ...) that the game removes once the fight starts and only brings back for the victory pose, most likely to "
                     "spare the PS1's limited performance for the characters and spells. This keeps it for the whole battle. Off: the "
                     "original behaviour.",
                     cfg.retain_battle_scenery))
        change.enhancements = true;
      ImGui::EndTable();
    }
  };
  // Left: the window and how the picture is framed; right: how it is drawn (texture filtering, retro look, enhancements).
  two_columns(
      scale, [&] { sec_display(); sec_wide(); sec_art(); },
      [&] { sec_filters(); sec_retro(); sec_enh(); });
  return change;
}

OptionsChange ImGuiLayer::options_window(platform::Config& cfg, const std::filesystem::path& file) {
  OptionsChange change;
  if (!options_open) {
    // Closed: the next open restores at Appearing.
    opts_scale_ = 0.0f;
    return change;
  }
  tab_changed_ = false;
  const ImGuiIO& io = ImGui::GetIO();
  const float scale = ui_scale_ > 0 ? ui_scale_ : 1.0f;
  // First-open size: ~75 % of the game window (at least the default size), centred; the user can still resize it.
  const ImVec2 size(std::min(std::max(kWindowW * scale, io.DisplaySize.x * 0.75f), io.DisplaySize.x - 16.0f),
                    std::min(std::max(kWindowH * scale, io.DisplaySize.y * 0.78f), io.DisplaySize.y - 16.0f));
  // [Ui] Settings*: the saved window (unscaled logical units, x scale here), clamped so it stays reachable on the current display.
  ImVec2 start_size = size;
  bool have_pos = false;
  ImVec2 start_pos;
  if (cfg.settings_w >= 0 && cfg.settings_h >= 0) {
    start_size = ImVec2(std::clamp(static_cast<float>(cfg.settings_w) * scale, 360.0f * scale, std::max(360.0f * scale, io.DisplaySize.x - 16.0f)),
                        std::clamp(static_cast<float>(cfg.settings_h) * scale, 240.0f * scale, std::max(240.0f * scale, io.DisplaySize.y - 16.0f)));
  }
  if (cfg.settings_x >= 0 && cfg.settings_y >= 0) {
    have_pos = true;
    start_pos = ImVec2(std::clamp(static_cast<float>(cfg.settings_x) * scale, 0.0f, std::max(0.0f, io.DisplaySize.x - start_size.x)),
                       std::clamp(static_cast<float>(cfg.settings_y) * scale, 0.0f, std::max(0.0f, io.DisplaySize.y - start_size.y)));
  }
  if (!tab_restored_) {
    tab_restored_ = true;
    if (select_tab_.empty()) select_tab_ = cfg.settings_tab;
  }
  // A UI-scale or display-size change while the window is open (a resolution switch): the saved logical size / position (written last
  // frame at the old scale) is re-applied x the new scale and clamped, not only at ImGuiCond_Appearing.
  const bool rescale = opts_scale_ > 0.0f && (opts_scale_ != scale || opts_disp_w_ != io.DisplaySize.x || opts_disp_h_ != io.DisplaySize.y);
  opts_scale_ = scale;
  opts_disp_w_ = io.DisplaySize.x, opts_disp_h_ = io.DisplaySize.y;
  const ImGuiCond cond = rescale ? ImGuiCond_Always : ImGuiCond_Appearing;
  ImGui::SetNextWindowSize(start_size, cond);
  if (have_pos) ImGui::SetNextWindowPos(start_pos, cond);
  else ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), cond, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f * scale, 240.0f * scale), ImVec2(FLT_MAX, FLT_MAX));
  if (ImGui::Begin("Settings", &options_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
    // Built-in tabs first, then the registered ones, all by order.
    struct Entry {
      const char* name;
      int order;
      // builtin: 1 General, 2 Graphics, 0 registered (index in tabs_).
      int builtin;
      size_t index;
    };
    std::vector<Entry> list = {{"General", 0, 1, 0}, {"Graphics", 10, 2, 0}};
    for (size_t i = 0; i < tabs_.size(); ++i) list.push_back({tabs_[i].name.c_str(), tabs_[i].order, 0, i});
    std::stable_sort(list.begin(), list.end(), [](const Entry& lhs, const Entry& rhs) { return lhs.order < rhs.order; });

    if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_NoTooltip)) {
      for (const Entry& entry : list) {
        const ImGuiTabItemFlags flags = select_tab_ == entry.name ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (!ImGui::BeginTabItem(entry.name, nullptr, flags)) continue;
        current_tab_ = entry.name;
        if (ImGui::BeginChild("body", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
          ImGui::PushID(entry.name);
          if (entry.builtin == 1) change = general_tab(cfg);
          else if (entry.builtin == 2) change = graphics_tab(cfg);
          else if (tabs_[entry.index].draw) tabs_[entry.index].draw();
          ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    select_tab_.clear();
    // Remember the window (logical = pixels / scale) and the shown tab; written once the mouse is released.
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    const int nx = static_cast<int>(std::lround(wp.x / scale)), ny = static_cast<int>(std::lround(wp.y / scale));
    const int nw = static_cast<int>(std::lround(ws.x / scale)), nh = static_cast<int>(std::lround(ws.y / scale));
    if (nx != cfg.settings_x || ny != cfg.settings_y || nw != cfg.settings_w || nh != cfg.settings_h) {
      cfg.settings_x = std::max(0, nx), cfg.settings_y = std::max(0, ny), cfg.settings_w = nw, cfg.settings_h = nh;
      geom_dirty_ = true;
    }
    if (cfg.settings_tab != current_tab_) {
      cfg.settings_tab = current_tab_;
      geom_dirty_ = true;
    }
  }
  ImGui::End();
  if (tab_changed_) change.other = true;
  if (geom_dirty_ && !ImGui::IsMouseDown(0)) {
    geom_dirty_ = false;
    // Same save path as every other setting.
    change.other = true;
  }
  if (change.any() && !cfg.save(file)) log::warn("could not write {}", file.string());
  return change;
}

void ImGuiLayer::toast(std::string text) {
  if (!initialised_) return;
  const std::string key = text.substr(0, text.find(':'));
  const double now = ImGui::GetTime();
  for (Toast& item : toasts_) {
    if (item.text.substr(0, item.text.find(':')) == key) {
      item.text = std::move(text);
      item.born = now;
      return;
    }
  }
  toasts_.push_back({std::move(text), now});
  if (toasts_.size() > 4) toasts_.erase(toasts_.begin());
}

void ImGuiLayer::draw_toasts() {
  constexpr double kLife = 1.8, kIn = 0.15, kOut = 0.45;
  const double now = ImGui::GetTime();
  std::erase_if(toasts_, [&](const Toast& item) { return now - item.born >= kLife; });
  if (toasts_.empty()) return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  const float pad = ImGui::GetFontSize() * 0.6f;
  float y = pad;
  for (const Toast& item : toasts_) {
    const double age = now - item.born;
    const float a = static_cast<float>(std::clamp(std::min(age / kIn, (kLife - age) / kOut), 0.0, 1.0));
    const ImVec2 ts = ImGui::CalcTextSize(item.text.c_str());
    const ImVec2 p1(disp.x - pad, y + ts.y + pad), p0(p1.x - ts.x - pad * 1.6f, y);
    dl->AddRectFilled(p0, p1, IM_COL32(16, 18, 28, static_cast<int>(210 * a)), pad * 0.5f);
    dl->AddRect(p0, p1, IM_COL32(120, 140, 200, static_cast<int>(160 * a)), pad * 0.5f);
    dl->AddText(ImVec2(p0.x + pad * 0.8f, p0.y + pad * 0.5f), IM_COL32(235, 238, 250, static_cast<int>(255 * a)), item.text.c_str());
    y = p1.y + pad * 0.4f;
  }
}

void ImGuiLayer::render(const Frame& frame) {
  draw_toasts();
  ImGui::Render();
  ImDrawData* draw_data = ImGui::GetDrawData();
  Imgui_ImplSDLGPU3_PrepareDrawData(draw_data, frame.ctx->upload_cmd());
  frame.ctx->begin_overlay();
  ImGui_ImplSDLGPU3_RenderDrawData(draw_data, frame.ctx->cmd(), frame.ctx->pass());
}

}  // namespace oc::render
