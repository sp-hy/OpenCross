#include "render/imgui_guide.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <imgui.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "core/log.h"
#include "net/json.h"
#include "render/imgui_layer.h"

namespace oc::render {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kIdleStop = std::chrono::seconds(60);
// Full stick tilt (or a held arrow / d-pad) scrolls this many rows a second.
constexpr float kScrollRows = 36.0f;
constexpr float kIndentEm = 1.4f;
// A held paging button starts repeating after this long, then turns a page this often.
constexpr float kPageHoldDelay = 1.0f;
constexpr float kPageRepeat = 0.08f;
constexpr int kTabWidth = 8;
constexpr float kMaxWidthPerHeight = 1.3f;
constexpr const char* kHotkeyHint = "~ on the keyboard or LB + RB on a controller";
constexpr const char* kRuntimeMissing = "The Microsoft Edge WebView2 runtime is not installed, so guides cannot be loaded.";

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// Written to a temporary file and renamed over the old one.
void write_file(const std::filesystem::path& path, std::string_view text) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  const std::filesystem::path temp = path.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
      log::warn("guide: could not write {}", temp.string());
      return;
    }
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) log::warn("guide: could not write {}: {}", path.string(), ec.message());
}

// The pages a paging button turns this frame, held `held` seconds of which `dt` this frame: one on the press, then one when it has
// been held kPageHoldDelay and one every kPageRepeat after that.
int pages_turned(bool pressed, float held, float dt) {
  auto turned = [](float time) { return time < kPageHoldDelay ? 0 : static_cast<int>((time - kPageHoldDelay) / kPageRepeat) + 1; };
  return (pressed ? 1 : 0) + turned(held) - turned(held - dt);
}

std::string lower(std::string_view text) {
  std::string out(text);
  for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return out;
}

std::string expand_tabs(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  int column = 0;
  for (const char ch : text) {
    if (ch == '\t') {
      const int spaces = kTabWidth - column % kTabWidth;
      out.append(static_cast<size_t>(spaces), ' ');
      column += spaces;
      continue;
    }
    out += ch;
    column = ch == '\n' ? 0 : column + ((static_cast<unsigned char>(ch) & 0xC0) == 0x80 ? 0 : 1);
  }
  return out;
}

float text_width(ImFont* font, std::string_view text) {
  return font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x;
}

