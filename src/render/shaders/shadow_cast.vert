#version 450
// [Enhancements] Lighting: a skinned model flattened onto a floor plane along one light, drawn into a channel of a floor-space shadow mask.
layout(location = 0) in vec3 in_pos0;
layout(location = 1) in vec3 in_pos1;
layout(location = 2) in uvec4 in_joint;   // this corner's two joints in xy
layout(location = 3) in vec4 in_weight;   // this corner's two weights in xy
layout(location = 4) in vec4 in_uvq;      // uv
layout(location = 5) in vec4 in_normal;   // this corner's normal, w = the joint that rotates it; zero on rigid meshes
struct Slot {
  mat4 model;
  mat4 palette[128];
};
layout(std430, set = 0, binding = 0) readonly buffer Slots { Slot slots[]; };
// light: xyz = the direction toward the light, w = the growth along the normals; channel: the mask channel; misc: x slot, y 1 = no texture, z caster strength,
// w 1 / half span of the floor area; plane: xy = the area's centre (world x, z), z = the floor's height, w = +1 y up, -1 y down; tile: xy = clip centre, z = clip half span,
// w = the height where a part's shadow is at its faintest; tone: the value stored is x + y * shadow, z = the faintest part of a shadow, 0..1.
layout(set = 1, binding = 0) uniform Push { vec4 light; vec4 channel; vec4 misc; vec4 plane; vec4 tile; vec4 tone; } pc;
layout(location = 0) out vec2 uv;
layout(location = 1) out float height;
void main() {
  int slot = int(pc.misc.x + 0.5);
  vec4 skinned = slots[slot].palette[in_joint.x] * vec4(in_pos0, 1.0) * in_weight.x + slots[slot].palette[in_joint.y] * vec4(in_pos1, 1.0) * in_weight.y;
  vec3 world = (slots[slot].model * vec4(skinned.xyz, 1.0)).xyz;
  if (dot(in_normal.xyz, in_normal.xyz) > 1e-6) world += normalize(mat3(slots[slot].model) * mat3(slots[slot].palette[uint(in_normal.w + 0.5)]) * in_normal.xyz) * pc.light.w;  // limbs thin as a few mask texels would otherwise read as threads
  height = max((world.y - pc.plane.z) * pc.plane.w, 0.0);
  vec2 ground = world.xz - pc.light.xz / pc.light.y * height;
  uv = in_uvq.xy;
  gl_Position = vec4(pc.tile.xy + (ground - pc.plane.xy) * pc.misc.w * pc.tile.z, 0.5, 1.0);
}
