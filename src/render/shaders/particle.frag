#version 450
layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 3, binding = 0) uniform Push { vec4 mode; vec4 texels; } pc;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 out_color;
void main() {
  vec4 t;
  if (pc.texels.z > 0.0) {
    // ParticleBatch::set_texels: uv is a packet texel coordinate (texel k is centred on k); texels = the page origin and the texel size in the image.
    // px: image pixels per texel (above 1 for a replaced image); the sample point is the texel's centre, or with a larger image the fragment's place
    // inside the texel kept half a pixel away from its edges.
    vec2 px = max(pc.texels.zw * vec2(textureSize(tex, 0)), vec2(1.0));
    vec2 texel = floor(uv + 0.5);
    vec2 inside = clamp(uv + 0.5 - texel, 0.5 / px, 1.0 - 0.5 / px);
    t = textureLod(tex, pc.texels.xy + (texel - 0.5 + inside) * pc.texels.zw, 0.0);
  } else {
    t = texture(tex, uv);
  }
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
