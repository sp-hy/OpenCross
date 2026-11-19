#include "ui/dialogue_log_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "core/log.h"
#include "render/camera.h"
#include "ui/ps1_font.h"
#include "ui/typewriter.h"

namespace oc::ui {

using render::pack_rgba;

namespace {

// The window's own words, in the language order en fr it de es jp.
constexpr const char* kTitle[6] = {"Dialogue log", "Journal des dialogues", "Registro dei dialoghi", "Dialogprotokoll", "Registro de diálogos",
                                   "会話ログ"};
constexpr const char* kLoaded[6] = {"Loaded: ", "Chargé : ", "Caricato: ", "Geladen: ", "Cargado: ", "ロード："};
constexpr const char* kEmpty[6] = {"No messages yet.", "Aucun message pour l'instant.", "Ancora nessun messaggio.", "Noch keine Nachrichten.",
                                   "Aún no hay mensajes.", "まだ会話はありません。"};
constexpr const char* kReplay[6] = {"Confirm: hear the voice again", "Valider : réécouter la voix", "Conferma: riascolta la voce",
                                    "Bestätigen: Stimme erneut hören", "Confirmar: volver a oír la voz", "決定：ボイスをもう一度聞く"};
constexpr const char* kClear[6] = {"Clear log", "Effacer le journal", "Cancella registro", "Protokoll löschen", "Borrar registro", "ログをけす"};
constexpr const char* kClearAsk[6] = {"Clear the whole log?", "Effacer tout le journal ?", "Cancellare tutto il registro?", "Ganzes Protokoll löschen?",
                                      "¿Borrar todo el registro?", "ログをすべてけしますか？"};
constexpr const char* kYes[6] = {"Yes", "Oui", "Sì", "Ja", "Sí", "はい"};
constexpr const char* kNo[6] = {"No", "Non", "No", "Nein", "No", "いいえ"};
// The {key(n)} button the Clear log prompt names: the key-item function.
constexpr int kClearKey = 2;

// The canvas box the skin's fill covers (the frame goes around it). Everything else sits inside the frame's inner edge
// (DialogueBox::frame_clear): kEdgeGap from it, the list kListBelowTitle under the title row, the selected row's band kInset inside the
// list's sides, a portrait kInset inside the band and its text kFaceGap after it, the note mark kMarkGap after the text column, the scroll
// bar kBarWidth wide at the right.
constexpr float kBoxLeft = 96, kBoxTop = 96, kBoxRight = 1184, kBoxBottom = 880;
constexpr float kEdgeGap = 8, kListBelowTitle = 68, kInset = 8, kFaceGap = 28, kMarkGap = 16, kBarWidth = 4;
constexpr float kFaceSize = 150, kItemGap = 22, kTextScale = 0.45f, kTitleScale = 0.5f;
// The title row: the Clear log glyph's size and drop, the gap after it, and the gaps between the row's parts.
constexpr float kKeySize = 40, kKeyDrop = 2, kKeyGap = 8, kPartGap = 40;
// The message window's colours: text, grey (colour 5), the choice highlight (colour 6).
const u32 kWhite = pack_rgba(233, 233, 233), kGrey = pack_rgba(106, 106, 106), kHighlight = pack_rgba(255, 166, 0);
constexpr u8 kHighlightColour = 6;

std::u32string utf32(std::string_view text) { return import::utf8_to_u32(text); }

std::string trimmed(std::string_view text) {
  const auto blank = [](char ch) { return ch == ' ' || ch == '\t'; };
  while (!text.empty() && blank(text.front())) text.remove_prefix(1);
  while (!text.empty() && blank(text.back())) text.remove_suffix(1);
  return std::string(text);
}

}  // namespace

struct DialogueLogView::Impl {
  render::GpuContext* ctx = nullptr;
  const vfs::Vfs* vfs = nullptr;
  render::SpriteBatch* sprites = nullptr;
  render::TextBatch text;
  font::FontSet fonts;
  DialogueBox box;
  bool ready = false, shown = false;
  const save::DialogueLog* log = nullptr;
  ArtMode mode = ArtMode::Hd;
  int skin_byte = 0;
  std::string portrait_set = "2021";
  import::Lang lang = import::Lang::En;

