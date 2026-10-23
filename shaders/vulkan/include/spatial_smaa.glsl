// Reference SMAA 1x, medium preset. Display-encoded UNORM edges; no history, jitter or predication.
layout(push_constant) uniform AAParams { vec4 metrics; } aa;
#define SMAA_RT_METRICS aa.metrics
#define SMAA_GLSL_4
#define SMAA_PRESET_MEDIUM
#define SMAA_REPROJECTION 0
#define SMAA_PREDICATION 0
#include "../../../3rdparty/smaa/SMAA.hlsl"
