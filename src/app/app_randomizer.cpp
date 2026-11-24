// The randomizer in the running game (game/randomizer.h): its settings follow the [Randomizer] feature at once, the plan is built the
// first time a read needs it (after the room-script scan), and the field, the shops and the field battles ask it at their reads.
#include <format>

#include "app/app.h"
#include "core/log.h"
#include "render/imgui_mods_tab.h"

namespace oc::app {

void App::init_randomizer() {
  randomizer_settings = game::randomizer::settings_from(features);
  if (randomizer_settings.enabled()) {
    log::info("randomizer: on, seed {}, spread {}", randomizer_settings.seed, randomizer_settings.spread);
    if (data_ready()) scan_service.start(files);
  }
  world.set_script_patch([this](std::string_view room, Bytes& code) {
    const game::randomizer::Plan* plan = randomizer();
    if (!plan) return;
    if (const int words = plan->patch_room(room, code)) log::info("randomizer: room {}: {} treasure literal(s) rewritten", room, words);
  });
  world.set_on_new_game([this](save::SaveSlot& slot) {
    const game::randomizer::Plan* plan = randomizer();
    if (!plan || (!plan->settings().start_gear && !plan->settings().start_elements)) return;
    plan->start_game(slot);
    log::info("randomizer: New Game: starting gear {}, starting elements {}", plan->settings().start_gear ? "moved" : "kept",
              plan->settings().start_elements ? "moved" : "kept");
  });
  render::RandomizerHooks hooks;
  hooks.status = [this]() -> std::string {
    if (!randomizer_settings.enabled() || !randomizer_plan) return {};
    std::string out;
    for (size_t pool = 0; pool < game::randomizer::kPools; ++pool) {
      const auto [count, moved] = randomizer_plan->counts()[pool];
      if (count) out += std::format("{}{}: {} of {} moved", out.empty() ? "" : "\n", game::randomizer::pool_name(static_cast<game::randomizer::Pool>(pool)), moved, count);
    }
    return out;
  };
  render::set_randomizer_hooks(std::move(hooks));
}

void App::update_randomizer() {
  const std::vector<features::ModInput> inputs = with_mods() ? mods.feature_inputs() : std::vector<features::ModInput>{};
  const features::Resolved now = features::resolve(inputs, config.features);
  features.values[features::kRandomizer] = now.values.at(features::kRandomizer);
  features.sources[features::kRandomizer] = now.sources.at(features::kRandomizer);
  const game::randomizer::Settings settings = game::randomizer::settings_from(features);
  if (settings == randomizer_settings) return;
  randomizer_settings = settings;
  randomizer_plan.reset();
  log::info("randomizer: {}, seed {}, spread {}", settings.enabled() ? "on" : "off", settings.seed, settings.spread);
  // Built now so the page can say what it moves; the first time, once the scan has finished (the Mods tab does not wait for it: the
  // first read that needs the plan does).
  if (!settings.enabled() || !data_ready()) return;
  scan_service.start(files);
  if (randomizer_pools || scan_service.ready()) randomizer();
}

const game::randomizer::Plan* App::randomizer() {
  if (!randomizer_settings.enabled() || !tables_ok || !data_ready()) return nullptr;
  if (randomizer_plan) return &*randomizer_plan;
  if (!randomizer_pools) {
    scan_service.start(files);
    const game::ScriptScan* scan = scan_service.wait();
    if (!scan) return nullptr;
    ui::shop::ShopData smiths = ui::shop::ShopData::from_tables(tables);
    if (!smiths.load_waa_shop(files)) log::warn("randomizer: waa_shop.bin missing; the smiths have no stock");
    randomizer_pools = game::randomizer::gather(*scan, tables, flow_ready ? &flow.machine().rules() : nullptr, smiths.waa_shop);
  }
  randomizer_plan = game::randomizer::build(randomizer_settings, *randomizer_pools);
  for (size_t pool = 0; pool < game::randomizer::kPools; ++pool) {
    const auto [count, moved] = randomizer_plan->counts()[pool];
    log::info("randomizer: {}: {} entries, {} moved", game::randomizer::pool_name(static_cast<game::randomizer::Pool>(pool)), count, moved);
  }
  return &*randomizer_plan;
}

ui::shop::StockMap App::randomizer_stock_map() {
  const game::randomizer::Plan* plan = randomizer();
  if (!plan || (!plan->settings().items && !plan->settings().elements)) return {};
  return [this](ui::shop::Kind kind, int shop_id, std::vector<int> row) {
    const game::randomizer::Plan* current = randomizer();
    if (!current || kind == ui::shop::Kind::Trading) return row;
    return current->stock(kind == ui::shop::Kind::Element, shop_id, std::move(row));
  };
}

void App::randomize_battle(int& scene, game::BattleView::StartOptions& bopt) {
  const game::randomizer::Plan* plan = randomizer();
  if (!plan || scene < 0) return;
  bopt.drops = plan->drops();
  if (!plan->moved_scene(scene)) return;
  const int moved = plan->scene(scene);
  bopt.enemy_tuning = plan->tuning(scene);
  log::info("randomizer: encounter scene {} -> scene {} (HP {} %, stats {} %, gold {} %)", scene, moved, bopt.enemy_tuning[0].hp_percent,
            bopt.enemy_tuning[0].stat_percent, bopt.enemy_tuning[0].gold_percent);
  scene = moved;
}

}  // namespace oc::app
