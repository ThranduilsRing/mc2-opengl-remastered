// water_draw_args.comp.slang -- native Vulkan port of shaders/gpu_driven_cmd_patch.comp for the water draw (step 3.4P).
//
// One invocation: turns the cull's atomic visibleCount (header[0]) into a VkDrawIndirectCommand
// { vertexCount = min(visibleCount, maxThin) * 6, instanceCount = 1, firstVertex = 0, firstInstance = 0 },
// so the water draw consumes the GPU-produced count without a CPU readback.
//
// Set 0:
//   b0  StructuredBuffer<uint>      header; [0] = visibleCount
//   b1  RWStructuredBuffer<uint>    draw (words 0..3), reflection dispatch (4..6)
//   header[1] = number of 8x8 half-res tiles holding water (the reflection dispatches one workgroup per tile)
//   b2  ConstantBuffer<DrawArgsParams>

struct DrawArgsParams {
    int4 v;   // maxThinRecords, vertsPerRecord, reflection tile grid x/y (0 = reflection off)
};

[[vk::binding(0, 0)]] StructuredBuffer<uint> header;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> drawArgs;
[[vk::binding(2, 0)]] ConstantBuffer<DrawArgsParams> P;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint count = min(header[0], uint(P.v.x));
    drawArgs[0] = count * uint(P.v.y);
    drawArgs[1] = 1u;
    drawArgs[2] = 0u;
    drawArgs[3] = 0u;
    // One workgroup per tile that holds water; no water (or reflection off) -> zero invocations.
    drawArgs[4] = min(header[1], uint(P.v.z) * uint(P.v.w));
    drawArgs[5] = 1u;
    drawArgs[6] = 1u;
    drawArgs[7] = 0u;
}
