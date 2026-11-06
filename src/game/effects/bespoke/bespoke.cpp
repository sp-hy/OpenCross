#include "game/effects/bespoke/bespoke.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "game/effects/bespoke/bespoke_util.h"
#include "game/effects/soft_raster.h"

namespace oc::effects {

// ---- projection ------------------------------------------------------------------------------------------------------------------
bool fx_project(const ViewParams& view, const Vec3& point, float& sx, float& sy, float& depth) {
  const Vec3 camera = util::add(view.rot.apply(point), view.trans);
  if (camera[2] < 32.0f) return false;
  sx = view.ox + view.h * camera[0] / camera[2] + view.scroll_x;
  sy = view.oy + view.h * camera[1] / camera[2] + view.scroll_y;
  depth = camera[2];
  return true;
}
int fx_slot_of_depth(float z) { return std::clamp(static_cast<int>(z * 0.25f), 1, 0xFFF); }

// Pc = R * P + T  =>  eye (Pc = 0) = -R^T T; the view axis (Pc = (0, 0, d)) = R^T (0, 0, d) + eye.
Vec3 fx_eye(const ViewParams& view) {
  const auto& matrix = view.rot.m;
  Vec3 eye{};
  for (size_t axis = 0; axis < 3; ++axis)
    eye[axis] = -(matrix[0 * 3 + axis] * view.trans[0] + matrix[1 * 3 + axis] * view.trans[1] + matrix[2 * 3 + axis] * view.trans[2]);
  return eye;
}
namespace {
Bytes& main_exe() {
  static Bytes image;
  return image;
}
}  // namespace
void set_ps1_main_exe(Bytes exe) { main_exe() = std::move(exe); }
int ps1_square_root0(int value) {
  // LZCS / LZCR: count of leading bits equal to the sign bit (32 for 0 and -1)
  const u32 bits = static_cast<u32>(value);
  int lz = 0;
  const u32 sign = bits >> 31;
  for (int bit = 31; bit >= 0 && ((bits >> bit) & 1u) == sign; --bit) ++lz;
  if (lz == 32) return 0;
  const int shift = lz & ~1;
  const int index = shift < 0x18 ? (value >> (0x18 - shift)) : static_cast<int>(static_cast<u32>(value) << (shift - 0x18));
  const Bytes& exe = main_exe();
  const long long addr = 0x8006BB0CLL + 2LL * (index - 0x40);
  // PS-EXE: 0x800-byte header, text at 0x80010000.
  const long long off = addr - (0x80010000LL - 0x800);
  if (exe.size() < 0x800 || off < 0x800 || off + 2 > static_cast<long long>(exe.size())) {
    if (value <= 0) return 0;
    return static_cast<int>(std::sqrt(static_cast<double>(value)));
  }
  const s16 entry = static_cast<s16>(exe[static_cast<size_t>(off)] | (exe[static_cast<size_t>(off) + 1] << 8));
  return static_cast<int>(static_cast<u32>(static_cast<int>(entry) << ((0x1F - shift) >> 1)) >> 12);
}

Vec3 fx_look_at(const ViewParams& view, float dist) {
  const Vec3 eye = fx_eye(view);
  const auto& matrix = view.rot.m;
  return {eye[0] + matrix[6] * dist, eye[1] + matrix[7] * dist, eye[2] + matrix[8] * dist};
}

void sort_prims(std::vector<FxPrim>& prims) {
  std::stable_sort(prims.begin(), prims.end(), [](const FxPrim& lhs, const FxPrim& rhs) {
    if (lhs.slot != rhs.slot) return lhs.slot > rhs.slot;
    return lhs.order > rhs.order;
  });
}

FxModelPose fx_model_pose(const FxModel& model, float alpha) {
  FxModelPose out;
  for (size_t k = 0; k < 3; ++k) out.pos[k] = model.prev_pos[k] + (model.pos[k] - model.prev_pos[k]) * alpha;
  out.pos[1] = -out.pos[1];
  int angles[3];
  float sc[3];
  for (int k = 0; k < 3; ++k) {
    int delta = (model.rot[k] - model.prev_rot[k]) & 0xFFF;
    if (delta >= 0x800) delta -= 0x1000;
    angles[k] = model.prev_rot[k] + static_cast<int>(std::lround(static_cast<float>(delta) * alpha));
    sc[k] = (static_cast<float>(model.prev_scale[k]) + static_cast<float>(model.scale[k] - model.prev_scale[k]) * alpha) / 4096.0f;
  }
  const Mat3 home = Mat3::from_angles(0, -model.home_rot[1], 0);
  const Mat3 rotation = Mat3::from_angles(angles[0], angles[1], angles[2]);
  Mat3 hr;
  for (size_t i = 0; i < 3; ++i)
    for (size_t j = 0; j < 3; ++j) {
      float sum = 0;
      for (size_t k = 0; k < 3; ++k) sum += home.m[i * 3 + k] * rotation.m[k * 3 + j];
      hr.m[i * 3 + j] = sum * sc[j];
    }
  out.rel = hr;
  return out;
}

// ---- meshes ------------------------------------------------------------------------------------------------------------------------
void FxMesh::finish() {
  if (verts.empty()) return;
  Vec3 lo = verts[0], hi = verts[0];
  for (const auto& vertex : verts)
    for (size_t k = 0; k < 3; ++k) {
      lo[k] = std::min(lo[k], vertex[k]);
      hi[k] = std::max(hi[k], vertex[k]);
    }
  centre = util::mul(util::add(lo, hi), 0.5f);
  float max_radius = 1;
  for (const auto& vertex : verts) max_radius = std::max(max_radius, util::len(util::sub(vertex, centre)));
  radius = max_radius;
  if (colours.size() < verts.size()) colours.resize(verts.size(), {128, 128, 128});
}

namespace {
// Box with every face cut into cells x cells cells (separate vertices per face so faces can fly apart).
void add_box(FxMesh& mesh, Vec3 centre, Vec3 half, std::array<u8, 3> colour, int cells) {
  for (int axis = 0; axis < 3; ++axis)
    for (int sgn = -1; sgn <= 1; sgn += 2) {
      const size_t a0 = static_cast<size_t>(axis), a1 = static_cast<size_t>((axis + 1) % 3), a2 = static_cast<size_t>((axis + 2) % 3);
      const float shade = 0.55f + 0.15f * static_cast<float>(axis) + (sgn > 0 ? 0.1f : 0.0f);
      const u16 base = static_cast<u16>(mesh.verts.size());
      for (int j = 0; j <= cells; ++j)
        for (int i = 0; i <= cells; ++i) {
          Vec3 point = centre;
          point[a0] += static_cast<float>(sgn) * half[a0];
          point[a1] += (2.0f * static_cast<float>(i) / static_cast<float>(cells) - 1.0f) * half[a1];
          point[a2] += (2.0f * static_cast<float>(j) / static_cast<float>(cells) - 1.0f) * half[a2];
          mesh.verts.push_back(point);
          mesh.colours.push_back({util::clamp8(static_cast<float>(colour[0]) * shade), util::clamp8(static_cast<float>(colour[1]) * shade),
                                  util::clamp8(static_cast<float>(colour[2]) * shade)});
        }
      for (int j = 0; j < cells; ++j)
        for (int i = 0; i < cells; ++i) {
          const u16 c00 = static_cast<u16>(base + j * (cells + 1) + i), c10 = static_cast<u16>(c00 + 1),
                    c01 = static_cast<u16>(c00 + cells + 1), c11 = static_cast<u16>(c01 + 1);
          mesh.tris.push_back({c00, c10, c01});
          mesh.tris.push_back({c10, c11, c01});
        }
    }
}
}  // namespace

FxMesh FxMesh::dummy_figure(u8 r, u8 g, u8 b) {
  FxMesh mesh;
  const std::array<u8, 3> body{r, g, b};
  const std::array<u8, 3> head{static_cast<u8>(std::min(255, r + 40)), static_cast<u8>(std::min(255, g + 40)),
                               static_cast<u8>(std::min(255, b + 40))};
  // Torso, head, arms, legs.
  add_box(mesh, {0, -20, 0}, {34, 50, 18}, body, 3);
  add_box(mesh, {0, -100, 0}, {20, 22, 20}, head, 3);
  add_box(mesh, {-52, -20, 0}, {12, 48, 12}, body, 2);
  add_box(mesh, {52, -20, 0}, {12, 48, 12}, body, 2);
  add_box(mesh, {-18, 85, 0}, {14, 60, 14}, body, 2);
  add_box(mesh, {18, 85, 0}, {14, 60, 14}, body, 2);
  mesh.finish();
  return mesh;
}

DummyStage::DummyStage() {
  caster_mesh = FxMesh::dummy_figure(120, 170, 220);
  target_mesh = FxMesh::dummy_figure(210, 150, 110);
  ctx.caster.pos = {-100, 0, 0};
  ctx.caster.mesh = &caster_mesh;
  ctx.target.pos = {100, 0, 0};
  ctx.target.mesh = &target_mesh;
  ctx.view.trans = {0, 0, 1000};
  ctx.view.h = 512.0f;
  ctx.colour[0] = 255;
  ctx.colour[1] = 200;
  ctx.colour[2] = 120;
  ctx.auto_input = true;
}

// ---- registry ------------------------------------------------------------------------------------------------------------------------
std::string overlay_stem(std::string_view path) {
  const size_t sl = path.find_last_of("/\\");
  if (sl != std::string_view::npos) path.remove_prefix(sl + 1);
  const size_t dot = path.find_last_of('.');
  if (dot != std::string_view::npos) path = path.substr(0, dot);
  std::string stem(path);
  for (char& ch : stem) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return stem;
}

const char* fx_pack_model_binding(std::string_view model, std::string_view overlay) {
  // The returned name encodes the CLUT row and texture page. saraman: page x 7 (0x1C0 / 64), y 256, CLUT row 0xEE at x 256.
  if (model == "sara") return "ee97";
  // ghost1 (Dragon Rider): page x 7, y 256, CLUT row 0xE2 at x 256.
  if (model == "act0" && overlay == "ghost1") return "e297";
  // jigoku (Dinner Guest): page x 10, y 256, CLUT row 0xEE at x 256.
  if (model == "act0" && overlay == "jigoku") return "ee9a";
  // sky (Flying Arrow): act0 = page x 7, CLUT row 0xE2 ("e297"); act1 = page x 8, y 256, CLUT row 0xE3 at x 256 ("e398").
  if (model == "act0" && overlay == "sky") return "e297";
  if (model == "act1" && overlay == "sky") return "e398";
  // tumaru (Pip's form changes): act0 = "e297", then its own entry 0.
  if (model == "act0" && overlay == "tumaru") return "e297";
  // ghost2 (Shadow Cat's Shadow, the ghost techs): act0 = "e297", then its own entry 0.
  if (model == "act0" && overlay == "ghost2") return "e297";
  // canon: page x 10, y 256, CLUT row 0xEA at x 256, then its own entry 0.
  if (model == "act0" && overlay == "canon") return "ea9a";
  return nullptr;
}

bool fx_slot_a_model(std::string_view name, int drp_type) { return drp_type == 0x0B && name == "act0"; }

BespokeRegistry& BespokeRegistry::instance() {
  static BespokeRegistry* registry = [] {
    auto* reg = new BespokeRegistry();
    register_band_wipes(*reg);
    register_line_rings(*reg);
    register_mesh_water(*reg);
    register_actor_scripts(*reg);
    register_mesh_warps(*reg);
    register_scene_remaps(*reg);
    register_emitter_patches(*reg);
    register_model_deforms(*reg);
    register_summon_ports(*reg);
    register_ground_ports(*reg);
    register_tech_ports(*reg);
    register_mesh_techs(*reg);
    register_death_ports(*reg);
    register_screen_ports(*reg);
    register_kamei_port(*reg);
    register_model_steers(*reg);
    return reg;
  }();
  return *registry;
}

void BespokeRegistry::add(BespokeInfo info) {
  for (auto& existing : infos_)
    if (existing.name == info.name) {
      existing = std::move(info);
      return;
    }
  infos_.push_back(std::move(info));
}

const BespokeInfo* BespokeRegistry::find(std::string_view name_or_path) const {
  const std::string stem = overlay_stem(name_or_path);
  for (const auto& info : infos_)
    if (info.name == stem) return &info;
  // "46black" in a path vs "black" registered (or the reverse): compare with the authoring index stripped.
  auto strip = [](const std::string& text) {
    size_t k = 0;
    while (k < text.size() && std::isdigit(static_cast<unsigned char>(text[k]))) ++k;
    return k < text.size() ? text.substr(k) : text;
  };
  const std::string bare = strip(stem);
  for (const auto& info : infos_)
    if (strip(info.name) == bare && (info.name == bare || stem == bare)) return &info;
  return nullptr;
}

void BespokeRegistry::bind_element(int eid, std::string_view overlay) {
  for (auto& element : elements_)
    if (element.first == eid) {
      element.second = overlay_stem(overlay);
      return;
    }
  elements_.emplace_back(eid, overlay_stem(overlay));
}

const BespokeInfo* BespokeRegistry::find_element(int eid) const {
  for (const auto& element : elements_)
    if (element.first == eid) return find(element.second);
  return nullptr;
}

size_t BespokeRegistry::bind_slots(const std::vector<EffectSlot>& slots) {
  size_t bound = 0;
  for (const auto& slot : slots) {
    if (slot.prg.empty()) continue;
    if (const BespokeInfo* info = find(slot.prg)) {
      bind_element(slot.eid, info->name);
      ++bound;
    }
  }
  return bound;
}

std::vector<const BespokeInfo*> BespokeRegistry::all() const {
  std::vector<const BespokeInfo*> result;
  for (const auto& info : infos_) result.push_back(&info);
  std::sort(result.begin(), result.end(), [](const BespokeInfo* lhs, const BespokeInfo* rhs) {
    return lhs->family != rhs->family ? lhs->family < rhs->family : lhs->name < rhs->name;
  });
  return result;
}

std::unique_ptr<BespokeEffect> BespokeRegistry::create(std::string_view name_or_path, const FxContext& ctx) const {
  const BespokeInfo* info = find(name_or_path);
  if (!info || !info->make) return nullptr;
  auto effect = info->make();
  effect->label_ = info->name;
  effect->start(ctx);
  return effect;
}

// ---- software raster -----------------------------------------------------------------------------------------------------------------
namespace {
void put(SoftCanvas& canvas, int x, int y, int blend, int r, int g, int b) {
  if (x < 0 || y < 0 || x >= canvas.width() || y >= canvas.height()) return;
  u8* pixel = &canvas.rgba()[(static_cast<size_t>(y) * static_cast<size_t>(canvas.width()) + static_cast<size_t>(x)) * 4];
  if (blend == kBlendInvert) {
    for (int channel = 0; channel < 3; ++channel) pixel[channel] = static_cast<u8>(255 - pixel[channel]);
    return;
  }
  const int colour[3] = {r, g, b};
  for (int channel = 0; channel < 3; ++channel)
    pixel[channel] = static_cast<u8>(blend_channel(blend, pixel[channel], std::clamp(colour[channel], 0, 255)));
}

void tri(SoftCanvas& canvas, const FxVert& p0, const FxVert& p1, const FxVert& p2, int blend) {
  const float minx = std::min({p0.x, p1.x, p2.x}), maxx = std::max({p0.x, p1.x, p2.x});
  const float miny = std::min({p0.y, p1.y, p2.y}), maxy = std::max({p0.y, p1.y, p2.y});
  const int x0 = std::max(0, static_cast<int>(std::floor(minx))), x1 = std::min(canvas.width() - 1, static_cast<int>(std::ceil(maxx)));
  const int y0 = std::max(0, static_cast<int>(std::floor(miny))), y1 = std::min(canvas.height() - 1, static_cast<int>(std::ceil(maxy)));
  const float den = (p1.y - p2.y) * (p0.x - p2.x) + (p2.x - p1.x) * (p0.y - p2.y);
  if (std::fabs(den) < 1e-9f) return;
  for (int py = y0; py <= y1; ++py)
    for (int px = x0; px <= x1; ++px) {
      const float fx = static_cast<float>(px) + 0.5f, fy = static_cast<float>(py) + 0.5f;
      const float l0 = ((p1.y - p2.y) * (fx - p2.x) + (p2.x - p1.x) * (fy - p2.y)) / den;
      const float l1 = ((p2.y - p0.y) * (fx - p2.x) + (p0.x - p2.x) * (fy - p2.y)) / den;
      const float l2 = 1.0f - l0 - l1;
      if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) continue;
      put(canvas, px, py, blend, static_cast<int>(l0 * p0.r + l1 * p1.r + l2 * p2.r), static_cast<int>(l0 * p0.g + l1 * p1.g + l2 * p2.g),
          static_cast<int>(l0 * p0.b + l1 * p1.b + l2 * p2.b));
    }
}
}  // namespace

