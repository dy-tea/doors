#version 450
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
void main() {
  fragColor = vec4(texture(tex, v_uv).rgb, 1.0);
}
