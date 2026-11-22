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
  // ---- read: confirms a window only after its text has stood still for the delay; log hands its text to the host
  {
    Sim sim;
    std::string err;
    const auto route_file = write_tmp("oc_route_read.txt", "0 read 20\n- log hello there\n- wait 200\n- quit\n");
    CHECK(sim.route.load(route_file, &err));
    sim.st.room = "r";
    sim.st.room_valid = true;
    sim.step();
    sim.step();
    CHECK(sim.last.note == "hello there");
    int typed = 0;
    sim.st.dialogue = true;
    sim.st.dialogue_progress = [&typed] { return std::vector<int>{typed}; };
    int first_press = -1;
    for (int i = 0; i < 60 && first_press < 0; ++i) {
      if (i < 30) ++typed;
      sim.step();
      if (sim.pressed[SDL_SCANCODE_RETURN]) first_press = i;
    }
    // Typing until step 29, then 20 still ticks.
    CHECK(first_press >= 49 && first_press <= 51);
    sim.st.dialogue = false;
    for (int i = 0; i < 40; ++i) {
      sim.step();
      CHECK(!sim.pressed[SDL_SCANCODE_RETURN]);
    }
  }
  // ---- put beside: hands the entity and the distance to the host; a bad entity fails the run
  {
    Sim sim;
    std::string err;
    CHECK(sim.route.load(write_tmp("oc_route_beside.txt", "0 put beside 5\n- put beside 7 60\n- quit\n"), &err));
    sim.st.room = "r";
    sim.st.room_valid = true;
    sim.step();
    CHECK(sim.last.put_beside == 5 && sim.last.put_dist == 0.0f && !sim.last.put);
    sim.step();
    CHECK(sim.last.put_beside == 7 && sim.last.put_dist == 60.0f);
    Sim bad;
    CHECK(bad.route.load(write_tmp("oc_route_beside_bad.txt", "0 put beside x\n"), &err));
    bad.step();
    CHECK(bad.last.failed);
  }
  // ---- put / put ent / put v: the point, the entity and the party V flag
  {
    Sim sim;
    std::string err;
    CHECK(sim.route.load(write_tmp("oc_route_put.txt", "0 put 10 -20\n- put ent 4 5 6\n- put v -998 163\n- quit\n"), &err));
    sim.step();
    CHECK(sim.last.put && sim.last.put_ent == -1 && !sim.last.put_v && sim.last.put_x == 10.0f && sim.last.put_z == -20.0f);
    sim.step();
    CHECK(sim.last.put && sim.last.put_ent == 4 && !sim.last.put_v && sim.last.put_x == 5.0f);
    sim.step();
    CHECK(sim.last.put && sim.last.put_ent == -1 && sim.last.put_v && sim.last.put_x == -998.0f && sim.last.put_z == 163.0f);
    Sim bad;
    CHECK(bad.route.load(write_tmp("oc_route_put_bad.txt", "0 put v 5\n"), &err));
    bad.step();
    CHECK(bad.last.failed);
  }
  // ---- battle harness: set battle hp (a value, a percentage), set critical, set hit; a bad unit fails the run
  {
    Sim sim;
    std::string err;
    CHECK(sim.route.load(write_tmp("oc_route_hp.txt", "0 set battle hp 3 1\n- set battle hp 0 25%\n- set critical 1\n- set hit 0\n- quit\n"), &err));
    sim.step();
    CHECK(sim.last.hp_unit == 3 && sim.last.hp_value == 1 && !sim.last.hp_percent && sim.last.critical == -1);
    sim.step();
    CHECK(sim.last.hp_unit == 0 && sim.last.hp_value == 25 && sim.last.hp_percent);
    sim.step();
    CHECK(sim.last.critical == 1 && sim.last.hp_unit == -1 && sim.last.hit == -1);
    sim.step();
    CHECK(sim.last.hit == 0 && sim.last.critical == -1);
    Sim bad;
    CHECK(bad.route.load(write_tmp("oc_route_hp_bad.txt", "0 set battle hp x 5\n"), &err));
    bad.step();
    CHECK(bad.last.failed);
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
  // ---- sides: the columns beside the 4:3 picture of a measured frame
  {
    // A 16:9 frame of 64 x 36: the 4:3 picture is 320 / 236 of the height wide, so about 7 columns are left on each side.
    const u32 width = 64, height = 36;
    std::vector<u8> frame(static_cast<size_t>(width) * height * 4, 0);
    float left = -1.0f, right = -1.0f;
    CHECK(game::side_columns_lit(frame.data(), width, height, left, right));
    CHECK(left == 0.0f && right == 0.0f);
    for (u32 y = 0; y < height; ++y)
      for (u32 x = 0; x < 4; ++x) frame[(static_cast<size_t>(y) * width + x) * 4 + 1] = 60;
    CHECK(game::side_columns_lit(frame.data(), width, height, left, right));
    CHECK(left > 0.5f && left < 0.6f && right == 0.0f);
    std::vector<u8> square(static_cast<size_t>(48) * 36 * 4, 200);
    CHECK(!game::side_columns_lit(square.data(), 48, 36, left, right));

    // The verb asks for one measured frame, then judges it.
    auto run_sides = [&](const char* range, float lit_left, float lit_right) {
      Sim sim;
      std::string err;
      const auto route_file = write_tmp("oc_route_sides.txt", (std::string("0 sides ") + range + "\n- quit\n").c_str());
      CHECK(sim.route.load(route_file, &err));
      sim.step();
      CHECK(sim.last.measure_sides);
      sim.step();
      CHECK(!sim.last.measure_sides && !sim.last.failed);
      sim.st.side_lit[0] = lit_left;
      sim.st.side_lit[1] = lit_right;
      ++sim.st.side_frames;
      int guard = 0;
      while (sim.step() && !sim.last.failed && ++guard < 20) {}
      return !sim.last.failed;
    };
    CHECK(run_sides("99 100", 1.0f, 0.995f));
    CHECK(!run_sides("99 100", 1.0f, 0.5f));
    CHECK(run_sides("0 0", 0.0f, 0.0f));
    CHECK(!run_sides("0 0", 0.0f, 0.01f));
    CHECK(run_sides("0 20", 0.12f, 0.0f));
    CHECK(!run_sides("99 100", -1.0f, -1.0f));
  }
  if (failures == 0) std::printf("route tests passed\n");
  return failures == 0 ? 0 : 1;
}
