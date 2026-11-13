// occ - OpenCross asset tool.
//   occ list <archive>                       list members of data/<archive>.dat
//   occ cat <path> <out>                     extract one member (e.g. cdrom/map/mapbin/alu_a01f.bin)
//   occ info <path>                          identify container type and print the subfile table
//   occ lzss <path> <out>                    decode an "sszl" member
//   occ room <name> <outdir>                 decode a room, write one PNG per layer/group, print camera/walk-mesh stats
//   occ rooms-validate                       validate every room that has a .bin and a .ctd
//   occ rooms-check                          initial-frame viewport coverage of every room (flags < 90 %)
//   occ rooms-frame                          empty rows under the picture per display height (216 / 224 / 240) over every reachable scroll
//   occ chip2 <room> <outdir>                write the RDE HD (4x) group images of a room
//   occ chip2-validate                       parse every hd chip2, report tiles without an HD tile
//   occ model <path-stem> <outdir>           import a model (e.g. battle/player/selju/selju); OBJ + skeleton/anims/mesh JSON + PNGs
//   occ script-disasm <room> [entity]        field script listing (same format as tools/py/cc_fieldscript.py disasm)
//   occ dialogue-choices [room] [-v]        every E1 MULTI_CHOICE of every room x language: headless box layout, cursor stepped through the choices
//                                            (DialogueBox::choice_cursor rect asserts), classes + counts
//   occ dialogue-sizes [room] [-v]          blast radius of the no-3-line-minimum window rule: messages whose frame changes height (count + examples)
//   occ script-soak                          run every room's script (INIT + 600 ticks, NullHost) and report problems
//   occ models-validate                      validate every model class (players, monsters, field, at0, weapons, kmd, ex)
//   occ room-songs [room]                    every PLAY_SONG of the room scripts with the song id and its BGM track
//   occ room-models [room]                   import every room model (subfiles 11+) of all rooms / one room, report failures
//   occ menu-rigs [--hd] [byte]              how each menu model's skeleton matches its character's field model ([Enhancements] MenuModelsInField);
//                                            with a model byte (hex), every joint of that pair; --audit: drift over every field clip and
//                                            mesh-region checks; --rooms: room copies of character field models
//   occ battlefields [--inc <file>]          which rooms fight on which stage (every BATTLE op of every room script -> bg_NN); --inc writes
//                                            src/data/battlefield_rooms.inc (the Debug tab's battlefield labels)
//   occ world-markers [--inc <file>]         the overworld screens' location markers: label text (EN) -> target room, and the room-prefix ->
//                                            place-name table it gives; --inc writes src/data/area_names.inc
//   occ dump-textures [stages|title|effects|rooms|models|weapons|all] [--raw] <outdir>   headless texture dump (tools/occ_dump_textures.cpp)
//   occ save-info | save-slots | save-roundtrip | save-blank <save.dat> ...   save-file tools, no GameDir needed (tools/occ_save.cpp)
// GameDir comes from -g <dir> or the OPENCROSS_GAMEDIR environment variable.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include "audio/bgm_table.h"
#include "data/battlefields.h"
#include "data/tables.h"
#include "game/battle/battle_pose.h"
#include "game/battle/model_table.h"
#include "game/disc_index.h"
#include "game/menu_rig.h"
#include "game/model_files.h"
#include "game/room_models.h"
#include "core/log.h"
#include "import/chip2.h"
#include "import/container.h"
#include "import/lzss.h"
#include "import/model.h"
#include "import/room.h"
#include "import/spritefont.h"
#include "import/text.h"
#include "script/field_script.h"
#include "script/field_vm.h"
#include "ui/dialogue_box.h"
#include "ui/typewriter.h"
#include "vfs/vfs.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cctype>
#include <iterator>
#include <map>
#include <optional>
#include <set>

// Defined in tools/occ_save.cpp.
int occ_save_main(const std::vector<std::string>& args);

using namespace oc;

// Defined in tools/occ_dump_textures.cpp.
int occ_dump_textures(vfs::Vfs& fs, const std::vector<std::string>& args);

