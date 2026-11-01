// ocstage - battle stage (battlefield) inspector / exporter / validator.
//   ocstage [-g <gamedir>] info <bg_NN>                 summary of one stage (battle/bg/bg_NN.prd, kamei): parts, atlases, cameras,
//                                                       sequences, emitters, lens tables (OCSTAGE_COLORS=1 / OCSTAGE_UV=1 add vertex
//                                                       colours / per-submesh uv ranges)
//   ocstage [-g <gamedir>] export <bg_NN> <outdir>      part_<entry>_<name>.obj, stage.mtl, tex_m*_c*.png, stage.json
//   ocstage [-g <gamedir>] objects <bg_NN>              the stage's object scripts as the engine draws them (mesh, sprite type, blend,
//                                                       flags, position / angle / scale, OT bias, colour, life)
//   ocstage [-g <gamedir>] envpage <bg_NN|all> [outdir] the stage's stock environment page as <bg>_env.png; `all` lists every stage's
//                                                       coverage (visible texels, distinct colours)
//   ocstage [-g <gamedir>] light <bg_NN|all>            where [Enhancements] Lighting takes the stage's sunlight from
//   ocstage [-g <gamedir>] [-v] validate                every battle/bg/*.prd; exit 0 when all pass (-v lists the passing ones too)
// <bg_NN> may also be a path below cdrom/ (with or without .prd).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "game/battle/stage_key_light.h"
#include "import/battle_stage.h"
#include "platform/paths.h"
#include "tool_util.h"
#include "vfs/vfs.h"

using namespace oc;
namespace fs = std::filesystem;

