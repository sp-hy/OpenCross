#include "ui/disc_prompt.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "import/container.h"
#include "import/tim.h"
#include "render/camera.h"

namespace oc::ui {

namespace {
// map/etc/systim.bin entry 5: 256 x 32 texels, 4bpp. Rows 0..15 = the prompt line with an empty digit cell; rows 16..31 = the
// "wrong disc" line in u 0..127 and the digits 1 and 2 as 12 x 16 cells at u 140 and 156.
constexpr int kTimEntry = 5, kTimW = 256, kTimH = 32, kLineH = 16;
constexpr int kDigitW = 12, kDigitU[2] = {140, 156};
// The lines' screen rectangles in field rows / columns: the prompt is 255 texels wide, the "wrong disc" line 128, both at row 104.
constexpr float kLineY = 104.0f, kPromptW = 255.0f, kErrorW = 128.0f;
struct LayoutInfo {
  float prompt_x, error_x;
  // Where the digit cell goes in the prompt line.
  int digit_u;
};
constexpr LayoutInfo kLayouts[2] = {{57.0f, 97.0f, 44}, {72.0f, 109.0f, 160}};
// The atlas: the prompt for disc 1, the prompt for disc 2, the "wrong disc" line, one 16-row band each.
constexpr int kAtlasH = 3 * kLineH;
}  // namespace

bool DiscPrompt::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::TextBatch& text) {
  ctx_ = &ctx, sprites_ = &sprites;
  if (!nearest_.create(ctx, false)) return false;
  const u8 px[4] = {255, 255, 255, 255};
  if (!white_.create(ctx, 1, 1, render::TextureFormat::Rgba8, px)) return false;
  white_set_ = render::TexRef(white_, nearest_);
  fonts_.init(vfs, text);
  fonts_.set_classic(true);
  auto raw = vfs.read("cdrom/map/etc/systim.bin");
  auto cpt = raw ? import::parse_cpt(ByteSpan(*raw)) : std::nullopt;
  if (!cpt || cpt->entries.size() <= static_cast<size_t>(kTimEntry)) {
    log::warn("disc prompt: map/etc/systim.bin unavailable; no prompt text");
    return true;
  }
  auto tim = import::parse_tim(cpt->entries[kTimEntry].data);
  if (!tim || tim->bpp != 4 || tim->image.w * 4 != kTimW || tim->image.h != kTimH) {
    log::warn("disc prompt: systim.bin TIM {} is not the 256 x 32 prompt sheet; no prompt text", kTimEntry);
    return true;
  }
  auto image = import::decode_tim(*tim);
  if (!image) return true;
  tim_rgba_ = import::to_rgba(*image).pixels;
  return true;
}

void DiscPrompt::shutdown() {
  if (!ctx_) return;
  art_set_ = {};
  white_set_ = {};
  art_.destroy();
  white_.destroy();
  nearest_.destroy();
  ctx_ = nullptr;
}

// The PS1 pastes the needed digit into the prompt line in VRAM; the atlas holds the line once per digit.
void DiscPrompt::build_art() {
  const int layout = static_cast<int>(layout_);
  if (built_layout_ == layout || tim_rgba_.empty() || !ctx_) return;
  built_layout_ = layout;
  constexpr size_t kRow = static_cast<size_t>(kTimW) * 4;
  std::vector<u8> atlas(kRow * kAtlasH, 0);
  for (int digit = 0; digit < 2; ++digit) {
    for (int y = 0; y < kLineH; ++y) {
      u8* dst = &atlas[(static_cast<size_t>(digit) * kLineH + static_cast<size_t>(y)) * kRow];
      std::memcpy(dst, &tim_rgba_[static_cast<size_t>(y) * kRow], kRow);
      std::memcpy(dst + static_cast<size_t>(kLayouts[layout].digit_u) * 4,
                  &tim_rgba_[static_cast<size_t>(kLineH + y) * kRow + static_cast<size_t>(kDigitU[digit]) * 4], kDigitW * 4);
    }
  }
  std::memcpy(&atlas[2 * kLineH * kRow], &tim_rgba_[kLineH * kRow], kLineH * kRow);
  art_set_ = {};
  art_.destroy();
  if (art_.create(*ctx_, kTimW, kAtlasH, render::TextureFormat::Rgba8, atlas.data())) art_set_ = render::TexRef(art_, nearest_);
}

void DiscPrompt::open(int needed_disc) {
  if (active() && needed_disc == disc_) return;
  disc_ = needed_disc;
  phase_ = Phase::FadeIn;
  frame_ = 0;
  pending_ = false;
  last_ = MountResult::Ok;
  log::info("disc prompt: disc {} needed", needed_disc);
}

