#version 450
// [Enhancements] Lighting: a full-screen triangle over the soft tile target (shadow_blur.frag).
layout(location = 0) out vec2 uv;
void main() {
  vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  uv = corner;
  gl_Position = vec4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.5, 1.0);
}
