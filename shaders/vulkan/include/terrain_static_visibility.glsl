#ifndef TERRAIN_STATIC_VISIBILITY_GLSL
#define TERRAIN_STATIC_VISIBILITY_GLSL
// The static sun shadow of the ground: the shadow bake's visibility mask (R8 with mips over the terrain grid, row 0 north, evaluated at
// max(terrain, water), props pre-filtered with contact hardening and terrain self-shadow included). One bilinear tap. While the bake is off
// (MC2_SHADOW_BAKE=0) the binding holds a 1x1 placeholder and the old static map with its tent filter is used instead.
layout(set=0, binding=17) uniform sampler2D u_staticMask;

// World units of water over which the ground's static shadow fades out.
const float TERRAIN_WATER_SHADOW_DEPTH = 24.0;

bool terrainStaticMaskBound() { return textureSize(u_staticMask, 0).x > 1; }

// Raw mask visibility in [0, 1] at a world XY (1 off the map).
float terrainStaticMask(vec2 xy) {
    float span = 128.0 * float(max(uT.mapSide - 1, 1));
    vec2 uv = vec2(xy.x + uT.halfMap, uT.halfMap - xy.y) / span;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    return texture(u_staticMask, uv).r;
}

float terrainStaticShadowLegacy(vec3 worldPos, vec3 normal, vec3 lightDir, LsvkShadowKnobs k) {
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
    float hardness = clamp(length(vec2(dFdx(pc.z),dFdy(pc.z)))*180.0,0.0,1.0);
    float radius = max(uT.shadowSoftness,0.5)*mix(2.4,0.8,hardness);
    return mix(0.4,1.0,mc2ShadowTent(u_staticShadow,vec3(pc.xy,refDepth),radius));
}

// Visibility in the receivers' [0.4, 1] domain.
float terrainStaticShadow(vec3 worldPos, vec3 normal, vec3 lightDir, LsvkShadowKnobs k) {
    if (terrainStaticMaskBound()) {
        // Under water the surface carries the shadow (water_geom.frag reads the same texel); the bed's own copy fades out with depth.
        float under = smoothstep(0.0, TERRAIN_WATER_SHADOW_DEPTH, uT.shorelineWaterElev - worldPos.z);
        return mix(mix(0.4, 1.0, terrainStaticMask(worldPos.xy)), 1.0, under);
    }
    return terrainStaticShadowLegacy(worldPos, normal, lightDir, k);
}
#endif
