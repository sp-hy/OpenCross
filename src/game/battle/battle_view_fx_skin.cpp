// BattleView: effect pack models: skinning, attachment, textures and VRAM residency.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>

#include "core/log.h"
#include "import/container.h"
#include "import/model.h"
#include "import/tim.h"
#include "game/battle/battle_view.h"
#include "game/battle/battle_view_util.h"

namespace oc::game {

using namespace bv;

// A bespoke overlay's own draws of a unit's mesh (FxModel::copies): the posed mesh is skinned on the CPU (the palette
// the mesh pipeline uses), every vertex goes to the PS1 world (y down), is offset and deformed by the module, projected with the effect view,
// back faces are culled (NCLIP > 0 = front, screen y down) and each face goes out as one textured triangle at the OT slot of its mean depth
// (AVSZ3 >> 4 like the effect meshes) with the copy's colour and blend (PS1 ABR 0..3 -> particle blends 1..4).
void BattleView::fx_model_copies(const effects::ViewParams& view, const render::Letterbox& lb, u32 order, const FxTriSink& sink) {
  const effects::FxModels* ms = fx_ ? fx_->bespoke_models() : nullptr;
  if (!ms) return;
  size_t emitted = 0;
  static const bool trace = std::getenv("OC_BESPOKE_TRACE") != nullptr;
  struct Log {
    size_t& count;
    int tick;
    ~Log() {
      if (trace && count && tick % 30 == 0) log::debug("bespoke copies: {} faces drawn", count);
    }
  } log_at_end{emitted, tick_count_};
  for (const effects::FxModel& om : ms->units) {
    if (om.copies.empty() || om.unit < 0 || om.unit >= battle::kUnits) continue;
    const Visual& visual = vis_[static_cast<size_t>(om.unit)];
    if (!battle_->unit(om.unit).present || !visual.has_model || !visual.model->gpu) continue;
    const float x = lerp(visual.px, visual.x, alpha_), z = lerp(visual.pz, visual.z, alpha_), yl = lerp(visual.py, visual.y, alpha_);
    const float fx = lerp(visual.pface_x, visual.face_x, alpha_), fz = lerp(visual.pface_z, visual.face_z, alpha_);
    render::Mat4 flip;
    flip.at(1, 1) = -1.0f;
    render::Mat4 model_mat = render::mat4_translation(x, yl, z) * render::mat4_rotation_y(std::atan2(-fx, -fz)) * flip;
    if (om.steer) {
      const effects::FxModelPose op = effects::fx_model_pose(om, alpha_);
      model_mat = render::mat4_translation(op.pos[0], op.pos[1], op.pos[2]) * render::mat4_rotation_y(std::atan2(-fx, -fz)) * flip *
                  render::mat4_from_rt(op.rel.m, {0, 0, 0});
    }
    visual.pose.palette(alpha_, palette_);
    const assets::ModelAsset& asset = visual.model->asset;
    const render::MeshModel& gpu = *visual.model->gpu;
    auto set_for = [&](int tex) -> render::TexRef {
      if (tex < 0) return white_.set;
      const bool use_hd = hd_ && static_cast<size_t>(tex) < gpu.hd_textures.size() && gpu.hd_textures[static_cast<size_t>(tex)];
      const render::Texture* texture = use_hd ? gpu.hd_textures[static_cast<size_t>(tex)].get()
                                              : (static_cast<size_t>(tex) < gpu.textures.size() ? gpu.textures[static_cast<size_t>(tex)].get() : nullptr);
      if (!texture) return white_.set;
      return fx_textures_.model_texture_set(*sprites_, hd_ ? linear_ : nearest_, *texture);
    };
    for (const effects::FxModel::Copy& cp : om.copies) {
      for (size_t mi = 0; mi < asset.meshes.size(); ++mi) {
        const bool part_off = (mi < asset.shading.size() && (asset.shading[mi].flags & 1)) || (mi < 32 && (om.part_hide >> mi) & 1u);
        // Part hidden by the shading flags / the overlay's part flags.
        if (part_off && !(mi < 32 && (om.part_show >> mi) & 1u)) continue;
        const assets::Mesh& mesh = asset.meshes[mi];
        if (mesh.trans == 0xFF) continue;
        // Skinned + deformed + projected pool.
        std::vector<std::array<float, 3>> scr(mesh.pool.size());
        std::vector<u8> ok(mesh.pool.size(), 0);
        for (size_t pi = 0; pi < mesh.pool.size(); ++pi) {
          const assets::SkinVertex& sv = mesh.pool[pi];
          float skinned[3] = {0, 0, 0};
          for (size_t k = 0; k < 2; ++k) {
            if (sv.weight[k] <= 0.0f || sv.joint[k] >= palette_.size()) continue;
            const render::Mat4& joint = palette_[sv.joint[k]];
            for (int row = 0; row < 3; ++row)
              skinned[row] += sv.weight[k] * (joint.at(row, 0) * sv.local[k][0] + joint.at(row, 1) * sv.local[k][1] + joint.at(row, 2) * sv.local[k][2] + joint.at(row, 3));
          }
          if (!mesh.skinned) skinned[0] = sv.bind_pos[0], skinned[1] = sv.bind_pos[1], skinned[2] = sv.bind_pos[2];
          effects::Vec3 world{};
          for (int row = 0; row < 3; ++row) world[static_cast<size_t>(row)] = model_mat.at(row, 0) * skinned[0] + model_mat.at(row, 1) * skinned[1] + model_mat.at(row, 2) * skinned[2] + model_mat.at(row, 3);
          effects::Vec3 ps1{world[0] + cp.offset[0], -world[1] + cp.offset[1], world[2] + cp.offset[2]};
          if (cp.deform) cp.deform(ps1);
          effects::Vec3 cam = view.rot.apply(ps1);
          for (size_t row = 0; row < 3; ++row) cam[row] += view.trans[row];
          if (cam[2] < 64.0f) continue;
          scr[pi] = {lb.ox + (view.ox + view.h * cam[0] / cam[2] + view.scroll_x) * lb.scale, lb.oy + (view.oy + view.h * cam[1] / cam[2] + view.scroll_y) * lb.scale_y,
                     cam[2]};
          ok[pi] = 1;
        }
        for (const assets::SubMesh& sm : mesh.submeshes) {
          const int tex = sm.material < mesh.materials.size() ? mesh.materials[sm.material].texture : -1;
          const render::TexRef set = set_for(tex);
          for (u32 k = sm.first_index; k + 2 < sm.first_index + sm.index_count && k + 2 < mesh.indices.size(); k += 3) {
            const assets::MeshVertex* mv[3] = {&mesh.vertices[mesh.indices[k]], &mesh.vertices[mesh.indices[k + 1]], &mesh.vertices[mesh.indices[k + 2]]};
            if (!ok[mv[0]->pool] || !ok[mv[1]->pool] || !ok[mv[2]->pool]) continue;
            const auto& p0 = scr[mv[0]->pool];
            const auto& p1 = scr[mv[1]->pool];
            const auto& p2 = scr[mv[2]->pool];
            // Back-face cull.
            if ((p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0]) <= 0.0f) continue;
            float xs[4] = {p0[0], p1[0], p2[0], p2[0]}, ys[4] = {p0[1], p1[1], p2[1], p2[1]};
            float us[4] = {mv[0]->uv[0], mv[1]->uv[0], mv[2]->uv[0], mv[2]->uv[0]}, vs[4] = {mv[0]->uv[1], mv[1]->uv[1], mv[2]->uv[1], mv[2]->uv[1]};
            const int slot = std::clamp(static_cast<int>((p0[2] + p1[2] + p2[2]) / 3.0f) >> 4, 1, 0xFFF);
            u8 col[3];
            for (size_t ch = 0; ch < 3; ++ch) {
              const float base = tex < 0 && sm.material < mesh.materials.size() ? mesh.materials[sm.material].color[ch] / 255.0f : 1.0f;
              col[ch] = static_cast<u8>(std::clamp(static_cast<float>(cp.rgb[ch]) * base, 0.0f, 255.0f));
            }
            sink(set, xs, ys, us, vs, col[0], col[1], col[2], cp.abr < 0 ? 0 : std::clamp(cp.abr + 1, 1, 4), slot, order++);
            ++emitted;
          }
        }
      }
    }
  }
}

