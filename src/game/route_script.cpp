#include "game/route_script.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <format>
#include <sstream>

#include "core/log.h"
#include "platform/keymap.h"

namespace oc::game {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kUnitsPerRad = 4096.0f / (2.0f * kPi);
constexpr u64 kDefaultTimeout = 1800;

int key_for(const std::string& name) {
  using platform::Action;
  auto first = [](Action action) { return platform::action_keys(action)[0]; };
  if (name == "up") return first(Action::Up);
  if (name == "down") return first(Action::Down);
  if (name == "left") return first(Action::Left);
  if (name == "right") return first(Action::Right);
  if (name == "confirm" || name == "ok") return first(Action::Ok);
  if (name == "cancel") return first(Action::Cancel);
  if (name == "run") return platform::action_keys(Action::Cancel)[1];
  if (name == "escape") return SDL_SCANCODE_ESCAPE;
  if (name == "tab" || name == "menu") return platform::action_keys(Action::Menu)[1];
  if (name == "triangle") return first(Action::Menu);
  if (name == "item") return first(Action::Item);
  if (name == "select") return platform::kSelectKey;
  if (name == "start" || name == "space") return platform::kStartKey;
  if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') return SDL_SCANCODE_A + (name[0] - 'a');
  // f1..f9: the hotkeys.
  if (name.size() == 2 && name[0] == 'f' && name[1] >= '1' && name[1] <= '9') return SDL_SCANCODE_F1 + (name[1] - '1');
  return -1;
}

int pad_button_for(const std::string& name) {
  if (name == "a") return SDL_GAMEPAD_BUTTON_SOUTH;
  if (name == "b") return SDL_GAMEPAD_BUTTON_EAST;
  if (name == "x") return SDL_GAMEPAD_BUTTON_WEST;
  if (name == "y") return SDL_GAMEPAD_BUTTON_NORTH;
  if (name == "start") return SDL_GAMEPAD_BUTTON_START;
  return -1;
}

bool to_int(const std::string& text, int& value) {
  if (text.empty()) return false;
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 0);
  if (*end) return false;
  value = static_cast<int>(parsed);
  return true;
}

bool to_hex(const std::string& text, int& value) {
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 16);
  if (!end || *end || text.empty()) return false;
  value = static_cast<int>(parsed);
  return true;
}

u64 timeout_arg(const std::vector<std::string>& args, size_t idx) {
  int timeout;
  return idx < args.size() && to_int(args[idx], timeout) && timeout > 0 ? static_cast<u64>(timeout) : kDefaultTimeout;
}

}  // namespace

bool Route::load(const std::filesystem::path& file, std::string* err) {
  std::ifstream in(file);
  if (!in) {
    if (err) *err = "cannot open " + file.string();
    return false;
  }
  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    const size_t comment_pos = line.find_first_of("#;");
    if (comment_pos != std::string::npos) line.erase(comment_pos);
    std::istringstream ss(line);
    Cmd cmd;
    cmd.line = line_no;
    std::string tok;
    if (!(ss >> cmd.when)) continue;
    while (ss >> tok) cmd.args.push_back(tok);
    if (cmd.args.empty()) {
      if (err) *err = "line " + std::to_string(line_no) + ": missing command";
      return false;
    }
    cmds_.push_back(std::move(cmd));
  }
  return true;
}

void Route::hold(int sc, bool down) {
  if (sc >= 0 && sc < platform::Input::kKeys) held_[static_cast<size_t>(sc)] = down;
}

void Route::fail(RouteOut& out, const Cmd& cmd, const std::string& why) {
  out.failed = true;
  out.done = true;
  out.message = "route line " + std::to_string(cmd.line) + " (" + cmd.args[0] + "): " + why;
}

bool Route::ready(const Cmd& cmd, const RouteState& st) const {
  if (cmd.when == "-") return true;
  if (cmd.when[0] == '+') {
    int ticks = 0;
    to_int(cmd.when.substr(1), ticks);
    return st.tick >= prev_end_ + static_cast<u64>(ticks);
  }
  int ticks = 0;
  to_int(cmd.when, ticks);
  return st.tick >= static_cast<u64>(ticks);
}

void Route::step(const RouteState& st, platform::Input& in, RouteOut& out) {
  prev_held_ = held_;
  pad_prev_held_ = pad_held_;
  if (pc_ >= cmds_.size()) {
    out.done = true;
  } else {
    const Cmd& cmd = cmds_[pc_];
    if (!started_ && ready(cmd, st)) {
      started_ = true;
      cmd_start_ = st.tick;
      phase_ = 0;
      aux_ = 0;
      quiet_ = 0;
      stuck_tick_ = st.tick;
      stuck_x_ = st.px;
      stuck_z_ = st.pz;
      cmd_room_ = st.room;
      path_.clear();
      path_i_ = 0;
      path_tick_ = st.tick;
      path_smooth_ = true;
      move_tick_ = st.tick;
      move_x_ = st.px;
      move_z_ = st.pz;
    }
    if (started_ && run(cmd, st, in, out)) {
      started_ = false;
      prev_end_ = st.tick;
      stick_on_ = false;
      ++pc_;
    }
  }
  if (as_left_ > 0 && st.tick >= as_next_ && out.screenshot.empty()) {
    out.screenshot = std::format("{}{:02}.png", as_prefix_, as_n_++);
    as_next_ = st.tick + static_cast<u64>(std::max(1, as_every_));
    --as_left_;
  }
  for (size_t i = 0; i < held_.size(); ++i) {
    in.key_down[i] = held_[i];
    in.key_pressed[i] = held_[i] && !prev_held_[i];
  }
  if (pad_on_) {
    in.pad_connected = true;
    for (size_t i = 0; i < pad_held_.size(); ++i) {
      in.pad_down[i] = pad_held_[i];
      in.pad_pressed[i] = pad_held_[i] && !pad_prev_held_[i];
    }
  }
  if (stick_on_) {
    in.pad_connected = true;
    in.pad_axis[SDL_GAMEPAD_AXIS_LEFTX] = stick_x_;
    in.pad_axis[SDL_GAMEPAD_AXIS_LEFTY] = stick_y_;
  }
}

