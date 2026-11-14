#pragma once
// [Shaders] Ssao / DepthOfField: screen-space ambient occlusion and depth of field on a 3D picture (the battle's stage, units and weapons),
// applied in the middle of its drawing: what is drawn afterwards (effect sprites, HUD, menus, windows) is untouched.
//
// The picture's depth buffer holds ordering-table slots (one per stage packet, render/stage_pipeline.h), not positions, so the passes read a
// view depth of their own. The host calls GpuContext::hold_depth before drawing the picture, then begin(): the picture's rendering is
// suspended (its colour and depth kept) and the view-depth target is begun at the output resolution (the picture's size, at most the
// window's), cleared to the far depth. The host draws the opaque stage and models into it with the pipelines' view-depth mode
// (StagePipeline / MeshPipeline::set_view_depth: camera z per pixel), then calls end(), which runs at half the output resolution:
//   SSAO: per pixel, 12 samples of a disc of kAoRadius world units around the view position rebuilt from the view depth (spun per pixel by a
//         fixed noise); a sample occludes by the cosine between the surface normal (from the neighbouring depths) and the direction to it,
//         fading to nothing at the radius, so the floor under a unit's feet and creases darken and the sky (far depth) never does. Blurred by
//         a separable 9-tap depth-aware blur.
//   DoF:  the picture is filtered down to half resolution with each pixel's blur amount (circle of confusion) from its view depth and the
//         focus (PostFxView::focus): sharp within kFocusNear / kFocusFar of the focus distance, fully blurred kFocusRamp beyond; the blur
//         radius and its mix scale with PostFxView::dof_amount. A 32-tap disc gather takes a sample only where both the pixel's and the
//         sample's blur reach it, so a sharp unit does not smear into the blurred background behind it.
// Then both are upsampled with depth-aware weights (output resolution) and the picture's rendering is resumed, colour and depth, and the result
// is drawn over it inside the scissor: colour = ao (a blur + (1 - a) colour), a = the pixel's blur amount. Everything drawn after end() (the
// effect sprites, which write no depth) is sharp and unoccluded, depth-tested against the picture as without the passes.
#include "render/pipeline.h"
#include "render/render_target.h"
#include "render/texture.h"

namespace oc::render {

struct PostFxView {
  bool ssao = false;
  bool dof = false;
  // The projection (camera space, y down): NDC x = ax x / z + cx, NDC y (down) = ay y / z + cy.
  float ax = 1, cx = 0, ay = 1, cy = 0;
  // The camera z in focus and the share of the blur, 0..1 (depth of field).
  float focus = 4096;
  float dof_amount = 1;
};

class PostFx {
 public:
  static constexpr float kAoRadius = 220.0f;
  // Fractions of the focus distance: the sharp band in front of / behind the focus, and the ramps to the full blur behind / in front.
  static constexpr float kFocusNear = 0.3f, kFocusFar = 0.25f, kFocusRamp = 0.9f, kFocusNearRamp = 0.45f;
  // The largest blur radius, a fraction of the output height.
  static constexpr float kMaxBlur = 0.008f;

  PostFx() = default;
  ~PostFx() { destroy(); }
  PostFx(const PostFx&) = delete;
  PostFx& operator=(const PostFx&) = delete;

  // Suspends the rendering of `frame` and begins the view-depth target: `depth_frame` and `depth_scissor` (`scissor` scaled) to draw it
  // with. False = no passes (the rendering is not suspended).
  bool begin(GpuContext& ctx, const Frame& frame, const Rect& scissor, Frame& depth_frame, Rect& depth_scissor);
  // Ends the view-depth target, runs the passes, resumes the rendering and draws the result over it.
  void end(GpuContext& ctx, const Rect& scissor, const PostFxView& view);
  // Releases every image, pipeline and shader (made again on the next begin()).
  void destroy();

 private:
  bool init(GpuContext& ctx);
  bool ensure(RenderTarget& target, u32 width, u32 height);
  void pass(GpuContext& ctx, int mode, const TexRef (&textures)[3], const float (&push)[16], bool over);

  GpuContext* ctx_ = nullptr;
  SDL_GPUShader* vs_ = nullptr;
  SDL_GPUShader* fs_ = nullptr;
  Pipeline pipeline_, pipeline_over_;
  Sampler nearest_, linear_;
  // Output resolution: the view depth, the upsampled result; half resolution: AO and its blur, the filtered picture and its blur.
  RenderTarget depth_, result_;
  RenderTarget ao_[2], dof_[2];
  // The suspended rendering: its frame and colour image.
  Frame frame_;
  TexRef scene_;
};

}  // namespace oc::render
