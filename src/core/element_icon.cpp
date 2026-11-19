// Element colour icons: the six shapes as polygons in a unit square (half extent 1, y down), triangulated once per detail level and
// kept; element_icon() only scales, moves and colours the stored triangles. Each icon is a body in its colour, an optional inner
// highlight, a dark outline (light for the black moon) and, for the leaf, a stem and a vein.
#include "core/element_icon.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace oc {

namespace {

struct Pt {
  float x, y;
};

enum Tone : u8 { kFill, kLine, kLight };

struct Piece {
  std::array<float, 6> xy;
  u8 tone;
};

constexpr float kPi = 3.14159265358979f;
constexpr int kDetailLevels = 2;

constexpr u8 kFillRgb[kElementIconCount][3] = {{60, 200, 70}, {248, 248, 248}, {14, 14, 24}, {234, 64, 52}, {252, 214, 40}, {62, 112, 246}};
constexpr u8 kLightRgb[kElementIconCount][3] = {{170, 240, 150}, {200, 200, 210}, {110, 110, 140}, {255, 222, 110}, {255, 246, 170}, {200, 226, 255}};
constexpr u8 kDarkLine[3] = {18, 18, 28};
constexpr u8 kBrightLine[3] = {236, 236, 246};
constexpr int kBlack = 2;

u32 pack(const u8* rgb, int light) {
  auto channel = [&](int index) { return static_cast<u32>(rgb[index] * light / 255); };
  return channel(0) | (channel(1) << 8) | (channel(2) << 16) | 0xFF000000u;
}

float cross(const std::array<float, 2>& a, const std::array<float, 2>& b, const std::array<float, 2>& c) {
  return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

// `count` points of the circle (cx, cy, radius) from angle `from` to `to` in radians, ends included.
void arc(std::vector<Pt>& out, float cx, float cy, float radius, float from, float to, int count) {
  for (int i = 0; i <= count; ++i) {
    const float angle = from + (to - from) * static_cast<float>(i) / static_cast<float>(count);
    out.push_back({cx + radius * std::cos(angle), cy + radius * std::sin(angle)});
  }
}

struct Shape {
  std::vector<Pt> body;
  std::vector<std::vector<Pt>> lights;
  struct Line {
    Pt from, to;
    float width;
  };
  std::vector<Line> lines;
};

Shape flame(bool simple) {
  Shape shape;
  shape.body = {{0.12f, -1.0f}, {0.30f, -0.62f}, {0.58f, -0.25f}, {0.74f, 0.15f}, {0.68f, 0.55f}, {0.40f, 0.88f}, {0.0f, 1.0f},
                {-0.40f, 0.88f}, {-0.68f, 0.55f}, {-0.74f, 0.15f}, {-0.60f, -0.50f}, {-0.30f, -0.15f}, {-0.20f, -0.55f}};
  if (!simple) {
    std::vector<Pt> inner;
    for (const Pt& point : shape.body) inner.push_back({point.x * 0.5f + 0.03f, point.y * 0.5f + 0.42f});
    shape.lights.push_back(std::move(inner));
  }
  return shape;
}

Shape drop(bool simple, int segments) {
  Shape shape;
  const float radius = 0.68f, centre = 0.28f, tip = -1.0f;
  const float tangent = std::acos(radius / (centre - tip));
  shape.body.push_back({0.0f, tip});
  arc(shape.body, 0.0f, centre, radius, tangent - kPi / 2, 1.5f * kPi - tangent, segments);
  if (!simple) {
    std::vector<Pt> shine;
    arc(shine, -0.28f, 0.30f, 0.14f, 0.0f, 2 * kPi, 8);
    shine.pop_back();
    shape.lights.push_back(std::move(shine));
  }
  return shape;
}

Shape leaf(bool simple, int segments) {
  Shape shape;
  const Pt base{-0.78f, 0.78f}, tip{0.82f, -0.82f};
  const float dx = tip.x - base.x, dy = tip.y - base.y, length = std::sqrt(dx * dx + dy * dy);
  const float nx = -dy / length, ny = dx / length;
  const float upper = 0.52f, lower = 0.38f;
  for (int i = 0; i <= segments; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(segments), bulge = upper * std::sin(kPi * t);
    shape.body.push_back({base.x + dx * t + nx * bulge, base.y + dy * t + ny * bulge});
  }
  for (int i = segments - 1; i >= 1; --i) {
    const float t = static_cast<float>(i) / static_cast<float>(segments), bulge = lower * std::sin(kPi * t);
    shape.body.push_back({base.x + dx * t - nx * bulge, base.y + dy * t - ny * bulge});
  }
  shape.lines.push_back({base, {-0.9f, 0.9f}, 0.0f});
  if (!simple) shape.lines.push_back({{base.x + dx * 0.10f, base.y + dy * 0.10f}, {base.x + dx * 0.80f, base.y + dy * 0.80f}, 0.08f});
  return shape;
}

Shape bolt() {
  Shape shape;
  shape.body = {{0.34f, -1.0f}, {-0.52f, 0.14f}, {-0.04f, 0.14f}, {-0.32f, 1.0f}, {0.54f, -0.22f}, {0.06f, -0.22f}, {0.58f, -1.0f}};
  return shape;
}

Shape star() {
  Shape shape;
  const float inner = 0.42f;
  for (int i = 0; i < 10; ++i) {
    const float angle = -kPi / 2 + kPi * static_cast<float>(i) / 5.0f, radius = i % 2 == 0 ? 1.0f : inner;
    shape.body.push_back({radius * std::cos(angle), radius * std::sin(angle) + 0.1f});
  }
  return shape;
}

Shape moon(int segments) {
  Shape shape;
  const float outer = 0.95f, cut = 0.76f, offset = 0.45f;
  const float along = (outer * outer - cut * cut + offset * offset) / (2 * offset), height = std::sqrt(outer * outer - along * along);
  const float axis = -kPi / 4;
  const float outer_angle = std::atan2(height, along), cut_angle = std::atan2(height, along - offset);
  arc(shape.body, 0.0f, 0.0f, outer, axis + outer_angle, axis + 2 * kPi - outer_angle, segments);
  std::vector<Pt> inner;
  arc(inner, offset * std::cos(axis), offset * std::sin(axis), cut, axis + 2 * kPi - cut_angle, axis + cut_angle, segments);
  shape.body.insert(shape.body.end(), inner.begin() + 1, inner.end() - 1);
  return shape;
}

Shape shape_of(int colour, bool simple) {
  const int segments = simple ? 10 : 24;
  switch (colour) {
    case 0: return leaf(simple, segments);
    case 1: return star();
    case 2: return moon(segments);
    case 3: return flame(simple);
    case 4: return bolt();
    default: return drop(simple, segments);
  }
}

void add_fill(std::vector<Piece>& pieces, const std::vector<Pt>& points, float scale, u8 tone) {
  std::vector<std::array<float, 2>> flat;
  for (const Pt& point : points) flat.push_back({point.x * scale, point.y * scale});
  for (const std::array<int, 3>& tri : triangulate_polygon(flat)) {
    Piece piece;
    for (size_t corner = 0; corner < 3; ++corner) {
      piece.xy[corner * 2] = flat[static_cast<size_t>(tri[corner])][0];
      piece.xy[corner * 2 + 1] = flat[static_cast<size_t>(tri[corner])][1];
    }
    piece.tone = tone;
    pieces.push_back(piece);
  }
}

void add_line_triangle(std::vector<Piece>& pieces, Pt a, Pt b, Pt c) {
  Piece piece;
  piece.xy = {a.x, a.y, b.x, b.y, c.x, c.y};
  piece.tone = kLine;
  pieces.push_back(piece);
}

// A segment of the given width; `cap` pushes both ends out by half the width (an open line).
void add_line(std::vector<Piece>& pieces, Pt from, Pt to, float width, bool cap) {
  const float dx = to.x - from.x, dy = to.y - from.y, length = std::sqrt(dx * dx + dy * dy);
  if (length <= 0.0f) return;
  const float half = width * 0.5f, ux = dx / length * half, uy = dy / length * half, nx = -uy, ny = ux;
  const float push = cap ? 1.0f : 0.0f;
  const Pt a{from.x - ux * push + nx, from.y - uy * push + ny}, b{from.x - ux * push - nx, from.y - uy * push - ny};
  const Pt c{to.x + ux * push - nx, to.y + uy * push - ny}, d{to.x + ux * push + nx, to.y + uy * push + ny};
  add_line_triangle(pieces, a, b, c);
  add_line_triangle(pieces, a, c, d);
}

// The closed outline of a polygon: its edges, and at every corner a bevel on both sides that closes the gap between two edges.
void add_loop(std::vector<Piece>& pieces, const std::vector<Pt>& loop, float width) {
  const size_t count = loop.size();
  const float half = width * 0.5f;
  auto normal = [&](size_t index) {
    const Pt from = loop[index], to = loop[(index + 1) % count];
    const float dx = to.x - from.x, dy = to.y - from.y, length = std::sqrt(dx * dx + dy * dy);
    return length > 0.0f ? Pt{-dy / length * half, dx / length * half} : Pt{0.0f, 0.0f};
  };
  for (size_t i = 0; i < count; ++i) {
    add_line(pieces, loop[i], loop[(i + 1) % count], width, false);
    const Pt corner = loop[(i + 1) % count], before = normal(i), after = normal((i + 1) % count);
    add_line_triangle(pieces, corner, {corner.x + before.x, corner.y + before.y}, {corner.x + after.x, corner.y + after.y});
    add_line_triangle(pieces, corner, {corner.x - before.x, corner.y - before.y}, {corner.x - after.x, corner.y - after.y});
  }
}

std::vector<Piece> build(int colour, bool simple) {
  const Shape shape = shape_of(colour, simple);
  const float outline = simple ? 0.26f : 0.18f, scale = 1.0f - outline * 0.5f;
  std::vector<Piece> pieces;
  add_fill(pieces, shape.body, scale, kFill);
  for (const std::vector<Pt>& light : shape.lights) add_fill(pieces, light, scale, kLight);
  std::vector<Pt> loop;
  for (const Pt& point : shape.body) loop.push_back({point.x * scale, point.y * scale});
  add_loop(pieces, loop, outline);
  for (const Shape::Line& line : shape.lines)
    add_line(pieces, {line.from.x * scale, line.from.y * scale}, {line.to.x * scale, line.to.y * scale}, line.width > 0.0f ? line.width : outline, true);
  return pieces;
}

const std::vector<Piece>& pieces_of(int colour, bool simple) {
  static const std::array<std::array<std::vector<Piece>, kDetailLevels>, kElementIconCount> kCache = [] {
    std::array<std::array<std::vector<Piece>, kDetailLevels>, kElementIconCount> cache;
    for (int index = 0; index < kElementIconCount; ++index)
      for (int level = 0; level < kDetailLevels; ++level) cache[static_cast<size_t>(index)][static_cast<size_t>(level)] = build(index, level == 1);
    return cache;
  }();
  return kCache[static_cast<size_t>(colour)][simple ? 1 : 0];
}

bool symbols_on = false;

}  // namespace

void set_element_symbols(bool on) { symbols_on = on; }
bool element_symbols() { return symbols_on; }

void element_icon(int colour, float cx, float cy, float size, float pixels, std::vector<IconTriangle>& out, int light) {
  if (colour < 0 || colour >= kElementIconCount) return;
  const float half = size * 0.5f;
  const int dim = std::clamp(light, 0, 255);
  const u8* line = colour == kBlack ? kBrightLine : kDarkLine;
  const u32 colours[3] = {pack(kFillRgb[colour], dim), pack(line, dim), pack(kLightRgb[colour], dim)};
  for (const Piece& piece : pieces_of(colour, pixels < kSimpleIconPixels)) {
    IconTriangle tri;
    for (size_t corner = 0; corner < 3; ++corner) {
      tri.xy[corner * 2] = cx + piece.xy[corner * 2] * half;
      tri.xy[corner * 2 + 1] = cy + piece.xy[corner * 2 + 1] * half;
    }
    tri.colour = colours[piece.tone];
    out.push_back(tri);
  }
}

std::vector<std::array<int, 3>> triangulate_polygon(const std::vector<std::array<float, 2>>& points) {
  std::vector<std::array<int, 3>> triangles;
  const int count = static_cast<int>(points.size());
  if (count < 3) return triangles;
  std::vector<int> order(static_cast<size_t>(count));
  std::iota(order.begin(), order.end(), 0);
  float area = 0.0f;
  for (int i = 0; i < count; ++i) {
    const auto& from = points[static_cast<size_t>(i)];
    const auto& to = points[static_cast<size_t>((i + 1) % count)];
    area += from[0] * to[1] - to[0] * from[1];
  }
  if (area < 0.0f) std::reverse(order.begin(), order.end());
  auto inside = [&](const std::array<float, 2>& point, int a, int b, int c) {
    const auto& pa = points[static_cast<size_t>(a)];
    const auto& pb = points[static_cast<size_t>(b)];
    const auto& pc = points[static_cast<size_t>(c)];
    return cross(pa, pb, point) >= 0.0f && cross(pb, pc, point) >= 0.0f && cross(pc, pa, point) >= 0.0f;
  };
  while (order.size() > 3) {
    const int size = static_cast<int>(order.size());
    bool clipped = false;
    for (int i = 0; i < size && !clipped; ++i) {
      const int prev = order[static_cast<size_t>((i + size - 1) % size)], cur = order[static_cast<size_t>(i)], next = order[static_cast<size_t>((i + 1) % size)];
      if (cross(points[static_cast<size_t>(prev)], points[static_cast<size_t>(cur)], points[static_cast<size_t>(next)]) <= 1e-7f) continue;
      bool ear = true;
      for (const int other : order)
        if (other != prev && other != cur && other != next && inside(points[static_cast<size_t>(other)], prev, cur, next)) {
          ear = false;
          break;
        }
      if (!ear) continue;
      triangles.push_back({prev, cur, next});
      order.erase(order.begin() + i);
      clipped = true;
    }
    if (!clipped) break;
  }
  if (order.size() == 3) triangles.push_back({order[0], order[1], order[2]});
  return triangles;
}

}  // namespace oc
