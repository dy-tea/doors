#version 450
layout(binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC {
  vec4 tint;
  float tint_strength;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

void main() {
  vec3 base = texture(tex, v_uv).rgb;
  fragColor = vec4(mix(base, pc.tint.rgb, pc.tint_strength), 1.0);
}
