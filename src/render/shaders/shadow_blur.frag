#version 450
// [Enhancements] Lighting: softens one shadow tile (the scissor picks it). Three rings of a disc, the outer ones weighing less, the middle ring turned half a step.
layout(set = 2, binding = 0) uniform sampler2D mask;
layout(set = 3, binding = 0) uniform Push { vec4 blur; } pc;  // x = the disc's radius in uv
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;
const int kTaps = 8;
const vec2 kRing[kTaps] = vec2[kTaps](vec2(1.0, 0.0), vec2(0.707, 0.707), vec2(0.0, 1.0), vec2(-0.707, 0.707), vec2(-1.0, 0.0), vec2(-0.707, -0.707), vec2(0.0, -1.0), vec2(0.707, -0.707));
const vec2 kTurned[kTaps] = vec2[kTaps](vec2(0.924, 0.383), vec2(0.383, 0.924), vec2(-0.383, 0.924), vec2(-0.924, 0.383), vec2(-0.924, -0.383), vec2(-0.383, -0.924), vec2(0.383, -0.924), vec2(0.924, -0.383));
void main() {
  vec4 sum = texture(mask, uv) * 2.0;
  float weight = 2.0;
  for (int i = 0; i < kTaps; ++i) {
    sum += texture(mask, uv + kRing[i] * pc.blur.x / 3.0);
    sum += texture(mask, uv + kTurned[i] * pc.blur.x * 2.0 / 3.0) * 0.7;
    sum += texture(mask, uv + kRing[i] * pc.blur.x) * 0.35;
    weight += 2.05;
  }
  out_color = vec4((sum / weight).rgb, 1.0);
}
