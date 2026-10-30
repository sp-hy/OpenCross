#include "platform/window.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "core/log.h"

namespace oc::platform {

static_assert(Input::kKeys >= SDL_SCANCODE_COUNT);
static_assert(Input::kButtons >= SDL_GAMEPAD_BUTTON_COUNT);
static_assert(Input::kAxes >= SDL_GAMEPAD_AXIS_COUNT);

namespace {

// SDL display id of a [Video] Display index (out of range -> the last monitor; none -> the primary).
SDL_DisplayID display_id(int index) {
  int count = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&count);
  SDL_DisplayID id = 0;
  if (ids && count > 0) id = ids[std::clamp(index, 0, count - 1)];
  SDL_free(ids);
  return id ? id : SDL_GetPrimaryDisplay();
}

// Pixels per window coordinate of the window (1 where SDL's window coordinates are pixels; 1.5 for a scaled 150 % display where they are
// points).
float pixel_density(SDL_Window* win) {
  const float density = SDL_GetWindowPixelDensity(win);
  return density > 0.0f ? density : 1.0f;
}

// A display mode's size in physical pixels (SDL_DisplayMode w / h are in window coordinates; pixel_density converts).
int mode_px(int coord, float density) { return static_cast<int>(std::lround(static_cast<float>(coord) * (density > 0.0f ? density : 1.0f))); }

// Sizes the windowed client area to `width` x `height` PHYSICAL pixels ([Video] Width / Height mean pixels at any Windows display
// scaling): set in window coordinates via the pixel density, then corrected from the measured pixel size (rounding of a fractional
// density) until the size is the closest the coordinate grid allows.
void set_pixel_size(SDL_Window* win, int width, int height) {
  float density = pixel_density(win);
  SDL_SetWindowSize(win, std::max(1, static_cast<int>(std::lround(width / density))), std::max(1, static_cast<int>(std::lround(height / density))));
  SDL_SyncWindow(win);
  for (int pass = 0; pass < 2; ++pass) {
    int pw = 0, ph = 0, cw = 0, ch = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    SDL_GetWindowSize(win, &cw, &ch);
    if ((pw == width && ph == height) || pw <= 0 || ph <= 0 || cw <= 0 || ch <= 0) break;
    const double sx = static_cast<double>(pw) / cw, sy = static_cast<double>(ph) / ch;
    const int nw = std::max(1, static_cast<int>(std::lround(width / sx))), nh = std::max(1, static_cast<int>(std::lround(height / sy)));
    if (nw == cw && nh == ch) break;
    SDL_SetWindowSize(win, nw, nh);
    SDL_SyncWindow(win);
  }
}

}  // namespace

Window::~Window() { destroy(); }

bool Window::create(const char* title, int width, int height, int window_mode, int display) {
#if defined(_WIN32)
  // Per-monitor DPI aware (SDL3 declares it too; a second call just fails): Windows must not bitmap-stretch the window by the display
  // scaling - the [Video] size is physical pixels and the swapchain renders at them.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    log::error("SDL_Init: {}", SDL_GetError());
    return false;
  }
  // Created hidden, placed / switched to its mode, then shown; the swapchain is created afterwards from the final size.
  // OC_QUIET (test runs): shown without activation and never focusable, so a harness window does not take the keyboard from the desktop.
  const bool quiet = std::getenv("OC_QUIET") != nullptr;
  SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
  if (quiet) {
    flags |= SDL_WINDOW_NOT_FOCUSABLE;
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
  }
  window_ = SDL_CreateWindow(title, width, height, flags);
  if (!window_) {
    log::error("SDL_CreateWindow: {}", SDL_GetError());
    return false;
  }
  apply_video(window_mode, width, height, display);
  SDL_ShowWindow(window_);
  SDL_SyncWindow(window_);
  resized_ = false;
  open_gamepad();
  return true;
}

