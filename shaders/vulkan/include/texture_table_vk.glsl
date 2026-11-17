#ifndef MC2_TEXTURE_TABLE_VK_GLSL
#define MC2_TEXTURE_TABLE_VK_GLSL
#extension GL_EXT_nonuniform_qualifier : require
// Each consumer supplies its pipeline's first free descriptor set.
layout(set = MC2_TEX_SET, binding = 0) uniform texture2D gTex[];
layout(set = MC2_TEX_SET, binding = 1) uniform sampler gSamp[16];
layout(set = MC2_TEX_SET, binding = 2) uniform texture2DArray gTexArr[];
#define MC2_SAMP_REPEAT_BIASED 0
#define MC2_SAMP_CLAMP_BIASED 1
#define MC2_SAMP_CLAMP_BIASED_LOD16 2
#define MC2_SAMP_REPEAT_BASE_LEVEL 3
#define MC2_SAMP_CLAMP_ANISO8_BIASED 4
#define MC2_SAMP_REPEAT_BIASED_LOD0 5
#define MC2_TEX(i, s) sampler2D(gTex[nonuniformEXT(i)], gSamp[s])
#define MC2_TEXARR(i, s) sampler2DArray(gTexArr[nonuniformEXT(i)], gSamp[s])
#endif
