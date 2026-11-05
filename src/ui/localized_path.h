#pragma once
// Language-suffixed image lookup shared by every screen that loads HD art: "<stem>_<lang><ext>" first, the plain name when the
// language has no variant. The Vfs resolves both through the mod overlay, so a mod can supply either.
#include <string>
#include <string_view>

#include "import/text.h"
#include "vfs/vfs.h"

namespace oc::ui {

inline const char* lang_suffix(import::Lang lang) {
  static const char* const kSuffix[6] = {"_en", "_fr", "_it", "_de", "_es", "_jp"};
  return kSuffix[static_cast<int>(lang)];
}

inline std::string localized_path(const vfs::Vfs& vfs, std::string_view path, import::Lang lang) {
  const size_t dot = path.rfind('.');
  if (dot == std::string_view::npos) return std::string(path);
  std::string localized(path.substr(0, dot));
  localized += lang_suffix(lang);
  localized += path.substr(dot);
  return vfs.exists(localized) ? localized : std::string(path);
}

}  // namespace oc::ui
