#include "render/gpu_context.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "core/log.h"
#include "render/buffer.h"
#include "render/render_target.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// The shader formats embedded by src/render/CMakeLists.txt (SPIR-V always; DXIL / MSL when shadercross produced them).
#ifndef OC_SHADER_FORMATS
#define OC_SHADER_FORMATS SDL_GPU_SHADERFORMAT_SPIRV
#endif

namespace oc::render {

namespace {

std::atomic<u32> g_gpu_messages{0};
SDL_LogOutputFunction g_previous_log = nullptr;
void* g_previous_log_user = nullptr;

void SDLCALL gpu_log(void* user, int category, SDL_LogPriority priority, const char* message) {
  if (category == SDL_LOG_CATEGORY_GPU && priority >= SDL_LOG_PRIORITY_WARN) {
    ++g_gpu_messages;
    if (priority >= SDL_LOG_PRIORITY_ERROR) log::error("[gpu] {}", message);
    else log::warn("[gpu] {}", message);
    return;
  }
  if (g_previous_log) g_previous_log(user, category, priority, message);
}

SDL_GPUTexture* make_image(SDL_GPUDevice* device, u32 width, u32 height, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage,
                           const char* name, SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1) {
  SDL_GPUTextureCreateInfo info{};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = format;
  info.usage = usage;
  info.width = width;
  info.height = height;
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  info.sample_count = samples;
  SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &info);
  if (!texture) log::error("{} {}x{}: SDL_CreateGPUTexture: {}", name, width, height, SDL_GetError());
  else SDL_SetGPUTextureName(device, texture, name);
  return texture;
}

}  // namespace

u32 GpuContext::validation_message_count() { return g_gpu_messages.load(); }

std::vector<std::string> GpuContext::available_drivers() {
  std::vector<std::string> names;
  const int count = SDL_GetNumGPUDrivers();
  for (int i = 0; i < count; ++i) names.emplace_back(SDL_GetGPUDriver(i));
  return names;
}

const char* GpuContext::driver() const { return device_ ? SDL_GetGPUDeviceDriver(device_) : ""; }

bool GpuContext::init(SDL_Window* window, bool vsync, const std::string& driver) {
  window_ = window;
  vsync_ = vsync;
  SDL_SetLogPriority(SDL_LOG_CATEGORY_GPU, SDL_LOG_PRIORITY_WARN);
  if (!g_previous_log) SDL_GetLogOutputFunction(&g_previous_log, &g_previous_log_user);
  SDL_SetLogOutputFunction(gpu_log, g_previous_log_user);

  if (!create_device(driver)) return false;
  if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
    log::error("SDL_ClaimWindowForGPUDevice: {}", SDL_GetError());
    return false;
  }
  SDL_SetGPUAllowedFramesInFlight(device_, kFramesInFlight);
  for (SDL_GPUTextureFormat format : {SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTUREFORMAT_D16_UNORM}) {
    depth_format_ = format;
    if (SDL_GPUTextureSupportsFormat(device_, format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) break;
  }
  if (!apply_swapchain()) return false;

  white_texture_ = make_image(device_, 1, 1, color_format(), SDL_GPU_TEXTUREUSAGE_SAMPLER, "white");
  SDL_GPUSamplerCreateInfo si{};
  si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
  si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  white_sampler_ = SDL_CreateGPUSampler(device_, &si);
  SDL_GPUTransferBufferCreateInfo ti{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, 256, 0};
  SDL_GPUTransferBuffer* staging = SDL_CreateGPUTransferBuffer(device_, &ti);
  if (!white_texture_ || !white_sampler_ || !staging) {
    log::error("white texture: {}", SDL_GetError());
    return false;
  }
  if (void* mapped = SDL_MapGPUTransferBuffer(device_, staging, false)) {
    std::memset(mapped, 0xFF, 256);
    SDL_UnmapGPUTransferBuffer(device_, staging);
  }
  UploadOp op;
  op.source = staging;
  op.texture = white_texture_;
  op.width = op.height = 1;
  op.pixels_per_row = 64;
  upload(op);
  white_ = TexRef(white_texture_, white_sampler_, 1, 1);
  return true;
}

