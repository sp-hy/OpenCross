#pragma once
// 2D textures (RGBA8 or R8 palette-index) uploaded through a transfer buffer, plus samplers and image decoding.
#include <vector>

#include "render/gpu_context.h"

namespace oc::render {

enum class TextureFormat { Rgba8, R8 };

// Uploads recorded by Texture::create(..., batch) and submitted together by flush(): one copy pass for many textures (the battle load
// uploads a hundred atlases). The textures hold no pixels before flush().
class UploadBatch {
 public:
  bool flush(GpuContext& ctx);
  bool empty() const { return ops_.empty(); }

 private:
  friend class Texture;
  std::vector<UploadOp> ops_;
};

class Texture {
 public:
  Texture() = default;
  ~Texture() { destroy(); }
  Texture(const Texture&) = delete;
  Texture& operator=(const Texture&) = delete;

  // `pixels` is tightly packed w*h*(4 or 1) bytes. With `batch` the upload is recorded into it instead of submitted at once;
  // with `mips` a full mip chain is generated.
  bool create(GpuContext& ctx, u32 width, u32 height, TextureFormat format, const void* pixels, UploadBatch* batch = nullptr,
              bool mips = false);
  // For a worker thread: creates the texture and uploads `pixels` in bands through GpuContext::band_staging(), on its own command buffers,
  // returning when the copies are done (nothing is recorded on the render thread's command buffers, no staging memory is released there).
  bool create_blocking(GpuContext& ctx, u32 width, u32 height, TextureFormat format, const void* pixels);
  void destroy();

  // Streaming path: re-uploads the whole image (tightly packed, same size/format as create()) through a persistent staging buffer.
  bool update(GpuContext& ctx, const void* pixels);
  // Uploads `pixels` (width x height, tightly packed as create()) into the rectangle at (x, y); the other texels keep their values.
  bool update_region(GpuContext& ctx, const void* pixels, u32 x, u32 y, u32 width, u32 height);

  SDL_GPUTexture* handle() const { return texture_; }
  explicit operator bool() const { return texture_ != nullptr; }
  u32 width() const { return width_; }
  u32 height() const { return height_; }

 private:
  // The GPU image alone (no pixels yet).
  bool create_image(GpuContext& ctx, u32 width, u32 height, TextureFormat format, bool mips);

  GpuContext* ctx_ = nullptr;
  SDL_GPUDevice* device_ = nullptr;
  SDL_GPUTexture* texture_ = nullptr;
  SDL_GPUTransferBuffer* staging_ = nullptr;
  u32 width_ = 0, height_ = 0;
  u32 bytes_per_pixel_ = 4;
  u64 bytes_ = 0;
};

class Sampler {
 public:
  Sampler() = default;
  ~Sampler() { destroy(); }
  Sampler(const Sampler&) = delete;
  Sampler& operator=(const Sampler&) = delete;

  bool create(GpuContext& ctx, bool linear, bool repeat = false);
  void destroy();
  SDL_GPUSampler* handle() const { return sampler_; }

 private:
  GpuContext* ctx_ = nullptr;
  SDL_GPUDevice* device_ = nullptr;
  SDL_GPUSampler* sampler_ = nullptr;
};

inline TexRef::TexRef(const Texture& texture, const Sampler& sampler)
    : texture(texture.handle()), sampler(sampler.handle()), width(texture.width()), height(texture.height()) {}

struct DecodedImage {
  u32 width = 0, height = 0;
  // width * height * 4 bytes.
  std::vector<u8> rgba;
};

// PNG/JPG/etc. (stb_image) -> RGBA8.
bool decode_image(ByteSpan file, DecodedImage& out);

}  // namespace oc::render
