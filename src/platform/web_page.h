#pragma once
// A hidden Edge browser (WebView2) that loads web pages and reads them with a script, for content a plain HTTP client cannot fetch.
// Without the WebView2 runtime (Proton) the page is opened in the running Steam client's own browser instead (SteamBrowser).
//
// One worker thread owns the browser: its own hidden window, the WebView2 environment (profile folder given to start()), muted, and a
// queue of requests answered in order. A request names a URL and a script expression. After the page has loaded, the expression is
// evaluated in it every 250 ms until it gives something other than null; that value comes back as JSON text. The string "verify"
// means the page wants the player (a site's "verify you are human" check): the browser window is shown, and hidden again when the
// page answers. A request fails when the page cannot be reached, when it times out (30 s, 3 minutes while the window is shown) or
// when the player closes the window. After each answer the browser goes to about:blank so the page stops running.
// The main thread only queues requests and collects answers; it never waits on the browser.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace oc::platform {

class WebPageReader {
 public:
  enum class State { Off, Starting, Ready, Unavailable };
  struct Answer {
    std::uint64_t id = 0;
    bool ok = false;
    // ok: the script's value as JSON text; else a sentence for the player.
    std::string json;
    std::string error;
  };

  WebPageReader();
  ~WebPageReader();
  WebPageReader(const WebPageReader&) = delete;
  WebPageReader& operator=(const WebPageReader&) = delete;

  // Starts the worker (no-op while running). The browser keeps its cookies and cache in `profile_dir`.
  void start(const std::filesystem::path& profile_dir);
  // Ends the worker and the browser; queued requests are dropped.
  void stop();
  State state() const;
  // Why the state is Unavailable ("The WebView2 runtime is not installed").
  std::string unavailable_reason() const;
  // Queues a request; the answer carries the returned id. Requests made before the browser is ready wait for it.
  std::uint64_t load(std::string url, std::string script);
  // The answers that arrived since the last call.
  std::vector<Answer> take_answers();
  // True while the browser window is shown for the player.
  bool verifying() const;
  // No request queued or running.
  bool idle() const;

  // A browser can start: the WebView2 runtime is installed or Steam is running.
  static bool available();
  // The installed WebView2 runtime's version; empty when there is none.
  static std::string runtime_version();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace oc::platform
