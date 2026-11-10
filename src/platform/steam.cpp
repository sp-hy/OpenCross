#include "platform/steam.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

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
constexpr const char* kFriendsInterface = "SteamFriends017";

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
// Slots in the friends object's table of functions.
enum FriendsSlot { kGetPersonaName = 0 };

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

class ClientBackend final : public SteamBackend {
public:
  ~ClientBackend() override {
    if (client_ && pipe_) {
      if (user_) call_slot<void>(client_, kReleaseUser, pipe_, user_);
      call_slot<bool>(client_, kReleasePipe, pipe_);
    }
  }

  bool open(const std::wstring& client_dll) {
    const HMODULE module = LoadLibraryExW(client_dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
      log::warn("steam: cannot load the client library ({})", GetLastError());
      return false;
    }
    const auto create_interface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"));
    get_callback_ = reinterpret_cast<GetCallbackFn>(GetProcAddress(module, "Steam_BGetCallback"));
    free_callback_ = reinterpret_cast<FreeCallbackFn>(GetProcAddress(module, "Steam_FreeLastCallback"));
    if (!create_interface || !get_callback_ || !free_callback_) {
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
      return false;
    }
    stats_ = call_slot<void*>(client_, kGenericInterface, user_, pipe_, kStatsInterface);
    if (!stats_) {
      log::warn("steam: {} unavailable", kStatsInterface);
      return false;
    }
    friends_ = call_slot<void*>(client_, kGenericInterface, user_, pipe_, kFriendsInterface);
    if (!friends_) log::warn("steam: {} unavailable", kFriendsInterface);
    return true;
  }

  bool poll(StatsEvent& event) override {
    CallbackMessage message{};
    std::int32_t call = 0;
    while (get_callback_(pipe_, &message, &call)) {
      const bool received = message.id == kStatsReceived;
      const bool stored = message.id == kStatsStored;
      if (received || stored) event = {received, message_is_ok(message)};
      free_callback_(pipe_);
      if (received || stored) return true;
    }
    return false;
  }

  void request_stats() override { call_slot<bool>(stats_, kRequestCurrentStats); }
  bool store_stats() override { return call_slot<bool>(stats_, kStoreStats); }
  bool set_achievement(const std::string& api_name) override { return call_slot<bool>(stats_, kSetAchievement, api_name.c_str()); }

  bool achieved(const std::string& api_name) override {
    bool unlocked = false;
    call_slot<bool>(stats_, kGetAchievement, api_name.c_str(), &unlocked);
    return unlocked;
  }

  int achievement_count() override { return static_cast<int>(call_slot<std::uint32_t>(stats_, kGetNumAchievements)); }

  std::string achievement_name(int index) override {
    const char* name = call_slot<const char*>(stats_, kGetAchievementName, static_cast<std::uint32_t>(index));
    return name ? name : "";
  }

  std::string achievement_title(const std::string& api_name) override {
    const char* title = call_slot<const char*>(stats_, kGetDisplayAttribute, api_name.c_str(), "name");
    return title ? title : "";
  }

  std::string persona_name() override {
    const char* name = friends_ ? call_slot<const char*>(friends_, kGetPersonaName) : nullptr;
    return name ? name : "";
  }

private:
  GetCallbackFn get_callback_ = nullptr;
  FreeCallbackFn free_callback_ = nullptr;
  void* client_ = nullptr;
  void* stats_ = nullptr;
  void* friends_ = nullptr;
  std::int32_t pipe_ = 0;
  std::int32_t user_ = 0;
};

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
  if (backend_) return true;
  const std::string app_id = std::to_string(kRdeAppId);
  _putenv_s("SteamAppId", app_id.c_str());
  _putenv_s("SteamGameId", app_id.c_str());

  std::wstring client_dll;
  if (!steam_running(client_dll)) {
    log::info("steam: not running");
    return false;
  }
  auto backend = std::make_unique<ClientBackend>();
  if (!backend->open(client_dll)) return false;
  attach(std::move(backend));
  log::info("steam: connected as app {}", kRdeAppId);
  return true;
#else
  return false;
#endif
}

void SteamClient::attach(std::unique_ptr<SteamBackend> backend) {
  backend_ = std::move(backend);
  request_stats();
}

void SteamClient::disconnect() {
  backend_.reset();
  ready_ = store_pending_ = false;
}

void SteamClient::request_stats() {
  retry_at_ = std::chrono::steady_clock::now() + kRetryDelay;
  backend_->request_stats();
}

void SteamClient::pump() {
  if (!connected()) return;
  StatsEvent event{};
  while (backend_->poll(event)) {
    if (event.received) {
      ready_ = event.ok;
      log::info("steam: stats {}", ready_ ? "received" : "unavailable");
    } else {
      store_pending_ = !event.ok;
      if (store_pending_) log::warn("steam: storing achievements failed; will retry");
      retry_at_ = std::chrono::steady_clock::now() + kRetryDelay;
    }
  }
  if (ready_) flush_unlocks();
  if (std::chrono::steady_clock::now() < retry_at_) return;
  if (!ready_) request_stats();
  else if (store_pending_) {
    retry_at_ = std::chrono::steady_clock::now() + kRetryDelay;
    backend_->store_stats();
  }
}

void SteamClient::flush_unlocks() {
  if (pending_.empty()) return;
  bool changed = false;
  const auto award = [&](const std::string& name) {
    if (backend_->achieved(name)) return;
    if (!backend_->set_achievement(name)) {
      log::warn("steam: {} is not an achievement of this app", name);
      return;
    }
    log::info("steam: unlocked {}", name);
    unlocked_.push_back(achievement_title(name));
    changed = true;
  };
  for (const std::string& name : pending_) award(name);
  pending_.clear();
  if (changed && !meta_.empty() && std::all_of(meta_required_.begin(), meta_required_.end(), [&](const std::string& name) { return backend_->achieved(name); }))
    award(meta_);
  if (!changed) return;
  retry_at_ = std::chrono::steady_clock::now() + kRetryDelay;
  store_pending_ = !backend_->store_stats();
}

void SteamClient::unlock(std::string_view api_name) {
  if (!connected()) return;
  if (std::find(pending_.begin(), pending_.end(), api_name) == pending_.end()) pending_.emplace_back(api_name);
}

void SteamClient::set_meta(std::string meta, std::vector<std::string> required) {
  meta_ = std::move(meta);
  meta_required_ = std::move(required);
}

std::optional<std::string> SteamClient::next_unlocked() {
  if (unlocked_.empty()) return std::nullopt;
  std::string title = std::move(unlocked_.front());
  unlocked_.erase(unlocked_.begin());
  return title;
}

std::string SteamClient::persona_name() { return backend_ ? backend_->persona_name() : std::string(); }

int SteamClient::achievement_count() { return ready_ ? backend_->achievement_count() : 0; }

std::string SteamClient::achievement_name(int index) { return ready_ ? backend_->achievement_name(index) : std::string(); }

std::string SteamClient::achievement_title(const std::string& api_name) {
  if (ready_) {
    std::string title = backend_->achievement_title(api_name);
    if (!title.empty()) return title;
  }
  return api_name;
}

bool SteamClient::achieved(const std::string& api_name) { return ready_ && backend_->achieved(api_name); }

}  // namespace oc::platform
