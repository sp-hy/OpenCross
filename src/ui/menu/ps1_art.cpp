#include "ui/menu/ps1_art.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>

#include "import/container.h"
#include "import/menu_art.h"
#include "import/tim.h"

namespace oc::ui::ps1 {
namespace {

// Face file stems by character id (face\1999\NN_<stem>.png in the HD art).
const char* const kFaces[50] = {"seljyu", "kid",   "gill",  "ishit",  "srash",  "jakotu", "ridel",  "karsh", "zoa",   "mal",
                                "col",    "luchi", "poshul", "raz",   "zappa",  "orcha",  "radius", "cap",   "oba",   "glen",
                                "rena",   "miki",  "harler", "janes", "ryu",    "star",   "spriggan", "lucky", "kabuo", "rablue",
                                "jilbelt", "skarl", "kinoko", "ningyo", "mell",  "rea",    "banklif", "sunef", "miko",  "doc",
                                "gyadaran", "yusha", "olha",  "C13base", "C13a", "C13b",   "C13aa",  "C13ab", "C13bb", "yama"};

Rgba decode(const import::Tim& tim) {
  Rgba out;
  auto img = import::decode_tim(tim);
  if (!img) return out;
  const import::Image rgba = import::to_rgba(*img);
  out.w = rgba.width, out.h = rgba.height, out.px = rgba.pixels;
  return out;
}

}  // namespace

const char* face_name(int id) { return id >= 0 && id < 50 ? kFaces[id] : nullptr; }

std::u32string bitmap_text(const import::JpGlyphs& glyphs, std::u32string text) {
  for (char32_t& ch : text) {
    if (ch == U' ' || ch == U'\n' || glyphs.index_for(ch) >= 0) continue;
    char32_t alt[3] = {0, 0, 0};
    // The JP font has curly quotes and full-width ASCII, not the Latin forms.
    if (ch == U'"') alt[0] = 0x201C, alt[1] = 0xFF02;
    else if (ch == U',') alt[0] = 0xFF0C, alt[1] = 0x3001;
    else if (ch == U'\'') alt[0] = 0xFF07, alt[1] = 0x2019;
    else if (ch >= 0x21 && ch <= 0x7E) alt[0] = 0xFF01 + (ch - 0x21);
    else if (ch == 0x2018 || ch == 0x2019) alt[0] = 0xFF07, alt[1] = 0x27;
    else if (ch == 0x201C || ch == 0x201D) alt[0] = 0xFF02, alt[1] = 0x22;
    else if (ch == 0x2026) alt[0] = 0x22EF, alt[1] = 0x2026;
    else if (ch == 0x2010 || ch == 0x2013 || ch == 0x2014) alt[0] = 0x30FC, alt[1] = 0xFF0D;
    for (char32_t candidate : alt)
      if (candidate && glyphs.index_for(candidate) >= 0) {
        ch = candidate;
        break;
      }
  }
  return text;
}

std::optional<import::Tim> parchment_tim(const vfs::Vfs& fs) {
  auto raw = fs.read_disc("cdrom/map/etc/systim.bin");
  if (!raw) return std::nullopt;
  for (size_t i = 0; i + 32 < raw->size(); i += 4) {
    if (std::memcmp(raw->data() + i, "\x10\0\0\0", 4) != 0) continue;
    auto tim = import::parse_tim(ByteSpan(raw->data() + i, raw->size() - i));
    if (tim && tim->bpp == 4 && tim->has_clut && tim->image.x == 896 && tim->image.y == 96 && tim->image.w == 16 && tim->image.h == 64) return tim;
  }
  return std::nullopt;
}

Rgba parchment(const vfs::Vfs& fs) {
  const auto tim = parchment_tim(fs);
  return tim ? decode(*tim) : Rgba{};
}

Rgba menutim(const vfs::Vfs& fs, std::string_view entry) {
  auto raw = fs.read_disc("cdrom/menu/menutim.prd");
  if (!raw) return {};
  auto drp = import::parse_drp(ByteSpan(raw->data(), raw->size()));
  if (!drp) return {};
  for (const auto& drp_entry : drp->entries)
    if (drp_entry.name == entry) {
      if (auto tim = import::parse_tim(drp_entry.data)) return decode(*tim);
    }
  return {};
}

namespace {
// The disc's menu art, decoded once per Vfs (the menu files are the same on both discs of a set).
const import::MenuArt* menu_art(const vfs::Vfs& fs) {
  static std::map<const vfs::Vfs*, std::optional<import::MenuArt>> cache;
  auto it = cache.find(&fs);
  if (it == cache.end()) {
    auto test = fs.read_disc("cdrom/menu/menutest.prd");
    auto tims = fs.read_disc("cdrom/menu/menutim.prd");
    auto elem = fs.read_disc("cdrom/menu/menuelem.prd");
    auto smithy = fs.read_disc("cdrom/menu/smithy.prd");
    std::vector<ByteSpan> packs;
    if (elem) packs.emplace_back(*elem);
    if (smithy) packs.emplace_back(*smithy);
    std::optional<import::MenuArt> art;
    if (test && tims) art = import::load_menu_art(*test, *tims, packs);
    it = cache.emplace(&fs, std::move(art)).first;
  }
  return it->second ? &*it->second : nullptr;
}
}  // namespace

SheetPicture sheet_picture(const vfs::Vfs& fs, std::string_view sheet, int frame) {
  SheetPicture out;
  const import::MenuArt* art = menu_art(fs);
  if (!art) return out;
  import::MenuPicture picture = art->compose(sheet, frame);
  if (!picture) return out;
  out.x = picture.x, out.y = picture.y;
  out.rgba.w = static_cast<u32>(picture.w), out.rgba.h = static_cast<u32>(picture.h), out.rgba.px = std::move(picture.rgba);
  return out;
}

bool sheet_bounds(const vfs::Vfs& fs, std::string_view sheet, int frame, SheetBounds& out) {
  const import::MenuArt* art = menu_art(fs);
  const import::MenuPicture extent = art ? art->extent(sheet, frame) : import::MenuPicture{};
  if (!extent) return false;
  out = {extent.x, extent.y, extent.w, extent.h};
  return true;
}

// A flat mesh drawn the menus' way on the CPU: each vertex (x, y) scaled by (scale_x, scale_y) / 4096 and turned by `angle` (4096 = a full
// turn, clockwise on screen) about the draw point, each triangle filled at the pixel centres it covers with its corner colours and texel
// coordinates interpolated, the colour = corner colour x texel / 0x80 x `colour` / 0x80. `semi`: semi-transparent texels (all of an
// untextured face) get alpha 64. The picture is the same square for every angle, centred on the draw point.
SheetPicture mesh_picture(const vfs::Vfs& fs, std::string_view mesh_name, int angle, int scale_x, int scale_y, u32 colour, bool semi) {
  SheetPicture out;
  const import::MenuArt* art = menu_art(fs);
  const import::MenuArt::Mesh* mesh = art ? art->mesh(mesh_name) : nullptr;
  if (!mesh || mesh->triangles.empty()) return out;
  const double turn = static_cast<double>(angle) * 2.0 * 3.14159265358979323846 / 4096.0, cs = std::cos(turn), sn = std::sin(turn);
  const double kx = scale_x / 4096.0, ky = scale_y / 4096.0;
  double reach = 0;
  for (const auto& triangle : mesh->triangles)
    for (const auto& pos : triangle.pos) reach = std::max(reach, std::hypot(pos[0] * kx, pos[1] * ky));
  const int half = static_cast<int>(std::ceil(reach)) + 1, side = 2 * half;
  out.x = out.y = -half;
  out.rgba.w = out.rgba.h = static_cast<u32>(side);
  out.rgba.px.assign(static_cast<size_t>(side) * static_cast<size_t>(side) * 4, 0);
  const int tint[3] = {static_cast<int>(colour & 0xFF), static_cast<int>(colour >> 8 & 0xFF), static_cast<int>(colour >> 16 & 0xFF)};
  for (const auto& triangle : mesh->triangles) {
    double px[3], py[3];
    for (int k = 0; k < 3; ++k) {
      const double x = triangle.pos[static_cast<size_t>(k)][0] * kx, y = triangle.pos[static_cast<size_t>(k)][1] * ky;
      px[k] = cs * x - sn * y + half, py[k] = sn * x + cs * y + half;
    }
    const double area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
    if (std::fabs(area) < 1e-9) continue;
    const import::MenuPicture* page = triangle.page >= 0 ? &mesh->pages[static_cast<size_t>(triangle.page)] : nullptr;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({px[0], px[1], px[2]})))), x1 = std::min(side - 1, static_cast<int>(std::ceil(std::max({px[0], px[1], px[2]}))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min({py[0], py[1], py[2]})))), y1 = std::min(side - 1, static_cast<int>(std::ceil(std::max({py[0], py[1], py[2]}))));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const double cx = x + 0.5, cy = y + 0.5;
        const double w0 = ((px[1] - cx) * (py[2] - cy) - (px[2] - cx) * (py[1] - cy)) / area;
        const double w1 = ((px[2] - cx) * (py[0] - cy) - (px[0] - cx) * (py[2] - cy)) / area;
        const double weight[3] = {w0, w1, 1.0 - w0 - w1};
        if (weight[0] < 0 || weight[1] < 0 || weight[2] < 0) continue;
        int rgb[3];
        for (int channel = 0; channel < 3; ++channel) {
          double value = 0;
          for (int k = 0; k < 3; ++k) value += weight[k] * triangle.rgb[static_cast<size_t>(k)][static_cast<size_t>(channel)];
          rgb[channel] = static_cast<int>(value);
        }
        u8 alpha = semi ? 64 : 255;
        if (page) {
          double u = 0, v = 0;
          for (int k = 0; k < 3; ++k) u += weight[k] * triangle.uv[static_cast<size_t>(k)][0], v += weight[k] * triangle.uv[static_cast<size_t>(k)][1];
          const u8* texel = &page->rgba[(static_cast<size_t>(std::clamp(static_cast<int>(v), 0, 255)) * 256 + static_cast<size_t>(std::clamp(static_cast<int>(u), 0, 255))) * 4];
          if (texel[3] == 0) continue;
          if (texel[3] != import::kStpAlpha) alpha = 255;
          for (int channel = 0; channel < 3; ++channel) rgb[channel] = rgb[channel] * texel[channel] / 0x80;
        }
        u8* dst = &out.rgba.px[(static_cast<size_t>(y) * static_cast<size_t>(side) + static_cast<size_t>(x)) * 4];
        for (int channel = 0; channel < 3; ++channel) dst[channel] = static_cast<u8>(std::min(255, rgb[channel] * tint[channel] / 0x80));
        dst[3] = alpha;
      }
  }
  return out;
}

