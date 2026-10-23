#ifndef GROUND_LIGHTING_GLSL
#define GROUND_LIGHTING_GLSL
// One terrain/cover compose. Existing sun-band, pool, ambient and contact inputs;
// no new light producer or terrain cover term. Preserve terrain operation order.
vec3 composeGroundLighting(vec3 baseColor, vec3 ambient, vec3 direct, float shadow, vec3 localLight) {
    return baseColor * ambient + baseColor * direct * shadow + localLight;
}
vec3 shadeGround(vec3 baseColor, vec3 N, float normalLight, float shadow,
                 float flatLitShare, vec3 worldPos) {
        vec3 lit = composeGroundLighting(baseColor, vec3(0), vec3(normalLight), shadow, vec3(0));
        // LOCAL-LIGHTS-4: the mission's night scale of the prelit ground (1.0 by day);
        // the local light pools below are added at full strength.
        lit *= uT.sceneLightScale;

        // VK-PARITY-TERRAIN-2: NIGHT-LIGHT-POOLS-1..4 / LIGHT-CLUSTER-PERF-1 /
        // MOVER-CONTACT-SHADOW-1 — the SAME shared body GL runs (terrain_lod_chunk.frag
        // :2056-2066), ADDITIVE after the shadow multiply. Cone spill / LIGHT-REALISM
        // reshape is baked into the pool by the shared CPU producer.
        float moverContact = 0.0;
        if (uT.numPoolLights > 0) {
            lit += tlp_terrainPoolLight(worldPos, uT.numPoolLights,
                                        uT.lightGridDim, vec2(uT.lightGridMinX, uT.lightGridMinY),
                                        vec2(uT.lightGridInvCellX, uT.lightGridInvCellY),
                                        uT.lightPoolSoft, uT.lightPoolMoverDim, uT.lightPoolMoverScale,
                                        uT.lightPoolMoverMax, uT.lightPoolCompress,
                                        uT.moverContactShadow, moverContact);
        }
        // The burnin already contains mission ambient; no second ambient floor.

        // MOVER-CONTACT-SHADOW-1 (terrain_lod_chunk.frag:2287-2288): contact-AO on the
        // fully composited lighting. moverContact==0 unless MC2_MOVER_CONTACT_SHADOW>0.
        if (moverContact > 0.0)
            lit *= (1.0 - 0.55 * moverContact);

    return lit;
}
#endif
