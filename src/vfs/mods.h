#pragma once
// Mods: folders of loose replacement files in <GameDir>\mods\<mod name>\..., resolved once against the shipped archives and
// installed into the VFS as an Overlay for the current art mode. A .zip / .7z / .rar in <GameDir>\mods is a mod too, read live with
// nothing unpacked (vfs/mod_archive.h): its files resolve as those of a folder would. A bare .dat there is a mod of that one file.
//
// Resolution of one mod file (relative path inside the mod folder, compared component-wise, case-insensitive):
//   1. file name "<archive>.dat" of a shipped data\<archive>.dat that is a ZIP -> each of its members is resolved by rule 2 as the path
//      <archive>/<member> and read from that ZIP; what it does not hold still comes from the shipped archive;
//   2. otherwise every shipped member with the same file name is a candidate (cdrom2 members count as cdrom/, the path the game
//      reads); the candidates sharing the longest trailing run of path components with the mod path win, so leading wrapper
//      folders are ignored and a bare file name falls back to a unique name match; one winner = matched, several = ambiguous
//      (not loaded), none = unmatched (not loaded).
// Priority: the mod list order; the first enabled mod that loads on the current art mode wins a target.
//
// Game data (is_game_data: text, battle tables and scripts, shop tables, program files, the base sound banks) is not art: "load on" does not
// apply to it, and the game reads much of it once at boot, so the overlay keeps what the first apply() installed until the next launch.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vfs/vfs.h"

namespace oc::vfs {

// "Load on" setting of a mod: in which art modes its files are active.
enum class ModLoadOn : std::uint8_t { Modded, Sd, Hd, All };
// The art mode the overlay is built for (Sd = original / classic art, Modded = HD art + mods).
enum class ModArt : std::uint8_t { Sd, Hd, Modded };

// The "load on" names "modded", "sd", "hd", "all"; an unknown name parses as Modded.
const char* load_on_name(ModLoadOn load_on);
ModLoadOn parse_load_on(std::string_view name);
bool loads_on(ModLoadOn load_on, ModArt art);

// A logical path that is game data rather than art.
bool is_game_data(std::string_view target);

// First components of a texture replacement path (render/texture_registry.h): a mod file <mod>\[wrapper\]textures\<category>\... is a
// texture replacement, not an archive member.
inline constexpr const char* kTextureCategories[] = {"battle_stage", "title", "effects", "effect_mesh", "room_mesh", "room_model", "model", "tiles"};

// One mod file: `target` = the logical VFS path ("hd/face/2021/00_seljyu.png") or, in Mod::archives, the archive name ("movie"); `file` =
// the file on disk; `member` = the normalised member of that file for a packed mod or a mod's <archive>.dat; `rel` = its path inside
// the mod (UTF-8, '/'), for display / logs.
struct ModFile {
  std::string target;
  std::filesystem::path file;
  std::string member;
  std::string rel;
};

// One mod. `name` is the folder or file name (UTF-8): the name shown in the Mods tab and the [Mods] ini key. `root` is the mod folder
// (empty for a file). `source` is the archive or bare .dat in the mods folder (empty for a folder). `error` = why an archive cannot be
// read, `may_be_slow` = a packed mod better extracted (ModArchive::may_be_slow). `files` are the matched
// member replacements that follow "load on", `data` the matched game data, `archives` the <archive>.dat files (their members are in
// `files` / `data`), `textures` the texture replacements (target = canonical texture
// path), `voices` the voice lines (<mod>\voices\**\*.mp3, or any .mp3 when the folder itself is called "voices"; played by
// audio::VoicePlayer, not VFS targets). `unmatched` / `ambiguous` list the rel paths that match no shipped member / whose best match is
// not unique.
struct Mod {
  std::string name;
  std::filesystem::path root;
  std::filesystem::path source;
  std::string error;
  bool may_be_slow = false;
  bool enabled = true;
  ModLoadOn load_on = ModLoadOn::Modded;
  std::vector<ModFile> files;
  std::vector<ModFile> data;
  std::vector<ModFile> archives;
  std::vector<ModFile> textures;
  std::vector<ModFile> voices;
  std::vector<std::string> unmatched;
  std::vector<std::string> ambiguous;
};

// Persisted per-mod settings, in priority order.
struct ModPref {
  std::string name;
  bool enabled = true;
  ModLoadOn load_on = ModLoadOn::Modded;
};

class ModManager {
 public:
  // Scans <mods_dir>\<name>\** and resolves every file against vfs.list(). Order and settings come from `prefs` (by name);
  // mods not in `prefs` are appended in name order with the defaults; prefs whose mod is gone are dropped. Logs each mod's
  // matched / unmatched / ambiguous counts.
  void scan(const Vfs& vfs, const std::filesystem::path& mods_dir, const std::vector<ModPref>& prefs);
  std::vector<ModPref> prefs() const;

  // The voice line at `path` = <archive>\voices\<rel> inside a scanned archive, if there is one (any case).
  std::optional<OverlayFile> packed_voice(const std::filesystem::path& path) const;

  std::vector<Mod>& mods() { return mods_; }
  const std::vector<Mod>& mods() const { return mods_; }
  const std::filesystem::path& dir() const { return dir_; }

  // Builds the overlay of the enabled mods that load on `art` (priority order, first wins) and installs it into `vfs`. The game data is
  // that of the first call, whatever changes later.
  void apply(Vfs& vfs, ModArt art);

  // Targets of mod `index` that another enabled mod also provides in a shared art mode, with the winning mod (may be `index` itself).
  struct Conflict {
    std::string target;
    size_t winner = 0;
  };
  std::vector<Conflict> conflicts(size_t index) const;

 private:
  std::filesystem::path dir_;
  std::vector<Mod> mods_;
  std::unordered_map<std::string, OverlayFile> packed_voices_;
  std::optional<std::unordered_map<std::string, OverlayFile>> boot_data_;
};

}  // namespace oc::vfs
