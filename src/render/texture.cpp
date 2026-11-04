#include "render/texture.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include <spng.h>

#include "core/log.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace oc::render {

namespace {

// Direct3D 12 wants upload rows aligned to 256 bytes; every row of the staging copy is padded to that pitch.
u32 row_pitch(u32 row_bytes) { return (row_bytes + 255u) & ~255u; }

void copy_rows(void* dst, const void* src, u32 row_bytes, u32 pitch, u32 rows) {
  for (u32 row = 0; row < rows; ++row)
    std::memcpy(static_cast<u8*>(dst) + static_cast<size_t>(row) * pitch, static_cast<const u8*>(src) + static_cast<size_t>(row) * row_bytes, row_bytes);
}

}  // namespace

namespace {

constexpr u8 kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

// PNG through libspng: any bit depth and colour type to RGBA8, tRNS applied, no
// gamma correction (as stb_image). The chunk CRCs are not checked: the files are local mod data, not network input.
bool decode_png(ByteSpan file, DecodedImage& out) {
  spng_ctx* ctx = spng_ctx_new(0);
  if (!ctx) return false;
  spng_set_crc_action(ctx, SPNG_CRC_USE, SPNG_CRC_USE);
  spng_set_image_limits(ctx, 1u << 15, 1u << 15);
  size_t size = 0;
  spng_ihdr ihdr{};
  int err = spng_set_png_buffer(ctx, file.data(), file.size());
  if (!err) err = spng_get_ihdr(ctx, &ihdr);
  if (!err) err = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &size);
  if (!err) {
    out.rgba.resize(size);
    err = spng_decode_image(ctx, out.rgba.data(), size, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
  }
  spng_ctx_free(ctx);
  if (err) {
    log::error("image decode failed: {}", spng_strerror(err));
    out.rgba.clear();
    return false;
  }
  out.width = ihdr.width;
  out.height = ihdr.height;
  return true;
}

}  // namespace

bool decode_image(ByteSpan file, DecodedImage& out) {
  if (file.size() >= sizeof(kPngSignature) && std::memcmp(file.data(), kPngSignature, sizeof(kPngSignature)) == 0) return decode_png(file, out);
  int w = 0, h = 0, channels = 0;
  stbi_uc* px = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &w, &h, &channels, 4);
  if (!px) {
    log::error("image decode failed: {}", stbi_failure_reason());
    return false;
  }
  out.width = static_cast<u32>(w);
  out.height = static_cast<u32>(h);
  out.rgba.assign(px, px + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  stbi_image_free(px);
  return true;
}

bool Texture::create_image(GpuContext& ctx, u32 width, u32 height, TextureFormat format, bool mips) {
  destroy();
  ctx_ = &ctx;
  device_ = ctx.device();
  width_ = width;
  height_ = height;
  bytes_per_pixel_ = format == TextureFormat::Rgba8 ? 4 : 1;
  SDL_GPUTextureCreateInfo info{};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = format == TextureFormat::Rgba8 ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : SDL_GPU_TEXTUREFORMAT_R8_UNORM;
  info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (mips ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0u);
  info.width = width;
  info.height = height;
  info.layer_count_or_depth = 1;
  info.num_levels = mips ? static_cast<u32>(std::bit_width(std::max(width, height))) : 1;
  info.sample_count = SDL_GPU_SAMPLECOUNT_1;
  texture_ = SDL_CreateGPUTexture(device_, &info);
  bytes_ = u64{width} * height * bytes_per_pixel_ * (mips ? 4 : 3) / 3;
  if (!texture_) {
    log::error("texture {}x{}: SDL_CreateGPUTexture: {}", width, height, SDL_GetError());
    destroy();
    return false;
  }
  return true;
}

bool Texture::create(GpuContext& ctx, u32 width, u32 height, TextureFormat format, const void* pixels, UploadBatch* batch, bool mips) {
  if (!create_image(ctx, width, height, format, mips)) return false;
  const u32 pitch = row_pitch(width * bytes_per_pixel_);
  const u32 bytes = pitch * height;
  SDL_GPUTransferBufferCreateInfo transfer_info{};
  transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
  transfer_info.size = bytes;
  SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
  void* mapped = transfer ? SDL_MapGPUTransferBuffer(device_, transfer, false) : nullptr;
  if (!mapped) {
    log::error("texture {}x{}: staging buffer: {}", width, height, SDL_GetError());
    if (transfer) SDL_ReleaseGPUTransferBuffer(device_, transfer);
    destroy();
    return false;
  }
  copy_rows(mapped, pixels, width * bytes_per_pixel_, pitch, height);
  SDL_UnmapGPUTransferBuffer(device_, transfer);

  UploadOp op;
  op.source = transfer;
  op.texture = texture_;
  op.width = width;
  op.height = height;
  op.pixels_per_row = pitch / bytes_per_pixel_;
  op.mips = mips;
  if (batch) batch->ops_.push_back(op);
  else ctx.upload(op);
  return true;
}

