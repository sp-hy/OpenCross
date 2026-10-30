#pragma once
// opencross.ini: tiny INI reader/writer plus the typed engine settings stored in it.
#include <cstdint>
#include <filesystem>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace oc::platform {

// Sections of key=value pairs, in file order. ';' and '#' start comment lines. Comments are not preserved on save.
class Ini {
 public:
  bool parse(std::string_view text);
  std::string serialize() const;
  std::string get(std::string_view section, std::string_view key, std::string_view fallback = {}) const;
  void set(std::string_view section, std::string_view key, std::string_view value);
  // All key=value pairs of one section, in file order.
  std::vector<std::pair<std::string, std::string>> entries(std::string_view section) const;

 private:
  struct Entry {
    std::string key, value;
  };
  struct Section {
    std::string name;
    std::vector<Entry> entries;
  };
  std::vector<Section> sections_;
};

// [Video] ArtMode: hd | original | modded (= HD art + the mods in <GameDir>\mods).
enum class ArtMode { Hd, Original, Modded };
// Every mode but Original draws RDE's HD assets (Modded = HD + the mod overlay in the VFS).
inline bool art_is_hd(ArtMode mode) { return mode != ArtMode::Original; }

// The aspect ratios the Display section offers, and whether a w x h size has one (3.5 % tolerance: 16:10 = 8:5, 21:9 takes the
// 2560 x 1080 (64:27) and 3440 x 1440 (43:18) ultrawides, 32:9 the 3840 x 1080 / 5120 x 1440 super-ultrawides).
// "System" = no filter: the Resolution list is the display's own modes (phones, TVs, streaming devices with odd shapes).
inline constexpr const char* kAspects[] = {"4:3", "16:10", "16:9", "21:9", "32:9", "System"};
inline bool aspect_matches(const std::string& aspect, int w, int h) {
  const size_t colon = aspect.find(':');
  if (colon == std::string::npos || h <= 0) return true;
  const double want = std::atof(aspect.c_str()) / std::atof(aspect.c_str() + colon + 1);
  const double have = static_cast<double>(w) / h;
  return want > 0 && have > want * 0.965 && have < want * 1.035;
}

// [Mods] one line per mod folder, in priority order (first wins): <folder name>=<1|0>,<modded|sd|hd|all> (enabled, load on).
struct ModSetting {
  std::string name;
  bool enabled = true;
  std::string load_on = "modded";
};

// [Video] WindowMode: windowed (fixed size Width x Height, not user-resizable), borderless (desktop-size fullscreen window on Display),
// exclusive (fullscreen with the display mode closest to Width x Height).
enum class WindowMode { Windowed, Borderless, Exclusive };
// [Video] Filter*: texture sampling of the 3D content (field actors, title scene) per art mode.
enum class TexFilter { Nearest, Bilinear };

// [Cheats] (Esc overlay "Cheats" tab): every toggle off by default, never locks saving. platform::cheats() is the
// live set (bound to Config::cheats by main) so the rule code in the lower layers can read it.
//   no_encounters      RDE "encounters off" (F3 / L3): ordinary field battles are won without being fought
//   battle_boost       RDE battle boost (F4 / R3): HP / stamina / elements refilled, negative status cleared
//   auto_battle        RDE auto-battle (F5 / L3 + R3): the party plays itself
//   god_mode           the party cannot be knocked out (Battle::set_invincible)
//   no_clip            the leader ignores the walk mesh and the solid entities
//   max_stamina        PC stamina never drops
//   free_elements      element casts cost no gauge and consume no stack copy
//   always_hit         every PC weapon attack hits
//   max_money          G is kept at the maximum
//   shop_all_elements  element shops sell every sellable element
//   shop_all_weapons   the smith lists every craftable weapon / armour / accessory
//   run_speed          field walk / run speed x2
//   always_loot        every enemy drops its rarest drop, every steal lands
//   no_craft_cost      the forge (Zappa) builds without using materials or gold
//   instant_growth     a won fight grants the stat growth of a full star
struct Cheats {
  bool no_encounters = false;
  bool battle_boost = false;
  bool auto_battle = false;
  bool god_mode = false;
  bool no_clip = false;
  bool max_stamina = false;
  bool free_elements = false;
  bool always_hit = false;
  bool max_money = false;
  bool shop_all_elements = false;
  bool shop_all_weapons = false;
  bool run_speed = false;
  bool always_loot = false;
  bool no_craft_cost = false;
  bool instant_growth = false;
};
// The live cheat set (a default all-off one until bind_cheats is called).
const Cheats& cheats();
void bind_cheats(const Cheats* set);