void raster_prims(SoftCanvas& canvas, const std::vector<FxPrim>& sorted) {
  for (const FxPrim& prim : sorted) {
    switch (prim.kind) {
      case FxPrim::Kind::Tri: tri(canvas, prim.v[0], prim.v[1], prim.v[2], prim.blend); break;
      case FxPrim::Kind::Quad:
        tri(canvas, prim.v[0], prim.v[1], prim.v[2], prim.blend);
        tri(canvas, prim.v[1], prim.v[3], prim.v[2], prim.blend);
        break;
      case FxPrim::Kind::Line: {
        const float dx = prim.v[1].x - prim.v[0].x, dy = prim.v[1].y - prim.v[0].y;
        const int steps = std::clamp(static_cast<int>(std::max(std::fabs(dx), std::fabs(dy))), 1, 4096);
        for (int i = 0; i <= steps; ++i) {
          const float frac = static_cast<float>(i) / static_cast<float>(steps);
          put(canvas, static_cast<int>(std::floor(prim.v[0].x + dx * frac)), static_cast<int>(std::floor(prim.v[0].y + dy * frac)), prim.blend,
              static_cast<int>(prim.v[0].r + (prim.v[1].r - prim.v[0].r) * frac),
              static_cast<int>(prim.v[0].g + (prim.v[1].g - prim.v[0].g) * frac),
              static_cast<int>(prim.v[0].b + (prim.v[1].b - prim.v[0].b) * frac));
        }
        break;
      }
    }
  }
}

