#pragma once
// Field camera built from the room's camera block.
//
// The game projects an actor point P as  Pc = RT * P / 4096 + TR;  screen = (160 + H * Pc.x / Pc.z, 120 + H * Pc.y / Pc.z)
// in the 320x240 "layer space" the background tiles live in (the background adds the scroll S to its tiles; actors get
// the same S added to the screen position). The engine keeps that exactly: view = [RT/4096 | TR], and the projection
// is an off-centre perspective that maps layer space (+ scroll) onto the letterboxed 4:3 area of the window.
#include <algorithm>
#include <cmath>

#include "assets/room_scene.h"
#include "render/mat4.h"

namespace oc::render {

// Shared depth space of background groups and actors: value = group z, or camera z / 16 for actors; larger = farther. Both are
// divided by kDepthRange to get the 0..1 depth-buffer value.
constexpr float kDepthRange = 8192.0f;
constexpr float kActorDepthDivisor = 16.0f;
// Layer space (the original viewport).
constexpr float kLayerW = 320.0f;
constexpr float kLayerH = 240.0f;

// Field display: the PS1 field engine draws into a 320x224 buffer and shows 216 of its rows (display screen (0, 10, 256, 216)); RDE's
// GP1(07) handler adds 8 lines to a 216-line range, so RDE presents buffer rows 0..223. The present quad fills the game viewport, which
// is the 4:3 fit inset by 8/960 of its height at the top and the bottom ("crop"). In layer units of the 240-line letterbox: rows 0..224
// span [2, 238].
constexpr float kFieldRows = 224.0f;
// = 2 layer units.
constexpr float kFieldCrop = 8.0f * kLayerH / 960.0f;
// Presentation: RDE shows those two crop bands black. Every 4:3 fit here is zoomed so that the 236 rows between the bands fill the
// window's height: the bands fall outside the window, nothing is resampled, and a window at least 320:236 wide (16:9, 16:10, 4:3 + a
// few pixels) loses no picture; a narrower one keeps the bands (width-limited).
// = 236 rows.
constexpr float kPresentRows = kLayerH - 2.0f * kFieldCrop;
inline float present_scale(float vw, float vh, float cols = kLayerW) { return std::min(vw / cols, vh / kPresentRows); }

// Widescreen ([Video] Widescreen, F7): whole layer columns the field / world-map / battle picture shows beyond each side of its 320
// columns (0 = RDE's 4:3 framing). Layer space is unchanged (scripts, scroll, the projection centre 160 keep their 320-wide meaning);
// the extra columns are centred around it. The host sets it once per frame (wide_extra_for) and forces 0 while the title room's 4:3
// layers are composed (the title's 3D sea uses the real value: Letterbox::wide). Menus, dialogue, movies and the HUD canvas use
// Letterbox::fit: always 4:3.
inline float g_wide_extra = 0.0f;
inline float wide_extra() { return g_wide_extra; }
inline void set_wide_extra(float columns) { g_wide_extra = std::max(0.0f, columns); }
// The frame's widescreen columns per side as the window shows them (set once per frame by the host with set_wide_extra; never forced
// to 0 for a 4:3-composed screen): what the screen veils (Letterbox::veil_rect) and the title's 3D sea cover.
inline float g_screen_extra = 0.0f;
inline float screen_extra() { return g_screen_extra; }
inline void set_screen_extra(float columns) { g_screen_extra = std::max(0.0f, columns); }
// [Video] Fov: scale of the GTE projection distance H of the 3D pictures that are not tied to painted backgrounds (battle, the title's sea;
// 1 = the PS1 view). Field rooms keep H (their pre-rendered layers are fixed to it).
inline float g_fov_scale = 1.0f;
inline float fov_scale() { return g_fov_scale; }
inline void set_fov_scale(float scale) { g_fov_scale = std::clamp(scale, 0.1f, 2.0f); }
// The packet-culling frame of the PS1 GTE / GPU rules: SX2 in -1024..1023 and a triangle span <= 1023 x 511 are limits of the PS1's
// 320 x 224 picture. Our picture is (320 + 2 e) columns wide and FovDeg scales H, so the limits are scaled to keep the PS1's ratio of limit to
// picture: the x limits and the x span by k = 1 + e / 160 about the projection centre 160. FovDeg needs no factor: H, the picture's tangent and
// the limit's tangent all scale together, so the limits in our pixels stay (a smaller H already puts the same limit further out in angle).
// e = 0 gives exactly the PS1 constants (-1024, 1023, 1023).
struct CullFrame {
  float x_lo = -1024.0f, x_hi = 1023.0f, x_span = 1023.0f;
  static constexpr float y_lo = -1024.0f, y_hi = 1023.0f, y_span = 511.0f;
};
inline CullFrame cull_frame(float wide_cols) {
  const float widen = 1.0f + std::max(0.0f, wide_cols) / 160.0f;
  CullFrame frame;
  frame.x_lo = 160.0f - 1184.0f * widen;
  frame.x_hi = 160.0f + 863.0f * widen;
  frame.x_span = 1023.0f * widen;
  return frame;
}
// Debug free camera (game/free_cam.h sets it with its on-state): the PS1 packet-drop rules above (SZ3 < 0, H >= 2 * SZ3, FLAG overflow, the SX / SY window,
// the span limits) exist for the shot cameras and clip geometry the free camera flies close to. While set, the stage pipeline hands every packet to the
// GPU's own clipping (near plane at camera z = 0) and the effect quads are clipped against a near plane at z = 16 instead of being dropped.
inline bool g_cull_bypass = false;
inline bool cull_bypass() { return g_cull_bypass; }
inline void set_cull_bypass(bool on) { g_cull_bypass = on; }
inline constexpr float kBypassNear = 16.0f;
// A picture wider than the PS1's (widescreen columns or FovDeg above 35): the stage's near-drop rules (a corner behind the camera, H >= 2 * SZ3, IR / SX
// saturation of a corner far off to the side) drop big floor / wall packets that are partly inside the widened view - the PS1 frame never showed them, ours
// does (a near floor tile popping at the screen edge). The stage pipeline then hands its packets to the GPU's near-plane clip as with the free camera;
// at 4:3 / FovDeg 35 the PS1 rules stay exact. Effects keep their (widened) PS1 rules: their drops are part of the effect's look.
inline bool wide_picture() { return g_wide_extra > 0.0f || g_fov_scale < 1.0f; }
// SX2 / SY2 saturation test (false = FLAG bits 14 / 13: the packet is dropped) and the GPU's triangle size rule, in `frame`.
inline bool cull_screen_ok(const CullFrame& frame, float sx, float sy) {
  return sx >= frame.x_lo && sx <= frame.x_hi && sy >= CullFrame::y_lo && sy <= CullFrame::y_hi;
}
inline bool cull_span_ok(const CullFrame& frame, float dx, float dy) { return dx <= frame.x_span && dy <= CullFrame::y_span; }
// Extra columns per side for a vw x vh window: the window's own shape at the 4:3 fit's height (240 rows), whole columns; 0 for a window
// of 4:3 or narrower (16:9 -> 49, i.e. 418 columns; 16:10 -> 28 = 376; 21:9 -> 115 = 550).
inline float wide_extra_for(float vw, float vh) {
  if (vw <= 0.0f || vh <= 0.0f) return 0.0f;
  return std::max(0.0f, std::floor((kPresentRows * vw / vh - kLayerW) * 0.5f + 1e-3f));
}
// A 4:3 UI canvas (RDE's 1280 x 960, or any cw x ch 4:3 picture: movies, the title pictures) placed as the 4:3 fit above: `s` = scale
// per canvas unit, (ox, oy) = the window origin of canvas (0, 0). Every full-screen 2D layer uses this, so the menus, HUD, dialogue and
// movies land on the same pixels as the field / battle picture (and share its zoom).
struct CanvasFit {
  float s = 1, ox = 0, oy = 0;
};
inline CanvasFit canvas_fit(float vw, float vh, float cw = 1280.0f, float ch = 960.0f) {
  const float scale = std::min(vw / cw, vh / (ch * kPresentRows / kLayerH));
  return {scale, (vw - cw * scale) * 0.5f, (vh - ch * scale) * 0.5f};
}

// RDE's canvas over the field and the world map (the message windows): the 1280 x 960 canvas on RDE's game viewport, the 4:3 fit of the
// window (height h = min(vh, vw * 3 / 4), centred) less a band of 8/960 h at the top and the bottom. x: canvas x -> ox + x sx with
// sx = h / 960. y: canvas 0 is the top of the band-less viewport, whose 944/960 h the 960 rows fill scaled by 928/960 (the 216-line
// display: canvas 960 is display line 216 of the 224 shown), so the canvas is drawn flatter than it is wide: sy = sx * 944/960 * 928/960.
// Not the field picture's own zoom (Letterbox).
struct OverlayFit {
  float sx = 1, sy = 1, ox = 0, oy = 0;
};
inline OverlayFit overlay_fit(float vw, float vh) {
  constexpr float kCanvasW = 1280.0f, kCanvasH = 960.0f, kBand = 8.0f, kBandFree = 944.0f / 960.0f, kDisplayLines = 928.0f / 960.0f;
  const float h = std::min(vh, vw * kCanvasH / kCanvasW);
  OverlayFit fit;
  fit.sx = h / kCanvasH;
  fit.sy = fit.sx * kBandFree * kDisplayLines;
  fit.ox = (vw - kCanvasW * fit.sx) * 0.5f;
  fit.oy = (vh - h) * 0.5f + kBand * fit.sx;
  return fit;
}

// The area of the window that shows layer space (4:3, or wider in widescreen: see field()). `scale` is horizontal; `scale_y` vertical
// (= scale except in the field frame). Layer column x maps to window x `ox + x * scale`; the visible columns are [x0, x0 + w]
// (x0 = -wide_extra(), w = 320 + 2 * wide_extra() in widescreen).
struct Letterbox {
  float ox = 0, oy = 0, scale = 1, scale_y = 1, vw = 320, vh = 240;
  float x0 = 0, w = kLayerW;
  // Window x of the first visible column and the window width of the visible columns.
  float left() const { return ox + x0 * scale; }
  float width() const { return w * scale; }
  // The 240 rows at the 236-row zoom (kPresentRows): rows 2..238 fill the height.
  static Letterbox fit(float vw, float vh) {
    Letterbox box;
    box.scale = present_scale(vw, vh);
    box.scale_y = box.scale;
    box.ox = (vw - kLayerW * box.scale) * 0.5f;
    box.oy = (vh - kLayerH * box.scale) * 0.5f;
    box.vw = vw;
    box.vh = vh;
    return box;
  }
  // The 240-row 4:3 fit with `extra` more columns on each side (columns 0..320 centred; = fit() when 0): the field / battle frame
  // before its crop (field()), the title's 3D sea and the frame the screen veils cover (veil_rect) with extra = screen_extra().
  // Whole columns can leave a few black pixels at the sides when the frame is height-limited (16:9 at 1080p: 418 columns = 1913 px of
  // 1920); the frame then shows the fractional rest of a column on each side so it covers the window's full width (the PS1-resolution
  // targets keep their whole columns and are stretched by that fraction, < 0.3 %).
  static Letterbox wide(float vw, float vh, float extra) {
    Letterbox box = fit(vw, vh);
    if (extra > 0.0f) {
      box.scale = box.scale_y = present_scale(vw, vh, kLayerW + 2.0f * extra);
      box.ox = (vw - kLayerW * box.scale) * 0.5f;
      box.oy = (vh - kLayerH * box.scale) * 0.5f;
      const float cols = std::max(kLayerW + 2.0f * extra, vw / box.scale);
      box.x0 = -(cols - kLayerW) * 0.5f;
      box.w = cols;
    }
    return box;
  }
  // Window rectangle (x, y, w, h) a screen-covering veil (fade, white-out, dim) covers: the 4:3 fit, or in widescreen the whole
  // widened frame (wide() with screen_extra(): every visible column, all 240 rows), also while a 4:3 screen is composed.
  struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
  };
  static Rect veil_rect(float vw, float vh) {
    const Letterbox box = wide(vw, vh, screen_extra());
    return {box.left(), box.oy, box.width(), kLayerH * box.scale_y};
  }
  // The field picture as RDE presents it: field rows 0..kFieldRows stretched into the cropped 4:3 viewport (see kFieldRows). Widescreen:
  // the same rows and height, `wide_extra()` more columns on each side (columns 0..320 stay centred; scale = the 4:3 fit's whenever the
  // window is at least 4:3, so HUD canvas positions still land on the same picture pixels).
  static Letterbox field(float vw, float vh) {
    Letterbox box = wide(vw, vh, wide_extra());
    box.oy += kFieldCrop * box.scale;
    box.scale_y = box.scale * (kLayerH - 2.0f * kFieldCrop) / kFieldRows;
    return box;
  }
  // `scale`:1 mapping onto a w x h render target (the classic-mode PS1-resolution field / battle pass, scale = its multiple of the PS1
  // size): layer columns 0..320 centred, so a target wider than 320 * scale (widescreen: 320 + 2 * wide_extra() columns) shows the extra
  // columns on both sides; 320 * scale wide = the plain identity.
  static Letterbox identity(float w, float h, float scale = 1.0f) {
    Letterbox box;
    box.vw = w;
    box.vh = h;
    box.scale = box.scale_y = scale;
    const float cols = w / scale;
    box.x0 = -std::max(0.0f, (cols - kLayerW) * 0.5f);
    box.ox = -box.x0 * scale;
    box.w = std::max(cols, kLayerW);
    return box;
  }
  // Widescreen: a screen-space primitive spanning all 320 columns (a flash, fade or colour wash) is stretched to the visible width;
  // anything narrower keeps its place. `x` = the primitive's `count` corner x values in layer columns.
  void widen_full_width(float* x, int count) const {
    if (x0 >= 0.0f || count <= 0) return;
    float lo = x[0], hi = x[0];
    for (int k = 1; k < count; ++k) {
      lo = std::min(lo, x[k]);
      hi = std::max(hi, x[k]);
    }
    if (lo > 0.0f || hi < kLayerW) return;
    for (int k = 0; k < count; ++k) x[k] = x[k] <= lo ? std::min(lo, x0) : x[k] >= hi ? std::max(hi, x0 + w) : x[k];
  }
};

