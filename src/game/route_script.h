#pragma once
// Debug automation: a route script (text file of timed inputs and checks, [Debug] Script=) and an input recorder /
// replayer ([Debug] Record= / Replay=). Both feed the same platform::Input the real window would, one simulation tick
// at a time, so a run is deterministic and needs no human.
//
// Route file: one command per line, '#' or ';' cut the line there wherever they stand (so neither can appear in an argument).
// [Debug] Replay is ignored when [Debug] Script is set. A failed `load` and an unknown `request` end the run with exit code 1 and
// no "route FAILED" line (only a log error). A line is  <when> <command> [args]  where <when> is
//   N     absolute driver tick (the line waits until that tick has been reached),
//   +N    N ticks after the previous line finished,
//   -     right after the previous line finished.
// A line only starts once the previous one has finished; blocking commands (walk, wait, skip, fight, ...) finish when
// their condition holds, or fail the run when their timeout (default 1800 ticks) runs out.
//   key down|up <name>            hold / release a key. Names: up down left right confirm|ok cancel run menu|tab triangle item
//                                 (RDE's keys, platform/keymap.h), escape, select, start|space, a..z, f1..f9
//   tap <name> [ticks]            press and release (default 3 ticks held, then 3 ticks free)
//   pad on|off                    a virtual pad is connected (no button held) from now on / not any more
//   pad tap <a|b|x|y|start> [ticks]   press and release a button of the virtual pad (connects it)
//   action                        = tap confirm
//   walk <x> <z> [tol] [timeout]  steer the stick until the player is within tol (default 24) of the world point
//   walk_ent <ent> [tol] [timeout]  same, towards entity <ent> of the room
//   approach <ent> [tol] [timeout]  walk_ent that never fails: also ends when blocked (40 ticks), timed out or the controls are taken
//   wait <ticks>                  wait (no timeout)
//   wait room <name> [timeout]    wait until the field room is <name> (and the screen has faded back in)
//   wait dialogue open|closed [timeout]
//   wait text <words...>          an open dialogue shows a message containing the words (plain text, newlines as spaces); no timeout
//                                 argument, the default timeout applies (every other wait form takes an optional [timeout])
//   wait battle start|end|results [timeout]   (results = the post-battle results screen is up)
//   wait idle [timeout]           controls free again (no dialogue, no fade, no transition)
//   wait flow <title|name|slots|opening|field> [timeout]   the game flow reached that state
//   wait movie start|end [timeout]   an FMV is playing / has finished
//   wait guide <0|1|2> [timeout]  the user guide shows that page (1 controller, 2 keyboard, 0 closed)
//   wait menu open|closed [timeout]  the main menu / a shop / the rename screen is up / gone
//   wait shop open|closed [timeout]  a shop screen (FE 5E / 68) is up / gone
//   skip [timeout]                confirm every 10 ticks until a dialogue that is open has closed (45 quiet ticks end it)
//   mash until flow <state> | room <name> | movie start|end | idle x | shop open|closed | menu open|closed [timeout]   confirm every 12 ticks until it holds
//                                 (`idle` still needs an argument after it, `x` above; its value is ignored)
//   choose <n> [timeout]          wait for a dialogue, move the choice cursor down n times, confirm
//   fight [timeout]               mash the battle menu (attack chain) until the battle view has finished
//   autoshot <every> <count> <prefix>   background screenshots every <every> ticks (<prefix>NN.png) while later commands run
//   screenshot <file.png>         capture the next rendered frame (relative names go to shots/ next to the route's ini file)
//   assert room <name> | assert var <hexid> <value> | assert dialogue open|closed | assert battle active|inactive | assert battle events <w1|a|w2 ...> (the window / action order starts with these tokens) |
//   assert result <n>             (BATTLE_RESULT)       assert flow <state> | assert menu open|closed | assert shop open|closed |
//   assert movie playing|idle      assert guide <0|1|2>     assert pause open|closed (the RDE pause screen)
//   assert text <words...>        the open dialogue's message contains the words
//   assert choiceline <n>         the choice hand / highlight is on message line n of the open list (translated numbering, DialogueBox::choice_line)
//   assert notext <words...>      a dialogue is open and its message does not contain the words
//   load <room> [param]           debug warp to a room
//   set var <hexid> <value>       debug write of a script variable
//   set option <hexmask> <0|1>    debug write of one bit of the Settings options word (0x200 = [button] Pressed: Run / Walk, 0x800 = analog stick)
//   put <x> <z>                   debug: place the party (leader and followers) at a world point on the walk mesh
//   set model <char> <byte>       D9 + DA on a party member (the character record model byte)
//   set hp <char> <value> | set status <char> <hexbits>   debug write of a character's current HP / status bits (1 poison, 8 burn,
//                                 10 sprain, 20 flu)
//   assert hp <char> <min> <max>  current HP of a character in [min, max]     assert status <char> <hexmask> set|clear
//   request <kind> [arg]          debug: ask the host for what a field opcode would (money <n> = set the gold, battle <encounter> = A3 (battlefield 0), rename <char> = FE 4A, party = FE 5B,
//                                 save = FE 85 (cleared-save prompt), ngplus = FE 84 (New Game+ list), menu = FE 72 (save-point main menu), disc <n> = FE 65 (n = disc - 1), movie <n> = 7E / D4 (on the disc in use), trading <row> = FE 67,
//                                 party_add <char> = add a character to the party, keyitem <n> = give key item n, slotdump <n> = write the game as one save
//                                 block to $OC_SLOT_DUMP<n>.bin, battle_disguise <encounter> = the disguised-fight variant of `battle`);
//                                 issued once, finishes 4 ticks later (wait for the screen with `wait menu|shop`)
//   assert name <char> <text>     the typed name of a character (UTF-8)       assert party <id,id,id>   the party ids in slot order (- = empty)
//   assert disc <n>               the disc in use                             assert location <id>      the saved location word of the state
//   mark / assert screen right|left|up|down   player screen movement since the mark
//   mark ent <n> / assert moved <n> <min>   entity <n> is at least <min> world units from where `mark ent` saw it
//   assert ent <n> model|shown|moving|paused|wet <value>   room model index / drawn / script motion active / motion preempted /
//                                 walk flags carry water (0x60000), each 0 or 1 except model
//   assert near <ent|player> <x> <z> <tol>   world position within tol
//   assert facing <who> <angle> [tol]   facing (4096/turn) of <who> = player | p0 | p1 | p2 | <ent>, within tol (default 64)
//   assert gap <a> <b> <min> <max>      xz distance between two entities (same names) is in [min, max]
//   mark time / assert elapsed <min> <max>   driver ticks since `mark time` are within [min, max]
//   assert height <p0..p2|ent> <min> <max>   world y (down = +) of the entity; assert anim <who> <id|stair>   submitted clip id / on a stair tile
//   trace <ent> <ticks>           log an entity's position / facing / model yaw every tick for N ticks and a speed / turn summary
//   (trace also logs clip / keyframe*4 per tick) assert traceclip <max_src_moves> <max_clip_changes>   cross-fade source keyframe moves / clip changes of the last trace
//   (trace also logs the head-turn angles while the head callback is on) assert traceheadturn <ticks> <span>   the last trace saw the head turn on >= ticks ticks over an ax range >= span (4096/turn)
//   assert traceyaw <ticks> <octants>   the last trace saw the model yaw change on >= ticks ticks and visit >= octants of the 8 45-deg sectors
//   dump                          log the room's entity table
//   art hd|original|modded|toggle   switch the art mode like F6 / the options overlay (returns at once; toggle = HD-family <-> original)
//   pos                           log the tick, room, player position and control yaw
//   assert offmesh p1|p2 <max> | ent <n> <max> | npcs <max>   ticks a party member / entity / the worst placed non-party model stood off the walk mesh is <= max
//   assert rel <who> <ref> <dx> <dy> <tol>   screen offset of <who> from <ref> (p0..p2 or entity) is (dx, dy) within tol
//   snap [hexvar...]              remember the simulation state: room, flow, room-load / battle-start / title-sequence counters, the
//                                 battle signature, every entity's position / facing / model yaw and the listed script variables
//   assert same [tol]             nothing of the `snap` state changed (positions within tol world units, default 0)
//   assert frozen                 no field tick ran and no message window's typewriter moved since `snap`
//   assert swaps <n>              exactly n in-place art swaps of the field room since `snap`
//   log <text...>                 message in the log
//   quit                          end the run
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "core/types.h"
#include "platform/window.h"

