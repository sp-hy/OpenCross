#pragma once
// Debug tab tools (Settings window, F8): start a battle on a chosen battlefield / formation, swap the enemies and the party, cast an element
// in a running battle, teleport to a room, start an entity script, set story variables.
//
// The UI only queues requests (draw() runs inside the ImGui frame); apply() runs them once per frame in the main loop, before the game
// advances, through the same paths the field scripts and the routes use: FieldWorld::request_battle (the 'enct' effect, then the battle
// hand-over), FieldWorld::load_room, FieldVM::start_script / set_var, the shared GameState's party, and BattleView's cast hook.
//
// Invincible party / enemies (Debug tab checkboxes) are pushed to the running battle through BattleView::Debug -> Battle::set_invincible.
//
// Every action locks saving for the rest of the session (the field's no-save gate, the menu's Save screen): the first one asks the player
// first ("This disables saving until you restart. Continue?").
#include <array>
#include <optional>
#include <string>
#include <vector>

#include "core/types.h"
#include "platform/window.h"

namespace oc::data {
struct GameTables;
struct ElemRec;
}
namespace oc::game {

class FieldWorld;
class BattleView;

class DebugTools {
 public:
  // What the host knows each frame.
  struct Ctx {
    // null: no field.
    FieldWorld* world = nullptr;
    // null: no battle view.
    BattleView* battle = nullptr;
    // null: no game data.
    data::GameTables* tables = nullptr;
    // A field room is up and nothing owns the screen (no battle, menu, shop, movie, title).
    bool field_free = false;
    // A battle is being fought (field encounter or the standalone view).
    bool battle_running = false;
  };

  // The tab body (inside the settings window). Queues requests only.
  void draw(const Ctx& ctx);
  // Once per frame before the game advances: runs the queued requests, restores a temporary scene record.
  void apply(const Ctx& ctx);

  // Once per frame after the window pumped `in`: drives the free camera (game/free_cam.h). Keys W A S D Q E, the sticks and the triggers that the
  // camera takes are removed from `in` while it is on and `applies` (a battle 3D picture or the title's sea is up); the mouse look needs the right
  // button held (the pointer is trapped, relative mode) and the press did not start on a UI window. Shift / left stick click = sprint x4.
  void poll_free_camera(platform::Input& in, bool applies, SDL_Window* window);

  bool saving_disabled() const { return saving_disabled_; }
  // What the last request did / why it was refused (shown in the tab).
  const std::string& status() const { return status_; }

  // ---- non-UI helpers (unit-testable) -------------------------------------------------------------------------------------
  // "NNN  Enemy xN, Enemy" for scene `i` (the formation as listed in scenedat.bin).
  static std::string scene_label(const data::GameTables& tables, size_t scene_index);
  // Replaces the enemy groups of `base` (u8 id, u8 count per group, id 0xFF / count 0 = empty); returns the edited record bytes.
  static std::array<u8, 28> with_enemies(const std::array<u8, 28>& base, const std::array<u8, 3>& ids, const std::array<u8, 3>& counts);
  // What an element record is, from the name's prefix glyph and the record: the picker shows it instead of the raw prefix (which the UI font lacks).
  enum class ElemKind { Element, Item, Tech, EnemySkill, Summon, EnemySummon, Unnamed };
  static ElemKind element_kind(const std::string& raw_name, const data::ElemRec& rec);
  static const char* element_kind_name(ElemKind kind);
  // `raw_name` without the prefix glyph, limited to what the UI font draws (Latin-1); other characters are dropped.
  static std::string plain_name(const std::string& raw_name);
  // The Invincible checkboxes' applied state, set from outside (route `set invincible`); apply() then keeps the battle view in step.
  void set_invincible(bool party, bool enemies) { inv_party_ = party, inv_enemies_ = enemies; }

 private:
  struct Action {
    enum class Kind { Room, Battle, Party, Cast, Script, SetVar, Invincible, FreeCam } kind = Kind::Room;
    std::string room;
    // Room: arg0 = entrance. Battle: arg0 = scene, arg1 = battlefield (-1 default). Cast: arg0 = caster, arg1 = element, arg2 = target.
    // Script: arg0 = entity, arg1 = slot, arg2 = class. SetVar: arg0 = id, arg1 = value.
    // Invincible: arg0 = party, arg1 = enemies (0 / 1). FreeCam: arg0 = on (the first switch-on locks saving).
    int arg0 = 0, arg1 = 0, arg2 = 0, arg3 = 0;
    std::array<int, 3> party{-1, -1, -1};
    // Battle: use `ids` / `counts` instead of the scene's own groups.
    bool custom_enemies = false;
    std::array<u8, 3> ids{0xFF, 0xFF, 0xFF}, counts{0, 0, 0};
    // Battle: apply `party` before the encounter.
    bool set_party_first = false;
  };
  void queue(Action action);
  bool element_combo();

  bool saving_disabled_ = false;
  // The mouse is trapped (SDL relative mode) for a free camera look.
  bool fc_looking_ = false;
  std::string status_;
  std::vector<Action> queue_;
  // Temporary scene record (custom enemies): restored once the battle has started.
  int restore_scene_ = -1;
  std::array<u8, 28> restore_raw_{};

  // picker state
  struct Pick {
    int sel = 0;
    char filter[64] = {};
  };
  Pick room_pick_, scene_pick_, el_pick_, caster_pick_, target_pick_, ent_pick_, field_pick_;
  Pick char_pick_[3], enemy_pick_[3];
  int entrance_ = 0, field_ = -1, enemy_count_[3] = {1, 0, 0};
  bool use_party_ = false, custom_enemies_ = false;
  int slot_ = 2;
  // The applied state of the two checkboxes.
  bool inv_party_ = false, inv_enemies_ = false;
  // Element picker: -1 all, else an ElemKind.
  int kind_filter_ = -1;
  int var_id_ = 0, var_val_ = 0;
  // The scene the enemy pickers were last filled from.
  int custom_scene_ = -1;
  // The party pickers were filled from the field party.
  bool party_init_ = false;

  // label caches (built once per tables / room list)
  const void* cache_key_ = nullptr;
  struct ElemInfo {
    // "id  Name  [colour]  Kind".
    std::string label;
    // Data colour index (green white black red yellow blue), -1 none.
    int colour = -1;
    ElemKind kind = ElemKind::Element;
  };
  std::vector<ElemInfo> elem_info_;
  std::vector<std::string> room_labels_, scene_labels_, char_labels_, enemy_labels_;
  // "bg_NN  K scenes  alu x4, ter x2" (data/battlefields.h); entry 0 = the scene's own stage.
  std::vector<std::string> field_labels_;
  // The bg per label, -1 first.
  std::vector<int> field_values_;
  std::vector<std::string> room_stems_;
  void build_caches(const Ctx& ctx);
};

}  // namespace oc::game
