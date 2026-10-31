// BattleView: effect scene, view, model loading, weapon meshes, clip packs and GPU residency.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>

#include "core/log.h"
#include "import/container.h"
#include "import/model.h"
#include "import/tim.h"
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"

namespace oc::game {

using namespace bv;

namespace {

// An effect pack's copy of a battle unit (its pages hold the unit's 1x texture at the top left, the copy samples it with the unit's own UVs)
// gets the art the unit itself is drawn with when that is something else (a texture mod's image, the Modded art's 4x texture): every page
// scaled to the art's size, the art pasted over its 1x region, as the copy's ModelAsset::mod_textures (its UVs are normalised).
void paste_unit_art(assets::ModelAsset& copy, const assets::ModelAsset& unit) {
  if (unit.textures.empty() || copy.textures.empty()) return;
  const import::Image& one = unit.textures[0];
  const import::Image& art = unit.drawn_texture(0, true);
  if (&art == &one || !one.width || !one.height || art.indexed) return;
  const double scale_x = double(art.width) / one.width, scale_y = double(art.height) / one.height;
  copy.mod_textures.assign(copy.textures.size(), {});
  for (size_t i = 0; i < copy.textures.size(); ++i) {
    const import::Image& page = copy.textures[i];
    if (page.indexed || !page.width || !page.height) continue;
    import::Image scaled;
    scaled.width = static_cast<u32>(std::lround(page.width * scale_x)), scaled.height = static_cast<u32>(std::lround(page.height * scale_y));
    scaled.pixels.resize(static_cast<size_t>(scaled.width) * scaled.height * 4);
    for (u32 y = 0; y < scaled.height; ++y)
      for (u32 x = 0; x < scaled.width; ++x) {
        const bool in_art = i == 0 && x < art.width && y < art.height;
        const u8* src = in_art ? &art.pixels[(static_cast<size_t>(y) * art.width + x) * 4]
                               : &page.pixels[(static_cast<size_t>(std::min(page.height - 1, static_cast<u32>(y / scale_y))) * page.width +
                                               std::min(page.width - 1, static_cast<u32>(x / scale_x))) * 4];
        std::memcpy(&scaled.pixels[(static_cast<size_t>(y) * scaled.width + x) * 4], src, 4);
      }
    copy.mod_textures[i] = std::move(scaled);
  }
}

}  // namespace

FxScene BattleView::fx_scene() const {
  FxScene scene;
  for (int i = 0; i < battle::kUnits; ++i) {
    const battle::Unit& unit = battle_->unit(i);
    const Visual& visual = vis_[static_cast<size_t>(i)];
    FxUnit& fx_unit = scene.unit[static_cast<size_t>(i)];
    fx_unit.present = unit.present;
    fx_unit.offstage = ghosted(i);
    fx_unit.pc = unit.pc;
    fx_unit.x = visual.x;
    fx_unit.z = visual.z;
    fx_unit.y = visual.y;
    fx_unit.yaw = std::atan2(-visual.face_x, -visual.face_z);
    fx_unit.height = visual.height;
    // Effect joints b >= 2 = attach point b - 2 of the posed model.
    if (visual.has_model)
      fx_unit.attach = [this, i](int point, float& x, float& y, float& z) {
        if (!vis_[static_cast<size_t>(i)].pose.attach_now(point, 1.0f, x, y, z)) return false;
        // The root joint matrix is a rotation times a scale, and every joint's world matrix carries that scale: the point of a model the
        // script scaled (anm op 83: Fire Dragon's form 2 rises at 0x480 = 0.28) shrinks toward its origin, in the model's own axes.
        anm_scale_point(i, x, y, z);
        return true;
      };
    // The joint rotation of attach point k.
    if (visual.has_model)
      fx_unit.attach_rot = [this, i](int point, float (&rot)[9]) { return vis_[static_cast<size_t>(i)].pose.attach_rot_now(point, 1.0f, rot); };
    // The posed model's joint j: the overlays' bone look-ups.
    if (visual.has_model)
      fx_unit.joint = [this, i](int joint, float& x, float& y, float& z) { return vis_[static_cast<size_t>(i)].pose.joint_now(joint, 1.0f, x, y, z); };
    if (visual.has_model && visual.model) {
      // Effect scales from the model's S1 extents: horizontal = clamp(((ey << 12) / 0x42B - 0x1000) / 2 + 0x1000, 0x300, 0x8000 past 0x10000),
      // the second the same of SquareRoot0(ex^2 + ez^2) / 0x23C. Serge: ey 1055 -> 4073.
      auto eff_scale = [](int extent, int divisor) {
        const int scale = ((extent << 12) / divisor - 0x1000) / 2 + 0x1000;
        return scale > 0x10000 ? 0x8000 : (scale < 0x300 ? 0x300 : scale);
      };
      const auto& ex = visual.model->asset.extent;
      fx_unit.fx_scale = eff_scale(ex[1], 0x42B);
      // The libgte table root, not std::sqrt.
      fx_unit.fx_scale_h = eff_scale(effects::ps1_square_root0(ex[0] * ex[0] + ex[2] * ex[2]), 0x23C);
    }
  }
  // The caster: for a group cast (the script's 8D slots) the first group member, which need not be the actor
  // (scene 453's henshin: actor 3, list [4, 3]; the camera and every caster selector follow unit 4).
  scene.casters = act_.casters;
  scene.caster = !act_.casters.empty() ? std::max(0, act_.casters.front()) : std::max(0, act_.actor);
  scene.targets = act_.targets;
  // The result code of this action on the unit (the overlays filter the lists by it).
  for (const battle::ActionEvent& event : act_.ev)
    if (event.target >= 0 && event.target < battle::kUnits) scene.unit[static_cast<size_t>(event.target)].hit = event.code;
  for (int i = 0; i < battle::kUnits; ++i) scene.unit[static_cast<size_t>(i)].dead = battle_->unit(i).present && battle_->unit(i).hp <= 0;
  // The pack's pseudo-actor = slot A: effect selector 8 is its model, attach points through ITS pose, SetRot x / z
  // and scale (the pseudo-actor's draw matrix, pa_matrix), yaw = its facing.
  if (pa_.on && pa_.pose && pa_.asset) {
    FxUnit& fx_unit = scene.pseudo;
    fx_unit.present = true;
    fx_unit.x = pa_.x;
    fx_unit.z = pa_.z;
    fx_unit.y = pa_.y;
    fx_unit.yaw = std::atan2(-pa_.face_x, -pa_.face_z);
    if (std::getenv("OC_PA_TRACE") && pa_.ticks % 20 == 0)
      std::fprintf(stderr, "pa: tick %d pos %.0f %.0f %.0f yaw %d scale %.3f %.3f %.3f rx %.2f rz %.2f hidden %d\n", pa_.ticks, pa_.x, pa_.y, pa_.z, static_cast<int>(std::lround(fx_unit.yaw * 4096.0f / 6.2831853f)) & 0xFFF, pa_.scale[0], pa_.scale[1], pa_.scale[2], pa_.rx, pa_.rz, pa_.hidden);
    {
      // The effect scales from the model's S1 extents, as for the units above.
      auto eff_scale = [](int extent, int divisor) {
        const int scale = ((extent << 12) / divisor - 0x1000) / 2 + 0x1000;
        return scale > 0x10000 ? 0x8000 : (scale < 0x300 ? 0x300 : scale);
      };
      const auto& ex = pa_.asset->extent;
      fx_unit.fx_scale = eff_scale(ex[1], 0x42B);
      fx_unit.fx_scale_h = eff_scale(effects::ps1_square_root0(ex[0] * ex[0] + ex[2] * ex[2]), 0x23C);
    }
    const float cx = std::cos(pa_.rx), sx = std::sin(pa_.rx);
    fx_unit.rot_z = pa_.rz;
    // v' = Rx * (S * v), the model-space part (the yaw, then Rz, come after it: RotMatrix = Rz * Ry * Rx; BattleFxRun consumers apply them)
    const float rzx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx};
    const float sc[3] = {pa_.scale[0], pa_.scale[1], pa_.scale[2]};
    fx_unit.attach = [this, rzx, sc](int point, float& x, float& y, float& z) {
      if (!pa_.pose->attach_or_root(point, 1.0f, x, y, z)) return false;
      const float scaled[3] = {x * sc[0], y * sc[1], z * sc[2]};
      x = rzx[0] * scaled[0] + rzx[1] * scaled[1] + rzx[2] * scaled[2];
      y = rzx[3] * scaled[0] + rzx[4] * scaled[1] + rzx[5] * scaled[2];
      z = rzx[6] * scaled[0] + rzx[7] * scaled[1] + rzx[8] * scaled[2];
      return true;
    };
    fx_unit.attach_rot = [this, rzx, sc](int point, float (&rot)[9]) {
      float pal[9];
      if (!pa_.pose->attach_rot_now(point, 1.0f, pal)) return false;
      for (int col_index = 0; col_index < 3; ++col_index)
        for (int row = 0; row < 3; ++row) {
          float sum = 0;
          for (int k = 0; k < 3; ++k) sum += rzx[row * 3 + k] * sc[k] * pal[k * 3 + col_index];
          rot[row * 3 + col_index] = sum;
        }
      return true;
    };
  }
  return scene;
}

