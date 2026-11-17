// rt_smoke.comp.slang -- RT infrastructure self-test (MC2_VK_RT_SELFTEST, vk_rt_selftest.cpp): one invocation per ray, an inline ray query
// against the test TLAS. No SBT, no RT pipeline. Slang port of the former rt_smoke.comp.
// rays[2*i] = (origin.xyz, tMin), rays[2*i+1] = (direction.xyz, tMax); results[i] = (hit, t, primitiveId, instanceCustomIndex).
// Host-side bindings: vk_rt_descriptors.h (kSmokeBindings).

[[vk::binding(0, 0)]] RaytracingAccelerationStructure uTlas;
[[vk::binding(1, 0)]] StructuredBuffer<float4> rays;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> results;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    uint n, stride;
    results.GetDimensions(n, stride);
    if (i >= n) return;

    float4 o = rays[2u * i];
    float4 d = rays[2u * i + 1u];

    RayDesc ray;
    ray.Origin = o.xyz;
    ray.TMin = o.w;
    ray.Direction = d.xyz;
    ray.TMax = d.w;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> rq;
    rq.TraceRayInline(uTlas, RAY_FLAG_FORCE_OPAQUE, 0xFF, ray);
    while (rq.Proceed()) {}

    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        results[i] = float4(1.0, rq.CommittedRayT(), float(rq.CommittedPrimitiveIndex()), float(rq.CommittedInstanceID()));
    } else {
        results[i] = float4(0.0, 0.0, -1.0, -1.0);
    }
}
