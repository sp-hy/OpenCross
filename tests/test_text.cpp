// Checks for the text importer: PS1 control-code tokenisation, markup parsing, spritefont header parsing on a synthetic
// blob, lang.dat lookup on tiny synthetic tables, and the dialogue typewriter.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "data/char_name.h"
#include "import/spritefont.h"
#include "import/text.h"
#include "ui/dialogue_box.h"
#include "ui/typewriter.h"

using namespace oc;
using import::TokKind;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

std::string text_of(const import::Message& message) {
  std::string text;
  for (const auto& token : message.tokens)
    if (token.kind == TokKind::Text) text += import::u32_to_utf8(token.text);
  return text;
}

// PS1 control codes: the glyph table (0x28 = '0', 0x32 = 'A', F8 2E = katakana NA), then the stream
// 05 FF | "01" | NL | PAGE NL | WAIT | DELAY 20 | COLOR 5 | "A" | NAME 0 | NA | AUTOCLOSE 120 | NL tokenised in order;
// END stops parsing, an unknown FF byte is a placeholder glyph, a truncated argument stops cleanly; a message block of two
// records with the text area at 16.
void test_ps1_tokens() {
  import::JpGlyphs jp;
  CHECK(jp.known_codes() > 1500);
  CHECK(jp.to_unicode(0x28, 0) == U'0');
  CHECK(jp.to_unicode(0x32, 0) == U'A');
  CHECK(jp.to_unicode(0xF8, 0x2E) == 0x30CA);
  CHECK(import::JpGlyphs::bitmap_index(0x28, 0) == 8);
  CHECK(import::JpGlyphs::bitmap_index(0xF8, 0x2E) == 256 + 14);
  CHECK(import::JpGlyphs::advance(8) == 8 && import::JpGlyphs::advance(0x76 - 0x20) == 12);

  const u8 stream[] = {0x05, 0xFF, 0x28, 0x29, 0x01, 0x02, 0x01, 0x03, 0x04, 0x14, 0x0E, 0x05, 0x32,
                       0x08, 0x00, 0xF8, 0x2E, 0x06, 0x78, 0x01};
  import::Message message = import::parse_ps1(stream, jp);
  using Kind = TokKind;
  const Kind want[] = {Kind::Speed, Kind::Text, Kind::Newline, Kind::Page, Kind::Wait, Kind::Delay, Kind::Color, Kind::Text,
                       Kind::Name,  Kind::Text, Kind::AutoClose};
  CHECK(message.tokens.size() == sizeof want / sizeof want[0]);
  for (size_t i = 0; i < message.tokens.size() && i < sizeof want / sizeof want[0]; ++i) CHECK(message.tokens[i].kind == want[i]);
  CHECK(message.tokens[0].arg == 255);
  CHECK(import::u32_to_utf8(message.tokens[1].text) == "01");
  CHECK(message.tokens[5].arg == 20 && message.tokens[6].arg == 5);
  CHECK(message.tokens[10].arg == 120);
  CHECK(import::u32_to_utf8(message.tokens[9].text) == "\xE3\x83\x8A");
  CHECK(message.source.find("{wend(120)}") != std::string::npos && message.source.find("{bpage}") != std::string::npos);

  const u8 end_stream[] = {0x28, 0x00, 0x29, 0xFF, 0xFF};
  import::Message end_message = import::parse_ps1(end_stream, jp);
  CHECK(end_message.tokens.size() == 2 && end_message.tokens[1].kind == Kind::End && text_of(end_message) == "0");
  const u8 unknown_stream[] = {0xFF, 0xFF, 0x00};
  CHECK(text_of(import::parse_ps1(unknown_stream, jp)) == "\xEF\xBC\x83\xEF\xBC\x83");

  const u8 truncated_stream[] = {0x28, 0x04};
  CHECK(text_of(import::parse_ps1(truncated_stream, jp)) == "0");

  const u8 block[] = {16, 0, 3, 0, 1, 0, 36, 0,
                      19, 0, 1, 0, 1, 0, 12, 0,
                      0x28, 0x29, 0x00,
                      0x32, 0x00};
  auto blk = import::parse_dialogue_block(block);
  CHECK(blk && blk->records.size() == 2);
  if (blk && blk->records.size() == 2) {
    CHECK(blk->records[0].bytes.size() == 3 && blk->records[1].bytes.size() == 2);
    CHECK(blk->records[0].width_px == 36);
  }
}