// The battle camera as the effect player's view: Pc = rot * P + trans with P in PS1 world space (y down); the stage is y up, so the y
// column of the rotation is negated.
effects::ViewParams BattleView::fx_view() const {
  effects::ViewParams view;
  const std::array<float, 3>* rows[3] = {&cam_.right, &cam_.down, &cam_.fwd};
  for (size_t row = 0; row < 3; ++row) {
    const auto& axis = *rows[row];
    view.rot.m[row * 3 + 0] = axis[0];
    view.rot.m[row * 3 + 1] = -axis[1];
    view.rot.m[row * 3 + 2] = axis[2];
    view.trans[row] = -dot(axis, cam_.eye);
  }
  view.h = cam_.h;
  view.ox = cam_.ox;
  view.oy = cam_.oy;
  // Effect objects with flag bit 17 are depth-cued while the fog flag is on (op 0x24 .. 0x6C).
  view.cue.on = fx_fog_on_;
  view.cue.near = fog_cur_.near;
  view.cue.far = fog_cur_.far;
  view.cue.rgb[0] = fog_cur_.r;
  view.cue.rgb[1] = fog_cur_.g;
  view.cue.rgb[2] = fog_cur_.b;
  return view;
}

// Sprite type 20 = a type-0x0B record = an Mdl model used as the particle body. The
// record has no texture words: its *name* is decoded as hex - chars 0-1 = CLUT row, char 2 bit 0 = tpage y 256, bit 3
// = CLUT x 256, char 3 = tpage x / 64 - an 8 bpp page (tpage 0x80 | ...). The page is cut from the VRAM the pack plays in.
void BattleView::fx_load_models(FxGpu& gpu, const FxPack& pack) {
  auto drp = import::parse_drp(pack.prd);
  if (!drp || !meshes_) return;
  bool any = false;
  for (const auto& entry : drp->entries) any = any || entry.type == 0x0B;
  if (!any) return;
  const import::Vram vram = pack.build_vram();
  // Dev: OC_VRAM_DUMP writes the pack's VRAM as 1024 x 512 halfwords.
  if (const char* dump = std::getenv("OC_VRAM_DUMP")) {
    if (FILE* file = std::fopen(dump, "wb")) {
      for (int y = 0; y < import::Vram::kHeight; ++y)
        for (int x = 0; x < import::Vram::kWidth; ++x) {
          const u16 halfword = vram.at(x, y);
          std::fwrite(&halfword, 2, 1, file);
        }
      std::fclose(file);
    }
  }
  auto hexval = [](char digit) -> int {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
  };
  for (const auto& entry : drp->entries) {
    if (entry.type != 0x0B || entry.name.size() < 4 || gpu.models.count(entry.name)) continue;
    std::string bind = entry.name;
    if (hexval(bind[0]) < 0 || hexval(bind[1]) < 0 || hexval(bind[2]) < 0 || hexval(bind[3]) < 0) {
      // No code in the name: the overlay binds the texture itself at init (saraman's 'sara': effects::fx_pack_model_binding).
      const char* alias = effects::fx_pack_model_binding(entry.name, pack.overlay);
      if (!alias) {
        log::debug("battle fx: model {}: the name is no texture binding", entry.name);
        continue;
      }
      bind = alias;
    }
    const int clut_y = hexval(bind[0]) * 16 + hexval(bind[1]), nib = hexval(bind[2]);
    const int tpage_y = (nib & 1) ? 256 : 0, clut_x = (nib & 8) ? 256 : 0, tpx = hexval(bind[3]) * 64;
    std::vector<import::Image> pages = fx_model_pages(vram, tpx, tpage_y, clut_x, clut_y);
    constexpr int kPageW = 384;
    // HD art: sky's act0 is a byte copy of battle/player/selju2/selju.obj (S1 mesh + S2 skeleton; S3 shading and the clips are the
    // pack's own) on a page that holds selju.ltc's image + CLUT, so in HD it gets the mesh + texture the HD Serge unit is drawn with (cdrom2).
    Bytes hd_mdl;
    std::string stem;
    if (hd_ && vfs_ && vfs_->hd_mode()) {
      auto hd_vram = std::make_unique<import::Vram>();
      stem = fx_hd_counterpart(entry.data, vram, tpx, tpage_y, clut_x, clut_y, hd_mdl, *hd_vram);
      if (!stem.empty()) {
        for (int row = 0; row < 2; ++row) {
          const std::vector<u16> clut = hd_vram->read_clut(clut_x, std::min(clut_y + row, 255), 256);
          for (int y = 0; y < 256; ++y)
            for (int x = 0; x < kPageW; ++x) {
              const u32 texel = import::bgr555_to_rgba_stp(hd_vram->texel(tpx * 2 + x, tpage_y + y, 8, clut));
              std::memcpy(&pages[static_cast<size_t>(row)].pixels[(static_cast<size_t>(y) * kPageW + static_cast<size_t>(x)) * 4], &texel, 4);
            }
        }
        gpu.art_dependent = true;
        log::info("battle fx: model {}: HD art = {} (cdrom2 mesh + texture)", entry.name, stem);
      }
    }
    std::string err;
    auto asset = import::load_room_model(hd_mdl.empty() ? ByteSpan(entry.data) : ByteSpan(hd_mdl), pages, &err);
    if (!asset) {
      log::debug("battle fx: model {}: {}", entry.name, err);
      continue;
    }
    auto model = std::make_unique<FxGpu::Model>();
    model->asset = std::move(*asset);
    if (!stem.empty())
      if (const std::shared_ptr<ModelHandle> unit = load_model_for(stem)) paste_unit_art(model->asset, unit->asset);
    model->gpu = meshes_->create_model(model->asset);
    if (!model->gpu) continue;
    model->pose.set_model(&model->asset);
    if (stem.empty()) model->page_x = tpx, model->page_y = tpage_y, model->clut_x = clut_x, model->clut_y = clut_y;
    // Motion of a model object no op 49 started: the clip with the most keyframes; op 49's own choice is the object's
    // clip byte (fx_draw_models).
    for (size_t i = 0; i < model->asset.clips.size(); ++i)
      if (model->clip < 0 || model->asset.clips[i].frame_count > model->asset.clips[static_cast<size_t>(model->clip)].frame_count) model->clip = static_cast<int>(i);
    log::debug("battle fx: model {}: {} joints, {} clips, tpage {},{} CLUT ({},{})", entry.name, model->asset.skeleton.joints.size(), model->asset.clips.size(), tpx,
               tpage_y, clut_x, clut_y);
    gpu.models.emplace(entry.name, std::move(model));
  }
}

