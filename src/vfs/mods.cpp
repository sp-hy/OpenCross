#include "vfs/mods.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <unordered_map>

#include "core/log.h"
#include "vfs/mod_archive.h"
#include "vfs/vfs.h"

namespace oc::vfs {

namespace {

namespace fs = std::filesystem;

// Shipped archives a mod can replace members of. cdrom2 members are addressed as cdrom/ (the path every caller reads).
constexpr std::array<const char*, 7> kArchives = {"cdrom", "cdrom2", "hd", "lang", "font", "ui", "movie"};

std::string u8(const fs::path& path) {
  const std::u8string text = path.generic_u8string();
  return std::string(text.begin(), text.end());
}

std::string lower(std::string text) {
  for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

std::vector<std::string> split(std::string_view path) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= path.size()) {
    size_t slash = path.find('/', start);
    if (slash == std::string_view::npos) slash = path.size();
    if (slash > start) out.emplace_back(path.substr(start, slash - start));
    start = slash + 1;
  }
  return out;
}

bool is_zip(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  char sig[4] = {};
  in.read(sig, 4);
  return in.gcount() == 4 && sig[0] == 'P' && sig[1] == 'K' && sig[2] == 3 && sig[3] == 4;
}

// Shipped member paths by lower-case file name: `by_name` maps a file name to the component lists of the logical paths carrying it,
// `archives` holds the archive names present in data\ as ZIPs.
struct Index {
  std::unordered_map<std::string, std::vector<std::vector<std::string>>> by_name;
  std::set<std::string> archives;
};

Index build_index(const Vfs& vfs) {
  Index idx;
  std::set<std::string> seen;
  for (const char* arc : kArchives) {
    const std::vector<std::string> names = vfs.list(arc);
    if (!names.empty()) idx.archives.insert(arc);
    const std::string prefix = std::string(arc) == "cdrom2" ? "cdrom" : arc;
    for (const std::string& name : names) {
      std::string logical = prefix + "/" + normalise_path(name);
      const bool is_directory = logical.back() == '/';
      if (is_directory) continue;
      if (!seen.insert(logical).second) continue;
      std::vector<std::string> comps = split(logical);
      idx.by_name[comps.back()].push_back(std::move(comps));
    }
  }
  return idx;
}

std::string join(const std::vector<std::string>& comps) {
  std::string joined;
  for (const std::string& comp : comps) {
    if (!joined.empty()) joined += '/';
    joined += comp;
  }
  return joined;
}

// Resolves one mod file. Returns 0 = unmatched, 1 = matched (target set), 2 = ambiguous.
int resolve(const Index& idx, const std::vector<std::string>& rel, std::string& target) {
  auto it = idx.by_name.find(rel.back());
  if (it == idx.by_name.end()) return 0;
  size_t best = 0, count = 0;
  const std::vector<std::string>* winner = nullptr;
  for (const std::vector<std::string>& cand : it->second) {
    size_t k = 0;
    while (k < rel.size() && k < cand.size() && rel[rel.size() - 1 - k] == cand[cand.size() - 1 - k]) ++k;
    if (k > best) {
      best = k;
      count = 1;
      winner = &cand;
    } else if (k == best) {
      ++count;
    }
  }
  if (!winner) return 0;
  if (count > 1) return 2;
  target = join(*winner);
  return 1;
}

// "[wrapper/]textures/<category>/..." (lower-case components) with a known category -> the canonical texture path.
bool texture_target(const std::vector<std::string>& rel, std::string& target) {
  for (size_t i = 0; i + 2 < rel.size(); ++i) {
    if (rel[i] != "textures") continue;
    const bool known =
        std::any_of(std::begin(kTextureCategories), std::end(kTextureCategories), [&](const char* category) { return rel[i + 1] == category; });
    if (!known) continue;
    target.clear();
    for (size_t k = i + 1; k < rel.size(); ++k) target += (k > i + 1 ? "/" : "") + rel[k];
    return true;
  }
  return false;
}

// A resolved game file goes with the files that follow "load on" or with the mod's game data.
void add_matched(Mod& mod, ModFile mod_file) {
  (is_game_data(mod_file.target) ? mod.data : mod.files).push_back(std::move(mod_file));
}

void log_list(const char* what, const std::vector<std::string>& list) {
  constexpr size_t kMax = 12;
  for (size_t i = 0; i < list.size() && i < kMax; ++i) log::info("mods:     {}: {}", what, list[i]);
  if (list.size() > kMax) log::info("mods:     ... {} more {}", list.size() - kMax, what);
}

bool is_packed(const std::string& lowered_name) {
  return lowered_name.ends_with(".zip") || lowered_name.ends_with(".7z") || lowered_name.ends_with(".rar");
}

// Length of the leading folders every member shares: an archive made from a folder starts inside it, where a mod folder would.
// "voices" and "textures" are mod layout, not wrappers.
size_t wrapper_length(const std::vector<std::string>& names) {
  if (names.empty()) return 0;
  size_t length = 0;
  while (true) {
    const size_t slash = names[0].find('/', length);
    if (slash == std::string::npos) return length;
    const std::string_view folder = std::string_view(names[0]).substr(length, slash - length);
    if (folder == "voices" || folder == "textures") return length;
    const std::string_view prefix = std::string_view(names[0]).substr(0, slash + 1);
    if (!std::all_of(names.begin(), names.end(), [&](const std::string& name) { return name.starts_with(prefix); })) return length;
    length = slash + 1;
  }
}

// A mod's <archive>.dat: every member is resolved as if it were a loose file at <archive>/<member>, so an exact path wins and a member
// the game does not have stays unmatched.
void add_archive(Mod& mod, const Vfs& vfs, const Index& idx, ModFile dat, const std::string& arc) {
  const std::string prefix = arc == "cdrom2" ? "cdrom" : arc;
  std::string error;
  const std::shared_ptr<ModArchive> packed = vfs.mod_archive(dat.file, &error);
  if (!packed) log::warn("mods: cannot read {}: {}", dat.rel, error);
  for (const std::string& member : packed ? packed->names() : std::vector<std::string>{}) {
    ModFile mod_file;
    mod_file.file = dat.file;
    mod_file.member = member;
    mod_file.rel = dat.rel + "/" + member;
    switch (resolve(idx, split(prefix + "/" + member), mod_file.target)) {
      case 1: add_matched(mod, std::move(mod_file)); break;
      case 2: mod.ambiguous.push_back(mod_file.rel); break;
      default: mod.unmatched.push_back(mod_file.rel); break;
    }
  }
  dat.target = arc;
  mod.archives.push_back(std::move(dat));
}

// Classifies one file of a mod (`rel` set, plus `member` inside an archive); `voice_folder` = the mod is called "voices". An <archive>.dat
// inside an archive stays unmatched: a ZIP cannot be read from within another archive without unpacking it.
void add_file(Mod& mod, const Vfs& vfs, const Index& idx, ModFile mod_file, bool voice_folder) {
  const std::vector<std::string> rel = split(lower(mod_file.rel));
  if (rel.empty()) return;
  const std::string& fname = rel.back();
  if (fname.size() > 4 && fname.ends_with(".dat")) {
    const std::string arc = fname.substr(0, fname.size() - 4);
    if (mod_file.member.empty() && idx.archives.contains(arc) && is_zip(mod_file.file)) {
      add_archive(mod, vfs, idx, std::move(mod_file), arc);
      return;
    }
  }
  if (fname.size() > 4 && fname.ends_with(".png")) {
    std::string tex;
    if (texture_target(rel, tex)) {
      mod_file.target = tex;
      mod.textures.push_back(std::move(mod_file));
      return;
    }
  }
  if (fname.size() > 4 && (fname.ends_with(".mp4") || fname.ends_with(".mkv")) &&
      idx.by_name.contains(fname.substr(0, fname.size() - 4) + ".bk2")) {
    mod_file.target = "movie/" + fname;
    mod.files.push_back(std::move(mod_file));
    return;
  }
  if (fname.size() > 4 && fname.ends_with(".mp3") && ((rel.size() > 1 && rel[0] == "voices") || voice_folder)) {
    mod_file.target = "voices/" + fname;
    mod.voices.push_back(std::move(mod_file));
    return;
  }
  switch (resolve(idx, rel, mod_file.target)) {
    case 1: add_matched(mod, std::move(mod_file)); break;
    case 2: mod.ambiguous.push_back(mod_file.rel); break;
    default: mod.unmatched.push_back(mod_file.rel); break;
  }
}

}  // namespace