Rgba sheet_frame(const vfs::Vfs& fs, int frame) {
  const SheetPicture picture = sheet_picture(fs, "main", frame);
  if (!picture.rgba) return {};
  Rgba out;
  out.w = kSheetFrameW, out.h = kSheetFrameH;
  out.px.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
  for (u32 row = 0; row < picture.rgba.h; ++row)
    for (u32 col = 0; col < picture.rgba.w; ++col) {
      const int dx = kSheetFrameOriginX + picture.x + static_cast<int>(col), dy = kSheetFrameOriginY + picture.y + static_cast<int>(row);
      if (dx < 0 || dy < 0 || dx >= kSheetFrameW || dy >= kSheetFrameH) continue;
      std::memcpy(&out.px[(static_cast<size_t>(dy) * out.w + static_cast<size_t>(dx)) * 4], picture.rgba.at(col, row), 4);
    }
  return out;
}

Rgba face(const vfs::Vfs& fs, int char_id) {
  const char* stem = face_name(char_id);
  if (!stem) return {};
  auto raw = fs.read_disc(std::string("cdrom/face/") + stem + ".tim");
  if (!raw) return {};
  auto tim = import::parse_tim(ByteSpan(raw->data(), raw->size()));
  return tim ? decode(*tim) : Rgba{};
}

