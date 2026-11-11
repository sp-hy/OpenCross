#include "render/imgui_multiplayer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <format>
#include <map>
#include <utility>

#include <imgui.h>

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
const ImVec4 kPmColour(1.0f, 0.62f, 0.92f, 1.0f);

// The Star Level after a star sign (U+2605), or after '*' when the font lacks the star.
std::string star_text(ImFont* font, int star) {
  const bool has_star = font && font->FindGlyphNoFallback(0x2605) != nullptr;
  return std::format("{}{}", has_star ? "★" : "*", star);
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
    case MultiplayerClient::State::VersionTooOld:
    case MultiplayerClient::State::BadAddress: return ImVec4(0.95f, 0.55f, 0.45f, 1.0f);
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

// A name drawn as a link: the hand pointer over it; true when it was clicked.
bool name_link(const std::string& name, const ImVec4& colour, bool bold) {
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::TextUnformatted(name.c_str());
  ImGui::PopStyleColor();
  if (bold) ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + 1.0f, at.y), ImGui::ColorConvertFloat4ToU32(colour), name.c_str());
  if (!ImGui::IsItemHovered()) return false;
  ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, max.y), max, ImGui::ColorConvertFloat4ToU32(colour));
  return ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

constexpr const char* kCardHeadings[] = {"", "HP", "ATK", "DEF", "MAG", "MDEF", "Elem."};
// The name column of a card is at most this many font heights wide (longer names are clipped).
constexpr float kCardNameMax = 8.0f;

// A card's name column at the current font: the longest name, at least a short one's and at most kCardNameMax font heights.
float card_name_width(const net::PartyCard& card) {
  float name = ImGui::CalcTextSize("Korcha").x;
  for (const net::CardMember& member : card.members) name = std::max(name, ImGui::CalcTextSize(member.name.c_str()).x);
  return std::min(name, ImGui::GetFontSize() * kCardNameMax);
}

// A card's stat column: the widest heading or four-digit number.
float card_stat_width() {
  float stat = ImGui::CalcTextSize("9999").x;
  for (const char* heading : kCardHeadings) stat = std::max(stat, ImGui::CalcTextSize(heading).x);
  return stat;
}

// The width of a card's table at the current font, the cell padding included.
float card_width(const net::PartyCard& card) {
  return card_name_width(card) + 6.0f * card_stat_width() + 7.0f * (2.0f * ImGui::GetStyle().CellPadding.x + 1.0f);
}

// The height of a card's table: the heading row and one row per member (the "(no party)" line when it has none).
float card_height(const net::PartyCard& card) {
  if (card.members.empty()) return ImGui::GetTextLineHeightWithSpacing();
  const float row = ImGui::GetTextLineHeight() + 2.0f * ImGui::GetStyle().CellPadding.y;
  return row * static_cast<float>(card.members.size() + 1);
}

// A party card as a table `width` pixels wide (0: as wide as its content): one row per member with the name, max HP, ATK / DEF / MAG /
// MDEF and the element count, the numbers right-aligned.
void draw_card(const char* id, const net::PartyCard& card, float width) {
  if (card.members.empty()) {
    ImGui::TextDisabled("(no party)");
    return;
  }
  const ImGuiTableFlags flags = (width > 0.0f ? ImGuiTableFlags_SizingStretchProp : ImGuiTableFlags_SizingFixedFit) |
                                ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_RowBg;
  if (!ImGui::BeginTable(id, 7, flags, ImVec2(width, 0.0f))) return;
  const float name = card_name_width(card), stat = card_stat_width();
  for (int i = 0; i < 7; ++i) ImGui::TableSetupColumn(kCardHeadings[i], 0, i == 0 ? name : stat);
  ImGui::TableHeadersRow();
  for (const net::CardMember& member : card.members) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(member.name.empty() ? "?" : member.name.c_str());
    for (const int value : {member.hp, member.atk, member.def, member.mag, member.mdef, member.elements}) {
      ImGui::TableNextColumn();
      char text[16];
      std::snprintf(text, sizeof text, "%d", value);
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text).x));
      ImGui::TextUnformatted(text);
    }
  }
  ImGui::EndTable();
}

// The two sides' colours: the local player's blue, the other player's red.
const ImVec4 kOwnColour(0.32f, 0.62f, 0.98f, 1.0f);
const ImVec4 kTheirColour(0.96f, 0.36f, 0.30f, 1.0f);
const ImVec4 kGold(1.0f, 0.84f, 0.38f, 1.0f);

ImU32 with_alpha(const ImVec4& colour, float alpha) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(colour.x, colour.y, colour.z, colour.w * std::clamp(alpha, 0.0f, 1.0f)));
}

// `colour` moved `amount` (0..1) towards white.
ImVec4 lighter(const ImVec4& colour, float amount) {
  return ImVec4(colour.x + (1.0f - colour.x) * amount, colour.y + (1.0f - colour.y) * amount, colour.z + (1.0f - colour.z) * amount, colour.w);
}

// `text` in `font` at `size` pixels, its top-left at `at`, with a dark outline; `bold` draws it twice, one pixel apart.
void outlined_text(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text, bool bold = false) {
  const float edge = std::max(1.0f, size * 0.05f);
  const ImU32 shade = IM_COL32(0, 0, 0, ((colour >> IM_COL32_A_SHIFT) & 0xFF) * 3 / 4);
  for (const ImVec2 offset : {ImVec2(-edge, 0.0f), ImVec2(edge, 0.0f), ImVec2(0.0f, -edge), ImVec2(0.0f, edge), ImVec2(-edge, -edge),
                              ImVec2(edge, edge), ImVec2(-edge, edge), ImVec2(edge, -edge)})
    dl->AddText(font, size, ImVec2(at.x + offset.x, at.y + offset.y), shade, text);
  dl->AddText(font, size, at, colour, text);
  if (bold) dl->AddText(font, size, ImVec2(at.x + edge, at.y), colour, text);
}

