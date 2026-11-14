#include "render/imgui_mods_tab.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <imgui.h>
#include <imgui_internal.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "core/features.h"
#include "core/log.h"
#include "platform/paths.h"
#include "render/imgui_layer.h"
#include "render/imgui_nav.h"
#include "render/texture_registry.h"
#include "vfs/vfs.h"

// The Mods tab: a list | detail split with a draggable divider (stacked when the tab is narrow), then a bottom bar (Rescan, Discover Mods, Model Viewer, the Textures
// popover, the restart notice). The list has two tabs in one row style: Mods (drag to reorder, on / off, warnings, Load on) and Engine
// features (addons the player switches, mod kits the mods use). The detail pane shows the selected row.

namespace oc::render {

namespace {

// The list as the tab's cache keys it: order, enabled, load on, file sets.
std::string list_signature(const vfs::ModManager& mods) {
  std::string text;
  for (const vfs::Mod& mod : mods.mods())
    text += mod.name + (mod.enabled ? "|1|" : "|0|") + vfs::load_on_name(mod.load_on) + "|" + std::to_string(mod.files.size()) + "|" + std::to_string(mod.data.size()) + "|" +
            std::to_string(mod.archives.size()) + "|" + std::to_string(mod.textures.size()) + "|" + std::to_string(mod.voices.size()) + "\n";
  return text;
}

// The enabled mods with game data, in list order: the game reads its data from them at launch.
std::string data_signature(const vfs::ModManager& mods) {
  std::string text;
  for (const vfs::Mod& mod : mods.mods())
    if (mod.enabled && !mod.data.empty()) text += mod.name + "\n";
  return text;
}

TextureDumpHooks g_dump_hooks;

// The tab's state for the session: the open list tab, the selection, the split, the mod a gamepad carries.
struct TabState {
  bool features_tab = false;
  // A feature link in a mod's details asks for the Engine features tab.
  bool open_features_tab = false;
  // The selected mod (folder name) and feature id.
  std::string mod;
  int feature = -1;
  // Show the mod kits no enabled mod uses, and the folders in mods with nothing in them (dimmed rows).
  bool show_unused = false;
  bool show_empty = false;
  // The list's share of the width, and of the height when the panes stack.
  float split = 0.5f;
  float stack_split = 0.5f;
  // The Mods list shows 0 every mod, 1..3 the ones that load in Mod / SD / HD.
  int art_filter = 0;
  // The mod (folder name) the gamepad's A carries while it is held on the mod's row, and whether it moved this frame (scroll to it).
  std::string carried;
  bool carried_moved = false;
};
TabState g_state;

// What the game started with: the game-data mods and the resolved features. The tab's state differing from it needs a restart.
struct LaunchState {
  bool taken = false;
  std::string data;
  std::map<int, std::vector<int>> features;
};
LaunchState g_launch;

// A list's row backgrounds: each row a band of the frame colour, a gap of the window colour between rows (BordersInnerH drawn in it). The
// rows' checkboxes and combos get a frame a step lighter than the band, so they stand out from it.
struct RowBands {
  RowBands() {
    ImVec4 band = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    band.w = std::max(band.w, 0.55f);
    const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const ImVec4 control(band.x + (text.x - band.x) * 0.12f, band.y + (text.y - band.y) * 0.12f, band.z + (text.z - band.z) * 0.12f,
                         std::min(band.w + 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg, band);
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, band);
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, control);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, ImGui::GetFontSize() * 0.18f));
  }
  ~RowBands() {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
  }
  static constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit |
                                            ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoSavedSettings;
};

// Puts the next item (`width` wide) against the right edge of the space left on this line.
void align_right(float width) {
  const float avail = ImGui::GetContentRegionAvail().x;
  if (avail > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
}

// A Show unused (n) / Hide unused button at the right end of the current line. True when clicked.
bool unused_toggle(bool shown, size_t count) {
  const std::string label = shown ? std::string("Hide unused") : std::format("Show unused ({})", count);
  ImGui::SameLine();
  align_right(ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f);
  return ImGui::SmallButton(label.c_str());
}

constexpr ImVec4 kWarnColour(0.95f, 0.75f, 0.35f, 1.0f);
constexpr ImVec4 kOverrideColour(0.96f, 0.58f, 0.24f, 1.0f);
constexpr ImVec4 kModColour(0.49f, 0.78f, 0.54f, 1.0f);
constexpr ImVec4 kFilesColour(0.42f, 0.64f, 1.0f, 1.0f);
constexpr ImVec4 kDataColour(0.79f, 0.66f, 1.0f, 1.0f);
constexpr ImVec4 kNewColour(0.49f, 0.78f, 0.54f, 1.0f);
constexpr ImVec4 kKitColour(0.42f, 0.64f, 1.0f, 1.0f);

bool same_key(std::string_view lhs, std::string_view rhs) {
  return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                    [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}

void tip(const char* text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) render::ImGuiLayer::tooltip(text);
}

// A folder in mods with nothing in it: no row (the footer names it).
bool is_empty(const vfs::Mod& mod) {
  return mod.error.empty() && !mod.has_manifest && mod.files.empty() && mod.data.empty() && mod.archives.empty() && mod.textures.empty() &&
         mod.shaders.empty() && mod.voices.empty() && mod.unmatched.empty() && mod.ambiguous.empty();
}

// Game data has no art mode.
bool data_only(const vfs::Mod& mod) { return !mod.data.empty() && mod.files.empty() && mod.textures.empty(); }

// A mod by its mod.ini Name when it has one (the folder name otherwise).
std::string shown_name(const vfs::Mod& mod) { return mod.has_manifest && !mod.manifest.name.empty() ? mod.manifest.name : mod.name; }
std::string shown_name(const vfs::ModManager& mods, const std::string& folder) {
  for (const vfs::Mod& mod : mods.mods())
    if (mod.name == folder) return shown_name(mod);
  return folder;
}

std::string joined(const vfs::ModManager& mods, const std::vector<std::string>& folders) {
  std::string out;
  for (const std::string& folder : folders) out += (out.empty() ? "" : ", ") + shown_name(mods, folder);
  return out;
}

// ---- small icons, each an item of the frame height: the UI font's symbol glyph (merged from Segoe UI Symbol, ImGuiLayer::init), an ASCII
// stand-in where the font lacks it ----
enum class Icon { Grip, Warn, Kit, Reset, Info };

void draw_icon(Icon icon, ImVec2 p, float h, ImU32 colour) {
  struct Glyph {
    ImWchar codepoint;
    const char* utf8;
    const char* ascii;
  };
  static constexpr Glyph kGlyphs[] = {{0x283F, "\xE2\xA0\xBF", ":"}, {0x26A0, "\xE2\x9A\xA0", "!"}, {0x2699, "\xE2\x9A\x99", "*"},
                                      {0x21BA, "\xE2\x86\xBA", "<"}, {0x24D8, "\xE2\x93\x98", "i"}};
  const Glyph& glyph = kGlyphs[static_cast<int>(icon)];
  const char* text = ImGui::GetFont()->FindGlyphNoFallback(glyph.codepoint) ? glyph.utf8 : glyph.ascii;
  const ImVec2 size = ImGui::CalcTextSize(text);
  ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (h - size.x) * 0.5f, p.y + (h - size.y) * 0.5f), colour, text);
}

