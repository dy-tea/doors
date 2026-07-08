#version 450
layout(binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC {
  vec2 halfpixel;
  float offset;
  float _pad0;
  vec2 refraction_rect_size;
  float refraction_edge_size_pixels;
  float refraction_corner_radius_pixels;
  float refraction_strength;
  float refraction_normal_pow;
  float refraction_RGB_fringing;
  int refraction_texture_repeat_mode;
  int refraction_mode;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

vec2 applyTextureRepeatMode(vec2 coord) {
  if (pc.refraction_texture_repeat_mode == 0) {
    return clamp(coord, 0.0, 1.0);
  } else if (pc.refraction_texture_repeat_mode == 1) {
    vec2 flip = mod(coord, 2.0);
    vec2 result;
    if (flip.x > 1.0)
      result.x = 1.0 - mod(coord.x, 1.0);
    else
      result.x = mod(coord.x, 1.0);

    if (flip.y > 1.0)
      result.y = 1.0 - mod(coord.y, 1.0);
    else
      result.y = mod(coord.y, 1.0);

    return result;
  }
  return coord;
}

float roundedRectangleDist(vec2 p, vec2 b, float r) {
  vec2 q = abs(p) - b + r;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main(void) {
  vec2 halfRefractionRectSize = 0.5 * pc.refraction_rect_size;
  vec2 position = v_uv * pc.refraction_rect_size - halfRefractionRectSize.xy;
  float cornerR = min(pc.refraction_corner_radius_pixels, min(halfRefractionRectSize.x, halfRefractionRectSize.y));

  vec4 sum = vec4(0.0);
  vec2 coordR, coordG, coordB;

  if (pc.refraction_mode == 1) {
    float distConcave = roundedRectangleDist(position, halfRefractionRectSize, cornerR);
    float fringing = pc.refraction_RGB_fringing * 0.3;
    float baseStrength = 0.2 * pc.refraction_strength;
    float edgeProximity = clamp(1.0 + distConcave / pc.refraction_edge_size_pixels, 0.0, 1.0);
    float shaped = sin(pow(edgeProximity, pc.refraction_normal_pow) * 1.57079632679);
    vec2 fromCenter = v_uv - vec2(0.5);
    float scaleR = 1.0 - shaped * baseStrength * (1.0 + fringing);
    float scaleG = 1.0 - shaped * baseStrength;
    float scaleB = 1.0 - shaped * baseStrength * (1.0 - fringing);
    coordR = applyTextureRepeatMode(vec2(0.5) + fromCenter * scaleR);
    coordG = applyTextureRepeatMode(vec2(0.5) + fromCenter * scaleG);
    coordB = applyTextureRepeatMode(vec2(0.5) + fromCenter * scaleB);
  } else {
    float distBulge = roundedRectangleDist(position, halfRefractionRectSize, pc.refraction_edge_size_pixels);
    float concaveFactor = pow(clamp(1.0 + distBulge / pc.refraction_edge_size_pixels, 0.0, 1.0), pc.refraction_normal_pow);
    const float h = 1.0;
    vec2 gradient = vec2(
        roundedRectangleDist(position + vec2(h, 0.0), halfRefractionRectSize, pc.refraction_edge_size_pixels) - roundedRectangleDist(position - vec2(h, 0.0), halfRefractionRectSize, pc.refraction_edge_size_pixels),
        roundedRectangleDist(position + vec2(0.0, h), halfRefractionRectSize, pc.refraction_edge_size_pixels) - roundedRectangleDist(position - vec2(0.0, h), halfRefractionRectSize, pc.refraction_edge_size_pixels)
      );
    vec2 normal = length(gradient) > 1e-6 ? -normalize(gradient) : vec2(0.0, 1.0);
    float finalStrength = 0.2 * concaveFactor * pc.refraction_strength;
    float fringingFactor = pc.refraction_RGB_fringing * 0.3;
    vec2 refractOffsetR = normal.xy * (finalStrength * (1.0 + fringingFactor));
    vec2 refractOffsetG = normal.xy * finalStrength;
    vec2 refractOffsetB = normal.xy * (finalStrength * (1.0 - fringingFactor));
    coordR = applyTextureRepeatMode(v_uv - refractOffsetR);
    coordG = applyTextureRepeatMode(v_uv - refractOffsetG);
    coordB = applyTextureRepeatMode(v_uv - refractOffsetB);
  }

  vec2 off;
  off = vec2(-pc.halfpixel.x * 2.0, 0.0) * pc.offset;
  sum.r += texture(tex, coordR + off).r;
  sum.g += texture(tex, coordG + off).g;
  sum.b += texture(tex, coordB + off).b;
  sum.a += texture(tex, coordG + off).a;

  off = vec2(-pc.halfpixel.x, pc.halfpixel.y) * pc.offset;
  sum.r += texture(tex, coordR + off).r * 2.0;
  sum.g += texture(tex, coordG + off).g * 2.0;
  sum.b += texture(tex, coordB + off).b * 2.0;
  sum.a += texture(tex, coordG + off).a * 2.0;

  off = vec2(0.0, pc.halfpixel.y * 2.0) * pc.offset;
  sum.r += texture(tex, coordR + off).r;
  sum.g += texture(tex, coordG + off).g;
  sum.b += texture(tex, coordB + off).b;
  sum.a += texture(tex, coordG + off).a;

  off = vec2(pc.halfpixel.x, pc.halfpixel.y) * pc.offset;
  sum.r += texture(tex, coordR + off).r * 2.0;
  sum.g += texture(tex, coordG + off).g * 2.0;
  sum.b += texture(tex, coordB + off).b * 2.0;
  sum.a += texture(tex, coordG + off).a * 2.0;

  off = vec2(pc.halfpixel.x * 2.0, 0.0) * pc.offset;
  sum.r += texture(tex, coordR + off).r;
  sum.g += texture(tex, coordG + off).g;
  sum.b += texture(tex, coordB + off).b;
  sum.a += texture(tex, coordG + off).a;

  off = vec2(pc.halfpixel.x, -pc.halfpixel.y) * pc.offset;
  sum.r += texture(tex, coordR + off).r * 2.0;
  sum.g += texture(tex, coordG + off).g * 2.0;
  sum.b += texture(tex, coordB + off).b * 2.0;
  sum.a += texture(tex, coordG + off).a * 2.0;

  off = vec2(0.0, -pc.halfpixel.y * 2.0) * pc.offset;
  sum.r += texture(tex, coordR + off).r;
  sum.g += texture(tex, coordG + off).g;
  sum.b += texture(tex, coordB + off).b;
  sum.a += texture(tex, coordG + off).a;

  off = vec2(-pc.halfpixel.x, -pc.halfpixel.y) * pc.offset;
  sum.r += texture(tex, coordR + off).r * 2.0;
  sum.g += texture(tex, coordG + off).g * 2.0;
  sum.b += texture(tex, coordB + off).b * 2.0;
  sum.a += texture(tex, coordG + off).a * 2.0;

  vec4 blurred = sum / 12.0;
  fragColor = blurred;
}
