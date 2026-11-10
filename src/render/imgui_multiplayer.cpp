#include "render/imgui_multiplayer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <format>
#include <map>

#include <imgui.h>

#include "net/multiplayer_client.h"
#include "render/imgui_layer.h"

namespace oc::render {

namespace {

using net::MultiplayerClient;

// The panel's lines when closed: how many, and how long a new line stays before it fades out.
constexpr size_t kPanelLines = 8;
constexpr double kLineShow = 10.0, kLineFade = 2.0;
// How many lines the open panel scrolls through, and how many of them fit in its height.
constexpr size_t kScrollLines = 100;
constexpr float kOpenRows = 10.0f;
// A bubble shows for kBubbleBase seconds plus kBubblePerByte per byte of text, at most kBubbleMax, fading over its last kBubbleFade; it
// wraps at kBubbleWidth font heights and keeps at most kBubbleBytes of the line.
constexpr double kBubbleBase = 4.0, kBubblePerByte = 0.05, kBubbleMax = 9.0, kBubbleFade = 0.6;
constexpr float kBubbleWidth = 14.0f;
constexpr size_t kBubbleBytes = 120;

// The Star Level after a star sign (U+2605), or after '*' when the font lacks the star.
std::string star_text(ImFont* font, int star) {
  const bool has_star = font && font->FindGlyphNoFallback(0x2605) != nullptr;
  return std::format("{}{}", has_star ? "\u2605" : "*", star);
}

const net::RemoteParty* party_of(const MultiplayerClient& client, const std::string& pid) {
  for (const net::RemoteParty& party : client.parties())
    if (party.pid == pid) return &party;
  return nullptr;
}

ImVec4 status_colour(MultiplayerClient::State state) {
  switch (state) {
    case MultiplayerClient::State::Connected: return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
    case MultiplayerClient::State::Banned:
    case MultiplayerClient::State::VersionTooOld: return ImVec4(0.95f, 0.55f, 0.45f, 1.0f);
    default: return ImVec4(0.92f, 0.82f, 0.50f, 1.0f);
  }
}

// Crossed swords for a player in a battle, a menu icon (a box with three bars) for one in a menu, a speech bubble for one typing in the
// chat; `size` pixels high, its top-left at `at`.
void draw_state_icon(ImDrawList* dl, ImVec2 at, float size, net::PresenceState state, ImU32 colour) {
  const float w = size * 1.1f, h = size * 0.8f, y = at.y + size * 0.1f;
  if (state == net::PresenceState::Battle) {
    const float thick = std::max(1.5f, size * 0.11f);
    const float guard = size * 0.16f;
    for (const float side : {1.0f, -1.0f}) {
      const float left = side > 0 ? at.x : at.x + w;
      const ImVec2 tip(left, y);
      const ImVec2 pommel(left + side * w, y + h);
      const ImVec2 cross(tip.x + (pommel.x - tip.x) * 0.72f, tip.y + (pommel.y - tip.y) * 0.72f);
      dl->AddLine(tip, pommel, colour, thick);
      const float blade_x = pommel.x - tip.x, blade_y = pommel.y - tip.y;
      const float length = std::hypot(blade_x, blade_y);
      const ImVec2 across(blade_y / length * guard, -blade_x / length * guard);
      dl->AddLine(ImVec2(cross.x - across.x, cross.y - across.y), ImVec2(cross.x + across.x, cross.y + across.y), colour, thick);
    }
    return;
  }
  if (state == net::PresenceState::Menu) {
    dl->AddRect(ImVec2(at.x, y), ImVec2(at.x + w, y + h), colour, size * 0.12f, 0, std::max(1.0f, size * 0.08f));
    for (int i = 1; i <= 3; ++i) {
      const float line_y = y + h * static_cast<float>(i) / 4.0f;
      dl->AddLine(ImVec2(at.x + w * 0.22f, line_y), ImVec2(at.x + w * 0.78f, line_y), colour, std::max(1.0f, size * 0.08f));
    }
    return;
  }
  const float body = h * 0.78f;
  dl->AddRectFilled(ImVec2(at.x, y), ImVec2(at.x + w, y + body), colour, size * 0.25f);
  dl->AddTriangleFilled(ImVec2(at.x + w * 0.25f, y + body - 1.0f), ImVec2(at.x + w * 0.5f, y + body - 1.0f), ImVec2(at.x + w * 0.2f, y + h), colour);
  for (int i = 1; i <= 3; ++i)
    dl->AddCircleFilled(ImVec2(at.x + w * static_cast<float>(i) / 4.0f, y + body * 0.5f), size * 0.07f, IM_COL32(20, 22, 30, 255));
}

}  // namespace

bool MultiplayerUi::typing() { return ImGui::GetIO().WantTextInput; }

void MultiplayerUi::open_chat() {
  chat_open_ = true;
  focus_input_ = true;
}

bool MultiplayerUi::edit_text(const char* id, const char* hint, std::string& value, TextField& field) {
  if (!field.active) std::snprintf(field.buffer, sizeof field.buffer, "%s", value.c_str());
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::InputTextWithHint(id, hint, field.buffer, sizeof field.buffer);
  field.active = ImGui::IsItemActive();
  if (!ImGui::IsItemDeactivatedAfterEdit() || value == field.buffer) return false;
  value = field.buffer;
  return true;
}

bool MultiplayerUi::draw_tab(platform::MultiplayerSettings& settings, const MultiplayerClient* client, const std::string& default_name,
                             bool allowed, ImFont* font) {
  bool changed = false;
  ImGuiLayer::section("Multiplayer");
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextDisabled("See other players' parties walk in your room and chat with them. Nothing of your game is shared but your name, "
                      "Star Level, room and the party leader's position.");
  ImGui::PopTextWrapPos();
  if (ImGuiLayer::begin_rows("mp_rows")) {
    ImGuiLayer::row("Enabled", "Connect to the multiplayer server while the game runs.");
    changed |= ImGui::Checkbox("##enabled", &settings.enabled);
    ImGuiLayer::row("Status");
    if (!allowed) {
      ImGui::TextDisabled("Test runs never connect");
    } else if (!settings.enabled) {
      ImGui::TextDisabled("Off");
    } else if (settings.url().empty()) {
      ImGui::TextColored(status_colour(MultiplayerClient::State::Waiting), "%s",
                         "No server address");
    } else if (client) {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(status_colour(client->state()), "%s", client->status_text().c_str());
      ImGui::PopTextWrapPos();
    }
    ImGuiLayer::row("Server", "Master: the public OpenCross server. Local / other: a server you run on this machine, or anyone's own, at the address below.");
    int server = static_cast<int>(settings.server);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##server", &server, "Master\0Local / other\0")) {
      settings.server = static_cast<platform::MultiplayerServer>(server);
      changed = true;
    }
    if (settings.server == platform::MultiplayerServer::Other) {
      ImGuiLayer::row("Address", "The server's address: ws://127.0.0.1:8787/ws for one run on this machine, wss://... for one online.");
      changed |= edit_text("##address", "ws://127.0.0.1:8787/ws", settings.server_url, server_url_);
    }
    ImGuiLayer::row("Name", "The name other players see. Empty: your Steam name when Steam is on, else Player.");
    changed |= edit_text("##name", default_name.c_str(), settings.name, name_);
    ImGuiLayer::end_rows();
  }