void icon(Icon kind, const ImVec4& colour) {
  const float h = ImGui::GetFrameHeight();
  ImGui::Dummy(ImVec2(h * 0.8f, h));
  draw_icon(kind, ImVec2(ImGui::GetItemRectMin().x - h * 0.1f, ImGui::GetItemRectMin().y), h, ImGui::GetColorU32(colour));
}

// A glyph as a small button, `height` tall (0: the frame height; the text line height sits it among small buttons): dim and lit while
// hovered, or always in `colour` when given. `now` = its tooltip at once (an info sign), else after the usual delay.
bool icon_button(const char* id, Icon kind, const char* text, bool now = false, float height = 0.0f, const ImVec4* colour = nullptr) {
  const float h = height > 0.0f ? height : ImGui::GetFrameHeight();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(h * 0.8f, h));
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  const ImVec2 p = ImGui::GetItemRectMin();
  if (hot) ImGui::GetWindowDrawList()->AddRectFilled(p, ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
  const ImU32 ink = colour ? ImGui::GetColorU32(*colour) : ImGui::GetColorU32(hot ? ImGuiCol_Text : ImGuiCol_TextDisabled);
  draw_icon(kind, ImVec2(p.x - h * 0.1f, p.y), h, ink);
  if (ImGui::IsItemHovered(now ? ImGuiHoveredFlags_None : ImGuiHoveredFlags_DelayShort)) render::ImGuiLayer::tooltip(text);
  return pressed;
}

// The width of a pick() over `names`: the longest name and the arrow.
float pick_width(std::span<const char* const> names) {
  float width = 0.0f;
  for (const char* name : names) width = std::max(width, ImGui::CalcTextSize(name).x);
  return width + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
}

// A value picked from a few short names: a combo as wide as the longest name, its text in `colour` when given (drawn over an empty preview,
// so the arrow keeps the usual colour). True when the value changed.
bool pick(const char* id, int& value, std::span<const char* const> names, const char* text, const ImVec4* colour = nullptr) {
  const ImVec2 cursor = ImGui::GetCursorScreenPos(), pad = ImGui::GetStyle().FramePadding;
  const ImVec2 at(cursor.x + pad.x, cursor.y + pad.y);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetNextItemWidth(pick_width(names));
  const bool open = ImGui::BeginCombo(id, "");
  dl->AddText(at, colour ? ImGui::GetColorU32(*colour) : ImGui::GetColorU32(ImGuiCol_Text), names[static_cast<size_t>(value)]);
  bool changed = false;
  if (!open) {
    tip(text);
    return false;
  }
  for (size_t k = 0; k < names.size(); ++k)
    if (ImGui::Selectable(names[k], static_cast<size_t>(value) == k)) value = static_cast<int>(k), changed = true;
  ImGui::EndCombo();
  return changed;
}

// A row's selectable over every column, submitted first so the row's own widgets (submitted after, on top) keep their clicks. The cursor
// goes back to the cell's start.
bool row_selectable(bool selected) {
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                         ImVec2(0, ImGui::GetFrameHeight()));
  ImGui::SetCursorScreenPos(start);
  return clicked;
}

// A small section label in the list ("Addons", "Mod kits"), on a line of the frame height.
void group_label(const char* text) {
  ImGui::Spacing();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s", text);
}

// ---- texture dump: the top bar's Textures button and its popover ----
// Texture replacement (render/texture_registry.h) is always on - a texture mod is switched by its own row; dumping is this popover. While a
// dump runs the button shows its progress, so the popover can close.
void draw_textures_button(platform::Config& cfg, bool* settings_dirty) {
  const bool running = g_dump_hooks.running && g_dump_hooks.running();
  const std::string label = running ? std::format("Textures {:.0f} %###textures", (g_dump_hooks.fraction ? g_dump_hooks.fraction() : 0.0f) * 100.0f)
                                    : std::string("Textures###textures");
  if (ImGui::Button(label.c_str())) ImGui::OpenPopup("textures_pop");
  tip("Dump the game's textures as PNGs, to make texture mods from.");
  if (!ImGui::BeginPopup("textures_pop")) return;
  const texture_registry::Stats ts = texture_registry::stats();
  ImGui::TextDisabled("This session: %u dumped, %u replaced%s", ts.dumped, ts.replaced, ts.failed ? " (write errors, see the log)" : "");
  if (g_dump_hooks.start) {
    const unsigned mask = (cfg.dump_stages ? 1u : 0u) | (cfg.dump_title ? 2u : 0u) | (cfg.dump_effects ? 4u : 0u) | (cfg.dump_rooms ? 8u : 0u) |
                          (cfg.dump_models ? 16u : 0u) | (cfg.dump_weapons ? 64u : 0u);
    bool settings = false;
    ImGui::BeginDisabled(running);
    if (ImGui::BeginTable("dump_kinds", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
      ImGui::TableNextColumn();
      settings |= ImGui::Checkbox("Battle stages", &cfg.dump_stages);
      tip("The battle stage (floor and scenery) textures.");
      ImGui::TableNextColumn();
      settings |= ImGui::Checkbox("Title", &cfg.dump_title);
      tip("The title scene textures.");
      ImGui::TableNextColumn();
      settings |= ImGui::Checkbox("Rooms", &cfg.dump_rooms);
      tip("The 3D room meshes and the models in them.");
      ImGui::TableNextColumn();
      settings |= ImGui::Checkbox("Models", &cfg.dump_models);
      tip("The character and monster models.");
      ImGui::TableNextColumn();
      settings |= ImGui::Checkbox("Weapons", &cfg.dump_weapons);
      tip("The battle weapons (every part of every weapon pack, the characters' and the monsters').");
      ImGui::TableNextColumn();
      settings |= ImGui::Checkbox("Effects", &cfg.dump_effects);
      tip("The effect sprite sheets and effect meshes.");
      ImGui::EndTable();
    }
    ImGui::EndDisabled();
    if (settings && settings_dirty) *settings_dirty = true;
    ImGui::BeginDisabled(running || mask == 0);
    if (ImGui::Button("Dump all textures")) g_dump_hooks.start(mask);
    ImGui::EndDisabled();
    tip("Loads the ticked kinds in the background and writes their textures (the ones that are not files) as PNGs to <GameDir>/dump/textures.\n"
        "Battle stages, the title and room meshes come as ONE image per object. The game stays playable meanwhile.\n"
        "Copy files (keeping the sub-folders) into mods/<name>/textures/ to replace them.");
    if (running) {
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) g_dump_hooks.cancel();
      ImGui::ProgressBar(g_dump_hooks.fraction ? g_dump_hooks.fraction() : 0.0f, ImVec2(ImGui::GetFontSize() * 22.0f, 0));
      ImGui::TextUnformatted(g_dump_hooks.status ? g_dump_hooks.status().c_str() : "");
    } else if (g_dump_hooks.summary) {
      const std::string summary = g_dump_hooks.summary();
      if (!summary.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(summary.c_str());
        ImGui::PopTextWrapPos();
      }
    }
  }
  ImGui::EndPopup();
}

