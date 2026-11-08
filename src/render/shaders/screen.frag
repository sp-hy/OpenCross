#version 450
// [Shaders] full-screen pass (render/screen_pass.h): the pause ramp, or the main filter, then the grade.
layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 2, binding = 1) uniform sampler2D lut;
// a: frame size, ramp on, filter (0 off, 1 crt, 2 composite); tint: ramp colour; b: CRT mask (0 classic, 1 aperture grille,
// 2 shadow mask), curvature on, glow on, grade (0 off, 1 film, 2 warm, 3 cold, 4 faded, 5 LUT); c: tick, frame pixels per PS1 pixel, LUT
// size N, LUT on.
layout(set = 3, binding = 0) uniform Push { vec4 a; vec4 tint; vec4 b; vec4 c; } pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// CRT: scanlines at the PS1's 240 lines (never finer than 2 output pixels), the phosphor mask, a vignette, gain for the darkening.
// Classic = a faint RGB triad when the window is wide enough to show it; aperture grille = stronger vertical RGB stripes; shadow mask =
// RGB triads shifted every other pair of rows. Glow: the neighbourhood (PS1-pixel radius) bleeds into the scanline gaps.
vec3 crt(vec2 p) {
  vec3 c = texture(tex, p).rgb;
  if (pc.b.z > 0.5) {
    vec2 r = pc.c.y / pc.a.xy;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
      float angle = float(i) * 0.7853982;
      sum += texture(tex, p + vec2(cos(angle), sin(angle)) * r * 1.5).rgb;
    }
    c += max(sum / 8.0 - 0.25, 0.0) * 0.35;
  }
  float lines = min(240.0, pc.a.y * 0.5);
  float scan = 0.5 + 0.5 * cos(p.y * lines * 6.2831853);
  c *= mix(1.0, 0.72 + 0.28 * scan, 0.85);
  int mask = int(pc.b.x + 0.5);
  if (mask == 0) {
    if (pc.a.x >= 1200.0) {
      int col = int(gl_FragCoord.x) % 3;
      vec3 m = vec3(col == 0 ? 1.0 : 0.88, col == 1 ? 1.0 : 0.88, col == 2 ? 1.0 : 0.88);
      c *= m;
    }
  } else {
    int shift = mask == 2 ? ((int(gl_FragCoord.y) / 2) % 2) * 2 : 0;
    int col = (int(gl_FragCoord.x) + shift) % 3;
    float low = mask == 2 ? 0.62 : 0.7;
    c *= vec3(col == 0 ? 1.0 : low, col == 1 ? 1.0 : low, col == 2 ? 1.0 : low) * (mask == 2 ? 1.16 : 1.1);
  }
  vec2 q = p * (1.0 - p);
  c *= mix(0.78, 1.0, clamp(pow(16.0 * q.x * q.y, 0.25), 0.0, 1.0));
  return c * 1.12;
}

// Composite video: YIQ; the luma lightly blurred over half a PS1 pixel, the chroma over about three; a dot crawl (a checkerboard of PS1
// pixels on colour edges that flips every tick) and a faint ghost four PS1 pixels to the left.
vec3 composite(vec2 p) {
  const mat3 to_yiq = mat3(0.299, 0.596, 0.211, 0.587, -0.274, -0.523, 0.114, -0.322, 0.312);
  const mat3 to_rgb = mat3(1.0, 1.0, 1.0, 0.956, -0.272, -1.106, 0.621, -0.647, 1.703);
  float dx = pc.c.y / pc.a.x;
  vec3 centre = to_yiq * texture(tex, p).rgb;
  float y = centre.x * 0.5 + 0.25 * ((to_yiq * texture(tex, p - vec2(dx * 0.5, 0.0)).rgb).x + (to_yiq * texture(tex, p + vec2(dx * 0.5, 0.0)).rgb).x);
  vec2 iq = vec2(0.0);
  float total = 0.0;
  for (int k = -3; k <= 3; ++k) {
    float w = exp(-0.5 * float(k * k) / 2.25);
    iq += (to_yiq * texture(tex, p + vec2(dx * float(k), 0.0)).rgb).yz * w;
    total += w;
  }
  iq /= total;
  vec2 cell = floor(gl_FragCoord.xy / max(pc.c.y, 1.0));
  float crawl = (mod(cell.x + cell.y + pc.c.x, 2.0) - 0.5) * 2.0;
  y += crawl * length(iq - centre.yz) * 0.25;
  y += 0.06 * (to_yiq * texture(tex, p - vec2(dx * 4.0, 0.0)).rgb).x;
  return clamp(to_rgb * vec3(y / 1.06, iq * 0.9), 0.0, 1.0);
}

vec3 saturate_by(vec3 c, float amount) { return mix(vec3(luma(c)), c, amount); }

// Built-in grades: film = a gentle S-curve, warm highlights and cool shadows, 0.9 saturation; warm / cold = a white-balance shift; faded =
// lifted blacks, lower contrast and 0.75 saturation. LUT: N x N squares, blue picks the square (blended between two), red / green the texel.
vec3 grade(vec3 c, int which) {
  c = clamp(c, 0.0, 1.0);
  if (which == 1) {
    c = mix(c, c * c * (3.0 - 2.0 * c), 0.35);
    float l = luma(c);
    c += mix(vec3(-0.01, 0.01, 0.025), vec3(0.03, 0.01, -0.02), l);
    return saturate_by(c, 0.9);
  }
  if (which == 2) return saturate_by(c * vec3(1.06, 1.0, 0.9), 1.05);
  if (which == 3) return saturate_by(c * vec3(0.92, 1.0, 1.08), 0.95);
  if (which == 4) return saturate_by(mix(vec3(0.08, 0.07, 0.1), c, 0.86), 0.75);
  float n = pc.c.z;
  float blue = c.b * (n - 1.0);
  float square = floor(blue);
  vec2 inner = vec2(c.r * (n - 1.0) + 0.5, c.g * (n - 1.0) + 0.5);
  vec3 low = texture(lut, vec2((square * n + inner.x) / (n * n), inner.y / n)).rgb;
  vec3 high = texture(lut, vec2((min(square + 1.0, n - 1.0) * n + inner.x) / (n * n), inner.y / n)).rgb;
  return mix(low, high, blue - square);
}

void main() {
  if (pc.a.z > 0.5) {  // pause screen filter: a monochrome purple ramp of the luminance
    vec3 c = texture(tex, uv).rgb;
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    out_color = vec4(l * pc.tint.rgb, 1.0);
    return;
  }
  int filter_mode = int(pc.a.w + 0.5);
  vec2 p = uv;
  if (filter_mode == 1 && pc.b.y > 0.5) {  // CRT curvature: a barrel warp, black outside the tube
    vec2 centred = uv * 2.0 - 1.0;
    centred *= 1.0 + vec2(0.06, 0.08) * centred.yx * centred.yx;
    p = centred * 0.5 + 0.5;
    if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) {
      out_color = vec4(0.0, 0.0, 0.0, 1.0);
      return;
    }
  }
  vec3 c = filter_mode == 1 ? crt(p) : filter_mode == 2 ? composite(p) : texture(tex, p).rgb;
  int grade_mode = int(pc.b.w + 0.5);
  if (grade_mode > 0 && (grade_mode < 5 || pc.c.w > 0.5)) c = grade(c, grade_mode);
  out_color = vec4(c, 1.0);
}
