// Native actor-visibility producer. Input layout matches GpuActorRecord (64 bytes).
struct ActorRecord {
    float3 worldCenter; float boundingRadius;
    float3 worldAabbMin; uint category;
    float3 worldAabbMax; uint flags;
    uint actorId; uint prevVisibilityBit; uint consumerFlags; uint blockIdx;
};
struct Params {
    float4 c0; float4 c1; float4 c2; float4 c3;
    uint4 counts; // record count, frame, unused, unused
    float4 options; // dilation, unused, unused, unused
};
[[vk::binding(0,0)]] StructuredBuffer<ActorRecord> actors;
[[vk::binding(1,0)]] ConstantBuffer<Params> P;
[[vk::binding(2,0)]] RWStructuredBuffer<uint> result;
// result[0..3]: visible, frame, dilated-only, culled; then one raw vis bit per record.
bool pointAdmit(float4 c, float dilation) {
    float s=c.w<0.0 ? -1.0 : 1.0;
    c*=s;
    if (c.w<1e-5) return false;
    float bound=c.w*(1.0+dilation);
    if (c.x < -bound || c.x > bound) return false;
    if (c.y < -bound || c.y > bound) return false;
    if (c.z < -dilation*c.w || c.z > bound) return false;
    return true;
}
bool sphereAdmit(float4 c, float radius, float scale) {
    c*=c.w<0.0 ? -1.0 : 1.0;
    if (c.w<1e-5) return radius>0.0;
    float t=radius*scale;
    if (c.x < -c.w-t || c.x > c.w+t) return false;
    if (c.y < -c.w-t || c.y > c.w+t) return false;
    if (c.z < -t || c.z > c.w+t) return false;
    return true;
}
[numthreads(64,1,1)]
void main(uint3 tid:SV_DispatchThreadID) {
    uint i=tid.x;
    if (i>=P.counts.x) return;
    ActorRecord a=actors[i];
    float4 clip=P.c0*a.worldCenter.x + P.c1*a.worldCenter.y + P.c2*a.worldCenter.z + P.c3;
    float scale=max(length(float3(P.c0.x,P.c1.x,P.c2.x)), length(float3(P.c0.y,P.c1.y,P.c2.y)));
    bool strict=a.boundingRadius>0.0 ? sphereAdmit(clip,a.boundingRadius,scale) : pointAdmit(clip,0.0);
    bool dilated=P.options.x>0.0 ? pointAdmit(clip,P.options.x) : strict;
    if ((a.flags&1u)!=0u) { strict=true; dilated=true; }
    bool visible=strict || dilated;
    result[4+i]=visible ? 1u : 0u;
    uint unused;
    if (visible) InterlockedAdd(result[0],1u,unused);
    else InterlockedAdd(result[3],1u,unused);
    if (dilated && !strict) InterlockedAdd(result[2],1u,unused);
    if (i==0u) result[1]=P.counts.y;
}