// `text` `size` pixels high at the cursor as an item of its size, outlined.
void big_text(const char* text, float size, ImU32 colour) {
  ImFont* font = ImGui::GetFont();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  outlined_text(ImGui::GetWindowDrawList(), font, size, at, colour, text, true);
  ImGui::Dummy(font->CalcTextSizeA(size, FLT_MAX, 0.0f, text));
}

// The first character (UTF-8 code point) of `name`; "?" for an empty one.
std::string first_letter(const std::string& name) {
  if (name.empty()) return "?";
  size_t length = 1;
  while (length < name.size() && (static_cast<unsigned char>(name[length]) & 0xC0) == 0x80) ++length;
  return name.substr(0, length);
}

ImTextureID texture_id(const MultiplayerUi::Face& face) {
  return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(&face.binding));
}

// The middle square of the face image, in texture coordinates.
void square_uv(const MultiplayerUi::Face& face, ImVec2& uv0, ImVec2& uv1) {
  const float w = static_cast<float>(face.texture.width()), h = static_cast<float>(face.texture.height());
  const float cut_x = w > h ? (1.0f - h / w) * 0.5f : 0.0f;
  const float cut_y = h > w ? (1.0f - w / h) * 0.5f : 0.0f;
  uv0 = ImVec2(cut_x, cut_y);
  uv1 = ImVec2(1.0f - cut_x, 1.0f - cut_y);
}

// A face as a large faded backdrop over `min`..`max` (a square): its opacity runs from `alpha_left` at the left edge to `alpha_right` at
// the right, so it fades out towards the middle of the screen.
void backdrop_face(ImDrawList* dl, const MultiplayerUi::Face& face, ImVec2 min, ImVec2 max, float alpha_left, float alpha_right) {
  ImVec2 uv0, uv1;
  square_uv(face, uv0, uv1);
  const ImU32 left = with_alpha(ImVec4(1, 1, 1, 1), alpha_left), right = with_alpha(ImVec4(1, 1, 1, 1), alpha_right);
  dl->PushTextureID(texture_id(face));
  dl->PrimReserve(6, 4);
  const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
  for (const int index : {0, 1, 2, 0, 2, 3}) dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + index));
  dl->PrimWriteVtx(min, uv0, left);
  dl->PrimWriteVtx(ImVec2(max.x, min.y), ImVec2(uv1.x, uv0.y), right);
  dl->PrimWriteVtx(max, uv1, right);
  dl->PrimWriteVtx(ImVec2(min.x, max.y), ImVec2(uv0.x, uv1.y), left);
  dl->PopTextureID();
}

// Seconds left until `deadline`, rounded up.
int seconds_left(double deadline, double now) { return static_cast<int>(std::ceil(std::max(0.0, deadline - now))); }

constexpr ImGuiWindowFlags kPopupFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

}  // namespace

bool MultiplayerUi::typing() { return ImGui::GetIO().WantTextInput; }

void MultiplayerUi::open_chat() {
  chat_open_ = true;
  focus_input_ = true;
}

std::optional<std::string_view> MultiplayerUi::reply_text(std::string_view line) {
  if (line.size() < 2 || line[0] != '/' || (line[1] != 'r' && line[1] != 'R') || (line.size() > 2 && line[2] != ' ')) return std::nullopt;
  line.remove_prefix(2);
  while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
  return line;
}

const net::ChatLine* MultiplayerUi::last_pm_from(const MultiplayerClient& client) {
  const std::deque<net::ChatLine>& lines = client.chat(net::ChatScope::Pm);
  for (auto line = lines.rbegin(); line != lines.rend(); ++line)
    if (!line->pid.empty() && line->pid != client.pid()) return &*line;
  return nullptr;
}

void MultiplayerUi::open_pm(const std::string& pid, const std::string& name) {
  pm_to_ = pid;
  pm_name_ = name;
  open_chat();
}

bool MultiplayerUi::take_close_options() { return std::exchange(close_options_, false); }

bool MultiplayerUi::edit_text(const char* id, const char* hint, std::string& value, TextField& field) {
  if (!field.active) std::snprintf(field.buffer, sizeof field.buffer, "%s", value.c_str());
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::InputTextWithHint(id, hint, field.buffer, sizeof field.buffer);
  field.active = ImGui::IsItemActive();
  if (!ImGui::IsItemDeactivatedAfterEdit() || value == field.buffer) return false;
  value = field.buffer;
  return true;
}

std::string MultiplayerUi::challenge_block(const MultiplayerClient& client, const platform::MultiplayerSettings& settings, const std::string& pid,
                                           bool free) const {
  if (!settings.challenges) return "You don't take challenges (Receive battle challenges is off)";
  if (client.state() != MultiplayerClient::State::Connected) return "Not connected";
  if (!client.can_challenge()) return "A challenge or a battle is already under way";
  if (!free) return "You are busy";
  if (!card_source_ || !card_source_()) return "No game is loaded";
  if (const net::RemoteParty* party = party_of(client, pid)) {
    if (!party->pvp) return "This player doesn't take challenges";
    if (party->has_pose && party->pose.state != net::PresenceState::Field) return "This player is busy";
  }
  return {};
}

