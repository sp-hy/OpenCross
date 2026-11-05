#pragma once
// The naming rule of the playable characters, one function for every screen. Serge (id 0) in Lynx's body reads as Lynx in battle and in the
// menus whatever his typed name; in field text he does too, unless a script set global flag 16. A name token that names him directly and the
// Dopple list use his own name, and every other character keeps its own name whatever its body.
#include <map>
#include <string>

#include "import/text.h"

namespace oc::data {

constexpr int kLynxBody = 0x31;
constexpr int kLastNamedChar = 0x2B;

// Menu = the battle and the menu screens, Field = field text.
enum class NameContext { Menu, Field };

// Where names come from. `custom` (optional) holds the typed names by character id.
struct CharNames {
  const std::map<int, std::u32string>* custom = nullptr;
  import::LangTables* tables = nullptr;
  import::Lang lang = import::Lang::En;
};

// A typed name the language can show: a save record of a never-renamed character holds the JP default in kana, which the non-JP fonts cannot
// draw, so it counts as unset there; blank and undecodable names are unset everywhere.
bool usable_name(const std::u32string& name, import::Lang lang);

// The body a record's model byte stands for: bytes 0x3C..0x3E are the party override slots, whose model is in script variables 0x170..0x174.
template <typename ReadVar>
int character_body(int model, ReadVar&& read_var) {
  const int body = model & 0xFF;
  return body >= 0x3C && body <= 0x3E ? read_var(0x170 + 2 * (body - 0x3C)) & 0xFF : body;
}

// What a body picks on the menu screens: bodies up to `last` have their own art, any other falls back to the character's. The portrait strip,
// the weapon pack and the profile text stop at kLastFormBody.
constexpr int kLastFormBody = kLynxBody;
constexpr int menu_form(int char_id, int body, int last = kLastFormBody) { return body <= last ? body : char_id; }

// `keep_custom` = global flag 16, which only field text reads.
bool shows_lynx(int char_id, int body, NameContext context, bool keep_custom);

// The typed name if usable, else the language's name of the id. `title` adds Viper's rank in JP.
std::u32string own_name(int char_id, const CharNames& names, bool title = true);
// The name a screen shows for a character: Lynx's name when shows_lynx holds, else own_name.
std::u32string character_name(int char_id, int body, NameContext context, bool keep_custom, const CharNames& names, bool title = true);

// A confirmed name equal to the language's name of the id stores no custom name (the character follows a language switch).
bool is_default_name(int char_id, const std::u32string& name, const CharNames& names);

// Field op FE 7E, Mojo learning his tech: a Mojo without a custom name gets the language's cmnmes row keyed ハッピーダン ("Mojoy" in
// English) as one. Empty when he has a custom name already, whatever it is, or the row is missing.
constexpr int kMojo = 0x1B;
std::u32string learned_tech_name(const CharNames& names);

// Kid's name on every battle screen of scene 0x186, the fight before the party learns who she is; it replaces a typed name too.
constexpr int kUnnamedKidScene = 0x186;
constexpr int kKid = 1;
const char* unnamed_kid_label(import::Lang lang);

// The speaker that picks a field line's generated words: a line without a speaker (0) speaks as Lynx while a party member wears his body.
constexpr int line_speaker(int speaker, bool lynx_in_party) { return speaker == 0 && lynx_in_party ? kLynxBody : speaker; }

}  // namespace oc::data