// Markup: SERGE% is name id 0, {66} / {99} are curly quotes, an unknown tag stays as text, {key(up)} is button 0x10; the
// JP form with a literal backslash-n (as lang.dat stores it), {SERGE%1}, {PARTY2%}, GRAY and {playwork}; the diamond
// marker and doubled apostrophes are dropped; flatten() resolves names and {Cap} / {-y} case.
void test_markup() {
  import::Message message = import::parse_markup(
      "By the way, SERGE%...\nYou'll{bpage}\nNext{wait(20)}{speed(255)}{color(2)}x{66}y{99}{weird}{HIDE}{key(up)}{mend}",
      import::Lang::En);
  using Kind = TokKind;
  const Kind want[] = {Kind::Text,  Kind::Name, Kind::Text,     Kind::Newline, Kind::Text, Kind::Page, Kind::Text, Kind::Delay, Kind::Speed,
                       Kind::Color, Kind::Text, Kind::HideName, Kind::Button,  Kind::End};
  CHECK(message.tokens.size() == sizeof want / sizeof want[0]);
  for (size_t i = 0; i < message.tokens.size() && i < sizeof want / sizeof want[0]; ++i) CHECK(message.tokens[i].kind == want[i]);
  CHECK(message.tokens.size() > 13 && message.tokens[1].arg == 0);
  CHECK(message.tokens[7].arg == 20 && message.tokens[8].arg == 255 && message.tokens[9].arg == 2);
  CHECK(message.tokens[4].text == U"You'll");
  CHECK(import::u32_to_utf8(message.tokens[10].text) == "x\xE2\x80\x9Cy\xE2\x80\x9D{weird}");
  CHECK(message.tokens[12].arg == 0x10);

  import::Message jp_message = import::parse_markup("{SERGE%}\\n{SERGE%1}{PARTY2%}{color(GRAY)}{playwork(3)}", import::Lang::Jp);
  CHECK(jp_message.tokens.size() == 6);
  CHECK(jp_message.tokens[0].kind == TokKind::Name && jp_message.tokens[1].kind == TokKind::Newline);
  CHECK(jp_message.tokens[2].kind == TokKind::Name && jp_message.tokens[2].arg2 == 1);
  CHECK(jp_message.tokens[3].kind == TokKind::PartyName && jp_message.tokens[3].arg == 2);
  CHECK(jp_message.tokens[4].kind == TokKind::Color && jp_message.tokens[4].arg == 5);
  CHECK(jp_message.tokens[5].kind == TokKind::PlayWork && jp_message.tokens[5].arg == 3);
  CHECK(text_of(import::parse_markup("a\xE2\x97\x86" "b''c", import::Lang::En)) == "ab'c");

  import::TextContext ctx;
  ctx.speaker_name = [](int id) { return id == 0 ? std::u32string(U"Serge") : std::u32string(U"?"); };
  import::Message flat = import::flatten(import::parse_markup("{Cap}hello SERGE%{-y}!{mend}", import::Lang::En), ctx);
  CHECK(text_of(flat) == "Hello Serge!");
  CHECK(flat.tokens.back().kind == TokKind::End);
}

void put32(Bytes& out, u32 value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(value >> (8 * i)));
}
void putf(Bytes& out, float value) {
  u32 bits;
  std::memcpy(&bits, &value, 4);
  put32(out, bits);
}