bool MultiplayerUi::send_challenge(MultiplayerClient& client, const std::string& pid, const std::string& name) {
  const std::optional<net::PartyCard> card = card_source_ ? card_source_() : std::nullopt;
  return card && client.challenge(pid, name, *card);
}

bool MultiplayerUi::draw_tab(platform::MultiplayerSettings& settings, MultiplayerClient* client, const std::string& default_name,
                             bool allowed, bool free, ImFont* font) {
  bool changed = false;
  ImGuiLayer::section("Multiplayer");
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextDisabled("See other players' parties walk in your room and chat with them. Nothing of your game is shared but your name, "
                      "Star Level, room and the party leader's position, and your party's card when you challenge or accept a battle.");
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
    ImGuiLayer::row("Name labels", "Show each player's name and Star Level above their party. Click a name for Message, Challenge and Mute.");
    changed |= ImGui::Checkbox("##labels", &settings.show_labels);
    ImGuiLayer::row("Chat bubbles", "A line said to the room also shows in a bubble over the speaker's party, yours included.");
    changed |= ImGui::Checkbox("##bubbles", &settings.chat_bubbles);
    ImGuiLayer::row("Mute hides party", "A muted player's party is hidden as well as their chat.");
    changed |= ImGui::Checkbox("##mutehides", &settings.mute_hides_party);
    ImGuiLayer::row("Receive battle challenges", "Other players may challenge you to a battle. Off: nobody can, and you cannot challenge anyone.");
    changed |= ImGui::Checkbox("##challenges", &settings.challenges);
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
    ImGuiLayer::row("Hide global chat", "Show only room chat and private messages; you can no longer send to everyone.");
    changed |= ImGui::Checkbox("##hideglobal", &settings.hide_global_chat);
    ImGuiLayer::row("Keys");
    ImGui::TextDisabled("T opens the chat, Tab switches Room / Global, Enter sends, Esc closes; Y / N answer a challenge");
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
      if (ImGui::SmallButton("Message")) {
        open_pm(party.pid, party.name);
        close_options_ = true;
      }
      ImGui::SameLine();
      const std::string block = challenge_block(*client, settings, party.pid, free);
      ImGui::BeginDisabled(!block.empty());
      if (ImGui::SmallButton("Challenge") && send_challenge(*client, party.pid, party.name)) close_options_ = true;
      ImGui::EndDisabled();
      if (!block.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) ImGuiLayer::tooltip(block.c_str());
      ImGui::SameLine();
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
  hide_global_ = settings.hide_global_chat;
  if (hide_global_) global_ = false;
  if (!chat_open_ && settings.chat == platform::ChatDisplay::Hidden) return;
  std::vector<const net::ChatLine*> lines;
  for (const net::ChatScope scope : {net::ChatScope::Room, net::ChatScope::Global, net::ChatScope::Pm})
    if (scope != net::ChatScope::Global || !hide_global_)
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
        if (age >= kLineShow) continue;
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
    if (line.scope == net::ChatScope::Pm) return ImVec4(kPmColour.x, kPmColour.y, kPmColour.z, alpha);
    return line.scope == net::ChatScope::Global ? ImVec4(0.70f, 0.85f, 1.0f, alpha) : ImVec4(0.96f, 0.96f, 0.98f, alpha);
  };
  // "[R] name: text": the name is a link (the player menu; on a private line, a reply) while the panel is open.
  const auto draw_line = [&](const net::ChatLine& line, float alpha) {
    const ImVec4 colour = line_colour(line, alpha);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    if (line.pid.empty()) {
      ImGui::TextWrapped("%s", line.text.c_str());
      ImGui::PopStyleColor();
      return;
    }
    const bool pm = line.scope == net::ChatScope::Pm;
    const bool own = line.pid == client.pid();
    ImGui::TextUnformatted(pm ? (own ? "[PM] to " : "[PM] ") : line.scope == net::ChatScope::Global ? "[G] " : "[R] ");
    ImGui::SameLine(0.0f, 0.0f);
    const std::string& pid = pm && own ? line.to : line.pid;
    const std::string& name = pm && own ? line.to_name : line.name;
    const bool admin = line.admin && !(pm && own);
    const ImVec4 name_colour = admin ? ImVec4(1.0f, 0.32f, 0.30f, alpha) : colour;
    if (name_link(name, name_colour, admin) && chat_open_ && pid != client.pid()) {
      if (pm) open_pm(pid, name);
      else if (const net::RemoteParty* party = party_of(client, pid)) open_player_menu(pid, name, party->star);
      else open_player_menu(pid, name, -1);
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextWrapped(": %s", line.text.c_str());
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
      const bool pm = !pm_to_.empty();
      if (pm) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kPmColour, "To %s", pm_name_.c_str());
        ImGui::SameLine();
      }
      const float box = pm || hide_global_ ? 0.0f : ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Global").x + ImGui::GetStyle().ItemSpacing.x;
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - box);
      if (focus_input_) {
        ImGui::GetIO().InputQueueCharacters.resize(0);
        ImGui::SetKeyboardFocusHere();
        focus_input_ = false;
      }
      const bool connected = client.state() == MultiplayerClient::State::Connected;
      const char* hint = !connected ? "Not connected" : pm ? "Say to this player only" : global_ ? "Say to everyone" : "Say to this room";
      const ImGuiInputTextFlags say_flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion;
      // Tab: Room <-> Global (not with global chat hidden); from a private target, back to Room / Global.
      const auto on_tab = [](ImGuiInputTextCallbackData* data) {
        MultiplayerUi& ui = *static_cast<MultiplayerUi*>(data->UserData);
        if (!ui.pm_to_.empty()) ui.pm_to_.clear();
        else if (!ui.hide_global_) ui.global_ = !ui.global_;
        return 0;
      };
      if (ImGui::InputTextWithHint("##say", hint, input_, sizeof input_, say_flags, on_tab, this)) {
        // "/r text": a private line to the player who sent the last private line here; "/r" alone makes them the target. With nobody to
        // reply to the line stays as typed.
        bool sent = false, keep_open = false;
        if (const std::optional<std::string_view> reply = reply_text(input_)) {
          const net::ChatLine* last = last_pm_from(client);
          keep_open = !last || reply->empty();
          if (last && reply->empty()) {
            open_pm(last->pid, last->name);
            input_[0] = '\0';
          } else if (last) {
            sent = client.send_pm(last->pid, *reply);
          }
        } else {
          sent = pm ? client.send_pm(pm_to_, input_) : client.send_chat(global_ ? net::ChatScope::Global : net::ChatScope::Room, input_);
        }
        if (!keep_open && (sent || input_[0] == '\0')) {
          input_[0] = '\0';
          chat_open_ = false;
        } else {
          focus_input_ = true;
        }
      }
      if (!pm && !hide_global_) {
        ImGui::SameLine();
        if (ImGui::Checkbox("Global", &global_)) focus_input_ = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGuiLayer::tooltip("On: everyone online reads the line. Off: only the players in this room. Tab switches it while typing.");
      }
    }
  }
  ImGui::End();
  ImGui::PopFont();
}

