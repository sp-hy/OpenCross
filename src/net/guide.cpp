#include "net/guide.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <utility>

#include "net/json.h"

namespace oc::net {

namespace {

// Every script starts the same way: null until the document has loaded, "verify" on a visitor check page; `clean` folds white space.
constexpr std::string_view kReady = R"JS(
if (document.readyState !== 'complete') return null;
if (/^just a moment/i.test(document.title) || document.querySelector('#challenge-form, #challenge-running, .cf-turnstile')) return 'verify';
const clean = text => (text || '').replace(/\s+/g, ' ').trim();
)JS";

constexpr std::string_view kHtmlBody = "return document.documentElement.outerHTML;";

// The FAQ list: the game headings (h2 "<game> Guides"), the category headings (h3) and one entry per text guide (maps and charts
// that are pictures are left out).
constexpr std::string_view kListBody = R"JS(
const entries = [];
let group = '', category = '';
for (const el of document.querySelectorAll('h2, .head h3.title, ol.guides > li')) {
  if (el.tagName === 'H2') { group = clean(el.textContent).replace(/ Guides$/, ''); continue; }
  if (el.tagName === 'H3') { category = clean(el.textContent); continue; }
  const link = el.querySelector('.float_l a.bold');
  if (!link || !/\/faqs\/\d+/.test(link.getAttribute('href') || '')) continue;
  const by = el.querySelector('.float_l span a');
  const facts = el.querySelector('.meta.float_r');
  const date = facts && facts.querySelector('.guide_date');
  const note = el.querySelector('.meta.float_l');
  const meta = clean(facts ? facts.textContent : '').replace(/,\s*\d{4}$/, '');
  entries.push({title: clean(link.textContent), author: by ? clean(by.textContent) : '', group, category, url: link.href,
                updated: date ? (date.getAttribute('title') || clean(date.textContent)) : '',
                meta: note ? meta + ', ' + clean(note.textContent).replace(/\*/g, '') : meta});
}
if (!entries.length) return {error: 'The guide list was not found on the page.'};
return {kind: 'list', entries};
)JS";