// Returns true when the command is finished (this tick still applies the input set so far).
bool Route::run(const Cmd& cmd, const RouteState& st, platform::Input&, RouteOut& out) {
  const std::vector<std::string>& args = cmd.args;
  const std::string& op = args[0];
  const u64 elapsed = st.tick - cmd_start_;
  auto bad = [&](const std::string& why) {
    fail(out, cmd, why);
    return true;
  };
  auto timed_out = [&](size_t timeout_idx) { return elapsed > timeout_arg(args, timeout_idx); };
  // Returns true once the tap and its release gap are over.
  auto tap_step = [&](int sc, u64 hold_ticks) {
    if (phase_ == 0) {
      hold(sc, true);
      aux_ = st.tick;
      phase_ = 1;
    } else if (phase_ == 1 && st.tick >= aux_ + hold_ticks) {
      hold(sc, false);
      phase_ = 2;
      aux_ = st.tick;
    } else if (phase_ == 2 && st.tick >= aux_ + 3) {
      phase_ = 3;
      return true;
    }
    return false;
  };

  if (op == "key") {
    if (args.size() < 3) return bad("key down|up <name>");
    const int sc = key_for(args[2]);
    if (sc < 0) return bad("unknown key " + args[2]);
    hold(sc, args[1] == "down");
    return true;
  }
  if (op == "tap" || op == "action") {
    const int sc = op == "action" ? SDL_SCANCODE_RETURN : key_for(args.size() > 1 ? args[1] : "");
    if (sc < 0) return bad("unknown key");
    int ticks = 3;
    if (op == "tap" && args.size() > 2) to_int(args[2], ticks);
    return tap_step(sc, static_cast<u64>(std::max(1, ticks)));
  }
  if (op == "pad") {
    if (args.size() < 2) return bad("pad on|off|tap <button>");
    if (args[1] == "on" || args[1] == "off") {
      pad_on_ = args[1] == "on";
      pad_held_.fill(false);
      return true;
    }
    if (args[1] != "tap" || args.size() < 3) return bad("pad on|off|tap <button>");
    const int button = pad_button_for(args[2]);
    if (button < 0) return bad("unknown pad button " + args[2]);
    int ticks = 3;
    if (args.size() > 3) to_int(args[3], ticks);
    pad_on_ = true;
    const size_t index = static_cast<size_t>(button);
    if (phase_ == 0) {
      pad_held_[index] = true;
      aux_ = st.tick;
      phase_ = 1;
    } else if (phase_ == 1 && st.tick >= aux_ + static_cast<u64>(std::max(1, ticks))) {
      pad_held_[index] = false;
      phase_ = 2;
      aux_ = st.tick;
    } else if (phase_ == 2 && st.tick >= aux_ + 3) {
      return true;
    }
    return false;
  }
  if (op == "wait") {
    if (args.size() < 2) return bad("wait <ticks|room|dialogue|battle|idle>");
    int wait_ticks;
    if (to_int(args[1], wait_ticks)) return elapsed >= static_cast<u64>(std::max(0, wait_ticks));
    if (args[1] == "room") {
      if (args.size() < 3) return bad("wait room <name>");
      if (st.room_valid && st.room == args[2] && !st.battle) return true;
      if (timed_out(3)) return bad("timed out waiting for room " + args[2] + " (in " + st.room + ")");
      return false;
    }
    // wait text <words...>: an open dialogue whose message contains the words (default timeout)
    if (args[1] == "text") {
      std::string want;
      for (size_t i = 2; i < args.size(); ++i) want += (i > 2 ? " " : "") + args[i];
      const std::string got = st.dialogue && st.dialogue_text ? st.dialogue_text() : std::string();
      if (st.dialogue && got.find(want) != std::string::npos) return true;
      if (elapsed > kDefaultTimeout) return bad("timed out waiting for dialogue text '" + want + "'" + (st.dialogue ? " (showing '" + got + "')" : ""));
      return false;
    }
    if (args[1] == "dialogue") {
      const bool want = args.size() > 2 && args[2] == "open";
      if (st.dialogue == want) return true;
      if (timed_out(3)) return bad(std::string("timed out waiting for dialogue ") + (want ? "open" : "closed"));
      return false;
    }
    if (args[1] == "battle") {
      const bool start = args.size() > 2 && args[2] == "start";
      // The post-battle results screen is showing.
      if (args.size() > 2 && args[2] == "results") {
        if (st.battle_finished) return true;
        if (timed_out(3)) return bad("timed out waiting for the results screen");
        return false;
      }
      if (start ? st.battle : !st.battle) return true;
      if (timed_out(3)) return bad(std::string("timed out waiting for battle ") + (start ? "start" : "end"));
      return false;
    }
    if (args[1] == "idle") {
      if (st.field_idle && !st.battle) return true;
      if (timed_out(2)) return bad("timed out waiting for the controls");
      return false;
    }
    if (args[1] == "flow") {
      if (args.size() < 3) return bad("wait flow <state>");
      if (st.flow == args[2] && !st.movie) return true;
      if (timed_out(3)) return bad("timed out waiting for flow state " + args[2] + " (in " + st.flow + ")");
      return false;
    }
    if (args[1] == "guide") {
      int page;
      if (args.size() < 3 || !to_int(args[2], page)) return bad("wait guide <0|1|2>");
      if (st.guide_page == page) return true;
      if (timed_out(3)) return bad("timed out waiting for guide page " + args[2] + " (on " + std::to_string(st.guide_page) + ")");
      return false;
    }
    if (args[1] == "movie" || args[1] == "menu" || args[1] == "shop") {
      const bool want = args.size() > 2 && (args[2] == "start" || args[2] == "open");
      const bool cur = args[1] == "movie" ? st.movie : args[1] == "menu" ? st.menu : st.shop;
      if (cur == want) return true;
      if (timed_out(3)) return bad("timed out waiting for " + args[1] + (want ? " to start" : " to end"));
      return false;
    }
    return bad("unknown wait kind " + args[1]);
  }
  if (op == "walk" || op == "walk_ent" || op == "approach") {
    float tx = 0, tz = 0;
    size_t next = 2;
    // approach <ent> [tol] [timeout]: walk_ent that never fails - it also ends when blocked, timed out or a dialogue opened (sweeps
    // that talk to every NPC of a room).
    const bool lenient = op == "approach";
    if (lenient && (st.dialogue || !st.field_idle) && elapsed > 0) {
      log::info("route: tick {} approach ended: controls taken at {:.0f},{:.0f}", st.tick, st.px, st.pz);
      return true;
    }
    if (op == "walk") {
      if (args.size() < 3) return bad("walk <x> <z>");
      tx = std::strtof(args[1].c_str(), nullptr);
      tz = std::strtof(args[2].c_str(), nullptr);
      next = 3;
    } else {
      int ent = 0;
      if (args.size() < 2 || !to_int(args[1], ent) || !st.ent_xz || !st.ent_xz(ent, tx, tz)) return bad("walk_ent <ent>: bad entity");
    }
    float tol = 24.0f;
    if (args.size() > next) tol = std::strtof(args[next].c_str(), nullptr);
    if (!st.have_player) return timed_out(next + 1) ? bad("no controlled entity") : false;
    if (std::hypot(tx - st.px, tz - st.pz) <= tol) return true;
    // Walked through an exit / into an encounter: nothing left to walk to.
    if (st.room != cmd_room_ || st.battle) return true;
    // Plan over the walk mesh (again every 40 ticks: entity targets move); steer at the next waypoint.
    // Not moving for 25 ticks while steering: the straight legs hit a wall edge; fall back to centroid-to-centroid legs.
    if (st.tick - move_tick_ >= 25) {
      if (std::hypot(st.px - move_x_, st.pz - move_z_) < 2.0f && st.field_idle && phase_ == 1) {
        path_smooth_ = !path_smooth_;
        path_tick_ = 0;
      }
      move_tick_ = st.tick;
      move_x_ = st.px;
      move_z_ = st.pz;
    }
    if (st.path && ((path_.empty() && phase_ == 0) || st.tick - path_tick_ >= 40)) {
      path_ = st.path(st.px, st.pz, tx, tz, path_smooth_);
      if (phase_ == 0) {
        std::string path_text;
        for (const auto& point : path_) path_text += std::format(" ({:.0f},{:.0f})", point[0], point[1]);
        log::debug("route: tick {} path from {:.0f},{:.0f} to {:.0f},{:.0f}:{}", st.tick, st.px, st.pz, tx, tz, path_text.empty() ? " none" : path_text);
      }
      path_i_ = 0;
      path_tick_ = st.tick;
      phase_ = 1;
    }
    float wx = tx, wz = tz;
    while (path_i_ < path_.size()) {
      wx = path_[path_i_][0];
      wz = path_[path_i_][1];
      if (path_i_ + 1 < path_.size() && std::hypot(wx - st.px, wz - st.pz) < 14.0f) ++path_i_;
      else break;
    }
    if (elapsed % 30 == 0)
      log::debug("route: tick {} walking at {:.0f},{:.0f} -> waypoint {:.0f},{:.0f} (#{} of {})", st.tick, st.px, st.pz, wx, wz, path_i_, path_.size());
    const float dx = wx - st.px, dz = wz - st.pz;
    if (timed_out(next + 1)) {
      const std::string why = std::format("timed out walking to {:.0f},{:.0f} (at {:.0f},{:.0f})", tx, tz, st.px, st.pz);
      if (!lenient) return bad(why);
      log::info("route: tick {} approach ended: {}", st.tick, why);
      return true;
    }
    if (st.tick - stuck_tick_ >= (lenient ? 40u : 150u)) {
      if (std::hypot(st.px - stuck_x_, st.pz - stuck_z_) < 3.0f && st.field_idle) {
        const std::string why = std::format("blocked at {:.0f},{:.0f} walking to {:.0f},{:.0f}", st.px, st.pz, tx, tz);
        if (!lenient) return bad(why);
        log::info("route: tick {} approach ended: {}", st.tick, why);
        return true;
      }
      stuck_tick_ = st.tick;
      stuck_x_ = st.px;
      stuck_z_ = st.pz;
    }
    // Pad angle = world bearing - control yaw; the field reads atan2(y, -x) of the stick (y up) as the pad angle.
    const float bearing = std::atan2(dz, -dx) * kUnitsPerRad;
    const int pad = (static_cast<int>(std::lround(bearing)) - st.control_yaw) & 0xFFF;
    const float rad = static_cast<float>(pad) * 2.0f * kPi / 4096.0f;
    stick_on_ = true;
    stick_x_ = -std::cos(rad);
    // SDL stick y is down-positive.
    stick_y_ = -std::sin(rad);
    return false;
  }
  if (op == "skip") {
    if (st.dialogue) {
      quiet_ = 0;
      if (st.tick - aux_ >= 10) {
        hold(SDL_SCANCODE_RETURN, true);
        aux_ = st.tick;
      } else if (st.tick - aux_ >= 2) {
        hold(SDL_SCANCODE_RETURN, false);
      }
      return elapsed > timeout_arg(args, 1) ? bad("dialogue did not close") : false;
    }
    hold(SDL_SCANCODE_RETURN, false);
    return ++quiet_ >= 45;
  }
  // mash until flow <state> | room <name> | movie start|end [timeout]: confirm every 12 ticks
  if (op == "mash") {
    if (args.size() < 3 || args[1] != "until") return bad("mash until flow|room|movie|idle <value> [timeout]");
    if (args[2] == "idle") {
      if (args.size() < 4) return bad("mash until idle <value=any> [timeout]");
    } else if (args.size() < 4) {
      return bad("mash until flow|room|movie <value> [timeout]");
    }
    bool done = false;
    if (args[2] == "flow") done = st.flow == args[3] && !st.movie;
    else if (args[2] == "room") done = st.room_valid && st.room == args[3];
    else if (args[2] == "movie") done = st.movie == (args[3] == "start");
    else if (args[2] == "idle") done = st.field_idle && !st.battle;
    else if (args[2] == "shop") done = st.shop == (args[3] == "open");
    else if (args[2] == "menu") done = st.menu == (args[3] == "open");
    else return bad("mash until what? " + args[2]);
    if (done) {
      hold(SDL_SCANCODE_RETURN, false);
      return true;
    }
    if (elapsed > timeout_arg(args, 4)) return bad("mash timed out waiting for " + args[2] + " " + args[3] + " (flow " + st.flow + ", room " + st.room + ")");
    const u64 phase = elapsed % 12;
    hold(SDL_SCANCODE_RETURN, phase < 3);
    return false;
  }
  if (op == "choose") {
    int choice = 0;
    if (args.size() < 2 || !to_int(args[1], choice)) return bad("choose <n>");
    if (elapsed > timeout_arg(args, 2)) return bad("choose timed out");
    // Phase 0: wait for the choice; plain messages in front of it are confirmed (every 24 ticks).
    if (phase_ == 0) {
      if (st.dialogue_choice) {
        hold(SDL_SCANCODE_RETURN, false);
        phase_ = 1;
        aux_ = 0;
        quiet_ = 0;
        return false;
      }
      if (st.dialogue) {
        const u64 cycle_pos = elapsed % 24;
        hold(SDL_SCANCODE_RETURN, cycle_pos < 3);
      } else {
        hold(SDL_SCANCODE_RETURN, false);
      }
      return false;
    }
    // Phase 1: move the cursor.
    if (phase_ == 1) {
      if (quiet_ >= choice) {
        phase_ = 2;
        aux_ = st.tick;
        return false;
      }
      if (st.tick - aux_ >= 6) {
        hold(SDL_SCANCODE_DOWN, true);
        aux_ = st.tick;
        ++quiet_;
      } else if (st.tick - aux_ >= 2) {
        hold(SDL_SCANCODE_DOWN, false);
      }
      return false;
    }
    // Phase 2: confirm until the dialogue closes.
    hold(SDL_SCANCODE_DOWN, false);
    // The confirm key is released on the way out, so the next command's press is an edge again.
    if (!st.dialogue) {
      hold(SDL_SCANCODE_RETURN, false);
      return true;
    }
    if (st.tick - aux_ >= 12) {
      hold(SDL_SCANCODE_RETURN, true);
      aux_ = st.tick;
    } else if (st.tick - aux_ >= 2) {
      hold(SDL_SCANCODE_RETURN, false);
    }
    return false;
  }
  if (op == "fight") {
    // Phase 0: wait for the battle view.
    if (phase_ == 0) {
      if (st.battle) {
        phase_ = 1;
        aux_ = st.tick;
      } else if (timed_out(1)) {
        return bad("no battle started");
      }
      return false;
    }
    for (int sc : {SDL_SCANCODE_RETURN, SDL_SCANCODE_DOWN}) hold(sc, false);
    if (!st.battle) return true;
    // The view rotates the PCs and chains Weak / Strong / Fierce itself.
    out.battle_auto = true;
    if (st.tick - aux_ > 3 * timeout_arg(args, 1)) return bad("battle did not finish");
    // Every attack is applied at once (Attack -> Weak -> target, then the menu returns while stamina remains) and the result panel
    // needs confirms too: tap confirm every 8 ticks (2 ticks held); presses during animations are ignored.
    if ((st.tick - aux_) % 8 < 2) hold(SDL_SCANCODE_RETURN, true);
    return false;
  }
  if (op == "autoshot") {
    if (args.size() < 4 || !to_int(args[1], as_every_) || !to_int(args[2], as_left_)) return bad("autoshot <every> <count> <prefix>");
    as_prefix_ = args[3];
    as_n_ = 0;
    as_next_ = st.tick;
    return true;
  }
  if (op == "screenshot") {
    if (args.size() < 2) return bad("screenshot <file>");
    out.screenshot = args[1];
    return true;
  }
  // assert same [tol]: the `snap` state is unchanged
  if (op == "assert" && args.size() >= 2 && args[1] == "same") {
    int tol = 0;
    if (args.size() > 2) to_int(args[2], tol);
    const Snap& snap = snap_;
    if (st.room != snap.room) return bad("room changed: " + snap.room + " -> " + st.room);
    if (st.flow != snap.flow) return bad("flow changed: " + snap.flow + " -> " + st.flow);
    if (st.room_loads != snap.room_loads) return bad(std::format("the room was loaded {} time(s) since the snap", st.room_loads - snap.room_loads));
    if (st.battle_starts != snap.battle_starts) return bad("args battle was started since the snap");
    if (st.battle != snap.battle) return bad(std::string("battle is now ") + (st.battle ? "active" : "inactive"));
    if (st.title_plays != snap.title_plays || st.title_seq != snap.title_seq)
      return bad(std::format("title sequence restarted: {} -> {} ({} start(s))", snap.title_seq, st.title_seq, st.title_plays - snap.title_plays));
    const std::string bsig = st.battle_sig ? st.battle_sig() : std::string();
    if (bsig != snap.battle_sig) return bad("battle state changed: [" + snap.battle_sig + "] -> [" + bsig + "]");
    for (size_t ent_id = 0; ent_id < snap.ents.size(); ++ent_id) {
      Snap::EntState now;
      now.ok = st.ent_xz && st.ent_xz(static_cast<int>(ent_id), now.x, now.z);
      if (now.ok != snap.ents[ent_id].ok) return bad(std::format("entity {} appeared / vanished", ent_id));
      if (!now.ok) continue;
      now.facing = st.ent_facing ? st.ent_facing(static_cast<int>(ent_id)) : 0;
      now.yaw = st.ent_yaw ? st.ent_yaw(static_cast<int>(ent_id)) : 0;
      const float dist = std::hypot(now.x - snap.ents[ent_id].x, now.z - snap.ents[ent_id].z);
      if (dist > static_cast<float>(tol) || now.facing != snap.ents[ent_id].facing || now.yaw != snap.ents[ent_id].yaw)
        return bad(std::format("entity {} changed: {:.1f},{:.1f} f {:03X} yaw {:03X} -> {:.1f},{:.1f} f {:03X} yaw {:03X}", ent_id,
                               static_cast<double>(snap.ents[ent_id].x), static_cast<double>(snap.ents[ent_id].z), snap.ents[ent_id].facing, snap.ents[ent_id].yaw,
                               static_cast<double>(now.x), static_cast<double>(now.z), now.facing, now.yaw));
    }
    for (const auto& [id, expected] : snap.vars) {
      const int got = st.var ? st.var(id) : 0;
      if (got != expected) return bad(std::format("var {:X} changed: {} -> {}", id, expected, got));
    }
    log::info("route: state unchanged since the snap ({} entities, {} vars, room loads {}, art swaps {}, title {} x{}, battle [{}])",
              snap.ents.size(), snap.vars.size(), st.room_loads, st.art_swaps - snap.art_swaps, st.title_seq, st.title_plays, bsig);
    return true;
  }
  // assert frozen: the field simulation and the message typewriters stood still since the `snap`
  if (op == "assert" && args.size() == 2 && args[1] == "frozen") {
    if (st.field_ticks != snap_.field_ticks) return bad(std::format("{} field tick(s) ran since the snap", st.field_ticks - snap_.field_ticks));
    const std::vector<int> now = st.dialogue_progress ? st.dialogue_progress() : std::vector<int>();
    for (size_t i = 0; i < std::max(now.size(), snap_.dialogue.size()); ++i) {
      const int was = i < snap_.dialogue.size() ? snap_.dialogue[i] : -1;
      const int cur = i < now.size() ? now[i] : -1;
      if (was != cur) return bad(std::format("message window {} moved: {:X} -> {:X}", i, was, cur));
    }
    log::info("route: frozen since the snap (field tick {}, windows {})", st.field_ticks, now.size());
    return true;
  }
  if (op == "assert") {
    if (args.size() < 3) return bad("assert <what> <value>");
    if (args[1] == "room") {
      if (!st.room_valid || st.room != args[2]) return bad("expected room " + args[2] + ", in " + st.room);
    } else if (args[1] == "title_hold") {
      if (st.title_hold != (args[2] == "on")) return bad(std::format("the title scene is {}", st.title_hold ? "held" : "not held"));
    } else if (args[1] == "earned" || args[1] == "unearned") {
      const bool earned = st.earned_achievements && std::find(st.earned_achievements->begin(), st.earned_achievements->end(), args[2]) != st.earned_achievements->end();
      if (earned != (args[1] == "earned")) return bad(std::format("achievement {} is {}", args[2], earned ? "earned" : "not earned"));
    } else if (args[1] == "var") {
      int id = 0, want = 0;
      if (args.size() < 4 || !to_hex(args[2], id) || !to_int(args[3], want) || !st.var) return bad("assert var <hexid> <value>");
      const int got = st.var(id);
      if (got != want) return bad(std::format("var {:X} is {}, expected {}", id, got, want));
    } else if (args[1] == "moved") {
      // assert moved <ent> <min_distance>: the entity is at least that far from its `mark ent` position
      int ent_id = 0, want = 0;
      float x = 0, z = 0;
      if (args.size() < 4 || !to_int(args[2], ent_id) || !to_int(args[3], want) || !st.ent_xz || !st.ent_xz(ent_id, x, z)) return bad("assert moved <ent> <min>");
      const float dist = std::hypot(x - ent_mark_x_, z - ent_mark_z_);
      log::info("route: entity {} moved {:.1f} since the mark", ent_id, static_cast<double>(dist));
      if (dist < static_cast<float>(want)) return bad(std::format("entity {} moved only {:.1f}, expected at least {}", ent_id, static_cast<double>(dist), want));
    } else if (args[1] == "ent") {
      // assert ent <n> model|shown|moving|paused|wet <value>: entity state
      int ent_id = 0, want = 0;
      float model = 0, bits_f = 0;
      if (args.size() < 5 || !to_int(args[2], ent_id) || !to_int(args[4], want) || !st.ent_xz || !st.ent_xz(-2000 - ent_id, model, bits_f))
        return bad("assert ent <n> model|shown|moving|paused|wet <value>");
      const int bits = static_cast<int>(bits_f);
      int got = 0;
      if (args[3] == "model") got = static_cast<int>(model);
      else if (args[3] == "shown") got = (bits & 2) ? 1 : 0;
      else if (args[3] == "moving") got = (bits & 4) ? 1 : 0;
      else if (args[3] == "paused") got = (bits & 8) ? 1 : 0;
      else if (args[3] == "wet") got = (bits & 16) ? 1 : 0;
      else return bad("assert ent: unknown key " + args[3]);
      log::info("route: entity {} {} = {} (want {})", ent_id, args[3], got, want);
      if (got != want) return bad(std::format("entity {} {} is {}, expected {}", ent_id, args[3], got, want));
    } else if (args[1] == "elapsed") {
      // assert elapsed <min> <max>: driver ticks since `mark time`
      int lo = 0, hi = 0;
      if (args.size() < 4 || !to_int(args[2], lo) || !to_int(args[3], hi)) return bad("assert elapsed <min> <max>");
      const int got = static_cast<int>(st.tick - time_mark_);
      log::info("route: {} ticks since the time mark (want {}..{})", got, lo, hi);
      if (got < lo || got > hi) return bad(std::format("{} ticks since the time mark, expected {}..{}", got, lo, hi));
    } else if (args[1] == "height" || args[1] == "anim") {
      // assert height <p0..p2|n> <min> <max> (world y, down = +) | assert anim <who> <id|stair>
      int ent_id = -2;
      if (args.size() >= 3 && args[2].size() == 2 && args[2][0] == 'p' && args[2][1] >= '0' && args[2][1] <= '2' && st.party_ent) ent_id = st.party_ent(args[2][1] - '0');
      else if (args.size() >= 3) to_int(args[2], ent_id);
      float y = 0, anim_bits = 0;
      if (ent_id < 0 || !st.ent_xz || !st.ent_xz(-3000 - ent_id, y, anim_bits)) return bad("assert height|anim <p0..p2|ent>: no such entity");
      const int anim = static_cast<int>(anim_bits) & 0xFFFF;
      const bool stair = (static_cast<int>(anim_bits) & 0x10000) != 0;
      if (args[1] == "height") {
        int lo = 0, hi = 0;
        if (args.size() < 5 || !to_int(args[3], lo) || !to_int(args[4], hi)) return bad("assert height <who> <min> <max>");
        log::info("route: height of {} = {:.1f} (want {}..{})", args[2], static_cast<double>(y), lo, hi);
        if (y < static_cast<float>(lo) || y > static_cast<float>(hi)) return bad(std::format("height of {} is {:.1f}, expected {}..{}", args[2], static_cast<double>(y), lo, hi));
      } else {
        int want = 0;
        const bool is_stair = args.size() >= 4 && args[3] == "stair";
        if (args.size() < 4 || (!is_stair && !to_int(args[3], want))) return bad("assert anim <who> <id|stair>");
        log::info("route: {} anim {} on args {} tile", args[2], anim, stair ? "stair" : "plain");
        if (is_stair ? !stair : anim != want) return bad(std::format("{} anim {} ({} tile), expected {}", args[2], anim, stair ? "stair" : "plain", args[3]));
      }
    } else if (args[1] == "near") {
      // assert near <ent|player> <x> <z> <tol>: world position within tol
      int ent_id = -1, tol = 0;
      float x = 0, z = 0;
      if (args.size() < 6 || !to_int(args[5], tol) || !st.ent_xz) return bad("assert near <ent|player> <x> <z> <tol>");
      const float tx = std::strtof(args[3].c_str(), nullptr), tz = std::strtof(args[4].c_str(), nullptr);
      if (args[2] == "player") {
        if (!st.have_player) return bad("assert near: no player");
        x = st.px;
        z = st.pz;
      } else if (!to_int(args[2], ent_id) || !st.ent_xz(ent_id, x, z)) {
        return bad("assert near: no such entity");
      }
      const float dist = std::hypot(x - tx, z - tz);
      log::info("route: {} at {:.0f},{:.0f}, {:.1f} from {:.0f},{:.0f}", args[2], static_cast<double>(x), static_cast<double>(z), static_cast<double>(dist),
                static_cast<double>(tx), static_cast<double>(tz));
      if (dist > static_cast<float>(tol)) return bad(std::format("{} is {:.1f} from {:.0f},{:.0f} (tol {})", args[2], static_cast<double>(dist), static_cast<double>(tx), static_cast<double>(tz), tol));
    } else if (args[1] == "swaps") {
      // assert swaps <n>: in-place art swaps of the field room since `snap`
      int want = 0;
      if (!to_int(args[2], want)) return bad("assert swaps <n>");
      const u64 got = st.art_swaps - snap_.art_swaps;
      if (got != static_cast<u64>(want)) return bad(std::format("{} art swaps since the snap, expected {}", got, want));
    } else if (args[1] == "traceclip") {
      // assert traceclip <max_src_moves> <max_clip_changes>: the last `trace` (clip / keyframe per tick)
      int back = 0, chg = 0;
      if (args.size() < 4 || !to_int(args[2], back) || !to_int(args[3], chg)) return bad("assert traceclip <max_src_moves> <max_clip_changes>");
      log::info("route: traced clip: {} cross-fade source moves, {} clip changes", trace_back_jumps_, trace_clip_changes_);
      if (trace_back_jumps_ > back || trace_clip_changes_ > chg)
        return bad(std::format("traced clip: {} source moves / {} clip changes, allowed {} / {}", trace_back_jumps_, trace_clip_changes_, back, chg));
    } else if (args[1] == "traceheadturn") {
      // assert traceheadturn <ticks> <span>: the last `trace` saw the head turn (ax range in 4096/turn)
      int ticks = 0, span = 0;
      if (args.size() < 4 || !to_int(args[2], ticks) || !to_int(args[3], span)) return bad("assert traceheadturn <ticks> <span>");
      log::info("route: traced head turned on {} ticks, ax {}..{}", trace_head_ticks_, trace_head_min_, trace_head_max_);
      if (trace_head_ticks_ < ticks || trace_head_max_ - trace_head_min_ < span)
        return bad(std::format("traced head turned on {} ticks over ax {}..{}, expected >= {} ticks / span {}", trace_head_ticks_, trace_head_min_,
                               trace_head_max_, ticks, span));
    } else if (args[1] == "traceyaw") {
      // assert traceyaw <ticks> <octants>: the last `trace` saw the model yaw turn (scripted facing)
      int ticks = 0, oct = 0;
      if (args.size() < 4 || !to_int(args[2], ticks) || !to_int(args[3], oct)) return bad("assert traceyaw <ticks> <octants>");
      const int got_oct = std::popcount(static_cast<unsigned>(trace_yaw_octants_));
      log::info("route: traced yaw turned on {} ticks, {} octants", trace_yaw_moves_, got_oct);
      if (trace_yaw_moves_ < ticks || got_oct < oct)
        return bad(std::format("traced yaw turned on {} ticks / {} octants, expected >= {} / {}", trace_yaw_moves_, got_oct, ticks, oct));
    } else if (args[1] == "screen") {
      // assert screen right|left|up|down: the player moved that way on screen since `mark`
      float x = 0, y = 0;
      if (!st.ent_xz || !st.ent_xz(-1, x, y)) return bad("assert screen: no player");
      const float dx = x - mark_x_, dy = y - mark_y_;
      log::info("route: screen delta {:.1f},{:.1f}", dx, dy);
      const bool ok = args[2] == "right" ? dx > 8 && std::fabs(dx) > std::fabs(dy)
                    : args[2] == "left"  ? dx < -8 && std::fabs(dx) > std::fabs(dy)
                    : args[2] == "up"    ? dy < -1 : args[2] == "down" ? dy > 1 : false;
      if (!ok) return bad(std::format("player moved {:.1f},{:.1f} on screen, expected {}", dx, dy, args[2]));
    } else if (args[1] == "facing" || args[1] == "gap") {
      auto who = [&](const std::string& token) {
        if (token == "player") return -1;
        if (token.size() == 2 && token[0] == 'p' && token[1] >= '0' && token[1] <= '2' && st.party_ent) {
          const int party_ent = st.party_ent(token[1] - '0');
          return party_ent < 0 ? -2 : party_ent;
        }
        int ent_id = -2;
        to_int(token, ent_id);
        return ent_id;
      };
      if (args[1] == "facing") {
        int want = 0, tol = 64;
        if (args.size() < 4 || !to_int(args[3], want) || !st.ent_facing) return bad("assert facing <who> <angle> [tol]");
        if (args.size() > 4) to_int(args[4], tol);
        const int ent_id = who(args[2]);
        const int got = st.ent_facing(ent_id);
        const int diff = ((got - want) & 0xFFF) > 0x800 ? 0x1000 - ((got - want) & 0xFFF) : ((got - want) & 0xFFF);
        log::info("route: facing of {} is {:03X} (want {:03X})", args[2], got, want & 0xFFF);
        if (diff > tol) return bad(std::format("facing of {} is {:03X}, expected {:03X} +-{}", args[2], got, want & 0xFFF, tol));
      } else {
        int lo = 0, hi = 0;
        float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
        if (args.size() < 6 || !to_int(args[4], lo) || !to_int(args[5], hi) || !st.ent_xz) return bad("assert gap <args> <b> <min> <max>");
        const int ea = who(args[2]), eb = who(args[3]);
        auto pos = [&](int ent_id, float& x, float& z) { return ent_id == -1 ? (x = st.px, z = st.pz, st.have_player) : st.ent_xz(ent_id, x, z); };
        if (!pos(ea, x0, z0) || !pos(eb, x1, z1)) return bad("assert gap: no such entity");
        const float dist = std::hypot(x1 - x0, z1 - z0);
        log::info("route: gap {} - {} = {:.1f}", args[2], args[3], static_cast<double>(dist));
        if (dist < static_cast<float>(lo) || dist > static_cast<float>(hi)) return bad(std::format("gap {:.1f} outside {}..{}", static_cast<double>(dist), lo, hi));
      }
    } else if (args[1] == "offmesh") {
      // assert offmesh <p1|p2|ent N|npcs> <max>: ticks that entity stood off the walkable mesh
      int ent_id = -2, max_ticks = 0;
      float count = 0, unused = 0;
      // Worst of every placed model entity that is not a party member.
      if (args.size() >= 4 && args[2] == "npcs") {
        if (!to_int(args[3], max_ticks) || !st.ent_xz) return bad("assert offmesh npcs <max>");
        int worst = -1;
        for (int npc = 0; npc < 128 && st.ent_xz(-1000 - npc, count, unused); ++npc)
          if (count > static_cast<float>(worst)) worst = static_cast<int>(count), ent_id = npc;
        log::info("route: worst npc off-mesh ticks = {} (entity {})", worst, ent_id);
        if (worst > max_ticks) return bad(std::format("entity {} stood off the walk mesh for {} ticks (max {})", ent_id, worst, max_ticks));
      } else {
        const bool by_ent = args.size() >= 5 && args[2] == "ent";
        if (by_ent) {
          if (!to_int(args[3], ent_id) || !to_int(args[4], max_ticks) || !st.ent_xz) return bad("assert offmesh ent <n> <max>");
        } else {
          if (args.size() < 4 || !to_int(args[3], max_ticks) || !st.ent_xz || !st.party_ent || args[2].size() != 2 || args[2][0] != 'p') return bad("assert offmesh p1|p2|ent <n>|npcs <max>");
          ent_id = st.party_ent(args[2][1] - '0');
        }
        if (ent_id < 0 || !st.ent_xz(-1000 - ent_id, count, unused)) return bad("assert offmesh: no such member");
        log::info("route: {} off-mesh ticks = {}", args[2], static_cast<int>(count));
        if (static_cast<int>(count) > max_ticks) return bad(std::format("{} stood off the walk mesh for {} ticks (max {})", args[2], static_cast<int>(count), max_ticks));
      }
    } else if (args[1] == "rel") {
      // assert rel <who> <ref> <dx> <dy> <tol>: screen offset of who from ref (px, p0..p2 or entity) within tol
      auto ent_of = [&](const std::string& token) {
        int ent_id = -2;
        if (token.size() == 2 && token[0] == 'p' && token[1] >= '0' && token[1] <= '2' && st.party_ent) ent_id = st.party_ent(token[1] - '0');
        else to_int(token, ent_id);
        return ent_id;
      };
      int dx = 0, dy = 0, tol = 10;
      float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      const int ea = ent_of(args[2]), eb = ent_of(args.size() > 3 ? args[3] : "");
      if (args.size() < 7 || !to_int(args[4], dx) || !to_int(args[5], dy) || !to_int(args[6], tol) || !st.ent_xz || ea < 0 || eb < 0 ||
          !st.ent_xz(-100 - ea, x0, y0) || !st.ent_xz(-100 - eb, x1, y1))
        return bad("assert rel <who> <ref> <dx> <dy> <tol>");
      const float ox = x0 - x1, oy = y0 - y1;
      log::info("route: screen offset of {} from {} = {:.1f},{:.1f} (want {},{} +-{})", args[2], args[3], static_cast<double>(ox),
                static_cast<double>(oy), dx, dy, tol);
      if (std::fabs(ox - static_cast<float>(dx)) > static_cast<float>(tol) || std::fabs(oy - static_cast<float>(dy)) > static_cast<float>(tol))
        return bad(std::format("{} is at {:.1f},{:.1f} from {} on screen, expected {},{} +-{}", args[2], static_cast<double>(ox),
                               static_cast<double>(oy), args[3], dx, dy, tol));
    } else if (args[1] == "text") {
      // assert text <words...>: the open dialogue's message contains the words
      std::string want;
      for (size_t i = 2; i < args.size(); ++i) want += (i > 2 ? " " : "") + args[i];
      const std::string got = st.dialogue && st.dialogue_text ? st.dialogue_text() : std::string();
      if (!st.dialogue || got.find(want) == std::string::npos) return bad("dialogue text is '" + got + "', expected '" + want + "'");
    } else if (args[1] == "choiceline") {
      // assert choiceline <n>: the choice hand / highlight sits on message line n (translated numbering)
      int line_no = 0;
      if (args.size() < 3 || !to_int(args[2], line_no)) return bad("assert choiceline <n>");
      if (st.choice_line != line_no) return bad("choice hand is on line " + std::to_string(st.choice_line) + ", expected " + args[2]);
    } else if (args[1] == "notext") {
      // assert notext <words...>: a dialogue is open and its message does not contain the words
      std::string want;
      for (size_t i = 2; i < args.size(); ++i) want += (i > 2 ? " " : "") + args[i];
      const std::string got = st.dialogue && st.dialogue_text ? st.dialogue_text() : std::string();
      if (!st.dialogue || got.find(want) != std::string::npos) return bad("dialogue text is '" + got + "', must not contain '" + want + "'");
    } else if (args[1] == "dialogue") {
      if (st.dialogue != (args[2] == "open")) return bad("dialogue is " + std::string(st.dialogue ? "open" : "closed"));
    } else if (args[1] == "battle" && args[2] == "events") {
      // assert battle events <w1|a|w2...>: the battle view's window / action order STARTS with exactly these tokens
      const std::vector<std::string> got = st.battle_events ? st.battle_events() : std::vector<std::string>();
      std::string want, have;
      for (size_t i = 3; i < args.size(); ++i) want += (i > 3 ? " " : "") + args[i];
      for (size_t i = 0; i < got.size(); ++i) have += (i ? " " : "") + got[i];
      if (args.size() < 4 || have.compare(0, want.size(), want) != 0 || (have.size() > want.size() && have[want.size()] != ' '))
        return bad("battle events are [" + have + "], expected to start with [" + want + "]");
    } else if (args[1] == "battle") {
      if (st.battle != (args[2] == "active")) return bad("battle is " + std::string(st.battle ? "active" : "inactive"));
    } else if (args[1] == "guide") {
      int page;
      if (!to_int(args[2], page)) return bad("assert guide <0|1|2>");
      if (st.guide_page != page) return bad("guide page is " + std::to_string(st.guide_page) + ", expected " + args[2]);
    } else if (args[1] == "pause") {
      if (st.pause != (args[2] == "open")) return bad(std::string("the pause screen is ") + (st.pause ? "up" : "not up"));
    } else if (args[1] == "flow") {
      if (st.flow != args[2]) return bad("flow is " + st.flow);
    } else if (args[1] == "menu" || args[1] == "shop" || args[1] == "movie") {
      const bool want = args[2] == "open" || args[2] == "playing";
      const bool cur = args[1] == "movie" ? st.movie : args[1] == "menu" ? st.menu : st.shop;
      if (cur != want) return bad(args[1] + (cur ? " is up" : " is not up"));
    } else if (args[1] == "name") {
      int id = 0;
      if (args.size() < 4 || !to_int(args[2], id) || !st.char_name) return bad("assert name <char> <text>");
      if (st.char_name(id) != args[3]) return bad(std::format("name of {} is '{}', expected '{}'", id, st.char_name(id), args[3]));
    } else if (args[1] == "party") {
      if (!st.party || st.party() != args[2]) return bad("party is " + (st.party ? st.party() : std::string("?")) + ", expected " + args[2]);
    } else if (args[1] == "disc") {
      int want = 0;
      if (!to_int(args[2], want) || !st.disc || st.disc() != want) return bad(std::format("disc is {}", st.disc ? st.disc() : -1));
    } else if (args[1] == "location") {
      int want = 0;
      if (!to_int(args[2], want) || !st.location || st.location() != want) return bad(std::format("location is {}", st.location ? st.location() : -1));
    } else if (args[1] == "result") {
      int want = 0;
      if (!to_int(args[2], want) || st.battle_result != want) return bad(std::format("BATTLE_RESULT is {}", st.battle_result));
    } else if (args[1] == "hp") {
      // assert hp <char> <min> <max>
      int id = 0, lo = 0, hi = 0;
      if (args.size() < 5 || !to_int(args[2], id) || !to_int(args[3], lo) || !to_int(args[4], hi) || !st.char_stat) return bad("assert hp <char> <min> <max>");
      const int got = st.char_stat(id, 0);
      log::info("route: char {} HP {} / {} (want {}..{})", id, got, st.char_stat(id, 1), lo, hi);
      if (got < lo || got > hi) return bad(std::format("char {} HP is {}, expected {}..{}", id, got, lo, hi));
    } else if (args[1] == "status") {
      // assert status <char> <hexmask> set|clear
      int id = 0, mask = 0;
      if (args.size() < 5 || !to_int(args[2], id) || !to_hex(args[3], mask) || !st.char_stat) return bad("assert status <char> <hexmask> set|clear");
      const int got = st.char_stat(id, 2);
      const bool want_set = args[4] == "set";
      log::info("route: char {} status {:#x}", id, got);
      if (got < 0 || ((got & mask) == mask) != want_set) return bad(std::format("char {} status is {:#x}, expected {:#x} {}", id, got, mask, args[4]));
    } else {
      return bad("unknown assert " + args[1]);
    }
    return true;
  }
  if (op == "load") {
    if (args.size() < 2) return bad("load <room> [param]");
    out.load_room = args[1];
    out.load_param = 0;
    if (args.size() > 2) to_int(args[2], out.load_param);
    return true;
  }
  // set model <char> <byte>: D9 + DA on a party member
  if (op == "set" && args.size() >= 4 && args[1] == "model") {
    int id = 0, value = 0;
    if (!to_int(args[2], id) || !to_int(args[3], value) || !st.set_char_stat) return bad("set model <char> <byte>");
    st.set_char_stat(id, 3, value);
    return true;
  }
  // set hp <char> <value> | set status <char> <hexbits>
  if (op == "set" && args.size() >= 4 && (args[1] == "hp" || args[1] == "status")) {
    int id = 0, value = 0;
    const bool hp = args[1] == "hp";
    if (!to_int(args[2], id) || !(hp ? to_int(args[3], value) : to_hex(args[3], value)) || !st.set_char_stat) return bad("set hp|status <char> <value>");
    st.set_char_stat(id, hp ? 0 : 2, value);
    return true;
  }
  // put <x> <z>
  if (op == "put") {
    int x = 0, z = 0;
    if (args.size() < 3 || !to_int(args[1], x) || !to_int(args[2], z)) return bad("put <x> <z>");
    out.put = true;
    out.put_x = static_cast<float>(x);
    out.put_z = static_cast<float>(z);
    return true;
  }
  if (op == "set" && args.size() >= 4 && args[1] == "option") {
    int mask = 0, value = 0;
    if (!to_hex(args[2], mask) || !to_int(args[3], value)) return bad("set option <hexmask> <0|1>");
    out.set_option = true;
    out.option_mask = mask;
    out.option_on = value;
    return true;
  }
  if (op == "set") {
    int id = 0, value = 0;
    if (args.size() < 4 || args[1] != "var" || !to_hex(args[2], id) || !to_int(args[3], value)) return bad("set var <hexid> <value>");
    out.set_var = true;
    out.var_id = id;
    out.var_value = value;
    return true;
  }
  // mark ent <n>: remember an entity's world position for `assert moved`
  if (op == "mark" && args.size() >= 3 && args[1] == "ent") {
    int ent_id = 0;
    if (!to_int(args[2], ent_id) || !st.ent_xz || !st.ent_xz(ent_id, ent_mark_x_, ent_mark_z_)) return bad("mark ent <n>: no such entity");
    return true;
  }
  if (op == "mark" && args.size() >= 2 && args[1] == "time") {
    time_mark_ = st.tick;
    return true;
  }
  // mark: remember the player's screen position for `assert screen`
  if (op == "mark") {
    if (!st.ent_xz || !st.ent_xz(-1, mark_x_, mark_y_)) return bad("mark: no player on screen");
    return true;
  }
  if (op == "pos") {
    log::info("route: tick {} room {} player {:.0f},{:.0f} yaw {:03X}", st.tick, st.room, st.px, st.pz, st.control_yaw);
    return true;
  }
  // trace <ent> <ticks>: log the entity's position / facing every tick, then a speed / turn summary
  if (op == "trace") {
    int ent_id = 0, trace_ticks = 0;
    if (args.size() < 3 || !to_int(args[1], ent_id) || !to_int(args[2], trace_ticks)) return bad("trace <ent> <ticks>");
    float x = 0, z = 0;
    if (!st.ent_xz || !st.ent_xz(ent_id, x, z)) return bad("trace: no such entity");
    const int facing = st.ent_facing ? st.ent_facing(ent_id) : 0;
    const int yw = st.ent_yaw ? st.ent_yaw(ent_id) : facing;
    if (phase_ == 0) {
      phase_ = 1;
      aux_ = 0;
      trace_x_ = x;
      trace_z_ = z;
      trace_f_ = facing;
      trace_max_step_ = 0;
      trace_max_turn_ = 0;
      trace_moving_ = 0;
      trace_idle_runs_ = 0;
      trace_idle_ = 0;
      trace_y_ = yw;
      trace_yaw_moves_ = 0;
      trace_yaw_octants_ = 1 << (((yw + 0x100) & 0xFFF) >> 9);
      trace_clip_ = st.ent_clip ? st.ent_clip(ent_id, false) : -1;
      trace_src_ = -1;
      trace_back_jumps_ = trace_clip_changes_ = 0;
      trace_head_ticks_ = trace_head_min_ = trace_head_max_ = 0;
    } else {
      const int ck = st.ent_clip ? st.ent_clip(ent_id, false) : -1;
      const int sk = st.ent_clip ? st.ent_clip(ent_id, true) : -1;
      if (ck != trace_clip_ && ((ck >> 12) != (trace_clip_ >> 12) || ck < 0 || trace_clip_ < 0)) ++trace_clip_changes_;
      // a cross-fade source is frozen: its keyframe may not move, nor wrap back to the start
      if (sk >= 0 && trace_src_ >= 0 && (sk >> 12) == (trace_src_ >> 12) && sk != trace_src_) ++trace_back_jumps_;
      log::info("route: trace ent {} t {} clip {} key4 {} fade-source {} key4 {}", ent_id, aux_, ck >> 12, ck & 0xFFF, sk >> 12, sk & 0xFFF);
      int hax = 0, haz = 0;
      if (st.ent_head && st.ent_head(ent_id, hax, haz)) {
        if (trace_head_ticks_++ == 0) trace_head_min_ = trace_head_max_ = hax;
        trace_head_min_ = std::min(trace_head_min_, hax);
        trace_head_max_ = std::max(trace_head_max_, hax);
        log::info("route: trace ent {} t {} head ax {} az {}", ent_id, aux_, hax, haz);
      }
      trace_clip_ = ck;
      trace_src_ = sk;
      const float dist = std::hypot(x - trace_x_, z - trace_z_);
      int dt = (facing - trace_f_) & 0xFFF;
      if (dt > 0x800) dt -= 0x1000;
      trace_max_step_ = std::max(trace_max_step_, dist);
      trace_max_turn_ = std::max(trace_max_turn_, std::abs(dt));
      if (dist > 0.01f) {
        ++trace_moving_;
        if (trace_idle_ > 0) ++trace_idle_runs_;
        trace_idle_ = 0;
      } else {
        ++trace_idle_;
      }
      if (yw != trace_y_) ++trace_yaw_moves_;
      trace_yaw_octants_ |= 1 << (((yw + 0x100) & 0xFFF) >> 9);
      log::info("route: trace ent {} t {} x {:.1f} z {:.1f} step {:.2f} facing {:03X} dturn {} yaw {:03X}", ent_id, aux_, static_cast<double>(x),
                static_cast<double>(z), static_cast<double>(dist), facing, dt, yw);
      trace_y_ = yw;
      trace_x_ = x;
      trace_z_ = z;
      trace_f_ = facing;
    }
    if (++aux_ > static_cast<u64>(trace_ticks)) {
      log::info("route: trace ent {} summary: ticks {} moving {} legs {} max step {:.2f} u/tick max facing change {} per tick, yaw turned on {} ticks, "
                "{} octants", ent_id, trace_ticks, trace_moving_, trace_idle_runs_, static_cast<double>(trace_max_step_), trace_max_turn_, trace_yaw_moves_,
                std::popcount(static_cast<unsigned>(trace_yaw_octants_)));
      return true;
    }
    return false;
  }
  if (op == "dump") {
    out.dump = true;
    return true;
  }
  // art hd|original|modded|toggle: the host switches like F6 (toggle = HD-family <-> original)
  if (op == "art") {
    if (args.size() < 2 || (args[1] != "hd" && args[1] != "original" && args[1] != "modded" && args[1] != "toggle")) return bad("art hd|original|modded|toggle");
    out.art = args[1];
    return true;
  }
  // snap [hexvar...]: remember the state `assert same` compares
  if (op == "snap") {
    Snap snap;
    snap.room = st.room;
    snap.flow = st.flow;
    snap.room_loads = st.room_loads;
    snap.art_swaps = st.art_swaps;
    snap.battle_starts = st.battle_starts;
    snap.title_plays = st.title_plays;
    snap.title_seq = st.title_seq;
    snap.battle = st.battle;
    snap.battle_sig = st.battle_sig ? st.battle_sig() : std::string();
    snap.field_ticks = st.field_ticks;
    snap.dialogue = st.dialogue_progress ? st.dialogue_progress() : std::vector<int>();
    for (int ent_id = 0; ent_id < 128 && st.room_valid; ++ent_id) {
      Snap::EntState ent;
      ent.ok = st.ent_xz && st.ent_xz(ent_id, ent.x, ent.z);
      if (ent.ok) {
        ent.facing = st.ent_facing ? st.ent_facing(ent_id) : 0;
        ent.yaw = st.ent_yaw ? st.ent_yaw(ent_id) : 0;
      }
      snap.ents.push_back(ent);
    }
    while (!snap.ents.empty() && !snap.ents.back().ok) snap.ents.pop_back();
    for (size_t i = 1; i < args.size(); ++i) {
      int id = 0;
      if (!to_hex(args[i], id)) return bad("snap [hexvar...]");
      snap.vars.emplace_back(id, st.var ? st.var(id) : 0);
    }
    snap_ = std::move(snap);
    log::info("route: snap: room {} flow {} {} entities, room loads {}, title {} x{}, battle [{}]", snap_.room, snap_.flow, snap_.ents.size(),
              snap_.room_loads, snap_.title_seq, snap_.title_plays, snap_.battle_sig);
    return true;
  }
  // request: issued once; the screen it opens is handled by the following wait / tap lines
  if (op == "request") {
    if (args.size() < 2) return bad("request <kind> [arg]");
    if (phase_ == 0) {
      out.request = args[1];
      out.request_arg = 0;
      if (args.size() > 2) to_int(args[2], out.request_arg);
      phase_ = 1;
      aux_ = st.tick;
      return false;
    }
    // Let the host take the request.
    return st.tick - aux_ >= 4;
  }
  if (op == "log") {
    std::string message;
    for (size_t i = 1; i < args.size(); ++i) message += (i > 1 ? " " : "") + args[i];
    log::info("route: {}", message);
    return true;
  }
  if (op == "quit") {
    out.done = true;
    return true;
  }
  return bad("unknown command " + op);
}

