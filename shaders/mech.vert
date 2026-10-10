// shaders/mech.vert — GPU mech batcher, Slice A substrate.
// NOTE: no #version directive — makeProgram() prepends "#version 430\n".
//
// Bone transform convention: GpuMechBone stores Stuff LinearMatrix4D rows.
// mat4(row0..row3) fills GLSL COLUMNS from those args => boneT is the
// transpose of the original Stuff matrix. boneT * v == row-vec v * M_original.
// Do NOT "fix" the transpose — it is intentional and load-bearing.

// Slice B1: define MC2_STATIC_PROP_LIGHTING before lighting.hglsl so
// calc_light's no-lights early-return preserves base_light (our ambient
// floor) instead of returning vec3(1). The macro name is unfortunate
// (we're not a static prop) but behaviorally correct — the BGR/RGB
// swizzle path it gates inside get_base_light() is irrelevant because
// mech.vert never calls get_base_light(). See lighting.hglsl:131,196.
#define MC2_STATIC_PROP_LIGHTING

#include <include/lighting.hglsl>
#include <include/terrain_depth_bias.hglsl>  // TERRAIN-DEPTH-BIAS-OWNERSHIP-1: OBJECT_DEPTH_BIAS

// MECH-VIEWUNIFORMS-BLOCKBINDING-1: gated mech ViewUniforms consumer.
// MC2_USE_VIEW_UNIFORMS is injected by gos_mech_batcher.cpp ONLY when the
// mech-specific opt-in gate MC2_MECH_VIEWUNIFORMS=1 is set (NOT the global
// MC2_VIEW_UNIFORMS, and NOT default). When defined, the anonymous
// ViewUniformsBlock (binding=3) exposes u_worldToClipGL directly, so the use
// site below is unchanged. The layout(binding=3) qualifier is honored at link
// (GL 4.2+ core; same as static_prop.vert), so no explicit glUniformBlockBinding
// is needed. NO-VISUAL-CHANGE on the gated path; legacy path (gate OFF) is
// byte-identical to today. NOTE: the shader file must be deployed (synced to the
// deploy dir's shaders/) for the gated path to take effect at runtime.
#ifdef MC2_USE_VIEW_UNIFORMS
#include <include/view_uniforms.hglsl>
#endif

// Vertex attributes — 48-byte skinning-ready ABI, locked for Slice A+.
// Storage types: see GpuMechVertex in gos_mech_batcher.h.
layout(location=0) in vec3  a_position;
layout(location=1) in vec3  a_normal;
layout(location=2) in vec2  a_uv;
layout(location=3) in uvec4 a_boneIndices;   // GL_UNSIGNED_BYTE via IPointer; Slice A: .x only
layout(location=4) in vec4  a_boneWeights;   // GL_UNSIGNED_BYTE normalized; Slice A: (1,0,0,0)
layout(location=5) in vec2  a_tangentOct;    // GL_SHORT normalized; zero-fill for stock
layout(location=6) in uint  a_aRGBLight;     // TG_TypeVertex::aRGBLight, BGRA packed

// Per-instance SSBO (binding 0). The 64 B record is generated from
// RenderCore/gpu_abi/GpuMechInstance.h; do not re-declare it here.
#include <include/gpu_abi_mech_instance.gen.hglsl>
#include <include/axis.hglsl>
layout(std430, binding=0) readonly buffer InstanceBuffer {
    GpuMechInstance instances[];
};

// Per-frame bone SSBO (binding 1).
struct GpuMechBone { vec4 row0, row1, row2, row3; };
layout(std430, binding=1) readonly buffer BoneBuffer {
    GpuMechBone bones[];
};

// 'uniform uint' crashes this engine's shader compiler — use int + cast.
uniform int  u_instanceBase;
uniform int  u_materialFlags;
// u_worldToClipGL: CPU-composed kAxisSwapMC2toGL * worldToClip (row-major, GL_FALSE).
// Same upload convention as static_prop.vert / terrain_overlay.vert.
// When MC2_USE_VIEW_UNIFORMS is defined (gated mech path), u_worldToClipGL
// comes from the anonymous ViewUniformsBlock UBO above (binding=3); the legacy
// uniform is suppressed here to avoid a GLSL redeclaration error.
#ifndef MC2_USE_VIEW_UNIFORMS
uniform mat4 u_worldToClipGL;
#endif
// Slice B1: 0 = Slice A passthrough (baseLight=vec3(1.0)),
// 1 = VS-side calc_light per-vertex. Driven by MC2_GPU_MECH_LIGHTING.
// 'uniform uint' crashes the engine's shader compile (see
// memory/uniform_uint_crash.md); use int.
uniform int u_lightingMode;

