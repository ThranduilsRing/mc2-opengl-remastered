// shaders/mech.frag — GPU mech batcher, Slice A substrate.
// NOTE: no #version directive — makeProgram() prepends "#version 430\n".

// render_contract.hglsl uses PREC macro for portability; the includer must
// define it. Same convention as static_prop.frag (memory: PREC defaults to
// highp in this engine).
#define PREC highp

#include <include/render_contract.hglsl>

// MECH-SPECULAR-V1: camera position for view direction. Included only on the
// ViewUniforms variant (MC2_USE_VIEW_UNIFORMS injected by gos_mech_batcher.cpp
// when MC2_MECH_VIEWUNIFORMS=1). Without this variant u_cameraWorldPos is
// unavailable and specular cannot be computed; the default/objectid variants
// are byte-identical (all 6 specular uniforms are inert at strength 0).
#ifdef MC2_USE_VIEW_UNIFORMS
#include <include/view_uniforms.hglsl>
#endif
#include <include/pbr_common.hglsl>

// [RENDER_CONTRACT]
//   Pass:           Mech
//   Color0:         RGBA, opaque (alpha-test for ALPHA_TEST_BIT materials)
//   GBuffer1:       rc_gbuffer1_screenShadowEligible
//   GBuffer2:       rc_gbuffer2_objectIdU32  // M2.5 (#ifdef MC2_OBJECT_ID_BUFFER)
//   StateContract:  depthTest=true, depthWrite=true, blend=Opaque, requiresMRT=true

in vec2 v_uv;
in vec4 v_litColor;
in vec4 v_highlightColor;
in vec4 v_fogRGB;  // .rgb = engine fog color, .a = per-actor haze factor (B2)
in vec3 v_normal;
// MECH-SPECULAR-V1: world position + sun direction (GL space) from mech.vert.
// Declared inside the MC2_USE_VIEW_UNIFORMS guard to match vert declarations;
// only the viewuniforms shader variant defines these varyings.
#ifdef MC2_USE_VIEW_UNIFORMS
in vec3       v_worldPos;
flat in vec3  v_mechSunDirGL;
flat in int   v_mechSunFound;
#endif
// BT2018-MECH-MATERIAL-GAMMA-1: imported-mech marker (renderFlags bit 3).
#ifdef MC2_IMPORTED_MECH_MATERIAL
flat in uint v_importedMech;
// Live-tunable (ImGui "BT2018 Imported Skin") imported-mech albedo knobs. Defaults
// gamma-NEUTRAL 1.0 + mild lift 1.1 (NOT full sRGB decode — legacy Blinn path is not
// linear/PBR, so pow(2.2) crushes dark BT skins). Gamma is a knob, not the fix.
uniform float u_importedGamma;        // exponent: 1.0 = neutral .. 2.2 = full sRGB decode
uniform float u_importedAlbedoScale;  // post-gamma brightness multiplier
uniform sampler2D u_mechAoTex;        // AO-1: unit 6, linear grayscale (.r), imported only
uniform float u_aoStrength;           // AO-1: 0 = no-op (== pre-AO look), 1 = full AO
// BT2018-MECH-MATERIAL-NORMALS-1: tangent-space normal map (unit 7, BC7-linear, XYZ in
// RGB). Sampled only when HAS_NORMAL (v_importedMech bit2) is set by the CPU, which
// happens iff the .ktx2 handle loaded -> missing artifact = bit clear = v_normal fallback.
uniform sampler2D u_mechNormalTex;    // NORMALS-1: unit 7
uniform float u_normalStrength;       // NORMALS-1: 0 = no-op (geometric normal), 1 = full map
in vec3 v_tangentGL;                  // NORMALS-1: world-space tangent from mech.vert
#endif

