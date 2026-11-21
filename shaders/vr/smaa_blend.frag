#version 450
#extension GL_GOOGLE_include_directive : require
#include "smaa_common.glsl"
#define SMAA_INCLUDE_VS 0
#include "SMAA.hlsl"
layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(set = 0, binding = 1) uniform sampler2D blendTex;
layout(location = 0) in vec2 texcoord;
layout(location = 1) in vec4 offset;
layout(location = 0) out vec4 color;

// The target encodes linear values back to sRGB, which restores these bytes.
vec3 Linearize(vec3 c) {
  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)),
             step(vec3(0.04045), c));
}

void main() {
  color = SMAANeighborhoodBlendingPS(texcoord, offset, colorTex, blendTex);
  if (push.linearize != 0u) color.rgb = Linearize(color.rgb);
}
