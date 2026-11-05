#include "data/char_name.h"

namespace oc::data {

bool usable_name(const std::u32string& name, import::Lang lang) {
  if (name.empty() || name.find(U'�') != std::u32string::npos || name.find_first_not_of(U"　 ") == std::u32string::npos) return false;
  if (lang != import::Lang::Jp)
    for (char32_t ch : name)
      if (ch >= 0x3000) return false;
  return true;
}

bool shows_lynx(int char_id, int body, NameContext context, bool keep_custom) {
  if (char_id != 0 || body != kLynxBody) return false;
  return context == NameContext::Menu || !keep_custom;
}

std::u32string own_name(int char_id, const CharNames& names, bool title) {
  std::u32string name;
  if (names.custom)
    if (const auto it = names.custom->find(char_id); it != names.custom->end() && usable_name(it->second, names.lang)) name = it->second;
  if (name.empty() && names.tables) name = names.tables->default_name(char_id, names.lang);
  if (title && names.lang == import::Lang::Jp && char_id == 5) name += U"大佐";
  return name;
}

std::u32string character_name(int char_id, int body, NameContext context, bool keep_custom, const CharNames& names, bool title) {
  if (shows_lynx(char_id, body, context, keep_custom) && names.tables) return names.tables->default_name(kLynxBody, names.lang);
  return own_name(char_id, names, title);
}

bool is_default_name(int char_id, const std::u32string& name, const CharNames& names) {
  return names.tables && name == names.tables->default_name(char_id, names.lang);
}

std::u32string learned_tech_name(const CharNames& names) {
  if (!names.tables || (names.custom && names.custom->contains(kMojo))) return {};
  const import::TextTable* rows = names.tables->table("cmnmes");
  const std::string* text = rows ? rows->get(std::string_view("ハッピーダン"), names.lang) : nullptr;
  return text ? import::utf8_to_u32(*text) : std::u32string();
}

const char* unnamed_kid_label(import::Lang lang) {
  switch (lang) {
    case import::Lang::Fr: return "???";
    case import::Lang::It: return "Ragazza";
    case import::Lang::De: return "Mädchen";
    case import::Lang::Es: return "Chica";
    case import::Lang::Jp: return "旅の娘";
    default: return "Girl";
  }
}

}  // namespace oc::data
