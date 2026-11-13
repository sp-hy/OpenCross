#include "game/room_view.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <future>
#include <thread>

#include "core/log.h"
#include "core/parallel.h"
#include "core/timer.h"
#include "game/room_models.h"
#include "import/battle_stage.h"
#include "import/chip2.h"
#include "import/room.h"
#include "render/stage_tiles.h"

namespace oc::game {

namespace {
// The field picture: 320 x 224 buffer rows (render/camera.h kFieldRows).
constexpr float kViewW = render::kLayerW;
constexpr float kViewH = render::kFieldRows;
// The loader subtracts 24 from y_min.
constexpr int kYMinBias = 24;

bool has_texels(const std::vector<u8>& rgba) {
  for (size_t i = 3; i < rgba.size(); i += 4)
    if (rgba[i]) return true;
  return false;
}
}  // namespace

struct RoomView::Item {
  render::Texture texture;
  render::TexRef set;
  // 4x set, built lazily (same 1x rectangle).
  std::unique_ptr<render::Texture> texture_hd;
  render::TexRef set_hd;
  // (layer index, group) images merged into this item.
  std::vector<std::pair<u32, u32>> parts;
  u32 group = 0;
  // RoomGroupImage::blend.
  s32 blend = -1;
  u16 z = 0;
  u32 first_layer = 0;
  int x = 0, y = 0, w = 0, h = 0;
};

struct RoomView::Prepared {
  // The import failed when `scene` is empty (`err` says why).
  std::optional<assets::RoomScene> scene;
  std::string err;
  // The .prd actually read (empty = none).
  std::string prd_path;
  std::vector<std::unique_ptr<Item>> items;
  // 1x image per item; 4x per item when composed ahead (empty = ensure_hd composes them).
  std::vector<std::vector<u8>> px, px_hd;
  // With a GPU context: the 4x textures made from px_hd on the worker (Texture::create_blocking); a null entry is uploaded from px_hd.
  std::vector<std::unique_ptr<render::Texture>> tex_hd;
  // The room's own models, built and resolved in parallel (an empty entry: build failed, loaded at first use instead).
  std::vector<std::optional<PreparedRoomModel>> models;
  // HD art: the chip2 read still running, or (chip2_done) its result.
  std::future<Chip2Result> chip2_future;
  Chip2Result chip2;
  bool chip2_done = false;
};

namespace {
// The 4x images composed ahead are held until the load uploads them; a room above this many bytes composes them at load time instead
// (ensure_hd streams them through a small window).
constexpr size_t kPrefetchHdBytes = size_t{512} << 20;
}  // namespace

RoomView::RoomView() = default;
RoomView::~RoomView() {
  drop_prefetch();
  clear();
}

void RoomView::drop_prefetch() {
  if (prefetch_.valid()) prefetch_.get();
  prefetch_name_.clear();
}

void RoomView::prefetch(std::string_view name, std::string prd_path) {
  if (!files_) return;
  const u32 generation = files_->generation();
  if (prefetch_.valid() && prefetch_name_ == name && prefetch_prd_ == prd_path && prefetch_hd_ == hd_ && prefetch_generation_ == generation) return;
  drop_prefetch();
  prefetch_name_ = std::string(name);
  prefetch_prd_ = std::move(prd_path);
  prefetch_hd_ = hd_;
  prefetch_generation_ = generation;
  log::debug("room {}: preparing ahead on a worker", prefetch_name_);
  prefetch_ = std::async(std::launch::async, [files = files_, ctx = ctx_, name = prefetch_name_, prd = prefetch_prd_, hd = hd_]() {
    ScopedTimer timer("room: prepare ahead (worker)");
    return prepare(*files, ctx, name, prd, true, hd);
  });
}

void RoomView::clear() {
  if (chip2_future_.valid()) chip2_future_.wait();
  chip2_future_ = {};
  items_.clear();
  prepared_models_.clear();
  scene_.reset();
  chip2_.reset();
  group_tint_.clear();
  hd_ready_ = hd_failed_ = false;
  loaded_ = false;
}

bool RoomView::init(const vfs::Vfs& files, render::GpuContext& ctx, render::SpriteBatch& sprites, render::MeshPipeline& meshes) {
  files_ = &files;
  ctx_ = &ctx;
  sprites_ = &sprites;
  meshes_ = &meshes;
  // Nearest sampling: tiles are 1:1 pixel art.
  if (!sampler_.create(ctx, false)) return false;
  // The widescreen edge fade ramp (smoothstep).
  if (fade_sampler_.create(ctx, true)) {
    u8 ramp[128 * 4];
    for (int i = 0; i < 128; ++i) {
      const float frac = static_cast<float>(i) / 127.0f, fade = 1.0f - frac * frac * (3.0f - 2.0f * frac);
      ramp[i * 4 + 0] = ramp[i * 4 + 1] = ramp[i * 4 + 2] = 0;
      ramp[i * 4 + 3] = static_cast<u8>(std::lround(fade * 255.0f));
    }
    if (fade_tex_.create(ctx, 128, 1, render::TextureFormat::Rgba8, ramp)) fade_set_ = render::TexRef(fade_tex_, fade_sampler_);
  }
  std::set<std::string> bins, ctds;
  constexpr std::string_view kDir = "map/mapbin/";
  for (std::string path : files.list("cdrom")) {
    for (char& ch : path)
      if (ch == '\\') ch = '/';
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const size_t at = lower.find(kDir);
    if (at == std::string::npos || lower.size() < at + kDir.size() + 4) continue;
    const std::string stem = lower.substr(at + kDir.size(), lower.size() - at - kDir.size() - 4);
    if (stem.find('/') != std::string::npos) continue;
    const std::string ext = lower.substr(lower.size() - 4);
    if (ext == ".bin") bins.insert(stem);
    if (ext == ".ctd") ctds.insert(stem);
  }
  for (const auto& bin_stem : bins)
    if (ctds.count(bin_stem)) rooms_.push_back(bin_stem);
  log::info("room view: {} rooms with .bin + .ctd", rooms_.size());
  return true;
}

RoomView::Limits RoomView::default_limits() const { return limits_; }

RoomView::Limits RoomView::widen(Limits limits, int extra) {
  if (extra <= 0) return limits;
  // Visible screen columns -extra .. 320 + extra, tiles at layer x -x_max .. 320 - x_min ("Pan limits") + scroll S: the picture covers the
  // view while S - x_max <= -extra and S + 320 - x_min >= 320 + extra. A picture narrower than the view is centred (black on both sides).
  const int lo = std::min(limits.x_min, limits.x_max), hi = std::max(limits.x_min, limits.x_max);
  int nlo = lo + extra, nhi = hi - extra;
  if (nlo > nhi) nlo = nhi = (lo + hi) / 2;
  limits.x_min = nlo;
  limits.x_max = nhi;
  return limits;
}

void RoomView::set_scroll(int x, int y, bool snap) {
  prev_pan_x_ = snap ? x : pan_x_;
  prev_pan_y_ = snap ? y : pan_y_;
  pan_x_ = x;
  pan_y_ = y;
}

// The CPU half of a load. The chip2 read (inflate of several MB) is independent of the bin / ctd import, so it overlaps it.
std::unique_ptr<RoomView::Prepared> RoomView::prepare(const vfs::Vfs& files, render::GpuContext* ctx, const std::string& name,
                                                      const std::string& prd_request, bool ahead, bool hd) {
  auto prep = std::make_unique<Prepared>();
  if (hd)
    prep->chip2_future = std::async(std::launch::async, [&files, name]() {
      Chip2Result result;
      result.first = read_chip2(files, name, &result.second);
      return result;
    });
  const std::string base = "cdrom/map/mapbin/" + name;
  std::optional<Bytes> bin, ctd, prd;
  {
    ScopedTimer timer("room: zip reads (bin/ctd/prd)");
    bin = files.read(base + ".bin");
    ctd = files.read(base + ".ctd");
    if (!bin || !ctd) {
      prep->err = ".bin/.ctd not found";
      return prep;
    }
    const std::string prd_path = prd_request.empty() ? base + ".prd" : prd_request;
    prd = files.read(prd_path);
    if (prd && prd_path != base + ".prd") log::info("room {}: effects from {} (disc index .prd record)", name, prd_path);
    if (prd) prep->prd_path = prd_path;
  }
  {
    ScopedTimer timer("room: import (lzss + 1x composition + effects)");
    prep->scene = import::load_room(name, *bin, *ctd, prd ? ByteSpan(*prd) : ByteSpan{}, &prep->err);
  }
  if (!prep->scene) return prep;
  {
    ScopedTimer timer("room: 1x merge");
    prep->items = plan_items(*prep->scene);
    prep->px = compose_1x(*prep->scene, prep->items);
  }
  {
    ScopedTimer timer("room: room models (parallel)");
    prep->models.resize(prep->scene->models.size());
    parallel_for(prep->models.size(), [&](size_t i) { prep->models[i] = prepare_room_model(*prep->scene, i, ctx); });
  }
  if (!hd || !ahead) return prep;
  prep->chip2 = prep->chip2_future.get();
  prep->chip2_done = true;
  if (!prep->chip2.first) return prep;
  size_t bytes = 0;
  for (const auto& item : prep->items)
    bytes += static_cast<size_t>(item->w) * static_cast<size_t>(item->h) * import::kChip2Scale * import::kChip2Scale * 4;
  if (bytes > kPrefetchHdBytes) return prep;
  ScopedTimer timer("room: chip2 4x composition (ahead)");
  prep->px_hd.resize(prep->items.size());
  parallel_for(prep->items.size(), [&](size_t i) { prep->px_hd[i] = compose_pixels(*prep->scene, &*prep->chip2.first, *prep->items[i], true); });
  if (!ctx) return prep;
  prep->tex_hd.resize(prep->items.size());
  for (size_t i = 0; i < prep->items.size(); ++i) {
    const Item& item = *prep->items[i];
    if (!has_texels(prep->px_hd[i])) {
      prep->px_hd[i] = {};
      continue;
    }
    auto tex = std::make_unique<render::Texture>();
    if (!tex->create_blocking(*ctx, static_cast<u32>(item.w * import::kChip2Scale), static_cast<u32>(item.h * import::kChip2Scale), render::TextureFormat::Rgba8,
                              prep->px_hd[i].data()))
      continue;
    prep->tex_hd[i] = std::move(tex);
    prep->px_hd[i] = {};
  }
  return prep;
}

bool RoomView::load(std::string_view name) {
  const std::string prd_request = std::move(prd_path_);
  prd_path_.clear();
  std::unique_ptr<Prepared> prep;
  if (prefetch_.valid()) {
    const bool match = prefetch_name_ == name && prefetch_prd_ == prd_request && prefetch_hd_ == hd_ && files_ &&
                       prefetch_generation_ == files_->generation();
    {
      ScopedTimer timer("room: wait for the prepared room");
      prep = prefetch_.get();
    }
    if (!match) {
      log::debug("room {}: the room prepared ahead ({}) does not match; loading normally", name, prefetch_name_);
      prep.reset();
    }
    prefetch_name_.clear();
  }
  clear();
  name_ = std::string(name);
  if (!prep) prep = prepare(*files_, ctx_, name_, prd_request, false, hd_);
  if (!prep->scene) {
    log::error("room {}: {}", name_, prep->err);
    return false;
  }
  loaded_prd_path_ = prep->prd_path;
  prepared_models_ = std::move(prep->models);
  scene_ = std::make_unique<assets::RoomScene>(std::move(*prep->scene));
  const assets::RoomScene* scene = scene_.get();
  if (prep->chip2_done) {
    chip2_ = std::move(prep->chip2.first);
    if (!chip2_) {
      log::warn("room {}: {}", name_, prep->chip2.second);
      hd_failed_ = true;
    }
  } else {
    chip2_future_ = std::move(prep->chip2_future);
  }
  layer_mask_ = 0xFFFFFFFFu;
  items_ = std::move(prep->items);
  if (!upload_items(prep->px)) return false;

  const auto& cam = scene_->camera;
  group_scale_ = cam.group_scale;
  layer_depths_.clear();
  if (std::ranges::any_of(group_scale_, [](u32 scale) { return scale != 4096u; }))
    for (const assets::RoomLayer& layer : scene->layers)
      for (const assets::RoomGroupImage& image : layer.groups) {
        const draw_offset::LayerDepth depth{image.z, static_cast<int>(image.group)};
        if (std::ranges::find(layer_depths_, depth) == layer_depths_.end()) layer_depths_.push_back(depth);
      }
  limits_ = {cam.x_min, cam.x_max, cam.y_min - kYMinBias, cam.y_max};
  camera_ = render::FieldCamera::from_room(cam);
  walk_.init(scene->walk, scene->attr_flags);
  layers_ = scene->layers.size();
  groups_ = scene->group_image_count();
  loaded_ = true;
  set_scroll(0, 0, true);
  if (hd_ && !prep->px_hd.empty()) {
    ScopedTimer timer("room: HD texture uploads (composed ahead)");
    bool uploaded = true;
    for (size_t i = 0; i < items_.size() && uploaded; ++i) {
      if (i < prep->tex_hd.size() && prep->tex_hd[i]) {
        items_[i]->texture_hd = std::move(prep->tex_hd[i]);
        items_[i]->set_hd = render::TexRef(*items_[i]->texture_hd, sampler_);
      } else {
        uploaded = upload_set(*items_[i], true, prep->px_hd[i]);
      }
    }
    hd_ready_ = uploaded;
    hd_failed_ = !uploaded;
    if (uploaded) log::info("room {}: built {} HD (4x) textures", name_, items_.size());
    else log::error("room {}: HD texture upload failed", name_);
  } else if (hd_) {
    ensure_hd();
  }
  log::info("room {}: {} layers, {} group images, {} textures, {} tris, pan x[{}..{}] y[{}..{}]", name_, layers_, groups_,
            items_.size(), scene->walk.triangles.size(), cam.x_min, cam.x_max, cam.y_min, cam.y_max);
  return true;
}

std::vector<std::unique_ptr<RoomView::Item>> RoomView::plan_items(const assets::RoomScene& scene) {
  // Merge the (layer, group) images that share a group and z (they sort identically) into one texture each, so a
  // room needs few textures. Within a merge, later layers are drawn over earlier ones. An image with no 1x texels (a kind only the
  // HD art may fill) keeps an item of its own, so it never widens the texture of the images it would merge with.
  constexpr u32 kShared = 0xFFFFFFFFu;
  struct Key {
    u32 group;
    // blend: RoomGroupImage::blend, -1 opaque, 0..3 semi-transparent with that ABR.
    s32 blend;
    u16 z;
    // kShared, or the layer of an image with no 1x texels.
    u32 own_layer;
    bool operator<(const Key& other) const {
      if (group != other.group) return group < other.group;
      if (blend != other.blend) return blend < other.blend;
      if (z != other.z) return z < other.z;
      return own_layer < other.own_layer;
    }
  };
  std::map<Key, std::vector<std::pair<u32, const assets::RoomGroupImage*>>> merged;
  for (const auto& layer : scene.layers)
    for (const auto& image : layer.groups)
      merged[{image.group, image.blend, image.z, has_texels(image.rgba) ? kShared : layer.index}].emplace_back(layer.index, &image);

  // Draw order: z is depth, larger = farther. Sort by z descending, then first layer, then group.
  struct Ordered {
    Key key;
    u32 first_layer;
    const std::vector<std::pair<u32, const assets::RoomGroupImage*>>* parts;
  };
  std::vector<Ordered> order;
  for (const auto& [key, parts] : merged) order.push_back({key, parts.front().first, &parts});
  std::sort(order.begin(), order.end(), [](const Ordered& lhs, const Ordered& rhs) {
    if (lhs.key.z != rhs.key.z) return lhs.key.z > rhs.key.z;
    if (lhs.first_layer != rhs.first_layer) return lhs.first_layer < rhs.first_layer;
    if (lhs.key.group != rhs.key.group) return lhs.key.group < rhs.key.group;
    // Opaque texels first, then the blended ones of the same tiles.
    return lhs.key.blend < rhs.key.blend;
  });

  std::vector<std::unique_ptr<Item>> items;
  for (const Ordered& ordered : order) {
    int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    for (const auto& part : *ordered.parts) {
      x0 = std::min(x0, part.second->x);
      y0 = std::min(y0, part.second->y);
      x1 = std::max(x1, part.second->x + static_cast<int>(part.second->width));
      y1 = std::max(y1, part.second->y + static_cast<int>(part.second->height));
    }
    auto item = std::make_unique<Item>();
    item->x = x0;
    item->y = y0;
    item->w = x1 - x0;
    item->h = y1 - y0;
    item->group = ordered.key.group;
    item->blend = ordered.key.blend;
    item->first_layer = ordered.first_layer;
    item->z = ordered.key.z;
    for (const auto& part : *ordered.parts) item->parts.emplace_back(part.first, part.second->group);
    items.push_back(std::move(item));
  }
  return items;
}

std::vector<std::vector<u8>> RoomView::compose_1x(const assets::RoomScene& scene, const std::vector<std::unique_ptr<Item>>& items) {
  std::vector<std::vector<u8>> px;
  px.reserve(items.size());
  for (const auto& item : items) px.push_back(compose_pixels(scene, nullptr, *item, false));
  return px;
}

bool RoomView::upload_items(const std::vector<std::vector<u8>>& px) {
  ScopedTimer timer("room: 1x texture uploads");
  for (size_t i = 0; i < items_.size(); ++i)
    if (!upload_set(*items_[i], false, px[i])) {
      log::error("room {}: texture upload failed", name_);
      return false;
    }
  art_x0_ = INT32_MAX, art_x1_ = INT32_MIN;
  for (const auto& it : items_) art_x0_ = std::min(art_x0_, it->x), art_x1_ = std::max(art_x1_, it->x + it->w);
  if (art_x0_ >= art_x1_) art_x0_ = 0, art_x1_ = static_cast<int>(kViewW);
  log::debug("room {}: painted columns {}..{} (pan limits x_min {} x_max {} -> {}..{})", name_, art_x0_, art_x1_, limits_.x_min, limits_.x_max,
             -limits_.x_max, static_cast<int>(kViewW) - limits_.x_min);
  return true;
}

std::optional<assets::StageAsset> RoomView::effect_meshes() const {
  if (!loaded_ || loaded_prd_path_.empty() || !files_) return std::nullopt;
  const auto prd = files_->read(loaded_prd_path_);
  if (!prd) return std::nullopt;
  std::string err;
  std::optional<assets::StageAsset> st = import::load_stage(*prd, {}, &err);
  if (!st || st->parts.empty()) return std::nullopt;
  // Texture dump / replace path: room_mesh/<object>_<content tag>.png.
  st->tex_group = "room_mesh";
  render::bake_object_textures(*st);
  render::apply_tile_textures(*st, "room_mesh");
  return st;
}

bool RoomView::reload_art(bool hd) {
  drop_prefetch();
  hd_ = hd;
  if (!loaded_ || !scene_) return false;
  // RDE's two file sets differ only in art for a room (checked over all 566 rooms of cdrom.dat / cdrom2.dat): .bin subfiles 11+ (room
  // model meshes / UVs) and the .ctd texture pages; the script, walk mesh, camera, zones, CLUTs and every .prd are byte-identical.
  const std::string base = "cdrom/map/mapbin/" + name_;
  const auto bin = files_->read(base + ".bin");
  const auto ctd = files_->read(base + ".ctd");
  std::optional<Bytes> prd;
  if (!loaded_prd_path_.empty()) prd = files_->read(loaded_prd_path_);
  std::string err;
  std::optional<assets::RoomScene> fresh;
  if (bin && ctd) fresh = import::load_room(name_, *bin, *ctd, prd ? ByteSpan(*prd) : ByteSpan{}, &err);
  if (!fresh || fresh->layers.size() != scene_->layers.size() || fresh->models.size() != scene_->models.size()) {
    log::warn("room {}: art swap: the other file set does not match ({}); the old art stays", name_, err.empty() ? "layer / model count" : err);
    set_art_mode(hd);
    return false;
  }
  scene_->layers = std::move(fresh->layers);
  scene_->models = std::move(fresh->models);
  scene_->clut_rows = fresh->clut_rows;
  scene_->ctd_tims = fresh->ctd_tims;
  // Effect sprites are cut from the room's composed VRAM (.ctd pages): copy the pixels into the live sheets so the emitters /
  // sequences (which hold pointers into them) keep running; the caller re-uploads the effect atlases.
  size_t copied = 0, skipped = 0;
  auto& old_sheets = scene_->effects.sheets;
  const auto& new_sheets = fresh->effects.sheets;
  for (size_t sheet = 0; sheet < old_sheets.size(); ++sheet) {
    if (sheet >= new_sheets.size() || new_sheets[sheet].name != old_sheets[sheet].name ||
        new_sheets[sheet].frames.size() != old_sheets[sheet].frames.size()) {
      ++skipped;
      continue;
    }
    for (size_t frame = 0; frame < old_sheets[sheet].frames.size(); ++frame) {
      auto& of = old_sheets[sheet].frames[frame];
      const auto& nf = new_sheets[sheet].frames[frame];
      for (size_t k = 0; k < of.size() && k < nf.size(); ++k)
        if (nf[k].w == of[k].w && nf[k].h == of[k].h) {
          of[k].rgba = nf[k].rgba;
          ++copied;
        }
    }
  }
  if (skipped) log::warn("room {}: art swap: {} effect sheets did not match and keep their pixels", name_, skipped);
  hd_ready_ = hd_failed_ = false;
  chip2_.reset();
  items_ = plan_items(*scene_);
  if (!upload_items(compose_1x(*scene_, items_))) {
    log::error("room {}: art swap: texture upload failed", name_);
    return false;
  }
  groups_ = scene_->group_image_count();
  if (hd_) ensure_hd();
  log::info("room {}: art swapped in place ({}): {} textures, {} models, {} effect sprites; no room load", name_, hd_ ? "hd" : "original",
            items_.size(), scene_->models.size(), copied);
  return true;
}

std::vector<u8> RoomView::compose_pixels(const assets::RoomScene& scene, const import::Chip2* chip2, const Item& item, bool hd) {
  // Merge the item's (layer, group) images into one texture covering its 1x rectangle, at 1x or 4x (`chip2` required). Pure CPU:
  // safe to run on worker threads (the HD textures of all items are composed in parallel).
  const int scale = hd ? import::kChip2Scale : 1;
  const size_t w = static_cast<size_t>(item.w * scale), h = static_cast<size_t>(item.h * scale);
  std::vector<u8> px(w * h * 4, 0);
  for (const auto& [layer, group] : item.parts) {
    // BG_LAYER_ON / OFF address the tile *group* (0..31) of a layer, not its index; the mask bit of the layer's group is tested
    // in draw() (items are per group).
    std::optional<assets::RoomGroupImage> hd_img;
    const assets::RoomGroupImage* image = nullptr;
    if (hd) {
      hd_img = import::compose_hd_group(scene, layer, group, *chip2, nullptr, nullptr, item.blend);
      if (hd_img) image = &*hd_img;
    } else {
      for (const auto& cand : scene.layers[layer].groups)
        if (cand.group == group && cand.blend == item.blend) image = &cand;
    }
    if (!image) continue;
    const size_t gx = static_cast<size_t>((image->x - item.x) * scale), gy = static_cast<size_t>((image->y - item.y) * scale);
    for (size_t j = 0; j < image->height; ++j)
      for (size_t i = 0; i < image->width; ++i) {
        const u8* src = &image->rgba[(j * image->width + i) * 4];
        if (!src[3]) continue;
        std::copy(src, src + 4, &px[((gy + j) * w + gx + i) * 4]);
      }
  }
  return px;
}

bool RoomView::upload_set(Item& item, bool hd, const std::vector<u8>& px) {
  const int scale = hd ? import::kChip2Scale : 1;
  const size_t w = static_cast<size_t>(item.w * scale), h = static_cast<size_t>(item.h * scale);
  ScopedTimer upload_t("upload", hd_upload_ms_, false);
  render::TexRef& set = hd ? item.set_hd : item.set;
  // An image kind with no texels in this art set gets no texture; draw() skips the item.
  if (!has_texels(px)) {
    set = {};
    if (hd) item.texture_hd.reset();
    return true;
  }
  if (hd) item.texture_hd = std::make_unique<render::Texture>();
  render::Texture& tex = hd ? *item.texture_hd : item.texture;
  if (!tex.create(*ctx_, static_cast<u32>(w), static_cast<u32>(h), render::TextureFormat::Rgba8, px.data())) return false;
  set = render::TexRef(tex, sampler_);
  return true;
}

bool RoomView::has_group(u32 group) const {
  if (!scene_) return false;
  for (const auto& layer : scene_->layers)
    for (const auto& image : layer.groups)
      if (image.group == group) return true;
  return false;
}

u32 RoomView::fixed_groups() const {
  u32 mask = 0;
  for (size_t g = 0; g < group_scale_.size() && g < 32; ++g)
    if (group_scale_[g] == 0) mask |= 1u << g;
  return mask;
}

size_t RoomView::visible_group_images() const {
  size_t count = 0;
  if (!scene_) return 0;
  for (const auto& layer : scene_->layers)
    for (const auto& image : layer.groups)
      if (image.group >= 32 || ((layer_mask_ >> image.group) & 1)) ++count;
  return count;
}

void RoomView::set_layer_mask(u32 mask) {
  // Every item merges parts of one tile group only (the merge key includes the group), so BG_LAYER_ON/OFF is a pure
  // draw-time toggle of whole items; no texture is rebuilt.
  if (mask != layer_mask_) log::debug("room {}: layer mask {:08X} -> {:08X}", name_, layer_mask_, mask);
  layer_mask_ = mask;
}

std::optional<import::Chip2> RoomView::read_chip2(const vfs::Vfs& files, const std::string& room, std::string* err) {
  std::optional<Bytes> file;
  {
    ScopedTimer timer("room: chip2 zip read");
    file = files.read("hd/map/mapbin/" + room + ".chip2");
  }
  if (!file) {
    *err = "no HD background (hd/map/mapbin/" + room + ".chip2), using 1x";
    return std::nullopt;
  }
  ScopedTimer timer("room: chip2 parse");
  return import::parse_chip2(*file, err);
}

void RoomView::ensure_hd() {
  if (hd_ready_ || hd_failed_ || !loaded_ || !scene_) return;
  // Failed until proven otherwise; draw() then falls back to the 1x set.
  hd_failed_ = true;
  ScopedTimer hd_total("room: HD total (chip2 wait/read+parse+compose+upload)");
  std::string err;
  if (!chip2_ && chip2_future_.valid()) {
    auto fetched = chip2_future_.get();
    chip2_ = std::move(fetched.first);
    err = std::move(fetched.second);
  } else if (!chip2_) {
    chip2_ = read_chip2(*files_, name_, &err);
  }
  if (!chip2_) {
    log::warn("room {}: {}", name_, err);
    return;
  }
  // Compose on worker threads (a sliding window bounds the memory of finished-but-not-uploaded 4x images); the GPU
  // uploads stay on this thread, in item order.
  const size_t window = std::max<size_t>(2, std::thread::hardware_concurrency());
  std::vector<std::future<std::vector<u8>>> jobs(items_.size());
  auto launch = [&](size_t i) {
    jobs[i] = std::async(std::launch::async, [this, i] {
      ScopedTimer timer("compose", nullptr, false);
      return compose_pixels(*scene_, &*chip2_, *items_[i], true);
    });
  };
  double compose_wait_ms = 0, upload_ms = 0;
  hd_upload_ms_ = &upload_ms;
  size_t next = 0;
  for (; next < items_.size() && next < window; ++next) launch(next);
  for (size_t i = 0; i < items_.size(); ++i) {
    std::vector<u8> px;
    {
      ScopedTimer timer("wait", &compose_wait_ms, false);
      px = jobs[i].get();
    }
    if (next < items_.size()) launch(next++);
    if (!upload_set(*items_[i], true, px)) {
      hd_upload_ms_ = nullptr;
      for (auto& job : jobs)
        if (job.valid()) job.wait();
      log::error("room {}: HD texture upload failed", name_);
      return;
    }
  }
  hd_upload_ms_ = nullptr;
  log::debug("timing: room: chip2 4x composition (parallel, main-thread wait): {:.1f} ms, HD texture create: {:.1f} ms",
             compose_wait_ms, upload_ms);
  hd_ready_ = true;
  hd_failed_ = false;
  log::info("room {}: built {} HD (4x) textures", name_, items_.size());
}

void RoomView::set_art_mode(bool hd) {
  if (hd != hd_) drop_prefetch();
  hd_ = hd;
  if (hd_ && loaded_) ensure_hd();
}

void RoomView::scroll_at(float alpha, float& sx, float& sy) const {
  // ONE display scroll per rendered frame for everything (layers, actors, shadows, effects): the retail integer scroll of the previous
  // and the current tick, interpolated by the render alpha. HD art keeps the sub-pixel value (4x texels, nothing shimmers against
  // the interpolated actors); original mode floors it like the PS1 tile placement. There is no second, separately eased camera:
  // two eases (integer + float) drift apart by up to 11 px and interpolating from one to the other every frame shows as a
  // constant shake / doubled picture.
  if (interp_frozen_) alpha = 1.0f;
  sx = static_cast<float>(prev_pan_x_) + static_cast<float>(pan_x_ - prev_pan_x_) * alpha;
  sy = static_cast<float>(prev_pan_y_) + static_cast<float>(pan_y_ - prev_pan_y_) * alpha;
  if (hd_ && hd_ready_) return;
  sx = std::floor(sx);
  sy = std::floor(sy);
}

// 4096 = scrolls with the foreground, larger = slower (farther). Scale 0 = fixed to the screen: its offset is always a multiple of 4096,
// which the console's 11-bit drawing coordinates wrap to nothing.
float RoomView::group_parallax(u32 group) const {
  const u32 scale = group < group_scale_.size() ? group_scale_[group] : 4096u;
  return scale == 0 ? 0.0f : 4096.0f / static_cast<float>(scale);
}

float RoomView::slot_parallax(int slot) const {
  const int group = draw_offset::group_at(layer_depths_, slot);
  return group == draw_offset::kGlobal ? 1.0f : group_parallax(static_cast<u32>(group));
}

void RoomView::draw(render::SpriteBatch& sprites, float vw, float vh, float alpha) const {
  const render::Letterbox lb = letterbox(vw, vh);
  const float scale_x = lb.scale, sy = lb.scale_y, ox = lb.ox, oy = lb.oy;
  float scroll_x, scroll_y;
  scroll_at(alpha, scroll_x, scroll_y);
  // Debug: OC_ROOM_LAYERS = comma list of layer indices to draw (isolates one background layer, like OC_STAGE_ONLY).
  static const char* only_layers = std::getenv("OC_ROOM_LAYERS");
  // [Video] FilterRooms = bilinear: the tiles are sampled bilinearly whatever their (nearest) sampler; not inside a Point / Box pass.
  const bool bg_lin = bg_linear && sprites.filter() == render::SpriteFilter::Sampler;
  if (bg_lin) sprites.set_filter(render::SpriteFilter::Linear);
  for (const auto& it : items_) {
    // The HD set once the room's HD art is built (an empty HD kind stays undrawn, never its 1x texels); sub-pixel placement only with it.
    const bool sub = hd_ && hd_ready_;
    const render::TexRef set = sub ? it->set_hd : it->set;
    if (!set) continue;
    if (only_layers) {
      bool keep = false;
      for (const auto& part : it->parts)
        if (std::strstr(std::format(",{},", only_layers).c_str(), std::format(",{},", part.first).c_str())) keep = true;
      if (!keep) continue;
    }
    // Groups switched off by BG_LAYER_OFF, outside the set_slab depth range (8F split), or in the other set_late pass are skipped.
    if (it->group < 32 && (!((layer_mask_ >> it->group) & 1) || ((hidden_groups_ >> it->group) & 1))) continue;
    if (const float ot = std::max(8.0f, static_cast<float>(it->z)); ot <= slab_lo_ || ot > slab_hi_) continue;
    if ((it->blend >= 0 && std::max(8.0f, static_cast<float>(it->z)) < late_z_) != late_pass_) continue;
    // Parallax: off = S * 4096 / scale.
    const float parallax = group_parallax(it->group);
    float px = static_cast<float>(it->x) + (parallax == 0.0f ? (fixed_to_edge_ ? lb.x0 : 0.0f) : scroll_x * parallax);
    float py = static_cast<float>(it->y) + scroll_y * parallax;
    if (!sub) {
      px = std::floor(px);
      py = std::floor(py);
    }
    // Clip to the 320x224 field picture (widescreen: its visible columns lb.x0 .. lb.x0 + lb.w) by adjusting UVs (SpriteBatch has no scissor).
    const float cx0 = std::max(px, lb.x0), cy0 = std::max(py, 0.0f);
    const float cx1 = std::min(px + static_cast<float>(it->w), lb.x0 + lb.w), cy1 = std::min(py + static_cast<float>(it->h), kViewH);
    if (cx1 <= cx0 || cy1 <= cy0) continue;
    const float fw = static_cast<float>(it->w), fh = static_cast<float>(it->h);
    // Depth in the shared ordering space: the layer z (minimum 8), larger = farther.
    const float depth = std::max(8.0f, static_cast<float>(it->z)) / render::kDepthRange;
    std::array<float, 3> tint = {1, 1, 1};
    if (it->group < group_tint_.size()) tint = group_tint_[it->group];
    if (tint[0] <= 0.003f && tint[1] <= 0.003f && tint[2] <= 0.003f) continue;
    // Semi-transparent tiles (flags bit 7) blend their STP texels with the PS1 ABR mode: 0 = (B + F) / 2, 1 = B + F, 2 = B - F,
    // 3 = B + F / 4. The chip2 HD art draws ABR 0 as RDE does, B + F / 2: additive with the colour halved. The group tint modulates
    // the colour (it fades the glow in / out).
    float kb = 1.0f;
    u8 texel_a = 255;
    render::BlendMode mode = render::BlendMode::Alpha;
    if (it->blend >= 0) {
      switch (it->blend) {
        case 0:
          if (sub) {
            mode = render::BlendMode::Add;
            kb = 0.5f;
          } else {
            mode = render::BlendMode::Average;
            texel_a = 128;
          }
          break;
        case 2: mode = render::BlendMode::Subtract; break;
        case 3: mode = render::BlendMode::Add; kb = 0.25f; break;
        default: mode = render::BlendMode::Add; break;
      }
    }
    const u32 col = render::pack_rgba(static_cast<u8>(std::lround(std::min(1.0f, tint[0] * kb) * 255.0f)),
                                      static_cast<u8>(std::lround(std::min(1.0f, tint[1] * kb) * 255.0f)),
                                      static_cast<u8>(std::lround(std::min(1.0f, tint[2] * kb) * 255.0f)), texel_a);
    sprites.set_blend(mode);
    sprites.draw_depth(set, ox + cx0 * scale_x, oy + cy0 * sy, (cx1 - cx0) * scale_x, (cy1 - cy0) * sy, depth, (cx0 - px) / fw,
                       (cy0 - py) / fh, (cx1 - px) / fw, (cy1 - py) / fh, col);
    // A darkening layer over all 320 columns (the title's fade from black) also covers the widescreen columns beside them: the visible
    // columns, or when the room is composed 4:3 over a widescreen backdrop (the title) the whole widened frame, every row. The strips
    // repeat the layer's middle texel.
    if (it->blend == 2 && px <= 0.0f && px + fw >= render::kLayerW) {
      const render::Letterbox::Rect veil = render::Letterbox::veil_rect(vw, vh);
      const bool whole = render::screen_extra() > render::wide_extra();
      const float x0 = ox + cx0 * scale_x, x1 = ox + cx1 * scale_x, y0 = oy + cy0 * sy, y1 = oy + cy1 * sy;
      const float left = whole ? veil.x : lb.left(), right = whole ? veil.x + veil.w : lb.left() + lb.width();
      const float top = whole ? std::min(veil.y, y0) : y0, bottom = whole ? std::max(veil.y + veil.h, y1) : y1;
      const float u = ((cx0 + cx1) * 0.5f - px) / fw, v = ((cy0 + cy1) * 0.5f - py) / fh;
      if (left < x0) sprites.draw_depth(set, left, top, x0 - left, bottom - top, depth, u, v, u, v, col);
      if (right > x1) sprites.draw_depth(set, x1, top, right - x1, bottom - top, depth, u, v, u, v, col);
      if (top < y0) sprites.draw_depth(set, x0, top, x1 - x0, y0 - top, depth, u, v, u, v, col);
      if (bottom > y1) sprites.draw_depth(set, x0, y1, x1 - x0, bottom - y1, depth, u, v, u, v, col);
    }
  }
  sprites.set_blend(render::BlendMode::Alpha);
  if (bg_lin) sprites.set_filter(render::SpriteFilter::Sampler);
  // The late layers only: the edge fade belongs to the first call.
  if (late_pass_) return;
  // [Video] WideEdgeFade: widescreen shows more columns than the room has (picture_columns): fade the room's cut edges to black.
  if (edge_fade > 0.5f && fade_set_ && loaded_) {
    float c0, c1;
    picture_columns(lb, scroll_x, c0, c1);
    const float fade_width = std::min(edge_fade, (c1 - c0) * 0.5f);
    const float y0 = oy, h = kViewH * sy;
    if (fade_width > 0.5f && c0 > lb.x0 + 0.5f) sprites.draw(fade_set_, ox + c0 * scale_x, y0, fade_width * scale_x, h, 0.0f, 0.0f, 1.0f, 1.0f);
    if (fade_width > 0.5f && c1 < lb.x0 + lb.w - 0.5f)
      sprites.draw(fade_set_, ox + (c1 - fade_width) * scale_x, y0, fade_width * scale_x, h, 1.0f, 0.0f, 0.0f, 1.0f);
  }
}

void RoomView::picture_columns(const render::Letterbox& lb, float scroll_x, float& c0, float& c1) const {
  c0 = lb.x0;
  c1 = lb.x0 + lb.w;
  // 4:3: the 320 columns.
  if (lb.x0 >= 0.0f || !loaded_) return;
  // Widescreen: the picture reaches as far as the layers' tiles go (art_x0_ .. art_x1_, screen columns
  // scroll + x); beyond them the frame stays black (no actor / effect / shadow drawn over it). The camera block's pan limits
  // (-x_max .. 320 - x_min) only bound the scroll: rooms such as man_bo1f paint further than 4:3 panning can reach.
  c0 = std::max(c0, scroll_x + static_cast<float>(art_x0_));
  c1 = std::min(c1, scroll_x + static_cast<float>(art_x1_));
  if (c1 < c0) c1 = c0;
}

render::Rect RoomView::clip_rect(const render::Letterbox& lb, float scroll_x, float fw, float fh, bool rounded) const {
  float c0, c1;
  picture_columns(lb, scroll_x, c0, c1);
  const float left = lb.ox + c0 * lb.scale, width = (c1 - c0) * lb.scale, height = kViewH * lb.scale_y;
  // The host's field pass clip (App::field_clip): rounded edges, clamped to the frame.
  if (rounded) {
    const float x0 = std::max(0.0f, std::round(left)), y0 = std::max(0.0f, std::round(lb.oy));
    return {static_cast<s32>(x0), static_cast<s32>(y0),
            static_cast<u32>(std::max(0.0f, std::min(fw - x0, std::round(width)))), static_cast<u32>(std::max(0.0f, std::min(fh - y0, std::round(height))))};
  }
  // The actor pass scissor: floor of the origin, ceil of the size.
  return {static_cast<s32>(std::max(0.0f, std::floor(left))), static_cast<s32>(std::max(0.0f, std::floor(lb.oy))),
          static_cast<u32>(std::ceil(width)), static_cast<u32>(std::ceil(height))};
}

void RoomView::draw_actors(render::MeshPipeline& meshes, const render::Frame& frame, float alpha,
                           std::span<const Actor* const> actors, std::span<const ActorLook> looks) const {
  if (!loaded_ || actors.empty()) return;
  const render::Letterbox lb = letterbox(static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
  float scroll_x, scroll_y;
  scroll_at(alpha, scroll_x, scroll_y);
  const render::Rect scissor = clip_rect(lb, scroll_x, static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height));
  const render::Mat4 view_proj = camera_.view_proj(scroll_x, scroll_y, lb), view = camera_.view();
  std::vector<render::Mat4> palette;
  meshes.begin(frame, scissor);
  for (size_t ai = 0; ai < actors.size(); ++ai) {
    const Actor* actor = actors[ai];
    if (!actor || !actor->loaded()) continue;
    actor->palette(alpha, palette);
    render::MeshPipeline::DrawParams dp;
    dp.view_proj = view_proj;
    dp.view = view;
    dp.model = actor->model_matrix(alpha);
    dp.palette = palette;
    dp.linear = linear_filter;
    dp.hd = actor->use_hd;
    if (ai < looks.size()) {
      const ActorLook& lk = looks[ai];
      for (int k = 0; k < 3; ++k) {
        dp.tint[k] = lk.tint[k];
        dp.water_tint[k] = lk.water_tint[k];
      }
      dp.water_level = lk.water_level;
    } else {
      for (int k = 0; k < 3; ++k) dp.tint[k] = actor->tint[k];
    }
    meshes.draw(*actor->gpu(), dp);
  }
  meshes.end();
}

}  // namespace oc::game

namespace oc::game {

std::string RoomView::describe_items() const {
  std::string out;
  for (const auto& it : items_) {
    const float ga = it->group < group_tint_.size() ? group_tint_[it->group][0] : 1.0f;
    out += std::format("  layer item group {} blend {} z {} (depth {}) rect {},{} {}x{} tint {:.2f} parts {}\n", it->group, it->blend, it->z, std::max(8, static_cast<int>(it->z)), it->x,
                       it->y, it->w, it->h, static_cast<double>(ga), it->parts.size());
  }
  return out;
}

void RoomView::set_group_tint(size_t group, int r, int g, int b) {
  if (group >= 256) return;
  if (group_tint_.size() <= group) group_tint_.resize(group + 1, {1.0f, 1.0f, 1.0f});
  group_tint_[group] = {std::clamp(static_cast<float>(r & 0xFF) / 128.0f, 0.0f, 1.0f), std::clamp(static_cast<float>(g & 0xFF) / 128.0f, 0.0f, 1.0f),
                        std::clamp(static_cast<float>(b & 0xFF) / 128.0f, 0.0f, 1.0f)};
}

}  // namespace oc::game