bool GpuContext::create_device(const std::string& driver) {
  SDL_PropertiesID props = SDL_CreateProperties();
#ifndef NDEBUG
  SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
#endif
  SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, (OC_SHADER_FORMATS & SDL_GPU_SHADERFORMAT_SPIRV) != 0);
  SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, (OC_SHADER_FORMATS & SDL_GPU_SHADERFORMAT_DXIL) != 0);
  SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, (OC_SHADER_FORMATS & SDL_GPU_SHADERFORMAT_MSL) != 0);
  if (!driver.empty()) SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, driver.c_str());
  device_ = SDL_CreateGPUDeviceWithProperties(props);
  if (!device_ && !driver.empty()) {
    log::warn("GPU driver '{}' unavailable ({}); using SDL's choice", driver, SDL_GetError());
    SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, nullptr);
    device_ = SDL_CreateGPUDeviceWithProperties(props);
    driver_fallback_ = device_ != nullptr;
  }
  SDL_DestroyProperties(props);
  if (!device_) {
    log::error("SDL_CreateGPUDevice: {}", SDL_GetError());
    return false;
  }
  const SDL_PropertiesID info = SDL_GetGPUDeviceProperties(device_);
  log::info("GPU: {} ({} driver {})", SDL_GetStringProperty(info, SDL_PROP_GPU_DEVICE_NAME_STRING, "?"), SDL_GetGPUDeviceDriver(device_),
            SDL_GetStringProperty(info, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, "?"));
  return true;
}

bool GpuContext::apply_swapchain() {
  SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
  if (!vsync_) {
    if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_MAILBOX)) mode = SDL_GPU_PRESENTMODE_MAILBOX;
    else if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE)) mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
  }
  if (!SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode)) {
    log::error("SDL_SetGPUSwapchainParameters: {}", SDL_GetError());
    return false;
  }
  static const char* mode_names[] = {"VSYNC", "IMMEDIATE", "MAILBOX"};
  log::info("swapchain: present {}", mode_names[mode]);
  rebuild_swapchain_ = false;
  return true;
}

bool GpuContext::ensure_frame_images(u32 width, u32 height) {
  const SDL_GPUSampleCount samples = sample_count_for(width, height);
  if (frame_color_ && frame_depth_ && extent_.width == width && extent_.height == height && frame_samples_ == samples) return true;
  for (SDL_GPUTexture* image : {frame_color_, frame_depth_, frame_msaa_color_, frame_overlay_depth_})
    if (image) SDL_ReleaseGPUTexture(device_, image);
  frame_msaa_color_ = frame_overlay_depth_ = nullptr;
  frame_color_ = make_image(device_, width, height, color_format(), SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, "frame colour");
  frame_depth_ = make_image(device_, width, height, depth_format_, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, "frame depth", samples);
  frame_samples_ = samples;
  extent_ = {width, height};
  if (!frame_color_ || !frame_depth_) return false;
  if (samples != SDL_GPU_SAMPLECOUNT_1) {
    frame_msaa_color_ = make_image(device_, width, height, color_format(), SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, "frame msaa colour", samples);
    frame_overlay_depth_ = make_image(device_, width, height, depth_format_, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, "frame overlay depth");
    if (!frame_msaa_color_ || !frame_overlay_depth_) {
      msaa_ = 1;
      return ensure_frame_images(width, height);
    }
  }
  log::info("frame images: {}x{}, {} samples", width, height, 1u << static_cast<u32>(samples));
  return true;
}

void GpuContext::set_msaa(u32 samples) {
  u32 supported = 1;
  for (const u32 count : {2u, 4u, 8u})
    if (count <= samples && SDL_GPUTextureSupportsSampleCount(device_, color_format(), sample_count(count)) &&
        SDL_GPUTextureSupportsSampleCount(device_, depth_format_, sample_count(count)))
      supported = count;
  if (supported == msaa_) return;
  msaa_ = supported;
  for (RenderTarget* target : multisampled_targets_) target->release_samples();
}

SDL_GPUSampleCount GpuContext::sample_count_for(u32 width, u32 height) const {
  u32 samples = msaa_;
  while (samples > 1 && static_cast<u64>(width) * height * samples > kMaxMsaaSamples) samples /= 2;
  return sample_count(samples);
}

void GpuContext::track_multisampled(RenderTarget* target, bool tracked) {
  std::erase(multisampled_targets_, target);
  if (tracked) multisampled_targets_.push_back(target);
}

void GpuContext::release_texture(SDL_GPUTexture* texture, u64 bytes) {
  if (!texture || !device_) return;
  std::lock_guard lock(release_mutex_);
  release_queue_.push_back({texture, bytes});
}

