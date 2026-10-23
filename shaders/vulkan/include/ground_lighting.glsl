#ifndef GROUND_LIGHTING_GLSL
#define GROUND_LIGHTING_GLSL
// One terrain/cover compose. Existing sun-band, pool, ambient and contact inputs;
// no new light producer or terrain cover term. Preserve terrain operation order.
vec3 shadeGround(vec3 baseColor, vec3 N, float normalLight, float shadow,
                 float flatLitShare, vec3 worldPos) {
        vec3 lit = baseColor * normalLight * shadow;

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
        // PAD-AREA-GLOW-1 (terrain_lod_chunk.frag:2075-2076). useCement gates the read of
        // the cement-word SSBO (dummy 16B until the snapshot lands).
        vec3 padGlow = vec3(uT.padGlowR, uT.padGlowG, uT.padGlowB);
        if (dot(padGlow, padGlow) > 0.0 && uT.useCement != 0)
            lit += padGlow * tlp_padGlowMask(worldPos.xy, uT.halfMap, uT.mapSide, uT.padGlowSpill);

        // VK-PARITY-TERRAIN-1: TERRAIN-AMBIENT-FLOOR-2, verbatim from
        // terrain_lod_chunk.frag:2267-2277. baseColor == base here (VK has no material
        // tint); flatLitShare == pureConcrete at stock u_cementLit=0 (the GL expression
        // cementLightBlend*(1-u_cementLit)). Floor/gate come from the same C++ producer.
        if (uT.ambientFloor > 0.0 && uT.ambientDayGate > 0.0) {
            float skyF   = N.z * 0.5 + 0.5;
            vec3  ambSky = vec3(0.55, 0.62, 0.75);
            vec3  ambGnd = vec3(0.34, 0.30, 0.24);
            vec3  ambCol = mix(ambGnd, ambSky, skyF);
            float ambShadow = mix(0.55, 1.0, shadow);
            vec3  ambTerm   = baseColor * ambCol * uT.ambientFloor * ambShadow
                            * uT.ambientDayGate * (1.0 - flatLitShare);
            lit += ambTerm;
        }

        // MOVER-CONTACT-SHADOW-1 (terrain_lod_chunk.frag:2287-2288): contact-AO on the
        // fully composited lighting. moverContact==0 unless MC2_MOVER_CONTACT_SHADOW>0.
        if (moverContact > 0.0)
            lit *= (1.0 - 0.55 * moverContact);

    return lit;
}
#endif