namespace {

// ---------------------------------------------------------------- rooms
std::string room_path(const std::string& name, const char* ext) { return "cdrom/map/mapbin/" + name + ext; }

std::vector<std::string> list_rooms(const vfs::Vfs& fs) {
  std::vector<std::string> bins, ctds;
  for (std::string member : fs.list("cdrom")) {
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
  return out;
}

int usage() {
  std::fprintf(stderr,
               "usage: occ [-g GameDir] (\n"
               "  list <archive>\n"
               "  | cat <path> <out>\n"
               "  | info <path>\n"
               "  | lzss <path> <out>\n"
               "  | dump-textures [stages|title|effects|rooms|models|weapons|all] [--raw] <outdir>\n"
               "  | room <name> <outdir>\n"
               "  | rooms-validate\n"
               "  | rooms-check\n"
               "  | battlefields [--inc <file>]\n"
               "  | world-markers [--inc <file>]\n"
               "  | rooms-frame\n"
               "  | chip2 <room> <outdir>\n"
               "  | chip2-validate\n"
               "  | script-disasm <room> [entity]\n"
               "  | dialogue-choices [room] [-v]\n"
               "  | dialogue-sizes [room] [-v]\n"
               "  | script-soak\n"
               "  | model <path-stem> <outdir>\n"
               "  | models-validate\n"
               "  | room-songs [room]\n"
               "  | room-models [room]\n"
               "  | menu-rigs [--hd] [--audit | --rooms | --npcs] [byte]\n"
               "  | save-info <save.dat>\n"
               "  | save-slots <save.dat>\n"
               "  | save-roundtrip <save.dat> <out.dat>\n"
               "  | save-blank <save.dat> <out.dat>)\n");
  return 2;
}

bool write_file(const std::string& path, ByteSpan data) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(file);
}

void print_container(ByteSpan data, int depth) {
  std::string pad(static_cast<size_t>(depth) * 2, ' ');
  if (import::is_cdmake_dummy(data)) {
    std::printf("%sCDMAKE dummy (%zu bytes)\n", pad.c_str(), data.size());
    return;
  }
  if (import::is_lzss(data)) {
    std::printf("%slzss (%zu bytes compressed)\n", pad.c_str(), data.size());
    return;
  }
  if (auto drp = import::parse_drp(data)) {
    std::printf("%sdrp: %zu entries\n", pad.c_str(), drp->entries.size());
    for (size_t i = 0; i < drp->entries.size(); ++i) {
      const auto& entry = drp->entries[i];
      std::printf("%s  [%zu] '%s' type 0x%02X @0x%X %zu bytes\n", pad.c_str(), i, entry.name.c_str(), entry.type, entry.offset,
                  entry.data.size());
      if (entry.type == 0x01 && depth < 4) print_container(entry.data, depth + 2);
    }
    return;
  }
  if (auto cpt = import::parse_cpt(data)) {
    std::printf("%scpt: %zu entries%s\n", pad.c_str(), cpt->entries.size(), cpt->has_eof_pointer ? " (+eof)" : "");
    for (size_t i = 0; i < cpt->entries.size(); ++i) {
      const auto& entry = cpt->entries[i];
      std::printf("%s  [%zu] @0x%X %u bytes", pad.c_str(), i, entry.offset, entry.size);
      if (entry.size >= 4) std::printf("  %02X %02X %02X %02X", entry.data[0], entry.data[1], entry.data[2], entry.data[3]);
      std::printf("\n");
    }
    return;
  }
  std::printf("%sunknown (%zu bytes)\n", pad.c_str(), data.size());
}

bool write_png(const std::string& path, u32 w, u32 h, const Bytes& rgba) {
  bool ok = false;
  auto sink = [](void* ctx, void* data, int size) {
    auto* stream = static_cast<std::ofstream*>(ctx);
    stream->write(static_cast<const char*>(data), size);
  };
  std::ofstream file(path, std::ios::binary);
  if (!file) return false;
  ok = stbi_write_png_to_func(sink, &file, static_cast<int>(w), static_cast<int>(h), 4, rgba.data(), static_cast<int>(w * 4)) != 0;
  return ok && static_cast<bool>(file);
}

int cmd_room(const vfs::Vfs& fs, const std::string& name, const std::string& outdir) {
  auto bin = fs.read(room_path(name, ".bin"));
  auto ctd = fs.read(room_path(name, ".ctd"));
  if (!bin || !ctd) {
    std::fprintf(stderr, "room %s: .bin/.ctd not found\n", name.c_str());
    return 1;
  }
  auto prd = fs.read(room_path(name, ".prd"));
  std::string err;
  auto room = import::load_room(name, *bin, *ctd, prd ? ByteSpan(*prd) : ByteSpan{}, &err);
  if (!room) {
    std::fprintf(stderr, "room %s: %s\n", name.c_str(), err.c_str());
    return 1;
  }
  std::filesystem::create_directories(outdir);
  const auto& camera = room->camera;
  std::printf("room %s: %zu layers, %zu group images, %u ctd TIMs, %u CLUT rows, %zu models, %zu prd entries\n",
              name.c_str(), room->layers.size(), room->group_image_count(), room->ctd_tims, room->clut_rows,
              room->models.size(), room->prd.size());
  std::printf("camera: H=%d pan x[%d..%d] y[%d..%d] trans=(%d,%d,%d) groups=%u scales:", camera.proj_h, camera.x_min, camera.x_max,
              camera.y_min, camera.y_max, camera.trans[0], camera.trans[1], camera.trans[2], camera.n_groups);
  for (u32 scale : camera.group_scale) std::printf(" %u", scale);
  std::printf("\nwalk mesh: %zu triangles, %zu vertices\n", room->walk.triangles.size(), room->walk.vertices.size());
  // Lighting zones: flag nibble per triangle, with the centroid range of each zone.
  {
    struct LightZone { int count = 0; float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f; } zones[16];
    for (const auto& triangle : room->walk.triangles) {
      const u32 flags = triangle.attr >= 0 && static_cast<size_t>(triangle.attr) < room->attr_flags.size() ? room->attr_flags[static_cast<size_t>(triangle.attr)] : 0;
      float cx = 0, cz = 0;
      for (size_t k = 0; k < 3; ++k) { cx += room->walk.vertices[triangle.v[k]][0] / 3; cz += room->walk.vertices[triangle.v[k]][2] / 3; }
      LightZone& zone = zones[flags & 15];
      ++zone.count; zone.x0 = std::min(zone.x0, cx); zone.x1 = std::max(zone.x1, cx); zone.z0 = std::min(zone.z0, cz); zone.z1 = std::max(zone.z1, cz);
    }
    for (int i = 0; i < 16; ++i)
      if (zones[i].count) std::printf("light zone %2d: %d tris, x %.0f..%.0f z %.0f..%.0f\n", i, zones[i].count,zones[i].x0, zones[i].x1, zones[i].z0, zones[i].z1);
  }
  // OCC_WALK=1 lists every triangle (vertices x z / y, adjacency, attr, attr flag word).
  if (std::getenv("OCC_WALK")) {
    for (size_t i = 0; i < room->walk.triangles.size(); ++i) {
      const auto& triangle = room->walk.triangles[i];
      const u32 flags = triangle.attr >= 0 && static_cast<size_t>(triangle.attr) < room->attr_flags.size() ? room->attr_flags[static_cast<size_t>(triangle.attr)] : 0;
      std::printf("tri %3zu adj %3d %3d %3d attr %2d flags %06X :", i, triangle.adj[0], triangle.adj[1], triangle.adj[2], triangle.attr, flags);
      for (size_t k = 0; k < 3; ++k) {
        const auto& vertex = room->walk.vertices[triangle.v[k]];
        std::printf(" (%.0f,%.0f,y%.0f)", vertex[0], vertex[2], vertex[1]);
      }
      std::printf("\n");
    }
  }
  int rc = 0;
  for (const auto& layer : room->layers) {
    for (const auto& group : layer.groups) {
      char file[512];
      if (group.blend < 0) std::snprintf(file, sizeof file, "%s/%s_L%02u_G%02u.png", outdir.c_str(), name.c_str(), layer.index, group.group);
      else std::snprintf(file, sizeof file, "%s/%s_L%02u_G%02u_B%d.png", outdir.c_str(), name.c_str(), layer.index, group.group, group.blend);
      std::printf("  layer %2u group %2u blend %2d: %zu tiles-layer, %ux%u at (%d,%d) z=%u -> %s\n", layer.index, group.group, group.blend,
                  layer.tiles.size(), group.width, group.height, group.x, group.y, group.z, file);
      if (!write_png(file, group.width, group.height, group.rgba)) {
        std::fprintf(stderr, "cannot write %s\n", file);
        rc = 1;
      }
    }
  }
  return rc;
}

int cmd_rooms_validate(const vfs::Vfs& fs) {
  const auto rooms = list_rooms(fs);
  size_t failed = 0, warned = 0, bbox = 0, extra = 0, tiles = 0;
  std::vector<std::pair<std::string, std::string>> table;
  for (const auto& room_name : rooms) {
    auto bin = fs.read(room_path(room_name, ".bin"));
    auto ctd = fs.read(room_path(room_name, ".ctd"));
    if (!bin || !ctd) {
      table.emplace_back(room_name, "read failed");
      ++failed;
      continue;
    }
    const auto validation = import::validate_room(*bin, *ctd);
    tiles += validation.tiles;
    bbox += validation.bbox_match;
    extra += validation.extra_clut_tims;
    if (!validation.ok()) {
      ++failed;
      for (size_t i = 0; i < validation.errors.size() && i < 6; ++i) table.emplace_back(room_name, "ERROR " + validation.errors[i]);
    }
    if (!validation.warnings.empty()) {
      ++warned;
      for (size_t i = 0; i < validation.warnings.size() && i < 6; ++i) table.emplace_back(room_name, "warn  " + validation.warnings[i]);
    }
  }
  std::printf("rooms: %zu  failures: %zu  with warnings: %zu  tiles: %zu  bbox==viewport+pan: %zu  rooms with unreferenced CLUT TIMs: %zu\n",
              rooms.size(), failed, warned, tiles, bbox, extra);
  if (!table.empty()) std::printf("%-14s %s\n", "room", "message");
  for (const auto& [room_name, message] : table) std::printf("%-14s %s\n", room_name.c_str(), message.c_str());
  return failed ? 1 : 0;
}

// ---------------------------------------------------------------- models
std::string lower(std::string text) {
  std::replace(text.begin(), text.end(), '\\', '/');
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return text;
}

Bytes read_or_empty(const vfs::Vfs& fs, const std::string& path) {
  auto data = fs.read(path);
  return data ? std::move(*data) : Bytes{};
}

std::string strip_digits(std::string text) {
  while (!text.empty() && std::isdigit(static_cast<unsigned char>(text.back()))) text.pop_back();
  return text;
}

// Files belonging to one model, from its path stem ("battle/player/selju/selju").
struct ModelFiles {
  bool field = false;
  Bytes obj, tex, ext;
  // Last path component.
  std::string stem;
};

bool gather_model(const vfs::Vfs& fs, std::string stem, ModelFiles& out) {
  stem = lower(stem);
  if (stem.ends_with(".obj")) stem.resize(stem.size() - 4);
  const std::string base = "cdrom/" + stem;
  out.stem = stem.substr(stem.find_last_of('/') + 1);
  out.field = stem.starts_with("fieldobj/");
  out.obj = read_or_empty(fs, base + ".obj");
  if (out.obj.empty()) return false;
  const std::string dir = "cdrom/" + stem.substr(0, stem.find_last_of('/'));
  if (out.field) {
    out.tex = read_or_empty(fs, base + ".tim");
    out.ext = read_or_empty(fs, "cdrom/fieldobj/ex/" + out.stem + ".bin");
  } else {
    out.tex = read_or_empty(fs, base + ".ltd");
    if (out.tex.empty()) out.tex = read_or_empty(fs, base + ".ltc");
    out.ext = read_or_empty(fs, dir + "/at0.prd");
  }
  return true;
}

void put_vec3(FILE* file, const assets::Vec3& vec, const char* fmt) {
  std::fputc('[', file);
  for (size_t i = 0; i < 3; ++i) {
    if (i) std::fputs(", ", file);
    std::fprintf(file, fmt, static_cast<double>(vec[i]));
  }
  std::fputc(']', file);
}

int cmd_model(const vfs::Vfs& fs, const std::string& stem_arg, const std::string& outdir) {
  ModelFiles mf;
  if (!gather_model(fs, stem_arg, mf)) {
    std::fprintf(stderr, "model %s: .obj not found\n", stem_arg.c_str());
    return 1;
  }
  std::string err;
  auto model = mf.field ? import::load_field_model(mf.obj, mf.tex, mf.ext, &err)
                        : import::load_model(mf.obj, mf.tex, mf.ext, &err);
  if (!model) {
    std::fprintf(stderr, "model %s: %s\n", stem_arg.c_str(), err.c_str());
    return 1;
  }
  const assets::ModelAsset& asset = *model;
  std::filesystem::create_directories(outdir);
  const std::string stem = mf.stem;
  auto path = [&](const std::string& suffix) { return outdir + "/" + stem + suffix; };

  // textures
  for (size_t i = 0; i < asset.textures.size(); ++i) {
    const std::string name = mf.field ? path(".png")
                             : i < asset.main_texture_count ? path("_clut" + std::to_string(i) + ".png")
                                                        : path("_overlay" + std::to_string(i - asset.main_texture_count + 1) + ".png");
    if (!write_png(name, asset.textures[i].width, asset.textures[i].height, asset.textures[i].pixels)) {
      std::fprintf(stderr, "cannot write %s\n", name.c_str());
      return 1;
    }
  }

  // OBJ in rest pose, y up (same convention as cc_model.py export)
  {
    FILE* file = std::fopen(path(".obj").c_str(), "wb");
    if (!file) return 1;
    std::fprintf(file, "# %s  (rest pose, y up)\n", stem_arg.c_str());
    for (const auto& mesh : asset.meshes)
      for (const auto& vertex : mesh.pool)
        std::fprintf(file, "v %.3f %.3f %.3f\n", static_cast<double>(vertex.bind_pos[0]), static_cast<double>(-vertex.bind_pos[1]),
                     static_cast<double>(vertex.bind_pos[2]));
    for (const auto& mesh : asset.meshes)
      for (const auto& vertex : mesh.vertices)
        std::fprintf(file, "vt %.5f %.5f\n", static_cast<double>(vertex.uv[0]), static_cast<double>(1 - vertex.uv[1]));
    size_t pbase = 0, vbase = 0;
    for (const auto& mesh : asset.meshes) {
      for (size_t tri = 0; tri + 2 < mesh.indices.size(); tri += 3) {
        std::fputc('f', file);
        for (size_t k = 0; k < 3; ++k) {
          const u32 vi = mesh.indices[tri + k];
          std::fprintf(file, " %zu/%zu", pbase + mesh.vertices[vi].pool + 1, vbase + vi + 1);
        }
        std::fputc('\n', file);
      }
      pbase += mesh.pool.size();
      vbase += mesh.vertices.size();
    }
    std::fclose(file);
  }
  // mesh.json: skinning data per pool vertex
  {
    FILE* file = std::fopen(path(".mesh.json").c_str(), "wb");
    if (!file) return 1;
    std::fputs("{\"vertices\": [", file);
    bool first = true;
    for (const auto& mesh : asset.meshes)
      for (const auto& vertex : mesh.pool) {
        const size_t influences = vertex.weight[1] > 0 ? 2 : 1;
        std::fprintf(file, "%s{\"index\": %u, \"joints\": [", first ? "" : ", ", vertex.source_index);
        for (size_t i = 0; i < influences; ++i) std::fprintf(file, "%s%u", i ? ", " : "", vertex.joint[i]);
        std::fputs("], \"weights\": [", file);
        for (size_t i = 0; i < influences; ++i) std::fprintf(file, "%s%.6f", i ? ", " : "", static_cast<double>(vertex.weight[i]));
        std::fputs("], \"pos_local\": [", file);
        for (size_t i = 0; i < influences; ++i) {
          if (i) std::fputs(", ", file);
          put_vec3(file, vertex.local[i], "%.0f");
        }
        std::fputs("]}", file);
        first = false;
      }
    size_t tris = 0, quads = 0;
    for (const auto& mesh : asset.meshes) {
      tris += mesh.indices.size() / 3;
      quads += mesh.quad_count;
    }
    std::fprintf(file, "], \"triangles\": %zu, \"quads\": %zu}\n", tris, quads);
    std::fclose(file);
  }
  // skeleton.json
  {
    FILE* file = std::fopen(path(".skeleton.json").c_str(), "wb");
    if (!file) return 1;
    std::fputs("{\"unit\": \"rot: degrees; trans: model units; angle order R = Rz*Ry*Rx\", \"joints\": [\n", file);
    for (size_t i = 0; i < asset.skeleton.joints.size(); ++i) {
      const auto& joint = asset.skeleton.joints[i];
      std::fprintf(file, "%s {\"index\": %zu, \"parent\": %d, \"rot_deg\": ", i ? ",\n" : "", i, joint.parent);
      put_vec3(file, joint.rest_rot_deg, "%.3f");
      std::fputs(", \"trans\": ", file);
      put_vec3(file, joint.rest_trans, "%.0f");
      std::fprintf(file, ", \"bone\": %d}", joint.bone);
    }
    std::fputs("\n], \"attach_points\": [\n", file);
    for (size_t i = 0; i < asset.attach.points.size(); ++i) {
      const auto& point = asset.attach.points[i];
      std::fprintf(file, "%s {\"index\": %zu, \"pos\": ", i ? ",\n" : "", i);
      put_vec3(file, point.pos, "%.0f");
      std::fprintf(file, ", \"joint\": %u}", point.joint);
    }
    std::fputs("\n], \"shading\": [", file);
    for (size_t i = 0; i < asset.shading.size(); ++i)
      std::fprintf(file, "%s[%u, %u, %u, %u]", i ? ", " : "", asset.shading[i].r, asset.shading[i].g, asset.shading[i].b, asset.shading[i].flags);
    std::fputs("]}\n", file);
    std::fclose(file);
  }
  // anims.json: per-frame deltas against the rest pose, like the Python export (null = unchanged from frame 0)
  {
    FILE* file = std::fopen(path(".anims.json").c_str(), "wb");
    if (!file) return 1;
    std::fputs("[", file);
    for (size_t ci = 0; ci < asset.clips.size(); ++ci) {
      const auto& clip = asset.clips[ci];
      std::fprintf(file, "%s\n{\"name\": \"%s\", \"source\": \"%s\", \"duration\": %.4f, \"frame_count\": %u, \"flags_rot_trans\": [",
                   ci ? "," : "", clip.name.c_str(), clip.source.c_str(), static_cast<double>(clip.duration), clip.frame_count);
      for (size_t j = 0; j < clip.tracks.size(); ++j)
        std::fprintf(file, "%s[%u, %u]", j ? ", " : "", clip.rot_animated[j], clip.trans_animated[j]);
      std::fputs("], \"frames\": [", file);
      for (u32 k = 0; k < clip.frame_count; ++k) {
        std::fprintf(file, "%s[", k ? ", " : "");
        for (size_t j = 0; j < clip.tracks.size(); ++j) {
          const auto& jt = clip.tracks[j];
          const auto& rest = asset.skeleton.joints[j];
          std::fprintf(file, "%s{\"rot_deg\": ", j ? ", " : "");
          if (k > 0 && !clip.rot_animated[j]) {
            std::fputs("null", file);
          } else {
            const auto& key = jt.rot[std::min<size_t>(k, jt.rot.size() - 1)];
            std::fprintf(file, "[%.4f, %.4f, %.4f]", static_cast<double>(key.v[0] - rest.rest_rot_deg[0]),
                         static_cast<double>(key.v[1] - rest.rest_rot_deg[1]), static_cast<double>(key.v[2] - rest.rest_rot_deg[2]));
          }
          std::fputs(", \"trans\": ", file);
          if (k > 0 && !clip.trans_animated[j]) {
            std::fputs("null", file);
          } else {
            const auto& key = jt.trans[std::min<size_t>(k, jt.trans.size() - 1)];
            std::fprintf(file, "[%.0f, %.0f, %.0f]", static_cast<double>(key.v[0] - rest.rest_trans[0]),
                         static_cast<double>(key.v[1] - rest.rest_trans[1]), static_cast<double>(key.v[2] - rest.rest_trans[2]));
          }
          std::fputc('}', file);
        }
        std::fputc(']', file);
      }
      std::fputs("]}", file);
    }
    std::fputs("\n]\n", file);
    std::fclose(file);
  }
  size_t verts = 0, tris = 0;
  for (const auto& mesh : asset.meshes) {
    verts += mesh.pool.size();
    tris += mesh.indices.size() / 3;
  }
  std::printf("model %s: %zu constructs, %zu vertices, %zu triangles, %zu joints, %zu clips, %zu textures -> %s\n",
              stem_arg.c_str(), asset.meshes.size(), verts, tris, asset.skeleton.joints.size(), asset.clips.size(), asset.textures.size(),
              outdir.c_str());
  return 0;
}

struct ModelClass {
  const char* name;
  size_t expect;
  size_t total = 0, ok = 0;
};

int cmd_models_validate(const vfs::Vfs& fs) {
  ModelClass battle_p{"battle player", 52}, battle_m{"battle monster", 148}, field{"field", 56}, at0{"at0 packs", 194},
      weapon{"weapon bin", 29}, kmd{"kmd", 17}, ex{"fieldobj/ex", 115};
  std::vector<std::string> failures;
  size_t warned = 0, stubs = 0, verts = 0, faces = 0, clips = 0;
  std::vector<std::string> names;
  for (const auto& member : fs.list("cdrom")) names.push_back(lower(member));
  std::sort(names.begin(), names.end());

  auto record = [&](ModelClass& model_class, const std::string& what, bool ok, const std::string& msg) {
    ++model_class.total;
    if (ok) ++model_class.ok;
    else failures.push_back(what + ": " + msg);
  };
  // Model directory -> joint count, for at0/ex cross checks.
  std::map<std::string, u32> model_joints;

  // pass 1: models (fills model_joints); pass 2: packs and meshes
  for (const std::string& name : names) {
    const bool bp = name.starts_with("battle/player/"), bm = name.starts_with("battle/monster/");
    const bool fo = name.starts_with("fieldobj/player/");
    if (!((bp || bm || fo) && name.ends_with(".obj"))) continue;
    ModelFiles mf;
    gather_model(fs, name, mf);
    const auto validation = import::validate_model(mf.obj, mf.tex, mf.ext, fo);
    verts += validation.vertices;
    faces += validation.faces;
    clips += validation.clips;
    warned += !validation.warnings.empty();
    model_joints[name.substr(0, name.find_last_of('/'))] = validation.joints;
    record(bp ? battle_p : bm ? battle_m : field, name, validation.ok(), validation.errors.empty() ? "" : validation.errors[0]);
  }
  for (const std::string& name : names) {
    const bool bat = name.starts_with("battle/player/") || name.starts_with("battle/monster/");
    if (bat && name.ends_with(".prd")) {
      auto data = read_or_empty(fs, "cdrom/" + name);
      std::string err;
      auto info = import::inspect_anim_pack(import::at0_pack(data), &err);
      bool ok = info.has_value();
      if (ok) {
        auto it = model_joints.find(name.substr(0, name.find_last_of('/')));
        if (it != model_joints.end() && it->second != info->joints) {
          ok = false;
          err = "anim J=" + std::to_string(info->joints) + " != model J=" + std::to_string(it->second);
        }
      }
      record(at0, name, ok, err);
    } else if (name.starts_with("battle/wepon/") && name.ends_with(".bin")) {
      std::string err;
      auto wp = import::load_weapon_pack(read_or_empty(fs, "cdrom/" + name), &err);
      record(weapon, name, wp.has_value(), err);
    } else if (name.starts_with("battle/wepon/") && name.ends_with(".kmd")) {
      std::string err;
      auto mesh = import::load_kmd(read_or_empty(fs, "cdrom/" + name), &err);
      record(kmd, name, mesh.has_value(), err);
    } else if (name.starts_with("fieldobj/ex/") && name.ends_with(".bin")) {
      auto data = read_or_empty(fs, "cdrom/" + name);
      auto pack = import::ex_pack(data);
      if (pack.empty()) {
        // dummy.bin is a text stub, not a cpt.
        ++stubs;
        continue;
      }
      std::string err;
      auto info = import::inspect_anim_pack(pack, &err);
      bool ok = info.has_value();
      if (ok) {
        const size_t slash = name.find_last_of('/');
        const std::string stem = name.substr(slash + 1, name.size() - slash - 5);
        for (const std::string& cand : {stem, strip_digits(stem)}) {
          auto it = model_joints.find("fieldobj/player/" + cand);
          if (it == model_joints.end()) continue;
          if (it->second != info->joints) {
            ok = false;
            err = "ex J=" + std::to_string(info->joints) + " != model " + cand + " J=" + std::to_string(it->second);
          }
          break;
        }
      }
      record(ex, name, ok, err);
    }
  }
  bool bad = false;
  std::printf("%-16s %6s %6s %8s\n", "class", "ok", "total", "expected");
  for (const ModelClass* model_class : {&battle_p, &battle_m, &field, &at0, &weapon, &kmd, &ex}) {
    const bool good = model_class->ok == model_class->total && model_class->total == model_class->expect;
    bad |= !good;
    std::printf("%-16s %6zu %6zu %8zu%s\n", model_class->name, model_class->ok, model_class->total, model_class->expect, good ? "" : "   <-- FAILURE");
  }
  std::printf("non-cpt ex stubs skipped: %zu   models with warnings: %zu   vertices: %zu  faces: %zu  clips: %zu\n", stubs,
              warned, verts, faces, clips);
  for (const auto& failure : failures) std::printf("  %s\n", failure.c_str());
  return bad ? 1 : 0;
}

// ---------------------------------------------------------------- chip2 (RDE HD backgrounds)
std::optional<import::Chip2> load_chip2(const vfs::Vfs& fs, const std::string& name, std::string* err) {
  auto data = fs.read("hd/map/mapbin/" + name + ".chip2");
  if (!data) {
    if (err) *err = "hd/map/mapbin/" + name + ".chip2 not found";
    return std::nullopt;
  }
  return import::parse_chip2(*data, err);
}

std::optional<assets::RoomScene> load_room_scene(const vfs::Vfs& fs, const std::string& name, std::string* err) {
  auto bin = fs.read(room_path(name, ".bin"));
  auto ctd = fs.read(room_path(name, ".ctd"));
  if (!bin || !ctd) {
    if (err) *err = "room .bin/.ctd not found";
    return std::nullopt;
  }
  auto prd = fs.read(room_path(name, ".prd"));
  return import::load_room(name, *bin, *ctd, prd ? ByteSpan(*prd) : ByteSpan{}, err);
}

int cmd_chip2(const vfs::Vfs& fs, const std::string& name, const std::string& outdir) {
  std::string err;
  auto chip = load_chip2(fs, name, &err);
  auto room = chip ? load_room_scene(fs, name, &err) : std::nullopt;
  if (!chip || !room) {
    std::fprintf(stderr, "chip2 %s: %s\n", name.c_str(), err.c_str());
    return 1;
  }
  std::filesystem::create_directories(outdir);
  int rc = 0;
  for (const auto& layer : room->layers)
    for (const auto& group : layer.groups) {
      u32 tile_count = 0, miss = 0;
      auto hd = import::compose_hd_group(*room, layer.index, group.group, *chip, &tile_count, &miss, group.blend);
      if (!hd) continue;
      char file[512];
      std::snprintf(file, sizeof file, "%s/%s_L%02u_G%02u_B%d_hd.png", outdir.c_str(), name.c_str(), layer.index, group.group, group.blend);
      std::printf("  layer %2u group %2u: %ux%u, %u tiles, %u without HD -> %s\n", layer.index, group.group, hd->width, hd->height, tile_count,
                  miss, file);
      if (!write_png(file, hd->width, hd->height, hd->rgba)) {
        std::fprintf(stderr, "cannot write %s\n", file);
        rc = 1;
      }
    }
  return rc;
}

int cmd_chip2_validate(const vfs::Vfs& fs) {
  std::vector<std::string> names;
  for (std::string member : fs.list("hd")) {
    std::replace(member.begin(), member.end(), '\\', '/');
    const std::string pre = "map/mapbin/";
    const size_t at = member.find(pre);
    if (at == std::string::npos || !member.ends_with(".chip2")) continue;
    names.push_back(member.substr(at + pre.size(), member.size() - at - pre.size() - 6));
  }
  std::sort(names.begin(), names.end());
  size_t parse_fail = 0, no_room = 0, tiles = 0, miss = 0, rooms_with_miss = 0;
  std::vector<std::string> notes;
  std::printf("%-16s %7s %7s\n", "room", "tiles", "no-HD");
  for (const auto& nm : names) {
    std::string err;
    auto chip = load_chip2(fs, nm, &err);
    if (!chip) {
      ++parse_fail;
      notes.push_back(nm + ": " + err);
      continue;
    }
    auto room = load_room_scene(fs, nm, &err);
    if (!room) {
      ++no_room;
      notes.push_back(nm + ": no room to match against (" + err + ")");
      continue;
    }
    size_t rt = 0, rm = 0;
    for (const auto& layer : room->layers)
      for (const auto& tile : layer.tiles) {
        ++rt;
        if (chip->lookup(tile.clut, tile.v, tile.u) == 0) ++rm;
      }
    tiles += rt;
    miss += rm;
    if (rm) {
      ++rooms_with_miss;
      std::printf("%-16s %7zu %7zu\n", nm.c_str(), rt, rm);
    }
  }
  for (const auto& note : notes) std::printf("  %s\n", note.c_str());
  std::printf("chip2 files: %zu, parse failures: %zu, without a room: %zu, rooms with misses: %zu, tiles: %zu, no HD tile: %zu\n",
              names.size(), parse_fail, no_room, rooms_with_miss, tiles, miss);
  return parse_fail ? 1 : 0;
}


// ---------------------------------------------------------------- field scripts
std::optional<script::FieldScriptModule> load_script_module(const vfs::Vfs& fs, const std::string& room, std::string* err) {
  auto bin = fs.read(room_path(room, ".bin"));
  if (!bin) {
    *err = "cannot read " + room_path(room, ".bin");
    return std::nullopt;
  }
  auto cpt = import::parse_cpt(*bin);
  if (!cpt || cpt->entries.size() < 4) {
    *err = "bad cpt";
    return std::nullopt;
  }
  auto dec = import::lzss_decode(cpt->entries[3].data);
  if (!dec) {
    *err = "script subfile: lzss failed";
    return std::nullopt;
  }
  return script::FieldScriptModule::parse(*dec, err);
}

int cmd_script_disasm(const vfs::Vfs& fs, const std::string& room, int entity) {
  std::string err;
  auto script_module = load_script_module(fs, room, &err);
  if (!script_module) {
    std::fprintf(stderr, "%s: %s\n", room.c_str(), err.c_str());
    return 1;
  }
  const std::string text = script::disassemble(*script_module, entity);
  // A4 operands may contain NUL characters, so write the raw bytes.
  std::fwrite(text.data(), 1, text.size(), stdout);
  return 0;
}

// ---------------------------------------------------------------- dialogue choices (AA1)
struct ChoiceInst {
  std::string room;
  int msg = 0, first = 0, last = 0;
  // dead: listed as unreferenced code; near_msg: the message op sits right before the E1 in the listing.
  bool dead = false;
  bool near_msg = true;
};

// E0 / E3 / E4 (message open) followed by E1 in the room listing; the message operand is the word at +1 (E0) or +2 (E3 / E4).
std::vector<ChoiceInst> find_choices(const script::FieldScriptModule& script_module, const std::string& room) {
  std::vector<ChoiceInst> out;
  const std::string text = script::disassemble(script_module, -1);
  int last_msg = -1, since = 99;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t line_end = text.find('\n', pos);
    if (line_end == std::string::npos) line_end = text.size();
    const std::string line = text.substr(pos, line_end - pos);
    pos = line_end + 1;
    if (line.size() < 10 || line[0] != ' ' || line[1] != ' ' || line[6] != ':') continue;
    std::vector<int> by;
    size_t i = 8;
    while (i + 1 < line.size() && std::isxdigit(static_cast<unsigned char>(line[i])) && std::isxdigit(static_cast<unsigned char>(line[i + 1]))) {
      by.push_back(static_cast<int>(std::strtol(line.substr(i, 2).c_str(), nullptr, 16)));
      i += 3;
    }
    if (by.empty() || line.find(".data") != std::string::npos) {
      last_msg = -1;
      continue;
    }
    ++since;
    if (by[0] == 0xE0 && by.size() >= 4) {
      last_msg = by[1] | (by[2] << 8);
      since = 0;
    } else if ((by[0] == 0xE3 || by[0] == 0xE4) && by.size() >= 5) {
      last_msg = by[2] | (by[3] << 8);
      since = 0;
    } else if (by[0] == 0xE1 && by.size() >= 2) {
      ChoiceInst choice;
      choice.room = room;
      choice.msg = last_msg;
      choice.first = by[1] >> 4;
      choice.last = by[1] & 15;
      choice.dead = line.find("unreferenced") != std::string::npos;
      choice.near_msg = last_msg >= 0 && since <= 12;
      out.push_back(choice);
    }
  }
  return out;
}

