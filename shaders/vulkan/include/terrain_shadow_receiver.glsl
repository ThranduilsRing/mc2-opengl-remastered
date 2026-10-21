#ifndef TERRAIN_SHADOW_RECEIVER_GLSL
#define TERRAIN_SHADOW_RECEIVER_GLSL
// T15: fixed-budget DYNAMIC shadow receiver for terrain only. Object/overlay receivers keep
// lighting_shadow_vk.glsl unchanged. Requires terrain_frame_vk.glsl (uT, u_dynamic* maps,
// mv_csmProject) to be included first.
//
// Budget: exactly four hardware-bilinear compare fetches (each a 2x2 PCF) per fragment, also
// across a cascade transition (two per cascade). There is no tent loop, no occluder search and no
// per-tap early-out, so the fetches are independent and issue back to back behind one wait.
// Slot P is always the near-array sampler; slot Q is the full-map sampler or the array, chosen
// once per fragment. Cascade selection, blend weight and biases are scalar math only.
//
// Composition with the cached static term: the caller still does min(static, dynamic) and the
// concrete rule; this function returns the same [0.4,1.0] visibility domain as before.

// Hardware-bilinear compare of two taps offset by +/- d along dir (in texels).
float tsrPairArray(vec2 uv, vec2 off, float layer, float z) {
    float a = texture(u_dynamicShadowArray, vec4(uv + off, layer, z));
    float b = texture(u_dynamicShadowArray, vec4(uv - off, layer, z));
    return 0.5 * (a + b);
}
float tsrPairFull(vec2 uv, vec2 off, float z) {
    float a = texture(u_dynamicFullMapShadow, vec3(uv + off, z));
    float b = texture(u_dynamicFullMapShadow, vec3(uv - off, z));
    return 0.5 * (a + b);
}
float tsrPairLegacy(vec2 uv, vec2 off, float z) {
    float a = texture(u_dynamicShadow, vec3(uv + off, z));
    float b = texture(u_dynamicShadow, vec3(uv - off, z));
    return 0.5 * (a + b);
}

// Kernel radius in texels: the same gradient-adaptive softness the tent used, mapped onto the
// two-tap half width (tent std-dev ~0.41 r; a +/-0.45 r pair plus bilinear footprint is close).
float tsrHalfWidth(float pcz) {
    float hardness = clamp(length(vec2(dFdx(pcz), dFdy(pcz))) * 180.0, 0.0, 1.0);
    float radius = clamp(max(uT.shadowSoftness, 0.5) * mix(2.4, 0.8, hardness), 0.5, 7.0);
    return 0.45 * radius;
}
float tsrCascadeBias(int kind, float nl) {
    float slope = max(1.0 - nl, 0.0);
    float tw = kind < 0 ? uT.csmFullMapTexelWorld : uT.csmCascadeTexelWorld[kind];
    return max(0.0012 * slope, 0.0004) + 1.5 * tw / max(uT.csmDepthSpan, 1.0);
}

const vec2 TSR_DIR_P = vec2(0.8944272, 0.4472136);
const vec2 TSR_DIR_Q = vec2(-0.4472136, 0.8944272);

// `sun` is the already altitude-clamped sun (lsvk_shadowSun). Returns visibility in [0.4,1].
float terrainDynamicShadow(vec3 worldPos, vec3 shadowN, vec3 sun) {
    float nl = max(dot(shadowN, sun), 0.0);
    if (nl < 0.05) return 1.0;

    if (uT.csmEnabled == 0) {
        // Legacy single dynamic map: four taps, two directions.
        vec4 ls = uT.dynamicLightSpaceMatrix * vec4(worldPos, 1.0);
        vec3 pc = ls.xyz / ls.w;
        pc.xy = pc.xy * 0.5 + 0.5;
        float hw = tsrHalfWidth(pc.z);
        if (pc.z < 0.0 || pc.z > 1.0 || pc.xy != clamp(pc.xy, 0.0, 1.0)) return 1.0;
        vec2 texel = 1.0 / vec2(textureSize(u_dynamicShadow, 0));
        float z = pc.z - max(0.005 * (1.0 - nl), 0.002);
        float v0 = tsrPairLegacy(pc.xy, TSR_DIR_P * hw * texel, z);
        float v1 = tsrPairLegacy(pc.xy, TSR_DIR_Q * hw * texel, z);
        return mix(0.4, 1.0, 0.5 * (v0 + v1));
    }

    int count = clamp(uT.csmCount, 1, MC2_VK_SHADOW_CSM_MAX);
    int a = count;
    vec3 pcA = vec3(0.0);
    for (int c = 0; c < count; ++c) {
        vec3 pc;
        if (mv_csmProject(worldPos, c, pc)) { a = c; pcA = pc; break; }
    }
    if (a == count) return 1.0;

    bool aFull = (a == count - 1);
    vec2 d2 = min(pcA.xy, 1.0 - pcA.xy);
    float edge = min(d2.x, d2.y);
    vec3 pcB = pcA;
    bool blend = false;
    float t = 1.0;
    if (!aFull && edge < 0.06) {
        vec3 p2;
        if (mv_csmProject(worldPos, a + 1, p2)) { blend = true; pcB = p2; t = smoothstep(0.0, 0.06, edge); }
    }

    // Slot P: near-array cascade (a, or the last array layer when a is the full map; weight 0).
    int kindP = max(aFull ? count - 2 : a, 0);
    // Slot Q: full map when a is the full map or the transition target is the full map;
    // otherwise the array (next cascade in a blend, orthogonal taps of the same cascade outside).
    int kindQ = aFull ? -1 : (blend ? (a + 1 == count - 1 ? -1 : a + 1) : a);
    vec3 pcQ = blend ? pcB : pcA;
    float wP = aFull ? 0.0 : (blend ? t : 0.5);

    float zP = pcA.z - tsrCascadeBias(kindP, nl);
    float zQ = pcQ.z - tsrCascadeBias(kindQ, nl);
    float hwP = tsrHalfWidth(pcA.z);
    float hwQ = tsrHalfWidth(pcQ.z);
    vec2 texP = 1.0 / vec2(textureSize(u_dynamicShadowArray, 0).xy);
    vec2 texQ = 1.0 / vec2(textureSize(u_dynamicFullMapShadow, 0));
    // Outside a blend the Q taps are the orthogonal pair of the SAME cascade.
    vec2 dirQ = blend ? TSR_DIR_P : TSR_DIR_Q;

    float vP = tsrPairArray(pcA.xy, TSR_DIR_P * hwP * texP, float(kindP), zP);
    float vQ;
    if (kindQ < 0) vQ = tsrPairFull(pcQ.xy, dirQ * hwQ * texQ, zQ);
    else           vQ = tsrPairArray(pcQ.xy, dirQ * hwQ * texP, float(kindQ), zQ);

    float vis = wP * vP + (1.0 - wP) * vQ;
    return mix(0.4, 1.0, vis);
}
#endif