std::string_view trim_right(std::string_view text) {
  while (!text.empty() && (text.back() == ' ' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

}  // namespace

void GuideOverlay::init(std::filesystem::path folder) {
  folder_ = std::move(folder);
  runtime_missing_ = platform::WebPageReader::runtime_version().empty();
}

void GuideOverlay::shutdown() {
  close();
  if (browser_on_) reader_.stop();
  browser_on_ = false;
}

void GuideOverlay::start_browser() {
  if (browser_on_) return;
  reader_.start(folder_ / "browser");
  browser_on_ = true;
  idle_since_ = Clock::now();
}

void GuideOverlay::request_list() {
  if (list_id_) return;
  if (runtime_missing_) {
    list_error_ = kRuntimeMissing;
    return;
  }
  start_browser();
  list_error_.clear();
  list_id_ = reader_.load(std::string(net::kGuideListUrl), std::string(net::guide_list_script()));
}

void GuideOverlay::start_fetch(const std::string& url) {
  fetch_ = {};
  failed_url_.clear();
  if (runtime_missing_) {
    fetch_error_ = kRuntimeMissing;
    failed_url_ = url;
    return;
  }
  start_browser();
  fetch_error_.clear();
  fetch_.url = url;
  fetch_.id = reader_.load(url, std::string(net::guide_page_script()));
  log::info("guide: loading {}", url);
}

void GuideOverlay::take_answers() {
  if (!browser_on_) return;
  for (platform::WebPageReader::Answer& answer : reader_.take_answers()) {
    if (answer.id == list_id_) {
      list_id_ = 0;
      net::GuideList fresh;
      if (!answer.ok) list_error_ = answer.error;
      else if (!net::parse_guide_list(answer.json, fresh)) list_error_ = net::guide_answer_error(answer.json);
      else {
        list_ = std::move(fresh);
        list_error_.clear();
        write_file(folder_ / "list.json", net::guide_list_to_json(list_));
        log::info("guide: list of {} guides", list_.entries.size());
      }
      continue;
    }
    if (!fetch_.id || answer.id != fetch_.id) continue;
    net::GuidePage page;
    if (!answer.ok || !net::parse_guide_page(answer.json, page)) {
      fetch_error_ = answer.ok ? net::guide_answer_error(answer.json) : answer.error;
      failed_url_ = fetch_.url;
      log::warn("guide: {} could not be read: {}", fetch_.url, fetch_error_);
      fetch_ = {};
      continue;
    }
    if (fetch_.guide.blocks.empty()) {
      fetch_.count = page.page_count;
      fetch_.next = page.page_index + 1;
    } else {
      ++fetch_.next;
    }
    net::add_guide_page(fetch_.guide, std::move(page));
    if (fetch_.next < fetch_.count && fetch_.next < static_cast<int>(fetch_.guide.pages.size())) {
      fetch_.id = reader_.load(fetch_.guide.pages[static_cast<size_t>(fetch_.next)].url, std::string(net::guide_page_script()));
      continue;
    }
    net::Guide done = std::move(fetch_.guide);
    done.url = fetch_.url;
    if (const net::GuideEntry* entry = entry_for(done.url)) done.title = entry->title;
    write_file(folder_ / net::guide_cache_name(done.url), net::guide_to_json(done));
    log::info("guide: {} loaded ({} blocks, {} pages)", done.url, done.blocks.size(), std::max<size_t>(1, done.pages.size()));
    fetch_ = {};
    if (done.url == wanted_url_) set_guide(std::move(done));
  }
}

void GuideOverlay::load_cached(const std::string& url) {
  net::Guide cached;
  if (!net::parse_guide(read_file(folder_ / net::guide_cache_name(url)), cached)) return;
  cached.url = url;
  set_guide(std::move(cached));
}

void GuideOverlay::set_guide(net::Guide guide) {
  if (!guide_.url.empty()) remember_place();
  for (net::GuideBlock& block : guide.blocks) {
    if (block.kind == net::GuideBlockKind::Preformatted) block.text = expand_tabs(block.text);
    if (block.kind == net::GuideBlockKind::TableRow && block.text.empty()) {
      for (size_t i = 0; i < block.cells.size(); ++i) block.text += (i ? "  |  " : "") + block.cells[i];
    }
  }
  guide_ = std::move(guide);
  rows_.clear();
  contents_.clear();
  layout_dirty_ = true;
  restore_place_ = true;
  found_row_ = scroll_to_ = SIZE_MAX;
  top_row_ = 0;
}

void GuideOverlay::update(const platform::Config& cfg) {
  if (!cfg.guide_enabled) {
    close();
    if (browser_on_) reader_.stop();
    browser_on_ = false;
    fetch_ = {};
    list_id_ = 0;
    return;
  }
  if (!list_read_) {
    list_read_ = true;
    net::parse_cached_guide_list(read_file(folder_ / "list.json"), list_);
  }
  take_answers();
  if (cfg.guide_url != wanted_url_) {
    wanted_url_ = cfg.guide_url;
    failed_url_.clear();
    fetch_error_.clear();
  }
  if (!wanted_url_.empty() && guide_.url != wanted_url_ && fetch_.url != wanted_url_ && failed_url_ != wanted_url_) {
    load_cached(wanted_url_);
    if (guide_.url != wanted_url_) start_fetch(wanted_url_);
  }
  if (browser_on_) {
    const Clock::time_point now = Clock::now();
    if (!reader_.idle() || list_id_ || fetch_.id) idle_since_ = now;
    else if (now - idle_since_ > kIdleStop) {
      reader_.stop();
      browser_on_ = false;
    }
  }
}

const net::GuideEntry* GuideOverlay::entry_for(std::string_view url) const {
  for (const net::GuideEntry& entry : list_.entries)
    if (entry.url == url) return &entry;
  return nullptr;
}

bool GuideOverlay::settings_rows(platform::Config& cfg) {
  bool changed = false;
  ImGuiLayer::row("Guide Overlay", "Shows a GameFAQs guide over the game, which pauses while it is open. Pick the guide below, then press ~ "
                                   "on the keyboard or LB + RB on a controller to open or close it.");
  // The list is read from the site when there is no cached one (once a session), and again each time the box is ticked.
  if (ImGui::Checkbox("##guide", &cfg.guide_enabled)) {
    changed = true;
    if (cfg.guide_enabled) {
      list_asked_ = true;
      request_list();
    }
  }
  if (!cfg.guide_enabled) return changed;
  if (list_.entries.empty() && !list_asked_) {
    list_asked_ = true;
    request_list();
  }
  ImGuiLayer::row("Guide", "The guide the overlay shows. Untick and tick Guide Overlay to load the list again.");
  const net::GuideEntry* picked = entry_for(cfg.guide_url);
  const std::string preview = picked                  ? picked->title + " - " + picked->author
                              : !guide_.title.empty() && guide_.url == cfg.guide_url ? guide_.title
                              : !cfg.guide_url.empty() ? cfg.guide_url
                              : list_.entries.empty()  ? std::string(list_id_ ? "Loading the guide list..." : "No guides listed")
                                                       : std::string("Pick a guide");
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::BeginCombo("##guidepick", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
    std::string heading;
    for (const net::GuideEntry& entry : list_.entries) {
      const std::string group = entry.group + " - " + entry.category;
      if (group != heading) {
        heading = group;
        ImGui::SeparatorText(heading.c_str());
      }
      const std::string label = entry.title + " - " + entry.author + "##" + entry.url;
      if (ImGui::Selectable(label.c_str(), entry.url == cfg.guide_url)) {
        cfg.guide_url = entry.url;
        changed = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !entry.meta.empty())
        ImGuiLayer::tooltip((entry.meta + (entry.updated.empty() ? "" : ", updated " + entry.updated)).c_str());
    }
    ImGui::EndCombo();
  }
  auto note = [](const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
  };
  if (runtime_missing_) note(kRuntimeMissing);
  if (browser_on_ && reader_.verifying()) note("GameFAQs is checking this computer: finish the check in the window that opened.");
  else if (list_id_) note("Loading the guide list...");
  else if (!list_error_.empty() && !runtime_missing_) note("Guide list: " + list_error_);
  if (fetch_.id) note("Loading the guide (page " + std::to_string(fetch_.next + 1) + " of " + std::to_string(fetch_.count) + ")...");
  else if (!fetch_error_.empty() && failed_url_ == cfg.guide_url && !runtime_missing_) note("The guide could not be loaded: " + fetch_error_);
  else if (!guide_.url.empty() && guide_.url == cfg.guide_url) note(std::string("Ready: press ") + kHotkeyHint + " to read it.");
  return changed;
}

void GuideOverlay::toggle(const platform::Config& cfg) {
  if (open_) {
    close();
    return;
  }
  if (!cfg.guide_enabled) return;
  open_ = true;
  if (!places_read_) {
    places_read_ = true;
    if (const std::optional<net::Json> saved = net::parse_json(read_file(folder_ / "reading.json")); saved && saved->is_object())
      for (const net::Json::Member& member : saved->members())
        places_[member.first] = {static_cast<int>(member.second.int_of("block")), static_cast<int>(member.second.int_of("line"))};
  }
}

void GuideOverlay::close() {
  if (!open_) return;
  remember_place();
  open_ = false;
}

void GuideOverlay::remember_place() {
  if (guide_.url.empty() || rows_.empty()) return;
  size_t row = std::min(top_row_, rows_.size() - 1);
  while (row < rows_.size() && rows_[row].block < 0) ++row;
  if (row >= rows_.size()) return;
  places_[guide_.url] = {rows_[row].block, rows_[row].line};
  save_places();
}

void GuideOverlay::save_places() const {
  net::Json root = net::Json::object();
  for (const auto& [url, place] : places_) {
    net::Json item = net::Json::object();
    item.set("block", place.block);
    item.set("line", place.line);
    root.set(url, std::move(item));
  }
  write_file(folder_ / "reading.json", net::to_json(root));
}

void GuideOverlay::layout(float width, ImFont* ui_font, ImFont* mono_font) {
  const float size = ImGui::GetFontSize();
  if (!layout_dirty_ && width == layout_width_ && size == layout_font_) return;
  std::optional<Place> keep;
  if (restore_place_) {
    if (const auto saved = places_.find(guide_.url); saved != places_.end()) keep = saved->second;
    restore_place_ = false;
  } else if (top_row_ < rows_.size()) {
    size_t row = top_row_;
    while (row < rows_.size() && rows_[row].block < 0) ++row;
    if (row < rows_.size()) keep = Place{rows_[row].block, rows_[row].line};
  }
  rows_.clear();
  contents_.clear();
  layout_dirty_ = false;
  layout_width_ = width;
  layout_font_ = size;
  content_width_ = width;
  const float ui_scale = size / ui_font->FontSize;
  const float indent_step = size * kIndentEm;
  int top_level = 6;
  for (const net::GuideBlock& block : guide_.blocks)
    if (block.kind == net::GuideBlockKind::Heading) top_level = std::min(top_level, std::max(1, block.level));
  const bool paged = guide_.pages.size() > 1;
  int line = 0;
  auto add = [&](int block, std::string_view text, float indent, bool bullet) {
    rows_.push_back({block, block < 0 ? 0 : line++, text, indent, bullet});
  };
  auto blank = [&] {
    if (!rows_.empty() && rows_.back().block >= 0) add(-1, {}, 0.0f, false);
  };
  auto wrapped = [&](int block, std::string_view text, float indent, bool bullet) {
    const float avail = std::max(size * 6.0f, width - indent);
    size_t start = 0;
    bool first = true;
    while (start < text.size()) {
      size_t end = text.find('\n', start);
      if (end == std::string_view::npos) end = text.size();
      const char* cursor = text.data() + start;
      const char* stop = text.data() + end;
      if (cursor == stop) add(block, {}, indent, false);
      while (cursor < stop) {
        const char* cut = ui_font->CalcWordWrapPositionA(ui_scale, cursor, stop, avail);
        if (cut <= cursor) {
          cut = cursor + 1;
          while (cut < stop && (static_cast<unsigned char>(*cut) & 0xC0) == 0x80) ++cut;
        }
        add(block, trim_right(std::string_view(cursor, static_cast<size_t>(cut - cursor))), indent, bullet && first);
        first = false;
        cursor = cut;
        while (cursor < stop && (*cursor == ' ' || *cursor == '\t')) ++cursor;
      }
      start = end + 1;
    }
  };
  int page = -1;
  for (size_t i = 0; i < guide_.blocks.size(); ++i) {
    const net::GuideBlock& block = guide_.blocks[i];
    const int index = static_cast<int>(i);
    line = 0;
    if (paged && block.page != page) {
      page = block.page;
      if (page >= 0 && page < static_cast<int>(guide_.pages.size())) contents_.push_back({guide_.pages[static_cast<size_t>(page)].name, rows_.size()});
    }
    switch (block.kind) {
      case net::GuideBlockKind::Heading: {
        blank();
        const int depth = std::max(0, block.level - top_level);
        if (depth < 3) contents_.push_back({std::string(static_cast<size_t>(depth + (paged ? 1 : 0)) * 3, ' ') + block.text, rows_.size()});
        wrapped(index, block.text, 0.0f, false);
        break;
      }
      case net::GuideBlockKind::Paragraph:
        wrapped(index, block.text, 0.0f, false);
        blank();
        break;
      case net::GuideBlockKind::ListItem:
        wrapped(index, block.text, indent_step * static_cast<float>(std::max(1, block.level)), true);
        if (i + 1 == guide_.blocks.size() || guide_.blocks[i + 1].kind != net::GuideBlockKind::ListItem) blank();
        break;
      case net::GuideBlockKind::Preformatted: {
        const std::string_view text = block.text;
        size_t start = 0;
        while (start <= text.size()) {
          size_t end = text.find('\n', start);
          if (end == std::string_view::npos) end = text.size();
          const std::string_view row = trim_right(text.substr(start, end - start));
          content_width_ = std::max(content_width_, text_width(mono_font, row));
          add(index, row, 0.0f, false);
          start = end + 1;
        }
        blank();
        break;
      }
      case net::GuideBlockKind::TableRow:
        wrapped(index, block.text, 0.0f, false);
        if (i + 1 == guide_.blocks.size() || guide_.blocks[i + 1].kind != net::GuideBlockKind::TableRow) blank();
        break;
      case net::GuideBlockKind::Rule:
        add(index, {}, 0.0f, false);
        blank();
        break;
    }
  }
  if (keep) {
    for (size_t i = 0; i < rows_.size(); ++i) {
      if (rows_[i].block > keep->block || (rows_[i].block == keep->block && rows_[i].line >= keep->line)) {
        scroll_to_ = i;
        break;
      }
    }
  }
}

void GuideOverlay::jump_to_row(size_t row) {
  scroll_to_ = row;
  top_row_ = row;
}

void GuideOverlay::search(bool forward) {
  if (query_.empty() || rows_.empty()) return;
  const std::string wanted = lower(query_);
  const size_t count = rows_.size();
  const size_t from = found_row_ < count ? found_row_ : top_row_;
  for (size_t step = 1; step <= count; ++step) {
    const size_t row = forward ? (from + step) % count : (from + count - step % count) % count;
    if (lower(rows_[row].text).find(wanted) != std::string::npos) {
      found_row_ = row;
      jump_to_row(row > 3 ? row - 3 : 0);
      return;
    }
  }
  found_row_ = SIZE_MAX;
}

void GuideOverlay::draw_header() {
  const net::GuideEntry* entry = entry_for(guide_.url);
  const std::string title = !guide_.title.empty() ? guide_.title : entry ? entry->title : guide_.url;
  ImGui::TextUnformatted(title.c_str());
  if (!guide_.author.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("by %s", guide_.author.c_str());
  }
  if (!guide_.updated.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", guide_.updated.c_str());
  }
  ImGui::SameLine();
  ImGui::TextLinkOpenURL("Open on GameFAQs", guide_.url.c_str());

  const ImGuiStyle& style = ImGui::GetStyle();
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.38f);
  ImGui::BeginDisabled(contents_.empty());
  if (ImGui::BeginCombo("##contents", contents_.empty() ? "No contents (use Search)" : "Contents", ImGuiComboFlags_HeightLarge)) {
    for (size_t i = 0; i < contents_.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(contents_[i].first.c_str())) jump_to_row(contents_[i].second);
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  const float buttons_w = (ImGui::GetFrameHeight() + style.ItemSpacing.x) * 2.0f;
  ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 6.0f, ImGui::GetContentRegionAvail().x - buttons_w));
  const bool entered = ImGui::InputTextWithHint("##find", "Search (Enter = next)", query_buffer_, sizeof(query_buffer_), ImGuiInputTextFlags_EnterReturnsTrue);
  if (query_ != query_buffer_) {
    query_ = query_buffer_;
    found_row_ = SIZE_MAX;
  }
  if (entered) {
    search(true);
    ImGui::SetKeyboardFocusHere(-1);
  }
  ImGui::SameLine();
  if (ImGui::ArrowButton("##prev", ImGuiDir_Up)) search(false);
  ImGui::SameLine();
  if (ImGui::ArrowButton("##next", ImGuiDir_Down)) search(true);
}

