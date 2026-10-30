#pragma once
// Steam presence and achievements for the Radical Dreamers Edition's AppID, from the player's own Steam client and no Valve code: nothing
// from the Steamworks SDK is built in or shipped. connect() sets the process's app id, loads the running client's steamclient64.dll and
// talks to it through the few interface slots declared in steam.cpp. That connection is what shows "Playing" and gives access to the
// app's achievements; ownership is Steam's own check. With Steam missing, closed, logged out or the game not owned, everything here does
// nothing and the game runs on.
//
// Achievements are stored once Steam has delivered the player's stats; unlock() queues until then, and a failed store retries.
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oc::platform {

inline constexpr std::uint64_t kRdeAppId = 1133760;

class SteamClient {
public:
  SteamClient() = default;
  ~SteamClient();
  SteamClient(const SteamClient&) = delete;
  SteamClient& operator=(const SteamClient&) = delete;

  // True when a Steam user is connected as the game.
  bool connect();
  void disconnect();
  bool connected() const { return stats_ != nullptr; }
  // Steam has delivered the stats, so achievements can be read and set.
  bool ready() const { return ready_; }

  // Once per frame: Steam's messages, the unlock queue, the store retry.
  void pump();

  // `api_name` from the app's schema (ach_...). Already unlocked ones are left alone.
  void unlock(std::string_view api_name);
  // The display name of the next achievement that has just been unlocked, for the on-screen notice.
  std::optional<std::string> next_unlocked();

  // Read-only views, valid once ready().
  int achievement_count();
  std::string achievement_name(int index);
  std::string achievement_title(const std::string& api_name);
  bool achieved(const std::string& api_name);

private:
  void flush_unlocks();
  void request_stats();

  void* client_ = nullptr;
  void* stats_ = nullptr;
  std::int32_t pipe_ = 0;
  std::int32_t user_ = 0;
  bool ready_ = false;
  bool store_pending_ = false;
  std::chrono::steady_clock::time_point retry_at_{};
  std::vector<std::string> pending_;
  std::vector<std::string> unlocked_;
};

}  // namespace oc::platform