// Blast radius of the "no 3-line minimum" window rule: every room message (EN row) whose tallest page has 1 or 2 lines has a
// window that is 2 / 1 rows (61.75 canvas units each) shorter than before. Lines come from layout_pages with the message's own wrap limit and
// the HD font's widths; default names stand in for typed ones.
int cmd_dialogue_sizes(const vfs::Vfs& fs, const std::string& only_room, bool verbose) {
  import::LangTables tables;
  tables.init(&fs);
  import::JpGlyphs jpg;
  std::optional<import::SpriteFont> font;
  if (auto font_bytes = fs.read("font/large_font_gn_70.spritefont")) font = import::parse_spritefont(std::move(*font_bytes));
  if (!font) {
    std::printf("dialogue-sizes: the message font is missing\n");
    return 1;
  }
  size_t total = 0, one = 0, two = 0, errs = 0;
  std::vector<std::string> examples1, examples2;
  for (const auto& room : list_rooms(fs)) {
    if (!only_room.empty() && room != only_room) continue;
    const int rid = import::room_id_from_name(room);
    auto bytes = import::read_room_dialogue_block(fs, room);
    if (!bytes) continue;
    auto blk = import::parse_dialogue_block(*bytes);
    if (!blk) continue;
    for (size_t idx = 0; idx < blk->records.size(); ++idx) {
      auto raw = tables.lookup_field(rid, static_cast<int>(idx), import::Lang::En);
      if (!raw) continue;
      if (*raw == "{ERROR}") {
        ++errs;
        continue;
      }
      ++total;
      ui::LayoutMetrics lm;
      lm.advance = [](char32_t) { return 22.0f; };
      lm.glyph_width = [&font](char32_t ch, bool italic) { return ui::wrap_glyph_width(font->measure(static_cast<u32>(ch)), italic); };
      lm.line_height = 57.78f;
      lm.max_width = ui::wrap_limit(rid, static_cast<int>(idx), import::Lang::En, ui::disc_line_chars(import::parse_ps1(blk->records[idx].bytes, jpg)));
      lm.wrap = true;
      const auto pages = ui::layout_pages(import::flatten(import::parse_markup(*raw, import::Lang::En), {}), lm);
      int lines = 1;
      for (const auto& page : pages) lines = std::max(lines, page.lines);
      if (lines > 2) continue;
      std::string line = room + " msg " + std::to_string(idx) + " (" + std::to_string(lines) + " line" + (lines > 1 ? "s" : "") + "): " + *raw;
      for (size_t k = 0; (k = line.find('\n', k)) != std::string::npos;) line.replace(k, 1, "\\n");
      if (lines == 1) {
        ++one;
        if (verbose || examples1.size() < 8) examples1.push_back(line);
      } else {
        ++two;
        if (verbose || examples2.size() < 8) examples2.push_back(line);
      }
    }
  }
  std::printf("dialogue-sizes (EN): %zu messages, %zu 1-line (window 2 rows shorter), %zu 2-line (1 row shorter), %zu {ERROR} rows skipped\n",
              total, one, two, errs);
  for (const auto& example : examples1) std::printf("  1: %s\n", example.c_str());
  for (const auto& example : examples2) std::printf("  2: %s\n", example.c_str());
  return 0;
}