class FieldCamera {
 public:
  struct Projected {
    // Layer space, scroll not included.
    float sx = 0, sy = 0;
    // Camera-space depth (game units).
    float z = 0;
  };

  // H is u16 in the file.
  static FieldCamera from_room(const assets::RoomCamera& camera) {
    FieldCamera out;
    for (size_t i = 0; i < 9; ++i) out.rot_[i] = static_cast<float>(camera.matrix[i]) / 4096.0f;
    for (size_t i = 0; i < 3; ++i) out.trans_[i] = static_cast<float>(camera.trans[i]);
    out.proj_h_ = static_cast<float>(static_cast<u16>(camera.proj_h));
    return out;
  }

  float h() const { return proj_h_; }
  Mat4 view() const { return mat4_from_rt(rot_, trans_); }

  // World point -> layer-space screen position + camera depth. False when behind the camera.
  bool project(const assets::Vec3& point, Projected& out) const {
    const float x = rot_[0] * point[0] + rot_[1] * point[1] + rot_[2] * point[2] + trans_[0];
    const float y = rot_[3] * point[0] + rot_[4] * point[1] + rot_[5] * point[2] + trans_[1];
    const float z = rot_[6] * point[0] + rot_[7] * point[1] + rot_[8] * point[2] + trans_[2];
    if (z < 1.0f) return false;
    out = {160.0f + proj_h_ * x / z, 120.0f + proj_h_ * y / z, z};
    return true;
  }