void GpuContext::release_queued(u64 bytes, size_t count) {
  std::lock_guard lock(release_mutex_);
  u64 freed = 0;
  size_t released = 0;
  while (released < release_queue_.size() && (released == 0 || (released < count && freed + release_queue_[released].bytes <= bytes))) {
    SDL_ReleaseGPUTexture(device_, release_queue_[released].texture);
    freed += release_queue_[released].bytes;
    ++released;
  }
  release_queue_.erase(release_queue_.begin(), release_queue_.begin() + static_cast<std::ptrdiff_t>(released));
}

void GpuContext::resolve(const PassImages& images) {
  if (!images.resolve) return;
  SDL_GPUColorTargetInfo color_info{};
  color_info.texture = images.color;
  color_info.load_op = SDL_GPU_LOADOP_LOAD;
  color_info.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
  color_info.resolve_texture = images.resolve;
  if (SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd_, &color_info, 1, nullptr)) SDL_EndGPURenderPass(pass);
  else log::error("resolve: SDL_BeginGPURenderPass: {}", SDL_GetError());
}

void GpuContext::set_anisotropy(u32 level) {
  level = std::clamp(level, 1u, 16u);
  if (level == anisotropy_) return;
  anisotropy_ = level;
  release_anisotropic_samplers();
}

SDL_GPUSampler* GpuContext::filtered(SDL_GPUSampler* sampler) {
  if (anisotropy_ <= 1) return sampler;
  std::lock_guard lock(samplers_mutex_);
  const auto it = linear_samplers_.find(sampler);
  if (it == linear_samplers_.end()) return sampler;
  LinearSampler& linear = it->second;
  if (!linear.anisotropic) {
    SDL_GPUSamplerCreateInfo info = linear.info;
    info.enable_anisotropy = true;
    info.max_anisotropy = static_cast<float>(anisotropy_);
    linear.anisotropic = SDL_CreateGPUSampler(device_, &info);
    if (!linear.anisotropic) {
      log::error("anisotropic sampler: {}", SDL_GetError());
      return sampler;
    }
  }
  return linear.anisotropic;
}

void GpuContext::register_linear_sampler(SDL_GPUSampler* sampler, const SDL_GPUSamplerCreateInfo& info) {
  std::lock_guard lock(samplers_mutex_);
  linear_samplers_[sampler] = {info, nullptr};
}

void GpuContext::unregister_sampler(SDL_GPUSampler* sampler) {
  std::lock_guard lock(samplers_mutex_);
  const auto it = linear_samplers_.find(sampler);
  if (it == linear_samplers_.end()) return;
  if (it->second.anisotropic) SDL_ReleaseGPUSampler(device_, it->second.anisotropic);
  linear_samplers_.erase(it);
}

void GpuContext::release_anisotropic_samplers() {
  std::lock_guard lock(samplers_mutex_);
  for (auto& [sampler, linear] : linear_samplers_) {
    if (linear.anisotropic) SDL_ReleaseGPUSampler(device_, linear.anisotropic);
    linear.anisotropic = nullptr;
  }
}

void GpuContext::set_vsync(bool on) {
  if (on == vsync_) return;
  vsync_ = on;
  rebuild_swapchain_ = true;
}

void GpuContext::set_clear_color(float r, float g, float b, float a) {
  clear_[0] = r;
  clear_[1] = g;
  clear_[2] = b;
  clear_[3] = a;
}

void GpuContext::drain_releases() {
  if (!device_) return;
  SDL_WaitForGPUIdle(device_);
  release_queued(~0ull, ~size_t{0});
  SDL_WaitForGPUIdle(device_);
}

void GpuContext::wait_idle() {
  if (device_) SDL_WaitForGPUIdle(device_);
}

SDL_GPUCommandBuffer* GpuContext::upload_cmd() {
  if (!upload_cmd_) upload_cmd_ = SDL_AcquireGPUCommandBuffer(device_);
  return upload_cmd_;
}

