#version 450
layout(binding = 0) uniform sampler2D _dummy;
layout(binding = 1) uniform GradientData {
  vec4 gradient_colors[10];
  vec4 gradient2_colors[10];
  int gradient_count;
  int gradient2_count;
  float gradient_angle;
  float gradient2_angle;
  float gradient_lerp;
} gd;
layout(push_constant) uniform PC {
  vec2 resolution;
  float border_radius;
  float border_width_px;
  float scale;
  vec4 border_color;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

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
  float t = clamp((dist - radius) * pc.scale + 0.5, 0.0, 1.0);
  return 1.0 - t * t * (3.0 - 2.0 * t);
}

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
  if (lo < 0 || lo > 8) lo = 9;
  vec4 c0 = colors[lo];
  if (hi >= count) return c0;
  if (hi < 0 || hi > 8) hi = 9;
  vec4 c1 = colors[hi];
  return mix(c0, c1, t);
}

void main() {
  vec2 px = v_uv * pc.resolution;
  float a_outer = rounding_alpha(px, pc.resolution, pc.border_radius);
  float inner_r = max(pc.border_radius - pc.border_width_px, 0.0);
  vec2 inner_offset = vec2(pc.border_width_px);
  vec2 inner_size = pc.resolution - inner_offset * 2.0;
  float a_inner = rounding_alpha(px - inner_offset, inner_size, inner_r);
  vec4 col;
  if (gd.gradient_count >= 2) {
    vec4 g1 = sampleGradient(v_uv, gd.gradient_colors, gd.gradient_count, gd.gradient_angle);
    if (gd.gradient2_count >= 2) {
      vec4 g2 = sampleGradient(v_uv, gd.gradient2_colors, gd.gradient2_count, gd.gradient2_angle);
      col = mix(g1, g2, gd.gradient_lerp);
    } else {
      col = g1;
    }
  } else {
    col = pc.border_color;
  }
  float alpha = a_outer * (1.0 - a_inner) * col.a;
  if (alpha < 0.001) discard;
  fragColor = vec4(col.rgb * alpha, alpha);
}