int cmd_dialogue_choices(const vfs::Vfs& fs, const std::string& only_room, bool verbose) {
  import::LangTables tables;
  tables.init(&fs);
  size_t instances = 0, checks = 0, fails = 0, no_row = 0, data_defects = 0;
  std::map<std::string, size_t> classes, fail_classes;
  std::vector<std::string> notes;
  // "room msg E1 range :: problem" -> languages.
  std::map<std::string, std::string> grouped;
  for (const auto& room : list_rooms(fs)) {
    if (!only_room.empty() && room != only_room) continue;
    std::string err;
    auto script_module = load_script_module(fs, room, &err);
    if (!script_module) continue;
    const int rid = import::room_id_from_name(room);
    Bytes guest_bytes;
    std::optional<import::DialogueBlock> guest_block;
    if (auto block = import::read_room_dialogue_block(fs, room)) {
      guest_bytes = std::move(*block);
      guest_block = import::parse_dialogue_block(guest_bytes);
    }
    import::JpGlyphs jpg;
    for (const ChoiceInst& choice : find_choices(*script_module, room)) {
      ++instances;
      if (choice.dead) {
        ++classes["dead code (unreferenced)"];
        continue;
      }
      if (choice.msg < 0) {
        ++fails;
        notes.push_back(room + ": E1 without a message op");
        continue;
      }
      for (int li = 0; li < 6; ++li) {
        const auto lang = static_cast<import::Lang>(li);
        const std::string tag = room + " msg " + std::to_string(choice.msg) + " E1 " + std::to_string(choice.first) + ".." + std::to_string(choice.last) + " [" +
                                import::lang_code(lang) + "]";
        auto raw = tables.lookup_field(rid, choice.msg, lang);
        if (!raw || *raw == "{ERROR}") {
          if (li == 0) {
            ++no_row;
            if (verbose) notes.push_back(tag + ": no text row");
          }
          continue;
        }
        import::Message msg = tables.field_message(rid, choice.msg, lang);
        import::Message flat = import::flatten(msg, tables.make_context(lang));
        bool name = false, color = false, italic = false, wend = false, fw = false, fset = false, bwait = false, pages = false;
        for (const auto& token : msg.tokens) {
          name |= token.kind == import::TokKind::Name || token.kind == import::TokKind::PartyName || token.kind == import::TokKind::PlayWork;
          color |= token.kind == import::TokKind::Color && token.arg != 0;
          italic |= token.kind == import::TokKind::Italic;
          wend |= token.kind == import::TokKind::AutoClose;
          fw |= token.kind == import::TokKind::FWait;
          fset |= token.kind == import::TokKind::FSet;
          bwait |= token.kind == import::TokKind::Wait;
          pages |= token.kind == import::TokKind::Page;
        }
        ui::LayoutMetrics lm;
        // About the large_ gn_70 average advance at scale 0.5.
        lm.advance = [](char32_t) { return 18.0f; };
        lm.line_height = 56.0f;
        lm.max_width = ui::wrap_limit(rid, choice.msg, lang, guest_block && static_cast<size_t>(choice.msg) < guest_block->records.size()
                                                                     ? ui::disc_line_chars(import::parse_ps1(guest_block->records[static_cast<size_t>(choice.msg)].bytes, jpg))
                                                                     : 0);
        lm.wrap = lang != import::Lang::Jp;
        if (lm.wrap) {
          lm.choice_space_first = 2.0f * 70.0f * 0.45f;
          lm.choice_space = 2.0f * 20.0f * 0.45f;
        }
        auto pg = ui::layout_pages(flat, lm);
        std::vector<std::string> bad;
        bool remap = false;
        if (pg.empty()) {
          bad.push_back("no pages");
        } else {
          const ui::TextPage& lp = pg.back();
          // run the typewriter to the state that accepts the pick
          ui::Typewriter tw;
          tw.start(pg);
          int guard = 0;
          while (guard++ < 40000 && tw.state() != ui::Typewriter::State::WaitClose && tw.state() != ui::Typewriter::State::Closing && !tw.closed()) {
            if (tw.state() == ui::Typewriter::State::WaitConfirm || tw.state() == ui::Typewriter::State::WaitPage) tw.confirm();
            tw.update(1.0f / 60.0f);
          }
          if (tw.state() != ui::Typewriter::State::WaitClose && tw.state() != ui::Typewriter::State::Closing)
            bad.push_back("typewriter never reaches the pick state");
          if (tw.page_index() + 1 != tw.page_count()) bad.push_back("pick state not on the last page");
          if (choice.last < choice.first) bad.push_back("E1 last < first");
          const float text_h = static_cast<float>(lp.lines) * lm.line_height;
          std::vector<int> gl;
          if (guest_block && static_cast<size_t>(choice.msg) < guest_block->records.size())
            gl = ui::DialogueBox::guest_choice_lines(import::parse_ps1(guest_block->records[static_cast<size_t>(choice.msg)].bytes, jpg));
          const int guest_first = gl.empty() ? -1 : gl.back();
          const int trans_first = ui::DialogueBox::first_choice_line(lp, lm.line_height);
          if (trans_first < 0) bad.push_back("DATA: translation has no choice-marked line (debug menu / untranslated)");
          if (guest_first < 0) bad.push_back("DATA: PS1 message has no choice-marked line (U+3000 x3)");
          int prev = -1;
          const bool usable = trans_first >= 0 && guest_first >= 0;
          remap = usable && trans_first != guest_first;
          for (int i = 0; usable && i <= choice.last - choice.first; ++i) {
            ++checks;
            const int row = choice.first + i;
            const int line = ui::DialogueBox::map_choice_line(trans_first, guest_first, row);
            const std::string ci = "choice " + std::to_string(i) + " (row " + std::to_string(row) + " -> line " + std::to_string(line) + "): ";
            if (line != prev + 1 && prev >= 0) bad.push_back(ci + "not the line after the previous choice");
            prev = line;
            const auto cur = ui::DialogueBox::choice_cursor(lp, lm.line_height, line, !lm.wrap, 3);
            if (!cur.valid) {
              bad.push_back("DATA: " + ci + "translated list is shorter than the E1 range (page has " + std::to_string(lp.lines) + " lines, tf " + std::to_string(trans_first) + " gf " + std::to_string(guest_first) + ")");
              continue;
            }
            const float cy = cur.y + cur.h * 0.5f, band0 = static_cast<float>(line) * lm.line_height;
            if (cur.w <= 0 || cur.h <= 0) bad.push_back(ci + "empty cursor rect");
            if (cur.x < -0.5f || cur.x > lp.width) bad.push_back(ci + "cursor x " + std::to_string(cur.x) + " outside the text");
            if (cy < band0 - lm.line_height || cy > band0 + 2 * lm.line_height) bad.push_back(ci + "cursor not on its line");
            if (cur.y < -lm.line_height || cur.y + cur.h > text_h + lm.line_height) bad.push_back(ci + "cursor outside the window");
            bool blank = true;
            for (const auto& glyph : lp.glyphs)
              if (static_cast<int>(std::lround(glyph.y / lm.line_height)) == line && glyph.ch != U' ') blank = false;
            if (blank) bad.push_back("DATA: " + ci + "line is blank (empty trailing option)");
            // the mapped line must be a choice line (first three glyphs spaces)
            std::vector<char32_t> head;
            for (const auto& glyph : lp.glyphs)
              if (static_cast<int>(std::lround(glyph.y / lm.line_height)) == line && head.size() < 3) head.push_back(glyph.ch);
            if (head.size() < 3 || head[0] > 0x3000 || (head[0] != U' ' && head[0] != 0x3000) || (head[1] != U' ' && head[1] != 0x3000) ||
                (head[2] != U' ' && head[2] != 0x3000))
              bad.push_back(ci + "selects a line that is not a choice line");
          }
        }
        std::string key = "n=" + std::to_string(choice.last - choice.first + 1) + " first=" + std::to_string(choice.first);
        key += remap ? " REMAP" : "";
        key += wend ? " wend" : "";
        key += pages ? " multipage" : "";
        key += bwait ? " bwait" : "";
        key += fw || fset ? " fwait/fset" : "";
        key += name ? " name" : "";
        key += color ? " color" : "";
        key += italic ? " italic" : "";
        ++classes[key];
        if (!bad.empty()) {
          for (const auto& problem : bad)
            if (problem.rfind("DATA: ", 0) != 0) {
              ++fails;
              ++fail_classes[key];
            } else {
              ++data_defects;
            }
          for (const auto& problem : bad) {
            std::string kind = problem;
            const size_t colon = kind.find("): ");
            if (!verbose && kind.rfind("choice ", 0) == 0 && colon != std::string::npos) kind = "choice line: " + kind.substr(colon + 3);
            const size_t pgp = kind.find(" (page has");
            if (pgp != std::string::npos && !verbose) kind.erase(pgp);
            std::string& langs = grouped[room + " msg " + std::to_string(choice.msg) + " E1 " + std::to_string(choice.first) + ".." + std::to_string(choice.last) + " :: " + kind];
            const std::string lc = import::lang_code(lang);
            if (langs.find(lc) == std::string::npos) langs += (langs.empty() ? "" : " ") + lc;
          }
        } else if (verbose && li == 0) {
          std::string plain;
          for (const auto& token : flat.tokens) {
            if (token.kind == import::TokKind::Text) plain += import::u32_to_utf8(token.text);
            else if (token.kind == import::TokKind::Newline) plain += " | ";
            else if (token.kind == import::TokKind::Page) plain += " || ";
          }
          notes.push_back(tag + " ok: " + key + " :: " + plain);
        }
      }
    }
  }
  std::printf("choice instances (E1) %zu, cursor checks %zu, failures %zu, text-data defects (not engine faults) %zu\n", instances, checks, fails, data_defects);
  std::printf("instances without an English text row (debug rooms / unused scripts): %zu\n", no_row);
  std::printf("classes (instance x language):\n");
  for (const auto& [class_key, class_count] : classes) std::printf("  %-48s %6zu  fail %zu\n", class_key.c_str(), class_count, fail_classes.count(class_key) ? fail_classes[class_key] : size_t{0});
  for (const auto& [problem, langs] : grouped) std::printf("  FAIL %s  [%s]\n", problem.c_str(), langs.c_str());
  for (const auto& note : notes) std::printf("  %s\n", note.c_str());
  return fails ? 1 : 0;
}

int cmd_script_soak(const vfs::Vfs& fs) {
  const auto rooms = list_rooms(fs);
  size_t crashes = 0, bad_rooms = 0;
  u64 instr = 0, invalid_pc = 0, overruns = 0, overflows = 0, unimpl = 0;
  std::map<u16, u64> unknown;
  std::vector<u64> hits(0x200, 0);
  std::vector<std::string> notes;
  for (const auto& room_name : rooms) {
    try {
      std::string err;
      auto script_module = load_script_module(fs, room_name, &err);
      if (!script_module) {
        ++crashes;
        notes.push_back(room_name + ": " + err);
        continue;
      }
      script::NullHost host;
      script::FieldVM vm(host);
      vm.load(std::move(*script_module));
      for (int tick = 0; tick < 600; ++tick) {
        // Exercise the triggered scripts too.
        if (tick == 5 || tick == 300) {
          for (int entity = 0; entity < static_cast<int>(vm.entity_count()); ++entity) {
            vm.trigger_action(entity);
            vm.trigger_collision(entity);
            vm.trigger_touch(entity);
          }
        }
        vm.tick();
      }
      const auto& st = vm.stats();
      instr += st.instructions;
      bool bad = false;
      if (st.invalid_pc) { invalid_pc += st.invalid_pc; bad = true; notes.push_back(room_name + ": invalid pc x" + std::to_string(st.invalid_pc)); }
      if (st.unimplemented) { unimpl += st.unimplemented; bad = true; }
      for (const auto& [opcode, hit_count] : st.unknown_ops) { unknown[opcode] += hit_count; bad = true; notes.push_back(room_name + ": unknown opcode " + std::to_string(opcode)); }
      if (st.init_overruns) notes.push_back(room_name + ": " + std::to_string(st.init_overruns) + " INIT script(s) parked/looping (step cap)");
      overruns += st.init_overruns;
      overflows += st.call_overflows;
      for (size_t i = 0; i < hits.size(); ++i) hits[i] += st.op_hits[i];
      bad_rooms += bad;
    } catch (const std::exception& ex) {
      ++crashes;
      notes.push_back(room_name + ": exception " + ex.what());
    }
  }
  size_t seen = 0;
  std::string never;
  for (const auto& op : script::op_table()) {
    const size_t idx = op.key > 0xFF ? 0x100 + (op.key & 0xFF) : op.key;
    if (hits[idx]) ++seen;
    else {
      char buf[16];
      std::snprintf(buf, sizeof buf, " %s%02X", op.key > 0xFF ? "FE" : "", op.key & 0xFF);
      never += buf;
    }
  }
  std::printf("rooms: %zu  instructions: %llu  crashes: %zu  rooms with invalid pc/unknown/unimplemented: %zu\n", rooms.size(),
              static_cast<unsigned long long>(instr), crashes, bad_rooms);
  std::printf("invalid pc: %llu  unknown opcode hits: %zu  unimplemented: %llu  INIT overruns (parked/looping INIT): %llu  CALL overflows: %llu\n",
              static_cast<unsigned long long>(invalid_pc), unknown.size(), static_cast<unsigned long long>(unimpl),
              static_cast<unsigned long long>(overruns), static_cast<unsigned long long>(overflows));
  std::printf("table opcodes executed at least once: %zu of %zu; never executed:%s\n", seen, script::op_table().size(), never.c_str());
  for (const auto& note : notes) std::printf("  %s\n", note.c_str());
  return crashes || invalid_pc || unimpl || !unknown.empty() ? 1 : 0;
}


// occ room-songs [room]: every PLAY_SONG (op 88) of the room's scripts with the song id and the RDE track it maps to (bgm_table);
// "$" = the song comes from a variable. Music is script driven (no room-header song): this is the room -> song list.
int cmd_room_songs(const vfs::Vfs& fs, const std::string& only) {
  for (const auto& room_name : list_rooms(fs)) {
    if (!only.empty() && room_name != only) continue;
    std::string err;
    auto script_module = load_script_module(fs, room_name, &err);
    if (!script_module) continue;
    const std::string text = script::disassemble(*script_module, -1);
    std::string out;
    size_t pos = 0;
    while ((pos = text.find("PLAY_SONG", pos)) != std::string::npos) {
      const size_t eol = text.find('\n', pos);
      const std::string operand = text.substr(pos + 9, eol == std::string::npos ? std::string::npos : eol - pos - 9);
      pos += 9;
      const size_t hash_pos = operand.find('#');
      char buf[48];
      if (hash_pos == std::string::npos) {
        out += " $var";
        continue;
      }
      const int id = std::atoi(operand.c_str() + hash_pos + 1);
      const audio::BgmEntry* entry = audio::bgm_entry(id);
      if (entry) std::snprintf(buf, sizeof buf, " %d(BGM%03d)", id, entry->file_no);
      else std::snprintf(buf, sizeof buf, " %d(%s)", id, id == 255 ? "STOP" : "NO TRACK");
      out += buf;
    }
    if (!out.empty()) std::printf("%-16s%s\n", room_name.c_str(), out.c_str());
  }
  return 0;
}