// Requests of a bespoke overlay on the models it steers: SetAnim and anmseq starts (table by tag + entry; entries >= 0x80 address the
// assigned external table, smaller ones the model's own S5 block).
void BattleView::fx_model_requests(bool at_init) {
  effects::FxModels* ms = fx_ ? fx_->bespoke_models() : nullptr;
  if (!ms) return;
  if (!at_init) fx_pack_drive_tick(*ms);
  if (ms->pack_closed) {
    ms->pack_closed = false;
    audio_.end_pack();
  }
  for (effects::FxModel& fx_model : ms->units) {
    if (fx_model.unit < 0 || fx_model.unit >= battle::kUnits) continue;
    Visual& visual = vis_[static_cast<size_t>(fx_model.unit)];
    // A pack brought an off-stage unit on (opthedo's pup): it stays drawn after the effect.
    if (fx_model.reveal) entry_shown_[static_cast<size_t>(fx_model.unit)] = true;
    if (fx_model.face_rot && !fx_model.release) {
      const float yaw = static_cast<float>(fx_model.rot[1] & 0xFFF) * 6.2831853f / 4096.0f;
      visual.face_x = visual.pface_x = -std::sin(yaw);
      visual.face_z = visual.pface_z = -std::cos(yaw);
    }
    // The pack lets go of the model where it stands: the unit's own placement continues from there (its anmseq MoveHome starts here).
    if (fx_model.release) {
      fx_model.release = false;
      fx_model.steer = false;
      visual.x = visual.px = fx_model.pos[0];
      visual.z = visual.pz = fx_model.pos[2];
      const float yaw = static_cast<float>(fx_model.rot[1] & 0xFFF) * 6.2831853f / 4096.0f;
      visual.face_x = visual.pface_x = -std::sin(yaw);
      visual.face_z = visual.pface_z = -std::cos(yaw);
    }
    // The pack hid the model: it stays undrawn until the action ends (the script's ghost flag takes over).
    if (fx_model.conceal) {
      fx_model.conceal = false;
      fx_conceal_[static_cast<size_t>(fx_model.unit)] = true;
    }
    if (fx_model.rebuilt) {
      fx_model.rebuilt = false;
      fx_hide({fx_model.unit}, 1, false);
      if (battle_->unit(fx_model.unit).in_turn_order()) start_spawn(fx_model.unit);
    }
    // Hang the pack's anim pack: the external clip set SetAnim 0x80 + k reads.
    if (!fx_model.clip_pack.empty()) {
      for (const auto& [name, data] : fx_pack_->clip_packs)
        if (name == fx_model.clip_pack) fx_hang_clip(fx_pack_->eid, name, data, fx_model.unit);
      fx_model.clip_pack.clear();
    }
    if (fx_model.clip >= 0 && visual.has_model) {
      visual.pose.play(fx_model.clip, fx_model.clip_flags & 0x1FFF, false, (fx_model.clip_flags & 0x6000) >> 13);
      // A pack's own clip plays once and holds its last keyframe (the actions' anmseq scripts switch the loop off, op 0x18).
      if (fx_model.clip >= 0x80) visual.pose.set_loop(false);
      if (fx_model.clip_loop >= 0) visual.pose.set_loop(fx_model.clip_loop != 0);
      // The clip's frame count: what the overlays' wait reads.
      fx_model.clip_frames = visual.pose.frames_of(fx_model.clip);
    }
    if (fx_model.anim_speed > 0 && visual.has_model) visual.pose.set_speed(fx_model.anim_speed);
    if (fx_model.script_entry >= 0) {
      const bseq::Table* table = nullptr;
      if (fx_model.script_entry >= 0x80) {
        if (!fx_model.script_table.empty()) {
          if (auto it = fx_pack_->tables.find(fx_model.script_table); it != fx_pack_->tables.end()) table = &it->second;
        } else if (fx_pack_->has_anim) {
          table = &fx_pack_->anim;
        }
      } else if (visual.scripts && visual.scripts->has_s5) {
        table = &visual.scripts->s5;
      }
      const int entry = fx_model.script_entry >= 0x80 ? fx_model.script_entry - 0x80 : fx_model.script_entry;
      const bool ok = table && start_script(fx_model.unit, table, entry, fx_model.script_variant, kReact);
      // The init's start steps the script in the same pass (the anmseq step of the PS1 loop follows the hand-over that ran the init):
      // otherwise everything the script starts (cam0, fgon, the React) trails the pack by one tick.
      if (ok && at_init) {
        bseq::Ctx ctx;
        fill_ctx(fx_model.unit, ctx);
        std::vector<bseq::Event> events;
        visual.vm.tick(ctx, events);
        for (const bseq::Event& event : events) on_script_event(fx_model.unit, event);
        if (!visual.vm.running()) visual.script_done = true;
      }
      log::debug("battle fx: overlay anmseq unit {} table '{}' entry {} variant {} {}{}", fx_model.unit, fx_model.script_table, entry, fx_model.script_variant,
                 ok ? "started" : "missing", ok && at_init ? " (stepped)" : "");
    }
    fx_model.clip = -1;
    fx_model.clip_loop = -1;
    fx_model.script_entry = -1;
  }
}

