#include "platform/config.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "core/log.h"

#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>

namespace oc::platform {

namespace {

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

int parse_int(const std::string& text, int fallback) {
  int value = fallback;
  auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc() ? value : fallback;
}

float parse_float(const std::string& text, float fallback) {
  if (text.empty()) return fallback;
  char* end = nullptr;
  float value = std::strtof(text.c_str(), &end);
  return end == text.c_str() ? fallback : value;
}

bool parse_bool(const std::string& text, bool fallback) {
  if (text == "1" || text == "true" || text == "yes" || text == "on") return true;
  if (text == "0" || text == "false" || text == "no" || text == "off") return false;
  return fallback;
}

float clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }

std::string fmt_float(float value) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(value));
  return buf;
}

const char* on_off(bool value) { return value ? "1" : "0"; }

}  // namespace

bool Ini::parse(std::string_view text) {
  sections_.clear();
  std::string current;
  while (!text.empty()) {
    size_t nl = text.find('\n');
    std::string_view line = trim(text.substr(0, nl));
    text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
    if (line.empty() || line.front() == ';' || line.front() == '#') continue;
    if (line.front() == '[') {
      size_t close = line.find(']');
      if (close != std::string_view::npos) current = std::string(trim(line.substr(1, close - 1)));
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    set(current, trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
  }
  return true;
}

std::string Ini::serialize() const {
  std::string out;
  for (const Section& section : sections_) {
    if (!section.name.empty()) out += "[" + section.name + "]\n";
    for (const Entry& entry : section.entries) out += entry.key + "=" + entry.value + "\n";
    out += "\n";
  }
  return out;
}

std::string Ini::get(std::string_view section, std::string_view key, std::string_view fallback) const {
  for (const Section& sec : sections_) {
    if (sec.name != section) continue;
    for (const Entry& entry : sec.entries)
      if (entry.key == key) return entry.value;
  }
  return std::string(fallback);
}

void Ini::set(std::string_view section, std::string_view key, std::string_view value) {
  Section* sec = nullptr;
  for (Section& candidate : sections_)
    if (candidate.name == section) sec = &candidate;
  if (!sec) {
    sections_.push_back({std::string(section), {}});
    sec = &sections_.back();
  }
  for (Entry& entry : sec->entries) {
    if (entry.key == key) {
      entry.value = std::string(value);
      return;
    }
  }
  sec->entries.push_back({std::string(key), std::string(value)});
}

std::vector<std::pair<std::string, std::string>> Ini::entries(std::string_view section) const {
  std::vector<std::pair<std::string, std::string>> out;
  for (const Section& sec : sections_) {
    if (sec.name != section) continue;
    for (const Entry& entry : sec.entries) out.emplace_back(entry.key, entry.value);
  }
  return out;
}

// Reads every key with its current default as the fallback. Legacy keys of earlier inis still migrate:
//   [Video] Fullscreen=1 -> WindowMode borderless; FilterHD -> Filter (Filter wins); WideHudEdges=1 -> WideHud
//   full; Ps1Res3D -> Ps1Res (Ps1ResMenus is dropped; an ini with FilterHD but no Filter that is not in classic art reads it as off, since
//   Ps1Res3D acted in classic art only); the old percentage Fov, Toasts and Aspect-as-widescreen keys are ignored.
//   [Enhancements] EnhancedLighting / SpellLights (either on) -> Lighting; SeamBlend 0 / 1 / 2 -> EdgeBlend off / on / on.
// Without a valid Aspect key the aspect is the shape of the stored size, so an existing ini keeps its resolution list.
bool Config::load(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  std::stringstream text;
  text << in.rdbuf();
  Ini ini;
  ini.parse(text.str());

  game_dir = ini.get("Paths", "GameDir", game_dir);
  save_dir = ini.get("Paths", "SaveDir", save_dir);
  save_file = ini.get("Paths", "SaveFile", save_file);
  width = std::max(320, parse_int(ini.get("Video", "Width"), width));
  height = std::max(240, parse_int(ini.get("Video", "Height"), height));
  {
    const std::string stored = ini.get("Video", "Aspect");
    bool known = false;
    for (const char* name : kAspects)
      if (stored == name) aspect = stored, known = true;
    if (!known)
      for (const char* name : kAspects)
        if (aspect_matches(name, width, height)) {
          aspect = name;
          break;
        }
  }
  {
    const std::string mode = ini.get("Video", "WindowMode");
    if (mode == "borderless") window_mode = WindowMode::Borderless;
    else if (mode == "exclusive") window_mode = WindowMode::Exclusive;
    else if (mode == "windowed") window_mode = WindowMode::Windowed;
    else if (parse_bool(ini.get("Video", "Fullscreen"), false)) window_mode = WindowMode::Borderless;
  }
  display = std::max(0, parse_int(ini.get("Video", "Display"), display));
  vsync = parse_bool(ini.get("Video", "VSync"), vsync);
  gpu_driver = ini.get("Video", "Driver", gpu_driver);
  if (gpu_driver == "auto") gpu_driver.clear();
  frame_cap = std::clamp(parse_int(ini.get("Video", "FrameCap"), frame_cap), 0, 1000);
  auto read_filter = [&](const char* key, TexFilter fallback) {
    const std::string name = ini.get("Video", key);
    return name == "nearest" ? TexFilter::Nearest : name == "bilinear" ? TexFilter::Bilinear : fallback;
  };
  filter = read_filter("FilterHD", filter);
  filter = read_filter("Filter", filter);
  filter_rooms = read_filter("FilterRooms", filter_rooms);
  filter_effects = read_filter("FilterEffects", filter_effects);
  fov_deg = std::clamp(parse_int(ini.get("Video", "FovDeg"), fov_deg), 30, 110);
  {
    const std::string wide = ini.get("Video", "WideHud");
    if (wide.empty()) wide_hud = parse_int(ini.get("Video", "WideHudEdges"), 0) != 0 ? 2 : wide_hud;
    for (int i = 0; i < 3; ++i)
      if (wide == kWideHudNames[i]) wide_hud = i;
  }
  wide_edge_fade = std::clamp(parse_int(ini.get("Video", "WideEdgeFade"), wide_edge_fade), 0, 96);
  fast_mode = parse_bool(ini.get("Game", "FastMode"), fast_mode);
  slow_mode = parse_bool(ini.get("Game", "SlowMode"), slow_mode) && !fast_mode;
  auto_save = parse_bool(ini.get("Game", "AutoSave"), auto_save);
  fast_rate = std::clamp(parse_int(ini.get("Game", "FastRate"), fast_rate), 0, 5);
  if (fast_rate == 1) fast_rate = 0;
  slow_rate = std::clamp(parse_int(ini.get("Game", "SlowRate"), slow_rate), 10, 90);
  widescreen = parse_bool(ini.get("Video", "Widescreen"), widescreen);
  {
    const std::string mode = ini.get("Video", "ArtMode", "hd");
    if (mode == "original" || mode == "classic") art_mode = ArtMode::Original;
    else if (mode == "modded") art_mode = ArtMode::Modded;
    else {
      if (mode != "hd") log::warn("config: unknown [Video] ArtMode '{}', using hd", mode);
      art_mode = ArtMode::Hd;
    }
  }
  mods.clear();
  dump_stages = parse_bool(ini.get("Mods", "DumpStages"), dump_stages);
  dump_title = parse_bool(ini.get("Mods", "DumpTitle"), dump_title);
  dump_rooms = parse_bool(ini.get("Mods", "DumpRooms"), dump_rooms);
  dump_models = parse_bool(ini.get("Mods", "DumpModels"), dump_models);
  dump_weapons = parse_bool(ini.get("Mods", "DumpWeapons"), dump_weapons);
  dump_effects = parse_bool(ini.get("Mods", "DumpEffects"), dump_effects);
  for (const auto& [name, value] : ini.entries("Mods")) {
    const bool is_setting = name == "DumpTextures" || name == "ReplaceTextures" || name == "DumpStages" || name == "DumpTitle" ||
                            name == "DumpRooms" || name == "DumpModels" || name == "DumpWeapons" || name == "DumpEffects";
    if (is_setting) continue;
    ModSetting mod;
    mod.name = name;
    const size_t comma = value.find(',');
    mod.enabled = parse_bool(std::string(trim(std::string_view(value).substr(0, comma))), true);
    if (comma != std::string::npos) mod.load_on = std::string(trim(std::string_view(value).substr(comma + 1)));
    mods.push_back(std::move(mod));
  }
  {
    const std::string legacy_3d = ini.get("Video", "Ps1Res3D");
    bool legacy_on = parse_bool(legacy_3d, false);
    if (ini.get("Video", "Filter").empty() && !ini.get("Video", "FilterHD").empty() && art_mode != ArtMode::Original) legacy_on = false;
    ps1_res = parse_bool(ini.get("Video", "Ps1Res"), legacy_on);
  }
  {
    const float stored = parse_float(ini.get("Video", "RenderScale"), render_scale);
    float best = 1.0f;
    for (const float scale : kRenderScales)
      if (std::abs(scale - stored) < std::abs(best - stored)) best = scale;
    render_scale = best;
  }
  ps1_jitter = parse_bool(ini.get("Video", "Ps1Jitter"), ps1_jitter);
  crt = parse_bool(ini.get("Video", "Crt"), crt);
  {
    const int stored_msaa = parse_int(ini.get("Video", "Msaa"), msaa);
    msaa = 1;
    for (const int level : kMsaaLevels)
      if (level <= stored_msaa) msaa = level;
    const int stored_anisotropy = parse_int(ini.get("Video", "Anisotropy"), anisotropy);
    anisotropy = 1;
    for (const int level : kAnisotropyLevels)
      if (level <= stored_anisotropy) anisotropy = level;
  }
  smooth_water_line = parse_bool(ini.get("Enhancements", "SmoothWaterLine"), smooth_water_line);
  battle_shadows = parse_bool(ini.get("Enhancements", "BattleShadows"), battle_shadows);
  autohide_cast_hud = parse_bool(ini.get("Enhancements", "AutoHideCastHud"), autohide_cast_hud);
  retain_battle_scenery = parse_bool(ini.get("Enhancements", "RetainBattleScenery"), retain_battle_scenery);
  smooth_effect_rings = parse_bool(ini.get("Enhancements", "SmoothEffectRings"), smooth_effect_rings);
  smooth_reflections = parse_bool(ini.get("Enhancements", "SmoothReflections"), smooth_reflections);
  {
    const bool legacy = parse_bool(ini.get("Enhancements", "EnhancedLighting"), false) || parse_bool(ini.get("Enhancements", "SpellLights"), false);
    lighting = parse_bool(ini.get("Enhancements", "Lighting"), legacy);
  }
  {
    const int legacy = parse_int(ini.get("Enhancements", "SeamBlend"), -1);
    edge_blend = parse_bool(ini.get("Enhancements", "EdgeBlend"), legacy < 0 ? edge_blend : legacy > 0);
  }
  master = clamp01(parse_float(ini.get("Audio", "Master"), master));
  music = clamp01(parse_float(ini.get("Audio", "Music"), music));
  sfx = clamp01(parse_float(ini.get("Audio", "Sfx"), sfx));
  voices = parse_bool(ini.get("Audio", "Voices"), voices);
  voice_volume = std::clamp(parse_int(ini.get("Audio", "VoiceVolume"), voice_volume), 0, 100);
  debug_room = ini.get("Debug", "Room", debug_room);
  debug_flow = ini.get("Debug", "Flow", debug_flow);
  debug_seed = static_cast<std::uint32_t>(std::max(0, parse_int(ini.get("Debug", "Seed"), static_cast<int>(debug_seed))));
  debug_screenshot = parse_bool(ini.get("Debug", "Screenshot"), debug_screenshot);
  debug_autowalk = parse_bool(ini.get("Debug", "AutoWalk"), debug_autowalk);
  debug_action_tick = parse_int(ini.get("Debug", "AutoActionTick"), debug_action_tick);
  debug_auto_target = parse_int(ini.get("Debug", "AutoTarget"), debug_auto_target);
  debug_screenshot_tick = parse_int(ini.get("Debug", "ScreenshotTick"), debug_screenshot_tick);
  language = ini.get("Game", "Language", language);
  settings_x = parse_int(ini.get("Ui", "SettingsX"), settings_x);
  settings_y = parse_int(ini.get("Ui", "SettingsY"), settings_y);
  settings_w = parse_int(ini.get("Ui", "SettingsW"), settings_w);
  settings_h = parse_int(ini.get("Ui", "SettingsH"), settings_h);
  settings_tab = ini.get("Ui", "SettingsTab", settings_tab);
  cheats.no_encounters = parse_bool(ini.get("Cheats", "NoEncounters"), cheats.no_encounters);
  cheats.battle_boost = parse_bool(ini.get("Cheats", "BattleBoost"), cheats.battle_boost);
  cheats.auto_battle = parse_bool(ini.get("Cheats", "AutoBattle"), cheats.auto_battle);
  cheats.god_mode = parse_bool(ini.get("Cheats", "GodMode"), cheats.god_mode);
  cheats.no_clip = parse_bool(ini.get("Cheats", "NoClip"), cheats.no_clip);
  cheats.max_stamina = parse_bool(ini.get("Cheats", "MaxStamina"), cheats.max_stamina);
  cheats.free_elements = parse_bool(ini.get("Cheats", "FreeElements"), cheats.free_elements);
  cheats.always_hit = parse_bool(ini.get("Cheats", "AlwaysHit"), cheats.always_hit);
  cheats.max_money = parse_bool(ini.get("Cheats", "MaxMoney"), cheats.max_money);
  cheats.shop_all_elements = parse_bool(ini.get("Cheats", "ShopAllElements"), cheats.shop_all_elements);
  cheats.shop_all_weapons = parse_bool(ini.get("Cheats", "ShopAllWeapons"), cheats.shop_all_weapons);
  cheats.run_speed = parse_bool(ini.get("Cheats", "RunSpeed"), cheats.run_speed);
  cheats.always_loot = parse_bool(ini.get("Cheats", "AlwaysLoot"), cheats.always_loot);
  cheats.instant_growth = parse_bool(ini.get("Cheats", "InstantGrowth"), cheats.instant_growth);
  cheats.no_craft_cost = parse_bool(ini.get("Cheats", "NoCraftCost"), cheats.no_craft_cost);
  portraits = ini.get("Game", "Portraits", portraits) == "1999" ? "1999" : "2021";
  bgm_source = ini.get("Audio", "BgmSource", bgm_source) == "original" ? "original" : "remastered";
  debug_exit_after_screenshot = parse_bool(ini.get("Debug", "ExitAfterScreenshot"), debug_exit_after_screenshot);
  debug_log = parse_bool(ini.get("Debug", "Log"), debug_log);
  steam = parse_bool(ini.get("Game", "Steam"), steam);
  debug_panels = parse_bool(ini.get("Debug", "Panels"), debug_panels);
  debug_entrance = parse_int(ini.get("Debug", "Entrance"), debug_entrance);
  debug_battle = parse_int(ini.get("Debug", "Battle"), debug_battle);
  debug_battle_auto = parse_bool(ini.get("Debug", "BattleAuto"), debug_battle_auto);
  debug_showcase = parse_bool(ini.get("Debug", "Showcase"), debug_showcase);
  debug_showcase_stage = parse_int(ini.get("Debug", "ShowcaseStage"), debug_showcase_stage);
  debug_script = ini.get("Debug", "Script", debug_script);
  debug_script_speed = std::max(1, parse_int(ini.get("Debug", "ScriptSpeed"), debug_script_speed));
  debug_record = ini.get("Debug", "Record", debug_record);
  debug_replay = ini.get("Debug", "Replay", debug_replay);
  debug_auto_confirm = parse_int(ini.get("Debug", "AutoConfirm"), debug_auto_confirm);
  debug_auto_teleport = parse_bool(ini.get("Debug", "AutoTeleport"), debug_auto_teleport);
  debug_sweep_ticks = parse_int(ini.get("Debug", "SweepTicks"), debug_sweep_ticks);
  debug_sweep_talk = parse_int(ini.get("Debug", "SweepTalk"), debug_sweep_talk);
  debug_show_message = parse_int(ini.get("Debug", "ShowMessage"), debug_show_message);
  debug_show_choice = parse_int(ini.get("Debug", "ShowChoice"), debug_show_choice);
  debug_show_choice_first = parse_int(ini.get("Debug", "ShowChoiceFirst"), debug_show_choice_first);
  debug_show_face = parse_int(ini.get("Debug", "ShowFace"), debug_show_face);
  return true;
}

bool Config::save(const std::filesystem::path& file) const {
  Ini ini;
  ini.set("Paths", "GameDir", game_dir);
  ini.set("Paths", "SaveDir", save_dir);
  if (save_file != "save.dat") ini.set("Paths", "SaveFile", save_file);
  ini.set("Video", "Aspect", aspect);
  ini.set("Video", "Width", std::to_string(width));
  ini.set("Video", "Height", std::to_string(height));
  ini.set("Video", "WindowMode", window_mode == WindowMode::Borderless ? "borderless" : window_mode == WindowMode::Exclusive ? "exclusive" : "windowed");
  ini.set("Video", "Display", std::to_string(display));
  ini.set("Video", "VSync", on_off(vsync));
  ini.set("Video", "Driver", gpu_driver.empty() ? "auto" : gpu_driver);
  ini.set("Video", "FrameCap", std::to_string(frame_cap));
  auto filter_name = [](TexFilter filter_mode) { return filter_mode == TexFilter::Nearest ? "nearest" : "bilinear"; };
  ini.set("Video", "Filter", filter_name(filter));
  ini.set("Video", "FilterRooms", filter_name(filter_rooms));
  ini.set("Video", "FilterEffects", filter_name(filter_effects));
  ini.set("Video", "FovDeg", std::to_string(fov_deg));
  ini.set("Video", "WideHud", kWideHudNames[std::clamp(wide_hud, 0, 2)]);
  ini.set("Video", "WideEdgeFade", std::to_string(wide_edge_fade));
  ini.set("Game", "FastMode", on_off(fast_mode));
  ini.set("Game", "SlowMode", on_off(slow_mode));
  ini.set("Game", "AutoSave", on_off(auto_save));
  ini.set("Game", "FastRate", std::to_string(fast_rate));
  ini.set("Game", "SlowRate", std::to_string(slow_rate));
  ini.set("Video", "Widescreen", on_off(widescreen));
  ini.set("Video", "ArtMode", art_mode == ArtMode::Modded ? "modded" : art_mode == ArtMode::Hd ? "hd" : "original");
  ini.set("Mods", "DumpStages", on_off(dump_stages));
  ini.set("Mods", "DumpTitle", on_off(dump_title));
  ini.set("Mods", "DumpRooms", on_off(dump_rooms));
  ini.set("Mods", "DumpModels", on_off(dump_models));
  ini.set("Mods", "DumpWeapons", on_off(dump_weapons));
  ini.set("Mods", "DumpEffects", on_off(dump_effects));
  for (const ModSetting& mod : mods) ini.set("Mods", mod.name, std::string(mod.enabled ? "1," : "0,") + mod.load_on);
  ini.set("Video", "Ps1Res", on_off(ps1_res));
  ini.set("Video", "RenderScale", fmt_float(render_scale));
  ini.set("Video", "Ps1Jitter", on_off(ps1_jitter));
  ini.set("Video", "Crt", on_off(crt));
  ini.set("Video", "Msaa", std::to_string(msaa));
  ini.set("Video", "Anisotropy", std::to_string(anisotropy));
  ini.set("Enhancements", "SmoothWaterLine", on_off(smooth_water_line));
  ini.set("Enhancements", "BattleShadows", on_off(battle_shadows));
  ini.set("Enhancements", "AutoHideCastHud", on_off(autohide_cast_hud));
  ini.set("Enhancements", "RetainBattleScenery", on_off(retain_battle_scenery));
  ini.set("Enhancements", "SmoothEffectRings", on_off(smooth_effect_rings));
  ini.set("Enhancements", "SmoothReflections", on_off(smooth_reflections));
  ini.set("Enhancements", "Lighting", on_off(lighting));
  ini.set("Enhancements", "EdgeBlend", on_off(edge_blend));
  ini.set("Audio", "Master", fmt_float(master));
  ini.set("Audio", "Music", fmt_float(music));
  ini.set("Audio", "Sfx", fmt_float(sfx));
  ini.set("Audio", "Voices", on_off(voices));
  ini.set("Audio", "VoiceVolume", std::to_string(voice_volume));
  if (!debug_room.empty()) ini.set("Debug", "Room", debug_room);
  if (!debug_flow.empty()) ini.set("Debug", "Flow", debug_flow);
  if (debug_seed) ini.set("Debug", "Seed", std::to_string(debug_seed));
  ini.set("Debug", "Screenshot", on_off(debug_screenshot));
  ini.set("Debug", "AutoWalk", on_off(debug_autowalk));
  ini.set("Debug", "AutoActionTick", std::to_string(debug_action_tick));
  ini.set("Debug", "AutoTarget", std::to_string(debug_auto_target));
  ini.set("Debug", "ScreenshotTick", std::to_string(debug_screenshot_tick));
  ini.set("Game", "Language", language);
  ini.set("Ui", "SettingsX", std::to_string(settings_x));
  ini.set("Ui", "SettingsY", std::to_string(settings_y));
  ini.set("Ui", "SettingsW", std::to_string(settings_w));
  ini.set("Ui", "SettingsH", std::to_string(settings_h));
  ini.set("Ui", "SettingsTab", settings_tab);
  ini.set("Cheats", "NoEncounters", on_off(cheats.no_encounters));
  ini.set("Cheats", "BattleBoost", on_off(cheats.battle_boost));
  ini.set("Cheats", "AutoBattle", on_off(cheats.auto_battle));
  ini.set("Cheats", "GodMode", on_off(cheats.god_mode));
  ini.set("Cheats", "NoClip", on_off(cheats.no_clip));
  ini.set("Cheats", "MaxStamina", on_off(cheats.max_stamina));
  ini.set("Cheats", "FreeElements", on_off(cheats.free_elements));
  ini.set("Cheats", "AlwaysHit", on_off(cheats.always_hit));
  ini.set("Cheats", "MaxMoney", on_off(cheats.max_money));
  ini.set("Cheats", "ShopAllElements", on_off(cheats.shop_all_elements));
  ini.set("Cheats", "ShopAllWeapons", on_off(cheats.shop_all_weapons));
  ini.set("Cheats", "RunSpeed", on_off(cheats.run_speed));
  ini.set("Cheats", "AlwaysLoot", on_off(cheats.always_loot));
  ini.set("Cheats", "InstantGrowth", on_off(cheats.instant_growth));
  ini.set("Cheats", "NoCraftCost", on_off(cheats.no_craft_cost));
  ini.set("Game", "Portraits", portraits);
  ini.set("Audio", "BgmSource", bgm_source);
  ini.set("Debug", "ExitAfterScreenshot", on_off(debug_exit_after_screenshot));
  ini.set("Debug", "Log", on_off(debug_log));
  ini.set("Game", "Steam", on_off(steam));
  ini.set("Debug", "Panels", on_off(debug_panels));
  ini.set("Debug", "Entrance", std::to_string(debug_entrance));
  ini.set("Debug", "AutoConfirm", std::to_string(debug_auto_confirm));
  ini.set("Debug", "AutoTeleport", on_off(debug_auto_teleport));
  ini.set("Debug", "SweepTicks", std::to_string(debug_sweep_ticks));
  ini.set("Debug", "SweepTalk", std::to_string(debug_sweep_talk));
  ini.set("Debug", "ShowMessage", std::to_string(debug_show_message));
  ini.set("Debug", "ShowChoice", std::to_string(debug_show_choice));
  ini.set("Debug", "ShowChoiceFirst", std::to_string(debug_show_choice_first));
  ini.set("Debug", "ShowFace", std::to_string(debug_show_face));
  ini.set("Debug", "Battle", std::to_string(debug_battle));
  ini.set("Debug", "BattleAuto", on_off(debug_battle_auto));
  ini.set("Debug", "Showcase", on_off(debug_showcase));
  ini.set("Debug", "ShowcaseStage", std::to_string(debug_showcase_stage));
  ini.set("Debug", "Script", debug_script);
  ini.set("Debug", "ScriptSpeed", std::to_string(debug_script_speed));
  ini.set("Debug", "Record", debug_record);
  ini.set("Debug", "Replay", debug_replay);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << "; OpenCross settings. Leave GameDir/SaveDir empty to auto-discover.\n\n" << ini.serialize();
  return static_cast<bool>(out);
}

namespace {
const Cheats kNoCheats;
const Cheats* g_cheats = nullptr;
}  // namespace

const Cheats& cheats() { return g_cheats ? *g_cheats : kNoCheats; }
void bind_cheats(const Cheats* set) { g_cheats = set; }

std::string language_from_locales(const std::vector<std::string>& codes) {
  for (const std::string& raw : codes) {
    std::string code;
    for (char letter : raw) {
      if (letter == '-' || letter == '_') break;
      code += static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    }
    if (code == "en" || code == "fr" || code == "it" || code == "de" || code == "es") return code;
    if (code == "ja") return "jp";
  }
  return "en";
}

std::string detect_os_language() {
  std::vector<std::string> codes;
  int count = 0;
  if (SDL_Locale** locales = SDL_GetPreferredLocales(&count)) {
    for (int i = 0; i < count; ++i)
      if (locales[i] && locales[i]->language) codes.emplace_back(locales[i]->language);
    SDL_free(locales);
  }
  return language_from_locales(codes);
}

}  // namespace oc::platform
