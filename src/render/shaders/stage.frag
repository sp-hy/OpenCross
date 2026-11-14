#version 450
// AFFINE (stage_affine.*.inc, built with -DAFFINE): `noperspective` on every non-flat varying = the PS1 GPU's screen-linear
// interpolation of texture coordinates and colours (the light position cam_pos stays perspective-correct).
#ifdef AFFINE
#define VARY noperspective
#else
#define VARY
#endif
layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 3, binding = 0) uniform Push { mat4 clip; vec4 fogc; vec4 color; vec4 params; vec4 uvw; vec4 jit; vec4 pinv; vec4 lpos[3]; vec4 lcol[3]; } pc;
layout(location = 8) in vec3 cam_pos;
VARY layout(location = 0) in vec2 uv;
VARY layout(location = 1) in vec3 color;
VARY layout(location = 2) in float cam_z;
VARY layout(location = 3) in vec2 uv_page;
VARY layout(location = 4) in vec2 uv_tex;
VARY layout(location = 5) in vec2 uv_scale;
layout(location = 6) flat in float ot_z;
VARY layout(location = 7) in float fog_p;
layout(location = 0) out vec4 out_color;
// View depth (render/post_fx.h): the camera z in 1/256 units as 24 bits in rgb, and as the depth (z / 65536).
vec4 view_depth_color(float z) {
  float v = clamp(floor(z * 256.0 + 0.5), 0.0, 16777215.0);
  float hi = floor(v / 65536.0);
  float mid = floor((v - hi * 65536.0) / 256.0);
  return vec4(hi, mid, v - hi * 65536.0 - mid * 256.0, 255.0) / 255.0;
}
void main() {
  vec2 t = uv;
  // +16: [Enhancements] EdgeBlend strip (StagePipeline::draw_seams): the face drawn again with its neighbour's texture mirrored across the shared edge;
  // uv_tex.x = the fragment's distance from the seam edge in band widths (0 on the edge), uv_tex.y = 1: alpha 1/2 on the edge fading to 0 one band away.
  // +32: the texture is a copy of the picture; a copied pixel that is black in 15-bit colour is a transparent texel. +96: a copy made with the
  // mask bit set, whose black pixels are opaque.
  bool opaque_copy = pc.params.y >= 95.5;
  float pyo = opaque_copy ? pc.params.y - 64.0 : pc.params.y;
  bool copy = pyo >= 31.5;
  float pyc = copy ? pyo - 32.0 : pyo;
  bool seam = pyc >= 15.5;
  float pyr = seam ? pyc - 16.0 : pyc;
  float py = pyr >= 7.5 ? pyr - 8.0 : pyr;  // +8: HD per-vertex depth cue (vertex shader)
  float seam_a = 1.0;
  if (seam) {
    seam_a = uv_tex.y > 0.0 ? 0.5 * clamp(1.0 - uv_tex.x / uv_tex.y, 0.0, 1.0) : 0.0;
    if (seam_a <= 0.0) discard;
  }
  bool scroll = py >= 3.5;
  float mode = scroll ? py - 4.0 : py;
  vec4 c;
  if (scroll && uv_scale.x > 0.0) {
    // op-36 texture window: the GPU wraps each texel inside (off, off + win); a window of 0 wraps in the 256-texel page
    float sflags = floor(pc.uvw.z / 65536.0);
    float offs = pc.uvw.z - sflags * 65536.0;
    vec2 off = vec2(mod(offs, 256.0), floor(offs / 256.0));
    vec2 win = vec2(mod(pc.uvw.w, 256.0), floor(pc.uvw.w / 256.0));
    vec2 span = vec2(win.x > 0.0 ? win.x : 256.0, win.y > 0.0 ? win.y : 256.0);
    // texel k is centred on k (it covers k - 1/2 .. k + 1/2), so the window starts half a texel before its first texel
    vec2 lo = vec2(win.x > 0.0 ? off.x : 0.0, win.y > 0.0 ? off.y : 0.0) - 0.5;
    if (mod(sflags, 2.0) >= 1.0) {
      // Scroll flag 1 (bilinear): the four image pixels of the footprint each wrap inside the window, so the filter blends across the wrap
      // (and never reaches the neighbouring page of the atlas).
      vec2 px = abs(uv_scale) * vec2(textureSize(tex, 0));
      vec2 len = span * px;
      vec2 p = (uv_tex - lo) * px - 0.5;
      vec2 p0 = floor(p);
      vec2 f = p - p0;
      vec2 a = mod(p0, len), b = mod(p0 + 1.0, len);
      vec4 c00 = texture(tex, uv_page + (lo + (a + 0.5) / px) * uv_scale);
      vec4 c10 = texture(tex, uv_page + (lo + (vec2(b.x, a.y) + 0.5) / px) * uv_scale);
      vec4 c01 = texture(tex, uv_page + (lo + (vec2(a.x, b.y) + 0.5) / px) * uv_scale);
      vec4 c11 = texture(tex, uv_page + (lo + (b + 0.5) / px) * uv_scale);
      c = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
    } else {
      // The gradients of the unwrapped coordinate: the wrap's jump would widen an anisotropic footprint across the atlas.
      vec2 unwrapped = uv_tex * uv_scale;
      c = textureGrad(tex, uv_page + (lo + mod(uv_tex - lo, span)) * uv_scale, dFdx(unwrapped), dFdy(unwrapped));
    }
  } else {
    c = texture(tex, t);
  }
  if (c.a < 0.5) discard;
  if (copy && !opaque_copy && all(lessThan(c.rgb, vec3(7.5 / 255.0)))) discard;
  // jit.w = 2: the view depth (StagePipeline::set_view_depth) instead of the picture.
  if (pc.jit.w > 1.5) {
    out_color = view_depth_color(cam_z);
    gl_FragDepth = clamp(cam_z / 65536.0, 0.0, 1.0);
    return;
  }
  // The packet builders multiply the corner colour by the object colour (DCPL: saturates at 255), then the depth cue (DPCS / DPCT) moves
  // it toward the far colour by fog_p; textured colours are modulation values (128 = 1.0), untextured ones raw (255 = 1.0).
  bool textured = uv_scale.x > 0.0;
  vec3 cm = min(color * pc.color.rgb, vec3(textured ? 255.0 / 128.0 : 1.0));
  if (fog_p > 0.0) cm = mix(cm, pc.fogc.rgb / (textured ? 128.0 : 255.0), fog_p);
  vec3 add = vec3(0.0);
  if (pc.jit.w > 0.5) {  // [Enhancements] SpellLights: point lights (camera space) projected onto the opaque scenery; the face normal from the screen derivatives
    vec3 fn = cross(dFdx(cam_pos), dFdy(cam_pos));
    float fl = length(fn);
    vec3 n = fl > 0.0 ? fn / fl : vec3(0.0);
    for (int i = 0; i < 3; ++i) {
      vec3 d = pc.lpos[i].xyz - cam_pos;
      float dist = length(d);
      float rad = abs(pc.lpos[i].w);  // a negative radius marks a PS1 slot light (linear falloff)
      float x2 = dist * dist / max(rad * rad, 1.0);  // (d / radius)^2
      float att = clamp(1.0 - x2, 0.0, 1.0);
      att = pc.lpos[i].w < 0.0 ? clamp(1.0 - sqrt(x2), 0.0, 1.0) : att;  // PS1 slot: linear; else 1 - x^2 (0.9 at x = 0.33, 0.75 at 0.5)
      float facing = fl > 0.0 ? mix(0.8, 1.0, abs(dot(n, d / max(dist, 1.0)))) : 0.9;  // a light at floor level still lights the floor
      add += pc.lcol[i].rgb * (att * facing);
    }
    add = add / (1.0 + max(add.r, max(add.g, add.b)) / 0.8);  // soft knee on the largest channel (hue kept): stacked lights stay below +0.8 (lights.h kLightKnee)
    cm = cm * vec3(pc.lcol[0].w, pc.lcol[1].w, pc.lcol[2].w) * (1.0 + add);  // stage colour x ambient (BK / 0x80) x (1 + sum of the point lights)
  }
  vec3 rgb = clamp(c.rgb * cm, 0.0, 1.0) * pc.color.w;
  out_color = vec4(rgb, pc.params.z * seam_a);
  // Ordering-table depth: slot = (average z of the packet >> 4) + bias, one value per packet, so two
  // coplanar floors with different biases resolve the way the PS1 draws them (the whole packet wins or loses, never per pixel).
  // mode: 1 = fixed slot (the bias alone), 2 = per-fragment z (dynamic geometry), else the packet slot.
  float slot = mode > 1.5 ? cam_z / 16.0 + pc.params.x : mode > 0.5 ? pc.params.x : floor(ot_z / 16.0) + pc.params.x;
  gl_FragDepth = clamp(slot / 8192.0, 0.0, 1.0);
}