// FxModels::PackDrive (saraman): the pack model the overlay places and animates itself. After the overlay's tick:
// load the motion file it asked for (decoded against the 'sara' skeleton and
// appended to the model's clips as source "mtc<n>"), its SetAnim (id 0x80 = clip 0 of the attached file), the anim
// speed and the pose step; `at_end` is set for the next tick.
void BattleView::fx_pack_drive_tick(effects::FxModels& ms) {
  effects::FxModels::PackDrive& drive = ms.pack_drive;
  if (!drive.active) return;
  for (size_t k = 0; k < drive.motion_read_left.size(); ++k)
    if (drive.motion_read_left[k] > 0 && --drive.motion_read_left[k] == 0) drive.motion_state[k] = drive.motion_read_state[k];
  FxGpu* gpu = fx_pack_ ? fx_gpu_for(fx_pack_) : nullptr;
  const auto it = gpu ? gpu->models.find(drive.name) : std::map<std::string, std::unique_ptr<FxGpu::Model>>::iterator{};
  // No such model in the pack: every request is consumed, a stream counts as failed.
  if (!gpu || it == gpu->models.end()) {
    if (drive.motion_request >= 0 && static_cast<size_t>(drive.motion_request) < drive.motion_state.size()) drive.motion_state[static_cast<size_t>(drive.motion_request)] = 2;
    drive.motion_request = -1;
    drive.clip = -1;
    drive.at_end = true;
    return;
  }
  FxGpu::Model& fx_model = *it->second;
  if (drive.motion_request >= 0) {
    const int motion_index = drive.motion_request;
    drive.motion_request = -1;
    if (static_cast<size_t>(motion_index) < fx_model.motion_clip.size()) {
      int& mc = fx_model.motion_clip[static_cast<size_t>(motion_index)];
      if (mc == -1) {
        mc = -2;
        const std::string& file = fxlib_.summon_motion_file(motion_index);
        std::optional<Bytes> mtc = file.empty() || !vfs_ ? std::nullopt : vfs_->read("cdrom/" + file);
        if (mtc) drive.motion_read_left[static_cast<size_t>(motion_index)] = effects::disc_read_ticks(mtc->size()) - 1;
        ByteSpan mdl;
        auto drp = import::parse_drp(fx_pack_->prd);
        if (drp)
          for (const auto& entry : drp->entries)
            if (entry.type == 0x0B && entry.name == drive.name) {
              mdl = entry.data;
              break;
            }
        std::string err;
        if (mtc && !mdl.empty()) {
          if (auto clips = import::load_mdl_clips(mdl, ByteSpan(*mtc), "mtc", &err); clips && !clips->empty()) {
            mc = static_cast<int>(fx_model.asset.clips.size());
            for (auto& clip : *clips) {
              clip.source = std::format("mtc{}", motion_index);
              clip.name = std::format("mtc{}_{:02}", motion_index, clip.index);
              fx_model.asset.clips.push_back(std::move(clip));
            }
            log::debug("battle fx: pack model {}: motion file {} '{}' = {} clip(s), {} keyframes", drive.name, motion_index, file,
                       fx_model.asset.clips.size() - static_cast<size_t>(mc), fx_model.asset.clips[static_cast<size_t>(mc)].frame_count);
          }
        }
        if (mc < 0) log::warn("battle fx: pack model {}: motion file {} '{}' not loaded{}{}", drive.name, motion_index, file, err.empty() ? "" : ": ", err);
      }
      const size_t request = static_cast<size_t>(motion_index);
      drive.motion_read_state[request] = mc >= 0 ? 1 : 2;
      if (drive.motion_read_left[request] <= 0) drive.motion_state[request] = drive.motion_read_state[request];
    }
  }
  if (drive.clip >= 0) {
    const int blend = drive.clip_flags & 0x1FFF;
    if (drive.clip >= 0x80) {
      const int base = drive.motion >= 0 && static_cast<size_t>(drive.motion) < fx_model.motion_clip.size() ? fx_model.motion_clip[static_cast<size_t>(drive.motion)] : -1;
      if (base >= 0) fx_model.pose.play_clip(base + (drive.clip - 0x80), blend);
    } else {
      fx_model.pose.play(drive.clip, blend);
    }
    drive.clip = -1;
  }
  fx_model.pose.set_loop(false);
  fx_model.pose.set_speed(drive.speed);
  fx_model.pose.tick();
  drive.at_end = fx_model.pose.at_end();
}

