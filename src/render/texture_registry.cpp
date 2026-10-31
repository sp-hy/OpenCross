#include "render/texture_registry.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>

#include "core/log.h"
#include "import/tim.h"
#include "render/texture.h"
#include "vfs/vfs.h"

#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

namespace oc::render {

namespace {

std::string sanitise(std::string_view text) {
  std::string out;
  for (char ch : text) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (ch == '\\') ch = '/';
    else if (std::isalnum(uch)) ch = static_cast<char>(std::tolower(uch));
    else if (ch != '/' && ch != '.' && ch != '-' && ch != '_') ch = '_';
    out.push_back(ch);
  }
  // Drop empty, "." and ".." components.
  std::string res;
  size_t start = 0;
  while (start <= out.size()) {
    size_t slash = out.find('/', start);
    if (slash == std::string::npos) slash = out.size();
    const std::string part = out.substr(start, slash - start);
    if (!part.empty() && part != "." && part != "..") res += (res.empty() ? "" : "/") + part;
    start = slash + 1;
  }
  return res;
}

struct State {
  std::mutex mutex;
  texture_registry::Settings settings;
  const vfs::Vfs* vfs = nullptr;
  std::filesystem::path dump_root;
  std::set<std::string> dumped, replaced, dumped_text;
  u32 failed = 0;
};

State& state() {
  static State instance;
  return instance;
}

}  // namespace

std::string texture_path(const TextureKey& key) {
  std::string stem;
  char idx[16] = {};
  if (key.index >= 0) std::snprintf(idx, sizeof idx, "%02d", key.index);
  stem = idx;
  if (!key.variant.empty()) stem += (stem.empty() ? "" : "_") + key.variant;
  if (stem.empty()) stem = "texture";
  std::string dir = key.category;
  if (!key.name.empty()) dir += "/" + key.name;
  return sanitise(dir + "/" + stem + ".png");
}

TextureKey stage_texture_key(const std::string& category, const std::string& name, int index, int mode, int clut_x, int clut_y) {
  return {category, name, index, "m" + std::to_string(mode) + "_c" + std::to_string(clut_x) + "x" + std::to_string(clut_y)};
}

bool is_content_named(const std::string& category) {
  return category == "effect_mesh" || category == "room_mesh" || category == "room_model";
}

TextureKey stage_group_key(const std::string& group, int index, int mode, int clut_x, int clut_y, u32 w, u32 h, const u8* rgba) {
  const size_t slash = group.find('/');
  const std::string category = group.substr(0, slash), name = slash == std::string::npos ? std::string() : group.substr(slash + 1);
  if (is_content_named(category)) return {category, "", -1, texture_content_tag(w, h, rgba)};
  return stage_texture_key(category, name, index, mode, clut_x, clut_y);
}

TextureKey raw_atlas_key(const std::string& group, int index, int mode, int clut_x, int clut_y) {
  const size_t slash = group.find('/');
  return stage_texture_key("raw/" + group.substr(0, slash), slash == std::string::npos ? std::string() : group.substr(slash + 1), index, mode, clut_x, clut_y);
}

TextureKey model_texture_key(const std::string& source, size_t index, size_t main_count, u32 w, u32 h, const u8* rgba) {
  if (!source.empty() && source[0] == '@') return {source.substr(1), "", -1, texture_content_tag(w, h, rgba)};
  // A weapon pack part has one texture: battle/wepon/<pack>/part<n> -> model/battle/wepon/<pack>/part<n>.png.
  if (source.starts_with("battle/wepon/")) {
    const size_t slash = source.find_last_of('/');
    return {"model", source.substr(0, slash), -1, source.substr(slash + 1)};
  }
  return {"model", source, -1, index < main_count ? "clut" + std::to_string(index) : "overlay" + std::to_string(index - main_count + 1)};
}

namespace {
// Alpha of a texel: 0 = transparent, below the 0xBF / 0xFF midpoint = STP set (mesh.frag's rule).
bool stp_alpha(u8 alpha) { return alpha < 0xDF; }
}  // namespace

