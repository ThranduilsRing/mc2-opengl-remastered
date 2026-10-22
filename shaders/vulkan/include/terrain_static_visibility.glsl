#ifndef TERRAIN_STATIC_VISIBILITY_GLSL
#define TERRAIN_STATIC_VISIBILITY_GLSL
layout(set=TERRAIN_RESOURCE_SET, binding=17) uniform sampler2D u_staticVisibilityTiles;

vec2 terrainStaticTile(ivec2 tile) {
    ivec2 size = textureSize(u_staticVisibilityTiles,0);
    if (any(lessThan(tile,ivec2(0))) || any(greaterThanEqual(tile,size))) return vec2(1.0);
    return texelFetch(u_staticVisibilityTiles,tile,0).rg;
}
float terrainStaticShadow(vec3 worldPos, vec3 normal, vec3 lightDir, LsvkShadowKnobs k) {
    lightDir = lsvk_shadowSun(lightDir,k);
    if ((k.flags & LSVK_KNOB_BANDFIX_V2)!=0) {
        float invR = length(vec3(uT.staticLightSpaceMatrix[0][0],uT.staticLightSpaceMatrix[1][0],uT.staticLightSpaceMatrix[2][0]));
        float texelWorld = invR>1e-8 ? 2.0/(invR*float(textureSize(u_staticShadow,0).x)) : 0.0;
        worldPos += normal*texelWorld;
        normal = vec3(0,0,1);
    }
    vec4 ls = uT.staticLightSpaceMatrix*vec4(worldPos,1);
    vec3 pc = ls.xyz/ls.w;
    pc.xy = pc.xy*0.5+0.5;
    if (pc.z<0.0 || pc.z>1.0 || any(lessThan(pc.xy,vec2(0))) || any(greaterThan(pc.xy,vec2(1)))) return 1.0;
    float nl = max(dot(normal,lightDir),0.0);
    if (nl<0.05) return 1.0;
    float refDepth = pc.z-max(0.005*(1.0-nl),0.002);
    // Derivatives stay in uniform flow even when a tile proves visibility early.
    float hardness = clamp(length(vec2(dFdx(pc.z),dFdy(pc.z)))*180.0,0.0,1.0);
    float radius = max(uT.shadowSoftness,0.5)*mix(2.4,0.8,hardness);
    ivec2 shadowSize = textureSize(u_staticShadow,0);
    ivec2 tileSize = textureSize(u_staticVisibilityTiles,0);
    if (shadowSize.x>32 && all(equal(tileSize,(shadowSize+31)/32))) {
        ivec2 base = ivec2(floor(pc.xy*vec2(shadowSize)-0.5));
        // Includes hardware bilinear support and the entire clamped radius-7 tent.
        ivec2 lo = ivec2(floor(vec2(base-8)/32.0));
        ivec2 hi = ivec2(floor(vec2(base+9)/32.0));
        vec2 a=terrainStaticTile(lo), b=terrainStaticTile(ivec2(hi.x,lo.y));
        vec2 c=terrainStaticTile(ivec2(lo.x,hi.y)), d=terrainStaticTile(hi);
        float lowest = min(min(a.x,b.x),min(c.x,d.x));
        float highest = max(max(a.y,b.y),max(c.y,d.y));
        if (refDepth<=lowest) return 1.0;
        if (refDepth>highest) return 0.4;
    }
    return mix(0.4,1.0,mc2ShadowTent(u_staticShadow,vec3(pc.xy,refDepth),radius));
}
#endif
