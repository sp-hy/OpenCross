#include "ui/menu/status_model.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>

#include "core/log.h"
#include "game/battle/model_table.h"
#include "import/container.h"
#include "import/model.h"
#include "import/room_effects.h"
#include "import/tim.h"
#include "render/texture_registry.h"

namespace oc::ui {

namespace {
// Menu models 0..0x32 follow the battle model order (the player block); 0x32 carries a second weapon on mount record 1.
constexpr int kLastMenuModel = 0x32;
// The menu model is narrowed along its own x axis (model scale 0xCCC / 4096, y and z 1).
constexpr float kModelScaleX = 3276.0f / 4096.0f;
// Projection of the menu screen: screen centre and projection-plane distance in PS1 pixels.
constexpr float kCentreX = 320, kCentreY = 240, kProjection = 512;
// Camera-space position of the model's feet before the pan: right of and below the view axis.
constexpr float kFeetRight = 100, kFeetBelow = 500;
// The floor shadow: every vertex slides along this ray (PS1 y down) onto the floor y = 0, flat polygons of grey 0x20.
constexpr int kShadowRay[3] = {-100, 150, -150};
constexpr float kShadowGrey = 32.0f / 255.0f;
// [Enhancements] Lighting: the share of the parchment's light the cast shadow removes, and how far of the feet it can fall.
constexpr float kCastStrength = 0.6f, kCastReach = 3072.0f;
// Characters whose weapon object is hidden on this screen (its shadow is still cast): Luccia, Harle, Starky, Sneff.
constexpr int kHiddenWeapon[4] = {0x0B, 0x16, 0x19, 0x25};
// Weapon tier byte of the prism weapons (drawn under the turning coloured lights instead of the glint).
constexpr int kPrismTier = 5;
// The glint's light, the battle stage default (PS1 y down).
constexpr float kWeaponLight[3] = {0, -3072, 0};
// The square-button poses blend over this many pose steps (the menu's 24-frame blend; two steps per menu tick, so 12 ticks).
constexpr int kPoseBlend = 24;
// Eye blink: shut for 4 menu ticks, then open for 40 + rand() % 20.
constexpr int kBlinkShut = 4, kBlinkOpen = 40, kBlinkOpenRange = 20;
// Closed-eye blocks: 32 x 32 texels each, the 8 bpp columns 64..95 and 96..127 of the eye TIM (cpt entry 4).
constexpr u32 kEyeBlock = 32, kEyeColumn[2] = {64, 96};
// The stock environment page (8 bpp, VRAM (320, 256), CLUT row 239) lies inside the menu's own frame buffer: the parchment of 64 x 64
// tiles from (0, 0), drawn with the mask bit set; frame rows from 448 on are outside the screen (zero).
constexpr int kEnvPageX = 320, kEnvPageY = 256, kEnvClutRow = 239, kFrameRows = 448, kParchmentTile = 64;
// Status effects of menu/stef.prd: status bit -> emitters.
struct StatusEffect {
  u32 bit;
  const char* emitter;
};
constexpr StatusEffect kStatusEffects[4] = {{0x01, "d01"}, {0x01, "d02"}, {0x20, "kaze"}, {0x08, "yake"}};
// At most this many menu ticks are caught up in one draw; the interpolation stops short of the next tick.
constexpr u32 kMaxSteps = 8;
// The camera's auto turn per menu tick.
constexpr int kAutoTurn = 8;
// Menu model ticks per second: one on every second 60 Hz frame.
constexpr float kTickRate = 30.0f;
constexpr float kMaxAlpha = 0.999f;
constexpr float kTurnDegrees = 360.0f / 4096.0f;
constexpr float kPi = 3.14159265358979323846f;

std::string model_stem(int model) {
  if (model < 0 || model > kLastMenuModel) return {};
  const std::string_view path = game::kBattleModels[static_cast<size_t>(model)];
  return std::string(path.substr(path.rfind('/') + 1));
}

// The camera rotation Rx(-pitch) * Ry(-yaw), row-major.
assets::Mat3 camera_rotation(const MenuCamera& camera) {
  const assets::Mat3 rx = assets::rotation_zyx({-static_cast<float>(camera.pitch & 0xFFF) * kTurnDegrees, 0, 0});
  const assets::Mat3 ry = assets::rotation_zyx({0, -static_cast<float>(camera.yaw & 0xFFF) * kTurnDegrees, 0});
  assets::Mat3 out{};
  for (size_t row = 0; row < 3; ++row)
    for (size_t col = 0; col < 3; ++col)
      for (size_t k = 0; k < 3; ++k) out[row * 3 + col] += rx[row * 3 + k] * ry[k * 3 + col];
  return out;
}

// The closed-eye copy of the model's texture page. The 4x TIM of the HD art holds both pages side by side (right half shut); the disc
// page gets the two eye blocks of the eye TIM pasted at the halfword positions of cpt entry 1 (x1 y1 x2 y2), through the model's CLUT.
std::optional<assets::ModelAsset> shut_eyes(const assets::ModelAsset& open, ByteSpan positions, ByteSpan eye_tim, ByteSpan atim) {
  assets::ModelAsset shut = open;
  auto right_half_left = [](import::Image& image) {
    const size_t row_bytes = static_cast<size_t>(image.width) * 4, half = row_bytes / 2;
    for (u32 y = 0; y < image.height; ++y) {
      u8* row = image.pixels.data() + y * row_bytes;
      std::copy(row + half, row + row_bytes, row);
    }
  };
  if (!open.hd_textures.empty() && open.hd_textures[0].width && atim.empty()) {
    right_half_left(shut.hd_textures[0]);
    if (!shut.textures.empty() && !shut.textures[0].indexed) right_half_left(shut.textures[0]);
    return shut;
  }
  const auto main = import::load_atim_main(atim);
  const auto tim = import::parse_tim(eye_tim);
  if (!main || !tim || tim->bpp != 8 || positions.size() < 4 || shut.textures.empty()) return std::nullopt;
  import::Image& page = shut.textures[0];
  if (page.indexed || page.width != main->width || page.height != main->height) return std::nullopt;
  const u32 tim_w = static_cast<u32>(tim->image.w) * 2;
  for (size_t block = 0; block < 2; ++block) {
    const u32 dest_x = static_cast<u32>(positions[2 * block]) * 2, dest_y = positions[2 * block + 1];
    for (u32 y = 0; y < kEyeBlock && y < tim->image.h; ++y)
      for (u32 x = 0; x < kEyeBlock; ++x) {
        const u32 src_x = kEyeColumn[block] + x, out_x = dest_x + x, out_y = dest_y + y;
        if (src_x >= tim_w || out_x >= page.width || out_y >= page.height) continue;
        const u16 word = tim->image.data[static_cast<size_t>(y) * tim->image.w + src_x / 2];
        const u8 index = static_cast<u8>((src_x & 1) ? word >> 8 : word & 0xFF);
        const u32 colour = index < main->clut.size() ? import::bgr555_to_rgba_stp(main->clut[index]) : 0u;
        u8* dst = &page.pixels[(static_cast<size_t>(out_y) * page.width + out_x) * 4];
        for (int k = 0; k < 4; ++k) dst[k] = static_cast<u8>(colour >> (8 * k));
      }
  }
  return shut;
}

// Texture dump / replace of an HD art menu model (model/menu/player/<stem>/clut0.png): its 4x image, both pages. Done here, not by the mesh
// pipeline, so the shut-eye copy is cut from the replacement too; the replacement stands in for the 1x texture as well.
void replace_menu_texture(assets::ModelAsset& asset, const std::string& stem) {
  namespace tr = render::texture_registry;
  if (!tr::active() || !asset.has_hd(0) || asset.hd_textures[0].indexed || asset.textures.empty()) return;
  import::Image& hd = asset.hd_textures[0];
  const render::TextureKey key = render::model_texture_key("menu/player/" + stem, 0, 1, hd.width, hd.height, hd.pixels.data());
  if (tr::settings().dump) tr::dump(key, hd.width, hd.height, render::model_file_pixels(hd.width, hd.height, hd.pixels.data()).data());
  u32 w = hd.width, h = hd.height;
  std::vector<u8> rgba = hd.pixels;
  if (!tr::replace(key, w, h, rgba)) return;
  render::model_restore_stp(hd.width, hd.height, hd.pixels.data(), w, h, rgba);
  hd.width = w, hd.height = h;
  hd.pixels = std::move(rgba);
  asset.textures[0] = hd;
}

// The menu's stock environment page as a 256 x 256 8 bpp texture: texel (u, v) is byte u & 1 of the frame word at (320 + u / 2, 256 + v),
// coloured by the frame word at (byte, 239). The parchment frame comes from the tile the menu uploads (hd/map/etc/tile.tim, its 4x image
// sampled every 4th texel at 1x, CLUT word | 0x8000).
std::optional<import::Image> menu_env_page(const vfs::Vfs& vfs) {
  const auto raw = vfs.read("hd/map/etc/tile.tim");
  if (!raw) return std::nullopt;
  const auto tim = import::parse_tim(*raw);
  if (!tim || tim->bpp != 4 || !tim->has_clut || tim->clut.data.size() < 16) return std::nullopt;
  const u32 width = static_cast<u32>(tim->image.w) * 4, height = tim->image.h, step = width / kParchmentTile;
  if (step == 0 || height < kParchmentTile * step) return std::nullopt;
  auto frame = [&](int x, int y) -> u16 {
    if (y >= kFrameRows) return 0;
    const u32 tx = static_cast<u32>(x % kParchmentTile) * step, ty = static_cast<u32>(y % kParchmentTile) * step;
    const u16 word = tim->image.data[static_cast<size_t>(ty) * tim->image.w + tx / 4];
    return static_cast<u16>(tim->clut.data[(word >> (4 * (tx & 3))) & 15] | 0x8000);
  };
  import::Image page;
  page.width = page.height = 256;
  page.pixels.resize(256 * 256 * 4);
  for (int v = 0; v < 256; ++v)
    for (int u = 0; u < 256; ++u) {
      const u16 word = frame(kEnvPageX + u / 2, kEnvPageY + v);
      const int index = (u & 1) ? word >> 8 : word & 0xFF;
      const u32 colour = import::bgr555_to_rgba_stp(frame(index, kEnvClutRow));
      u8* dst = &page.pixels[(static_cast<size_t>(v) * 256 + static_cast<size_t>(u)) * 4];
      for (int k = 0; k < 4; ++k) dst[k] = static_cast<u8>(colour >> (8 * k));
    }
  return page;
}

// Prism weapon lighting (GTE NCCS, no glint): three light rows turned every menu frame by x += 0x20, z += 0x40 of 4096 (RotMatrixZYX
// applied to (0, 1, 0), (-0.707, -0.707, 0), (0.707, -0.707, 0)); the light matrix is the weapon's world matrix W times those rows and it
// lights the NEGATED vertex normal; the colour matrix sends light 0 to blue, 1 to green, 2 to red at 0.75; back colour 0.75. The shader dots
// dir_i with W * normal, so dir_i = -W^-T * (W L)_i.
void prism_light(const render::Mat4& world, int ticks, render::MeshPipeline::DrawParams& wd) {
  constexpr float kDiag = 2896.0f / 4096.0f;
  constexpr float kBase[3][3] = {{0.0f, 1.0f, 0.0f}, {-kDiag, -kDiag, 0.0f}, {kDiag, -kDiag, 0.0f}};
  const assets::Mat3 spin = assets::rotation_zyx({static_cast<float>((0x20 * ticks) & 0xFFF) * kTurnDegrees, 0.0f,
                                                  static_cast<float>((0x40 * ticks) & 0xFFF) * kTurnDegrees});
  float rows[3][3];
  for (size_t j = 0; j < 3; ++j)
    for (size_t k = 0; k < 3; ++k) rows[j][k] = spin[k * 3] * kBase[j][0] + spin[k * 3 + 1] * kBase[j][1] + spin[k * 3 + 2] * kBase[j][2];
  float w[3][3];
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) w[row][col] = world.at(row, col);
  const float det = w[0][0] * (w[1][1] * w[2][2] - w[1][2] * w[2][1]) - w[0][1] * (w[1][0] * w[2][2] - w[1][2] * w[2][0]) +
                    w[0][2] * (w[1][0] * w[2][1] - w[1][1] * w[2][0]);
  if (std::fabs(det) < 1e-9f) return;
  // inverse_t[r][c] = (W^-1)^T[r][c] = cofactor(r, c) / det.
  float inverse_t[3][3];
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) {
      const int r0 = (row + 1) % 3, r1 = (row + 2) % 3, c0 = (col + 1) % 3, c1 = (col + 2) % 3;
      inverse_t[row][col] = (w[r0][c0] * w[r1][c1] - w[r0][c1] * w[r1][c0]) / det;
    }
  wd.light.on = true;
  wd.light.spec = false;
  for (int i = 0; i < 3; ++i) {
    float llm[3];
    for (size_t k = 0; k < 3; ++k) {
      llm[k] = 0.0f;
      for (int j = 0; j < 3; ++j) llm[k] += w[i][j] * rows[j][k];
    }
    for (int row = 0; row < 3; ++row) wd.light.dir[i][row] = -(inverse_t[row][0] * llm[0] + inverse_t[row][1] * llm[1] + inverse_t[row][2] * llm[2]);
    wd.light.bk[i] = 0.75f;
    for (size_t c = 0; c < 3; ++c) wd.light.rgb[i][c] = c == static_cast<size_t>(2 - i) ? 0.75f : 0.0f;
  }
}
}  // namespace