// One guide page: the header (title, author, version line), the page list ("Page N of M" pages, or the named pages of the table of
// contents) and the body. A text guide is one preformatted block (the site splits it over several <pre> elements); an HTML guide
// is walked in order: headings, paragraphs (inline runs between blocks too), list items with their depth, preformatted text, table
// rows, rules and pictures as "[Image: alt]".
constexpr std::string_view kPageBody = R"JS(
const wrap = document.querySelector('#faqwrap');
if (!wrap) return {error: 'This page holds no guide text.'};
const blocks = [];
const text = wrap.querySelector('#faqtext');
if (text) {
  let all = '';
  for (const pre of text.querySelectorAll('pre')) all += pre.textContent;
  blocks.push({k: 'pre', t: all.replace(/\r\n?/g, '\n')});
} else {
  const inline = new Set(['A', 'ABBR', 'B', 'BR', 'CODE', 'EM', 'FONT', 'I', 'KBD', 'MARK', 'S', 'SMALL', 'SPAN', 'STRONG', 'SUB', 'SUP', 'U']);
  const skip = new Set(['BUTTON', 'FORM', 'IFRAME', 'INPUT', 'NOSCRIPT', 'SCRIPT', 'SELECT', 'STYLE', 'SVG']);
  let run = '';
  const flush = () => {
    const paragraph = run.replace(/[ \t\xa0]+/g, ' ').replace(/ *\n */g, '\n').trim();
    if (paragraph) blocks.push({k: 'p', t: paragraph});
    run = '';
  };
  const inlineText = node => {
    if (node.nodeType === Node.TEXT_NODE) return node.nodeValue.replace(/\s+/g, ' ');
    if (node.nodeType !== Node.ELEMENT_NODE || skip.has(node.tagName)) return '';
    if (node.tagName === 'BR') return '\n';
    let out = '';
    for (const child of node.childNodes) out += inlineText(child);
    return out;
  };
  const list = (el, depth) => {
    for (const item of el.children) {
      if (item.tagName === 'UL' || item.tagName === 'OL') { list(item, depth + 1); continue; }
      if (item.tagName !== 'LI' || item.querySelector(':scope > a[onclick]')) continue;
      let own = '';
      for (const child of item.childNodes)
        if (!(child.nodeType === Node.ELEMENT_NODE && (child.tagName === 'UL' || child.tagName === 'OL'))) own += inlineText(child);
      own = clean(own);
      if (own) blocks.push({k: 'li', l: depth, t: own});
      for (const child of item.children) if (child.tagName === 'UL' || child.tagName === 'OL') list(child, depth + 1);
    }
  };
  const walk = node => {
    for (const child of node.childNodes) {
      if (child.nodeType === Node.TEXT_NODE) { run += child.nodeValue.replace(/\s+/g, ' '); continue; }
      if (child.nodeType !== Node.ELEMENT_NODE || skip.has(child.tagName) || child.classList.contains('ftoc')) continue;
      const tag = child.tagName;
      if (inline.has(tag)) { run += inlineText(child); continue; }
      flush();
      if (/^H[1-6]$/.test(tag)) {
        const anchor = child.querySelector('a[name]');
        const title = clean(child.textContent);
        if (title) blocks.push({k: 'h', l: +tag[1], t: title, id: child.id || (anchor ? anchor.getAttribute('name') : '')});
      } else if (tag === 'P') {
        run = inlineText(child);
        flush();
      } else if (tag === 'PRE') {
        blocks.push({k: 'pre', t: child.textContent.replace(/\r\n?/g, '\n')});
      } else if (tag === 'UL' || tag === 'OL') {
        list(child, 1);
      } else if (tag === 'TABLE') {
        for (const row of child.rows) blocks.push({k: 'tr', c: Array.from(row.cells, cell => clean(cell.textContent))});
      } else if (tag === 'HR') {
        blocks.push({k: 'hr'});
      } else if (tag === 'IMG') {
        blocks.push({k: 'p', t: '[Image' + (child.alt ? ': ' + clean(child.alt) : '') + ']'});
      } else {
        walk(child);
        flush();
      }
    }
  };
  walk(wrap);
  flush();
}
if (!blocks.length) return {error: 'This page holds no guide text.'};
const head = document.querySelector('h2.title');
const by = head && head.querySelector('a');
const facts = head && head.parentElement ? Array.from(head.parentElement.querySelectorAll('p')).find(para => /Updated/.test(para.textContent)) : null;
const base = location.origin + location.pathname.replace(/\/$/, '');
const root = (base.match(/^.*\/faqs\/\d+/) || [base])[0];
let index = 0, count = 1;
const pages = [];
const paging = document.querySelector('ul.paginate');
const numbered = paging && paging.textContent.match(/Page (\d+) of (\d+)/);
if (numbered) {
  index = +numbered[1] - 1;
  count = +numbered[2];
  for (let i = 0; i < count; ++i) pages.push({name: 'Page ' + (i + 1), url: root + (i ? '?page=' + i : '')});
} else {
  const toc = wrap.querySelector('.ftoc > ol');
  const links = toc ? Array.from(toc.children).filter(li => li.tagName === 'LI').map(li => li.querySelector(':scope > a'))
                          .filter(link => link && link.getAttribute('href') && !/[#?\/:]/.test(link.getAttribute('href'))) : [];
  if (links.length > 1) {
    for (const link of links) pages.push({name: clean(link.textContent), url: root + '/' + link.getAttribute('href')});
    index = Math.max(0, pages.findIndex(page => page.url === base));
    count = pages.length;
  }
}
return {kind: 'page', title: clean((document.querySelector('h1.page-title') || document).textContent || document.title),
        author: by ? clean(by.textContent) : '', updated: facts ? clean(facts.textContent) : '', index, count, pages, blocks};
)JS";

std::string wrap_script(std::string_view body) { return "(() => {" + std::string(kReady) + std::string(body) + "})()"; }

constexpr const char* kKindNames[] = {"h", "p", "li", "pre", "tr", "hr"};

std::optional<GuideBlockKind> kind_from_name(std::string_view name) {
  for (size_t i = 0; i < std::size(kKindNames); ++i)
    if (name == kKindNames[i]) return static_cast<GuideBlockKind>(i);
  return std::nullopt;
}

void read_blocks(const Json& array, std::vector<GuideBlock>& out, int page) {
  for (const Json& item : array.items()) {
    const std::optional<GuideBlockKind> kind = kind_from_name(item.string_of("k"));
    if (!kind) continue;
    GuideBlock block;
    block.kind = *kind;
    block.level = static_cast<int>(item.int_of("l"));
    block.text = item.string_of("t");
    block.anchor = item.string_of("id");
    for (const Json& cell : item["c"].items()) block.cells.push_back(cell.as_string());
    block.page = item["pg"].is_null() ? page : static_cast<int>(item.int_of("pg"));
    out.push_back(std::move(block));
  }
}

Json write_blocks(const std::vector<GuideBlock>& blocks) {
  Json array = Json::array();
  for (const GuideBlock& block : blocks) {
    Json item = Json::object();
    item.set("k", kKindNames[static_cast<int>(block.kind)]);
    if (block.level) item.set("l", block.level);
    if (!block.text.empty()) item.set("t", block.text);
    if (!block.anchor.empty()) item.set("id", block.anchor);
    if (!block.cells.empty()) {
      Json cells = Json::array();
      for (const std::string& cell : block.cells) cells.push(cell);
      item.set("c", std::move(cells));
    }
    if (block.page) item.set("pg", block.page);
    array.push(std::move(item));
  }
  return array;
}

void read_pages(const Json& array, std::vector<GuidePageLink>& out) {
  for (const Json& item : array.items()) out.push_back({item.string_of("name"), item.string_of("url")});
}

Json write_pages(const std::vector<GuidePageLink>& pages) {
  Json array = Json::array();
  for (const GuidePageLink& link : pages) {
    Json item = Json::object();
    item.set("name", link.name);
    item.set("url", link.url);
    array.push(std::move(item));
  }
  return array;
}

}  // namespace

