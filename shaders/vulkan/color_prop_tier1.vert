// VK-P11 STATIC-PROP COLOR — TIER-1 (parity) instanced static-prop VS.
// =============================================================================
// Uses the same world->clip transform as the GL static-prop path and forwards
// the Stuff-space normal to the FS so the GBuffer1 (MRT1) normal can be written
// bit-adjacent to the GL producer (shaders/static_prop.frag):
//   GBuffer1 = vec4(normalize(v_normal)*0.5+0.5, 0.25)
//
// ★v_normal is STUFF-SPACE, NOT GL/MC2-space. The GL producer's Stuff->MC2 frame
// swap applies to POSITIONS only; the GBuffer normal is written un-swapped (only
// the GL PBR block swaps for lighting). We therefore forward a_normal WITHOUT the
// vec3(-x, z, y) swizzle, exactly like static_prop.vert forwards v_normal.
#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uint a_localVertexID;
layout(location = 4) in uint a_aRGBLight;

// Per-frame UBO: generated from RenderCore/gpu_abi/GpuColorFrameUBO.h.
#include "include/color_frame_ubo_vk.glsl"

// VK-PROP-AMBIENT-IBL-1: LightsData SSBO (binding 4), verbatim mirror of
// shaders/include/lighting.hglsl's ObjectLights layout (byte-identical std430/std140
// per that header's note -- every member is mat4/vec4-array + ivec4 tail). Copied
// per-frame from the live GL LightsData SSBO (binding 20) by the island. We only need
// the AMBIENT-type sum here (matches static_prop.vert:414-429's ambientTerm scan) --
// NOT the full calc_light dispatch; sun color/direction stay the island's existing
// kSunColor/terrainSunMC2 approximation (out of scope for this fix).
#include "../include/gpu_abi_object_lights.gen.hglsl"
#define MAX_LIGHTS_IN_WORLD GPU_ABI_OBJECTLIGHTS_LIGHT_TO_WORLD_LEN
#define TG_LIGHT_AMBIENT 0
layout(set = 0, binding = 4, std430) readonly buffer LightsData {
    ObjectLights light[];
} lightsData_;

// SH-L2 evaluator -- verbatim port of static_prop.vert::evalShL2 (Ramamoorthi-Hanrahan
// diffuse-irradiance reconstruction). Coefficients arrive pre-convolved (see
// RenderCore/IblShCoeffs.h header comment); do not re-apply the cosine kernel here.
vec3 evalShL2(vec3 n, vec3 sh[9]) {
    const float kSH_c1 = 0.429043;
    const float kSH_c2 = 0.511664;
    const float kSH_c3 = 0.743125;
    const float kSH_c4 = 0.886227;
    const float kSH_c5 = 0.247708;
    return
          kSH_c1 * sh[8] * (n.x * n.x - n.z * n.z)
        + kSH_c3 * sh[6] * (n.y * n.y)
        + kSH_c4 * sh[0]
        - kSH_c5 * sh[6]
        + 2.0 * kSH_c1 * sh[4] * (n.x * n.y)
        + 2.0 * kSH_c1 * sh[5] * (n.y * n.z)
        + 2.0 * kSH_c1 * sh[7] * (n.x * n.z)
        + 2.0 * kSH_c2 * sh[3] * n.x
        + 2.0 * kSH_c2 * sh[1] * n.y
        + 2.0 * kSH_c2 * sh[2] * n.z;
}

// VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: per-PACKET (not per-type) albedo layer,
// pushed fresh before each vkCmdDrawIndexed. GL's coalesce path samples texArrayLayer
// per-DRAW (PerDrawEntry), never per-type — a multi-packet type (e.g. a fence type's
// separate "post"/"arch" sub-mesh packets) can have DIFFERENT textures per packet.
// The prior per-type SSBO-by-typeID lookup forced every packet of a type to reuse
// one ("first-valid-wins") layer, so non-first packets sampled the WRONG BC7 layer —
// this produced solid-black fragments (wrong layer's black margin block, alpha>=0.5
// so no alpha-test discard caught it). -1 = no texture for this packet (teal fallback).
// FENCE-UVSCALE-1: uvScaleX/Y added alongside the layer fields (same push
// constant range, widened 8->16 bytes). GL's coalesce path crops each draw's
// sampled UV to this packet's sub-region within the group's max-dims array
// layer (static_prop.frag:206 `fract(v_uv) * vec2(uvScaleX, uvScaleY)`);
// forwarded to the FS via vUvScale (flat) so the FS applies the identical crop
// right where GL does, instead of assuming 1.0 (full layer -> narrower/gapped
// panels wherever a packet's unique texture is smaller than the group max).
layout(push_constant) uniform PacketLayerPC {
    int   packetLayerOff;
    int   packetLayerOn;
    float packetUvScaleX;
    float packetUvScaleY;
} uPacket;

