#version 450
#include "color_helpers.glsl"

layout(binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC {
  vec2 halfpixel;
  float offset;
  float noise_strength;
  float vibrancy;
  float vibrancy_darkness;
  float brightness;
  float contrast;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

void main() {
  vec2 uv = v_uv;
  vec4 s = texture(tex, uv) * 4.0;
  s += texture(tex, uv - pc.halfpixel * pc.offset);
  s += texture(tex, uv + pc.halfpixel * pc.offset);
  s += texture(tex, uv + vec2(pc.halfpixel.x, -pc.halfpixel.y) * pc.offset);
  s += texture(tex, uv + vec2(-pc.halfpixel.x, pc.halfpixel.y) * pc.offset);
  vec4 color = s / 8.0;

  if (pc.noise_strength > 0.0)
    color.rgb = addNoise(color.rgb, v_uv, pc.noise_strength * 0.5);
  if (pc.vibrancy > 0.0)
    color.rgb = applyVibrancy(color.rgb, pc.vibrancy, pc.vibrancy_darkness, 1.0);
  color.rgb = applyBrightnessContrast(color.rgb, pc.brightness, pc.contrast);
  fragColor = color;
}
