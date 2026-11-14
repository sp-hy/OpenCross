// `opencross ocguide`: the guide overlay's page reading without the game. `html <url> <file>` saves the page as the hidden browser
// sees it; `list` prints the guide list; `guide <url>` prints a guide's pages, sections and blocks. The browser profile is in the
// `guides` folder of the user's local data folder, as in the game. `identity` prints what any page can read about that browser (user agent, client-hint brands,
// languages, the WebView2 host object), from a local data: page: it makes no request. The other modes contact GameFAQs: run them
// rarely, one page at a time.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "net/guide.h"
#include "net/json.h"
#include "platform/paths.h"
#include "platform/web_page.h"

namespace {

using oc::platform::WebPageReader;

int usage() {
  std::fprintf(stderr,
               "usage: opencross ocguide html <url> <file>   save the page's HTML as the browser sees it\n"
               "       opencross ocguide list                print the guide list\n"
               "       opencross ocguide guide <url>         print a guide's pages, sections and blocks\n"
               "       opencross ocguide identity            print what a page can read about the browser (no request)\n");
  return 2;
}

// Loads one page with `script` and returns its answer; waits up to four minutes (a site check may need the player).
WebPageReader::Answer read_page(WebPageReader& reader, const std::string& url, const std::string& script) {
  const std::uint64_t id = reader.load(url, script);
  bool told = false;
  for (int i = 0; i < 4 * 60 * 20; ++i) {
    for (WebPageReader::Answer& answer : reader.take_answers())
      if (answer.id == id) return answer;
    if (reader.verifying() && !told) {
      std::printf("the site asks for a check: finish it in the browser window\n");
      told = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return {id, false, {}, "no answer"};
}

void print_guide(const oc::net::GuidePage& page) {
  std::printf("title: %s\nauthor: %s\nupdated: %s\npage %d of %d\n", page.title.c_str(), page.author.c_str(), page.updated.c_str(),
              page.page_index + 1, page.page_count);
  for (const oc::net::GuideBlock& block : page.blocks) {
    const std::string_view text = block.text;
    std::printf("  [%s%s] %.*s%s\n", oc::net::guide_block_kind_name(block.kind), block.level ? std::to_string(block.level).c_str() : "",
                static_cast<int>(text.size() > 90 ? 90 : text.size()), text.data(), text.size() > 90 ? "..." : "");
  }
}

// What a page sees of the browser, read on a local page.
constexpr const char* kIdentityPage = "data:text/html,<p></p>";
constexpr const char* kIdentityScript = R"JS((() => {
if (document.readyState !== 'complete') return null;
const hints = navigator.userAgentData;
return {userAgent: navigator.userAgent, brands: hints ? hints.brands.map(brand => brand.brand + ' ' + brand.version) : [],
        platform: hints ? hints.platform : navigator.platform, languages: navigator.languages, webdriver: navigator.webdriver,
        hostObject: !!(window.chrome && window.chrome.webview), visibility: document.visibilityState};
})())JS";

}  // namespace

int ocguide_main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string_view mode = argv[1];
  if (!((mode == "html" && argc == 4) || (mode == "list" && argc == 2) || (mode == "guide" && argc == 3) || (mode == "identity" && argc == 2))) return usage();
  if (!WebPageReader::available()) {
    std::printf("neither the Microsoft Edge WebView2 runtime nor Steam is available\n");
    return 1;
  }
  WebPageReader reader;
  reader.start(oc::platform::local_data_dir() / "guides" / "browser");
  const std::string url = mode == "list"       ? std::string(oc::net::kGuideListUrl)
                          : mode == "identity" ? std::string(kIdentityPage)
                                               : std::string(argv[2]);
  const std::string script = mode == "html"       ? std::string(oc::net::guide_html_script())
                             : mode == "list"     ? std::string(oc::net::guide_list_script())
                             : mode == "identity" ? std::string(kIdentityScript)
                                                  : std::string(oc::net::guide_page_script());
  const WebPageReader::Answer answer = read_page(reader, url, script);
  reader.stop();
  if (!answer.ok) {
    std::printf("failed: %s\n", answer.error.c_str());
    return 1;
  }
  if (mode == "identity") {
    std::printf("%s\n", answer.json.c_str());
    return 0;
  }
  if (mode == "html") {
    const std::optional<oc::net::Json> value = oc::net::parse_json(answer.json);
    if (!value || value->type() != oc::net::Json::Type::String) {
      std::printf("failed: the page gave no HTML\n");
      return 1;
    }
    std::ofstream(argv[3], std::ios::binary) << value->as_string();
    std::printf("%zu bytes written to %s\n", value->as_string().size(), argv[3]);
    return 0;
  }
  if (mode == "list") {
    oc::net::GuideList list;
    if (!oc::net::parse_guide_list(answer.json, list)) {
      std::printf("failed: %s\n", oc::net::guide_answer_error(answer.json).c_str());
      return 1;
    }
    for (const oc::net::GuideEntry& entry : list.entries)
      std::printf("%-12s %-22s %-36s %-18s %-24s %s\n", entry.group.substr(0, 12).c_str(), entry.category.c_str(), entry.title.c_str(),
                  entry.author.c_str(), entry.meta.c_str(), entry.url.c_str());
    std::printf("%zu guides\n", list.entries.size());
    return 0;
  }
  oc::net::GuidePage page;
  if (!oc::net::parse_guide_page(answer.json, page)) {
    std::printf("failed: %s\n", oc::net::guide_answer_error(answer.json).c_str());
    return 1;
  }
  print_guide(page);
  return 0;
}