std::vector<u8> model_file_pixels(u32 w, u32 h, const u8* rgba) {
  std::vector<u8> out(rgba, rgba + size_t(w) * h * 4);
  for (size_t i = 3; i < out.size(); i += 4) out[i] = out[i] ? u8{0xFF} : u8{0};
  return out;
}

void model_restore_stp(u32 original_w, u32 original_h, const u8* original, u32 w, u32 h, std::vector<u8>& rgba) {
  size_t stp = 0, plain = 0;
  for (size_t i = 3; i < size_t(original_w) * original_h * 4; i += 4)
    if (original[i]) ++(stp_alpha(original[i]) ? stp : plain);
  // Where the original texel is transparent (art painted past the old edge), the more common flag.
  const bool common = stp >= plain, uniform = stp == 0 || plain == 0;
  for (u32 y = 0; y < h; ++y)
    for (u32 x = 0; x < w; ++x) {
      u8& alpha = rgba[(size_t(y) * w + x) * 4 + 3];
      if (alpha < 0x80) {
        alpha = 0;
        continue;
      }
      bool flag = common;
      if (!uniform) {
        const u8 under = original[(size_t(y * original_h / h) * original_w + x * original_w / w) * 4 + 3];
        if (under) flag = stp_alpha(under);
      }
      alpha = flag ? import::kStpAlpha : u8{0xFF};
    }
}

std::string texture_content_tag(u32 w, u32 h, const u8* rgba) {
  u32 hash = 2166136261u;
  const auto mix = [&](u32 value) {
    for (int i = 0; i < 4; ++i) hash = (hash ^ ((value >> (8 * i)) & 0xFF)) * 16777619u;
  };
  mix(w);
  mix(h);
  const size_t byte_count = static_cast<size_t>(w) * h * 4;
  for (size_t i = 0; i < byte_count; ++i) hash = (hash ^ rgba[i]) * 16777619u;
  char buf[16];
  std::snprintf(buf, sizeof buf, "%08x", hash);
  return buf;
}

