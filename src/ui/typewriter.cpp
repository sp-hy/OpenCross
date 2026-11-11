#include "ui/typewriter.h"

#include <algorithm>
#include <array>

namespace oc::ui {

using import::TokKind;

namespace {

// The pen of one line. width sums the advances; with a grid the glyphs sit on whole grid steps, the pen dropping the fraction of a step
// at every glyph.
struct Pen {
  float grid = 0;
  float width = 0;
  int steps = 0;

  float x() const { return grid > 0 ? static_cast<float>(steps) * grid : width; }
  void advance(float by) {
    width += by;
    if (grid > 0) steps = static_cast<int>(static_cast<float>(steps) + by / grid);
  }
};

// Collects a page's glyphs with their advances; a finished page is then word-wrapped and placed (place_page). Events are recorded at the
// glyph index they fire before.
struct PageBuilder {
  const LayoutMetrics& metrics;
  // The wrap limit of the message; 0 = no wrap.
  float limit = 0;
  std::vector<TextPage> pages;
  TextPage cur;
  int line_start = 0;
  // Per glyph of the page: its pen advance, the width it counts for in word wrap, and whether it sits on a choice line.
  std::vector<float> advances;
  std::vector<float> wrap_widths;
  std::vector<char> on_choice_line;
  // Glyph count of the page at each newline token.
  std::vector<int> newlines;
  u8 color = 0;
  bool italic = false;
  // choice_line: this line starts with the three-space choice indent (choice mode only);
  // in_indent: still inside that line's leading space run.
  bool choice_line = false;
  bool in_indent = false;
  // {input(n)}: the next digits_left glyphs are number-entry digits, the next one being digit_next.
  int digits_left = 0;
  int digit_next = 0;

  PageBuilder(const LayoutMetrics& layout_metrics, float wrap_limit) : metrics(layout_metrics), limit(wrap_limit) {}

  float advance(char32_t ch) const { return metrics.advance ? metrics.advance(ch) : 0.0f; }

  void new_line() {
    line_start = static_cast<int>(cur.glyphs.size());
    newlines.push_back(line_start);
    choice_line = in_indent = false;
  }

  // advance_override >= 0 replaces the glyph's own advance (the choice indent, a button glyph).
  void add_glyph(char32_t ch, float advance_override = -1.0f) {
    const float layout_width = advance_override >= 0 || !metrics.glyph_width ? -1.0f : metrics.glyph_width(ch, italic);
    const float glyph_advance = advance_override >= 0 ? advance_override : metrics.pen_grid > 0 && layout_width >= 0 ? layout_width : advance(ch);
    LayoutGlyph glyph;
    glyph.ch = ch;
    glyph.color = color;
    glyph.italic = italic;
    if (digits_left > 0) {
      glyph.digit = digit_next++;
      --digits_left;
    }
    cur.glyphs.push_back(glyph);
    advances.push_back(glyph_advance);
    wrap_widths.push_back(layout_width >= 0 ? layout_width : glyph_advance);
    on_choice_line.push_back(choice_line);
  }

  // The page as a reading-order walk of glyphs and newlines. A space remembers where the line may break, and the walk checks the width it
  // has reached before each entry: past the limit it turns the remembered space into a newline and walks on from there. So the last glyph
  // of a page may overhang, a space on the page's first entry never breaks, a line without a space since the last break stays whole, and a
  // space remembered before a newline still breaks when the next line overflows without a space of its own. Choice lines never break; with
  // no limit nothing breaks. The kept glyphs are then placed line by line at their advances.
  void place_page() {
    struct Entry {
      int glyph;
      bool choice;
      bool newline;
    };
    std::vector<Entry> entries;
    entries.reserve(cur.glyphs.size() + newlines.size());
    const int glyph_count = static_cast<int>(cur.glyphs.size());
    size_t next_newline = 0;
    for (int i = 0; i <= glyph_count; ++i) {
      const bool choice = i > 0 && on_choice_line[static_cast<size_t>(i - 1)];
      for (; next_newline < newlines.size() && newlines[next_newline] == i; ++next_newline) entries.push_back({-1, choice, true});
      if (i < glyph_count) entries.push_back({i, on_choice_line[static_cast<size_t>(i)] != 0, false});
    }
    float width = 0;
    int candidate = -1;
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
      const Entry& entry = entries[static_cast<size_t>(i)];
      if (limit <= 0 || candidate < 0 || width <= limit || entry.choice) {
        if (i > 0 && !entry.newline && cur.glyphs[static_cast<size_t>(entry.glyph)].ch == U' ') candidate = i;
      } else {
        entries[static_cast<size_t>(candidate)].newline = true;
        i = candidate;
        candidate = -1;
        width = 0;
        continue;
      }
      width = entry.newline ? 0.0f : width + wrap_widths[static_cast<size_t>(entry.glyph)];
    }

