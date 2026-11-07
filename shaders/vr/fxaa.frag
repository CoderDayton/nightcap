#version 450
// FXAA for one VR eye, after Timothy Lottes' FXAA (console variant). The eye
// holds sRGB-encoded bytes, so luma is computed on perceptual values, which
// is what FXAA expects.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Push {
  vec2 texel;          // 1 / source extent
  uint linearize;      // 1 when the target view re-encodes to sRGB
} push;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

const float kEdgeThreshold = 1.0 / 8.0;
const float kEdgeThresholdMin = 1.0 / 24.0;
const float kReduceMul = 1.0 / 8.0;
const float kReduceMin = 1.0 / 128.0;
const float kSpanMax = 8.0;

float Luma(vec3 rgb) { return dot(rgb, vec3(0.299, 0.587, 0.114)); }

vec3 Sample(vec2 at) { return textureLod(source, at, 0.0).rgb; }

// The target encodes linear values back to sRGB, which restores these bytes.
vec3 Linearize(vec3 c) {
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)),
             step(vec3(0.04045), c));
}

void main() {
  const vec4 center = textureLod(source, uv, 0.0);
  const float lumaM = Luma(center.rgb);
  const float lumaNW = Luma(Sample(uv + vec2(-1.0, -1.0) * push.texel));
  const float lumaNE = Luma(Sample(uv + vec2(1.0, -1.0) * push.texel));
  const float lumaSW = Luma(Sample(uv + vec2(-1.0, 1.0) * push.texel));
  const float lumaSE = Luma(Sample(uv + vec2(1.0, 1.0) * push.texel));
  const float lumaMin =
      min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  const float lumaMax =
      max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

  vec3 rgb = center.rgb;
  // Flat areas, where nothing aliases, keep their exact pixels.
  if (lumaMax - lumaMin >= max(kEdgeThresholdMin, lumaMax * kEdgeThreshold)) {
    vec2 dir = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)),
                    (lumaNW + lumaSW) - (lumaNE + lumaSE));
    const float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) *
                                 (0.25 * kReduceMul),
                             kReduceMin);
    const float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpDirMin, vec2(-kSpanMax), vec2(kSpanMax)) * push.texel;
    const vec3 rgbA = 0.5 * (Sample(uv + dir * (1.0 / 3.0 - 0.5)) +
                             Sample(uv + dir * (2.0 / 3.0 - 0.5)));
    const vec3 rgbB =
        rgbA * 0.5 + 0.25 * (Sample(uv + dir * -0.5) + Sample(uv + dir * 0.5));
    const float lumaB = Luma(rgbB);
    rgb = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
  }
  color = vec4(push.linearize != 0u ? Linearize(rgb) : rgb, center.a);
}
