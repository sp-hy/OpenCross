#include "ui/retry_prompt.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/log.h"

namespace oc::ui {

namespace {
// By language (en fr it de es jp): the heading, Retry, Retry after changes, Give up.
const std::array<std::array<const char*, 4>, 6> kLines = {{
    {"The party exhausted all energy...", "Retry", "Retry after changes", "Give up"},
    {"L'équipe est à court d'énergie...", "Réessayer", "Réessayer après modifications", "Abandonner"},
    {"Il gruppo ha esaurito le forze...", "Riprova", "Riprova dopo le modifiche", "Rinuncia"},
    {"Die Gruppe ist völlig erschöpft ...", "Erneut versuchen", "Nach Änderungen erneut versuchen", "Aufgeben"},
    {"El grupo ha agotado todas sus fuerzas.", "Reintentar", "Reintentar tras los cambios", "Rendirse"},
    {"パーティは力尽きてしまった…", "もう一度戦う", "準備してから戦う", "あきらめる"},
}};
// The window as the field's centred message window (mode 3): text area centred on PS1 (0xA0, 0x6C), 13 rows a line.
constexpr int kCentreX = 0xA0, kCentreY = 0x6C, kLineRows = 13;
constexpr float kCanvasPerPs1 = 4.0f;
}  // namespace

std::array<std::u32string, 4> RetryPrompt::lines(import::Lang lang) {
  const auto& row = kLines[std::clamp(static_cast<size_t>(lang), size_t{0}, kLines.size() - 1)];
  std::array<std::u32string, 4> out;
  for (size_t i = 0; i < out.size(); ++i) out[i] = import::utf8_to_u32(row[i]);
  return out;
}

void RetryPrompt::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites) {
  ctx_ = &ctx, vfs_ = &vfs, sprites_ = &sprites;
}

void RetryPrompt::open(bool hd, int skin_byte, import::Lang lang) {
  if (!ctx_) return;
  if (!box_ || box_lang_ != lang) {
    auto box = std::make_unique<DialogueDemo>();
    if (!box->init(*ctx_, *vfs_, *sprites_, lang)) {
      log::warn("retry: no message window; the game over follows");
      choice_ = Choice::GiveUp;
      return;
    }
    box->show_status(false);
    box->set_mode(hd ? ArtMode::Hd : ArtMode::Original);
    box_ = std::move(box);
    box_lang_ = lang;
  } else {
    box_->set_art_mode(hd ? ArtMode::Hd : ArtMode::Original);
  }
  box_->set_skin_byte(skin_byte);
  box_->set_portrait(-1);
  // The options are indented as the field's choice lines are, so the hand stands before them.
  const std::u32string indent = lang == import::Lang::Jp ? U"　　" : U"   ";
  const std::array<std::u32string, 4> text = lines(lang);
  const int line_count = static_cast<int>(text.size());
  const auto show = [&](int blank_lines) {
    import::Message message;
    for (int i = 0; i < line_count; ++i) {
      if (i > 0) message.tokens.push_back({import::TokKind::Newline});
      message.tokens.push_back({import::TokKind::Text, 0, 0, i > 0 ? indent + text[static_cast<size_t>(i)] : text[0]});
    }
    for (int i = 0; i < blank_lines; ++i) {
      message.tokens.push_back({import::TokKind::Newline});
      message.tokens.push_back({import::TokKind::Text, 0, 0, U"　"});
    }
    message.tokens.push_back({import::TokKind::End});
    box_->show_text(message);
    box_->set_line_choice(1, line_count - 1);
  };
  show(0);
  // The frame grows by one fill row a line; a face whose lines are taller than a row (the Japanese one) gets blank lines at the end
  // until the frame holds every line.
  const float row = DialogueBox::frame_size(0, 0.0f, 2).h - DialogueBox::frame_size(0, 0.0f, 1).h;
  if (const int rows = static_cast<int>(std::ceil(box_->text_height() / row)); row > 0.0f && rows > line_count) show(rows - line_count);
  const int width = static_cast<int>(box_->text_width() / kCanvasPerPs1);
  box_->set_ps1_window(kCentreX - width / 2, kCentreY - line_count * kLineRows / 2);
  box_->show_at_once();
  open_ = true;
  choice_ = Choice::None;
}

int RetryPrompt::input(int dy, bool confirm) {
  if (!open_ || !box_) return 0;
  if (dy != 0) {
    box_->move_choice(dy);
    return 1;
  }
  if (!confirm) return 0;
  box_->confirm();
  switch (box_->chosen()) {
    case 1: choice_ = Choice::Retry; break;
    case 2: choice_ = Choice::RetryAfterChanges; break;
    case 3: choice_ = Choice::GiveUp; break;
    default: return 0;
  }
  open_ = false;
  box_->close();
  return 8;
}

void RetryPrompt::update(float dt) {
  if (open_ && box_) box_->update(dt);
}

void RetryPrompt::draw(float vw, float vh) {
  if (open_ && box_) box_->draw(vw, vh);
}

RetryPrompt::Choice RetryPrompt::take_choice() { return std::exchange(choice_, Choice::None); }

}  // namespace oc::ui