// occ battlefields [--inc <file>]: every BATTLE op (A3 / C9) of every room script -> the stage it fights on, by BattleView's rule: the op's
// battlefield operand when it is an immediate 0..99, else the scene record's bg (scene = id, + 0x200 for C9). Prints bg_NN, the number of scene
// records with that bg and the rooms; --inc writes the per-stage room lists (one quoted string per bg_00..bg_99) as a C++ initializer, the
// file data/battlefields.h includes as battlefield_rooms.inc.
int cmd_battlefields(const vfs::Vfs& fs, const std::vector<std::string>& args) {
  data::GameTables tables;
  std::string err;
  if (!tables.load(fs, &err)) {
    std::fprintf(stderr, "battlefields: tables: %s\n", err.c_str());
    return 1;
  }
  std::string inc;
  for (size_t i = 1; i + 1 < args.size(); ++i)
    if (args[i] == "--inc") inc = args[i + 1];
  std::vector<std::vector<std::string>> rooms(100);
  int unresolved = 0;
  for (const auto& room_name : list_rooms(fs)) {
    auto script_module = load_script_module(fs, room_name, &err);
    if (!script_module) continue;
    const std::string text = script::disassemble(*script_module, -1);
    std::vector<bool> seen(100, false);
    size_t pos = 0;
    // A listing line looks like "  023C: C9 82 81 16 80 02 80     BATTLE    #386, #22, #2".
    while ((pos = text.find("BATTLE ", pos)) != std::string::npos) {
      const size_t bol = text.rfind('\n', pos) + 1, eol = text.find('\n', pos);
      const std::string line = text.substr(bol, eol == std::string::npos ? std::string::npos : eol - bol);
      pos += 7;
      unsigned addr = 0, op = 0;
      if (std::sscanf(line.c_str(), " %x: %x", &addr, &op) != 2 || (op != 0xA3 && op != 0xC9)) continue;
      const size_t ops = line.find("BATTLE ") + 7;
      const size_t h1 = line.find('#', ops);
      if (h1 == std::string::npos) {
        ++unresolved;
        continue;
      }
      const int id = std::atoi(line.c_str() + h1 + 1) + (op == 0xC9 ? 0x200 : 0);
      int bg = -1;
      const size_t comma = line.find(',', h1);
      if (comma != std::string::npos && line.find('#', comma) == comma + 2) bg = std::atoi(line.c_str() + comma + 3);
      if (bg < 0 || bg >= 100) bg = id >= 0 && static_cast<size_t>(id) < tables.scenes.size() ? tables.scenes[static_cast<size_t>(id)].bg() : -1;
      if (bg < 0 || bg >= 100) {
        ++unresolved;
        continue;
      }
      if (!seen[static_cast<size_t>(bg)]) rooms[static_cast<size_t>(bg)].push_back(room_name);
      seen[static_cast<size_t>(bg)] = true;
    }
  }
  std::vector<int> scenes(100, 0);
  for (const auto& scene : tables.scenes)
    if (scene.bg() < 100) ++scenes[scene.bg()];
  std::string out;
  for (int bg = 0; bg < 100; ++bg) {
    std::string list;
    for (const auto& room_name : rooms[static_cast<size_t>(bg)]) list += (list.empty() ? "" : " ") + room_name;
    out += "\"" + list + "\",\n";
    if (scenes[static_cast<size_t>(bg)] == 0 && list.empty()) continue;
    std::printf("bg_%02d  %3d scenes  %-34s %s\n", bg, scenes[static_cast<size_t>(bg)], data::battlefield_areas(bg).c_str(), list.c_str());
  }
  if (unresolved) std::printf("(%d battle ops with a variable id / field were not resolved)\n", unresolved);
  if (!inc.empty()) {
    std::ofstream file(inc, std::ios::binary);
    file << out;
    std::printf("battlefields: wrote %s\n", inc.c_str());
  }
  return 0;
}

// occ world-markers [--inc <file>]: every location marker of the overworld screens (wld_*): the marker entity's
// label window (SETUP_DIALOG_WINDOW ... #56) names the message DIALOG shows, SET_NEXT_ROOM names the room it enters. The label text (EN) is
// cleaned: line breaks / runs of blanks -> one space, a trailing "- North -" / "- Clotho -" style subtitle dropped. Prints "text -> room" per
// marker, then the table: one row per marker room stem ("opa_a01f" = Lizard Rock) and one per room prefix (the prefix's most frequent name,
// shortest on a tie: "opa" = Lizard Rock) for the rooms no marker enters; --inc writes it as a C++ initializer ({stem-or-prefix, name} rows),
// the file data/battlefields.h includes as area_names.inc.
int cmd_world_markers(const vfs::Vfs& fs, const std::vector<std::string>& args) {
  std::string inc;
  for (size_t i = 1; i + 1 < args.size(); ++i)
    if (args[i] == "--inc") inc = args[i + 1];
  import::LangTables tables;
  tables.init(&fs);
  // by_stem: marker room stem -> name (first marker wins); by_prefix: prefix -> (name, markers).
  std::map<std::string, std::string> by_stem;
  std::map<std::string, std::vector<std::pair<std::string, int>>> by_prefix;
  for (const auto& room_name : list_rooms(fs)) {
    if (!room_name.starts_with("wld_")) continue;
    std::string err;
    auto script_module = load_script_module(fs, room_name, &err);
    if (!script_module) continue;
    const int rid = import::room_id_from_name(room_name);
    const std::string text = script::disassemble(*script_module, -1);
    int msg = -1;
    bool label = false;
    size_t pos = 0;
    while (pos < text.size()) {
      const size_t eol = text.find('\n', pos);
      const std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
      pos = eol == std::string::npos ? text.size() : eol + 1;
      // A "; ----" line starts a new entity script.
      if (line.starts_with("; ----")) {
        msg = -1;
        label = false;
        continue;
      }
      if (line.find("SETUP_DIALOG_WINDOW") != std::string::npos) {
        label = line.find("#56") != std::string::npos;
        continue;
      }
      if (const size_t dialog_pos = line.find("DIALOG    0x"); dialog_pos != std::string::npos && label) {
        msg = static_cast<int>(std::strtol(line.c_str() + dialog_pos + 10, nullptr, 16));
        continue;
      }
      if (const size_t next_room_pos = line.find("SET_NEXT_ROOM #"); next_room_pos != std::string::npos && msg >= 0) {
        const int target = std::atoi(line.c_str() + next_room_pos + 15);
        const char* stem = import::room_name_for_id(target);
        auto raw = tables.lookup_field(rid, msg, import::Lang::En);
        std::string raw_name;
        if (raw) {
          const import::Message flat = import::flatten(tables.field_message(rid, msg, import::Lang::En), tables.make_context(import::Lang::En));
          // A line break becomes a blank.
          for (const auto& token : flat.tokens) raw_name += token.kind == import::TokKind::Text ? import::u32_to_utf8(token.text) : std::string(" ");
        }
        // Blanks collapsed, trimmed.
        std::string name;
        for (char ch : raw_name) {
          if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
          if (ch == ' ' && (name.empty() || name.back() == ' ')) continue;
          name += ch;
        }
        while (!name.empty() && name.back() == ' ') name.pop_back();
        // "Lizard Rock - North -", "Future Fate- Lachesis -": drop the subtitle between the dashes.
        if (name.ends_with("-")) {
          size_t cut = name.rfind(" - ", name.size() - 2);
          if (cut == std::string::npos) cut = name.rfind("- ", name.size() - 2);
          if (cut != std::string::npos && cut > 0) name.resize(cut);
          while (!name.empty() && name.back() == ' ') name.pop_back();
        }
        std::printf("%-8s msg %2d  %-30s -> %4d %s\n", room_name.c_str(), msg, name.c_str(), target, stem ? stem : "?");
        if (stem && !name.empty() && !std::string_view(stem).starts_with("wld_")) {
          by_stem.try_emplace(stem, name);
          auto& prefix_names = by_prefix[std::string(stem).substr(0, 3)];
          bool found = false;
          for (auto& [prefix_name, marker_count] : prefix_names)
            if (prefix_name == name) ++marker_count, found = true;
          if (!found) prefix_names.push_back({name, 1});
        }
        msg = -1;
        label = false;
      }
    }
  }
  std::string out;
  for (const auto& [stem, name] : by_stem) out += "{\"" + stem + "\", \"" + name + "\"},\n";
  for (const auto& [prefix, names] : by_prefix) {
    std::string best;
    int best_k = 0;
    for (const auto& [candidate, marker_count] : names)
      if (marker_count > best_k || (marker_count == best_k && candidate.size() < best.size())) best = candidate, best_k = marker_count;
    std::printf("%s = %s\n", prefix.c_str(), best.c_str());
    out += "{\"" + prefix + "\", \"" + best + "\"},\n";
  }
  if (!inc.empty()) {
    std::ofstream file(inc, std::ios::binary);
    file << out;
    std::printf("world-markers: wrote %s\n", inc.c_str());
  }
  return 0;
}

// Rest-pose position of every joint of a model, in shares of the joints' vertical extent, the number of mesh vertices bound to it and their
// centre (the joint's position without vertices): where a joint's piece lies when joints share one point.
struct RigJointRow {
  std::array<float, 3> pos{}, centre{};
  int vertices = 0;
};
std::vector<RigJointRow> rig_rows(const assets::ModelAsset& model) {
  std::vector<RigJointRow> rows(model.skeleton.joints.size());
  std::vector<assets::Mat3> rot(rows.size());
  float low = 0.0f, high = 0.0f;
  for (size_t j = 0; j < rows.size(); ++j) {
    const assets::Joint& joint = model.skeleton.joints[j];
    const assets::Mat3 local = assets::rotation_zyx(joint.rest_rot_deg);
    if (joint.parent < 0) {
      rot[j] = local;
      rows[j].pos = joint.rest_trans;
    } else {
      const size_t parent = static_cast<size_t>(joint.parent);
      const assets::Mat3& up = rot[parent];
      for (size_t row = 0; row < 3; ++row) {
        rows[j].pos[row] = rows[parent].pos[row] + up[row * 3] * joint.rest_trans[0] + up[row * 3 + 1] * joint.rest_trans[1] + up[row * 3 + 2] * joint.rest_trans[2];
        for (size_t col = 0; col < 3; ++col) rot[j][row * 3 + col] = up[row * 3] * local[col] + up[row * 3 + 1] * local[3 + col] + up[row * 3 + 2] * local[6 + col];
      }
    }
    low = j == 0 ? rows[j].pos[1] : std::min(low, rows[j].pos[1]);
    high = j == 0 ? rows[j].pos[1] : std::max(high, rows[j].pos[1]);
  }
  const float height = high > low ? high - low : 1.0f;
  for (RigJointRow& row : rows)
    for (float& axis : row.pos) axis /= height;
  for (const assets::Mesh& mesh : model.meshes)
    for (const assets::SkinVertex& vertex : mesh.pool) {
      if (vertex.joint[0] >= rows.size()) continue;
      RigJointRow& row = rows[vertex.joint[0]];
      ++row.vertices;
      for (size_t axis = 0; axis < 3; ++axis) row.centre[axis] += vertex.bind_pos[axis] / height;
    }
  for (RigJointRow& row : rows)
    for (size_t axis = 0; axis < 3; ++axis) row.centre[axis] = row.vertices > 0 ? row.centre[axis] / static_cast<float>(row.vertices) : row.pos[axis];
  return rows;
}

// The model's joint matrices at the first keyframe of its stand-by clip (clip 1; the rest pose without one).
std::vector<render::Mat4> stand_palette(const assets::ModelAsset& model) {
  const assets::AnimationClip* clip = model.clips.size() > 1 ? &model.clips[1] : nullptr;
  std::vector<render::Mat4> out;
  for (size_t j = 0; j < model.skeleton.joints.size(); ++j) {
    const assets::Joint& joint = model.skeleton.joints[j];
    assets::Vec3 rot = joint.rest_rot_deg, trans = joint.rest_trans;
    if (clip && j < clip->tracks.size()) {
      if (!clip->tracks[j].rot.empty()) rot = clip->tracks[j].rot[0].v;
      if (!clip->tracks[j].trans.empty()) trans = clip->tracks[j].trans[0].v;
    }
    const render::Mat4 local = render::mat4_from_rt(assets::rotation_zyx(rot), trans);
    out.push_back(joint.parent >= 0 ? out[static_cast<size_t>(joint.parent)] * local : local);
  }
  return out;
}

// How the posed menu model follows the field model in its stand pose: per paired bone the angle between the field bone and its menu bone
// (mean, worst), and the angle between the two models' root-to-head lines (the lean).
struct PoseAgreement {
  float mean = 0.0f, worst = 0.0f, lean = 0.0f;
};
PoseAgreement pose_agreement(const assets::ModelAsset& field, const game::MenuRig& rig) {
  const std::vector<render::Mat4> field_palette = stand_palette(field);
  std::vector<render::Mat4> menu_palette;
  game::menu_rig_palette(rig, field_palette, menu_palette);
  std::vector<int> partner(field_palette.size(), -1);
  for (size_t k = 0; k < rig.field_joint.size(); ++k)
    if (rig.field_joint[k] >= 0) partner[static_cast<size_t>(rig.field_joint[k])] = static_cast<int>(k);
  auto angle = [](const render::Mat4& from_a, const render::Mat4& to_a, const render::Mat4& from_b, const render::Mat4& to_b, float& out) {
    float a[3], b[3], dot = 0.0f, length_a = 0.0f, length_b = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
      a[axis] = to_a.at(axis, 3) - from_a.at(axis, 3), b[axis] = to_b.at(axis, 3) - from_b.at(axis, 3);
      dot += a[axis] * b[axis], length_a += a[axis] * a[axis], length_b += b[axis] * b[axis];
    }
    if (length_a < 400.0f || length_b < 100.0f) return false;
    out = std::acos(std::clamp(dot / std::sqrt(length_a * length_b), -1.0f, 1.0f)) * 180.0f / 3.14159265f;
    return true;
  };
  PoseAgreement result;
  int bones = 0;
  for (size_t i = 0; i < field_palette.size(); ++i) {
    const int parent = field.skeleton.joints[i].parent;
    if (parent < 0 || partner[i] < 0 || partner[static_cast<size_t>(parent)] < 0) continue;
    float degrees = 0.0f;
    if (!angle(field_palette[static_cast<size_t>(parent)], field_palette[i], menu_palette[static_cast<size_t>(partner[static_cast<size_t>(parent)])],
               menu_palette[static_cast<size_t>(partner[i])], degrees))
      continue;
    result.mean += degrees, result.worst = std::max(result.worst, degrees), ++bones;
  }
  if (bones > 0) result.mean /= static_cast<float>(bones);
  if (field.attach.points.size() > 1) {
    const size_t head = field.attach.points[1].joint;
    if (head < partner.size() && partner[head] >= 0)
      angle(field_palette[0], field_palette[head], menu_palette[0], menu_palette[static_cast<size_t>(partner[head])], result.lean);
  }
  return result;
}

// A field model and the detailed body [Enhancements] MenuModelsInField draws for it: every playable character's (menu model, by model byte)
// and every story NPC's of game::npc_bodies (battle model, with the first room copy of its field model). `only` >= 0 keeps the one with that
// battle model number.
struct RigPair {
  int body = -1;
  std::string field_name;
  std::optional<assets::ModelAsset> field, menu;
  std::string err;
};

