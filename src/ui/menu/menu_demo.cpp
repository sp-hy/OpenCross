#include "ui/menu/menu_demo.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>

#include "core/log.h"
#include "platform/paths.h"

namespace oc::ui {

namespace {
struct Step {
  ArtMode mode;
  MenuScreen screen;
  int variant;
  const char* tag;
};
std::vector<Step> make_steps() {
  std::vector<Step> steps;
  for (ArtMode mode : {ArtMode::Hd, ArtMode::Original}) {
    steps.push_back({mode, MenuScreen::Main, 0, "main"});
    steps.push_back({mode, MenuScreen::Main, 1, "main_pick"});
    steps.push_back({mode, MenuScreen::Main, 2, "main_roster"});
    steps.push_back({mode, MenuScreen::Main, 3, "main_roster_ask"});
    steps.push_back({mode, MenuScreen::Status, 0, "status"});
    steps.push_back({mode, MenuScreen::Status, 1, "status_equip"});
    steps.push_back({mode, MenuScreen::ElementChoice, 0, "element_choice"});
    steps.push_back({mode, MenuScreen::ElementUse, 0, "element_use"});
    steps.push_back({mode, MenuScreen::ElementUse, 1, "element_use_target"});
    steps.push_back({mode, MenuScreen::ElementUse, 2, "element_use_roster"});
    steps.push_back({mode, MenuScreen::ElementGrid, 0, "element_list"});
    steps.push_back({mode, MenuScreen::ElementGrid, 1, "element_grid"});
    steps.push_back({mode, MenuScreen::ElementGrid, 2, "element_options"});
    steps.push_back({mode, MenuScreen::Equip, 0, "equip"});
    steps.push_back({mode, MenuScreen::Equip, 1, "equip_list"});
    steps.push_back({mode, MenuScreen::Equip, 2, "equip_accessories"});
    steps.push_back({mode, MenuScreen::KeyItems, 0, "key_items"});
    steps.push_back({mode, MenuScreen::Settings, 0, "settings"});
    steps.push_back({mode, MenuScreen::Settings, 1, "skin_picker"});
    steps.push_back({mode, MenuScreen::Settings, 2, "controller_field"});
    steps.push_back({mode, MenuScreen::Settings, 3, "controller_battle"});
    steps.push_back({mode, MenuScreen::Save, 0, "save_choice"});
    steps.push_back({mode, MenuScreen::Save, 4, "save_choice_nosave"});
    steps.push_back({mode, MenuScreen::Save, 1, "save_grid"});
    steps.push_back({mode, MenuScreen::Save, 2, "load_grid"});
    steps.push_back({mode, MenuScreen::Save, 3, "save_confirm"});
  }
  // OC_MENU_ONLY: screenshot only the steps whose tag contains this text.
  if (const char* only = std::getenv("OC_MENU_ONLY"))
    std::erase_if(steps, [&](const Step& step) { return std::string(step.tag).find(only) == std::string::npos; });
  return steps;
}

// Finds the directory holding the demo's save copy, searching the exe dir and its parents.
std::filesystem::path find_save_copy() {
  std::filesystem::path dir = platform::exe_dir();
  for (int i = 0; i < 8 && !dir.empty(); ++i) {
    if (std::filesystem::exists(dir / "tmp" / "save.dat")) return dir / "tmp";
    if (dir == dir.parent_path()) break;
    dir = dir.parent_path();
  }
  return {};
}
}  // namespace

struct MenuDemo::Impl {
  render::GpuContext* ctx = nullptr;
  render::SpriteBatch* sprites = nullptr;
  data::GameTables tables;
  MenuRules rules;
  render::TextBatch text;
  MainMenu menu;
  save::SaveDat dat;
  GameState state;
  std::filesystem::path out_path;
  bool ready = false;
  // screenshot sequence
  std::vector<Step> steps;
  size_t step = 0;
  int wait = 0;
  bool active = false;
  std::filesystem::path out_dir;
  std::vector<std::filesystem::path> shots;
  ArtMode saved_mode = ArtMode::Hd;
};

MenuDemo::MenuDemo() : impl_(std::make_unique<Impl>()) {}
MenuDemo::~MenuDemo() { shutdown(); }

bool MenuDemo::init(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, import::Lang lang, int slot) {
  Impl& impl = *impl_;
  impl.ctx = &ctx;
  impl.sprites = &sprites;
  std::string err;
  if (!impl.tables.load(vfs, &err)) {
    log::warn("menu demo: tables: {}", err);
    return false;
  }
  impl.rules.tables = &impl.tables;
  if (!impl.rules.load_pg(vfs)) log::warn("menu demo: menu_pg.bin missing; element eligibility is not enforced");
  const std::filesystem::path tmp = find_save_copy();
  if (tmp.empty()) {
    log::warn("menu demo: tmp/save.dat not found");
    return false;
  }
  try {
    impl.dat = save::SaveDat::load(tmp / "save.dat");
    const save::MemCard* card = impl.dat.card(1);
    if (!card) throw std::runtime_error("no memory card 1");
    bool found = false;
    for (const auto& listed : save::list_slots(*card))
      if (listed.slot == slot) {
        impl.state = GameState(save::read_slot(*card, listed.block), &impl.rules);
        found = true;
      }
    if (!found) throw std::runtime_error("slot " + std::to_string(slot) + " missing");
  } catch (const std::exception& error) {
    log::warn("menu demo: save: {}", error.what());
    return false;
  }
  impl.out_path = tmp / "save_menu.dat";
  if (!impl.text.init(ctx, sprites)) return false;
  if (!impl.menu.init(ctx, vfs, sprites, impl.text, impl.tables, lang)) return false;
  impl.menu.set_rules(&impl.rules);
  impl.menu.set_save_target(&impl.dat, impl.out_path);
  impl.ready = true;
  log::info("menu demo: ready (slot {} of {}, saving to {})", slot, (tmp / "save.dat").string(), impl.out_path.string());
  return true;
}

void MenuDemo::shutdown() {
  Impl& impl = *impl_;
  if (!impl.ctx) return;
  impl.menu.shutdown();
  impl.text.destroy();
  impl.ctx = nullptr;
  impl.ready = false;
}

void MenuDemo::open() {
  Impl& impl = *impl_;
  if (impl.ready) impl.menu.open(impl.state, true);
}
bool MenuDemo::is_open() const { return impl_->ready && !impl_->menu.closed(); }
void MenuDemo::set_mode(ArtMode mode) { impl_->menu.set_mode(mode); }
MainMenu& MenuDemo::menu() { return impl_->menu; }
GameState& MenuDemo::state() { return impl_->state; }
bool MenuDemo::sequence_active() const { return impl_->active; }
const std::vector<std::filesystem::path>& MenuDemo::shots() const { return impl_->shots; }

void MenuDemo::screenshot_sequence(render::GpuContext& ctx, std::filesystem::path out_dir) {
  Impl& impl = *impl_;
  if (!impl.ready) return;
  impl.ctx = &ctx;
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  impl.out_dir = std::move(out_dir);
  impl.steps = make_steps();
  impl.step = 0;
  impl.wait = 0;
  impl.active = true;
  impl.shots.clear();
  impl.saved_mode = impl.menu.mode();
  if (impl.menu.closed()) open();
}

bool MenuDemo::update(const platform::Input& in) {
  Impl& impl = *impl_;
  if (!impl.ready) return false;
  if (impl.active) {
    // Per step: set the screen, let a few frames pass (textures upload, text atlases build), then request the shot.
    if (impl.step >= impl.steps.size()) {
      impl.active = false;
      impl.menu.set_mode(impl.saved_mode);
      log::info("menu demo: {} screenshots written to {}", impl.shots.size(), impl.out_dir.string());
      return !impl.menu.closed();
    }
    const Step& current = impl.steps[impl.step];
    if (impl.wait == 0) {
      if (impl.menu.closed()) open();
      impl.menu.set_mode(current.mode);
      impl.menu.debug_show(current.screen, current.variant);
    }
    ++impl.wait;
    // The layout audit (OC_TEXT_AUDIT=1) reports the frame drawn after this update.
    if (impl.wait == 5) impl.text.audit_clear();
    if (impl.wait == 6) {
      const auto path = impl.out_dir / std::format("{:02}_{}_{}.png", impl.step + 1, current.tag, current.mode == ArtMode::Hd ? "hd" : "orig");
      impl.text.audit_report(std::format("{}_{}", current.tag, current.mode == ArtMode::Hd ? "hd" : "orig"));
      impl.ctx->request_screenshot(path);
      impl.shots.push_back(path);
    } else if (impl.wait >= 9) {
      ++impl.step;
      impl.wait = 0;
    }
    return true;
  }
  if (impl.menu.closed()) return false;
  impl.menu.update(in);
  return !impl.menu.closed();
}

void MenuDemo::draw(const render::Frame& frame, float vw, float vh, bool resume) {
  if (!impl_->ready) return;
  if (!impl_->menu.closed()) impl_->menu.draw(frame, vw, vh, resume);
}

}  // namespace oc::ui
