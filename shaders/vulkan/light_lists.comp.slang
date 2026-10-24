// World-XY lists conservatively intersect each light sphere with a cell's
// vertical prism. Source order and full per-cell capacity preserve accumulation.
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
    uint first = cell * n, count = 0;
    for (uint i = 0; i < n; ++i) {
        PoolLight l = lights[i];
        float2 delta = l.position.xy - clamp(l.position.xy, lo, hi);
        float scale = asfloat(P.info.z);
        float r = l.farDistance * (l.mover > 0.5 && scale < 0.999 ? max(scale, 0.05) : 1.0) * 1.00001;
        if (l.lightType != 0 && r > 0 && dot(delta, delta) <= r * r)
            indices[first + count++] = i;
    }
    cells[cell] = uint2(first, count);
}
