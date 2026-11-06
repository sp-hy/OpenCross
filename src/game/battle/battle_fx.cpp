#include "game/battle/battle_fx.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <set>

#include "core/log.h"
#include "game/battle/model_table.h"
#include "import/battle_stage.h"
#include "import/container.h"
#include "import/lzss.h"
#include "import/room_effects.h"
#include "import/tim.h"

namespace oc::game {

using effects::Basis;
using effects::Vec3;

// ------------------------------------------------------------------------------------------------------------ VRAM sections
namespace {

// The stock host overlays have no private ops: a pack's op >= 0x100 does nothing under them.
bool stock_host(std::string_view overlay) { return overlay == "stdtech0" || overlay == "stdtech1" || overlay == "stdmag0" || overlay == "magtest"; }

// Does the pack-table script (entry, variant) reach an op of this kind, for a landed or a failed hit? React = 0x59 / 0x5A (CallDamTarget /
// CallDamTargetAll: the targets run variant 7), Damage = 0x41 (CallDamage, the number).
bool script_emits(const bseq::Table& table, int entry, int variant, bseq::Event::Kind kind) {
  for (const int hit : {0, 2}) {
    bseq::Vm vm;
    if (!vm.start(&table, entry, variant)) return false;
    bseq::Ctx ctx;
    ctx.clip_frames = [](int) { return 8; };
    ctx.hit_code = hit;
    std::vector<bseq::Event> ev;
    for (int tick = 0; tick < 2000 && vm.running(); ++tick) {
      ev.clear();
      vm.tick(ctx, ev);
      for (const bseq::Event& event : ev)
        if (event.kind == kind) return true;
    }
  }
  return false;
}

// Does the script END (no wait on another script's flag left running) without ever reaching an op of this kind, for a landed and a failed hit?
bool script_ends_without(const bseq::Table& table, int entry, int variant, bseq::Event::Kind kind) {
  for (const int hit : {0, 2}) {
    bseq::Vm vm;
    if (!vm.start(&table, entry, variant)) return false;
    bseq::Ctx ctx;
    ctx.clip_frames = [](int) { return 8; };
    ctx.hit_code = hit;
    std::vector<bseq::Event> ev;
    for (int tick = 0; tick < 2000 && vm.running(); ++tick) {
      ev.clear();
      vm.tick(ctx, ev);
      for (const bseq::Event& event : ev)
        if (event.kind == kind) return false;
    }
    if (vm.running()) return false;
  }
  return true;
}

// One egfx (.ctd) section: a VRAM rectangle. type 0x1200 = pixels, 0x1201 = CLUT rows. x, w in halfwords; y, h in rows.
struct VramSection {
  u32 type = 0x1200;
  int x = 0, y = 0, w = 0, h = 0;
  std::vector<u16> data;
};

// Lenient reader: a malformed section ends the list instead of dropping the file.
std::vector<VramSection> read_ctd(ByteSpan data) {
  std::vector<VramSection> out;
  auto rd16 = [&](size_t offset) -> u32 { return offset + 2 <= data.size() ? static_cast<u32>(data[offset] | (data[offset + 1] << 8)) : 0u; };
  size_t offset = 0;
  while (offset + 2048 <= data.size()) {
    VramSection section;
    section.type = rd16(offset) | (rd16(offset + 2) << 16);
    section.x = static_cast<int>(rd16(offset + 4) + rd16(offset + 8));
    section.y = static_cast<int>(rd16(offset + 6) + rd16(offset + 10));
    section.w = static_cast<int>(rd16(offset + 12));
    const u32 nsec = rd16(offset + 24) | (rd16(offset + 26) << 16);
    if ((section.type & 0xFFFEu) != 0x1200 || section.w == 0 || nsec > 1010 || offset + 2048ull * (1ull + nsec) > data.size()) break;
    bool ok = true;
    for (u32 i = 0; i < nsec && ok; ++i) {
      const u32 rows = rd16(offset + 28 + 2ull * i);
      if (static_cast<size_t>(rows) * static_cast<size_t>(section.w) * 2 > 2048) ok = false;
      const size_t sd = offset + 2048ull * (1 + i);
      for (size_t k = 0; ok && k < static_cast<size_t>(rows) * static_cast<size_t>(section.w); ++k) section.data.push_back(static_cast<u16>(rd16(sd + 2 * k)));
      section.h += static_cast<int>(rows);
    }
    if (!ok) break;
    out.push_back(std::move(section));
    offset += 2048ull * (1ull + nsec);
  }
  return out;
}

// Writes sections back in the egfx layout (header sector: u32 type, u16 x, y, dx 0, dy 0, width, rows per sector, 1, sector count; u32 sections
// in the file @20, u32 sectors @24, u16 rows[] @28; then the rows, whole rows per 2048-byte sector).
Bytes write_ctd(const std::vector<const VramSection*>& secs) {
  Bytes out;
  for (const VramSection* section : secs) {
    const int rps = std::max(1, 2048 / (2 * section->w));
    const int nsec = (section->h + rps - 1) / rps;
    const size_t base = out.size();
    out.resize(base + 2048ull * static_cast<size_t>(1 + nsec), 0);
    auto w16 = [&](size_t offset, u32 value) {
      out[offset] = static_cast<u8>(value);
      out[offset + 1] = static_cast<u8>(value >> 8);
    };
    w16(base, section->type & 0xFFFF);
    w16(base + 2, section->type >> 16);
    w16(base + 4, static_cast<u32>(section->x));
    w16(base + 6, static_cast<u32>(section->y));
    w16(base + 12, static_cast<u32>(section->w));
    w16(base + 14, static_cast<u32>(rps));
    w16(base + 16, 1);
    w16(base + 18, static_cast<u32>(nsec));
    w16(base + 20, static_cast<u32>(secs.size()));
    w16(base + 24, static_cast<u32>(nsec));
    for (int i = 0; i < nsec; ++i) {
      const int rows = std::min(rps, section->h - i * rps);
      w16(base + 28 + 2ull * static_cast<size_t>(i), static_cast<u32>(rows));
      const size_t sd = base + 2048ull * static_cast<size_t>(1 + i);
      const size_t first = static_cast<size_t>(i) * static_cast<size_t>(rps) * static_cast<size_t>(section->w);
      for (size_t k = 0; k < static_cast<size_t>(rows) * static_cast<size_t>(section->w); ++k) w16(sd + 2 * k, section->data[first + k]);
    }
  }
  return out;
}

std::optional<import::Tim> tim_of(ByteSpan raw) {
  Bytes dec;
  if (import::is_lzss(raw)) {
    auto decoded = import::lzss_decode(raw);
    if (!decoded) return std::nullopt;
    dec = std::move(*decoded);
    raw = dec;
  }
  return import::parse_tim(raw);
}

// Every entry of a drp in registration order (a nested drp's entries are linked in place).
void flatten_drp(ByteSpan data, std::vector<import::DrpEntry>& out, int depth = 0) {
  auto drp = import::parse_drp(data);
  if (!drp) return;
  for (const auto& entry : drp->entries) {
    out.push_back(entry);
    if (entry.type == 1 && depth < 4 && import::is_drp(entry.data)) flatten_drp(entry.data, out, depth + 1);
  }
}

// A drp with `outer`'s entries followed by `inner`'s (nested type-1 drps flattened), in the layout parse_drp reads: "drp" + NUL, u32 0, u32 64 * count,
// u32 offset[count], then per entry u32 0, char name[4], u8 type, u24 16 * length, payload (padded to 4).
Bytes merge_drp(ByteSpan outer, ByteSpan inner) {
  std::vector<import::DrpEntry> entries;
  if (auto outer_drp = import::parse_drp(outer)) entries = outer_drp->entries;
  flatten_drp(inner, entries);
  Bytes out(12 + 4 * entries.size(), 0);
  out[0] = 'd', out[1] = 'r', out[2] = 'p';
  auto put32 = [&](size_t offset, u32 value) {
    for (int k = 0; k < 4; ++k) out[offset + static_cast<size_t>(k)] = static_cast<u8>(value >> (8 * k));
  };
  put32(8, static_cast<u32>(entries.size() * 64));
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];
    put32(12 + 4 * i, static_cast<u32>(out.size()));
    const size_t header = out.size();
    out.resize(header + 12, 0);
    for (size_t k = 0; k < 4 && k < entry.name.size(); ++k) out[header + 4 + k] = static_cast<u8>(entry.name[k]);
    const u32 len = static_cast<u32>(entry.data.size()) * 16u;
    out[header + 8] = entry.type;
    out[header + 9] = static_cast<u8>(len), out[header + 10] = static_cast<u8>(len >> 8), out[header + 11] = static_cast<u8>(len >> 16);
    out.insert(out.end(), entry.data.begin(), entry.data.end());
    while (out.size() % 4) out.push_back(0);
  }
  return out;
}

