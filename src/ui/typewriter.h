#pragma once
// Dialogue page layout and the typewriter state machine (no rendering, no GPU types).
#include <functional>
#include <vector>

#include "import/text.h"

namespace oc::ui {

struct LayoutGlyph {
  char32_t ch = 0;
  // Top-left of the pen box, relative to the text origin; color is the {color(n)} index.
  float x = 0, y = 0;
  u8 color = 0;
  bool italic = false;
  // Number entry ({input(n)}): the glyph shows digit `digit` (0 = leftmost) of the entry; -1 = an ordinary glyph.
  int digit = -1;
};

struct PageEvent {
  enum class Kind : u8 { Delay, Speed, Wait, FWait, AutoClose, FSet };
  Kind kind = Kind::Delay;
  // Fires before glyph `at` is revealed (== glyph count: after the last one).
  u32 at = 0;
  int arg = 0;
};

struct TextPage {
  std::vector<LayoutGlyph> glyphs;
  std::vector<PageEvent> events;
  // width: widest line. ends_message is false when a {bpage} follows.
  float width = 0;
  int lines = 0;
  bool ends_message = true;
};

struct LayoutMetrics {
  // advance: pen advance of one glyph. max_width: the wrap limit, raised to the message's widest word; 0 = never wrap. wrap: automatic word
  // wrap (non-JP).
  std::function<float(char32_t)> advance;
  float line_height = 0;
  float max_width = 0;
  bool wrap = false;
  // The width a glyph counts for against max_width, when it differs from the pen advance (wrap_glyph_width); empty = the advance.
  std::function<float(char32_t, bool italic)> wrap_advance;
  // Choice mode (non-JP; the message is laid out with a choice flag once the script marks it as a choice): a line that
  // starts with three spaces is a choice line; its indent run is laid out with special space widths - the first space (it and the
  // next two all spaces) w0 = 70, the others w0 = 20, advance = 2 w0 * 0.45 canvas units at glyph scale 0.5 - and the line is
  // never wrapped. choice_space_first / choice_space are those advances at the caller's scale (0 = rule off).
  float choice_space_first = 0, choice_space = 0;
};

// Splits a flattened message (import::flatten output) into pages: lines, wrapping, events positioned by glyph index.
std::vector<TextPage> layout_pages(const import::Message& flat, const LayoutMetrics& metrics);

// The width a glyph counts for in word wrap at glyph scale 0.5, from its sprite-font measure: 0.45 of the measure less the 10 texels of
// padding, an italic glyph's measure being read at 0.8; a blank (measure 0) counts 18.
float wrap_glyph_width(float measure, bool italic);
// The widest line of a disc message in characters: a typed-name or item token counts 6, and a confirm wait ends a line as a newline does.
int disc_line_chars(const import::Message& ps1);
// The wrap limit of a field message at glyph scale 0.5, from its disc line width: 62 per character up to 925, 1000 without a disc message,
// 0 for JP and for the messages shown as authored.
float wrap_limit(int room, int message, import::Lang lang, int disc_chars);

// Typewriter pacing: `speed` / 64 glyphs per 60 Hz tick (0x80 = 2 per tick, 1 per tick when a message sets no {speed});
// {wait(n)} / {wend(n)} count 30 Hz field frames.
constexpr float kSpeedTickHz = 60.0f;
constexpr float kScriptFrameHz = 30.0f;
constexpr int kDefaultSpeed = 64;

// Opening of a field message window. The window grows from nothing to full size in kWindowGrowTicks 60 Hz ticks (scale k / 16 on
// the k-th tick, counting the tick it opens on as the first); the text, its waits and the number entry start on the tick after it
// is full, and confirm presses before that are dropped. The window and its text are not drawn while it grows; they fade in over
// the kWindowFadeTicks ticks after the text starts. A window opened without the grow is full at once. Closing has no animation.
constexpr int kWindowGrowTicks = 16;
constexpr int kWindowFadeTicks = 3;

class WindowOpening {
 public:
  // The window is opened inside a tick whose own update already ran: its first update() is the next tick.
  void start(bool grow);
  // Advances by dt and returns the part of dt that belongs to the text (0 while the window grows).
  float update(float dt);
  bool growing() const { return grow_left_s_ > kTickEpsilon; }
  // Draw opacity of the window and its text, 0..1.
  float alpha() const;

 private:
  static constexpr float kTickEpsilon = 1e-4f;
  float grow_left_s_ = 0;
  float since_text_s_ = 0;
};

class Typewriter {
 public:
  enum class State : u8 { Idle, Revealing, WaitConfirm, WaitPage, WaitClose, Closing, Closed };

  void start(std::vector<TextPage> pages);
  // Same message laid out again (other font metrics): keeps the page, the revealed count (clamped), events and state.
  void replace_pages(std::vector<TextPage> pages);
  void update(float dt);
  // Confirm key: advances a wait / page / close; while revealing it fast-forwards to the next wait.
  void confirm();

  State state() const { return state_; }
  bool closed() const { return state_ == State::Closed; }
  bool waiting_for_input() const {
    return state_ == State::WaitConfirm || state_ == State::WaitPage || state_ == State::WaitClose;
  }
  size_t page_index() const { return page_; }
  size_t page_count() const { return pages_.size(); }
  u32 visible() const { return visible_; }
  const TextPage* page() const { return page_ < pages_.size() ? &pages_[page_] : nullptr; }

  // Field event register = script variable 0x22: {fset(n)} writes n to it when the reveal reaches it; {fwait(n)} holds the reveal
  // there until it equals n. Scripts poll / set it to sync with the text (Kid collapsing at {fset(3)}, the window closing after the
  // script answers 1). Unset (the demo): both are ignored.
  std::function<int()> event_get;
  std::function<void(int)> event_set;

 private:
  // One 1/60 s tick.
  void step();
  void begin_page();

  std::vector<TextPage> pages_;
  size_t page_ = 0;
  u32 visible_ = 0;
  size_t ev_ = 0;
  State state_ = State::Idle;
  int speed_ = kDefaultSpeed;
  // acc_: glyph accumulator. delay_s_: pause left. tick_acc_: time carried between update() calls.
  // auto_close_: frames until close, -1 = none.
  float acc_ = 0;
  float delay_s_ = 0;
  float tick_acc_ = 0;
  int auto_close_ = -1;
  float close_s_ = 0;
  bool fast_ = false;
};

}  // namespace oc::ui
