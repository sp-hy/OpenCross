// Retry lost battles (Quality of life) without the GPU: which lost battles end the game (the wipe rule the choice is offered on), the battle-start
// snapshot putting back the save and the field variables a battle's end writes, and the choice window's text in every language.
#include <cstdio>
#include <vector>

#include "game/game_session.h"
#include "ui/retry_prompt.h"

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
}  // namespace

int main() {
  using battle::Outcome;
  // --- a wipe ends the game unless the battle is losable (flag 2); no other ending does
  CHECK(game::wipe_ends_game(Outcome::Wiped, 0));
  CHECK(game::wipe_ends_game(Outcome::Wiped, 0x40));
  CHECK(game::wipe_ends_game(Outcome::Wiped, 0x21));
  CHECK(!game::wipe_ends_game(Outcome::Wiped, 2));
  CHECK(!game::wipe_ends_game(Outcome::Wiped, 0x42));
  CHECK(!game::wipe_ends_game(Outcome::Wiped, 0x33));
  for (const Outcome outcome : {Outcome::Victory, Outcome::Fled, Outcome::Scripted, Outcome::Stalled, Outcome::Ongoing}) CHECK(!game::wipe_ends_game(outcome, 0));

  // --- the snapshot puts back the save (gold, items, element stock, records) and the variables, whatever happened in between
  {
    ui::GameState gs;
    gs.slot.set_gold(1234);
    gs.slot.items[0] = {0x20, 3};
    gs.slot.elements[0] = {0x05, 2};
    gs.slot.chars[0].cur_hp = 80;
    gs.slot.chars[0].grid[0] = 7;
    std::vector<u16> vars(0x200, 0);
    vars[0x3D0 / 2] = 0x11;
    vars[0x32 / 2] = 0;
    const game::BattleSnapshot snapshot = game::take_battle_snapshot(gs, vars);
    gs.slot.set_gold(9999);
    gs.slot.items[0] = {};
    gs.slot.elements[0].qty = 0;
    gs.slot.chars[0].cur_hp = 1;
    gs.slot.chars[0].grid[0] = 0;
    vars[0x3D0 / 2] = 0x22;
    vars[0x32 / 2] = 1;
    game::restore_battle_snapshot(snapshot, gs, vars);
    CHECK(gs.gold() == 1234);
    CHECK(gs.slot.items[0].id == 0x20 && gs.slot.items[0].qty == 3);
    CHECK(gs.slot.elements[0].id == 0x05 && gs.slot.elements[0].qty == 2);
    CHECK(gs.slot.chars[0].cur_hp == 80 && gs.slot.chars[0].grid[0] == 7);
    CHECK(vars[0x3D0 / 2] == 0x11 && vars[0x32 / 2] == 0);
    // A shorter variable span takes what fits.
    std::vector<u16> few(4, 0xFFFF);
    game::restore_battle_snapshot(snapshot, gs, few);
    CHECK(few[0] == 0 && few[3] == 0);
  }

  // --- the window: a heading and three options in each of the six languages
  for (int lang = 0; lang <= static_cast<int>(import::Lang::Jp); ++lang)
    for (const std::u32string& line : ui::RetryPrompt::lines(static_cast<import::Lang>(lang))) CHECK(!line.empty());
  CHECK(ui::RetryPrompt::lines(import::Lang::En)[1] == U"Retry" && ui::RetryPrompt::lines(import::Lang::En)[3] == U"Give up");

  if (failures) return 1;
  std::printf("test_battle_retry: ok\n");
  return 0;
}
