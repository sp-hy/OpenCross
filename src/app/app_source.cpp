// Where the game data comes from. [Paths] Source=auto searches, in order: an RDE install (find_game_dir), the images [Paths] Disc1 /
// Disc2 name and those in [Paths] DiscDir, the images next to opencross.exe, the optical drives; the first place that has a Chrono
// Cross disc is used. rde searches only the first, disc skips it. While nothing can be played the "no game files found" screen shows
// (searched again every 2 s), and when Japanese and USA sets are both found with no [Disc] Region the version picker asks. Disc mode
// fixes the settings that need RDE's data and keeps its saves apart from RDE's.
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

std::vector<import::disc::FoundDisc> App::optical_discs() {
  std::vector<import::disc::FoundDisc> found;
  for (import::disc::DriveCandidate& drive : import::disc::scan_optical_drives())
    found.push_back({drive.drive + (drive.product.empty() ? "" : " (" + drive.product + ")"),
                     std::shared_ptr<const import::disc::SectorSource>(std::move(drive.source)), std::move(drive.identity)});
  return found;
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
  for (import::disc::FoundDisc& disc : optical_discs()) {
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

App::SourceSearch App::search_sources() {
  using platform::DataSource;
  SourceSearch search;
  if (config.source != DataSource::Disc) {
    search.rde_dir = platform::find_game_dir(config.game_dir);
    search.searched.push_back("Radical Dreamers Edition install (data\\cdrom.dat): " +
                              (config.game_dir.empty() ? std::string() : "[Paths] GameDir " + config.game_dir + ", ") + "the folder of opencross.exe (" +
                              utf8(exe) + ") and its parent folders");
    if (!search.rde_dir.empty() || config.source == DataSource::Rde) return search;
  }
  const auto finish = [&] {
    search.groups = import::disc::group_discs(search.found);
    search.choice = import::disc::choose_set(search.groups, region_of_word(config.disc_region));
    return search;
  };
  for (const std::string* named : {&config.disc1, &config.disc2}) {
    if (named->empty()) continue;
    const fs::path file = from_utf8(*named);
    search.searched.push_back("[Paths] Disc" + std::string(named == &config.disc1 ? "1" : "2") + ": " + *named);
    std::error_code ec;
    if (fs::is_regular_file(file, ec)) scan_image_file(file, search);
    else search.notes.push_back(*named + ": file not found");
  }
  if (!config.disc_dir.empty()) {
    search.searched.push_back("[Paths] DiscDir: " + config.disc_dir);
    for (const fs::path& file : import::disc::image_files_in(from_utf8(config.disc_dir))) scan_image_file(file, search);
  }
  if (!search.found.empty()) return finish();
  search.searched.push_back("Disc images in the folder of opencross.exe: " + utf8(exe));
  for (const fs::path& file : import::disc::image_files_in(exe)) scan_image_file(file, search);
  if (!search.found.empty()) return finish();
  search.searched.push_back("Optical drives");
  search.found = optical_discs();
  return finish();
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
  const import::disc::DiscGroup& group = search.groups[static_cast<size_t>(search.choice.group)];
  auto backend = std::make_unique<import::disc::DiscBackend>();
  std::string boot_file;
  for (int number = 1; number <= 2; ++number) {
    const int index = group.disc[static_cast<size_t>(number - 1)];
    if (index < 0) {
      log::info("data: disc {} not found; it is asked for when the story needs it", number);
      continue;
    }
    const import::disc::FoundDisc& disc = search.found[static_cast<size_t>(index)];
    boot_file = disc.identity.boot_file;
    drive_discs[static_cast<size_t>(number - 1)] = dynamic_cast<const import::disc::DriveSource*>(disc.source.get());
    std::string error;
    if (backend->mount(number, disc.source, &error))
      log::info("data: disc {}: {} ({}, {})", number, disc.where, disc.identity.known_build ? disc.identity.build : "unknown build",
                disc.source->description());
    else
      log::warn("data: disc {}: {}: {}", number, disc.where, error);
  }
  const Region region = group.region == Region::Unknown ? Region::Japan : group.region;
  disc_region_word = region == Region::Usa ? "usa" : "jp";
  disc_build_key = group.key;
  if (!group.known_build) {
    source_warning = "Unknown Chrono Cross version (boot file " + boot_file + "): playing it as the " + import::disc::region_name(region) + " version.";
    log::warn("data: {}", source_warning);
  }
  disc_backend = backend.get();
  files.open(std::move(backend), exe);
  // Disc 1 is in the drive until the story or a save says otherwise.
  files.set_disc(1);
  platform::set_disc_mode(true);
  apply_disc_mode();
  log::warn("data: mods were made for RDE's data; on the {} discs a mod's files may not match the game's", disc_region_word);
}

void App::apply_disc_mode() {
  disc_held = config;
  config.art_mode = platform::ArtMode::Original;
  config.bgm_source = "original";
  config.ps1_res_ui = true;
  config.language = disc_region_word == "usa" ? "en" : "jp";
  config.legacy_field_models = true;
  config.portraits = "1999";
  log::info("disc mode ({}): classic art, original music, PS1-resolution UI, language {}", disc_region_word, config.language);
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
  stored.ps1_res_ui = disc_held.ps1_res_ui;
  stored.language = disc_held.language;
  stored.legacy_field_models = disc_held.legacy_field_models;
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

std::string App::image_signature() {
  std::string signature;
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
  for (import::disc::FoundDisc& found : optical_discs()) search.found.push_back(std::move(found));
  Result result = Result::NoDisc;
  for (const import::disc::FoundDisc& found : search.found) {
    const import::disc::DiscIdentity& identity = found.identity;
    const std::string key = import::disc::build_key(identity);
    if (static_cast<int>(identity.disc_number) != disc) {
      if (result == Result::NoDisc) result = Result::WrongDisc;
      continue;
    }
    if (key != disc_build_key) {
      result = Result::WrongBuild;
      continue;
    }
    std::string error;
    if (!disc_backend->mount(disc, found.source, &error)) {
      log::warn("data: disc {}: {}: {}", disc, found.where, error);
      continue;
    }
    drive_discs[static_cast<size_t>(disc - 1)] = dynamic_cast<const import::disc::DriveSource*>(found.source.get());
    log::info("data: disc {} mounted from {}", disc, found.where);
    return Result::Ok;
  }
  log::info("data: disc {} not found ({})", disc, result == Result::WrongBuild ? "another version's disc" : result == Result::WrongDisc ? "the other disc" : "no disc");
  return result;
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
    if (const auto now = std::chrono::steady_clock::now(); now >= next_image_check) {
      next_image_check = now + kRescanEvery;
      if (std::string stamps = image_signature(); stamps != image_stamps) {
        image_stamps = std::move(stamps);
        disc_source_changed = true;
      }
    }
    const bool changed = disc_source_changed;
    disc_source_changed = false;
    return changed;
  };
  host.browse = [this](int) { browse_for_discs(); };
  disc_prompt.set_host(std::move(host));
  image_stamps = image_signature();
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
