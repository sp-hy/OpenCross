#include "render/imgui_mods_tab.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <imgui.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "core/features.h"
#include "core/log.h"
#include "platform/paths.h"
#include "render/imgui_layer.h"
#include "render/texture_registry.h"
#include "vfs/vfs.h"

namespace oc::render {

namespace {

// Conflicts are recomputed only when the list (order, enabled, load on, file sets) changes, not every frame.
struct ConflictCache {
  std::string signature;
  std::vector<std::vector<vfs::ModManager::Conflict>> per_mod;
};

std::string signature(const vfs::ModManager& mods) {
  std::string text;
  for (const vfs::Mod& mod : mods.mods())
    text += mod.name + (mod.enabled ? "|1|" : "|0|") + vfs::load_on_name(mod.load_on) + "|" + std::to_string(mod.files.size()) + "|" + std::to_string(mod.data.size()) + "|" +
            std::to_string(mod.archives.size()) + "|" + std::to_string(mod.textures.size()) + "|" + std::to_string(mod.voices.size()) + "\n";
  return text;
}

const ConflictCache& conflicts(const vfs::ModManager& mods) {
  static ConflictCache cache;
  std::string sig = signature(mods);
  if (sig != cache.signature || cache.per_mod.size() != mods.mods().size()) {
    cache.signature = std::move(sig);
    cache.per_mod.clear();
    for (size_t i = 0; i < mods.mods().size(); ++i) cache.per_mod.push_back(mods.conflicts(i));
  }
  return cache;
}

TextureDumpHooks g_dump_hooks;

// Texture dump block (bottom of the tab): counters, the Dump button, the category boxes (game::DumpSet bits), progress.
// Texture replacement (render/texture_registry.h) is always on - a texture mod is switched by its own row; dumping is the button here.
void draw_dump_block(platform::Config& cfg, bool* settings_dirty) {
  ImGui::Separator();
  const texture_registry::Stats ts = texture_registry::stats();
  ImGui::TextDisabled("Textures this session: %u dumped, %u replaced%s", ts.dumped, ts.replaced, ts.failed ? " (write errors, see the log)" : "");
  if (g_dump_hooks.start) {
    const bool running = g_dump_hooks.running && g_dump_hooks.running();
    const unsigned mask = (cfg.dump_stages ? 1u : 0u) | (cfg.dump_title ? 2u : 0u) | (cfg.dump_effects ? 4u : 0u) | (cfg.dump_rooms ? 8u : 0u) |
                          (cfg.dump_models ? 16u : 0u) | (cfg.dump_weapons ? 64u : 0u);
    ImGui::BeginDisabled(running || mask == 0);
    if (ImGui::Button("Dump all textures")) g_dump_hooks.start(mask);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      render::ImGuiLayer::tooltip("Loads the checked categories headlessly in the background and writes their textures (the ones that are not files) as PNGs\n"
                                  "to <GameDir>/dump/textures. Battle stages, the title and room meshes come as ONE image per object. The game stays playable meanwhile.\n"
                                  "Copy files (keeping the sub-folders) into mods/<name>/textures/ to replace them.");
    bool settings = false;
    ImGui::BeginDisabled(running);
    settings |= ImGui::Checkbox("Battle stages", &cfg.dump_stages);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) render::ImGuiLayer::tooltip("Include the battle stage (floor and scenery) textures. Part of what Dump all textures writes.");
    ImGui::SameLine();
    settings |= ImGui::Checkbox("Title", &cfg.dump_title);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) render::ImGuiLayer::tooltip("Include the title scene textures. Part of what Dump all textures writes.");
    ImGui::SameLine();
    settings |= ImGui::Checkbox("Rooms (3D meshes + models)", &cfg.dump_rooms);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) render::ImGuiLayer::tooltip("Include the 3D room meshes and the models in them. Part of what Dump all textures writes.");
    ImGui::SameLine();
    settings |= ImGui::Checkbox("Models", &cfg.dump_models);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) render::ImGuiLayer::tooltip("Include the character and monster models. Part of what Dump all textures writes.");
    ImGui::SameLine();
    settings |= ImGui::Checkbox("Weapons", &cfg.dump_weapons);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) render::ImGuiLayer::tooltip("Include the battle weapons (every part of every weapon pack, the characters' and the monsters'). Part of what Dump all textures writes.");
    ImGui::SameLine();
    settings |= ImGui::Checkbox("Effects (sheets + meshes)", &cfg.dump_effects);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) render::ImGuiLayer::tooltip("Include the effect sprite sheets and effect meshes. Part of what Dump all textures writes.");
    ImGui::EndDisabled();
    if (settings && settings_dirty) *settings_dirty = true;
    if (running) {
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) g_dump_hooks.cancel();
      ImGui::SameLine();
      ImGui::ProgressBar(g_dump_hooks.fraction ? g_dump_hooks.fraction() : 0.0f, ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0));
      ImGui::SameLine();
      ImGui::TextUnformatted(g_dump_hooks.status ? g_dump_hooks.status().c_str() : "");
    } else if (g_dump_hooks.summary) {
      const std::string summary = g_dump_hooks.summary();
      if (!summary.empty()) ImGui::TextWrapped("%s", summary.c_str());
    }
  }
}