Rgba portrait(const vfs::Vfs& fs, int char_id) {
  if (char_id < 0 || char_id > 49) return {};
  char path[48];
  std::snprintf(path, sizeof path, "cdrom/menu/playtim/p%02d.tim", char_id);
  auto raw = fs.read_disc(path);
  if (!raw) return {};
  auto tim = import::parse_tim(ByteSpan(raw->data(), raw->size()));
  return tim ? decode(*tim) : Rgba{};
}

Rgba slot_icon(const vfs::Vfs& fs, int icon) {
  if (icon < 0 || icon > 14) return {};
  static const char kHex[] = "0123456789abcdef";
  // The sheet name uses icon + 1.
  const char name[5] = {'p', kHex[icon + 1], 'i', 'c', 0};
  return menutim(fs, name);
}

namespace {
// Copies the opaque texels (alpha != 0) of src (sx, sy, w, h) to dst at (dx, dy).
void blit(Rgba& dst, const Rgba& src, int sx, int sy, int w, int h, int dx, int dy) {
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int tx = sx + x, ty = sy + y, ox = dx + x, oy = dy + y;
      if (tx < 0 || ty < 0 || tx >= static_cast<int>(src.w) || ty >= static_cast<int>(src.h)) continue;
      if (ox < 0 || oy < 0 || ox >= static_cast<int>(dst.w) || oy >= static_cast<int>(dst.h)) continue;
      const u8* texel = src.at(static_cast<u32>(tx), static_cast<u32>(ty));
      if (texel[3] == 0) continue;
      std::memcpy(&dst.px[(static_cast<size_t>(oy) * dst.w + static_cast<size_t>(ox)) * 4], texel, 4);
    }
}
}  // namespace