// The faces' CLUT bit selects the next palette row (textures[1] = row clut_y + 1; Sonja's wings 'ee97' use row 239 for the lime), so two pages
// are baked, one per row. A GPU texture page is 256 x 256 TEXELS whatever the depth (an 8 bpp page = 128 halfwords from tpage x * 64): the faces'
// u bytes run 0..255 over it, and a face's page bit (+1 tpage column = 64 halfwords = +128 texels at 8 bpp) reaches 383, so the page is cut 384
// texels wide.
std::vector<import::Image> BattleView::fx_model_pages(const import::Vram& vram, int page_x, int page_y, int clut_x, int clut_y) {
  constexpr int kPageW = 384;
  std::vector<import::Image> pages;
  for (int row = 0; row < 2; ++row) {
    const std::vector<u16> clut = vram.read_clut(clut_x, std::min(clut_y + row, 255), 256);
    import::Image page;
    page.width = kPageW;
    page.height = 256;
    page.pixels.assign(static_cast<size_t>(kPageW) * 256 * 4, 0);
    for (int y = 0; y < 256; ++y)
      for (int x = 0; x < kPageW; ++x) {
        const u32 texel = import::bgr555_to_rgba_stp(vram.texel(page_x * 2 + x, page_y + y, 8, clut));
        std::memcpy(&page.pixels[(static_cast<size_t>(y) * kPageW + static_cast<size_t>(x)) * 4], &texel, 4);
      }
    pages.push_back(std::move(page));
  }
  return pages;
}

