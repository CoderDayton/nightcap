// Shared setup for the SMAA passes: the upstream SMAA.hlsl (third_party/smaa)
// in its GLSL 4 mode at the High preset. Vulkan, like Direct3D, puts the
// texture origin at the top left, which is what SMAA and its lookup textures
// expect, so nothing is flipped.
layout(push_constant) uniform Push {
  vec4 metrics;    // 1/width, 1/height, width, height
  uint linearize;  // 1 when the target view re-encodes to sRGB
} push;
#define SMAA_RT_METRICS push.metrics
#define SMAA_GLSL_4 1
#define SMAA_PRESET_HIGH 1