// ---- recorder / replayer --------------------------------------------------------------------------------------------
bool InputRecorder::open(const std::filesystem::path& file, const std::string& room, int entrance) {
  out_.open(file, std::ios::trunc);
  if (!out_) return false;
  out_ << "OCREPLAY 1 " << room << ' ' << entrance << '\n';
  return true;
}

void InputRecorder::write(double dt, int ticks, const platform::Input& in) {
  if (!out_.is_open()) return;
  auto list = [&](const std::array<bool, platform::Input::kKeys>& flags) {
    std::string text;
    for (size_t i = 0; i < flags.size(); ++i)
      if (flags[i]) text += (text.empty() ? "" : ",") + std::to_string(i);
    return text.empty() ? std::string("-") : text;
  };
  auto mask = [&](const std::array<bool, platform::Input::kButtons>& flags) {
    u32 bits = 0;
    for (size_t i = 0; i < flags.size(); ++i)
      if (flags[i]) bits |= 1u << i;
    return bits;
  };
  std::string axes;
  for (size_t i = 0; i < in.pad_axis.size(); ++i) axes += (i ? "," : "") + std::to_string(static_cast<int>(std::lround(in.pad_axis[i] * 1000.0f)));
  out_ << static_cast<long long>(std::llround(dt * 1e6)) << ' ' << ticks << ' ' << list(in.key_down) << ' ' << list(in.key_pressed) << ' '
       << (in.pad_connected ? 1 : 0) << ' ' << std::hex << mask(in.pad_down) << ' ' << mask(in.pad_pressed) << std::dec << ' ' << axes << '\n';
}

