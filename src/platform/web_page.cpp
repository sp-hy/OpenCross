// The page reader: a worker thread with a message loop (WebView2 runs on a single-threaded COM apartment), one hidden top-level window
// that hosts the browser and doubles as the "verify" window, and a poll timer that evaluates the request's script. Without the
// WebView2 runtime (or with OC_GUIDE_BROWSER=steam) the page is a SteamBrowser instead, pumped by a second timer; it has no window
// to show, so a "verify" page there runs into the timeout.
#include "platform/web_page.h"

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#include "core/log.h"
#include "platform/steam_abi.h"
#include "platform/steam_browser.h"

namespace oc::platform {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

constexpr UINT kWakeMessage = WM_APP + 1;
constexpr UINT kQuitMessage = WM_APP + 2;
constexpr UINT_PTR kPollTimer = 1, kPumpTimer = 2;
constexpr UINT kPollMs = 250, kPumpMs = 16;
// A Steam script run that sends no part back in this time is given up, so the next poll runs it again.
constexpr auto kScriptLost = std::chrono::seconds(3);
constexpr auto kPageTimeout = std::chrono::seconds(30);
constexpr auto kVerifyTimeout = std::chrono::minutes(3);
// A "verify" answer that lasts this long shows the window (sites that check silently answer sooner).
constexpr auto kVerifyGrace = std::chrono::seconds(4);
constexpr int kWindowW = 1000, kWindowH = 760;
constexpr wchar_t kWindowClass[] = L"OpenCrossWebPage";
constexpr wchar_t kWindowTitle[] = L"OpenCross - confirm the guide site";

std::wstring widen(std::string_view text) {
  if (text.empty()) return {};
  const int size = static_cast<int>(text.size());
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0);
  std::wstring out(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), size, out.data(), length);
  return out;
}

std::string narrow(const wchar_t* text) {
  if (!text || !*text) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(length > 0 ? length - 1 : 0), '\0');
  if (length > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
  return out;
}

}  // namespace

struct WebPageReader::Impl {
  struct Request {
    std::uint64_t id = 0;
    std::string url, script;
  };

  // Shared with the main thread, under `mutex`.
  mutable std::mutex mutex;
  std::condition_variable window_made;
  std::thread worker;
  State state = State::Off;
  std::string reason;
  std::deque<Request> queue;
  std::vector<Answer> answers;
  bool verifying = false;
  bool busy = false;
  bool window_done = false;
  HWND hwnd = nullptr;
  std::uint64_t next_id = 1;

  // The worker's own.
  std::filesystem::path profile;
  ComPtr<ICoreWebView2Environment> environment;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> view;
  std::unique_ptr<SteamBrowser> steam;
  std::optional<Request> current;
  bool page_loaded = false;
  bool script_running = false;
  bool shown = false;
  Clock::time_point deadline;
  std::optional<Clock::time_point> verify_since;

  void run();
  void create_browser();
  void start_steam();
  void pump_steam();
  void browser_failed(HRESULT result);
  void unavailable(std::string why);
  void navigate(const std::string& url);
  void attach(ICoreWebView2Controller* made);
  void page_completed(ICoreWebView2NavigationCompletedEventArgs* args);
  void next_request();
  void poll();
  void script_answered(std::uint64_t id, HRESULT result, const wchar_t* value);
  void script_value(std::string json);
  void finish(bool ok, std::string json, std::string error);
  void show(bool on);
  void answer(Answer reply);
  static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
};

WebPageReader::WebPageReader() : impl_(std::make_unique<Impl>()) {}

WebPageReader::~WebPageReader() { stop(); }

void WebPageReader::start(const std::filesystem::path& profile_dir) {
  std::unique_lock lock(impl_->mutex);
  if (impl_->worker.joinable()) return;
  impl_->state = State::Starting;
  impl_->reason.clear();
  impl_->window_done = false;
  impl_->profile = profile_dir;
  impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });
  impl_->window_made.wait(lock, [&] { return impl_->window_done; });
}

