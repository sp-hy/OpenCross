#pragma once
// Where things live: the exe, the RDE install (data\cdrom.dat), whether the game runs from the PS1 discs instead, and the save folder.
#include <filesystem>
#include <string>
#include <string_view>

#include "core/types.h"

namespace oc::platform {

std::filesystem::path exe_dir();
// The running exe's own file; empty when unknown.
std::filesystem::path exe_path();
// The user's own folder for OpenCross data that is neither a setting nor a save (caches): %LOCALAPPDATA%\OpenCross, else the exe's
// folder. Not created here.
std::filesystem::path local_data_dir();

// Config value if it is a valid install, else the exe dir, else its parents. Empty path if nothing is found.
std::filesystem::path find_game_dir(std::string_view configured);

// Disc mode: the game data comes from the player's PS1 discs instead of an RDE install (set once at boot). The settings that need RDE's
// data (art mode, music source, language, HD models in the field, portraits) are fixed in it, and the settings window greys them out.
void set_disc_mode(bool on);
bool disc_mode();
// The save folder of disc mode without [Paths] SaveDir: %LOCALAPPDATA%\OpenCross\saves\<region> (local_data_dir), region "jp" or "usa".
std::filesystem::path disc_save_dir(std::string_view region);

// The save folder. RDE uses <Documents>\My Games\CHRONO CROSS  THE RADICAL DREAMERS
// EDITION\Steam\<SteamID64>\ of the logged-in Steam user and creates it at boot. Order here:
//   1. [Paths] SaveDir (created when missing); a relative one is taken from `base` when that is given (the player's own ini: the exe's
//      folder), else from the working folder (--config runs);
//   2. the Steam\<id> folder holding the newest save.dat (never another user's folder when one already has a save);
//   3. the current Steam user without the Steam API: the registry's active account id, else the MostRecent user of
//      <SteamPath>\config\loginusers.vdf -> Steam\<kSteamId64Base + account id>;
//   4. the newest existing Steam\<id> folder;
//   5. a `saves` folder next to opencross.exe.
// `dir` is empty only when nothing at all can be used; `how` names the rule for the log. Folders of rules 1, 3 and 5 may not exist yet
// (the caller creates them before writing save.dat).
struct SaveDirPick {
  std::filesystem::path dir;
  std::string how;
};
SaveDirPick find_save_dir(std::string_view configured, const std::filesystem::path& base = {});

// Steam id64 of the current / most recent Steam user from the registry and loginusers.vdf; 0 = unknown.
u64 current_steam_id64();
// MostRecent "1" user of a loginusers.vdf text (0 = none). Exposed for the unit test.
u64 parse_loginusers_most_recent(std::string_view vdf);
// The PersonaName (the Steam display name) of user `id64` in a loginusers.vdf text; empty when absent. Exposed for the unit test.
std::string parse_loginusers_persona(std::string_view vdf, u64 id64);
// The current / most recent Steam user's display name as Steam last stored it on this machine (no Steam connection); empty = unknown.
std::string current_steam_persona();

// Individual account, public universe: id64 = base + account id.
inline constexpr u64 kSteamId64Base = 76561197960265728ull;

}  // namespace oc::platform
