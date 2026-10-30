// Update tab (platform/updater.h): version tags, the GitHub releases/latest JSON, the asset pick. No network unless
// `test_updater --live`: one real check against GitHub, printed (not run by ctest).
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#include "core/version.h"
#include "platform/updater.h"

using namespace oc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

int cmp(const char* lhs, const char* rhs) { return platform::compare_versions(*platform::parse_version(lhs), *platform::parse_version(rhs)); }

// Trimmed from a real api.github.com answer: nested author html_url, escapes, null body, two assets.
const char* kJson = R"({
  "url": "https://api.github.com/repos/sp-hy/OpenCross/releases/1",
  "html_url": "https://github.com/sp-hy/OpenCross/releases/tag/v0.2.0",
  "id": 1,
  "author": {"login": "sp-hy", "html_url": "https://github.com/sp-hy", "site_admin": false},
  "tag_name": "v0.2.0",
  "name": "OpenCross \"0.2\" é",
  "draft": false, "prerelease": false,
  "published_at": "2026-10-20T10:00:00Z",
  "assets": [
    {"name": "README.txt", "size": 12, "digest": null, "browser_download_url": "https://github.com/x/README.txt", "uploader": {"login": "a"}},
    {"name": "OpenCross-0.2.0.zip", "size": 4096, "digest": "sha256:ABCDEF", "browser_download_url": "https://github.com/x/OpenCross-0.2.0.zip"}
  ],
  "body": null, "reactions": {"+1": 3, "list": [1, 2, [3]]}
})";
}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--live") {
    platform::Updater updater;
    updater.check();
    while (updater.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const platform::Updater::Status st = updater.status();
    std::printf("current v%s, state %d, latest '%s' (%s) %s, message '%s'\n", OC_VERSION_STRING, static_cast<int>(st.state), st.latest.c_str(),
                st.published.c_str(), st.html_url.c_str(), st.message.c_str());
    return 0;
  }
  CHECK(platform::parse_version(OC_VERSION_STRING).has_value());
  CHECK(!platform::parse_version("latest").has_value());
  CHECK(platform::parse_version("v1.2.3-beta")->size() == 3);
  CHECK(cmp("v0.1.0", "0.0.9") == 1);
  CHECK(cmp("v1.2", "1.2.0") == 0);
  CHECK(cmp("1.10.0", "1.9.9") == 1);
  CHECK(cmp("v0.0.1", "v0.0.2") == -1);

  platform::ReleaseInfo release;
  CHECK(platform::parse_release_json(kJson, release));
  CHECK(release.tag == "v0.2.0");
  CHECK(release.html_url == "https://github.com/sp-hy/OpenCross/releases/tag/v0.2.0");
  CHECK(release.name == "OpenCross \"0.2\" \xC3\xA9");
  CHECK(release.published == "2026-10-20");
  CHECK(release.assets.size() == 2);
  if (release.assets.size() == 2) {
    CHECK(release.assets[1].size == 4096);
    CHECK(release.assets[1].sha256 == "abcdef");
    CHECK(release.assets[0].sha256.empty());
  }
  const platform::ReleaseAsset* asset = platform::pick_asset(release);
  CHECK(asset && asset->name == "OpenCross-0.2.0.zip");
  release.assets.push_back({"opencross.exe", "https://github.com/x/opencross.exe", "", 10});
  asset = platform::pick_asset(release);
  CHECK(asset && asset->name == "opencross.exe");
  CHECK(!platform::pick_launcher_proxy(release));
  release.assets.push_back({"xinput9_1_0.dll", "https://github.com/x/xinput9_1_0.dll", "abc", 20});
  const platform::ReleaseAsset* proxy = platform::pick_launcher_proxy(release);
  CHECK(proxy && proxy->sha256 == "abc");
  // The proxy never counts as the exe to install.
  asset = platform::pick_asset(release);
  CHECK(asset && asset->name == "opencross.exe");

  CHECK(!platform::parse_release_json(R"({"message": "Not Found"})", release));
  // Truncated JSON is rejected.
  CHECK(!platform::parse_release_json("{\"tag_name\": \"v1\"", release));

  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("updater: all checks passed\n");
  return failures ? 1 : 0;
}