    // The spaces that became newlines leave the page; the events follow their glyphs.
    std::vector<LayoutGlyph> kept;
    kept.reserve(cur.glyphs.size());
    std::vector<u32> new_index(cur.glyphs.size() + 1, 0);
    Pen pen{metrics.pen_grid};
    float pen_y = 0;
    cur.width = 0;
    for (const Entry& entry : entries) {
      if (entry.glyph >= 0) new_index[static_cast<size_t>(entry.glyph)] = static_cast<u32>(kept.size());
      if (entry.newline) {
        pen = Pen{metrics.pen_grid};
        pen_y += metrics.line_height;
        continue;
      }
      LayoutGlyph glyph = cur.glyphs[static_cast<size_t>(entry.glyph)];
      glyph.x = pen.x();
      glyph.y = pen_y;
      kept.push_back(glyph);
      pen.advance(advances[static_cast<size_t>(entry.glyph)]);
      cur.width = std::max(cur.width, pen.width);
    }
    new_index[cur.glyphs.size()] = static_cast<u32>(kept.size());
    for (PageEvent& page_event : cur.events) page_event.at = new_index[page_event.at];
    cur.glyphs = std::move(kept);
  }

  void event(PageEvent::Kind kind, int arg) {
    cur.events.push_back({kind, static_cast<u32>(cur.glyphs.size()), arg});
  }

  void finish_page(bool ends_message) {
    cur.ends_message = ends_message;
    place_page();
    int lines = 0;
    for (const LayoutGlyph& glyph : cur.glyphs)
      if (metrics.line_height > 0) lines = std::max(lines, static_cast<int>(glyph.y / metrics.line_height + 0.5f) + 1);
    cur.lines = lines;
    pages.push_back(std::move(cur));
    cur = TextPage();
    line_start = 0;
    advances.clear();
    wrap_widths.clear();
    on_choice_line.clear();
    newlines.clear();
    choice_line = in_indent = false;
  }
};

// The widest run of glyphs between spaces and newlines over the whole message; a page turn does not end a run.
float widest_word(const import::Message& flat, const LayoutMetrics& metrics) {
  float word = 0, widest = 0;
  bool italic = false;
  const auto add = [&](char32_t ch) {
    word += metrics.glyph_width ? metrics.glyph_width(ch, italic) : metrics.advance ? metrics.advance(ch) : 0.0f;
    widest = std::max(widest, word);
  };
  for (const import::Token& token : flat.tokens) {
    if (token.kind == TokKind::End) break;
    if (token.kind == TokKind::Newline) word = 0;
    if (token.kind == TokKind::Italic) italic = token.arg != 0;
    if (token.kind == TokKind::Button) add(button_placeholder(metrics.wrap));
    if (token.kind != TokKind::Text) continue;
    for (const char32_t ch : token.text) {
      if (ch == U' ') word = 0;
      else add(ch);
    }
  }
  return widest;
}

}  // namespace

float wrap_glyph_width(float measure, bool italic) {
  constexpr float kPadding = 10.0f, kPerTexel = 0.45f, kBlank = 40.0f, kItalic = 0.8f;
  const float slant = italic ? kItalic : 1.0f;
  const float width = std::max(0.0f, measure * slant - kPadding);
  return (width > 0 ? width : kBlank * slant) * kPerTexel;
}