// Summon overlays that unpack a compressed resource block of their pack at init: find the type-0x25 entry `tag`, LZSS-decode it, then
// register the nested drp like a pack (TIMs to VRAM, meshes, emitters, sequences). The meshes
// the pack's emitters name (pou0..3, hir0, kab0 ... cro0, niw0 ...) live only there. enrou / tfleaf use 'thre'; seint / tgfrog / tunico use 'comp'.
const char* unpacked_block(std::string_view overlay) {
  if (overlay == "enrou" || overlay == "tfleaf") return "thre";
  if (overlay == "seint" || overlay == "tgfrog" || overlay == "tunico") return "comp";
  return nullptr;
}

// The pack as the overlay leaves it registered: `prd` with the decoded type-0x25 block `tag` appended (unchanged without one).
Bytes with_unpacked_block(Bytes prd, const char* tag, int eid) {
  if (!tag) return prd;
  auto drp = import::parse_drp(prd);
  if (!drp) return prd;
  for (const auto& entry : drp->entries) {
    if (entry.type != 0x25 || entry.name != tag) continue;
    std::optional<Bytes> dec = import::is_lzss(entry.data) ? import::lzss_decode(entry.data) : std::optional<Bytes>(Bytes(entry.data.begin(), entry.data.end()));
    if (!dec || !import::is_drp(*dec)) break;
    log::debug("battle fx: eid {}: overlay block '{}' unpacked ({} bytes)", eid, tag, dec->size());
    return merge_drp(prd, *dec);
  }
  return prd;
}

// Registering a nested drp (type 0x01) registers each of its entries as the pack's own (nested drps recurse), so the lookups of a sequence, an
// emitter or a sheet find them like top-level entries. The trap-summon packs keep the jar's sequence `test` and its emitters in `tubo`.
void add_nested_entries(assets::RoomEffects& fx, ByteSpan prd, const import::Vram& vram, int depth) {
  auto drp = import::parse_drp(prd);
  if (!drp || depth >= 4) return;
  for (const auto& entry : drp->entries) {
    if (entry.type != 1 || !import::is_drp(entry.data)) continue;
    assets::RoomEffects inner = import::load_battle_effects(entry.data, {}, vram, {});
    log::debug("battle fx: nested '{}' depth {}: {} emitters, {} sequences", entry.name, depth, inner.emitters.size(), inner.sequences.size());
    std::move(inner.sheets.begin(), inner.sheets.end(), std::back_inserter(fx.sheets));
    std::move(inner.emitters.begin(), inner.emitters.end(), std::back_inserter(fx.emitters));
    std::move(inner.sequences.begin(), inner.sequences.end(), std::back_inserter(fx.sequences));
    std::move(inner.blobs.begin(), inner.blobs.end(), std::back_inserter(fx.blobs));
    std::move(inner.errors.begin(), inner.errors.end(), std::back_inserter(fx.errors));
    fx.tims += inner.tims;
    fx.meshes += inner.meshes;
    add_nested_entries(fx, entry.data, vram, depth + 1);
  }
}

// RDE's 4x sheet pages: when the pack of `eid` loads, RDE uploads `path` (hd.dat) over the sheet's texture rect and its CLUT over the
// sheet's CLUT row. Luminaire's lens-flare rings are the only such sheet; OpenCross draws the page in HD art.
struct HdSheetPage {
  int eid;
  std::string_view sheet, path;
};
constexpr HdSheetPage kHdSheetPages[] = {{223, "renz", "hd/battle/shine/renz0.tim"}};

// RDE's 4x texel pages without a CLUT, uploaded in HD art over (x, y) of the pack's texture space when the pack of `eid` loads: Canon Cannon's
// face expressions, which the cast copies into the caster's texture.
struct HdTexelPage {
  int eid, x, y;
  std::string_view path;
};
constexpr HdTexelPage kHdTexelPages[] = {{462, 0x2C0, 0x100, "hd/battle/canon/face.tim"}};

}  // namespace

import::Vram FxPack::build_vram() const {
  import::Vram vram = resident ? resident->vram : import::Vram{};
  for (const VramSection& sec : read_ctd(ctd)) vram.blit(sec.x, sec.y, sec.w, sec.h, sec.data);
  if (auto drp = import::parse_drp(prd))
    for (const auto& entry : drp->entries)
      if (entry.type == 4)
        if (auto tim = tim_of(entry.data)) vram.blit_tim(*tim);
  return vram;
}

const assets::StageAsset* FxPack::mesh_asset(std::string_view name, size_t& part) const {
  for (const assets::StageAsset* asset : {meshes.get(), resident ? resident->meshes.get() : nullptr}) {
    if (!asset) continue;
    for (size_t i = 0; i < asset->parts.size(); ++i)
      if (asset->parts[i].name == name) {
        part = i;
        return asset;
      }
  }
  return nullptr;
}

// battle/battle.prd: the TIMs of every level go to VRAM in registration order; the nested drps' effect entries join
// the registry. The VRAM is also kept as ctd sections so the mesh importer sees the same texture pages as the packs' own egfx.
void BattleFxLibrary::load_resident() {
  resident_.reset();
  if (battle_prd_.empty()) return;
  auto resident = std::make_shared<FxResident>();
  std::vector<import::DrpEntry> all;
  flatten_drp(battle_prd_, all);
  std::vector<VramSection> secs;
  for (const auto& entry : all) {
    if (entry.type != 4) continue;
    auto tim = tim_of(entry.data);
    if (!tim) continue;
    resident->vram.blit_tim(*tim);
    if (tim->has_clut && tim->clut.w && tim->clut.h) secs.push_back({0x1201, tim->clut.x, tim->clut.y, tim->clut.w, tim->clut.h, tim->clut.data});
    if (tim->image.w && tim->image.h) secs.push_back({0x1200, tim->image.x, tim->image.y, tim->image.w, tim->image.h, tim->image.data});
  }
  std::vector<const VramSection*> sp;
  for (const auto& section : secs) sp.push_back(&section);
  resident->vram_ctd = write_ctd(sp);
  if (auto top = import::parse_drp(battle_prd_))
    for (const auto& entry : top->entries) {
      if (entry.type != 1 || !import::is_drp(entry.data)) continue;
      resident->packs.push_back(std::make_unique<assets::RoomEffects>(import::load_battle_effects(entry.data, {}, resident->vram, {})));
      if (entry.name == "sato") {
        sato_.assign(entry.data.begin(), entry.data.end());
        std::string serr;
        if (auto st = import::load_stage(sato_, resident->vram_ctd, &serr)) {
          resident->meshes = std::make_shared<assets::StageAsset>(std::move(*st));
          resident->meshes->tex_group = "effect_mesh/resident";
        }
        else log::debug("battle fx: resident meshes: {}", serr);
      }
    }
  resident_ = std::move(resident);
}

std::set<u32> actor_script_tags(const vfs::Vfs& vfs) {
  std::set<u32> tags;
  const std::function<void(ByteSpan)> walk = [&](ByteSpan bytes) {
    const auto drp = import::parse_drp(bytes);
    if (!drp) return;
    for (const auto& entry : drp->entries)
      if (entry.type == 0x1A) {
        bseq::Table table;
        if (table.parse(entry.data)) table.sequence_tags(tags);
      } else if (entry.type == 1 && import::is_drp(entry.data)) {
        walk(entry.data);
      }
  };
  if (const auto prd = vfs.read("cdrom/battle/battle.prd")) walk(*prd);
  std::set<std::string> seen;
  for (const std::string_view sv : kBattleModels) {
    const std::string stem(sv);
    if (stem.empty() || !seen.insert(stem).second) continue;
    if (const auto at0 = vfs.read("cdrom/" + stem.substr(0, stem.find_last_of('/')) + "/at0.prd")) walk(*at0);
    const auto obj = vfs.read("cdrom/" + stem + ".obj");
    if (!obj) continue;
    bseq::Table table;
    if (bseq::load_model_table(*obj, table)) table.sequence_tags(tags);
    if (obj->size() < 32) continue;
    const u32 s6 = static_cast<u32>((*obj)[24]) | (*obj)[25] << 8 | (*obj)[26] << 16 | static_cast<u32>((*obj)[27]) << 24;
    if (s6 && s6 + 12 < obj->size()) walk(ByteSpan(obj->data() + s6, obj->size() - s6));
  }
  return tags;
}