// FxModels::skin_pack while the overlay drives the pack model (PackDrive): the block's place as the overlay wrote it (the root is built
// from its rotation and scale like a steered unit, without a home yaw) and the pose the host stepped for it.
bool BattleView::fx_skin_pack_driven(effects::FxModels& ms, effects::FxSkinnedModel& out) {
  const effects::FxModels::PackDrive& drive = ms.pack_drive;
  FxGpu* gpu = fx_pack_ ? fx_gpu_for(fx_pack_) : nullptr;
  if (!gpu) return false;
  const auto it = gpu->models.find(drive.name);
  if (it == gpu->models.end()) return false;
  FxGpu::Model& fx_model = *it->second;
  ms.pack_model = drive.name;
  effects::FxModel tmp;
  tmp.pos = tmp.prev_pos = drive.pos;
  for (int k = 0; k < 3; ++k) tmp.rot[k] = tmp.prev_rot[k] = drive.rot[k], tmp.scale[k] = tmp.prev_scale[k] = drive.scale[k];
  const effects::FxModelPose op = effects::fx_model_pose(tmp, 1.0f);
  render::Mat4 flip;
  flip.at(1, 1) = -1.0f;
  const render::Mat4 model = render::mat4_translation(op.pos[0], op.pos[1], op.pos[2]) * flip * render::mat4_from_rt(op.rel.m, {0, 0, 0});
  fx_model.pose.palette(alpha_, palette_);
  fx_skin_asset(fx_model.asset, model, nullptr, out);
  out.pos = drive.pos;
  for (int k = 0; k < 3; ++k) out.rgb[k] = drive.rgb[k];
  return true;
}