bool is_game_data(std::string_view target) {
  constexpr std::array<std::string_view, 5> kFolders = {"lang/", "cdrom/battle/data/", "cdrom/battle/event/", "cdrom/battle/mes/", "cdrom/prog/"};
  constexpr std::array<std::string_view, 7> kFiles = {"cdrom/menu/el_shop.bin",       "cdrom/menu/waa_shop.bin", "cdrom/menu/trade.bin",
                                                      "cdrom/menu/waa_tbl.bin",       "cdrom/menu/keyitem.dat",  "cdrom/sound/effect/common/effect00.dat",
                                                      "cdrom/sound/wave/wave0000.dat"};
  const auto within = [&](std::string_view folder) { return target.starts_with(folder); };
  return std::any_of(kFolders.begin(), kFolders.end(), within) || std::find(kFiles.begin(), kFiles.end(), target) != kFiles.end();
}

const char* load_on_name(ModLoadOn load_on) {
  switch (load_on) {
    case ModLoadOn::Modded: return "modded";
    case ModLoadOn::Sd: return "sd";
    case ModLoadOn::Hd: return "hd";
    case ModLoadOn::All: return "all";
  }
  return "modded";
}

ModLoadOn parse_load_on(std::string_view name) {
  const std::string lowered = lower(std::string(name));
  if (lowered == "sd" || lowered == "original") return ModLoadOn::Sd;
  if (lowered == "hd") return ModLoadOn::Hd;
  if (lowered == "all") return ModLoadOn::All;
  return ModLoadOn::Modded;
}

