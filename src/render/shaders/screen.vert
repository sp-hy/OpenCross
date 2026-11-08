#version 450
layout(location = 0) out vec2 uv;
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));  // one oversized triangle
  uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
  // SDL_GPU clip space has +y up on every backend; everything above works in the PS1's y-down frame.
  gl_Position.y = -gl_Position.y;
}