std::string BattleView::fx_hd_counterpart(ByteSpan mdl, const import::Vram& vram, int img_x, int img_y, int clut_x, int clut_y, Bytes& out_mdl,
                                          import::Vram& out_vram) {
  auto fnv = [](ByteSpan bytes) {
    u64 hash = 1469598103934665603ull;
    for (u8 byte : bytes) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
  };
  // Once per session: index the battle models that ship a cdrom2 (HD art) .obj (battle/player, 3 battle/monster).
  if (!fx_hd_mesh_indexed_) {
    fx_hd_mesh_indexed_ = true;
    for (const std::string& file : vfs_->list("cdrom2")) {
      if (!file.starts_with("battle/") || !file.ends_with(".obj")) continue;
      const auto disc = vfs_->read_disc("cdrom/" + file);
      if (!disc) continue;
      const ByteSpan s1 = import::mdl_section(*disc, 0);
      if (!s1.empty()) fx_hd_mesh_index_[fnv(s1)].push_back(file.substr(0, file.size() - 4));
    }
  }
  const ByteSpan s1 = import::mdl_section(mdl, 0);
  if (s1.empty()) return {};
  const auto it = fx_hd_mesh_index_.find(fnv(s1));
  if (it == fx_hd_mesh_index_.end()) return {};
  for (const std::string& stem : it->second) {
    const auto disc = vfs_->read_disc("cdrom/" + stem + ".obj");
    if (!disc) continue;
    const ByteSpan d1 = import::mdl_section(*disc, 0);
    if (d1.size() != s1.size() || std::memcmp(d1.data(), s1.data(), s1.size()) != 0) continue;
    // The page must hold the model's own disc texture (same indices, CLUT row 0 at the binding) - else the pack recoloured / repainted it.
    auto tex_of = [&](bool disc_only) -> std::optional<import::AtimMain> {
      for (const char* ext : {".ltd", ".ltc"}) {
        const std::string path = "cdrom/" + stem + ext;
        if (auto bytes = disc_only ? vfs_->read_disc(path) : vfs_->read(path)) return import::load_atim_main(*bytes);
      }
      return std::nullopt;
    };
    const auto t1 = tex_of(true);
    if (!t1 || t1->width % 2 || t1->clut.empty()) continue;
    bool same = vram.read_clut(clut_x, clut_y, static_cast<int>(t1->clut.size())) == t1->clut;
    for (u32 y = 0; same && y < t1->height; ++y)
      for (u32 x = 0; same && x < t1->width; ++x)
        same = vram.texel(img_x * 2 + static_cast<int>(x), img_y + static_cast<int>(y), 8) == t1->indices[static_cast<size_t>(y) * t1->width + x];
    if (!same) continue;
    // HD art mode: cdrom2 first (or a mod's file).
    const auto hd_obj = vfs_->read("cdrom/" + stem + ".obj");
    const auto t2 = tex_of(false);
    if (!hd_obj || !t2 || t2->width != t1->width || t2->height != t1->height || t2->clut.size() != t1->clut.size()) continue;
    auto spliced = import::mdl_with_mesh_of(mdl, *hd_obj);
    if (!spliced) continue;
    out_vram = vram;
    std::vector<u16> hw(static_cast<size_t>(t2->width / 2) * t2->height);
    for (size_t i = 0; i < hw.size(); ++i) hw[i] = static_cast<u16>(t2->indices[2 * i] | (t2->indices[2 * i + 1] << 8));
    out_vram.blit(img_x, img_y, static_cast<int>(t2->width / 2), static_cast<int>(t2->height), hw);
    out_vram.blit(clut_x, clut_y, static_cast<int>(t2->clut.size()), 1, t2->clut);
    out_mdl = std::move(*spliced);
    return stem;
  }
  return {};
}

