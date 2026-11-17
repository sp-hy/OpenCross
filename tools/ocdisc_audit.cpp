// ocdisc audit: every file of a PS1 disc image through the importers the engine uses for it, next to the same importers on the cdrom.dat
// member of that name. The disc's file table gives sizes rounded up to 8 bytes, so a disc file may carry up to 7 trailing bytes the
// member does not. Two VFS instances serve the "cdrom" archive, one from the image and one from cdrom.dat; per-file checks run on every
// member they apply to, whole-game checks (tables, text, effect library, menu art) once. A Japanese disc holds the members' bytes, so
// every outcome and every decoded result (a digest of it) must be the same on both; any other build only has to import wherever the
// member does.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "assets/battle_stage.h"
#include "assets/model.h"
#include "assets/room_effects.h"
#include "assets/room_scene.h"
#include "audio/akao_bank.h"
#include "audio/akao_player.h"
#include "audio/sfx.h"
#include "battle/pac_vm.h"
#include "core/parallel.h"
#include "data/tables.h"
#include "game/battle/battle_fx.h"
#include "game/battle/battle_model_cache.h"
#include "game/disc_index.h"
#include "game/model_files.h"
#include "game/title_backdrop.h"
#include "import/battle_stage.h"
#include "import/container.h"
#include "import/disc/disc_backend.h"
#include "import/disc/disc_ident.h"
#include "import/disc/disc_image.h"
#include "import/lzss.h"
#include "import/model.h"
#include "import/ps1_font.h"
#include "import/ps1_text.h"
#include "import/room.h"
#include "import/room_effects.h"
#include "import/tim.h"
#include "ui/menu/game_state.h"
#include "ui/menu/ps1_art.h"
#include "ui/ps1_window.h"
#include "ui/shop/shop_logic.h"
#include "vfs/archive.h"
#include "vfs/vfs.h"

using namespace oc;

namespace {

// FNV-1a over the fields a check feeds it.
class Hasher {
 public:
  void bytes(const void* data, size_t size) {
    const u8* at = static_cast<const u8*>(data);
    for (size_t i = 0; i < size; ++i) value_ = (value_ ^ at[i]) * 0x100000001B3ull;
  }
  template <class T>
    requires std::is_arithmetic_v<T>
  void add(T number) {
    bytes(&number, sizeof number);
  }
  template <class T, size_t N>
  void add(const std::array<T, N>& numbers) {
    for (const T& number : numbers) add(number);
  }
  template <class T>
  void add(const std::vector<T>& numbers) {
    add(numbers.size());
    for (const T& number : numbers) add(number);
  }
  void add(const std::string& text) {
    add(text.size());
    bytes(text.data(), text.size());
  }
  template <class T, class Fn>
  void each(const std::vector<T>& items, Fn feed) {
    add(items.size());
    for (const T& item : items) feed(item);
  }
  u64 value() const { return value_; }