StatusModelView::Entry* StatusModelView::entry(int model, bool hd_art) {
  const std::string stem = model_stem(model);
  if (stem.empty()) return nullptr;
  const int key = model * 2 + (hd_art ? 1 : 0);
  if (auto it = cache_.find(key); it != cache_.end()) return it->second.get();
  auto created = std::make_unique<Entry>();
  auto ome = vfs_.read("cdrom/menu/player/" + stem + ".ome");
  // The 4x HD texture is used only in HD art mode: original art keeps the disc texture.
  std::optional<Bytes> hd;
  if (hd_art) hd = vfs_.read("hd/menu/player/" + stem + ".tim");
  if (ome) {
    if (auto cpt = import::parse_cpt(*ome); cpt && cpt->entries.size() >= 6) {
      std::string err;
      // The menu Mdl has a short header: the disc file has 5 sections (S1 at 0x1C), the HD art mode's variant 4
      // sections (S1 at 0x18, no S5; its "EOF" word equals S4, the anim pack that follows it, so the word cannot be trusted). Rebuild the
      // standard 6-section header (S1 at 0x20, missing sections 0, EOF = the real size).
      Bytes mdl(cpt->entries[3].data.begin(), cpt->entries[3].data.end());
      if (mdl.size() > 0x20 && (mdl[0] == 4 || mdl[0] == 5)) {
        const size_t nsec = mdl[0], old_hdr = 4 * (nsec + 2), shift = 0x20 - old_hdr;
        Bytes fixed(mdl.size() + shift, 0);
        auto rd = [&](size_t offset) {
          return static_cast<u32>(mdl[offset] | mdl[offset + 1] << 8 | mdl[offset + 2] << 16 | static_cast<u32>(mdl[offset + 3]) << 24);
        };
        auto wr = [&](size_t offset, u32 value) {
          for (int k = 0; k < 4; ++k) fixed[offset + static_cast<size_t>(k)] = static_cast<u8>(value >> (8 * k));
        };
        wr(0, 6);
        // A missing section (offset 0, e.g. the S5 of most disc models) stays missing.
        for (size_t k = 0; k < nsec; ++k)
          if (const u32 offset = rd(4 + 4 * k)) wr(4 + 4 * k, offset + static_cast<u32>(shift));
        wr(28, static_cast<u32>(fixed.size()));
        std::copy(mdl.begin() + static_cast<std::ptrdiff_t>(old_hdr), mdl.end(), fixed.begin() + 0x20);
        mdl = std::move(fixed);
      }
      // Entry 3 = Mdl, entry 5 = Atim cpt (main texture + overlay); entry 4 is the closed-eye TIM. The importer wants the Mdl bytes only.
      if (auto asset = import::load_model(mdl, cpt->entries[5].data, ByteSpan{}, &err, hd ? ByteSpan(*hd) : ByteSpan{})) {
        // HD art only: the original art's menu model is another mesh with the disc texture.
        if (cpt->entries[5].data.empty()) replace_menu_texture(*asset, stem);
        if (auto shut = shut_eyes(*asset, cpt->entries[1].data, cpt->entries[4].data, cpt->entries[5].data))
          created->body_shut = game::Actor::make_model(meshes_, std::move(*shut));
        created->body = game::Actor::make_model(meshes_, std::move(*asset));
      } else {
        log::warn("status model {}: {}", stem, err);
      }
    }
  }
  if (created->body) created->pose.set_model(&created->body->asset);
  Entry* result = created.get();
  cache_[key] = std::move(created);
  return result;
}