std::vector<RigPair> rig_pairs(vfs::Vfs& fs, const game::DiscIndex& discs, bool hd, int only) {
  std::vector<RigPair> pairs;
  for (int model = 0; model <= game::kLastMenuModel; ++model) {
    const std::string field_stem = discs.field_model_stem(1, model);
    if (field_stem.empty() || (only >= 0 && model != only)) continue;
    RigPair pair{model, field_stem};
    pair.field = game::read_model_files(fs, "fieldobj/player/" + field_stem, &pair.err);
    pair.menu = game::read_menu_model(fs, model, hd, &pair.err);
    pairs.push_back(std::move(pair));
  }
  std::vector<game::NpcBody> wanted;
  for (const game::NpcBody& npc : game::npc_bodies())
    if (only < 0 || npc.battle_model == only) wanted.push_back(npc);
  for (const auto& room_name : list_rooms(fs)) {
    if (wanted.empty()) break;
    auto bin = fs.read(room_path(room_name, ".bin"));
    auto ctd = fs.read(room_path(room_name, ".ctd"));
    if (!bin || !ctd) continue;
    std::string err;
    const auto models = import::load_room_models(*bin, *ctd, &err);
    for (size_t k = 0; models && k < models->size(); ++k) {
      auto model = game::build_room_model((*models)[k], &err);
      if (!model) continue;
      const u64 fingerprint = game::body_fingerprint(*model);
      const auto found = std::ranges::find(wanted, fingerprint, &game::NpcBody::fingerprint);
      if (found == wanted.end()) continue;
      RigPair pair{found->battle_model, std::format("{}:{}", room_name, k)};
      pair.field = std::move(model);
      pair.menu = game::read_body_model(fs, found->battle_model, hd, &pair.err);
      pairs.push_back(std::move(pair));
      wanted.erase(found);
    }
  }
  return pairs;
}

// occ menu-rigs [--hd] [byte]: for every menu model, the field model of the same character model byte and how well their skeletons match
// (game/menu_rig.h): joint counts, unmatched field joints (with children), mean joint distance (share of the height), bone angles, the
// pairing strategy kept (under / beside / full = BesideWhenFull), scale, and in the stand pose the mean / worst bone angle between the
// field and the posed menu model and the lean of the root-to-head line.
// With a byte: every menu joint of that pair, its parent, its field partner, rest positions (% of the height) and bound vertex counts.
int cmd_menu_rigs(vfs::Vfs& fs, bool hd, int only) {
  fs.set_hd_mode(hd);
  game::DiscIndex discs;
  if (!discs.load(fs)) {
    std::fprintf(stderr, "menu-rigs: no disc index\n");
    return 1;
  }
  std::printf("%-4s %-18s %-9s %3s %3s %9s %7s %6s %6s %-6s %6s %6s %6s %6s  %s\n", "byte", "field", "menu", "fJ", "mJ", "unmatched", "offset%", "p90",
              "max", "plan", "scale", "pose", "worst", "lean", "usable");
  float lean_sum = 0.0f, pose_sum = 0.0f;
  constexpr const char* kPlanNames[4] = {"under", "beside", "full", "map"};
  int usable = 0, total = 0;
  for (const RigPair& pair : rig_pairs(fs, discs, hd, only)) {
    const int model = pair.body;
    const std::string& field_stem = pair.field_name;
    const auto& field = pair.field;
    auto menu = pair.menu;
    if (!field || !menu) {
      std::printf("%-4X %-18s %-9s %s\n", model, field_stem.c_str(), game::body_model_stem(model).c_str(), pair.err.c_str());
      continue;
    }
    const game::MenuRig rig = game::match_body_rig(*field, *menu, model);
    ++total;
    usable += rig.usable ? 1 : 0;
    const PoseAgreement pose = pose_agreement(*field, rig);
    if (rig.usable) lean_sum += pose.lean, pose_sum += pose.mean;
    std::printf("%-4X %-18s %-9s %3zu %3zu %5d (%d) %7.1f %6.1f %6.1f %-6s %6.2f %6.1f %6.1f %6.1f  %s\n", model, field_stem.c_str(),
                game::body_model_stem(model).c_str(), field->skeleton.joints.size(), menu->skeleton.joints.size(), rig.unmatched, rig.unmatched_inner,
                rig.mean_offset * 100.0f, rig.bone_p90, rig.bone_max, kPlanNames[static_cast<int>(rig.plan)], rig.scale, pose.mean, pose.worst, pose.lean,
                rig.usable ? "yes" : "no");
    if (only < 0) continue;
    const std::vector<RigJointRow> menu_rows = rig_rows(*menu), field_rows = rig_rows(*field);
    std::printf("menu joint  parent  field  menu pos %%            verts  vertex centre %%        field pos %%           verts\n");
    for (size_t j = 0; j < menu_rows.size(); ++j) {
      const int partner = rig.field_joint[j];
      std::printf("%5zu %7d %6d  %6.1f %6.1f %6.1f  %5d  %6.1f %6.1f %6.1f", j, menu->skeleton.joints[j].parent, partner, menu_rows[j].pos[0] * 100.0f,
                  menu_rows[j].pos[1] * 100.0f, menu_rows[j].pos[2] * 100.0f, menu_rows[j].vertices, menu_rows[j].centre[0] * 100.0f,
                  menu_rows[j].centre[1] * 100.0f, menu_rows[j].centre[2] * 100.0f);
      if (partner >= 0) {
        const RigJointRow& row = field_rows[static_cast<size_t>(partner)];
        std::printf("   %6.1f %6.1f %6.1f  %5d", row.pos[0] * 100.0f, row.pos[1] * 100.0f, row.pos[2] * 100.0f, row.vertices);
      }
      std::printf("\n");
    }
    std::printf("field joint  parent  field pos %%            verts  vertex centre %%       menu joints\n");
    for (size_t i = 0; i < field_rows.size(); ++i) {
      std::printf("%5zu %7d  %6.1f %6.1f %6.1f  %5d  %6.1f %6.1f %6.1f ", i, field->skeleton.joints[i].parent, field_rows[i].pos[0] * 100.0f,
                  field_rows[i].pos[1] * 100.0f, field_rows[i].pos[2] * 100.0f, field_rows[i].vertices, field_rows[i].centre[0] * 100.0f,
                  field_rows[i].centre[1] * 100.0f, field_rows[i].centre[2] * 100.0f);
      for (size_t j = 0; j < rig.field_joint.size(); ++j)
        if (rig.field_joint[j] == static_cast<int>(i)) std::printf(" %zu", j);
      std::printf("\n");
    }
  }
  std::printf("menu-rigs: %d of %d usable; over the usable ones, mean stand-pose bone angle %.1f, mean lean %.1f degrees\n", usable, total,
              usable > 0 ? pose_sum / static_cast<float>(usable) : 0.0f, usable > 0 ? lean_sum / static_cast<float>(usable) : 0.0f);
  return 0;
}

// occ menu-rigs --audit [--hd] [byte]: how each menu model follows its field model over every field clip (game/menu_rig.h). Per character:
// the worst drift of a paired bone (the angle between the field bone and its menu bone at a keyframe, minus that angle in the rest pose), its
// menu joint and clip; then the region flags: a menu joint carrying mesh whose driver (its field partner, else its nearest paired ancestor's)
// is not the field joint whose mesh lies nearest to its own (by vertex centre, shares of the height), nor that joint's parent or child, with
// the distance between the two centres.
int cmd_menu_rig_audit(vfs::Vfs& fs, bool hd, int only) {
  constexpr float kMinField = 20.0f, kMinMenu = 10.0f, kDegrees = 180.0f / 3.14159265f;
  fs.set_hd_mode(hd);
  game::DiscIndex discs;
  if (!discs.load(fs)) {
    std::fprintf(stderr, "menu-rigs: no disc index\n");
    return 1;
  }
  int flagged = 0;
  for (const RigPair& pair : rig_pairs(fs, discs, hd, only)) {
    const int model = pair.body;
    const auto& field = pair.field;
    auto menu = pair.menu;
    if (!field || !menu) continue;
    const game::MenuRig rig = game::match_body_rig(*field, *menu, model);
    std::vector<int> partner(field->skeleton.joints.size(), -1);
    for (size_t k = 0; k < rig.field_joint.size(); ++k)
      if (rig.field_joint[k] >= 0 && partner[static_cast<size_t>(rig.field_joint[k])] < 0) partner[static_cast<size_t>(rig.field_joint[k])] = static_cast<int>(k);
    game::UnitPose pose;
    pose.set_model(&*field);
    std::vector<render::Mat4> field_palette, menu_palette;
    auto bone_angle = [&](size_t i) -> float {
      const int parent = field->skeleton.joints[i].parent;
      if (parent < 0 || partner[i] < 0 || partner[static_cast<size_t>(parent)] < 0) return -1.0f;
      const size_t k = static_cast<size_t>(partner[i]), menu_parent = static_cast<size_t>(partner[static_cast<size_t>(parent)]);
      float a[3], b[3], dot = 0.0f, length_a = 0.0f, length_b = 0.0f;
      for (int axis = 0; axis < 3; ++axis) {
        a[axis] = field_palette[i].at(axis, 3) - field_palette[static_cast<size_t>(parent)].at(axis, 3);
        b[axis] = (menu_palette[k].at(axis, 3) - menu_palette[menu_parent].at(axis, 3)) * rig.scale;
        dot += a[axis] * b[axis], length_a += a[axis] * a[axis], length_b += b[axis] * b[axis];
      }
      if (length_a < kMinField * kMinField || length_b < kMinMenu * kMinMenu) return -1.0f;
      return std::acos(std::clamp(dot / std::sqrt(length_a * length_b), -1.0f, 1.0f)) * kDegrees;
    };
    pose.palette_at(-1, 0.0f, field_palette);
    game::menu_rig_palette(rig, field_palette, menu_palette);
    std::vector<float> rest(field_palette.size(), -1.0f);
    for (size_t i = 0; i < rest.size(); ++i) rest[i] = bone_angle(i);
    float worst = 0.0f;
    std::string worst_where = "-";
    for (size_t index = 0; index < field->clips.size(); ++index) {
      const assets::AnimationClip& clip = field->clips[index];
      for (u32 key = 0; key < clip.frame_count; ++key) {
        pose.palette_at(static_cast<int>(index), static_cast<float>(key), field_palette);
        game::menu_rig_palette(rig, field_palette, menu_palette);
        for (size_t i = 0; i < field_palette.size(); ++i) {
          const float now = bone_angle(i);
          if (now < 0.0f || rest[i] < 0.0f || std::fabs(now - rest[i]) <= worst) continue;
          worst = std::fabs(now - rest[i]);
          worst_where = std::format("menu joint {} (field {}) in {} key {}", partner[i], i, clip.name, key);
        }
      }
    }
    std::printf("%-4X %-9s plan %-6s drift %5.1f  %s\n", model, game::body_model_stem(model).c_str(),
                rig.plan == game::MenuRigPlan::Map ? "map" : "auto", worst, worst_where.c_str());
    const std::vector<RigJointRow> menu_rows = rig_rows(*menu), field_rows = rig_rows(*field);
    int total = 0;
    for (const RigJointRow& row : menu_rows) total += row.vertices;
    for (size_t j = 0; j < menu_rows.size(); ++j) {
      if (menu_rows[j].vertices * 50 < total) continue;
      int driver = -1;
      for (int at = static_cast<int>(j); at >= 0 && driver < 0; at = menu->skeleton.joints[static_cast<size_t>(at)].parent) driver = rig.field_joint[static_cast<size_t>(at)];
      size_t nearest = 0;
      float best = 1e9f;
      for (size_t i = 0; i < field_rows.size(); ++i) {
        if (field_rows[i].vertices == 0) continue;
        float distance = 0.0f;
        for (size_t axis = 0; axis < 3; ++axis) distance += (field_rows[i].centre[axis] - menu_rows[j].centre[axis]) * (field_rows[i].centre[axis] - menu_rows[j].centre[axis]);
        if (distance < best) best = distance, nearest = i;
      }
      if (driver < 0) continue;
      const int nearest_parent = field->skeleton.joints[nearest].parent, driver_parent = field->skeleton.joints[static_cast<size_t>(driver)].parent;
      if (static_cast<int>(nearest) == driver || nearest_parent == driver || driver_parent == static_cast<int>(nearest)) continue;
      ++flagged;
      std::printf("     region: menu joint %zu (%d verts, %s) is driven by field %d, nearest field mesh is joint %zu (%.1f%% away)\n", j, menu_rows[j].vertices,
                  rig.field_joint[j] >= 0 ? "paired" : "rides its parent", driver, nearest, std::sqrt(best) * 100.0f);
    }
  }
  std::printf("menu-rigs --audit: %d region flags\n", flagged);
  return 0;
}

