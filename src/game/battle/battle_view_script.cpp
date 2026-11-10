// BattleView: the battle-script event stream and the system message window (tag expansion, paging, input).
#include "audio/voice.h"
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "import/text.h"

namespace oc::game {

using namespace bv;

namespace {

// Battle ids whose btel_<lang>.csv rows live under another id (first match wins).
int btel_battle_id(int bid) {
  static constexpr int kRemap[][2] = {{4, 0x199},     {5, 0x19B},     {0xA, 0x19C},   {0xB, 0x196},   {0x34, 0x31},
                                      {0x35, 0x32},   {0x129, 0x128}, {0x12C, 0x129}, {0x158, 0x157}, {0x19A, 0x199},
                                      {0x1A8, 0x19E}, {0x1E3, 0x1A5}, {0x1E4, 0x1A6}, {0x1E5, 0x1DE}};
  for (const auto& remap : kRemap)
    if (remap[0] == bid) return remap[1];
  return bid;
}

// Expand a page: the text before the (cut + 1)-th "{wait(" tag, {playwork(0)} = `actor`, a literal backslash-n
// = LF; the other tags are kept (the window's text draw skips every tag; {color(n)} colours are handled by smsg_colour).
std::string smsg_expand(const std::string& src, int cut, const std::string& actor, const std::string& item = {}) {
  std::string out;
  size_t pos = 0;
  while (pos < src.size()) {
    const size_t open = src.find('{', pos);
    if (open == std::string::npos) {
      out += src.substr(pos);
      break;
    }
    const size_t close = src.find('}', open);
    out += src.substr(pos, open - pos);
    if (close == std::string::npos) break;
    const std::string tag = src.substr(open, close + 1 - open);
    pos = close + 1;
    if (tag == "{playwork(0)}") {
      out += actor;
    } else if (tag == "{itemwork(0)}") {
      // The stolen item / element (item id, element id + 0x200); empty for a btel row.
      out += item;
    } else if (tag.rfind("{wait(", 0) == 0) {
      if (--cut < 0) break;
    } else {
      out += tag;
    }
  }
  for (size_t k = 0; (k = out.find("\\n", k)) != std::string::npos;) out.replace(k, 2, "\n");
  return out;
}

std::string smsg_strip_tags(const std::string& src) {
  std::string out;
  for (size_t i = 0; i < src.size(); ++i) {
    if (src[i] == '{') {
      const size_t close = src.find('}', i);
      if (close == std::string::npos) break;
      i = close;
      continue;
    }
    out += src[i];
  }
  return out;
}

// n of the first "{name(n)}", -1 if absent.
int smsg_tag_arg(const std::string& src, const char* name) {
  const size_t pos = src.find(name);
  return pos == std::string::npos ? -1 : std::atoi(src.c_str() + pos + std::strlen(name));
}

int smsg_count(const std::string& src, const char* what) {
  int count = 0;
  for (size_t at = 0; (at = src.find(what, at)) != std::string::npos; at += std::strlen(what)) ++count;
  return count;
}

// {color(n)}: white, green, purple, black, red, yellow, blue, grey. The word is B << 16 | G << 8 | R; the default text colour 0x757575 is the
// 128 white of the table scaled by 0x75 / 0x80, so are the others.
u32 smsg_colour(int index) {
  static const u8 kRgb[8][3] = {{128, 128, 128}, {0, 128, 0}, {96, 0, 96}, {0, 0, 0}, {128, 0, 0}, {128, 128, 0}, {32, 32, 128}, {96, 96, 96}};
  const u8* rgb = kRgb[std::clamp(index, 0, 7)];
  auto scale = [](u32 value) { return (value * 0x75u + 0x40u) / 0x80u; };
  return (scale(rgb[2]) << 16) | (scale(rgb[1]) << 8) | scale(rgb[0]);
}

struct SmsgSeg {
  std::string text;
  u32 colour = 0x757575;
};

// The expanded page as lines of colour segments: every tag but {color(n)} is skipped (the window draw 0x83 does the same); the colour persists to
// the next tag, also across lines.
std::vector<std::vector<SmsgSeg>> smsg_segments(const std::string& src) {
  std::vector<std::vector<SmsgSeg>> lines(1);
  u32 col = 0x757575;
  std::string cur;
  auto flush = [&] {
    if (!cur.empty()) lines.back().push_back({cur, col});
    cur.clear();
  };
  for (size_t i = 0; i < src.size(); ++i) {
    if (src[i] == '{') {
      const size_t close = src.find('}', i);
      if (close == std::string::npos) break;
      if (src.compare(i, 7, "{color(") == 0) {
        flush();
        const int colour_index = std::atoi(src.c_str() + i + 7);
        col = colour_index == 0 ? 0x757575u : smsg_colour(colour_index);
      }
      i = close;
    } else if (src[i] == '\n') {
      flush();
      lines.emplace_back();
    } else {
      cur += src[i];
    }
  }
  flush();
  return lines;
}

// The message window grows 8 steps per frame; the half extent counts them down.
constexpr int kSmsgGrowStep = 8;

// The events that hold the stream until the action executor is idle: the blocking ops, and the start of an attacked (slot 4) run.
bool holds_stream(const battle::pac::Event& event) {
  return event.kind == battle::pac::Event::Kind::Barrier || (event.kind == battle::pac::Event::Kind::Mark && event.a == 4);
}
// PS1 y -> HUD canvas.
constexpr float kSmsgPY = kCanvasH / 216.0f;

// The 4-char tag of ops C0 / C2 / C4 / C5 (little-endian word).
std::string tag_name(int word) {
  const char tag[5] = {static_cast<char>(word & 0xFF), static_cast<char>((word >> 8) & 0xFF), static_cast<char>((word >> 16) & 0xFF),
                       static_cast<char>((word >> 24) & 0xFF), 0};
  return tag;
}

}  // namespace

// First present unit whose model's S6 DRP (its `cam` bank) holds the shot / timeline `name` (a resident registry file), or null.
const CamBank* BattleView::resident_cam_bank(const std::string& name, bool sequence) const {
  for (int unit = 0; unit < battle::kUnits; ++unit) {
    const Visual& visual = vis_[static_cast<size_t>(unit)];
    if (!bunit(unit).present || !visual.scripts) continue;
    if (sequence ? visual.scripts->cam.sequence(name) != nullptr : visual.scripts->cam.camera(name) != nullptr) return &visual.scripts->cam;
  }
  return nullptr;
}

void BattleView::script_event(const battle::pac::Event& event) {
  using EK = battle::pac::Event::Kind;
  const bool unit_ok = event.unit >= 0 && event.unit < battle::kUnits && bunit(event.unit).present;
  switch (event.kind) {
    // E0: only reached while no window is open (script_pump); opens the window and goes on.
    case EK::Dialogue: {
      ScriptMsgReq req;
      req.unit = event.unit;
      req.msg = event.a;
      // Op 62 sets the position (x, y) for the next window only.
      if (smsg_pos_set_) {
        req.has_pos = true;
        req.x = smsg_pos_x_;
        req.y = smsg_pos_y_;
        smsg_pos_set_ = false;
      }
      smsg_open(req);
      break;
    }
    // 2C / 38: the HUD follows the stream, not the script that ran ahead.
    case EK::Cutscene:
      hud_off_ = event.a != 0;
      if (event.a != 0) orbit_block_next_ = true;
      break;
    case EK::EngineFlag:
      if (event.a == 0x39) hud_script_off_ = event.b == 0;
      break;
    // E2: waited for in script_pump (the window closes on the button / {wend}, smsg_tick / smsg_input).
    case EK::WaitDialogue: break;
    // Ordering only (script_pump).
    case EK::Queue: case EK::Barrier: case EK::Mark: break;
    case EK::DialogPos:
      smsg_pos_set_ = true;
      smsg_pos_x_ = event.a;
      smsg_pos_y_ = event.b;
      break;
    // 85: the home facing becomes yaw a << 6 (4096 = 360 deg) and the model turns to it at once.
    case EK::HomeFacing:
      if (unit_ok) {
        Visual& visual = vis_[static_cast<size_t>(event.unit)];
        const float yaw = static_cast<float>(event.a) * (2.0f * kPi / 64.0f);
        visual.home_fx = visual.face_x = visual.pface_x = -std::sin(yaw);
        visual.home_fz = visual.face_z = visual.pface_z = -std::cos(yaw);
      }
      break;
    case EK::Pose:  // 3D: the pose = a, read by the idle script (GetPose); 8 = dead, 1 = stand-by
      if (unit_ok) {
        Visual& visual = vis_[static_cast<size_t>(event.unit)];
        visual.script_pose = event.a;
        if (visual.script_kind == kIdle) start_idle(event.unit);
      }
      break;
    case EK::Anim:  // 58 / 5A: start script a; b = 1: only when no anmseq stream runs
      if (unit_ok) {
        Visual& visual = vis_[static_cast<size_t>(event.unit)];
        if (event.b && !visual.script_done) break;
        const bool ext = event.a >= 0x80;
        const bseq::Table* table = !visual.scripts ? nullptr : ext ? (visual.scripts->has_atk ? &visual.scripts->atk : nullptr) : (visual.scripts->has_s5 ? &visual.scripts->s5 : nullptr);
        start_script(event.unit, table, ext ? event.a - 0x80 : event.a, 0, event.a == 1 ? kIdle : kNone);
      }
      break;
    // 7A / 86: the acting list = {unit} until the next command setup rewrites it (anchor_frame ref 1).
    case EK::Camera:
      script_actor_ = unit_ok ? event.unit : -1;
      break;
    // C0: a type-0x10 timeline by tag; C2: a type-0x0C shot by tag.
    case EK::EffectStart: {
      const std::string name = tag_name(event.b);
      const bool ok = event.a == 0 ? camera_.play_sequence(name) : camera_.select(name);
      log::debug("battle script: {} '{}' {}", event.a == 0 ? "timeline" : "shot", name, ok ? "started" : "missing");
      break;
    }
    case EK::EffectStop:  // C1: ends every timeline and clears the screen copy warp; C3: ends the shot
      if (event.a == 0) {
        camera_.stop_sequences();
        fx_warp_.on = false;
      }
      break;
    case EK::Killed: log::debug("battle script: unit {} kill script end (A6) at tick {}", event.unit, tick_count_); break;
    // 84: the low-HP bit (anmseq flag 31) until the next hit on the unit.
    case EK::QuarterHp:
      if (unit_ok) vis_[static_cast<size_t>(event.unit)].low_hp_forced = true;
      break;
    // B0 / B1: the height tween (tick_units).
    case EK::Lift:
      if (unit_ok) vis_[static_cast<size_t>(event.unit)].lift = {event.a, event.b, 0};
      break;
    // C4 / C5: held in script_pump while the effect object lives.
    case EK::EffectWait: break;
    default:
      // SyncPos (91), Invuln,
      // SfxMute, ScriptedDeath (5C: Battle::check_end), EngineFlag (7D / 81 / AF: VM state; 7D is read by the target label),
      // SetResult, NoDrops, Defeated, Ghost, MenuCursor: consumed by the rules.
      break;
  }
}

// The event window. Size (non-JP fitting, also used for JP here): cells = clamp((widest line + 11) / 12, 1, 26), lines =
// clamp(most lines of one chunk, 1, 5); empty pieces followed by a LF are not counted. Place: text x = 32 + (22 - cells) * 6, rect
// y 0x2C, text y 0x2E; after op 62 rect (x, y), text (x, y + 1). Rect w = cells * 12 & ~3, h = lines * 16 (grows from its
// centre by 8 px a frame, the text shows once it is open). A system request opens the plate instead and leaves the event window alone.
void BattleView::smsg_open(const ScriptMsgReq& req) {
  ScriptMsg& msg = req.sys ? plate_ : smsg_;
  msg = ScriptMsg{};
  msg.sys = req.sys;
  msg.item = req.item;
  const std::string sys_row = req.text;
  const std::string* row = req.sys ? &sys_row : lang_tables_.battle_event(static_cast<u32>(btel_battle_id(scene_id_)), static_cast<u32>(req.msg), lang_);
  if (!row || row->empty()) {
    log::debug("battle script: message {} of battle {} has no btel row", req.msg, scene_id_);
    return;
  }
  std::string text = *row;
  if (text.rfind("{size ", 0) == 0 && text.find('}') != std::string::npos) text.erase(0, text.find('}') + 1);
  for (size_t from = 0;;) {
    const size_t bpage = text.find("{bpage}", from);
    std::string chunk = text.substr(from, bpage == std::string::npos ? std::string::npos : bpage - from);
    if (from > 0 && chunk.rfind("\\n", 0) == 0) chunk.erase(0, 2);
    msg.pages.push_back(std::move(chunk));
    if (bpage == std::string::npos) break;
    from = bpage + 7;
  }
  // {playwork(0)} = the current actor's name; the script op carries no unit, so the first PC.
  msg.unit = req.unit >= 0 && req.unit < battle::kUnits ? req.unit : 0;
  const std::string actor = bunit(msg.unit).name;
  const bool jp = lang_ == import::Lang::Jp;
  const int slot = jp ? ui::font::kKb : ui::font::kBn;
  const float sc = ui::font::slot(slot).scale;
  float maxw = 0;
  int maxl = 0;
  for (const std::string& page : msg.pages) {
    const std::string expanded = smsg_strip_tags(smsg_expand(page, 0x7FFFFFFF, actor, msg.item));
    int lines = 0;
    for (size_t start = 0;;) {
      const size_t lf = expanded.find('\n', start);
      // An empty piece followed by a LF is neither measured nor counted.
      if (lf == start) {
        start = lf + 1;
        continue;
      }
      const std::u32string piece = import::utf8_to_u32(expanded.substr(start, lf == std::string::npos ? std::string::npos : lf - start));
      fonts_.prepare(slot, piece);
      maxw = std::max(maxw, std::trunc(fonts_.measure(slot, piece, sc) / kPs1Px));
      ++lines;
      if (lf == std::string::npos) break;
      start = lf + 1;
    }
    maxl = std::max(maxl, lines);
  }
  msg.cells = std::clamp((static_cast<int>(maxw) + 11) / 12, 1, 26);
  msg.lines = std::clamp(maxl, 1, 5);
  if (req.has_pos) {
    msg.x = static_cast<float>(req.x);
    msg.y = static_cast<float>(req.y);
    // The system text starts at the rect origin (0x20, 0x90).
    msg.ty = req.sys ? msg.y : msg.y + 1;
  } else {
    msg.x = static_cast<float>(32 + (22 - msg.cells) * 6);
    msg.y = 0x2C;
    msg.ty = 0x2E;
  }
  msg.open = 1;
  route_events_.push_back("w" + std::to_string(req.msg));
  // Voices: voices/battle/<scene>/<msg>-<page>-<unit>.mp3.
  if (audio::VoicePlayer* voices = audio::VoicePlayer::instance(); voices && !req.sys) voices->battle_open(scene_id_, req.msg, actor);
  log::debug("battle script: message {} ({} page(s), {} x {} cells) at ({}, {}) at tick {}: {}", req.msg, msg.pages.size(), msg.cells, msg.lines, msg.x, msg.y, tick_count_, *row);
}

// The executor (queue_ + act_) has finished everything that was queued before the Barrier: the groups of earlier scripted entries (script_seq below
// the barrier's seq) and every unscripted group up to the action id the barrier was raised in (a PC's attack before the enemy's turn).
bool BattleView::script_barrier_clear(const battle::pac::Event& barrier) const {
  const auto relevant = [&](const battle::ActionEvent& group) { return group.script_seq >= 0 ? group.script_seq < barrier.seq : group.action <= barrier.b; };
  // An approach / return walk (no hits): the PC's command state is not idle yet.
  if (act_.phase != Act::Phase::None && act_.ev.empty()) return false;
  if (act_.phase != Act::Phase::None && !act_.ev.empty() && relevant(act_.ev.front())) return false;
  // 66 / 65 / 2C wait for the PC's command state to end: it must be home.
  if (engaged_ && (barrier.a == 0x66 || barrier.a == 0x65 || barrier.a == 0x2C)) return false;
  for (const std::vector<battle::ActionEvent>& group : queue_)
    if (!group.empty() && relevant(group.front())) return false;
  return true;
}

// The VM runs a slot synchronously, so brain_.hidden(u) is already true while the view still replays the actions queued BEFORE the
// script's ghost op. Op 34 / 35 / 5B must take effect only after the blocking ops in front of it (A4 / 2C / 65 / 66 waits on
// the action executor), so evt409's right pick (`queue_attack self` 'presents', A4, 2C, set_result 0, ghost_self) keeps the
// boxer on stage for the whole pack; the Ghost(set) event releases after the Barrier.
bool BattleView::ghosted(int unit) const {
  bool barrier = false;
  for (const battle::pac::Event& event : script_stream_) {
    if (holds_stream(event)) barrier = true;
    else if (event.kind == battle::pac::Event::Kind::Ghost && event.unit == unit) {
      if (barrier && event.a != 0) return false;
      break;
    }
  }
  // A pack hid the model for the rest of the action (FxModel::conceal).
  if (fx_conceal_[static_cast<size_t>(unit)]) return true;
  return ghost_view_[static_cast<size_t>(unit)] || brain_.hidden(unit);
}

// One call per game tick. The scripts run synchronously, so the recorded events are released here in
// the order and at the pace the live VM would have produced them (see battle_view.h, "Battle-script stream").
void BattleView::script_pump() {
  if (ui_ == Ui::Intro) return;
  for (int guard = 0; guard < 256 && !script_stream_.empty(); ++guard) {
    const battle::pac::Event& front = script_stream_.front();
    using EK = battle::pac::Event::Kind;
    if (script_gap_ < 0) script_gap_ = front.vt > script_prev_vt_ && script_prev_vt_ != 0 ? static_cast<int>(front.vt - script_prev_vt_) : 0;
    // A 2D delay (2n ticks) or the ticks the script took.
    if (script_gap_ > 0) {
      --script_gap_;
      return;
    }
    if (front.kind == EK::Dialogue || front.kind == EK::WaitDialogue) {
      // E0 blocks while the event window is not closed; E2 until it has closed. The system plate is another window.
      if (smsg_.open != 0) return;
    } else if (front.kind == EK::Killed) {
      // A6 waits for the move-wait; the barrier's watchdog applies.
      if (move_wait_ != 0 && ++script_stall_ < 3600) return;
      if (script_stall_ >= 3600) {
        battle_audit_note(std::format("script A6 of unit {} left by the 3600-tick timeout (move-wait {} of unit {})", front.unit, move_wait_, move_wait_unit_));
        move_wait_ = 0;
      }
      script_stall_ = 0;
    } else if (front.kind == EK::EffectWait && front.a == 1 && camera_.shot_running(tag_name(front.b))) {
      // C5: until the shot C2 started with that tag has run out.
      return;
    } else if (holds_stream(front)) {
      // The PC walks home first.
      if (engaged_ && act_.phase == Act::Phase::None && queue_.empty() && (front.a == 0x66 || front.a == 0x65 || front.a == 0x2C)) begin_disengage();
      if (!script_barrier_clear(front) && ++script_stall_ < 3600) return;
      if (script_stall_ >= 3600) battle_audit_note(std::format("script barrier op {:#x} left by the 3600-tick timeout (phase {} queue {} engaged {})", front.a, static_cast<int>(act_.phase), queue_.size(), engaged_));
      script_stall_ = 0;
    }
    const battle::pac::Event ev = front;
    script_stream_.pop_front();
    script_gap_ = -1;
    script_prev_vt_ = std::max<u32>(ev.vt, 1);
    script_passed_ = ev.seq + 1;
    script_event(ev);
    // The window opens now; whatever follows (E2) is looked at on the next tick.
    if (ev.kind == EK::Dialogue) return;
  }
}

// Per tick: the opening / closing animation, the {wait(n)} reveal steps and the {wend(n)} self-close (n frames each).
void BattleView::smsg_tick() {
  tick_window(smsg_);
  tick_window(plate_);
}

void BattleView::tick_window(ScriptMsg& msg) {
  if (msg.open == 0) return;
  if (audio::VoicePlayer* voices = audio::VoicePlayer::instance(); voices && !msg.sys) voices->battle_sync(static_cast<int>(msg.page), msg.open < 0 || msg.page >= msg.pages.size());
  const int grow_frames = msg.grow_frames(kSmsgGrowStep);
  // Closing.
  if (msg.open < 0) {
    if (++msg.open >= 0) {
      msg.open = 0;
      log::debug("battle script: message closed at tick {}", tick_count_);
      msg.pages.clear();
    }
    return;
  }
  ++msg.open;
  if (msg.open <= grow_frames || msg.page >= msg.pages.size()) return;
  const std::string& pg = msg.pages[msg.page];
  // The next {wait( of this page: n frames, then the text up to the one after it.
  if (msg.waits < smsg_count(pg, "{wait(")) {
    if (msg.wait_left < 0) {
      size_t at = 0;
      for (int k = 0; k <= msg.waits; ++k) at = pg.find("{wait(", k == 0 ? 0 : at + 6);
      msg.wait_left = std::max(0, std::atoi(pg.c_str() + at + 6));
    }
    if (msg.wait_left-- <= 0) {
      ++msg.waits;
      msg.wait_left = -1;
    }
    return;
  }
  if (msg.wend < 0) {
    const int wend_frames = smsg_tag_arg(pg, "{wend(");
    if (wend_frames >= 0) msg.wend = wend_frames + 1;
  }
  // {wend}: the page / window ends without the button.
  if (msg.wend > 0 && --msg.wend == 0) {
    msg.wend = -1;
    msg.waits = 0;
    if (++msg.page >= msg.pages.size()) msg.open = -grow_frames;
  }
}

bool BattleView::smsg_input(bool ok) {
  if (smsg_.open == 0) return false;
  if (smsg_.open < 0 || smsg_.page >= smsg_.pages.size()) return true;
  const int grow_frames = smsg_.grow_frames(kSmsgGrowStep);
  const std::string& pg = smsg_.pages[smsg_.page];
  // A page end ({bpage}) or the message end ({mend}) goes on with the confirm button, unless {wend} ends it.
  if (ok && smsg_.open > grow_frames && smsg_.waits >= smsg_count(pg, "{wait(") && smsg_tag_arg(pg, "{wend(") < 0) {
    smsg_.waits = 0;
    smsg_.wait_left = -1;
    if (++smsg_.page >= smsg_.pages.size()) smsg_.open = -grow_frames;
  }
  return true;
}

// Window drawing: while it opens it draws only the white chamfered outline, centred on the rect
// and sized 16 + w * n / half by 16 + h * n / half (n = the steps done, 8 a frame, half = max(w, h) / 2; the box gets size - 2), so the last frame before
// the snap equals the outer skin; when open it draws the battle window skin (the command box's: battle_04.tim, 8 px frame outside the
// rect, subtractive fill) round the rect. Closing runs the outline back down. The text is the 0x83 draw (16 px lines, {color(n)}
// segments); the page / end cursor is the battle_86 cell ((tick & 0x3F) >> 3), 16 x 16 PS1 px at (text x + 8 + the last line's
// width, text y + 16 * its line), shown while the stream waits on {bpage} / {mend} (page revealed, no {wend}).
void BattleView::build_script_msg() {
  build_window(plate_);
  build_window(smsg_);
}

void BattleView::build_window(const ScriptMsg& msg) {
  if (msg.open == 0) return;
  const float w = msg.rect_w(), h = msg.rect_h();
  const int half = std::max(1, std::max(static_cast<int>(w), static_cast<int>(h)) / 2);
  const int grow_frames = msg.grow_frames(kSmsgGrowStep);
  const int age = msg.open > 0 ? msg.open : -msg.open;
  // The system message has no outline frames.
  if (msg.open < 0 && msg.sys) return;
  if (msg.open < 0 || age < grow_frames) {
    const float steps = static_cast<float>(std::min(age * kSmsgGrowStep, half));
    const float cw = 16.0f + w * steps / static_cast<float>(half), ch = 16.0f + h * steps / static_cast<float>(half);
    const float x0 = msg.x + std::trunc(w * 0.5f) - std::trunc(cw * 0.5f), y0 = msg.y + std::trunc(h * 0.5f) - std::trunc(ch * 0.5f);
    add_box(x0 * kPs1Px, y0 * kSmsgPY, (cw - 2.0f) * kPs1Px, (ch - 2.0f) * kSmsgPY, false);
    return;
  }
  add_skin_box((msg.x - 8.0f) * kPs1Px, (msg.y - 8.0f) * kSmsgPY, (w + 16.0f) * kPs1Px, (h + 16.0f) * kSmsgPY);
  if (msg.open <= grow_frames || msg.page >= msg.pages.size()) return;
  const std::string actor = bunit(msg.unit).name;
  const std::string& pg = msg.pages[msg.page];
  const auto lines = smsg_segments(smsg_expand(pg, msg.waits, actor, msg.item));
  const bool jp = lang_ == import::Lang::Jp;
  const int slot = jp ? ui::font::kKb : ui::font::kBn;
  const float sc = ui::font::slot(slot).scale;
  float last_w = 0;
  for (size_t line = 0; line < lines.size(); ++line) {
    float x = 0;
    for (const SmsgSeg& seg : lines[line]) {
      const std::u32string text = import::utf8_to_u32(seg.text);
      btext(0x83, text, msg.x + x, msg.ty + 16.0f * static_cast<float>(line), seg.colour);
      fonts_.prepare(slot, text);
      x += std::trunc(fonts_.measure(slot, text, sc) / kPs1Px);
    }
    last_w = x;
  }
  if (msg.waits >= smsg_count(pg, "{wait(") && smsg_tag_arg(pg, "{wend(") < 0) {
    const int cell = (tick_count_ & 0x3F) >> 3;
    add_img(hud_[kHPageCur], (msg.x + 8.0f + last_w) * kPs1Px, (msg.ty + 16.0f * static_cast<float>(lines.size() - 1)) * kSmsgPY, 16 * kPs1Px, 16 * kSmsgPY,
            static_cast<float>(cell) / 8.0f, 0, static_cast<float>(cell + 1) / 8.0f, 1);
  }
}

}  // namespace oc::game