  // One list row: a message (its laid-out glyphs, portrait and voice mark) or a divider (its label), at list y `y`, `h` high.
  struct Item {
    size_t entry = 0;
    float y = 0, h = 0;
    bool divider = false;
    int face = -1;
    bool voice = false;
    std::vector<LayoutGlyph> glyphs;
    std::u32string label;
  };
  std::vector<Item> items;
  float total_h = 0, line_h = 0;
  // Where the parts sit (canvas units): the title row's y and its clear sides, the list's rows and clear sides, the selected row's band,
  // the portrait, text and note mark columns and the scroll bar's x.
  struct Place {
    float title_y = 0, title_left = 0, title_right = 0;
    float list_top = 0, list_bottom = 0, clear_left = 0, clear_right = 0;
    float band_left = 0, band_right = 0, face_x = 0, text_x = 0, text_right = 0, mark_x = 0, bar_x = 0;
    float list_h() const { return list_bottom - list_top; }
  } at;
  // What the layout was built from.
  bool built = false;
  u32 built_generation = 0;
  const save::DialogueLog* built_log = nullptr;
  ArtMode built_mode = ArtMode::Hd;
  import::Lang built_lang = import::Lang::En;
  float built_width = 0;
  float scroll = 0;
  int cursor = 0;
  std::vector<std::string> replay;
  // The Clear log question is up, its cursor is on Yes; it was answered Yes (take_clear).
  bool asking = false, ask_yes = false, clear_wanted = false;