uniform sampler2D u_tex;
// Slice C2/C3/D + PBR-LAYERED-1: StandardLit GGX with two-layer triplanar material.
// Metal061B = worn substrate (units 1+2), PaintedMetal003 = paint layer (units 3+4).
// All sampled only when u_standardLitEnabled != 0 and MC2_USE_VIEW_UNIFORMS defined.
uniform sampler2D u_pbrNormalTex;      // unit 1: Metal061B NormalGL (RGB8, linear)
uniform sampler2D u_pbrOrmTex;         // unit 2: Metal061B packed ORM (R=AO G=Rough B=Metal)
uniform sampler2D u_pbrPaintNormalTex; // unit 3: PaintedMetal003 NormalGL
uniform sampler2D u_pbrPaintOrmTex;    // unit 4: PaintedMetal003 packed ORM
uniform int   u_standardLitEnabled;          // 0=Blinn-Phong, 1=GGX StandardLit (default ON)
uniform float u_pbrTileScale;               // UV tile scale (fallback path, default 4.0)
// PBR-TUNE-1: material influence knobs.
uniform float u_pbrMetallicInfluence;       // scale raw ORM metallic (default 0.15)
uniform float u_pbrRoughnessMin;            // roughness floor (default 0.45)
uniform float u_pbrRoughnessMax;            // roughness ceiling (default 0.90)
uniform float u_pbrAmbientSpecularStrength; // fake env fill for metals (default 0.25)
// PBR-LAYERED-1: paint/wear blend + triplanar.
uniform float u_pbrWearStrength;      // wear blend 0=all paint, 1=full metal reveal (default 1.0)
uniform int   u_pbrTriplanar;         // 1=world-space triplanar (default OFF)
uniform float u_pbrTriplanarScale;    // triplanar world-unit scale (default 0.2)
// PBR-IBL-MECH-1: SH-L2 image-based ambient (same coeff set as static props).
// u_iblShStrength == 0.0 -> evalShL2 skipped, falls back to hardcoded sky color.
uniform vec3  u_iblSh[9];
uniform float u_iblShStrength;
uniform int u_materialFlags;  // bit 0: ALPHA_TEST
// Slice B2: u_fogValue retained for backward compat / parity with
// static_prop convention but no longer drives the mix — per-actor
// haze is in v_fogRGB.a now. The CPU side stops uploading the
// uniform (location -1 = silent no-op for glUniform1f) but keeping
// the declaration leaves room for a global haze override later.
uniform float u_fogValue;
// Slice A debug: 0=normal, 1=solid magenta, 2=texture only, 3=light only,
// 4=normal-as-color. Wired through MC2_MECH_FRAG_DEBUG env var on the C++
// side via a uniform write at flush time.
uniform int u_debugMode;
// MECH-AMBIENT-1: gated hemisphere ambient fill strength. 0.0 = OFF (no-op,
// byte-identical to legacy). The C++ side uploads 0.0 when MC2_MECH_AMBIENT_V1
// is off, so the term below vanishes. Sky/ground hemisphere colors are fixed
// conservative constants (no new uniforms/material data).
uniform float u_mechAmbientV1Strength;
// MECH-SPECULAR-V1: conservative Blinn specular sheen, gated by
// u_mechSpecularV1Strength. 0.0 = OFF (byte-identical). Only effective when
// MC2_USE_VIEW_UNIFORMS is defined (viewuniforms shader variant); the remaining
// variants have no camera position so strength is always uploaded as 0 by the
// C++ side. Glass/cockpit heuristic uses dark-pixel (luminance + maxChannel)
// classification — no hue/blue detection. Roughness parameters set Blinn
// exponent via shininess = 2/a^2 - 2 (energy-conserving-ish). Additive white
// highlight (not albedo-tinted) — reads as a metal glint, conservative.
#ifdef MC2_USE_VIEW_UNIFORMS
uniform float u_mechSpecularV1Strength;   // 0 = OFF; C++ uploads 0 when gate off
uniform float u_mechMetalRoughness;       // base surface roughness (default 0.85)
uniform float u_mechGlassRoughness;       // glass/cockpit roughness (default 0.25)
uniform float u_mechGlassLumaThresh;      // glass if luma < this (default 0.12)
uniform float u_mechGlassMaxChanThresh;   // AND max(rgb) < this (default 0.18)
uniform int   u_mechSpecDebugMask;        // 1 = visualize cockpit mask in green/grey
// MECH-BACK-FILL-1: cool-sky fill for the shadow hemisphere in the StandardLit
// GGX path. Adds albedo * coolSkyColor * max(-NdotL,0) * strength so the dark
// side is never pitch-black. 0=OFF (legacy behaviour). Default 0.
uniform float u_mechBackFillStrength;
#endif