// The model objects (the model takes the object's colour, world position, matrix incl. scale,
// and motion frame) through the skinned mesh pipeline with the battle camera, like the units. Model space is y down, the mesh pipeline's
// world y up: model = T(x, -y, z) * F * (R * S), F = diag(1, -1, 1).
void BattleView::fx_draw_models(const render::Frame& frame, const render::Letterbox& lb, FxGpu& gpu,
                                const std::vector<effects::EmitterSystem::MeshInstance>& inst) {
  if (gpu.models.empty() || !meshes_) return;
  bool open = false;
  const float fw = static_cast<float>(frame.extent.width), fh = static_cast<float>(frame.extent.height);
  const float x0 = std::clamp(std::round(lb.left()), 0.0f, fw), y0 = std::clamp(std::round(lb.oy), 0.0f, fh);
  const render::Rect scissor{static_cast<int32_t>(x0), static_cast<int32_t>(y0),
                             static_cast<u32>(std::min(fw - x0, std::round(lb.width()))), static_cast<u32>(std::min(fh - y0, std::round(kBufferRows * lb.scale_y)))};
  const render::Mat4 vp = cam_.view_proj(lb), view = cam_.view();
  const effects::FxModels* fms = fx_ ? fx_->bespoke_models() : nullptr;
  const assets::StageAsset* mount_asset = fx_pack_ ? fx_pack_->meshes.get() : nullptr;
  FxGpu* mount_gpu = fms && !fms->mount_mesh.empty() && gpu.stage && mount_asset ? &gpu : nullptr;
  std::vector<render::StageInstance> mounted;
  const effects::ViewParams fx_params = fx_view();
  for (const auto& mi : inst) {
    if (mi.sprite_type != 20 || !mi.name || mi.off_screen(fx_params)) continue;
    // A model the overlay hid is skipped.
    if (fms && fms->pack_model_hidden && *mi.name == fms->pack_model) continue;
    // The driven model, hidden.
    if (fms && fms->pack_drive.active && fms->pack_drive.hidden && *mi.name == fms->pack_drive.name) continue;
    const auto it = gpu.models.find(*mi.name);
    if (it == gpu.models.end()) continue;
    FxGpu::Model& fx_model = *it->second;
    render::Mat4 model = render::mat4_translation(mi.world_pos[0], -mi.world_pos[1], mi.world_pos[2]);
    render::Mat4 rs;
    for (int row = 0; row < 3; ++row)
      for (int col_index = 0; col_index < 3; ++col_index) rs.at(row, col_index) = mi.world_mat.m[static_cast<size_t>(row * 3 + col_index)] * (row == 1 ? -1.0f : 1.0f);
    model = model * rs;
    // Motion set = op 49's SetAnim(model, the clip byte of op 04); ids >= 0x80 (the extension table) and ids
    // past the table start nothing. Without a loop flag the motion stops on its last keyframe.
    const int clip = mi.clip >= 0 && mi.clip < 0x80 && static_cast<size_t>(mi.clip) < fx_model.asset.clips.size() ? mi.clip : mi.clip < 0 ? fx_model.clip : -1;
    float mf = mi.motion_prev + (mi.motion - mi.motion_prev) * alpha_ + (fms ? fms->pack_extra_frames : 0.0f);
    if (clip >= 0 && !mi.loop && mi.clip >= 0) mf = std::min(mf, std::max(0.0f, static_cast<float>(fx_model.asset.clips[static_cast<size_t>(clip)].frame_count) - 1.0f));
    fx_model.pose.palette_at(clip, mf, palette_);
    if (!open) {
      meshes_->begin(frame, scissor);
      open = true;
    }
    render::MeshPipeline::DrawParams dp;
    dp.view_proj = vp;
    dp.view = view;
    dp.model = model;
    dp.palette = palette_;
    dp.linear = linear_filter_ < 0 ? hd_ : linear_filter_ != 0;
    dp.hd = false;
    for (size_t channel = 0; channel < 3; ++channel) dp.tint[channel] = static_cast<float>(mi.rgb[channel]) / 128.0f;
    // Per-face slots like the units.
    unit_ot_faces(fx_model.asset, dp);
    meshes_->draw(*fx_model.gpu, dp);
    // The overlay's mounted mesh: JointMatrix(attach.joint) * [RotMatrixZYX(mount.rot) | attach.pos + mount.pos], to camera space.
    if (mount_gpu && *mi.name == fms->mount_model) {
      const auto part = mount_gpu->parts.find(fms->mount_mesh);
      const assets::MountPoint& mount = fx_model.asset.attach.mounts[0];
      const assets::AttachPoint& ap = fx_model.asset.attach.points[std::min<size_t>(mount.attach, fx_model.asset.attach.points.size() - 1)];
      if (part != mount_gpu->parts.end() && mount.attach < fx_model.asset.attach.points.size() && ap.joint < palette_.size()) {
        assets::Vec3 off = ap.pos, deg{0, 0, 0};
        for (size_t k = 0; k < 3; ++k) {
          off[k] += mount.pos[k];
          deg[k] = mount.rot[k] * (360.0f / 4096.0f);
        }
        const render::Mat4 world = model * palette_[ap.joint] * render::mat4_from_rt(assets::rotation_zyx(deg), off);
        const std::array<float, 3>* axes[3] = {&cam_.right, &cam_.down, &cam_.fwd};
        render::StageInstance in;
        in.part = part->second;
        for (int row = 0; row < 3; ++row) {
          const std::array<float, 3>& axis = *axes[row];
          for (int col = 0; col < 3; ++col)
            in.cam[static_cast<size_t>(row * 3 + col)] = axis[0] * world.at(0, col) + axis[1] * world.at(1, col) + axis[2] * world.at(2, col);
          in.cam[static_cast<size_t>(9 + row)] =
              axis[0] * (world.at(0, 3) - cam_.eye[0]) + axis[1] * (world.at(1, 3) - cam_.eye[1]) + axis[2] * (world.at(2, 3) - cam_.eye[2]);
        }
        for (size_t channel = 0; channel < 3; ++channel) in.rgb[channel] = static_cast<u8>(mi.rgb[channel]);
        mounted.push_back(in);
      }
    }
  }
  if (open) meshes_->end();
  if (!mounted.empty()) {
    const effects::ViewParams fx_cam = fx_view();
    stage_pipe_.begin(frame, scissor);
    stage_pipe_.set_gte(fx_cam.h, fx_cam.ox, fx_cam.oy);
    stage_pipe_.draw(*mount_gpu->stage, render::StagePipeline::projection(lb, fx_cam.h, fx_cam.ox, fx_cam.oy), mounted, linear_filter_ < 0 ? hd_ : linear_filter_ != 0,
                     mount_asset);
    stage_pipe_.end();
  }
}

