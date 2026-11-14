#include "platform/steam_browser.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <charconv>
#include <cstring>
#include <random>
#include <system_error>

#include "core/log.h"

namespace oc::platform {

namespace {

using namespace steam_abi;

constexpr const char* kHtmlInterface = "STEAMHTMLSURFACE_INTERFACE_VERSION_005";
constexpr const char* kUtilsInterface = "SteamUtils011";

// Slots in the HTML surface's table of functions.
enum HtmlSlot {
  kInit = 1,
  kShutdown = 2,
  kCreateBrowser = 3,
  kRemoveBrowser = 4,
  kLoadUrl = 5,
  kSetSize = 6,
  kExecuteJavascript = 12,
  kAllowStartRequest = 35,
  kJsDialogResponse = 36,
};
// The utils slot that runs the client's per-frame work for this process; without it no call result arrives.
constexpr int kUtilsRunFrame = 13;

// The HTML surface's messages. Each starts with the browser handle; the ones read here then hold a string pointer at offset 8.
enum HtmlMessage : std::int32_t {
  kBrowserReady = 4501,
  kStartRequest = 4503,
  kCloseBrowser = 4504,
  kFinishedRequest = 4506,
  kChangedTitle = 4508,
  kJsAlert = 4514,
  kJsConfirm = 4515,
  kBrowserRestarted = 4527,
};

constexpr std::uint32_t kPageW = 1280, kPageH = 800;
// Characters of the value per title (the browser keeps 4096 of a title; the rest is the part's header).
constexpr int kPartSize = 3900;

std::uint32_t handle_of(const unsigned char* data, std::int32_t size) {
  std::uint32_t handle = 0;
  if (size >= 4) std::memcpy(&handle, data, sizeof(handle));
  return handle;
}

const char* text_at(const unsigned char* data, std::int32_t size, std::int32_t offset) {
  const char* text = nullptr;
  if (size >= offset + static_cast<std::int32_t>(sizeof(text))) std::memcpy(&text, data + offset, sizeof(text));
  return text ? text : "";
}

// The next space-separated number in `text` from `at`; false when there is none.
bool read_number(std::string_view text, size_t& at, std::uint32_t& out) {
  const size_t end = text.find(' ', at);
  if (end == std::string_view::npos) return false;
  const auto [ptr, error] = std::from_chars(text.data() + at, text.data() + end, out);
  if (error != std::errc() || ptr != text.data() + end) return false;
  at = end + 1;
  return true;
}

// Undoes the page side's %XX escapes (of '%', ' ' and '\', which a title would not keep as they are).
std::string unescape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    unsigned value = 0;
    if (text[i] == '%' && i + 2 < text.size() && std::from_chars(text.data() + i + 1, text.data() + i + 3, value, 16).ec == std::errc()) {
      out += static_cast<char>(value);
      i += 2;
    } else {
      out += text[i];
    }
  }
  return out;
}

}  // namespace

SteamBrowser::~SteamBrowser() {
  if (html_) {
    if (browser_) call_slot<void>(html_, kRemoveBrowser, browser_);
    if (initialised_) call_slot<bool>(html_, kShutdown);
  }
  if (client_ && pipe_) {
    if (user_) call_slot<void>(client_, kReleaseUser, pipe_, user_);
    call_slot<bool>(client_, kReleasePipe, pipe_);
  }
}

bool SteamBrowser::open(std::string& reason) {
  reason = "Steam is not running.";
  set_app_id();
  std::wstring library;
  if (!client_library(library)) return false;
  const HMODULE module = LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!module) {
    log::warn("steam browser: cannot load the client library ({})", GetLastError());
    return false;
  }
  const auto create_interface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"));
  get_callback_ = reinterpret_cast<GetCallbackFn>(GetProcAddress(module, "Steam_BGetCallback"));
  free_callback_ = reinterpret_cast<FreeCallbackFn>(GetProcAddress(module, "Steam_FreeLastCallback"));
  get_call_result_ = reinterpret_cast<GetCallResultFn>(GetProcAddress(module, "Steam_GetAPICallResult"));
  reason = "Steam's browser is not available.";
  if (!create_interface || !get_callback_ || !free_callback_ || !get_call_result_) {
    log::warn("steam browser: the client library lacks an expected export");
    return false;
  }
  client_ = create_interface(kClientInterface, nullptr);
  if (!client_) return false;
  pipe_ = call_slot<std::int32_t>(client_, kCreatePipe);
  if (pipe_) user_ = call_slot<std::int32_t>(client_, kConnectGlobalUser, pipe_);
  if (!user_) {
    reason = "Steam has no user signed in.";
    return false;
  }
  utils_ = call_slot<void*>(client_, kGenericInterface, user_, pipe_, kUtilsInterface);
  html_ = call_slot<void*>(client_, kGenericInterface, user_, pipe_, kHtmlInterface);
  if (!utils_ || !html_) {
    log::warn("steam browser: {} or {} unavailable", kUtilsInterface, kHtmlInterface);
    return false;
  }
  initialised_ = call_slot<bool>(html_, kInit);
  if (!initialised_) {
    log::warn("steam browser: the HTML surface did not start");
    return false;
  }
  create_call_ = call_slot<std::uint64_t>(html_, kCreateBrowser, static_cast<const char*>(nullptr), static_cast<const char*>(nullptr));
  if (!create_call_) {
    log::warn("steam browser: no browser was created");
    return false;
  }
  std::random_device random;
  token_ = "p" + std::to_string(random()) + std::to_string(random());
  log::info("steam browser: asked for a browser");
  return true;
}

