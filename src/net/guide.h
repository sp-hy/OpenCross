#pragma once
// The guide overlay's content: the GameFAQs guide list of the game and the guides themselves, read from the site's pages by scripts
// that run in the hidden browser (platform::WebPageReader) and answer in a small JSON schema of our own. The same schema, merged over
// all of a guide's pages, is the cache file of a guide.
//
// Page answers:
//   list:  {"kind":"list","entries":[{"title","author","group","category","url","updated","meta"}...]} (group: the game the guide
//          is filed under; meta: version, size and the site's recommendation)
//   guide: {"kind":"page","title","author","updated","index","count","pages":[{"name","url"}...],"blocks":[block...]}
//   error: {"error":"sentence"}; a script answers null until the page is ready and "verify" while the site checks the visitor.
// A block is {"k":kind,"l":level,"t":text,"id":anchor,"c":[cells]}: kind h (heading, level 1-6), p (paragraph), li (list item,
// level = depth), pre (preformatted text, kept as is), tr (table row: cells), hr (rule).
#include <string>
#include <string_view>
#include <vector>

namespace oc::net {

// The version of the scripts and of the cache schema. Cached files of another version are read again from the site once, so raise it
// whenever the scripts or the schema change; nothing else ever reads a cached guide or list again.
inline constexpr int kGuideFormat = 1;

inline constexpr std::string_view kGuideListUrl = "https://gamefaqs.gamespot.com/ps/196917-chrono-cross/faqs";

struct GuideEntry {
  std::string title, author, group, category, url, updated, meta;
};

struct GuideList {
  std::vector<GuideEntry> entries;
};

enum class GuideBlockKind { Heading, Paragraph, ListItem, Preformatted, TableRow, Rule };

struct GuideBlock {
  GuideBlockKind kind = GuideBlockKind::Paragraph;
  int level = 0;
  std::string text;
  std::string anchor;
  std::vector<std::string> cells;
  // The guide page the block came from (0-based).
  int page = 0;
};

struct GuidePageLink {
  std::string name, url;
};

// One page of a guide as the page script read it.
struct GuidePage {
  std::string title, author, updated;
  int page_index = 0, page_count = 1;
  std::vector<GuidePageLink> pages;
  std::vector<GuideBlock> blocks;
};

// A whole guide: every page's blocks in order.
struct Guide {
  std::string url, title, author, updated;
  std::vector<GuidePageLink> pages;
  std::vector<GuideBlock> blocks;
};

// The scripts for WebPageReader::load. html: the page's whole HTML as a string (ocguide's dump).
std::string_view guide_list_script();
std::string_view guide_page_script();
std::string_view guide_html_script();

// The list answer of the page script / the list's cache file (refused when it is of another kGuideFormat).
bool parse_guide_list(std::string_view json, GuideList& out);
bool parse_cached_guide_list(std::string_view json, GuideList& out);
std::string guide_list_to_json(const GuideList& list);
bool parse_guide_page(std::string_view json, GuidePage& out);
// The "error" sentence of an answer, else a general one.
std::string guide_answer_error(std::string_view json);

// Appends a page to the guide (the first page also sets its title, author and page list).
void add_guide_page(Guide& guide, GuidePage page);
// The cache file of a guide; parse_guide refuses one of another kGuideFormat.
std::string guide_to_json(const Guide& guide);
bool parse_guide(std::string_view json, Guide& out);

// The cache file name of a guide URL: its numeric id ("faqs/12345" -> "12345.json"), else a hash of the URL.
std::string guide_cache_name(std::string_view url);
const char* guide_block_kind_name(GuideBlockKind kind);

}  // namespace oc::net