// ------------------------------------------------------------------------------------------------------------ library
bool BattleFxLibrary::init(const vfs::Vfs& vfs, bool bind_globals) {
  vfs_ = &vfs;
  cache_.clear();
  slots_.clear();
  const auto fnd1 = vfs.read("cdrom/cdrom.fnd");
  if (!fnd1) return false;
  const auto fnd2 = vfs.read("cdrom/cdrom2.fnd");
  const Bytes b2 = fnd2 ? *fnd2 : Bytes{};
  slots_ = effects::battle_effect_slots(*fnd1, b2, [&](const std::string& path) { return vfs.exists("cdrom/" + path); });
  // Summon motion streams: group-11 local index 0x2D + n (group 11 starts at fnd entry 4323).
  for (size_t i = 0; i < motion_files_.size(); ++i)
    motion_files_[i] = effects::fnd_name(*fnd1, 4323 + 0x2D + i);
  if (auto particle_prd = vfs.read("cdrom/effect/particle.prd")) {
    lib_bytes_ = std::move(*particle_prd);
    import::Vram empty;
    lib_ = import::load_room_effects(lib_bytes_, empty);
  }
  if (bind_globals) effects::BespokeRegistry::instance().bind_slots(slots_);
  // The main exe carries libgte tables some overlays depend on.
  if (bind_globals)
    if (auto exe = vfs.read("cdrom/prog/slps_023.64")) effects::set_ps1_main_exe(std::move(*exe));
  if (auto battle_prd = vfs.read("cdrom/battle/battle.prd")) battle_prd_ = std::move(*battle_prd);
  load_resident();
  charge_scanned_ = false;
  return true;
}

// battle/battle.prd > 'sato' (nested drp): 37 timinfo sheets, 8 meshes (maru, wa0 ...), the 4 cameras ch10..ch13 and the six LZSS type-0x25
// entries ch00..ch05, each a drp of ~15 emitters and the sequence "ch0N". The sheets cut their texels from the TIMs at the top level of battle.prd.
std::shared_ptr<const FxPack> BattleFxLibrary::charge_pack(int colour) {
  if (colour < 0 || colour >= static_cast<int>(charge_.size()) || battle_prd_.empty()) return nullptr;
  if (!charge_scanned_) {
    charge_scanned_ = true;
    if (!resident_ || sato_.empty()) return nullptr;
    const import::Vram& vram = resident_->vram;
    auto sato = import::parse_drp(sato_);
    if (!sato) return nullptr;
    for (const auto& entry : sato->entries) {
      if (entry.type != 0x25 || entry.name.size() != 4 || entry.name.compare(0, 3, "ch0") != 0) continue;
      const int idx = entry.name[3] - '0';
      if (idx < 0 || idx >= static_cast<int>(charge_.size())) continue;
      std::optional<Bytes> dec = import::is_lzss(entry.data) ? import::lzss_decode(entry.data) : std::optional<Bytes>(Bytes(entry.data.begin(), entry.data.end()));
      if (!dec) continue;
      auto pack = std::make_shared<FxPack>();
      // No slot: a negative id keeps the GPU caches apart from the element packs.
      pack->eid = -1 - idx;
      pack->fx = import::load_battle_effects(*dec, {}, vram, {});
      // The sheets and meshes (maru, wa0 ...) are sato's: resident.
      pack->resident = resident_;
      pack->start_seq = entry.name;
      pack->has_main = pack->fx.find_sequence(entry.name) != nullptr;
      pack->overlay = "charge";
      if (pack->has_main) charge_[static_cast<size_t>(idx)] = pack;
    }
  }
  return charge_[static_cast<size_t>(colour)];
}

const effects::EffectSlot* BattleFxLibrary::slot(int eid) const {
  for (const auto& entry : slots_)
    if (entry.eid == eid) return &entry;
  return nullptr;
}

std::shared_ptr<const FxPack> BattleFxLibrary::get(int eid) {
  if (auto it = cache_.find(eid); it != cache_.end()) return it->second;
  std::shared_ptr<FxPack> out;
  const effects::EffectSlot* slot_entry = slot(eid);
  if (slot_entry && slot_entry->prd.size() > 4 && slot_entry->prd.compare(slot_entry->prd.size() - 4, 4, ".prd") == 0) {
    auto prd = vfs_->read("cdrom/" + slot_entry->prd);
    if (prd && prd->size() >= 0x40) prd = with_unpacked_block(std::move(*prd), unpacked_block(effects::overlay_stem(slot_entry->prg)), eid);
    if (prd && prd->size() >= 0x40) {
      Bytes ctd;
      if (!slot_entry->ctd.empty())
        if (auto ctd_file = vfs_->read("cdrom/" + slot_entry->ctd)) ctd = std::move(*ctd_file);
      auto pack = std::make_shared<FxPack>();
      pack->eid = eid;
      pack->slot = *slot_entry;
      pack->resident = resident_;
      // VRAM at the time the pack plays: battle.prd's TIMs (resident since battle init), then the egfx streamed in, then the
      // eins' own TIMs (registration). The particle library of the field is not part of it.
      static const import::Vram kEmpty;
      pack->fx = import::load_battle_effects(*prd, ctd, resident_ ? resident_->vram : kEmpty, {});
      bool own_meshes = false;
      if (auto drp = import::parse_drp(*prd))
        for (const auto& entry : drp->entries) own_meshes = own_meshes || entry.type == 2;
      if (own_meshes) {
        // The mesh importer takes one egfx: the resident VRAM sections followed by the pack's (a malformed section ends the pack's list).
        std::vector<VramSection> base, mine = read_ctd(ctd);
        if (resident_) base = read_ctd(resident_->vram_ctd);
        std::vector<const VramSection*> all;
        for (const auto& sec : base) all.push_back(&sec);
        for (const auto& sec : mine) all.push_back(&sec);
        const Bytes egfx = write_ctd(all);
        std::string serr;
        if (auto st = import::load_stage(*prd, egfx, &serr)) {
          pack->meshes = std::make_shared<assets::StageAsset>(std::move(*st));
          pack->meshes->tex_group = "effect_mesh/eid_" + std::to_string(eid);
        }
        else log::debug("battle fx: eid {} meshes: {}", eid, serr);
      }
      pack->ctd = std::move(ctd);
      pack->prd = std::move(*prd);
      const import::Vram vram = pack->build_vram();
      add_nested_entries(pack->fx, pack->prd, vram, 0);
      for (const HdSheetPage& page : kHdSheetPages) {
        if (page.eid != eid) continue;
        const auto tim_file = vfs_->read(std::string(page.path));
        const auto tim = tim_file ? import::parse_tim(*tim_file) : std::nullopt;
        for (assets::SpriteSheet& sheet : pack->fx.sheets)
          if (tim && sheet.name == page.sheet) pack->hd_sprites += import::cut_hd_sheet(sheet, vram, *tim);
      }
      for (const HdTexelPage& page : kHdTexelPages) {
        if (page.eid != eid) continue;
        const auto tim_file = vfs_->read(std::string(page.path));
        const auto tim = tim_file ? import::parse_tim(*tim_file) : std::nullopt;
        auto indices = tim && tim->bpp == 8 ? import::decode_tim(*tim) : std::nullopt;
        if (indices && indices->indexed) pack->hd_texels = FxPack::HdTexels{page.x, page.y, std::move(*indices)};
      }
      pack->has_main = pack->fx.find_sequence("main") != nullptr;
      // Pack-table entries the sequences start on the casters (ops 0x43 / 0x45, entry s16 at data +4).
      std::set<int> caster_entries;
      // Ops 0x4F / 0x57 run a model's OWN S5 entry (its Damage ops are outside the pack).
      bool own_s5 = false;
      for (const auto& sq : pack->fx.sequences)
        for (const auto& cmd : sq.cmds)
          if (cmd.op == 0x47 || cmd.op == 0x48) pack->has_damage_op = true;
          else if (cmd.op == 0x44 || cmd.op == 0x46) pack->has_react_op = true;
          else if ((cmd.op == 0x43 || cmd.op == 0x45) && cmd.data.size() >= 6) caster_entries.insert(static_cast<s16>(cmd.data[4] | (cmd.data[5] << 8)));
          else if (cmd.op == 0x4F || cmd.op == 0x57) own_s5 = true;
      pack->overlay = effects::overlay_stem(slot_entry->prg);
      pack->bespoke = effects::BespokeRegistry::instance().find_element(eid);
      // Entries register from nested archives (type 1) as well: the Element Shot packs keep `main` and `tech` inside `elda`.
      const std::function<void(ByteSpan)> register_scripts = [&](ByteSpan bytes) {
        const auto drp = import::parse_drp(bytes);
        if (!drp) return;
        for (const auto& entry : drp->entries)
          if (entry.type == 0x1A) {
            // The last one registered wins.
            pack->has_anim = pack->anim.parse(entry.data) || pack->has_anim;
            bseq::Table table;
            if (table.parse(entry.data)) pack->tables[entry.name] = std::move(table);
          } else if (entry.type == 0x19) {
            // The caster's tech clips.
            pack->clip_packs.emplace_back(entry.name, Bytes(entry.data.begin(), entry.data.end()));
          } else if (entry.type == 0x01) {
            register_scripts(entry.data);
          }
      };
      register_scripts(pack->prd);
      // The caster variant = its acting-list index ; up to three casters (a group cast).
      if (pack->has_anim)
        for (const int entry : caster_entries)
          for (int variant = 0; variant < 3 && !pack->caster_script_reacts; ++variant)
            pack->caster_script_reacts = script_emits(pack->anim, entry, variant, bseq::Event::React);
      // The reaction is the only place the hit could land and it calls no CallDamage: a stock host (its overlay code calls no damage
      // routine: stdtech0 / stdmag0), no bespoke module, no sequence damage op (0x47 / 0x48), no own-S5 run (0x4F / 0x57), and variant 7
      // RUNS TO ITS END without a Damage op (a variant 7 that waits on another script's flag is left alone).
      pack->reaction_lands_no_number = pack->has_anim && !pack->bespoke && !pack->has_damage_op && !own_s5 &&
                                       (pack->overlay == "stdtech0" || pack->overlay == "stdtech1" || pack->overlay == "stdmag0") &&
                                       script_ends_without(pack->anim, 0, 7, bseq::Event::Damage);
      // ... nor the casters' own runs of the table (entry 0 + the sequence-started entries, variants 0..2): a caster's Damage op is CallDamage on
      // the caster's block, which the hit pass mirrors from the target, i.e. the number of a melee tech.
      caster_entries.insert(0);
      for (const int entry : caster_entries)
        for (int variant = 0; variant < 3 && pack->reaction_lands_no_number; ++variant)
          if (script_emits(pack->anim, entry, variant, bseq::Event::Damage)) pack->reaction_lands_no_number = false;
      // A pack with only a type-0x1A 'main' table (kill, comebigi, konsin, bre_roza) is the caster's script: the stock host runs its entry 0.
      if (pack->has_main || !pack->fx.emitters.empty() || pack->tables.count("main")) out = pack;
      else log::debug("battle fx: eid {} has no main sequence, emitters nor script table", eid);
    }
  }
  cache_[eid] = out;
  return out;
}

