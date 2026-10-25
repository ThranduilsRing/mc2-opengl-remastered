// World-XY lists conservatively intersect each light sphere/cone with a cell's
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

// Matches lpa_spotAim in light_pool_attenuation.hglsl.
float3 spotAim(uint packed) {
    float2 e = float2((packed >> 8) & 255, (packed >> 16) & 255) * (2.0 / 255.0) - 1.0;
    float3 f = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (f.z < 0.0) f.xy = (1.0 - abs(f.yx)) * float2(f.x >= 0.0 ? 1.0 : -1.0, f.y >= 0.0 ? 1.0 : -1.0);
    return normalize(f);
}

// Maximum projection of a unit spherical cone on one positive axis, including
// its origin. The negative bound is the same projection on the opposite axis.
float coneExtent(float axis, float cosine, float sine) {
    return axis >= cosine ? 1.0 : max(0.0, axis * cosine + sqrt(max(0.0, 1.0 - axis * axis)) * sine);
}

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
        if (l.lightType == 0 || r <= 0 || dot(delta, delta) > r * r) continue;
        uint cone = l.lightType >> 24;
        if (cone != 0) {
            float3 aim = spotAim(l.lightType);
            // Byte 255 uses the fragment shader's 1e-3 angle floor, rather than
            // a zero-width cone. Widen it and pad world bounds for roundoff.
            float c = min(float(cone) * (1.0 / 255.0), 0.999999);
            float s = sqrt(max(0.0, 1.0 - c * c));
            float2 extentHi = float2(coneExtent(aim.x, c, s), coneExtent(aim.y, c, s));
            float2 extentLo = float2(coneExtent(-aim.x, c, s), coneExtent(-aim.y, c, s));
            float2 coneLo = l.position.xy - r * (extentLo + 1e-5) - 0.01;
            float2 coneHi = l.position.xy + r * (extentHi + 1e-5) + 0.01;
            if (any(hi < coneLo) || any(lo > coneHi)) continue;
        }
        indices[first + count++] = i;
    }
    cells[cell] = uint2(first, count);
}
