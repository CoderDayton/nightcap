#version 450
#extension GL_GOOGLE_include_directive : require
#include "smaa_common.glsl"
#define SMAA_INCLUDE_PS 0
#include "SMAA.hlsl"
layout(location = 0) out vec2 texcoord;
layout(location = 1) out vec4 offset;

void main() {
  texcoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(texcoord * 2.0 - 1.0, 0.0, 1.0);
  SMAANeighborhoodBlendingVS(texcoord, offset);
}