namespace {

std::string stage_path(std::string arg) {
  std::replace(arg.begin(), arg.end(), '\\', '/');
  const std::string lowered = tool::lower_ascii(arg);
  if (lowered.rfind("cdrom/", 0) == 0) arg = arg.substr(6);
  if (tool::lower_ascii(arg).rfind("battle/", 0) != 0) arg = "battle/bg/" + arg;
  if (arg.size() < 4 || tool::lower_ascii(arg).compare(arg.size() - 4, 4, ".prd") != 0) arg += ".prd";
  return arg;
}

std::vector<std::string> list_stage_files(const vfs::Vfs& game_fs) {
  std::vector<std::string> names;
  for (const std::string& name : game_fs.list("cdrom")) {
    const std::string lowered = tool::lower_ascii(name);
    if (lowered.rfind("battle/bg/", 0) == 0 && lowered.size() > 4 && lowered.compare(lowered.size() - 4, 4, ".prd") == 0) names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

struct Raw {
  Bytes prd, ctd;
};

bool read_stage(const vfs::Vfs& game_fs, const std::string& rel, Raw& out) {
  auto prd = game_fs.read("cdrom/" + rel);
  if (!prd) return false;
  out.prd = std::move(*prd);
  out.ctd.clear();
  if (auto ctd = game_fs.read("cdrom/" + rel.substr(0, rel.size() - 4) + ".ctd")) out.ctd = std::move(*ctd);
  return true;
}

// Reads and loads the stage a command-line argument names; prints the failure to stderr.
bool load_stage_arg(const vfs::Vfs& game_fs, const std::string& arg, std::string& rel, Raw& raw, std::optional<assets::StageAsset>& stage) {
  rel = stage_path(arg);
  if (!read_stage(game_fs, rel, raw)) {
    std::fprintf(stderr, "cannot read cdrom/%s\n", rel.c_str());
    return false;
  }
  std::string err;
  stage = import::load_stage(raw.prd, raw.ctd, &err);
  if (!stage) {
    std::fprintf(stderr, "load failed: %s\n", err.c_str());
    return false;
  }
  return true;
}

std::string json_str(const std::string& text) {
  std::string out = "\"";
  for (char ch : text) {
    if (ch == '"' || ch == '\\') out.push_back('\\');
    if (static_cast<unsigned char>(ch) < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof escaped, "\\u%04x", ch);
      out += escaped;
    } else {
      out.push_back(ch);
    }
  }
  return out + "\"";
}

std::string mat_name(const assets::StageAsset& stage, int tex) {
  if (tex < 0) return "untextured";
  const auto& texture = stage.textures[static_cast<size_t>(tex)];
  return "m_" + std::to_string(texture.mode) + "_" + std::to_string(texture.clut_x) + "_" + std::to_string(texture.clut_y);
}

size_t untextured_triangles(const assets::StagePart& part) {
  size_t count = 0;
  for (const auto& submesh : part.submeshes)
    if (submesh.texture < 0) count += submesh.index_count / 3;
  return count;
}

int cmd_info(const vfs::Vfs& game_fs, const std::string& arg) {
  Raw raw;
  std::string rel;
  std::optional<assets::StageAsset> stage;
  if (!load_stage_arg(game_fs, arg, rel, raw, stage)) return 1;
  std::printf("stage %s: %zu parts, %zu atlases, %u TIMs%s\n", rel.c_str(), stage->parts.size(), stage->textures.size(), stage->tim_count,
              raw.ctd.empty() ? "" : " (+ctd)");
  std::printf("atlas: VRAM x0 %d hw, width %d hw (%d px at 8bpp) x 256 rows from y %d\n", stage->atlas_x0_hw, stage->atlas_width_hw,
              stage->atlas_width_hw * 2, stage->atlas_y0);
  std::printf("parts:\n");
  for (const auto& part : stage->parts) {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& vert : part.vertices)
      for (int i = 0; i < 3; ++i) {
        lo[i] = std::min(lo[i], vert.pos[static_cast<size_t>(i)]);
        hi[i] = std::max(hi[i], vert.pos[static_cast<size_t>(i)]);
      }
    if (std::getenv("OCSTAGE_COLORS"))
      for (const auto& vert : part.vertices)
        std::printf("   v %.0f %.0f %.0f  col %.2f %.2f %.2f\n", vert.pos[0], vert.pos[1], vert.pos[2], vert.color[0], vert.color[1], vert.color[2]);
    if (std::getenv("OCSTAGE_UV"))
      for (const auto& submesh : part.submeshes) {
        float u_lo = 9, u_hi = -9, v_lo = 9, v_hi = -9, raw_u_lo = 1e9f, raw_u_hi = -1e9f, raw_v_lo = 1e9f, raw_v_hi = -1e9f;
        for (u32 i = submesh.first_index; i < submesh.first_index + submesh.index_count; ++i) {
          const auto& vert = part.vertices[part.indices[i]];
          u_lo = std::min(u_lo, vert.uv[0]);
          u_hi = std::max(u_hi, vert.uv[0]);
          v_lo = std::min(v_lo, vert.uv[1]);
          v_hi = std::max(v_hi, vert.uv[1]);
          raw_u_lo = std::min(raw_u_lo, vert.uv_raw[0]);
          raw_u_hi = std::max(raw_u_hi, vert.uv_raw[0]);
          raw_v_lo = std::min(raw_v_lo, vert.uv_raw[1]);
          raw_v_hi = std::max(raw_v_hi, vert.uv_raw[1]);
        }
        std::printf("   sub tex %d tris %u u[%.3f,%.3f] v[%.3f,%.3f] packet u[%.0f,%.0f] v[%.0f,%.0f]", submesh.texture, submesh.index_count / 3, u_lo, u_hi,
                    v_lo, v_hi, raw_u_lo, raw_u_hi, raw_v_lo, raw_v_hi);
        if (submesh.direct_page >= 0) std::printf(" direct page (%d, %d)", submesh.direct_page & 0xFFFF, submesh.direct_page >> 16);
        std::printf("\n");
      }
    std::printf("  [%3u] %-5s %s src verts %5u faces %5u -> verts %5zu tris %5zu (untex %zu semi %u)  bbox x[%.0f,%.0f] y[%.0f,%.0f] z[%.0f,%.0f] emitters %zu\n",
                part.entry, part.name.c_str(), part.ground ? "ground" : "scenery", part.source_vertices, part.source_faces, part.vertices.size(),
                part.indices.size() / 3, untextured_triangles(part), part.semi_transparent_faces, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                part.emitters.size());
  }
  for (size_t i = 0; i < stage->textures.size(); ++i) {
    const auto& texture = stage->textures[i];
    std::printf("texture %zu: mode %d clut (%d,%d) %ux%u\n", i, texture.mode, texture.clut_x, texture.clut_y, texture.image.width, texture.image.height);
  }
  std::printf("cameras: %zu\n", stage->cameras.size());
  for (const auto& camera : stage->cameras)
    std::printf("  %-5s keys %zu duration %u mode %u\n", camera.name.c_str(), camera.keys.size(), camera.duration, camera.mode);
  std::printf("sequences: %zu\n", stage->sequences.size());
  for (const auto& sequence : stage->sequences) {
    std::printf("  %-5s %zu cmds:", sequence.name.c_str(), sequence.commands.size());
    for (const auto& command : sequence.commands)
      std::printf(" %02X%s%s%s", command.op, command.target.empty() ? "" : "(", command.target.c_str(), command.target.empty() ? "" : ")");
    std::printf("\n");
  }
  std::printf("emitters: %zu\n", stage->emitters.size());
  for (const auto& emitter : stage->emitters) std::printf("  %-5s -> %s\n", emitter.name.c_str(), emitter.mesh.empty() ? "(none)" : emitter.mesh.c_str());
  for (const auto& lens : stage->lens) {
    std::printf("lenz %s: %zu tables, sprites per table", lens.name.c_str(), lens.tables.size());
    for (const auto& table : lens.tables) std::printf(" %zu", table.size());
    std::printf("\n");
  }
  return 0;
}

int cmd_objects(const vfs::Vfs& game_fs, const std::string& arg) {
  Raw raw;
  std::string rel;
  std::optional<assets::StageAsset> stage;
  if (!load_stage_arg(game_fs, arg, rel, raw, stage)) return 1;
  std::printf("%s: %zu objects (static = LIFE 0 and no velocity; blend 0 opaque 1 avg 2 add 3 sub 4 add/4; W = world-oriented, D = depth-cue, C = cull-test, F = fixed slot)\n",
              rel.c_str(), stage->emitters.size());
  for (const auto& emitter : stage->emitters) {
    const std::string life = emitter.life ? " life " + std::to_string(emitter.life) : std::string{};
    std::printf("  %-5s %-5s type %2d %s blend %d %s%s%s%s pos %6d %6d %6d ang %5d %5d %5d scale %.2f %.2f %.2f bias %5d col %3d %3d %3d%s%s%s\n",
                emitter.name.c_str(), emitter.mesh.empty() ? "-" : emitter.mesh.c_str(), emitter.sprite_type,
                emitter.is_static() ? "static  " : "particle", emitter.blend(), (emitter.flags >> 26) & 1 ? "W" : "-",
                (emitter.flags >> 17) & 1 ? "D" : "-", (emitter.flags >> 28) & 1 ? "C" : "-", (emitter.flags >> 23) & 1 ? "F" : "-", emitter.pos[0],
                emitter.pos[1], emitter.pos[2], emitter.angle[0], emitter.angle[1], emitter.angle[2], emitter.scale[0] / 4096.0,
                emitter.scale[1] / 4096.0, emitter.scale[2] / 4096.0, emitter.bias, emitter.color[0] >> 4, emitter.color[1] >> 4,
                emitter.color[2] >> 4, life.c_str(), emitter.ext & 1 ? " uv-scroll" : "",
                emitter.ext & 0x30 ? (emitter.ext & 0x20 ? " two-sided" : " reversed-cull") : "");
  }
  return 0;
}

// One line per stage: where [Enhancements] Lighting takes its sunlight from, as elevation and azimuth.
int cmd_light(const vfs::Vfs& game_fs, const std::string& arg) {
  std::vector<std::string> names{arg};
  if (arg == "all") {
    names.clear();
    for (int i = 0; i < 130; ++i) {
      char name[16];
      std::snprintf(name, sizeof name, "bg_%02d", i);
      if (game_fs.exists("cdrom/" + stage_path(name))) names.emplace_back(name);
    }
  }
  for (const std::string& name : names) {
    Raw raw;
    std::string rel;
    std::optional<assets::StageAsset> stage;
    if (!load_stage_arg(game_fs, name, rel, raw, stage)) continue;
    const game::KeyLight light = game::key_light(*stage);
    static const char* const kSources[] = {"camera", "flare", "sun"};
    std::printf("%-6s %-6s elevation %5.1f azimuth %6.1f\n", name.c_str(), kSources[static_cast<int>(light.source)], std::asin(std::clamp(light.dir[1], -1.0f, 1.0f)) * 57.29578f,
                std::atan2(light.dir[2], light.dir[0]) * 57.29578f);
  }
  return 0;
}

// Writes one OBJ per part (vertex colours after the position), stage.mtl with one material per atlas texture and stage.json with the
// atlas, cameras (y written back in the file's +y-down space), sequences (command bytes as hex) and emitters.
int cmd_export(const vfs::Vfs& game_fs, const std::string& arg, const std::string& outdir) {
  Raw raw;
  std::string rel;
  std::optional<assets::StageAsset> stage;
  if (!load_stage_arg(game_fs, arg, rel, raw, stage)) return 1;
  fs::create_directories(outdir);
  std::vector<std::string> files;
  for (const auto& part : stage->parts) {
    char file_name[64];
    std::string safe_name = part.name;
    std::replace(safe_name.begin(), safe_name.end(), '/', '_');
    std::snprintf(file_name, sizeof file_name, "part_%02u_%s.obj", part.entry, safe_name.c_str());
    std::ofstream obj(fs::path(outdir) / file_name);
    obj << "# part " << part.name << "  verts " << part.source_vertices << " faces " << part.source_faces << "\nmtllib stage.mtl\no " << part.name << "\n";
    char line[160];
    for (const auto& vert : part.vertices) {
      std::snprintf(line, sizeof line, "v %d %d %d %.4f %.4f %.4f\n", static_cast<int>(vert.pos[0]), static_cast<int>(vert.pos[1]),
                    static_cast<int>(vert.pos[2]), vert.color[0], vert.color[1], vert.color[2]);
      obj << line;
    }
    for (const auto& vert : part.vertices) {
      std::snprintf(line, sizeof line, "vt %.6f %.6f\n", vert.uv[0], vert.uv[1]);
      obj << line;
    }
    for (const auto& submesh : part.submeshes) {
      obj << "usemtl " << mat_name(*stage, submesh.texture) << "\n";
      for (u32 i = 0; i < submesh.index_count; i += 3) {
        obj << "f";
        for (u32 k = 0; k < 3; ++k) {
          const u32 idx = part.indices[submesh.first_index + i + k] + 1;
          obj << " " << idx << "/" << idx;
        }
        obj << "\n";
      }
    }
    files.push_back(file_name);
  }
  {
    std::ofstream mtl(fs::path(outdir) / "stage.mtl");
    mtl << "newmtl untextured\nKd 0.8 0.8 0.8\n";
    for (const auto& texture : stage->textures) {
      char png[64];
      std::snprintf(png, sizeof png, "tex_m%d_c%d_%d.png", texture.mode, texture.clut_x, texture.clut_y);
      if (!tool::write_png((fs::path(outdir) / png).string(), texture.image.width, texture.image.height, texture.image.pixels)) {
        std::fprintf(stderr, "cannot write %s\n", png);
        return 1;
      }
      mtl << "newmtl m_" << texture.mode << "_" << texture.clut_x << "_" << texture.clut_y << "\nKd 1 1 1\nmap_Kd " << png << "\n";
    }
  }
  std::ofstream json(fs::path(outdir) / "stage.json");
  json << "{\n \"stage\": " << json_str(arg) << ",\n \"parts\": [";
  for (size_t i = 0; i < files.size(); ++i) json << (i ? ", " : "") << json_str(files[i]);
  json << "],\n \"atlas\": {\"vram_x0_halfwords\": " << stage->atlas_x0_hw << ", \"width_halfwords\": " << stage->atlas_width_hw
       << ", \"rows\": \"256..511\", \"px_8bpp\": [" << stage->atlas_width_hw * 2 << ", 256]},\n \"cameras\": [";
  for (size_t ci = 0; ci < stage->cameras.size(); ++ci) {
    const auto& camera = stage->cameras[ci];
    json << (ci ? ",\n  " : "\n  ") << "{\"name\": " << json_str(camera.name) << ", \"header\": {\"flags\": " << camera.flags
         << ", \"w4\": " << (static_cast<u32>(camera.duration) | (static_cast<u32>(camera.mode) << 16)) << ", \"w8\": " << camera.w8
         << ", \"ref\": [" << int(camera.ref[0]) << ", " << int(camera.ref[1]) << ", " << int(camera.ref[2]) << ", " << int(camera.ref[3])
         << "]}, \"keys\": [";
    for (size_t k = 0; k < camera.keys.size(); ++k) {
      const auto& key = camera.keys[k];
      json << (k ? ", " : "") << "{\"w0\": " << key.w0 << ", \"eye\": [" << int(key.eye[0]) << ", " << -int(key.eye[1]) << ", "
           << int(key.eye[2]) << "], \"h\": " << int(key.projection) << ", \"target\": [" << int(key.target[0]) << ", "
           << -int(key.target[1]) << ", " << int(key.target[2]) << "], \"roll\": " << key.roll_raw << "}";
    }
    json << "]}";
  }
  json << "],\n \"sequences\": [";
  for (size_t si = 0; si < stage->sequences.size(); ++si) {
    const auto& sequence = stage->sequences[si];
    json << (si ? ",\n  " : "\n  ") << "{\"name\": " << json_str(sequence.name) << ", \"cmds\": [";
    for (size_t k = 0; k < sequence.commands.size(); ++k) {
      const auto& command = sequence.commands[k];
      std::string hex;
      for (u8 byte : command.data) {
        char hex_byte[4];
        std::snprintf(hex_byte, sizeof hex_byte, "%02x", byte);
        hex += hex_byte;
      }
      json << (k ? ", " : "") << "{\"off\": " << command.offset << ", \"op\": " << command.op << ", \"len\": " << command.length
           << ", \"jump\": " << (command.jump ? "true" : "false") << ", \"name\": " << (command.target.empty() ? "null" : json_str(command.target))
           << ", \"data\": \"" << hex << "\"}";
    }
    json << "]}";
  }
  json << "],\n \"emitters\": [";
  for (size_t i = 0; i < stage->emitters.size(); ++i)
    json << (i ? ", " : "") << "{\"name\": " << json_str(stage->emitters[i].name) << ", \"mesh\": "
         << (stage->emitters[i].mesh.empty() ? "null" : json_str(stage->emitters[i].mesh)) << "}";
  json << "]\n}\n";
  std::printf("exported %s -> %s (%zu parts, %zu atlases)\n", arg.c_str(), outdir.c_str(), files.size(), stage->textures.size());
  return 0;
}

int cmd_envpage(const vfs::Vfs& game_fs, const std::string& arg, const std::string& outdir) {
  std::vector<std::string> names;
  if (arg == "all") names = list_stage_files(game_fs);
  else names.push_back(stage_path(arg));
  if (!outdir.empty()) fs::create_directories(outdir);
  for (const std::string& rel : names) {
    Raw raw;
    if (!read_stage(game_fs, rel, raw)) {
      std::fprintf(stderr, "cannot read %s\n", rel.c_str());
      continue;
    }
    auto stage = import::load_stage(raw.prd, raw.ctd);
    if (!stage) continue;
    size_t lit = 0;
    std::set<u32> colours;
    for (size_t i = 0; i + 3 < stage->env_rgba.size(); i += 4) {
      u32 colour;
      std::memcpy(&colour, &stage->env_rgba[i], 4);
      if (stage->env_rgba[i + 3]) ++lit;
      colours.insert(colour);
    }
    const std::string stem = fs::path(rel).stem().string();
    std::printf("%-12s env page: %s, %zu / 65536 visible texels, %zu colours\n", stem.c_str(), stage->env_rgba.empty() ? "none" : "present", lit,
                colours.size());
    if (!outdir.empty() && !stage->env_rgba.empty())
      tool::write_png(outdir + "/" + stem + "_env.png", 256, 256, Bytes(stage->env_rgba.begin(), stage->env_rgba.end()));
  }
  return 0;
}

// A world-space part that is mostly untextured with a bright mean vertex colour draws flat white / pale; backdrops ("far*", "roof*"
// parts or emitters) are exempt.
bool is_pale_untextured(const assets::StagePart& part, size_t untextured, double& mean_colour) {
  auto is_backdrop = [](const std::string& name) { return name.rfind("far", 0) == 0 || name.rfind("roof", 0) == 0; };
  bool backdrop = is_backdrop(part.name);
  for (const auto& emitter : part.emitters) backdrop = backdrop || is_backdrop(emitter);
  if (backdrop || part.indices.empty() || untextured * 2 < part.indices.size() / 3) return false;
  double sum = 0;
  for (const auto& vert : part.vertices) sum += (vert.color[0] + vert.color[1] + vert.color[2]) / 3.0;
  mean_colour = sum / static_cast<double>(part.vertices.size());
  return mean_colour > 0.6;
}

int cmd_validate(const vfs::Vfs& game_fs, bool verbose) {
  u32 white_parts = 0;
  const std::vector<std::string> names = list_stage_files(game_fs);
  u32 ok = 0, bad = 0, meshes = 0, ground = 0, verts = 0, faces = 0, tims = 0, cams = 0, seqs = 0, lens = 0, with_ctd = 0;
  u64 split_verts = 0;
  for (const std::string& name : names) {
    Raw raw;
    if (!read_stage(game_fs, name, raw)) {
      std::printf("PROBLEM %s: unreadable\n", name.c_str());
      ++bad;
      continue;
    }
    if (!raw.ctd.empty()) ++with_ctd;
    auto validation = import::validate_stage(raw.prd, raw.ctd);
    std::string err;
    auto stage = import::load_stage(raw.prd, raw.ctd, &err);
    if (!stage) validation.errors.push_back("load_stage: " + err);
    else {
      for (const auto& part : stage->parts) split_verts += part.vertices.size();
      u32 part_vertices = 0, part_faces = 0;
      for (const auto& part : stage->parts) part_vertices += part.source_vertices, part_faces += part.source_faces;
      if (part_vertices != validation.source_vertices || part_faces != validation.source_faces)
        validation.errors.push_back("asset/validator count mismatch");
      for (const auto& part : stage->parts) {
        const size_t untextured = untextured_triangles(part);
        double mean_colour = 0;
        if (!is_pale_untextured(part, untextured, mean_colour)) continue;
        ++white_parts;
        std::printf("WARN %s part %s: %zu/%zu triangles untextured, mean vertex colour %.2f (draws pale / white)\n", name.c_str(),
                    part.name.c_str(), untextured, part.indices.size() / 3, mean_colour);
      }
    }
    meshes += validation.meshes;
    ground += validation.ground_meshes;
    verts += validation.source_vertices;
    faces += validation.source_faces;
    tims += validation.tims;
    cams += validation.cameras;
    seqs += validation.sequences;
    lens += validation.lens;
    if (verbose || !validation.ok())
      std::printf("%s %-24s meshes %3u+%u verts %6u faces %6u tims %2u cams %2u seqs %2u x[%u,%u)\n", validation.ok() ? "ok  " : "FAIL",
                  name.c_str(), validation.meshes, validation.ground_meshes, validation.source_vertices, validation.source_faces,
                  validation.tims, validation.cameras, validation.sequences, validation.atlas_x_lo, validation.atlas_x_hi);
    for (size_t i = 0; i < validation.errors.size() && i < 3; ++i) std::printf("  PROBLEM %s: %s\n", name.c_str(), validation.errors[i].c_str());
    ++(validation.ok() ? ok : bad);
  }
  std::printf("stages %zu (ok %u, failed %u, with ctd %u): scenery %u + ground %u = %u meshes, %u source vertices, %u faces "
              "(%llu corner-split vertices), %u TIMs, %u cameras, %u sequences, %u lenz\n",
              names.size(), ok, bad, with_ctd, meshes, ground, meshes + ground, verts, faces,
              static_cast<unsigned long long>(split_verts), tims, cams, seqs, lens);
  if (white_parts) std::printf("%u pale untextured world-space parts flagged (WARN lines above)\n", white_parts);
  return bad ? 1 : 0;
}

}  // namespace