void MultiplayerUi::draw_labels(const std::vector<net::RemoteLabel>& labels, const MultiplayerClient& client, ImFont* font) {
  if (labels.empty()) return;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  ImGui::PushFont(font);
  const float size = ImGui::GetFontSize();
  ImFont* current = ImGui::GetFont();
  const float pad = size * 0.3f;
  const ImGuiIO& io = ImGui::GetIO();
  const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse;
  for (const net::RemoteLabel& label : labels) {
    const net::RemoteParty* party = party_of(client, label.pid);
    const std::string text = party ? party->name + "  " + star_text(current, party->star) : label.text;
    const ImVec2 text_size = current->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    const bool icon = label.state == net::PresenceState::Battle || label.state == net::PresenceState::Menu || label.state == net::PresenceState::Typing;
    const float icon_w = icon ? size * 1.1f + pad : 0.0f;
    const float total = text_size.x + icon_w;
    const ImVec2 top_left(label.x - total * 0.5f, label.y);
    const ImVec2 box_min(top_left.x - pad, top_left.y - pad * 0.5f), box_max(top_left.x + total + pad, top_left.y + text_size.y + pad * 0.5f);
    const bool hovered = !io.WantCaptureMouse && ImGui::IsMouseHoveringRect(box_min, box_max, false);
    dl->AddRectFilled(box_min, box_max, hovered ? IM_COL32(40, 46, 70, 170) : IM_COL32(10, 12, 20, 120), pad);
    dl->AddText(current, size, ImVec2(top_left.x + 1.0f, top_left.y + 1.0f), IM_COL32(0, 0, 0, 200), text.c_str());
    dl->AddText(current, size, top_left, IM_COL32(240, 242, 250, 255), text.c_str());
    if (icon) draw_state_icon(dl, ImVec2(top_left.x + text_size.x + pad, top_left.y), text_size.y, label.state, IM_COL32(240, 242, 250, 230));
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (hovered && clicked && party) open_player_menu(party->pid, party->name, party->star);
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
    if (line.pid.empty() || client.muted(line.pid)) continue;
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

void MultiplayerUi::open_player_menu(const std::string& pid, const std::string& name, int star) {
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  menu_ = PlayerMenu{pid, name, star, mouse.x, mouse.y, true};
}

bool MultiplayerUi::draw_player_menu(MultiplayerClient& client, platform::MultiplayerSettings& settings, bool free, ImFont* font) {
  if (!menu_) return false;
  if (client.state() != MultiplayerClient::State::Connected) {
    menu_.reset();
    return false;
  }
  const PlayerMenu menu = *menu_;
  bool changed = false, close = false;
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 pivot(menu.x > io.DisplaySize.x * 0.6f ? 1.0f : 0.0f, menu.y > io.DisplaySize.y * 0.6f ? 1.0f : 0.0f);
  ImGui::SetNextWindowPos(ImVec2(menu.x, menu.y), ImGuiCond_Always, pivot);
  if (ImGui::Begin("##mp_player", nullptr, kPopupFlags & ~ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::PushFont(font);
    const net::RemoteParty* party = party_of(client, menu.pid);
    const int star = party ? party->star : menu.star;
    if (star >= 0) ImGui::Text("%s  %s", menu.name.c_str(), star_text(font, star).c_str());
    else ImGui::TextUnformatted(menu.name.c_str());
    ImGui::PopFont();
    ImGui::Separator();
    if (ImGui::Selectable("Message")) {
      open_pm(menu.pid, menu.name);
      close = true;
    }
    const std::string block = challenge_block(client, settings, menu.pid, free);
    ImGui::BeginDisabled(!block.empty());
    if (ImGui::Selectable("Challenge to battle")) {
      send_challenge(client, menu.pid, menu.name);
      close = true;
    }
    ImGui::EndDisabled();
    if (!block.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) ImGuiLayer::tooltip(block.c_str());
    const auto muted = std::find(settings.muted.begin(), settings.muted.end(), menu.pid);
    const bool is_muted = muted != settings.muted.end();
    if (ImGui::Selectable(is_muted ? "Unmute" : "Mute")) {
      if (is_muted) settings.muted.erase(muted);
      else settings.muted.push_back(menu.pid);
      changed = close = true;
    }
    // A click outside the menu closes it; a click on one of its entries makes that entry active this frame, which must still count as
    // inside.
    const ImGuiHoveredFlags inside = ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem;
    if (!menu.fresh && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(inside)) close = true;
  }
  ImGui::End();
  if (close) {
    menu_.reset();
    if (chat_open_) focus_input_ = true;
  } else {
    menu_->fresh = false;
  }
  return changed;
}

void MultiplayerUi::accept(MultiplayerClient& client) {
  const std::optional<net::PartyCard> card = card_source_ ? card_source_() : std::nullopt;
  if (card) client.accept_challenge(*card);
  else client.decline_challenge();
}

const MultiplayerUi::Face* MultiplayerUi::face(int char_id) {
  if (const auto found = faces_.find(char_id); found != faces_.end()) return found->second.get();
  std::unique_ptr<Face>& slot = faces_[char_id];
  DecodedImage image;
  if (!gpu_ || !face_source_ || !face_source_(char_id, image) || image.width == 0 || image.height == 0) return nullptr;
  if (!face_sampler_.handle() && !face_sampler_.create(*gpu_, true)) return nullptr;
  auto made = std::make_unique<Face>();
  if (!made->texture.create(*gpu_, image.width, image.height, TextureFormat::Rgba8, image.rgba.data(), nullptr, true)) return nullptr;
  made->binding.texture = made->texture.handle();
  made->binding.sampler = face_sampler_.handle();
  slot = std::move(made);
  return slot.get();
}

void MultiplayerUi::draw_portrait(ImDrawList* dl, const net::CardMember& member, float x, float y, float size, bool own) {
  const ImVec4& colour = own ? kOwnColour : kTheirColour;
  const ImVec2 min(x, y), max(x + size, y + size);
  const float rounding = size * 0.08f;
  dl->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(ImVec4(colour.x * 0.22f, colour.y * 0.22f, colour.z * 0.22f, 0.92f)), rounding);
  if (const Face* portrait = face(member.char_id)) {
    ImVec2 uv0, uv1;
    square_uv(*portrait, uv0, uv1);
    dl->AddImageRounded(texture_id(*portrait), min, max, uv0, uv1, IM_COL32_WHITE, rounding);
  } else {
    ImFont* font = ImGui::GetFont();
    const std::string letter = first_letter(member.name);
    const float letter_size = size * 0.55f;
    const ImVec2 extent = font->CalcTextSizeA(letter_size, FLT_MAX, 0.0f, letter.c_str());
    outlined_text(dl, font, letter_size, ImVec2(x + (size - extent.x) * 0.5f, y + (size - extent.y) * 0.5f), with_alpha(lighter(colour, 0.5f), 1.0f),
                  letter.c_str(), true);
  }
  dl->AddRect(min, max, with_alpha(lighter(colour, 0.25f), 1.0f), rounding, 0, std::max(1.0f, size * 0.035f));
}

int MultiplayerUi::draw_incoming(const net::IncomingChallenge& incoming, double now, ImFont* font) {
  if (incoming.deadline != incoming_deadline_) {
    incoming_deadline_ = incoming.deadline;
    incoming_span_ = std::max(1.0, incoming.deadline - now);
  }
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, ImGui::GetFontSize() * 1.2f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.94f);
  int answer = 0;
  if (ImGui::Begin("##mp_incoming", nullptr, kPopupFlags)) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const std::vector<net::CardMember>& members = incoming.card.members;
    // Shrunk to fit small windows: the natural size in font heights, measured at the full font (the window keeps last frame's scale).
    ImGui::SetWindowFontScale(1.0f);
    const float base = ImGui::GetFontSize();
    const float natural_w = std::max(card_width(incoming.card), base * 22.0f) + 2.0f * style.WindowPadding.x;
    const float natural_h = base * 14.0f + card_height(incoming.card) + 2.0f * style.WindowPadding.y;
    const float scale = std::clamp(std::min((io.DisplaySize.x - base) / natural_w, (io.DisplaySize.y - base * 1.5f) / natural_h), 0.5f, 1.0f);
    ImGui::SetWindowFontScale(scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * scale, style.ItemSpacing.y * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(style.CellPadding.x * scale, style.CellPadding.y * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x * scale, style.FramePadding.y * scale));
    const float em = ImGui::GetFontSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The challenger's side: a red wash from the left and the leader's face fading in at the right, under a red rim.
    const ImVec2 win = ImGui::GetWindowPos(), win_size = ImGui::GetWindowSize();
    const ImVec2 win_max(win.x + win_size.x, win.y + win_size.y);
    dl->PushClipRect(win, win_max, false);
    dl->AddRectFilledMultiColor(win, win_max, with_alpha(kTheirColour, 0.26f), with_alpha(kTheirColour, 0.0f), with_alpha(kTheirColour, 0.0f),
                                with_alpha(kTheirColour, 0.26f));
    if (!members.empty())
      if (const Face* leader = face(members.front().char_id))
        backdrop_face(dl, *leader, ImVec2(win_max.x - win_size.y, win.y), win_max, 0.0f, 0.15f);
    dl->AddRectFilled(win, ImVec2(win_max.x, win.y + std::max(2.0f, em * 0.16f)), with_alpha(kTheirColour, 0.95f), style.WindowRounding,
                      ImDrawFlags_RoundCornersTop);
    dl->PopClipRect();

    // The portraits, then the name with the Star Level and the line under it.
    const float portrait = em * 3.6f, gap = em * 0.35f;
    const float row_w = members.empty() ? 0.0f : static_cast<float>(members.size()) * (portrait + gap) - gap;
    const std::string title = std::format("{}  {}", incoming.name, star_text(font, incoming.star));
    const float title_size = em * 1.5f;
    const ImVec2 title_extent = ImGui::GetFont()->CalcTextSizeA(title_size, FLT_MAX, 0.0f, title.c_str());
    const char* subtitle = "wants to battle you";
    if (!members.empty()) {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      for (size_t i = 0; i < members.size(); ++i)
        draw_portrait(dl, members[i], at.x + static_cast<float>(i) * (portrait + gap), at.y, portrait, false);
      ImGui::Dummy(ImVec2(row_w, portrait));
      ImGui::SameLine(0.0f, em * 0.8f);
      const float text_h = title_extent.y + ImGui::GetStyle().ItemSpacing.y +ImGui::GetTextLineHeight();
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (portrait - text_h) * 0.5f));
    }
    ImGui::BeginGroup();
    big_text(title.c_str(), title_size, with_alpha(lighter(kTheirColour, 0.55f), 1.0f));
    ImGui::TextUnformatted(subtitle);
    ImGui::EndGroup();
    const float header_w = (members.empty() ? 0.0f : row_w + em * 0.8f) + std::max(title_extent.x, ImGui::CalcTextSize(subtitle).x);
    const float content_w = std::max(card_width(incoming.card), header_w);

    ImGui::Dummy(ImVec2(0.0f, em * 0.2f));
    draw_card("##incoming_card", incoming.card, 0.0f);

    // The time left as a bar that empties, red for the last third, and the seconds after it.
    const float fraction = static_cast<float>(std::clamp((incoming.deadline - now) / incoming_span_, 0.0, 1.0));
    const std::string seconds = std::format("{} s", seconds_left(incoming.deadline, now));
    const float bar_w = std::max(em * 4.0f, content_w - ImGui::CalcTextSize(seconds.c_str()).x - ImGui::GetStyle().ItemSpacing.x);
    const ImVec2 bar_at = ImGui::GetCursorScreenPos();
    const float line = ImGui::GetTextLineHeight(), bar_h = line * 0.4f, bar_y = bar_at.y + (line - bar_h) * 0.5f;
    dl->AddRectFilled(ImVec2(bar_at.x, bar_y), ImVec2(bar_at.x + bar_w, bar_y + bar_h), IM_COL32(255, 255, 255, 30), bar_h * 0.5f);
    if (fraction > 0.0f)
      dl->AddRectFilled(ImVec2(bar_at.x, bar_y), ImVec2(bar_at.x + bar_w * fraction, bar_y + bar_h), with_alpha(fraction > 1.0f / 3.0f ? kGold : kTheirColour, 1.0f),
                        bar_h * 0.5f);
    ImGui::Dummy(ImVec2(bar_w, line));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", seconds.c_str());

    // Accept / Decline: the chosen one filled in its colour (green / red), the other plain.
    const float button_w = (content_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f, button_h = ImGui::GetFrameHeight() * 1.25f;
    const auto choice = [&](const char* label, bool chosen, const ImVec4& colour) {
      if (chosen) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(colour.x * 0.75f, colour.y * 0.75f, colour.z * 0.75f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colour);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, lighter(colour, 0.2f));
      }
      const bool pressed = ImGui::Button(label, ImVec2(button_w, button_h));
      if (chosen) ImGui::PopStyleColor(3);
      return pressed;
    };
    if (choice("Accept (Y)", accept_chosen_, ImVec4(0.30f, 0.68f, 0.42f, 1.0f))) answer = 1;
    ImGui::SameLine();
    if (choice("Decline (N)", !accept_chosen_, kTheirColour)) answer = 2;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_w);
    ImGui::TextDisabled("Pad: d-pad to choose, confirm to select, cancel to decline");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar(3);
  }
  ImGui::End();
  return answer;
}