// Spritefont header: three glyphs written unsorted (C, A, B) come out sorted, find / find_or_default, line spacing 111.5,
// a 4 x 2 B4G4R4A4 texture (stride 8) whose texel (0, 0) is A=F R=0 G=F B=F = 0xF0FF and (0, 1) is 0x1234; trailing
// bytes (as in the ch_100 files) are ignored; a bad magic, a truncated glyph table and truncated pixels are refused.
void test_spritefont() {
  Bytes font_bytes;
  const char magic[] = "DXTKfont";
  font_bytes.insert(font_bytes.end(), magic, magic + 8);
  put32(font_bytes, 3);
  auto glyph = [&](u32 ch, int left, int top, int right, int bottom, float x_offset, float y_offset, float x_advance) {
    put32(font_bytes, ch);
    put32(font_bytes, static_cast<u32>(left));
    put32(font_bytes, static_cast<u32>(top));
    put32(font_bytes, static_cast<u32>(right));
    put32(font_bytes, static_cast<u32>(bottom));
    putf(font_bytes, x_offset);
    putf(font_bytes, y_offset);
    putf(font_bytes, x_advance);
  };
  glyph('C', 2, 0, 4, 2, 1, 2, 3);
  glyph('A', 0, 0, 2, 2, 11, 13, -24);
  glyph('B', 2, 0, 3, 2, 0, 0, 1);
  putf(font_bytes, 111.5f);
  put32(font_bytes, 'B');
  put32(font_bytes, 4);
  put32(font_bytes, 2);
  put32(font_bytes, 115);
  put32(font_bytes, 8);
  put32(font_bytes, 2);
  const u16 texels[8] = {0xF0FF, 0, 0, 0, 0x1234, 0, 0, 0};
  for (u16 texel : texels) {
    font_bytes.push_back(static_cast<u8>(texel & 0xFF));
    font_bytes.push_back(static_cast<u8>(texel >> 8));
  }
  font_bytes.insert(font_bytes.end(), {1, 2, 3, 4, 5});

  std::string err;
  auto font = import::parse_spritefont(Bytes(font_bytes), &err);
  CHECK(font.has_value());
  if (font) {
    CHECK(font->glyphs.size() == 3 && font->glyphs[0].ch == 'A' && font->glyphs[2].ch == 'C');
    const import::SfGlyph* glyph_a = font->find('A');
    CHECK(glyph_a && glyph_a->width() == 2 && glyph_a->x_offset == 11.0f && glyph_a->x_advance == -24.0f);
    CHECK(font->find('Z') == nullptr);
    CHECK(font->find_or_default('Z') == font->find('B'));
    CHECK(font->line_spacing == 111.5f && font->tex_width == 4 && font->format == 115 && font->stride == 8);
    u8 rgba[4];
    CHECK(font->texel(0, 0, rgba) && rgba[0] == 0 && rgba[1] == 255 && rgba[2] == 255 && rgba[3] == 255);
    CHECK(font->texel(0, 1, rgba) && rgba[3] == 0x1 * 17 && rgba[0] == 0x2 * 17 && rgba[1] == 0x3 * 17 && rgba[2] == 0x4 * 17);
    CHECK(!font->texel(4, 0, rgba));
  }
  Bytes bad_magic = font_bytes;
  bad_magic[0] = 'X';
  CHECK(!import::parse_spritefont(std::move(bad_magic), &err).has_value());
  Bytes cut_glyphs(font_bytes.begin(), font_bytes.begin() + 40);
  CHECK(!import::parse_spritefont(std::move(cut_glyphs), &err).has_value());
  Bytes cut_pixels(font_bytes.begin(), font_bytes.end() - 5 - 4);
  CHECK(!import::parse_spritefont(std::move(cut_pixels), &err).has_value());
}

// lang.dat lookup: the wend patch replaces the timed close, the mesgen column is picked by the detected speaker, a smaller
// room after a larger one is not loaded, there is no cross-language fallback; the Ex_ variants (FR) take their speaker
// from the first tag of the first variant; the language-column table decodes UTF-16 and TSV quoting.
void test_lang() {
  import::LangTables tables;
  tables.set_field_csv(import::Lang::En,
                       "\xEF\xBB\xBF" "27\t15\tHello {SERGE%}\\nline{wend(5)}\n"
                       "27\t16\t{GUILE%}Hi {WHO}\n"
                       "40\t0\tlate\n"
                       "30\t0\tskipped: smaller room after a larger one\n"
                       "40\t1\tkept\n");
  tables.set_mesgen(import::Lang::En, "{SERGE%},SERGE%\n{WHO},a,b,c\n");
  tables.set_wend_csv("# patch\n27\t15\t{wend(120)}\n");
  auto hello = tables.lookup_field(27, 15, import::Lang::En);
  CHECK(hello && *hello == "Hello SERGE%\nline{wend(120)}");
  import::LookupInfo info;
  auto guile = tables.lookup_field(27, 16, import::Lang::En, 0, &info);
  CHECK(guile && *guile == "{GUILE%}Hi c" && info.speaker == 2);
  CHECK(tables.lookup_field(40, 0, import::Lang::En).has_value());
  CHECK(!tables.lookup_field(30, 0, import::Lang::En).has_value());
  CHECK(tables.lookup_field(40, 1, import::Lang::En).has_value());
  CHECK(!tables.lookup_field(99, 0, import::Lang::En).has_value());
  CHECK(!tables.lookup_field(27, 15, import::Lang::Jp).has_value());

  tables.set_field_csv(import::Lang::Fr, "22\t1\t{PARTY2%}base\n");
  tables.set_ex_csv(import::Lang::Fr, "22\t1\t{GUILE%}a\t{GUILE%}b\t{GUILE%}c\n");
  import::LookupInfo fr_info;
  auto fr = tables.lookup_field(22, 1, import::Lang::Fr, 0, &fr_info);
  CHECK(fr && *fr == "{GUILE%}b" && fr_info.variant && fr_info.speaker == 2);

  import::TextTable tab;
  tab.parse("# \tText\tJP\tEN\tFR\r\n0x000001\t\xCE\xB4\t\xCE\xB4\tBone\t\"Os \"\"x\"\"\"\r\n0x801F2760\ta\tb\tYes\tOui\\nX\r\n");
  CHECK(tab.rows() == 2);
  const std::string* bone = tab.get(u64{1}, import::Lang::En);
  CHECK(bone && *bone == "Bone");
  const std::string* oui = tab.get(u64{0x801F2760}, import::Lang::Fr);
  CHECK(oui && *oui == "Oui\nX");
  const std::string* quoted = tab.get(std::string_view("0x000001"), import::Lang::Fr);
  CHECK(quoted && *quoted == "Os \"x\"");
  CHECK(tab.get(u64{7}, import::Lang::En) == nullptr);
  const u8 utf16[] = {0xFF, 0xFE, 'a', 0, 0x42, 0x30};
  CHECK(import::decode_text_member(utf16) == "a\xE3\x81\x82");
}