// SUN-AXIS-FIX-1: 1 = hand calc_light's DIFFUSE directional branches the same
// Stuff->MC2 swap this shader already applies to the sun direction for SPECULAR
// at :279. 0 = shipped behaviour, bit-for-bit. See the derivation block in
// shaders/include/lighting.hglsl. Driven by MC2_SUN_AXIS_FIX.
//
// DELIBERATELY NOT APPLIED TO THE SPECULAR BLOCK BELOW: that consumer already
// swaps and is the one path that has been correct all along. Touching it would
// break the thing that works and double-apply the correction.
uniform int u_sunAxisMc2Fix;
// Slice C2: 0 = rigid per-bone (Slice A behavior, single boneIndices.x
// lookup), 1 = weighted multi-bone blend across all 4 bone slots. Stock
// data has boneWeights = (1,0,0,0) so the two paths are byte-identical
// for stock; the weighted path matters for imported meshes (Track D
// Assimp pipeline) that ship multi-bone weights in the existing 48B
// vertex format.
uniform int u_skinningMode;

// Varyings — FS does NOT read the SSBO; all per-instance data forwarded here.
out vec2 v_uv;
out vec4 v_litColor;
out vec4 v_highlightColor;
// Slice B2: vec4 — .rgb = fog color (engine global from g_scene.fogColor),
// .a = per-actor haze factor (0=clear, 1=fully fogged) sourced from
// inst.fogRGB.a (mech3d.cpp packs Mech3DAppearance::hazeFactor into the
// alpha byte of desc.fogARGB).
out vec4 v_fogRGB;
out vec3 v_normal;   // world-space for GBuffer1
// MECH-SPECULAR-V1: per-vertex sun direction + world position forwarded to
// the fragment shader for the specular term. All in GL world space (same
// Stuff->GL swap applied as for the vertex position and normal). Strictly
// inside the MC2_USE_VIEW_UNIFORMS guard because the specular term requires
// the camera position from the ViewUniformsBlock (binding=3). flat varyings
// must be written by every provoking vertex even when the frag-side gate
// is off; the vert-side fill is cheap (one SSBO read + short loop that
// early-exits at the first INFINITE light).
#ifdef MC2_USE_VIEW_UNIFORMS
out vec3 v_worldPos;
flat out vec3 v_mechSunDirGL;
flat out int  v_mechSunFound;
#endif
// M2.5: forward per-instance ObjectID to FS for the
// layout(location=2) out uint emission under
// #ifdef MC2_OBJECT_ID_BUFFER. `flat` qualifier MANDATORY: GL spec
// FORBIDS linear interpolation of integer varyings; without `flat`
// the program fails to link and [MECHBATCHER v1] event=shader_fail
// fires -> GPU mech path goes silently dormant.
#ifdef MC2_OBJECT_ID_BUFFER
flat out uint v_objectIdRaw;
#endif
// BT2018-MECH-MATERIAL-GAMMA-1: forward the imported-mech marker (renderFlags bit 3)
// so the frag can sRGB->linear correct imported BT skins only. flat (integer varying).
#ifdef MC2_IMPORTED_MECH_MATERIAL
flat out uint v_importedMech;
// BT2018-MECH-MATERIAL-NORMALS-1: world-space tangent for the imported-mech TBN.
// Decoded from a_tangentOct (loc 5; batcher writes a UV-gradient face tangent for
// imported types, zero for stock). Bitangent reconstructed in the frag as
// cross(N,T) (handedness w=+1 for v1; mirrored-UV islands = documented residual).
// Smooth varying: the face tangent is constant across a triangle's 3 corners, so
// interpolation is well-behaved. Only consumed when HAS_NORMAL (v_importedMech bit2).
out vec3 v_tangentGL;
#endif