bool GpuContext::begin_frame(Frame& out) {
  if (rebuild_swapchain_ && !apply_swapchain()) return false;
  cmd_ = SDL_AcquireGPUCommandBuffer(device_);
  if (!cmd_) {
    log::error("SDL_AcquireGPUCommandBuffer: {}", SDL_GetError());
    return false;
  }
  u32 width = 0, height = 0;
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd_, window_, &swapchain_image_, &width, &height)) {
    log::error("SDL_WaitAndAcquireGPUSwapchainTexture: {}", SDL_GetError());
    swapchain_image_ = nullptr;
  }
  if (!swapchain_image_ || width == 0 || height == 0 || !ensure_frame_images(width, height)) {
    SDL_CancelGPUCommandBuffer(cmd_);
    cmd_ = nullptr;
    swapchain_image_ = nullptr;
    return false;
  }
  targets_.clear();
  streams_.clear();
  held_depth_ = nullptr;
  suspended_ = false;
  in_frame_ = true;
  overlay_ = false;
  begin_rendering(current_images(), true, clear_);

  out.ctx = this;
  out.index = frame_index_;
  out.serial = ++frame_serial_;
  out.extent = extent_;
  return true;
}

void GpuContext::begin_rendering(const PassImages& images, bool clear_color, const float (&rgba)[4], bool load_depth) {
  end_rendering();
  const Extent extent = images.extent;
  SDL_GPUColorTargetInfo color_info{};
  color_info.texture = images.color;
  color_info.clear_color = {rgba[0], rgba[1], rgba[2], rgba[3]};
  color_info.load_op = clear_color ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
  color_info.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPUDepthStencilTargetInfo depth_info{};
  depth_info.texture = images.depth;
  depth_info.clear_depth = 1.0f;
  depth_info.load_op = load_depth ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
  depth_info.store_op = images.depth && images.depth == held_depth_ ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
  depth_info.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
  depth_info.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
  pass_ = SDL_BeginGPURenderPass(cmd_, &color_info, 1, &depth_info);
  pass_extent_ = extent;
  pass_samples_ = images.samples;
  if (!pass_) {
    log::error("SDL_BeginGPURenderPass: {}", SDL_GetError());
    return;
  }
  const SDL_GPUViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
  SDL_SetGPUViewport(pass_, &viewport);
  set_scissor({0, 0, extent.width, extent.height});
}

void GpuContext::end_rendering() {
  if (pass_) SDL_EndGPURenderPass(pass_);
  pass_ = nullptr;
  pass_samples_ = SDL_GPU_SAMPLECOUNT_1;
}

GpuContext::PassImages GpuContext::current_images() const {
  if (!targets_.empty()) {
    const RenderTarget& target = *targets_.back();
    SDL_GPUTexture* msaa_color = target.msaa_color();
    return {msaa_color ? msaa_color : target.color(), msaa_color ? target.color() : nullptr, target.depth(), {target.width(), target.height()},
            target.sample_count()};
  }
  if (frame_msaa_color_ && !overlay_) return {frame_msaa_color_, frame_color_, frame_depth_, extent_, frame_samples_};
  return {frame_color_, nullptr, frame_overlay_depth_ ? frame_overlay_depth_ : frame_depth_, extent_, SDL_GPU_SAMPLECOUNT_1};
}

void GpuContext::begin_overlay() {
  if (!in_frame_ || overlay_ || !frame_msaa_color_ || !targets_.empty()) return;
  end_rendering();
  resolve(current_images());
  overlay_ = true;
  begin_rendering(current_images(), false, clear_);
}

void GpuContext::begin_target(RenderTarget& target, const Frame& frame, Frame& out) { begin_target(target, frame, out, clear_); }

void GpuContext::begin_target(RenderTarget& target, const Frame& frame, Frame& out, const float (&clear_rgba)[4]) {
  target.prepare_samples(*this);
  targets_.push_back(&target);
  begin_rendering(current_images(), true, clear_rgba);
  out = frame;
  out.extent = {target.width(), target.height()};
}

void GpuContext::end_target(RenderTarget& target) {
  if (!targets_.empty() && targets_.back() == &target) {
    end_rendering();
    resolve(current_images());
    targets_.pop_back();
  }
  if (suspended_ && targets_.size() == suspended_level_) return;
  begin_rendering(current_images(), false, clear_);
}

void GpuContext::copy_color(RenderTarget& target) {
  if (!in_frame_ || !target.valid()) return;
  end_rendering();
  const PassImages images = current_images();
  resolve(images);
  SDL_GPUBlitInfo blit{};
  blit.source = {images.resolve ? images.resolve : images.color, 0, 0, 0, 0, images.extent.width, images.extent.height};
  blit.destination = {target.color(), 0, 0, 0, 0, target.width(), target.height()};
  blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
  blit.filter = SDL_GPU_FILTER_LINEAR;
  SDL_BlitGPUTexture(cmd_, &blit);
  begin_rendering(images, false, clear_);
}