// The typewriter: {wait} without parentheses is plain text ({bwait} is the confirm wait); pages, events and glyph
// positions of a two-page message; the reveal at 30 Hz with a 30-frame pause, the page / confirm / close states;
// instant speed with a timed close; a confirm while revealing fast-forwards to the next wait / page end; word wrap
// breaks at spaces; choice mode: a line opened by three spaces lays its indent out with the choice space widths (63,
// then 18, 18) and is never wrapped, so the option text starts at 99 while plain lines keep the font advance; {fset(n)}
// / {fwait(n)} write and poll the field event register, and neither time nor a confirm passes an unanswered {fwait}.
// The naming rule: only character 0 in Lynx's body reads as Lynx; battle and menus ignore global flag 16, field text honours it; the typed
// name wins over the language's name otherwise, and a kana record is unset outside JP.
void test_char_names() {
  using data::NameContext;
  CHECK(data::shows_lynx(0, 0x31, NameContext::Menu, false));
  CHECK(data::shows_lynx(0, 0x31, NameContext::Menu, true));
  CHECK(data::shows_lynx(0, 0x31, NameContext::Field, false));
  CHECK(!data::shows_lynx(0, 0x31, NameContext::Field, true));
  CHECK(!data::shows_lynx(0, 0, NameContext::Menu, false));
  CHECK(!data::shows_lynx(1, 0x31, NameContext::Menu, false));
  CHECK(!data::shows_lynx(1, 0x31, NameContext::Field, false));
  CHECK(!data::shows_lynx(43, 0x2F, NameContext::Menu, false));

  const auto var = [](int id) { return id == 0x172 ? 0x1131 : 0x05; };
  CHECK(data::character_body(0x31, var) == 0x31);
  CHECK(data::character_body(0x131, var) == 0x31);
  CHECK(data::character_body(0x3D, var) == 0x31);
  CHECK(data::character_body(0x3C, var) == 0x05);
  CHECK(data::character_body(0x3F, var) == 0x3F);

  import::LangTables tables;
  tables.set_table("sysmes",
                   "key\ten\tde\tjp\n"
                   "0x010000\tSerge\tSerge-de\t\xE3\x82\xBB\xE3\x83\xAB\xE3\x82\xB8\xE3\x83\xA5\n"
                   "0x010001\tKid\tKid-de\tKid-jp\n"
                   "0x010005\tViper\tViper-de\tViper-jp\n"
                   "0x010031\tLynx\tLynx-de\tLynx-jp\n");
  const std::map<int, std::u32string> custom{{0, U"Hero"}, {1, U"\u30AD\u30C3\u30C9"}};
  data::CharNames names{&custom, &tables, import::Lang::En};
  CHECK(data::character_name(0, 0x31, NameContext::Menu, false, names) == U"Lynx");
  CHECK(data::character_name(0, 0x31, NameContext::Menu, true, names) == U"Lynx");
  CHECK(data::character_name(0, 0x31, NameContext::Field, false, names) == U"Lynx");
  CHECK(data::character_name(0, 0x31, NameContext::Field, true, names) == U"Hero");
  CHECK(data::character_name(0, 0, NameContext::Menu, false, names) == U"Hero");
  CHECK(data::own_name(0, names) == U"Hero");
  CHECK(data::character_name(1, 0x31, NameContext::Menu, false, names) == U"Kid");
  names.lang = import::Lang::De;
  CHECK(data::character_name(0, 0x31, NameContext::Menu, false, names) == U"Lynx-de");
  CHECK(data::character_name(5, 5, NameContext::Menu, false, names) == U"Viper-de");
  names.custom = nullptr;
  CHECK(data::character_name(0, 0, NameContext::Menu, false, names) == U"Serge-de");
  names.custom = &custom;
  names.lang = import::Lang::Jp;
  CHECK(data::character_name(1, 1, NameContext::Menu, false, names) == U"\u30AD\u30C3\u30C9");
  CHECK(data::character_name(5, 5, NameContext::Menu, false, names) == U"Viper-jp\u5927\u4F50");
  CHECK(data::character_name(5, 5, NameContext::Field, false, names, false) == U"Viper-jp");

  CHECK(data::usable_name(U"Hero", import::Lang::En));
  CHECK(!data::usable_name(U"\u30BB\u30EB", import::Lang::En));
  CHECK(data::usable_name(U"\u30BB\u30EB", import::Lang::Jp));
  CHECK(!data::usable_name(U"\u3000 ", import::Lang::Jp));
  CHECK(!data::usable_name(U"a\uFFFD", import::Lang::Jp));

  CHECK(data::line_speaker(0, true) == 0x31);
  CHECK(data::line_speaker(0, false) == 0);
  CHECK(data::line_speaker(7, true) == 7);
  tables.set_field_csv(import::Lang::En, "27\t1\t{SUFFIX}\n");
  std::string columns = "{SUFFIX}";
  for (int column = 0; column < 50; ++column) columns += "," + std::to_string(column);
  tables.set_mesgen(import::Lang::En, columns + "\n");
  CHECK(tables.lookup_field(27, 1, import::Lang::En, data::line_speaker(0, true)) == "49");
}