// FxModels::skin: the overlays' own skinning of a unit. In battle every vertex = the joint's world matrix x the pool record (WORLD space,
// PS1 axes), every normal byte = clamp(R x (VDDM s8 << 5) >> 12, -0xFFF, 0xFFF) >> 5 with R = the rotation of the joint the record belongs
// to (8-byte vertex: its joint; 16-byte: J2 with rec0's record). The pose and placement are the ones the unit is drawn with (steered by the
// overlay or not).
render::Mat4 BattleView::fx_unit_matrix(int unit) const {
  const Visual& visual = vis_[static_cast<size_t>(unit)];
  const effects::FxModels* ms = fx_ ? fx_->bespoke_models() : nullptr;
  const effects::FxModel* om = ms && static_cast<size_t>(unit) < ms->units.size() ? &ms->units[static_cast<size_t>(unit)] : nullptr;
  const float x = lerp(visual.px, visual.x, alpha_), z = lerp(visual.pz, visual.z, alpha_), yl = lerp(visual.py, visual.y, alpha_);
  const float fx = lerp(visual.pface_x, visual.face_x, alpha_), fz = lerp(visual.pface_z, visual.face_z, alpha_);
  render::Mat4 flip;
  flip.at(1, 1) = -1.0f;
  render::Mat4 model = render::mat4_translation(x, yl, z) * render::mat4_rotation_y(std::atan2(-fx, -fz)) * flip;
  if (om && om->steer) {
    const effects::FxModelPose op = effects::fx_model_pose(*om, alpha_);
    model = render::mat4_translation(op.pos[0], op.pos[1], op.pos[2]) * render::mat4_rotation_y(std::atan2(-fx, -fz)) * flip *
            render::mat4_from_rt(op.rel.m, {0, 0, 0});
  }
  return model;
}

// FxModels::attach: the attach point of the S3 table in its joint's space, through the posed joint, in the PS1 world.
bool BattleView::fx_attach_unit(int unit, int point, effects::Vec3& out, bool raw) {
  if (unit < 0 || unit >= battle::kUnits || !battle_->unit(unit).present) return false;
  const Visual& visual = vis_[static_cast<size_t>(unit)];
  float mx = 0, my = 0, mz = 0;
  if (!visual.has_model || !visual.pose.attach_or_root(point, alpha_, mx, my, mz)) return false;
  render::Mat4 model = fx_unit_matrix(unit);
  // A held model's joint matrices stay as the last (pre-hide) pass left them, placement and facing included: the link's offset
  // (attach - pos) is then stale and each tick moves the model by (carrier attach - frozen attach).
  if (raw && visual.pose.held()) {
    render::Mat4 flip;
    flip.at(1, 1) = -1.0f;
    model = render::mat4_translation(visual.hold_x, visual.hold_y, visual.hold_z) * render::mat4_rotation_y(std::atan2(-visual.hold_fx, -visual.hold_fz)) * flip;
  }
  effects::Vec3 world{};
  for (int row = 0; row < 3; ++row) world[static_cast<size_t>(row)] = model.at(row, 0) * mx + model.at(row, 1) * my + model.at(row, 2) * mz + model.at(row, 3);
  out = {world[0], -world[1], world[2]};
  return true;
}

bool BattleView::fx_skin_unit(int unit, effects::FxSkinnedModel& out) {
  out = {};
  if (unit == effects::kPseudoUnit) {
    if (!pa_.on || !pa_.pose || !pa_.asset) return false;
    pa_.pose->palette(alpha_, palette_);
    fx_skin_asset(*pa_.asset, pa_matrix(alpha_), nullptr, out);
    out.pos = {pa_.x, -pa_.y, pa_.z};
    for (int k = 0; k < 3; ++k) out.rgb[k] = pa_.rgb[k];
    return true;
  }
  if (unit < 0 || unit >= battle::kUnits || !battle_->unit(unit).present) return false;
  const Visual& visual = vis_[static_cast<size_t>(unit)];
  if (!visual.has_model || !visual.model) return false;
  const effects::FxModels* ms = fx_ ? fx_->bespoke_models() : nullptr;
  const effects::FxModel* om = ms && static_cast<size_t>(unit) < ms->units.size() ? &ms->units[static_cast<size_t>(unit)] : nullptr;
  const render::Mat4 model = fx_unit_matrix(unit);
  visual.pose.palette(alpha_, palette_);
  fx_skin_asset(visual.model->asset, model, om, out);
  // PS1 y down, as the overlays' own block fields.
  out.pos = {om ? om->pos[0] : visual.x, om ? om->pos[1] : -visual.y, om ? om->pos[2] : visual.z};
  // The model colour: the overlay's write and the pack's op 0x58 ramp (one colour field on the PS1, kept apart here).
  const FxUnitState& state = fx_unit_[static_cast<size_t>(unit)];
  for (int k = 0; k < 3; ++k) {
    const float colour = (om ? static_cast<float>(om->rgb[k]) : 128.0f) * (state.tinted ? state.rgb[k] / 128.0f : 1.0f);
    out.rgb[k] = static_cast<int>(std::clamp(colour, 0.0f, 255.0f));
  }
  return true;
}

