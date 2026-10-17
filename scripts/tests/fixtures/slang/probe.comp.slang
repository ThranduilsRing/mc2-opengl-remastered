// Test fixture only. Set 0: b0 read-only SSBO, b1 read-write SSBO.
[[vk::binding(0, 0)]] StructuredBuffer<float> src;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> dst;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { dst[id.x] = src[id.x] * 2.0; }