std::string_view guide_list_script() {
  static const std::string script = wrap_script(kListBody);
  return script;
}

std::string_view guide_page_script() {
  static const std::string script = wrap_script(kPageBody);
  return script;
}

std::string_view guide_html_script() {
  static const std::string script = wrap_script(kHtmlBody);
  return script;
}

bool parse_guide_list(std::string_view json, GuideList& out) {
  const std::optional<Json> value = parse_json(json);
  if (!value || value->string_of("kind") != "list") return false;
  out.entries.clear();
  for (const Json& item : (*value)["entries"].items()) {
    GuideEntry entry{item.string_of("title"), item.string_of("author"),  item.string_of("group"), item.string_of("category"),
                     item.string_of("url"),   item.string_of("updated"), item.string_of("meta")};
    if (!entry.title.empty() && !entry.url.empty()) out.entries.push_back(std::move(entry));
  }
  return !out.entries.empty();
}

bool parse_cached_guide_list(std::string_view json, GuideList& out) {
  const std::optional<Json> value = parse_json(json);
  return value && value->int_of("format") == kGuideFormat && parse_guide_list(json, out);
}

std::string guide_list_to_json(const GuideList& list) {
  Json root = Json::object();
  root.set("kind", "list");
  root.set("format", kGuideFormat);
  Json entries = Json::array();
  for (const GuideEntry& entry : list.entries) {
    Json item = Json::object();
    item.set("title", entry.title);
    item.set("author", entry.author);
    item.set("group", entry.group);
    item.set("category", entry.category);
    item.set("url", entry.url);
    item.set("updated", entry.updated);
    item.set("meta", entry.meta);
    entries.push(std::move(item));
  }
  root.set("entries", std::move(entries));
  return to_json(root);
}

bool parse_guide_page(std::string_view json, GuidePage& out) {
  const std::optional<Json> value = parse_json(json);
  if (!value || value->string_of("kind") != "page") return false;
  out = {};
  out.title = value->string_of("title");
  out.author = value->string_of("author");
  out.updated = value->string_of("updated");
  out.page_index = static_cast<int>(value->int_of("index"));
  out.page_count = std::max(1, static_cast<int>(value->int_of("count", 1)));
  read_pages((*value)["pages"], out.pages);
  read_blocks((*value)["blocks"], out.blocks, out.page_index);
  return !out.blocks.empty();
}

std::string guide_answer_error(std::string_view json) {
  const std::optional<Json> value = parse_json(json);
  if (value && !value->string_of("error").empty()) return value->string_of("error");
  return "The page format was not recognised.";
}

void add_guide_page(Guide& guide, GuidePage page) {
  if (guide.blocks.empty()) {
    guide.title = std::move(page.title);
    guide.author = std::move(page.author);
    guide.updated = std::move(page.updated);
    guide.pages = std::move(page.pages);
  }
  for (GuideBlock& block : page.blocks) {
    block.page = page.page_index;
    guide.blocks.push_back(std::move(block));
  }
}

std::string guide_to_json(const Guide& guide) {
  Json root = Json::object();
  root.set("kind", "guide");
  root.set("format", kGuideFormat);
  root.set("url", guide.url);
  root.set("title", guide.title);
  root.set("author", guide.author);
  root.set("updated", guide.updated);
  root.set("pages", write_pages(guide.pages));
  root.set("blocks", write_blocks(guide.blocks));
  return to_json(root);
}

bool parse_guide(std::string_view json, Guide& out) {
  const std::optional<Json> value = parse_json(json);
  if (!value || value->string_of("kind") != "guide" || value->int_of("format") != kGuideFormat) return false;
  out = {};
  out.url = value->string_of("url");
  out.title = value->string_of("title");
  out.author = value->string_of("author");
  out.updated = value->string_of("updated");
  read_pages((*value)["pages"], out.pages);
  read_blocks((*value)["blocks"], out.blocks, 0);
  return !out.blocks.empty();
}

std::string guide_cache_name(std::string_view url) {
  const size_t faqs = url.find("/faqs/");
  if (faqs != std::string_view::npos) {
    size_t end = faqs + 6;
    while (end < url.size() && std::isdigit(static_cast<unsigned char>(url[end]))) ++end;
    if (end > faqs + 6) return std::string(url.substr(faqs + 6, end - faqs - 6)) + ".json";
  }
  std::uint64_t hash = 1469598103934665603ull;
  for (const char ch : url) hash = (hash ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.json", static_cast<unsigned long long>(hash));
  return name;
}

const char* guide_block_kind_name(GuideBlockKind kind) { return kKindNames[static_cast<int>(kind)]; }

}  // namespace oc::net