 private:
  u64 value_ = 0xCBF29CE484222325ull;
};

void feed(Hasher& hash, const import::Image& image) {
  hash.add(image.width);
  hash.add(image.height);
  hash.add(image.indexed);
  hash.add(image.pixels);
  hash.add(image.palette);
}

void feed(Hasher& hash, const import::Tim& tim) {
  hash.add(tim.bpp);
  hash.add(tim.has_clut);
  for (const import::TimBlock* block : {&tim.clut, &tim.image}) {
    hash.add(block->x);
    hash.add(block->y);
    hash.add(block->w);
    hash.add(block->h);
    hash.add(block->data);
  }
}

void feed(Hasher& hash, const assets::Mesh& mesh) {
  hash.each(mesh.pool, [&](const assets::SkinVertex& vertex) {
    hash.add(vertex.source_index);
    hash.add(vertex.bind_pos);
    hash.add(vertex.joint);
    hash.add(vertex.weight);
    for (const assets::Vec3& local : vertex.local) hash.add(local);
    hash.add(vertex.normal);
    hash.add(vertex.normal_joint);
  });
  hash.each(mesh.vertices, [&](const assets::MeshVertex& vertex) {
    hash.add(vertex.pool);
    hash.add(vertex.uv);
    hash.add(vertex.rgb);
  });
  hash.add(mesh.indices);
  hash.add(mesh.tri_quad);
  hash.each(mesh.materials, [&](const assets::Material& material) {
    hash.add(material.texture);
    hash.add(material.color);
  });
  hash.each(mesh.submeshes, [&](const assets::SubMesh& sub) {
    hash.add(sub.material);
    hash.add(sub.first_index);
    hash.add(sub.index_count);
    hash.add(sub.not_gouraud_textured);
  });
  hash.add(mesh.skinned);
  hash.add(mesh.trans);
}

void feed(Hasher& hash, const std::vector<assets::AnimationClip>& clips) {
  hash.each(clips, [&](const assets::AnimationClip& clip) {
    hash.add(clip.name);
    hash.add(clip.source);
    hash.add(clip.index);
    hash.add(clip.frame_count);
    hash.add(clip.duration);
    hash.each(clip.tracks, [&](const assets::JointTrack& track) {
      for (const std::vector<assets::Key3>* keys : {&track.rot, &track.trans})
        hash.each(*keys, [&](const assets::Key3& key) {
          hash.add(key.time);
          hash.add(key.v);
        });
    });
    hash.add(clip.rot_animated);
    hash.add(clip.trans_animated);
  });
}

void feed(Hasher& hash, const assets::AnimSeqTable& table) {
  hash.each(table, [&](const std::vector<s16>& entry) { hash.add(entry); });
}

void feed(Hasher& hash, const assets::ModelAsset& model) {
  hash.add(model.kind);
  hash.each(model.meshes, [&](const assets::Mesh& mesh) { feed(hash, mesh); });
  hash.each(model.skeleton.joints, [&](const assets::Joint& joint) {
    hash.add(joint.parent);
    hash.add(joint.rest_rot_deg);
    hash.add(joint.rest_trans);
    hash.add(joint.bone);
  });
  feed(hash, model.clips);
  for (const assets::AttachPoint& point : model.attach.points) {
    hash.add(point.pos);
    hash.add(point.joint);
    hash.add(point.used);
  }
  for (const assets::ShadingEntry& entry : model.shading) hash.add(std::array<u8, 4>{entry.r, entry.g, entry.b, entry.flags});
  hash.each(model.textures, [&](const import::Image& image) { feed(hash, image); });
  hash.add(model.main_texture_count);
  hash.add(model.drp_entries);
  hash.add(model.extent);
  feed(hash, model.anim_seq);
  feed(hash, model.ex_anim_seq);
  hash.add(model.eye.valid);
  hash.add(model.eye.x);
  hash.add(model.eye.y);
}

void feed(Hasher& hash, const assets::RoomEffects& fx) {
  hash.each(fx.sheets, [&](const assets::SpriteSheet& sheet) {
    hash.add(sheet.name);
    hash.add(std::array<u16, 4>{sheet.tex_x, sheet.tex_y, sheet.clut_x, sheet.clut_y});
    hash.each(sheet.frames, [&](const std::vector<assets::EffectSprite>& frame) {
      hash.each(frame, [&](const assets::EffectSprite& sprite) {
        hash.add(std::array<s32, 8>{sprite.x, sprite.y, sprite.u, sprite.v, sprite.clut, sprite.w, sprite.h, sprite.flags});
        hash.add(sprite.rgba);
      });
    });
    hash.add(std::array<s32, 4>{sheet.tim_x, sheet.tim_y, sheet.tim_w, sheet.tim_h});
  });
  hash.each(fx.emitters, [&](const assets::Emitter& emitter) {
    // payload_size is left out: see check_drp.
    hash.add(emitter.name);
    hash.add(std::array<s32, 5>{emitter.start, emitter.duration, emitter.period, emitter.repeat, static_cast<s32>(emitter.flags)});
    hash.each(emitter.ops, [&](const assets::EffectOp& op) { hash.add(std::array<u32, 3>{op.op, op.len, op.offset}); });
  });
  hash.each(fx.sequences, [&](const assets::Sequence& sequence) {
    hash.add(sequence.name);
    hash.add(sequence.flags);
    hash.each(sequence.cmds, [&](const assets::SeqCmd& cmd) {
      hash.add(std::array<u32, 4>{cmd.op, cmd.len, cmd.offset, cmd.wait});
      hash.add(cmd.name);
      hash.add(cmd.data);
    });
  });
  hash.each(fx.blobs, [&](const assets::EffectBlob& blob) {
    hash.add(blob.name);
    hash.add(blob.type);
    hash.add(blob.data);
  });
  hash.add(fx.tims);
  hash.add(fx.meshes);
  hash.add(fx.errors.size());
}

void feed(Hasher& hash, const assets::StageAsset& stage) {
  hash.each(stage.parts, [&](const assets::StagePart& part) {
    hash.add(part.name);
    hash.add(part.entry);
    hash.add(part.ground);
    hash.add(part.source_vertices);
    hash.add(part.source_faces);
    for (const auto& point : part.cull_points) hash.add(point);
    hash.each(part.vertices, [&](const assets::StageVertex& vertex) {
      hash.add(vertex.pos);
      hash.add(vertex.uv);
      hash.add(vertex.color);
    });
    hash.add(part.indices);
    hash.each(part.submeshes, [&](const assets::StageSubMesh& sub) {
      hash.add(std::array<s64, 7>{sub.texture, sub.first_index, sub.index_count, sub.semi, sub.abr, sub.object_texture, sub.direct_page});
    });
  });
  hash.each(stage.textures, [&](const assets::StageTexture& texture) {
    hash.add(std::array<int, 3>{texture.mode, texture.clut_x, texture.clut_y});
    feed(hash, texture.image);
  });
  hash.each(stage.cameras, [&](const assets::StageCamera& camera) {
    hash.add(camera.name);
    hash.add(camera.flags);
    hash.add(camera.duration);
    hash.add(camera.mode);
    hash.each(camera.keys, [&](const assets::StageCameraKey& key) {
      hash.add(key.eye);
      hash.add(key.target);
      hash.add(key.projection);
      hash.add(key.roll_raw);
    });
  });
  hash.each(stage.sequences, [&](const assets::StageSequence& sequence) {
    hash.add(sequence.name);
    hash.each(sequence.commands, [&](const assets::StageSeqCommand& command) {
      hash.add(std::array<u32, 3>{command.offset, command.op, command.length});
      hash.add(command.target);
      hash.add(command.data);
    });
    hash.add(sequence.trailing);
    hash.add(sequence.trailing_is_pad);
  });
  hash.each(stage.lens, [&](const assets::StageLens& lens) {
    hash.add(lens.name);
    hash.add(lens.raw);
  });
  hash.each(stage.emitters, [&](const assets::StageEmitter& emitter) {
    hash.add(emitter.name);
    hash.add(emitter.mesh);
    hash.add(emitter.sprite);
    hash.add(emitter.pos);
    hash.add(emitter.flags);
  });
  hash.add(stage.tim_count);
  hash.add(stage.env_rgba);
}

void feed(Hasher& hash, const assets::RoomScene& room) {
  hash.add(room.camera.matrix);
  hash.add(room.camera.trans);
  hash.add(room.camera.group_scale);
  hash.add(room.clut_rows);
  hash.each(room.layers, [&](const assets::RoomLayer& layer) {
    hash.each(layer.tiles, [&](const assets::RoomTile& tile) {
      hash.add(std::array<s32, 8>{tile.x, tile.y, tile.u, tile.v, tile.clut, tile.group, tile.flags, tile.z});
    });
    hash.each(layer.groups, [&](const assets::RoomGroupImage& group) {
      hash.add(std::array<s64, 6>{group.blend, group.group, group.x, group.y, group.width, group.height});
      hash.add(group.rgba);
    });
  });
  hash.each(room.walk.vertices, [&](const std::array<float, 3>& vertex) { hash.add(vertex); });
  hash.each(room.walk.triangles, [&](const assets::WalkTriangle& triangle) {
    hash.add(triangle.v);
    hash.add(triangle.adj);
    hash.add(triangle.attr);
  });
  hash.add(room.attr_flags);
  hash.each(room.models, [&](const assets::RoomModelHeader& model) {
    hash.add(model.size);
    hash.add(model.mdl);
    feed(hash, model.texture);
  });
  hash.each(room.zones, [&](const assets::RoomZone& zone) {
    for (const auto& corner : zone.corner) hash.add(corner);
  });
  hash.each(room.prd, [&](const assets::RoomPrdEntry& entry) {
    // The size is left out: see check_drp.
    hash.add(entry.name);
    hash.add(entry.type);
  });
  feed(hash, room.effects);
  hash.add(room.ctd_tims);
  hash.add(room.script_bytes);
  hash.add(room.yos1_bytes);
  hash.add(room.yos0_bytes);
  hash.add(room.dialogue_bytes);
}

void feed(Hasher& hash, const audio::Bank& bank) {
  hash.add(bank.id);
  hash.add(bank.first_index);
  hash.add(bank.spu);
  hash.add(bank.samples);
  hash.each(bank.inst, [&](const audio::Instrument& inst) {
    hash.add(std::array<s64, 6>{inst.start, inst.loop, inst.fine_tune, inst.base_note, inst.adsr1, inst.adsr2});
    hash.add(inst.sample != nullptr);
  });
}

void feed(Hasher& hash, const std::vector<std::string>& messages) {
  hash.each(messages, [&](const std::string& message) { hash.add(message); });
}

void feed(Hasher& hash, const ui::ps1::Rgba& image) {
  hash.add(image.w);
  hash.add(image.h);
  hash.add(image.px);
}

// One check's result on one source. A check that does not apply to the file's bytes (not that kind of file) or to the build (a
// text table it does not have) is skipped.
struct Outcome {
  bool ok = false;
  bool skipped = false;
  u64 digest = 0;
  std::string error;
};

Outcome skipped() {
  Outcome outcome;
  outcome.skipped = true;
  return outcome;
}

Outcome done(bool ok, const Hasher& hash, std::string error = {}) {
  Outcome outcome;
  outcome.ok = ok;
  outcome.digest = hash.value();
  outcome.error = std::move(error);
  return outcome;
}

// One source of the cdrom archive and what the checks share on it.
struct Source {
  const vfs::Vfs& fs;
  Bytes particle_lib;
  std::shared_ptr<const audio::InstrumentTable> base_table;