// occ menu-rigs --rooms [--hd]: which room models are a playable character's field model (the same skeleton and mesh, game::same_skeleton /
// same_mesh), so MenuModelsInField also draws those NPCs with the character's menu model; per character byte the room models with the same
// skeleton and mesh, with the same skeleton but another mesh (another person on the character's rig: not taken), and with the field
// model's joint count and parents but other rest values (not taken).
int cmd_menu_rig_rooms(vfs::Vfs& fs, bool hd) {
  fs.set_hd_mode(hd);
  game::DiscIndex discs;
  if (!discs.load(fs)) {
    std::fprintf(stderr, "menu-rigs: no disc index\n");
    return 1;
  }
  std::vector<std::optional<assets::Skeleton>> skeletons(game::kLastMenuModel + 1);
  std::vector<std::optional<assets::ModelAsset>> fields(skeletons.size());
  for (int model = 0; model <= game::kLastMenuModel; ++model)
    if (const std::string stem = discs.field_model_stem(1, model); !stem.empty()) {
      skeletons[static_cast<size_t>(model)] = game::read_field_skeleton(fs, stem);
      std::string err;
      fields[static_cast<size_t>(model)] = game::read_model_files(fs, "fieldobj/player/" + stem, &err);
    }
  std::vector<int> found(skeletons.size(), 0), other_mesh(skeletons.size(), 0), shaped(skeletons.size(), 0);
  std::vector<std::string> first(skeletons.size());
  int rooms = 0, models = 0, characters = 0, ambiguous = 0;
  for (const auto& room_name : list_rooms(fs)) {
    auto bin = fs.read(room_path(room_name, ".bin"));
    auto ctd = fs.read(room_path(room_name, ".ctd"));
    if (!bin || !ctd) continue;
    std::string err;
    auto scene = import::load_room(room_name, *bin, *ctd, {}, &err);
    if (!scene) continue;
    ++rooms;
    for (size_t k = 0; k < scene->models.size(); ++k) {
      const auto skeleton = import::load_mdl_skeleton(scene->models[k].mdl);
      if (!skeleton) continue;
      ++models;
      int hits = 0;
      std::optional<assets::ModelAsset> room_asset;
      for (size_t model = 0; model < skeletons.size(); ++model) {
        if (!skeletons[model]) continue;
        if (game::same_skeleton(*skeleton, *skeletons[model])) {
          if (!room_asset) room_asset = import::load_room_model(scene->models[k].mdl, scene->models[k].page_textures);
          if (!room_asset || !fields[model] || !game::same_mesh(*room_asset, *fields[model])) {
            ++other_mesh[model];
            continue;
          }
          ++hits;
          if (found[model]++ == 0) first[model] = std::format("{} model {}", room_name, k);
          continue;
        }
        bool parents = skeleton->joints.size() == skeletons[model]->joints.size();
        for (size_t j = 0; parents && j < skeleton->joints.size(); ++j) parents = skeleton->joints[j].parent == skeletons[model]->joints[j].parent;
        if (parents) ++shaped[model];
      }
      characters += hits > 0 ? 1 : 0;
      ambiguous += hits > 1 ? 1 : 0;
    }
  }
  std::printf("%-4s %-9s %8s %10s %14s  %s\n", "byte", "menu", "same", "other mesh", "parents only", "first");
  for (size_t model = 0; model < skeletons.size(); ++model)
    if (skeletons[model])
      std::printf("%-4zX %-9s %8d %10d %14d  %s\n", model, game::menu_model_stem(static_cast<int>(model)).c_str(), found[model], other_mesh[model],
                  shaped[model], first[model].c_str());
  std::printf("menu-rigs --rooms: %d rooms, %d room models, %d that are a character's field model (%d matching more than one)\n", rooms, models,
              characters, ambiguous);
  // The field model bytes past the menu models (disguises and other bodies): the character whose field model each one copies, if any.
  for (int byte = game::kLastMenuModel + 1; byte < 0x100; ++byte) {
    const std::string stem = discs.field_model_stem(1, byte);
    if (stem.empty()) continue;
    std::string err;
    const auto body = game::read_model_files(fs, "fieldobj/player/" + stem, &err);
    std::string copies = "none";
    for (size_t model = 0; body && model < skeletons.size(); ++model)
      if (skeletons[model] && fields[model] && game::same_skeleton(body->skeleton, *skeletons[model]) && game::same_mesh(*body, *fields[model])) {
        copies = std::format("{:X} {}", model, game::menu_model_stem(static_cast<int>(model)));
        break;
      }
    std::printf("field model byte %X: %s, copies %s\n", byte, stem.c_str(), copies.c_str());
  }
  return 0;
}

// Height over width of a model's rest-pose vertices (0 without any).
float body_aspect(const assets::ModelAsset& model) {
  float low[3] = {1e9f, 1e9f, 1e9f}, high[3] = {-1e9f, -1e9f, -1e9f};
  for (const auto& mesh : model.meshes)
    for (const auto& vertex : mesh.pool)
      for (size_t axis = 0; axis < 3; ++axis) {
        low[axis] = std::min(low[axis], vertex.bind_pos[axis]);
        high[axis] = std::max(high[axis], vertex.bind_pos[axis]);
      }
  const float width = high[0] - low[0];
  return width > 0 ? (high[1] - low[1]) / width : 0.0f;
}

size_t body_triangles(const assets::ModelAsset& model) {
  size_t triangles = 0;
  for (const auto& mesh : model.meshes) triangles += mesh.indices.size() / 3;
  return triangles;
}

// A model's colours: per textured triangle the texel under the centre of its texture coordinates, binned to four levels a channel, as shares
// of those triangles.
constexpr size_t kColourBins = 64;
std::array<float, kColourBins> body_colours(const assets::ModelAsset& model) {
  std::array<float, kColourBins> bins{};
  float total = 0;
  for (const auto& mesh : model.meshes)
    for (const auto& part : mesh.submeshes) {
      if (part.material >= mesh.materials.size()) continue;
      const int texture = mesh.materials[part.material].texture;
      if (texture < 0 || static_cast<size_t>(texture) >= model.textures.size()) continue;
      const import::Image& image = model.textures[static_cast<size_t>(texture)];
      if (image.indexed || image.width == 0 || image.height == 0 || image.pixels.size() < size_t{image.width} * image.height * 4) continue;
      for (u32 i = part.first_index; i + 2 < part.first_index + part.index_count && i + 2 < mesh.indices.size(); i += 3) {
        float u = 0, v = 0;
        for (u32 corner = 0; corner < 3; ++corner) {
          const auto& vertex = mesh.vertices[mesh.indices[i + corner]];
          u += vertex.uv[0] / 3.0f;
          v += vertex.uv[1] / 3.0f;
        }
        const u32 x = std::min(image.width - 1, static_cast<u32>(std::clamp(u, 0.0f, 1.0f) * static_cast<float>(image.width)));
        const u32 y = std::min(image.height - 1, static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * static_cast<float>(image.height)));
        const u8* texel = &image.pixels[(size_t{y} * image.width + x) * 4];
        if (texel[3] == 0) continue;
        bins[(texel[0] >> 6) * 16 + (texel[1] >> 6) * 4 + (texel[2] >> 6)] += 1;
        total += 1;
      }
    }
  if (total > 0)
    for (float& bin : bins) bin /= total;
  return bins;
}

float colour_overlap(const std::array<float, kColourBins>& lhs, const std::array<float, kColourBins>& rhs) {
  float overlap = 0;
  for (size_t i = 0; i < kColourBins; ++i) overlap += std::min(lhs[i], rhs[i]);
  return overlap;
}

// occ menu-rigs --npcs [--hd]: for every monster battle model with at least kMinNpcJoints joints, the room models it may be the battle body
// of, grouped by game::body_fingerprint (each with its joints, triangles, height over width, colour overlap, the number of rooms it is in
// and one of them): those in the most rooms where one of the model's enemy names speaks (a message opening with "Name:"), then those whose
// colours (body_colours) overlap the battle model's most. What the NPC bodies of [Enhancements] MenuModelsInField are chosen from.
int cmd_menu_rig_npcs(vfs::Vfs& fs, bool hd) {
  constexpr size_t kMinNpcJoints = 12, kShown = 3;
  fs.set_hd_mode(hd);
  data::GameTables tables;
  std::string err;
  if (!tables.load(fs, &err)) {
    std::fprintf(stderr, "menu-rigs: tables: %s\n", err.c_str());
    return 1;
  }
  struct Body {
    size_t joints = 0, triangles = 0;
    float aspect = 0;
    std::array<float, kColourBins> colours{};
    std::string room;
    size_t index = 0;
    int rooms = 0;
  };
  std::map<u64, Body> bodies;
  std::map<std::string, std::vector<u64>> room_bodies;
  for (const auto& room_name : list_rooms(fs)) {
    auto bin = fs.read(room_path(room_name, ".bin"));
    auto ctd = fs.read(room_path(room_name, ".ctd"));
    if (!bin || !ctd) continue;
    const auto models = import::load_room_models(*bin, *ctd, &err);
    if (!models) continue;
    std::vector<u64>& seen = room_bodies[room_name];
    for (size_t k = 0; k < models->size(); ++k) {
      const auto model = game::build_room_model((*models)[k], &err);
      if (!model || model->skeleton.joints.size() < kMinNpcJoints) continue;
      const u64 fingerprint = game::body_fingerprint(*model);
      Body& body = bodies[fingerprint];
      if (body.rooms == 0)
        body = {model->skeleton.joints.size(), body_triangles(*model), body_aspect(*model), body_colours(*model), room_name, k, 0};
      if (std::ranges::find(seen, fingerprint) == seen.end()) {
        seen.push_back(fingerprint);
        ++body.rooms;
      }
    }
  }
  import::LangTables lang;
  lang.init(&fs);
  std::map<std::string, std::vector<std::string>> speaking;
  for (int id = 0; id < 2000; ++id) {
    const char* room = import::room_name_for_id(id);
    if (!room) continue;
    for (int msg = 0; msg < 400; ++msg) {
      const std::string* row = lang.raw_field(id, msg, import::Lang::En);
      if (!row) continue;
      // The rows write a line break as the two characters \n; markup closes with '}'.
      const size_t colon = row->find(":\\n");
      if (colon == std::string::npos || colon == 0 || colon > 40) continue;
      const size_t brace = row->rfind('}', colon);
      const size_t start = brace == std::string::npos ? 0 : brace + 1;
      auto& rooms = speaking[row->substr(start, colon - start)];
      if (std::ranges::find(rooms, room) == rooms.end()) rooms.emplace_back(room);
    }
  }
  std::map<size_t, std::vector<std::string>> names;
  for (size_t enemy = 0; enemy < tables.enemies.size(); ++enemy) {
    const size_t model = tables.enemies[enemy].model();
    if (model >= game::kBattleModels.size() || !std::string_view(game::kBattleModels[model]).starts_with("battle/monster/")) continue;
    auto& list = names[model];
    const std::string name = tables.enemy_name(static_cast<int>(enemy));
    if (!name.empty() && std::ranges::find(list, name) == list.end()) list.push_back(name);
  }
  const auto print_body = [&](u64 fingerprint, const std::array<float, kColourBins>& colours, const char* why) {
    const Body& body = bodies[fingerprint];
    std::printf("  %-8s %016llX: %zu joints, %zu triangles, height/width %.2f, colours %.2f, in %d rooms (%s model %zu)\n", why,
                static_cast<unsigned long long>(fingerprint), body.joints, body.triangles, body.aspect, colour_overlap(colours, body.colours),
                body.rooms, body.room.c_str(), body.index);
  };
  for (const auto& [model, list] : names) {
    const std::string stem(game::kBattleModels[model]);
    const auto battle = game::read_model_files(fs, stem, &err);
    if (!battle || battle->skeleton.joints.size() < kMinNpcJoints) continue;
    const auto colours = body_colours(*battle);
    std::string label;
    std::vector<std::string> rooms;
    for (const std::string& name : list) {
      label += (label.empty() ? "" : ", ") + name;
      if (const auto it = speaking.find(name); it != speaking.end())
        for (const std::string& room : it->second)
          if (std::ranges::find(rooms, room) == rooms.end()) rooms.push_back(room);
    }
    std::printf("%zu %s (%s): %zu joints, %zu triangles, height/width %.2f; speaks in %zu rooms\n", model,
                stem.substr(stem.rfind('/') + 1).c_str(), label.c_str(), battle->skeleton.joints.size(), body_triangles(*battle),
                body_aspect(*battle), rooms.size());
    std::map<u64, int> counts;
    for (const std::string& room : rooms)
      for (const u64 fingerprint : room_bodies[room]) ++counts[fingerprint];
    std::vector<std::pair<int, u64>> by_rooms;
    for (const auto& [fingerprint, count] : counts) by_rooms.emplace_back(count, fingerprint);
    std::ranges::sort(by_rooms, std::greater{});
    for (size_t i = 0; i < by_rooms.size() && i < kShown; ++i)
      print_body(by_rooms[i].second, colours, std::format("{} rooms", by_rooms[i].first).c_str());
    std::vector<std::pair<float, u64>> by_colour;
    for (const auto& [fingerprint, body] : bodies) by_colour.emplace_back(colour_overlap(colours, body.colours), fingerprint);
    std::ranges::sort(by_colour, std::greater{});
    for (size_t i = 0; i < by_colour.size() && i < kShown; ++i) print_body(by_colour[i].second, colours, "colour");
  }
  return 0;
}

// occ room-models [room]: imports every room model (subfiles 11+) of the room(s) and reports failures.
int cmd_room_models(const vfs::Vfs& fs, const std::string& only) {
  size_t rooms_n = 0, models = 0, failed = 0, no_tex = 0, clips = 0;
  for (const auto& room_name : list_rooms(fs)) {
    if (!only.empty() && room_name != only) continue;
    auto bin = fs.read(room_path(room_name, ".bin"));
    auto ctd = fs.read(room_path(room_name, ".ctd"));
    if (!bin || !ctd) continue;
    std::string err;
    auto scene = import::load_room(room_name, *bin, *ctd, {}, &err);
    if (!scene) {
      std::printf("%-14s room failed: %s\n", room_name.c_str(), err.c_str());
      ++failed;
      continue;
    }
    ++rooms_n;
    for (size_t k = 0; k < scene->models.size(); ++k) {
      const auto& room_model = scene->models[k];
      ++models;
      if (room_model.texture.pixels.empty()) ++no_tex;
      auto asset = import::load_room_model(room_model.mdl, room_model.page_textures, &err);
      if (!asset) {
        ++failed;
        std::printf("%-14s model %zu: %s\n", room_name.c_str(), k, err.c_str());
        continue;
      }
      clips += asset->clips.size();
      if (!only.empty()) {
        std::printf("%s model %zu: %zu joints, %zu constructs, %zu clips, texture %ux%u\n", room_name.c_str(), k, asset->skeleton.joints.size(),
                    asset->meshes.size(), asset->clips.size(), room_model.texture.width, room_model.texture.height);
        // construct bounds (bind pose), S3 shading flags and clip lengths: what a model is before a script poses it
        for (size_t construct = 0; construct < asset->meshes.size(); ++construct) {
          float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
          for (const auto& vertex : asset->meshes[construct].pool) {
            const auto& p3 = vertex.bind_pos;
            for (int axis = 0; axis < 3; ++axis) lo[axis] = std::min(lo[axis], p3[axis]), hi[axis] = std::max(hi[axis], p3[axis]);
          }
          std::printf("    construct %zu: %zu tris, bind x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f, shading flags 0x%02X\n", construct,
                      asset->meshes[construct].indices.size() / 3, static_cast<double>(lo[0]), static_cast<double>(hi[0]), static_cast<double>(lo[1]),
                      static_cast<double>(hi[1]), static_cast<double>(lo[2]), static_cast<double>(hi[2]), construct < asset->shading.size() ? asset->shading[construct].flags : 0);
        }
        for (size_t j = 0; j < asset->skeleton.joints.size() && j < 3; ++j) {
          const auto& jt = asset->skeleton.joints[j];
          std::printf("    joint %zu: parent %d rest rot (%.0f %.0f %.0f) trans (%.0f %.0f %.0f)\n", j, jt.parent, static_cast<double>(jt.rest_rot_deg[0]),
                      static_cast<double>(jt.rest_rot_deg[1]), static_cast<double>(jt.rest_rot_deg[2]), static_cast<double>(jt.rest_trans[0]),
                      static_cast<double>(jt.rest_trans[1]), static_cast<double>(jt.rest_trans[2]));
        }
        for (const auto& cl : asset->clips) {
          std::printf("    clip %s: %u frames", cl.name.c_str(), cl.frame_count);
          if (!cl.tracks.empty() && !cl.tracks[0].trans.empty() && !cl.tracks[0].rot.empty())
            std::printf("; joint 0 key 0 trans (%.0f %.0f %.0f) rot (%.0f %.0f %.0f)", static_cast<double>(cl.tracks[0].trans[0].v[0]),
                        static_cast<double>(cl.tracks[0].trans[0].v[1]), static_cast<double>(cl.tracks[0].trans[0].v[2]),
                        static_cast<double>(cl.tracks[0].rot[0].v[0]), static_cast<double>(cl.tracks[0].rot[0].v[1]),
                        static_cast<double>(cl.tracks[0].rot[0].v[2]));
          std::printf("\n");
          if (std::getenv("OCC_JOINTS") && cl.frame_count <= 1)
            for (size_t j = 0; j < cl.tracks.size(); ++j)
              if (!cl.tracks[j].rot.empty() && !cl.tracks[j].trans.empty())
                std::printf("      j%zu parent %d rot (%.0f %.0f %.0f) trans (%.0f %.0f %.0f)\n", j, asset->skeleton.joints[j].parent,
                            static_cast<double>(cl.tracks[j].rot[0].v[0]), static_cast<double>(cl.tracks[j].rot[0].v[1]),
                            static_cast<double>(cl.tracks[j].rot[0].v[2]), static_cast<double>(cl.tracks[j].trans[0].v[0]),
                            static_cast<double>(cl.tracks[j].trans[0].v[1]), static_cast<double>(cl.tracks[j].trans[0].v[2]));
        }
      }
    }
  }
  std::printf("rooms: %zu  room models: %zu  failures: %zu  without texture: %zu  clips: %zu\n", rooms_n, models, failed, no_tex, clips);
  return failed ? 1 : 0;
}
}  // namespace