void SteamBrowser::pump(std::vector<Event>& events) {
  call_slot<void>(utils_, kUtilsRunFrame);
  CallbackMessage received{};
  std::int32_t call = 0;
  while (get_callback_(pipe_, &received, &call)) {
    message(received.id, received.data, received.size, events);
    free_callback_(pipe_);
  }
}

void SteamBrowser::message(std::int32_t id, const unsigned char* data, std::int32_t size, std::vector<Event>& events) {
  if (id == kCallCompleted) {
    CallCompleted completed{};
    if (size < static_cast<std::int32_t>(sizeof(completed))) return;
    std::memcpy(&completed, data, sizeof(completed));
    if (completed.call != create_call_ || completed.callback != kBrowserReady) return;
    std::uint32_t handle = 0;
    bool failed = true;
    create_call_ = 0;
    if (!get_call_result_(pipe_, completed.call, &handle, sizeof(handle), kBrowserReady, &failed) || failed || !handle) {
      log::warn("steam browser: the browser did not start");
      events.push_back({Event::Kind::Closed, {}});
      return;
    }
    browser_ = handle;
    call_slot<void>(html_, kSetSize, browser_, kPageW, kPageH);
    log::info("steam browser: ready");
    events.push_back({Event::Kind::Ready, {}});
    return;
  }
  if (id < kBrowserReady || id > kBrowserRestarted || !browser_) return;
  const std::uint32_t handle = handle_of(data, size);
  if (id == kBrowserRestarted) {
    std::uint32_t old_handle = 0;
    if (size >= 8) std::memcpy(&old_handle, data + 4, sizeof(old_handle));
    if (old_handle == browser_) browser_ = handle;
    return;
  }
  if (handle != browser_) return;
  switch (id) {
    case kStartRequest: call_slot<void>(html_, kAllowStartRequest, browser_, true); break;
    case kCloseBrowser:
      browser_ = 0;
      events.push_back({Event::Kind::Closed, {}});
      break;
    case kFinishedRequest: events.push_back({Event::Kind::Loaded, text_at(data, size, 8)}); break;
    case kChangedTitle: title(text_at(data, size, 8), events); break;
    // Dialogs the page opens are dismissed.
    case kJsAlert:
    case kJsConfirm: call_slot<void>(html_, kJsDialogResponse, browser_, false); break;
    default: break;
  }
}

// A part reads "<token> <run> <part> <parts> <escaped json text>"; each one received asks the page for the next.
void SteamBrowser::title(std::string_view text, std::vector<Event>& events) {
  if (text.substr(0, token_.size() + 1) != token_ + " ") return;
  size_t at = token_.size() + 1;
  std::uint32_t run = 0, part = 0, parts = 0;
  if (!read_number(text, at, run) || !read_number(text, at, part) || !read_number(text, at, parts) || run != run_ || part != received_parts_ ||
      part >= parts)
    return;
  received_ += text.substr(at);
  ++received_parts_;
  progress_ = std::chrono::steady_clock::now();
  const std::string sent = "window." + token_;
  if (received_parts_ < parts) {
    const std::string next = std::to_string(received_parts_), size = std::to_string(kPartSize);
    run_script("(() => { const sent = " + sent + "; if (sent && sent.run === " + std::to_string(run_) + ") document.title = '" + token_ + " " +
               std::to_string(run_) + " " + next + " ' + sent.parts + ' ' + sent.text.slice(" + next + " * " + size + ", (" + next + " + 1) * " + size +
               "); })()");
    return;
  }
  run_script("(() => { const sent = " + sent + "; if (sent) { document.title = sent.title; delete " + sent + "; } })()");
  events.push_back({Event::Kind::Answer, unescape(received_)});
  received_.clear();
}

void SteamBrowser::load(const std::string& url) {
  if (browser_) call_slot<void>(html_, kLoadUrl, browser_, url.c_str(), static_cast<const char*>(nullptr));
}

void SteamBrowser::evaluate(const std::string& expression) {
  if (!browser_) return;
  ++run_;
  received_.clear();
  received_parts_ = 0;
  progress_ = std::chrono::steady_clock::now();
  const std::string run = std::to_string(run_), size = std::to_string(kPartSize);
  run_script("(() => { let value = null; try { value = " + expression +
             "; } catch (error) { value = null; }\n"
             "const text = JSON.stringify(value === undefined ? null : value).replace(/[% \\\\]/g, mark => '%' + mark.charCodeAt(0).toString(16));\n"
             "const parts = Math.max(1, Math.ceil(text.length / " + size + "));\n"
             "window." + token_ + " = {run: " + run + ", text, parts, title: document.title};\n"
             "document.title = '" + token_ + " " + run + " 0 ' + parts + ' ' + text.slice(0, " + size + ");\n})()");
}

bool SteamBrowser::stalled(std::chrono::steady_clock::duration limit) const { return std::chrono::steady_clock::now() - progress_ > limit; }

void SteamBrowser::run_script(const std::string& script) { call_slot<void>(html_, kExecuteJavascript, browser_, script.c_str()); }

}  // namespace oc::platform
#endif