void flatten_prims(const std::vector<FxPrim>& sorted, std::vector<FlatQuad>& out, int cells) {
  cells = std::max(1, cells);
  for (const FxPrim& prim : sorted) {
    FlatQuad quad{};
    quad.blend = prim.blend;
    quad.slot = prim.slot;
    if (prim.kind == FxPrim::Kind::Line) {
      const float dx = prim.v[1].x - prim.v[0].x, dy = prim.v[1].y - prim.v[0].y, length = std::max(1e-3f, std::sqrt(dx * dx + dy * dy));
      const float nx = -dy / length * 0.5f, ny = dx / length * 0.5f;
      quad.x[0] = prim.v[0].x + nx; quad.y[0] = prim.v[0].y + ny;
      quad.x[1] = prim.v[1].x + nx; quad.y[1] = prim.v[1].y + ny;
      quad.x[2] = prim.v[0].x - nx; quad.y[2] = prim.v[0].y - ny;
      quad.x[3] = prim.v[1].x - nx; quad.y[3] = prim.v[1].y - ny;
      // Corners 0 / 2 sit at the start, 1 / 3 at the end of the line.
      for (int k = 0; k < 4; ++k) {
        const FxVert& corner = prim.v[k & 1];
        quad.c[k][0] = corner.r, quad.c[k][1] = corner.g, quad.c[k][2] = corner.b;
      }
      quad.r = static_cast<u8>((prim.v[0].r + prim.v[1].r) / 2);
      quad.g = static_cast<u8>((prim.v[0].g + prim.v[1].g) / 2);
      quad.b = static_cast<u8>((prim.v[0].b + prim.v[1].b) / 2);
      out.push_back(quad);
    } else if (prim.kind == FxPrim::Kind::Tri) {
      for (int k = 0; k < 3; ++k) {
        quad.x[k] = prim.v[k].x;
        quad.y[k] = prim.v[k].y;
      }
      // Degenerate fourth corner.
      quad.x[3] = prim.v[2].x;
      quad.y[3] = prim.v[2].y;
      for (int k = 0; k < 4; ++k) {
        const FxVert& corner = prim.v[k < 3 ? k : 2];
        quad.c[k][0] = corner.r, quad.c[k][1] = corner.g, quad.c[k][2] = corner.b;
      }
      quad.r = static_cast<u8>((prim.v[0].r + prim.v[1].r + prim.v[2].r) / 3);
      quad.g = static_cast<u8>((prim.v[0].g + prim.v[1].g + prim.v[2].g) / 3);
      quad.b = static_cast<u8>((prim.v[0].b + prim.v[1].b + prim.v[2].b) / 3);
      out.push_back(quad);
    } else {
      // Quad: one prim with its four corner colours; cells > 1 splits it into a bilinear grid (uniform colour collapses to one cell).
      const bool uniform = prim.v[0].r == prim.v[1].r && prim.v[0].r == prim.v[2].r && prim.v[0].r == prim.v[3].r &&
                           prim.v[0].g == prim.v[1].g && prim.v[0].g == prim.v[2].g && prim.v[0].g == prim.v[3].g &&
                           prim.v[0].b == prim.v[1].b && prim.v[0].b == prim.v[2].b && prim.v[0].b == prim.v[3].b;
      const int grid = uniform ? 1 : cells;
      auto at = [&](float u, float v, float& x, float& y, float colour[3]) {
        const float weights[4] = {(1 - u) * (1 - v), u * (1 - v), (1 - u) * v, u * v};
        x = y = colour[0] = colour[1] = colour[2] = 0;
        for (int k = 0; k < 4; ++k) {
          x += weights[k] * prim.v[k].x;
          y += weights[k] * prim.v[k].y;
          colour[0] += weights[k] * prim.v[k].r;
          colour[1] += weights[k] * prim.v[k].g;
          colour[2] += weights[k] * prim.v[k].b;
        }
      };
      for (int j = 0; j < grid; ++j)
        for (int i = 0; i < grid; ++i) {
          const float u0 = static_cast<float>(i) / static_cast<float>(grid), u1 = static_cast<float>(i + 1) / static_cast<float>(grid);
          const float v0 = static_cast<float>(j) / static_cast<float>(grid), v1 = static_cast<float>(j + 1) / static_cast<float>(grid);
          const float uu[4] = {u0, u1, u0, u1}, vv[4] = {v0, v0, v1, v1};
          float colour[3], tx, ty;
          for (int k = 0; k < 4; ++k) {
            at(uu[k], vv[k], quad.x[k], quad.y[k], colour);
            for (int channel = 0; channel < 3; ++channel)
              quad.c[k][channel] = static_cast<u8>(std::clamp(colour[channel] + 0.5f, 0.0f, 255.0f));
          }
          at((u0 + u1) * 0.5f, (v0 + v1) * 0.5f, tx, ty, colour);
          quad.r = static_cast<u8>(std::clamp(colour[0], 0.0f, 255.0f));
          quad.g = static_cast<u8>(std::clamp(colour[1], 0.0f, 255.0f));
          quad.b = static_cast<u8>(std::clamp(colour[2], 0.0f, 255.0f));
          out.push_back(quad);
        }
    }
  }
}

}  // namespace oc::effects
