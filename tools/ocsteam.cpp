// `opencross ocsteam`: connects to the running Steam client as the game (Steam shows "Playing" for as long as this runs) and prints the
// app's achievements with their state. Read-only: it never sets or stores anything.
#include <chrono>
#include <cstdio>
#include <thread>

#include "platform/steam.h"

int ocsteam_main(int, char**) {
  oc::platform::SteamClient steam;
  if (!steam.connect()) {
    std::printf("not connected: Steam is closed, logged out, or the game is not owned\n");
    return 1;
  }
  std::printf("connected as app %llu\n", static_cast<unsigned long long>(oc::platform::kRdeAppId));
  for (int i = 0; i < 500 && !steam.ready(); ++i) {
    steam.pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!steam.ready()) {
    std::printf("Steam did not deliver the stats\n");
    return 1;
  }
  const int count = steam.achievement_count();
  int unlocked = 0;
  for (int i = 0; i < count; ++i) {
    const std::string name = steam.achievement_name(i);
    const bool achieved = steam.achieved(name);
    unlocked += achieved ? 1 : 0;
    std::printf("%c %-26s %s\n", achieved ? '*' : ' ', name.c_str(), steam.achievement_title(name).c_str());
  }
  std::printf("%d of %d unlocked\n", unlocked, count);
  return 0;
}
