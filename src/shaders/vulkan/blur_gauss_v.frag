#version 450
#include "color_helpers.glsl"

layout(binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC {
  vec2 texel_size;
  float radius;
  float _pad0;
  float vibrancy;
  float vibrancy_darkness;
  float brightness;
  float contrast;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

float gauss(float x, float s) {
  return exp(-(x * x) / (2.0 * s * s));
}

void main() {
  float sigma = max(pc.radius / 3.0, 1.0);
  vec4 color = vec4(0.0);
  float total = 0.0;
  for (float i = -pc.radius; i <= pc.radius; i += 1.0) {
    float w = gauss(i, sigma);
    color += texture(tex, v_uv + vec2(0.0, i * pc.texel_size.y)) * w;
    total += w;
  }
  color = color / total;

  if (pc.vibrancy > 0.0)
    color.rgb = applyVibrancy(color.rgb, pc.vibrancy, pc.vibrancy_darkness, 2.0);
  color.rgb = applyBrightnessContrast(color.rgb, pc.brightness, pc.contrast);
  fragColor = color;
}