  explicit Source(const vfs::Vfs& files) : fs(files) {
    particle_lib = fs.read("cdrom/effect/particle.prd").value_or(Bytes{});
    base_table = audio::load_akao_base_table(fs);
  }
  std::optional<Bytes> read(const std::string& member) const { return fs.read("cdrom/" + member); }
};

std::string stem_of(std::string_view member) {
  const size_t dot = member.rfind('.');
  return std::string(member.substr(0, dot == std::string_view::npos ? member.size() : dot));
}

std::string file_stem(std::string_view member) {
  const std::string stem = stem_of(member);
  return stem.substr(stem.find_last_of('/') + 1);
}

Outcome check_tim(const Source& source, const std::string& member) {
  const std::optional<Bytes> bytes = source.read(member);
  if (!bytes) return done(false, {}, "missing");
  const std::optional<import::Tim> tim = import::parse_tim(*bytes);
  Hasher hash;
  if (!tim) return done(false, hash, "TIM does not parse");
  feed(hash, *tim);
  const std::optional<import::Image> image = import::decode_tim(*tim);
  if (image) feed(hash, *image);
  return done(image.has_value(), hash, image ? "" : "TIM does not decode");
}

Outcome check_sszl(const Source& source, const std::string& member) {
  const std::optional<Bytes> bytes = source.read(member);
  if (!bytes || !import::is_lzss(*bytes)) return skipped();
  const std::optional<Bytes> decoded = import::lzss_decode(*bytes);
  Hasher hash;
  if (decoded) hash.add(*decoded);
  return done(decoded.has_value(), hash, decoded ? "" : "sszl does not decode");
}

Outcome check_drp(const Source& source, const std::string& member) {
  const std::optional<Bytes> bytes = source.read(member);
  if (!bytes || !import::is_drp(*bytes)) return skipped();
  const std::optional<import::Drp> drp = import::parse_drp(*bytes);
  Hasher hash;
  if (!drp) return done(false, hash, "drp does not parse");
  // The last entry's declared length can run up to a few zero bytes past the end of the cdrom.dat member; the disc file holds those
  // zeros, so an entry's trailing zero bytes are not hashed.
  hash.each(drp->entries, [&](const import::DrpEntry& entry) {
    hash.add(entry.name);
    hash.add(entry.type);
    hash.add(entry.offset);
    size_t length = entry.data.size();
    while (length > 0 && entry.data[length - 1] == 0) --length;
    hash.bytes(entry.data.data(), length);
  });
  return done(true, hash);
}

Outcome check_room(const Source& source, const std::string& member) {
  const std::string stem = stem_of(member);
  const std::optional<Bytes> bin = source.read(member);
  const std::optional<Bytes> ctd = source.read(stem + ".ctd");
  if (!bin || !ctd) return skipped();
  const Bytes prd = source.read(stem + ".prd").value_or(Bytes{});
  std::string error;
  const std::optional<assets::RoomScene> room = import::load_room(file_stem(member), *bin, *ctd, prd, &error);
  Hasher hash;
  if (room) feed(hash, *room);
  return done(room.has_value(), hash, room ? "" : "load_room: " + error);
}

Outcome check_room_validation(const Source& source, const std::string& member) {
  const std::optional<Bytes> bin = source.read(member);
  const std::optional<Bytes> ctd = source.read(stem_of(member) + ".ctd");
  if (!bin || !ctd) return skipped();
  const import::RoomValidation validation = import::validate_room(*bin, *ctd);
  Hasher hash;
  feed(hash, validation.errors);
  feed(hash, validation.warnings);
  return done(validation.ok(), hash, validation.ok() ? "" : validation.errors.front());
}

Outcome check_room_models(const Source& source, const std::string& member) {
  const std::string stem = stem_of(member);
  const std::optional<Bytes> bin = source.read(member);
  const std::optional<Bytes> ctd = source.read(stem + ".ctd");
  if (!bin || !ctd) return skipped();
  std::string error;
  const auto headers = import::load_room_models(*bin, *ctd, &error);
  Hasher hash;
  if (!headers) return done(false, hash, "load_room_models: " + error);
  for (const assets::RoomModelHeader& header : *headers) {
    std::string model_error;
    const auto model = import::load_room_model(header.mdl, header.page_textures, &model_error);
    hash.add(model.has_value());
    if (model) feed(hash, *model);
    else error = model_error;
  }
  return done(error.empty(), hash, error.empty() ? "" : "load_room_model: " + error);
}

Outcome check_room_sound(const Source& source, const std::string& member) {
  const std::string stem = stem_of(member);
  const std::optional<Bytes> bin = source.read(member);
  const std::optional<Bytes> ctd = source.read(stem + ".ctd");
  if (!bin || !ctd) return skipped();
  const std::optional<assets::RoomScene> room = import::load_room(file_stem(member), *bin, *ctd);
  if (!room) return skipped();
  audio::SfxBank bank;
  Hasher hash;
  if (!bank.load(source.fs)) return done(false, hash, "effect00 / wave0000 do not load");
  bank.set_room(room->yos0_bytes, room->yos1_bytes);
  for (int frame = 0; frame < 256; ++frame) {
    audio::SfxProgram program;
    hash.add(bank.make_room(frame, program));
  }
  return done(true, hash);
}

Outcome check_room_effects(const Source& source, const std::string& member) {
  const std::optional<Bytes> prd = source.read(member);
  if (!prd || !import::is_drp(*prd)) return skipped();
  const assets::RoomEffects fx = import::load_room_effects(*prd, import::Vram{}, source.particle_lib);
  Hasher hash;
  feed(hash, fx);
  return done(fx.errors.empty(), hash, fx.errors.empty() ? "" : fx.errors.front());
}

Outcome check_model(const Source& source, const std::string& member) {
  const std::string stem = stem_of(member);
  std::string error;
  const std::optional<assets::ModelAsset> model = game::read_model_files(source.fs, stem, &error);
  Hasher hash;
  if (model) feed(hash, *model);
  if (!model) return done(false, hash, "read_model_files: " + error);
  if (stem.starts_with("battle/")) {
    const std::optional<assets::ModelAsset> battle = game::ModelCache::read_model(source.fs, stem, &error);
    if (battle) feed(hash, *battle);
    if (!battle) return done(false, hash, "battle read_model: " + error);
  }
  return done(true, hash);
}

Outcome check_model_validation(const Source& source, const std::string& member) {
  const std::string stem = stem_of(member);
  const bool field = member.starts_with("fieldobj/");
  const std::optional<Bytes> obj = source.read(member);
  if (!obj) return done(false, {}, "missing");
  Bytes texture, anims;
  if (field) {
    texture = source.read(stem + ".tim").value_or(Bytes{});
    anims = source.read("fieldobj/ex/" + file_stem(member) + ".bin").value_or(Bytes{});
  } else {
    texture = source.read(stem + ".ltc").value_or(source.read(stem + ".ltd").value_or(Bytes{}));
    anims = source.read(stem.substr(0, stem.find_last_of('/') + 1) + "at0.prd").value_or(Bytes{});
  }
  const import::ModelValidation validation = import::validate_model(*obj, texture, anims, field);
  Hasher hash;
  feed(hash, validation.errors);
  hash.add(std::array<u32, 5>{validation.joints, validation.constructs, validation.vertices, validation.faces, validation.clips});
  return done(validation.ok(), hash, validation.ok() ? "" : validation.errors.front());
}

Outcome check_field_ex(const Source& source, const std::string& member) {
  const std::optional<Bytes> bin = source.read(member);
  if (!bin) return done(false, {}, "missing");
  const ByteSpan pack = import::ex_pack(*bin);
  if (pack.empty()) return skipped();
  std::string error;
  const auto info = import::inspect_anim_pack(pack, &error);
  Hasher hash;
  feed(hash, import::ex_anim_seq(*bin));
  if (info) hash.add(std::array<u32, 3>{info->clips, info->joints, info->frames});
  return done(info.has_value(), hash, info ? "" : "anim pack: " + error);
}

Outcome check_weapon(const Source& source, const std::string& member) {
  const std::optional<Bytes> bytes = source.read(member);
  if (!bytes) return done(false, {}, "missing");
  std::string error;
  const std::optional<assets::WeaponPack> pack = import::load_weapon_pack(*bytes, &error);
  Hasher hash;
  if (!pack) return done(false, hash, "load_weapon_pack: " + error);
  hash.each(pack->parts, [&](const assets::WeaponPart& part) {
    feed(hash, part.texture);
    feed(hash, part.mesh);
  });
  for (size_t part = 0; part < pack->parts.size(); ++part) {
    const std::optional<assets::ModelAsset> model = game::ModelCache::read_weapon(source.fs, member, static_cast<int>(part));
    hash.add(model.has_value());
    if (model) feed(hash, *model);
  }
  return done(true, hash);
}

Outcome check_stage(const Source& source, const std::string& member) {
  const std::optional<Bytes> prd = source.read(member);
  if (!prd || !import::is_drp(*prd)) return skipped();
  const Bytes ctd = source.read(stem_of(member) + ".ctd").value_or(Bytes{});
  std::string error;
  const std::optional<assets::StageAsset> stage = import::load_stage(*prd, ctd, &error);
  Hasher hash;
  if (stage) feed(hash, *stage);
  return done(stage.has_value(), hash, stage ? "" : "load_stage: " + error);
}

Outcome check_stage_validation(const Source& source, const std::string& member) {
  const std::optional<Bytes> prd = source.read(member);
  if (!prd || !import::is_drp(*prd)) return skipped();
  const import::StageValidation validation = import::validate_stage(*prd, source.read(stem_of(member) + ".ctd").value_or(Bytes{}));
  Hasher hash;
  feed(hash, validation.errors);
  return done(validation.ok(), hash, validation.ok() ? "" : validation.errors.front());
}

Outcome check_effect_pack(const Source& source, const std::string& member) {
  const std::optional<Bytes> prd = source.read(member);
  if (!prd || !import::is_drp(*prd)) return skipped();
  const Bytes ctd = source.read(stem_of(member) + ".ctd").value_or(Bytes{});
  const assets::RoomEffects fx = import::load_battle_effects(*prd, ctd, import::Vram{}, source.particle_lib);
  Hasher hash;
  feed(hash, fx);
  std::string error;
  const std::optional<assets::StageAsset> meshes = import::load_stage(*prd, ctd, &error);
  hash.add(meshes.has_value());
  if (meshes) feed(hash, *meshes);
  return done(fx.errors.empty(), hash, fx.errors.empty() ? "" : fx.errors.front());
}

Outcome check_pac(const Source& source, const std::string& member) {
  std::optional<Bytes> bytes = source.read(member);
  if (!bytes) return done(false, {}, "missing");
  battle::pac::Module module;
  std::string error;
  const bool ok = module.parse(std::move(*bytes), &error);
  Hasher hash;
  if (ok) {
    hash.add(module.entities);
    hash.add(module.base);
    hash.add(module.end);
    hash.each(module.scripts(), [&](const battle::pac::ScriptExtent& script) {
      hash.add(std::array<u32, 4>{static_cast<u32>(script.entity), static_cast<u32>(script.slot), script.start, script.end});
    });
    for (size_t at = 0; at < module.code_size(); ++at) hash.add(module.at(static_cast<u32>(at)));
  }
  return done(ok, hash, error);
}

Outcome check_music(const Source& source, const std::string& member) {
  const std::string name = file_stem(member);
  if (!name.starts_with("music")) return skipped();
  const int number = std::atoi(name.c_str() + 5);
  const std::optional<audio::AkaoMusic> music = audio::load_akao_music(source.fs, number, source.base_table);
  Hasher hash;
  if (!music) return done(false, hash, "load_akao_music failed");
  hash.add(music->seq->id);
  hash.add(music->seq->bank_id);
  hash.add(music->seq->channel_mask);
  hash.each(music->seq->channels, [&](const std::pair<int, u32>& channel) {
    hash.add(channel.first);
    hash.add(channel.second);
  });
  return done(true, hash);
}

Outcome check_wave(const Source& source, const std::string& member) {
  const std::optional<Bytes> bytes = source.read(member);
  if (!bytes || bytes->size() < 4 || std::memcmp(bytes->data(), "AKAO", 4) != 0) return skipped();
  const std::shared_ptr<const audio::Bank> bank = audio::parse_bank(*bytes);
  Hasher hash;
  if (bank) feed(hash, *bank);
  return done(bank != nullptr, hash, bank ? "" : "parse_bank failed");
}

Outcome check_menu_model(const Source& source, const std::string& member) {
  const std::optional<Bytes> bytes = source.read(member);
  if (!bytes) return done(false, {}, "missing");
  std::string error;
  const std::optional<assets::ModelAsset> model = import::load_menu_model(*bytes, {}, &error);
  Hasher hash;
  if (model) feed(hash, *model);
  return done(model.has_value(), hash, model ? "" : "load_menu_model: " + error);
}

Outcome check_tables(const Source& source, const std::string&) {
  data::GameTables tables;
  std::string error;
  const bool ok = tables.load(source.fs, &error);
  Hasher hash;
  if (ok) hash.add(data::battle_data_fingerprint(tables));
  return done(ok, hash, error);
}

Outcome check_menu_tables(const Source& source, const std::string&) {
  ui::MenuRules rules;
  ui::shop::ShopData shop;
  const bool pg = rules.load_pg(source.fs);
  const bool waa = shop.load_waa_shop(source.fs);
  Hasher hash;
  hash.add(pg);
  hash.add(waa);
  return done(pg && waa, hash, pg ? (waa ? "" : "waa_shop") : "menu_pg");
}

Outcome check_sfx(const Source& source, const std::string&) {
  audio::SfxBank bank;
  Hasher hash;
  if (!bank.load(source.fs)) return done(false, hash, "effect00 / wave0000 do not load");
  hash.add(bank.defined_ids());
  for (int id = 0; id < audio::SfxBank::kGlobalCount; ++id) {
    audio::SfxProgram program;
    hash.add(bank.make_global(id, program));
  }
  return done(source.base_table != nullptr, hash, source.base_table ? "" : "wave0000 table");
}

// The text tables Ps1Text builds from the disc's files, one check per table (the table name is the member argument's stand-in).
Outcome check_text_table(const Source& source, const std::string& name) {
  import::Ps1Text text;
  Hasher hash;
  if (!text.open(source.fs)) return done(false, hash, "Ps1Text::open failed");
  const std::optional<import::TextTable> table = text.table(name);
  if (!table) return skipped();
  hash.add(table->rows());
  for (size_t row = 0; row < table->rows(); ++row) feed(hash, table->row(row));
  return done(true, hash);
}

Outcome check_text_rooms(const Source& source, const std::string&) {
  import::Ps1Text text;
  Hasher hash;
  if (!text.open(source.fs)) return done(false, hash, "Ps1Text::open failed");
  size_t messages = 0;
  for (int room = 0; room < 1024; ++room)
    for (int msg = 0; msg < 2048; ++msg) {
      const std::string* line = text.field(room, msg);
      if (!line) break;
      hash.add(*line);
      ++messages;
    }
  return done(messages > 0, hash, messages > 0 ? "" : "no room messages");
}

Outcome check_text_battles(const Source& source, const std::string&) {
  import::Ps1Text text;
  Hasher hash;
  if (!text.open(source.fs)) return done(false, hash, "Ps1Text::open failed");
  size_t messages = 0;
  for (u32 battle_id = 0; battle_id < 1024; ++battle_id)
    for (u32 msg = 0; msg < 256; ++msg) {
      const std::string* line = text.battle_event(battle_id, msg);
      if (!line) break;
      hash.add(*line);
      ++messages;
    }
  return done(messages > 0, hash, messages > 0 ? "" : "no battle messages");
}

Outcome check_font(const Source& source, const std::string&) {
  const std::shared_ptr<const import::Ps1Font> font = import::load_ps1_font(source.fs);
  Hasher hash;
  hash.add(font != nullptr);
  return done(font != nullptr, hash, font ? "" : "load_ps1_font failed");
}

Outcome check_disc_index(const Source& source, const std::string&) {
  game::DiscIndex index;
  const bool ok = index.load(source.fs);
  Hasher hash;
  hash.add(index.size(1));
  hash.add(index.size(2));
  return done(ok, hash, ok ? "" : "DiscIndex::load failed");
}

Outcome check_title(const Source& source, const std::string&) {
  const std::optional<assets::StageAsset> stage = game::load_title_stage(source.fs);
  Hasher hash;
  if (stage) feed(hash, *stage);
  return done(stage.has_value(), hash, stage ? "" : "load_title_stage failed");
}

Outcome check_menu_art(const Source& source, const std::string&) {
  namespace art = ui::ps1;
  Hasher hash;
  feed(hash, art::parchment(source.fs));
  for (const char* entry : {"wind", "suji", "icon", "mpla"}) feed(hash, art::menutim(source.fs, entry));
  for (int frame = 1; frame <= 64; ++frame) feed(hash, art::sheet_frame(source.fs, frame));
  for (int id = 0; id < 50; ++id) {
    feed(hash, art::face(source.fs, id));
    feed(hash, art::portrait(source.fs, id));
  }
  for (int icon = 0; icon < 15; ++icon) feed(hash, art::slot_icon(source.fs, icon));
  for (int type : {8, 9, 10, 11, 12, 13, 38})
    for (int variant = 1; variant <= 4; ++variant) feed(hash, art::plate(source.fs, type, variant));
  for (int nn = 1; nn <= 64; ++nn) feed(hash, art::keyitem(source.fs, nn));
  for (int skin = 1; skin <= 17; ++skin) feed(hash, ui::ps1win::skin(source.fs, skin));
  const bool ok = static_cast<bool>(art::parchment(source.fs)) && static_cast<bool>(art::menutim(source.fs, "wind")) &&
                  static_cast<bool>(art::face(source.fs, 0)) && static_cast<bool>(ui::ps1win::skin(source.fs, 1));
  return done(ok, hash, ok ? "" : "menu art missing");
}

Outcome check_battle_fx(const Source& source, const std::string&) {
  game::BattleFxLibrary library;
  Hasher hash;
  if (!library.init(source.fs, false)) return done(false, hash, "BattleFxLibrary::init failed");
  feed(hash, library.particle_lib());
  size_t packs = 0, failed = 0;
  std::string error;
  for (size_t eid = 0; eid < library.slot_count(); ++eid) {
    const std::shared_ptr<const game::FxPack> pack = library.get(static_cast<int>(eid));
    hash.add(pack != nullptr);
    if (!pack) continue;
    ++packs;
    feed(hash, pack->fx);
    hash.add(pack->meshes != nullptr);
    if (pack->meshes) feed(hash, *pack->meshes);
    for (const auto& [name, model] : game::fx_pack_models(*pack)) {
      hash.add(name);
      feed(hash, model);
    }
    if (!pack->fx.errors.empty()) {
      ++failed;
      if (error.empty()) error = "effect " + std::to_string(eid) + ": " + pack->fx.errors.front();
    }
    library.clear_cache();
  }
  for (int colour = 0; colour < 6; ++colour) {
    const std::shared_ptr<const game::FxPack> pack = library.charge_pack(colour);
    hash.add(pack != nullptr);
    if (pack) feed(hash, pack->fx);
  }
  hash.add(packs);
  return done(failed == 0 && packs > 0, hash, error);
}

using Run = Outcome (*)(const Source&, const std::string&);

struct Check {
  const char* name;
  // Null for a whole-game check (run once).
  bool (*applies)(std::string_view member);
  Run run;
  // The format checks of a validator rather than an import: on a build other than Japan's, whose data differs, a failure is listed
  // as a note, not a problem.
  bool validation = false;
};

bool under(std::string_view member, std::string_view prefix, std::string_view suffix) {
  return member.starts_with(prefix) && member.ends_with(suffix);
}

// A character model: field players, battle players, battle monsters.
bool is_model(std::string_view member) {
  return member.ends_with(".obj") &&
         (member.starts_with("fieldobj/player/") || member.starts_with("battle/player/") || member.starts_with("battle/monster/"));
}

const Check kChecks[] = {
    {"tim", [](std::string_view member) { return member.ends_with(".tim"); }, check_tim},
    {"sszl", [](std::string_view) { return true; }, check_sszl},
    {"drp", [](std::string_view) { return true; }, check_drp},
    {"room", [](std::string_view member) { return under(member, "map/mapbin/", ".bin"); }, check_room},
    {"room-check", [](std::string_view member) { return under(member, "map/mapbin/", ".bin"); }, check_room_validation, true},
    {"room-models", [](std::string_view member) { return under(member, "map/mapbin/", ".bin"); }, check_room_models},
    {"room-sound", [](std::string_view member) { return under(member, "map/mapbin/", ".bin"); }, check_room_sound},
    {"room-prd", [](std::string_view member) { return under(member, "map/mapbin/", ".prd"); }, check_room_effects},
    {"model", is_model, check_model},
    {"model-check", is_model, check_model_validation, true},
    {"field-ex", [](std::string_view member) { return under(member, "fieldobj/ex/", ".bin"); }, check_field_ex},
    {"weapon", [](std::string_view member) { return under(member, "battle/wepon/", ".bin"); }, check_weapon},
    {"stage", [](std::string_view member) { return under(member, "battle/bg/", ".prd"); }, check_stage},
    {"stage-check", [](std::string_view member) { return under(member, "battle/bg/", ".prd"); }, check_stage_validation, true},
    {"effect", [](std::string_view member) { return under(member, "battle/effects/", ".prd"); }, check_effect_pack},
    {"pac", [](std::string_view member) { return under(member, "battle/event/", ".pac"); }, check_pac},
    {"music", [](std::string_view member) { return under(member, "sound/music/", ".dat"); }, check_music},
    {"wave", [](std::string_view member) {
       return under(member, "sound/wave/", ".dat") || under(member, "sound/exwave/", ".dat") || under(member, "battle/effects/syoukan/wave", ".dat");
     },
     check_wave},
    {"menu-model", [](std::string_view member) { return under(member, "menu/player/", ".ome"); }, check_menu_model},
    {"tables", nullptr, check_tables},
    {"menu-tables", nullptr, check_menu_tables},
    {"sfx", nullptr, check_sfx},
    {"text-tables", nullptr, check_text_table},
    {"text-rooms", nullptr, check_text_rooms},
    {"text-battles", nullptr, check_text_battles},
    {"font", nullptr, check_font},
    {"disc-index", nullptr, check_disc_index},
    {"title", nullptr, check_title},
    {"menu-art", nullptr, check_menu_art},
    {"battle-fx", nullptr, check_battle_fx},
};

Outcome guarded(const Check& check, const Source& source, const std::string& member) {
  try {
    return check.run(source, member);
  } catch (const std::exception& error) {
    return done(false, {}, std::string("exception: ") + error.what());
  }
}

// The two file indexes. The discs do not hold them as files; the engine carries a copy (the built-in disc names), but the battle effect
// library reads them as files, so both sources serve cdrom.dat's.
constexpr const char* kIndexFiles[] = {"cdrom.fnd", "cdrom2.fnd"};

// The cdrom archive of cdrom.dat alone (no cdrom2 override, no other archive), limited to the members the disc holds and the indexes, so
// both sources serve the same set of files.
class CdromArchive final : public vfs::Backend {
 public:
  CdromArchive(const std::filesystem::path& file, const std::vector<std::string>& members)
      : archive_(file), members_(members.begin(), members.end()) {
    members_.insert(std::begin(kIndexFiles), std::end(kIndexFiles));
  }
  bool ok() const { return archive_.ok(); }
  bool has_archive(std::string_view archive) const override { return archive == "cdrom"; }
  bool contains(std::string_view archive, const std::string& member, int) const override {
    return archive == "cdrom" && members_.contains(member) && archive_.contains(member);
  }
  std::optional<Bytes> read(std::string_view archive, const std::string& member, int disc) const override {
    return contains(archive, member, disc) ? archive_.read(member) : std::nullopt;
  }
  std::vector<std::string> list(std::string_view archive) const override {
    std::vector<std::string> out;
    if (archive == "cdrom")
      for (const std::string& member : members_)
        if (archive_.contains(member)) out.push_back(member);
    return out;
  }

