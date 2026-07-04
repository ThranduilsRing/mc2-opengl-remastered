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
    vec3 seed;
    seed.z = float((a_aRGBLight >>  0) & 0xFFu) / 255.0;  // b -> (used as-is; small tint)
    seed.y = float((a_aRGBLight >>  8) & 0xFFu) / 255.0;  // g
    seed.x = float((a_aRGBLight >> 16) & 0xFFu) / 255.0;  // r
    // NdotL vs terrain sun (both in MC2 world). static_prop.vert:363 maps the
    // Stuff-space normal to MC2 world as normalize(vec3(-n.x, n.z, n.y)).
    vec3  nMC2   = normalize(vec3(-worldNormalStuff.x, worldNormalStuff.z, worldNormalStuff.y));
    vec3  L      = normalize(uFrame.terrainSunMC2.xyz);
    float ndl    = max(dot(nMC2, L), 0.0);
    const vec3 kAmbient  = vec3(0.32, 0.34, 0.38);   // ObjectLights ambient not fed
    const vec3 kSunColor = vec3(0.95, 0.93, 0.86);   // per-actor sun color not fed
    vArgb = seed + kAmbient + kSunColor * ndl;
}