namespace oc::game {

// Snapshot of the game the route driver reads each tick. The callbacks are filled by the host:
//   ent_xz(entity) -> position; ent_facing(entity) -> facing (4096/turn), -1 = the controlled entity;
//   ent_yaw(entity) -> facing the model shows (4096/turn); ent_head(entity) -> head-turn angles (ax, az), false = head not turning;
//   ent_clip(entity, ...) -> (clip << 12) | keyframe * 4, -1 none; party_ent(slot) -> entity index (-1 none);
//   char_name(char) -> typed name (UTF-8, "" = none); party() -> party ids "0,1,22" in slot order; disc() -> disc in use (1 / 2);
//   location() -> saved location word of the state; path(...) -> walk-mesh waypoints;
//   battle_events() -> route events of the battle view ("w<msg>" script window opened, "a" action began), empty without a view;
//   battle_sig() -> battle state summary (scene, unit HP / KO / outcome), "" = no battle;
//   char_stat(char, 0 current HP | 1 max HP | 2 status bits) -> value, -1 = none; set_char_stat is the debug write of the same.
struct RouteState {
  // Driver tick (counts every simulated step, field or battle).
  u64 tick = 0;
  std::string room;
  bool room_valid = false;
  const std::vector<std::string>* earned_achievements = nullptr;
  bool title_hold = false;
  // Controlled entity position.
  float px = 0, pz = 0;
  int control_yaw = 0;
  bool have_player = false;
  bool dialogue = false;
  // A choice is pending in the open dialogue.
  bool dialogue_choice = false;
  // Message line the choice hand is on (-1: no list is taking the pick yet).
  int choice_line = -1;
  // Plain text of the open dialogue message ("" = none).
  std::function<std::string()> dialogue_text;
  bool battle = false;
  // Result panel confirmed (the driver hands back to the field next step).
  bool battle_finished = false;
  // Game flow state (title / name / slots / opening / field).
  std::string flow = "field";
  bool movie = false;
  // The main menu (or a save screen / rename) is up.
  bool menu = false;
  bool shop = false;
  // User guide page on screen: 1 controller, 2 keyboard, 0 none.
  int guide_page = 0;
  // The pause screen is up.
  bool pause = false;
  // Field simulation ticks so far.
  u64 field_ticks = 0;
  // Typewriter position of every field message window (FieldWorld::dialogue_progress).
  std::function<std::vector<int>()> dialogue_progress;
  // Controls free.
  bool field_idle = false;
  // BATTLE_RESULT script variable.
  int battle_result = 0;
  std::function<bool(int, float&, float&)> ent_xz;
  std::function<int(int)> ent_facing;
  std::function<int(int)> ent_yaw;
  std::function<bool(int, int&, int&)> ent_head;
  std::function<int(int, bool)> ent_clip;
  std::function<int(int)> party_ent;
  // Script variable read.
  std::function<int(int)> var;
  std::function<std::string(int)> char_name;
  std::function<std::string()> party;
  std::function<int()> disc;
  std::function<int()> location;
  std::function<std::vector<std::array<float, 2>>(float, float, float, float, bool)> path;
  // Counters since start: FieldWorld::load_room calls, in-place art swaps of the field room, field battles, title sequences (tit0..3).
  u64 room_loads = 0;
  u64 art_swaps = 0;
  u64 battle_starts = 0;
  u64 title_plays = 0;
  // The last title sequence started.
  std::string title_seq;
  std::function<std::vector<std::string>()> battle_events;
  std::function<std::string()> battle_sig;
  std::function<int(int, int)> char_stat;
  std::function<void(int, int, int)> set_char_stat;
};

// What the route driver asks the host to do this tick.
struct RouteOut {
  // Request a screenshot of the next frame.
  std::string screenshot;
  // Debug warp.
  std::string load_room;
  int load_param = 0;
  // `put x z`: place the party at a world point.
  bool put = false;
  float put_x = 0, put_z = 0;
  bool set_var = false;
  // `set option <hexmask> <0|1>`: one bit of the Settings options word.
  bool set_option = false;
  int option_mask = 0, option_on = 0;
  int var_id = 0, var_value = 0;
  // `fight`: let the battle view play itself.
  bool battle_auto = false;
  // Log the entity table.
  bool dump = false;
  // Host request (see "request"): kind.
  std::string request;
  // `art`: hd / original / modded / toggle.
  std::string art;
  int request_arg = 0;
  bool done = false;
  bool failed = false;
  std::string message;
};

class Route {
 public:
  bool load(const std::filesystem::path& file, std::string* err);
  // One driver tick: fills `in` with the input of this tick (key_down is the held set, key_pressed the edges).
  void step(const RouteState& st, platform::Input& in, RouteOut& out);
  size_t line_count() const { return cmds_.size(); }

