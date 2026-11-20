#pragma once
// The Elements page's spell (ui::SpellView): a BattleView of its own in preview mode. When the cursor moves to an element, the running
// battle casts it at once if its caster is the same and no cast is playing; otherwise a battle is loaded in slices over the following
// frames (scene kScene, the caster alone in the party). It then plays the cast again and again; the picture is fitted to the box's
// height and centred across it, cut to the box, over black. The view is made on the first draw and keeps its last battle until another
// element is shown.
#include <memory>
#include <vector>

#include "battle/battle.h"
#include "game/battle/battle_view.h"
#include "ui/menu/menu_model.h"

namespace oc::game {

class SpellPreview : public ui::SpellView {
 public:
  // The references must outlive the preview.
  void configure(render::GpuContext& ctx, const vfs::Vfs& vfs, render::SpriteBatch& sprites, render::MeshPipeline& meshes,
                 const data::GameTables& tables);
  void set_art_mode(bool hd, bool force);
  void set_language(import::Lang lang);
  void draw(const render::Frame& frame, float vw, float vh, const ui::SpellViewSlot& slot) override;

 private:
  static constexpr int kScene = 0;
  static constexpr double kLoadBudgetMs = 12.0;
  void begin(int element, int caster);

  render::GpuContext* ctx_ = nullptr;
  const vfs::Vfs* vfs_ = nullptr;
  render::SpriteBatch* sprites_ = nullptr;
  render::MeshPipeline* meshes_ = nullptr;
  const data::GameTables* tables_ = nullptr;
  bool hd_ = true;
  import::Lang lang_ = import::Lang::En;
  std::unique_ptr<BattleView> view_;
  bool failed_ = false;
  // wanted_: the slot's element; shown_ / caster_: the element and caster of the view's battle; loading_: its load is running.
  int wanted_ = 0, shown_ = 0, caster_ = -1;
  bool loading_ = false, running_ = false;
  std::vector<battle::PartyMember> party_;
  BattleView::StartOptions options_;
};

}  // namespace oc::game