int StatusModelView::min_distance(int model) {
  if (auto it = min_distance_.find(model); it != min_distance_.end()) return it->second;
  int value = 0;
  if (const std::string stem = model_stem(model); !stem.empty())
    if (auto ome = vfs_.read("cdrom/menu/player/" + stem + ".ome"))
      if (auto cpt = import::parse_cpt(*ome); cpt && cpt->entries.size() > 2) {
        const auto& text = cpt->entries[2].data;
        const char* first = reinterpret_cast<const char*>(text.data());
        std::from_chars(first, first + text.size(), value);
      }
  min_distance_[model] = value;
  return value;
}

void StatusModelView::set_weapon(Entry& shown, int weapon_model, int part) {
  if (shown.weapon_model == weapon_model && shown.weapon_part == part) return;
  shown.weapon_model = weapon_model;
  shown.weapon_part = part;
  shown.weapon.reset();
  if (part < 0 || weapon_model < 0 || weapon_model > kLastMenuModel) return;
  const std::string_view bin = game::kBattleWeaponBins[static_cast<size_t>(weapon_model)];
  if (bin.empty()) return;
  const std::string key = std::string(bin) + "#" + std::to_string(part);
  if (auto it = weapons_.find(key); it != weapons_.end()) {
    shown.weapon = it->second;
    return;
  }
  std::shared_ptr<game::ModelHandle> handle;
  if (auto raw = vfs_.read("cdrom/" + std::string(bin))) {
    std::string err;
    if (auto pack = import::load_weapon_pack(*raw, &err); pack && part < static_cast<int>(pack->parts.size())) {
      assets::ModelAsset weapon_asset;
      weapon_asset.kind = "weapon";
      weapon_asset.source = game::weapon_texture_source(bin, part);
      weapon_asset.skeleton.joints.emplace_back();
      weapon_asset.textures.push_back(pack->parts[static_cast<size_t>(part)].texture);
      weapon_asset.main_texture_count = 1;
      weapon_asset.meshes.push_back(pack->parts[static_cast<size_t>(part)].mesh);
      handle = game::Actor::make_model(meshes_, std::move(weapon_asset));
    } else {
      log::warn("status weapon {}: {}", key, err.empty() ? "no such part" : err);
    }
  }
  weapons_[key] = handle;
  shown.weapon = handle;
}