layout(location=0) out vec4 FragColor;
layout(location=1) out vec4 GBuffer1;
#ifdef MC2_OBJECT_ID_BUFFER
// M2.5: per-pixel mech ObjectID. Emitted to GL_COLOR_ATTACHMENT2
// (R32_UINT; M1.5 substrate). `flat in` matches mech.vert's
// `flat out uint v_objectIdRaw`. Alpha-tested fragments that
// discard() at line 56 skip this write naturally -- the attachment-2
// pixel retains the clear value (0 = Handle::invalid()), correctly
// classified as background under lookupAtPixel.
flat in uint v_objectIdRaw;
layout(location=2) out uint v_objectId;
#endif

const int ALPHA_TEST_BIT = 1;

// PBR-IBL-MECH-1: SH-L2 diffuse irradiance evaluator (Ramamoorthi-Hanrahan 2001).
// Mirror of static_prop.vert::evalShL2. n must be Y-up world space (v_normal is Y-up).
// Returns diffuse irradiance pre-convolved with the cosine lobe kernel.
// Called only when u_iblShStrength > 0.0 so the guard short-circuits to
// the fallback sky color when IBL is disabled (byte-identical default path).
vec3 evalShL2(vec3 n) {
    const float kSH_c1 = 0.429043;
    const float kSH_c2 = 0.511664;
    const float kSH_c3 = 0.743125;
    const float kSH_c4 = 0.886227;
    const float kSH_c5 = 0.247708;
    return (
          kSH_c1 * u_iblSh[8] * (n.x * n.x - n.z * n.z)
        + kSH_c3 * u_iblSh[6] * (n.y * n.y)
        + kSH_c4 * u_iblSh[0]
        - kSH_c5 * u_iblSh[6]
        + 2.0 * kSH_c1 * u_iblSh[4] * (n.x * n.y)
        + 2.0 * kSH_c1 * u_iblSh[5] * (n.y * n.z)
        + 2.0 * kSH_c1 * u_iblSh[7] * (n.x * n.z)
        + 2.0 * kSH_c2 * u_iblSh[3] * n.x
        + 2.0 * kSH_c2 * u_iblSh[1] * n.y
        + 2.0 * kSH_c2 * u_iblSh[2] * n.z
    );
}

