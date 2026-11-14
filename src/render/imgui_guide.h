#pragma once
// The guide overlay: a GameFAQs guide read over the paused game, all ImGui. The settings rows (General tab, Quality of life) switch it
// on and pick the guide from the site's list; the hidden browser (platform::WebPageReader) downloads the list and the guide, and both
// are kept in a cache folder so a guide read once opens at once and offline. The hotkey shows the guide in a window over the dimmed
// game: title, author and source link, a contents list (headings and pages), search, re-download, and the text itself (preformatted
// guides in a monospaced font, HTML guides as headings, paragraphs, lists and tables). The place read last is kept per guide.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "net/guide.h"
#include "platform/config.h"
#include "platform/web_page.h"
#include "platform/window.h"

struct ImFont;

namespace oc::render {

class ImGuiLayer;

class GuideOverlay {
 public:
  // `folder` holds list.json, one <id>.json per guide, reading.json (the places read) and the browser profile.
  void init(std::filesystem::path folder);
  void shutdown();

  // Each frame before the ImGui frame: takes the browser's answers, loads or downloads the guide `cfg` names, and ends the browser
  // after a minute without work. Off: closes the overlay and the browser.
  void update(const platform::Config& cfg);
  // The settings rows, inside the Quality of life table. True when `cfg` changed.
  bool settings_rows(platform::Config& cfg);

  bool open() const { return open_; }
  void toggle(const platform::Config& cfg);
  void close();
  // The overlay window, inside the ImGui frame. `in` is the frame's input (the pad scrolls, B closes).
  void draw(ImGuiLayer& ui, const platform::Input& in, double dt);

 private:
  struct Row {
    int block = 0;
    int line = 0;
    std::string_view text;
    float indent = 0.0f;
    bool bullet = false;
  };
  struct Place {
    int block = 0;
    int line = 0;
  };
  struct Fetch {
    std::string url;
    std::uint64_t id = 0;
    net::Guide guide;
    int next = 0, count = 1;
  };

  void start_browser();
  void request_list();
  void start_fetch(const std::string& url);
  void take_answers();
  void load_cached(const std::string& url);
  void set_guide(net::Guide guide);
  void layout(float width, ImFont* ui_font, ImFont* mono_font);
  void remember_place();
  void save_places() const;
  void jump_to_row(size_t row);
  void search(bool forward);
  void draw_header();
  void draw_body(const platform::Input& in, double dt, ImFont* ui_font, ImFont* mono_font);
  const net::GuideEntry* entry_for(std::string_view url) const;

  std::filesystem::path folder_;
  platform::WebPageReader reader_;
  bool browser_on_ = false;
  bool runtime_missing_ = false;
  std::chrono::steady_clock::time_point idle_since_{};

  net::GuideList list_;
  bool list_read_ = false, list_asked_ = false;
  std::uint64_t list_id_ = 0;
  std::string list_error_;

  net::Guide guide_;
  Fetch fetch_;
  std::string fetch_error_, failed_url_;
  std::string wanted_url_;

  bool open_ = false;
  std::map<std::string, Place> places_;
  bool places_read_ = false;

  // The guide laid out for one body width; rebuilt when the width, the font size or the guide changes.
  std::vector<Row> rows_;
  std::vector<std::pair<std::string, size_t>> contents_;
  float layout_width_ = 0.0f, layout_font_ = 0.0f, content_width_ = 0.0f;
  bool layout_dirty_ = true;
  // A row to bring to the top once the body is laid out (SIZE_MAX = none), the row the search found, the place to restore.
  size_t scroll_to_ = SIZE_MAX, found_row_ = SIZE_MAX;
  bool restore_place_ = false;
  std::string query_;
  char query_buffer_[128] = {};
  size_t top_row_ = 0;
  // How long the page-down / page-up buttons have been held (seconds).
  float page_down_held_ = 0.0f, page_up_held_ = 0.0f;
};

}  // namespace oc::render
