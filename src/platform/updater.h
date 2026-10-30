#pragma once
// Self-update from the GitHub releases of sp-hy/OpenCross (settings tab "Update").
//   check():    GET api.github.com/repos/sp-hy/OpenCross/releases/latest (drafts and pre-releases are never "latest"); the tag
//               "v1.2.3" is compared with core/version.h.
//   download(): the release asset "opencross.exe" (or a .zip holding opencross.exe, root or one folder deep) -> <exe>.new next to
//               the running exe; checked: PE header, the asset size, and its SHA-256 when GitHub lists a digest.
//               Then the swap, no helper exe: a running exe can be RENAMED on Windows (not overwritten), so
//               <exe> -> <exe>.old, <exe>.new -> <exe>. The next launch runs the new version; restart() starts it now.
//   The launcher proxy (asset XINPUT9_1_0.dll, the OpenCross button in RDE's own launcher) goes with it, by the same swap, next to the
//   exe when RDE's launcher is there. check() also offers it alone when the installed copy is missing or differs from the release's
//   digest, which is how an install that arrived as a bare opencross.exe gets it.
//   cleanup_after_update(): at boot, deletes a left-over <exe>.old / <exe>.new (retries a few seconds while the old process exits).
// All network work runs on one worker thread; the UI polls state().
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace oc::platform {

inline constexpr const char* kReleasesUrl = "https://github.com/sp-hy/OpenCross/releases";
inline constexpr const char* kNexusUrl = "https://www.nexusmods.com/chronocrosstheradicaldreamersedition/mods/97";
inline constexpr const char* kNexusBugsUrl = "https://www.nexusmods.com/chronocrosstheradicaldreamersedition/mods/97?tab=bugs";
inline constexpr const char* kLauncherProxyName = "XINPUT9_1_0.dll";

// "v1.2.3" / "1.2" / "1.2.3.4" -> numbers (missing parts 0); nullopt when it does not start with a number.
std::optional<std::vector<int>> parse_version(std::string_view text);
// -1 / 0 / 1 like strcmp, padding the shorter one with zeros.
int compare_versions(const std::vector<int>& lhs, const std::vector<int>& rhs);

// One release asset: `url` = browser_download_url, `sha256` = lower-case hex of the "digest": "sha256:..." field (empty when GitHub
// lists none).
struct ReleaseAsset {
  std::string name;
  std::string url;
  std::string sha256;
  uint64_t size = 0;
};
// The releases/latest answer: `tag` = tag_name, `name` = the release title, `html_url` = the release page, `published` = published_at
// cut to "YYYY-MM-DD".
struct ReleaseInfo {
  std::string tag;
  std::string name;
  std::string html_url;
  std::string published;
  std::vector<ReleaseAsset> assets;
};
// Parses the releases/latest JSON; false when it has no tag_name.
bool parse_release_json(std::string_view json, ReleaseInfo& out);
// The asset to install: "opencross.exe", else a .zip whose name starts with "opencross" (case-insensitive), else any single .exe / .zip.
const ReleaseAsset* pick_asset(const ReleaseInfo& release);
// The launcher proxy asset (case-insensitive name); null when the release has none.
const ReleaseAsset* pick_launcher_proxy(const ReleaseInfo& release);

// States: check() takes Idle / UpToDate / Available / Failed / Installed to Checking, download() takes Available to Downloading and
// then Installed or Failed. Status: `latest` = the tag of the newest release (Available / UpToDate / Installed), `html_url` its page,
// `message` = what went wrong when Failed, `progress` = 0..1 while Downloading, `launcher_only` = the exe is current and Available /
// Installed is about the launcher proxy alone. The destructor cancels and joins the worker.
class Updater {
public:
  enum class State { Idle, Checking, UpToDate, Available, Downloading, Installed, Failed };
  struct Status {
    State state = State::Idle;
    std::string latest;
    std::string html_url;
    std::string published;
    std::string message;
    float progress = 0.0f;
    bool launcher_only = false;
  };

  Updater();
  ~Updater();
  Updater(const Updater&) = delete;
  Updater& operator=(const Updater&) = delete;

  void check();
  void download();
  Status status() const;
  bool busy() const;

  // Starts the (new) exe with the same command line; call once the game has shut down. False when CreateProcess failed.
  static bool restart();
  static void cleanup_after_update();

private:
  void run(bool install);
  void set_failed(std::string msg);
  // One release asset into `body`, checked against its listed size and digest; the error text, empty when it worked.
  std::string fetch(const ReleaseAsset& asset, std::string& body);
  std::string install_launcher_proxy(const ReleaseAsset& asset);

  mutable std::mutex mu_;
  Status st_;
  std::optional<ReleaseAsset> asset_;
  std::optional<ReleaseAsset> proxy_asset_;
  // The tag swapped in this session (a re-check of the same release stays Installed).
  std::string installed_;
  std::thread worker_;
  std::atomic<bool> cancel_{false};
};

}  // namespace oc::platform
