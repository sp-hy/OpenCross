#include "platform/paths.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace oc::platform {

namespace fs = std::filesystem;

namespace {
bool is_game_dir(const fs::path& dir) {
  std::error_code ec;
  return !dir.empty() && fs::exists(dir / "data" / "cdrom.dat", ec);
}

bool iequals(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (size_t i = 0; i < lhs.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i]))) return false;
  return true;
}

#ifdef _WIN32
// The logged-in user's 32-bit Steam account id while Steam runs (0 otherwise).
u32 registry_active_user() {
  DWORD value = 0, size = sizeof value;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser", RRF_RT_REG_DWORD, nullptr, &value, &size) !=
      ERROR_SUCCESS)
    return 0;
  return static_cast<u32>(value);
}

fs::path registry_steam_path() {
  wchar_t buf[1024];
  DWORD size = sizeof buf;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
    return {};
  return fs::path(buf);
}
#endif
}  // namespace

fs::path exe_dir() {
  const char* base = SDL_GetBasePath();
  return base ? fs::path(base) : fs::current_path();
}

fs::path exe_path() {
#ifdef _WIN32
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (length == 0) return {};
    if (length < buf.size()) return fs::path(buf.substr(0, length));
    buf.resize(buf.size() * 2);
  }
#else
  return {};
#endif
}

fs::path find_game_dir(std::string_view configured) {
  if (!configured.empty() && is_game_dir(fs::path(configured))) return fs::path(configured);
  fs::path dir = exe_dir();
  for (int i = 0; i < 12; ++i) {
    if (is_game_dir(dir)) return dir;
    fs::path parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  return {};
}

// The file is "users" { "<id64>" { "AccountName" "..." ... "MostRecent" "1" } ... }: only the quoted tokens matter, a 17-digit token
// opens a user block and the MostRecent "1" pair inside it picks the user.
u64 parse_loginusers_most_recent(std::string_view vdf) {
  std::vector<std::string_view> tokens;
  for (size_t i = 0; i < vdf.size(); ++i) {
    if (vdf[i] != '"') continue;
    const size_t end = vdf.find('"', i + 1);
    if (end == std::string_view::npos) break;
    tokens.push_back(vdf.substr(i + 1, end - i - 1));
    i = end;
  }
  u64 current = 0;
  for (size_t i = 0; i < tokens.size(); ++i) {
    const std::string_view token = tokens[i];
    if (token.size() == 17) {
      bool digits = true;
      for (char ch : token) digits = digits && ch >= '0' && ch <= '9';
      if (digits) {
        current = std::strtoull(std::string(token).c_str(), nullptr, 10);
        continue;
      }
    }
    if (current && iequals(token, "MostRecent") && i + 1 < tokens.size() && tokens[i + 1] == "1") return current;
  }
  return 0;
}

u64 current_steam_id64() {
#ifdef _WIN32
  if (const u32 account = registry_active_user()) return kSteamId64Base + account;
  const fs::path steam = registry_steam_path();
  if (!steam.empty()) {
    std::ifstream file(steam / "config" / "loginusers.vdf", std::ios::binary);
    if (file) {
      const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      return parse_loginusers_most_recent(text);
    }
  }
#endif
  return 0;
}

SaveDirPick find_save_dir(std::string_view configured) {
  std::error_code ec;
  if (!configured.empty()) return {fs::path(configured), "[Paths] SaveDir"};

  fs::path docs;
  if (const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS)) docs = documents;
  if (docs.empty()) {
    if (const char* home = std::getenv("USERPROFILE")) docs = fs::path(home) / "Documents";
  }
  const fs::path fallback = exe_dir() / "saves";
  if (docs.empty()) return {fallback, "no Documents folder: saves next to the exe"};
  const fs::path root = docs / "My Games" / "CHRONO CROSS  THE RADICAL DREAMERS EDITION" / "Steam";

  fs::path best, best_dir_only;
  fs::file_time_type best_time{}, best_dir_time{};
  for (const fs::directory_entry& entry : fs::directory_iterator(root, ec)) {
    if (!entry.is_directory(ec)) continue;
    fs::path save = entry.path() / "save.dat";
    if (fs::exists(save, ec)) {
      auto save_time = fs::last_write_time(save, ec);
      if (best.empty() || save_time > best_time) {
        best = entry.path();
        best_time = save_time;
      }
    }
    auto dir_time = entry.last_write_time(ec);
    if (best_dir_only.empty() || dir_time > best_dir_time) {
      best_dir_only = entry.path();
      best_dir_time = dir_time;
    }
  }
  if (!best.empty()) return {best, "newest RDE save.dat"};
  if (const u64 id = current_steam_id64()) return {root / std::to_string(id), "current Steam user (registry / loginusers.vdf)"};
  if (!best_dir_only.empty()) return {best_dir_only, "newest RDE Steam\\<id> folder"};
  return {fallback, "no Steam user found: saves next to the exe"};
}

}  // namespace oc::platform
