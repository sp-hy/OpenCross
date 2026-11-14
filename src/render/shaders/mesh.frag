#version 450
layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 2, binding = 1) uniform sampler2D matcap;
// color.rgb: flat colour, color.w: back-face test (vertex shader). misc.x: 1 = flat colour, misc.y: depth scale, misc.z: 1 = water on,
// misc.w: water level (+y down; model y, or world y in ot mode), tint.rgb: colour multiplier, water.rgb: colour below the level (replaces
// tint), fog.rgb: far colour / 128, fog_rng.xy: fog near / far (GTE depth cue; far <= near = off), fog_rng.z: STP texel filter (0 all,
// 1 texels without STP only, 2 STP texels only; flat faces count as STP), fog_rng.w: 1 = white texels (FE 45 flash CLUT). ot.x: 1 = field ordering-table faces (2 = with the smooth water line), ot.y: nearest-corner sort,
// ot.z: layer bias (slots), ot.w: submerged-face bias (slots); ot2.x: face filter (0 all, 1 dry only, 2 submerged only), ot2.y: 1 = depth
// only (alpha-0 output into a colour-keeping blend), ot2.z: 1 / kDepthRange (slot -> depth-buffer value), ot2.w: output alpha (0.5 = ABR 0).
// slot.y: 1 = [Enhancements] ModelPixelDepth (ot mode: the fragment's own z in the slot space; a nearest-corner face is pulled toward the
// camera by kNearPull slots, never past its own slot, so it stays over the body it cuts into but not over parts well in front of it);
// slot.w: 1 = view depth (MeshPipeline::set_view_depth: the camera z instead of the picture).
// [Shaders] (render/mesh_pipeline.h CharacterShading): shade.x = style (0 off, 1 realistic, 2 toon, 3 cel, 4 matcap), shade.y = 1 on the
// outline pass, slot.z = the style's strength; rim = colour + strength (0 = off); glow = colour + strength.
layout(set = 3, binding = 0) uniform Push { vec4 color; vec4 misc; vec4 tint; vec4 water; vec4 fog; vec4 fog_rng; vec4 ot; vec4 ot2; vec4 slot; vec4 shade; vec4 rim; vec4 glow; } pc;
layout(location = 0) in vec3 uv;
layout(location = 1) in float cam_z;
layout(location = 2) in float local_y;
layout(location = 3) flat in float face_slot;
layout(location = 4) flat in float face_wet;
layout(location = 5) in vec4 v_light;
layout(location = 6) in float world_y;
layout(location = 7) flat in float face_all;
layout(location = 8) flat in vec2 slab_v;
layout(location = 9) in vec3 v_extra;
layout(location = 10) flat in vec3 v_amb;
layout(location = 11) in vec3 v_corner;
layout(location = 12) flat in vec3 v_key;
layout(location = 13) in vec3 v_normal;
layout(location = 14) in vec3 v_eye;
layout(location = 0) out vec4 out_color;
// View depth (render/post_fx.h): the camera z in 1/256 units as 24 bits in rgb, and as the depth (z / 65536).
vec4 view_depth_color(float z) {
  float v = clamp(floor(z * 256.0 + 0.5), 0.0, 16777215.0);
  float hi = floor(v / 65536.0);
  float mid = floor((v - hi * 65536.0) / 256.0);
  return vec4(hi, mid, v - hi * 65536.0 - mid * 256.0, 255.0) / 255.0;
}
void main() {
  bool ot = pc.ot.x > 0.5;
  bool wet = ot ? face_wet > 0.5 : (pc.misc.z > 0.5 && local_y > pc.misc.w);
  if (ot && pc.ot.x > 1.5) {  // smooth water line: the exact plane inside a straddling face
    wet = face_all > 0.5 || (face_wet > 0.5 && world_y > pc.misc.w);
    float fs = face_slot + pc.ot.z + (wet ? pc.ot.w : 0.0);
    if (fs <= slab_v.x || fs > slab_v.y) discard;
  }
  if (ot && pc.ot2.x > 0.5 && (pc.ot2.x > 1.5) != wet) discard;
  vec4 c = pc.misc.x > 0.5 ? vec4(pc.color.rgb, 1.0) : texture(tex, uv.xy / uv.z);
  if (c.a < 0.5) discard;
  // Texel alpha: 1 = STP clear, 0.75 (import kStpAlpha 0xBF) = STP set; the midpoint separates them under bilinear filtering too.
  bool stp = pc.misc.x > 0.5 || c.a < 0.875;
  if (pc.fog_rng.z > 0.5 && (pc.fog_rng.z > 1.5) != stp) discard;
  if (pc.slot.w > 0.5) {
    out_color = view_depth_color(cam_z);
    gl_FragDepth = clamp(cam_z / 65536.0, 0.0, 1.0);
    return;
  }
  if (pc.fog_rng.w > 0.5) c.rgb = vec3(1.0);  // FE 45 white CLUT: every non-transparent texel 0x7FFF, STP kept
  // Submerged faces take the water colour; a textured face's texel is modulated by its corner's packet colour (flat / solid faces: neutral).
  vec3 light = (wet ? pc.water.rgb * pc.tint.a : pc.tint.rgb) * (pc.misc.x > 0.5 ? vec3(1.0) : v_corner);
  if (v_light.w > 0.5) light = v_light.rgb;  // PS1 lit path: the GTE NCCT corner colour, no depth cue
  else if (pc.fog_rng.y > pc.fog_rng.x) {  // libgte SetFogNearFar(a, b): p = b (z - a) / ((b - a) z), DPCS: rgb += p (far - rgb)
    float p = clamp(pc.fog_rng.y * (cam_z - pc.fog_rng.x) / ((pc.fog_rng.y - pc.fog_rng.x) * max(cam_z, 1.0)), 0.0, 1.0);
    light = mix(light, pc.fog.rgb * pc.tint.a, p);
  }
  if (v_extra != vec3(0.0)) light = max(light + v_extra, light * 0.65);  // [Enhancements] Lighting (never < 65 % of the PS1 colour)
  light *= v_amb;  // [Enhancements] Lighting: the scene ambient (1 = neutral)
  vec3 rgb = c.rgb * light;
  int style = int(pc.shade.x + 0.5);
  bool has_normal = dot(v_normal, v_normal) > 1e-6;
  vec3 n = has_normal ? normalize(v_normal) : vec3(0.0);
  float facing = 1.0;
  if (has_normal && (style > 0 || pc.rim.w > 0.0 || pc.glow.w > 0.0)) {
    // A drawn face looks at the viewer: a normal pointing away (a mesh wound the other way, or a silhouette) is turned around first.
    if (style == 4) {
      facing = abs(normalize(v_eye).z);
    } else {
      vec3 view_dir = normalize(v_eye);
      if (dot(n, view_dir) < 0.0) n = -n;
      facing = dot(n, view_dir);
    }
  }
  if (style > 0 && has_normal) {
    vec3 unstyled = rgb;
    float d = dot(n, v_key);
    float t = d * 0.5 + 0.5;
    if (style == 1) {
      rgb *= 0.6 + 0.5 * t;
      vec3 h = normalize(v_key + normalize(v_eye));
      rgb += light * (0.22 * pow(max(dot(n, h), 0.0), 16.0));
    } else if (style == 2) {
      rgb *= mix(0.55, 1.0, smoothstep(-0.05, 0.2, d));
    } else if (style == 3) {
      float band = (floor(t * 4.0) + smoothstep(0.42, 0.58, fract(t * 4.0))) / 4.0;
      rgb *= 0.5 + 0.6 * band;
    } else {
      vec3 mn = normalize(v_eye);
      if (mn.z < 0.0) mn = -mn;
      rgb *= 1.6 * texture(matcap, vec2(mn.x * 0.5 + 0.5, 0.5 - mn.y * 0.5)).rgb;
    }
    rgb = mix(unstyled, rgb, pc.slot.z);
  }
  if (pc.rim.w > 0.0 && has_normal) rgb += light * pc.rim.rgb * (0.4 * pc.rim.w * pow(1.0 - facing, 3.0));
  if (pc.glow.w > 0.0) rgb = mix(rgb, 1.5 * rgb * pc.glow.rgb, 0.35 * pc.glow.w) + pc.glow.rgb * (pc.glow.w * (0.12 + 0.5 * (1.0 - facing) * (1.0 - facing)));
  if (pc.shade.y > 0.5) rgb *= 0.15;
  out_color = vec4(clamp(rgb, 0.0, 1.0), pc.ot2.y > 0.5 ? 0.0 : pc.ot2.w);
  // The outline pass writes its depth one slot (16 units of camera z) behind its own, so its model covers it inside the silhouette while
  // anything drawn later behind the model stays behind the outline too.
  float outline_back = pc.shade.y > 0.5 ? 1.0 : 0.0;
  if (ot) {
    // One slot per face (+ layer bias, + the submerged push); the fraction orders fragments of faces sharing a slot without leaving it.
    float bias = pc.ot.z + (wet ? pc.ot.w : 0.0) + outline_back;
    if (pc.slot.y > 0.5) {
      const float kNearPull = 1.0;
      float z = max(cam_z, 0.0) / 16.0;
      if (pc.ot.y > 0.5) z = max(z - kNearPull, face_slot);
      gl_FragDepth = clamp((z + bias + 0.001) * pc.ot2.z, 0.0, 1.0);
    } else {
      float fine = clamp(cam_z / 16.0 - face_slot, 0.0, 1.0);
      gl_FragDepth = clamp((face_slot + bias + 0.001 + 0.998 * fine) * pc.ot2.z, 0.0, 1.0);
    }
  } else {
    gl_FragDepth = clamp((cam_z + 16.0 * outline_back) * pc.misc.y, 0.0, 1.0);
  }
}
