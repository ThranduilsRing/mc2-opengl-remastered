// The waterSurfaceRow cliff filter, baked only when the raw visual field changes.
#include "../include/terrain_cliff_math.hglsl"
[[vk::binding(0, 0)]] StructuredBuffer<float> raw;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> surface;
struct Params { int side; int pad0; int pad1; int pad2; };
[[vk::binding(2, 0)]] ConstantBuffer<Params> P;
float rawAt(int2 p) {
    p = clamp(p, int2(0), int2(P.side - 1));
    return raw[p.x + p.y * P.side];
}
// 2D dispatch keeps the group count per axis at side/8 (a flattened 1D
// dispatch exceeds maxComputeWorkGroupCount[0] = 65535 on large fields).
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(P.side) || id.y >= uint(P.side)) return;
    int2 p = int2(id.xy);
    uint index = id.x + id.y * uint(P.side);
    float c = rawAt(p);
    float n = rawAt(p + int2(0, -2)), s = rawAt(p + int2(0, 2));
    float e = rawAt(p + int2(2, 0)), w = rawAt(p + int2(-2, 0));
    float rise = max(max(abs(n - c), abs(s - c)), max(abs(e - c), abs(w - c)));
    // Match the vector row's left-associated diagonal and cardinal additions.
    float diagonal = rawAt(p + int2(-2, -2)) + rawAt(p + int2(2, -2))
                   + rawAt(p + int2(-2, 2)) + rawAt(p + int2(2, 2));
    float cardinal = n + s + e + w;
    surface[index] = rise <= 24.0 ? c : cliffFilteredHeight(c, cardinal, diagonal, rise);
}