namespace texture_registry {

void configure(const Settings& new_settings) {
  std::lock_guard lock(state().mutex);
  state().settings = new_settings;
}
Settings settings() {
  std::lock_guard lock(state().mutex);
  return state().settings;
}
void set_dump_root(const std::filesystem::path& root) {
  std::lock_guard lock(state().mutex);
  state().dump_root = root;
}
void bind(const vfs::Vfs* vfs) {
  std::lock_guard lock(state().mutex);
  state().vfs = vfs;
  if (vfs && state().dump_root.empty()) state().dump_root = vfs->game_dir() / "dump" / "textures";
}

const vfs::Vfs* bound_vfs() {
  std::lock_guard lock(state().mutex);
  return state().vfs;
}

bool active() {
  const vfs::Vfs* vfs;
  {
    std::lock_guard lock(state().mutex);
    if (state().settings.dump) return true;
    if (!state().settings.replace) return false;
    vfs = state().vfs;
  }
  return vfs && vfs->has_textures();
}

static bool dump_impl(const TextureKey& key, u32 w, u32 h, const u8* rgba, bool forced) {
  State& st = state();
  if (!w || !h || !rgba) return false;
  const std::string path = texture_path(key);
  std::filesystem::path file;
  {
    std::lock_guard lock(st.mutex);
    if ((!forced && !st.settings.dump) || st.dump_root.empty()) return false;
    if (!st.dumped.insert(path).second) return false;
    file = st.dump_root / std::filesystem::path(std::u8string(path.begin(), path.end()));
  }
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  std::ofstream out(file, std::ios::binary);
  bool ok = false;
  if (out) {
    const auto sink = [](void* ctx, void* data, int size) { static_cast<std::ofstream*>(ctx)->write(static_cast<const char*>(data), size); };
    ok = stbi_write_png_to_func(sink, &out, static_cast<int>(w), static_cast<int>(h), 4, rgba, static_cast<int>(w * 4)) != 0 && static_cast<bool>(out);
  }
  if (!ok) {
    std::lock_guard lock(st.mutex);
    ++st.failed;
    log::warn("textures: cannot write {}", file.string());
  }
  return ok;
}

bool dump(const TextureKey& key, u32 w, u32 h, const u8* rgba) { return dump_impl(key, w, h, rgba, false); }
bool dump_forced(const TextureKey& key, u32 w, u32 h, const u8* rgba) { return dump_impl(key, w, h, rgba, true); }

unsigned generation() {
  const vfs::Vfs* vfs;
  {
    std::lock_guard lock(state().mutex);
    vfs = state().vfs;
  }
  return vfs ? static_cast<unsigned>(vfs->generation()) : 0u;
}

bool dump_text(const std::string& rel_path, const std::string& text, bool forced) {
  State& st = state();
  std::filesystem::path file;
  const std::string path = sanitise(rel_path);
  {
    std::lock_guard lock(st.mutex);
    if ((!forced && !st.settings.dump) || st.dump_root.empty()) return false;
    if (!st.dumped_text.insert(path).second) return false;
    file = st.dump_root / std::filesystem::path(std::u8string(path.begin(), path.end()));
  }
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  std::ofstream out(file, std::ios::binary);
  if (out) out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return out && static_cast<bool>(out);
}

static bool load_replacement(const vfs::Vfs& vfs, const vfs::OverlayFile& file, const std::string& path, u32& w, u32& h, std::vector<u8>& rgba) {
  const auto bytes = vfs.read_mod_file(file);
  if (!bytes) return false;
  DecodedImage img;
  if (!decode_image(ByteSpan(*bytes), img) || !img.width || !img.height) {
    log::warn("textures: cannot decode replacement {}", file.label());
    return false;
  }
  w = img.width;
  h = img.height;
  rgba = std::move(img.rgba);
  State& st = state();
  std::lock_guard lock(st.mutex);
  if (st.replaced.insert(path).second) log::info("textures: replaced {} ({}x{}) by {}", path, w, h, file.label());
  return true;
}

bool replace_tile(const std::string& dir, const std::string& hash, u32& w, u32& h, std::vector<u8>& rgba) {
  State& st = state();
  const vfs::Vfs* vfs;
  {
    std::lock_guard lock(st.mutex);
    if (!st.settings.replace || !st.vfs) return false;
    vfs = st.vfs;
  }
  const std::string clean_dir = sanitise(dir);
  if (auto file = vfs->texture_file_matching(clean_dir + "/tile_", hash + ".png")) {
    if (load_replacement(*vfs, *file, clean_dir + "/tile_" + hash, w, h, rgba)) return true;
  }
  const std::string global = "tiles/" + hash + ".png";
  if (auto file = vfs->texture_file(global)) return load_replacement(*vfs, *file, global, w, h, rgba);
  return false;
}

bool replace(const TextureKey& key, u32& w, u32& h, std::vector<u8>& rgba) {
  State& st = state();
  const vfs::Vfs* vfs;
  {
    std::lock_guard lock(st.mutex);
    if (!st.settings.replace || !st.vfs) return false;
    vfs = st.vfs;
  }
  const std::string path = texture_path(key);
  const auto file = vfs->texture_file(path);
  return file && load_replacement(*vfs, *file, path, w, h, rgba);
}

Stats stats() {
  std::lock_guard lock(state().mutex);
  return {static_cast<u32>(state().dumped.size()), static_cast<u32>(state().replaced.size()), state().failed};
}
void begin_dump() {
  std::lock_guard lock(state().mutex);
  state().dumped.clear();
  state().dumped_text.clear();
}

void reset_stats() {
  std::lock_guard lock(state().mutex);
  state().dumped.clear();
  state().dumped_text.clear();
  state().replaced.clear();
  state().failed = 0;
}

}  // namespace texture_registry
}  // namespace oc::render