bool Texture::create_blocking(GpuContext& ctx, u32 width, u32 height, TextureFormat format, const void* pixels) {
  if (!create_image(ctx, width, height, format, false)) return false;
  const u32 row_bytes = width * bytes_per_pixel_, pitch = row_pitch(row_bytes);
  const u32 band_rows = GpuContext::kBandBytes / pitch;
  std::lock_guard lock(ctx.band_mutex());
  SDL_GPUTransferBuffer* staging = ctx.band_staging();
  if (!staging || band_rows == 0) {
    destroy();
    return false;
  }
  for (u32 y = 0; y < height; y += band_rows) {
    const u32 rows = std::min(band_rows, height - y);
    void* mapped = SDL_MapGPUTransferBuffer(device_, staging, false);
    if (!mapped) {
      log::error("texture {}x{}: band staging map: {}", width, height, SDL_GetError());
      destroy();
      return false;
    }
    copy_rows(mapped, static_cast<const u8*>(pixels) + static_cast<size_t>(y) * row_bytes, row_bytes, pitch, rows);
    SDL_UnmapGPUTransferBuffer(device_, staging);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
      log::error("texture {}x{}: SDL_AcquireGPUCommandBuffer: {}", width, height, SDL_GetError());
      destroy();
      return false;
    }
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTextureTransferInfo source{staging, 0, pitch / bytes_per_pixel_, rows};
    const SDL_GPUTextureRegion destination{texture_, 0, 0, 0, y, 0, width, rows, 1};
    SDL_UploadToGPUTexture(pass, &source, &destination, false);
    SDL_EndGPUCopyPass(pass);
    // The staging buffer is rewritten by the next band: wait for this copy.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) {
      log::error("texture {}x{}: band submit: {}", width, height, SDL_GetError());
      destroy();
      return false;
    }
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);
  }
  return true;
}

bool UploadBatch::flush(GpuContext& ctx) {
  if (ops_.empty()) return true;
  ctx.upload(ops_);
  ops_.clear();
  return true;
}

bool Texture::update(GpuContext& ctx, const void* pixels) {
  if (!texture_) return false;
  const u32 pitch = row_pitch(width_ * bytes_per_pixel_);
  const u32 bytes = pitch * height_;
  if (!staging_) {
    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = bytes;
    staging_ = SDL_CreateGPUTransferBuffer(device_, &transfer_info);
  }
  void* mapped = staging_ ? SDL_MapGPUTransferBuffer(device_, staging_, true) : nullptr;
  if (!mapped) {
    log::error("texture update {}x{}: {}", width_, height_, SDL_GetError());
    return false;
  }
  copy_rows(mapped, pixels, width_ * bytes_per_pixel_, pitch, height_);
  SDL_UnmapGPUTransferBuffer(device_, staging_);

  UploadOp op;
  op.source = staging_;
  op.keep_source = true;
  op.texture = texture_;
  op.width = width_;
  op.height = height_;
  op.pixels_per_row = pitch / bytes_per_pixel_;
  op.cycle = true;
  ctx.upload(op);
  return true;
}

void Texture::destroy() {
  if (device_ && staging_) SDL_ReleaseGPUTransferBuffer(device_, staging_);
  if (ctx_ && texture_) ctx_->release_texture(texture_, bytes_);
  staging_ = nullptr;
  texture_ = nullptr;
  device_ = nullptr;
  ctx_ = nullptr;
  width_ = height_ = 0;
  bytes_ = 0;
}

bool Sampler::create(GpuContext& ctx, bool linear, bool repeat) {
  destroy();
  device_ = ctx.device();
  SDL_GPUSamplerCreateInfo info{};
  info.min_filter = info.mag_filter = linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
  info.mipmap_mode = linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  info.address_mode_u = info.address_mode_v = info.address_mode_w =
      repeat ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  info.max_lod = 1000.0f;
  sampler_ = SDL_CreateGPUSampler(device_, &info);
  if (!sampler_) {
    log::error("SDL_CreateGPUSampler: {}", SDL_GetError());
    return false;
  }
  ctx_ = &ctx;
  if (linear) ctx.register_linear_sampler(sampler_, info);
  return true;
}

void Sampler::destroy() {
  if (ctx_ && sampler_) ctx_->unregister_sampler(sampler_);
  if (device_ && sampler_) SDL_ReleaseGPUSampler(device_, sampler_);
  sampler_ = nullptr;
  device_ = nullptr;
  ctx_ = nullptr;
}

}  // namespace oc::render
