#include "import/text.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>

#include "import/container.h"
#include "import/lzss.h"
#include "vfs/vfs.h"

namespace oc::import {

#include "import/text_tables.inc"

namespace {

constexpr char32_t kReplacement = 0xFFFD;

const char* const kSpeakers[50] = {
    "SERGE",  "KID",    "GUILE",   "NORRIS", "NIKKI",  "VIPER",  "RIDDEL", "KARSH",  "ZOAH",   "MARCY",
    "KORCHA", "LUCCIA", "POSHUL",  "RAZZLY", "ZAPPA",  "ORCHA",  "RADIUS", "FARGO",  "MACHA",  "GLENN",
    "LEENA",  "MIKI",   "HARLEY",  "JANICE", "DRAGGY", "STARKY", "SPRIGG", "MOJO",   "TURNIP", "NEOFIO",
    "GRECO",  "SKELLY", "FUNGUY",  "IRENES", "MEL",    "LEEA",   "VAN",    "SNEFF",  "STEENA", "DOC",
    "GROBYC", "PIERRE", "ORLHA",   "PIP",    nullptr,  nullptr,  nullptr,  nullptr,  nullptr,  "LYNX"};

bool is_space_tab(char ch) { return ch == ' ' || ch == '\t'; }

// Strict integer parse of a whole string (base auto-detected, like Python int(s, 0)).
bool parse_int(std::string_view text, long long& out) {
  if (text.empty()) return false;
  std::string tmp(text);
  char* end = nullptr;
  errno = 0;
  long long value = std::strtoll(tmp.c_str(), &end, 0);
  if (end == tmp.c_str() || *end != '\0' || errno != 0) return false;
  out = value;
  return true;
}

std::string_view strip_bom(std::string_view text) {
  if (text.size() >= 3 && static_cast<u8>(text[0]) == 0xEF && static_cast<u8>(text[1]) == 0xBB && static_cast<u8>(text[2]) == 0xBF)
    text.remove_prefix(3);
  return text;
}

// Calls callback(line) for each '\n' separated line with a trailing '\r' removed.
template <typename LineCallback>
void for_each_line(std::string_view text, LineCallback&& callback) {
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    callback(line);
    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
}

void replace_all(std::string& text, std::string_view from, std::string_view to) {
  if (from.empty()) return;
  size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
}

// ----- speaker detection (RDE 0x451580) -----
int detect_speaker(std::string_view text, int dflt, Lang lang) {
  size_t i = 0, length = text.size();
  while (i < length) {
    while (i < length && is_space_tab(text[i])) ++i;
    if (i >= length || text[i] != '{') break;
    size_t j = text.find('}', i);
    if (j == std::string_view::npos) break;
    std::string_view tag = text.substr(i + 1, j - i - 1);
    size_t pct = tag.find('%');
    if (pct != std::string_view::npos) {
      int speaker = speaker_id_from_tag(tag.substr(0, pct));
      if (speaker > 0) {
        if (lang == Lang::Jp && speaker == 10 && text.substr(j + 1, 3) == "\xE6\xAF\x8D") return 18;
        return speaker;
      }
    }
    i = j + 1;
  }
  return dflt ? dflt : 1;
}

std::string normalise_nonjp(std::string_view in, Lang lang) {
  std::u32string input = utf8_to_u32(in);
  std::u32string output;
  output.reserve(input.size());
  for (char32_t ch : input) {
    if (ch == 0x20 || (ch >= 0x2002 && ch <= 0x200B) || ch == 0x3000 || ch == 0xFEFF || ch == 9)
      output.push_back(U' ');
    else if (ch == 0x2018 || ch == 0x2019)
      output.push_back(U'\'');
    else if (ch == 0x201D)
      output.push_back(U'"');
    else if (ch == 0x201C)
      output.push_back(lang == Lang::De ? ch : U'"');
    else
      output.push_back(ch);
  }
  return u32_to_utf8(output);
}

// ----- markup tokenizer helpers -----
std::string ascii_lower(std::string_view text) {
  std::string lowered(text);
  for (char& ch : lowered) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return lowered;
}

// "name(arg)" -> name, arg; plain "name" leaves has_arg false.
struct Call {
  std::string name;
  std::string arg;
  bool has_arg = false;
};
Call split_call(std::string_view tag) {
  Call call;
  size_t paren = tag.find('(');
  if (paren == std::string_view::npos || tag.back() != ')') {
    call.name = std::string(tag);
    return call;
  }
  call.name = std::string(tag.substr(0, paren));
  call.arg = std::string(tag.substr(paren + 1, tag.size() - paren - 2));
  call.has_arg = true;
  return call;
}

int button_arg(const std::string& arg) {
  long long value;
  if (parse_int(arg, value)) return static_cast<int>(value);
  std::string lowered = ascii_lower(arg);
  if (lowered == "up") return 0x10;
  if (lowered == "down") return 0x11;
  if (lowered == "right") return 0x12;
  if (lowered == "left") return 0x13;
  return 0;
}

struct Builder {
  Message msg;
  std::u32string run;
  void flush() {
    if (run.empty()) return;
    Token token;
    token.kind = TokKind::Text;
    token.text = std::move(run);
    run.clear();
    msg.tokens.push_back(std::move(token));
  }
  void push(TokKind kind, int arg = 0, int arg2 = 0) {
    flush();
    Token token;
    token.kind = kind;
    token.arg = arg;
    token.arg2 = arg2;
    msg.tokens.push_back(std::move(token));
  }
  Message finish() {
    flush();
    // A newline that directly follows a page break belongs to the break.
    auto& tokens = msg.tokens;
    for (size_t i = 0; i + 1 < tokens.size();) {
      if (tokens[i].kind == TokKind::Page && tokens[i + 1].kind == TokKind::Newline)
        tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(i) + 1);
      else
        ++i;
    }
    return std::move(msg);
  }
};

// Handles one {tag}; false if unknown (caller prints it as plain text).
bool handle_tag(Builder& builder, std::string_view tag) {
  if (tag.empty()) return false;
  if (tag == "mend") return builder.push(TokKind::End), true;
  if (tag == "bpage") return builder.push(TokKind::Page), true;
  if (tag == "bwait") return builder.push(TokKind::Wait), true;
  if (tag == "italic") return builder.push(TokKind::Italic, 1), true;
  if (tag == "italic_end") return builder.push(TokKind::Italic, 0), true;
  if (tag == "66") return builder.run.push_back(0x201C), true;
  if (tag == "99") return builder.run.push_back(0x201D), true;
  // "n/a" marks a deleted string.
  if (tag == "n/a") return true;
  if (tag == "HIDE") return builder.push(TokKind::HideName), true;
  if (tag == "-y") return builder.push(TokKind::Suffix, 1), true;
  if (tag == "-i") return builder.push(TokKind::Suffix, 2), true;
  if (tag == "Cap") return builder.push(TokKind::Case, 1), true;
  if (tag == "CAP") return builder.push(TokKind::Case, 2), true;
  if (tag == "CapL") return builder.push(TokKind::Case, 3), true;
  if (tag == "accent") return builder.push(TokKind::Accent, 0), true;
  if (tag == "\xE2\x99\xAA") return builder.run.push_back(0x266A), true;
  if (tag == "\xE2\x99\xAA\xE2\x99\xAA") return builder.run.append(2, 0x266A), true;
  // Battle window size tags are ignored.
  if (tag.rfind("wmode", 0) == 0 || tag.rfind("size", 0) == 0) return true;

  // Name tokens: SERGE%  SERGE%1  PARTY1%
  size_t pct = tag.find('%');
  if (pct != std::string_view::npos) {
    std::string_view nm = tag.substr(0, pct);
    std::string_view rest = tag.substr(pct + 1);
    int trunc = 0;
    if (rest.size() == 1 && std::isdigit(static_cast<unsigned char>(rest[0]))) trunc = rest[0] - '0';
    else if (!rest.empty()) return false;
    if (nm.size() == 6 && nm.rfind("PARTY", 0) == 0 && std::isdigit(static_cast<unsigned char>(nm[5])))
      return builder.push(TokKind::PartyName, nm[5] - '0'), true;
    int id = speaker_id_from_tag(nm);
    if (id >= 0) return builder.push(TokKind::Name, id, trunc), true;
    return false;
  }

  Call call = split_call(tag);
  if (!call.has_arg) return false;
  long long value = 0;
  bool num = parse_int(call.arg, value);
  const std::string& name = call.name;
  if (name == "color") {
    if (!num) {
      if (call.arg == "GRAY") value = 5;
      else return false;
    }
    return builder.push(TokKind::Color, static_cast<int>(value)), true;
  }
  if (name == "key") return builder.push(TokKind::Button, button_arg(call.arg)), true;
  if (!num) return false;
  int arg = static_cast<int>(value);
  if (name == "wait") return builder.push(TokKind::Delay, arg), true;
  if (name == "speed") return builder.push(TokKind::Speed, arg), true;
  if (name == "wend") return builder.push(TokKind::AutoClose, arg), true;
  if (name == "playwork") return builder.push(TokKind::PlayWork, arg), true;
  if (name == "itemwork") return builder.push(TokKind::ItemWork, arg), true;
  if (name == "item") return builder.push(TokKind::Item, arg), true;
  if (name == "num") return builder.push(TokKind::Num, arg), true;
  if (name == "fset") return builder.push(TokKind::FSet, arg), true;
  if (name == "fwait") return builder.push(TokKind::FWait, arg), true;
  if (name == "input") return builder.push(TokKind::Input, arg), true;
  if (name == "gram") return builder.push(TokKind::Accent, arg), true;
  if (name == "case") return builder.push(TokKind::Case, arg), true;
  if (name == "suffix") return builder.push(TokKind::Suffix, arg), true;
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------------------------- languages / utf8
const char* lang_code(Lang lang) {
  static const char* const codes[6] = {"en", "fr", "it", "de", "es", "jp"};
  return codes[static_cast<int>(lang)];
}

bool parse_lang(std::string_view code, Lang& out) {
  std::string lowered = ascii_lower(code);
  for (int i = 0; i < 6; ++i) {
    if (lowered == lang_code(static_cast<Lang>(i))) {
      out = static_cast<Lang>(i);
      return true;
    }
  }
  return false;
}

std::u32string utf8_to_u32(std::string_view text) {
  std::u32string out;
  out.reserve(text.size());
  size_t i = 0, length = text.size();
  while (i < length) {
    u8 lead = static_cast<u8>(text[i]);
    int len = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 0;
    if (len == 0 || i + static_cast<size_t>(len) > length) {
      out.push_back(kReplacement);
      ++i;
      continue;
    }
    char32_t cp = len == 1 ? lead : len == 2 ? (lead & 0x1F) : len == 3 ? (lead & 0x0F) : (lead & 0x07);
    bool ok = true;
    for (int k = 1; k < len; ++k) {
      u8 cont = static_cast<u8>(text[i + static_cast<size_t>(k)]);
      if ((cont & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (cont & 0x3F);
    }
    if (!ok) {
      out.push_back(kReplacement);
      ++i;
      continue;
    }
    out.push_back(cp);
    i += static_cast<size_t>(len);
  }
  return out;
}

std::string u32_to_utf8(std::u32string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char32_t cp : text) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

std::string decode_text_member(ByteSpan raw) {
  if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
    std::u32string decoded;
    decoded.reserve(raw.size() / 2);
    for (size_t i = 2; i + 1 < raw.size(); i += 2) {
      char32_t cp = static_cast<char32_t>(raw[i] | (raw[i + 1] << 8));
      if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < raw.size()) {
        char32_t low = static_cast<char32_t>(raw[i + 2] | (raw[i + 3] << 8));
        if (low >= 0xDC00 && low < 0xE000) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          i += 2;
        }
      }
      decoded.push_back(cp);
    }
    return u32_to_utf8(decoded);
  }
  std::string_view sv(reinterpret_cast<const char*>(raw.data()), raw.size());
  return std::string(strip_bom(sv));
}

// ---------------------------------------------------------------------------------------------- speakers
const char* speaker_tag(int id) { return id >= 0 && id < 50 ? kSpeakers[id] : nullptr; }

int speaker_id_from_tag(std::string_view name) {
  for (int i = 0; i < 50; ++i)
    if (kSpeakers[i] && name == kSpeakers[i]) return i;
  return -1;
}

// ---------------------------------------------------------------------------------------------- markup
Message parse_markup(std::string_view text, Lang lang) {
  std::u32string chars = utf8_to_u32(text);
  Builder builder;
  builder.msg.source = std::string(text);
  const bool en = lang == Lang::En;
  size_t i = 0, count = chars.size();
  while (i < count) {
    char32_t ch = chars[i];
    if (ch == U'\\' && i + 1 < count && chars[i + 1] == U'n') {
      builder.push(TokKind::Newline);
      i += 2;
    } else if (ch == U'\n') {
      builder.push(TokKind::Newline);
      ++i;
    } else if (ch == U'\r') {
      ++i;
    } else if (ch == U'{') {
      size_t j = chars.find(U'}', i);
      if (j != std::u32string::npos) {
        std::string tag = u32_to_utf8(std::u32string_view(chars).substr(i + 1, j - i - 1));
        // An unknown {...} is kept as plain characters.
        if (!handle_tag(builder, tag)) builder.run.append(chars, i, j - i + 1);
        i = j + 1;
      } else {
        builder.run.push_back(ch);
        ++i;
      }
    } else if (en && ch == 0x25C6) {
      // The diamond marker prints nothing.
      ++i;
    } else if (en && ch == U'\'' && i + 1 < count && chars[i + 1] == U'\'') {
      builder.run.push_back(U'\'');
      i += 2;
    } else if (en && ch >= U'A' && ch <= U'Z' && (i == 0 || !(chars[i - 1] >= U'A' && chars[i - 1] <= U'Z'))) {
      // Bare name token "SERGE%": letters run followed by '%'.
      size_t j = i;
      std::string word;
      while (j < count && chars[j] >= U'A' && chars[j] <= U'Z' && word.size() < 8) word.push_back(static_cast<char>(chars[j++]));
      int id = (j < count && chars[j] == U'%') ? speaker_id_from_tag(word) : -1;
      if (id >= 0) {
        builder.push(TokKind::Name, id);
        i = j + 1;
      } else {
        builder.run.push_back(ch);
        ++i;
      }
    } else {
      builder.run.push_back(ch);
      ++i;
    }
  }
  return builder.finish();
}

// ---------------------------------------------------------------------------------------------- flatten
Message flatten(const Message& message, const TextContext& ctx) {
  Builder builder;
  builder.msg.source = message.source;
  int case_mode = 0;
  bool hide = false;
  auto emit = [&](std::u32string text) {
    if (text.empty()) return;
    if (case_mode == 1 && !text.empty()) {
      text[0] = (text[0] < 0x80) ? static_cast<char32_t>(std::toupper(static_cast<int>(text[0]))) : text[0];
      case_mode = 0;
    } else if (case_mode == 2) {
      for (char32_t& ch : text)
        if (ch < 0x80) ch = static_cast<char32_t>(std::toupper(static_cast<int>(ch)));
      case_mode = 0;
    }
    builder.run += text;
  };
  auto named = [&](const std::function<std::u32string(int)>& resolve, int arg, const char* fallback) {
    std::u32string name = resolve ? resolve(arg) : std::u32string();
    if (name.empty()) {
      for (const char* ch = fallback; *ch; ++ch) name.push_back(static_cast<char32_t>(*ch));
    }
    return name;
  };
  for (const Token& token : message.tokens) {
    switch (token.kind) {
      case TokKind::Text: emit(token.text); break;
      case TokKind::Name: {
        if (hide) {
          hide = false;
          break;
        }
        const char* tag = speaker_tag(token.arg);
        std::u32string name = named(ctx.speaker_name, token.arg, tag ? tag : "?");
        if (token.arg2 > 0 && static_cast<size_t>(token.arg2) < name.size()) name.resize(static_cast<size_t>(token.arg2));
        // A case code in front of a name has no effect.
        case_mode = 0;
        builder.run += name;
        break;
      }
      case TokKind::PartyName: emit(named(ctx.party_name, token.arg, ("PARTY" + std::to_string(token.arg)).c_str())); break;
      case TokKind::PlayWork: emit(named(ctx.script_name, token.arg, "PLAYER")); break;
      case TokKind::ItemWork: emit(named(ctx.script_item, token.arg, "ITEM")); break;
      case TokKind::Item: emit(named(ctx.item_name, token.arg, "ITEM")); break;
      case TokKind::Num: {
        int number = ctx.script_number ? ctx.script_number(token.arg) : 0;
        std::string digits = std::to_string(number);
        emit(std::u32string(digits.begin(), digits.end()));
        break;
      }
      case TokKind::Case: case_mode = (token.arg == 1 || token.arg == 2) ? token.arg : 0; break;
      case TokKind::HideName: hide = true; break;
      case TokKind::Suffix:
      case TokKind::Accent:
        // No display form.
        break;
      default: builder.push(token.kind, token.arg, token.arg2); break;
    }
  }
  return builder.finish();
}

std::string describe(const Message& message) {
  std::string out;
  auto num = [&](const char* nm, int value) { out += "[" + std::string(nm) + " " + std::to_string(value) + "]"; };
  for (const Token& token : message.tokens) {
    switch (token.kind) {
      case TokKind::Text: out += "[txt \"" + u32_to_utf8(token.text) + "\"]"; break;
      case TokKind::Newline: out += "[nl]"; break;
      case TokKind::Page: out += "[page]"; break;
      case TokKind::Wait: out += "[wait]"; break;
      case TokKind::Delay: num("delay", token.arg); break;
      case TokKind::Speed: num("speed", token.arg); break;
      case TokKind::AutoClose: num("autoclose", token.arg); break;
      case TokKind::End: out += "[end]"; break;
      case TokKind::Color: num("color", token.arg); break;
      case TokKind::Italic: num("italic", token.arg); break;
      case TokKind::Name: num("name", token.arg); break;
      case TokKind::PartyName: num("party", token.arg); break;
      case TokKind::PlayWork: num("playwork", token.arg); break;
      case TokKind::ItemWork: num("itemwork", token.arg); break;
      case TokKind::Item: num("item", token.arg); break;
      case TokKind::Num: num("num", token.arg); break;
      case TokKind::Button: num("button", token.arg); break;
      case TokKind::Case: num("case", token.arg); break;
      case TokKind::Suffix: num("suffix", token.arg); break;
      case TokKind::HideName: out += "[hide]"; break;
      case TokKind::FWait: num("fwait", token.arg); break;
      case TokKind::FSet: num("fset", token.arg); break;
      case TokKind::Input: num("input", token.arg); break;
      case TokKind::Accent: num("accent", token.arg); break;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------------------------- JP glyphs
JpGlyphs::JpGlyphs() {
  for (const auto& glyph : kJpGlyphChars) {
    unicode_[glyph.key] = glyph.cp;
    u8 b0 = glyph.key > 0xFF ? static_cast<u8>(glyph.key >> 8) : static_cast<u8>(glyph.key);
    u8 b1 = glyph.key > 0xFF ? static_cast<u8>(glyph.key & 0xFF) : 0;
    index_of_.emplace(static_cast<char32_t>(glyph.cp), bitmap_index(b0, b1));
  }
  // The table only has the glyphs the JP text uses; the Latin blocks are contiguous, so complete them:
  // ASCII digits 0x28.., A-Z 0x32.., a-z 0x4C.. and the full-width forms D0.. digits, DA.. A-Z, F800.. a-z.
  auto fill = [&](u32 key, char32_t cp) {
    if (unicode_.emplace(key, cp).second) {
      const u8 b0 = key > 0xFF ? static_cast<u8>(key >> 8) : static_cast<u8>(key);
      const u8 b1 = key > 0xFF ? static_cast<u8>(key & 0xFF) : 0;
      index_of_.emplace(cp, bitmap_index(b0, b1));
    }
  };
  for (u32 i = 0; i < 10; ++i) fill(0x28 + i, U'0' + i), fill(0xD0 + i, 0xFF10 + i);
  for (u32 i = 0; i < 26; ++i) {
    fill(0x32 + i, U'A' + i);
    fill(0x4C + i, U'a' + i);
    fill(0xDA + i, 0xFF21 + i);
    fill(0xF800 + i, 0xFF41 + i);
  }
}

int JpGlyphs::bitmap_index(u8 b0, u8 b1) {
  if (b0 < 0xF8) return static_cast<int>(b0) - 0x20;
  return (static_cast<int>(b0) - 0xF7) * 256 + (static_cast<int>(b1) - 0x20);
}

char32_t JpGlyphs::to_unicode(u8 b0, u8 b1) const {
  u32 key = b0 < 0xF8 ? b0 : (static_cast<u32>(b0) << 8) | b1;
  auto it = unicode_.find(key);
  return it == unicode_.end() ? 0 : it->second;
}

int JpGlyphs::index_for(char32_t cp) const {
  auto it = index_of_.find(cp);
  return it == index_of_.end() ? -1 : it->second;
}

bool JpGlyphs::load_bitmaps(ByteSpan exe) {
  const size_t need = kJpFontOffset + static_cast<size_t>(kJpGlyphSlots) * kJpGlyphBytes;
  if (exe.size() < need) return false;
  bitmaps_.resize(static_cast<size_t>(kJpGlyphSlots) * kJpGlyphSize);
  for (size_t i = 0; i < bitmaps_.size(); ++i) {
    size_t offset = kJpFontOffset + i * 2;
    bitmaps_[i] = static_cast<u16>(exe[offset] | (exe[offset + 1] << 8));
  }
  return true;
}

const u16* JpGlyphs::rows(int index) const {
  if (bitmaps_.empty() || index < 0 || index >= kJpGlyphSlots) return nullptr;
  return &bitmaps_[static_cast<size_t>(index) * kJpGlyphSize];
}

// ---------------------------------------------------------------------------------------------- PS1 stream
std::optional<DialogueBlock> parse_dialogue_block(ByteSpan block) {
  if (block.size() < 8) return std::nullopt;
  size_t first = block[0] | (block[1] << 8);
  size_t record_count = first / 8;
  if (record_count == 0 || first > block.size()) return std::nullopt;
  DialogueBlock dialogue;
  std::set<size_t> ptrs;
  for (size_t i = 0; i < record_count; ++i) {
    DialogueBlock::Record record;
    const u8* bytes = &block[i * 8];
    record.ptr = static_cast<u16>(bytes[0] | (bytes[1] << 8));
    record.width_chars = static_cast<u16>(bytes[2] | (bytes[3] << 8));
    record.lines = static_cast<u16>(bytes[4] | (bytes[5] << 8));
    record.width_px = static_cast<u16>(bytes[6] | (bytes[7] << 8));
    ptrs.insert(record.ptr);
    dialogue.records.push_back(record);
  }
  ptrs.insert(block.size());
  for (auto& record : dialogue.records) {
    if (record.ptr > block.size()) return std::nullopt;
    auto it = ptrs.upper_bound(record.ptr);
    size_t end = it == ptrs.end() ? block.size() : std::min(*it, block.size());
    record.bytes = block.subspan(record.ptr, end - record.ptr);
  }
  return dialogue;
}

namespace {
// PS1 control code -> (argument bytes). -1 = unknown/no-op without args.
int ctrl_args(u8 code) {
  switch (code) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x1F: return 0;
    case 0x04: case 0x05: case 0x06: case 0x08: case 0x0A: case 0x0B: case 0x0E: case 0x10: case 0x12:
    case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1B: case 0x1C: case 0x1E: return 1;
    case 0x07: return 2;
    // 09 0C 0D 0F 11 13 14 1A 1D are no-ops.
    default: return 0;
  }
}
}  // namespace

Message parse_ps1(ByteSpan stream, const JpGlyphs& glyphs) {
  Builder builder;
  size_t i = 0, size = stream.size();
  bool ended = false;
  while (i < size && !ended) {
    u8 code = stream[i];
    if (code >= 0x20) {
      char32_t cp;
      if (code == 0xFF) {
        // Not a prefix: a placeholder glyph (debug string).
        cp = 0xFF03;
        i += 1;
      } else if (code >= 0xF8) {
        if (i + 1 >= size) break;
        cp = glyphs.to_unicode(code, stream[i + 1]);
        i += 2;
      } else {
        cp = glyphs.to_unicode(code, 0);
        i += 1;
      }
      builder.run.push_back(cp ? cp : U'?');
      continue;
    }
    int na = ctrl_args(code);
    if (i + 1 + static_cast<size_t>(na) > size) break;
    int arg = 0;
    if (na == 1) arg = stream[i + 1];
    else if (na == 2) arg = stream[i + 1] | (stream[i + 2] << 8);
    i += 1 + static_cast<size_t>(na);
    switch (code) {
      case 0x00: builder.push(TokKind::End); ended = true; break;
      case 0x01: builder.push(TokKind::Newline); break;
      case 0x02: builder.push(TokKind::Page); break;
      case 0x03: builder.push(TokKind::Wait); break;
      case 0x04: builder.push(TokKind::Delay, arg); break;
      case 0x05: builder.push(TokKind::Speed, arg); break;
      case 0x06: builder.push(TokKind::AutoClose, arg); break;
      case 0x07: builder.push(TokKind::Item, arg); break;
      case 0x08: builder.push(TokKind::Name, arg); break;
      case 0x0A: case 0x0B: builder.push(TokKind::Num, arg); break;
      case 0x0E: builder.push(TokKind::Color, arg); break;
      case 0x10: builder.push(TokKind::Input, arg); break;
      case 0x12: case 0x1F: builder.push(TokKind::Accent, arg); break;
      case 0x15: builder.push(TokKind::PlayWork, arg); break;
      case 0x16: builder.push(TokKind::FWait, arg); break;
      case 0x17: builder.push(TokKind::FSet, arg); break;
      case 0x18: builder.push(TokKind::PartyName, arg); break;
      case 0x19: builder.push(TokKind::ItemWork, arg); break;
      case 0x1B: builder.push(TokKind::Button, arg); break;
      case 0x1C: builder.push(TokKind::Case, arg); break;
      case 0x1E: builder.push(TokKind::Suffix, arg); break;
      default: break;
    }
  }
  Message message = builder.finish();
  // Drop the newline that follows a timed close ("06 xx 01").
  while (!message.tokens.empty() && message.tokens.back().kind == TokKind::Newline) message.tokens.pop_back();
  message.source = ps1_to_markup(stream, glyphs);
  return message;
}

std::string ps1_to_markup(ByteSpan stream, const JpGlyphs& glyphs) {
  std::string out;
  size_t i = 0, size = stream.size();
  char buf[48];
  while (i < size) {
    u8 code = stream[i];
    if (code >= 0x20) {
      char32_t cp;
      int len = 1;
      if (code == 0xFF) {
        cp = 0xFF03;
      } else if (code >= 0xF8 && i + 1 < size) {
        cp = glyphs.to_unicode(code, stream[i + 1]);
        len = 2;
      } else {
        cp = glyphs.to_unicode(code, 0);
      }
      if (cp) {
        out += u32_to_utf8(std::u32string_view(&cp, 1));
      } else {
        out += "[";
        for (int k = 0; k < len; ++k) {
          std::snprintf(buf, sizeof buf, "%02x", stream[i + static_cast<size_t>(k)]);
          out += buf;
        }
        out += "]";
      }
      i += static_cast<size_t>(len);
      continue;
    }
    int na = ctrl_args(code);
    int arg = 0;
    if (i + 1 + static_cast<size_t>(na) <= size) {
      if (na == 1) arg = stream[i + 1];
      else if (na == 2) arg = stream[i + 1] | (stream[i + 2] << 8);
    }
    i += 1 + static_cast<size_t>(na);
    switch (code) {
      case 0x00: out += "{mend}"; break;
      case 0x01: out += "\\n"; break;
      case 0x02: out += "{bpage}"; break;
      case 0x03: out += "{bwait}"; break;
      case 0x04: out += "{wait(" + std::to_string(arg) + ")}"; break;
      case 0x05: out += "{speed(" + std::to_string(arg) + ")}"; break;
      case 0x06: out += "{wend(" + std::to_string(arg) + ")}"; break;
      case 0x07: out += "{item(" + std::to_string(arg) + ")}"; break;
      case 0x08: {
        const char* tag = speaker_tag(arg);
        out += tag ? "{" + std::string(tag) + "%}" : "{name(" + std::to_string(arg) + ")}";
        break;
      }
      case 0x0A: case 0x0B: out += "{num(" + std::to_string(arg) + ")}"; break;
      case 0x0E: out += "{color(" + std::to_string(arg) + ")}"; break;
      case 0x10: out += "{input(" + std::to_string(arg) + ")}"; break;
      case 0x12: out += "{gram(" + std::to_string(arg) + ")}"; break;
      case 0x15: out += "{playwork(" + std::to_string(arg) + ")}"; break;
      case 0x16: out += "{fwait(" + std::to_string(arg) + ")}"; break;
      case 0x17: out += "{fset(" + std::to_string(arg) + ")}"; break;
      case 0x18: out += "{PARTY" + std::to_string(arg) + "%}"; break;
      case 0x19: out += "{itemwork(" + std::to_string(arg) + ")}"; break;
      case 0x1B: out += "{key(" + std::to_string(arg) + ")}"; break;
      case 0x1C: out += "{case(" + std::to_string(arg) + ")}"; break;
      case 0x1E: out += "{suffix(" + std::to_string(arg) + ")}"; break;
      case 0x1F: out += "{accent}"; break;
      default: break;
    }
  }
  return out;
}

std::optional<Bytes> read_room_dialogue_block(const vfs::Vfs& vfs, std::string_view room_name) {
  auto bin = vfs.read("cdrom/map/mapbin/" + std::string(room_name) + ".bin");
  if (!bin) return std::nullopt;
  auto cpt = parse_cpt(*bin);
  if (!cpt || cpt->entries.size() <= 8) return std::nullopt;
  ByteSpan sub = cpt->entries[8].data;
  if (!is_lzss(sub)) return std::nullopt;
  return lzss_decode(sub);
}

const char* room_name_for_id(int room_id) {
  for (const auto& entry : kRoomNames)
    if (entry.id == room_id) return entry.name;
  return nullptr;
}

int room_id_from_name(std::string_view name) {
  for (const auto& entry : kRoomNames)
    if (name == entry.name) return entry.id;
  return -1;
}

// ---------------------------------------------------------------------------------------------- tables
void FieldTable::parse(std::string_view text, bool rule) {
  text = strip_bom(text);
  long long max_room = -1;
  for_each_line(text, [&](std::string_view line) {
    if (line.empty() || line[0] == '#') return;
    size_t t1 = line.find('\t');
    if (t1 == std::string_view::npos) return;
    size_t t2 = line.find('\t', t1 + 1);
    long long room, msg;
    if (!parse_int(line.substr(0, t1), room)) return;
    std::string_view msg_text = line.substr(t1 + 1, t2 == std::string_view::npos ? std::string_view::npos : t2 - t1 - 1);
    if (!parse_int(msg_text, msg)) return;
    bool ok = !rule || room >= max_room;
    max_room = std::max(max_room, room);
    if (!ok) return;
    rows.insert_or_assign(key(static_cast<u32>(room), static_cast<u32>(msg)),
                          t2 == std::string_view::npos ? std::string() : std::string(line.substr(t2 + 1)));
  });
}

void TextTable::parse(std::string_view text) {
  header_.clear();
  rows_.clear();
  by_text_.clear();
  by_num_.clear();
  text = strip_bom(text);
  // Excel "Unicode text": TAB separates cells unless quoted; "" is a quote; line breaks in quotes become LF.
  std::vector<std::vector<std::string>> all;
  std::vector<std::string> cells;
  std::string cur;
  bool inq = false;
  size_t length = text.size();
  for (size_t i = 0; i < length; ++i) {
    char ch = text[i];
    if (inq) {
      if (ch == '"') {
        if (i + 1 < length && text[i + 1] == '"') {
          cur.push_back('"');
          ++i;
        } else {
          inq = false;
        }
      } else if (ch == '\r') {
        if (i + 1 < length && text[i + 1] == '\n') ++i;
        cur.push_back('\n');
      } else {
        cur.push_back(ch);
      }
    } else if (ch == '"' && cur.empty()) {
      inq = true;
    } else if (ch == '\t') {
      cells.push_back(std::move(cur));
      cur.clear();
    } else if (ch == '\r' || ch == '\n') {
      if (ch == '\r' && i + 1 < length && text[i + 1] == '\n') ++i;
      cells.push_back(std::move(cur));
      cur.clear();
      if (std::any_of(cells.begin(), cells.end(), [](const std::string& cell) { return !cell.empty(); }))
        all.push_back(std::move(cells));
      cells.clear();
    } else {
      cur.push_back(ch);
    }
  }
  if (!cur.empty() || !cells.empty()) {
    cells.push_back(std::move(cur));
    all.push_back(std::move(cells));
  }
  if (all.empty()) return;
  header_ = std::move(all.front());
  for (size_t row_index = 1; row_index < all.size(); ++row_index) {
    auto& row = all[row_index];
    for (auto& cell : row) {
      replace_all(cell, "{66}", "\xE2\x80\x9C");
      replace_all(cell, "{99}", "\xE2\x80\x9D");
      replace_all(cell, "\\n", "\n");
    }
    u32 idx = static_cast<u32>(rows_.size());
    long long number;
    if (parse_int(row[0], number)) by_num_[static_cast<u64>(number)] = idx;
    by_text_[row[0]] = idx;
    rows_.push_back(std::move(row));
  }
}

int TextTable::column(Lang lang) const {
  std::string want = ascii_lower(lang_code(lang));
  for (size_t i = 1; i < header_.size(); ++i) {
    std::string heading = header_[i];
    while (!heading.empty() && std::isspace(static_cast<unsigned char>(heading.back()))) heading.pop_back();
    if (ascii_lower(heading) == want) return static_cast<int>(i);
  }
  return -1;
}

const std::string* TextTable::get(std::string_view key, Lang lang) const {
  int col = column(lang);
  auto it = by_text_.find(std::string(key));
  if (col < 0 || it == by_text_.end()) return nullptr;
  const auto& row = rows_[it->second];
  return static_cast<size_t>(col) < row.size() ? &row[static_cast<size_t>(col)] : nullptr;
}

const std::string* TextTable::get(u64 key, Lang lang) const {
  int col = column(lang);
  auto it = by_num_.find(key);
  if (col < 0 || it == by_num_.end()) return nullptr;
  const auto& row = rows_[it->second];
  return static_cast<size_t>(col) < row.size() ? &row[static_cast<size_t>(col)] : nullptr;
}

// ---------------------------------------------------------------------------------------------- LangTables
namespace {
void parse_ex(std::string_view text, std::unordered_map<u64, std::vector<std::string>>& out) {
  text = strip_bom(text);
  long long max_room = -1;
  for_each_line(text, [&](std::string_view line) {
    size_t t1 = line.find('\t');
    if (t1 == std::string_view::npos) return;
    size_t t2 = line.find('\t', t1 + 1);
    if (t2 == std::string_view::npos) return;
    long long room, msg;
    if (!parse_int(line.substr(0, t1), room) || !parse_int(line.substr(t1 + 1, t2 - t1 - 1), msg)) return;
    bool ok = room >= max_room;
    max_room = std::max(max_room, room);
    if (!ok) return;
    std::string_view cells = line.substr(t2 + 1);
    std::vector<std::string> variants(50);
    int count = 0;
    size_t pos = 0;
    for (int k = 1; k <= 43; ++k) {
      size_t open = cells.find('{', pos);
      if (open == std::string_view::npos) break;
      size_t cell_end = cells.find('\t', open);
      if (cell_end == std::string_view::npos) cell_end = cells.size();
      variants[static_cast<size_t>(k)] = std::string(cells.substr(open, cell_end - open));
      pos = cell_end;
      ++count;
    }
    if (count == 0) return;
    for (int k = count + 1; k <= 43; ++k) variants[static_cast<size_t>(k)] = variants[static_cast<size_t>(k - 1)];
    for (int k = 44; k <= 48; ++k) variants[static_cast<size_t>(k)] = variants[43];
    out.insert_or_assign(FieldTable::key(static_cast<u32>(room), static_cast<u32>(msg)), std::move(variants));
  });
}

void parse_mesgen(std::string_view text, std::vector<std::pair<std::string, std::vector<std::string>>>& out) {
  out.clear();
  text = strip_bom(text);
  for_each_line(text, [&](std::string_view line) {
    // Empty fields collapse (wcstok semantics).
    std::vector<std::string> toks;
    size_t pos = 0;
    while (pos <= line.size()) {
      size_t comma = line.find(',', pos);
      std::string_view token = line.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
      if (!token.empty()) toks.emplace_back(token);
      if (comma == std::string_view::npos) break;
      pos = comma + 1;
    }
    if (toks.empty()) return;
    std::string key = std::move(toks[0]);
    std::vector<std::string> vals(toks.begin() + 1, toks.end());
    while (vals.size() < 64) vals.push_back(vals.empty() ? std::string() : vals[0]);
    out.emplace_back(std::move(key), std::move(vals));
  });
}
}  // namespace

bool LangTables::read_member(std::string_view name, std::string& out) const {
  if (!vfs_) return false;
  auto raw = vfs_->read("lang/" + std::string(name));
  if (!raw) return false;
  out = decode_text_member(*raw);
  return true;
}

bool LangTables::load_language(Lang lang) {
  PerLang& per_lang = per(lang);
  if (per_lang.loaded) return true;
  std::string txt;
  const std::string code = lang_code(lang);
  if (!read_member(code + ".csv", txt)) return false;
  per_lang.field = FieldTable();
  per_lang.field.parse(txt, true);
  if (lang == Lang::Fr || lang == Lang::It || lang == Lang::De || lang == Lang::Es) {
    if (read_member("Ex_" + code + ".csv", txt)) parse_ex(txt, per_lang.ex);
  }
  if (read_member(lang == Lang::En ? "mesgen_en.csv" : "mesgen.csv", txt)) parse_mesgen(txt, per_lang.mesgen);
  if (read_member("btel_" + code + ".csv", txt)) per_lang.battle.parse(txt, false);
  per_lang.loaded = true;
  return true;
}

bool LangTables::ensure(Lang lang) { return per(lang).loaded || load_language(lang); }

void LangTables::ensure_wend() {
  if (wend_loaded_) return;
  wend_loaded_ = true;
  std::string txt;
  if (read_member("mess_wend.csv", txt)) wend_.parse(txt, true);
}

void LangTables::set_field_csv(Lang lang, std::string_view csv) {
  per(lang).field = FieldTable();
  per(lang).field.parse(csv, true);
  per(lang).loaded = true;
}
void LangTables::set_ex_csv(Lang lang, std::string_view csv) {
  per(lang).ex.clear();
  parse_ex(csv, per(lang).ex);
  per(lang).loaded = true;
}
void LangTables::set_mesgen(Lang lang, std::string_view csv) {
  parse_mesgen(csv, per(lang).mesgen);
  per(lang).loaded = true;
}
void LangTables::set_wend_csv(std::string_view csv) {
  wend_ = FieldTable();
  wend_.parse(csv, true);
  wend_loaded_ = true;
}
void LangTables::set_battle_csv(Lang lang, std::string_view csv) {
  per(lang).battle = FieldTable();
  per(lang).battle.parse(csv, false);
  per(lang).loaded = true;
}

const std::string* LangTables::raw_field(int room_id, int msg, Lang lang) {
  if (!ensure(lang)) return nullptr;
  return per(lang).field.find(static_cast<u32>(room_id), static_cast<u32>(msg));
}

size_t LangTables::field_row_count(Lang lang) { return ensure(lang) ? per(lang).field.rows.size() : 0; }

std::optional<std::string> LangTables::lookup_field(int room_id, int msg, Lang lang, int speaker, LookupInfo* info) {
  if (!ensure(lang)) return std::nullopt;
  PerLang& per_lang = per(lang);
  const std::string* row = per_lang.field.find(static_cast<u32>(room_id), static_cast<u32>(msg));
  if (!row) return std::nullopt;
  std::string base = *row;
  if (lang != Lang::Jp) {
    ensure_wend();
    if (const std::string* wend = wend_.find(static_cast<u32>(room_id), static_cast<u32>(msg))) {
      size_t at = base.find("{wend(");
      if (!wend->empty() && at != std::string::npos) base = base.substr(0, at) + *wend;
    }
  }
  std::string res;
  int sp;
  bool variant = false;
  auto ex = per_lang.ex.find(FieldTable::key(static_cast<u32>(room_id), static_cast<u32>(msg)));
  if (ex != per_lang.ex.end() && !ex->second[1].empty()) {
    sp = detect_speaker(ex->second[1], speaker, lang);
    variant = true;
    res = sp != 0 && sp < 50 ? ex->second[static_cast<size_t>(sp)] : base;
  } else {
    res = base;
    sp = detect_speaker(res, speaker, lang);
  }
  for (size_t entry = 0; entry < per_lang.mesgen.size(); ++entry) {
    const auto& [key, vals] = per_lang.mesgen[entry];
    int replaced = 0;
    while (res.find(key) != std::string::npos && replaced < 16) {
      std::string rep = static_cast<size_t>(sp) < vals.size() ? vals[static_cast<size_t>(sp)] : vals[0];
      if (lang == Lang::En && entry == 26 && sp == 14 && room_id == 0x1D6 && (msg == 0 || msg == 3 || msg == 6)) rep = "ah";
      size_t at = res.find(key);
      res.replace(at, key.size(), rep);
      ++replaced;
    }
  }
  replace_all(res, "\\n", "\n");
  replace_all(res, "{n/a}", "");
  if (lang != Lang::Jp) res = normalise_nonjp(res, lang);
  if (info) {
    info->speaker = sp;
    info->variant = variant;
  }
  return res;
}

Message LangTables::field_message(int room_id, int msg, Lang lang, int speaker) {
  auto markup = lookup_field(room_id, msg, lang, speaker);
  if (!markup) {
    Message message = parse_markup("MESSAGE NOT FOUND{mend}", Lang::Jp);
    message.source = "MESSAGE NOT FOUND";
    return message;
  }
  return parse_markup(*markup, lang);
}

const std::string* LangTables::battle_event(u32 battle_id, u32 msg, Lang lang) {
  if (!ensure(lang)) return nullptr;
  return per(lang).battle.find(battle_id, msg);
}

void LangTables::set_table(std::string_view name, std::string_view utf8) { tables_[std::string(name)].parse(utf8); }

const TextTable* LangTables::table(std::string_view name) {
  std::string key(name);
  auto it = tables_.find(key);
  if (it != tables_.end()) return &it->second;
  if (tables_tried_[key]) return nullptr;
  tables_tried_[key] = true;
  std::string txt;
  if (!read_member(key + ".txt", txt)) return nullptr;
  TextTable& loaded = tables_[key];
  loaded.parse(txt);
  return &loaded;
}

const std::string* LangTables::sysmes(u32 key, Lang lang) {
  const TextTable* sysmes_table = table("sysmes");
  return sysmes_table ? sysmes_table->get(static_cast<u64>(key), lang) : nullptr;
}
const std::string* LangTables::menu_text(u32 addr, Lang lang) {
  const TextTable* menu_table = table("menutest");
  return menu_table ? menu_table->get(static_cast<u64>(addr), lang) : nullptr;
}
const std::string* LangTables::battle_message(u32 id, Lang lang) {
  const TextTable* battle_table = table("btmes");
  return battle_table ? battle_table->get(static_cast<u64>(id), lang) : nullptr;
}

std::u32string LangTables::default_name(int speaker, Lang lang) {
  const std::string* text = sysmes(0x10000u + static_cast<u32>(speaker), lang);
  std::u32string name;
  if (text && !text->empty()) {
    name = utf8_to_u32(*text);
  } else if (const char* tag = speaker_tag(speaker)) {
    for (const char* ch = tag; *ch; ++ch)
      name.push_back(ch == tag ? static_cast<char32_t>(*ch) : static_cast<char32_t>(std::tolower(*ch)));
  }
  if (name.size() > 15) name.resize(15);
  return name;
}

std::u32string LangTables::item_name(int item_id, Lang lang) {
  const std::string* text = sysmes(static_cast<u32>(item_id), lang);
  return text ? utf8_to_u32(*text) : std::u32string();
}

TextContext LangTables::make_context(Lang lang) {
  TextContext context;
  context.speaker_name = [this, lang](int id) { return default_name(id, lang); };
  context.party_name = [this, lang](int slot) { return default_name(slot < 0 ? 0 : slot, lang); };
  context.item_name = [this, lang](int id) { return item_name(id, lang); };
  context.script_name = [this, lang](int var) { return default_name(var, lang); };
  context.script_item = [this, lang](int var) { return item_name(var, lang); };
  return context;
}

}  // namespace oc::import