// ---------------------------------------------------------------- rooms-check: initial-frame coverage
// Software composite of every group image that would be visible at room start (all groups on: the default mask is 0 hidden bits;
// script BG_LAYER ops are not run here) into a 320x240 viewport, at scroll S = 0 (centred) and the 8 pan-limit extremes, with the
// per-group parallax off = (S << 12) / scale; the y_min-24 overscan of the clamp is ignored. Reports rooms whose worst-case coverage is < 90 %.
double viewport_coverage(const assets::RoomScene& room, int sx, int sy) {
  std::vector<u8> cov(320 * 240, 0);
  for (const auto& layer : room.layers)
    for (const auto& group : layer.groups) {
      const u32 scale = group.group < room.camera.group_scale.size() && room.camera.group_scale[group.group] ? room.camera.group_scale[group.group] : 4096;
      const int ox = static_cast<int>((static_cast<long long>(sx) * 4096) / scale), oy = static_cast<int>((static_cast<long long>(sy) * 4096) / scale);
      for (u32 j = 0; j < group.height; ++j) {
        const int y = group.y + static_cast<int>(j) + oy;
        if (y < 0 || y >= 240) continue;
        for (u32 i = 0; i < group.width; ++i) {
          const int x = group.x + static_cast<int>(i) + ox;
          if (x < 0 || x >= 320) continue;
          if (group.rgba[(static_cast<size_t>(j) * group.width + i) * 4 + 3]) cov[static_cast<size_t>(y) * 320 + x] = 1;
        }
      }
    }
  size_t covered = 0;
  for (u8 pixel : cov) covered += pixel;
  return static_cast<double>(covered) / (320.0 * 240.0);
}

int cmd_rooms_check(const vfs::Vfs& fs) {
  const auto names = list_rooms(fs);
  size_t low = 0, failed = 0;
  std::printf("%-12s %7s %7s  worst scroll\n", "room", "centre", "worst");
  for (const auto& nm : names) {
    std::string err;
    auto room = load_room_scene(fs, nm, &err);
    if (!room) {
      ++failed;
      std::printf("%-12s load failed: %s\n", nm.c_str(), err.c_str());
      continue;
    }
    const auto& camera = room->camera;
    const double centre = viewport_coverage(*room, 0, 0);
    double worst = centre;
    int wx = 0, wy = 0;
    for (int sx : {static_cast<int>(camera.x_min), 0, static_cast<int>(camera.x_max)})
      for (int sy : {static_cast<int>(camera.y_min), 0, static_cast<int>(camera.y_max)}) {
        const double cv = viewport_coverage(*room, sx, sy);
        if (cv < worst) worst = cv, wx = sx, wy = sy;
      }
    if (worst < 0.90) {
      ++low;
      std::printf("%-12s %6.1f%% %6.1f%%  (%d,%d)\n", nm.c_str(), centre * 100.0, worst * 100.0, wx, wy);
    }
  }
  std::printf("rooms-check: %zu rooms, %zu failed to load, %zu with initial-frame coverage < 90%%\n", names.size(), failed, low);
  return failed ? 1 : 0;
}

// ---------------------------------------------------------------- rooms-frame: black rows under the picture per display height
// For every room and every scroll the follow camera can reach (clamp: x in [x_min..x_max], y in [y_min-24..y_max]; the
// extremes and every 8th value in between), composes the tile coverage (all groups, parallax off = (S << 12) / scale) of a 320 x H window
// and counts the bottom rows with no tile at all. H = 216 (the PS1 field display), 224 (RDE: 216 + 8), 240 (a full 240-line frame).
// Also checks the room bbox rule.
namespace {
int empty_bottom_rows(const assets::RoomScene& room, int sx, int sy, int h) {
  std::vector<u8> row(static_cast<size_t>(h), 0);
  for (const auto& layer : room.layers)
    for (const auto& group : layer.groups) {
      const u32 scale = group.group < room.camera.group_scale.size() && room.camera.group_scale[group.group] ? room.camera.group_scale[group.group] : 4096;
      const int ox = static_cast<int>((static_cast<long long>(sx) * 4096) / scale), oy = static_cast<int>((static_cast<long long>(sy) * 4096) / scale);
      if (group.x + ox >= 320 || group.x + ox + static_cast<int>(group.width) <= 0) continue;
      for (u32 j = 0; j < group.height; ++j) {
        const int y = group.y + static_cast<int>(j) + oy;
        if (y < 0 || y >= h || row[static_cast<size_t>(y)]) continue;
        for (u32 i = 0; i < group.width; ++i) {
          const int x = group.x + static_cast<int>(i) + ox;
          if (x >= 0 && x < 320 && group.rgba[(static_cast<size_t>(j) * group.width + i) * 4 + 3]) {
            row[static_cast<size_t>(y)] = 1;
            break;
          }
        }
      }
    }
  int empty_rows = 0;
  for (int y = h - 1; y >= 0 && !row[static_cast<size_t>(y)]; --y) ++empty_rows;
  return empty_rows;
}
}  // namespace

int cmd_rooms_frame(const vfs::Vfs& fs) {
  const auto names = list_rooms(fs);
  constexpr int kHeights[3] = {216, 224, 240};
  size_t failed = 0, bbox_ok = 0, rooms_with[3] = {0, 0, 0};
  int worst[3] = {0, 0, 0};
  for (const auto& nm : names) {
    std::string err;
    auto room = load_room_scene(fs, nm, &err);
    if (!room) {
      ++failed;
      std::printf("%-12s load failed: %s\n", nm.c_str(), err.c_str());
      continue;
    }
    const auto& camera = room->camera;
    int bx0 = INT32_MAX, by0 = INT32_MAX, bx1 = INT32_MIN, by1 = INT32_MIN;
    for (const auto& layer : room->layers)
      for (const auto& group : layer.groups) {
        bx0 = std::min(bx0, group.x);
        by0 = std::min(by0, group.y);
        bx1 = std::max(bx1, group.x + static_cast<int>(group.width));
        by1 = std::max(by1, group.y + static_cast<int>(group.height));
      }
    if (bx0 == -camera.x_max && by0 == -camera.y_max && bx1 == 320 - camera.x_min && by1 == 240 - camera.y_min) ++bbox_ok;
    const int ylo = camera.y_min - 24, yhi = camera.y_max;
    std::vector<int> xs{camera.x_min, camera.x_max}, ys{ylo, yhi};
    for (int x = camera.x_min; x < camera.x_max; x += 8) xs.push_back(x);
    for (int y = ylo; y < yhi; y += 8) ys.push_back(y);
    int empty[3] = {0, 0, 0}, ey[3] = {0, 0, 0};
    for (size_t k = 0; k < 3; ++k)
      for (int x : xs)
        for (int y : ys) {
          const int rows = empty_bottom_rows(*room, x, y, kHeights[k]);
          if (rows > empty[k]) empty[k] = rows, ey[k] = y;
        }
    for (size_t k = 0; k < 3; ++k) {
      if (empty[k]) ++rooms_with[k];
      worst[k] = std::max(worst[k], empty[k]);
    }
    if (empty[0] || empty[1] || empty[2])
      std::printf("%-12s y[%d..%d] empty bottom rows: h216 %d  h224 %d (scroll y %d)  h240 %d (scroll y %d)\n", nm.c_str(), ylo, yhi, empty[0], empty[1],
                  ey[1], empty[2], ey[2]);
  }
  std::printf("rooms-frame: %zu rooms, %zu failed, bbox == (-x_max,-y_max)..(320-x_min,240-y_min) in %zu\n", names.size(), failed, bbox_ok);
  for (size_t k = 0; k < 3; ++k)
    std::printf("  display %d rows: %zu rooms show empty rows under the picture at some reachable scroll (max %d rows)\n", kHeights[k],
                rooms_with[k], worst[k]);
  return failed ? 1 : 0;
}

int occ_main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string game_dir = std::getenv("OPENCROSS_GAMEDIR") ? std::getenv("OPENCROSS_GAMEDIR") : "";
  if (args.size() >= 2 && args[0] == "-g") {
    game_dir = args[1];
    args.erase(args.begin(), args.begin() + 2);
  }
  if (!args.empty() && args[0].starts_with("save-")) return occ_save_main(args);
  if (args.empty() || game_dir.empty()) return usage();

  vfs::Vfs fs;
  if (!fs.open(game_dir)) return 1;

  const std::string& cmd = args[0];
  if (cmd == "list" && args.size() == 2) {
    for (const auto& member : fs.list(args[1])) std::printf("%s\n", member.c_str());
    return 0;
  }
  if (cmd == "cat" && args.size() == 3) {
    auto data = fs.read(args[1]);
    if (!data) {
      std::fprintf(stderr, "not found: %s\n", args[1].c_str());
      return 1;
    }
    return write_file(args[2], *data) ? 0 : 1;
  }
  if (cmd == "info" && args.size() == 2) {
    auto data = fs.read(args[1]);
    if (!data) {
      std::fprintf(stderr, "not found: %s\n", args[1].c_str());
      return 1;
    }
    print_container(*data, 0);
    return 0;
  }
  if (cmd == "lzss" && args.size() == 3) {
    auto data = fs.read(args[1]);
    if (!data) {
      std::fprintf(stderr, "not found: %s\n", args[1].c_str());
      return 1;
    }
    auto out = import::lzss_decode(*data);
    if (!out) {
      std::fprintf(stderr, "not an lzss stream\n");
      return 1;
    }
    return write_file(args[2], *out) ? 0 : 1;
  }
  if (cmd == "dump-textures") return occ_dump_textures(fs, args);
  if (cmd == "room" && args.size() == 3) return cmd_room(fs, args[1], args[2]);
  if (cmd == "rooms-validate" && args.size() == 1) return cmd_rooms_validate(fs);
  if (cmd == "rooms-check" && args.size() == 1) return cmd_rooms_check(fs);
  if (cmd == "battlefields") return cmd_battlefields(fs, args);
  if (cmd == "world-markers") return cmd_world_markers(fs, args);
  if (cmd == "rooms-frame" && args.size() == 1) return cmd_rooms_frame(fs);
  if (cmd == "chip2" && args.size() == 3) return cmd_chip2(fs, args[1], args[2]);
  if (cmd == "chip2-validate" && args.size() == 1) return cmd_chip2_validate(fs);
  if (cmd == "script-disasm" && (args.size() == 2 || args.size() == 3))
    return cmd_script_disasm(fs, args[1], args.size() == 3 ? std::atoi(args[2].c_str()) : -1);
  if (cmd == "dialogue-choices") {
    std::string only;
    bool verbose = false;
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "-v") verbose = true;
      else only = args[i];
    }
    return cmd_dialogue_choices(fs, only, verbose);
  }
  if (cmd == "dialogue-sizes") {
    std::string only;
    bool verbose = false;
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "-v") verbose = true;
      else only = args[i];
    }
    return cmd_dialogue_sizes(fs, only, verbose);
  }
  if (cmd == "script-soak" && args.size() == 1) return cmd_script_soak(fs);
  if (cmd == "model" && args.size() == 3) return cmd_model(fs, args[1], args[2]);
  if (cmd == "models-validate" && args.size() == 1) return cmd_models_validate(fs);
  if (cmd == "room-songs" && (args.size() == 1 || args.size() == 2)) return cmd_room_songs(fs, args.size() == 2 ? args[1] : std::string());
  if (cmd == "room-models" && (args.size() == 1 || args.size() == 2)) return cmd_room_models(fs, args.size() == 2 ? args[1] : std::string());
  if (cmd == "menu-rigs" && args.size() <= 3) {
    bool hd = false, rooms = false, audit = false, npcs = false;
    int only = -1;
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "--hd") hd = true;
      else if (args[i] == "--rooms") rooms = true;
      else if (args[i] == "--audit") audit = true;
      else if (args[i] == "--npcs") npcs = true;
      else only = static_cast<int>(std::strtol(args[i].c_str(), nullptr, 16));
    }
    if (npcs) return cmd_menu_rig_npcs(fs, hd);
    if (rooms) return cmd_menu_rig_rooms(fs, hd);
    return audit ? cmd_menu_rig_audit(fs, hd, only) : cmd_menu_rigs(fs, hd, only);
  }
  return usage();
}
