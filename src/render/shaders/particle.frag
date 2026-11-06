#version 450
layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 3, binding = 0) uniform Push { vec4 mode; } pc;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 out_color;
void main() {
  vec4 t = texture(tex, uv);
  float a = t.a * color.a;
  if (a <= 0.0) discard;
  if (pc.mode.x != 0.0) {
    out_color = vec4(t.rgb * color.rgb * 2.0, a);
    return;
  }
  // texel * colour / 0x80 in 5-bit levels (blend 4 arrives with its colour quartered). The PS1 rounds that down, which costs half a
  // level on average; the same half level is taken off here without the steps, so faint glows are as dim as on the PS1.
  vec3 lit = t.rgb * color.rgb * 2.0 * 31.0;
  out_color = vec4(max(lit - 0.5, vec3(0.0)) / 31.0, a);
}