void MultiplayerUi::draw_challenges(MultiplayerClient& client, double now, ImFont* font) {
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::PushFont(font);
  if (!client.incoming()) accept_chosen_ = false;
  if (const std::optional<net::IncomingChallenge>& incoming = client.incoming()) {
    const int answer = draw_incoming(*incoming, now, font);
    if (answer == 1) accept(client);
    else if (answer == 2) decline(client);
  } else {
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, ImGui::GetFontSize() * 2.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (const std::optional<net::OutgoingChallenge>& outgoing = client.outgoing()) {
      if (ImGui::Begin("##mp_outgoing", nullptr, kPopupFlags)) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Waiting for %s…  %d s", outgoing->name.c_str(), seconds_left(outgoing->expires, now));
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) client.cancel_challenge();
      }
      ImGui::End();
    } else if (client.accepted()) {
      if (ImGui::Begin("##mp_accepted", nullptr, kPopupFlags)) ImGui::TextUnformatted("Starting the battle…");
      ImGui::End();
    }
  }
  ImGui::PopFont();
}

void MultiplayerUi::start_versus(net::PvpStart start, double now) {
  versus_ = std::move(start);
  versus_end_ = now + kVersusSeconds;
}

void MultiplayerUi::draw_turn_banner(const std::string& text, bool own, ImFont* font) {
  if (text.empty()) return;
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::PushFont(font);
  ImFont* current = ImGui::GetFont();
  // Sized by the window's height (the battle HUD scales with it), never below the chat text; close to the corner, clear of the battle's own
  // top-right captions.
  const float size = std::max(ImGui::GetFontSize(), io.DisplaySize.y * 0.035f);
  const float pad = size * 0.4f, margin = size * 0.2f, edge = size * 0.18f;
  const ImVec2 text_size = current->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
  const ImVec2 box_max(io.DisplaySize.x - margin, margin + text_size.y + pad);
  const ImVec2 box_min(box_max.x - text_size.x - pad * 2.0f - edge, margin);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(box_min, box_max, IM_COL32(8, 10, 18, 200), pad * 0.6f);
  dl->AddRectFilled(box_min, ImVec2(box_min.x + edge, box_max.y), own ? IM_COL32(90, 150, 255, 235) : IM_COL32(230, 80, 70, 235), pad * 0.6f,
                    ImDrawFlags_RoundCornersLeft);
  dl->AddRect(box_min, box_max, IM_COL32(255, 255, 255, 60), pad * 0.6f);
  const ImVec2 at(box_min.x + edge + pad, box_min.y + pad * 0.5f);
  dl->AddText(current, size, ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 200), text.c_str());
  dl->AddText(current, size, at, own ? IM_COL32(245, 247, 255, 255) : IM_COL32(200, 204, 214, 255), text.c_str());
  ImGui::PopFont();
}