// The typed settings of opencross.ini. Each member's ini section / key and meaning:
//   [Paths]  GameDir / SaveDir (empty = auto-discover), SaveFile: file name inside SaveDir (routes use a scratch copy, never the user's).
//   [Video]  Aspect: the aspect ratio the Resolution list is filtered by (kAspects; an ini without the key takes the shape of its stored size).
//            Width / Height: window size (windowed) or the exclusive fullscreen mode. WindowMode (legacy Fullscreen=1 reads as borderless).
//            Display: monitor index (0 = primary / first). VSync. Driver: the graphics backend (auto | vulkan | direct3d12; changes apply on restart). FrameCap: frames per second limit when VSync is off (0 = unlimited).
//            Filter: nearest | bilinear for the 3D model textures, every art mode (an old FilterHD key migrates; Filter wins);
//            FilterRooms (room background tiles) and FilterEffects (field effect sprites) are the same choice for the other categories.
//            WideEdgeFade: widescreen only, fade the room picture's cut edge into black over this many layer pixels (0 = off).
//            FovDeg: horizontal field of view in degrees of the 4:3 picture (battle, title sea); 35 = the PS1's own (H 512:
//            2 atan(160 / 512) = 34.7); 30..110, larger = wider.
//            WideHud ("Wide Menus/HUD", widescreen only, default full): default = the battle HUD and the main menu on the 4:3 canvas, 16:9 = spread to a
//            16:9 frame's width, full = spread to the window's edges (stored as the kWideHudNames word; old `WideHudEdges=1` reads as full).
//            Widescreen (F7 toggles): the field, world map, battle and the title's 3D sea fill the window's own width (any shape wider
//            than 4:3; render::wide_extra_for); menus, dialogue, the title logo / menu and movies stay 4:3. Off = 4:3 with black bars.
//            ArtMode (see the enum). Ps1Res (Retro look, every art mode): everything is drawn at the PS1's resolution and scaled up with
//            sharp pixels - the field and battle 3D at 320 x 224, the menus / shops / title menu / name entry / Continue / party screens at
//            their PS1 overlay's display size (512x448, 640x448, ...), the battle HUD / menus / results, the pause screen and the field
//            dialogue windows in a 320(+2e) x 240 picture; the PS1 disc textures are used only in classic art.
//            RenderScale: the 3D pictures (field picture, battle 3D, title sea, world map) are drawn into an off-screen target at this
//            multiple of the output resolution (above 1 = supersampled, area-filtered down; below 1 = bilinear up) and composited under
//            the 2D / UI drawn at output resolution; with Ps1Res the multiple of the PS1 320 x 224 target instead (2 = 640 x 448); one of
//            kRenderScales (snapped on load). Ps1Jitter: GTE integer vertex snapping + affine texture mapping in the 3D pipelines
//            (render/ps1_jitter.h). Crt: final full-frame CRT post pass (scanlines, mask, vignette), ImGui excluded (render/crt_pass.h).
//            Msaa: multisample antialiasing samples of the window-resolution pictures (1 = off, 2, 4, 8; lowered to what the GPU
//            supports); the PS1-resolution passes stay aliased. Anisotropy: anisotropic filtering level of the bilinear textures of the
//            3D models and stages (1 = off, 2, 4, 8, 16).
//   [Game]   FastMode / SlowMode (RDE's speed toggles; exclusive; hotkeys F2 / F1 (platform/keymap.h),
//            pad RT / LT): the simulation (field, battle) runs FastRate x faster (0 = RDE's: 4x in the field, 2x in battle; 2..5 =
//            that many times; FixedStep caps at 5 ticks per frame) or at SlowRate percent (10..90, default RDE's 50); movies stay at
//            normal speed. Both are off in the Radius tutorial (map / battle 386). AutoSave: RDE's auto-save (card 1 slot 0) on every location change.
//            Language: en fr it de es jp (dialogue text). Portraits: 2021 or 1999 (RDE face art set).
//   [Mods]   one line per mod folder (ModSetting; folders not listed get the defaults, Esc overlay "Mods" tab). DumpStages / DumpTitle /
//            DumpRooms / DumpModels / DumpEffects: which categories the Mods tab "Dump all textures" button writes (battle stage + title 3D
//            scenes; room 3D meshes + models; battle / field unit models; effect sprite sheets + effect meshes - large, off by default).
//   [Enhancements] (all independent of Ps1Res) SmoothEffectRings: ring / tube effect meshes get extra segments.
//            SmoothWaterLine: submerged actor parts are cut at the exact water plane, not per whole triangle. Lighting: key light on the
//            models + bright spell effects light the battle units and stage. EdgeBlend: cross-fades the faces along the texture seams of
//            the stage / title meshes, any art mode. SmoothReflections: env-mapped model parts (FATE, Lavos, ...) slide their reflection
//            sub-texel (off = the PS1's one-texel steps). BattleShadows: smooth per-vertex floor darkening in battles.
//            AutoHideCastHud: the battle HUD / menus / windows are hidden during element, tech and summon casts (as pad Select held).
//            RetainBattleScenery: the battle stage's intro-only layer (bg_00's sea, fish and birds, dust, smoke ...) stays for the whole battle.
//   [Audio]  Master / Music / Sfx: bus gains 0..1. Voices: play the voice lines of the mods' voices folders.
//            VoiceVolume 0..100 (the voice bus gain, times Master). BgmSource: remastered (RDE HCA files) or original (PS1 AKAO sequences).
//   [Debug]  Room: jump straight to this field room; empty = start at the title. Flow: title | name | slots | opening | field (field = no
//            flow, uses Room or alu_a01f); empty = title unless Room is set. Seed: seeds every RNG (field, battle, effects) for
//            reproducible runs; 0 = unseeded battles / effects. Screenshot: save the first rendered frame to screenshot.png next to the
//            exe; ScreenshotTick: its tick (-1 = 40, or AutoActionTick + 90 with AutoWalk); ExitAfterScreenshot: quit once it is written.
//            AutoWalk: walk the player to the nearest talkable NPC and press the action key at AutoActionTick; AutoTarget: entity index to
//            walk to (-1 = nearest NPC with an ON_ACTION script); AutoConfirm: press confirm every N ticks while a dialogue is open
//            (0 = never); AutoTeleport: put the player next to the target at once instead of walking. Log: write opencross.log next to
//            the exe (Debug tab checkbox; --config runs and Debug builds always log). Panels: show the field debug panels (F8 toggles).
//            Entrance: ROOM_PARAM (entrance record) the debug room is entered with. SweepTicks: load every room in turn, run it for N
//            ticks, log failures, then exit (0 = off); SweepTalk: with SweepTicks, also start every entity's ON_ACTION / ON_COLLISION for
//            N ticks (softlock hunt). ShowMessage: open this dialogue message of the debug room at tick 30 (-1 = off); ShowChoice: make
//            lines 0..N the in-window choices (-1 = plain message); ShowChoiceFirst: the first choice line (E1 operand high nibble);
//            ShowFace: force this portrait (face) id on the message (-1 = the scripts' own). Script: route file of timed inputs / commands
//            (src/game/route_script.h); the run exits when it ends; ScriptSpeed: simulation ticks per rendered frame while it runs.
//            Record: write the real session's inputs to this file (replayable); Replay: replay a recorded session (room / entrance come
//            from the file). Battle: start directly in a battle with this encounter scene id (-1 = off); BattleAuto: the battle plays
//            itself (first ready PC: Attack -> fierce on the first enemy).
//   [Ui]     SettingsX / Y / W / H: the Esc settings window's position / size in UNSCALED logical units (window pixels / ImGui UI scale),
//            -1 = default placement; SettingsTab: the last tab's name, empty = General.
struct Config {
  std::string game_dir;
  std::string save_dir;
  std::string save_file = "save.dat";
  std::string aspect = "4:3";
  int width = 1280;
  int height = 960;
  WindowMode window_mode = WindowMode::Windowed;
  int display = 0;
  bool vsync = true;
  // SDL GPU driver name; empty = SDL's choice.
  std::string gpu_driver;
  int frame_cap = 0;
  TexFilter filter = TexFilter::Bilinear;
  TexFilter filter_rooms = TexFilter::Nearest;
  TexFilter filter_effects = TexFilter::Bilinear;
  int wide_edge_fade = 4;
  int fov_deg = 35;
  int wide_hud = 2;
  static constexpr const char* kWideHudNames[] = {"default", "16:9", "full"};
  bool fast_mode = false;
  bool slow_mode = false;
  bool auto_save = true;
  int fast_rate = 0;
  int slow_rate = 50;
  bool widescreen = false;
  ArtMode art_mode = ArtMode::Hd;
  std::vector<ModSetting> mods;
  bool dump_stages = true, dump_title = true, dump_rooms = true, dump_models = true, dump_weapons = true, dump_effects = false;
  bool ps1_res = false;
  float render_scale = 1.0f;
  static constexpr float kRenderScales[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
  bool ps1_jitter = false;
  bool smooth_effect_rings = true;
  bool smooth_water_line = true;
  bool lighting = false;
  bool edge_blend = false;
  bool smooth_reflections = true;
  bool battle_shadows = true;
  bool autohide_cast_hud = false;
  bool retain_battle_scenery = false;
  bool crt = false;
  int msaa = 1;
  static constexpr int kMsaaLevels[] = {1, 2, 4, 8};
  int anisotropy = 1;
  static constexpr int kAnisotropyLevels[] = {1, 2, 4, 8, 16};
  float master = 1.0f;
  float music = 1.0f;
  float sfx = 1.0f;
  bool voices = true;
  int voice_volume = 100;
  std::string debug_room;
  std::string debug_flow;
  std::uint32_t debug_seed = 0;
  bool debug_screenshot = false;
  bool debug_autowalk = false;
  int debug_action_tick = 200;
  int debug_auto_target = -1;
  int debug_screenshot_tick = -1;
  std::string language = "en";
  std::string portraits = "2021";
  std::string bgm_source = "remastered";
  bool debug_exit_after_screenshot = false;
  bool debug_log = false;
  bool steam = false;
  bool debug_panels = false;
  int debug_entrance = 0;
  int debug_auto_confirm = 0;
  bool debug_auto_teleport = false;
  int debug_sweep_ticks = 0;
  int debug_sweep_talk = 0;
  int debug_show_message = -1;
  int debug_show_choice = -1;
  int debug_show_choice_first = 0;
  int debug_show_face = -1;
  std::string debug_script;
  int debug_script_speed = 2;
  std::string debug_record;
  std::string debug_replay;
  int debug_battle = -1;
  bool debug_battle_auto = false;
  // [Debug] Showcase / ShowcaseStage: the Battle= scene shows every playable character on stage bg_NN (-1 = the
  // scene's own) with the free camera on.
  bool debug_showcase = false;
  int debug_showcase_stage = -1;

  Cheats cheats;
  int settings_x = -1, settings_y = -1, settings_w = -1, settings_h = -1;
  std::string settings_tab;

  // load() returns false when the file is missing or unreadable (defaults stay in place).
  bool load(const std::filesystem::path& file);
  bool save(const std::filesystem::path& file) const;
};

// First launch (no opencross.ini): the [Game] Language the OS asks for: en fr it de es -> the same, ja -> jp, anything else en.
// `codes` are SDL_GetPreferredLocales() language codes in preference order ("en", "ja", "pt"...); the first supported one wins.
std::string language_from_locales(const std::vector<std::string>& codes);
// SDL_GetPreferredLocales() mapped by language_from_locales (SDL must be initialised); "en" when the OS reports nothing.
std::string detect_os_language();

}  // namespace oc::platform
