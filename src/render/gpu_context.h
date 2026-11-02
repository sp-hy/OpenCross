#pragma once
// SDL_GPU device, window swapchain and frame loop.
//
// Usage per frame:   Frame f; if (ctx.begin_frame(f)) { ...draw through the batches...; ctx.end_frame(); }
// begin_frame() has begun a render pass into the frame's colour + depth images (cleared) with a full-window viewport and scissor;
// end_frame() presents them. It returns false when the frame must be skipped (minimised / resizing).
//
// Every frame records two command buffers: `upload_cmd()` (copies into GPU memory, executed first) and `cmd()` (the draws). Data
// written during the frame (StreamBuffer, Texture::update) therefore reaches the GPU before any draw of that frame runs, in the order
// the uploads were recorded.
#include <SDL3/SDL_gpu.h>

#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.h"

struct SDL_Window;

namespace oc::render {

inline constexpr u32 kFramesInFlight = 2;

class GpuContext;
class RenderTarget;
class StreamBuffer;
class Texture;
class Sampler;

// 1, 2, 4 or 8 samples as SDL's enum (other counts round down).
inline SDL_GPUSampleCount sample_count(u32 samples) {
  return samples >= 8 ? SDL_GPU_SAMPLECOUNT_8 : samples >= 4 ? SDL_GPU_SAMPLECOUNT_4 : samples >= 2 ? SDL_GPU_SAMPLECOUNT_2 : SDL_GPU_SAMPLECOUNT_1;
}

struct Extent {
  u32 width = 0, height = 0;
};

struct Rect {
  s32 x = 0, y = 0;
  u32 w = 0, h = 0;
};

// A texture as the fragment shaders sample it: the image with the sampler it is read through. A value; nothing to free.
struct TexRef {
  SDL_GPUTexture* texture = nullptr;
  SDL_GPUSampler* sampler = nullptr;
  u32 width = 0, height = 0;

  TexRef() = default;
  TexRef(SDL_GPUTexture* texture, SDL_GPUSampler* sampler, u32 width, u32 height)
      : texture(texture), sampler(sampler), width(width), height(height) {}
  inline TexRef(const Texture& texture, const Sampler& sampler);
  explicit operator bool() const { return texture != nullptr; }
  bool operator==(const TexRef& other) const { return texture == other.texture && sampler == other.sampler; }
  bool operator!=(const TexRef& other) const { return !(*this == other); }
};

struct Frame {
  GpuContext* ctx = nullptr;
  // 0..kFramesInFlight-1, for per-frame resources.
  u32 index = 0;
  // Counts begin_frame calls: tells passes of one frame (several begin()s) from the next frame.
  u64 serial = 0;
  // The image being rendered into (the window, or the target of begin_target).
  Extent extent{};

  Rect full() const { return {0, 0, extent.width, extent.height}; }
  // Clips the following draws to `rect`; begin_target / end_target / clear_depth reset it to the whole image.
  inline void set_scissor(const Rect& rect) const;
  // Clears the depth of the rendering in progress; the colour is kept.
  inline void clear_depth() const;
};

// One copy from an unmapped transfer buffer into a texture (level 0, whole image) or a buffer range.
struct UploadOp {
  SDL_GPUTransferBuffer* source = nullptr;
  u32 source_offset = 0;
  // The source is released after the copy unless kept (a persistent staging buffer).
  bool keep_source = false;

  SDL_GPUTexture* texture = nullptr;
  u32 width = 0, height = 0;
  // Row stride of the source in pixels (0 = tightly packed).
  u32 pixels_per_row = 0;
  // Generate the texture's mip chain after the copy.
  bool mips = false;
  // The texture may still be read by an earlier frame: give it fresh memory instead of waiting (streamed textures).
  bool cycle = false;

  SDL_GPUBuffer* buffer = nullptr;
  u32 buffer_offset = 0;
  u32 size = 0;
};

class GpuContext {
 public:
  GpuContext() = default;
  ~GpuContext() { shutdown(); }
  GpuContext(const GpuContext&) = delete;
  GpuContext& operator=(const GpuContext&) = delete;

  // `driver` = an SDL GPU driver name ("vulkan", "direct3d12", "metal") or empty for SDL's choice. A driver that cannot be created
  // falls back to SDL's choice (driver_fallback() tells).
  bool init(SDL_Window* window, bool vsync, const std::string& driver = {});
  void shutdown();

  // The driver in use ("vulkan", "direct3d12", ...).
  const char* driver() const;
  bool driver_fallback() const { return driver_fallback_; }
  static std::vector<std::string> available_drivers();
  // Number of GPU warnings/errors SDL reported since init (debug mode validates API usage; Debug builds).
  static u32 validation_message_count();