float jp_glyph_width(char32_t ch, float measure) {
  constexpr float kPerTexel = 0.45f, kCell = 70.0f, kHalfCell = 40.0f, kCellPerTexel = 0.35f;
  const bool latin = (ch >= U'a' && ch <= U'z') || (ch >= U'A' && ch <= U'Z') || ch == U'-' || ch == U'[' || ch == U']';
  const bool half_digit = (ch >= U'0' && ch <= U'9') || ch == 0xB2 || ch == 0xB3 || ch == 0xB9;
  return latin ? measure * kPerTexel : 2.0f * (half_digit ? kHalfCell : kCell) * kCellPerTexel;
}

int disc_line_chars(const import::Message& ps1) {
  constexpr int kNameChars = 6;
  int widest = 0, line = 0;
  for (const import::Token& token : ps1.tokens) {
    switch (token.kind) {
      case TokKind::Text: line += static_cast<int>(token.text.size()); break;
      case TokKind::Item:
      case TokKind::Name:
      case TokKind::PlayWork:
      case TokKind::PartyName: line += kNameChars; break;
      case TokKind::Newline:
      case TokKind::Page:
      case TokKind::Wait:
      case TokKind::AutoClose:
      case TokKind::End:
        widest = std::max(widest, line);
        line = 0;
        break;
      default: break;
    }
  }
  return std::max(widest, line);
}

float wrap_limit(int room, int message, import::Lang lang, int disc_chars) {
  constexpr float kPerChar = 62.0f, kLongestLine = 925.0f, kNoDiscLine = 1000.0f;
  constexpr int kWideLine = 30;
  struct RoomMessage {
    int room, message;
  };
  // The translations that run longer than their disc lines and are shown as authored.
  static constexpr int kMermaidRoom = 387;
  static constexpr std::array<std::array<int, 9>, 5> kMermaidMessages{{{6, 10, 11, 13, 14, 16},
                                                                        {8, 12, 13, 16, 17, 26, 28, 32, 39},
                                                                        {16},
                                                                        {6, 10, 11, 13},
                                                                        {6, 10, 11, 16, 17, 27, 34, 40, 48}}};
  static constexpr std::array<RoomMessage, 4> kUnwrappedEnglish{{{414, 19}, {414, 21}, {414, 23}, {442, 33}}};
  // The cheers of the ending, measured as if their disc lines were long.
  static constexpr std::array<RoomMessage, 4> kWideLines{{{1490, 1}, {1490, 3}, {1492, 0}, {1492, 1}}};

  const auto listed = [&](const auto& list) {
    return std::any_of(list.begin(), list.end(), [&](const RoomMessage& entry) { return entry.room == room && entry.message == message; });
  };
  if (lang == import::Lang::Jp) return 0.0f;
  if (room == kMermaidRoom) {
    const auto& ids = kMermaidMessages[static_cast<size_t>(lang)];
    if (std::find(ids.begin(), ids.end(), message) != ids.end()) return 0.0f;
  }
  if (lang == import::Lang::En && listed(kUnwrappedEnglish)) return 0.0f;
  const int chars = listed(kWideLines) ? kWideLine : disc_chars;
  return chars > 0 ? std::min(static_cast<float>(chars) * kPerChar, kLongestLine) : kNoDiscLine;
}

