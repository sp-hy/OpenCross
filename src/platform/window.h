#pragma once
// SDL3 window, event pump, input snapshot and frame timing.
#include <array>
#include <string>
#include <vector>

#include "core/types.h"
#include "platform/rumble.h"

struct SDL_Window;
struct SDL_Gamepad;
union SDL_Event;

namespace oc::platform {

// Input state refreshed by Window::pump(). *_pressed are edges since the previous pump. The array sizes cover SDL's scancode, gamepad
// button and axis counts (checked in window.cpp); pad_axis holds sticks as -1..1 and triggers as 0..1.
struct Input {
  static constexpr int kKeys = 512;
  static constexpr int kButtons = 32;
  static constexpr int kAxes = 8;
  std::array<bool, kKeys> key_down{};
  std::array<bool, kKeys> key_pressed{};
  bool pad_connected = false;
  std::array<bool, kButtons> pad_down{};
  std::array<bool, kButtons> pad_pressed{};
  std::array<float, kAxes> pad_axis{};
};

// The left stick as the d-pad: a direction tilted past half way holds that d-pad button and presses it on the first call it does, so a
// screen moves its cursor with the stick at its own d-pad repeat. `held` keeps the stick's directions (up, down, left, right) between
// calls.
void stick_to_dpad(Input& input, std::array<bool, 4>& held);

class Window {
 public:
  using EventHook = void (*)(const SDL_Event&, void* user);

  Window() = default;
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  // The window is never user-resizable: its size comes from the video settings (apply_video).
  bool create(const char* title, int width, int height, int window_mode, int display);
  // Tools / demos: windowed or borderless fullscreen on the first monitor.
  bool create(const char* title, int width, int height, bool fullscreen) { return create(title, width, height, fullscreen ? 1 : 0, 0); }
  void destroy();

  // Window mode (platform::WindowMode as int: 0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen), size (windowed size /
  // exclusive mode; borderless uses the desktop size) and monitor index. Live: the caller recreates the swapchain afterwards.
  // Sizes are PHYSICAL pixels at any Windows display scaling (SDL window coordinates are converted with the window's pixel density;
  // resolutions() / desktop_size() report pixels too).
  void apply_video(int window_mode, int width, int height, int display);

  struct Resolution {
    int w = 0, h = 0;
  };
  // Connected monitors (index = the [Video] Display value).
  static std::vector<std::string> display_names();
  // The monitor's exclusive modes (refresh rates merged), plus common sizes that fit it unless `modes_only`; largest first, unique.
  static std::vector<Resolution> resolutions(int display, bool modes_only);
  // Desktop size of a monitor (0x0 if unknown).
  static Resolution desktop_size(int display);
  // True once after the window's pixel size changed (mode switch, monitor move): recreate the swapchain.
  bool take_resized() {
    const bool was_resized = resized_;
    resized_ = false;
    return was_resized;
  }

  // Called for every raw SDL event (e.g. by the UI layer) before the engine looks at it.
  void set_event_hook(EventHook hook, void* user) {
    hook_ = hook;
    hook_user_ = user;
  }

  // Drains events and refreshes `input`. Returns false once the user asked to quit.
  bool pump(Input& input);

  SDL_Window* handle() const { return window_; }
  void pixel_size(int& w, int& h) const;
  // Drives the open gamepad's motors (SDL_RumbleGamepad). No-op without a pad or while the window is unfocused (RDE gates
  // XInputSetState on the same flag). Call once per presented frame; repeats the request before SDL's duration lapses.
  void set_rumble(const RumbleOut& out);

 private:
  void open_gamepad();

  SDL_Window* window_ = nullptr;
  bool resized_ = false;
  bool windowed_ = true;
  // The windowed client size in physical pixels (apply_video), re-applied when the display scale changes.
  int req_w_ = 0, req_h_ = 0;
  SDL_Gamepad* pad_ = nullptr;
  RumbleOut rumble_{};
  unsigned long long rumble_sent_ms_ = 0;
  EventHook hook_ = nullptr;
  void* hook_user_ = nullptr;
};

// Seconds since the previous tick() call, from the high-resolution counter.
class FrameTimer {
 public:
  FrameTimer();
  double tick();

 private:
  u64 last_;
  u64 freq_;
};

// [Video] FrameCap: sleeps at the end of a frame so frames start at most `fps` times a second (0 = no limit).
class FrameLimiter {
 public:
  void wait(int fps);

 private:
  u64 next_ns_ = 0;
};

// Fixed 60 Hz simulation accumulator. advance(dt) returns how many ticks to simulate this frame and the
// interpolation alpha (0..1) of the remainder, for rendering between the last two simulated states. At most kMaxTicks
// ticks per frame: after a stall, time is dropped instead of spiralling.
class FixedStep {
 public:
  static constexpr double kHz = 60.0;
  static constexpr int kMaxTicks = 5;

  struct Result {
    int ticks;
    float alpha;
  };
  Result advance(double dt);

 private:
  double acc_ = 0.0;
};

}  // namespace oc::platform