void main() {
    // textureLod(.., 0.0) instead of texture(): AMD RX 7900 XTX
    // strict-fails auto-LOD sampling on mech paint-scheme textures even
    // with MIN_FILTER=GL_LINEAR — hardware appears to access mip 1+
    // (which is undefined / empty) and returns black. Constant UV (zero
    // derivatives) clamps LOD to base level and works; varying UV
    // doesn't. textureLod forces level 0 explicitly. Diagnosed
    // 2026-05-08 via debug-mode 7 vs 8 contrast.
    // Static_prop.frag escapes this trap because its textures have
    // a complete mip pyramid by the time it samples; mech textures
    // load with the paint-scheme cycle that doesn't always leave all
    // mip levels populated when the GPU mech batcher's flush runs.
    vec4 tex_color = textureLod(u_tex, v_uv, 0.0);

    if ((u_materialFlags & ALPHA_TEST_BIT) != 0 && tex_color.a < 0.5) {
        discard;
    }

#ifdef MC2_IMPORTED_MECH_MATERIAL
    // BT2018-MECH-MATERIAL-GAMMA-1: imported BT skins are sRGB-authored PBR albedo.
    // This legacy Blinn path samples them un-linearized, so they read ~2.2x too
    // bright/flat (the StandardLit GGX branch already does pow(rgb,2.2)). Decode
    // sRGB->linear ONCE, imported mechs only (renderFlags bit 3). Stock mechs:
    // v_importedMech == 0 -> byte-identical no-op. Alpha already handled above.
    if (v_importedMech != 0u) {
        tex_color.rgb = pow(tex_color.rgb, vec3(u_importedGamma));
        tex_color.rgb *= u_importedAlbedoScale;
    }
#endif

    vec4 c = tex_color * v_litColor;
    c.rgb += v_highlightColor.rgb * v_highlightColor.a;
#ifdef MC2_IMPORTED_MECH_MATERIAL
    // AO-1: imported BT mechs with a valid AO map (renderFlags bit4 -> v_importedMech
    // bit1). Linear grayscale, sample .r, conservative multiply. The HAS_AO bit is set
    // by the CPU ONLY when the AO handle loaded, so this never samples a missing texture
    // (white/no-op fallback = bit clear). textureLod(...,0) avoids AMD mip strict-fail.
    if ((v_importedMech & 2u) != 0u) {
        float ao = textureLod(u_mechAoTex, v_uv, 0.0).r;
        c.rgb *= mix(1.0, ao, u_aoStrength);
    }
#endif

    // BT2018-MECH-MATERIAL-NORMALS-1: the per-fragment shading normal. Defaults to the
    // geometric world normal (byte-identical to pre-NORMALS-1 for stock + AO-only mechs).
    // For imported mechs with HAS_NORMAL (v_importedMech bit2) and a non-degenerate
    // tangent, perturb it by the tangent-space normal map. Bitangent = cross(N,T) with
    // handedness w=+1 (mirrored-UV islands = documented residual). textureLod(...,0) avoids
    // AMD mip strict-fail. Mix by u_normalStrength so 0 == no-op / fallback-proven.
    vec3 Nshade = normalize(v_normal);
#ifdef MC2_IMPORTED_MECH_MATERIAL
    if ((v_importedMech & 4u) != 0u && dot(v_tangentGL, v_tangentGL) > 1e-8) {
        vec3 T  = normalize(v_tangentGL - Nshade * dot(Nshade, v_tangentGL));
        vec3 B  = cross(Nshade, T);                          // w = +1
        vec3 nt = textureLod(u_mechNormalTex, v_uv, 0.0).xyz * 2.0 - 1.0;  // BC7 XYZ
        vec3 Nmap = normalize(mat3(T, B, Nshade) * nt);
        Nshade = normalize(mix(Nshade, Nmap, u_normalStrength));
    }
#endif
    // MECH-AMBIENT-1 (gated): conservative hemisphere ambient FILL. Lifts
    // shadowed/under-lit surfaces for readability without touching the lit
    // model, PBR, materials, or team-baked albedo. Keyed on the world normal's
    // up-component (worldMC2 y = up). Multiplied by albedo so it tints with the
    // mech's own colour. u_mechAmbientV1Strength == 0.0 -> exact no-op
    // (byte-identical default path). Added before fog so it hazes consistently.
    {
        vec3  Nw    = Nshade;   // NORMALS-1: normal-mapped for imported mechs, else v_normal
        float upAmt = 0.5 + 0.5 * Nw.y;                       // 0 = down, 1 = up
        vec3  hemi  = mix(vec3(0.20, 0.18, 0.16),             // ground (warm/dark)
                          vec3(0.55, 0.60, 0.70), upAmt);     // sky (cool)
        c.rgb += tex_color.rgb * hemi * u_mechAmbientV1Strength;
    }
    // MECH-SPECULAR-V1 (gated): conservative Blinn specular metal sheen + optional
    // glass/cockpit heuristic (dark-pixel classification). Added AFTER the ambient
    // term so the specular contribution is not swallowed by ambient, and BEFORE
    // the fog mix so specular fogs correctly with the rest of the surface. Placed
    // BEFORE the debug-mode if-chain so explicit debug modes 1-9 still overwrite c.
    //
    // Byte-identical when gate off: u_mechSpecularV1Strength == 0.0 short-circuits
    // the entire block; all 6 uniforms are inert. Only effective on the
    // MC2_USE_VIEW_UNIFORMS variant (u_cameraWorldPos available); other variants
    // have no ViewUniformsBlock and the C++ side always uploads strength 0.
    //
    // Space invariant: v_normal, v_mechSunDirGL, and u_cameraWorldPos are all
    // GL world space. v_mechSunDirGL was Stuff->GL-swapped in mech.vert alongside
    // the position/normal swap. No in-frag space conversion required or performed.
#ifdef MC2_USE_VIEW_UNIFORMS
    if (u_mechSpecularV1Strength > 0.0 && v_mechSunFound != 0) {
        // Glass/cockpit heuristic: classify DARK pixels only (no hue/blue detection).
        // A pixel is "glass" if both luminance AND max channel are below threshold.
        float luma    = dot(tex_color.rgb, vec3(0.299, 0.587, 0.114));
        float maxChan = max(max(tex_color.r, tex_color.g), tex_color.b);
        bool  isGlass = (luma < u_mechGlassLumaThresh) && (maxChan < u_mechGlassMaxChanThresh);

        if (u_mechSpecDebugMask != 0) {
            // Cockpit-mask visualization: green = glass-classified, grey = metal.
            // Placed inside the strength > 0 block so it has no effect when the
            // gate is off. Debug modes 1-9 (below) will still override this if
            // u_debugMode != 0 — the debug-mode chain runs after this block.
            c = vec4(isGlass ? vec3(0.0, 1.0, 0.0) : vec3(0.15), 1.0);
        } else {
            // Blinn specular. N, V, L all in GL world space.
            // roughness -> Blinn power: shininess = 2/a^2 - 2 (Phong<->GGX mapping).
            // Clamped to [1, 2048] so extreme roughness gives a broad diffuse-like
            // lobe rather than a degenerate or negative exponent.
            float rough     = isGlass ? u_mechGlassRoughness : u_mechMetalRoughness;
            float specMul   = isGlass ? 1.6 : 1.0;  // glass cockpit slightly hotter (modest)
            vec3  N = Nshade;       // NORMALS-1: normal-mapped (imported) else v_normal; GL space
            vec3  V = normalize(u_cameraWorldPos.xyz - v_worldPos);  // GL space
            // v_mechSunDirGL is the NEGATED surface->sun direction (lighting.hglsl
            // convention: light_dir.xyz = -(surface->sun)); negate to recover L.
            vec3  L = normalize(-v_mechSunDirGL);                    // surface->sun, GL space
            vec3  H = normalize(L + V);
            float NdotL     = max(dot(N, L), 0.0);
            float NdotH     = max(dot(N, H), 0.0);
            float a         = max(rough, 0.04);
            float shininess = clamp(2.0 / (a * a) - 2.0, 1.0, 2048.0);
            // NdotL gate avoids backface specular (physically correct: no highlight
            // when surface faces away from the light).
            float spec = pow(NdotH, shininess) * NdotL;
            // Additive white highlight — not albedo-tinted — reads as a metal glint.
            c.rgb += spec * u_mechSpecularV1Strength * specMul;
        }
    }
#endif  // MC2_USE_VIEW_UNIFORMS

    // GBuffer normal; updated to PBR detail normal when StandardLit active.
    vec3 N_gbuf = Nshade;   // NORMALS-1: normal-mapped (imported) else v_normal

#ifdef MC2_USE_VIEW_UNIFORMS
    // Slice C2/C3/D: StandardLit GGX PBR surface-detail layer.
    // Gated by u_standardLitEnabled (MC2_STANDARD_LIT_V1=1) and sun presence.
    // Replaces the Blinn-Phong c.rgb with Cook-Torrance GGX result.
    //
    // GLSL-UB-AUDIT-2 / UB2-02: the texture()+dFdx/dFdy sampling below is gated
    // ONLY by u_standardLitEnabled (a uniform → dynamically-uniform control flow,
    // so derivatives/implicit-LOD are well-defined). It must NOT be gated by
    // v_mechSunFound, which is a `flat in int` that differs between triangles —
    // a 2x2 quad straddling a triangle edge would diverge and the texture LOD /
    // derivatives become undefined (vendor-divergent: bites NVIDIA). The
    // sun-dependent APPLICATION (N_gbuf, lighting, c) stays under the inner
    // v_mechSunFound guard, so output is byte-identical: when v_mechSunFound==0
    // the samples are computed but discarded.
    if (u_standardLitEnabled != 0) {
        vec3 N_vtx = normalize(v_normal);

        // PBR-LAYERED-1: sample both material layers via triplanar or UV.
        // Metal061B (units 1+2) = worn metal substrate.
        // PaintedMetal003 (units 3+4) = paint layer; metalness encodes wear.
        vec3 orm61;    // Metal061B ORM
        vec3 N_61;     // Metal061B world-space normal
        vec3 ormPaint; // PaintedMetal003 ORM
        vec3 N_paint;  // PaintedMetal003 world-space normal

        if (u_pbrTriplanar != 0) {
            float ts = u_pbrTriplanarScale;
            orm61    = sampleTriplanar      (u_pbrOrmTex,        v_worldPos, N_vtx, ts);
            N_61     = sampleTriplanarNormal(u_pbrNormalTex,     v_worldPos, N_vtx, ts);
            ormPaint = sampleTriplanar      (u_pbrPaintOrmTex,   v_worldPos, N_vtx, ts);
            N_paint  = sampleTriplanarNormal(u_pbrPaintNormalTex, v_worldPos, N_vtx, ts);
        } else {
            vec2 pbrUV = v_uv * u_pbrTileScale;
            orm61    = texture(u_pbrOrmTex,       pbrUV).rgb;
            N_61     = applyPbrNormal(N_vtx, v_worldPos, v_uv, u_pbrNormalTex,     u_pbrTileScale, 1.0);
            ormPaint = texture(u_pbrPaintOrmTex,  pbrUV).rgb;
            N_paint  = applyPbrNormal(N_vtx, v_worldPos, v_uv, u_pbrPaintNormalTex, u_pbrTileScale, 1.0);
        }

        // Wear blend: PaintedMetal003 metalness drives where paint is thin/worn.
        // wornMask=0 -> all paint, wornMask=1 -> full metal substrate exposed.
        float wornMask  = clamp(ormPaint.b * u_pbrWearStrength, 0.0, 1.0);

        float ao        = mix(ormPaint.r, orm61.r, wornMask);
        float roughness = clamp(mix(ormPaint.g, orm61.g, wornMask), u_pbrRoughnessMin, u_pbrRoughnessMax);
        float metallic  = mix(ormPaint.b, orm61.b, wornMask) * u_pbrMetallicInfluence;
        vec3  N_pbr     = normalize(mix(N_paint, N_61, wornMask));

        // UB2-02: sun-dependent application only. All texture sampling +
        // derivatives above this point ran in uniform control flow; nothing
        // below samples a texture with implicit LOD, so this inner branch on the
        // flat varying v_mechSunFound is safe.
        if (v_mechSunFound != 0) {
        N_gbuf = N_pbr;

        vec3 V = normalize(u_cameraWorldPos.xyz - v_worldPos);
        vec3 L = normalize(-v_mechSunDirGL);
        vec3 albedo = pow(tex_color.rgb, vec3(2.2));

        vec3 lightColor   = vec3(2.0, 1.9, 1.7);
        // PBR-IBL-MECH-1: SH-L2 ambient from HDRI sky. Replaces the flat
        // sky-color fill with directional irradiance from the loaded HDRI.
        // Falls back to the conservative sky color when IBL is disabled.
        vec3 ambientColor = (u_iblShStrength > 0.0)
            ? max(evalShL2(N_pbr) * u_iblShStrength, vec3(0.0))
            : vec3(0.4, 0.45, 0.5);

        StandardLitInput si;
        si.albedo       = albedo;
        si.N            = N_pbr;
        si.V            = V;
        si.L            = L;
        si.lightColor   = lightColor;
        si.ambientColor = ambientColor;
        si.roughness               = roughness;
        si.metallic                = metallic;
        si.ao                      = ao;
        si.ambientSpecularStrength = u_pbrAmbientSpecularStrength;

        vec3 pbrLit = StandardLit(si);
        pbrLit += v_highlightColor.rgb * v_highlightColor.a;

        // MECH-BACK-FILL-1: fills shadow hemisphere with cool-sky light so the
        // dark side isn't pitch-black. Keyed to the anti-sun direction (backFace=1
        // directly away from sun, 0 at terminator/lit side). Cool blue-sky tint
        // contrasts the warm direct light; no AO applied (this is directional fill).
        if (u_mechBackFillStrength > 0.0) {
            float backFace = max(-dot(N_pbr, L), 0.0);
            pbrLit += albedo * vec3(0.38, 0.47, 0.62) * backFace * u_mechBackFillStrength;
        }

        c = vec4(pbrLit, tex_color.a);

        // PBR debug modes 10-15 (overridden by standard modes 1-9 if also set).
        if      (u_debugMode == 10) c = vec4(roughness, roughness, roughness, 1.0);
        else if (u_debugMode == 11) c = vec4(metallic,  metallic,  metallic,  1.0);
        else if (u_debugMode == 12) c = vec4(N_pbr * 0.5 + 0.5, 1.0);
        else if (u_debugMode == 13) c = vec4(ormPaint.ggg, 1.0);          // paint roughness
        else if (u_debugMode == 14) c = vec4(N_paint * 0.5 + 0.5, 1.0);  // paint normal
        else if (u_debugMode == 15) c = vec4(wornMask, wornMask, wornMask, 1.0); // wear mask
        }  // UB2-02 inner guard: if (v_mechSunFound != 0)
    }
#endif  // MC2_USE_VIEW_UNIFORMS

    // Slice B2: per-actor haze. v_fogRGB.a=0 → clear, =1 → fully fogged.
    c.rgb  = mix(c.rgb, v_fogRGB.rgb, v_fogRGB.a);

    // Debug overrides (MC2_MECH_FRAG_DEBUG=N).
    if      (u_debugMode == 1) c = vec4(1.0, 0.0, 1.0, 1.0);                    // solid magenta
    else if (u_debugMode == 2) c = vec4(tex_color.rgb, 1.0);                    // texture only
    else if (u_debugMode == 3) c = vec4(v_litColor.rgb, 1.0);                   // light only
    else if (u_debugMode == 4) c = vec4(normalize(v_normal) * 0.5 + 0.5, 1.0);  // normal-as-color
    else if (u_debugMode == 5) c = vec4(fract(v_uv.x), fract(v_uv.y), 0.0, 1.0); // UV visualizer
    else if (u_debugMode == 6) c = vec4(tex_color.aaa, 1.0);                    // texture alpha as grey
    else if (u_debugMode == 7) c = vec4(texture(u_tex, vec2(0.5, 0.5)).rgb, 1.0); // sample at hardcoded UV
    else if (u_debugMode == 8) c = vec4(textureLod(u_tex, v_uv, 0.0).rgb, 1.0);  // explicit LOD 0 sample
    else if (u_debugMode == 9) c = vec4(v_litColor.rgb / max(textureLod(u_tex, v_uv, 0.0).rgb, vec3(0.001)), 1.0); // lighting / texture (texture-inversion overlay)
    // DEBUG-VIEW-SWEEP-1: shared 50-series full-scene views (see gos_terrain.frag).
    else if (u_debugMode == 50) c = vec4(vec3(clamp((gl_FragCoord.z - 0.85) / 0.15, 0.0, 1.0)), 1.0); // normalized depth grayscale
    else if (u_debugMode == 51) c = vec4(vec3(0.12), 1.0);                          // overdraw proxy (one flat layer)
    else if (u_debugMode == 52) c = vec4(0.0, 0.0, 0.0, 1.0);                       // chunk-tint view: mechs unsupported -> black

    FragColor = c;
    GBuffer1  = rc_gbuffer1_screenShadowEligible(N_gbuf);
#ifdef MC2_OBJECT_ID_BUFFER
    // M2.5: emit per-pixel RenderObjectHandle.raw(). Debug-mode pixels
    // (u_debugMode 1..9 at lines 65-73) DO NOT discard; they still emit
    // a valid handle, which is the correct substrate behavior (a
    // lookupAtPixel on a debug-color pixel returns the actor's handle).
    v_objectId = v_objectIdRaw;
#endif
}