// ---- engine features ----

// The player's own value of a setting ([Features] in opencross.ini), or end().
auto own_setting(platform::Config& cfg, std::string_view key) {
  return std::find_if(cfg.features.begin(), cfg.features.end(), [&](const auto& entry) { return same_key(entry.first, key); });
}

// Where a setting's value comes from, as a line of text when a mod sets it: "Set by <mod>.", or, when the player's value replaces the mod's,
// "Overriding <mod>" and a reset glyph in orange, all one button (click: drop the player's value). Nothing for the default or the player's
// own value where no mod sets one. True when the click dropped the player's value.
bool source_note(platform::Config& cfg, const vfs::ModManager& mods, const std::string& key, const std::string& mod_source) {
  if (mod_source.empty()) return false;
  const std::string name = shown_name(mods, mod_source);
  const auto own = own_setting(cfg, key);
  if (own == cfg.features.end()) {
    ImGui::TextDisabled("Set by %s.", name.c_str());
    return false;
  }
  const std::string text = "Overriding " + name;
  const ImGuiStyle& style = ImGui::GetStyle();
  const float h = ImGui::GetFrameHeight(), gap = style.ItemSpacing.x * 0.5f;
  const ImVec2 size = ImGui::CalcTextSize(text.c_str());
  // The text lines up with the text above; the hover fill reaches a frame padding past it.
  const bool pressed = ImGui::InvisibleButton("##reset", ImVec2(size.x + gap + h * 0.8f, h));
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  const ImVec2 p = ImGui::GetItemRectMin(), q = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (hot) dl->AddRectFilled(ImVec2(p.x - style.FramePadding.x, p.y), q, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
  const ImU32 ink = ImGui::GetColorU32(kOverrideColour);
  dl->AddText(ImVec2(p.x, p.y + (h - size.y) * 0.5f), ink, text.c_str());
  draw_icon(Icon::Reset, ImVec2(p.x + size.x + gap - h * 0.1f, p.y), h, ink);
  tip(("Your setting replaces " + name + "'s. Click to use " + name + "'s again.").c_str());
  if (!pressed) return false;
  cfg.features.erase(own);
  return true;
}

// Stores the player's value of a setting. A value equal to `base` (what the mods, or the default, give without the player) is no setting
// of the player's: it is dropped, so the source mark shows the mod or the default again.
void store_setting(platform::Config& cfg, const std::string& key, int value, int base) {
  const auto entry = own_setting(cfg, key);
  if (value == base) {
    if (entry != cfg.features.end()) cfg.features.erase(entry);
  } else if (entry != cfg.features.end()) {
    entry->second = std::to_string(value);
  } else {
    cfg.features.emplace_back(key, std::to_string(value));
  }
}

// A setting's control: a checkbox for 0 / 1, else a number box as wide as its widest value (three digits at least).
bool is_flag(const features::Setting& setting) { return setting.min == 0 && setting.max == 1; }
float control_width(const features::Setting& setting) {
  if (is_flag(setting)) return ImGui::GetFrameHeight();
  const size_t digits = std::max<size_t>({3, std::to_string(setting.min).size(), std::to_string(setting.max).size()});
  return ImGui::CalcTextSize(std::string(digits, '0').c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// One setting as a row of the Settings table (control | label): the control at the left of its column (as wide as the table's widest, so
// every label starts at one x), level with the label's first line; the label wrapping in the rest, and under it where a mod's value comes
// in (source_note). `base` = its value without the player's. True when the player changed it.
bool setting_row(platform::Config& cfg, const vfs::ModManager& mods, const features::Feature& feature, size_t k, int value, int base,
                 const std::string& mod_source) {
  const features::Setting& setting = feature.settings[k];
  const std::string key = std::string(feature.section) + "." + setting.key;
  bool changed = false;
  ImGui::PushID(static_cast<int>(k));
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  bool edited = false;
  if (is_flag(setting)) {
    bool on = value != 0;
    edited = ImGui::Checkbox("##value", &on);
    value = on ? 1 : 0;
  } else {
    ImGui::SetNextItemWidth(control_width(setting));
    edited = ImGui::InputInt("##value", &value, 0, 0);
    value = std::clamp(value, setting.min, setting.max);
    tip(std::format("{} to {}.", setting.min, setting.max).c_str());
  }
  if (edited) {
    store_setting(cfg, key, value, base);
    changed = true;
  }
  ImGui::TableNextColumn();
  ImGui::PushTextWrapPos(0.0f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(setting.label);
  changed |= source_note(cfg, mods, key, mod_source);
  ImGui::PopTextWrapPos();
  ImGui::PopID();
  return changed;
}

// The features as the enabled mods and the player's settings give them (`now`), and as the mods alone give them (`mods_only`: which mod
// sets a key, and the value a player's setting equal to it drops back to).
struct FeatureView {
  features::Resolved now;
  features::Resolved mods_only;
};

FeatureView feature_view(const platform::Config& cfg, const vfs::ModManager& mods) {
  const std::vector<features::ModInput> inputs = mods.feature_inputs();
  return {features::resolve(inputs, cfg.features), features::resolve(inputs, {})};
}

const std::vector<std::string>& users_of(const FeatureView& view, int id) {
  static const std::vector<std::string> kNone;
  const auto it = view.now.users.find(id);
  return it == view.now.users.end() ? kNone : it->second;
}

// The Engine features list: addons (a checkbox and the name) and mod kits (who uses them; the unused ones behind Show unused), one
// selectable row each.
bool draw_feature_list(platform::Config& cfg, const vfs::ModManager& mods, const FeatureView& view) {
  bool settings = false;
  const RowBands bands;
  const ImGuiTableFlags kFlags = RowBands::kFlags;
  if (g_state.feature < 0)
    for (const features::Feature& feature : features::table())
      if (feature.player) {
        g_state.feature = feature.id;
        break;
      }
  group_label("Addons");
  ImGui::SameLine();
  align_right(ImGui::GetFrameHeight() * 0.8f);
  icon_button("##about", Icon::Info,
              "Addons change how the base game plays. Your addon settings replace the ones your mods set.\n"
              "Mod kits do nothing on their own: the mods that use them switch them on.",
              true);
  if (ImGui::BeginTable("addons", 2, kFlags)) {
    ImGui::TableSetupColumn("on");
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    for (const features::Feature& feature : features::table()) {
      if (!*feature.section || !feature.player || feature.settings.empty()) continue;
      ImGui::PushID(feature.id);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (row_selectable(g_state.feature == feature.id)) g_state.feature = feature.id;
      const bool on = view.now.values.at(feature.id)[0] != 0;
      const std::string key = std::string(feature.section) + ".Enabled";
      bool value = on;
      if (ImGui::Checkbox("##on", &value)) {
        store_setting(cfg, key, value ? 1 : 0, view.mods_only.values.at(feature.id)[0]);
        g_state.feature = feature.id;
        settings = true;
      }
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(feature.label);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  // Mod kits: the ones an enabled mod uses, or every one with Show unused.
  size_t used = 0, unused = 0;
  for (const features::Feature& feature : features::table())
    if (*feature.section && !feature.player) ++(users_of(view, feature.id).empty() ? unused : used);
  if (!used && !unused) return settings;
  group_label("Mod kits");
  if (unused && unused_toggle(g_state.show_unused, unused)) g_state.show_unused = !g_state.show_unused;
  if (!used && !g_state.show_unused) ImGui::TextDisabled("No enabled mod uses one.");
  if ((used || g_state.show_unused) && ImGui::BeginTable("kits", 3, kFlags)) {
    ImGui::TableSetupColumn("icon");
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("state");
    for (const features::Feature& feature : features::table()) {
      if (!*feature.section || feature.player) continue;
      const std::vector<std::string>& users = users_of(view, feature.id);
      if (users.empty() && !g_state.show_unused) continue;
      ImGui::PushID(feature.id);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (row_selectable(g_state.feature == feature.id)) g_state.feature = feature.id;
      icon(Icon::Kit, kKitColour);
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(feature.label);
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      if (users.empty()) ImGui::TextDisabled("Unused");
      else ImGui::TextColored(kModColour, "Used by %s", joined(mods, users).c_str());
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  return settings;
}

// A feature's details: what it does; an addon's settings (usable while it is on) and the mods that set it; a mod kit's users.
bool draw_feature_detail(platform::Config& cfg, const vfs::ModManager& mods, const FeatureView& view) {
  const features::Feature* feature = features::find(features::table(), g_state.feature);
  if (!feature) return false;
  bool settings = false;
  render::ImGuiLayer::section(feature->label);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    render::ImGuiLayer::tooltip(std::format("For modders: [{0}] in mod.ini sets it, Requires={0} names it. Level {1} in this build.", feature->section,
                                            feature->level)
                                    .c_str());
  ImGui::PushTextWrapPos(0.0f);
  if (*feature->about) ImGui::TextUnformatted(feature->about);
  ImGui::PopTextWrapPos();
  const std::vector<std::string>& users = users_of(view, feature->id);
  if (feature->player) {
    // Enabled is the list's checkbox (here only where a mod's value comes in); the rest one banded row each (greyed while the addon is off).
    settings |= source_note(cfg, mods, std::string(feature->section) + ".Enabled", view.mods_only.sources.at(feature->id)[0]);
    const bool on = view.now.values.at(feature->id)[0] != 0;
    if (feature->settings.size() > 1) {
      render::ImGuiLayer::section("Settings");
      if (!on) ImGui::TextDisabled("Switch it on in the list to change these.");
      ImGui::BeginDisabled(!on);
      const RowBands bands;
      float gutter = 0.0f;
      for (size_t k = 1; k < feature->settings.size(); ++k) gutter = std::max(gutter, control_width(feature->settings[k]));
      if (ImGui::BeginTable("settings", 2, RowBands::kFlags)) {
        ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, gutter);
        ImGui::TableSetupColumn("setting", ImGuiTableColumnFlags_WidthStretch);
        for (size_t k = 1; k < feature->settings.size(); ++k)
          settings |= setting_row(cfg, mods, *feature, k, view.now.values.at(feature->id)[k], view.mods_only.values.at(feature->id)[k],
                                  view.mods_only.sources.at(feature->id)[k]);
        ImGui::EndTable();
      }
      ImGui::EndDisabled();
    }
  } else {
    render::ImGuiLayer::section("Used by");
    if (users.empty()) ImGui::TextDisabled("No enabled mod.");
    else ImGui::TextColored(kModColour, "%s", joined(mods, users).c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("Nothing to switch: the mods that use it decide.");
    // [Features] lines for a mod kit in opencross.ini (written by hand or for an older build) do nothing, a kit having no settings: they
    // can be removed here.
    const std::string prefix = std::string(feature->section) + ".";
    const bool own = std::any_of(cfg.features.begin(), cfg.features.end(), [&](const auto& entry) {
      return entry.first.size() > prefix.size() && same_key(std::string_view(entry.first).substr(0, prefix.size()), prefix);
    });
    if (own) {
      ImGui::TextColored(kWarnColour, "Your opencross.ini has [Features] lines for it. They do nothing: it has no settings.");
      ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 0.5f);
      if (icon_button("##reset", Icon::Reset, "Remove those lines from opencross.ini.")) {
        std::erase_if(cfg.features, [&](const auto& entry) {
          return entry.first.size() > prefix.size() && same_key(std::string_view(entry.first).substr(0, prefix.size()), prefix);
        });
        settings = true;
      }
    }
  }
  return settings;
}

// ---- mods ----

struct Issue {
  std::string title, detail;
};

std::string first_names(const std::vector<std::string>& names) {
  std::string out;
  for (size_t k = 0; k < names.size() && k < 3; ++k) out += (out.empty() ? "" : ", ") + names[k];
  if (names.size() > 3) out += ", ...";
  return out;
}

std::vector<Issue> mod_issues(const vfs::Mod& mod, size_t index, const std::vector<vfs::ModManager::Conflict>& conflicts) {
  std::vector<Issue> out;
  if (!mod.error.empty()) out.push_back({"Can't be read", mod.error});
  if (mod.may_be_slow) out.push_back({"May run slowly", "A large 7z or RAR can only be read from the start. Extract it into a folder in mods."});
  if (!mod.unmatched.empty())
    out.push_back({std::format("{} file{} match no game file", mod.unmatched.size(), mod.unmatched.size() == 1 ? "" : "s"), first_names(mod.unmatched)});
  if (!mod.ambiguous.empty())
    out.push_back({std::format("{} file{} match several game files", mod.ambiguous.size(), mod.ambiguous.size() == 1 ? "" : "s"),
                   first_names(mod.ambiguous) + ": the same name in two archives"});
  std::vector<std::string> lost;
  for (const auto& conflict : conflicts)
    if (conflict.winner != index) lost.push_back(conflict.target);
  if (!lost.empty())
    out.push_back({std::format("{} file{} replaced by a mod above", lost.size(), lost.size() == 1 ? "" : "s"), first_names(lost)});
  for (const std::string& problem : mod.manifest.problems) out.push_back({"mod.ini", problem});
  if (mod.has_manifest) {
    const features::ModInput input{mod.name, &mod.manifest};
    for (const features::Resolved::Issue& issue : features::resolve({&input, 1}, {}).issues) out.push_back({issue.text, ""});
  }
  return out;
}

// What the tab draws from, per mod (its conflicts and issues) and for the features. Rebuilt only when the list or the player's [Features]
// lines change, or by Rescan (`invalidate`), not every frame.
struct TabCache {
  std::string signature;
  std::vector<std::vector<vfs::ModManager::Conflict>> conflicts;
  std::vector<std::vector<Issue>> issues;
  FeatureView features;
};
TabCache g_cache;

const TabCache& tab_cache(const platform::Config& cfg, const vfs::ModManager& mods) {
  std::string signature = list_signature(mods);
  for (const auto& [key, value] : cfg.features) signature += key + "=" + value + "\n";
  if (signature == g_cache.signature && g_cache.conflicts.size() == mods.mods().size()) return g_cache;
  g_cache.signature = std::move(signature);
  g_cache.conflicts.clear();
  g_cache.issues.clear();
  for (size_t i = 0; i < mods.mods().size(); ++i) {
    g_cache.conflicts.push_back(mods.conflicts(i));
    g_cache.issues.push_back(mod_issues(mods.mods()[i], i, g_cache.conflicts.back()));
  }
  g_cache.features = feature_view(cfg, mods);
  return g_cache;
}

void invalidate() { g_cache.signature.clear(); }

const std::vector<Issue>& issues_of(const TabCache& cache, size_t index) {
  static const std::vector<Issue> kNone;
  return index < cache.issues.size() ? cache.issues[index] : kNone;
}

// A mod's marks at the end of its row: the cog (it uses engine features) and the warning with the issue count, laid out (draw = false:
// only measured). Returns the width.
float draw_contents(const vfs::Mod& mod, const std::vector<Issue>& issues, bool draw) {
  const float h = ImGui::GetFrameHeight(), gap = ImGui::GetStyle().ItemSpacing.x * 0.5f;
  const bool kit = mod.has_manifest && (!mod.manifest.sections.empty() || !mod.manifest.requires_features.empty());
  const std::string issue_count = std::to_string(issues.size());
  if (!draw) {
    float width = kit ? h * 0.8f : 0.0f;
    if (!issues.empty()) width += (kit ? gap : 0.0f) + h * 0.8f + 2.0f + ImGui::CalcTextSize(issue_count.c_str()).x;
    return width;
  }
  if (kit) {
    icon(Icon::Kit, kKitColour);
    tip("Uses engine features (its mod.ini). Select the mod to see which.");
  }
  if (!issues.empty()) {
    if (kit) ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    icon(Icon::Warn, kWarnColour);
    ImGui::SameLine(0, 2.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kWarnColour, "%s", issue_count.c_str());
    ImGui::EndGroup();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
      std::string text;
      for (const Issue& issue : issues) text += (text.empty() ? "" : "\n") + issue.title;
      render::ImGuiLayer::tooltip((text + "\n\nSelect the mod for the details.").c_str());
    }
  }
  return 0.0f;
}

// Load on as the Mods list names it, in vfs::ModLoadOn order; the art filter's names (All = no filter, then the art modes) and the art
// modes of filters 1..3.
constexpr const char* kLoadOnNames[] = {"Mod", "SD", "HD", "All"};
constexpr const char* kFilterNames[] = {"All", "Mod", "SD", "HD"};
constexpr vfs::ModArt kFilterArt[] = {vfs::ModArt::Modded, vfs::ModArt::Sd, vfs::ModArt::Hd};

// The drag and drop payload of a Mods row: its index in the list.
constexpr const char* kModPayload = "OC_MOD_ROW";

// Moves list[from] so it lands before what is now list[to] (to = size: the end).
void move_mod(std::vector<vfs::Mod>& list, size_t from, size_t to) {
  if (to == from || to == from + 1) return;
  if (to > from) std::rotate(list.begin() + static_cast<std::ptrdiff_t>(from), list.begin() + static_cast<std::ptrdiff_t>(from) + 1,
                             list.begin() + static_cast<std::ptrdiff_t>(to));
  else std::rotate(list.begin() + static_cast<std::ptrdiff_t>(to), list.begin() + static_cast<std::ptrdiff_t>(from),
                   list.begin() + static_cast<std::ptrdiff_t>(from) + 1);
}

// The Mods list: grip (drag to reorder), on / off, the name with its marks (cog, warnings) at the right end (sliding under a long name),
// Load on. Alt+Up / Alt+Down moves the selected mod; on a gamepad, A held on a row carries it, the D-pad or the left stick moves it and
// releasing A places it. Above it: the art mode filter, Show empty (folders with nothing in them, dimmed rows) and, at the right end, the
// load order note (the info sign). While a row is dragged a line shows where it will land. True when the list changed.
bool draw_mod_list(vfs::ModManager& mods, const TabCache& cache, vfs::ModArt art) {
  std::vector<vfs::Mod>& list = mods.mods();
  bool changed = false;
  const size_t empties = static_cast<size_t>(std::count_if(list.begin(), list.end(), [](const vfs::Mod& mod) { return is_empty(mod); }));
  const auto listed = [](const vfs::Mod& mod) {
    if (is_empty(mod)) return g_state.show_empty;
    return g_state.art_filter == 0 || data_only(mod) || vfs::loads_on(mod.load_on, kFilterArt[g_state.art_filter - 1]);
  };

  // The bar above the list.
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("Art mode");
  ImGui::SameLine();
  pick("##art_filter", g_state.art_filter, kFilterNames, "Show only the mods that load in this art mode (All: every mod).");
  if (empties) {
    ImGui::SameLine();
    if (ImGui::Button(g_state.show_empty ? "Hide empty" : std::format("Show empty ({})", empties).c_str())) g_state.show_empty = !g_state.show_empty;
    tip("Folders in mods with nothing in them: nothing loads from them.");
  }
  ImGui::SameLine();
  align_right(ImGui::GetFrameHeight() * 0.8f);
  icon_button("##order", Icon::Info,
              "Load order prioritizes mods higher in the list: where two mods change the same file, the higher one wins.\n"
              "Drag a row by its grip to move it (or select it and press Alt+Up / Alt+Down).\n"
              "On a gamepad: hold A on a mod, move it with the D-pad and release A to place it.",
              true);

  // The next listed row from `i` in direction `step`, or -1.
  const auto neighbour = [&](int i, int step) {
    for (int j = i + step; j >= 0 && j < static_cast<int>(list.size()); j += step)
      if (listed(list[static_cast<size_t>(j)])) return j;
    return -1;
  };
  if (g_state.mod.empty() || std::none_of(list.begin(), list.end(), [&](const vfs::Mod& mod) { return mod.name == g_state.mod && listed(mod); }))
    for (const vfs::Mod& mod : list)
      if (listed(mod)) {
        g_state.mod = mod.name;
        break;
      }

  // A move by key: the carried mod by the D-pad or the left stick, or the selected mod by Alt+Up / Alt+Down. While a mod can move its keys
  // are claimed, so the list's navigation does not move the focus with them.
  const ImGuiID mover = ImGui::GetID("##move");
  if (!ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown)) g_state.carried.clear();
  const auto claim = [&](std::initializer_list<ImGuiKey> keys) {
    for (const ImGuiKey key : keys) ImGui::SetKeyOwner(key, mover);
  };
  const auto pressed = [&](ImGuiKey key) { return ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, mover); };
  int step = 0;
  std::string moving;
  if (!g_state.carried.empty()) {
    claim({ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown});
    step = pressed(ImGuiKey_GamepadDpadUp) || pressed(ImGuiKey_GamepadLStickUp)       ? -1
           : pressed(ImGuiKey_GamepadDpadDown) || pressed(ImGuiKey_GamepadLStickDown) ? 1
                                                                                       : 0;
    moving = g_state.carried;
  } else if (ImGui::GetIO().KeyAlt && !ImGui::GetDragDropPayload() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    claim({ImGuiKey_UpArrow, ImGuiKey_DownArrow});
    step = pressed(ImGuiKey_UpArrow) ? -1 : pressed(ImGuiKey_DownArrow) ? 1 : 0;
    moving = g_state.mod;
  }

  // A drop: the dragged row's index and where it lands (before list[drop_to]); the insertion line while dragging (x0, x1, y; y < 0: none).
  size_t drop_from = 0, drop_to = 0;
  bool dropped = false;
  ImVec4 drop_line(0, 0, -1.0f, 0);
  const float h = ImGui::GetFrameHeight();
  const RowBands bands;
  if (ImGui::BeginTable("mods", 4, RowBands::kFlags)) {
    ImGui::TableSetupColumn("grip", ImGuiTableColumnFlags_WidthFixed, h * 0.8f);
    ImGui::TableSetupColumn("on");
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("load");
    for (size_t i = 0; i < list.size(); ++i) {
      vfs::Mod& mod = list[i];
      if (!listed(mod)) continue;
      const bool empty = is_empty(mod);
      // By name, so a row keeps its id (and its grip stays active) when the order changes.
      ImGui::PushID(mod.name.c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const bool selected = g_state.mod == mod.name;
      const bool carried = g_state.carried == mod.name;
      if (row_selectable(selected)) g_state.mod = mod.name;
      bool held = ImGui::IsItemActive();
      if (carried && g_state.carried_moved) {
        ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
        g_state.carried_moved = false;
      }
      // The whole row takes a dragged row: above it from its top half, below it from its bottom half.
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(kModPayload, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
          const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
          const bool below = ImGui::GetIO().MousePos.y >= (a.y + b.y) * 0.5f;
          drop_line = ImVec4(a.x, b.x, below ? b.y + 1.0f : a.y - 1.0f, 0);
          if (payload->IsDelivery()) {
            drop_from = *static_cast<const size_t*>(payload->Data);
            drop_to = below ? i + 1 : i;
            dropped = true;
          }
        }
        ImGui::EndDragDropTarget();
      }
      ImGui::InvisibleButton("##grip", ImVec2(h * 0.8f, h));
      held |= ImGui::IsItemActive();
      const bool grip_hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
      draw_icon(Icon::Grip, ImVec2(ImGui::GetItemRectMin().x - h * 0.1f, ImGui::GetItemRectMin().y), h,
                ImGui::GetColorU32(grip_hot || carried ? ImGuiCol_Text : ImGuiCol_TextDisabled));
      if (grip_hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      // A held on the row (it, or its grip, active from the gamepad) picks the mod up.
      if (held && g_state.carried.empty() && ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        g_state.carried = mod.name;
        g_state.mod = mod.name;
      }
      if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kModPayload, &i, sizeof(i));
        g_state.mod = mod.name;
        ImGui::TextUnformatted(shown_name(mod).c_str());
        ImGui::EndDragDropSource();
      } else {
        tip("Drag to change the load order.");
      }
      ImGui::TableNextColumn();
      ImGui::BeginDisabled(empty);
      changed |= ImGui::Checkbox("##on", &mod.enabled);
      ImGui::EndDisabled();
      tip(empty ? "An empty folder: nothing to switch." : "Switches this mod on or off.");

      // The name cell: the name (and (empty)) from the left; the marks against the right end, sliding under the name when the row is narrow.
      ImGui::TableNextColumn();
      const ImVec2 cell = ImGui::GetCursorScreenPos();
      const float cell_right = cell.x + ImGui::GetContentRegionAvail().x;
      const bool active = !empty && mod.enabled && (data_only(mod) || vfs::loads_on(mod.load_on, art));
      ImGui::AlignTextToFramePadding();
      const std::string name = shown_name(mod);
      if (mod.enabled && !empty) ImGui::TextUnformatted(name.c_str());
      else ImGui::TextDisabled("%s", name.c_str());
      if (empty) {
        ImGui::SameLine();
        ImGui::TextDisabled("(empty)");
      }
      const float name_end = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
      const std::vector<Issue>& issues = issues_of(cache, i);
      const float contents_w = draw_contents(mod, issues, false);
      if (contents_w > 0.0f) {
        const float x = cell_right - contents_w;
        ImGui::PushClipRect(ImVec2(std::max(x, name_end), cell.y - h), ImVec2(cell_right, cell.y + h * 2.0f), true);
        ImGui::SetCursorScreenPos(ImVec2(x, cell.y));
        draw_contents(mod, issues, true);
        ImGui::PopClipRect();
      }

      // Load on, green while the mod is active in the current art mode. A fixed value (game data, an empty folder) is plain text where the
      // combo's would be.
      ImGui::TableNextColumn();
      const char* const kActive = "\nGreen: active in the current art mode.";
      if (empty || data_only(mod)) {
        const char* text = empty ? "-" : "All";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetStyle().FramePadding.x);
        ImGui::AlignTextToFramePadding();
        if (active) ImGui::TextColored(kModColour, "%s", text);
        else ImGui::TextDisabled("%s", text);
        tip(empty ? "An empty folder: nothing loads from it."
                  : (std::string("Game data has no art mode: it loads in every art mode.") + kActive).c_str());
      } else {
        int load_on = static_cast<int>(mod.load_on);
        const std::string text = std::string(mod.data.empty() ? "The art mode this mod loads in: Mod (Modded), SD (Original), HD, or All."
                                                              : "The art mode this mod loads in: Mod (Modded), SD (Original), HD, or All. Its game "
                                                                "data loads in every art mode.") +
                                 kActive;
        if (pick("##load_on", load_on, kLoadOnNames, text.c_str(), active ? &kModColour : nullptr)) {
          mod.load_on = static_cast<vfs::ModLoadOn>(load_on);
          changed = true;
        }
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (step) {
    const auto it = std::find_if(list.begin(), list.end(), [&](const vfs::Mod& mod) { return mod.name == moving && listed(mod); });
    const int from = it == list.end() ? -1 : static_cast<int>(it - list.begin());
    const int to = from < 0 ? -1 : neighbour(from, step);
    if (to >= 0) {
      std::swap(list[static_cast<size_t>(from)], list[static_cast<size_t>(to)]);
      g_state.carried_moved = !g_state.carried.empty();
      changed = true;
    }
  }
  if (drop_line.z >= 0.0f) {
    const ImU32 accent = ImGui::GetColorU32(kFilesColour);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(drop_line.x + 4.0f, drop_line.z), ImVec2(drop_line.y, drop_line.z), accent, 2.0f);
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(drop_line.x + 4.0f, drop_line.z), 3.5f, accent);
  }
  if (dropped && drop_from < list.size() && drop_to != drop_from && drop_to != drop_from + 1) {
    move_mod(list, drop_from, drop_to);
    changed = true;
  }
  return changed;
}

// A mod's details: version / author / folder, its contents, the engine features it sets or uses (links to the Engine features tab), its
// issues.
void draw_mod_detail(const vfs::ModManager& mods, const TabCache& cache) {
  const std::vector<vfs::Mod>& list = mods.mods();
  const auto it = std::find_if(list.begin(), list.end(), [](const vfs::Mod& mod) { return mod.name == g_state.mod; });
  if (it == list.end()) {
    ImGui::TextDisabled("Select a mod to see its details.");
    return;
  }
  const vfs::Mod& mod = *it;
  const size_t index = static_cast<size_t>(it - list.begin());
  render::ImGuiLayer::section(shown_name(mod).c_str());
  std::string meta;
  if (mod.has_manifest && !mod.manifest.version.empty()) meta += "Version " + mod.manifest.version + ", ";
  if (mod.has_manifest && !mod.manifest.author.empty()) meta += "by " + mod.manifest.author + ", ";
  meta += "mods/" + mod.name;
  if (!mod.enabled) meta += " (off)";
  ImGui::TextDisabled("%s", meta.c_str());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) render::ImGuiLayer::tooltip(reinterpret_cast<const char*>(mod.root.u8string().c_str()));

  render::ImGuiLayer::section("Contents");
  const auto line = [](size_t count, const ImVec4& colour, const char* text) {
    if (!count) return;
    ImGui::TextColored(colour, "%zu", count);
    ImGui::SameLine();
    ImGui::TextUnformatted(text);
  };
  line(mod.files.size(), kFilesColour, "replaced files, in the art modes Load on picks");
  line(mod.textures.size(), kFilesColour, "textures");
  line(mod.voices.size(), kFilesColour, "voice lines");
  line(mod.shaders.size(), kFilesColour, "shader looks");
  line(mod.archives.size(), kFilesColour, "archives");
  line(mod.data.size(), kDataColour, "game data files, every art mode");
  line(mod.added.size(), kNewColour, "new files");
  if (index < cache.conflicts.size()) {
    size_t wins = 0;
    for (const auto& conflict : cache.conflicts[index]) wins += conflict.winner == index ? 1 : 0;
    if (wins) ImGui::TextDisabled("Wins %zu file%s over the mods below it.", wins, wins == 1 ? "" : "s");
  }
  if (!mod.data.empty()) ImGui::TextDisabled("Changes game data: switching it on or off, or moving it, applies the next time the game starts.");

  render::ImGuiLayer::section("Engine features");
  if (!mod.has_manifest || (mod.manifest.sections.empty() && mod.manifest.requires_features.empty())) {
    ImGui::TextDisabled("No mod.ini: this mod changes no feature.");
  } else {
    // Each feature this build has as a link, what the mod.ini sets in its tooltip (for modders). A feature this build lacks is an issue.
    size_t shown = 0;
    const auto feature_link = [&](const std::string& section, const std::string& detail) {
      const features::Feature* feature = features::find(features::table(), section);
      if (!feature) return;
      ++shown;
      ImGui::PushID(section.c_str());
      if (ImGui::TextLink(feature->label)) {
        g_state.feature = feature->id;
        g_state.open_features_tab = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) render::ImGuiLayer::tooltip(std::format("[{}] {}", section, detail).c_str());
      ImGui::PopID();
    };
    for (const features::Manifest::Section& section : mod.manifest.sections) {
      std::string values;
      for (const auto& [key, value] : section.values) values += (values.empty() ? "" : ", ") + key + "=" + value;
      feature_link(section.name, values.empty() ? std::string("named in mod.ini") : values);
    }
    for (const std::string& required : mod.manifest.requires_features) {
      if (std::any_of(mod.manifest.sections.begin(), mod.manifest.sections.end(), [&](const auto& section) { return same_key(section.name, required); })) continue;
      feature_link(required, "used by its scripts (Requires)");
    }
    if (!shown) ImGui::TextDisabled("None that this OpenCross has (see Issues).");
  }

  render::ImGuiLayer::section("Issues");
  const std::vector<Issue>& issues = issues_of(cache, index);
  if (issues.empty()) ImGui::TextDisabled("None.");
  ImGui::PushTextWrapPos(0.0f);
  for (const Issue& issue : issues) {
    ImGui::Bullet();
    ImGui::TextColored(kWarnColour, "%s", issue.title.c_str());
    if (!issue.detail.empty()) {
      ImGui::Indent();
      ImGui::TextDisabled("%s", issue.detail.c_str());
      ImGui::Unindent();
    }
  }
  ImGui::PopTextWrapPos();
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
  bool changed = false, settings = false;
  const vfs::ModArt art = mod_art(cfg.art_mode);

  // List | detail, split by a draggable divider; the bar of buttons under them. Narrower than the settings tabs' two columns, the list
  // stacks above the detail (the divider between them). A list edit rebuilds the cache before the detail pane reads it (its per-mod entries
  // follow the order).
  const TabCache& cache = tab_cache(cfg, mods);
  if (!g_launch.taken) g_launch = {true, data_signature(mods), cache.features.now.values};
  const FeatureView& view = cache.features;
  // Game data and features are read once at launch: a restart is needed while either differs from what the game started with.
  const bool restart = data_signature(mods) != g_launch.data || cache.features.now.values != g_launch.features;
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const bool wide = avail.x >= ImGuiLayer::two_column_width();
  const float bar = ImGui::GetFrameHeightWithSpacing() * (restart && !wide ? 2.0f : 1.0f);
  const float height = std::max(avail.y - bar - ImGui::GetStyle().ItemSpacing.y - 1.0f, ImGui::GetFrameHeightWithSpacing() * 10.0f);
  const float divider = ImGui::GetFontSize() * 0.6f;
  const float spacing = ImGui::GetStyle().ItemSpacing.y;
  const float span = wide ? std::max(avail.x - divider, 1.0f) : std::max(height - divider - spacing * 2.0f, 1.0f);
  const float list_size = std::clamp(wide ? g_state.split : g_state.stack_split, 0.28f, 0.8f) * span;

  // The panes are flattened into the tab's navigation; side by side, Up / Down stay in a pane (nav columns).
  const ImGuiID panes = ImGui::GetID("mods_panes");
  if (ImGui::BeginChild("mods_list", wide ? ImVec2(list_size, height) : ImVec2(0, list_size), ImGuiChildFlags_NavFlattened)) {
    if (wide) begin_nav_column(panes, 0);
    if (ImGui::BeginTabBar("mods_left")) {
      size_t shown = 0, on = 0;
      for (const vfs::Mod& mod : mods.mods())
        if (!is_empty(mod)) ++shown, on += mod.enabled ? 1 : 0;
      const std::string mods_label = std::format("Mods ({}/{})###mods", on, shown);
      if (ImGui::BeginTabItem(mods_label.c_str())) {
        g_state.features_tab = false;
        if (ImGui::BeginChild("rows", ImVec2(0, 0), ImGuiChildFlags_NavFlattened)) {
          if (shown == 0) ImGui::TextDisabled("No mods found.");
          changed |= draw_mod_list(mods, cache, art);
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      const ImGuiTabItemFlags flags = g_state.open_features_tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
      g_state.open_features_tab = false;
      if (ImGui::BeginTabItem("Engine features###features", nullptr, flags)) {
        g_state.features_tab = true;
        if (ImGui::BeginChild("rows", ImVec2(0, 0), ImGuiChildFlags_NavFlattened)) settings |= draw_feature_list(cfg, mods, view);
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    if (wide) end_nav_column();
  }
  ImGui::EndChild();

  if (wide) ImGui::SameLine(0, 0);
  // The divider is for the mouse only.
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  ImGui::InvisibleButton("##divider", wide ? ImVec2(divider, height) : ImVec2(avail.x, divider));
  ImGui::PopItemFlag();
  const bool divider_hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  if (divider_hot) ImGui::SetMouseCursor(wide ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
  if (ImGui::IsItemActive()) {
    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    (wide ? g_state.split : g_state.stack_split) = std::clamp((list_size + (wide ? delta.x : delta.y)) / span, 0.28f, 0.8f);
  }
  {
    const ImVec2 p = ImGui::GetItemRectMin();
    const ImVec2 from = wide ? ImVec2(p.x + divider * 0.5f, p.y) : ImVec2(p.x, p.y + divider * 0.5f);
    const ImVec2 to = wide ? ImVec2(from.x, p.y + height) : ImVec2(p.x + avail.x, from.y);
    ImGui::GetWindowDrawList()->AddLine(from, to, ImGui::GetColorU32(divider_hot ? ImGuiCol_SeparatorActive : ImGuiCol_Separator),
                                        divider_hot ? 2.0f : 1.0f);
  }
  if (wide) ImGui::SameLine(0, 0);
  if (changed || settings) tab_cache(cfg, mods);

  // Side padding only: the first header lines up with the list's tab bar.
  const float detail_h = wide ? height : std::max(span - list_size, ImGui::GetFrameHeightWithSpacing() * 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetStyle().WindowPadding.x, 0.0f));
  if (ImGui::BeginChild("mods_detail", ImVec2(0, detail_h), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened)) {
    if (wide) begin_nav_column(panes, 1);
    // Every line wraps at the pane's edge.
    ImGui::PushTextWrapPos(0.0f);
    if (g_state.features_tab) settings |= draw_feature_detail(cfg, mods, view);
    else draw_mod_detail(mods, cache);
    ImGui::PopTextWrapPos();
    if (wide) end_nav_column();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();

  // Bottom bar.
  ImGui::Separator();
  if (ImGui::Button("Rescan")) {
    mods.scan(files, mods.dir().empty() ? files.game_dir() / "mods" : mods.dir(), mods.prefs());
    invalidate();
    changed = true;
  }
  tip("Looks at the mods folder again for new, removed or changed mods.");
  ImGui::SameLine();
  if (ImGui::Button("Discover Mods")) SDL_OpenURL("https://www.nexusmods.com/games/chronocrosstheradicaldreamersedition");
  tip("Opens the game's page on Nexus Mods in your web browser.");
  ImGui::SameLine();
  if (ImGui::Button("Model Viewer")) open_model_viewer();
  tip("Opens a separate window that shows the character and monster models, with and without the enabled mods.");
  ImGui::SameLine();
  draw_textures_button(cfg, settings_dirty);
  // The restart notice: at the right end of the bar, or on a line of its own under it when the tab is narrow.
  if (restart) {
    constexpr const char* kRestart = "Restart required before changes take effect.";
    const float h = ImGui::GetFrameHeight();
    if (wide) {
      ImGui::SameLine();
      align_right(h * 0.8f + ImGui::GetStyle().ItemSpacing.x * 0.5f + ImGui::CalcTextSize(kRestart).x);
    }
    ImGui::BeginGroup();
    icon(Icon::Warn, kOverrideColour);
    ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 0.5f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kOverrideColour, "%s", kRestart);
    ImGui::EndGroup();
    tip("A changed engine feature, or a mod with game data switched on or off or moved: it applies the next time the game starts.\n"
        "Mods with only art and files apply at once.");
  }

  if (settings && settings_dirty) *settings_dirty = true;
  if (changed) store_mod_prefs(cfg, mods);
  return changed;
}

}  // namespace oc::render