  // Takes effect on the next begin_frame.
  void set_vsync(bool on);
  // Multisample antialiasing of the frame and of the multisampled render targets: 1, 2, 4 or 8 samples, lowered to what the GPU supports.
  // A change releases every multisampled image; the next begin_frame / begin_target makes them again.
  void set_msaa(u32 samples);
  u32 msaa() const { return msaa_; }
  // The MSAA of an image of this size: halved until it holds at most kMaxMsaaSamples samples (a supersampled render-scale target is
  // already antialiased, and multisampling it would take gigabytes).
  static constexpr u64 kMaxMsaaSamples = 3840ull * 2160ull * 8ull;
  SDL_GPUSampleCount sample_count_for(u32 width, u32 height) const;
  // The sample count of the rendering in progress (1 outside a pass): the pipelines bind their variant of it.
  SDL_GPUSampleCount pass_sample_count() const { return pass_samples_; }
  // Anisotropic filtering level of the linear samplers the 3D pipelines bind (1 = off, up to 16).
  void set_anisotropy(u32 level);
  // `sampler` as the 3D pipelines bind it: a linear Sampler's anisotropic twin when anisotropic filtering is on, else itself.
  SDL_GPUSampler* filtered(SDL_GPUSampler* sampler);
  // Before ImGui: moves the rest of the frame into a single-sampled rendering of the resolved frame image (ImGui's pipeline is single-sampled).
  void begin_overlay();
  void set_clear_color(float r, float g, float b, float a = 1.0f);
  void request_resize() { rebuild_swapchain_ = true; }
  // Saves the next frame handed to end_frame() as a PNG (blocks for that frame).
  void request_screenshot(const std::filesystem::path& file) { screenshot_path_ = file; }
  // Hands the next finished frame to `callback` as tightly packed RGBA8 (w*h*4, alpha 255): used to freeze the last field frame
  // for the battle transition (blocks for that frame).
  void request_capture(std::function<void(std::vector<u8>&&, u32, u32)> callback) { capture_cb_ = std::move(callback); }

  bool begin_frame(Frame& frame);
  void end_frame();
  // Inside a frame: ends the current rendering (the window or an enclosing target) and begins rendering into `target` (colour cleared
  // to the clear colour or `clear_rgba`, depth to 1, viewport / scissor = the target). `out` = the frame with the target's extent.
  // end_target() makes the colour image samplable and resumes the rendering it suspended: colour kept, depth cleared. Targets nest
  // (e.g. the 8F ripple's deep-part target inside the render-scale scene target); end them in reverse order.
  void begin_target(RenderTarget& target, const Frame& frame, Frame& out);
  void begin_target(RenderTarget& target, const Frame& frame, Frame& out, const float (&clear_rgba)[4]);
  void end_target(RenderTarget& target);
  // Inside a frame: copies the colour of the rendering in progress (resolved when multisampled) into `target`'s colour image, scaled to
  // its size with bilinear filtering, and resumes the rendering: colour kept, depth cleared.
  void copy_color(RenderTarget& target);
  void clear_depth();
  void set_scissor(const Rect& rect);
  void wait_idle();

  // Releases a texture of `bytes` once the frames using it are done, a share per frame (end_frame): unloading a battle frees hundreds of
  // textures and megabytes, and the driver's cost grows with both (~0.7 ms per texture), so one frame would stall. Any thread; ignored
  // after shutdown.
  void release_texture(SDL_GPUTexture* texture, u64 bytes);
  // Waits for the GPU and releases everything queued at once: for the room sweep, which unloads more per frame than the shares free.
  void drain_releases();
  static constexpr u64 kReleaseBytesPerFrame = 64ull << 20;
  static constexpr size_t kReleaseTexturesPerFrame = 16;

  // A 1x1 opaque white texture (solid-colour quads through the sprite batch).
  TexRef white() const { return white_; }

  // Records the copies. Inside a frame they go to upload_cmd(); outside they are submitted at once. The sources must be unmapped.
  void upload(std::span<const UploadOp> ops);
  void upload(const UploadOp& op) { upload(std::span<const UploadOp>(&op, 1)); }

  // The staging buffer of the uploads done off the render thread (Texture::create_blocking), kBandBytes, created on first use and kept until
  // shutdown, so no large staging memory is ever released on the render thread. Hold band_mutex() while it is mapped or in flight.
  static constexpr u32 kBandBytes = 32u << 20;
  SDL_GPUTransferBuffer* band_staging();
  std::mutex& band_mutex() { return band_mutex_; }

  SDL_GPUDevice* device() const { return device_; }
  // The frame's draw command buffer and the render pass in progress (null outside a frame). Uniform pushes go to cmd().
  SDL_GPUCommandBuffer* cmd() const { return cmd_; }
  SDL_GPURenderPass* pass() const { return pass_; }
  // The frame's upload command buffer (acquired on first use). No pass is left open on it between calls.
  SDL_GPUCommandBuffer* upload_cmd();
  SDL_GPUTextureFormat color_format() const { return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; }
  SDL_GPUTextureFormat depth_format() const { return depth_format_; }
  Extent extent() const { return extent_; }

