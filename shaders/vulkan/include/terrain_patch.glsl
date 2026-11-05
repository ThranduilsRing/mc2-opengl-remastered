// Vertex-only patch data. One 32-byte record per indirect draw, read through buffer device address and indexed by gl_DrawID: the push block
// carries the address of the first record of the run this vkCmdDrawIndexedIndirect call draws. LOCKSTEP with TerrainPatchConstants in
// vulkan_terrain_island_internal.h. The including vertex shader declares GL_ARB_shader_draw_parameters and GL_EXT_buffer_reference at the top.
struct TerrainPatchRec {
    int blockOriginX;
    int blockOriginY;
    int quadCountX;
    int quadCountY;
    int edgeStitch;
    int visualMode;
    float skirtDepth;
    int pad;
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer TerrainPatchRecs {
    TerrainPatchRec p[];
};
layout(push_constant) uniform TerrainPatchBasePC {
    TerrainPatchRecs recs;   // device address of the first record of this call's run
} uPatchBase;
#define uP (uPatchBase.recs.p[gl_DrawIDARB])
