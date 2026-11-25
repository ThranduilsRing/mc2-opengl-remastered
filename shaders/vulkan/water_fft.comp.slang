// Stateless Phillips spectrum, two 64x64 cascades. One workgroup per FFT row.
// Original implementation; see ../PROVENANCE. No previous-frame input.
struct Spectrum { float2 h; float2 x; float2 y; };
[[vk::binding(0, 0)]] RWStructuredBuffer<Spectrum> rows;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> displacement;
struct Frame { float4 fog; float4 timeCamera; };
[[vk::binding(2, 0)]] ConstantBuffer<Frame> F;
struct Pass { uint axis; uint pad0; uint pad1; uint pad2; };
[[vk::binding(3, 0)]] ConstantBuffer<Pass> P;
groupshared float2 H[64], X[64], Y[64];
float2 mulC(float2 a, float2 b) { return float2(a.x*b.x-a.y*b.y, a.x*b.y+a.y*b.x); }
uint hash(uint n) { n ^= n >> 16; n *= 0x7feb352du; n ^= n >> 15; n *= 0x846ca68bu; return n ^ (n >> 16); }
float2 initial(uint2 p, uint cascade) {
    float span = cascade == 0 ? 256.0 : 64.0;
    float2 k = float2(int2(p) - int2(p.x >= 32 ? 64 : 0, p.y >= 32 ? 64 : 0)) * (6.2831853/span);
    float k2 = dot(k,k);
    if (k2 < 1e-6 || (cascade == 0 && k2 >= 0.0096383)) return 0;
    float alignment = dot(normalize(k), normalize(float2(0.8,0.6)));
    float phillips = 0.0012 * exp(-1.0/(k2*75.0*75.0)) * alignment*alignment
                   * exp(-k2*0.16) / (k2*k2);
    uint seed = hash(p.x + p.y*64 + cascade*8191 + 17);
    float u = (float(seed & 0xffffffu)+1.0)/16777217.0;
    float angle = float(hash(seed) & 0xffffffu)*(6.2831853/16777216.0);
    return sqrt(-2.0*log(u))*float2(cos(angle),sin(angle))
         * sqrt(phillips*0.5)*(6.2831853/span)*4096.0;
}
[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    uint reversed = reversebits(lane) >> 26;
    uint cascade = group.y;
    uint base = cascade*4096;
    if (P.axis == 0) {
        uint2 p = uint2(reversed,group.x);
        float2 k = float2(int2(p)-int2(p.x>=32?64:0,p.y>=32?64:0))*(6.2831853/(cascade==0?256.0:64.0));
        float kl = length(k);
        float angle = sqrt(32.0*kl)*F.timeCamera.x;
        float2 e = float2(cos(angle),sin(angle));
        float2 opposite = initial((64-p)&63,cascade)*float2(1,-1);
        float2 h = mulC(initial(p,cascade),e) + mulC(opposite,e*float2(1,-1));
        H[lane] = h;
        X[lane] = kl>0 ? float2(h.y,-h.x)*k.x/kl : 0;
        Y[lane] = kl>0 ? float2(h.y,-h.x)*k.y/kl : 0;
    } else {
        Spectrum v = rows[base+reversed*64+group.x];
        H[lane]=v.h; X[lane]=v.x; Y[lane]=v.y;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint width=2; width<=64; width*=2) {
        uint half=width/2, j=lane%half, a=(lane/width)*width+j;
        float phase=6.2831853*float(j)/float(width);
        float2 w=float2(cos(phase),sin(phase))*(lane%width<half?1.0:-1.0);
        float2 h=H[a]+mulC(w,H[a+half]);
        float2 x=X[a]+mulC(w,X[a+half]);
        float2 y=Y[a]+mulC(w,Y[a+half]);
        GroupMemoryBarrierWithGroupSync();
        H[lane]=h; X[lane]=x; Y[lane]=y;
        GroupMemoryBarrierWithGroupSync();
    }
    if (P.axis==0) {
        Spectrum v; v.h=H[lane]/64.0; v.x=X[lane]/64.0; v.y=Y[lane]/64.0;
        rows[base+group.x*64+lane]=v;
    } else {
        displacement[base+lane*64+group.x]=float4(X[lane].x,Y[lane].x,H[lane].x,0)/64.0;
    }
}
