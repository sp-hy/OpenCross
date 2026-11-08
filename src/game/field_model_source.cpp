#include "game/field_model_source.h"

#include <mutex>
#include <vector>

#include "game/battle/battle_seq.h"
#include "game/field_world_internal.h"
#include "game/model_files.h"
#include "game/room_models.h"
#include "import/room.h"
#include "vfs/vfs.h"

namespace oc::game {

namespace {

bool is_move(bseq::Event::Kind kind) {
  using K = bseq::Event::Kind;
  return kind == K::MoveSpeed || kind == K::MoveTime || kind == K::MoveHome || kind == K::MovePos || kind == K::Jump;
}

}  // namespace

BattleFieldClips battle_field_clips(ByteSpan obj) {
  BattleFieldClips out;
  bseq::Table table;
  if (!bseq::load_model_table(obj, table)) return out;
  // The VMs share a few battle-wide words: one reader at a time, the words are put back after, and no battle runs meanwhile
  // (FieldWorld::take_battle waits for the readers).
  static std::mutex vm_lock;
  const std::lock_guard<std::mutex> guard(vm_lock);
  const bseq::Vm::Shared shared = bseq::Vm::shared();
  // Runs entry `entry` (variant 0) for up to 4 s of ticks; `on_event` returns false to stop.
  const auto run = [&](int entry, const auto& on_event) {
    bseq::Vm vm;
    bseq::Ctx ctx;
    ctx.rand = [] { return 0; };
    if (!vm.start(&table, entry, 0)) return;
    std::vector<bseq::Event> events;
    for (int tick = 0; tick < 240 && vm.running(); ++tick) {
      events.clear();
      vm.tick(ctx, events);
      for (const bseq::Event& event : events)
        if (!on_event(event)) return;
    }
  };
  run(1, [&](const bseq::Event& event) {
    if (event.kind != bseq::Event::SetAnim || event.a >= 0x80) return true;
    out.stand = event.a;
    return false;
  });
  int current = -1;
  run(2, [&](const bseq::Event& event) {
    if (event.kind == bseq::Event::SetAnim && event.a < 0x80) {
      current = out.stance = event.a;
    } else if (is_move(event.kind) && out.run < 0 && current >= 0) {
      out.run = current;
    }
    return true;
  });
  bseq::Vm::restore(shared);
  return out;
}

void adapt_battle_model(assets::ModelAsset& asset, const BattleFieldClips& found) {
  std::vector<assets::AnimationClip> own, at0;
  for (assets::AnimationClip& clip : asset.clips) {
    if (clip.source == "s4") own.push_back(std::move(clip));
    else if (clip.source == "at0") at0.push_back(std::move(clip));
  }
  const auto pick = [&](int index, int fallback) -> const assets::AnimationClip* {
    for (const int want : {index, fallback})
      for (const assets::AnimationClip& clip : own)
        if (want >= 0 && clip.index == static_cast<u32>(want)) return &clip;
    return own.empty() ? nullptr : &own.front();
  };
  const assets::AnimationClip* stand = pick(found.stand, 1);
  const assets::AnimationClip* run = pick(found.run, 3);
  const assets::AnimationClip* stance = pick(found.stance, 2);
  std::vector<assets::AnimationClip> out;
  const auto add = [&](const assets::AnimationClip* clip, u32 index, const char* source) {
    if (!clip) return;
    assets::AnimationClip copy = *clip;
    copy.source = source;
    copy.index = index;
    out.push_back(std::move(copy));
  };
  for (const u32 slot : {0u, 1u, 4u, 5u, 6u, 7u}) add(stand, slot, "s4");
  add(run, 2, "s4");
  add(run, 3, "s4");
  add(stance, 8, "s4");
  for (const assets::AnimationClip& clip : own) add(&clip, clip.index, "ex");
  for (const assets::AnimationClip& clip : at0)
    if (clip.index < 0x60) add(&clip, 0x20 + clip.index, "ex");
  asset.clips = std::move(out);
  asset.anim_seq.clear();
  asset.ex_anim_seq.clear();
}

std::string character_battle_stem(const vfs::Vfs& files, int char_id) {
  if (char_id < 0 || char_id >= 44) return {};
  const std::string folder = field_internal::kCharFolders[char_id];
  // Folder variants: selju -> selju2/selju, gren -> gren1/gren1 (battle/player has "2" / "1" suffixed folders).
  for (const std::string& stem : {"battle/player/" + folder + "/" + folder, "battle/player/" + folder + "1/" + folder + "1",
                                  "battle/player/" + folder + "2/" + folder})
    if (files.exists("cdrom/" + stem + ".obj")) return stem;
  return {};
}

std::optional<assets::ModelAsset> read_battle_model_for_field(const vfs::Vfs& files, const std::string& stem, bool hd_battle_texture,
                                                              std::string* err) {
  const std::optional<Bytes> obj = files.read("cdrom/" + stem + ".obj");
  if (!obj) {
    if (err) *err = "cdrom/" + stem + ".obj not found";
    return std::nullopt;
  }
  std::optional<assets::ModelAsset> asset = read_model_files(files, stem, err, hd_battle_texture);
  if (!asset) return std::nullopt;
  adapt_battle_model(*asset, battle_field_clips(ByteSpan(*obj)));
  return asset;
}

namespace {

std::optional<assets::RoomModelHeader> room_model_record(const vfs::Vfs& files, const std::string& room, int k, std::string* err) {
  const std::string base = "cdrom/map/mapbin/" + room;
  const std::optional<Bytes> bin = files.read(base + ".bin"), ctd = files.read(base + ".ctd");
  if (!bin || !ctd) {
    if (err) *err = base + ".bin / .ctd not found";
    return std::nullopt;
  }
  std::optional<std::vector<assets::RoomModelHeader>> models = import::load_room_models(*bin, *ctd, err);
  if (!models) return std::nullopt;
  if (k < 0 || k >= static_cast<int>(models->size())) {
    if (err) *err = room + " has " + std::to_string(models->size()) + " models";
    return std::nullopt;
  }
  return std::move((*models)[static_cast<size_t>(k)]);
}

}  // namespace

std::optional<assets::ModelAsset> read_room_model(const vfs::Vfs& files, const std::string& room, int k, std::string* err) {
  std::optional<assets::RoomModelHeader> record = room_model_record(files, room, k, err);
  return record ? build_room_model(*record, err) : std::nullopt;
}

std::optional<Bytes> read_room_model_mdl(const vfs::Vfs& files, const std::string& room, int k) {
  std::optional<assets::RoomModelHeader> record = room_model_record(files, room, k, nullptr);
  if (!record) return std::nullopt;
  return Bytes(record->mdl.begin(), record->mdl.end());
}

}  // namespace oc::game
