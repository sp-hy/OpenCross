#version 450
// The frame's depth buffer as colour, for a controlled run's depth readback (GpuContext::request_depth_capture): the 0..1 depth value
// (ordering-table slot / kDepthRange) as a 24-bit integer in r, g, b (high byte first), a = 255.
layout(set = 2, binding = 0) uniform sampler2D depth_tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
  const ivec2 size = textureSize(depth_tex, 0);
  const ivec2 texel = clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1);
  const float depth = clamp(texelFetch(depth_tex, texel, 0).r, 0.0, 1.0);
  const uint value = uint(depth * 16777215.0 + 0.5);
  out_color = vec4(float((value >> 16) & 255u), float((value >> 8) & 255u), float(value & 255u), 255.0) / 255.0;
}