void StatusModelView::release() {
  cache_.clear();
  weapons_.clear();
  shadow_pipe_.destroy();
  env_page_.destroy();
  env_sampler_.destroy();
  env_page_tried_ = false;
  emitters_.reset();
  effect_gpu_.clear();
  particles_.destroy();
  effect_sprites_.destroy();
  effects_ready_ = false;
}

void StatusModelView::step_blink() {
  if (!eyes_shut_) {
    if (blink_count_ == 0) eyes_shut_ = true, blink_count_ = kBlinkShut;
  } else if (blink_count_ == 0) {
    eyes_shut_ = false;
    blink_count_ = static_cast<int>(blink_rng_() % kBlinkOpenRange) + kBlinkOpen;
  }
  --blink_count_;
}

// Every session (screen entry, member change) loads the pack afresh: all its emitters run from registration, then those of the statuses
// the character does not have are stopped. Emitters bound to the actor follow the model's attach points (selector point - 2), at the
// model's origin otherwise.
void StatusModelView::start_effects(const render::Frame& frame, u32 status) {
  if (!effects_ready_) {
    if (!frame.ctx) return;
    effects_ready_ = effect_sprites_.init(*frame.ctx) && particles_.init(*frame.ctx) && effect_gpu_.init(*frame.ctx, effect_sprites_);
    if (!effects_ready_) return;
    if (auto pack = vfs_.read("cdrom/menu/stef.prd")) {
      import::Vram empty;
      effect_pack_ = import::load_room_effects(*pack, empty);
      effect_pack_loaded_ = true;
      effect_bank_.add(&effect_pack_);
      effect_gpu_.load(effect_bank_);
    }
  }
  if (!effect_pack_loaded_) return;
  emitters_ = std::make_unique<effects::EmitterSystem>(effect_bank_);
  effects::ActorHooks hooks;
  hooks.actor_basis = [this](int, int point, int, int, effects::Basis& out, int&) {
    out = effects::Basis{};
    if (!effect_model_ || point < 2 || point - 2 >= static_cast<int>(effect_model_->attach.points.size())) return true;
    const assets::AttachPoint& attach = effect_model_->attach.points[static_cast<size_t>(point - 2)];
    if (attach.joint >= effect_palette_.size()) return true;
    const render::Mat4& joint = effect_palette_[attach.joint];
    for (int row = 0; row < 3; ++row)
      out.t[static_cast<size_t>(row)] = (joint.at(row, 0) * attach.pos[0] + joint.at(row, 1) * attach.pos[1] + joint.at(row, 2) * attach.pos[2] +
                                         joint.at(row, 3)) *
                                        (row == 0 ? kModelScaleX : 1.0f);
    return true;
  };
  hooks.anchor_basis = [](int, effects::Basis& out) {
    out = effects::Basis{};
    out.m = effects::Mat3::identity().scaled_columns(kModelScaleX, 1.0f, 1.0f);
    return true;
  };
  emitters_->set_hooks(std::move(hooks));
  emitters_->register_all();
  for (const StatusEffect& effect : kStatusEffects)
    if ((status & effect.bit) == 0) emitters_->kill(effect.emitter);
}

