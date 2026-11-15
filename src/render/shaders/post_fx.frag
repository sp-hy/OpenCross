#version 450
// [Shaders] Ssao / DepthOfField passes (render/post_fx.h), one mode per pass. The view depth is the camera z in 1/256 units as 24 bits in rgb
// (mesh.frag / stage.frag built with VIEW_DEPTH), cleared to the far value. Half-resolution texel h covers view-depth texel 2h.
layout(set = 2, binding = 0) uniform sampler2D t0;
layout(set = 2, binding = 1) uniform sampler2D t1;
layout(set = 2, binding = 2) uniform sampler2D t2;
// opts: mode (0 SSAO, 1 AO blur, 2 DoF filter, 3 DoF blur, 4 combine, 5 composite), SSAO on, DoF on, largest blur radius (half-resolution
// pixels); proj: ax, cx, ay, cy (NDC x = ax x / z + cx, y down likewise); focus: focus z, AO radius (world units), blur direction;
// band: sharp band in front of / behind the focus, ramp to the full blur behind / in front (fractions of the focus distance); extra: the
// camera z where the AO starts to fade and where it is gone (0 = no limit).
layout(set = 3, binding = 0) uniform Push { vec4 opts; vec4 proj; vec4 focus; vec4 band; vec4 extra; } pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

const float kSky = 65000.0;

float view_z(sampler2D depth, ivec2 texel) {
  ivec2 size = textureSize(depth, 0);
  vec3 encoded = texelFetch(depth, clamp(texel, ivec2(0), size - 1), 0).rgb;
  return dot(floor(encoded * 255.0 + 0.5), vec3(65536.0, 256.0, 1.0)) / 256.0;
}

vec3 view_pos(sampler2D depth, ivec2 texel, float z) {
  vec2 ndc = (vec2(texel) + 0.5) / vec2(textureSize(depth, 0)) * 2.0 - 1.0;
  return vec3((ndc.x - pc.proj.y) / pc.proj.x * z, (ndc.y - pc.proj.w) / pc.proj.z * z, z);
}

// The blur amount at camera z: 0 inside the sharp band around the focus, 1 a ramp beyond it; the sky is fully blurred.
float coc(float z) {
  if (z >= kSky) return 1.0;
  float rel = (z - pc.focus.x) / pc.focus.x;
  return rel >= 0.0 ? smoothstep(pc.band.y, pc.band.y + pc.band.z, rel) : smoothstep(pc.band.x, pc.band.x + pc.band.w, -rel);
}

float ssao() {
  ivec2 full = ivec2(gl_FragCoord.xy) * 2;
  float z = view_z(t0, full);
  if (z >= kSky || (pc.extra.y > 0.0 && z >= pc.extra.y)) return 1.0;
  vec3 pos = view_pos(t0, full, z);
  vec3 right = view_pos(t0, full + ivec2(1, 0), view_z(t0, full + ivec2(1, 0))), left = view_pos(t0, full - ivec2(1, 0), view_z(t0, full - ivec2(1, 0)));
  vec3 below = view_pos(t0, full + ivec2(0, 1), view_z(t0, full + ivec2(0, 1))), above = view_pos(t0, full - ivec2(0, 1), view_z(t0, full - ivec2(0, 1)));
  vec3 dx = abs(right.z - z) < abs(z - left.z) ? right - pos : pos - left;
  vec3 dy = abs(below.z - z) < abs(z - above.z) ? below - pos : pos - above;
  vec3 normal = normalize(cross(dx, dy));
  if (dot(normal, pos) > 0.0) normal = -normal;
  float radius = pc.focus.y;
  vec2 size = vec2(textureSize(t0, 0));
  float radius_px = clamp(radius * pc.proj.x * 0.5 * size.x / z, 3.0, 0.15 * size.y);
  float spin = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  float occlusion = 0.0;
  const int kSamples = 12;
  for (int k = 0; k < kSamples; ++k) {
    float r = radius_px * (float(k) + 0.5) / float(kSamples);
    float angle = spin + float(k) * 2.3999632;
    ivec2 tap = full + ivec2(floor(vec2(cos(angle), sin(angle)) * r + 0.5));
    vec3 v = view_pos(t0, tap, view_z(t0, tap)) - pos;
    float d2 = dot(v, v);
    float cosine = dot(v, normal) * inversesqrt(d2 + 1e-3);
    occlusion += max(cosine - 0.2, 0.0) * clamp(1.0 - d2 / (radius * radius), 0.0, 1.0);
  }
  float ao = clamp(1.0 - 3.0 * occlusion / float(kSamples), 0.0, 1.0);
  return pc.extra.y > 0.0 ? mix(ao, 1.0, smoothstep(pc.extra.x, pc.extra.y, z)) : ao;
}

float ao_blur() {
  ivec2 h = ivec2(gl_FragCoord.xy);
  ivec2 size = textureSize(t0, 0);
  ivec2 dir = ivec2(pc.focus.zw);
  float z = view_z(t1, h * 2);
  float sum = 0.0, total = 0.0;
  for (int k = -4; k <= 4; ++k) {
    ivec2 tap = clamp(h + dir * k, ivec2(0), size - 1);
    float zs = view_z(t1, tap * 2);
    float w = exp(-float(k * k) / 8.0) * max(0.0, 1.0 - abs(zs - z) / (0.04 * z + 8.0));
    sum += texelFetch(t0, tap, 0).r * w;
    total += w;
  }
  return total > 0.0 ? sum / total : texelFetch(t0, h, 0).r;
}

