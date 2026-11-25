#pragma once
// The Bestiary page's creature (EnemyModelSlot): the record's battle model as the battle picks it (a monster mods add by name with a
// model of its own wears that, any other the model of its model number in the shared battle model table) in its
// stand-by clip 1, looping at the battle's rate (half a keyframe per 30 Hz model tick), turning about its vertical axis by 8 (4096 = a turn)
// per model tick. Its centre and extent are measured once over the whole clip (with the weapon it holds); the camera stands just far enough
// back for every point to stay inside the slot's box at every turn, the box centre on the creature's centre. A silhouette is the same model drawn with its colour multiplied
// by 0. The model is drawn as the battle draws it: its shading entries (hidden parts, env-mapped parts on the model's own page, part
// colours; the stage's stock env page is not there, so those parts keep their own texture), one ordering-table slot per face, and the
// parts its spawn script hides or shows on its first tick. A record with a weapon (an enemy's weapon item, a character's equipped one)
// holds it as the battle attaches it: the part of the model's own weapon pack on the model's first mount (else the grip, the right hand
// or point 15) and on its second mount when it has one, each slot not drawn while the spawn script hides it. The last few models stay
// loaded, so scrolling back and forth does not read them again.
#include <array>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "game/actor.h"
#include "game/battle/battle_pose.h"
#include "render/mesh_pipeline.h"
#include "ui/menu/menu_model.h"
#include "vfs/vfs.h"

namespace oc::ui {

class EnemyModelLibrary : public EnemyModelView {
 public:
  EnemyModelLibrary(render::MeshPipeline& meshes, const vfs::Vfs& vfs) : meshes_(meshes), vfs_(vfs) {}
  void draw(const render::Frame& frame, float vw, float vh, const EnemyModelSlot& slot) override;
  // Drops the GPU models before the pipeline is destroyed.
  void release() { cache_.clear(); }

 private:
  struct Entry {
    // The model's file stem and its weapon pack ("" = none).
    std::string stem, weapon_bin;
    int weapon_part = -1;
    std::shared_ptr<game::ModelHandle> body, weapon;
    // Slot 0's attach point; -1 = none. The spawn script's attached-object hide words (bit s = slot s hidden) and part words.
    int weapon_attach = -1;
    int weapon_hide = 0;
    u32 part_hide = 0, part_show = 0;
    game::UnitPose pose;
    // The centre of the clip's bounds and its extent: per point its height off the centre and its distance from the vertical axis through
    // the centre, only the points no other one exceeds in both (highest first).
    float centre[3] = {0, 0, 0};
    std::vector<std::array<float, 2>> reach;
  };
  // The entry of model `stem` holding part `weapon_part` of `weapon_bin`, loaded when missing (null when the model cannot be read); the
  // newest entry is first.
  Entry* entry(const std::string& stem, const std::string& weapon_bin, int weapon_part);
  // The matrix the weapon of attached slot `weapon_slot` is drawn with in the pose `palette`; false when the slot carries none.
  static bool weapon_matrix(const Entry& held, size_t weapon_slot, const std::vector<render::Mat4>& palette, render::Mat4& out);
  render::MeshPipeline& meshes_;
  const vfs::Vfs& vfs_;
  std::list<std::unique_ptr<Entry>> cache_;
  // vfs::Vfs::generation() the cache was built for.
  u32 generation_ = 0;
  // The model shown and the turn: model ticks are taken from the time drawn, `accum` is the part of the next one.
  std::string shown_;
  int shown_weapon_ = -1;
  float accum_ = 0, yaw_ = 0;
  std::vector<render::Mat4> palette_;
};

}  // namespace oc::ui