// The skinning of fx_skin_unit / fx_skin_pack on a posed asset (palette_ filled): WORLD vertices (PS1 axes), normal bytes, faces.
void BattleView::fx_skin_asset(const assets::ModelAsset& asset, const render::Mat4& model_mat, const effects::FxModel* om, effects::FxSkinnedModel& out) {
  out.parts = static_cast<int>(std::min<size_t>(asset.meshes.size(), out.shading.size()));
  for (const import::Image& im : asset.textures) out.tex_width.push_back(static_cast<int>(im.width));
  for (size_t i = 0; i < out.shading.size(); ++i) {
    const assets::ShadingEntry& entry = asset.shading[i];
    u8 flags = entry.flags;
    if (om && i < 32 && ((om->part_hide >> i) & 1u)) flags = static_cast<u8>(flags | 1);
    if (om && i < 32 && ((om->part_show >> i) & 1u)) flags = static_cast<u8>(flags & ~1);
    out.shading[i] = {entry.r, entry.g, entry.b, flags};
  }
  for (size_t mi = 0; mi < asset.meshes.size() && static_cast<int>(mi) < out.parts; ++mi) {
    const assets::Mesh& mesh = asset.meshes[mi];
    const u32 base = static_cast<u32>(out.verts.size());
    for (const assets::SkinVertex& sv : mesh.pool) {
      float skinned[3] = {0, 0, 0}, normal[3] = {0, 0, 0};
      for (size_t k = 0; k < 2; ++k) {
        if (sv.weight[k] <= 0.0f || sv.joint[k] >= palette_.size()) continue;
        const render::Mat4& joint = palette_[sv.joint[k]];
        for (int row = 0; row < 3; ++row)
          skinned[row] += sv.weight[k] * (joint.at(row, 0) * sv.local[k][0] + joint.at(row, 1) * sv.local[k][1] + joint.at(row, 2) * sv.local[k][2] + joint.at(row, 3));
      }
      if (!mesh.skinned) skinned[0] = sv.bind_pos[0], skinned[1] = sv.bind_pos[1], skinned[2] = sv.bind_pos[2];
      const size_t nj = sv.normal_joint;
      if (mesh.skinned && sv.joint[nj] < palette_.size()) {
        const render::Mat4& joint = palette_[sv.joint[nj]];
        for (int row = 0; row < 3; ++row) normal[row] = joint.at(row, 0) * sv.normal[0] + joint.at(row, 1) * sv.normal[1] + joint.at(row, 2) * sv.normal[2];
      } else {
        normal[0] = sv.normal[0], normal[1] = sv.normal[1], normal[2] = sv.normal[2];
      }
      effects::Vec3 world{}, world_normal{};
      for (int row = 0; row < 3; ++row) {
        world[static_cast<size_t>(row)] = model_mat.at(row, 0) * skinned[0] + model_mat.at(row, 1) * skinned[1] + model_mat.at(row, 2) * skinned[2] + model_mat.at(row, 3);
        world_normal[static_cast<size_t>(row)] = model_mat.at(row, 0) * normal[0] + model_mat.at(row, 1) * normal[1] + model_mat.at(row, 2) * normal[2];
      }
      out.verts.push_back({world[0], -world[1], world[2]});
      std::array<s8, 3> nb{};
      const float ps1n[3] = {world_normal[0], -world_normal[1], world_normal[2]};
      for (size_t row = 0; row < 3; ++row) {
        // (n << 5) rotated, >> 12 by the GTE.
        const int scaled = std::clamp(static_cast<int>(std::floor(ps1n[row] * 32.0f)), -0xFFF, 0xFFF);
        nb[row] = static_cast<s8>(scaled >> 5);
      }
      out.normals.push_back(nb);
    }
    for (const assets::SubMesh& sm : mesh.submeshes) {
      const int tex = sm.material < mesh.materials.size() ? mesh.materials[sm.material].texture : -1;
      // Flat faces are never handed to a builder.
      if (tex < 0) continue;
      const u32 end = std::min<u32>(sm.first_index + sm.index_count, static_cast<u32>(mesh.indices.size()));
      for (u32 k = sm.first_index; k + 2 < end; k += 3) {
        const size_t tri = k / 3;
        effects::FxSkinFace face;
        face.tex = tex;
        face.part = static_cast<int>(mi);
        const u32 i0 = mesh.indices[k], i1 = mesh.indices[k + 1], i2 = mesh.indices[k + 2];
        const bool quad = tri < mesh.tri_quad.size() && mesh.tri_quad[tri] == 1 && k + 5 < end && tri + 1 < mesh.tri_quad.size() && mesh.tri_quad[tri + 1] == 2;
        // Halves (v0, v1, v2) / (v2, v1, v3).
        const u32 ids[4] = {i0, i1, i2, quad ? mesh.indices[k + 5] : i2};
        face.n = quad ? 4 : 3;
        for (int corner = 0; corner < face.n; ++corner) {
          const assets::MeshVertex& mv = mesh.vertices[ids[corner]];
          face.v[corner] = base + mv.pool;
          face.u[corner] = mv.uv[0];
          face.w[corner] = mv.uv[1];
        }
        out.faces.push_back(face);
        if (quad) k += 3;
      }
    }
  }
}