 private:
  friend class StreamBuffer;
  friend class Sampler;
  friend class RenderTarget;

  // What one render pass draws into. Multisampled: `color` is the multisampled image, resolved into `resolve` once its drawing is done
  // (end_target, begin_overlay, end_frame), not each time the pass is suspended.
  struct PassImages {
    SDL_GPUTexture* color = nullptr;
    SDL_GPUTexture* resolve = nullptr;
    SDL_GPUTexture* depth = nullptr;
    Extent extent{};
    SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1;
  };
  struct LinearSampler {
    SDL_GPUSamplerCreateInfo info{};
    SDL_GPUSampler* anisotropic = nullptr;
  };

  bool create_device(const std::string& driver);
  bool apply_swapchain();
  bool ensure_frame_images(u32 width, u32 height);
  // Begins a render pass into the given images (colour cleared or loaded, depth always cleared) with viewport / scissor = their extent.
  void begin_rendering(const PassImages& images, bool clear_color, const float (&rgba)[4]);
  void end_rendering();
  // The images of the innermost begin_target, or the frame's.
  PassImages current_images() const;
  void register_linear_sampler(SDL_GPUSampler* sampler, const SDL_GPUSamplerCreateInfo& info);
  void unregister_sampler(SDL_GPUSampler* sampler);
  void release_anisotropic_samplers();
  void track_multisampled(RenderTarget* target, bool tracked);
  // Releases queued textures: at least one, then up to `bytes` bytes and `count` textures.
  void release_queued(u64 bytes, size_t count);
  // A colour-only pass that resolves the multisampled image of `images` (no-op when single-sampled).
  void resolve(const PassImages& images);
  void encode(SDL_GPUCommandBuffer* cmd, std::span<const UploadOp> ops);
  void flush_streams();
  void readback(SDL_GPUCommandBuffer* cmd);
  void register_stream(StreamBuffer& stream) { streams_.push_back(&stream); }

  SDL_Window* window_ = nullptr;
  SDL_GPUDevice* device_ = nullptr;
  bool driver_fallback_ = false;
  SDL_GPUTextureFormat depth_format_ = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

  // The window-sized colour + depth images every frame is drawn into, blitted to the swapchain at end_frame (the swapchain image
  // itself cannot be read back, and its format varies).
  SDL_GPUTexture* frame_color_ = nullptr;
  SDL_GPUTexture* frame_depth_ = nullptr;
  // With MSAA: the multisampled colour image resolved into frame_color_ (frame_depth_ is multisampled too), and the single-sampled depth
  // of the overlay pass.
  SDL_GPUTexture* frame_msaa_color_ = nullptr;
  SDL_GPUTexture* frame_overlay_depth_ = nullptr;
  SDL_GPUSampleCount frame_samples_ = SDL_GPU_SAMPLECOUNT_1;
  bool overlay_ = false;
  Extent extent_{};
  SDL_GPUTexture* swapchain_image_ = nullptr;

  SDL_GPUCommandBuffer* cmd_ = nullptr;
  SDL_GPUCommandBuffer* upload_cmd_ = nullptr;
  std::mutex band_mutex_;
  SDL_GPUTransferBuffer* band_staging_ = nullptr;
  SDL_GPURenderPass* pass_ = nullptr;
  Extent pass_extent_{};
  SDL_GPUSampleCount pass_samples_ = SDL_GPU_SAMPLECOUNT_1;
  u32 msaa_ = 1;
  std::vector<RenderTarget*> multisampled_targets_;
  struct QueuedRelease {
    SDL_GPUTexture* texture = nullptr;
    u64 bytes = 0;
  };
  std::mutex release_mutex_;
  std::vector<QueuedRelease> release_queue_;
  u32 anisotropy_ = 1;
  std::mutex samplers_mutex_;
  std::unordered_map<SDL_GPUSampler*, LinearSampler> linear_samplers_;
  u32 frame_index_ = 0;
  u64 frame_serial_ = 0;

  bool vsync_ = true;
  bool rebuild_swapchain_ = false;
  bool in_frame_ = false;
  // begin_target nesting (innermost last); empty = the frame images are being rendered.
  std::vector<RenderTarget*> targets_;
  std::vector<StreamBuffer*> streams_;
  std::filesystem::path screenshot_path_;
  std::function<void(std::vector<u8>&&, u32, u32)> capture_cb_;
  float clear_[4] = {0.05f, 0.05f, 0.08f, 1.0f};

  SDL_GPUTexture* white_texture_ = nullptr;
  SDL_GPUSampler* white_sampler_ = nullptr;
  TexRef white_;
};

inline void Frame::set_scissor(const Rect& rect) const {
  if (ctx) ctx->set_scissor(rect);
}

inline void Frame::clear_depth() const {
  if (ctx) ctx->clear_depth();
}

}  // namespace oc::render
