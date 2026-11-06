// Route runner (debug automation) and input recorder / replayer: parsing, timing, blocking commands, failure reporting,
// and the replay file round trip.
#include <SDL3/SDL_scancode.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "game/route_script.h"

using namespace oc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

std::filesystem::path write_tmp(const char* name, const char* text) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
  std::ofstream(path) << text;
  return path;
}

// Runs the route against a scripted state until it is done or `max` ticks passed; returns the tick it finished at (or max).
struct Sim {
  game::Route route;
  game::RouteState st;
  game::RouteOut last;
  u64 tick = 0;
  std::vector<std::string> shots;
  bool step() {
    st.tick = tick++;
    platform::Input in;
    game::RouteOut out;
    route.step(st, in, out);
    if (!out.screenshot.empty()) shots.push_back(out.screenshot);
    last = out;
    held = in.key_down;
    pressed = in.key_pressed;
    return !out.done;
  }
  std::array<bool, platform::Input::kKeys> held{}, pressed{};
};

}  // namespace

int main() {
  // ---- parsing and basic timing
  {
    Sim sim;
    std::string err;
    const auto route_file = write_tmp("oc_route_a.txt", "# comment\n0 key down up\n+5 key up up   ; trailing comment\n- screenshot a.png\n- quit\n");
    CHECK(sim.route.load(route_file, &err));
    CHECK(sim.route.line_count() == 4);
    sim.st.room = "r";
    sim.st.room_valid = true;
    // Tick 0: key down; the pressed edge lasts one tick.
    sim.step();
    CHECK(sim.held[SDL_SCANCODE_UP] && sim.pressed[SDL_SCANCODE_UP]);
    sim.step();
    CHECK(sim.held[SDL_SCANCODE_UP] && !sim.pressed[SDL_SCANCODE_UP]);
    int guard = 0;
    while (sim.step() && ++guard < 100) {}
    CHECK(!sim.last.failed);
    CHECK(sim.shots.size() == 1 && sim.shots[0] == "a.png");
    CHECK(!sim.held[SDL_SCANCODE_UP]);
    // The +5 delay held the key for 5 ticks at least.
    CHECK(sim.tick >= 7);
  }
  // ---- blocking waits, asserts and timeouts
  {
    Sim sim;
    std::string err;
    const auto route_file = write_tmp("oc_route_b.txt", "0 wait room b 30\n- assert room b\n- assert var 32 7\n- quit\n");
    CHECK(sim.route.load(route_file, &err));
    sim.st.room = "a";
    sim.st.room_valid = true;
    sim.st.var = [](int id) { return id == 0x32 ? 7 : 0; };
    // Still waiting.
    for (int i = 0; i < 5; ++i) CHECK(sim.step());
    sim.st.room = "b";
    int guard = 0;
    while (sim.step() && ++guard < 20) {}
    CHECK(!sim.last.failed);

    Sim timeout_sim;
    CHECK(timeout_sim.route.load(route_file, &err));
    timeout_sim.st.room = "a";
    timeout_sim.st.room_valid = true;
    guard = 0;
    while (timeout_sim.step() && ++guard < 100) {}
    // Timed out waiting for room b.
    CHECK(timeout_sim.last.failed);
    CHECK(timeout_sim.last.message.find("room b") != std::string::npos);
  }
  // ---- walk: steers the stick at the target and finishes within the tolerance
  {
    Sim sim;
    std::string err;
    const auto route_file = write_tmp("oc_route_c.txt", "0 walk 100 0 5 200\n- quit\n");
    CHECK(sim.route.load(route_file, &err));
    sim.st.have_player = true;
    sim.st.room = "r";
    sim.st.room_valid = true;
    sim.st.px = 0;
    sim.st.pz = 0;
    sim.st.control_yaw = 0;
    sim.st.field_idle = true;
    platform::Input in;
    game::RouteOut out;
    sim.st.tick = 0;
    sim.route.step(sim.st, in, out);
    CHECK(in.pad_connected);
    // Target is +x: heading (-cos h, sin h) = (1, 0) gives h = 0x800; with yaw 0 the pad angle is 0x800 = "right".
    CHECK(in.pad_axis[0] > 0.99f);
    CHECK(std::fabs(in.pad_axis[1]) < 0.01f);
    sim.st.px = 98;
    sim.st.tick = 1;
    game::RouteOut out2;
    platform::Input in2;
    sim.route.step(sim.st, in2, out2);
    sim.st.tick = 2;
    game::RouteOut out3;
    platform::Input in3;
    sim.route.step(sim.st, in3, out3);
    // Arrived: the stick is released.
    CHECK(!in3.pad_connected);
  }
  // ---- wait idle: not while the pad lock, the menu-button wait or a load fade still runs
  {
    Sim sim;
    std::string err;
    const auto route_file = write_tmp("oc_route_idle.txt", "0 wait idle 30\n- quit\n");
    CHECK(sim.route.load(route_file, &err));
    sim.st.room = "r";
    sim.st.room_valid = true;
    sim.st.field_idle = true;
    sim.st.settling = true;
    for (int i = 0; i < 5; ++i) CHECK(sim.step());
    sim.st.settling = false;
    int guard = 0;
    while (sim.step() && ++guard < 20) {}
    CHECK(!sim.last.failed);
  }
  // ---- bad command
  {
    Sim sim;
    std::string err;
    const auto route_file = write_tmp("oc_route_d.txt", "0 frobnicate 1\n");
    CHECK(sim.route.load(route_file, &err));
    sim.step();
    CHECK(sim.last.failed);
  }
  // ---- recorder / replayer round trip
  {
    const auto replay_file = std::filesystem::temp_directory_path() / "oc_replay.txt";
    {
      game::InputRecorder rec;
      CHECK(rec.open(replay_file,"alu_a01f", 3));
      platform::Input in;
      in.key_down[SDL_SCANCODE_UP] = true;
      in.key_pressed[SDL_SCANCODE_UP] = true;
      in.key_down[SDL_SCANCODE_LSHIFT] = true;
      rec.write(1.0 / 60.0, 1, in);
      in.key_pressed[SDL_SCANCODE_UP] = false;
      in.pad_connected = true;
      in.pad_axis[0] = -0.5f;
      in.pad_down[3] = true;
      rec.write(0.033, 2, in);
    }
    game::InputReplayer rep;
    std::string err;
    CHECK(rep.load(replay_file, &err));
    CHECK(rep.room() == "alu_a01f" && rep.entrance() == 3 && rep.frames() == 2);
    game::ReplayFrame fr;
    CHECK(rep.next(fr));
    CHECK(fr.ticks == 1 && std::fabs(fr.dt - 1.0 / 60.0) < 1e-5);
    CHECK(fr.in.key_down[SDL_SCANCODE_UP] && fr.in.key_pressed[SDL_SCANCODE_UP] && fr.in.key_down[SDL_SCANCODE_LSHIFT]);
    CHECK(!fr.in.pad_connected);
    CHECK(rep.next(fr));
    CHECK(fr.ticks == 2 && !fr.in.key_pressed[SDL_SCANCODE_UP] && fr.in.key_down[SDL_SCANCODE_UP]);
    CHECK(fr.in.pad_connected && fr.in.pad_down[3] && std::fabs(fr.in.pad_axis[0] + 0.5f) < 1e-3f);
    CHECK(!rep.next(fr));
  }
  if (failures == 0) std::printf("route tests passed\n");
  return failures == 0 ? 0 : 1;
}
