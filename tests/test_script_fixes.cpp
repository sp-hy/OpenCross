// Checks the table of per-room field script corrections: how a table is applied to a hand-built script (overwrites, appends,
// all-or-nothing on a mismatch), and, with the game install (skipped without one), that every row matches the shipped data and
// leaves code that decodes with every jump on an instruction.
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "import/container.h"
#include "import/lzss.h"
#include "import/script_fixes.h"
#include "platform/paths.h"
#include "script/field_script.h"
#include "vfs/vfs.h"

using namespace oc;

namespace {
int failures = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      ++failures;                                             \
    }                                                         \
  } while (0)

// One entity, code area = `code`.
Bytes make_script(const Bytes& code) {
  Bytes out(0x84 + 0x40, 0);
  out[0x80] = 1;
  out.insert(out.end(), code.begin(), code.end());
  return out;
}

void test_apply() {
  const Bytes code = {0xFF, 0x25, 0x0A, 0x05, 0xCD, 0xFD, 0xC0, 0x00, 0x80, 0x00};
  const import::ScriptFix rows[] = {
      {"room_a", 4, "CD FD", "AF FD"},
      {"room_b", 1, "25", "01"},
      {"room_a", 10, "", "06 0D"},
  };
  Bytes script = make_script(code);
  std::string error;
  CHECK(import::apply_script_fixes("room_a", script, rows, &error) == 2);
  CHECK(error.empty());
  CHECK(script.size() == 0xC4 + code.size() + 2);
  CHECK(script[0xC4 + 3] == 0x05 && script[0xC4 + 4] == 0xAF && script[0xC4 + 5] == 0xFD);
  CHECK(script[0xC4 + 10] == 0x06 && script[0xC4 + 11] == 0x0D);

  Bytes other = make_script(code);
  CHECK(import::apply_script_fixes("room_c", other, rows, &error) == 0);
  CHECK(other == make_script(code));

  const import::ScriptFix mismatch[] = {
      {"room_a", 4, "CD FD", "AF FD"},
      {"room_a", 1, "26", "01"},
  };
  Bytes untouched = make_script(code);
  CHECK(import::apply_script_fixes("room_a", untouched, mismatch, &error) == -1);
  CHECK(!error.empty());
  CHECK(untouched == make_script(code));

  const import::ScriptFix bad_append[] = {{"room_a", 4, "", "00"}};
  CHECK(import::apply_script_fixes("room_a", untouched, bad_append, &error) == -1);
  const import::ScriptFix bad_length[] = {{"room_a", 4, "CD FD", "AF"}};
  CHECK(import::apply_script_fixes("room_a", untouched, bad_length, &error) == -1);
  const import::ScriptFix bad_hex[] = {{"room_a", 4, "CD FG", "AF FD"}};
  CHECK(import::apply_script_fixes("room_a", untouched, bad_hex, &error) == -1);
  const import::ScriptFix past_end[] = {{"room_a", 9, "00 00", "01 01"}};
  CHECK(import::apply_script_fixes("room_a", untouched, past_end, &error) == -1);
  CHECK(untouched == make_script(code));
}

// Every instruction start reached from `starts` through straight-line decoding and jump / call targets.
void walk(const script::FieldScriptModule& module, std::vector<u32> starts, std::set<u32>& seen) {
  while (!starts.empty()) {
    u32 off = starts.back();
    starts.pop_back();
    while (off < module.code.size() && seen.insert(off).second) {
      std::string error;
      const auto insn = script::decode_insn(module.code, off, &error);
      CHECK(insn.has_value());
      if (!insn) return;
      for (const auto& [target, is_call] : insn->targets) starts.push_back(target);
      if (insn->flow != script::Flow::Next) break;
      off += insn->length;
    }
  }
}

void test_shipped_rooms() {
  std::string configured = std::getenv("OPENCROSS_GAMEDIR") ? std::getenv("OPENCROSS_GAMEDIR") : "";
  const auto dir = platform::find_game_dir(configured);
  vfs::Vfs files;
  if (dir.empty() || !files.open(dir)) {
    std::printf("SKIP shipped rooms: no game install\n");
    return;
  }
  std::set<std::string> rooms;
  for (const auto& row : import::script_fixes()) rooms.insert(std::string(row.room));
  for (const std::string& room : rooms) {
    const auto bin = files.read("cdrom/map/mapbin/" + room + ".bin");
    CHECK(bin.has_value());
    if (!bin) continue;
    const auto cpt = import::parse_cpt(*bin);
    CHECK(cpt && cpt->entries.size() > 3);
    if (!cpt || cpt->entries.size() <= 3) continue;
    auto script = import::lzss_decode(cpt->entries[3].data);
    CHECK(script.has_value());
    if (!script) continue;
    const size_t retail_size = script->size();
    size_t rows = 0;
    for (const auto& row : import::script_fixes()) rows += row.room == room;
    std::string error;
    const int applied = import::apply_script_fixes(room, *script, &error);
    if (applied != static_cast<int>(rows)) std::printf("  %s: %s\n", room.c_str(), error.c_str());
    CHECK(applied == static_cast<int>(rows));
    const auto module = script::FieldScriptModule::parse(*script, &error);
    CHECK(module.has_value());
    if (!module) continue;
    const u32 retail_code = static_cast<u32>(retail_size - module->code_base);
    // Everything reachable from the script pointers decodes, and the appended code is reached.
    std::set<u32> seen;
    std::vector<u32> starts;
    for (size_t entity = 0; entity < module->entity_count(); ++entity)
      for (size_t slot = 0; slot < script::kScriptSlots; ++slot)
        if (const u16 ptr = module->script_ptr(entity, slot)) starts.push_back(ptr);
    walk(*module, starts, seen);
    if (module->code.size() > retail_code) CHECK(seen.contains(retail_code));
  }
  std::printf("shipped rooms: %zu checked\n", rooms.size());
}

}  // namespace

int main() {
  test_apply();
  test_shipped_rooms();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("script fixes: ok\n");
  return 0;
}