void test_menu_forms() {
  CHECK(data::menu_form(0, 0x31) == 0x31);
  CHECK(data::menu_form(0, 0x33) == 0);
  CHECK(data::menu_form(0, 0x32) == 0);
  CHECK(data::menu_form(0, 0x32, 0x32) == 0x32);
  CHECK(data::menu_form(0, 0) == 0);
  CHECK(data::menu_form(43, 0x2D) == 0x2D);
  CHECK(data::menu_form(1, 1) == 1);
  const auto read_var = [](int var) { return var == 0x172 ? 0x31 : 0; };
  CHECK(data::menu_form(0, data::character_body(0x3D, read_var)) == 0x31);
  CHECK(data::menu_form(0, data::character_body(0x3C, read_var)) == 0);
}

void test_word_wrap() {
  using namespace ui;
  LayoutMetrics lm;
  lm.advance = [](char32_t) { return 10.0f; };
  lm.line_height = 20;
  lm.wrap = true;
  const auto layout = [&](const char* markup, float limit) {
    lm.max_width = limit;
    return layout_pages(import::flatten(import::parse_markup(markup, import::Lang::En), {}), lm);
  };
  // The width is checked before each glyph: the last glyph of a page may overhang the limit.
  CHECK(layout("abcde fghij{mend}", 105)[0].lines == 1);
  const auto broken = layout("abcde fghijk{mend}", 105);
  CHECK(broken[0].lines == 2 && broken[0].glyphs.size() == 11 && broken[0].glyphs[5].ch == U'f' && broken[0].glyphs[5].x == 0);
  // A space that crosses the limit breaks before the word in front of it.
  CHECK(layout("ab cdefgh ij{mend}", 85)[0].lines == 2);
  // The limit is at least the widest word, and a word is never cut.
  CHECK(layout("a abcdefghij b{mend}", 45)[0].lines == 3);
  CHECK(layout("abcdefghij{mend}", 45)[0].lines == 1);
  // No limit, no wrap.
  CHECK(layout("abcde fghij klmno{mend}", 0)[0].lines == 1);
  // Events stay on their glyphs when a space leaves the page.
  const auto waiting = layout("abcd efgh{bwait}ij{mend}", 65);
  CHECK(waiting[0].lines == 2 && waiting[0].glyphs.size() == 10 && waiting[0].events.size() == 1 && waiting[0].events[0].at == 8);
  // The wrap counts the measured widths, not the pen advances.
  CHECK(wrap_glyph_width(64.0f, false) == 0.45f * 54.0f);
  CHECK(wrap_glyph_width(0.0f, false) == 18.0f);
  lm.wrap_advance = [](char32_t, bool) { return 20.0f; };
  CHECK(layout("abcde fghij{mend}", 105)[0].lines == 2);
  CHECK(layout("abcde fghij{mend}", 105)[0].glyphs[5].x == 0);
  lm.wrap_advance = nullptr;
  // A choice line is never wrapped.
  lm.choice_space_first = 63;
  lm.choice_space = 18;
  CHECK(layout("Q?\n   abcde fghij klmno pqrst{mend}", 105)[0].lines == 2);
}

