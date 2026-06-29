#extension GL_OES_standard_derivatives : enable
precision highp float;
uniform vec2 resolution;
uniform float shadow_size;
uniform vec4 shadow_color;
uniform float border_radius;
uniform vec2 inner_size;
uniform vec2 hole_pos;
uniform vec2 hole_size;

varying vec2 v_uv;

float sdRoundedBox(vec2 p, vec2 b, float r) {
  vec2 q = abs(p) - b + r;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
  vec2 px = v_uv * resolution;
  vec2 half_inner = inner_size * 0.5;
  vec2 center = resolution * 0.5;
  float d_shadow = sdRoundedBox(px - center, half_inner, border_radius);
  vec2 half_hole = hole_size * 0.5;
  vec2 hole_center = hole_pos + half_hole;
  float d_hole = sdRoundedBox(px - hole_center, half_hole, border_radius);
  float alpha = 1.0 - smoothstep(0.0, shadow_size, d_shadow);
  float fw = fwidth(d_hole);
  float hole = smoothstep(-fw * 0.5, fw * 0.5, d_hole);
  alpha *= hole * shadow_color.a;
  if (alpha < 0.003) discard;
  gl_FragColor = vec4(shadow_color.rgb * alpha, alpha);
}