// Leaves fullscreen first so position and size apply to the windowed state and a fullscreen switch picks the monitor the window is
// on. Windowed: Width / Height are physical pixels (a 150 % Windows display does not turn 1280 x 960 into 1920 x 1440), so the window
// moves to the monitor first for its pixel density, is clamped to the usable area in pixels, sized, then centred. Exclusive: the mode
// whose PIXEL size is closest to Width x Height (modes list window coordinates x pixel_density), then the highest refresh rate of that
// size; borderless (or no mode found) uses the desktop-size fullscreen window.
void Window::apply_video(int window_mode, int width, int height, int display) {
  if (!window_) return;
  const SDL_DisplayID id = display_id(display);
  const int centred = static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(id));
  SDL_SetWindowFullscreen(window_, false);
  SDL_SyncWindow(window_);
  SDL_SetWindowResizable(window_, false);
  windowed_ = window_mode == 0;
  if (window_mode == 0) {
    SDL_SetWindowBordered(window_, true);
    SDL_SetWindowPosition(window_, centred, centred);
    SDL_SyncWindow(window_);
    const float density = pixel_density(window_);
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(id, &usable) && usable.w > 0 && usable.h > 0) {
      width = std::min(width, mode_px(usable.w, density));
      height = std::min(height, mode_px(usable.h, density));
    }
    req_w_ = std::max(width, 320);
    req_h_ = std::max(height, 240);
    set_pixel_size(window_, req_w_, req_h_);
    SDL_SetWindowPosition(window_, centred, centred);
  } else {
    SDL_SetWindowPosition(window_, centred, centred);
    SDL_SyncWindow(window_);
    SDL_DisplayMode mode{};
    bool have_mode = false;
    if (window_mode == 2) {
      int count = 0;
      if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(id, &count)) {
        long best = -1;
        for (int i = 0; i < count; ++i) {
          if (!modes[i]) continue;
          const long dx = mode_px(modes[i]->w, modes[i]->pixel_density) - width, dy = mode_px(modes[i]->h, modes[i]->pixel_density) - height;
          const long score = dx * dx + dy * dy;
          if (best < 0 || score < best || (score == best && modes[i]->refresh_rate > mode.refresh_rate)) {
            best = score;
            mode = *modes[i];
          }
        }
        have_mode = best >= 0;
        SDL_free(modes);
      }
    }
    if (have_mode) {
      if (!SDL_SetWindowFullscreenMode(window_, &mode)) log::warn("exclusive mode {}x{}: {}", mode.w, mode.h, SDL_GetError());
    } else {
      if (window_mode == 2) log::warn("no exclusive mode near {}x{}: borderless instead", width, height);
      SDL_SetWindowFullscreenMode(window_, nullptr);
    }
    if (!SDL_SetWindowFullscreen(window_, true)) log::warn("fullscreen: {}", SDL_GetError());
  }
  SDL_SyncWindow(window_);
  resized_ = true;
  int pw = 0, ph = 0;
  pixel_size(pw, ph);
  log::info("window: mode {} display {} -> {}x{} pixels", window_mode, display, pw, ph);
}

std::vector<std::string> Window::display_names() {
  std::vector<std::string> out;
  int count = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&count);
  for (int i = 0; ids && i < count; ++i) {
    const char* name = SDL_GetDisplayName(ids[i]);
    std::string label = std::to_string(i + 1) + ": " + (name && *name ? name : "Display");
    if (const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(ids[i]))
      label += " (" + std::to_string(mode_px(desktop->w, desktop->pixel_density)) + "x" + std::to_string(mode_px(desktop->h, desktop->pixel_density)) + ")";
    out.push_back(std::move(label));
  }
  SDL_free(ids);
  if (out.empty()) out.push_back("1: Display");
  return out;
}

Window::Resolution Window::desktop_size(int display) {
  const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display_id(display));
  return desktop ? Resolution{mode_px(desktop->w, desktop->pixel_density), mode_px(desktop->h, desktop->pixel_density)} : Resolution{};
}

std::vector<Window::Resolution> Window::resolutions(int display, bool modes_only) {
  std::vector<Resolution> out;
  const SDL_DisplayID id = display_id(display);
  int count = 0;
  if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(id, &count)) {
    for (int i = 0; i < count; ++i)
      if (modes[i]) out.push_back({mode_px(modes[i]->w, modes[i]->pixel_density), mode_px(modes[i]->h, modes[i]->pixel_density)});
    SDL_free(modes);
  }
  if (!modes_only) {
    static constexpr Resolution kCommon[] = {{640, 480},   {800, 600},   {960, 720},   {1024, 768},  {1280, 720},  {1280, 960},
                                             {1366, 768},  {1440, 1080}, {1600, 900},  {1600, 1200}, {1920, 1080}, {1920, 1200},
                                             {1920, 1440}, {2560, 1440}, {2560, 1600}, {2880, 2160}, {3840, 2160}};
    const Resolution desk = desktop_size(display);
    for (const Resolution& res : kCommon)
      if (desk.w == 0 || (res.w <= desk.w && res.h <= desk.h)) out.push_back(res);
  }
  std::sort(out.begin(), out.end(), [](const Resolution& lhs, const Resolution& rhs) { return lhs.w != rhs.w ? lhs.w > rhs.w : lhs.h > rhs.h; });
  out.erase(std::unique(out.begin(), out.end(), [](const Resolution& lhs, const Resolution& rhs) { return lhs.w == rhs.w && lhs.h == rhs.h; }),
            out.end());
  return out;
}

void Window::destroy() {
  if (pad_) SDL_CloseGamepad(pad_);
  pad_ = nullptr;
  if (window_) SDL_DestroyWindow(window_);
  window_ = nullptr;
}

void Window::open_gamepad() {
  if (pad_) return;
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  if (ids && count > 0) {
    pad_ = SDL_OpenGamepad(ids[0]);
    if (pad_) log::info("gamepad: {}", SDL_GetGamepadName(pad_));
  }
  SDL_free(ids);
}