// Engine features (core/features.h): per setting the value of this run's mods and defaults, and the player's own value ([Features] in
// opencross.ini), which wins. Features are read at launch, so a change takes effect the next time the game starts.
void draw_features_block(platform::Config& cfg, const vfs::ModManager& mods, bool* settings_dirty) {
  ImGui::Separator();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Engine features");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    render::ImGuiLayer::tooltip("Gameplay features that mods turn on with their mod.ini. Your own setting here wins over every mod.");
  ImGui::SameLine();
  ImGui::TextDisabled("Changes take effect the next time the game starts.");
  const auto same_key = [](std::string_view lhs, std::string_view rhs) {
    return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
  };
  const std::vector<features::ModInput> inputs = mods.feature_inputs();
  const features::Resolved now = features::resolve(inputs, cfg.features);
  bool settings = false;
  for (const features::Feature& feature : features::table()) {
    if (!*feature.section) continue;
    ImGui::PushID(feature.id);
    ImGui::TextDisabled("%s", feature.label);
    for (size_t k = 0; k < feature.settings.size(); ++k) {
      const features::Setting& setting = feature.settings[k];
      ImGui::PushID(static_cast<int>(k));
      const std::string key = std::string(feature.section) + "." + setting.key;
      const auto own = std::find_if(cfg.features.begin(), cfg.features.end(), [&](const auto& entry) { return same_key(entry.first, key); });
      const std::string& source = now.sources.at(feature.id)[k];
      int value = now.values.at(feature.id)[k];
      bool edited = false;
      ImGui::Indent();
      if (setting.min == 0 && setting.max == 1) {
        bool on = value != 0;
        edited = ImGui::Checkbox(setting.label, &on);
        value = on ? 1 : 0;
      } else {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        edited = ImGui::SliderInt(setting.label, &value, setting.min, setting.max);
      }
      ImGui::SameLine();
      ImGui::TextDisabled("%s", source.empty() ? "(default)" : source == "opencross.ini" ? "(your setting)" : ("(" + source + ")").c_str());
      if (own != cfg.features.end()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) {
          cfg.features.erase(own);
          settings = true;
          edited = false;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) render::ImGuiLayer::tooltip("Drops your setting: the mods' value (or the default) applies again.");
      }
      if (edited) {
        const auto entry = std::find_if(cfg.features.begin(), cfg.features.end(), [&](const auto& item) { return same_key(item.first, key); });
        if (entry != cfg.features.end()) entry->second = std::to_string(value);
        else cfg.features.emplace_back(key, std::to_string(value));
        settings = true;
      }
      ImGui::Unindent();
      ImGui::PopID();
    }
    ImGui::PopID();
  }
  if (settings && settings_dirty) *settings_dirty = true;
}

}  // namespace

void open_model_viewer() {
  const std::u8string exe = platform::exe_path().u8string();
  const char* args[] = {reinterpret_cast<const char*>(exe.c_str()), "modelview", nullptr};
  // No standard streams: the game has no console to hand down.
  const SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args);
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
  SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
  SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
  SDL_Process* process = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);
  if (process) SDL_DestroyProcess(process);
  else log::warn("model viewer did not start: {}", SDL_GetError());
}

void set_texture_dump_hooks(TextureDumpHooks hooks) { g_dump_hooks = std::move(hooks); }

std::vector<vfs::ModPref> mod_prefs(const platform::Config& cfg) {
  std::vector<vfs::ModPref> out;
  out.reserve(cfg.mods.size());
  for (const platform::ModSetting& setting : cfg.mods) out.push_back({setting.name, setting.enabled, vfs::parse_load_on(setting.load_on)});
  return out;
}

void store_mod_prefs(platform::Config& cfg, const vfs::ModManager& mods) {
  cfg.mods.clear();
  for (const vfs::ModPref& pref : mods.prefs()) cfg.mods.push_back({pref.name, pref.enabled, vfs::load_on_name(pref.load_on)});
}