// ---------------------------------------------------------------------------------------------------------------- run
BattleFxRun::BattleFxRun(std::shared_ptr<const FxPack> pack, const assets::RoomEffects& particle_lib, u32 seed) : pack_(std::move(pack)) {
  env_meshes_ = effects::EnvMeshes(ByteSpan(pack_->prd.data(), pack_->prd.size()));
  bank_.add(&pack_->fx);
  if (pack_->extra) bank_.add(pack_->extra.get());
  if (pack_->resident)
    for (const auto& resident_pack : pack_->resident->packs) bank_.add_library(resident_pack.get());
  bank_.add_library(&particle_lib);
  effects::EmitterSystem::Config cfg;
  cfg.seed = seed ? seed : 1;
  // Position shift is 5 in battle (3 in the field / title): quads, velocities, radii x4.
  cfg.pos_shift = 5;
  // OC_FX_ONLY footprint measurements apply to the pack's objects only.
  cfg.dev_only_filter = true;
  em_ = std::make_unique<effects::EmitterSystem>(bank_, cfg);
  seq_ = std::make_unique<effects::SequencePlayer>(bank_, *em_, seed ? seed : 1);

  effects::ActorHooks ah;
  ah.actor_basis = [this](int sel, int joint, int second_sel, int second_joint, Basis& out, int& scale) { return actor_basis(sel, joint, second_sel, second_joint, out, scale); };
  ah.anchor_basis = [](int, Basis& out) {
    out = Basis{};
    return true;
  };
  em_->set_hooks(std::move(ah));

  effects::SequenceHooks sh;
  sh.camera = [this](std::string_view name, const assets::EffectBlob*) {
    if (cb_.camera) cb_.camera(name);
  };
  sh.akao = [this](std::string_view name, const assets::EffectBlob*, int volume, int pan8, int /*mask: 0 in every stock pack*/) {
    if (cb_.sound) cb_.sound(name, volume, pan8);
  };
  sh.shake = [this](int ax, int ay, int ticks) {
    if (cb_.shake) cb_.shake(ax, ay, ticks);
  };
  // op 0x65: the back colour ramp of the lit models.
  sh.ambient = [this](int r, int g, int b, int ticks) {
    if (cb_.ambient) cb_.ambient(r, g, b, ticks);
  };
  sh.other = [this](const assets::SeqCmd& cmd) { on_other(cmd); };
  sh.actor_busy = [this] { return cb_.actor_busy ? cb_.actor_busy() : false; };
  sh.has_target = [this](int target) { return target >= 0 && target < static_cast<int>(scene_.targets.size()); };
  seq_->set_hooks(std::move(sh));
}

BattleFxRun::~BattleFxRun() = default;

void BattleFxRun::set_sfx(audio::Sfx* sfx) { seq_->set_sfx(sfx); }
void BattleFxRun::set_lights(effects::LightTable* table) { em_->set_lights(table); }

