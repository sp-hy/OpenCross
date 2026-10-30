#version 450
// Corner c, influence i at location 2c + i: c 0 = this vertex, 1 / 2 = the triangle's next corners, 3 = the quad partner.
layout(location = 0) in vec3 in_pos[8];
layout(location = 8) in uvec4 in_joint01;   // corner 0 (xy), corner 1 (zw)
layout(location = 9) in uvec4 in_joint23;   // corner 2 (xy), corner 3 (zw)
layout(location = 10) in vec4 in_weight01;
layout(location = 11) in vec4 in_weight23;
layout(location = 12) in vec4 in_uvq;       // uv, quad half (0 triangle, 1 first, 2 second), this vertex's corner index k in its triangle
layout(location = 13) in vec4 in_normal;    // VDDM normal / 128 of corner 0, w = the joint that rotates it
layout(location = 14) in vec4 in_rgb;       // the corner's packet colour / 128 (1 = neutral)
layout(location = 15) in vec4 in_smooth;    // rigid meshes: the corner's smoothed model-space normal, w = 1 when present
struct FrameSlot {
  mat4 view_proj;
  mat4 view;
  mat4 model;
  vec4 slab;  // x / y: ordering-table slab (ot mode): a face is drawn when x < slot <= y; z / w: PS1 jitter (PS1 pixels per NDC unit, 0 = off)
  // PS1 lit path (DrawParams::Light): [0] = back colour / 256, w = 1 when lit; [1..3] = light matrix rows (direction * k / 4096);
  // [4..6] = light colours / 256.
  vec4 lit[9];  // [7] = weapon specular light direction (w = 1 on), [8] = eye position (DrawParams::Light::spec)
  // [Enhancements] el[0] = (EnhancedLighting on, key strength, SpellLights count, SmoothReflections on); el[1 + 2i] = light i position (render world) + radius;
  // el[2 + 2i] = light i colour x intensity. All zero = the PS1 look.
  vec4 el[18];  // [17] = scene ambient multiplier (rgb, w = 1 on); el[2 + 2i].w = 1: the light is a PS1 slot light (linear falloff)
  mat4 palette[128];
};
layout(std430, set = 0, binding = 0) readonly buffer Frames { FrameSlot slots[]; };
#define u slots[int(pc.slot.x + 0.5)]
layout(set = 1, binding = 0) uniform Push { vec4 color; vec4 misc; vec4 tint; vec4 water; vec4 fog; vec4 fog_rng; vec4 ot; vec4 ot2; vec4 slot; } pc;
layout(location = 0) out vec3 uv;           // (uv * k, k): k = clip w with the PS1 jitter (affine texturing), else 1; the fragment divides
layout(location = 1) out float cam_z;
layout(location = 2) out float local_y;
layout(location = 3) flat out float face_slot;
layout(location = 4) flat out float face_wet;
layout(location = 6) out float world_y;       // smooth water line: this corner's world y (interpolated)
layout(location = 7) flat out float face_all;  // smooth water line: 1 when every corner is below the level
layout(location = 8) flat out vec2 slab_v;     // smooth water line: the pass's slab for the fragment shader
layout(location = 5) out vec4 v_light;      // lit path: the corner's colour multiplier (Gouraud, GT3), w = 1 when lit
layout(location = 9) out vec3 v_extra;      // [Enhancements] additive colour multiplier (key light shading + spell lights), 0 = off
layout(location = 10) flat out vec3 v_amb;  // [Enhancements] scene ambient multiplier (1 = off; not on the PS1 lit path, which carries the back colour itself)
layout(location = 11) out vec3 v_corner;    // the corner's packet colour multiplier (Gouraud), applied on the flat path
vec4 skin(vec3 p0, vec3 p1, uvec2 j, vec2 w) {
  return u.palette[j.x] * vec4(p0, 1.0) * w.x + u.palette[j.y] * vec4(p1, 1.0) * w.y;
}
// GTE NCLIP on screen coordinates (y down): SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1.
float nclip(vec2 a, vec2 b, vec2 c) {
  return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
}
void main() {
  vec4 sk0 = skin(in_pos[0], in_pos[1], in_joint01.xy, in_weight01.xy);
  vec4 wl[4];
  wl[0] = u.model * vec4(sk0.xyz, 1.0);
  wl[1] = u.model * vec4(skin(in_pos[2], in_pos[3], in_joint01.zw, in_weight01.zw).xyz, 1.0);
  wl[2] = u.model * vec4(skin(in_pos[4], in_pos[5], in_joint23.xy, in_weight23.xy).xyz, 1.0);
  wl[3] = u.model * vec4(skin(in_pos[6], in_pos[7], in_joint23.zw, in_weight23.zw).xyz, 1.0);
  gl_Position = u.view_proj * wl[0];
  cam_z = (u.view * wl[0]).z;
  float aff = 1.0;
  if (u.slab.z > 0.0 && gl_Position.w > 0.0) {  // [Video] Ps1Jitter: GTE integer screen coordinates, GPU affine texture mapping
    vec2 g = vec2(u.slab.z, u.slab.w);
    gl_Position.xy = floor(gl_Position.xy / gl_Position.w * g + 0.5) / g * gl_Position.w;
    aff = gl_Position.w;
  }
  uv = vec3(in_uvq.xy * aff, aff);
  if (pc.fog.w > 1.5 || pc.fog.w < -1.5) {
    // Env-mapped part (shading flag 0x80 with 0x20 / 0x40; battle env map): the corner's UV is built from its skinned normal bytes (the
    // VDDM normal turned by the joint matrix and the model's placement, PS1 axes: y down): u = (nx >> 1) + 0x40, v = (ny >> 1) + 0xC0 inside the
    // face's own 128-texel half of the page (pc.fog.w = the texture width: the page bit is folded into u).
    uint nj = uint(in_normal.w + 0.5);
    vec3 nw = mat3(u.model) * (mat3(u.palette[nj]) * in_normal.xyz);
    vec3 nb = clamp(vec3(nw.x, -nw.y, nw.z) * 128.0, -128.0, 127.0);
    // pc.fog.w < 0: the STOCK env page (no shading flag 0x80): the same bytes, but u carries no half-page base
    float tw = abs(pc.fog.w);
    float base = pc.fog.w < 0.0 ? 0.0 : floor(in_uvq.x * tw / 128.0) * 128.0;  // the page bit adds 128 texels (64 halfwords at 8 bpp), so the window follows the face's half
    // The PS1 corner UV is a whole texel (s8 normal >> 1): a slowly turning part steps its reflection one texel at a time (FATE's idle: a corner
    // moves ~0.07-0.1 texel per 60 Hz frame, so it jumps every ~10-14 frames; retail and ours alike). [Enhancements] SmoothReflections
    // (el[0].w = 1) keeps the fraction, so the reflection slides with the (interpolated) pose instead.
    vec2 h = u.el[0].w > 0.5 ? nb.xy * 0.5 : floor(nb.xy * 0.5);
    vec2 e = vec2(h.x + 64.0 + base, h.y + 192.0) / vec2(tw, 256.0);
    uv = vec3(e * aff, aff);
  }
  local_y = sk0.y;
  v_corner = in_rgb.rgb;
  v_light = vec4(pc.tint.rgb * v_corner, 0.0);
  if (u.lit[0].w > 0.5) {
    // PS1 lit path: the skinner rotates the VDDM normal (n << 5) by the joint's world matrix and clamps it to
    // +-0xFFF; the face builder runs NCCT on it: I_i = max(0, LLM_i . N) (lm), c = max(0, BK + LCM . I), corner colour =
    // min(RGBC * c, 255) with RGBC = the flat colour (pc.tint * 128). No depth cue on this path. The 8-bit corner colour is Gouraud-interpolated
    // by the GPU; interpolating the multiplier is the same up to the per-corner clamp, kept here.
    uint nj = uint(in_normal.w + 0.5);
    vec3 n = clamp(mat3(u.model) * mat3(u.palette[nj]) * in_normal.xyz, vec3(-4095.0 / 4096.0), vec3(4095.0 / 4096.0));
    vec3 c = u.lit[0].xyz;
    for (int i = 0; i < 3; ++i) c += u.lit[4 + i].xyz * clamp(dot(u.lit[1 + i].xyz, n), 0.0, 32767.0 / 4096.0);
    c = clamp(c, 0.0, 32767.0 / 4096.0);
    v_light = vec4(min(pc.tint.rgb * v_corner * c, vec3(255.0 / 128.0) * pc.tint.a), 1.0);
  }
  v_extra = vec3(0.0);
  v_amb = (u.el[17].w > 0.5 && u.lit[0].w < 0.5) ? u.el[17].xyz : vec3(1.0);
  if (u.el[0].x > 0.5 || u.el[0].z > 0.5) {
    // [Enhancements] EnhancedLighting: the vertex normal against a camera-relative key light (from above, toward the camera, a little from the left;
    // 'up' = the model's own up in this world, so it holds in the y-down field world too): +-0.35 of the colour multiplier (el[0].y * dot * 0.5);
    // the fragment never lets the result fall below 65 % of the PS1 colour. Not on the PS1-lit path (it has its own normals shading).
    // SpellLights: per light (1 - d / radius)^2 x colour, facing mix 0.4 (turned away) .. 1 (towards), 0.7 without a normal.
    vec3 nraw = in_normal.xyz;
    bool hasn = dot(nraw, nraw) > 1e-6;
    vec3 nw = hasn ? normalize(mat3(u.model) * mat3(u.palette[uint(in_normal.w + 0.5)]) * nraw) : vec3(0.0);
    if (u.el[0].x > 0.5 && u.lit[0].w < 0.5 && hasn) {
      vec3 up = normalize(mat3(u.model) * vec3(0.0, -1.0, 0.0));
      vec3 right = vec3(u.view[0][0], u.view[1][0], u.view[2][0]);
      vec3 fwd = vec3(u.view[0][2], u.view[1][2], u.view[2][2]);
      vec3 key = normalize(up * 0.70 - fwd * 0.55 - right * 0.35);
      v_extra += pc.tint.rgb * (u.el[0].y * (dot(nw, key) * 0.5));
    }
    float vis = clamp(max(pc.tint.r, max(pc.tint.g, pc.tint.b)), 0.0, 1.0);
    int nl = int(u.el[0].z + 0.5);
    vec3 spell = vec3(0.0);
    for (int i = 0; i < nl; ++i) {
      vec3 d = u.el[1 + 2 * i].xyz - wl[0].xyz;
      float dist = length(d);
      bool ps1 = u.el[2 + 2 * i].w > 0.5;
      if (ps1 && u.lit[0].w > 0.5) continue;  // the lit path already shades from the PS1 slots
      float x2 = dist * dist / max(u.el[1 + 2 * i].w * u.el[1 + 2 * i].w, 1.0);  // (d / radius)^2
      float att = clamp(1.0 - x2, 0.0, 1.0);
      att = ps1 ? clamp(1.0 - sqrt(x2), 0.0, 1.0) : att;  // PS1 slot: linear (k = intensity - dist * falloff); else 1 - x^2 (0.9 at x = 0.33, 0.75 at 0.5)
      float facing = hasn ? mix(0.5, 1.0, clamp(dot(nw, d / max(dist, 1.0)) * 0.5 + 0.5, 0.0, 1.0)) : 0.8;
      spell += u.el[2 + 2 * i].rgb * (att * facing * vis);
    }
    v_extra += spell / (1.0 + max(spell.r, max(spell.g, spell.b)) / 0.8);  // soft knee on the largest channel (hue kept): stacked lights stay below +0.8 (lights.h kLightKnee)
  }
  // The triangle in source order (g[i] = corner i), identical in all three invocations, so every per-face value agrees bit for bit.
  int k = int(in_uvq.w + 0.5);
  vec4 g0 = wl[(3 - k) % 3], g1 = wl[(4 - k) % 3], g2 = wl[(5 - k) % 3];
  if (u.lit[7].w > 0.5) {
    // Weapon specular: n = the face normal (the loader's per-face normal table, rotated by the model matrix; here
    // from the triangle, facing the camera since only front faces are drawn), l = the unit vector toward the stage light, v = the
    // unit vector from this corner to the eye point (the camera in battle; the menu's (d sin yaw, d cos yaw, 0), which is not the camera, so
    // the normal is not turned toward it). r = 2 (n . l) n - l (VectorNormalSS), s = r . v; term = min(255, 256 s^9)
    // (`s*s >> 12` twice, `(s*s >> 12) * s >> 16`), added to the packet colour byte (clamped 255) -> here to the colour multiplier (x / 128).
    vec3 fn = cross(g1.xyz - g0.xyz, g2.xyz - g0.xyz);
    vec3 eye = u.lit[8].xyz;
    if (dot(fn, inverse(u.view)[3].xyz - g0.xyz) < 0.0) fn = -fn;
    float fl = length(fn);
    if (fl > 0.0) {
      // [Enhancements] Lighting: the same term at each corner on the smoothed normal (turned to this face's side), interpolated across the
      // face, so the glint blends over neighbouring faces instead of lighting them one by one.
      vec3 n = fn / fl, l = u.lit[7].xyz;
      if (u.el[0].x > 0.5 && in_smooth.w > 0.5) {
        mat3 nm = transpose(inverse(mat3(u.model) * mat3(u.palette[in_joint01.x])));
        vec3 ns = normalize(nm * in_smooth.xyz);
        n = dot(ns, n) < 0.0 ? -ns : ns;
      }
      float nl = dot(n, l), term = 0.0;
      if (nl > 0.0) {
        vec3 r = normalize(2.0 * nl * n - l);
        vec3 tv = eye - wl[0].xyz;
        float s = length(tv) > 0.0 ? dot(r, normalize(tv)) : 0.0;
        if (s > 0.0) term = min(255.0, 256.0 * pow(s, 9.0));
      }
      v_light = vec4(min(v_light.rgb + vec3(term / 128.0), vec3(255.0 / 128.0)), 1.0);
    }
  }
  face_slot = 0.0;
  face_wet = 0.0;
  face_all = 0.0;
  world_y = wl[0].y;
  slab_v = u.slab.xy;
  if (pc.ot.x > 0.5) {
    // The face's ordering-table slot: AVSZ3 / AVSZ4 average z >> 4, or the nearest z >> 4 (op 93). A quad is one primitive: both halves
    // take the slot of all four corners, summed in source order (v0..v3) so the halves agree bit for bit.
    float sz;
    if (in_uvq.z > 0.5) {
      vec4 v0 = in_uvq.z > 1.5 ? wl[3] : g0, v2 = in_uvq.z > 1.5 ? g0 : g2, v3 = in_uvq.z > 1.5 ? g2 : wl[3];
      float z0 = (u.view * v0).z, z1 = (u.view * g1).z, z2 = (u.view * v2).z, z3 = (u.view * v3).z;
      sz = pc.ot.y > 0.5 ? min(min(z0, z1), min(z2, z3)) : (z0 + z1 + z2 + z3) / 4.0;
    } else {
      float z0 = (u.view * g0).z, z1 = (u.view * g1).z, z2 = (u.view * g2).z;
      sz = pc.ot.y > 0.5 ? min(z0, min(z1, z2)) : (z0 + z1 + z2) / 3.0;
    }
    face_slot = floor(max(sz, 0.0) / 16.0);
    // A face is under water when any corner's world y is below the level (level - VY < 0).
    face_wet = (pc.misc.z > 0.5 && max(g0.y, max(g1.y, g2.y)) > pc.misc.w) ? 1.0 : 0.0;
    // The face's ordering-table index as the fragment shader files it; outside the slab: collapsed (not drawn in this pass).
    float full = face_slot + pc.ot.z + (face_wet > 0.5 ? pc.ot.w : 0.0);
    bool out_of_slab = full <= u.slab.x || full > u.slab.y;
    if (pc.ot.x > 1.5) {
      // Smooth water line: a face straddling the level keeps both slots (dry fragments the normal one, submerged ones the pushed-back
      // one; the fragment shader files each); it is collapsed only when neither slot is in this pass's slab.
      face_all = (pc.misc.z > 0.5 && min(g0.y, min(g1.y, g2.y)) > pc.misc.w) ? 1.0 : 0.0;
      float fd = face_slot + pc.ot.z, fw = fd + pc.ot.w;
      bool dry_in = face_all < 0.5 && fd > u.slab.x && fd <= u.slab.y;
      bool wet_in = face_wet > 0.5 && fw > u.slab.x && fw <= u.slab.y;
      out_of_slab = !(dry_in || wet_in);
    }
    if (out_of_slab) {
      gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
      return;
    }
  }
  if (abs(pc.color.w) > 0.5) {
    // PS1 back-face test: a triangle is drawn when NCLIP > 0; a quad when NCLIP(v0,v1,v2) > 0 or NCLIP(v1,v2,v3) <= 0,
    // i.e. when either half faces the camera (the second half here is (v2,v1,v3): its NCLIP >= 0).
    vec4 q0 = u.view_proj * g0, q1 = u.view_proj * g1, q2 = u.view_proj * g2, q3 = u.view_proj * wl[3];
    float role = in_uvq.z;
    if (q0.w > 0.0 && q1.w > 0.0 && q2.w > 0.0 && (role < 0.5 || q3.w > 0.0)) {
      vec2 s0 = q0.xy / q0.w, s1 = q1.xy / q1.w, s2 = q2.xy / q2.w, s3 = q3.xy / q3.w;
      float a = nclip(s0, s1, s2);
      bool front = a > 0.0;
      // Both halves evaluate the same two expressions with the same operand order: the quad is kept or dropped as a whole.
      if (role > 1.5) front = nclip(s3, s1, s0) > 0.0 || a >= 0.0;  // second half (v2,v1,v3): (v0,v1,v2) = (s3,s1,s0)
      else if (role > 0.5) front = a > 0.0 || nclip(s2, s1, s3) >= 0.0;  // first half (v0,v1,v2): (v2,v1,v3) = (s2,s1,s3)
      if (pc.color.w < 0.0) front = !front;  // debug: inverted test
      if (!front) gl_Position = vec4(0.0, 0.0, 2.0, 1.0);  // all three corners: a degenerate point beyond the far plane
    }
  }
  // SDL_GPU clip space has +y up on every backend; everything above works in the PS1's y-down frame.
  gl_Position.y = -gl_Position.y;
}