void StatusModelView::draw_effects(const render::Frame& frame, float vw, float vh, const MenuModelSlot& slot, const effects::Mat3& rotation,
                                   float alpha) {
  if (!emitters_ || !effects_ready_) return;
  effects::ViewParams view;
  view.rot = rotation;
  view.trans = {static_cast<float>(slot.camera.pan_x) + kFeetRight, static_cast<float>(slot.camera.pan_y) + kFeetBelow,
                static_cast<float>(slot.camera.distance)};
  view.h = kProjection;
  view.ox = kCentreX, view.oy = kCentreY;
  std::vector<effects::DrawQuad> quads;
  emitters_->collect_quads(view, alpha, quads);
  if (quads.empty()) return;
  effects::sort_draw_order(quads);
  render::Letterbox letterbox;
  letterbox.ox = slot.origin_x, letterbox.oy = slot.origin_y;
  letterbox.scale = slot.unit_x, letterbox.scale_y = slot.unit_y;
  letterbox.vw = vw, letterbox.vh = vh;
  particles_.begin(frame, vw, vh);
  effect_gpu_.draw(particles_, quads, letterbox);
  particles_.end();
}

bool StatusModelView::shadow_pipe_ready(const render::Frame& frame) {
  if (!cast_shadows_ || shadow_pipe_failed_ || !frame.ctx) return false;
  if (shadow_pipe_.ready()) return true;
  shadow_pipe_failed_ = !shadow_pipe_.init(*frame.ctx);
  if (shadow_pipe_failed_) log::warn("status model: shadow pipeline failed, flat shadow");
  return !shadow_pipe_failed_;
}