void GpuContext::hold_depth() {
  if (!in_frame_ || !pass_ || suspended_) return;
  const PassImages images = current_images();
  held_depth_ = images.depth;
  begin_rendering(images, false, clear_);
}

TexRef GpuContext::suspend_rendering(SDL_GPUSampler* sampler) {
  if (!in_frame_ || !pass_ || suspended_) return {};
  end_rendering();
  const PassImages images = current_images();
  resolve(images);
  suspended_ = true;
  suspended_level_ = targets_.size();
  return {images.resolve ? images.resolve : images.color, sampler, images.extent.width, images.extent.height};
}

void GpuContext::resume_rendering() {
  if (!in_frame_ || !suspended_) return;
  suspended_ = false;
  const PassImages images = current_images();
  const bool load_depth = images.depth && images.depth == held_depth_;
  held_depth_ = nullptr;
  begin_rendering(images, false, clear_, load_depth);
}

void GpuContext::clear_depth() {
  if (!in_frame_) return;
  begin_rendering(current_images(), false, clear_);
}

void GpuContext::set_scissor(const Rect& rect) {
  if (!pass_) return;
  const s32 max_w = static_cast<s32>(pass_extent_.width), max_h = static_cast<s32>(pass_extent_.height);
  const s32 x0 = std::clamp(rect.x, 0, max_w), y0 = std::clamp(rect.y, 0, max_h);
  const s32 x1 = std::clamp(rect.x + static_cast<s32>(rect.w), x0, max_w), y1 = std::clamp(rect.y + static_cast<s32>(rect.h), y0, max_h);
  const SDL_Rect scissor{x0, y0, x1 - x0, y1 - y0};
  SDL_SetGPUScissor(pass_, &scissor);
}

SDL_GPUTransferBuffer* GpuContext::band_staging() {
  if (!band_staging_ && device_) {
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = kBandBytes;
    band_staging_ = SDL_CreateGPUTransferBuffer(device_, &info);
    if (!band_staging_) log::error("band staging buffer: {}", SDL_GetError());
  }
  return band_staging_;
}

void GpuContext::upload(std::span<const UploadOp> ops) {
  if (ops.empty() || !device_) return;
  SDL_GPUCommandBuffer* cmd = in_frame_ ? upload_cmd() : SDL_AcquireGPUCommandBuffer(device_);
  if (!cmd) {
    log::error("upload: SDL_AcquireGPUCommandBuffer: {}", SDL_GetError());
    return;
  }
  encode(cmd, ops);
  if (!in_frame_) SDL_SubmitGPUCommandBuffer(cmd);
}

void GpuContext::encode(SDL_GPUCommandBuffer* cmd, std::span<const UploadOp> ops) {
  SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
  for (const UploadOp& op : ops) {
    if (op.texture) {
      const SDL_GPUTextureTransferInfo source{op.source, op.source_offset, op.pixels_per_row ? op.pixels_per_row : op.width, op.height};
      const SDL_GPUTextureRegion destination{op.texture, 0, 0, 0, 0, 0, op.width, op.height, 1};
      SDL_UploadToGPUTexture(pass, &source, &destination, op.cycle);
    } else if (op.buffer) {
      const SDL_GPUTransferBufferLocation source{op.source, op.source_offset};
      const SDL_GPUBufferRegion destination{op.buffer, op.buffer_offset, op.size};
      SDL_UploadToGPUBuffer(pass, &source, &destination, false);
    }
  }
  SDL_EndGPUCopyPass(pass);
  for (const UploadOp& op : ops) {
    if (op.texture && op.mips) SDL_GenerateMipmapsForGPUTexture(cmd, op.texture);
    if (!op.keep_source && op.source) SDL_ReleaseGPUTransferBuffer(device_, op.source);
  }
}

void GpuContext::flush_streams() {
  if (streams_.empty()) return;
  SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(upload_cmd());
  for (StreamBuffer* stream : streams_) stream->flush(pass);
  SDL_EndGPUCopyPass(pass);
  streams_.clear();
}

