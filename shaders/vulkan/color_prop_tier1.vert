// VK-P11 STATIC-PROP COLOR — TIER-1 (parity) instanced static-prop VS.
// =============================================================================
// Identical world->clip transform to color_prop.vert (Tier-0), but ALSO forwards
// the Stuff-space normal to the FS so the GBuffer1 (MRT1) normal can be written
// bit-adjacent to the GL producer (shaders/static_prop.frag):
//   GBuffer1 = vec4(normalize(v_normal)*0.5+0.5, 0.25)
//
// ★v_normal is STUFF-SPACE, NOT GL/MC2-space. The GL producer's Stuff->MC2 frame
// swap applies to POSITIONS only; the GBuffer normal is written un-swapped (only
// the GL PBR block swaps for lighting). We therefore forward a_normal WITHOUT the
// vec3(-x, z, y) swizzle, exactly like static_prop.vert forwards v_normal.
#version 450

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uint a_localVertexID;
layout(location = 4) in uint a_aRGBLight;

layout(set = 0, binding = 0, std140) uniform ColorFrameUBO {
    mat4  worldToClipGL;   // gos_GetObjectFixBMVPMat4() — GL_FALSE (column-major load)
    float objectDepthBias; // reverse-Z surface bias (== OBJECT_DEPTH_BIAS)
    int   instBase;
    int   materialReady;   // VK-P11 MATERIAL: 1 = sample alpha-OFF albedo array
    int   materialReadyOn; // VK-P11 MATERIAL alpha-ON: 1 = sample alpha-ON albedo array
    vec4  terrainSunMC2;    // surface->sun (MC2 world); .w pad
} uFrame;

struct Instance {
    mat4  modelMatrix;       // off 0
    uint  typeID;            // off 64
    uint  firstColorOffset;  // off 68
    uint  flags;             // off 72
    uint  lightDataIndex;    // off 76
    vec4  aRGBHighlight;     // off 80
    vec4  fogRGB;            // off 96
};
layout(set = 0, binding = 1, std430) readonly buffer Instances {
    Instance i[];
} uInstances;

// VK-P11 STATIC-PROP MATERIAL: per-type group-relative albedo layer (int; -1 = no
// alpha-OFF texture -> FS keeps teal for that type). Indexed by inst.typeID.
layout(set = 0, binding = 2, std430) readonly buffer TypeAlbedoLayer {
    int layer[];
} uTypeLayer;

// VK-P11 STATIC-PROP MATERIAL alpha-ON: per-type group-relative layer into the alpha-ON
// array (-1 for non-class-1 types). Indexed by inst.typeID.
layout(set = 0, binding = 5, std430) readonly buffer TypeAlbedoLayerOn {
    int layer[];
} uTypeLayerOn;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec3 vNormalStuff;   // Stuff-space normal (un-swapped) for GBuffer1
flat layout(location = 2) out int  vLayer;    // alpha-OFF albedo array layer (-1 = none)
layout(location = 3) out vec3 vArgb;          // VERTEX-BAKED lit color (GL default model)
flat layout(location = 4) out int  vLayerOn;  // alpha-ON albedo array layer (-1 = none)

