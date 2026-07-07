precision highp float;
uniform vec2 resolution;
uniform float border_radius;
uniform float border_width_px;
uniform float scale;

uniform vec4 gradient_colors[10];
uniform int gradient_count;
uniform float gradient_angle;
uniform vec4 gradient2_colors[10];
uniform int gradient2_count;
uniform float gradient2_angle;
uniform float gradient_lerp;

uniform vec4 border_color;
varying vec2 v_uv;

float rounding_alpha(vec2 coords, vec2 size, float radius) {
  if (size.x <= 0.0 || size.y <= 0.0) return 0.0;
  if (coords.x < 0.0 || coords.x > size.x || coords.y < 0.0 || coords.y > size.y) return 0.0;

  float r = min(radius, min(size.x, size.y) * 0.5);
  if (r <= 0.0) return 1.0;

  vec2 center;
  bool in_corner = false;

  if (coords.x < r && coords.y < r) {
    center = vec2(r, r);
    in_corner = true;
  } else if (size.x - r < coords.x && coords.y < r) {
    center = vec2(size.x - r, r);
    in_corner = true;
  } else if (size.x - r < coords.x && size.y - r < coords.y) {
    center = vec2(size.x - r, size.y - r);
    in_corner = true;
  } else if (coords.x < r && size.y - r < coords.y) {
    center = vec2(r, size.y - r);
    in_corner = true;
  }

  if (!in_corner) return 1.0;

  float dist = distance(coords, center);
  float t = clamp((dist - r) * scale + 0.5, 0.0, 1.0);
  return 1.0 - t * t * (3.0 - 2.0 * t);
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

  float a_outer = rounding_alpha(px, resolution, border_radius);

  float inner_r = max(border_radius - border_width_px, 0.0);
  vec2 inner_offset = vec2(border_width_px);
  vec2 inner_size = resolution - inner_offset * 2.0;
  float a_inner = rounding_alpha(px - inner_offset, inner_size, inner_r);

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

  float alpha = a_outer * (1.0 - a_inner) * col.a;
  if (alpha < 0.001) discard;
  gl_FragColor = vec4(col.rgb * alpha, alpha);
}
