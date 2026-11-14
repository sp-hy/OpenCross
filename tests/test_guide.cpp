// net/guide.h: the guide overlay's page answers (list, page, error), the merge of a guide's pages, the cache round trip and its format
// check, and cache names. Offline: canned JSON only, nothing is fetched.
#include <cstdio>
#include <string>

#include "net/guide.h"

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
}  // namespace

int main() {
  {
    net::GuideList list;
    CHECK(net::parse_guide_list(R"({"kind":"list","entries":[
        {"title":"Guide and Walkthrough","author":"Someone","group":"Chrono Cross","category":"Full Game Guides",
         "url":"https://example.invalid/faqs/48347","updated":"10/03/2016","meta":"v.1, 770KB"},
        {"title":"","url":"https://example.invalid/faqs/1"},
        {"title":"No address"}]})",
                                list));
    CHECK(list.entries.size() == 1 && list.entries[0].author == "Someone" && list.entries[0].group == "Chrono Cross");
    net::GuideList again;
    CHECK(net::parse_guide_list(net::guide_list_to_json(list), again) && again.entries.size() == 1);
    CHECK(again.entries[0].category == "Full Game Guides" && again.entries[0].meta == "v.1, 770KB" && again.entries[0].updated == "10/03/2016");
    net::GuideList cached;
    CHECK(net::parse_cached_guide_list(net::guide_list_to_json(list), cached) && cached.entries.size() == 1);
    CHECK(!net::parse_cached_guide_list(R"({"kind":"list","entries":[{"title":"T","url":"u"}]})", cached));
    CHECK(!net::parse_cached_guide_list(R"({"kind":"list","format":0,"entries":[{"title":"T","url":"u"}]})", cached));
    CHECK(!net::parse_guide_list(R"({"kind":"list","entries":[]})", again));
    CHECK(!net::parse_guide_list(R"({"error":"The guide list was not found on the page."})", again));
    CHECK(!net::parse_guide_list("not json", again));
  }
  {
    CHECK(net::guide_answer_error(R"({"error":"This page holds no guide text."})") == "This page holds no guide text.");
    CHECK(net::guide_answer_error("[1,2]") == "The page format was not recognised.");
  }
  {
    net::GuidePage first, second;
    CHECK(net::parse_guide_page(R"({"kind":"page","title":"T","author":"A","updated":"Version: 2","index":0,"count":2,
        "pages":[{"name":"Page 1","url":"u"},{"name":"Page 2","url":"u?page=1"}],
        "blocks":[{"k":"h","l":2,"t":"Purpose","id":"section1"},{"k":"p","t":"Text"},{"k":"li","l":2,"t":"Item"},
                  {"k":"tr","c":["a","b"]},{"k":"hr"},{"k":"zz","t":"unknown kind"}]})",
                                first));
    CHECK(first.page_count == 2 && first.pages.size() == 2 && first.blocks.size() == 5);
    CHECK(first.blocks[0].kind == net::GuideBlockKind::Heading && first.blocks[0].level == 2 && first.blocks[0].anchor == "section1");
    CHECK(first.blocks[2].kind == net::GuideBlockKind::ListItem && first.blocks[2].level == 2);
    CHECK(first.blocks[3].kind == net::GuideBlockKind::TableRow && first.blocks[3].cells.size() == 2 && first.blocks[3].cells[1] == "b");
    CHECK(first.blocks[4].kind == net::GuideBlockKind::Rule);
    CHECK(net::parse_guide_page(R"({"kind":"page","title":"Other","index":1,"count":2,"blocks":[{"k":"pre","t":"  ASCII\n  art"}]})", second));
    net::GuidePage empty;
    CHECK(!net::parse_guide_page(R"({"kind":"page","blocks":[]})", empty));

    net::Guide guide;
    guide.url = "https://example.invalid/faqs/69678";
    net::add_guide_page(guide, first);
    net::add_guide_page(guide, second);
    CHECK(guide.title == "T" && guide.author == "A" && guide.pages.size() == 2 && guide.blocks.size() == 6);
    CHECK(guide.blocks[0].page == 0 && guide.blocks[5].page == 1 && guide.blocks[5].text == "  ASCII\n  art");

    net::Guide cached;
    CHECK(net::parse_guide(net::guide_to_json(guide), cached));
    CHECK(cached.url == guide.url && cached.title == "T" && cached.updated == "Version: 2" && cached.pages.size() == 2);
    CHECK(cached.blocks.size() == 6 && cached.blocks[5].page == 1 && cached.blocks[5].kind == net::GuideBlockKind::Preformatted);
    CHECK(cached.blocks[3].cells == guide.blocks[3].cells && cached.blocks[0].anchor == "section1");
    CHECK(!net::parse_guide(R"({"kind":"page","blocks":[{"k":"p","t":"x"}]})", cached));
    CHECK(!net::parse_guide(R"({"kind":"guide","format":0,"blocks":[{"k":"p","t":"x"}]})", cached));
    CHECK(!net::parse_guide(R"({"kind":"guide","blocks":[{"k":"p","t":"x"}]})", cached));
  }
  {
    CHECK(net::guide_cache_name("https://gamefaqs.gamespot.com/ps/196917-chrono-cross/faqs/48347") == "48347.json");
    CHECK(net::guide_cache_name("https://gamefaqs.gamespot.com/ps/196917-chrono-cross/faqs/79953/ng-plus") == "79953.json");
    const std::string hashed = net::guide_cache_name("https://example.invalid/somewhere");
    CHECK(hashed.size() == 21 && hashed.substr(16) == ".json" && hashed == net::guide_cache_name("https://example.invalid/somewhere"));
    CHECK(hashed != net::guide_cache_name("https://example.invalid/elsewhere"));
  }
  {
    CHECK(net::guide_list_script().find("'verify'") != std::string_view::npos);
    CHECK(net::guide_page_script().find("#faqwrap") != std::string_view::npos);
    CHECK(net::guide_html_script().find("outerHTML") != std::string_view::npos);
  }
  if (failures) std::printf("%d failure(s)\n", failures);
  else std::printf("guide: all checks passed\n");
  return failures ? 1 : 0;
}