  // Ground-plane (XZ) world directions of the screen's right and up (away from the camera) axes, normalised: world = RT^T *
  // camera-space direction, so camera +x is row 0 of RT and camera +z is row 2.
  void ground_axes(float& right_x, float& right_z, float& up_x, float& up_z) const {
    auto norm = [](float& x, float& z) {
      const float len = std::hypot(x, z);
      if (len > 1e-6f) {
        x /= len;
        z /= len;
      }
    };
    right_x = rot_[0];
    right_z = rot_[2];
    up_x = rot_[6];
    up_z = rot_[8];
    norm(right_x, right_z);
    norm(up_x, up_z);
  }

  // Projection * view for a window `lb` with scroll (sx, sy) in layer pixels. Clip z is the constant 0.5 (inside [0, 1]) and clip w the
  // camera z; the real depth is written per pixel by the mesh shader (camera z / 16 / kDepthRange).
  Mat4 view_proj(float scroll_x, float scroll_y, const Letterbox& lb) const {
    const float ax = 2.0f * lb.scale * proj_h_ / lb.vw;
    const float ay = 2.0f * lb.scale_y * proj_h_ / lb.vh;
    const float cx = (2.0f * (lb.scale * (160.0f + scroll_x) + lb.ox)) / lb.vw - 1.0f;
    const float cy = (2.0f * (lb.scale_y * (120.0f + scroll_y) + lb.oy)) / lb.vh - 1.0f;
    Mat4 proj;
    proj.at(0, 0) = ax;
    proj.at(0, 2) = cx;
    proj.at(0, 3) = 0;
    proj.at(1, 1) = ay;
    proj.at(1, 2) = cy;
    proj.at(1, 3) = 0;
    proj.at(2, 2) = 0.5f;
    proj.at(2, 3) = 0;
    proj.at(3, 2) = 1.0f;
    proj.at(3, 3) = 0;
    return proj * view();
  }

 private:
  assets::Mat3 rot_{1, 0, 0, 0, 1, 0, 0, 0, 1};
  assets::Vec3 trans_{0, 0, 0};
  float proj_h_ = 512;
};

}  // namespace oc::render
