// rt_scene_probe.comp.slang -- RT scene material probe (MC2_VK_RT_SELFTEST, vk_rt_scene.cpp probeMaterials): one invocation per ray, an inline
// ray query against the scene TLAS with the alpha-cutout test every ray-visible consumer must run (docs/rt-scene-contract.md). No SBT, no RT
// pipeline. Slang port of the former rt_scene_probe.comp. Host-side bindings: vk_rt_descriptors.h kSceneProbeBindings.
// rays[2*i] = (origin.xyz, tMin), rays[2*i+1] = (direction.xyz, tMax).
// results[2*i] = (hit, t, primitiveId, instanceCustomIndex); results[2*i+1] = (material row, albedo layer, packet / tile, flags), all -1 on a miss.
// Material row of a hit = instance SBT record offset + geometry index (one geometry per prop packet, one per terrain tile).

struct MaterialRow {         // vk_rt_scene.h MaterialRow, 32 bytes
    int albedoLayer;
    uint flags;
    float alphaCut;
    int materialId;
    float uvScaleX;
    float uvScaleY;
    uint firstIndex;         // into uvIndex: this geometry's triangles, 3 rebased VBO vertex indices each
    uint packet;
};
static const uint kMatAlphaOn = 1u;
static const uint kMatCutout = 2u;

[[vk::binding(0, 0)]] RaytracingAccelerationStructure uTlas;
[[vk::binding(1, 0)]] StructuredBuffer<float4> rays;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> results;
[[vk::binding(3, 0)]] StructuredBuffer<MaterialRow> rows;
[[vk::binding(4, 0)]] StructuredBuffer<uint> uvIndex;
[[vk::binding(5, 0)]] Sampler2DArray uAlphaOff;
// PROP-ARRAY-BUCKETS: the alpha-ON size buckets on bindings 6..9 (MaterialRow.albedoLayer carries the bucket).
[[vk::binding(6, 0)]] Sampler2DArray uAlphaOn;
[[vk::binding(7, 0)]] Sampler2DArray uAlphaOn1;
[[vk::binding(8, 0)]] Sampler2DArray uAlphaOn2;
[[vk::binding(9, 0)]] Sampler2DArray uAlphaOn3;
// The alpha-OFF size buckets: bucket 0 on binding 5, buckets 1..3 on bindings 10..12.
[[vk::binding(10, 0)]] Sampler2DArray uAlphaOff1;
[[vk::binding(11, 0)]] Sampler2DArray uAlphaOff2;
[[vk::binding(12, 0)]] Sampler2DArray uAlphaOff3;

struct Push {
    uint2 vbo;               // the static-prop VBO device address (40-byte vertices, uv at +24)
    uint stride;
    uint mode;               // 0: alpha-tested cutouts; 1: every candidate committed (the opaque reference)
};
[[vk::push_constant]] Push pc;

// PROP-ARRAY-BUCKETS (GameOS/gameos/prop_array_buckets.h): a packet's alpha-ON layer is (bucket << 16) | layer-in-bucket.
static const int kPropBucketLayerBits = 16;
int propBucketOf(int layer) { return layer >> kPropBucketLayerBits; }
int propBucketLayer(int layer) { return layer & ((1 << kPropBucketLayerBits) - 1); }

float alphaOnAt(int layer, float2 uv)
{
    float3 tc = float3(uv, float(propBucketLayer(layer)));
    switch (propBucketOf(layer)) {
        case 1: return uAlphaOn1.SampleLevel(tc, 0.0).a;
        case 2: return uAlphaOn2.SampleLevel(tc, 0.0).a;
        case 3: return uAlphaOn3.SampleLevel(tc, 0.0).a;
        default: return uAlphaOn.SampleLevel(tc, 0.0).a;
    }
}

float alphaOffAt(int layer, float2 uv)
{
    float3 tc = float3(uv, float(propBucketLayer(layer)));
    switch (propBucketOf(layer)) {
        case 1: return uAlphaOff1.SampleLevel(tc, 0.0).a;
        case 2: return uAlphaOff2.SampleLevel(tc, 0.0).a;
        case 3: return uAlphaOff3.SampleLevel(tc, 0.0).a;
        default: return uAlphaOff.SampleLevel(tc, 0.0).a;
    }
}

float2 vertexUv(uint v)
{
    float* f = (float*)(uint64_t(pc.vbo.x) | (uint64_t(pc.vbo.y) << 32));
    uint base = (v * pc.stride + 24u) / 4u;
    return float2(f[base], f[base + 1u]);
}

// The raster's cut (color_prop_tier1.frag): fract(uv) * uvScale on the packet's layer at mip 0, kept when alpha >= the threshold.
bool alphaKeeps(MaterialRow m, uint prim, float2 bary)
{
    if (m.albedoLayer < 0) return true;
    uint i = m.firstIndex + 3u * prim;
    float2 uv = vertexUv(uvIndex[i]) * (1.0 - bary.x - bary.y) + vertexUv(uvIndex[i + 1u]) * bary.x + vertexUv(uvIndex[i + 2u]) * bary.y;
    float2 st = frac(uv) * float2(m.uvScaleX, m.uvScaleY);
    float a = (m.flags & kMatAlphaOn) != 0u ? alphaOnAt(m.albedoLayer, st) : alphaOffAt(m.albedoLayer, st);
    return a >= m.alphaCut;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    uint nRes, resStride, nRows, rowStride;
    results.GetDimensions(nRes, resStride);
    rows.GetDimensions(nRows, rowStride);
    if (2u * i + 1u >= nRes) return;

    float4 o = rays[2u * i];
    float4 d = rays[2u * i + 1u];

    RayDesc ray;
    ray.Origin = o.xyz;
    ray.TMin = o.w;
    ray.Direction = d.xyz;
    ray.TMax = d.w;

    RayQuery<RAY_FLAG_NONE> rq;
    rq.TraceRayInline(uTlas, RAY_FLAG_NONE, 0xFF, ray);
    while (rq.Proceed()) {
        if (rq.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint row = rq.CandidateInstanceContributionToHitGroupIndex() + rq.CandidateGeometryIndex();
        bool keep = pc.mode == 1u || row >= nRows;
        if (!keep) keep = (rows[row].flags & kMatCutout) == 0u ||
                          alphaKeeps(rows[row], rq.CandidatePrimitiveIndex(), rq.CandidateTriangleBarycentrics());
        if (keep) rq.CommitNonOpaqueTriangleHit();
    }

    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        uint row = rq.CommittedInstanceContributionToHitGroupIndex() + rq.CommittedGeometryIndex();
        results[2u * i] = float4(1.0, rq.CommittedRayT(), float(rq.CommittedPrimitiveIndex()), float(rq.CommittedInstanceID()));
        if (row < nRows)
            results[2u * i + 1u] = float4(float(row), float(rows[row].albedoLayer), float(rows[row].packet), float(rows[row].flags));
        else
            results[2u * i + 1u] = float4(float(row), -1.0, -1.0, -1.0);
    } else {
        results[2u * i] = float4(0.0, 0.0, -1.0, -1.0);
        results[2u * i + 1u] = float4(-1.0);
    }
}
