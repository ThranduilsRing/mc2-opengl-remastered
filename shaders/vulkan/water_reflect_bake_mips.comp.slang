// First level converts native reverse-Z D32 into height above the water.
// Later levels conservatively reduce the previous R16F level, including odd edges.
[[vk::binding(0, 0)]] Texture2D<float> source;
[[vk::image_format("r16f")]]
[[vk::binding(1, 0)]] RWTexture2D<float> destination;
struct Params { int2 size; int fromDepth; float heightRange; };
[[vk::binding(2, 0)]] ConstantBuffer<Params> P;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= uint2(P.size))) return;
    uint w, h;
    source.GetDimensions(w, h);
    int2 lo = int2(tid.xy) * int2(w, h) / P.size;
    int2 hi = (int2(tid.xy) + 1) * int2(w, h) / P.size;
    float height = 0;
    for (int y = lo.y; y < hi.y; ++y)
        for (int x = lo.x; x < hi.x; ++x)
            height = max(height, source.Load(int3(x, y, 0)));
    height *= P.fromDepth != 0 ? P.heightRange : 1.0;
    // Round outward: half precision must never understate a blocker height.
    destination[tid.xy] = height > 0 ? height + max(0.001, height * 0.001) : 0;
}