void test_wrap_limit() {
  using import::Lang;
  using ui::wrap_limit;
  const auto chars = [](const char* markup) { return ui::disc_line_chars(import::parse_markup(markup, Lang::Jp)); };
  CHECK(chars("ab{SERGE%}c\nabcd{mend}") == 9);
  CHECK(chars("abcd{bwait}ef\nabc{mend}") == 4);
  CHECK(chars("ab{bpage}abc{wait(10)}d{mend}") == 4);
  CHECK(ui::disc_line_chars({}) == 0);

  CHECK(wrap_limit(74, 34, Lang::De, 11) == 682.0f);
  CHECK(wrap_limit(74, 34, Lang::De, 15) == 925.0f);
  CHECK(wrap_limit(74, 34, Lang::De, 0) == 1000.0f);
  CHECK(wrap_limit(74, 34, Lang::Jp, 11) == 0.0f);
  // The messages shown as authored are per language.
  CHECK(wrap_limit(387, 6, Lang::En, 5) == 0.0f);
  CHECK(wrap_limit(387, 6, Lang::Fr, 5) == 310.0f);
  CHECK(wrap_limit(387, 8, Lang::Fr, 10) == 0.0f);
  CHECK(wrap_limit(414, 19, Lang::En, 26) == 0.0f);
  CHECK(wrap_limit(414, 19, Lang::De, 26) == 925.0f);
  CHECK(wrap_limit(1490, 1, Lang::En, 9) == 925.0f);
}

void test_typewriter() {
  using namespace ui;
  import::Message message = import::parse_markup("ab{wait(30)}c{bpage}d{wait}{mend}", import::Lang::En);
  import::Message flat = import::flatten(message, {});
  CHECK(text_of(flat).find("{wait}") != std::string::npos);
  flat = import::flatten(import::parse_markup("ab{wait(30)}c{bpage}d{bwait}e{mend}", import::Lang::En), {});
  LayoutMetrics lm;
  lm.advance = [](char32_t) { return 10.0f; };
  lm.line_height = 20;
  auto pages = layout_pages(flat, lm);
  CHECK(pages.size() == 2);
  if (pages.size() != 2) return;
  CHECK(pages[0].glyphs.size() == 3 && !pages[0].ends_message && pages[0].width == 30.0f);
  CHECK(pages[1].glyphs.size() == 2 && pages[1].ends_message);
  CHECK(pages[0].events.size() == 1 && pages[0].events[0].at == 2 && pages[0].events[0].arg == 30);

  Typewriter tw;
  tw.start(pages);
  tw.update(0.1f);
  CHECK(tw.visible() == 2 && tw.state() == Typewriter::State::Revealing);
  tw.update(0.5f);
  CHECK(tw.visible() == 2);
  tw.update(0.7f);
  CHECK(tw.visible() == 3 && tw.state() == Typewriter::State::WaitPage);
  tw.confirm();
  CHECK(tw.page_index() == 1 && tw.visible() == 0);
  tw.update(0.1f);
  CHECK(tw.visible() == 1 && tw.state() == Typewriter::State::WaitConfirm);
  tw.confirm();
  tw.update(0.1f);
  CHECK(tw.state() == Typewriter::State::WaitClose && tw.visible() == 2);
  tw.confirm();
  CHECK(tw.closed());

  flat = import::flatten(import::parse_markup("{speed(255)}hello{wend(30)}{mend}", import::Lang::En), {});
  Typewriter instant;
  instant.start(layout_pages(flat, lm));
  instant.update(0.02f);
  CHECK(instant.visible() == 5 && instant.state() == Typewriter::State::Closing);
  instant.update(1.1f);
  CHECK(instant.closed());

  flat = import::flatten(import::parse_markup("{speed(8)}abcdef{wait(90)}g{mend}", import::Lang::En), {});
  Typewriter fast_forward;
  fast_forward.start(layout_pages(flat, lm));
  fast_forward.update(0.05f);
  fast_forward.confirm();
  fast_forward.update(0.05f);
  CHECK(fast_forward.visible() == 7 && fast_forward.state() == Typewriter::State::WaitClose);

  lm.wrap = true;
  lm.max_width = 45;
  auto wrapped = layout_pages(import::flatten(import::parse_markup("abc def ghi{mend}", import::Lang::En), {}), lm);
  CHECK(wrapped.size() == 1 && wrapped[0].lines == 3);

  lm.wrap = false;
  lm.choice_space_first = 63;
  lm.choice_space = 18;
  auto choice_pages = layout_pages(import::flatten(import::parse_markup("Q?\n   Ab\n   No{mend}", import::Lang::En), {}), lm);
  CHECK(choice_pages.size() == 1);
  if (choice_pages.size() == 1) {
    const auto& glyphs = choice_pages[0].glyphs;
    CHECK(glyphs.size() == 12);
    if (glyphs.size() == 12) {
      CHECK(glyphs[1].x == 10.0f);
      CHECK(glyphs[2].x == 0.0f && glyphs[3].x == 63.0f && glyphs[4].x == 81.0f);
      CHECK(glyphs[5].x == 99.0f && glyphs[5].ch == U'A');
      CHECK(glyphs[10].x == 99.0f && glyphs[10].ch == U'N');
    }
  }
  lm.choice_space_first = lm.choice_space = 0;

  lm.wrap = false;
  int event_register = 0;
  Typewriter event_tw;
  event_tw.event_get = [&] { return event_register; };
  event_tw.event_set = [&](int value) { event_register = value; };
  event_tw.start(layout_pages(import::flatten(import::parse_markup("{speed(255)}ab{fset(3)}{fwait(1)}c{mend}", import::Lang::En), {}), lm));
  event_tw.update(0.1f);
  CHECK(event_register == 3 && event_tw.visible() == 2 && event_tw.state() == Typewriter::State::Revealing);
  event_tw.confirm();
  event_tw.update(0.1f);
  CHECK(event_tw.visible() == 2);
  event_register = 1;
  event_tw.update(0.05f);
  CHECK(event_tw.visible() == 3 && event_tw.state() == Typewriter::State::WaitClose);
}

