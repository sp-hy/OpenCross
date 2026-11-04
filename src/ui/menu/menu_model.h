#pragma once
// Hook for the 3D character model of the Status screen (the member's model on the right).
// oc_ui cannot depend on the mesh pipeline / Actor (oc_game depends on oc_ui), so the host supplies a MenuModelView
// (ui/menu/status_model.h in oc_menu_model is the implementation) and MainMenu calls it between the parchment and the rest of its sprites.
#include "core/types.h"
#include "render/gpu_context.h"

namespace oc::ui {

// The Status camera, PS1 units: yaw / pitch 4096 = a turn, distance and pan in model units. The camera looks at the model's feet from
// `distance` away, turned by -yaw about the vertical axis and tilted by -pitch.
struct MenuCamera {
  int yaw = 0;
  int pitch = 0;
  int distance = 2000;
  int pan_x = 0;
  int pan_y = 0;
};

// The camera drawn between two model ticks: `prev` blended into `cur` by `alpha` (0..1), so the turn moves at the drawing rate like the pose.
// Yaw and pitch start from the previous angle within one turn (0..4095) and move by the unwrapped difference.
struct MenuCameraNow {
  float yaw = 0, pitch = 0, distance = 0, pan_x = 0, pan_y = 0;
};

inline MenuCameraNow menu_camera_at(const MenuCamera& prev, const MenuCamera& cur, float alpha) {
  auto mix = [alpha](int a, int b) { return static_cast<float>(a) + static_cast<float>(b - a) * alpha; };
  auto turn = [alpha](int a, int b) { return static_cast<float>(a & 0xFFF) + static_cast<float>(b - a) * alpha; };
  return {turn(prev.yaw, cur.yaw), turn(prev.pitch, cur.pitch), mix(prev.distance, cur.distance), mix(prev.pan_x, cur.pan_x), mix(prev.pan_y, cur.pan_y)};
}

struct MenuModelSlot {
  // Character record id (0 Serge .. 43).
  int char_id = 0;
  // Menu model (menu/player .ome, battle model order) = record byte 0x2D up to 0x32, else the character id; the weapon pack follows
  // the same byte up to 0x31.
  int model = 0;
  int weapon_model = 0;
  // Part of the weapon pack = weapon record tier & 0xF, -1 = no weapon.
  int weapon_part = -1;
  // Constructs not drawn (bit k = construct k).
  u32 hidden_meshes = 0;
  // The character's status bits (record +0xB0): 0x01, 0x08 and 0x20 show their effect around the model.
  u32 status = 0;
  // Window pixels of the PS1 screen point (0, 0) and of one PS1 pixel.
  float origin_x = 0, origin_y = 0;
  float unit_x = 1, unit_y = 1;
  MenuCamera camera;
  // The camera before the last model tick: drawn blended into `camera` over the tick, like the pose.
  MenuCamera camera_prev;
  // Animation of the menu model: 0..2 the square-button poses, 3 hurt.
  int pose = 0;
  // Changes whenever the screen (re)starts the model: the pose then starts without a blend.
  u32 session = 0;
  // 4x textures + linear filtering.
  bool hd = true;
  // The menu model's tick counter (30 Hz, every second 60 Hz menu frame): the pose, the eye blink, the prism lights and the status effects step
  // once per tick, like the camera's auto turn.
  u32 tick = 0;
  // Seconds since the previous draw: the interpolation between two ticks.
  float dt = 0;
};

class MenuModelView {
 public:
  virtual ~MenuModelView() = default;
  // Called with no SpriteBatch pass open, once per frame while the Status screen is shown.
  virtual void draw(const render::Frame& frame, float vw, float vh, const MenuModelSlot& slot) = 0;
  // The nearest camera distance of a menu model (its .ome's own value), 0 when unknown.
  virtual int min_distance(int model) = 0;
};

}  // namespace oc::ui