vfs::ModArt mod_art(platform::ArtMode mode) {
  switch (mode) {
    case platform::ArtMode::Hd: break;
    case platform::ArtMode::Original: return vfs::ModArt::Sd;
    case platform::ArtMode::Modded: return vfs::ModArt::Modded;
  }
  return vfs::ModArt::Hd;
}

bool draw_mods_tab(platform::Config& cfg, vfs::ModManager& mods, const vfs::Vfs& files, bool* settings_dirty) {
  bool changed = false;
  std::vector<vfs::Mod>& list = mods.mods();
  const vfs::ModArt art = mod_art(cfg.art_mode);

  if (ImGui::Button("Rescan")) {
    mods.scan(files, mods.dir().empty() ? files.game_dir() / "mods" : mods.dir(), mods.prefs());
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) render::ImGuiLayer::tooltip("Looks at the mods folder again for new, removed or changed mods.");
  ImGui::SameLine();
  if (ImGui::Button("Discover Mods")) SDL_OpenURL("https://www.nexusmods.com/games/chronocrosstheradicaldreamersedition");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) render::ImGuiLayer::tooltip("Opens the game's page on Nexus Mods in your web browser.");
  ImGui::SameLine();
  if (ImGui::Button("Model Viewer")) open_model_viewer();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    render::ImGuiLayer::tooltip("Opens a separate window that shows the character and monster models, with and without the enabled mods.");
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("Top of the list wins a file several mods replace.");
  if (ImGui::IsItemHovered())
    render::ImGuiLayer::tooltip(mods.dir().empty() ? "<GameDir>/mods" : reinterpret_cast<const char*>(mods.dir().u8string().c_str()));
  if (list.empty()) {
    ImGui::TextDisabled("No mods found.");
    draw_features_block(cfg, mods, settings_dirty);
    draw_dump_block(cfg, settings_dirty);
    if (changed) store_mod_prefs(cfg, mods);
    return changed;
  }

  const ConflictCache& cc = conflicts(mods);
  int move_from = -1, move_to = -1;
  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit |
                                     ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX;
  const float row_h = ImGui::GetFrameHeightWithSpacing();
  constexpr const char* kSlowWarning = "May run slowly: extract it to a folder";
  constexpr const char* kRestartNotice = "Won't change until restart";
  size_t notices = 0;
  float name_w = ImGui::CalcTextSize("Mod").x;
  for (const vfs::Mod& mod : list) {
    name_w = std::max(name_w, ImGui::CalcTextSize(mod.name.c_str()).x);
    if (mod.may_be_slow) ++notices, name_w = std::max(name_w, ImGui::CalcTextSize(kSlowWarning).x);
    if (!mod.data.empty()) ++notices, name_w = std::max(name_w, ImGui::CalcTextSize(kRestartNotice).x);
    if (mod.has_manifest && !mod.manifest.sections.empty()) ++notices;
  }
  const float table_h = std::min(row_h * (static_cast<float>(list.size() + notices) + 1.5f), ImGui::GetContentRegionAvail().y * 0.6f);
  name_w += ImGui::GetStyle().ItemSpacing.x * 2.0f;
  if (ImGui::BeginTable("mods", 6, kFlags, ImVec2(0, table_h))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("On");
    ImGui::TableSetupColumn("Order");
    ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthFixed, name_w);
    ImGui::TableSetupColumn("Load on");
    ImGui::TableSetupColumn("Files");
    ImGui::TableSetupColumn("Issues", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < list.size(); ++i) {
      vfs::Mod& mod = list[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (ImGui::Checkbox("##on", &mod.enabled)) changed = true;
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) render::ImGuiLayer::tooltip("Switches this mod on or off.");
      ImGui::TableNextColumn();
      ImGui::BeginDisabled(i == 0);
      if (ImGui::ArrowButton("##up", ImGuiDir_Up)) move_from = static_cast<int>(i), move_to = static_cast<int>(i) - 1;
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) render::ImGuiLayer::tooltip("Moves the mod up: it wins files over the mods below it.");
      ImGui::SameLine(0, 2);
      ImGui::BeginDisabled(i + 1 >= list.size());
      if (ImGui::ArrowButton("##down", ImGuiDir_Down)) move_from = static_cast<int>(i), move_to = static_cast<int>(i) + 1;
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) render::ImGuiLayer::tooltip("Moves the mod down: the mods above it win files over it.");
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("%zu", i + 1);
      ImGui::TableNextColumn();
      // Game data has no art mode.
      const bool data_only = !mod.data.empty() && mod.files.empty() && mod.textures.empty();
      const bool active = mod.enabled && (data_only || vfs::loads_on(mod.load_on, art));
      ImGui::AlignTextToFramePadding();
      if (active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.9f, 0.45f, 1.0f));
      ImGui::TextUnformatted(mod.name.c_str());
      if (active) ImGui::PopStyleColor();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        render::ImGuiLayer::tooltip(active ? "Active in the current art mode." : "Not active: switched off, or its Load on setting excludes the current art mode.");
      if (mod.may_be_slow) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "%s", kSlowWarning);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
          render::ImGuiLayer::tooltip("A large 7z or RAR can only be read from the start, which may cause long pauses while the game loads. Extract it into a folder in mods to avoid them.");
      }
      if (!mod.data.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "%s", kRestartNotice);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
          render::ImGuiLayer::tooltip("This mod changes game data such as stats, text or scripts. Switching it on or off, or moving it, takes effect the next time the game starts. Game data loads in every art mode.");
      }
      if (mod.has_manifest && !mod.manifest.sections.empty()) {
        std::string names;
        for (const features::Manifest::Section& section : mod.manifest.sections) names += (names.empty() ? "" : ", ") + section.name;
        ImGui::TextDisabled("Features: %s", names.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
          render::ImGuiLayer::tooltip("Engine features this mod turns on (its mod.ini). Switching the mod on or off, or moving it, takes effect the next time the game starts.");
      }
      ImGui::TableNextColumn();
      if (data_only) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("All");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) render::ImGuiLayer::tooltip("Game data has no art mode: it loads in every art mode.");
      } else {
        int load_on = static_cast<int>(mod.load_on);
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("Modded").x + ImGui::GetFrameHeight() * 2.0f);
        if (ImGui::Combo("##loadon", &load_on, "Modded\0SD\0HD\0All\0")) {
          mod.load_on = static_cast<vfs::ModLoadOn>(load_on);
          changed = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
          render::ImGuiLayer::tooltip(mod.data.empty() ? "The art mode this mod loads in: Modded, SD (Original), HD, or All."
                                                       : "The art mode this mod loads in: Modded, SD (Original), HD, or All. Its game data loads in every art mode.");
      }
      ImGui::TableNextColumn();
      {
        std::string files_txt = std::to_string(mod.files.size());
        if (!mod.data.empty()) files_txt += " + " + std::to_string(mod.data.size()) + " data";
        if (!mod.archives.empty()) files_txt += " + " + std::to_string(mod.archives.size()) + " archive";
        if (!mod.textures.empty()) files_txt += " + " + std::to_string(mod.textures.size()) + " texture";
        if (!mod.voices.empty()) files_txt += " + " + std::to_string(mod.voices.size()) + " voice";
        if (!mod.added.empty()) files_txt += " (" + std::to_string(mod.added.size()) + " new)";
        ImGui::TextUnformatted(files_txt.c_str());
      }
      ImGui::TableNextColumn();
      {
        std::string issues;
        auto add = [&](size_t count, const char* what) {
          if (count) issues += (issues.empty() ? "" : ", ") + std::to_string(count) + " " + what;
        };
        add(mod.unmatched.size(), "unmatched");
        add(mod.ambiguous.size(), "ambiguous");
        if (i < cc.per_mod.size() && !cc.per_mod[i].empty()) {
          size_t wins = 0;
          for (const auto& conflict : cc.per_mod[i]) wins += conflict.winner == i ? 1 : 0;
          add(cc.per_mod[i].size() - wins, "overridden");
          add(wins, "wins");
        }
        if (!mod.error.empty()) issues += (issues.empty() ? "cannot read: " : ", cannot read: ") + mod.error;
        add(mod.manifest.problems.size(), "mod.ini problem(s)");
        if (mod.has_manifest) {
          const features::ModInput input{mod.name, &mod.manifest};
          for (const features::Resolved::Issue& issue : features::resolve({&input, 1}, {}).issues) issues += (issues.empty() ? "" : ", ") + issue.text;
        }
        if (issues.empty()) ImGui::TextDisabled("-");
        else ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "%s", issues.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
          render::ImGuiLayer::tooltip("unmatched: files that match no game file and sit in no archive folder (cdrom, hd, ...). ambiguous: files that match several. overridden: another mod above wins "
                            "a file. wins: this mod wins a file another mod also has. mod.ini: lines of the mod's manifest that are not understood, and features or settings this build does not have.");
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (move_from >= 0 && move_to >= 0) {
    std::swap(list[static_cast<size_t>(move_from)], list[static_cast<size_t>(move_to)]);
    changed = true;
  }

  draw_features_block(cfg, mods, settings_dirty);
  draw_dump_block(cfg, settings_dirty);
  if (changed) store_mod_prefs(cfg, mods);
  return changed;
}

}  // namespace oc::render
