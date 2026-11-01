#version 450
// [Enhancements] Lighting: one quad on the floor (y = 0), filed like the stage's other floor decals. See shadow_floor.frag.
layout(set = 1, binding = 0) uniform Push { mat4 clip; vec4 rect; vec4 params; vec4 lpos[3]; vec4 lcol[3]; } pc;
layout(location = 0) out vec2 ground;
layout(location = 1) out float cam_z;
void main() {
  const vec2 corners[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));
  ground = mix(pc.rect.xy, pc.rect.zw, corners[gl_VertexIndex]);
  gl_Position = pc.clip * vec4(ground.x, 0.0, ground.y, 1.0);
  cam_z = gl_Position.w;
  // SDL_GPU clip space has +y up on every backend; the transform works in the PS1's y-down frame.
  gl_Position.y = -gl_Position.y;
}