// Choice lines: the script's choice rows are guest (PS1) line numbers of the Japanese text, whose last page here has two
// question lines and the options at rows 2 and 3 (three U+3000 indent); the translated text lays its lines out
// differently (one question line), so row 2 maps to the first option and row 3 to the second; no marked line leaves the
// row unchanged and the result is never negative; the cursor rect exists for the option lines only.
void test_choice_lines() {
  using ui::DialogueBox;
  import::Message guest = import::parse_markup("a{bpage}\nQ1\nQ2\n　　　yes \n　　　no{mend}", import::Lang::Jp);
  const std::vector<int> guest_lines = DialogueBox::guest_choice_lines(guest);
  CHECK(guest_lines.size() == 2 && guest_lines[0] == -1 && guest_lines[1] == 2);
  import::Message en = import::parse_markup("Do you get my drift?\n   Understood\n   I don't understand{mend}", import::Lang::En);
  ui::LayoutMetrics lm;
  lm.advance = [](char32_t) { return 10.0f; };
  lm.line_height = 50;
  const auto pages = ui::layout_pages(import::flatten(en, {}), lm);
  CHECK(pages.size() == 1 && pages[0].lines == 3);
  const int first_line = DialogueBox::first_choice_line(pages[0], lm.line_height);
  CHECK(first_line == 1);
  CHECK(DialogueBox::map_choice_line(first_line, guest_lines[1], 2) == 1);
  CHECK(DialogueBox::map_choice_line(first_line, guest_lines[1], 3) == 2);
  CHECK(DialogueBox::map_choice_line(-1, guest_lines[1], 3) == 3);
  CHECK(DialogueBox::map_choice_line(0, 5, 2) == 0);
  for (int line = 1; line <= 2; ++line) {
    const auto cursor = DialogueBox::choice_cursor(pages[0], lm.line_height, line, false, 3);
    CHECK(cursor.valid && cursor.w > 0 && cursor.h > 0 && cursor.y >= static_cast<float>(line) * lm.line_height - 6.0f);
  }
  CHECK(!DialogueBox::choice_cursor(pages[0], lm.line_height, 3, false, 3).valid);
}