// A requested screenshot / capture reads the frame's colour image back once the frame's fence signals.
void GpuContext::end_frame() {
  if (!in_frame_) return;
  in_frame_ = false;
  end_rendering();
  targets_.clear();
  resolve(current_images());

  const bool capture = !screenshot_path_.empty() || static_cast<bool>(capture_cb_);
  SDL_GPUTransferBuffer* download = nullptr;
  if (capture) {
    const SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, extent_.width * extent_.height * 4, 0};
    download = SDL_CreateGPUTransferBuffer(device_, &info);
    if (download) {
      SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd_);
      const SDL_GPUTextureRegion source{frame_color_, 0, 0, 0, 0, 0, extent_.width, extent_.height, 1};
      const SDL_GPUTextureTransferInfo destination{download, 0, extent_.width, extent_.height};
      SDL_DownloadFromGPUTexture(pass, &source, &destination);
      SDL_EndGPUCopyPass(pass);
    } else {
      log::error("screenshot: readback buffer allocation failed: {}", SDL_GetError());
    }
  }

  SDL_GPUBlitInfo blit{};
  blit.source = {frame_color_, 0, 0, 0, 0, extent_.width, extent_.height};
  blit.destination = {swapchain_image_, 0, 0, 0, 0, extent_.width, extent_.height};
  blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
  blit.filter = SDL_GPU_FILTER_NEAREST;
  SDL_BlitGPUTexture(cmd_, &blit);

  flush_streams();
  if (upload_cmd_) {
    SDL_SubmitGPUCommandBuffer(upload_cmd_);
    upload_cmd_ = nullptr;
  }

  if (download) {
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd_);
    if (fence) {
      SDL_WaitForGPUFences(device_, true, &fence, 1);
      SDL_ReleaseGPUFence(device_, fence);
    }
    const std::filesystem::path shot_path = screenshot_path_;
    auto callback = std::move(capture_cb_);
    screenshot_path_.clear();
    capture_cb_ = nullptr;
    std::vector<u8> px(static_cast<size_t>(extent_.width) * extent_.height * 4);
    if (void* mapped = SDL_MapGPUTransferBuffer(device_, download, false)) {
      std::memcpy(px.data(), mapped, px.size());
      SDL_UnmapGPUTransferBuffer(device_, download);
      for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
      if (callback) callback(std::vector<u8>(px), extent_.width, extent_.height);
      if (!shot_path.empty()) {
        if (stbi_write_png(shot_path.string().c_str(), static_cast<int>(extent_.width), static_cast<int>(extent_.height), 4, px.data(),
                           static_cast<int>(extent_.width) * 4))
          log::info("screenshot saved: {}", shot_path.string());
        else
          log::error("screenshot: could not write {}", shot_path.string());
      }
    } else {
      log::error("screenshot: SDL_MapGPUTransferBuffer: {}", SDL_GetError());
    }
    SDL_ReleaseGPUTransferBuffer(device_, download);
  } else {
    if (capture) {
      screenshot_path_.clear();
      capture_cb_ = nullptr;
    }
    SDL_SubmitGPUCommandBuffer(cmd_);
  }
  cmd_ = nullptr;
  swapchain_image_ = nullptr;
  frame_index_ = (frame_index_ + 1) % kFramesInFlight;
  release_queued(kReleaseBytesPerFrame, kReleaseTexturesPerFrame);
}

void GpuContext::shutdown() {
  if (!device_) return;
  SDL_WaitForGPUIdle(device_);
  release_queued(~0ull, ~size_t{0});
  if (upload_cmd_) SDL_CancelGPUCommandBuffer(upload_cmd_);
  upload_cmd_ = nullptr;
  if (band_staging_) SDL_ReleaseGPUTransferBuffer(device_, band_staging_);
  band_staging_ = nullptr;
  if (white_texture_) SDL_ReleaseGPUTexture(device_, white_texture_);
  if (white_sampler_) SDL_ReleaseGPUSampler(device_, white_sampler_);
  release_anisotropic_samplers();
  linear_samplers_.clear();
  white_texture_ = nullptr;
  white_sampler_ = nullptr;
  white_ = TexRef();
  for (SDL_GPUTexture* image : {frame_color_, frame_depth_, frame_msaa_color_, frame_overlay_depth_})
    if (image) SDL_ReleaseGPUTexture(device_, image);
  frame_color_ = frame_depth_ = frame_msaa_color_ = frame_overlay_depth_ = nullptr;
  extent_ = {};
  SDL_ReleaseWindowFromGPUDevice(device_, window_);
  SDL_DestroyGPUDevice(device_);
  device_ = nullptr;
  SDL_SetLogOutputFunction(g_previous_log ? g_previous_log : SDL_GetDefaultLogOutputFunction(), g_previous_log_user);
}

}  // namespace oc::render