void WebPageReader::stop() {
  HWND window = nullptr;
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->worker.joinable()) return;
    window = impl_->hwnd;
  }
  if (window) PostMessageW(window, kQuitMessage, 0, 0);
  impl_->worker.join();
  std::lock_guard lock(impl_->mutex);
  impl_->state = State::Off;
  impl_->queue.clear();
  impl_->busy = impl_->verifying = false;
  impl_->hwnd = nullptr;
}

WebPageReader::State WebPageReader::state() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->state;
}

std::string WebPageReader::unavailable_reason() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->reason;
}

std::uint64_t WebPageReader::load(std::string url, std::string script) {
  std::lock_guard lock(impl_->mutex);
  const std::uint64_t id = impl_->next_id++;
  if (impl_->state == State::Unavailable) {
    impl_->answers.push_back({id, false, {}, impl_->reason});
    return id;
  }
  impl_->queue.push_back({id, std::move(url), std::move(script)});
  if (impl_->hwnd) PostMessageW(impl_->hwnd, kWakeMessage, 0, 0);
  return id;
}

std::vector<WebPageReader::Answer> WebPageReader::take_answers() {
  std::lock_guard lock(impl_->mutex);
  std::vector<Answer> out;
  out.swap(impl_->answers);
  return out;
}

bool WebPageReader::verifying() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->verifying;
}

bool WebPageReader::idle() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->queue.empty() && !impl_->busy;
}

bool WebPageReader::available() {
  std::wstring library;
  return !runtime_version().empty() || steam_abi::client_library(library);
}

std::string WebPageReader::runtime_version() {
  LPWSTR version = nullptr;
  if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || !version) return {};
  std::string out = narrow(version);
  CoTaskMemFree(version);
  return out;
}

void WebPageReader::Impl::run() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorA(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  RegisterClassExW(&window_class);
  const HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, kWindowW, kWindowH,
                                      nullptr, nullptr, instance, this);
  {
    std::lock_guard lock(mutex);
    hwnd = window;
    window_done = true;
    if (!window) {
      state = State::Unavailable;
      reason = "The browser window could not be created.";
    }
  }
  window_made.notify_all();
  if (window) {
    create_browser();
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (controller) controller->Close();
    steam.reset();
    view.Reset();
    controller.Reset();
    environment.Reset();
    DestroyWindow(window);
  }
  if (SUCCEEDED(com)) CoUninitialize();
}

void WebPageReader::Impl::create_browser() {
  const char* choice = std::getenv("OC_GUIDE_BROWSER");
  if ((choice && std::string_view(choice) == "steam") || WebPageReader::runtime_version().empty()) {
    start_steam();
    return;
  }
  const std::wstring folder = profile.wstring();
  const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, folder.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this](HRESULT result, ICoreWebView2Environment* made) -> HRESULT {
        if (FAILED(result) || !made) {
          browser_failed(result);
          return S_OK;
        }
        environment = made;
        const HRESULT created = environment->CreateCoreWebView2Controller(
            hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([this](HRESULT outcome, ICoreWebView2Controller* host) -> HRESULT {
                    if (FAILED(outcome) || !host) browser_failed(outcome);
                    else attach(host);
                    return S_OK;
                  }).Get());
        if (FAILED(created)) browser_failed(created);
        return S_OK;
      }).Get());
  if (FAILED(started)) browser_failed(started);
}

void WebPageReader::Impl::start_steam() {
  steam = std::make_unique<SteamBrowser>();
  std::string why;
  if (!steam->open(why)) {
    steam.reset();
    log::warn("web page: Steam's browser did not start ({})", why);
    unavailable(why + " Guides need Steam or the Microsoft Edge WebView2 runtime.");
    return;
  }
  SetTimer(hwnd, kPumpTimer, kPumpMs, nullptr);
}