void BattleFxRun::on_other(const assets::SeqCmd& original) {
  cursor_ = seq_->dispatch_cursor();
  // A sequence started by a task that served target k > 0 runs a copy whose target operands read k: 0x45 caster s16@10
  // := 0, 0x46 / 0x57 / 0x59 target s16@10 := k, 0x47 / 0x6F / 0x70 target s16@8 := k, 0x58 target byte @0xB := k (offsets from the command).
  assets::SeqCmd patched;
  const assets::SeqCmd* cmd_ptr = &original;
  // The same operand rewrite with both parameters (caster, target). A per-target copy is (0, k); a CLONE of the charge sequence
  // for the group's extra caster i is (i, 0): op 0x45 s16@10 := i, every target operand := 0.
  const int clone_caster = seq_->dispatch_caster();
  if (const int inherited = seq_->dispatch_inherited(); inherited != 0 || clone_caster != 0) {
    auto put16 = [&](size_t off, int value) {
      if (off >= 4 && off - 4 + 2 <= patched.data.size()) {
        patched.data[off - 4] = static_cast<u8>(value);
        patched.data[off - 3] = static_cast<u8>(value >> 8);
      }
    };
    patched = original;
    switch (original.op) {
      case 0x45: put16(10, clone_caster); cmd_ptr = &patched; break;
      case 0x46: case 0x57: case 0x59: put16(10, inherited); cmd_ptr = &patched; break;
      case 0x47: case 0x6F: case 0x70: put16(8, inherited); cmd_ptr = &patched; break;
      case 0x58:
        if (patched.data.size() > 7) patched.data[7] = static_cast<u8>(inherited);
        cmd_ptr = &patched;
        break;
      default: break;
    }
  }
  const assets::SeqCmd& cmd = *cmd_ptr;
  // CallDamage on target s16@8.
  if (cmd.op == 0x47) {
    damage_seen_ = true;
    const int idx = cmd.data.size() >= 6 ? static_cast<s16>(cmd.data[4] | (cmd.data[5] << 8)) : 0;
    if (cb_.damage) cb_.damage(std::clamp(idx, 0, std::max(0, static_cast<int>(scene_.targets.size()) - 1)));
    return;
  }
  // Model copies (the model block and its joints): effects read
  // the copy, so the effect stays on the pose / place of the moment while the model moves on. 0x6F / 0x70 target s16@8, 0x71 / 0x72 every
  // target, 0x77 / 0x78 caster s16@8 (on / off). Every copy is freed when a pack starts.
  if (cmd.op == 0x6F || cmd.op == 0x70 || cmd.op == 0x71 || cmd.op == 0x72 || cmd.op == 0x77 || cmd.op == 0x78) {
    const bool on = cmd.op == 0x6F || cmd.op == 0x71 || cmd.op == 0x77;
    const int idx = cmd.data.size() >= 6 ? static_cast<s16>(cmd.data[4] | (cmd.data[5] << 8)) : 0;
    std::vector<int> unit_list;
    if (cmd.op == 0x71 || cmd.op == 0x72) unit_list = scene_.targets;
    else if (cmd.op == 0x77 || cmd.op == 0x78) {
      if (idx == 0) unit_list.push_back(scene_.caster);
    } else if (idx >= 0 && idx < static_cast<int>(scene_.targets.size())) {
      unit_list.push_back(scene_.targets[static_cast<size_t>(idx)]);
    }
    for (int unit : unit_list)
      if (unit >= 0 && unit < battle::kUnits) {
        auto& frozen = frozen_[static_cast<size_t>(unit)];
        // Copies only when no copy exists.
        if (on && !frozen) frozen = scene_.unit[static_cast<size_t>(unit)];
        if (!on) frozen.reset();
      }
    return;
  }
  if (cmd.op == 0x48) {
    damage_seen_ = true;
    if (cb_.damage) cb_.damage(-1);
    return;
  }
  // With no overlay resident (the charge) this flags the element pack ready to start.
  if (cmd.op == 0x41) {
    ready_seen_ = true;
    if (cb_.ready) cb_.ready();
    return;
  }
  // The overlay's dispatcher sees every op before the stock handler; some replace a stock op (0x59).
  if (bespoke_ && cmd.op < 0x100) {
    int args[16];
    int arg_count = 0;
    for (size_t i = 0; i + 1 < cmd.data.size() && arg_count < 16; i += 2) args[arg_count++] = static_cast<s16>(cmd.data[i] | (cmd.data[i + 1] << 8));
    models_.cursor = cursor_;
    if (bespoke_->stock_op(cmd.op, args, arg_count)) return;
  }
  if (cmd.op >= 0x100) {
    ++private_ops_;
  }
  if (cmd.op >= 0x100 && bespoke_) {
    int args[16];
    int arg_count = 0;
    for (size_t i = 0; i + 1 < cmd.data.size() && arg_count < 16; i += 2) args[arg_count++] = static_cast<s16>(cmd.data[i] | (cmd.data[i + 1] << 8));
    // The per-target loop index some overlays address the target list with.
    models_.cursor = cursor_;
    if (!bespoke_->command(cmd.op, args, arg_count)) ++private_ignored_[cmd.op];
    apply_list_overrides();
    return;
  }
  if (cmd.op >= 0x100 && !stock_host(pack_->overlay)) ++private_ignored_[cmd.op];
  if (cb_.other) cb_.other(cmd);
}

effects::FxContext BattleFxRun::make_context(const effects::ViewParams& view) const {
  effects::FxContext ctx;
  auto place = [&](effects::FxActor& actor, int unit) {
    if (unit < 0 || unit >= battle::kUnits) return;
    const FxUnit& fx_unit = scene_.unit[static_cast<size_t>(unit)];
    actor.pos = {fx_unit.x, 0.0f, fx_unit.z};
  };
  place(ctx.caster, scene_.caster);
  place(ctx.target, scene_.targets.empty() ? scene_.caster : scene_.targets.front());
  ctx.view = view;
  ctx.seed = 1;
  // Battle host for the overlays: the steerable models, the pack's emitters / sequences / meshes.
  ctx.models = const_cast<effects::FxModels*>(&models_);
  ctx.emitters = em_.get();
  ctx.sequences = seq_.get();
  ctx.meshes = pack_->meshes.get();
  ctx.pack_fx = &pack_->fx;
  ctx.pack_prd = ByteSpan(pack_->prd.data(), pack_->prd.size());
  if (cb_.stage) ctx.hooks.set_stage = cb_.stage;
  if (cb_.stage_sound) ctx.hooks.stage_sound = cb_.stage_sound;
  if (cb_.stage_shot) ctx.hooks.stage_shot = cb_.stage_shot;
  if (cb_.fog_off) ctx.hooks.fog_off = cb_.fog_off;
  if (cb_.lights) ctx.hooks.lights = cb_.lights;
  if (cb_.form) ctx.hooks.form_swap = cb_.form;
  if (cb_.pose) ctx.hooks.set_pose = cb_.pose;
  if (cb_.song) ctx.hooks.play_song = cb_.song;
  ctx.hooks.start_for_caster = [this](std::string_view name, int caster) { seq_->start_caster_clone(name, caster); };
  ctx.hooks.script_gate = [] { return bseq::Vm::global(0); };
  ctx.hooks.clear_script_gate = [] { bseq::Vm::set_global(0, 0); };
  return ctx;
}

// The caster / target lists as the overlays see them: one model block per unit, home = the stand position (y 0 = the ground), yaw from
// the view's facing (FxUnit::yaw = atan2(-face_x, -face_z) and face = (-sin ry, cos ry), so ry = pi - yaw).
void BattleFxRun::init_models() {
  models_ = effects::FxModels{};
  models_.units.resize(battle::kUnits);
  for (int i = 0; i < battle::kUnits; ++i) {
    const FxUnit& fu = scene_.unit[static_cast<size_t>(i)];
    effects::FxModel& model = models_.units[static_cast<size_t>(i)];
    model.unit = i;
    model.pc = fu.pc;
    model.home = model.pos = {fu.x, 0.0f, fu.z};
    model.home_rot[1] = model.rot[1] = static_cast<int>(std::lround(fu.yaw * 4096.0f / 6.2831853f)) & 0xFFF;
    model.height = fu.height;
    model.hidden = !fu.present;
    model.offstage = fu.offstage;
    model.hit = fu.hit;
    model.dead = fu.dead;
    model.fx_scale = fu.fx_scale;
    model.fx_scale_h = fu.fx_scale_h;
    // The posed model's joint translations (model space) for the overlays' bone look-ups.
    if (fu.joint)
      model.joint_local = [fn = fu.joint](int joint, Vec3& out) {
        float x = 0, y = 0, z = 0;
        if (!fn(joint, x, y, z)) return false;
        out = {x, y, z};
        return true;
      };
  }
  models_.casters = scene_.casters.empty() ? std::vector<int>{scene_.caster} : scene_.casters;
  models_.targets = scene_.targets;
  // Everyone: enemies first, then the party.
  for (int i = battle::kPcSlots; i < battle::kUnits; ++i)
    if (scene_.unit[static_cast<size_t>(i)].present) models_.everyone.push_back(i);
  for (int i = 0; i < battle::kPcSlots; ++i)
    if (scene_.unit[static_cast<size_t>(i)].present) models_.everyone.push_back(i);
  models_.point = [this](int sel, int joint, Vec3& out) {
    float yaw = 0;
    return point_of(sel, joint, out, yaw);
  };
  sync_unsteered();
}

// A model block the overlay does not steer is where the unit stands: the unit's own scripts move it (a reaction's toss lifts it), and
// the overlays read that live position (PS1 axes, y down).
void BattleFxRun::sync_unsteered() {
  for (int i = 0; i < battle::kUnits; ++i) {
    effects::FxModel& model = models_.units[static_cast<size_t>(i)];
    if (model.steer) continue;
    const FxUnit& fu = scene_.unit[static_cast<size_t>(i)];
    model.pos = {fu.x, -fu.y, fu.z};
  }
}

