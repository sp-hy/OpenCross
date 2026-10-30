#include "platform/steam.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "core/log.h"

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

#ifdef _WIN32
namespace {

constexpr const char* kClientInterface = "SteamClient017";
constexpr const char* kStatsInterface = "STEAMUSERSTATS_INTERFACE_VERSION012";

// Slots in the client object's table of functions.
enum ClientSlot { kCreatePipe = 0, kReleasePipe = 1, kConnectGlobalUser = 2, kReleaseUser = 4, kGenericInterface = 12 };
// Slots in the user stats object's table of functions.
enum StatsSlot {
  kRequestCurrentStats = 0,
  kGetAchievement = 6,
  kSetAchievement = 7,
  kStoreStats = 10,
  kGetDisplayAttribute = 12,
  kGetNumAchievements = 14,
  kGetAchievementName = 15,
};

constexpr std::int32_t kStatsReceived = 1101, kStatsStored = 1102;
constexpr std::int32_t kResultOk = 1;
constexpr auto kRetryDelay = std::chrono::seconds(10);

struct CallbackMessage {
  std::int32_t user;
  std::int32_t id;
  unsigned char* data;
  std::int32_t size;
};

using GetCallbackFn = bool (*)(std::int32_t pipe, CallbackMessage* message, std::int32_t* call);
using FreeCallbackFn = bool (*)(std::int32_t pipe);
using CreateInterfaceFn = void* (*)(const char* version, int* status);

GetCallbackFn get_callback;
FreeCallbackFn free_callback;

template <typename Result, typename... Args>
Result call_slot(void* object, int slot, Args... args) {
  const auto table = *static_cast<Result (***)(void*, Args...)>(object);
  return table[slot](object, args...);
}

// Stats messages start with the game id, then the result.
bool message_is_ok(const CallbackMessage& message) {
  std::uint64_t game = 0;
  std::int32_t result = 0;
  if (message.size < 12) return false;
  std::memcpy(&game, message.data, sizeof(game));
  std::memcpy(&result, message.data + 8, sizeof(result));
  return game == kRdeAppId && result == kResultOk;
}

bool steam_running(std::wstring& client_dll) {
  DWORD pid = 0, size = sizeof(pid);
  constexpr const wchar_t* kKey = L"Software\\Valve\\Steam\\ActiveProcess";
  if (RegGetValueW(HKEY_CURRENT_USER, kKey, L"pid", RRF_RT_REG_DWORD, nullptr, &pid, &size) != ERROR_SUCCESS || pid == 0) return false;
  wchar_t path[MAX_PATH] = {};
  size = sizeof(path);
  if (RegGetValueW(HKEY_CURRENT_USER, kKey, L"SteamClientDll64", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) return false;
  client_dll = path;
  return true;
}

}  // namespace
#endif

SteamClient::~SteamClient() { disconnect(); }

bool SteamClient::connect() {
#ifdef _WIN32
  if (client_) return connected();
  const std::string app_id = std::to_string(kRdeAppId);
  _putenv_s("SteamAppId", app_id.c_str());
  _putenv_s("SteamGameId", app_id.c_str());

  std::wstring client_dll;
  if (!steam_running(client_dll)) {
    log::info("steam: not running");
    return false;
  }
  const HMODULE module = LoadLibraryExW(client_dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!module) {
    log::warn("steam: cannot load the client library ({})", GetLastError());
    return false;
  }
  const auto create_interface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"));
  get_callback = reinterpret_cast<GetCallbackFn>(GetProcAddress(module, "Steam_BGetCallback"));
  free_callback = reinterpret_cast<FreeCallbackFn>(GetProcAddress(module, "Steam_FreeLastCallback"));
  if (!create_interface || !get_callback || !free_callback) {
    log::warn("steam: the client library lacks an expected export");
    return false;
  }
  client_ = create_interface(kClientInterface, nullptr);
  if (!client_) {
    log::warn("steam: {} unavailable", kClientInterface);
    return false;
  }
  pipe_ = call_slot<std::int32_t>(client_, kCreatePipe);
  if (pipe_) user_ = call_slot<std::int32_t>(client_, kConnectGlobalUser, pipe_);
  if (!user_) {
    log::info("steam: no user connected (logged out, or the game is not owned)");
    disconnect();
    return false;
  }
  stats_ = call_slot<void*>(client_, kGenericInterface, user_, pipe_, kStatsInterface);
  if (!stats_) {
    log::warn("steam: {} unavailable", kStatsInterface);
    disconnect();
    return false;
  }
  request_stats();
  log::info("steam: connected as app {}", kRdeAppId);
  return true;
#else
  return false;
#endif
}

void SteamClient::disconnect() {
#ifdef _WIN32
  if (client_ && pipe_) {
    if (user_) call_slot<void>(client_, kReleaseUser, pipe_, user_);
    call_slot<bool>(client_, kReleasePipe, pipe_);
  }
#endif
  client_ = stats_ = nullptr;
  pipe_ = user_ = 0;
  ready_ = store_pending_ = false;
}

void SteamClient::request_stats() {
#ifdef _WIN32
  retry_at_ = std::chrono::steady_clock::now() + kRetryDelay;
  call_slot<bool>(stats_, kRequestCurrentStats);
#endif
}

void SteamClient::pump() {
#ifdef _WIN32
  if (!connected()) return;
  CallbackMessage message{};
  std::int32_t call = 0;
  while (get_callback(pipe_, &message, &call)) {
    if (message.id == kStatsReceived) {
      ready_ = message_is_ok(message);
      log::info("steam: stats {}", ready_ ? "received" : "unavailable");
    } else if (message.id == kStatsStored) {
      store_pending_ = !message_is_ok(message);
      if (store_pending_) log::warn("steam: storing achievements failed; will retry");
      retry_at_ = std::chrono::steady_clock::now() + kRetryDelay;
    }
    free_callback(pipe_);
  }
  if (ready_) flush_unlocks();
  if (std::chrono::steady_clock::now() < retry_at_) return;
  if (!ready_) request_stats();
  else if (store_pending_) {
    retry_at_ = std::chrono::steady_clock::now() + kRetryDelay;
    call_slot<bool>(stats_, kStoreStats);
  }
#endif
}

void SteamClient::flush_unlocks() {
#ifdef _WIN32
  if (pending_.empty()) return;
  bool changed = false;
  for (const std::string& name : pending_) {
    if (achieved(name)) continue;
    if (!call_slot<bool>(stats_, kSetAchievement, name.c_str())) {
      log::warn("steam: {} is not an achievement of this app", name);
      continue;
    }
    log::info("steam: unlocked {}", name);
    unlocked_.push_back(achievement_title(name));
    changed = true;
  }
  pending_.clear();
  if (!changed) return;
  retry_at_ = std::chrono::steady_clock::now() + kRetryDelay;
  store_pending_ = !call_slot<bool>(stats_, kStoreStats);
#endif
}

void SteamClient::unlock(std::string_view api_name) {
  if (!connected()) return;
  if (std::find(pending_.begin(), pending_.end(), api_name) == pending_.end()) pending_.emplace_back(api_name);
}

std::optional<std::string> SteamClient::next_unlocked() {
  if (unlocked_.empty()) return std::nullopt;
  std::string title = std::move(unlocked_.front());
  unlocked_.erase(unlocked_.begin());
  return title;
}

int SteamClient::achievement_count() {
#ifdef _WIN32
  if (ready_) return static_cast<int>(call_slot<std::uint32_t>(stats_, kGetNumAchievements));
#endif
  return 0;
}

std::string SteamClient::achievement_name(int index) {
#ifdef _WIN32
  if (ready_) {
    const char* name = call_slot<const char*>(stats_, kGetAchievementName, static_cast<std::uint32_t>(index));
    if (name) return name;
  }
#else
  (void)index;
#endif
  return {};
}

std::string SteamClient::achievement_title(const std::string& api_name) {
#ifdef _WIN32
  if (ready_) {
    const char* title = call_slot<const char*>(stats_, kGetDisplayAttribute, api_name.c_str(), "name");
    if (title && *title) return title;
  }
#endif
  return api_name;
}

bool SteamClient::achieved(const std::string& api_name) {
  bool unlocked = false;
#ifdef _WIN32
  if (ready_) call_slot<bool>(stats_, kGetAchievement, api_name.c_str(), &unlocked);
#else
  (void)api_name;
#endif
  return unlocked;
}

}  // namespace oc::platform