void GuideOverlay::draw_body(const platform::Input& in, double dt, ImFont* ui_font, ImFont* mono_font) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float width = ImGui::GetContentRegionAvail().x - style.ScrollbarSize - style.WindowPadding.x * 2.0f;
  layout(std::max(width, ImGui::GetFontSize() * 8.0f), ui_font, mono_font);
  if (content_width_ > layout_width_) ImGui::SetNextWindowContentSize(ImVec2(content_width_ + style.WindowPadding.x, 0.0f));
  if (!ImGui::BeginChild("##text", ImVec2(0, 0), ImGuiChildFlags_Borders,
                         ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus)) {
    ImGui::EndChild();
    return;
  }
  const float row_h = ImGui::GetTextLineHeightWithSpacing();
  const float page = std::max(row_h, ImGui::GetWindowHeight() - row_h * 2.0f);
  if (scroll_to_ != SIZE_MAX) {
    ImGui::SetScrollY(static_cast<float>(scroll_to_) * row_h);
    scroll_to_ = SIZE_MAX;
  } else {
    float move = 0.0f;
    const float seconds = static_cast<float>(dt);
    const float speed = kScrollRows * row_h * seconds;
    const bool typing = ImGui::GetIO().WantTextInput;
    const bool pad = in.pad_connected;
    if (pad) {
      const float stick = in.pad_axis[SDL_GAMEPAD_AXIS_LEFTY] + in.pad_axis[SDL_GAMEPAD_AXIS_RIGHTY];
      if (std::fabs(stick) > 0.25f) move += stick * speed * 2.0f;
      if (in.pad_down[SDL_GAMEPAD_BUTTON_DPAD_DOWN]) move += speed;
      if (in.pad_down[SDL_GAMEPAD_BUTTON_DPAD_UP]) move -= speed;
    }
    // The shoulder buttons, PageUp / PageDown and Space turn a page on the press and keep turning pages while held. Both shoulders
    // down is the chord that closes the overlay: it turns no page.
    const bool shoulders = pad && !(in.pad_down[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] && in.pad_down[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER]);
    const bool down_held = (shoulders && in.pad_down[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER]) ||
                           (!typing && (in.key_down[SDL_SCANCODE_PAGEDOWN] || in.key_down[SDL_SCANCODE_SPACE]));
    const bool down_pressed = (shoulders && in.pad_pressed[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER]) ||
                              (!typing && (in.key_pressed[SDL_SCANCODE_PAGEDOWN] || in.key_pressed[SDL_SCANCODE_SPACE]));
    const bool up_held = (shoulders && in.pad_down[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER]) || (!typing && in.key_down[SDL_SCANCODE_PAGEUP]);
    const bool up_pressed = (shoulders && in.pad_pressed[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER]) || (!typing && in.key_pressed[SDL_SCANCODE_PAGEUP]);
    page_down_held_ = down_held ? page_down_held_ + seconds : 0.0f;
    page_up_held_ = up_held ? page_up_held_ + seconds : 0.0f;
    move += page * static_cast<float>(pages_turned(down_pressed, page_down_held_, seconds) - pages_turned(up_pressed, page_up_held_, seconds));
    if (!typing) {
      if (in.key_down[SDL_SCANCODE_DOWN]) move += speed;
      if (in.key_down[SDL_SCANCODE_UP]) move -= speed;
      if (in.key_pressed[SDL_SCANCODE_HOME]) move = -ImGui::GetScrollY();
      if (in.key_pressed[SDL_SCANCODE_END]) move = ImGui::GetScrollMaxY();
    }
    if (move != 0.0f) ImGui::SetScrollY(std::clamp(ImGui::GetScrollY() + move, 0.0f, ImGui::GetScrollMaxY()));
  }
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float size = ImGui::GetFontSize();
  const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
  const ImU32 heading = ImGui::GetColorU32(ImGuiCol_SliderGrabActive);
  const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
  const ImU32 found = ImGui::GetColorU32(ImGuiCol_Header);
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(rows_.size()), row_h);
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      const Row& row = rows_[static_cast<size_t>(i)];
      const ImVec2 pos = ImGui::GetCursorScreenPos();
      if (static_cast<size_t>(i) == found_row_)
        draw->AddRectFilled(pos, ImVec2(pos.x + content_width_, pos.y + ImGui::GetTextLineHeight()), found);
      if (row.block >= 0) {
        const net::GuideBlock& block = guide_.blocks[static_cast<size_t>(row.block)];
        const char* begin = row.text.data();
        const char* end = begin + row.text.size();
        switch (block.kind) {
          case net::GuideBlockKind::Heading: draw->AddText(ui_font, size, pos, heading, begin, end); break;
          case net::GuideBlockKind::ListItem:
            if (row.bullet) draw->AddCircleFilled(ImVec2(pos.x + row.indent - size * 0.55f, pos.y + size * 0.55f), size * 0.14f, dim);
            draw->AddText(ui_font, size, ImVec2(pos.x + row.indent, pos.y), text, begin, end);
            break;
          case net::GuideBlockKind::Preformatted: draw->AddText(mono_font, size, pos, text, begin, end); break;
          case net::GuideBlockKind::Rule:
            draw->AddLine(ImVec2(pos.x, pos.y + size * 0.5f), ImVec2(pos.x + layout_width_, pos.y + size * 0.5f), dim);
            break;
          default: draw->AddText(ui_font, size, pos, text, begin, end); break;
        }
      }
      ImGui::Dummy(ImVec2(content_width_, ImGui::GetTextLineHeight()));
    }
  }
  top_row_ = static_cast<size_t>(std::max(0.0f, ImGui::GetScrollY()) / row_h);
  ImGui::EndChild();
}