void BattleFxRun::start(const FxScene& scene, const effects::ViewParams& view) {
  scene_ = scene;
  cursor_ = 0;
  ticks_ = 0;
  pace_left_ = 0;
  if (const char* dt = std::getenv("OC_FX_DT"); dt && !pace_fixed_) set_pace(dt);
  em_->register_all();
  // Pack registration: a type-0x24 entry sets the GTE back colour from its first three bytes at once (e.g. 80 80 80). The importer keeps no such blob, so read the raw drp.
  if (cb_.ambient)
    if (auto drp = import::parse_drp(pack_->prd))
      for (const auto& entry : drp->entries)
        if (entry.type == 0x24 && entry.data.size() >= 3) cb_.ambient(entry.data[0], entry.data[1], entry.data[2], 0);
  if (pack_->bespoke && pack_->bespoke->battle) {
    init_models();
    bespoke_ = effects::BespokeRegistry::instance().create(pack_->bespoke->name, make_context(view));
  }
  const bool own = scene_.targets.size() == 1 && scene_.targets[0] == scene_.caster;
  if (!pack_->start_seq.empty()) {
    if (!seq_->start(pack_->start_seq)) log::debug("battle fx: pack {} has no sequence '{}'", pack_->eid, pack_->start_seq);
    // After the charge sequence started: for every further acting-list entry (a group cast) clone the
    // template task and bind it to caster i.
    else
      for (size_t i = 1; i < scene_.casters.size(); ++i) seq_->start_caster_clone(pack_->start_seq, static_cast<int>(i));
  } else if (bespoke_ && models_.suppress_main) {
    // saraman starts 'main' itself once its first motion file is in.
    log::debug("battle fx: eid {}: the overlay starts 'main' itself", pack_->eid);
  } else if (!seq_->start_main(own)) {
    // Only 'own' / 'main' start automatically. The other packs (most techs, many enemy attacks) are driven by the actors' anmseq scripts:
    // ops 0x68 / 0x7D start the pack's sequence of the given tag (start_sequence()).
    log::debug("battle fx: eid {} has no main / own: its sequences wait for the anmseq scripts", pack_->eid);
  }
}

int BattleFxRun::start_script_sequences(const std::set<u32>& actor_tags) {
  if (!pack_->start_seq.empty()) return 0;
  // A pack that carries caster scripts or clips (types 0x1A / 0x19), or has no 'main', is driven by scripts of its own: every root
  // sequence may start. One played by its 'main' alone runs another root sequence only when a unit's script names it. A root
  // sequence already running (the overlay's init started it) or one the overlay starts later from its own op is left to the overlay.
  const bool own_scripts = !pack_->has_main || !pack_->tables.empty() || !pack_->clip_packs.empty();
  std::set<std::string> named;
  for (const u32 tag : actor_tags) named.insert(bseq::tag_str(tag));
  std::set<std::string> children, reached;
  for (const auto& sq : pack_->fx.sequences)
    for (const auto& cmd : sq.cmds)
      if (cmd.op == 0x2F) children.insert(cmd.name);
  std::vector<std::string> todo = {"main", "own"};
  while (!todo.empty()) {
    const std::string name = todo.back();
    todo.pop_back();
    if (!reached.insert(name).second) continue;
    if (const auto* sq = pack_->fx.find_sequence(name))
      for (const auto& cmd : sq->cmds)
        if (cmd.op == 0x2F) todo.push_back(cmd.name);
  }
  int started = 0;
  for (const auto& sq : pack_->fx.sequences)
    if (!children.count(sq.name) && !reached.count(sq.name) && sq.name != "loop" && !seq_->running(sq.name) &&
        !(bespoke_ && bespoke_->starts_sequence(sq.name)) && (own_scripts || named.count(sq.name)))
      started += start_sequence(sq.name) ? 1 : 0;
  return started;
}

bool BattleFxRun::start_sequence(std::string_view tag) {
  // The pack's own group first; a tag it lacks falls back to the global registry = the battle's camera timelines
  // (battle.prd > tana: atk0 ...), which the caller plays.
  return pack_->fx.find_sequence(tag) != nullptr && seq_->start(tag);
}

void BattleFxRun::set_pace(std::string_view spec) {
  pace_.clear();
  size_t i = 0;
  while (i < spec.size()) {
    size_t j = spec.find(',', i);
    if (j == std::string_view::npos) j = spec.size();
    const std::string item(spec.substr(i, j - i));
    const size_t colon = item.find(':');
    const int pack_tick = colon == std::string::npos ? 0 : std::atoi(item.substr(0, colon).c_str());
    const int dt = std::atoi(item.c_str() + (colon == std::string::npos ? 0 : colon + 1));
    if (dt >= 1) pace_.emplace_back(pack_tick, std::min(dt, 10));
    i = j + 1;
  }
  std::sort(pace_.begin(), pace_.end());
}

void BattleFxRun::catch_up(const FxScene& scene, int ticks) {
  scene_ = scene;
  for (int i = 0; i < ticks; ++i) {
    seq_->tick();
    em_->tick();
  }
}

void BattleFxRun::tick(const FxScene& scene) {
  scene_ = scene;
  apply_list_overrides();
  if (bespoke_) {
    sync_unsteered();
    // Before the sequence: its private ops reach the overlay during seq_->tick().
    for (auto& model : models_.units) model.save_prev();
  }
  seq_->tick();
  em_->tick();
  if (bespoke_) {
    int dt = 1;
    for (const auto& entry : pace_)
      if (entry.first <= ticks_) dt = entry.second;
    // One overlay call per dt ticks as far as the emitter fires go.
    if (dt > 1) {
      const bool fresh = pace_left_ <= 0;
      if (fresh) pace_left_ = dt;
      --pace_left_;
      em_->begin_overlay_tick(fresh);
    }
    bespoke_->tick();
    if (dt > 1) em_->end_overlay_tick();
  }
  if (bespoke_ && models_.pack_drive.active && !models_.pack_drive.host) {
    // No view consumes the overlay-driven pack model (the headless tools, test_battle_fx): a stream request is acknowledged as loaded with
    // no clip and the clip reported over, so the overlay's state machine (saraman: 'main' once file 0 is in) runs through.
    effects::FxModels::PackDrive& drive = models_.pack_drive;
    if (drive.motion_request >= 0 && static_cast<size_t>(drive.motion_request) < drive.motion_state.size()) drive.motion_state[static_cast<size_t>(drive.motion_request)] = 2;
    drive.motion_request = -1;
    drive.clip = -1;
    drive.at_end = true;
  }
  ++ticks_;
  idle_ = seq_->active_non_loop() == 0 ? idle_ + 1 : 0;
}

bool BattleFxRun::done() const {
  if (ticks_ < 2) return false;
  if (ticks_ > kTickCap) return true;
  if (seq_->active_non_loop() != 0) return false;
  // A pack without 'main' plays when the actors' scripts ask for its sequences: keep it while the caster's script runs.
  // The same hold for every pack - the caster's script is part of the running effect.
  if (cb_.actor_busy && cb_.actor_busy()) return false;
  if (bespoke_ && !bespoke_->done()) return idle_ > 600;
  // The pack ends with its sequences: the overlay exit frees every emitter object at once, live or not. Emitters still running (a 'loop' sequence's) hold the pack for a bounded time.
  const auto st = em_->stats();
  if (st.running == 0) return true;
  return idle_ > 30;
}

void BattleFxRun::collect(const effects::ViewParams& view, float alpha, FxFrame& out) const {
  out.quads.clear();
  out.flats.clear();
  out.textured.clear();
  em_->collect_quads(view, alpha, out.quads);
  effects::sort_draw_order(out.quads);
  if (bespoke_) {
    std::vector<effects::FxPrim> prims;
    bespoke_->set_view(view);
    bespoke_->draw(prims);
    effects::sort_prims(prims);
    std::vector<effects::FxPrim> plain;
    for (auto& prim : prims) (prim.tex >= 0 || prim.src != effects::FxPrim::TexSrc::Pack ? out.textured : plain).push_back(prim);
    effects::flatten_prims(plain, out.flats);
  }
}

int BattleFxRun::shake_x() const {
  int ax = 0, ay = 0;
  seq_->shake_amplitude(ax, ay);
  return ax;
}
int BattleFxRun::shake_y() const {
  int ax = 0, ay = 0;
  seq_->shake_amplitude(ax, ay);
  return ay;
}

// An overlay that replaced the list getters (seint unico etrap tunico) makes every selector and per-target loop read its
// lists; an empty caster list = no caster (the selectors fall back to the identity at the origin).
void BattleFxRun::apply_list_overrides() {
  if (!bespoke_) return;
  if (models_.target_override) scene_.targets = models_.target_list;
  if (models_.caster_override) {
    scene_.caster = models_.caster_list.empty() ? -1 : models_.caster_list.front();
    scene_.casters = models_.caster_list;
  }
}