bool loads_on(ModLoadOn load_on, ModArt art) {
  switch (load_on) {
    case ModLoadOn::Modded: return art == ModArt::Modded;
    case ModLoadOn::Sd: return art == ModArt::Sd;
    case ModLoadOn::Hd: return art == ModArt::Hd;
    case ModLoadOn::All: return true;
  }
  return art == ModArt::Modded;
}

// Mods are collected in name order, then reordered by the saved priority. Each file is classified in turn (add_file): a <archive>.dat
// that is a ZIP = its members are resolved one by one (add_archive); textures\<category>\...png = a dumped texture's replacement; an .mp3 under voices\ (or
// anywhere in a mod called "voices") = a voice line (its "voices/<name>" target is not a VFS target, kept for the count / display);
// an .mp4 / .mkv named after a shipped movie (a01op_de.mp4 for a01op_de.bk2) = its replacement, kept as "movie/<name>.mp4|.mkv";
// everything else is resolved against the shipped member index.
void ModManager::scan(const Vfs& vfs, const fs::path& mods_dir, const std::vector<ModPref>& prefs) {
  dir_ = mods_dir;
  mods_.clear();
  packed_voices_.clear();
  std::error_code ec;
  if (mods_dir.empty() || !fs::is_directory(mods_dir, ec)) {
    log::info("mods: no mods folder at {}", u8(mods_dir));
    return;
  }

  std::vector<Mod> found;
  for (const fs::directory_entry& entry : fs::directory_iterator(mods_dir, ec)) {
    Mod mod;
    mod.name = u8(entry.path().filename());
    const std::string lowered = lower(mod.name);
    if (entry.is_directory(ec)) mod.root = entry.path();
    else if (is_packed(lowered) || lowered.ends_with(".dat")) mod.source = entry.path();
    else continue;
    found.push_back(std::move(mod));
  }
  std::sort(found.begin(), found.end(), [](const Mod& lhs, const Mod& rhs) { return lower(lhs.name) < lower(rhs.name); });
  for (const ModPref& pref : prefs) {
    auto it = std::find_if(found.begin(), found.end(), [&](const Mod& mod) { return mod.name == pref.name; });
    if (it == found.end()) continue;
    it->enabled = pref.enabled;
    it->load_on = pref.load_on;
    mods_.push_back(std::move(*it));
    found.erase(it);
  }
  for (Mod& mod : found) mods_.push_back(std::move(mod));
  if (mods_.empty()) {
    log::info("mods: {} has no mods", u8(mods_dir));
    return;
  }

  const Index idx = build_index(vfs);
  for (Mod& mod : mods_) {
    if (!mod.root.empty()) {
      std::vector<fs::path> files;
      for (auto it = fs::recursive_directory_iterator(mod.root, fs::directory_options::skip_permission_denied, ec);
           it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) files.push_back(it->path());
      }
      std::sort(files.begin(), files.end());
      const bool voice_folder = lower(mod.name) == "voices";
      for (const fs::path& file : files) add_file(mod, vfs, idx, {{}, file, {}, u8(file.lexically_relative(mod.root))}, voice_folder);
    } else if (is_packed(lower(mod.name))) {
      const bool voice_folder = lower(u8(mod.source.stem())) == "voices";
      const std::shared_ptr<ModArchive> packed = vfs.mod_archive(mod.source, &mod.error);
      mod.may_be_slow = packed && packed->may_be_slow();
      const size_t wrapper = packed && !voice_folder ? wrapper_length(packed->names()) : 0;
      for (const std::string& member : packed ? packed->names() : std::vector<std::string>{})
        add_file(mod, vfs, idx, {{}, mod.source, member, member.substr(wrapper)}, voice_folder);
      for (const ModFile& voice : mod.voices) {
        const fs::path rel = fs::path(std::u8string(voice.rel.begin(), voice.rel.end()));
        const fs::path path = voice.rel.starts_with("voices/") ? mod.source / rel : mod.source / "voices" / rel;
        packed_voices_.emplace(lower(u8(path)), OverlayFile{voice.file, voice.member});
      }
    } else {
      add_file(mod, vfs, idx, {{}, mod.source, {}, mod.name}, false);
    }
    log::info("mods: '{}' ({}, load on {}): {} matched, {} game data, {} archive(s), {} texture(s), {} voice(s), {} unmatched, {} ambiguous{}{}", mod.name,
              mod.enabled ? "enabled" : "disabled", load_on_name(mod.load_on), mod.files.size(), mod.data.size(), mod.archives.size(), mod.textures.size(),
              mod.voices.size(), mod.unmatched.size(), mod.ambiguous.size(), mod.error.empty() ? "" : ", cannot read: ", mod.error);
    for (const ModFile& archive : mod.archives) log::info("mods:     archive: {} overlays data\\{}.dat", archive.rel, archive.target);
    log_list("unmatched", mod.unmatched);
    log_list("ambiguous", mod.ambiguous);
  }
}