void main() {
    uint instIdx = uint(u_instanceBase) + uint(gl_InstanceID);
    GpuMechInstance inst = instances[instIdx];
#ifdef MC2_IMPORTED_MECH_MATERIAL
    // bit0 = imported (renderFlags bit3), bit1 = HAS_AO (renderFlags bit4),
    // bit2 = HAS_NORMAL (renderFlags bit5). NORMALS-1 widened 3u -> 7u.
    v_importedMech = (inst.renderFlags >> 3) & 7u;
#endif

    // Bone transform: boneT is the transpose of the Stuff LinearMatrix4D.
    // boneT * vec4(pos,1) yields the world position in the Stuff/MLR camera
    // frame (.x=left, .y=elev, .z=forward).
    // Slice C2 weighted skinning branch. Stock data: weights=(1,0,0,0),
    // weighted-sum collapses to mat4(bones[idx0]) — byte-identical to
    // the rigid path. Imported meshes (Track D Assimp) can ship
    // multi-bone weights in the existing 48B vertex format.
    mat4 boneT;
    if (u_skinningMode != 0) {
        boneT = mat4(0.0);
        for (int bi = 0; bi < 4; ++bi) {
            float w = a_boneWeights[bi];
            if (w > 0.0) {
                GpuMechBone bn = bones[a_boneIndices[bi] + inst.baseBoneOffset];
                boneT += w * mat4(bn.row0, bn.row1, bn.row2, bn.row3);
            }
        }
    } else {
        // Slice A rigid-per-bone path (single bone lookup).
        GpuMechBone b = bones[a_boneIndices.x + inst.baseBoneOffset];
        boneT = mat4(b.row0, b.row1, b.row2, b.row3);
    }
    vec4 worldStuff = boneT * vec4(a_position, 1.0);

    // MC2/Stuff axis swap: terrainMVP is composed in MC2 world coords
    // (x=east, y=north, z=elev). Without this swap world.y (elev) gets
    // read as MC2 north and world.z (forward) as MC2 elev — mech ends up
    // in the sky / off-screen. Same swap as static_prop.vert (after
    // 2026-05-04 trees-in-the-sky fix).
    vec3 worldMC2 = mc2ToRender(worldStuff.xyz);
    vec3 normalStuff = mat3(boneT) * a_normal;
    vec3 worldNormal = normalize(mc2ToRender(normalStuff));

    // F1 Stage A: direct GL clip emit.
    // TERRAIN-DEPTH-BIAS-OWNERSHIP-1: mechs take OBJECT_DEPTH_BIAS so they sit on
    // top of cement/road transition tiles over true-depth terrain (tiny -> no
    // distance show-through).
    vec4 clip = u_worldToClipGL * vec4(worldMC2, 1.0);
    clip.z += OBJECT_DEPTH_BIAS * clip.w;
    gl_Position = clip;
    // No clip.w sign test — per memory/clip_w_sign_trap.md, clip.w sign
    // is not front/back in MC2 world coords; static_prop.vert omits this too.

    // Slice B1: per-vertex GPU lighting via calc_light from lighting.hglsl.
    // u_lightingMode=0 keeps Slice A's flat-white passthrough (used as a
    // fast bisect lever during soak); =1 enables calc_light. The
    // LightsData UBO at LIGHT_DATA_ATTACHMENT_SLOT=0 is bound once at
    // session start by MC_TextureManager (mclib/txmmgr.cpp:318); no
    // per-frame rebind needed. inst.lightDataIndex selects this actor's
    // ObjectLights entry, populated per-actor by
    // mechShape->CacheGpuLightData() in Mech3DAppearance::update().
    //
    // Ambient floor 0.35 prevents shadowed mechs from going pure black
    // (CPU mech path has implicit ambient via its lighting model).
    // Tunable post-soak.
    const float kAmbientFloor = 0.35;
    // Slice B+ (2026-05-09): per-actor lightsOut from inst.renderFlags
    // bit 1. Set by mech3d.cpp from OBJECT_STATUS_DESTROYED / DISABLED
    // / SHUTDOWN. Skip per-light contributions and use ambient floor
    // only — visually similar to the CPU path's
    // mechShape->SetLightsOut(true) effect.
    const uint kRenderFlagLightsOut = 0x2u;
    bool lightsOut = (inst.renderFlags & kRenderFlagLightsOut) != 0u;

    vec3 baseLight;
    if (u_lightingMode != 0) {
        if (lightsOut) {
            baseLight = vec3(kAmbientFloor);
        } else {
            vec3 base = vec3(kAmbientFloor);
            vec3 litRGB = calc_light(int(inst.lightDataIndex),
                                     worldNormal,
                                     worldMC2,
                                     base,
                                     u_sunAxisMc2Fix);
            baseLight = clamp(litRGB + inst.aRGBHighlight.rgb * inst.aRGBHighlight.a,
                              0.0, 1.0);
        }
    } else {
        // Slice A passthrough: flat white + highlight (lightsOut still
        // dims via the ambient floor for parity with the CPU path).
        if (lightsOut) {
            baseLight = vec3(kAmbientFloor);
        } else {
            baseLight = clamp(vec3(1.0) + inst.aRGBHighlight.rgb * inst.aRGBHighlight.a,
                              0.0, 1.0);
        }
    }

    v_uv             = a_uv;
    v_litColor       = vec4(baseLight, 1.0);
    v_highlightColor = inst.aRGBHighlight;
    // Slice B2: combine engine-global fog COLOR with per-actor haze
    // FACTOR. g_scene is from scene.hglsl (included transitively via
    // lighting.hglsl). inst.fogRGB.a carries hazeFactor packed by
    // mech3d.cpp; .rgb of inst.fogRGB is unused for mechs.
    v_fogRGB         = vec4(g_scene.fogColor.rgb, inst.fogRGB.a);
    v_normal         = worldNormal;

#ifdef MC2_IMPORTED_MECH_MATERIAL
    // BT2018-MECH-MATERIAL-NORMALS-1: decode the octahedral tangent (loc 5), transform
    // through the SAME bone matrix + Stuff->GL swap as the normal so T and N share a
    // frame, then Gram-Schmidt against worldNormal. Stock verts carry a_tangentOct = 0
    // which decodes to a degenerate dir; harmless because the frag only reads v_tangentGL
    // when HAS_NORMAL is set (never for stock). octDecode: standard oct -> unit vec3.
    vec2 te = a_tangentOct;                                  // GL_SHORT-normalized [-1,1]
    vec3 tLocal = vec3(te.xy, 1.0 - abs(te.x) - abs(te.y));
    if (tLocal.z < 0.0)
        tLocal.xy = (1.0 - abs(tLocal.yx)) *
                    vec2(tLocal.x >= 0.0 ? 1.0 : -1.0, tLocal.y >= 0.0 ? 1.0 : -1.0);
    tLocal = normalize(tLocal);
    vec3 tStuff = mat3(boneT) * tLocal;
    vec3 tGL    = mc2ToRender(tStuff);       // same swap as worldNormal
    // Gram-Schmidt orthonormalize against the (already normalized) world normal.
    tGL = tGL - worldNormal * dot(worldNormal, tGL);
    float tLen = length(tGL);
    v_tangentGL = (tLen > 1e-5) ? (tGL / tLen) : vec3(0.0);  // 0 -> frag falls back to v_normal
#endif

#ifdef MC2_USE_VIEW_UNIFORMS
    // MECH-SPECULAR-V1: forward world position and sun direction for the
    // per-fragment specular term in mech.frag. Sun direction convention:
    // lighting.hglsl calc_light INFINITE branch uses dot(normal, -light_dir),
    // so light_dir.xyz is negated surface->sun. We store it as-is; the frag
    // negates to recover L. Sun direction is Stuff-space in ObjectLights, so
    // apply the same Stuff->GL swap used for worldNormal above:
    //   GL = (-x, z, y) from Stuff = (x, y, z).
    // Stock MC2 missions use TG_LIGHT_INFINITEWITHFALLOFF (type 2) for the
    // sun; accept TG_LIGHT_INFINITE (type 1) as well (both are directional).
    v_worldPos     = worldMC2;
    v_mechSunDirGL = vec3(0.0);
    v_mechSunFound = 0;
    {
        ObjectLights ld_spec = light[int(inst.lightDataIndex)];
        int n_spec = min(ld_spec.numLights.x, MAX_LIGHTS_IN_WORLD);
        for (int i = 0; i < n_spec; ++i) {
            int lt = int(ld_spec.light_dir[i].w);
            if (lt == TG_LIGHT_INFINITE || lt == TG_LIGHT_INFINITEWITHFALLOFF) {
                vec3 sdStuff   = ld_spec.light_dir[i].xyz;
                // Stuff->GL axis swap (mirrors worldNormal computation above).
                v_mechSunDirGL = mc2ToRender(sdStuff);
                v_mechSunFound = 1;
                break;
            }
        }
    }
#endif
#ifdef MC2_OBJECT_ID_BUFFER
    // M2.5: forward per-instance ObjectID through the existing SSBO read;
    // no extra memory traffic. Driver handles `flat` carry-through as one
    // register write per provoking vertex.
    v_objectIdRaw    = inst.objectIdRaw;
#endif
}
