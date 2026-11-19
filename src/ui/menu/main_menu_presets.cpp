// The element grid editor's presets ([Game] GridPresets): the label pair Save Preset / Load Preset, the slot window that takes the stock
// list's place while a slot is chosen (three slots per character, each showing its colour mix and top-level element), the report of the
// elements a load could not find, and the save / load actions on the GRID chunk of the collection file.
#include <string>

#include "core/log.h"
#include "platform/config.h"
#include "ui/menu/main_menu_impl.h"
#include "ui/menu/menu_common.h"

namespace oc::ui {

namespace {

constexpr int kReportRows = kVisibleRows - 1;

const Str6 kSaveLabel = {{"Save Preset", "Sauver préréglage", "Salva preset", "Preset sichern", "Guardar preset", "プリセットほぞん"}};
const Str6 kLoadLabel = {{"Load Preset", "Charger préréglage", "Carica preset", "Preset laden", "Cargar preset", "プリセットよみこみ"}};
const Str6 kSaveHelp = {{"Saves this grid\nto one of three slots.", "Enregistre cette grille\ndans l'un des trois\nemplacements.",
                         "Salva questa griglia\nin uno dei tre slot.", "Speichert dieses Raster\nin einem von drei\nPlätzen.",
                         "Guarda esta cuadrícula\nen uno de los tres\nhuecos.", "このグリッドを\n3つのスロットに\nほぞんします。"}};
const Str6 kLoadHelp = {{"Fills this grid from\nyour elements.", "Remplit cette grille\navec vos éléments.", "Riempie questa griglia\ncon i tuoi elementi.",
                         "Füllt dieses Raster\nmit deinen Elementen.", "Llena esta cuadrícula\ncon tus elementos.", "もっているエレメントで\nグリッドをうめます。"}};
const Str6 kSlotWord = {{"Slot", "Emplacement", "Slot", "Platz", "Hueco", "スロット"}};
const Str6 kEmpty = {{"Empty", "Vide", "Vuoto", "Leer", "Vacío", "なし"}};
const Str6 kTop = {{"top", "max", "massimo", "Spitze", "máximo", "さいじょうい"}};
const Str6 kMissing = {{"Missing elements:", "Éléments manquants :", "Elementi mancanti:", "Fehlende Elemente:", "Elementos que faltan:", "たりないエレメント："}};
const char* const kColourFallback[6] = {"Green", "White", "Black", "Red", "Yellow", "Blue"};

}  // namespace

std::u32string MainMenu::Impl::preset_label(bool save_row) { return utf32(S6(save_row ? kSaveLabel : kLoadLabel)); }

const char* MainMenu::Impl::preset_help(bool save_row) { return S6(save_row ? kSaveHelp : kLoadHelp); }

// "Red/Blue, top: Volcano": the two colours the preset holds most of, then its highest-level element; "Empty" for an unused slot.
std::u32string MainMenu::Impl::preset_summary_text(const save::GridPreset* preset) {
  if (!preset) return utf32(S6(kEmpty));
  const PresetSummary summary = gs->preset_summary(*preset);
  std::u32string text;
  for (const int colour : summary.colours) {
    if (colour < 0) continue;
    if (!text.empty()) text += U'/';
    std::string name = trim(menu_str(0x801F4A30u + 3u * static_cast<u32>(colour), kColourFallback[colour]));
    if (!name.empty() && name[0] >= 'a' && name[0] <= 'z') name[0] = static_cast<char>(name[0] - 'a' + 'A');
    text += utf32(name);
  }
  if (text.empty()) text = U"-";
  if (summary.top_id != 0) text += utf32(std::string(", ") + S6(kTop) + ": ") + element_name(summary.top_id);
  return text;
}

// The window: a caption row in an empty box, then per slot a "Slot n" row and its summary row; or, for the report, the missing elements
// with their copies short (Up / Down scroll).
void MainMenu::Impl::build_preset_window() {
  auto row_y = [](int row) { return 42.0f + 24.0f * static_cast<float>(row); };
  const bool report = prompt == GridPrompt::PresetReport;
  const std::u32string caption = report ? utf32(S6(kMissing)) : preset_label(prompt == GridPrompt::PresetSave);
  const float caption_rel = fit_rel(caption, 124, 0.9f);
  empty_box(275, row_y(0));
  TP(301.0f + (128.0f - twp(caption, caption_rel)) * 0.5f, row_y(0) + 5.0f, caption, ps1_colour(0x909090), caption_rel);
  if (report) {
    const int count = static_cast<int>(preset_missing.size());
    for (int row = 0; row < kReportRows && prompt_cur + row < count; ++row) {
      const auto& [id, copies] = preset_missing[static_cast<size_t>(prompt_cur + row)];
      const std::u32string name = element_name(id);
      const float y = row_y(row + 1) + 5.0f;
      TPfit(282, y, name, 150, ps1_colour(0x909090), 0.9f);
      if (copies > 1) TP(440, y, utf32("x" + std::to_string(copies)), ps1_colour(0x909090), 0.8f);
    }
    if (prompt_cur > 0) scroll_arrow(485, 35, true);
    if (prompt_cur + kReportRows < count) scroll_arrow(485, 311, false);
    return;
  }
  const int character = cur_char();
  for (int slot_index = 0; slot_index < save::GridPresets::kSlots; ++slot_index) {
    const u32 colour = slot_index == prompt_cur ? ps1_colour(0x7A6AA5) : ps1_colour(0x909090);
    const float y = row_y(1 + 2 * slot_index) + 5.0f;
    TP(282, y, utf32(std::string(S6(kSlotWord)) + " " + std::to_string(slot_index + 1)), colour, 0.9f);
    TPfit(294, y + 24.0f, preset_summary_text(preset_cache.find(character, slot_index)), 176, colour, 0.9f);
  }
  tri(251, 47.0f + 24.0f * static_cast<float>(1 + 2 * prompt_cur), true);
}

// Save writes the member's cells to the chosen slot; Load fills the grid from the stock (an empty slot refuses) and opens the report when
// elements were missing; true when the grid was rebuilt. The log line gives the stock and grid copies before and after, whose sum only
// changes by what was missing.
bool MainMenu::Impl::run_preset(int character) {
  if (prompt == GridPrompt::PresetSave) {
    save::GridPreset preset = gs->preset_of(character);
    preset.slot = static_cast<u8>(prompt_cur);
    log::info("grid preset: character {} saved slot {} with {} cells", character, prompt_cur, preset.cells.size());
    preset_cache.set(std::move(preset));
    preset_cache.write(*preset_store);
    if (preset_changed) preset_changed();
    prompt = GridPrompt::None;
    sfx(0x17);
    return false;
  }
  const save::GridPreset* preset = preset_cache.find(character, prompt_cur);
  if (!preset) {
    sfx(0x18);
    return false;
  }
  auto copies_held = [&] {
    int stock = 0, grid = 0;
    for (const save::ItemStack& stack : gs->slot.elements) stock += stack.id != 0 ? stack.qty : 0;
    for (const save::GridPresetCell& cell : gs->preset_of(character).cells) grid += cell.count;
    return std::pair{stock, grid};
  };
  const auto [stock_before, grid_before] = copies_held();
  const PresetLoad load = gs->apply_preset(character, *preset, platform::cheats().more_summons);
  const auto [stock_after, grid_after] = copies_held();
  log::info("grid preset: character {} loaded slot {}: {} placed, {} elements short; stock {} -> {}, grid {} -> {}, total {} -> {}", character,
            prompt_cur, load.placed, load.missing.size(), stock_before, stock_after, grid_before, grid_after, stock_before + grid_before,
            stock_after + grid_after);
  list_cur -= list_scroll, list_scroll = 0;
  sfx(0x17);
  preset_missing = load.missing;
  prompt = preset_missing.empty() ? GridPrompt::None : GridPrompt::PresetReport;
  prompt_cur = 0;
  return true;
}

// Up / Down choose the slot (OK acts, Cancel closes); the report scrolls with Up / Down and closes on OK or Cancel. True when a load
// rebuilt the grid.
bool MainMenu::Impl::update_preset_prompt(const Nav& nav) {
  if (prompt == GridPrompt::PresetReport) {
    const int count = static_cast<int>(preset_missing.size());
    if (nav.up && prompt_cur > 0) --prompt_cur, sfx(1);
    if (nav.down && prompt_cur + kReportRows < count) ++prompt_cur, sfx(1);
    if (nav.ok || nav.cancel) prompt = GridPrompt::None, sfx(nav.ok ? 1 : 6);
    return false;
  }
  if (nav.up) prompt_cur = wrap(prompt_cur - 1, save::GridPresets::kSlots), sfx(1);
  if (nav.down) prompt_cur = wrap(prompt_cur + 1, save::GridPresets::kSlots), sfx(1);
  if (nav.ok) return run_preset(cur_char());
  if (nav.cancel) prompt = GridPrompt::None, sfx(6);
  return false;
}

}  // namespace oc::ui