// Each SDL request runs kHoldMs; it is refreshed every 250 ms while the motors stay on. A pad without motors keeps the state at rest.
void Window::set_rumble(const RumbleOut& want) {
  RumbleOut out = want;
  if (!pad_ || !window_ || !(SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS)) out = {};
  const Uint64 now = SDL_GetTicks();
  constexpr Uint32 kHoldMs = 400;
  const bool changed = !(out == rumble_);
  if (!changed && (out == RumbleOut{} || now - rumble_sent_ms_ < 250)) return;
  rumble_ = out;
  rumble_sent_ms_ = now;
  if (pad_ && !SDL_RumbleGamepad(pad_, out.low, out.high, out == RumbleOut{} ? 0 : kHoldMs)) rumble_ = {};
  if (changed) log::debug("rumble: output low {} high {}", out.low, out.high);
}

void Window::pixel_size(int& w, int& h) const {
  w = h = 0;
  if (window_) SDL_GetWindowSizeInPixels(window_, &w, &h);
}

bool Window::pump(Input& input) {
  bool running = true;
  input.key_pressed.fill(false);
  input.pad_pressed.fill(false);
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    if (hook_) hook_(ev, hook_user_);
    switch (ev.type) {
      case SDL_EVENT_QUIT:
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED: running = false; break;
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      case SDL_EVENT_WINDOW_DISPLAY_CHANGED: resized_ = true; break;
      case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        // Moved to a monitor with another scaling (or the scaling changed): keep the window's size in physical pixels.
        if (windowed_ && req_w_ > 0 && req_h_ > 0) set_pixel_size(window_, req_w_, req_h_);
        resized_ = true;
        break;
      case SDL_EVENT_KEY_DOWN:
        if (ev.key.scancode < Input::kKeys) {
          if (!ev.key.repeat) input.key_pressed[ev.key.scancode] = true;
          input.key_down[ev.key.scancode] = true;
        }
        break;
      case SDL_EVENT_KEY_UP:
        if (ev.key.scancode < Input::kKeys) input.key_down[ev.key.scancode] = false;
        break;
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (ev.gbutton.button < Input::kButtons) input.pad_pressed[ev.gbutton.button] = true;
        break;
      case SDL_EVENT_GAMEPAD_ADDED: open_gamepad(); break;
      case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad_ && SDL_GetGamepadID(pad_) == ev.gdevice.which) {
          SDL_CloseGamepad(pad_);
          pad_ = nullptr;
          open_gamepad();
        }
        break;
      default: break;
    }
  }
  input.pad_connected = pad_ != nullptr;
  for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button)
    input.pad_down[button] = pad_ && SDL_GetGamepadButton(pad_, static_cast<SDL_GamepadButton>(button));
  for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
    float value = pad_ ? static_cast<float>(SDL_GetGamepadAxis(pad_, static_cast<SDL_GamepadAxis>(axis))) / 32767.0f : 0.0f;
    input.pad_axis[axis] = std::clamp(value, -1.0f, 1.0f);
  }
  return running;
}

void stick_to_dpad(Input& input, std::array<bool, 4>& held) {
  constexpr float kTilt = 0.5f;
  const float stick_x = input.pad_axis[SDL_GAMEPAD_AXIS_LEFTX], stick_y = input.pad_axis[SDL_GAMEPAD_AXIS_LEFTY];
  const bool now[4] = {stick_y < -kTilt, stick_y > kTilt, stick_x < -kTilt, stick_x > kTilt};
  constexpr SDL_GamepadButton kButtons[4] = {SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                                             SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
  for (size_t i = 0; i < 4; ++i) {
    const auto button = static_cast<size_t>(kButtons[i]);
    if (now[i]) input.pad_down[button] = true;
    if (now[i] && !held[i]) input.pad_pressed[button] = true;
    held[i] = now[i];
  }
}

FrameTimer::FrameTimer() : last_(SDL_GetPerformanceCounter()), freq_(SDL_GetPerformanceFrequency()) {}

double FrameTimer::tick() {
  u64 now = SDL_GetPerformanceCounter();
  double dt = static_cast<double>(now - last_) / static_cast<double>(freq_);
  last_ = now;
  return dt;
}

void FrameLimiter::wait(int fps) {
  const u64 now = SDL_GetTicksNS();
  if (fps <= 0) {
    next_ns_ = now;
    return;
  }
  const u64 period = 1000000000ull / static_cast<u64>(fps);
  const bool fell_behind = next_ns_ == 0 || now > next_ns_ + period;
  if (fell_behind) next_ns_ = now;
  next_ns_ += period;
  if (next_ns_ > now) SDL_DelayPrecise(next_ns_ - now);
}

FixedStep::Result FixedStep::advance(double dt) {
  constexpr double step = 1.0 / kHz;
  acc_ += std::min(dt, 0.25);
  int ticks = 0;
  while (acc_ >= step && ticks < kMaxTicks) {
    acc_ -= step;
    ++ticks;
  }
  if (ticks == kMaxTicks) acc_ = std::min(acc_, step);
  return {ticks, static_cast<float>(acc_ / step)};
}

}  // namespace oc::platform