// The weapon techs' mesh sprites name the caster's weapon mesh by the PC's two-digit character number (guill_1-3 "02" = Guile, stick / stick2
// "19" = Glenn, bigcard "37" = Sneff, high "39" = Doc; the weapon .bin is a cpt, so the battle registers the equipped part under that name).
// Returns the present PC it denotes, with a weapon mesh loaded.
int BattleView::fx_weapon_mesh_unit(std::string_view name) const {
  if (name.size() != 2 || !battle_) return -1;
  for (int i = 0; i < battle::kUnits; ++i) {
    const battle::Unit& unit = battle_->unit(i);
    const Visual& visual = vis_[static_cast<size_t>(i)];
    if (unit.present && unit.pc && visual.weapon && visual.weapon->gpu && std::format("{:02d}", unit.src_id) == name) return i;
  }
  return -1;
}

// Like fx_draw_models: the object's placement (world position, the rotation / scale matrix of op 06 ANGLE / SCALE, PS1 y down) applied to the
// rigid weapon mesh (bound to one joint: the identity here, the hand joint when the unit draws it). Wanda In's 'ls' object shrinks it along z
// (SCALE_RATE z -68 / tick over 60 ticks) at the caster's attach point while op 0x7A hides the mounted one. The object's blend mode is applied
// as for the stock meshes: 0 opaque, 1..4 one pass with ABR blend - 1, 5 the
// mesh twice, B + F then B - F, 6 / 7 B - F then B + F (only STP texels blend, the others stay opaque, as for every model).
void BattleView::fx_draw_weapon_meshes(const render::Frame& frame, const render::Letterbox& lb,
                                       const std::vector<std::pair<int, const effects::EmitterSystem::MeshInstance*>>& inst) {
  if (inst.empty() || !meshes_) return;
  const float fw = static_cast<float>(frame.extent.width), fh = static_cast<float>(frame.extent.height);
  const float x0 = std::clamp(std::round(lb.left()), 0.0f, fw), y0 = std::clamp(std::round(lb.oy), 0.0f, fh);
  const render::Rect scissor{static_cast<int32_t>(x0), static_cast<int32_t>(y0),
                             static_cast<u32>(std::min(fw - x0, std::round(lb.width()))), static_cast<u32>(std::min(fh - y0, std::round(kBufferRows * lb.scale_y)))};
  const render::Mat4 vp = cam_.view_proj(lb), view = cam_.view();
  const render::Mat4 identity;
  meshes_->begin(frame, scissor);
  for (const auto& [unit, mi] : inst) {
    const Visual& visual = vis_[static_cast<size_t>(unit)];
    if (!visual.weapon || !visual.weapon->gpu) continue;
    render::Mat4 model = render::mat4_translation(mi->world_pos[0], -mi->world_pos[1], mi->world_pos[2]);
    render::Mat4 rs;
    for (int row = 0; row < 3; ++row)
      for (int col_index = 0; col_index < 3; ++col_index) rs.at(row, col_index) = mi->world_mat.m[static_cast<size_t>(row * 3 + col_index)] * (row == 1 ? -1.0f : 1.0f);
    model = model * rs;
    render::MeshPipeline::DrawParams dp;
    dp.view_proj = vp;
    dp.view = view;
    dp.model = model;
    dp.palette = std::span<const render::Mat4>(&identity, 1);
    dp.linear = linear_filter_ < 0 ? hd_ : linear_filter_ != 0;
    dp.hd = false;
    for (size_t channel = 0; channel < 3; ++channel) dp.tint[channel] = static_cast<float>(mi->rgb[channel]) / 128.0f;
    const int blend = mi->blend;
    if (blend >= 5) {
      // ABR 1 = B + F, 2 = B - F.
      dp.model_abr = blend == 5 ? 1 : 2;
      meshes_->draw(*visual.weapon->gpu, dp);
      dp.model_abr = blend == 5 ? 2 : 1;
    } else if (blend >= 1) {
      dp.model_abr = blend - 1;
    }
    meshes_->draw(*visual.weapon->gpu, dp);
  }
  meshes_->end();
}

