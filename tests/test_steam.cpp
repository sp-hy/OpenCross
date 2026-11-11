// platform/steam.h: SteamClient's unlock queue and the "all achievements" meta unlock, against a stand-in for the user stats calls.
#include <algorithm>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "game/achievements.h"
#include "platform/steam.h"

using namespace oc;
using platform::SteamClient;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

struct Account {
  std::set<std::string> achieved;
  std::set<std::string> schema;
  std::vector<std::string> set_calls;
  std::vector<platform::StatsEvent> events;
  int stores = 0;
};

class FakeBackend final : public platform::SteamBackend {
public:
  explicit FakeBackend(Account& account) : account_(account) {}

  bool poll(platform::StatsEvent& event) override {
    if (account_.events.empty()) return false;
    event = account_.events.front();
    account_.events.erase(account_.events.begin());
    return true;
  }
  void request_stats() override {}
  bool store_stats() override {
    ++account_.stores;
    return true;
  }
  bool set_achievement(const std::string& api_name) override {
    if (!account_.schema.count(api_name)) return false;
    account_.set_calls.push_back(api_name);
    account_.achieved.insert(api_name);
    return true;
  }
  bool achieved(const std::string& api_name) override { return account_.achieved.count(api_name) != 0; }
  int achievement_count() override { return static_cast<int>(account_.schema.size()); }
  std::string achievement_name(int index) override { return *std::next(account_.schema.begin(), index); }
  std::string achievement_title(const std::string& api_name) override { return "title of " + api_name; }
  std::string persona_name() override { return "Serge"; }

private:
  Account& account_;
};

struct Session {
  Account account;
  SteamClient client;
  std::vector<std::string> names = game::Achievements::api_names();
  std::string meta = game::Achievements::meta_api_name();

  explicit Session(std::size_t achieved_before) {
    account.schema.insert(names.begin(), names.end());
    account.schema.insert(meta);
    for (std::size_t i = 0; i < achieved_before; ++i) account.achieved.insert(names[i]);
    account.events.push_back({true, true});
    client.attach(std::make_unique<FakeBackend>(account));
    client.set_meta(meta, names);
  }

  bool meta_stored() const { return account.achieved.count(meta) != 0; }
  int meta_sets() const { return static_cast<int>(std::count(account.set_calls.begin(), account.set_calls.end(), meta)); }
};
}  // namespace

int main() {
  CHECK(game::Achievements::api_names().size() == 37);
  {
    SteamClient offline;
    CHECK(offline.persona_name().empty());
    Session session(0);
    CHECK(session.client.persona_name() == "Serge");
  }

  {
    Session session(36);
    session.client.unlock(session.names[36]);
    CHECK(!session.meta_stored());
    session.client.pump();
    CHECK(session.client.ready());
    CHECK(session.account.achieved.size() == 37 + 1);
    CHECK(session.meta_stored());
    CHECK(session.meta_sets() == 1);
    CHECK(session.account.stores == 1);
    CHECK(session.client.next_unlocked() == "title of " + session.names[36]);
    CHECK(session.client.next_unlocked() == "title of " + session.meta);
    CHECK(!session.client.next_unlocked());
  }
  {
    Session session(35);
    session.client.unlock(session.names[35]);
    session.client.pump();
    CHECK(!session.meta_stored());
    CHECK(session.account.achieved.size() == 36);
    session.client.unlock(session.names[36]);
    session.client.pump();
    CHECK(session.meta_stored());
    CHECK(session.meta_sets() == 1);
  }
  {
    Session session(35);
    session.client.unlock(session.names[35]);
    session.client.unlock(session.names[36]);
    session.client.pump();
    CHECK(session.meta_stored());
    CHECK(session.meta_sets() == 1);
  }
  {
    Session session(36);
    session.client.unlock(session.names[0]);
    session.client.pump();
    CHECK(!session.meta_stored());
    CHECK(session.account.set_calls.empty());
    CHECK(session.account.stores == 0);
  }
  {
    Session session(37);
    session.client.unlock(session.names[5]);
    session.client.pump();
    CHECK(!session.meta_stored());
    CHECK(session.account.set_calls.empty());
  }
  {
    Session session(37);
    session.account.achieved.insert(session.meta);
    session.client.unlock(session.names[5]);
    session.client.pump();
    CHECK(session.meta_sets() == 0);
    CHECK(session.account.stores == 0);
  }
  {
    Session session(36);
    session.account.schema.erase(session.names[36]);
    session.client.unlock(session.names[36]);
    session.client.pump();
    CHECK(!session.meta_stored());
    CHECK(session.account.set_calls.empty());
  }
  {
    Session session(36);
    session.account.events.clear();
    session.client.unlock(session.names[36]);
    session.client.pump();
    CHECK(!session.client.ready());
    CHECK(!session.meta_stored());
    session.account.events.push_back({true, true});
    session.client.pump();
    CHECK(session.meta_stored());
  }
  {
    Session session(36);
    session.client.set_meta({}, {});
    session.client.unlock(session.names[36]);
    session.client.pump();
    CHECK(!session.meta_stored());
    CHECK(session.account.achieved.size() == 37);
  }

  if (failures == 0) std::printf("steam tests passed\n");
  return failures == 0 ? 0 : 1;
}