std::vector<TextPage> layout_pages(const import::Message& flat, const LayoutMetrics& metrics) {
  const bool wraps = metrics.wrap && metrics.max_width > 0;
  PageBuilder builder(metrics, wraps ? std::max(metrics.max_width, widest_word(flat, metrics)) : 0.0f);
  bool ended = false;
  for (const import::Token& token : flat.tokens) {
    if (ended) break;
    switch (token.kind) {
      case TokKind::Text:
        for (size_t i = 0; i < token.text.size(); ++i) {
          const char32_t ch = token.text[i];
          const auto space_at = [&](size_t j) { return j < token.text.size() && token.text[j] == U' '; };
          if (metrics.choice_space_first > 0) {
            if (static_cast<int>(builder.cur.glyphs.size()) == builder.line_start && space_at(i) && space_at(i + 1) && space_at(i + 2))
              builder.choice_line = builder.in_indent = true;
            if (builder.in_indent && ch == U' ') {
              builder.add_glyph(ch, space_at(i + 1) && space_at(i + 2) ? metrics.choice_space_first : metrics.choice_space);
              continue;
            }
            builder.in_indent = false;
          }
          builder.add_glyph(ch);
        }
        break;
      case TokKind::Newline: builder.new_line(); break;
      case TokKind::Page: builder.finish_page(false); break;
      case TokKind::Wait: builder.event(PageEvent::Kind::Wait, 0); break;
      case TokKind::Delay: builder.event(PageEvent::Kind::Delay, token.arg); break;
      case TokKind::Speed: builder.event(PageEvent::Kind::Speed, token.arg); break;
      case TokKind::AutoClose: builder.event(PageEvent::Kind::AutoClose, token.arg); break;
      case TokKind::End: ended = true; break;
      case TokKind::Color: builder.color = static_cast<u8>(token.arg); break;
      case TokKind::Italic: builder.italic = token.arg != 0; break;
      case TokKind::Button: {
        const size_t at = static_cast<size_t>(&token - flat.tokens.data());
        const import::Token* next = at + 1 < flat.tokens.size() ? &flat.tokens[at + 1] : nullptr;
        const bool spaced = next && next->kind == TokKind::Text && !next->text.empty() && (next->text[0] == U' ' || next->text[0] == U' ');
        const float advance = spaced ? metrics.button_advance_spaced : metrics.button_advance;
        builder.add_glyph(button_placeholder(metrics.wrap), advance > 0 ? advance : -1.0f);
        builder.cur.glyphs.back().button = token.arg;
        break;
      }
      case TokKind::FWait: builder.event(PageEvent::Kind::FWait, token.arg); break;
      case TokKind::FSet: builder.event(PageEvent::Kind::FSet, token.arg); break;
      case TokKind::Input:
        builder.digits_left = std::clamp(token.arg, 0, 8);
        builder.digit_next = 0;
        break;
      default: break;
    }
  }
  builder.finish_page(true);
  // An empty page that only follows a {bpage} adds nothing to show.
  if (builder.pages.size() > 1 && builder.pages.back().glyphs.empty() && builder.pages.back().events.empty()) {
    builder.pages.pop_back();
    builder.pages.back().ends_message = true;
  }
  return std::move(builder.pages);
}

// ---------------------------------------------------------------------------------------------- WindowOpening
void WindowOpening::start(bool grow) {
  grow_left_s_ = grow ? static_cast<float>(kWindowGrowTicks - 1) / kSpeedTickHz : 0.0f;
  since_text_s_ = 0;
}

float WindowOpening::update(float dt) {
  if (growing()) {
    grow_left_s_ -= dt;
    if (growing()) return 0.0f;
    dt = std::max(0.0f, -grow_left_s_);
    grow_left_s_ = 0;
  }
  since_text_s_ += dt;
  return dt;
}

