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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oc::platform {

inline constexpr std::uint64_t kRdeAppId = 1133760;

struct StatsEvent {
  bool received;
  bool ok;
};

// The user stats calls SteamClient needs. The real one talks to the running client; tests supply their own.
class SteamBackend {
public:
  virtual ~SteamBackend() = default;
  // The next stats message (received, or stored when `received` is false); false when none is waiting.
  virtual bool poll(StatsEvent& event) = 0;
  virtual void request_stats() = 0;
  virtual bool store_stats() = 0;
  virtual bool set_achievement(const std::string& api_name) = 0;
  virtual bool achieved(const std::string& api_name) = 0;
  virtual int achievement_count() = 0;
  virtual std::string achievement_name(int index) = 0;
  virtual std::string achievement_title(const std::string& api_name) = 0;
};

class SteamClient {
public:
  SteamClient() = default;
  ~SteamClient();
  SteamClient(const SteamClient&) = delete;
  SteamClient& operator=(const SteamClient&) = delete;

  // True when a Steam user is connected as the game.
  bool connect();
  void disconnect();
  // Use `backend` instead of the running client and ask it for the stats.
  void attach(std::unique_ptr<SteamBackend> backend);
  bool connected() const { return backend_ != nullptr; }
  // Steam has delivered the stats, so achievements can be read and set.
  bool ready() const { return ready_; }

  // Once per frame: Steam's messages, the unlock queue, the store retry.
  void pump();

  // `api_name` from the app's schema (ach_...). Already unlocked ones are left alone.
  void unlock(std::string_view api_name);
  // `meta` is unlocked as soon as an unlock made by this client leaves every name in `required` achieved.
  void set_meta(std::string meta, std::vector<std::string> required);
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

  std::unique_ptr<SteamBackend> backend_;
  bool ready_ = false;
  bool store_pending_ = false;
  std::chrono::steady_clock::time_point retry_at_{};
  std::vector<std::string> pending_;
  std::string meta_;
  std::vector<std::string> meta_required_;
  std::vector<std::string> unlocked_;
};

}  // namespace oc::platform
