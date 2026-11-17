// Static lattice hashes for the ten short wave trains, four channels per layer.
[[vk::image_format("rgba8")]]
[[vk::binding(0, 0)]] RWTexture2DArray<float4> packets;
struct Params { float span; float halfMap; uint size; uint pad; };
[[vk::binding(1, 0)]] ConstantBuffer<Params> P;

float h21(float2 ip) {
    float3 q = frac(float3(ip.x, ip.y, ip.x) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}
static const float wavelengths[10] = {23.1, 15.5, 52.4, 35.3, 23.8, 16.0, 54.0, 36.4, 24.6, 16.5};

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= P.size) || tid.z >= 3) return;
    float4 result = 0;
    for (uint c = 0; c < 4; ++c) {
        uint j = tid.z * 4 + c;
        if (j < 10) {
            float i = float(j + 2);
            float2 origin = floor(float2(-P.halfMap, P.halfMap - P.span) /
                                  (6.0 * wavelengths[j]) + float2(i * 19.19, i * 7.73));
            result[c] = h21(origin + float2(tid.xy));
        }
    }
    packets[tid] = result;
}