// The 2 x 2 view-depth texels' colours weighted by their blur amounts, so a texel on a silhouette holds only the blurred side's colour and a
// sharp unit does not bleed into the blur around it; the amount is the largest of the four.
vec4 dof_filter() {
  vec2 full_size = vec2(textureSize(t1, 0));
  ivec2 full = ivec2(gl_FragCoord.xy) * 2;
  vec3 rgb = vec3(0.0), sharp_rgb = vec3(0.0);
  float total = 0.0, amount = 0.0;
  for (int k = 0; k < 4; ++k) {
    ivec2 texel = full + ivec2(k & 1, k >> 1);
    float c = coc(view_z(t1, texel));
    vec3 colour = texture(t0, (vec2(texel) + 0.5) / full_size).rgb;
    rgb += colour * c;
    sharp_rgb += colour;
    total += c;
    amount = max(amount, c);
  }
  return vec4(total > 1e-3 ? rgb / total : 0.25 * sharp_rgb, amount);
}

// The blur amount at a view-depth texel, eased in beside a nearer, sharper surface: scaled by the share of the 5 x 5 texels around it that
// are not one, so the blur fades in over two pixels from a unit's silhouette instead of switching per pixel along its stair-stepped edge.
float edge_amount(ivec2 texel, float z) {
  float c = coc(z);
  if (c <= 0.0) return 0.0;
  float occluders = 0.0;
  for (int j = -2; j <= 2; ++j)
    for (int i = -2; i <= 2; ++i) {
      float zs = view_z(t2, texel + ivec2(i, j));
      if (zs < z - (0.05 * z + 8.0) && coc(zs) < c) occluders += 1.0;
    }
  return c * (1.0 - occluders / 25.0);
}

vec4 dof_blur() {
  vec2 size = vec2(textureSize(t0, 0));
  vec4 centre = texture(t0, uv);
  float reach = centre.a * pc.opts.w;
  if (reach < 0.5) return centre;
  vec3 sum = centre.rgb;
  float total = 1.0;
  const int kTaps = 32;
  for (int k = 0; k < kTaps; ++k) {
    float r = reach * sqrt((float(k) + 0.5) / float(kTaps));
    float angle = float(k) * 2.3999632;
    vec4 tap = texture(t0, uv + vec2(cos(angle), sin(angle)) * r / size);
    float w = clamp(tap.a * pc.opts.w - r + 1.0, 0.0, 1.0);
    sum += tap.rgb * w;
    total += w;
  }
  return vec4(sum / total, centre.a);
}

vec4 combine() {
  ivec2 texel = ivec2(gl_FragCoord.xy);
  float z = view_z(t2, texel);
  ivec2 half_size = pc.opts.y > 0.5 ? textureSize(t0, 0) : textureSize(t1, 0);
  vec2 hp = (vec2(texel) + 0.5) * 0.5 - 0.5;
  ivec2 h0 = ivec2(floor(hp));
  vec2 frac_part = hp - vec2(h0);
  float ao = 0.0, total = 0.0, blurred_total = 0.0;
  vec3 blurred = vec3(0.0);
  for (int j = 0; j < 2; ++j)
    for (int i = 0; i < 2; ++i) {
      ivec2 h = clamp(h0 + ivec2(i, j), ivec2(0), half_size - 1);
      float bilinear = (i == 0 ? 1.0 - frac_part.x : frac_part.x) * (j == 0 ? 1.0 - frac_part.y : frac_part.y);
      float zh = view_z(t2, h * 2);
      float w = bilinear * (abs(zh - z) < 0.05 * z + 8.0 ? 1.0 : 1e-3) + 1e-6;
      ao += texelFetch(t0, h, 0).r * w;
      total += w;
      // The blurred texel holds its block's farthest, most blurred colour: matched against the block's farthest depth.
      float zb = max(max(zh, view_z(t2, h * 2 + ivec2(1, 0))), max(view_z(t2, h * 2 + ivec2(0, 1)), view_z(t2, h * 2 + ivec2(1, 1))));
      float wb = bilinear * (abs(zb - z) < 0.05 * z + 8.0 ? 1.0 : 1e-3) + 1e-6;
      blurred += texelFetch(t1, h, 0).rgb * wb;
      blurred_total += wb;
    }
  ao = pc.opts.y > 0.5 ? ao / total : 1.0;
  float amount = pc.opts.z > 0.5 ? clamp(edge_amount(texel, z) * pc.opts.w, 0.0, 1.0) : 0.0;
  return vec4(ao * amount * blurred / blurred_total, 1.0 - ao * (1.0 - amount));
}

void main() {
  int mode = int(pc.opts.x + 0.5);
  if (mode == 0) out_color = vec4(ssao(), 0.0, 0.0, 1.0);
  else if (mode == 1) out_color = vec4(ao_blur(), 0.0, 0.0, 1.0);
  else if (mode == 2) out_color = dof_filter();
  else if (mode == 3) out_color = dof_blur();
  else if (mode == 4) out_color = combine();
  else out_color = texture(t0, uv);
}