  ImGuiLayer::section("Other players");
  if (ImGuiLayer::begin_rows("mp_draw")) {
    ImGuiLayer::row("Show players", "Draw the other players' parties in your room.");
    changed |= ImGui::Checkbox("##show", &settings.show_players);
    ImGuiLayer::row("Most parties", "At most this many parties are drawn, the ones nearest the camera.");
    ImGui::SetNextItemWidth(-FLT_MIN);
    changed |= ImGui::SliderInt("##max", &settings.max_parties, 1, 32, "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGuiLayer::row("Translucent", "Draw the other parties see-through.");
    changed |= ImGui::Checkbox("##translucent", &settings.translucent);
    ImGuiLayer::row("Name labels", "Show each player's name and Star Level above their party.");
    changed |= ImGui::Checkbox("##labels", &settings.show_labels);
    ImGuiLayer::row("Chat bubbles", "A line said to the room also shows in a bubble over the speaker's party, yours included.");
    changed |= ImGui::Checkbox("##bubbles", &settings.chat_bubbles);
    ImGuiLayer::row("Mute hides party", "A muted player's party is hidden as well as their chat.");
    changed |= ImGui::Checkbox("##mutehides", &settings.mute_hides_party);
    ImGuiLayer::end_rows();
  }

  ImGuiLayer::section("Chat");
  if (ImGuiLayer::begin_rows("mp_chat")) {
    ImGuiLayer::row("Show chat", "Always: the last lines stay on screen. New lines: a line shows for a few seconds. Hidden: only while you type.");
    int chat = static_cast<int>(settings.chat);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##chat", &chat, "Always\0New lines\0Hidden\0")) {
      settings.chat = static_cast<platform::ChatDisplay>(chat);
      changed = true;
    }
    ImGuiLayer::row("Keys");
    ImGui::TextDisabled("T opens the chat, Tab switches Room / Global, Enter sends, Esc closes");
    ImGuiLayer::end_rows();
  }

