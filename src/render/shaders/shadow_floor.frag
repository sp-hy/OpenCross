#version 450
// [Enhancements] Lighting: multiplies the finished floor by what the shadows leave of its light. The floor's colour multiplier is
// (1 + knee(sum of the spell lights)) (stage.frag); this keeps that form with the spell lights cut by their mask channel and the base light cut by the
// shadow's strength. How much of a unit's shadow is the sun's and how much the spell's was settled in the mask (render::spell_shadow).
layout(set = 2, binding = 0) uniform sampler2D mask;
// rect: x0, z0, x1, z1; params: x shadow strength, y depth bias (slots), z 0.5 / mask reach, w texel size in uv; lpos: position, radius (negative = linear falloff); lcol: colour.
layout(set = 3, binding = 0) uniform Push { mat4 clip; vec4 rect; vec4 params; vec4 lpos[3]; vec4 lcol[3]; } pc;
layout(location = 0) in vec2 ground;
layout(location = 1) in float cam_z;
layout(location = 0) out vec4 out_color;
const float kContactRadius = 1.25, kFarRadius = 3.5;  // mask texels
const int kTaps = 8;
const vec2 kRing[kTaps] = vec2[kTaps](vec2(1.0, 0.0), vec2(0.707, 0.707), vec2(0.0, 1.0), vec2(-0.707, 0.707), vec2(-1.0, 0.0), vec2(-0.707, -0.707), vec2(0.0, -1.0), vec2(0.707, -0.707));
vec3 knee(vec3 sum) { return sum / (1.0 + max(sum.r, max(sum.g, sum.b)) / 0.8); }
void main() {
  vec2 uv = vec2(0.5 + ground.x * pc.params.z, 0.5 - ground.y * pc.params.z);
  vec4 centre = texture(mask, uv), near_ring = vec4(0.0), far_ring = vec4(0.0);
  for (int i = 0; i < kTaps; ++i) {
    near_ring += texture(mask, uv + kRing[i] * kContactRadius * pc.params.w);
    far_ring += texture(mask, uv + kRing[i] * kFarRadius * pc.params.w);
  }
  vec4 contact = (centre * 2.0 + near_ring) / (2.0 + float(kTaps));
  vec4 wide = (centre + near_ring + far_ring) / (1.0 + 2.0 * float(kTaps));
  // A crisp edge at the feet, a little softer and fainter with the height of what casts it.
  vec4 shadow = mix(wide, contact, smoothstep(0.45, 0.9, contact));
  vec3 lit = vec3(0.0), unblocked = vec3(0.0);
  for (int i = 0; i < 3; ++i) {
    float dist = length(pc.lpos[i].xyz - vec3(ground.x, 0.0, ground.y));
    float rad = abs(pc.lpos[i].w);
    float x2 = dist * dist / max(rad * rad, 1.0);
    float att = pc.lpos[i].w < 0.0 ? clamp(1.0 - sqrt(x2), 0.0, 1.0) : clamp(1.0 - x2, 0.0, 1.0);
    vec3 light = pc.lcol[i].rgb * att * 0.9;
    lit += light;
    unblocked += light * (1.0 - shadow.g);
  }
  float base = 1.0 - pc.params.x * max(shadow.r, shadow.g);
  out_color = vec4((vec3(base) + knee(unblocked)) / (vec3(1.0) + knee(lit)), 1.0);
  gl_FragDepth = clamp((cam_z / 16.0 + pc.params.y) / 8192.0, 0.0, 1.0);
}