// ---- actor selectors ---------------------------------------------------------------------------------------------------
// The model an effect reads: the copy taken by ops 0x6F / 0x71 / 0x77 while it exists, else the live model.
const FxUnit& BattleFxRun::unit_at(int unit) const {
  if (unit == kPseudoSlot) return scene_.pseudo;
  const size_t index = static_cast<size_t>(unit);
  return frozen_[index] ? *frozen_[index] : scene_.unit[index];
}

// The actor lists are the action's casters, targets and everyone. 1 / 9 = caster 0,
// 2 / 0xD = target 0, 0xE..0x15 = target 1..8, 10 / 11 = caster 1 / 2, 3 / 4 = the casters' / targets' centroid (yaw = mean yaw), 5 = everyone's
// centroid, 0x16 / 0x17 = the targets' centroid on the ground, 0x19 / 0x1A = the side of caster 0 / target 0: the point (-2048, 0, 0) facing
// 0x800 when its x < 1, else (2048, 0, 0) facing 0. A missing actor gives the identity at the origin.
// Joint: 0 = the origin, 1 = origin + the height offset, 0x22 = the origin on the ground, 0x23 = half the height
// offset, other = attach point joint - 2 of the model (FxUnit::attach, else 55 % of the height). The basis keeps
// the actor's yaw as its rotation.
bool BattleFxRun::point_of(int sel, int joint, Vec3& pos, float& yaw) const {
  const int nt = static_cast<int>(scene_.targets.size());
  auto target = [&](int index) { return index >= 0 && index < nt ? scene_.targets[static_cast<size_t>(index)] : -1; };
  auto height_of = [&](int unit) {
    const float height = unit_at(unit).height;
    switch (joint) {
      case 0: case 0x22: return 0.0f;
      case 1: return -height;
      case 0x23: return -height * 0.5f;
      default: return -height * 0.55f;
    }
  };
  // A model the overlay steers owns its yaw as well (bboy's targets turn to 0xC00 before the 'bway' basis is read).
  auto yaw_of = [&](int unit) {
    if (bespoke_ && static_cast<size_t>(unit) < models_.units.size() && models_.units[static_cast<size_t>(unit)].steer)
      return static_cast<float>(models_.units[static_cast<size_t>(unit)].rot[1] & 0xFFF) * 6.2831853f / 4096.0f;
    return unit_at(unit).yaw;
  };
  std::vector<int> us;
  bool ground = false;
  switch (sel) {
    case 1: case 9: us = {scene_.caster}; break;
    case 2: case 0xD: us = {target(0)}; break;
    // Only reached with a pseudo-actor (actor_basis falls back to the caster otherwise).
    case 8: us = {kPseudoSlot}; break;
    case 3:  // the bounding-box midpoint of every acting-list entry
      us = scene_.casters.empty() ? std::vector<int>{scene_.caster} : scene_.casters;
      break;
    case 4: case 0x16: case 0x17:
      for (int k = 0; k < nt; ++k) us.push_back(target(k));
      ground = sel != 4;
      break;
    case 5:
      for (int i = 0; i < battle::kUnits; ++i)
        if (scene_.unit[static_cast<size_t>(i)].present) us.push_back(i);
      break;
    // Acting-list entry 1 / 2 (none when the list is shorter).
    case 10: case 11: {
      const size_t entry = static_cast<size_t>(sel - 9);
      us = {entry < scene_.casters.size() ? scene_.casters[entry] : -1};
      break;
    }
    case 0x19: case 0x1A: {
      const int unit = sel == 0x19 ? scene_.caster : target(0);
      if (unit < 0) return false;
      const bool neg = unit_at(unit).x < 1.0f;
      pos = {neg ? -2048.0f : 2048.0f, 0.0f, 0.0f};
      // Facing 0x800 or 0, mapped onto the FxUnit yaw convention.
      yaw = neg ? 3.14159265f : 0.0f;
      return true;
    }
    default:
      if (sel >= 0xE && sel <= 0x15) us = {target(sel - 0xD)};
      break;
  }
  us.erase(std::remove(us.begin(), us.end(), -1), us.end());
  if (us.empty()) return false;
  float sx = 0, sy = 0, sz = 0, syaw = 0;
  // Selectors 3 / 4 / 5 / 0x16 / 0x17: the point of a list is the MIDPOINT OF ITS BOUNDING BOX per axis, (max + min) / 2 on the
  // s16 coordinates (C division), not the mean; the yaw alone is a mean.
  float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
  for (int unit : us) {
    const FxUnit& fu = unit_at(unit);
    float ax = 0, ay = 0, az = 0;
    // A model the overlay steers (FxModel::steer: Freefall lifts its target to y -15000) is where the overlay put it: the effect selectors read
    // the model block's position, which the overlay writes; the view's own placement is only the home.
    // PS1 y down: a script-lifted unit's points are above the floor.
    float ox = 0, oy = -fu.y, oz = 0;
    if (bespoke_ && static_cast<size_t>(unit) < models_.units.size() && models_.units[static_cast<size_t>(unit)].steer) {
      const effects::FxModel& om = models_.units[static_cast<size_t>(unit)];
      ox = om.pos[0] - fu.x;
      oy = om.pos[1];
      oz = om.pos[2] - fu.z;
    }
    // Joint 0x22: y is ABSOLUTE 0, the unit's own (lifted / launched) y does not count, so a splash is filled at the floor under an
    // airborne target.
    if (joint == 0x22) oy = 0;
    if (joint >= 2 && joint != 0x22 && joint != 0x23 && fu.attach && fu.attach(joint - 2, ax, ay, az)) {
      // the attach point through the model's yaw (the same rotation the basis gets), on the model origin
      const float unit_yaw = yaw_of(unit);
      const float cs = std::cos(unit_yaw), sn = std::sin(unit_yaw);
      float dx = cs * ax + sn * az, dy = ay, dz = -sn * ax + cs * az;
      // Rz in front of the yaw (RotMatrix = Rz * Ry * Rx).
      if (fu.rot_z != 0.0f) {
        const float cz = std::cos(fu.rot_z), sz2 = std::sin(fu.rot_z), nx = cz * dx - sz2 * dy;
        dy = sz2 * dx + cz * dy;
        dx = nx;
      }
      sx += fu.x + dx;
      sz += fu.z + dz;
      sy += dy;
    } else {
      sx += fu.x;
      sz += fu.z;
      sy += height_of(unit);
    }
    sx += ox;
    sy += oy;
    sz += oz;
    syaw += yaw_of(unit);
    const float pt[3] = {sx, sy, sz};
    for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], pt[k]), hi[k] = std::max(hi[k], pt[k]);
    sx = sy = sz = 0;
  }
  const float count = static_cast<float>(us.size());
  yaw = syaw / count;
  auto mid = [&](int axis) { return us.size() == 1 ? lo[axis] : std::trunc((std::round(hi[axis]) + std::round(lo[axis])) * 0.5f); };
  // PS1 world: y down, feet at 0.
  pos = {mid(0), ground ? 0.0f : mid(1), mid(2)};
  return true;
}