void WebPageReader::Impl::pump_steam() {
  if (!steam) return;
  std::vector<SteamBrowser::Event> events;
  steam->pump(events);
  for (SteamBrowser::Event& event : events) {
    switch (event.kind) {
      case SteamBrowser::Event::Kind::Ready:
        {
          std::lock_guard lock(mutex);
          state = State::Ready;
        }
        log::info("web page: browser ready (Steam)");
        next_request();
        break;
      case SteamBrowser::Event::Kind::Loaded:
        if (current && event.text != "about:blank") {
          page_loaded = true;
          poll();
        }
        break;
      case SteamBrowser::Event::Kind::Answer:
        script_running = false;
        if (current) script_value(std::move(event.text));
        break;
      case SteamBrowser::Event::Kind::Closed:
        KillTimer(hwnd, kPumpTimer);
        KillTimer(hwnd, kPollTimer);
        steam.reset();
        if (current) answer({current->id, false, {}, "Steam's browser closed."});
        current.reset();
        unavailable("Steam's browser closed.");
        return;
    }
  }
}

void WebPageReader::Impl::browser_failed(HRESULT result) {
  const bool missing = result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  log::warn("web page: the browser did not start (HRESULT {:#010x})", static_cast<unsigned>(result));
  unavailable(missing ? "The Microsoft Edge WebView2 runtime is not installed." : "The Microsoft Edge WebView2 browser did not start.");
}

void WebPageReader::Impl::unavailable(std::string why) {
  std::lock_guard lock(mutex);
  state = State::Unavailable;
  reason = std::move(why);
  for (const Request& request : queue) answers.push_back({request.id, false, {}, reason});
  queue.clear();
  busy = false;
}

void WebPageReader::Impl::navigate(const std::string& url) {
  if (steam) steam->load(url);
  else if (FAILED(view->Navigate(widen(url).c_str()))) finish(false, {}, "The page address is not valid.");
}

void WebPageReader::Impl::attach(ICoreWebView2Controller* made) {
  controller = made;
  controller->get_CoreWebView2(&view);
  if (!view) {
    browser_failed(E_FAIL);
    return;
  }
  controller->put_IsVisible(FALSE);
  RECT bounds;
  GetClientRect(hwnd, &bounds);
  controller->put_Bounds(bounds);
  ComPtr<ICoreWebView2_8> muted;
  if (SUCCEEDED(view.As(&muted))) muted->put_IsMuted(TRUE);
  ComPtr<ICoreWebView2Settings> settings;
  if (SUCCEEDED(view->get_Settings(&settings))) {
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_AreDefaultScriptDialogsEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
  }
  EventRegistrationToken token{};
  view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*) -> HRESULT {
                                 page_loaded = false;
                                 return S_OK;
                               }).Get(),
                               &token);
  view->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>([this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
        page_completed(args);
        return S_OK;
      }).Get(),
      &token);
  // Pages never open windows of their own.
  view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                 args->put_Handled(TRUE);
                                 return S_OK;
                               }).Get(),
                               &token);
  {
    std::lock_guard lock(mutex);
    state = State::Ready;
  }
  log::info("web page: browser ready (WebView2 {})", WebPageReader::runtime_version());
  next_request();
}

void WebPageReader::Impl::page_completed(ICoreWebView2NavigationCompletedEventArgs* args) {
  if (!current) return;
  LPWSTR source = nullptr;
  if (SUCCEEDED(view->get_Source(&source)) && source) {
    const bool blank = std::wstring_view(source) == L"about:blank";
    CoTaskMemFree(source);
    if (blank) return;
  }
  BOOL success = FALSE;
  args->get_IsSuccess(&success);
  int http_status = 0;
  ComPtr<ICoreWebView2NavigationCompletedEventArgs2> with_status;
  if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&with_status)))) with_status->get_HttpStatusCode(&http_status);
  // An error page with an HTTP status is still a page (a site's check page answers 403); only no answer at all fails at once.
  if (!success && http_status == 0) {
    COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
    args->get_WebErrorStatus(&status);
    if (status == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) return;
    log::warn("web page: {} failed to load (status {})", current->url, static_cast<int>(status));
    finish(false, {}, "The page could not be reached (no internet connection?).");
    return;
  }
  page_loaded = true;
  poll();
}

