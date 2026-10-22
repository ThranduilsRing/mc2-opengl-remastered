#ifndef MOVER_SHADOW_GLSL
#define MOVER_SHADOW_GLSL
// Consume the existing dynamic receiver. Until shadow P6 provides its frame,
// terrain's valid dummy descriptors/enable bits give visibility 1; no producer.
float moverVisibility(vec3 worldPos, vec3 N) {
    if (uT.csmEnabled == 0 && uT.enableDynShadows == 0) return 1.0;
    return terrainDynamicShadow(worldPos, N, lsvk_shadowSun(uT.terrainLightDir.xyz, mv_shadowKnobs()));
}
#endif