 private:
  vfs::Archive archive_;
  std::set<std::string> members_;
};

// The disc with the indexes of `archive` added.
class DiscWithIndexes final : public vfs::Backend {
 public:
  DiscWithIndexes(std::unique_ptr<vfs::Backend> disc, const CdromArchive& archive) : disc_(std::move(disc)) {
    for (const char* name : kIndexFiles)
      if (std::optional<Bytes> bytes = archive.read("cdrom", name, 1)) indexes_.emplace(name, std::move(*bytes));
  }
  bool has_archive(std::string_view archive) const override { return disc_->has_archive(archive); }
  bool contains(std::string_view archive, const std::string& member, int disc) const override {
    return (archive == "cdrom" && indexes_.contains(member)) || disc_->contains(archive, member, disc);
  }
  std::optional<Bytes> read(std::string_view archive, const std::string& member, int disc) const override {
    const auto index = archive == "cdrom" ? indexes_.find(member) : indexes_.end();
    return index != indexes_.end() ? std::optional<Bytes>(index->second) : disc_->read(archive, member, disc);
  }
  std::vector<std::string> list(std::string_view archive) const override { return disc_->list(archive); }

 private:
  std::unique_ptr<vfs::Backend> disc_;
  std::map<std::string, Bytes> indexes_;
};

struct Tally {
  size_t files = 0, exact_failed = 0, disc_failed = 0;
  std::vector<std::string> problems, notes;
};

}  // namespace