  ImGuiLayer::section("Players in this room");
  const std::vector<net::RemoteParty>* parties = client && client->state() == MultiplayerClient::State::Connected ? &client->parties() : nullptr;
  if (!parties || parties->empty()) ImGui::TextDisabled("Nobody else is here.");
  if (parties && !parties->empty() && ImGui::BeginTable("mp_players", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
    for (const net::RemoteParty& party : *parties) {
      ImGui::PushID(party.pid.c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::PushFont(font);
      ImGui::Text("%s  %s", party.name.c_str(), star_text(font, party.star).c_str());
      ImGui::PopFont();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGuiLayer::tooltip(("Player " + party.pid).c_str());
      ImGui::TableNextColumn();
      const auto muted = std::find(settings.muted.begin(), settings.muted.end(), party.pid);
      const bool is_muted = muted != settings.muted.end();
      if (ImGui::SmallButton(is_muted ? "Unmute" : "Mute")) {
        if (is_muted) settings.muted.erase(muted);
        else settings.muted.push_back(party.pid);
        changed = true;
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (!settings.muted.empty()) {
    ImGui::Text("%d muted", static_cast<int>(settings.muted.size()));
    ImGui::SameLine();
    if (ImGui::SmallButton("Unmute all")) {
      settings.muted.clear();
      changed = true;
    }
  }
  return changed;
}

void MultiplayerUi::draw_chat(MultiplayerClient& client, const platform::MultiplayerSettings& settings, double now, ImFont* font) {
  if (!chat_open_ && settings.chat == platform::ChatDisplay::Hidden) return;
  std::vector<const net::ChatLine*> lines;
  for (const net::ChatScope scope : {net::ChatScope::Room, net::ChatScope::Global})
    for (const net::ChatLine& line : client.chat(scope))
      if (line.pid.empty() || !client.muted(line.pid)) lines.push_back(&line);
  std::stable_sort(lines.begin(), lines.end(), [](const net::ChatLine* lhs, const net::ChatLine* rhs) { return lhs->ts < rhs->ts; });

  // Closed: the recent lines with their fade (always: the last lines, fully shown).
  std::vector<std::pair<const net::ChatLine*, float>> shown;
  if (!chat_open_) {
    for (size_t i = lines.size() > kPanelLines ? lines.size() - kPanelLines : 0; i < lines.size(); ++i) {
      float alpha = 1.0f;
      if (settings.chat == platform::ChatDisplay::New) {
        const double age = now - lines[i]->received;
        if (!lines[i]->fresh || age >= kLineShow) continue;
        alpha = static_cast<float>(std::clamp((kLineShow - age) / kLineFade, 0.0, 1.0));
      }
      shown.emplace_back(lines[i], alpha);
    }
    if (shown.empty()) return;
  }

  const ImGuiIO& io = ImGui::GetIO();
  ImGui::PushFont(font);
  const float em = ImGui::GetFontSize();
  const float pad = em * 0.6f;
  const float width = std::min(io.DisplaySize.x - 2.0f * pad, em * 32.0f);
  ImGui::SetNextWindowPos(ImVec2(pad, io.DisplaySize.y - pad), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, io.DisplaySize.y * 0.6f));
  float panel_alpha = 0.0f;
  for (const auto& entry : shown) panel_alpha = std::max(panel_alpha, entry.second);
  ImGui::SetNextWindowBgAlpha(chat_open_ ? 0.6f : 0.35f * panel_alpha);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (!chat_open_) flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus;
  const auto line_colour = [](const net::ChatLine& line, float alpha) {
    if (line.pid.empty()) return ImVec4(0.65f, 0.65f, 0.70f, alpha);
    return line.scope == net::ChatScope::Global ? ImVec4(0.70f, 0.85f, 1.0f, alpha) : ImVec4(0.96f, 0.96f, 0.98f, alpha);
  };
  const auto draw_line = [&](const net::ChatLine& line, float alpha) {
    ImGui::PushStyleColor(ImGuiCol_Text, line_colour(line, alpha));
    const char* scope = line.scope == net::ChatScope::Global ? "G" : "R";
    if (line.pid.empty()) {
      ImGui::TextWrapped("%s", line.text.c_str());
    } else if (!line.admin) {
      ImGui::TextWrapped("[%s] %s: %s", scope, line.name.c_str(), line.text.c_str());
    } else {
      ImGui::Text("[%s] ", scope);
      ImGui::SameLine(0.0f, 0.0f);
      const ImVec2 name_at = ImGui::GetCursorScreenPos();
      const ImVec4 red(1.0f, 0.32f, 0.30f, alpha);
      ImGui::TextColored(red, "%s", line.name.c_str());
      ImGui::GetWindowDrawList()->AddText(ImVec2(name_at.x + 1.0f, name_at.y), ImGui::ColorConvertFloat4ToU32(red), line.name.c_str());
      ImGui::SameLine(0.0f, 0.0f);
      ImGui::TextWrapped(": %s", line.text.c_str());
    }
    ImGui::PopStyleColor();
  };
  if (ImGui::Begin("##mp_chat", nullptr, flags)) {
    if (!chat_open_) {
      for (const auto& [line, alpha] : shown) draw_line(*line, alpha);
    } else {
      const float rows = ImGui::GetTextLineHeightWithSpacing() * kOpenRows;
      if (ImGui::BeginChild("##lines", ImVec2(0.0f, rows), ImGuiChildFlags_None)) {
        for (size_t i = lines.size() > kScrollLines ? lines.size() - kScrollLines : 0; i < lines.size(); ++i) draw_line(*lines[i], 1.0f);
        if (client.chat_serial() != scrolled_serial_ || focus_input_) {
          scrolled_serial_ = client.chat_serial();
          ImGui::SetScrollHereY(1.0f);
        }
      }
      ImGui::EndChild();
      const float box = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Global").x;
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - box - ImGui::GetStyle().ItemSpacing.x);
      if (focus_input_) {
        ImGui::GetIO().InputQueueCharacters.resize(0);
        ImGui::SetKeyboardFocusHere();
        focus_input_ = false;
      }
      const bool connected = client.state() == MultiplayerClient::State::Connected;
      const char* hint = !connected ? "Not connected" : global_ ? "Say to everyone" : "Say to this room";
      const ImGuiInputTextFlags say_flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion;
      const auto on_tab = [](ImGuiInputTextCallbackData* data) {
        bool& global = *static_cast<bool*>(data->UserData);
        global = !global;
        return 0;
      };
      if (ImGui::InputTextWithHint("##say", hint, input_, sizeof input_, say_flags, on_tab, &global_)) {
        if (client.send_chat(global_ ? net::ChatScope::Global : net::ChatScope::Room, input_) || input_[0] == '\0') {
          input_[0] = '\0';
          chat_open_ = false;
        } else {
          focus_input_ = true;
        }
      }
      ImGui::SameLine();
      if (ImGui::Checkbox("Global", &global_)) focus_input_ = true;
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGuiLayer::tooltip("On: everyone online reads the line. Off: only the players in this room. Tab switches it while typing.");
    }
  }
  ImGui::End();
  ImGui::PopFont();
}

void MultiplayerUi::draw_labels(const std::vector<net::RemoteLabel>& labels, const MultiplayerClient& client, ImFont* font) const {
  if (labels.empty()) return;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  ImGui::PushFont(font);
  const float size = ImGui::GetFontSize();
  ImFont* current = ImGui::GetFont();
  const float pad = size * 0.3f;
  for (const net::RemoteLabel& label : labels) {
    const net::RemoteParty* party = party_of(client, label.pid);
    const std::string text = party ? party->name + "  " + star_text(current, party->star) : label.text;
    const ImVec2 text_size = current->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    const bool icon = label.state == net::PresenceState::Battle || label.state == net::PresenceState::Menu || label.state == net::PresenceState::Typing;
    const float icon_w = icon ? size * 1.1f + pad : 0.0f;
    const float total = text_size.x + icon_w;
    const ImVec2 top_left(label.x - total * 0.5f, label.y);
    dl->AddRectFilled(ImVec2(top_left.x - pad, top_left.y - pad * 0.5f), ImVec2(top_left.x + total + pad, top_left.y + text_size.y + pad * 0.5f),
                      IM_COL32(10, 12, 20, 120), pad);
    dl->AddText(current, size, ImVec2(top_left.x + 1.0f, top_left.y + 1.0f), IM_COL32(0, 0, 0, 200), text.c_str());
    dl->AddText(current, size, top_left, IM_COL32(240, 242, 250, 255), text.c_str());
    if (icon) draw_state_icon(dl, ImVec2(top_left.x + text_size.x + pad, top_left.y), text_size.y, label.state, IM_COL32(240, 242, 250, 230));
  }
  ImGui::PopFont();
}

void MultiplayerUi::draw_bubbles(const std::vector<net::RemoteLabel>& labels, const std::optional<net::RemoteLabel>& own,
                                 const MultiplayerClient& client, double now, bool names_shown, ImFont* font) const {
  struct Bubble {
    const net::ChatLine* line = nullptr;
    float alpha = 0.0f;
  };
  std::map<std::string, Bubble> newest;
  for (const net::ChatLine& line : client.chat(net::ChatScope::Room)) {
    if (line.pid.empty() || !line.fresh || client.muted(line.pid)) continue;
    const double life = std::min(kBubbleBase + kBubblePerByte * static_cast<double>(line.text.size()), kBubbleMax);
    const double age = now - line.received;
    if (age < 0.0 || age >= life) continue;
    newest[line.pid] = {&line, static_cast<float>(std::clamp((life - age) / kBubbleFade, 0.0, 1.0))};
  }
  if (newest.empty()) return;

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  ImGui::PushFont(font);
  ImFont* current = ImGui::GetFont();
  const float size = ImGui::GetFontSize();
  const float pad = size * 0.4f;
  const float tail = size * 0.4f;
  const auto draw_one = [&](const Bubble& bubble, float x, float bottom) {
    std::string text = bubble.line->text;
    if (text.size() > kBubbleBytes) {
      size_t cut = kBubbleBytes;
      while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
      text = text.substr(0, cut) + "…";
    }
    const ImVec2 text_size = current->CalcTextSizeA(size, FLT_MAX, size * kBubbleWidth, text.c_str());
    const ImVec2 box_min(x - text_size.x * 0.5f - pad, bottom - tail - text_size.y - pad * 2.0f);
    const ImVec2 box_max(x + text_size.x * 0.5f + pad, bottom - tail);
    const auto fade = [&](int alpha) { return static_cast<int>(std::lround(static_cast<float>(alpha) * bubble.alpha)); };
    const ImU32 fill = IM_COL32(248, 248, 252, fade(225));
    const ImU32 edge = IM_COL32(20, 22, 30, fade(160));
    dl->AddRectFilled(box_min, box_max, fill, pad);
    dl->AddRect(box_min, box_max, edge, pad);
    dl->AddTriangleFilled(ImVec2(x - tail * 0.7f, box_max.y - 1.0f), ImVec2(x + tail * 0.7f, box_max.y - 1.0f), ImVec2(x, bottom), fill);
    dl->AddText(current, size, ImVec2(box_min.x + pad, box_min.y + pad), IM_COL32(20, 22, 30, fade(255)), text.c_str(), nullptr,
                size * kBubbleWidth);
  };
  for (const net::RemoteLabel& label : labels)
    if (const auto found = newest.find(label.pid); found != newest.end())
      draw_one(found->second, label.x, names_shown ? label.y - size * 0.2f : label.y + size);
  if (own && !client.pid().empty())
    if (const auto found = newest.find(client.pid()); found != newest.end()) draw_one(found->second, own->x, own->y + size);
  ImGui::PopFont();
}

}  // namespace oc::render