// The PS1 view: camera = translate(pan + (100, 500), distance) * Rx(-pitch) * Ry(-yaw) applied to the world (y down, feet on y = 0), screen
// = centre (320, 240) + 512 * camera xy / camera z in PS1 pixels, which the slot maps to the window. The model is narrowed along its own x;
// the weapon objects hang on mount records 0 (and 1 for model 0x32) at JointMatrix * [RotMatrixZYX(rot) | attach + offset]. The floor
// shadow (every vertex of the body and the weapons slid along kShadowRay onto y = 0, one flat grey) lies behind everything; the status
// effects are depth-tested against the model.
void StatusModelView::draw(const render::Frame& frame, float vw, float vh, const MenuModelSlot& slot) {
  // A texture mod switched on / off: the models and weapons are built again.
  if (const u32 generation = vfs_.generation(); generation != generation_) {
    generation_ = generation;
    cache_.clear();
    weapons_.clear();
  }
  Entry* shown = entry(slot.model, slot.hd);
  if (!shown || !shown->body) return;
  const bool restarted = shown->session != slot.session;
  if (restarted) {
    shown->session = slot.session;
    // The screen's set-up steps the new pose once before its first frame.
    shown->pose.play(slot.pose, 0);
    shown->pose.tick();
    shown->pose_id = slot.pose;
    shown->accum = 0;
    shown->ticks = 0;
    shown->pose_half_due = false;
  } else if (slot.pose != shown->pose_id) {
    shown->pose.play(slot.pose, kPoseBlend);
    shown->pose_id = slot.pose;
  }
  if (effect_session_ != slot.session || !emitters_) {
    effect_session_ = slot.session;
    start_effects(frame, slot.status);
  }
  // A menu tick advances the pose by two engine steps (the menu sets the animation speed and the blend rate per vsync, and a tick spans two
  // vsyncs): the first at the tick, the second half-way to the next; in between the pose is interpolated by the time drawn.
  // A restart begins with the camera turning from yaw 0 by kAutoTurn per tick, so its yaw counts the ticks the restart is already into.
  const u32 steps = std::min(restarted ? static_cast<u32>(std::max(slot.camera.yaw, 0) / kAutoTurn) : slot.tick - last_tick_, kMaxSteps);
  last_tick_ = slot.tick;
  shown->accum = steps > 0 ? 0.0f : std::min(shown->accum + slot.dt * kTickRate, kMaxAlpha);
  for (u32 step = 0; step < steps; ++step) {
    if (shown->pose_half_due) shown->pose.tick();
    shown->pose.tick();
    shown->pose_half_due = true;
    ++shown->ticks;
    step_blink();
    if (emitters_) emitters_->tick();
  }
  if (shown->pose_half_due && shown->accum >= 0.5f) {
    shown->pose.tick();
    shown->pose_half_due = false;
  }
  shown->pose.palette(shown->pose_half_due ? shown->accum * 2.0f : std::max(shown->accum - 0.5f, 0.0f) * 2.0f, palette_);
  effect_model_ = &shown->body->asset;
  effect_palette_ = palette_;
  set_weapon(*shown, slot.weapon_model, slot.weapon_part);

  const MenuCamera& camera = slot.camera;
  const assets::Mat3 camera_rot = camera_rotation(camera);
  const render::Mat4 view = render::mat4_translation(static_cast<float>(camera.pan_x) + kFeetRight, static_cast<float>(camera.pan_y) + kFeetBelow,
                                                     static_cast<float>(camera.distance)) *
                            render::mat4_from_rt(camera_rot, {0, 0, 0});
  render::Mat4 proj;
  proj.at(0, 0) = 2.0f * slot.unit_x * kProjection / vw;
  proj.at(0, 2) = 2.0f * (slot.origin_x + slot.unit_x * kCentreX) / vw - 1.0f;
  proj.at(1, 1) = 2.0f * slot.unit_y * kProjection / vh;
  proj.at(1, 2) = 2.0f * (slot.origin_y + slot.unit_y * kCentreY) / vh - 1.0f;
  proj.at(2, 2) = 0.5f;
  proj.at(3, 2) = 1.0f;
  proj.at(3, 3) = 0.0f;
  render::Mat4 model;
  model.at(0, 0) = kModelScaleX;
  const int shadow_step = 4096 / kShadowRay[1];
  render::Mat4 flatten;
  flatten.at(0, 1) = static_cast<float>(shadow_step * kShadowRay[0]) / 4096.0f;
  flatten.at(1, 1) = 0.0f;
  flatten.at(2, 1) = static_cast<float>(shadow_step * kShadowRay[2]) / 4096.0f;

  // The weapon objects: slot 1 only on model 0x32; the record's attach point (an all-zero record = point 0) and turn.
  struct Hung {
    render::Mat4 matrix;
    bool drawn = false;
  };
  Hung hung[2];
  int hung_count = 0;
  const assets::AttachPoints& attach = shown->body->asset.attach;
  const bool weapon_hidden = std::find(std::begin(kHiddenWeapon), std::end(kHiddenWeapon), slot.char_id) != std::end(kHiddenWeapon);
  for (int index = 0; index < 2 && shown->weapon && shown->weapon->gpu; ++index) {
    if (index == 1 && slot.model != kLastMenuModel) continue;
    const assets::MountPoint& mount = attach.mounts[static_cast<size_t>(index)];
    const assets::AttachPoint& point = attach.points[mount.attach];
    if (point.joint >= palette_.size()) continue;
    assets::Vec3 offset = point.pos, degrees{};
    for (size_t k = 0; k < 3; ++k) offset[k] += mount.pos[k], degrees[k] = mount.rot[k] * kTurnDegrees;
    hung[hung_count].matrix = palette_[point.joint] * render::mat4_from_rt(assets::rotation_zyx(degrees), offset);
    hung[hung_count++].drawn = !(index == 0 && weapon_hidden);
  }

  render::MeshPipeline::DrawParams dp;
  dp.view_proj = proj * view;
  dp.view = view;
  dp.palette = palette_;
  dp.linear = slot.hd;
  dp.hd = slot.hd;
  // The menu is drawn over the field / world map in the same render pass, and those leave their depth values behind (room layers, actors):
  // the model would be depth-tested against them and show holes. It is the topmost 3D element of the menu: start from a clean depth buffer.
  frame.clear_depth();
  if (shadow_pipe_ready(frame)) {
    // The shadow pipeline's world is y up; the flat shadow's ray is the direction toward its light in it.
    const float ray[3] = {static_cast<float>(kShadowRay[0]), static_cast<float>(kShadowRay[1]), static_cast<float>(kShadowRay[2])};
    const float length = std::sqrt(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);
    std::array<render::ShadowPipeline::Cast, render::ShadowPipeline::kChannels> casts;
    casts[0] = {{ray[0] / length, ray[1] / length, ray[2] / length}, 1.0f};
    render::Mat4 up;
    up.at(1, 1) = -1.0f;
    u32 hidden = slot.hidden_meshes, env = 0;
    const auto& shadow_shading = shown->body->asset.shading;
    for (size_t k = 0; k < shadow_shading.size() && k < 32; ++k) {
      if (shadow_shading[k].flags & 1) hidden |= 1u << k;
      if ((shadow_shading[k].flags & 0x80) && (shadow_shading[k].flags & 0x60)) env |= 1u << k;
    }
    shadow_pipe_.begin_mask(frame);
    shadow_pipe_.cast(*shown->body->gpu, up * model, palette_, hidden, env, casts);
    for (int i = 0; i < hung_count; ++i) shadow_pipe_.cast(*shown->weapon->gpu, up * model, std::span<const render::Mat4>(&hung[i].matrix, 1), 0, 0, casts);
    shadow_pipe_.end_mask();
    render::ShadowPipeline::Floor floor;
    floor.clip = proj * view;
    floor.rect = {-kCastReach, -kCastReach, kCastReach, kCastReach};
    floor.strength = kCastStrength;
    shadow_pipe_.draw_floor(floor);
  } else {
    meshes_.begin(frame, frame.full());
    render::MeshPipeline::DrawParams shadow = dp;
    shadow.model = flatten * model;
    shadow.solid = true;
    shadow.solid_rgb[0] = shadow.solid_rgb[1] = shadow.solid_rgb[2] = kShadowGrey;
    meshes_.draw(*shown->body->gpu, shadow);
    for (int i = 0; i < hung_count; ++i) {
      render::MeshPipeline::DrawParams weapon_shadow = shadow;
      weapon_shadow.palette = std::span<const render::Mat4>(&hung[i].matrix, 1);
      meshes_.draw(*shown->weapon->gpu, weapon_shadow);
    }
    meshes_.end();
  }
  frame.clear_depth();
  meshes_.begin(frame, frame.full());

  dp.model = model;
  dp.ot_faces = true;
  dp.hidden_meshes = slot.hidden_meshes;
  // The S3 shading entries: flag bit 0 = part hidden, 0x10 = nearest-corner sort, 0x20 / 0x40 without 0x80 = the stock environment page
  // (replace / over with ABR = bits 2-3), 0x80 with 0x20 / 0x40 = env-mapped with the model's own page, rgb = part colour (0x80 neutral).
  const auto& shading = shown->body->asset.shading;
  for (size_t k = 0; k < shading.size() && k < 32; ++k) {
    const assets::ShadingEntry& entry_k = shading[k];
    if (entry_k.flags & 1) dp.hidden_meshes |= 1u << k;
    if (entry_k.flags & 0x10) dp.near_meshes |= 1u << k;
    if ((entry_k.flags & 0x80) && (entry_k.flags & 0x60)) dp.env_meshes |= 1u << k;
    if ((entry_k.flags & 0x60) && !(entry_k.flags & 0x80)) {
      if (entry_k.flags & 0x40) dp.stock_env_over |= 1u << k;
      else dp.stock_env_replace |= 1u << k;
      dp.stock_env_abr[k] = static_cast<u8>((entry_k.flags >> 2) & 3);
    }
    if (entry_k.r != 0x80 || entry_k.g != 0x80 || entry_k.b != 0x80) {
      dp.part_mask |= 1u << k;
      dp.part_tint[k][0] = entry_k.r / 128.0f, dp.part_tint[k][1] = entry_k.g / 128.0f, dp.part_tint[k][2] = entry_k.b / 128.0f;
    }
  }
  if ((dp.stock_env_over || dp.stock_env_replace) && frame.ctx) {
    if (!env_page_tried_) {
      env_page_tried_ = true;
      if (auto texture = menu_env_page(vfs_))
        if (!env_page_.create(*frame.ctx, texture->width, texture->height, render::TextureFormat::Rgba8, texture->pixels.data()) ||
            !env_sampler_.create(*frame.ctx, false))
          env_page_.destroy();
    }
    if (env_page_) dp.stock_env_set = render::TexRef(env_page_, env_sampler_);
  }
  const game::ModelHandle& body = eyes_shut_ && shown->body_shut ? *shown->body_shut : *shown->body;
  meshes_.draw(*body.gpu, dp);
  for (int i = 0; i < hung_count; ++i) {
    if (!hung[i].drawn) continue;
    render::MeshPipeline::DrawParams wd = dp;
    wd.palette = std::span<const render::Mat4>(&hung[i].matrix, 1);
    wd.hidden_meshes = wd.near_meshes = wd.part_mask = wd.env_meshes = wd.stock_env_over = wd.stock_env_replace = 0;
    const render::Mat4 world = model * hung[i].matrix;
    if (slot.weapon_part == kPrismTier) {
      prism_light(world, shown->ticks, wd);
    } else {
      // The glint: light = the unit vector from the weapon toward the light, eye = the point (d sin yaw, d cos yaw, 0) the menu leaves for it.
      float to_light[3] = {kWeaponLight[0] - world.at(0, 3), kWeaponLight[1] - world.at(1, 3), kWeaponLight[2] - world.at(2, 3)};
      const float length = std::sqrt(to_light[0] * to_light[0] + to_light[1] * to_light[1] + to_light[2] * to_light[2]);
      const float yaw = static_cast<float>(camera.yaw & 0xFFF) * (2.0f * kPi / 4096.0f);
      const float eye[3] = {static_cast<float>(camera.distance) * std::sin(yaw), static_cast<float>(camera.distance) * std::cos(yaw), 0.0f};
      wd.light.spec = length > 0.0f;
      for (size_t k = 0; k < 3; ++k) wd.light.spec_dir[k] = length > 0.0f ? to_light[k] / length : 0.0f, wd.light.spec_eye[k] = eye[k];
    }
    meshes_.draw(*shown->weapon->gpu, wd);
  }
  meshes_.end();
  effects::Mat3 effect_rot;
  std::copy(camera_rot.begin(), camera_rot.end(), effect_rot.m.begin());
  draw_effects(frame, vw, vh, slot, effect_rot, shown->accum);
}

}  // namespace oc::ui