  int lang_index() const { return std::clamp(static_cast<int>(lang), 0, 5); }
  bool jp() const { return lang == import::Lang::Jp; }
  render::FaceId face() {
    render::FaceId id = fonts.face(jp() ? font::kKtMsg : font::kGn, mode == ArtMode::Original);
    if (id == render::kNoFace) id = fonts.face(font::kGn, false);
    return id;
  }
  LayoutMetrics metrics(render::FaceId id);
  void place(render::FaceId id);
  void ensure_layout();
  void reveal_cursor();
  float max_scroll() const { return std::max(0.0f, total_h - at.list_h()); }
};

// The parts inside the frame's inner edge: the title row under the top side and between the sides as they run along its rows, the list
// from under it to above the bottom side, its columns between the sides as they run along the list's rows.
void DialogueLogView::Impl::place(render::FaceId id) {
  const DialogueBox::FrameClear& clear = box.frame_clear(kBoxLeft, kBoxTop, kBoxRight, kBoxBottom);
  Place out;
  out.title_y = clear.top() + kEdgeGap;
  const float title_bottom = out.title_y + text.line_height(id, kTitleScale);
  out.title_left = clear.left(out.title_y, title_bottom) + kEdgeGap + kInset;
  out.title_right = clear.right(out.title_y, title_bottom) - kEdgeGap - kInset;
  out.list_top = out.title_y + kListBelowTitle;
  out.list_bottom = clear.bottom() - kEdgeGap;
  out.clear_left = clear.left(out.list_top, out.list_bottom);
  out.clear_right = clear.right(out.list_top, out.list_bottom);
  out.band_left = out.clear_left + kEdgeGap;
  out.bar_x = out.clear_right - kEdgeGap - kBarWidth;
  out.band_right = out.bar_x - kInset * 0.5f;
  out.face_x = out.band_left + kInset;
  out.text_x = out.face_x + kFaceSize + kFaceGap;
  text.prepare(id, U"♪");
  out.mark_x = out.band_right - kInset - text.measure(id, U"♪", kTextScale);
  out.text_right = out.mark_x - kMarkGap;
  if (out.title_y != at.title_y || out.clear_left != at.clear_left || out.clear_right != at.clear_right || out.list_bottom != at.list_bottom)
    log::debug("dialogue log: skin {} clear inside x {}..{} y {}..{}, title at y {} x {}..{}, list y {}..{} x {}..{}, text x {}..{}",
               box.skin_index(), clear.left(clear.top(), clear.bottom()), clear.right(clear.top(), clear.bottom()), clear.top(), clear.bottom(),
               out.title_y, out.title_left, out.title_right, out.list_top, out.list_bottom, out.clear_left, out.clear_right, out.text_x,
               out.text_right);
  at = out;
}

// The message window's word wrap (DialogueBox::layout) at the list's text scale, on the list's text column.
LayoutMetrics DialogueLogView::Impl::metrics(render::FaceId id) {
  LayoutMetrics out;
  render::TextBatch* batch = &text;
  const float ratio = kTextScale / 0.5f;
  out.line_height = batch->line_height(id, kTextScale);
  out.max_width = at.text_right - at.text_x;
  if (batch->is_ps1(id)) {
    out.advance = [batch, id](char32_t ch) { return batch->advance(id, ch, kTextScale); };
    out.glyph_width = [batch, id](char32_t ch, bool) { return batch->advance(id, ch, kTextScale); };
    return out;
  }
  out.pen_grid = 2.0f;
  if (jp()) {
    out.glyph_width = [batch, id, ratio](char32_t ch, bool) { return jp_glyph_width(ch, batch->glyph_measure(id, ch)) * ratio; };
  } else {
    out.wrap = true;
    out.glyph_width = [batch, id, ratio](char32_t ch, bool italic) { return wrap_glyph_width(batch->glyph_measure(id, ch), italic) * ratio; };
  }
  return out;
}

// Rebuilt when the log, the art mode, the language or the text column's width (the skin's frame) changed: each entry laid out once, top
// to bottom, then every glyph made drawable.
void DialogueLogView::Impl::ensure_layout() {
  if (!ready) return;
  fonts.set_classic(mode == ArtMode::Original);
  const render::FaceId id = face();
  if (id != render::kNoFace) place(id);
  const u32 generation = log ? log->generation() : 0;
  const float width = at.text_right - at.text_x;
  if (built && built_log == log && built_generation == generation && built_mode == mode && built_lang == lang && built_width == width) return;
  built = true;
  built_log = log, built_generation = generation, built_mode = mode, built_lang = lang, built_width = width;
  items.clear();
  total_h = 0;
  if (id == render::kNoFace) return;
  const int words = lang_index();
  std::u32string all = utf32(kTitle[words]) + utf32(kEmpty[words]) + utf32(kReplay[words]) + utf32(kClear[words]) + utf32(kClearAsk[words]) +
                       utf32(kYes[words]) + utf32(kNo[words]) + U"0123456789/:, ♪";
  const LayoutMetrics lm = metrics(id);
  line_h = lm.line_height;
  const std::vector<save::DialogueEntry> none;
  const std::vector<save::DialogueEntry>& entries = log ? log->entries() : none;
  for (size_t i = 0; i < entries.size(); ++i) {
    const save::DialogueEntry& entry = entries[i];
    Item item;
    item.entry = i;
    item.y = total_h;
    if (entry.kind == save::DialogueEntry::Kind::Divider) {
      char time[16];
      std::snprintf(time, sizeof time, "%u:%02u", entry.play_seconds / 3600, entry.play_seconds / 60 % 60);
      item.divider = true;
      item.label = utf32(std::string(kLoaded[lang_index()]) + entry.area + (entry.area.empty() ? "" : ", ") + time);
      item.h = line_h + kItemGap;
      all += item.label;
    } else {
      // The text one line per message line; the line that is the option picked in the highlight colour (or the option added under it).
      import::Message flat;
      bool marked = entry.choice.empty();
      const std::string choice = trimmed(entry.choice);
      for (size_t from = 0;;) {
        const size_t end = entry.text.find('\n', from);
        const std::string_view line = std::string_view(entry.text).substr(from, end == std::string::npos ? std::string::npos : end - from);
        const bool picked = !marked && trimmed(line) == choice;
        if (picked) flat.tokens.push_back({import::TokKind::Color, kHighlightColour});
        flat.tokens.push_back({import::TokKind::Text, 0, 0, utf32(line)});
        if (picked) {
          flat.tokens.push_back({import::TokKind::Color, 0});
          marked = true;
        }
        if (end == std::string::npos) break;
        flat.tokens.push_back({import::TokKind::Newline});
        from = end + 1;
      }
      if (!marked) {
        flat.tokens.push_back({import::TokKind::Newline});
        flat.tokens.push_back({import::TokKind::Color, kHighlightColour});
        flat.tokens.push_back({import::TokKind::Text, 0, 0, utf32("   " + choice)});
      }
      for (const import::Token& token : flat.tokens) all += token.text;
      std::vector<TextPage> pages = layout_pages(flat, lm);
      int lines = 0;
      for (TextPage& page : pages) {
        for (LayoutGlyph& glyph : page.glyphs) glyph.y += static_cast<float>(lines) * line_h;
        item.glyphs.insert(item.glyphs.end(), page.glyphs.begin(), page.glyphs.end());
        lines += std::max(page.lines, 1);
      }
      item.face = entry.face;
      item.voice = !entry.voices.empty();
      item.h = std::max(static_cast<float>(lines) * line_h, item.face >= 0 ? kFaceSize : 0.0f) + kItemGap;
    }
    total_h += item.h;
    items.push_back(std::move(item));
  }
  text.prepare(id, all);
  cursor = std::clamp(cursor, 0, std::max(0, static_cast<int>(items.size()) - 1));
  scroll = std::clamp(scroll, 0.0f, max_scroll());
}

void DialogueLogView::Impl::reveal_cursor() {
  if (items.empty()) {
    scroll = 0;
    return;
  }
  const Item& item = items[static_cast<size_t>(cursor)];
  if (item.y < scroll) scroll = item.y;
  if (item.y + item.h > scroll + at.list_h()) scroll = std::min(item.y, item.y + item.h - at.list_h());
  scroll = std::clamp(scroll, 0.0f, max_scroll());
}

DialogueLogView::DialogueLogView() : impl_(std::make_unique<Impl>()) {}
DialogueLogView::~DialogueLogView() { shutdown(); }

bool DialogueLogView::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites) {
  Impl& impl = *impl_;
  impl.ctx = &ctx;
  impl.vfs = &vfs;
  impl.sprites = &sprites;
  if (!impl.text.init(ctx, sprites) || !impl.box.init(ctx, vfs, sprites, impl.text)) return false;
  impl.fonts.init(vfs, impl.text);
  impl.ready = true;
  return true;
}