std::optional<OverlayFile> ModManager::packed_voice(const fs::path& path) const {
  auto it = packed_voices_.find(lower(u8(path)));
  if (it == packed_voices_.end()) return std::nullopt;
  return it->second;
}

std::vector<ModPref> ModManager::prefs() const {
  std::vector<ModPref> out;
  out.reserve(mods_.size());
  for (const Mod& mod : mods_) out.push_back({mod.name, mod.enabled, mod.load_on});
  return out;
}

void ModManager::apply(Vfs& vfs, ModArt art) {
  if (!boot_data_) {
    boot_data_.emplace();
    for (const Mod& mod : mods_)
      if (mod.enabled)
        for (const ModFile& file : mod.data) boot_data_->try_emplace(file.target, OverlayFile{file.file, file.member});
  }
  Overlay ov;
  ov.enhance = art == ModArt::Modded;
  ov.files = *boot_data_;
  size_t active = 0;
  for (const Mod& mod : mods_) {
    if (!mod.enabled || !loads_on(mod.load_on, art)) continue;
    ++active;
    for (const ModFile& file : mod.files) ov.files.try_emplace(file.target, OverlayFile{file.file, file.member});
    for (const ModFile& texture : mod.textures) ov.textures.try_emplace(texture.target, OverlayFile{texture.file, texture.member});
  }
  log::info("mods: {} active mod(s), {} file(s), {} texture(s)", active, ov.files.size(), ov.textures.size());
  vfs.set_overlay(std::move(ov));
}

std::vector<ModManager::Conflict> ModManager::conflicts(size_t index) const {
  std::vector<Conflict> out;
  if (index >= mods_.size() || !mods_[index].enabled) return out;
  const Mod& me = mods_[index];
  constexpr std::array<ModArt, 3> kArts = {ModArt::Sd, ModArt::Hd, ModArt::Modded};
  const auto shares_mode = [&](const Mod& other) {
    for (ModArt art : kArts)
      if (loads_on(me.load_on, art) && loads_on(other.load_on, art)) return true;
    return false;
  };
  const auto provides = [](const Mod& mod, const ModFile& file) {
    const auto same = [&](const ModFile& candidate) { return candidate.target == file.target; };
    return std::any_of(mod.files.begin(), mod.files.end(), same) || std::any_of(mod.textures.begin(), mod.textures.end(), same) ||
           std::any_of(mod.data.begin(), mod.data.end(), same);
  };
  const auto check = [&](const ModFile& file, bool any_mode = false) {
    bool clash = false;
    size_t winner = index;
    for (size_t j = 0; j < mods_.size(); ++j) {
      if (j == index || !mods_[j].enabled || !(any_mode || shares_mode(mods_[j])) || !provides(mods_[j], file)) continue;
      clash = true;
      if (j < winner) winner = j;
    }
    if (clash) out.push_back({file.target, winner});
  };
  for (const ModFile& file : me.files) check(file);
  for (const ModFile& texture : me.textures) check(texture);
  for (const ModFile& data : me.data) check(data, true);
  return out;
}

}  // namespace oc::vfs