bool BattleFxRun::actor_basis(int sel, int joint, int second_sel, int second_joint, Basis& out, int& scale) const {
  scale = 4096;
  out = Basis{};
  // Dev trace of every ORIENT_BASIS resolve (selector bytes, the points).
  static const bool trace_basis = std::getenv("OC_BASIS_TRACE") != nullptr;
  if (trace_basis) log::debug("basis: call sel {} joint {} c {} d {}", sel, joint, second_sel, second_joint);
  if (sel == 0) return true;
  Vec3 point{0, 0, 0};
  float yaw = 0;
  // The single-actor selectors return the actor's model block (none for the averages); op 0x39 with flag bit 1 / 2 takes
  // its effect scale: every later scale op is multiplied by it.
  // Selectors 8 / 0xC (the registered slot) return the model block too: slot 0 = the caster, 1 = target 0.
  {
    const int nt = static_cast<int>(scene_.targets.size());
    int unit = -1;
    if (sel == 8 && scene_.pseudo.present) unit = kPseudoSlot;
    else if (sel == 1 || sel == 9 || sel == 8) unit = scene_.caster;
    else if (sel == 2 || sel == 0xD || sel == 0xC) unit = nt > 0 ? scene_.targets[0] : -1;
    else if (sel >= 0xE && sel <= 0x15) unit = sel - 0xD < nt ? scene_.targets[static_cast<size_t>(sel - 0xD)] : -1;
    if (unit >= 0 && unit <= kPseudoSlot) scale = unit_at(unit).fx_scale;
  }
  if (sel == 8 || sel == 0xC) {
    if (em_->slot_position(sel == 8 ? 0 : 1, point)) {
      out.t = point;
      return true;
    }
    // Slot A with no registered effect particle: the pack's pseudo-actor (act0), else the caster / target themselves.
    if (!(sel == 8 && scene_.pseudo.present)) sel = sel == 8 ? 1 : 2;
  }
  if (!point_of(sel, joint, point, yaw)) return true;
  if (second_sel != 0) {
    Vec3 point2{0, 0, 0};
    float yaw2 = 0;
    if (point_of(second_sel, second_joint, point2, yaw2)) {
      if (trace_basis) log::debug("basis: sel {} joint {} -> ({:.0f} {:.0f} {:.0f}) | sel {} joint {} -> ({:.0f} {:.0f} {:.0f})", sel, joint, point[0], point[1], point[2], second_sel, second_joint, point2[0], point2[1], point2[2]);
      // RotMatrix(0, -ratan2(dz, dx), ratan2(dy, |dxz|)) (local +X from the first point to the second, pitched), then
      // ScaleMatrix(|dxz| * 2 / cos(pitch), 1, same) in 4.12 = the X / Z columns scaled by the distance / 2048; origin at the midpoint.
      const float dx = point2[0] - point[0], dy = point2[1] - point[1], dz = point2[2] - point[2];
      const float hd = std::hypot(dx, dz);
      const int yaw_a = -effects::ratan2(static_cast<int>(dz), static_cast<int>(dx));
      const int pitch_a = effects::ratan2(static_cast<int>(dy), static_cast<int>(hd));
      const float cp = std::max(0.05f, static_cast<float>(effects::rcos(pitch_a)) / 4096.0f);
      const float stretch = std::max(0.05f, hd * 2.0f / cp / 4096.0f);
      out.m = effects::Mat3::from_angles(0, yaw_a, pitch_a).scaled_columns(stretch, 1.0f, stretch);
      out.t = {(point[0] + point2[0]) * 0.5f, (point[1] + point2[1]) * 0.5f, (point[2] + point2[2]) * 0.5f};
      return true;
    }
  }
  const float cs = std::cos(yaw), sn = std::sin(yaw);
  out.m.m = {cs, 0, sn, 0, 1, 0, -sn, 0, cs};
  out.t = point;
  // An object with flag bit 7 has its matrix multiplied by the attach joint's WORLD rotation when the op-0x39 selector names one actor
  // (with a 2nd point the scaled look-at matrix is used instead, which this path never uses) - e.g. a beam that turns
  // with the eye bone and sweeps across the party. The joint's world rotation = the actor's yaw * the posed joint's rotation.
  if (second_sel == 0 && joint >= 2 && joint != 0x22 && joint != 0x23) {
    const int nt = static_cast<int>(scene_.targets.size());
    int unit = -1;
    if (sel == 8) unit = kPseudoSlot;
    else if (sel == 1 || sel == 9) unit = scene_.caster;
    else if (sel == 2 || sel == 0xD) unit = nt > 0 ? scene_.targets[0] : -1;
    else if (sel >= 0xE && sel <= 0x15) unit = sel - 0xD < nt ? scene_.targets[static_cast<size_t>(sel - 0xD)] : -1;
    // Selectors 10 / 0xB: acting-list entry 1 / 2, returned as the model too.
    else if (sel == 10 || sel == 0xB) {
      const size_t entry = static_cast<size_t>(sel - 9);
      unit = entry < scene_.casters.size() ? scene_.casters[entry] : -1;
    }
    float rot[9];
    if (unit >= 0 && unit <= kPseudoSlot && unit_at(unit).attach_rot && unit_at(unit).attach_rot(joint - 2, rot)) {
      effects::Mat3 pal;
      for (size_t i = 0; i < 9; ++i) pal.m[i] = rot[i];
      out.joint = out.m * pal;
      // Rz * Ry * Rx * S * joint.
      if (unit == kPseudoSlot && unit_at(unit).rot_z != 0.0f) {
        const float cz = std::cos(unit_at(unit).rot_z), sz2 = std::sin(unit_at(unit).rot_z);
        effects::Mat3 rz;
        rz.m = {cz, -sz2, 0, sz2, cz, 0, 0, 0, 1};
        out.joint = rz * out.joint;
      }
      out.has_joint = true;
      if (trace_basis) log::debug("basis: joint rot unit {} attach {} row0 {:.2f} {:.2f} {:.2f} row1 {:.2f} {:.2f} {:.2f} row2 {:.2f} {:.2f} {:.2f}", unit, joint - 2, out.joint.m[0], out.joint.m[1], out.joint.m[2], out.joint.m[3], out.joint.m[4], out.joint.m[5], out.joint.m[6], out.joint.m[7], out.joint.m[8]);
    }
  }
  return true;
}

// ---- audit ------------------------------------------------------------------------------------------------------------------
BattleFxRun::OpClass BattleFxRun::op_class(int op, int len) {
  switch (op) {
    // flow and spawning (SequencePlayer): marker, end, emitter start / stop / kill, camera pool, child sequence, CD wait, per-target loop,
    // loop label, actor wait, yield, condition wait; 0x25 does nothing
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x25: case 0x2F: case 0x42: case 0x4D: case 0x4E: case 0x50: case 0x55: case 0x5A: case 0x67:
    case 0x7A:
    // sound: AKAO by name, SFX id
    case 0x4C: case 0x51:
    // screen: shake, screen colour layers, depth-cue ramp, depth cue on / off; the ground's OT bias ramp (StageScene::ground_bias_ramp)
    case 0x2E: case 0x2B: case 0x2C: case 0x2D: case 0x24: case 0x6C: case 0x6D: case 0x6E:
    // actors: visibility, renderer reset, anmseq entries, CallDamage, the charge hand-over, colour ramp, motion, shadow bit, sideways facing
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x61: case 0x62:
    case 0x63: case 0x64: case 0x68: case 0x69: case 0x49: case 0x4A: case 0x4B: case 0x43: case 0x44: case 0x45: case 0x46: case 0x4F:
    case 0x57: case 0x47: case 0x48: case 0x41: case 0x58: case 0x59: case 0x74: case 0x75: case 0x76: case 0x7B:
    // model copies the effect reads instead of the moving model; slow motion
    case 0x6F: case 0x70: case 0x71: case 0x72: case 0x77: case 0x78: case 0x56: case 0x79:
    // the screen copy warp bracket
    case 0x3E: case 0x3F: case 0x40:
    // stage layers: the stage scene's own emitters of a name switched off / on (through StageScene::emitters(),
    // BattleView::fx_stage_layer); 0x7B arms the bti0 pair 0x5F / 0x60
    case 0x26: case 0x27: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x5F: case 0x60:
    // the caster's own damage numbers (BattleView::fx_other)
    case 0x6B:
    // the lit model path: back colour ramp (SequencePlayer -> Callbacks::ambient), the three light intensities zeroed
    // (BattleView::fx_other); the lights themselves are sprite type 17 objects (EmitterSystem / LightTable)
    case 0x65: case 0x66:
    // pad rumble (SequencePlayer::vib_op -> platform::Rumble), the music volume fade (BattleView::fx_other -> BattleAudio::fade_music)
    case 0x28: case 0x29: case 0x54:
      return OpClass::Presented;
    // no picture and nothing to do here: the battle continuation hold counter (0x53 hold, 0x52 release one hold next frame) and the reload of
    // bttim.ltd into the shared VRAM (0x6A); the packs here own their textures and the effect end is BattleFxRun::done()
    case 0x52: case 0x53: case 0x6A:
      return OpClass::NonVisual;
    // short form = shake (rooms); the 16-byte battle form = colour matrix
    case 0x2A: return len < 16 ? OpClass::Presented : OpClass::Missing;
    default: return op >= 0x100 ? OpClass::Private : OpClass::Missing;
  }
}

}  // namespace oc::game
