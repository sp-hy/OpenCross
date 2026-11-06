// The scene target into the window, with the field's damage-floor jolt and the encounter feed-back pass (sequence ops 3E / 40):
// out(n) = 0.5 * scene(n) + 0.5 * zoom(out(n - 1)) about the projection centre; the zoom / translation / colour / ABR are the
// sequence's (SeqPost). One step per game tick, repeated frames show the step again. After the battle-start white one strip pass runs
// per update from the update's anchor (the previous update's finished output). The FE 14 frame echo is the same pass with the
// previous output scaled and turned by FE 12 / FE 13 (FieldWorld::frame_echo_point).
#include "app/app.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/log.h"
#include "game/encounter_strips.h"

namespace oc::app {

// The picture rectangle of one feed-back pass: the scene target size, target pixels per window pixel, the zoom centres and the room
// picture's rectangle (window pixels) with its texture coordinates.
struct FeedbackRect {
  u32 tw = 0, th = 0;
  float fx = 1, fy = 1;
  // The zoom centre x (160) and y at the strips (kFbCy; fitted to the retail burst centre) in window pixels.
  float cx = 0, cy = 0;
  // Post-white: the GTE's own centre y (RDE crops fit (160, 120) best).
  float cy_hold = 0;
  float px0 = 0, py0 = 0, pw = 0, ph = 0;
  float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  render::Letterbox lb;
};

namespace {

// The projection centre y at the strips (fitted to the retail burst centre; the field's own projection centre is 120).
constexpr float kFbCy = 150.0f;

}  // namespace

void App::composite_scene(const SceneFrame& sf) {
  const render::Frame& frame = sf.frame;
  const float vw = sf.vw, vh = sf.vh;
  ctx.end_target(*sf.srt);
  // outset is what is shown: the scene, or the feed-back output of the encounter's 3E / 40 pass.
  render::TexRef outset = *sf.sset;
  bool fb_done = false;
  if (sf.jolt.on) keep_jolt_picture(sf);
  if (sf.jolt.post) feedback_pass(sf, outset, fb_done);
  sprites.begin(frame, vw, vh, true);
  sprites.set_blend(render::BlendMode::Add);
  // Above 1x the Box filter averages the area under each window pixel (supersampling); below 1x the bilinear sampler of scene_set. The
  // feed-back output is already window-sized.
  if (sf.rscale > 1.0f && !fb_done) sprites.set_filter(render::SpriteFilter::Box);
  sprites.draw(outset, 0.0f, 0.0f, vw, vh);
  sprites.set_filter(render::SpriteFilter::Sampler);
  sprites.set_blend(render::BlendMode::Alpha);
  if (sf.jolt.on && !fb_done) jolt_paste(sf);
  sprites.end();
}

// The window-sized targets of the jolt and the feed-back passes. Made while the field is shown (App::begin_scene_target), so the first frame of
// an encounter's swirl allocates nothing; a new size (window change) remakes them and drops what they held.
bool App::ensure_transition_targets(u32 w, u32 h) {
  bool ok = true;
  for (int i = 0; i < 2; ++i) {
    if (!jolt_rt[i].valid() || jolt_rt[i].width() != w || jolt_rt[i].height() != h) {
      jolt_have = false;
      ok = ensure_target(jolt_rt[i], jolt_set[i], w, h, linear) && ok;
    }
    if (fb_rt[i].valid() && fb_rt[i].width() == w && fb_rt[i].height() == h && fb_set[i]) continue;
    fb_set[i] = {};
    fb_have = false;
    if (fb_rt[i].create(ctx, w, h)) fb_set[i] = fb_rt[i].ref(linear);
    ok = ok && static_cast<bool>(fb_set[i]);
  }
  if (!fb_frozen.valid() || fb_frozen.width() != w || fb_frozen.height() != h || !fb_frozen_set) {
    fb_frozen_set = {};
    fb_frozen_have = false;
    if (fb_frozen.create(ctx, w, h)) fb_frozen_set = fb_frozen.ref(linear);
  }
  return ok;
}

// This frame's picture into jolt_rt[jolt_cur] at window size (the scene target area-filtered down above 1x): the next tick's previous picture.
void App::keep_jolt_picture(const SceneFrame& sf) {
  const u32 tw = sf.frame.extent.width, th = sf.frame.extent.height;
  if (!ensure_target(jolt_rt[jolt_cur], jolt_set[jolt_cur], tw, th, linear)) {
    jolt_have = false;
    return;
  }
  render::Frame jf;
  ctx.begin_target(jolt_rt[jolt_cur], sf.frame, jf);
  draw_scene_copy(sf, jf, *sf.sset);
  ctx.end_target(jolt_rt[jolt_cur]);
}

// `scene` (the scene target, or a window-sized copy of it) over the whole of `target`, which was cleared black: Add is a plain copy.
void App::draw_scene_copy(const SceneFrame& sf, const render::Frame& target, const render::TexRef& scene) {
  const float tw = static_cast<float>(target.extent.width), th = static_cast<float>(target.extent.height);
  sprites.begin(target, tw, th, true);
  sprites.set_blend(render::BlendMode::Add);
  if (sf.rscale > 1.0f && scene == *sf.sset) sprites.set_filter(render::SpriteFilter::Box);
  sprites.draw(scene, 0.0f, 0.0f, tw, th);
  sprites.set_filter(render::SpriteFilter::Sampler);
  sprites.set_blend(render::BlendMode::Alpha);
  sprites.end();
}

// The true feed-back: the pass of this game tick into fb_rt[fb_cur], the previous output in the other target. The targets are window-sized.
void App::feedback_pass(const SceneFrame& sf, render::TexRef& outset, bool& fb_done) {
  const render::Frame& frame = sf.frame;
  const float vw = sf.vw, vh = sf.vh;
  const render::TexRef sset = *sf.sset;
  const game::FieldWorld::JoltView& jolt = sf.jolt;
  const game::FieldWorld::TransitionStrips ts = world.transition_strips();
  // One pass per game tick (60 Hz); a repeated frame of the same tick shows its output again.
  const bool fb_new_tick = jolt.n != fb_last_n;
  if (fb_new_tick) {
    // The strip pass keeps its anchor (the last finished update's output) for the 4 ticks of an update.
    if (fb_flip_next) {
      fb_cur = 1 - fb_cur;
      fb_have = fb_last_n >= 0;
    }
    fb_last_n = jolt.n;
  }
  const u32 tw = frame.extent.width, th = frame.extent.height;
  const bool ok = ensure_transition_targets(tw, th);
  const render::Letterbox lb = world.view().letterbox(vw, vh);
  float ssx = 0, ssy = 0;
  world.view().scroll_at(step.alpha, ssx, ssy);
  const render::Rect pr = world.view().clip_rect(lb, ssx, vw, vh, true);
  if (!ok || pr.w <= 2 || pr.h <= 2) return;
  FeedbackRect rect;
  rect.tw = tw;
  rect.th = th;
  rect.fx = static_cast<float>(tw) / vw;
  rect.fy = static_cast<float>(th) / vh;
  rect.cx = lb.ox + 160.0f * lb.scale;
  rect.cy = lb.oy + kFbCy * lb.scale_y;
  rect.px0 = static_cast<float>(pr.x);
  rect.py0 = static_cast<float>(pr.y);
  rect.pw = static_cast<float>(pr.w);
  rect.ph = static_cast<float>(pr.h);
  rect.lb = lb;
  const bool white_now = world.transition_white();
  if (!ts.active || white_now) fb_flip_next = true;
  const float fcy = ts.active ? 120.0f : kFbCy;
  rect.cy_hold = lb.oy + fcy * lb.scale_y;
  rect.u0 = (rect.px0 + 0.5f) / vw;
  rect.v0 = (rect.py0 + 0.5f) / vh;
  rect.u1 = (rect.px0 + rect.pw - 0.5f) / vw;
  rect.v1 = (rect.py0 + rect.ph - 0.5f) / vh;
  // The last pre-white output becomes the hold's picture.
  if ((world.transition_prefreeze() || white_now) && !fb_frozen_have && fb_frozen_set) {
    render::Frame fz;
    ctx.begin_target(fb_frozen, frame, fz);
    draw_scene_copy(sf, fz, fb_have ? fb_set[1 - fb_cur] : sset);
    ctx.end_target(fb_frozen);
    fb_frozen_have = true;
  }
  if (!white_now && !ts.active && !world.transition_prefreeze()) fb_frozen_have = false;
  if (white_now) {
    // The battle start clears to white: the strips sample the white buffer, so the white is also the next frame's 'previous output'
    // (the first white frames are pure 255, then 0.5 * field + 0.5 white decays back over ~6 frames).
    render::Frame ff;
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    ctx.begin_target(fb_rt[fb_cur], frame, ff, white);
    ctx.end_target(fb_rt[fb_cur]);
  } else if (ts.active && !fb_new_tick) {
    // A repeated frame of the same game tick: this tick's pass is already in the target.
  } else if (ts.active) {
    feedback_hold_pass(sf, rect, ts);
  } else {
    feedback_field_pass(sf, rect);
  }
  outset = fb_set[fb_cur];
  fb_done = true;
}

// The pass with the battle start's `main` bracket. The finished scene (the frozen last pre-white output times a gain: the field
// is not redrawn while the battle loads) is drawn first; then the previous output, scaled by 4096 / (4096 + tz) about (160, 120) and
// tinted by the bracket colour, goes over it as one rectangle at 50 % (the four 80-wide strips side by side; ABR 0 = (B + F) / 2).
// Where the copy does not reach (zoom < 1) nothing is drawn: the scene shows there. A colour gain above 1 (the ramp reaches 1.48)
// adds 0.5 (g - 1) more.
// RDE presents one pass per update. Every tick j of the update redraws the pass from the update's ANCHOR (the previous update's
// finished output; one resample, so the smear keeps its sharpness) with the partial factors e = (j + 1) / N of the update's own:
// zoom s^e, previous-output weight 0.5^e, colour gain g^e, scene weight sigma(e); the last tick is exactly RDE's pass and becomes the
// next anchor. RDE presents the hold at 60 Hz, so the period is one tick (kHoldPassTicks): every tick is a real pass.
void App::feedback_hold_pass(const SceneFrame& sf, const FeedbackRect& rect, const game::FieldWorld::TransitionStrips& ts) {
  const render::TexRef sset = *sf.sset;
  const float tw = static_cast<float>(rect.tw), th = static_cast<float>(rect.th);
  const auto pass_t0 = std::chrono::steady_clock::now();
  constexpr int kPassTicks = game::encstrips::kHoldPassTicks;
  const int uj = std::max(0, ts.m - 1) % kPassTicks;
  const float ue = static_cast<float>(uj + 1) / static_cast<float>(kPassTicks);
  // The bracket at the period's first tick.
  const game::encstrips::Params bp = game::encstrips::main_bracket(ts.m - uj);
  const float zoom = std::pow(bp.scale(), ue);
  const float wq = std::pow(0.5f, ue);
  const float gq[3] = {std::pow(bp.gain(0), ue), std::pow(bp.gain(1), ue), std::pow(bp.gain(2), ue)};
  fb_flip_next = uj == kPassTicks - 1;
  render::Frame ff;
  ctx.begin_target(fb_rt[fb_cur], sf.frame, ff);
  sprites.begin(ff, tw, th, true);
  sprites.set_blend(render::BlendMode::Add);
  const render::TexRef scene_src = fb_frozen_have ? fb_frozen_set : sset;
  // The scene's weight after e of the update is the geometric injection sum 0.5 (1 - (0.5 g)^e) / (1 - 0.5 g) (= 0.5 at e = 1, 1 - 0.5^e for g = 1);
  // the alpha blend scales the scene by (1 - wq).
  float sc[3] = {ts.scene_gain, ts.scene_gain, ts.scene_gain};
  for (int k = 0; k < 3; ++k) {
    const float den = std::max(0.05f, 1.0f - 0.5f * bp.gain(k));
    sc[k] *= 0.5f * (1.0f - std::pow(0.5f * bp.gain(k), ue)) / den / (1.0f - static_cast<float>(static_cast<u8>(wq * 255.0f + 0.5f)) / 255.0f);
  }
  if (sf.rscale > 1.0f && scene_src == sset) sprites.set_filter(render::SpriteFilter::Box);
  for (float off = 0.0f; std::max({sc[0], sc[1], sc[2]}) - off > 0.001f; off += 1.0f) {
    u8 c3[3];
    for (int k = 0; k < 3; ++k) c3[k] = static_cast<u8>(std::clamp(sc[k] - off, 0.0f, 1.0f) * 255.0f + 0.5f);
    sprites.draw(scene_src, 0.0f, 0.0f, tw, th, 0, 0, 1, 1, render::pack_rgba(c3[0], c3[1], c3[2], 255));
  }
  sprites.set_filter(render::SpriteFilter::Sampler);
  sprites.end();
  const render::Rect prt{static_cast<s32>(rect.px0 * rect.fx), static_cast<s32>(rect.py0 * rect.fy),
                         static_cast<u32>(rect.pw * rect.fx) + 1, static_cast<u32>(rect.ph * rect.fy) + 1};
  ff.set_scissor(prt);
  sprites.begin(ff, tw, th, true);
  const float dx = rect.cx + (rect.px0 - rect.cx) * zoom, dy = rect.cy_hold + (rect.py0 - rect.cy_hold) * zoom;
  const render::TexRef prev = fb_have ? fb_set[1 - fb_cur] : sset;
  auto c8 = [](float value) { return static_cast<u8>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
  sprites.set_blend(render::BlendMode::Alpha);
  sprites.draw(prev, dx * rect.fx, dy * rect.fy, rect.pw * zoom * rect.fx, rect.ph * zoom * rect.fy, rect.u0, rect.v0, rect.u1, rect.v1,
               render::pack_rgba(c8(gq[0]), c8(gq[1]), c8(gq[2]), c8(wq)));
  if (gq[0] > 1.0f || gq[1] > 1.0f || gq[2] > 1.0f) {
    sprites.set_blend(render::BlendMode::Add);
    sprites.draw(prev, dx * rect.fx, dy * rect.fy, rect.pw * zoom * rect.fx, rect.ph * zoom * rect.fy, rect.u0, rect.v0, rect.u1, rect.v1,
                 render::pack_rgba(c8(wq * (gq[0] - 1.0f)), c8(wq * (gq[1] - 1.0f)), c8(wq * (gq[2] - 1.0f)), 255));
  }
  sprites.set_blend(render::BlendMode::Alpha);
  sprites.end();
  ff.set_scissor(ff.full());
  ctx.end_target(fb_rt[fb_cur]);
  if (frametime_log)
    log::info("strip pass: tick {} m {} {:.3f} ms cpu", world.tick_count(), ts.m,
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pass_t0).count());
}

// Before the white (the enc2 bracket): this tick's picture 50 % blended with the zoomed previous output. The RDE field presents
// at 30 Hz (one strip pass per 2 ticks, kFieldPassTicks); tick j of the period draws from the period's anchor with the partial powers
// e = (j + 1) / 2 (zoom s^e, translation e t), tick 1 is the real pass and becomes the anchor. The blend weight stays 0.5 on BOTH
// ticks (not 0.5^e): the scene is live here, and a scene weight alternating 0.29 / 0.5 would make the moving sparks and actors pulse at
// 30 Hz; with a constant 0.5 the picture's weight sum is 1 every tick and only the echo's position steps by sqrt(s) on the half tick,
// which is monotonic.
void App::feedback_field_pass(const SceneFrame& sf, const FeedbackRect& rect) {
  const render::TexRef sset = *sf.sset;
  const game::FieldWorld::JoltView& jolt = sf.jolt;
  const float tw = static_cast<float>(rect.tw), th = static_cast<float>(rect.th);
  const int pass_ticks = jolt.mode == 0 ? game::encstrips::kFieldPassTicks : 1;
  const int uj = jolt.n % pass_ticks;
  const float ue = static_cast<float>(uj + 1) / static_cast<float>(pass_ticks);
  fb_flip_next = uj == pass_ticks - 1;
  const float js = std::pow(jolt.scale, ue), jtx = jolt.tx * ue, jty = jolt.ty * ue;
  const float dx = rect.cx + (rect.px0 - rect.cx) * js + jtx * rect.lb.scale, dy = rect.cy + (rect.py0 - rect.cy) * js + jty * rect.lb.scale_y;
  render::Frame ff;
  ctx.begin_target(fb_rt[fb_cur], sf.frame, ff);
  draw_scene_copy(sf, ff, sset);
  const render::Rect prt{static_cast<s32>(rect.px0 * rect.fx), static_cast<s32>(rect.py0 * rect.fy),
                         static_cast<u32>(rect.pw * rect.fx) + 1, static_cast<u32>(rect.ph * rect.fy) + 1};
  ff.set_scissor(prt);
  sprites.begin(ff, tw, th, true);
  auto c8 = [&](int value, float gain) {
    return static_cast<u8>(std::clamp(std::pow(std::clamp(static_cast<float>(value) * 2.0f * gain, 0.0f, 255.0f) / 255.0f, ue) * 255.0f, 0.0f, 255.0f));
  };
  const float mode_gain = jolt.mode == 3 ? 0.25f : 1.0f;
  const u8 a = jolt.mode == 0 ? 128 : 255;
  sprites.set_blend(jolt.mode == 1 || jolt.mode == 3 ? render::BlendMode::Add : jolt.mode == 2 ? render::BlendMode::Subtract : render::BlendMode::Alpha);
  const render::TexRef prev = fb_have ? fb_set[1 - fb_cur] : sset;
  const u32 colour = render::pack_rgba(c8(jolt.rgb[0], mode_gain), c8(jolt.rgb[1], mode_gain), c8(jolt.rgb[2], mode_gain), a);
  if (jolt.echo) {
    // The frame echo: five strips side by side (64 columns each in 4:3), each corner projected about the field's projection centre.
    // The texture rectangle is the picture rectangle itself (no half-texel inset), so scale 1.0 copies the picture in place.
    constexpr int kStrips = 5;
    const float eu0 = rect.px0 / static_cast<float>(rect.tw) * rect.fx, eu1 = (rect.px0 + rect.pw) / static_cast<float>(rect.tw) * rect.fx;
    const float ev0 = rect.py0 / static_cast<float>(rect.th) * rect.fy, ev1 = (rect.py0 + rect.ph) / static_cast<float>(rect.th) * rect.fy;
    const float ecx = rect.lb.ox + 160.0f * rect.lb.scale, ecy = rect.lb.oy + 120.0f * rect.lb.scale_y;
    auto corner = [&](float wx, float wy, float* xy) {
      float x = (wx - ecx) / rect.lb.scale, y = (wy - ecy) / rect.lb.scale_y;
      game::FieldWorld::frame_echo_point(jolt, ue, x, y);
      xy[0] = (ecx + x * rect.lb.scale) * rect.fx;
      xy[1] = (ecy + y * rect.lb.scale_y) * rect.fy;
    };
    for (int i = 0; i < kStrips; ++i) {
      const float sx0 = rect.px0 + rect.pw * static_cast<float>(i) / kStrips, sx1 = rect.px0 + rect.pw * static_cast<float>(i + 1) / kStrips;
      const float su0 = eu0 + (eu1 - eu0) * static_cast<float>(i) / kStrips, su1 = eu0 + (eu1 - eu0) * static_cast<float>(i + 1) / kStrips;
      float xy[8];
      corner(sx0, rect.py0, xy);
      corner(sx1, rect.py0, xy + 2);
      corner(sx0, rect.py0 + rect.ph, xy + 4);
      corner(sx1, rect.py0 + rect.ph, xy + 6);
      const float uv[8] = {su0, ev0, su1, ev0, su0, ev1, su1, ev1};
      sprites.draw_quad(prev, xy, uv, colour);
    }
  } else {
    sprites.draw(prev, dx * rect.fx, dy * rect.fy, rect.pw * js * rect.fx, rect.ph * js * rect.fy, rect.u0, rect.v0, rect.u1, rect.v1, colour);
  }
  sprites.set_blend(render::BlendMode::Alpha);
  sprites.end();
  ff.set_scissor(ff.full());
  ctx.end_target(fb_rt[fb_cur]);
}

// The previous picture (the other display buffer) at 50 % (abr 0), translated by (tx, ty) PS1 px and scaled about the screen centre
// (160, 120) of the 320 x 240 layout. The first frame of a jolt has no previous target yet: it ghosts itself. Only the room picture
// (field_clip's rect) is pasted, from the same rect of the previous picture and clipped to it: the 320 x 224 strips of the PS1 never
// reach the crop band / letterbox / widescreen black, which would otherwise darken a bar of the picture (the top rows with the shift
// or the zoom) at every window size. Runs inside composite_scene's sprite pass over the window frame.
void App::jolt_paste(const SceneFrame& sf) {
  const render::Frame& frame = sf.frame;
  const float vw = sf.vw, vh = sf.vh;
  const render::TexRef sset = *sf.sset;
  const game::FieldWorld::JoltView& jolt = sf.jolt;
  const render::Letterbox lb = world.view().letterbox(vw, vh);
  float ssx = 0, ssy = 0;
  world.view().scroll_at(step.alpha, ssx, ssy);
  const render::Rect pr = world.view().clip_rect(lb, ssx, vw, vh, true);
  if (pr.w <= 2 || pr.h <= 2) return;
  const float cx = lb.ox + 160.0f * lb.scale, cy = lb.oy + 120.0f * lb.scale_y;
  const float px0 = static_cast<float>(pr.x), py0 = static_cast<float>(pr.y);
  const float pw_ = static_cast<float>(pr.w), ph_ = static_cast<float>(pr.h);
  const float dx = cx + (px0 - cx) * jolt.scale + jolt.tx * lb.scale, dy = cy + (py0 - cy) * jolt.scale + jolt.ty * lb.scale_y;
  const float u0 = (px0 + 0.5f) / vw, v0 = (py0 + 0.5f) / vh, u1 = (px0 + pw_ - 0.5f) / vw, v1 = (py0 + ph_ - 0.5f) / vh;
  sprites.end();
  frame.set_scissor(pr);
  sprites.begin(frame, vw, vh, true);
  // The encounter's 3E / 40 pass: a copy of the current picture, ABR mode / colour of the sequence (field_world.h SeqPost).
  if (jolt.post) {
    auto c8 = [&](int value, float gain) { return static_cast<u8>(std::clamp(static_cast<float>(value) * 2.0f * gain, 0.0f, 255.0f)); };
    const float mode_gain = jolt.mode == 3 ? 0.25f : 1.0f;
    const u8 a = jolt.mode == 0 ? 128 : 255;
    sprites.set_blend(jolt.mode == 1 || jolt.mode == 3 ? render::BlendMode::Add : jolt.mode == 2 ? render::BlendMode::Subtract : render::BlendMode::Alpha);
    sprites.draw(sset, dx, dy, pw_ * jolt.scale, ph_ * jolt.scale, u0, v0, u1, v1,
                 render::pack_rgba(c8(jolt.rgb[0], mode_gain), c8(jolt.rgb[1], mode_gain), c8(jolt.rgb[2], mode_gain), a));
    sprites.set_blend(render::BlendMode::Alpha);
  } else {
    sprites.draw(jolt_have ? jolt_set[1 - jolt_cur] : sset, dx, dy, pw_ * jolt.scale, ph_ * jolt.scale, u0, v0, u1, v1,
                 render::pack_rgba(255, 255, 255, 128));
  }
  sprites.end();
  frame.set_scissor(frame.full());
  sprites.begin(frame, vw, vh, true);
}

}  // namespace oc::app
