// BattleView in a versus battle (rules/versus.md): the rules start from the shared inputs, the perspective (the game that plays side B
// shows units 3..5 as its party), the commands of both sides with the link (VersusLink: the other side's in the order its game applied
// them, the state hash after every command), the 60 s turn clock, the turn banner, the Run confirm and the Victory / Defeat / Draw plate.
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "core/log.h"
#include "import/text.h"

namespace oc::game {

using namespace bv;

namespace {

constexpr float kRowScale = kCanvasH / 216.0f;
// Seeds the random player of this game's own side apart from the battle's generator.
constexpr u32 kAutoSalt = 0x5A17C0DEu;
// OpenCross text (no game text exists for it), per language: English, French, Italian, German, Spanish, Japanese.
const char* kYourTurn[6] = {"Your turn", "Votre tour", "Il tuo turno", "Dein Zug", "Tu turno", "あなたのターン"};
const char* kTheirTurn[6] = {"{}'s turn", "Tour de {}", "Turno di {}", "{} ist am Zug", "Turno de {}", "{}のターン"};
const char32_t* kSurrender[6] = {U"Surrender?", U"Abandonner ?", U"Arrendersi?", U"Aufgeben?", U"¿Rendirse?", U"降参しますか？"};
const char32_t* kYes[6] = {U"Yes", U"Oui", U"Sì", U"Ja", U"Sí", U"はい"};
const char32_t* kNo[6] = {U"No", U"Non", U"No", U"Nein", U"No", U"いいえ"};
const char* kWaitingFor[6] = {"Waiting for {}... {}", "En attente de {}... {}", "In attesa di {}... {}", "Warte auf {}... {}", "Esperando a {}... {}",
                              "{}を待っています... {}"};
const char* kReconnecting[6] = {"Reconnecting... {}", "Reconnexion... {}", "Riconnessione... {}", "Verbinde neu... {}", "Reconectando... {}",
                                "再接続中... {}"};

// The windows: a line of the window text (style 0x83) is 10 px high with the text at its top; a window's interior is as wide as its text
// in whole 12 px cells. The Run confirm's cursor stands this far left of its words.
constexpr float kWindowLineH = 10, kAskCursorGap = 12;
float window_cells(float width) { return static_cast<float>((static_cast<int>(width) + 11) / 12 * 12); }

}  // namespace

battle::Command BattleView::rules_command(battle::Command cmd) const {
  cmd.actor = rules_unit(cmd.actor);
  cmd.target = rules_unit(cmd.target);
  return cmd;
}

bool BattleView::apply_command(const battle::Command& cmd, std::string* why) {
  const battle::Command rules = rules_command(cmd);
  return versus_ ? versus_apply(rules, true, why) : battle_->apply(rules, why);
}

void BattleView::open_menu_now() {
  if (!versus_) {
    battle_->open_menu(active_pc_);
    return;
  }
  if (versus_focus_ == active_pc_) return;
  versus_focus_ = active_pc_;
  battle::Command cmd;
  cmd.kind = battle::Command::Kind::Focus;
  cmd.actor = rules_unit(active_pc_);
  versus_apply(cmd, true);
}

void BattleView::open_element_menu_now() {
  if (!versus_) {
    battle_->open_element_menu(active_pc_);
    return;
  }
  battle::Command cmd;
  cmd.kind = battle::Command::Kind::ElementMenu;
  cmd.actor = rules_unit(active_pc_);
  versus_apply(cmd, true);
}

bool BattleView::versus_apply(const battle::Command& cmd, bool local, std::string* why) {
  const int turn = battle_->turn_side();
  if (!battle_->apply(cmd, why)) return false;
  ++versus_seq_;
  if (versus_link_) {
    if (local) versus_link_->send(cmd);
    versus_link_->send_hash(versus_seq_, battle_->state_hash());
  }
  // The next menu names its PC again after an action, an engage (a HIT% menu left sends its attacker home on the other game) or a turn
  // change.
  if (cmd.kind < battle::Command::Kind::EndTurn || cmd.kind == battle::Command::Kind::Engage || battle_->turn_side() != turn) versus_focus_ = -1;
  // A menu opened again (its HIT% menu left, or another character's) or the turn ended: no hit of the standing chain follows.
  if (stay_unit_ >= 0 && (cmd.kind == battle::Command::Kind::Focus || cmd.kind == battle::Command::Kind::EndTurn)) stay_left_ = true;
  return true;
}

bool BattleView::stay_holds() const {
  if (battle_->outcome() != battle::Outcome::Ongoing) return true;
  return !stay_left_ && battle_->can_act(rules_unit(stay_unit_)) && bunit(stay_target_).standing();
}

bool BattleView::stay_continues(const battle::ActionEvent& event) const {
  return !stay_left_ && event.kind == battle::ActionEvent::Kind::Weapon && event.actor == stay_unit_ && (event.aim >= 0 ? event.aim : event.target) == stay_target_;
}

void BattleView::end_stay() {
  const int unit = stay_unit_;
  stay_unit_ = stay_target_ = -1;
  stay_left_ = false;
  end_camera_tasks();
  Visual& visual = vis_[static_cast<size_t>(unit)];
  if (!bunit(unit).alive()) {
    begin_move(unit, visual.home_x, visual.home_z, 0, true);
    return;
  }
  act_ = Act{};
  act_.actor = unit;
  start_return();
}

// The other side's (or this side's random player's) confirmed attack target: its attacker runs up to it as a HIT% menu's PC does and
// stands there as a chain's attacker, until its hits come or a Focus or End Turn sends it home.
void BattleView::stage_engage(int unit, int target) {
  if (stay_unit_ == unit && stay_target_ == target) return;
  stay_unit_ = unit;
  stay_target_ = target;
  stay_left_ = false;
  act_ = Act{};
  act_.actor = unit;
  act_.targets = {target};
  act_.stage_only = true;
  Visual& visual = vis_[static_cast<size_t>(unit)];
  audio_.activate(visual.sound_key);
  end_camera_tasks();
  camera_.set_bank(visual.scripts ? &visual.scripts->cam : &bank_);
  face_toward(unit, target);
  select_approach();
  act_.phase = Act::Phase::Approach;
  if (!(visual.scripts && visual.scripts->has_s5 && start_script(unit, &visual.scripts->s5, 2, 0, kApproach))) {
    visual.script_done = true;
    const float dist = approach_distance(unit, target);
    const Visual& target_visual = vis_[static_cast<size_t>(target)];
    const float dx = target_visual.x - visual.x, dz = target_visual.z - visual.z, length = std::max(1.0f, std::hypot(dx, dz));
    begin_move(unit, visual.x + dx / length * dist, visual.z + dz / length * dist, 30, false);
  }
}

void BattleView::map_event(battle::ActionEvent& event) const {
  if (!versus_flip_) return;
  event.actor = rules_unit(event.actor);
  event.target = rules_unit(event.target);
  event.aim = rules_unit(event.aim);
  for (u8& unit : event.group)
    if (unit < battle::kUnits) unit = static_cast<u8>(rules_unit(unit));
  event.trap = view_trap(event.trap);
  event.trap_before = view_trap(event.trap_before);
}

battle::TrapHud BattleView::view_trap(battle::TrapHud trap) const {
  if (versus_flip_) std::swap(trap.pc, trap.enemy);
  return trap;
}

battle::Outcome BattleView::presented_outcome() const {
  const battle::Outcome outcome = battle_->outcome();
  if (!versus_) return outcome;
  if (versus_result_ == VersusResult::Void) return battle::Outcome::Stalled;
  if (outcome == battle::Outcome::Ongoing) return outcome;
  return battle_->winner() == versus_side_ ? battle::Outcome::Victory : battle::Outcome::Wiped;
}

bool BattleView::start_versus_rules(const VersusSetup& setup) {
  // The scene script's state of an earlier battle goes (no script runs in a versus battle).
  brain_.begin_battle(*battle_, *rng_);
  if (!battle_->start_versus(setup.side_a, setup.side_b, setup.battlefield, setup.opening_stamina)) return false;
  versus_seed_ = setup.seed;
  versus_seq_ = 0;
  versus_focus_ = versus_turn_seen_ = -1;
  versus_turn_ticks_ = versus_idle_ = 0;
  versus_held_.reset();
  versus_auto_held_.reset();
  versus_auto_.reset();
  versus_run_ask_ = false;
  versus_opponent_ = setup.opponent;
  versus_stars_ = battle_->star_counter(versus_side_);
  log::info("battle: versus seed {} side {} battlefield {} opening stamina {} against {}", setup.seed, versus_side_ == 0 ? 'A' : 'B', setup.battlefield,
            setup.opening_stamina, setup.opponent);
  return true;
}

// The link first (an ended link ends the battle), then the turn clock of this game's side; then, once the view has been idle a moment, the
// next commands of whoever holds the turn: the other game's through the link, this game's own from the random player under set_auto. One
// action is applied at a time, with the state commands that go with it (the Focus before it, the Dopplegang pick after it).
void BattleView::versus_pump(int ticks, bool idle) {
  if (!versus_ || finish_ != Finish::None || ui_ == Ui::Intro || battle_->outcome() != battle::Outcome::Ongoing) return;
  if (versus_link_) {
    switch (versus_link_->status()) {
      case VersusLink::Status::Open: break;
      case VersusLink::Status::Desync:
      case VersusLink::Status::Closed:
        log::warn("battle: versus link {} after command {}: no result", versus_link_->status() == VersusLink::Status::Desync ? "out of step" : "closed",
                  versus_seq_);
        versus_result_ = VersusResult::Void;
        begin_finish();
        return;
      case VersusLink::Status::Conceded:
      case VersusLink::Status::Forfeited:
        log::info("battle: versus link: {} side forfeits after command {}", versus_link_->status() == VersusLink::Status::Conceded ? "the other" : "this",
                  versus_seq_);
        battle_->concede(versus_link_->status() == VersusLink::Status::Conceded ? 1 - versus_side_ : versus_side_);
        collect_events();
        return;
    }
  }
  const int turn = battle_->turn_side();
  if (turn != versus_turn_seen_) {
    versus_turn_seen_ = turn;
    versus_turn_ticks_ = 0;
    versus_run_ask_ = false;
    versus_focus_ = -1;
  }
  versus_idle_ = idle ? versus_idle_ + ticks : 0;
  if (!idle) return;
  const int acting = battle_->acting_side();
  const bool local = acting == versus_side_;
  if (local && (versus_turn_ticks_ += ticks) >= kVersusTurnTicks) {
    battle::Command end;
    end.kind = battle::Command::Kind::EndTurn;
    end.actor = battle::side_first(versus_side_);
    end.timeout = true;
    log::info("battle: versus turn of side {} timed out", versus_side_ == 0 ? 'A' : 'B');
    versus_apply(end, true);
    versus_run_ask_ = false;
    hdr_on_ = false;
    if (engaged_) begin_disengage();
    ui_ = Ui::Wait;
    collect_events();
    return;
  }
  if (local && auto_ && !versus_auto_) versus_auto_ = std::make_unique<LocalVersusOpponent>(versus_side_, versus_seed_ ^ kAutoSalt);
  VersusLink* source = local ? (auto_ ? versus_auto_.get() : nullptr) : versus_link_;
  if (!source || versus_idle_ < kVersusActionGap) return;
  std::optional<battle::Command>& held = local ? versus_auto_held_ : versus_held_;
  bool acted = false;
  for (int guard = 0; guard < 64 && battle_->outcome() == battle::Outcome::Ongoing && battle_->acting_side() == acting; ++guard) {
    battle::Command cmd;
    if (held) cmd = *held, held.reset();
    else if (!source->poll(*battle_, cmd)) break;
    // An engage is shown as an action is: its run-up plays before the next command.
    const bool action = cmd.kind < battle::Command::Kind::EndTurn || cmd.kind == battle::Command::Kind::Engage;
    // An engage of another attacker or target first sends the standing attacker home and waits until it has set off.
    const bool engage_waits = cmd.kind == battle::Command::Kind::Engage && stay_unit_ >= 0 &&
                              (stay_unit_ != rules_unit(cmd.actor) || stay_target_ != rules_unit(cmd.target));
    if (engage_waits) stay_left_ = true;
    if ((action && acted) || engage_waits) {
      held = cmd;
      break;
    }
    std::string why;
    if (!versus_apply(cmd, local, &why)) {
      log::warn("battle: versus command of side {} refused after command {}: {}", acting == 0 ? 'A' : 'B', versus_seq_, why);
      // Both games apply the same commands: one this game refuses means they are out of step.
      if (!local) {
        versus_result_ = VersusResult::Void;
        begin_finish();
        return;
      }
      continue;
    }
    // The other side's Dopplegang pick is shown by its cast, as this game's own is.
    if (cmd.kind == battle::Command::Kind::Form) pending_form_ = cmd.form, pending_form_unit_ = rules_unit(cmd.actor);
    if (cmd.kind == battle::Command::Kind::Engage) stage_engage(rules_unit(cmd.actor), rules_unit(cmd.target));
    acted = acted || action || cmd.kind == battle::Command::Kind::EndTurn;
  }
  if (acted) versus_idle_ = 0;
  collect_events();
}

// "Your turn" with the seconds left, or "<name>'s turn", or while a player is out of reach "Waiting for <name>... N" / "Reconnecting... N"
// with the seconds left to come back.
std::optional<BattleView::VersusBanner> BattleView::versus_banner() const {
  if (!versus_ || ui_ == Ui::Intro || ui_ == Ui::Result || finish_ != Finish::None || battle_->outcome() != battle::Outcome::Ongoing) return std::nullopt;
  const int li = std::clamp(static_cast<int>(lang_), 0, 5);
  VersusBanner banner;
  banner.own = battle_->turn_side() == versus_side_;
  banner.text = banner.own ? std::format("{}  {}", kYourTurn[li], std::max(0, (kVersusTurnTicks - versus_turn_ticks_ + 59) / 60))
                           : std::vformat(kTheirTurn[li], std::make_format_args(versus_opponent_));
  if (const std::optional<VersusLink::Away> away = versus_link_ ? versus_link_->away() : std::nullopt) {
    const int seconds = away->seconds;
    banner.text = away->self ? std::vformat(kReconnecting[li], std::make_format_args(seconds))
                             : std::vformat(kWaitingFor[li], std::make_format_args(versus_opponent_, seconds));
  }
  return banner;
}

// The Run confirm over the command box.
void BattleView::build_versus_hud() {
  if (!versus_ || !versus_run_ask_ || ui_ == Ui::Intro || ui_ == Ui::Result || finish_ != Finish::None || battle_->outcome() != battle::Outcome::Ongoing)
    return;
  const int li = std::clamp(static_cast<int>(lang_), 0, 5);
  const size_t ask = op_count();
  // The question in the window text and the Yes / No rows in the menu text (11 px apart, the cursor 12 px left of the words) in a window
  // as wide as the widest of them, centred where the command box would be.
  const float title_w = window_text_width(kSurrender[li]);
  const float row_w = kAskCursorGap + std::max(menu_text_width(kYes[li]), menu_text_width(kNo[li]));
  const float ask_w = window_cells(std::max(title_w, row_w));
  const float ask_x = std::trunc(160.0f - ask_w * 0.5f);
  constexpr float kAskY = 96, kRowsY = kAskY + kWindowLineH + 2, kAskH = kRowsY + 11 + kWindowLineH - kAskY;
  add_skin_box((ask_x - 8) * kPs1Px, (kAskY - 8) * kRowScale, (ask_w + 16) * kPs1Px, (kAskH + 16) * kRowScale);
  btext(0x83, kSurrender[li], ask_x + std::trunc((ask_w - title_w) * 0.5f), kAskY, 0x8A8A8A);
  const float rows_x = ask_x + std::trunc((ask_w - row_w) * 0.5f);
  for (int row = 0; row < 2; ++row) {
    const float y = kRowsY + 11.0f * static_cast<float>(row);
    if (row == versus_run_cursor_) add_menu_cursor(rows_x * kPs1Px, (y + 1) * kRowScale);
    btext(2, row == 0 ? kYes[li] : kNo[li], rows_x + kAskCursorGap, y, 0x8A8A8A);
  }
  set_group(ask, 2);
}

float BattleView::window_text_width(const std::u32string& text) {
  const int slot = lang_ == import::Lang::Jp ? ui::font::kKb : ui::font::kBn;
  fonts_.prepare(slot, text);
  return std::ceil(fonts_.measure(slot, text, ui::font::slot(slot).scale) / kPs1Px);
}

float BattleView::menu_text_width(const std::u32string& text) {
  const int slot = lang_ == import::Lang::Jp ? ui::font::kBattleJp : ui::font::kBattle;
  fonts_.prepare(slot, text);
  return std::ceil(fonts_.measure(slot, text, ui::font::slot(slot).scale) / kPs1Px);
}

std::string BattleView::route_plate() const {
  if (!versus_plate_shown_) return {};
  return versus_result_ == VersusResult::Won ? "Victory" : versus_result_ == VersusResult::Lost ? "Defeat" : versus_result_ == VersusResult::Draw ? "Draw" : "No result";
}

// The outcome plate: the word in the command headline face, centred in a window as wide as the word at the middle of the screen.
void BattleView::build_versus_plate() {
  const std::string text = route_plate();
  const std::u32string word(text.begin(), text.end());
  const size_t first = op_count();
  // The headline face's em, in PS1 rows, is the window's height.
  const ui::font::SlotInfo& face = ui::font::slot(ui::font::kCh);
  fonts_.prepare(ui::font::kCh, word);
  const float width = std::ceil(fonts_.measure(ui::font::kCh, word, face.scale) / kPs1Px);
  const float inner_w = window_cells(width), inner_h = std::ceil(static_cast<float>(face.size) * 96.0f / 72.0f * face.scale / kRowScale);
  const float inner_x = std::trunc(160.0f - inner_w * 0.5f), inner_y = std::trunc(110.0f - inner_h * 0.5f);
  add_skin_box((inner_x - 8) * kPs1Px, (inner_y - 8) * kRowScale, (inner_w + 16) * kPs1Px, (inner_h + 16) * kRowScale);
  btext(0xA, word, inner_x + std::trunc((inner_w + width) * 0.5f), inner_y, 0);
  set_group(first, 2);
}

void BattleView::finish_or_plate() {
  if (!versus_) {
    finished_ = true;
    return;
  }
  ui_ = Ui::Result;
  result_age_ = 0;
  versus_plate_shown_ = true;
  log::info("battle: versus plate {} at view tick {}", route_plate(), tick_count_);
}

}  // namespace oc::game
