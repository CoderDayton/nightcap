#version 450
#extension GL_GOOGLE_include_directive : require
#include "smaa_common.glsl"
#define SMAA_INCLUDE_VS 0
#include "SMAA.hlsl"
// Luma edges on the eye's sRGB-encoded bytes, as SMAA recommends.
layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(location = 0) in vec2 texcoord;
layout(location = 1) in vec4 offset[3];
layout(location = 0) out vec2 edges;

void main() { edges = SMAALumaEdgeDetectionPS(texcoord, offset, colorTex); }