int ocstage_main(int argc, char** argv) {
  std::string gamedir;
  std::vector<std::string> args;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-g" && i + 1 < argc) gamedir = argv[++i];
    else if (arg == "-v") verbose = true;
    else args.push_back(arg);
  }
  if (args.empty() || (args[0] != "info" && args[0] != "export" && args[0] != "validate" && args[0] != "objects" && args[0] != "envpage" && args[0] != "light") ||
      ((args[0] == "info" || args[0] == "objects" || args[0] == "envpage" || args[0] == "light") && args.size() < 2) || (args[0] == "export" && args.size() < 3)) {
    std::fprintf(stderr, "usage: ocstage [-g <gamedir>] [-v] info <bg_NN> | objects <bg_NN> | envpage <bg_NN|all> [outdir] | light <bg_NN|all> | export <bg_NN> <outdir> | validate\n");
    return 2;
  }
  if (gamedir.empty()) gamedir = tool::env_game_dir();
  vfs::Vfs game_fs;
  const auto dir = platform::find_game_dir(gamedir);
  if (dir.empty() || !game_fs.open(dir)) {
    std::fprintf(stderr, "game directory not found (use -g <dir> or OPENCROSS_GAMEDIR)\n");
    return 1;
  }
  if (args[0] == "info") return cmd_info(game_fs, args[1]);
  if (args[0] == "objects") return cmd_objects(game_fs, args[1]);
  if (args[0] == "light") return cmd_light(game_fs, args[1]);
  if (args[0] == "envpage") return cmd_envpage(game_fs, args[1], args.size() > 2 ? args[2] : "");
  if (args[0] == "export") return cmd_export(game_fs, args[1], args[2]);
  return cmd_validate(game_fs, verbose);
}
