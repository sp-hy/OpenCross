#include "render/model_tiles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "core/log.h"
#include "import/model.h"
#include "import/tim.h"
#include "render/texture_registry.h"
#include "vfs/vfs.h"

namespace oc::render {

namespace {

namespace tr = texture_registry;

bool rgba_ok(const import::Image& image) {
  return !image.indexed && image.width && image.height && image.pixels.size() >= size_t(image.width) * image.height * 4;
}

import::Image cut(const import::Image& page, u32 x, u32 y, u32 w, u32 h) {
  import::Image out;
  out.width = w, out.height = h;
  out.pixels.resize(size_t(w) * h * 4);
  for (u32 row = 0; row < h; ++row) std::memcpy(&out.pixels[size_t(row) * w * 4], &page.pixels[(size_t(y + row) * page.width + x) * 4], size_t(w) * 4);
  return out;
}

TextureKey tile_key(const ModelTile& tile) { return {"room_model", "", -1, "tile_" + tile.hash}; }

// Art for a rect of a page, any size.
struct Patch {
  u32 x = 0, y = 0, w = 0, h = 0;
  const import::Image* image = nullptr;
};

// `page` enlarged (nearest) to the largest patch scale, every patch pasted (nearest) over its rect.
import::Image compose(const import::Image& page, const std::vector<Patch>& patches) {
  double scale_x = 1, scale_y = 1;
  for (const Patch& patch : patches) {
    scale_x = std::max(scale_x, double(patch.image->width) / patch.w);
    scale_y = std::max(scale_y, double(patch.image->height) / patch.h);
  }
  import::Image out;
  out.width = static_cast<u32>(std::lround(page.width * scale_x)), out.height = static_cast<u32>(std::lround(page.height * scale_y));
  out.pixels.resize(size_t(out.width) * out.height * 4);
  for (u32 y = 0; y < out.height; ++y)
    for (u32 x = 0; x < out.width; ++x) {
      const u32 sx = std::min(page.width - 1, static_cast<u32>(x / scale_x)), sy = std::min(page.height - 1, static_cast<u32>(y / scale_y));
      std::memcpy(&out.pixels[(size_t(y) * out.width + x) * 4], &page.pixels[(size_t(sy) * page.width + sx) * 4], 4);
    }
  for (const Patch& patch : patches) {
    const u32 x0 = static_cast<u32>(std::lround(patch.x * scale_x)), x1 = std::min(out.width, static_cast<u32>(std::lround((patch.x + patch.w) * scale_x)));
    const u32 y0 = static_cast<u32>(std::lround(patch.y * scale_y)), y1 = std::min(out.height, static_cast<u32>(std::lround((patch.y + patch.h) * scale_y)));
    const import::Image& art = *patch.image;
    for (u32 y = y0; y < y1; ++y)
      for (u32 x = x0; x < x1; ++x) {
        const u32 sx = std::min(art.width - 1, static_cast<u32>(u64(x - x0) * art.width / (x1 - x0)));
        const u32 sy = std::min(art.height - 1, static_cast<u32>(u64(y - y0) * art.height / (y1 - y0)));
        std::memcpy(&out.pixels[(size_t(y) * out.width + x) * 4], &art.pixels[(size_t(sy) * art.width + sx) * 4], 4);
      }
  }
  return out;
}

bool inside(const ModelTile& tile, const FieldCharacterArt::Match& character) {
  return tile.texture == 0 && tile.x + tile.w <= character.w && tile.y + tile.h <= character.h;
}

// The game's index, built once per VFS.
const FieldCharacterArt& characters_of(const vfs::Vfs& vfs) {
  static std::mutex mutex;
  static std::map<const vfs::Vfs*, std::unique_ptr<FieldCharacterArt>> cache;
  std::lock_guard lock(mutex);
  auto& slot = cache[&vfs];
  if (!slot) slot = std::make_unique<FieldCharacterArt>(vfs);
  return *slot;
}

}  // namespace

std::vector<ModelTile> model_tiles(const assets::ModelAsset& asset) {
  struct Rect {
    u32 x0, y0, x1, y1;
  };
  std::vector<std::vector<Rect>> rects(asset.textures.size());
  for (const assets::Mesh& mesh : asset.meshes)
    for (const assets::SubMesh& sub : mesh.submeshes) {
      if (sub.material >= mesh.materials.size()) continue;
      const int texture = mesh.materials[sub.material].texture;
      if (texture < 0 || static_cast<size_t>(texture) >= asset.textures.size() || !rgba_ok(asset.textures[static_cast<size_t>(texture)])) continue;
      const import::Image& page = asset.textures[static_cast<size_t>(texture)];
      for (u32 i = sub.first_index; i + 2 < sub.first_index + sub.index_count && i + 2 < mesh.indices.size(); i += 3) {
        Rect rect{page.width, page.height, 0, 0};
        for (u32 k = 0; k < 3; ++k) {
          const auto& uv = mesh.vertices[mesh.indices[i + k]].uv;
          const u32 x = std::min(page.width - 1, static_cast<u32>(std::max(0.0f, uv[0] * page.width)));
          const u32 y = std::min(page.height - 1, static_cast<u32>(std::max(0.0f, uv[1] * page.height)));
          rect.x0 = std::min(rect.x0, x), rect.y0 = std::min(rect.y0, y), rect.x1 = std::max(rect.x1, x + 1), rect.y1 = std::max(rect.y1, y + 1);
        }
        rects[static_cast<size_t>(texture)].push_back(rect);
      }
    }
  std::vector<ModelTile> out;
  for (size_t texture = 0; texture < rects.size(); ++texture) {
    std::vector<Rect>& list = rects[texture];
    for (bool merged = true; merged;) {
      merged = false;
      for (size_t i = 0; i < list.size() && !merged; ++i)
        for (size_t j = i + 1; j < list.size(); ++j) {
          const Rect& a = list[i];
          const Rect& b = list[j];
          if (a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1) {
            list[i] = {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(j));
            merged = true;
            break;
          }
        }
    }
    const import::Image& page = asset.textures[texture];
    for (const Rect& rect : list) {
      ModelTile tile;
      tile.texture = static_cast<u32>(texture);
      tile.x = rect.x0, tile.y = rect.y0, tile.w = rect.x1 - rect.x0, tile.h = rect.y1 - rect.y0;
      const import::Image pixels = cut(page, tile.x, tile.y, tile.w, tile.h);
      bool any = false;
      for (size_t i = 3; i < pixels.pixels.size() && !any; i += 4) any = pixels.pixels[i] != 0;
      if (!any) continue;
      tile.hash = texture_content_tag(tile.w, tile.h, pixels.pixels.data());
      out.push_back(std::move(tile));
    }
  }
  return out;
}

FieldCharacterArt::FieldCharacterArt(const vfs::Vfs& vfs) {
  for (const std::string& member : vfs.list("cdrom2")) {
    if (!member.starts_with("fieldobj/player/") || !member.ends_with(".tim")) continue;
    const auto bytes = vfs.read("cdrom2/" + member);
    const auto tim = bytes ? import::parse_tim(*bytes) : std::nullopt;
    auto image = tim ? import::decode_tim(*tim, 0, true) : std::nullopt;
    if (!image) continue;
    import::Image rgba = image->indexed ? import::to_rgba(*image) : std::move(*image);
    if (!rgba_ok(rgba)) continue;
    Entry entry;
    entry.match = {member.substr(0, member.size() - 4), rgba.width, rgba.height, 0};
    entry.image = std::move(rgba);
    characters_.push_back(std::move(entry));
  }
}

std::optional<FieldCharacterArt::Match> FieldCharacterArt::closest(const assets::ModelAsset& asset, const std::vector<ModelTile>& tiles) const {
  if (asset.textures.empty() || !rgba_ok(asset.textures[0])) return std::nullopt;
  const import::Image& page = asset.textures[0];
  // The texels page 0's faces sample.
  std::vector<u8> sampled(size_t(page.width) * page.height, 0);
  for (const ModelTile& tile : tiles)
    if (tile.texture == 0)
      for (u32 y = tile.y; y < tile.y + tile.h; ++y) std::fill_n(&sampled[size_t(y) * page.width + tile.x], tile.w, u8{1});
  std::optional<Match> best;
  for (const Entry& entry : characters_) {
    const import::Image& art = entry.image;
    if (art.width > page.width || art.height > page.height) continue;
    double total = 0, sum_a = 0, sum_b = 0, sum_aa = 0, sum_bb = 0, sum_ab = 0;
    size_t texels = 0, both = 0;
    for (u32 y = 0; y < art.height; ++y)
      for (u32 x = 0; x < art.width; ++x) {
        const u8* want = &art.pixels[(size_t(y) * art.width + x) * 4];
        if (!want[3] || !sampled[size_t(y) * page.width + x]) continue;
        const u8* got = &page.pixels[(size_t(y) * page.width + x) * 4];
        ++texels;
        if (!got[3]) {
          total += 255.0;
          continue;
        }
        total += (std::abs(want[0] - got[0]) + std::abs(want[1] - got[1]) + std::abs(want[2] - got[2])) / 3.0;
        const double a = 0.299 * want[0] + 0.587 * want[1] + 0.114 * want[2], b = 0.299 * got[0] + 0.587 * got[1] + 0.114 * got[2];
        sum_a += a, sum_b += b, sum_aa += a * a, sum_bb += b * b, sum_ab += a * b;
        ++both;
      }
    // Too little of the character's texture sampled to tell.
    if (texels < 64 || both < 64) continue;
    const double n = static_cast<double>(both);
    const double var_a = sum_aa - sum_a * sum_a / n, var_b = sum_bb - sum_b * sum_b / n;
    Match match = entry.match;
    match.score = total / static_cast<double>(texels);
    match.correlation = var_a > 0 && var_b > 0 ? (sum_ab - sum_a * sum_b / n) / std::sqrt(var_a * var_b) : 0.0;
    if (!best || match.score < best->score) best = std::move(match);
  }
  return best;
}

std::optional<FieldCharacterArt::Match> FieldCharacterArt::match(const assets::ModelAsset& asset, const std::vector<ModelTile>& tiles) const {
  auto best = closest(asset, tiles);
  return best && best->correlation >= kMinCorrelation && best->score <= kMaxScore ? best : std::nullopt;
}

u32 dump_room_model_tiles(const assets::ModelAsset& asset, const FieldCharacterArt& characters, bool forced) {
  const std::vector<ModelTile> tiles = model_tiles(asset);
  const auto character = characters.match(asset, tiles);
  u32 written = 0;
  for (const ModelTile& tile : tiles) {
    if (character && inside(tile, *character)) continue;
    const import::Image pixels = cut(asset.textures[tile.texture], tile.x, tile.y, tile.w, tile.h);
    const std::vector<u8> file = model_file_pixels(tile.w, tile.h, pixels.pixels.data());
    written += (forced ? tr::dump_forced(tile_key(tile), tile.w, tile.h, file.data()) : tr::dump(tile_key(tile), tile.w, tile.h, file.data())) ? 1u : 0u;
  }
  return written;
}

void resolve_room_model_textures(assets::ModelAsset& asset, const vfs::Vfs& vfs) {
  const bool replacing = tr::active(), enhanced = vfs.enhanced();
  if ((!replacing && !enhanced) || asset.textures.empty()) return;
  const FieldCharacterArt& characters = characters_of(vfs);
  if (replacing && tr::settings().dump) dump_room_model_tiles(asset, characters, false);
  std::vector<std::vector<Patch>> patches(asset.textures.size());
  // Keeps the patch images alive until the pages are composed.
  std::vector<std::unique_ptr<import::Image>> images;
  const std::vector<ModelTile> tiles = model_tiles(asset);
  const auto character = characters.match(asset, tiles);
  if (character) {
    std::optional<import::Image> hd;
    if (const auto bytes = vfs.read("hd/" + character->stem + ".tim"))
      if (auto image = import::load_hd_tim(*bytes); image && rgba_ok(*image) && image->width % character->w == 0) hd = std::move(*image);
    const import::Image original = cut(asset.textures[0], 0, 0, character->w, character->h);
    const import::Image& best = hd ? *hd : original;
    auto art = std::make_unique<import::Image>(original);
    if (replacing && tr::replace({"model", character->stem, -1, "clut0"}, art->width, art->height, art->pixels)) {
      model_restore_stp(best.width, best.height, best.pixels.data(), art->width, art->height, art->pixels);
    } else if (enhanced && hd) {
      *art = std::move(*hd);
    } else {
      art.reset();
    }
    if (art) {
      log::debug("room model: field character {} drawn with its {}x{} art", character->stem, art->width, art->height);
      patches[0].push_back({0, 0, character->w, character->h, art.get()});
      images.push_back(std::move(art));
    }
  }
  if (replacing)
    for (const ModelTile& tile : tiles) {
      if (character && inside(tile, *character)) continue;
      const import::Image original = cut(asset.textures[tile.texture], tile.x, tile.y, tile.w, tile.h);
      auto art = std::make_unique<import::Image>(original);
      if (!tr::replace(tile_key(tile), art->width, art->height, art->pixels)) continue;
      model_restore_stp(original.width, original.height, original.pixels.data(), art->width, art->height, art->pixels);
      log::debug("room model: tile {} (page {} at {},{} {}x{}) replaced by {}x{}", tile.hash, tile.texture, tile.x, tile.y, tile.w, tile.h, art->width,
                 art->height);
      patches[tile.texture].push_back({tile.x, tile.y, tile.w, tile.h, art.get()});
      images.push_back(std::move(art));
    }
  for (size_t texture = 0; texture < patches.size(); ++texture) {
    if (patches[texture].empty()) continue;
    asset.mod_textures.resize(asset.textures.size());
    asset.mod_textures[texture] = compose(asset.textures[texture], patches[texture]);
  }
}

}  // namespace oc::render