// Frame rows come from the text (rows = lines + 1) with no 3-line minimum: a one-line "!" is a wide 2-row window under
// 300 canvas units, each extra line adds one row of 61.75 units, and skin 8 sits its bottom corner lower.
void test_frame_rows() {
  using ui::DialogueBox;
  ui::LayoutMetrics lm;
  lm.advance = [](char32_t) { return 20.0f; };
  lm.line_height = 57.78f;
  auto frame_of = [&](const char* markup, int skin) {
    const auto pages = ui::layout_pages(import::flatten(import::parse_markup(markup, import::Lang::En), {}), lm);
    int lines = 1;
    float width = 0;
    for (const auto& page : pages) {
      lines = std::max(lines, page.lines);
      width = std::max(width, page.width);
    }
    return std::make_pair(lines, DialogueBox::frame_size(skin, width, lines));
  };
  const auto bang = frame_of("!{wend(60)}", 3);
  CHECK(bang.first == 1 && bang.second.rows == 2);
  CHECK(bang.second.w > bang.second.h);
  CHECK(bang.second.h < 300.0f);
  const auto three = frame_of("a\nb\nc{mend}", 3);
  CHECK(three.first == 3 && three.second.rows == 4);
  CHECK(std::fabs((three.second.h - bang.second.h) - 2 * 61.75f) < 0.01f);
  const auto skin8_one_line = frame_of("!{mend}", 8);
  CHECK(skin8_one_line.second.rows == 2 && skin8_one_line.second.h > bang.second.h);
  const auto skin8_two_lines = frame_of("a\nb{mend}", 8);
  CHECK(skin8_two_lines.second.rows == 3);
}

// Number entry: {input(n)} survives flatten and marks the next n glyphs (the placeholder digits) as digits 0..n-1; the text after them
// stays ordinary.
void test_digit_entry() {
  ui::LayoutMetrics lm;
  lm.advance = [](char32_t) { return 10.0f; };
  lm.line_height = 50;
  const auto pages =
      ui::layout_pages(import::flatten(import::parse_markup("Price?\n{speed(255)}   {input(2)}00G{mend}", import::Lang::En), {}), lm);
  CHECK(pages.size() == 1);
  if (pages.size() != 1) return;
  const auto& glyphs = pages[0].glyphs;
  CHECK(glyphs.size() == 12);
  if (glyphs.size() != 12) return;
  CHECK(glyphs[8].digit == -1 && glyphs[9].digit == 0 && glyphs[10].digit == 1 && glyphs[11].digit == -1);
  CHECK(glyphs[9].ch == U'0' && glyphs[11].ch == U'G' && glyphs[10].x == 40.0f);
}

// Window opening, one update per 60 Hz tick after the open tick: no text time for the 15 ticks that complete the 16-tick grow, the
// text from the 16th; drawn from the 17th tick (alpha 1/3, 2/3, 1). Without the grow the text starts on the first tick.
void test_window_opening() {
  const float tick = 1.0f / 60.0f;
  ui::WindowOpening opening;
  opening.start(true);
  float text_time = 0;
  for (int k = 1; k <= 15; ++k) text_time += opening.update(tick);
  CHECK(text_time < 1e-3f && !opening.growing() && opening.alpha() == 0.0f);
  CHECK(std::fabs(opening.update(tick) - tick) < 1e-4f);
  CHECK(opening.alpha() < 1.0f / 255.0f);
  opening.update(tick);
  CHECK(std::fabs(opening.alpha() - 1.0f / 3.0f) < 0.01f);
  opening.update(tick);
  CHECK(std::fabs(opening.alpha() - 2.0f / 3.0f) < 0.01f);
  opening.update(tick);
  CHECK(opening.alpha() == 1.0f);

  ui::WindowOpening grow_in_steps;
  grow_in_steps.start(true);
  CHECK(grow_in_steps.growing());
  CHECK(grow_in_steps.update(14 * tick) == 0.0f && grow_in_steps.growing());
  CHECK(std::fabs(grow_in_steps.update(2 * tick) - tick) < 1e-4f);

  ui::WindowOpening full_at_once;
  full_at_once.start(false);
  CHECK(!full_at_once.growing() && std::fabs(full_at_once.update(tick) - tick) < 1e-6f);
}

}  // namespace

int main() {
  test_ps1_tokens();
  test_markup();
  test_spritefont();
  test_lang();
  test_char_names();
  test_typewriter();
  test_choice_lines();
  test_menu_forms();
  test_word_wrap();
  test_wrap_limit();
  test_frame_rows();
  test_digit_entry();
  test_window_opening();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("test_text: ok\n");
  return 0;
}