void DialogueLogView::shutdown() {
  Impl& impl = *impl_;
  if (!impl.ctx) return;
  impl.box.destroy();
  impl.fonts.reset();
  impl.text.destroy();
  impl.ready = false;
  impl.ctx = nullptr;
}

void DialogueLogView::set_log(const save::DialogueLog* log) { impl_->log = log; }

void DialogueLogView::set_style(ArtMode mode, int skin_byte, const std::string& portrait_set, import::Lang lang) {
  Impl& impl = *impl_;
  if (!impl.ready) return;
  if (mode != impl.box.mode()) impl.box.set_mode(mode);
  if (skin_byte != impl.skin_byte || mode != impl.mode) impl.box.set_skin_byte(skin_byte);
  if (portrait_set != impl.portrait_set) impl.box.set_portrait_set(portrait_set);
  impl.mode = mode;
  impl.skin_byte = skin_byte;
  impl.portrait_set = portrait_set;
  impl.lang = lang;
}

void DialogueLogView::set_key_caps(bool keyboard) { impl_->box.set_button_style(keyboard, impl_->lang_index()); }

void DialogueLogView::open() {
  Impl& impl = *impl_;
  impl.shown = true;
  impl.asking = false;
  impl.replay.clear();
  impl.ensure_layout();
  impl.cursor = std::max(0, static_cast<int>(impl.items.size()) - 1);
  impl.scroll = impl.max_scroll();
}

void DialogueLogView::close() {
  impl_->shown = false;
  impl_->asking = false;
}
bool DialogueLogView::active() const { return impl_->shown; }
bool DialogueLogView::asking() const { return impl_->asking; }

int DialogueLogView::input(const Nav& nav) {
  Impl& impl = *impl_;
  impl.ensure_layout();
  if (impl.asking) {
    if (nav.cancel) {
      impl.asking = false;
      return 6;
    }
    if (nav.replay) {
      impl.asking = false;
      impl.clear_wanted = impl.ask_yes;
      return impl.ask_yes ? 8 : 1;
    }
    if (nav.move == 0 && nav.page == 0) return 0;
    impl.ask_yes = !impl.ask_yes;
    return 1;
  }
  const int count = static_cast<int>(impl.items.size());
  if (count == 0) return 0;
  if (nav.clear) {
    impl.asking = true;
    impl.ask_yes = false;
    return 8;
  }
  const int before = impl.cursor;
  int cursor = std::clamp(impl.cursor + nav.move, 0, count - 1);
  // A page: as many entries as fill the list's height, at least one.
  for (int step = 0; step < std::abs(nav.page); ++step) {
    const int dir = nav.page < 0 ? -1 : 1;
    float moved = 0;
    do {
      moved += impl.items[static_cast<size_t>(cursor)].h;
      cursor += dir;
    } while (cursor > 0 && cursor < count - 1 && moved + impl.items[static_cast<size_t>(cursor)].h <= impl.at.list_h());
    cursor = std::clamp(cursor, 0, count - 1);
  }
  if (nav.first) cursor = 0;
  if (nav.last) cursor = count - 1;
  impl.cursor = cursor;
  impl.reveal_cursor();
  if (nav.replay && impl.log) {
    const Impl::Item& item = impl.items[static_cast<size_t>(impl.cursor)];
    if (item.voice && item.entry < impl.log->entries().size()) impl.replay = impl.log->entries()[item.entry].voices;
  }
  return impl.cursor != before ? 1 : 0;
}