int ocdisc_audit(const std::string& image, const std::string& against, size_t disc_index, const std::string& only) {
  std::string error;
  std::shared_ptr<const import::disc::SectorSource> disc_source = import::disc::open_image(std::filesystem::path(image), &error, disc_index - 1);
  if (!disc_source) {
    std::fprintf(stderr, "ocdisc: %s\n", error.c_str());
    return 1;
  }
  const import::disc::DiscIdentity identity = import::disc::identify(*disc_source);
  if (!identity.chrono_cross) {
    std::fprintf(stderr, "ocdisc: not a Chrono Cross disc: %s\n", identity.reason.c_str());
    return 1;
  }
  const bool japan = identity.region == import::disc::Region::Japan;
  const int number = identity.disc_number >= 2 ? 2 : 1;
  auto backend = std::make_unique<import::disc::DiscBackend>();
  if (!backend->mount(number, disc_source, &error)) {
    std::fprintf(stderr, "ocdisc: %s\n", error.c_str());
    return 1;
  }
  const std::vector<std::string> members = backend->list("cdrom");
  auto archive = std::make_unique<CdromArchive>(std::filesystem::path(against), members);
  if (!archive->ok()) {
    std::fprintf(stderr, "ocdisc: cannot open %s\n", against.c_str());
    return 1;
  }
  vfs::Vfs disc_fs;
  disc_fs.open(std::make_unique<DiscWithIndexes>(std::move(backend), *archive), std::filesystem::path(image).parent_path());
  disc_fs.set_disc(number);
  vfs::Vfs exact_fs;
  exact_fs.open(std::move(archive), std::filesystem::path(against).parent_path());
  exact_fs.set_disc(number);

  const Source exact(exact_fs), disc(disc_fs);
  std::printf("%s: disc %d, build %s; against %s (%s)\n", image.c_str(), number, identity.build.c_str(), against.c_str(),
              japan ? "decoded results must match" : "must import wherever the member does");

  std::map<std::string, Tally> tallies;
  std::mutex mutex;
  // A disc file is judged against the member: same outcome and digest (Japan), or importing wherever the member imports.
  const auto judge = [&](const Check& check, const std::string& member, const Outcome& on_exact, const Outcome& on_disc) {
    if (on_exact.skipped && on_disc.skipped) return;
    const std::lock_guard lock(mutex);
    Tally& tally = tallies[check.name];
    ++tally.files;
    const bool exact_ok = on_exact.ok && !on_exact.skipped;
    const bool disc_ok = on_disc.ok && !on_disc.skipped;
    if (!exact_ok && !on_exact.skipped) ++tally.exact_failed;
    if (!disc_ok && !on_disc.skipped) ++tally.disc_failed;
    std::string problem;
    if (japan && (exact_ok != disc_ok || on_exact.skipped != on_disc.skipped))
      problem = exact_ok ? "fails on the disc: " + on_disc.error : "imports only on the disc";
    else if (japan && on_exact.digest != on_disc.digest)
      problem = "decoded result differs";
    else if (!japan && exact_ok && !disc_ok && !on_disc.skipped)
      problem = "fails on the disc: " + on_disc.error;
    if (problem.empty()) return;
    const std::string line = (member.empty() ? std::string("(game)") : member) + ": " + problem;
    (!japan && check.validation ? tally.notes : tally.problems).push_back(line);
  };

  std::vector<std::pair<const Check*, std::string>> jobs;
  // `only`: a comma-separated list of check names (empty = every check).
  const auto selected = [&](const Check& check) {
    return only.empty() || ("," + only + ",").find("," + std::string(check.name) + ",") != std::string::npos;
  };
  for (const Check& check : kChecks) {
    if (!selected(check)) continue;
    if (check.run == check_text_table) {
      for (const char* name : {"sysmes", "btmes", "keyitem", "waa_spec", "menutest", "shopmenu", "savetbl", "ome"}) jobs.emplace_back(&check, name);
      continue;
    }
    if (!check.applies) {
      jobs.emplace_back(&check, std::string());
      continue;
    }
    for (const std::string& member : members)
      if (check.applies(member) && exact_fs.exists("cdrom/" + member)) jobs.emplace_back(&check, member);
  }
  parallel_for(jobs.size(), [&](size_t i) {
    const auto& [check, member] = jobs[i];
    judge(*check, member, guarded(*check, exact, member), guarded(*check, disc, member));
  });

  std::printf("%-12s %7s %13s %12s %9s %6s\n", "check", "files", "member fails", "disc fails", "problems", "notes");
  size_t problems = 0;
  for (const Check& check : kChecks) {
    const Tally& tally = tallies[check.name];
    std::printf("%-12s %7zu %13zu %12zu %9zu %6zu\n", check.name, tally.files, tally.exact_failed, tally.disc_failed, tally.problems.size(),
                tally.notes.size());
    problems += tally.problems.size();
  }
  for (const Check& check : kChecks)
    for (const auto& [kind, lines] : {std::pair<const char*, const std::vector<std::string>*>{"", &tallies[check.name].problems},
                                      {"note: ", &tallies[check.name].notes}}) {
      std::vector<std::string> list = *lines;
      std::ranges::sort(list);
      for (const std::string& line : list) std::printf("%s%s: %s\n", kind, check.name, line.c_str());
    }
  std::printf("%zu problem(s)\n", problems);
  return problems == 0 ? 0 : 1;
}