float WindowOpening::alpha() const {
  const float tick = 1.0f / kSpeedTickHz;
  return std::clamp((since_text_s_ + kTickEpsilon - tick) / (static_cast<float>(kWindowFadeTicks) * tick), 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------------------------- Typewriter
void Typewriter::start(std::vector<TextPage> pages) {
  pages_ = std::move(pages);
  page_ = 0;
  speed_ = kDefaultSpeed;
  auto_close_ = -1;
  tick_acc_ = 0;
  state_ = pages_.empty() ? State::Closed : State::Revealing;
  begin_page();
}

void Typewriter::replace_pages(std::vector<TextPage> pages) {
  if (pages.empty()) return;
  pages_ = std::move(pages);
  if (page_ >= pages_.size()) page_ = pages_.size() - 1;
  const TextPage& page = pages_[page_];
  visible_ = std::min<u32>(visible_, static_cast<u32>(page.glyphs.size()));
  ev_ = std::min(ev_, page.events.size());
}

void Typewriter::begin_page() {
  visible_ = 0;
  ev_ = 0;
  acc_ = 0;
  delay_s_ = 0;
  fast_ = false;
}

void Typewriter::update(float dt, TextPace pace) {
  pace_ = pace;
  tick_acc_ += dt;
  const float tick = 1.0f / kSpeedTickHz;
  int guard = 0;
  while (tick_acc_ >= tick && guard++ < 600) {
    tick_acc_ -= tick;
    step();
  }
  if (guard >= 600) tick_acc_ = 0;
}

// One 1/60 s tick of the reveal: a running delay counts down first; then every event due before the next glyph runs (a delay or
// wait stops the tick, an unsatisfied {fwait} is retried next tick); then glyphs are revealed - all of them at once at speed 255 or
// after a fast-forward confirm, else speed / 64 per tick, stopping early when the next event comes due. A fully revealed page waits
// for the page turn, starts its {wend} countdown or waits to close.
void Typewriter::step() {
  const float tick = 1.0f / kSpeedTickHz;
  if (state_ == State::Closing) {
    close_s_ -= tick;
    if (close_s_ <= 0) state_ = State::Closed;
    return;
  }
  if (state_ != State::Revealing) return;
  const TextPage& page = pages_[page_];
  if (delay_s_ > 0 && !fast_) {
    delay_s_ -= tick;
    return;
  }
  delay_s_ = 0;
  for (;;) {
    while (ev_ < page.events.size() && page.events[ev_].at <= visible_) {
      const PageEvent& event = page.events[ev_++];
      switch (event.kind) {
        case PageEvent::Kind::Delay:
          if (!fast_ && event.arg > 0) {
            delay_s_ = static_cast<float>(event.arg) / kScriptFrameHz * pace_.wait;
            return;
          }
          break;
        case PageEvent::Kind::Speed: speed_ = event.arg; break;
        case PageEvent::Kind::Wait:
          state_ = State::WaitConfirm;
          fast_ = false;
          return;
        case PageEvent::Kind::FWait:
          if (event_get && event_get() != event.arg) {
            --ev_;
            return;
          }
          break;
        case PageEvent::Kind::AutoClose: auto_close_ = event.arg; break;
        case PageEvent::Kind::FSet:
          if (event_set) event_set(event.arg);
          break;
      }
    }
    if (visible_ >= page.glyphs.size()) {
      fast_ = false;
      if (!page.ends_message) {
        state_ = State::WaitPage;
      } else if (auto_close_ >= 0) {
        state_ = State::Closing;
        close_s_ = static_cast<float>(auto_close_) / kScriptFrameHz * pace_.wait;
      } else {
        state_ = State::WaitClose;
      }
      return;
    }
    if (speed_ >= 255 || fast_) {
      ++visible_;
      continue;
    }
    acc_ += static_cast<float>(speed_) / 64.0f * pace_.reveal;
    while (acc_ >= 1.0f && visible_ < page.glyphs.size()) {
      ++visible_;
      acc_ -= 1.0f;
      if (ev_ < page.events.size() && page.events[ev_].at <= visible_) break;
    }
    return;
  }
}

void Typewriter::reveal_now() {
  if (state_ != State::Revealing) return;
  fast_ = true;
  step();
}

void Typewriter::confirm() {
  switch (state_) {
    case State::Revealing: fast_ = true; break;
    case State::WaitConfirm:
      state_ = State::Revealing;
      break;
    case State::WaitPage:
      if (page_ + 1 < pages_.size()) {
        ++page_;
        begin_page();
        state_ = State::Revealing;
      } else {
        state_ = State::Closed;
      }
      break;
    case State::WaitClose:
    case State::Closing: state_ = State::Closed; break;
    default: break;
  }
}

}  // namespace oc::ui