void GuideOverlay::draw(ImGuiLayer& ui, const platform::Input& in, double dt) {
  if (!open_) return;
  ui.request_mono_font();
  if (in.pad_connected && in.pad_pressed[SDL_GAMEPAD_BUTTON_EAST]) {
    close();
    return;
  }
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  // Wide screens get the width a 16:9 screen would (1.3 times the height at most), so lines stay readable and text guides fill it.
  const float width = std::min(io.DisplaySize.x * 0.8f, io.DisplaySize.y * kMaxWidthPerHeight);
  ImGui::SetNextWindowSize(ImVec2(width, io.DisplaySize.y * 0.86f), ImGuiCond_Appearing);
  bool keep = true;
  if (ImGui::Begin("Guide###guide_overlay", &keep, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav)) {
    if (wanted_url_.empty()) {
      ImGui::TextWrapped("No guide picked yet: choose one under Settings (Esc) > General > Quality of life.");
    } else if (guide_.url != wanted_url_) {
      if (fetch_.id) ImGui::Text("Loading the guide (page %d of %d)...", fetch_.next + 1, fetch_.count);
      else if (!fetch_error_.empty()) {
        ImGui::TextWrapped("The guide could not be loaded: %s", fetch_error_.c_str());
        if (!runtime_missing_ && ImGui::Button("Try again")) start_fetch(wanted_url_);
      } else {
        ImGui::TextUnformatted("Loading the guide...");
      }
      if (browser_on_ && reader_.verifying()) ImGui::TextWrapped("GameFAQs is checking this computer: finish the check in the window that opened.");
    } else {
      draw_header();
      draw_body(in, dt, ImGui::GetFont(), ui.mono_font());
    }
  }
  ImGui::End();
  if (!keep) close();
}

}  // namespace oc::render