// FxModels::skin_pack: the pack's sprite-type-20 model as the last of its live objects placed it (the object's place,
// angles, scale and colour are copied into the shared model block every tick; with no live object the block keeps the last ones).
bool BattleView::fx_skin_pack(effects::FxSkinnedModel& out) {
  out = {};
  if (effects::FxModels* dm = fx_ ? fx_->bespoke_models() : nullptr; dm && dm->pack_drive.active) return fx_skin_pack_driven(*dm, out);
  FxGpu* fx_gpu = fx_ && fx_pack_ ? fx_gpu_for(fx_pack_) : nullptr;
  if (!fx_gpu || fx_gpu->models.empty()) return false;
  if (fx_gpu->models.empty() || !fx_->bespoke_models()) return false;
  std::vector<effects::EmitterSystem::MeshInstance> inst;
  fx_->emitters().collect_meshes(fx_view(), alpha_, inst, true);
  for (const auto& mi : inst)
    if (mi.sprite_type == 20 && mi.name && fx_gpu->models.count(*mi.name)) {
      fx_pack_place_.name = *mi.name;
      fx_pack_place_.pos = mi.world_pos;
      fx_pack_place_.mat = mi.world_mat;
      fx_pack_place_.clip = mi.clip;
      fx_pack_place_.loop = mi.loop;
      fx_pack_place_.motion = mi.motion_prev + (mi.motion - mi.motion_prev) * alpha_;
      for (int k = 0; k < 3; ++k) fx_pack_place_.rgb[k] = mi.rgb[k];
      fx_pack_place_.valid = true;
    }
  if (!fx_pack_place_.valid) return false;
  const auto it = fx_gpu->models.find(fx_pack_place_.name);
  if (it == fx_gpu->models.end()) return false;
  FxGpu::Model& fx_model = *it->second;
  fx_->bespoke_models()->pack_model = fx_pack_place_.name;
  render::Mat4 model = render::mat4_translation(fx_pack_place_.pos[0], -fx_pack_place_.pos[1], fx_pack_place_.pos[2]);
  render::Mat4 rs;
  for (int row = 0; row < 3; ++row)
    for (int col_index = 0; col_index < 3; ++col_index) rs.at(row, col_index) = fx_pack_place_.mat.m[static_cast<size_t>(row * 3 + col_index)] * (row == 1 ? -1.0f : 1.0f);
  model = model * rs;
  const int clip = fx_pack_place_.clip >= 0 && fx_pack_place_.clip < 0x80 && static_cast<size_t>(fx_pack_place_.clip) < fx_model.asset.clips.size()
                       ? fx_pack_place_.clip
                       : fx_pack_place_.clip < 0 ? fx_model.clip : -1;
  // Plus the overlay's own pose steps (the trap summons).
  float mf = fx_pack_place_.motion + fx_->bespoke_models()->pack_extra_frames;
  if (clip >= 0 && !fx_pack_place_.loop && fx_pack_place_.clip >= 0)
    mf = std::min(mf, std::max(0.0f, static_cast<float>(fx_model.asset.clips[static_cast<size_t>(clip)].frame_count) - 1.0f));
  fx_model.pose.palette_at(clip, mf, palette_);
  fx_skin_asset(fx_model.asset, model, nullptr, out);
  out.pos = fx_pack_place_.pos;
  for (int k = 0; k < 3; ++k) out.rgb[k] = fx_pack_place_.rgb[k];
  return true;
}

render::TexRef BattleView::fx_pack_model_texture_set(int tex) {
  const effects::FxModels* ms = fx_ ? fx_->bespoke_models() : nullptr;
  FxGpu* fx_gpu = ms && fx_pack_ ? fx_gpu_for(fx_pack_) : nullptr;
  if (!fx_gpu || tex < 0) return {};
  const auto it = fx_gpu->models.find(ms->pack_model);
  if (it == fx_gpu->models.end() || !it->second->gpu) return {};
  const render::MeshModel& gpu = *it->second->gpu;
  const render::Texture* texture = static_cast<size_t>(tex) < gpu.textures.size() ? gpu.textures[static_cast<size_t>(tex)].get() : nullptr;
  if (!texture) return {};
  return fx_textures_.model_texture_set(*sprites_, hd_ ? linear_ : nearest_, *texture);
}

render::TexRef BattleView::fx_unit_texture_set(int unit, int tex, u8 white) {
  if (tex < 0) return {};
  const assets::ModelAsset* asset = nullptr;
  const render::MeshModel* model_gpu = nullptr;
  if (unit == effects::kPseudoUnit) {
    asset = pa_.on ? pa_.asset : nullptr;
    model_gpu = pa_.on ? pa_.gpu : nullptr;
  } else if (unit >= 0 && unit < battle::kUnits) {
    const Visual& visual = vis_[static_cast<size_t>(unit)];
    if (visual.has_model && visual.model) asset = &visual.model->asset, model_gpu = visual.model->gpu.get();
  }
  if (!asset || !model_gpu) return {};
  // The white-CLUT silhouette: 1 = every non-transparent texel white, 2 = every texel white (STP set: blended).
  if (white) {
    if (static_cast<size_t>(tex) >= asset->textures.size()) return {};
    return fx_textures_.white_texture_set(*ctx_, *sprites_, hd_ ? linear_ : nearest_, asset->drawn_texture(static_cast<size_t>(tex), hd_), white);
  }
  const render::MeshModel& gpu = *model_gpu;
  const bool use_hd = hd_ && static_cast<size_t>(tex) < gpu.hd_textures.size() && gpu.hd_textures[static_cast<size_t>(tex)];
  const render::Texture* texture = use_hd ? gpu.hd_textures[static_cast<size_t>(tex)].get()
                                          : (static_cast<size_t>(tex) < gpu.textures.size() ? gpu.textures[static_cast<size_t>(tex)].get() : nullptr);
  if (!texture) return {};
  return fx_textures_.model_texture_set(*sprites_, hd_ ? linear_ : nearest_, *texture);
}