void WebPageReader::Impl::next_request() {
  if (current || !(view || (steam && steam->ready()))) return;
  {
    std::lock_guard lock(mutex);
    if (queue.empty()) {
      busy = false;
      return;
    }
    current = std::move(queue.front());
    queue.pop_front();
    busy = true;
  }
  page_loaded = script_running = false;
  verify_since.reset();
  deadline = Clock::now() + kPageTimeout;
  SetTimer(hwnd, kPollTimer, kPollMs, nullptr);
  navigate(current->url);
}

void WebPageReader::Impl::poll() {
  if (!current) return;
  const Clock::time_point now = Clock::now();
  if (now > deadline) {
    finish(false, {}, shown                   ? "The site check was not finished in time."
                      : steam && verify_since ? "The site asked for a check that Steam's browser cannot show."
                                              : "The page took too long to answer.");
    return;
  }
  if (steam && script_running && steam->stalled(kScriptLost)) script_running = false;
  if (!page_loaded || script_running) return;
  script_running = true;
  if (steam) {
    steam->evaluate(current->script);
    return;
  }
  const std::uint64_t id = current->id;
  const HRESULT queued = view->ExecuteScript(widen(current->script).c_str(),
                                             Callback<ICoreWebView2ExecuteScriptCompletedHandler>([this, id](HRESULT result, LPCWSTR value) -> HRESULT {
                                               script_answered(id, result, value);
                                               return S_OK;
                                             }).Get());
  if (FAILED(queued)) script_running = false;
}

void WebPageReader::Impl::script_answered(std::uint64_t id, HRESULT result, const wchar_t* value) {
  script_running = false;
  if (!current || current->id != id || FAILED(result)) return;
  script_value(narrow(value));
}

void WebPageReader::Impl::script_value(std::string json) {
  if (json.empty() || json == "null") return;
  if (json == "\"verify\"") {
    const Clock::time_point now = Clock::now();
    if (!verify_since) verify_since = now;
    if (!steam && !shown && now - *verify_since >= kVerifyGrace) {
      show(true);
      deadline = now + kVerifyTimeout;
    }
    return;
  }
  finish(true, std::move(json), {});
}

void WebPageReader::Impl::finish(bool ok, std::string json, std::string error) {
  if (!current) return;
  KillTimer(hwnd, kPollTimer);
  answer({current->id, ok, std::move(json), std::move(error)});
  current.reset();
  if (shown) show(false);
  bool more = false;
  {
    std::lock_guard lock(mutex);
    more = !queue.empty();
  }
  if (more) next_request();
  else {
    {
      std::lock_guard lock(mutex);
      busy = false;
    }
    navigate("about:blank");
  }
}

void WebPageReader::Impl::show(bool on) {
  shown = on;
  if (on) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    SetWindowPos(hwnd, HWND_TOP, work.left + (work.right - work.left - kWindowW) / 2, work.top + (work.bottom - work.top - kWindowH) / 2,
                 kWindowW, kWindowH, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
  } else {
    ShowWindow(hwnd, SW_HIDE);
  }
  controller->put_IsVisible(on ? TRUE : FALSE);
  std::lock_guard lock(mutex);
  verifying = on;
}

void WebPageReader::Impl::answer(Answer reply) {
  std::lock_guard lock(mutex);
  answers.push_back(std::move(reply));
}

LRESULT CALLBACK WebPageReader::Impl::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!impl) return DefWindowProcW(window, message, wparam, lparam);
  switch (message) {
    case WM_SIZE:
      if (impl->controller) {
        RECT bounds;
        GetClientRect(window, &bounds);
        impl->controller->put_Bounds(bounds);
      }
      return 0;
    // The player closing the window gives up the request; the window itself stays for the next one.
    case WM_CLOSE:
      if (impl->current) impl->finish(false, {}, "The site check window was closed.");
      else if (impl->shown) impl->show(false);
      return 0;
    case WM_TIMER:
      if (wparam == kPollTimer) impl->poll();
      else if (wparam == kPumpTimer) impl->pump_steam();
      return 0;
    case kWakeMessage: impl->next_request(); return 0;
    case kQuitMessage: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window, message, wparam, lparam);
  }
}

}  // namespace oc::platform
