// BattleView in a versus battle (rules/versus.md): the rules start from the shared inputs, the perspective (the game that plays side B
// shows units 3..5 as its party), the commands of both sides with the link (VersusLink: the other side's in the order its game applied
// them, the state hash after every command), the 60 s turn clock, the turn banner, the Run confirm and the Victory / Defeat / Draw plate.
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"

#include <algorithm>
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
const char32_t* kYourTurn[6] = {U"Your turn", U"Votre tour", U"Il tuo turno", U"Dein Zug", U"Tu turno", U"あなたのターン"};
const char* kTheirTurn[6] = {"{}'s turn", "Tour de {}", "Turno di {}", "{} ist am Zug", "Turno de {}", "{}のターン"};
const char32_t* kSurrender[6] = {U"Surrender?", U"Abandonner ?", U"Arrendersi?", U"Aufgeben?", U"¿Rendirse?", U"降参しますか？"};
const char32_t* kYes[6] = {U"Yes", U"Oui", U"Sì", U"Ja", U"Sí", U"はい"};
const char32_t* kNo[6] = {U"No", U"Non", U"No", U"Nein", U"No", U"いいえ"};
const char* kWaitingFor[6] = {"Waiting for {}... {}", "En attente de {}... {}", "In attesa di {}... {}", "Warte auf {}... {}", "Esperando a {}... {}",
                              "{}を待っています... {}"};
const char* kReconnecting[6] = {"Reconnecting... {}", "Reconnexion... {}", "Riconnessione... {}", "Verbinde neu... {}", "Reconectando... {}",
                                "再接続中... {}"};

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
  // The next menu names its PC again after an action or a turn change.
  if (cmd.kind < battle::Command::Kind::EndTurn || battle_->turn_side() != turn) versus_focus_ = -1;
  return true;
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
    const bool action = cmd.kind < battle::Command::Kind::EndTurn;
    if (action && acted) {
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
    acted = acted || action || cmd.kind == battle::Command::Kind::EndTurn;
  }
  if (acted) versus_idle_ = 0;
  collect_events();
}

// The banner at the top of the screen: "Your turn" with the seconds left, or "<name>'s turn", or while a player is out of reach "Waiting for
// <name>... N" / "Reconnecting... N" with the seconds left to come back; the Run confirm over the command box.
void BattleView::build_versus_hud() {
  if (!versus_ || ui_ == Ui::Intro || ui_ == Ui::Result || finish_ != Finish::None || battle_->outcome() != battle::Outcome::Ongoing) return;
  const int li = std::clamp(static_cast<int>(lang_), 0, 5);
  const bool jp = li == 5;
  const bool local = battle_->turn_side() == versus_side_;
  const size_t first = op_count();
  std::u32string label = local ? kYourTurn[li] : import::utf8_to_u32(std::vformat(kTheirTurn[li], std::make_format_args(versus_opponent_)));
  if (local) label += import::utf8_to_u32(std::format("  {}", std::max(0, (kVersusTurnTicks - versus_turn_ticks_ + 59) / 60)));
  if (const std::optional<VersusLink::Away> away = versus_link_ ? versus_link_->away() : std::nullopt) {
    const int seconds = away->seconds;
    label = import::utf8_to_u32(away->self ? std::vformat(kReconnecting[li], std::make_format_args(seconds))
                                           : std::vformat(kWaitingFor[li], std::make_format_args(versus_opponent_, seconds)));
  }
  add_skin_box(110 * kPs1Px, 4 * kRowScale, 100 * kPs1Px, 13 * kRowScale);
  btext(jp ? 0x10081 : 0x81, label, 160, 7, local ? 0x8A8A8A : 0x757575);
  set_group(first, 2);
  if (!versus_run_ask_) return;
  const size_t ask = op_count();
  add_skin_box(110 * kPs1Px, 92 * kRowScale, 100 * kPs1Px, 40 * kRowScale);
  btext(jp ? 0x10081 : 0x81, kSurrender[li], 160, 96, 0x8A8A8A);
  for (int row = 0; row < 2; ++row) {
    const float y = 108.0f + 11.0f * static_cast<float>(row);
    if (row == versus_run_cursor_) add_menu_cursor(136 * kPs1Px, (y + 1) * kRowScale);
    btext(jp ? 0x10002 : 2, row == 0 ? kYes[li] : kNo[li], 148, y, 0x8A8A8A);
  }
  set_group(ask, 2);
}

std::string BattleView::route_plate() const {
  if (!versus_plate_shown_) return {};
  return versus_result_ == VersusResult::Won ? "Victory" : versus_result_ == VersusResult::Lost ? "Defeat" : versus_result_ == VersusResult::Draw ? "Draw" : "No result";
}

// The outcome plate: the word in the command headline face, centred, over a window at the middle of the screen.
void BattleView::build_versus_plate() {
  const std::string text = route_plate();
  const std::u32string word(text.begin(), text.end());
  const size_t first = op_count();
  add_skin_box(100 * kPs1Px, 96 * kRowScale, 120 * kPs1Px, 28 * kRowScale);
  fonts_.prepare(ui::font::kCh, word);
  const float width = fonts_.measure(ui::font::kCh, word, ui::font::slot(ui::font::kCh).scale) / kPs1Px;
  btext(0xA, word, 160 + width * 0.5f, 101, 0);
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