bool BattleView::fx_vram_ready() {
  bool rebuilt = false;
  const bool ready = fx_textures_.vram_ready(fx_pack_.get(), rebuilt);
  if (rebuilt) fx_pack_place_ = {};
  return ready;
}

// FxModels::vram_moves: applied to the pack's VRAM after each overlay tick. A pack model whose pages a copy lands in is cut again (lasdes2's
// eye frames on the 'ef8f' page).
void BattleView::fx_vram_apply() {
  effects::FxModels* ms = fx_ ? fx_->bespoke_models() : nullptr;
  if (!ms || ms->vram_moves.empty()) return;
  const std::vector<effects::FxModels::VramMove> moves = ms->vram_moves;
  const bool ready = fx_vram_ready();
  fx_textures_.apply_vram_moves(*ms, ready);
  FxGpu* gpu = ready ? fx_gpu_for(fx_pack_) : nullptr;
  if (!gpu || !ctx_) return;
  for (auto& [name, model] : gpu->models) {
    if (model->page_x < 0 || !model->gpu) continue;
    // The page and its neighbour column (the faces' page bit): 192 halfwords x 256 rows.
    const bool hit = std::any_of(moves.begin(), moves.end(), [&](const effects::FxModels::VramMove& move) {
      const int x = move.scroll ? move.x : move.dx, y = move.scroll ? move.y : move.dy;
      return x < model->page_x + 192 && x + move.w > model->page_x && y < model->page_y + 256 && y + move.h > model->page_y;
    });
    if (!hit) continue;
    std::vector<import::Image> pages = fx_model_pages(fx_textures_.vram, model->page_x, model->page_y, model->clut_x, model->clut_y);
    for (size_t row = 0; row < pages.size() && row < model->gpu->textures.size(); ++row)
      if (model->gpu->textures[row] && row < model->asset.textures.size() && model->asset.textures[row].width == pages[row].width &&
          model->asset.textures[row].height == pages[row].height) {
        model->gpu->textures[row]->update(*ctx_, pages[row].pixels.data());
        model->asset.textures[row].pixels = std::move(pages[row].pixels);
      }
  }
}

render::TexRef BattleView::fx_vram_page_set(u16 tpage, u16 clut) {
  if (!fx_vram_ready()) return {};
  return fx_textures_.vram_page_set(*ctx_, *sprites_, nearest_, tpage, clut);
}

render::TexRef BattleView::stock_env_page_set() {
  const assets::StageAsset* asset = stage_scene_.ready() ? stage_scene_.asset() : nullptr;
  return fx_textures_.stock_env_page_set(*ctx_, *sprites_, nearest_, asset);
}

// The shading entries of a model as the stock draw reads them (battle_view.h). Part colour: the per-part colour word is
// (model colour x entry rgb) >> 7 for every part (the env parts' word is multiplied the same way), so an entry rgb
// other than 0x80 scales that part's texels: sky's act0 / act1 (tech/sky.prd, 0xFC 0xFC 0xFC on parts 0 / 1) draw at ~2x, gyada / jakotu /
// malchera / pierre / zoah's env-over parts (0x60..0x70) at 0.75..0.88. Op 0xA0 (anm_unit_apply) writes the same word and overrides this.
void BattleView::shading_params(const assets::ModelAsset& asset, render::MeshPipeline::DrawParams& dp) {
  for (size_t k = 0; k < asset.shading.size() && k < 32; ++k) {
    const assets::ShadingEntry& entry = asset.shading[k];
    // Flag bit 0: the part starts hidden.
    if (entry.flags & 1) dp.hidden_meshes |= 1u << k;
    // Env-mapped with the model's own page.
    if ((entry.flags & 0x80) && (entry.flags & 0x60)) dp.env_meshes |= 1u << k;
    // Flag 0x20 / 0x40 without 0x80: the stock env page.
    if ((entry.flags & 0x60) && !(entry.flags & 0x80)) {
      if (entry.flags & 0x40) dp.stock_env_over |= 1u << k;
      else dp.stock_env_replace |= 1u << k;
      dp.stock_env_abr[k] = static_cast<u8>((entry.flags >> 2) & 3);
    }
    if (entry.r != 0x80 || entry.g != 0x80 || entry.b != 0x80) {
      dp.part_mask |= 1u << k;
      dp.part_tint[k][0] = entry.r / 128.0f, dp.part_tint[k][1] = entry.g / 128.0f, dp.part_tint[k][2] = entry.b / 128.0f;
    }
  }
  if (dp.stock_env_over || dp.stock_env_replace) dp.stock_env_set = stock_env_page_set();
}

}  // namespace oc::game
