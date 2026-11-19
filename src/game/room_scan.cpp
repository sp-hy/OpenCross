#include "game/room_scan.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <iterator>
#include <map>

#include "battle/opponents.h"
#include "core/parallel.h"
#include "data/tables.h"
#include "game/disc_index.h"
#include "import/container.h"
#include "import/lzss.h"
#include "import/script_fixes.h"
#include "import/text.h"
#include "vfs/vfs.h"

namespace oc::game {

std::vector<std::string> room_stems(const vfs::Vfs& files) {
  std::vector<std::string> bins, ctds;
  for (std::string member : files.list("cdrom")) {
    std::replace(member.begin(), member.end(), '\\', '/');
    std::transform(member.begin(), member.end(), member.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const size_t at = member.find("map/mapbin/");
    if (at == std::string::npos || member.size() < at + 16) continue;
    const std::string stem = member.substr(at + 11, member.size() - at - 15);
    if (stem.find('/') != std::string::npos) continue;
    if (member.ends_with(".bin")) bins.push_back(stem);
    if (member.ends_with(".ctd")) ctds.push_back(stem);
  }
  std::sort(bins.begin(), bins.end());
  std::sort(ctds.begin(), ctds.end());
  std::vector<std::string> out;
  std::set_intersection(bins.begin(), bins.end(), ctds.begin(), ctds.end(), std::back_inserter(out));
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::optional<script::FieldScriptModule> load_room_script(const vfs::Vfs& files, std::string_view stem, std::string* error) {
  auto fail = [&](std::string message) -> std::optional<script::FieldScriptModule> {
    if (error) *error = std::move(message);
    return std::nullopt;
  };
  const auto bin = files.read("cdrom/map/mapbin/" + std::string(stem) + ".bin");
  if (!bin) return fail("no .bin");
  const auto cpt = import::parse_cpt(*bin);
  if (!cpt || cpt->entries.size() < 4) return fail("bad cpt");
  auto script = import::lzss_decode(cpt->entries[3].data);
  if (!script) return fail("script subfile: lzss failed");
  import::apply_script_fixes(stem, *script);
  return script::FieldScriptModule::parse(*script, error);
}

ScriptScan scan_rooms(const vfs::Vfs& files) {
  using Clock = std::chrono::steady_clock;
  auto since = [](Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); };
  const auto start = Clock::now();
  ScriptScan out;
  const std::vector<std::string> stems = room_stems(files);
  out.rooms.resize(stems.size());
  std::vector<double> read_times(stems.size(), 0.0);
  parallel_for(stems.size(), [&](size_t i) {
    RoomScan& room = out.rooms[i];
    room.stem = stems[i];
    room.room_id = import::room_id_from_name(room.stem);
    const auto read_start = Clock::now();
    const auto module = load_room_script(files, room.stem, &room.error);
    read_times[i] = since(read_start);
    if (module) {
      room.error.clear();
      room.scan = script::scan_script(*module);
    }
    room.milliseconds = since(read_start);
  });
  for (double time : read_times) out.read_milliseconds += time;
  out.milliseconds = since(start);
  return out;
}

bool is_treasure(const script::ScanEvent& event) {
  if (event.kind != script::EventKind::Give || !event.flag.valid() || !event.flag_at_load) return false;
  return std::any_of(event.values.begin(), event.values.end(),
                     [](int value) { return script::give_class(value) != script::GiveClass::None; });
}

std::string event_id(std::string_view stem, const script::ScanEvent& event) {
  return std::format("{}:{}:{:04X}", stem, event.entity, event.offset);
}

u32 flag_key(const script::ScanFlag& flag) {
  if (flag.bit >= 0) return static_cast<u32>(flag.var * 8 + flag.bit);
  return 0x10000u + static_cast<u32>(flag.var) * 0x10000u + static_cast<u32>(flag.value & 0xFFFF);
}

std::vector<Treasure> treasures(const ScriptScan& scan) {
  std::map<u32, Treasure> by_key;
  for (size_t room = 0; room < scan.rooms.size(); ++room) {
    const auto& events = scan.rooms[room].scan.events;
    for (size_t index = 0; index < events.size(); ++index) {
      const script::ScanEvent& event = events[index];
      if (!is_treasure(event)) continue;
      Treasure& treasure = by_key[flag_key(event.flag)];
      treasure.key = flag_key(event.flag);
      treasure.flag = event.flag;
      treasure.chest = treasure.chest || event.capacity_check;
      for (int value : event.values)
        if (std::find(treasure.gives.begin(), treasure.gives.end(), value) == treasure.gives.end()) treasure.gives.push_back(value);
      treasure.sites.push_back({room, index});
    }
  }
  std::vector<Treasure> out;
  for (auto& [key, treasure] : by_key) out.push_back(std::move(treasure));
  return out;
}

std::vector<int> battle_scenes(const data::GameTables& tables, int battle_id) {
  std::vector<int> out;
  const int variants = battle_id >= 0x200 ? 1 : 8;
  for (int variant = 0; variant < variants; ++variant) {
    const int scene = tables.scene_for(battle_id, variant);
    if (scene < 0 || static_cast<size_t>(scene) >= tables.scenes.size()) continue;
    if (std::find(out.begin(), out.end(), scene) == out.end()) out.push_back(scene);
  }
  return out;
}

std::vector<data::BattleSite> battle_sites(const ScriptScan& scan, const data::GameTables& tables) {
  std::vector<data::BattleSite> out;
  for (const RoomScan& room : scan.rooms)
    for (const script::ScanEvent& event : room.scan.events) {
      if (event.kind != script::EventKind::Battle) continue;
      data::BattleSite site;
      site.stem = room.stem;
      site.story_lo = script::story_range(event.conds).lo;
      for (int battle_id : event.values)
        for (int scene : battle_scenes(tables, battle_id)) {
          const data::SceneRec& rec = tables.scenes[static_cast<size_t>(scene)];
          for (int group = 0; group < 3; ++group) {
            const int enemy = rec.enemy_id(group);
            if (rec.enemy_count(group) && enemy != 0xFF && std::find(site.enemies.begin(), site.enemies.end(), enemy) == site.enemies.end())
              site.enemies.push_back(enemy);
          }
        }
      out.push_back(std::move(site));
    }
  return out;
}

data::BestiaryOrder bestiary_order(const ScriptScan& scan, const data::GameTables& tables, const vfs::Vfs& files) {
  std::vector<std::string> names;
  std::vector<bool> placed(tables.enemies.size(), false);
  for (size_t record = 0; record < tables.enemies.size(); ++record) names.push_back(tables.enemy_name(static_cast<int>(record)));
  std::vector<std::vector<int>> scenes;
  for (const data::SceneRec& scene : tables.scenes) {
    std::vector<int>& fields = scenes.emplace_back();
    for (int group = 0; group < 3; ++group)
      if (scene.enemy_count(group) > 0 && scene.enemy_id(group) < placed.size()) {
        placed[scene.enemy_id(group)] = true;
        fields.push_back(scene.enemy_id(group));
      }
  }
  DiscIndex discs;
  discs.load(files);
  const std::vector<battle::SceneOpponents> fights =
      battle::scene_opponents(tables, [&](int scene) { return files.read(discs.battle_script(1, scene)); });
  std::vector<bool> fought(placed.size(), false), kept(placed.size(), false), decided(placed.size(), false);
  for (const battle::SceneOpponents& fight : fights) {
    for (int record : fight.fought)
      if (record >= 0 && static_cast<size_t>(record) < fought.size()) {
        fought[static_cast<size_t>(record)] = true;
        if (!fight.counted_end) decided[static_cast<size_t>(record)] = true;
      }
    for (int record : fight.kept)
      if (record >= 0 && static_cast<size_t>(record) < kept.size()) kept[static_cast<size_t>(record)] = true;
  }
  std::vector<bool> left_out(placed.size(), false);
  for (size_t record = 0; record < placed.size(); ++record)
    if (kept[record] && !fought[record]) left_out[record] = true, placed[record] = false;
  data::BestiaryOrder order = data::bestiary_order(battle_sites(scan, tables), names, placed, scenes);
  order.left_out = std::move(left_out);
  order.counted_end.assign(placed.size(), false);
  for (size_t record = 0; record < placed.size(); ++record) order.counted_end[record] = fought[record] && !decided[record];
  order.worlds = data::area_worlds(room_stems(scan));
  return order;
}

std::vector<std::string> room_stems(const ScriptScan& scan) {
  std::vector<std::string> out;
  for (const RoomScan& room : scan.rooms) out.push_back(room.stem);
  return out;
}

}  // namespace oc::game