void main() {
    Instance inst = uInstances.i[uFrame.instBase + gl_InstanceIndex];
    // Row-vector * M — identical to the GL VS (Stuff std430 column-major convention).
    vec4 world_stuff = vec4(a_position, 1.0) * inst.modelMatrix;
    // POSITION: Stuff -> MC2 frame swizzle — identical to the GL VS.
    vec3 world_mc2 = vec3(-world_stuff.x, world_stuff.z, world_stuff.y);
    vec4 clip = uFrame.worldToClipGL * vec4(world_mc2, 1.0);
    clip.z += uFrame.objectDepthBias * clip.w;
    gl_Position = clip;
    vUv = a_uv;
    // NORMAL: forward in STUFF-SPACE (un-swapped). BIT-EXACT mirror of
    // static_prop.vert:352 `worldNormal = a_normal * mat3(inst.modelMatrix)`
    // (ROW-VECTOR convention — Stuff std430 col-major; `mat3(M) * a_normal` would
    // apply the INVERSE rotation and break parity). The Stuff->MC2 axis swap is NOT
    // applied to the normal (positions only), so this is the un-swapped Stuff-space
    // normal the GL GBuffer1 is written from. The FS then does normalize()*0.5+0.5.
    vec3 worldNormalStuff = a_normal * mat3(inst.modelMatrix);
    vNormalStuff = worldNormalStuff;

    // VK-P11 STATIC-PROP MATERIAL: albedo layer for this type (bounded read).
    int layer = -1;
    if (uFrame.materialReady != 0 && inst.typeID < uint(uTypeLayer.layer.length()))
        layer = uTypeLayer.layer[inst.typeID];
    vLayer = layer;
    // alpha-ON layer for this type (bounded read); -1 for non-class-1 types.
    int layerOn = -1;
    if (uFrame.materialReadyOn != 0 && inst.typeID < uint(uTypeLayerOn.layer.length()))
        layerOn = uTypeLayerOn.layer[inst.typeID];
    vLayerOn = layerOn;

    // -------------------------------------------------------------------------
    // VK-P11 VERTEX-BAKED LIGHTING (replaces the earlier computePBR path).
    // The GL DEFAULT prop lit color is NOT PBR (static_prop.frag's GGX block is
    // gated by u_pbrV1Strength, default 0.0). The GL default (static_prop.vert:438)
    // is the vertex-baked term:
    //     lit = base_light (per-vertex hot-color magic seed)
    //         + ambientTerm (ObjectLights row)
    //         + sunColor * NdotL   (NdotL vs u_terrainSunMC2, MC2 world)
    // and static_prop.frag:346 does FragColor = tex_color * vec4(lit, a).
    //
    // PARITY GAP (documented, not byte-parity): the per-type magic-color SSBO and
    // the ObjectLights UBO (ambientTerm + per-actor sunColor) are NOT fed to the
    // island. We reproduce the SHAPE of the model with the inputs we DO have:
    //   * base_light seed: the per-vertex a_aRGBLight attribute, decoded exactly as
    //     static_prop.vert:298-301 (B|G<<8|R<<16|A<<24) -> the same hot-color bytes
    //     the GL VS feeds get_base_light(). For non-magic verts this is a small/zero
    //     seed; it carries the window/hot-color emissive tint for free.
    //   * ambientTerm: a constant neutral fill (ObjectLights ambient not fed).
    //   * sunColor: white (per-actor sun color not fed); NdotL vs the terrain sun.
    // This is the ACTUAL default lighting MODEL (albedo * baked light), NOT PBR.
    //
    // ★VK-FLIP-FIELD-REPORT-1 GREEN-GLOW FIX. The prior code used the RAW per-vertex
    // bytes (r,g,b)/255 directly as the base-light seed. That is WRONG: GL feeds the
    // decoded bytes through get_base_light() (lighting.hglsl:80-170), which resolves
    // the hot-color MAGIC TAGS before they become light. In particular the
    // "Building Base Lights" tag 0xff00ff00 (Hot Green) and the "Outside Building
    // Lights" tag 0xffffff00 (Hot Yellow) contribute ONLY AT NIGHT — in daytime
    // get_base_light returns ZERO for them. The raw-byte path dumped their green
    // (g=0xff) byte straight into vArgb -> the GREEN GLOW AT BUILDING BASES the field
    // report describes. We now replicate get_base_light's DAYTIME resolution exactly
    // (isNight=false, nightFactor=0 — the same stub the GL static-prop VS uses):
    //   * hot-pink  0xffff00ff -> vec3(0x2f/255) grey  (windows painted dark grey by day)
    //   * hot-yellow/green/red/blue magic tags -> vec3(0)  (night-only, zero by day)
    //   * generic non-magic colored seed (0x00ffffff != 0) -> RGB (the .zyx reorder;
    //     our decode below already lands r/g/b in RGB order so we use it directly)
    //   * all-zero seed -> vec3(0)
    // The per-type ini HotGreenRGB/HotYellowRGB colors are NOT fed to the island, but
    // they are IRRELEVANT by day (their tags resolve to zero regardless of the ini).
    uint b8 = (a_aRGBLight >>  0) & 0xFFu;
    uint g8 = (a_aRGBLight >>  8) & 0xFFu;
    uint r8 = (a_aRGBLight >> 16) & 0xFFu;
    uint a8 = (a_aRGBLight >> 24) & 0xFFu;
    // start_v_light packing (lighting.hglsl:80): b | g<<8 | r<<16 | a<<24.
    uint startV = b8 | (g8 << 8) | (r8 << 16) | (a8 << 24);
    vec3 seed;
    if      (startV == 0xffff00ffu) seed = vec3(0x2f / 255.0);        // hot-pink -> daytime grey
    else if (startV == 0xffffff00u) seed = vec3(0.0);                 // hot-yellow -> night-only
    else if (startV == 0xff00ff00u) seed = vec3(0.0);                 // hot-green  -> night-only (the glow)
    else if (startV == 0xffff0000u) seed = vec3(0.0);                 // hot-red -> blink (0 by day)
    else if (startV == 0xff0000ffu) seed = vec3(0.0);                 // hot-blue -> blink (0 by day)
    else if ((startV & 0x00ffffffu) != 0u)
        seed = vec3(float(r8) / 255.0, float(g8) / 255.0, float(b8) / 255.0);  // generic colored seed (RGB)
    else
        seed = vec3(0.0);                                             // unlit vert (the common building case)
    seed = clamp(seed, vec3(0.0), vec3(1.0));                         // get_base_light clamps its return

    // NdotL vs terrain sun (both in MC2 world). static_prop.vert:363 maps the
    // Stuff-space normal to MC2 world as normalize(vec3(-n.x, n.z, n.y)).
    vec3  nMC2   = normalize(vec3(-worldNormalStuff.x, worldNormalStuff.z, worldNormalStuff.y));
    vec3  L      = normalize(uFrame.terrainSunMC2.xyz);
    float ndl    = max(dot(nMC2, L), 0.0);

    // AMBIENT: the GL LOD-chunk-parity prop look (launcher defaults) folds THREE
    // ambient-ish terms into v_argb that the island cannot feed byte-exact (ObjectLights
    // ambient row, V-AMBIENT-STATIC-1 hemisphere, V-IBL-STATIC-1 SH-L2). We reproduce
    // the SHAPE with the one input we DO have — the surface normal — via the SAME
    // hemisphere-ambient form GL's V-AMBIENT-STATIC-1 uses (static_prop.vert):
    //   hemi_t = 0.5 + 0.5*n.y (Stuff-space Y-up); ambient = mix(ground, sky, hemi_t).
    // This is normal-dependent (rooftops brighter than shadowed walls) instead of the
    // prior FLAT constant (0.32,0.34,0.38) that over-lit downward/side faces and
    // washed out big flat-roofed shapes (hangar/quonset) — the second field-report
    // signature. Magnitudes are the GL V-AMBIENT-STATIC-1 tints scaled up to also
    // stand in for the (unfed) ObjectLights ambient + IBL-SH fills, so shadowed sides
    // are lit but not flat-bright.
    const vec3 kAmbientSky    = vec3(0.30, 0.32, 0.38);   // upward faces (sky bounce)
    const vec3 kAmbientGround = vec3(0.14, 0.13, 0.11);   // downward faces (ground bounce)
    float hemi_t = 0.5 + 0.5 * worldNormalStuff.y;        // Stuff-space .y = up
    vec3  kAmbient = mix(kAmbientGround, kAmbientSky, clamp(hemi_t, 0.0, 1.0));
    const vec3 kSunColor = vec3(0.95, 0.93, 0.86);   // per-actor sun color not fed
    vArgb = seed + kAmbient + kSunColor * ndl;
}
