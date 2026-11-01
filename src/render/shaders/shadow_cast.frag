#version 450
layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 3, binding = 0) uniform Push { vec4 light; vec4 channel; vec4 misc; vec4 plane; vec4 tile; vec4 tone; } pc;
layout(location = 0) in vec2 uv;
layout(location = 1) in float height;
layout(location = 0) out vec4 out_color;
void main() {
  if (pc.misc.y < 0.5 && texture(tex, uv).a < 0.5) discard;
  // The mask keeps the strongest value per texel, so overlapping faces and units never darken twice; the value says how near the floor the part is.
  out_color = vec4(pc.tone.x) + pc.tone.y * pc.channel * pc.misc.z * mix(1.0, pc.tone.z, clamp(height / pc.tile.w, 0.0, 1.0));
}