#include "../include/gpu_abi_static_prop_instance.gen.hglsl"
#include "../include/axis.hglsl"
layout(set = 0, binding = 1, std430) readonly buffer Instances {
    GpuStaticPropInstance i[];
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
flat layout(location = 5) out vec2 vUvScale;  // FENCE-UVSCALE-1: this packet's uvScaleX/Y

void main() {
    GpuStaticPropInstance inst = uInstances.i[uFrame.instBase + gl_InstanceIndex];
    // Row-vector * M — identical to the GL VS (Stuff std430 column-major convention).
    vec4 world_stuff = vec4(a_position, 1.0) * inst.modelMatrix;
    // POSITION: Stuff -> MC2 frame swizzle — identical to the GL VS.
    vec3 world_mc2 = mc2ToRender(world_stuff.xyz);
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

    // VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: albedo layer now comes from the
    // per-PACKET push constant (this draw's own layer), not a per-type SSBO lookup
    // by inst.typeID. Fixes multi-packet types (e.g. fences) where sub-mesh packets
    // target different texture layers within the same group array.
    vLayer   = (uFrame.materialReady   != 0) ? uPacket.packetLayerOff : -1;
    vLayerOn = (uFrame.materialReadyOn != 0) ? uPacket.packetLayerOn  : -1;
    // FENCE-UVSCALE-1: forward this packet's uvScale unconditionally (cheap;
    // defaults to 1.0 from the CPU side when unavailable, so no gating needed).
    vUvScale = vec2(uPacket.packetUvScaleX, uPacket.packetUvScaleY);

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
    // Stuff-space normal to MC2 world as normalize(mc2ToRender(n)).
    vec3  nMC2   = normalize(mc2ToRender(worldNormalStuff));
    vec3  L      = normalize(uFrame.terrainSunMC2.xyz);
    // GL static_prop.vert:~730: ALPHA_TEST (tree cards/foliage) instances are two-sided,
    // so ndl uses abs(); solid props use max(.,0). The alpha-ON packet group IS the
    // ALPHA_TEST class (same test the FS uses). Without this, back-facing cards read dark.
    bool alphaTestPacket = (inst.flags & 4u) != 0u;   // per-instance flag bit-2, same as GL
    float ndlRaw = dot(nMC2, L);
    float ndl    = alphaTestPacket ? abs(ndlRaw) : max(ndlRaw, 0.0);

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
    // Magnitudes: the GL V-AMBIENT-STATIC-1 hemisphere tints, scaled modestly to also
    // stand in for the (unfed) ObjectLights ambient + IBL-SH@0.5 fills. Kept MODERATE:
    // an earlier higher bump (0.42/0.24) over-lit the whole-frame footprint on large
    // roofs/hedges (mid pose 49->55/ch) and washed shaded faces, so these sit just above
    // GL's raw V-AMBIENT to cover the fill gap without a flat wash. Normal-dependent
    // (rooftops brighter than shadowed walls) — matches the GL hangar-roof tint.
    // VK-FLIP-FIELD-REPORT-2 WASHOUT FIX: round-1 (9b63ab6d) tuning was still too hot on
    // near-flat-up faces (measured +45/255 mean over GL on the mc2_01 hangar roof at the
    // airport_bldgs_mid oracle pose, byte-for-byte pixel compare vs GL). Reduced the sky
    // (near-up-facing) tint by the measured delta; ground tint left as-is (shadowed/
    // downward faces were not over-lit in the same measurement).
    const vec3 kAmbientSky    = vec3(0.00, 0.02, 0.08);   // upward faces (sky bounce)
    const vec3 kAmbientGround = vec3(0.14, 0.13, 0.11);   // downward faces (ground bounce)
    float hemi_t = 0.5 + 0.5 * worldNormalStuff.y;        // Stuff-space .y = up
    vec3  kAmbientFallback = mix(kAmbientGround, kAmbientSky, clamp(hemi_t, 0.0, 1.0));
    const vec3 kSunColor = vec3(0.95, 0.93, 0.86);   // per-actor sun color not fed

    // VK-PROP-AMBIENT-IBL-1: real feed closes two of the three documented gaps above.
    //   * ambientTerm: sum of TG_LIGHT_AMBIENT rows at this instance's ObjectLights
    //     entry (identical scan to static_prop.vert:414-429), now that LightsData is
    //     copied per-frame (uFrame.lightingReady).
    //   * IBL-SH: real SH-L2 coefficients (uFrame.iblSh) evaluated with the SAME
    //     evalShL2 kernel GL uses, gated by uFrame.iblShStrength (0.0 = short-circuit,
    //     byte-identical contribution when the feature/env is off — mirrors GL's
    //     `if (u_iblShStrength > 0.0)` guard).
    // The hemisphere tint (kAmbientFallback, tuned at the WASHOUT FIX) remains as the
    // fail-soft path when LightsData hasn't been copied yet (first frame / feed miss)
    // so props never regress to flat/black ambient.
    // ADJUDICATION-1 WASHOUT FIX (2026-07-06): the real-feed branch below was the
    // UNtuned path — canonical config hits it (lightingReady=1), while the prior
    // "washout fix" only tuned the kAmbientFallback branch. Measured GL/VK hangar
    // delta was +47/255 (VK 171 vs GL 123). Two causes, both fixed here to match
    // static_prop.vert:414-438 exactly:
    //   1) sunColor was HARDCODED kSunColor (0.95,0.93,0.86); GL scans the real
    //      per-actor sun light_color from ObjectLights (first INFINITE row) and
    //      uses vec3(0) when none is found. Hardcoding added a bright sun GL never
    //      applied (~+25/255 of the delta, present even with IBL off).
    //   2) IBL: unchanged kernel/axis; the residual IBL over-contribution rides on
    //      top of the phantom sun, so removing (1) also collapses the IBL-on delta.
    // TG_LIGHT types (match calc_light / static_prop.vert): 1=INFINITE, 2=INFINITEWITHFALLOFF.
    vec3 ambientTerm = vec3(0.0);
    vec3 sunColorFed = vec3(0.0);
    bool sunFound    = false;
    if (uFrame.lightingReady != 0) {
        ObjectLights ld = lightsData_.light[int(inst.lightDataIndex)];
        int n = min(ld.numLights.x, MAX_LIGHTS_IN_WORLD);
        for (int i = 0; i < n; ++i) {
            int lt = int(ld.light_dir[i].w);
            if (lt == TG_LIGHT_AMBIENT) {
                ambientTerm += ld.light_color[i].xyz;
            } else if (!sunFound && (lt == 1 || lt == 2)) {
                sunColorFed = ld.light_color[i].xyz;
                sunFound = true;
            }
        }
    } else {
        ambientTerm = kAmbientFallback;
    }
    // Use the real scanned sun color when the feed is live (GL parity). Only the
    // fail-soft branch (no feed) falls back to the neutral kSunColor stand-in.
    vec3 sunTerm = (uFrame.lightingReady != 0) ? sunColorFed : kSunColor;

    vec3 iblTerm = vec3(0.0);
    if (uFrame.iblShStrength > 0.0) {
        vec3 shCoeffs[9];
        for (int i = 0; i < 9; ++i) shCoeffs[i] = uFrame.iblSh[i].xyz;
        // worldNormal is Stuff-space Y-up (un-swapped), matching static_prop.vert's
        // evalShL2(normalize(worldNormal)) call (SH projector's axis convention).
        iblTerm = evalShL2(normalize(worldNormalStuff), shCoeffs) * uFrame.iblShStrength;
    }

    // ADJUDICATION-1 WASHOUT FIX (round 2): GL's static_prop.vert:524 clamps its
    // final `lit` to [0,1] before handing it to the fragment (`lit = clamp(lit +
    // aRGBHighlight, 0.0, 1.0)`). This island had NO clamp at all -- vArgb was
    // passed raw (often >1.0: measured iblSh0 DC term alone, 1.71 * kSH_c4 * 0.5
    // strength, already exceeds 1.0 before adding the 0.3 ambient fill) straight
    // into the frag's `albedo * vArgb` multiply. An unclamped >1 vArgb multiplies
    // the albedo brighter than GL's clamped-at-white equivalent on every bright
    // material (hangar roofs measured GL 123.8 vs VK 171.1, +47/255) -- this is
    // the dominant remaining washout term after the sun-color fix (91051001 /
    // ADJUDICATION-1 sun fix landed +47.3->+43.5; the missing clamp is the rest).
    vArgb = clamp(seed + ambientTerm + iblTerm + sunTerm * ndl, 0.0, 1.0);
}