std::optional<net::PvpStart> MultiplayerUi::draw_versus(const MultiplayerClient& client, double now, ImFont* font) {
  if (!versus_) return std::nullopt;
  const bool waiting = now >= versus_end_;
  const bool over = client.pvp_battle() != versus_->battle || client.pvp_gone() != net::PvpGone::None;
  if (over || (waiting && (client.pvp_opponent_side() || now >= versus_end_ + kSideWait))) {
    std::optional<net::PvpStart> done = std::move(versus_);
    versus_.reset();
    return done;
  }
  draw_versus_screen(*versus_, versus_->first == client.pid(), waiting, now - (versus_end_ - kVersusSeconds), font);
  return std::nullopt;
}

// The whole window: dimmed, each half washed in its side's colour with its leader's face fading towards the middle, a slanted rule
// through the middle. Over it, per side (the player's left, the other's right, mirrored): the name with the Star Level, the members'
// portraits (leaders on the outside) and the card; a large "VS" between the portraits; who moves first under it all. The sides slide in
// from the edges as it opens; everything shrinks to fit a small window and grows a little in a large one.
void MultiplayerUi::draw_versus_screen(const net::PvpStart& start, bool own_first, bool waiting, double seconds, ImFont* font) {
  constexpr float kTitleSize = 1.4f, kVsSize = 2.8f, kSlideSeconds = 0.35f;
  // A large window grows the layout to fill up to kFill of its width, the font at most kMaxScale times its size.
  constexpr float kFill = 0.75f, kMaxScale = 1.35f;
  const ImGuiIO& io = ImGui::GetIO();
  const float width = io.DisplaySize.x, height = io.DisplaySize.y;
  const float opened = std::clamp(static_cast<float>(seconds) / kSlideSeconds, 0.0f, 1.0f);
  const float ease = 1.0f - (1.0f - opened) * (1.0f - opened) * (1.0f - opened);

  ImGui::PushFont(font);
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const ImGuiWindowFlags flags = (kPopupFlags & ~ImGuiWindowFlags_AlwaysAutoResize) | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground;
  if (ImGui::Begin("##mp_versus", nullptr, flags)) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImFont* current = ImGui::GetFont();
    const net::PartyCard* cards[2] = {&start.own, &start.card};

    // The natural layout at the full font, then one scale that fits it into the window (which keeps last frame's scale).
    ImGui::SetWindowFontScale(1.0f);
    const float base = ImGui::GetFontSize();
    const float natural_card = std::max(card_width(start.own), card_width(start.card));
    const float natural_gap = current->CalcTextSizeA(base * kVsSize, FLT_MAX, 0.0f, "VS").x + base * 2.0f;
    const float natural_table = std::max(card_height(start.own), card_height(start.card));
    const float natural_fixed = base * kTitleSize * 1.2f + natural_table + ImGui::GetTextLineHeightWithSpacing() * 2.0f + base * 2.4f;
    const float natural_w = 2.0f * natural_card + natural_gap;
    const float fit = std::min(width * 0.94f / natural_w, height * 0.94f / (natural_fixed + base * 3.0f));
    const float grow = std::clamp(width * kFill / natural_w, 1.0f, kMaxScale);
    const float scale = std::clamp(std::min(fit, grow), 0.4f, kMaxScale);
    ImGui::SetWindowFontScale(scale);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(style.CellPadding.x * scale, style.CellPadding.y * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * scale, style.ItemSpacing.y * scale));
    const float em = ImGui::GetFontSize();
    const float column = std::max(card_width(start.own), card_width(start.card));
    const float gap = current->CalcTextSizeA(em * kVsSize, FLT_MAX, 0.0f, "VS").x + em * 2.0f;
    const float table_h = std::max(card_height(start.own), card_height(start.card));
    const float title_h = em * kTitleSize * 1.2f, spacing = em * 0.8f, line = ImGui::GetTextLineHeightWithSpacing();
    const float portrait_gap = em * 0.4f;
    const float fixed_h = title_h + table_h + 2.0f * line + 3.0f * spacing;
    const float portrait = std::clamp(std::min((column - 2.0f * portrait_gap) / 3.0f, height * 0.94f - fixed_h), em * 2.0f, em * 7.0f);
    const float total_h = fixed_h + portrait;
    const float x0 = (width - (2.0f * column + gap)) * 0.5f, y0 = std::max(0.0f, (height - total_h) * 0.5f);
    const float slide = (1.0f - ease) * width * 0.3f;
    const float column_x[2] = {x0 - slide, x0 + column + gap + slide};
    const float portrait_y = y0 + title_h + spacing, table_y = portrait_y + portrait + spacing;

    // The backdrop: dimmed, the halves washed in the sides' colours, the leaders' faces, the slanted rule.
    ImDrawList* back = ImGui::GetBackgroundDrawList();
    back->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(4, 6, 12, static_cast<int>(200.0f * ease)));
    const float face_side = std::min(height, width * 0.5f);
    for (int side = 0; side < 2; ++side) {
      const bool own = side == 0;
      const net::PartyCard& card = *cards[side];
      const float edge = own ? 0.0f : width, middle = width * 0.5f;
      const ImU32 strong = with_alpha(own ? kOwnColour : kTheirColour, 0.32f * ease), clear = with_alpha(own ? kOwnColour : kTheirColour, 0.0f);
      back->AddRectFilledMultiColor(ImVec2(std::min(edge, middle), 0.0f), ImVec2(std::max(edge, middle), height), own ? strong : clear,
                                    own ? clear : strong, own ? clear : strong, own ? strong : clear);
      if (card.members.empty()) continue;
      if (const Face* leader = face(card.members.front().char_id)) {
        const float face_x = own ? -slide : width - face_side + slide;
        backdrop_face(back, *leader, ImVec2(face_x, height - face_side), ImVec2(face_x + face_side, height), own ? 0.22f * ease : 0.0f,
                      own ? 0.0f : 0.22f * ease);
      }
    }
    const float lean = height * 0.06f;
    back->AddLine(ImVec2(width * 0.5f + lean, 0.0f), ImVec2(width * 0.5f - lean, height), with_alpha(ImVec4(1, 1, 1, 1), 0.14f * ease),
                  std::max(1.0f, em * 0.12f));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int side = 0; side < 2; ++side) {
      const bool own = side == 0;
      const net::PartyCard& card = *cards[side];
      const float left = column_x[side], right = left + column;
      const ImVec4& colour = own ? kOwnColour : kTheirColour;
      // The name: the player's at the left edge of their column, the other's at the right edge of theirs.
      const std::string title = own ? std::format("You  {}", star_text(font, start.own.star)) : std::format("{}  {}", start.name, star_text(font, start.star));
      const float title_size = em * kTitleSize;
      const ImVec2 extent = current->CalcTextSizeA(title_size, FLT_MAX, 0.0f, title.c_str());
      dl->PushClipRect(ImVec2(left, y0), ImVec2(right, y0 + title_h), true);
      outlined_text(dl, current, title_size, ImVec2(own ? left : right - extent.x, y0), with_alpha(lighter(colour, 0.5f), 1.0f), title.c_str(), true);
      dl->PopClipRect();
      // The portraits, leaders on the outside.
      const size_t count = card.members.size();
      const float row_w = count == 0 ? 0.0f : static_cast<float>(count) * (portrait + portrait_gap) - portrait_gap;
      const float row_x = left + (column - row_w) * 0.5f;
      for (size_t i = 0; i < count; ++i) {
        const size_t place = own ? i : count - 1 - i;
        draw_portrait(dl, card.members[i], row_x + static_cast<float>(place) * (portrait + portrait_gap), portrait_y, portrait, own);
      }
      // The card on a dark panel.
      const float pad = em * 0.3f;
      dl->AddRectFilled(ImVec2(left - pad, table_y - pad), ImVec2(right + pad, table_y + table_h + pad), IM_COL32(8, 10, 18, 190), pad * 1.5f);
      dl->AddRect(ImVec2(left - pad, table_y - pad), ImVec2(right + pad, table_y + table_h + pad), with_alpha(colour, 0.55f), pad * 1.5f);
      ImGui::SetCursorScreenPos(ImVec2(left, table_y));
      draw_card(own ? "##own_card" : "##their_card", card, column);
    }

    // "VS" between the portraits, popping in as the sides arrive, on a soft glow.
    const float vs_size = em * kVsSize * (1.0f + 0.5f * (1.0f - ease));
    const ImVec2 vs_extent = current->CalcTextSizeA(vs_size, FLT_MAX, 0.0f, "VS");
    const ImVec2 vs_centre(width * 0.5f, portrait_y + portrait * 0.5f);
    dl->AddCircleFilled(vs_centre, vs_extent.y * 0.75f, with_alpha(kGold, 0.12f * ease), 32);
    outlined_text(dl, current, vs_size, ImVec2(vs_centre.x - vs_extent.x * 0.5f, vs_centre.y - vs_extent.y * 0.5f), with_alpha(kGold, ease), "VS", true);

    // Who moves first, and the wait for the other side.
    const std::string first = own_first ? std::string("You go first") : std::format("{} goes first", start.name);
    const float footer_y = table_y + table_h + spacing;
    const ImVec2 first_extent = ImGui::CalcTextSize(first.c_str());
    outlined_text(dl, current, em, ImVec2((width - first_extent.x) * 0.5f, footer_y), with_alpha(own_first ? lighter(kOwnColour, 0.4f) : lighter(kTheirColour, 0.4f), ease),
                  first.c_str());
    if (waiting) {
      const std::string wait = std::format("Waiting for {}...", start.name);
      const ImVec2 wait_extent = ImGui::CalcTextSize(wait.c_str());
      dl->AddText(current, em, ImVec2((width - wait_extent.x) * 0.5f, footer_y + line), ImGui::GetColorU32(ImGuiCol_TextDisabled), wait.c_str());
    }
    ImGui::PopStyleVar(2);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopFont();
}

}  // namespace oc::render