void DiscPrompt::attempt() {
  pending_ = false;
  last_ = host_.try_mount ? host_.try_mount(disc_) : MountResult::NoDisc;
  log::info("disc prompt: disc {} attempt: {}", disc_,
            last_ == MountResult::Ok ? "mounted" : last_ == MountResult::NoDisc ? "no disc" : last_ == MountResult::WrongDisc ? "wrong disc" : "wrong build");
  phase_ = last_ == MountResult::Ok ? Phase::FadeOut : Phase::Error;
  frame_ = 0;
}

void DiscPrompt::update(const Input& input) {
  if (!active()) return;
  if (phase_ != Phase::FadeOut) {
    if (input.browse && host_.browse) host_.browse(disc_);
    if (input.rescan || (host_.changed && host_.changed())) pending_ = true;
  }
  ++frame_;
  switch (phase_) {
    case Phase::Closed:
      break;
    case Phase::FadeIn:
      if (frame_ >= kFadeInFrames) phase_ = Phase::Waiting, frame_ = 0, held_level_ = (kFadeInFrames - 1) * kLevelStep;
      break;
    case Phase::Waiting:
      break;
    case Phase::Error:
      if (frame_ >= kErrorFrames + kRecoverFrames) phase_ = Phase::Waiting, frame_ = 0, held_level_ = kHoldLevel;
      break;
    case Phase::FadeOut:
      if (frame_ >= kFadeOutFrames) phase_ = Phase::Closed, frame_ = 0;
      break;
  }
  if (phase_ == Phase::Waiting && pending_) attempt();
}

int DiscPrompt::level() const {
  switch (phase_) {
    case Phase::Closed:
      return 0;
    case Phase::FadeIn:
      return frame_ * kLevelStep;
    case Phase::Waiting:
      return held_level_;
    case Phase::Error:
      return kHoldLevel;
    case Phase::FadeOut:
      return std::max(0, (kFadeOutFrames - frame_) * kLevelStep);
  }
  return 0;
}

DiscPrompt::Line DiscPrompt::line() const {
  switch (phase_) {
    case Phase::Closed:
    case Phase::FadeOut:
      return Line::None;
    case Phase::FadeIn:
    case Phase::Waiting:
      return Line::Prompt;
    case Phase::Error:
      return frame_ < kErrorFrames ? Line::WrongDisc : Line::Prompt;
  }
  return Line::None;
}

void DiscPrompt::draw(const render::Frame& frame, float vw, float vh, bool resume) {
  if (!active() || !sprites_ || !white_set_) return;
  build_art();
  const bool hint = !hint_.empty() && phase_ != Phase::FadeOut;
  if (hint) fonts_.prepare(font::kBn, hint_);
  sprites_->begin(frame, vw, vh, resume);
  if (const int amount = level(); amount > 0) {
    const auto rect = render::Letterbox::veil_rect(vw, vh);
    const u8 grey = static_cast<u8>(std::min(amount, 255));
    sprites_->set_blend(render::BlendMode::Subtract);
    sprites_->draw(white_set_, rect.x, rect.y, rect.w, rect.h, 0, 0, 1, 1, render::pack_rgba(grey, grey, grey, 255));
    sprites_->set_blend(render::BlendMode::Alpha);
  }
  const render::Letterbox lb = render::Letterbox::field(vw, vh);
  const Line shown = line();
  if (art_set_ && shown != Line::None) {
    const LayoutInfo& info = kLayouts[static_cast<int>(layout_)];
    const bool prompt = shown == Line::Prompt;
    const float x = prompt ? info.prompt_x : info.error_x, w = prompt ? kPromptW : kErrorW;
    const float band = prompt ? static_cast<float>(std::clamp(disc_, 1, 2) - 1) : 2.0f;
    const float v0 = band * kLineH / kAtlasH, v1 = (band + 1.0f) * kLineH / kAtlasH;
    sprites_->draw(art_set_, lb.ox + x * lb.scale, lb.oy + kLineY * lb.scale_y, w * lb.scale, kLineH * lb.scale_y, 0.0f, v0,
                   w / kTimW, v1);
  }
  if (hint) {
    const render::CanvasFit fit = render::canvas_fit(vw, vh);
    const float k = fit.s;
    fonts_.draw(font::kBn, hint_, (fit.ox + 640.0f * k) / k, (fit.oy + 880.0f * k) / k, font::h1(0x606060, 31, 4),
                font::slot(font::kBn).scale, font::kCentre, k);
  }
  sprites_->end();
}

}  // namespace oc::ui
