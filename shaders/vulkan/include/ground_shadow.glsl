// One filtered ground-mask sample and the shared current-frame mover seam.
#ifndef MC2_GROUND_SHADOW_GLSL
#define MC2_GROUND_SHADOW_GLSL
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

float terrainStaticShadow(vec3 worldPos) {
    if (!terrainStaticMaskBound()) return 1.0;
    float under=smoothstep(0.0,TERRAIN_WATER_SHADOW_DEPTH,uT.shorelineWaterElev-worldPos.z);
    float shadow = terrainStaticMask(worldPos.xy);
    return mix(mix(sunShadowFloor(0.4), 1.0, shadow), 1.0, under);
}
float terrainDynamicShadow(vec3 worldPos,vec3 N) {
    float shadow = moverVisibility(worldPos, N);
    return mix(sunShadowFloor(0.4), 1.0, shadow);
}
#endif