 private:
  struct Cmd {
    int line = 0;
    std::string when;
    std::vector<std::string> args;
  };
  bool ready(const Cmd& cmd, const RouteState& st) const;
  // Runs one tick of a command; true = finished.
  bool run(const Cmd& cmd, const RouteState& st, platform::Input& in, RouteOut& out);
  void hold(int scancode, bool down);
  void press_edge(int scancode, platform::Input& in);
  void fail(RouteOut& out, const Cmd& cmd, const std::string& why);

  std::vector<Cmd> cmds_;
  size_t pc_ = 0;
  // The current command has begun; the tick it began; the tick the previous one finished.
  bool started_ = false;
  u64 cmd_start_ = 0;
  u64 prev_end_ = 0;
  std::string cmd_room_;
  std::vector<std::array<float, 2>> path_;
  size_t path_i_ = 0;
  u64 path_tick_ = 0;
  // `mark time`.
  u64 time_mark_ = 0;
  bool path_smooth_ = true;
  u64 move_tick_ = 0;
  float move_x_ = 0, move_z_ = 0;
  // `mark` / `mark ent`.
  float mark_x_ = 0, mark_y_ = 0;
  float ent_mark_x_ = 0, ent_mark_z_ = 0;
  // Per-command scratch.
  u64 aux_ = 0;
  // `trace` state: position / step, clip + keyframe per tick (backward jumps inside a clip), ticks the head was turning and the
  // range of its ax, model yaw (ticks it changed, octants visited).
  float trace_x_ = 0, trace_z_ = 0, trace_max_step_ = 0;
  int trace_f_ = 0, trace_max_turn_ = 0, trace_moving_ = 0, trace_idle_runs_ = 0, trace_idle_ = 0;
  int trace_src_ = -1, trace_clip_ = -1, trace_back_jumps_ = 0, trace_clip_changes_ = 0;
  int trace_head_ticks_ = 0, trace_head_min_ = 0, trace_head_max_ = 0;
  int trace_y_ = 0, trace_yaw_moves_ = 0, trace_yaw_octants_ = 0;
  int phase_ = 0;
  std::array<bool, platform::Input::kKeys> held_{};
  std::array<bool, platform::Input::kKeys> prev_held_{};
  float stick_x_ = 0, stick_y_ = 0;
  bool stick_on_ = false;
  // `pad`: the virtual pad is connected; its held buttons this tick and the last.
  bool pad_on_ = false;
  std::array<bool, platform::Input::kButtons> pad_held_{};
  std::array<bool, platform::Input::kButtons> pad_prev_held_{};
  int quiet_ = 0;
  u64 stuck_tick_ = 0;
  float stuck_x_ = 0, stuck_z_ = 0;
  u32 fight_step_ = 0;
  // Autoshot state.
  int as_every_ = 0, as_left_ = 0, as_n_ = 0;
  u64 as_next_ = 0;
  std::string as_prefix_;
  // `snap` / `assert same`.
  struct Snap {
    std::string room, flow, title_seq, battle_sig;
    u64 room_loads = 0, art_swaps = 0, battle_starts = 0, title_plays = 0, field_ticks = 0;
    std::vector<int> dialogue;
    bool battle = false;
    struct EntState {
      bool ok = false;
      float x = 0, z = 0;
      int facing = 0, yaw = 0;
    };
    std::vector<EntState> ents;
    std::vector<std::pair<int, int>> vars;
  } snap_;
};

// ---- recorder / replayer --------------------------------------------------------------------------------------------
// File: first line "OCREPLAY 1 <room> <entrance>", then one line per rendered frame:
//   <dt_microseconds> <ticks> <held scancodes, comma separated or -> <pressed scancodes or -> <pad_connected> <pad_down hex>
//   <pad_pressed hex> <axes x1000 comma separated>
class InputRecorder {
 public:
  bool open(const std::filesystem::path& file, const std::string& room, int entrance);
  void write(double dt, int ticks, const platform::Input& in);
  void close() { out_.close(); }
  bool active() const { return out_.is_open(); }

 private:
  std::ofstream out_;
};

struct ReplayFrame {
  double dt = 0;
  int ticks = 0;
  platform::Input in;
};

class InputReplayer {
 public:
  bool load(const std::filesystem::path& file, std::string* err);
  // Fills `f` with the next frame; false at the end.
  bool next(ReplayFrame& frame);
  const std::string& room() const { return room_; }
  int entrance() const { return entrance_; }
  size_t frames() const { return lines_.size(); }

 private:
  std::string room_;
  int entrance_ = 0;
  std::vector<std::string> lines_;
  size_t pos_ = 0;
  platform::Input prev_;
};

}  // namespace oc::game
