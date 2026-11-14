#pragma once
// A hidden page in the running Steam client's own browser (its HTML surface), for WebPageReader when the WebView2 runtime is missing
// (Proton) or when OC_GUIDE_BROWSER=steam asks for it. Owned and pumped by one thread, which opens its own pipe to the client.
//
// The page is never drawn, and the browser keeps its dialogs to itself, so a script's value comes back through the page title, the
// one piece of page state the client reports. The expression is wrapped so that it keeps its value as JSON in a page variable named
// after a token chosen at open() and sets the title to the first part (3900 characters, '%', ' ' and '\' escaped as %XX) behind a
// header of token, run, part and part count; each part received asks the page for the next, and after the last the page gets its
// own title back. Every navigation the page starts is allowed; dialogs are dismissed.
#ifdef _WIN32
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "platform/steam_abi.h"

namespace oc::platform {

class SteamBrowser {
 public:
  struct Event {
    enum class Kind { Ready, Loaded, Answer, Closed };
    Kind kind;
    // Loaded: the page's address; Answer: the script's value as JSON text.
    std::string text;
  };

  SteamBrowser() = default;
  ~SteamBrowser();
  SteamBrowser(const SteamBrowser&) = delete;
  SteamBrowser& operator=(const SteamBrowser&) = delete;

  // Connects to the running client and asks it for a browser (Ready follows). False, with `reason` set, when that cannot start.
  bool open(std::string& reason);
  // Steam's messages since the last call.
  void pump(std::vector<Event>& events);
  bool ready() const { return browser_ != 0; }
  void load(const std::string& url);
  // Evaluates `expression` in the page; its value arrives as an Answer. A newer call drops the older one's value.
  void evaluate(const std::string& expression);
  // No part of the current evaluation has arrived for `limit`.
  bool stalled(std::chrono::steady_clock::duration limit) const;

 private:
  void message(std::int32_t id, const unsigned char* data, std::int32_t size, std::vector<Event>& events);
  void title(std::string_view text, std::vector<Event>& events);
  void run_script(const std::string& script);

  steam_abi::GetCallbackFn get_callback_ = nullptr;
  steam_abi::FreeCallbackFn free_callback_ = nullptr;
  steam_abi::GetCallResultFn get_call_result_ = nullptr;
  void* client_ = nullptr;
  void* utils_ = nullptr;
  void* html_ = nullptr;
  std::int32_t pipe_ = 0, user_ = 0;
  bool initialised_ = false;
  std::uint64_t create_call_ = 0;
  std::uint32_t browser_ = 0;
  std::string token_;
  std::uint32_t run_ = 0;
  // The current run's parts so far.
  std::string received_;
  std::uint32_t received_parts_ = 0;
  std::chrono::steady_clock::time_point progress_{};
};

}  // namespace oc::platform
#endif