// Hangs the pack's external anim packs (a type-0x19 entry is an external anim pack with
// the acting model's joint count). A name from the table tech, tec1..tec5 goes to the caster whose list index
// equals its index (one caster here: 'tech' = index 0; double / triple techs would name tec1.. for the others), any other name to every caster
// and target. The pack is relocated onto the model as the clip set SetAnim 0x80 + k reads.
void BattleView::fx_hang_clip_packs(const FxPack& pack) {
  if (pack.clip_packs.empty() || !vfs_ || !battle_) return;
  for (const auto& [name, data] : pack.clip_packs) {
    std::vector<int> units;
    if (name.rfind("tec", 0) == 0) {
      const int k = name == "tech" ? 0 : (name.size() == 4 && name[3] >= '1' && name[3] <= '5' ? name[3] - '0' : -1);
      // The caster whose list index = k: list entry k (scene 453's henshin 'tech' pack is the form-2 model's, list [4, 3] -> unit 4).
      if (k >= 0 && static_cast<size_t>(k) < act_.casters.size()) units.push_back(act_.casters[static_cast<size_t>(k)]);
      else if (k == 0 && act_.actor >= 0) units.push_back(act_.actor);
    } else {
      for (int caster : act_.casters)
        if (caster >= 0) units.push_back(caster);
      if (act_.casters.empty() && act_.actor >= 0) units.push_back(act_.actor);
      for (int target : act_.targets)
        if (std::find(units.begin(), units.end(), target) == units.end()) units.push_back(target);
    }
    for (int unit : units) fx_hang_clip(pack.eid, name, data, unit);
  }
}

// Hangs one pack entry on one unit: the pack's anim pack becomes the model's external clip set. False (and a
// warning) when it does not fit the model's skeleton.
bool BattleView::fx_hang_clip(int eid, const std::string& name, const Bytes& data, int unit) {
  if (!vfs_ || !battle_ || unit < 0 || unit >= battle::kUnits) return false;
  Visual& visual = vis_[static_cast<size_t>(unit)];
  const battle::Unit& unit_rec = battle_->unit(unit);
  if (!visual.has_model || !unit_rec.present) return false;
  const std::string stem = unit_model_stem(unit_rec);
  const std::optional<Bytes> obj = stem.empty() ? std::nullopt : vfs_->read("cdrom/" + stem + ".obj");
  std::string err;
  auto clips = obj ? import::load_mdl_clips(*obj, data, "tech", &err) : std::nullopt;
  if (!clips || clips->empty()) {
    log::warn("battle fx: eid {} clip pack '{}' on unit {} ({}): {}", eid, name, unit, stem, err.empty() ? "not loaded" : err);
    return false;
  }
  visual.tech_clips = std::move(*clips);
  visual.pose.set_ext_clips(&visual.tech_clips);
  log::debug("battle fx: eid {} clip pack '{}' ({} clip(s), {} keyframes) hung on unit {} ({}) as SetAnim 0x80+", eid, name, visual.tech_clips.size(),
             visual.tech_clips[0].frame_count, unit, stem);
  return true;
}

