#extension GL_OES_standard_derivatives : enable
precision highp float;
uniform vec2 resolution;
uniform float border_radius;
uniform float border_width_px;

uniform vec4 gradient_colors[10];
uniform int gradient_count;
uniform float gradient_angle;
uniform vec4 gradient2_colors[10];
uniform int gradient2_count;
uniform float gradient2_angle;
uniform float gradient_lerp;

uniform vec4 border_color;
varying vec2 v_uv;

float sdRoundedBox(vec2 p, vec2 b, float r) {
  vec2 q = abs(p) - b + r;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// sample a linear gradient defined by an array of color stops at a given angle
vec4 sampleGradient(vec2 uv, vec4 colors[10], int count, float angle) {
  if (count <= 0) return vec4(0.0);
  if (count == 1) return colors[0];

  float s = sin(angle);
  float c = cos(angle);
  float progress = uv.y * s + uv.x * (1.0 - s);
  progress = clamp(progress, 0.0, 1.0) * float(count - 1);
  int lo = int(floor(progress));
  int hi = lo + 1;
  float t = progress - float(lo);
  vec4 c0 = colors[0], c1 = colors[0];

  if (lo < 0 || lo > 8) lo = 9;
  c0 = colors[lo];

  if (hi >= count) return c0;

  if (hi < 0 || hi > 8) hi = 9;
  c1 = colors[hi];

  return mix(c0, c1, t);
}

void main() {
  vec2 px = v_uv * resolution;
  vec2 center = resolution * 0.5;
  vec2 p = px - center;
  float d_outer = sdRoundedBox(p, center, border_radius);
  float inner_r = max(border_radius - border_width_px, 0.0);
  vec2 inner_half = center - vec2(border_width_px);
  float d_inner = sdRoundedBox(p, inner_half, inner_r);
  float aa = fwidth(d_outer) * 0.5;
  float a_outer = 1.0 - smoothstep(-aa, aa, d_outer);
  aa = fwidth(d_inner) * 0.5;
  float a_inner = smoothstep(-aa, aa, d_inner);
  vec4 col;
  if (gradient_count >= 2) {
    vec4 g1 = sampleGradient(v_uv, gradient_colors, gradient_count, gradient_angle);
    if (gradient2_count >= 2) {
      vec4 g2 = sampleGradient(v_uv, gradient2_colors, gradient2_count, gradient2_angle);
      col = mix(g1, g2, gradient_lerp);
    } else {
      col = g1;
    }
  } else {
    col = border_color;
  }
  float alpha = a_outer * a_inner * col.a;
  if (alpha < 0.001) discard;
  gl_FragColor = vec4(col.rgb * alpha, alpha);
}
