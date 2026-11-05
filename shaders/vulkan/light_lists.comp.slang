// World-XY lists conservatively intersect each light sphere with a cell's
// vertical prism. Source order per cell preserves accumulation. Cells draw their slices from one shared pool (indices[0] is the next free word,
// seeded to 1 by the host; P.info.w is the pool size in words): count first, reserve once, then write; a cell that would overrun the pool is cut short.
struct PoolLight {
    float3 position; uint lightType;
    float3 color; float closeDistance;
    float farDistance; float oneOverDistance; float mover; float intensity;
};
struct GridParams { float4 minInv; uint4 info; };
[[vk::binding(0, 0)]] ConstantBuffer<GridParams> P;
[[vk::binding(1, 0)]] StructuredBuffer<PoolLight> lights;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint2> cells;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> indices;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint dim = P.info.x, n = P.info.y, cell = tid.x;
    if (cell >= dim * dim) return;
    float2 lo = P.minInv.xy + float2(cell % dim, cell / dim) / P.minInv.zw;
    float2 hi = lo + 1.0 / P.minInv.zw;
    float scale = asfloat(P.info.z);
    uint count = 0;
    for (uint pass = 0; pass < 2; ++pass) {
        uint first = 0, k = 0;
        if (pass == 1) {
            if (count == 0) { cells[cell] = uint2(0, 0); return; }
            InterlockedAdd(indices[0], count, first);
            if (first >= P.info.w) { cells[cell] = uint2(0, 0); return; }
        }
        for (uint i = 0; i < n; ++i) {
            PoolLight l = lights[i];
            float2 delta = l.position.xy - clamp(l.position.xy, lo, hi);
            float r = l.farDistance * (l.mover > 0.5 && scale < 0.999 ? max(scale, 0.05) : 1.0) * 1.00001;
            if (l.lightType != 0 && r > 0 && dot(delta, delta) <= r * r) {
                if (pass == 0) ++count;
                else if (first + k < P.info.w) { indices[first + k] = i; ++k; }
            }
        }
        if (pass == 1) cells[cell] = uint2(first, k);
    }
}