std::vector<std::string> DialogueLogView::take_replay() { return std::exchange(impl_->replay, {}); }
bool DialogueLogView::take_clear() { return std::exchange(impl_->clear_wanted, false); }

size_t DialogueLogView::entry_count() const { return impl_->items.size(); }
int DialogueLogView::cursor() const { return impl_->cursor; }

// The dimmed picture, the skin's frame round the list, the title row (title, position, the replay hint and the Clear log prompt with its
// button glyph; while the question is up the question and its Yes / No, the chosen one in the highlight colour), then the visible rows
// clipped to the list: the selected row's band, portraits, text and voice marks, a divider's label between two rules; the scroll bar at
// the right.
void DialogueLogView::draw(const render::Frame& frame, float vw, float vh, bool resume) {
  Impl& impl = *impl_;
  if (!impl.ready || !impl.shown || !impl.sprites) return;
  impl.ensure_layout();
  const render::FaceId id = impl.face();
  if (id == render::kNoFace) return;
  const int lang = impl.lang_index();
  const std::u32string title = utf32(kTitle[lang]);
  const std::u32string position =
      impl.items.empty() ? std::u32string() : utf32(std::to_string(impl.cursor + 1) + "/" + std::to_string(impl.items.size()));
  const bool can_replay = !impl.items.empty() && impl.items[static_cast<size_t>(impl.cursor)].voice;
  // Visible rows; their portraits are loaded before the pass.
  const Impl::Place& at = impl.at;
  const float list_h = at.list_h();
  size_t first = 0, last = 0;
  while (first < impl.items.size() && impl.items[first].y + impl.items[first].h <= impl.scroll) ++first;
  last = first;
  while (last < impl.items.size() && impl.items[last].y < impl.scroll + list_h) ++last;
  for (size_t i = first; i < last; ++i)
    if (impl.items[i].face >= 0) impl.box.set_portrait(impl.items[i].face);

  const render::OverlayFit of = render::overlay_fit(vw, vh);
  const float zoom = of.sx, flat = of.sy / of.sx, yo = of.oy / flat;
  const float left = (vw / zoom - 1280.0f) * 0.5f;
  render::SpriteBatch& sprites = *impl.sprites;
  const render::TexRef white = frame.ctx->white();
  const auto rect = [&](float x, float y, float w, float h, u32 colour) {
    sprites.draw(white, (left + x) * zoom, y * zoom + yo, w * zoom, h * zoom, 0, 0, 1, 1, colour);
  };
  const auto line = [&](const std::u32string& str, float x, float y, u32 colour, float scale) {
    impl.text.draw(id, str, (left + x) * zoom, y * zoom + yo, colour, scale * zoom);
  };
  const auto flatten = [&] {
    sprites.set_view(vw, vh / flat);
    impl.text.set_pixel_grid(1.0f, flat);
  };

  sprites.begin(frame, vw, vh, resume);
  sprites.draw(white, 0, 0, vw, vh, 0, 0, 1, 1, pack_rgba(0, 0, 0, 190));
  impl.box.draw_frame(kBoxLeft, kBoxTop, kBoxRight, kBoxBottom, 1.0f, vw, vh);
  flatten();
  line(title, at.title_left, at.title_y, kWhite, kTitleScale);
  const float title_end = at.title_left + impl.text.measure(id, title, kTitleScale);
  const float position_x = at.title_right - impl.text.measure(id, position, kTitleScale);
  if (impl.asking) {
    float x = title_end + kPartGap;
    const char* const parts[3] = {kClearAsk[lang], kYes[lang], kNo[lang]};
    const u32 colours[3] = {kWhite, impl.ask_yes ? kHighlight : kGrey, impl.ask_yes ? kGrey : kHighlight};
    for (int i = 0; i < 3; ++i) {
      const std::u32string shown = utf32(parts[i]);
      line(shown, x, at.title_y + 6, colours[i], kTextScale);
      x += impl.text.measure(id, shown, kTextScale) + kPartGap;
    }
  } else {
    if (!position.empty()) line(position, position_x, at.title_y, kGrey, kTitleScale);
    float hints_end = position_x;
    if (!impl.items.empty()) {
      const std::u32string clear = utf32(kClear[lang]);
      const float clear_x = position_x - kPartGap - impl.text.measure(id, clear, kTextScale);
      hints_end = clear_x - kKeyGap - kKeySize;
      impl.box.draw_key(kClearKey, hints_end, at.title_y + kKeyDrop, kKeySize, vw, vh);
      flatten();
      line(clear, clear_x, at.title_y + 6, kGrey, kTextScale);
    }
    if (can_replay) {
      const std::u32string hint = utf32(kReplay[lang]);
      line(hint, (title_end + hints_end - impl.text.measure(id, hint, kTextScale)) * 0.5f, at.title_y + 6, kGrey, kTextScale);
    }
  }
  if (impl.items.empty()) line(utf32(kEmpty[lang]), at.text_x, at.list_top, kGrey, kTextScale);
  if (impl.total_h > list_h) {
    const float bar_h = std::max(24.0f, list_h * list_h / impl.total_h);
    const float bar_y = at.list_top + (list_h - bar_h) * impl.scroll / impl.max_scroll();
    rect(at.bar_x, at.list_top, kBarWidth, list_h, pack_rgba(255, 255, 255, 40));
    rect(at.bar_x, bar_y, kBarWidth, bar_h, pack_rgba(233, 233, 233, 200));
  }
  sprites.end();

  const float px_left = std::max(0.0f, (left + at.clear_left) * zoom), px_top = std::max(0.0f, at.list_top * of.sy + of.oy);
  frame.set_scissor({static_cast<s32>(px_left), static_cast<s32>(px_top), static_cast<u32>(std::max(0.0f, (at.clear_right - at.clear_left) * zoom)),
                     static_cast<u32>(std::max(0.0f, list_h * of.sy))});
  sprites.begin(frame, vw, vh, true);
  flatten();
  for (size_t i = first; i < last; ++i) {
    const Impl::Item& item = impl.items[i];
    const float y = at.list_top + item.y - impl.scroll;
    if (static_cast<int>(i) == impl.cursor) rect(at.band_left, y - kItemGap * 0.5f, at.band_right - at.band_left, item.h, pack_rgba(255, 255, 255, 34));
    if (item.divider) {
      const float width = impl.text.measure(id, item.label, kTextScale);
      const float x = (at.band_left + at.band_right - width) * 0.5f, mid = y + impl.line_h * 0.5f;
      rect(at.face_x, mid, std::max(0.0f, x - 24 - at.face_x), 2, kGrey);
      rect(x + width + 24, mid, std::max(0.0f, at.band_right - kInset - x - width - 24), 2, kGrey);
      line(item.label, x, y, kGrey, kTextScale);
      continue;
    }
    if (item.face >= 0) {
      impl.box.draw_portrait(item.face, at.face_x, y, kFaceSize, 1.0f, vw, vh);
      flatten();
    }
    for (render::TextBatch::Pass pass : {render::TextBatch::Pass::Backdrop, render::TextBatch::Pass::Fill})
      for (const LayoutGlyph& glyph : item.glyphs)
        impl.text.draw_glyph(id, glyph.ch, (left + at.text_x + glyph.x) * zoom, (y + glyph.y) * zoom + yo, glyph.color == kHighlightColour ? kHighlight : kWhite,
                             kTextScale * zoom, pass);
    if (item.voice) line(U"♪", at.mark_x, y, kGrey, kTextScale);
  }
  sprites.end();
  frame.set_scissor(frame.full());
  sprites.set_view(vw, vh);
  impl.text.set_pixel_grid(0.0f, 0.0f);
}

}  // namespace oc::ui