// The pack end. A pose still on one of the dropped clips shows the rest pose until its next SetAnim (the idle restart); nothing plays an
// external clip in between.
void BattleView::fx_drop_clip_packs() {
  for (int i = 0; i < battle::kUnits; ++i) {
    Visual& visual = vis_[static_cast<size_t>(i)];
    if (!visual.pose.ext_clips()) continue;
    visual.pose.set_ext_clips(nullptr);
    visual.tech_clips.clear();
  }
}

// The resident battle.prd meshes (maru, wa0, sank ...), uploaded once per scene.
BattleView::FxGpu* BattleView::fx_gpu_resident(const std::shared_ptr<const FxPack>& pack) {
  if (!pack || !pack->resident || !pack->resident->meshes) return nullptr;
  constexpr int kResidentKey = -1000;
  if (auto it = fx_gpu_.find(kResidentKey); it != fx_gpu_.end()) return it->second.get();
  auto gpu = std::make_unique<FxGpu>();
  const assets::StageAsset& st = *pack->resident->meshes;
  for (size_t i = 0; i < st.parts.size(); ++i) gpu->parts.emplace(st.parts[i].name, static_cast<int>(i));
  gpu->stage = stage_pipe_.create(st);
  FxGpu* raw = gpu.get();
  fx_gpu_[kResidentKey] = std::move(gpu);
  return raw;
}

BattleView::FxGpu* BattleView::fx_gpu_for(const std::shared_ptr<const FxPack>& pack) {
  if (auto it = fx_gpu_.find(pack->eid); it != fx_gpu_.end()) return it->second.get();
  auto gpu = std::make_unique<FxGpu>();
  gpu->sprites = std::make_unique<effects::GpuEffects>();
  effects::EffectBank bank;
  bank.add(&pack->fx);
  if (pack->extra) bank.add(pack->extra.get());
  // battle.prd's sheets (b28, 64ke, smok ...): resident, named by the packs.
  if (pack->resident)
    for (const auto& resident_pack : pack->resident->packs) bank.add_library(resident_pack.get());
  bank.add_library(&fxlib_.particle_lib());
  const void* lib_key = pack->resident.get();
  // The library sheets (battle.prd's, the particle library): one upload per scene.
  if (!fx_lib_gpu_ || fx_lib_key_ != lib_key) {
    fx_lib_gpu_.reset();
    auto lib = std::make_shared<effects::GpuEffects>();
    if (lib->init(*ctx_, *sprites_) && lib->load(bank, effects::GpuEffects::Which::LibrariesOnly)) {
      fx_lib_gpu_ = std::move(lib);
      fx_lib_key_ = lib_key;
    }
  }
  if (fx_lib_gpu_) gpu->sprites->set_shared(fx_lib_gpu_);
  if (!gpu->sprites->init(*ctx_, *sprites_) ||
      !gpu->sprites->load(bank, fx_lib_gpu_ ? effects::GpuEffects::Which::SkipLibraries : effects::GpuEffects::Which::All)) {
    log::warn("battle fx: eid {}: texture upload failed", pack->eid);
    return nullptr;
  }
  if (pack->meshes) {
    const assets::StageAsset& st = *pack->meshes;
    for (size_t i = 0; i < st.parts.size(); ++i) gpu->parts.emplace(st.parts[i].name, static_cast<int>(i));
    // Effect meshes draw through the stage pipeline.
    gpu->stage = stage_pipe_.create(st);
  }
  fx_load_models(*gpu, *pack);
  // Bespoke overlays' own textured packets sample the pack's mesh atlases through the particle batch.
  if (pack->meshes && pack->bespoke) {
    const assets::StageAsset& st = *pack->meshes;
    for (const assets::StageTexture& stage_tex : st.textures) {
      auto tex = std::make_unique<render::Texture>();
      render::TexRef set;
      if (stage_tex.image.width && stage_tex.image.height && !stage_tex.image.indexed &&
          tex->create(*ctx_, stage_tex.image.width, stage_tex.image.height, render::TextureFormat::Rgba8, stage_tex.image.pixels.data()))
        set = render::TexRef(*tex, nearest_);
      gpu->textures.push_back(std::move(tex));
      gpu->sets.push_back(set);
    }
  }
  FxGpu* raw = gpu.get();
  fx_gpu_[pack->eid] = std::move(gpu);
  return raw;
}
}  // namespace oc::game
