// water_draw_args.comp.slang -- native Vulkan port of shaders/gpu_driven_cmd_patch.comp for the water draw (step 3.4P).
//
// One workgroup: turns the cull's atomic visibleCount (header[0]) into a VkDrawIndirectCommand
// { vertexCount = min(visibleCount, maxThin) * 6, instanceCount = 1, firstVertex = 0, firstInstance = 0 },
// so the water draw consumes the GPU-produced count without a CPU readback.
//
// The reflection dispatch is 2-D (ROW_TILES wide) so the tile count is not limited by maxComputeWorkGroupCount[0]:
// { x = min(N, ROW_TILES), y = ceil(N / ROW_TILES), 1 }. The spare groups of the last row read SENTINEL_TILE, written here
// into tileList[N .. x*y) (water_reflection.comp returns on it). Keep ROW_TILES/SENTINEL_TILE equal to water_reflect_dispatch.h.
//
// Set 0:
//   b0  StructuredBuffer<uint>      header; [0] = visibleCount
//   b1  RWStructuredBuffer<uint>    draw (words 0..3), reflection dispatch (4..6)
//   header[1] = number of 8x8 tiles (full-res reflection image) holding water (one workgroup per tile)
//   b2  ConstantBuffer<DrawArgsParams>
//   b3  RWStructuredBuffer<uint>    tileList (sentinel padding after the live tiles)

struct DrawArgsParams {
    int4 v;   // maxThinRecords, vertsPerRecord, reflection tile grid x/y (0 = reflection off)
};

static const uint ROW_TILES = 1024u;
static const uint SENTINEL_TILE = 0xFFFFFFFFu;

[[vk::binding(0, 0)]] StructuredBuffer<uint> header;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> drawArgs;
[[vk::binding(2, 0)]] ConstantBuffer<DrawArgsParams> P;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> tileList;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    // No water (or reflection off) -> N = 0 -> zero invocations.
    uint n = min(header[1], uint(P.v.z) * uint(P.v.w));
    uint gx = min(n, ROW_TILES);
    uint gy = (n + ROW_TILES - 1u) / ROW_TILES;
    if (tid.x == 0u)
    {
        uint count = min(header[0], uint(P.v.x));
        drawArgs[0] = count * uint(P.v.y);
        drawArgs[1] = 1u;
        drawArgs[2] = 0u;
        drawArgs[3] = 0u;
        drawArgs[4] = gx;
        drawArgs[5] = gy;
        drawArgs[6] = 1u;
        drawArgs[7] = 0u;
    }
    for (uint i = n + tid.x; i < gx * gy; i += 64u)
        tileList[i] = SENTINEL_TILE;
}