bool InputReplayer::load(const std::filesystem::path& file, std::string* err) {
  std::ifstream in(file);
  std::string head;
  if (!in || !std::getline(in, head)) {
    if (err) *err = "cannot read " + file.string();
    return false;
  }
  std::istringstream hs(head);
  std::string magic;
  int ver = 0;
  if (!(hs >> magic >> ver >> room_ >> entrance_) || magic != "OCREPLAY" || ver != 1) {
    if (err) *err = "not an OCREPLAY 1 file";
    return false;
  }
  std::string line;
  while (std::getline(in, line))
    if (!line.empty()) lines_.push_back(line);
  return true;
}

bool InputReplayer::next(ReplayFrame& frame) {
  if (pos_ >= lines_.size()) return false;
  std::istringstream ss(lines_[pos_++]);
  long long us = 0;
  std::string down, pressed, axes;
  int connected = 0;
  u32 pdown = 0, ppressed = 0;
  ss >> us >> frame.ticks >> down >> pressed >> connected >> std::hex >> pdown >> ppressed >> std::dec >> axes;
  frame.dt = static_cast<double>(us) * 1e-6;
  frame.in = platform::Input{};
  auto fill = [](const std::string& text, std::array<bool, platform::Input::kKeys>& flags) {
    if (text == "-") return;
    std::istringstream ks(text);
    std::string token;
    while (std::getline(ks, token, ',')) {
      const int i = std::atoi(token.c_str());
      if (i >= 0 && i < platform::Input::kKeys) flags[static_cast<size_t>(i)] = true;
    }
  };
  fill(down, frame.in.key_down);
  fill(pressed, frame.in.key_pressed);
  frame.in.pad_connected = connected != 0;
  for (size_t i = 0; i < frame.in.pad_down.size(); ++i) {
    frame.in.pad_down[i] = (pdown >> i) & 1;
    frame.in.pad_pressed[i] = (ppressed >> i) & 1;
  }
  {
    std::istringstream as(axes);
    std::string token;
    size_t i = 0;
    while (std::getline(as, token, ',') && i < frame.in.pad_axis.size()) frame.in.pad_axis[i++] = static_cast<float>(std::atoi(token.c_str())) / 1000.0f;
  }
  return true;
}

}  // namespace oc::game