Rgba plate(const vfs::Vfs& fs, int type, int variant) {
  const char* colour = type == 8 ? "mnbl" : type == 9 ? "mngr" : type == 10 ? "mnku" : type == 11 ? "mnre" : type == 12 ? "mnwh" : type == 13 ? "mnye" : type == 38 ? "mnni" : nullptr;
  if (!colour) return {};
  const Rgba frame_sheet = menutim(fs, colour), strip = menutim(fs, "mpla");
  if (!frame_sheet || !strip) return {};
  Rgba out;
  out.w = 184, out.h = 24;
  out.px.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
  // The last part of every frame, drawn first.
  blit(out, strip, 1, 0, 173, 24, 5, 0);
  struct Part {
    int u, v, w, h, x, y;
  };
  static constexpr Part kF1[4] = {{40, 0, 24, 16, 4, 0}, {40, 16, 16, 8, 4, 16}, {64, 0, 16, 16, 163, 0}, {56, 16, 24, 8, 155, 16}};
  static constexpr Part kF2[4] = {{80, 0, 24, 16, 4, 0}, {80, 16, 8, 8, 4, 16}, {104, 0, 16, 16, 163, 0}, {96, 16, 24, 8, 155, 16}};
  static constexpr Part kF4[4] = {{0, 0, 24, 12, 4, 0}, {0, 12, 12, 12, 4, 12}, {16, 12, 24, 12, 155, 12}, {28, 0, 12, 12, 167, 0}};
  const Part* parts = variant == 2 ? kF2 : variant == 4 ? kF4 : kF1;
  for (int i = 0; i < 4; ++i) blit(out, frame_sheet, parts[i].u, parts[i].v, parts[i].w, parts[i].h, parts[i].x, parts[i].y);
  return out;
}

Rgba keyitem(const vfs::Vfs& fs, int nn) {
  // cdrom file index 152 + NN (research data/cdrom_index.csv rows 153..216; 206..212 do not exist) = the order of hd/menu/keyitem/NN.png
  // (checked by matching the 4x-reduced PNGs against every TIM by mean colour error: 42 of the 57 PNGs pick the TIM of their own index
  // first; the error is large for all pairs, 70..190 per channel, so the order of the index is the rule and the match only supports it).
  static const char* const kNames[64] = {
      "tele",  "shark",  "kid",    "suzu",   "musibue", "ilurans", "icegun", "ice",   "hekeran", "rindo",  "egg",   "gaikotu", "sebone",
      "rokkotu", "kotuban", "kenko", "hone",  "kaori",   "kinoko",  "jakotu", "hidora", "kyakuma", "chikaro", "ryu",  "white",   "black",
      "red",   "blue",   "yellow", "green",  "seimei",  "uroko",   "hoshi",  "cardkey", "bomb",  "ryu2",   "ryu3",  "pass",    "kajitu",
      "katami", "hundle", "mirror", "tate",   "sio",     "chrono",  "soubi",  "broti", "gekijo",  "monsho", "book",  "igaku",   "badge",
      "tokiegg", nullptr, nullptr,  nullptr,  nullptr,   nullptr,   nullptr,  nullptr, "gofu",    "time",   "kajiya", "uroko"};
  if (nn < 1 || nn > 64 || !kNames[nn - 1]) return {};
  auto raw = fs.read_disc(std::string("cdrom/menu/keyitem/") + kNames[nn - 1] + ".tim");
  if (!raw) return {};
  auto tim = import::parse_tim(ByteSpan(raw->data(), raw->size()));
  return tim ? decode(*tim) : Rgba{};
}

}  // namespace oc::ui::ps1
