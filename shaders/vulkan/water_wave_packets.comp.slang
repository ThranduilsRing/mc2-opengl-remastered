// Static world-space noise for the ten short wave trains, four channels per layer.
[[vk::image_format("rgba8")]]
[[vk::binding(0, 0)]] RWTexture2DArray<float4> packets;
struct Params { float span; float halfMap; uint size; uint pad; };
[[vk::push_constant]] ConstantBuffer<Params> P;

float h21(float2 ip) {
    precise float3 q = frac(float3(ip.x, ip.y, ip.x) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}
float vnoise(float2 uv) {
    float2 i = floor(uv), f = frac(uv);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(h21(i), h21(i + float2(1, 0)), f.x),
                lerp(h21(i + float2(0, 1)), h21(i + float2(1, 1)), f.x), f.y);
}
static const float wavelengths[10] = {23.1, 15.5, 52.4, 35.3, 23.8, 16.0, 54.0, 36.4, 24.6, 16.5};

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= P.size) || tid.z >= 3) return;
    float2 grid = (float2(tid.xy) + 0.5) * (P.span / float(P.size));
    float2 p = float2(grid.x - P.halfMap, P.halfMap - grid.y);
    float4 result = 0;
    for (uint c = 0; c < 4; ++c) {
        uint j = tid.z * 4 + c;
        if (j < 10) {
            float i = float(j + 2);
            result[c] = vnoise(p / (6.0 * wavelengths[j]) + float2(i * 19.19, i * 7.73));
        }
    }
    packets[tid] = result;
}
