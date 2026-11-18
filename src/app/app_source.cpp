// Where the game data comes from (the search itself is app/data_source.h). While nothing can be played the "no game files found"
// screen shows (searched again every 2 s), and when Japanese and USA sets are both found with no [Disc] Region the version picker asks.
// Disc mode fixes the settings that need RDE's data and keeps its saves apart from RDE's; discs in optical drives are watched, and a
// disc the story needs that is not mounted brings up the insert-disc screen.
#include "app/app.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <chrono>
#include <system_error>

#include "core/log.h"
#include "platform/paths.h"
#include "render/imgui_data_screen.h"

namespace oc::app {

namespace fs = std::filesystem;
using import::disc::Region;

namespace {

std::string utf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

fs::path from_utf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

// Size and write time: an image file that changes is opened again.
std::string file_stamp(const fs::path& file) {
  std::error_code ec;
  const auto size = fs::file_size(file, ec);
  const auto time = fs::last_write_time(file, ec).time_since_epoch().count();
  return std::to_string(size) + "/" + std::to_string(time);
}

Region region_of_word(const std::string& word) { return word == "jp" ? Region::Japan : word == "usa" ? Region::Usa : Region::Unknown; }

std::string disc_words(const import::disc::DiscIdentity& identity) {
  if (!identity.chrono_cross) return "no Chrono Cross disc (" + identity.reason + ")";
  return (identity.known_build ? identity.build : std::string("an unknown build")) + ", disc " + std::to_string(identity.disc_number);
}

constexpr auto kRescanEvery = std::chrono::seconds(2);

}  // namespace

void App::scan_image_file(const fs::path& file, SourceSearch& search) {
  const std::string stamp = file_stamp(file);
  if (auto known = unusable_images.find(file); known != unusable_images.end() && known->second.first == stamp) {
    search.notes.insert(search.notes.end(), known->second.second.begin(), known->second.second.end());
    return;
  }
  std::vector<std::string> notes;
  const size_t before = search.found.size();
  import::disc::add_image_file(file, search.found, notes);
  if (search.found.size() == before) unusable_images[file] = {stamp, notes};
  search.notes.insert(search.notes.end(), notes.begin(), notes.end());
}

void App::poll_drive_discs() {
  if (!disc_backend) return;
  for (size_t side = 0; side < drive_discs.size(); ++side) {
    if (drive_discs[side] && drive_discs[side]->media_changed()) {
      drive_discs[side] = nullptr;
      disc_backend->unmount(static_cast<int>(side) + 1);
      drive_wait = disc_source_changed = true;
      log::info("data: disc {} left its drive", side + 1);
    }
  }
  const auto now = std::chrono::steady_clock::now();
  if (!drive_wait || now < next_drive_scan) return;
  next_drive_scan = now + kRescanEvery;
  for (import::disc::FoundDisc& disc : app::optical_discs()) {
    const import::disc::DiscIdentity& identity = disc.identity;
    const std::string key = import::disc::build_key(identity);
    const int number = static_cast<int>(identity.disc_number);
    if (key != disc_build_key || (number != 1 && number != 2) || drive_discs[static_cast<size_t>(number - 1)]) continue;
    std::string error;
    if (!disc_backend->mount(number, disc.source, &error)) {
      log::warn("data: disc {} in {}: {}", number, disc.where, error);
      continue;
    }
    drive_discs[static_cast<size_t>(number - 1)] = dynamic_cast<const import::disc::DriveSource*>(disc.source.get());
    drive_wait = false;
    disc_source_changed = true;
    log::info("data: disc {} mounted from {}", number, disc.where);
  }
}

SourceSearch App::search_sources() {
  return app::search_sources(config, exe, [this](const fs::path& file, SourceSearch& search) { scan_image_file(file, search); });
}

bool App::init_source() {
  SourceSearch search = search_sources();
  if (!search.playable() && !source_screen(search)) {
    log::info("data: the player quit before any game data was found");
    return false;
  }
  if (!search.rde_dir.empty()) {
    game_dir = search.rde_dir;
    log::info("game dir: {}", game_dir.string());
  } else {
    open_discs(search);
  }
  pick_save_dir();
  return true;
}

void App::open_discs(const SourceSearch& search) {
  const OpenedDiscs opened = app::open_discs(search, files, exe);
  disc_backend = opened.backend;
  drive_discs = opened.drives;
  disc_region_word = opened.region_word;
  disc_build_key = opened.build_key;
  source_warning = opened.warning;
  apply_disc_mode();
  log::warn("data: mods were made for RDE's data; on the {} discs a mod's files may not match the game's", disc_region_word);
}

void App::apply_disc_mode() {
  disc_held = config;
  apply_disc_settings(config, disc_region_word);
}

void App::save_config() const {
  if (!own_ini) return;
  if (!platform::disc_mode()) {
    config.save(config_file);
    return;
  }
  platform::Config stored = config;
  stored.art_mode = disc_held.art_mode;
  stored.bgm_source = disc_held.bgm_source;
  stored.language = disc_held.language;
  stored.portraits = disc_held.portraits;
  stored.save(config_file);
}

void App::pick_save_dir() {
  if (platform::disc_mode() && config.save_dir.empty()) {
    save_dir = platform::disc_save_dir(disc_region_word);
    log::info("save dir: {} (disc mode)", save_dir.string());
    return;
  }
  const platform::SaveDirPick save_pick = platform::find_save_dir(config.save_dir, own_ini ? profile_dir : fs::path());
  save_dir = save_pick.dir;
  log::info("save dir: {} ({})", save_dir.empty() ? "(not found)" : save_dir.string(), save_pick.how);
}

void App::disc_changed(int disc) {
  files.set_disc(disc);
  if (!disc_backend || disc_backend->mounted(disc)) return;
  log::warn("data: disc {} is needed and not mounted", disc);
  // It may be in a drive already.
  drive_wait = true;
}

std::string App::source_signature() {
  std::string signature = import::disc::drive_media_signature(platform::list_optical_drives());
  const auto add = [&](const fs::path& file) { signature += utf8(file) + "|" + file_stamp(file) + "\n"; };
  for (const std::string* named : {&config.disc1, &config.disc2})
    if (!named->empty()) add(from_utf8(*named));
  if (!config.disc_dir.empty())
    for (const fs::path& file : import::disc::image_files_in(from_utf8(config.disc_dir))) add(file);
  for (const fs::path& file : import::disc::image_files_in(exe)) add(file);
  return signature;
}

ui::DiscPrompt::MountResult App::mount_disc(int disc) {
  using Result = ui::DiscPrompt::MountResult;
  if (!disc_backend) return Result::NoDisc;
  if (disc_backend->mounted(disc)) return Result::Ok;
  SourceSearch search;
  for (const std::string* named : {&config.disc1, &config.disc2})
    if (!named->empty()) scan_image_file(from_utf8(*named), search);
  if (!config.disc_dir.empty())
    for (const fs::path& file : import::disc::image_files_in(from_utf8(config.disc_dir))) scan_image_file(file, search);
  for (const fs::path& file : import::disc::image_files_in(exe)) scan_image_file(file, search);
  for (import::disc::FoundDisc& found : app::optical_discs(true)) search.found.push_back(std::move(found));
  using import::disc::DiscFit;
  const import::disc::DiscPick pick = import::disc::pick_disc(search.found, disc, disc_build_key);
  if (pick.index >= 0) {
    const import::disc::FoundDisc& found = search.found[static_cast<size_t>(pick.index)];
    std::string error;
    if (!disc_backend->mount(disc, found.source, &error)) {
      log::warn("data: disc {}: {}: {}", disc, found.where, error);
      return Result::NotGame;
    }
    drive_discs[static_cast<size_t>(disc - 1)] = dynamic_cast<const import::disc::DriveSource*>(found.source.get());
    log::info("data: disc {} mounted from {}", disc, found.where);
    return Result::Ok;
  }
  for (const import::disc::FoundDisc& found : search.found)
    log::info("data: disc {} wanted; {} holds {}", disc, found.where, disc_words(found.identity));
  switch (pick.fit) {
    case DiscFit::NoDisc:
      log::info("data: disc {} not found (no disc)", disc);
      return Result::NoDisc;
    case DiscFit::NotChronoCross:
      log::info("data: disc {} not found (not a Chrono Cross disc)", disc);
      return Result::NotGame;
    case DiscFit::OtherDisc:
      log::info("data: disc {} not found (the other disc)", disc);
      return Result::WrongDisc;
    case DiscFit::OtherBuild:
      log::info("data: disc {} not found (another version's disc)", disc);
      return Result::WrongBuild;
    case DiscFit::Fits:
      break;
  }
  return Result::NoDisc;
}

void App::init_disc_prompt() {
  if (!disc_backend || !flow_ready) return;
  disc_prompt_ok = disc_prompt.init(ctx, files, sprites, menu_text);
  if (!disc_prompt_ok) return;
  disc_prompt.set_layout(disc_region_word == "usa" ? ui::DiscPrompt::Layout::Usa : ui::DiscPrompt::Layout::Japan);
  disc_prompt.set_hint(U"F9  Browse for image...      F10  Search again");
  ui::DiscPrompt::Host host;
  host.try_mount = [this](int disc) { return mount_disc(disc); };
  host.changed = [this] {
    std::vector<std::string> notes;
    if (take_browsed_files(notes)) disc_source_changed = true;
    for (const std::string& note : notes) log::info("data: {}", note);
    if (const auto now = std::chrono::steady_clock::now(); now >= next_source_check) {
      next_source_check = now + kRescanEvery;
      if (std::string stamps = source_signature(); stamps != source_stamps) {
        source_stamps = std::move(stamps);
        disc_source_changed = true;
      }
    }
    const bool changed = disc_source_changed;
    disc_source_changed = false;
    return changed;
  };
  host.browse = [this](int) { browse_for_discs(); };
  disc_prompt.set_host(std::move(host));
  source_stamps = source_signature();
}

bool App::disc_prompt_frame() {
  if (!disc_prompt_ok) return false;
  if (!disc_prompt.active()) {
    int needed = 0;
    if (const ui::SlotEntry* slot = flow_ready ? flow.loading_slot() : nullptr) needed = slot->data.var(0x42) + 1;
    else if (world_ok && world.view().loaded() && !(flow_ready && flow.scene_mode())) needed = world.vm().var(0x42) + 1;
    if ((needed != 1 && needed != 2) || disc_backend->mounted(needed)) return false;
    log::info("data: disc {} is not in the drive: the insert-disc screen", needed);
    disc_prompt.open(needed);
  }
  const ui::DiscPrompt::Input actions{input.key_pressed[SDL_SCANCODE_F9], input.key_pressed[SDL_SCANCODE_F10]};
  for (int tick = 0; tick < step.ticks; ++tick) disc_prompt.update(tick == 0 ? actions : ui::DiscPrompt::Input{});
  if (!disc_prompt.active()) log::info("data: disc {} is in, the game goes on", disc_prompt.needed_disc());
  step.alpha = 1.0f;
  loop_speed.field = {0, 1.0f};
  return true;
}

void App::browse_for_discs() {
  static const SDL_DialogFileFilter kFilters[] = {
      {"Disc images", "cue;bin;img;iso;ccd;mds;mdf;m3u;pbp;chd"},
      {"All files", "*"},
  };
  SDL_ShowOpenFileDialog(
      [](void* user, const char* const* list, int) {
        App& app = *static_cast<App*>(user);
        std::vector<fs::path> picked;
        for (const char* const* entry = list; entry && *entry; ++entry) picked.push_back(from_utf8(*entry));
        std::lock_guard lock(app.browse_mutex);
        app.browsed_files = std::move(picked);
      },
      this, window.handle(), kFilters, static_cast<int>(std::size(kFilters)), utf8(exe).c_str(), true);
}

bool App::take_browsed_files(std::vector<std::string>& notes) {
  std::vector<fs::path> picked;
  {
    std::lock_guard lock(browse_mutex);
    if (!browsed_files) return false;
    picked = std::move(*browsed_files);
    browsed_files.reset();
  }
  notes.clear();
  bool any = false;
  for (const fs::path& file : picked) {
    std::vector<import::disc::FoundDisc> found;
    import::disc::add_image_file(file, found, notes);
    for (const import::disc::FoundDisc& disc : found) {
      std::string& key = disc.identity.disc_number == 2 ? config.disc2 : config.disc1;
      key = utf8(file);
      any = true;
      log::info("data: Browse...: {} is disc {}, kept as [Paths] Disc{}", disc.where, disc.identity.disc_number, disc.identity.disc_number == 2 ? 2 : 1);
    }
  }
  if (any) save_config();
  return true;
}

bool App::source_screen(SourceSearch& search) {
  log::info("data: {}; searched:", search.choice.ask_region ? "Japanese and USA discs found, asking which to play" : "no game files found");
  for (const std::string& line : search.searched) log::info("  {}", line);
  for (const std::string& line : search.notes) log::info("  {}", line);
  using Clock = std::chrono::steady_clock;
  auto next_scan = Clock::now() + kRescanEvery;
  // [Debug] Screenshot: one shot of the screen (at [Debug] ScreenshotTick frames, else 30), then ExitAfterScreenshot quits.
  const u64 shot_frame = config.debug_screenshot_tick >= 0 ? static_cast<u64>(config.debug_screenshot_tick) : 30;
  u64 frames = 0;
  std::string picker_warning;
  for (const import::disc::DiscGroup& group : search.groups)
    if (!group.known_build) picker_warning = "An unknown Chrono Cross version was found too; it plays as the closest known version.";
  // Why the files picked with Browse... cannot be played, until the next pick.
  std::vector<std::string> browse_notes;
  while (window.pump(input)) {
    const bool browsed = take_browsed_files(browse_notes);
    if (!search.choice.ask_region && (browsed || Clock::now() >= next_scan)) {
      search = search_sources();
      search.notes.insert(search.notes.end(), browse_notes.begin(), browse_notes.end());
      next_scan = Clock::now() + kRescanEvery;
      if (search.playable() && !search.choice.ask_region) return true;
    }
    if (window.take_resized()) ctx.request_resize();
    render::Frame frame;
    if (!ctx.begin_frame(frame)) {
      SDL_Delay(10);
      continue;
    }
    ui.new_frame();
    if (search.choice.ask_region) {
      const render::VersionPick pick = render::draw_version_picker(picker_warning);
      if (pick == render::VersionPick::Quit) quit_requested = true;
      if (pick == render::VersionPick::Japanese || pick == render::VersionPick::Usa) {
        config.disc_region = pick == render::VersionPick::Usa ? "usa" : "jp";
        save_config();
        log::info("data: version picked: [Disc] Region = {}", config.disc_region);
        search.choice = import::disc::choose_set(search.groups, region_of_word(config.disc_region));
      }
    } else {
      render::NotFoundView view{search.searched, search.notes, std::chrono::duration<double>(next_scan - Clock::now()).count()};
      const render::NotFoundAction action = render::draw_not_found(view);
      if (action == render::NotFoundAction::Quit) quit_requested = true;
      if (action == render::NotFoundAction::Retry) next_scan = Clock::now();
      if (action == render::NotFoundAction::Browse) browse_for_discs();
    }
    ui.render(frame);
    ++frames;
    if (config.debug_screenshot && frames == shot_frame) ctx.request_screenshot(exe / "screenshot.png");
    ctx.end_frame();
    if (config.debug_screenshot && config.debug_exit_after_screenshot && frames > shot_frame + 3) quit_requested = true;
    if (quit_requested) return false;
    if (search.playable() && !search.choice.ask_region) return true;
    limiter.wait(60);
  }
  quit_requested = true;
  return false;
}

}  // namespace oc::app
