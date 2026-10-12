// GpuStaticPropBatcher: loadProgramsIfNeeded, ring capacity, light-freshness gate and the late-register / submit telemetry.
// Declared in the class headers; shared names in gos_static_prop_batcher_internal.h.
#include "gos_static_prop_batcher_internal.h"
#include "mc2fmt.h"


namespace gos_static_prop_batcher_detail {


void loadProgramsIfNeeded() {
    if (s_programLoadTried) return;
    mc2::fmt::print_err("[GPUPROPS-DIAG] loadProgramsIfNeeded ENTER\n");
    const char* glv   = (const char*)glGetString(GL_VERSION);
    const char* glslv = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
    mc2::fmt::print_err("[GPUPROPS-DIAG] GL_VERSION={}\n", mc2::fmt::safe(glv ? glv : "(null)"));
    mc2::fmt::print_err("[GPUPROPS-DIAG] GL_SHADING_LANGUAGE_VERSION={}\n", mc2::fmt::safe(glslv ? glslv : "(null)"));
    s_programLoadTried = true;

    // Plan v3.8 Step 7.2 — env decision + extension probe MUST land between
    // the latch flip and the legacy makeProgram call. If they ran later
    // (e.g. inside finalizeGeometry's Step 5.3), the latch would already
    // have prevented the coalesce program from compiling and the second
    // makeProgram below would never fire. Pattern verified at
    // gameosmain.cpp:930.
    coalesce_resetEnvOnce();
    s_hasShaderDrawParams = (glewIsSupported("GL_ARB_shader_draw_parameters") != 0);
    // Step 12B.2 — forced-no_extension hook. After the probe but BEFORE any
    // downstream code reads s_hasShaderDrawParams, force the flag false
    // when MC2_COALESCE_FORCE_DISARM=no_extension is set. finalizeGeometry's
    // Step 5.5 then naturally routes through `event=disarmed reason=no_extension`.
    if (s_coalesceForceDisarm == CoalesceForceDisarm::NoExtension) {
        s_hasShaderDrawParams = false;
    }
    mc2::fmt::print_err("[GPUPROPS-DIAG] coalesceEnvDisabled={:d} hasShaderDrawParams={:d} forceDisarm={:d}\n", mc2::fmt::as_i32((int)s_coalesceEnvDisabled), mc2::fmt::as_i32((int)s_hasShaderDrawParams), mc2::fmt::as_i32((int)s_coalesceForceDisarm));

    // makeProgram() is the project's shader loader (see gos_postprocess.cpp
    // for existing usage). Pass the "#version 430\n" prefix explicitly — the
    // shader files must NOT contain a #version directive.
    // GLSL 430 required for std430 SSBO. gos_render.cpp now requests a GL
    // 4.3 core context (bumped from 4.0) to match.
    //
    // Plan v3.8 Step 7.1 — two prefixes. Coalesce variant requires
    // GL_ARB_shader_draw_parameters (gl_DrawIDARB / gl_BaseInstanceARB)
    // and gates legacy-only uniforms out via #define MC2_COALESCE 1.
    // M1.5 M2 plan-review fix: both prefixes are runtime std::string
    // builders so MC2_OBJECT_ID_BUFFER can append a #define conditionally.
    // GLSL preprocessor does not inherit C++ build flags
    // (memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md).
    std::string legacyPrefix = "#version 430\n";
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        legacyPrefix += "#define MC2_OBJECT_ID_BUFFER 1\n";
    }
    // F1-3D flip: MC2_USE_VIEW_UNIFORMS injected by default (kill-switch
    // pattern matching s_viewUniformsDisabled). Disabled only when
    // MC2_VIEW_UNIFORMS=0. Pre-flip F1-3B required explicit =1; that left the
    // shader compiled without the define (PBR/ViewUniforms block compiled out)
    // even though the UBO upload was already default-ON, causing all PBR
    // uniforms to resolve to -1. Gate is process-lifetime: shaders compiled
    // once at startup. Only static_prop.{vert,frag} affected.
    static const bool s_viewUniformsShaderEnabled = mc2::gate::get<"MC2_VIEW_UNIFORMS">();
    if (s_viewUniformsShaderEnabled) {
        legacyPrefix += "#define MC2_USE_VIEW_UNIFORMS 1\n";
    }

    std::string coalescePrefix =
        "#version 430\n"
        "#extension GL_ARB_shader_draw_parameters : require\n"
        "#define MC2_COALESCE 1\n";
    if (RenderWorld::IsObjectIdBufferEnabled()) {
        coalescePrefix += "#define MC2_OBJECT_ID_BUFFER 1\n";
    }
    if (s_viewUniformsShaderEnabled) {
        coalescePrefix += "#define MC2_USE_VIEW_UNIFORMS 1\n";
    }
    // STATICPROP-PBR-SLOTS: gate ORM (roughness/metallic) sampling in the
    // fragment shader. Appended to BOTH prefixes; the legacy lane has no
    // MC2_COALESCE / materialTable_, so its compile-guard is false → no effect
    // there (intended). Gate-OFF (s_ormSlotsEnabled == false) is byte-identical.
    if (s_ormSlotsEnabled) {
        legacyPrefix   += "#define MC2_STATICPROP_PBR_SLOTS 1\n";
        coalescePrefix += "#define MC2_STATICPROP_PBR_SLOTS 1\n";
    }

    // Step 7.3 — legacy program (unchanged identity / no rename).
    s_staticPropProgramObj = glsl_program::makeProgram(
        "static_prop",
        "shaders/static_prop.vert",
        "shaders/static_prop.frag",
        legacyPrefix.c_str());
    if (!s_staticPropProgramObj || !s_staticPropProgramObj->is_valid()) {
        mc2::fmt::print_err("[GPUPROPS] failed to compile/link static_prop shader pair — GPU path disabled for this session; all static props will CPU-fallback via submit()==false\n");
        s_staticPropProgramObj = nullptr;
        s_staticPropProgram    = 0;
        s_programLoadFailed    = true;
        return;
    }
    s_staticPropProgram = s_staticPropProgramObj->shp_;

    // FOLIAGE-STATICPROP-DEPTH-PREPASS-1: the depth-prepass program is built
    // LATER, after the coalesce color program below — it must use the SAME
    // shader variant (legacy vs coalesce prefix) as the program that is
    // actually bound for the static-prop color pass, or GL_EQUAL parity fails
    // (the depth it lays would derive vertex transform / alpha differently).
    // See the "depth-prepass program (variant-matched)" block at the end of
    // this function.

    // Wire GL program name into PipelineRegistry so applyPipeline() can bind it.
    // Both IDs share the same program — alpha distinction is texture-array + shader
    // discard, not a separate program object.
    RenderCore::bindProgram(RenderCore::PipelineId::StaticPropOpaque,    s_staticPropProgram);
    RenderCore::bindProgram(RenderCore::PipelineId::StaticPropAlphaTest, s_staticPropProgram);
    // VERTEXLAYOUT-AUTHORITY-1: record the fixed static-prop 40B VAO identity
    // (gos_static_prop_batcher.cpp:2310-2323) into the pipeline-key path. Both
    // ids share the VAO; layout is invariant across legacy/coalesce/depth
    // variants. Pure metadata; no GL change.
    RenderCore::recordPipelineVertexLayout(RenderCore::PipelineId::StaticPropOpaque,    RenderCore::VertexLayoutId::StaticProp40B);
    RenderCore::recordPipelineVertexLayout(RenderCore::PipelineId::StaticPropAlphaTest, RenderCore::VertexLayoutId::StaticProp40B);
    s_loc_u_parityWrite        = glGetUniformLocation(s_staticPropProgram, "u_parityWrite");
    s_loc_u_parityVertsPerType = glGetUniformLocation(s_staticPropProgram, "u_parityVertsPerType");
    s_loc_u_parityBaseVertex   = glGetUniformLocation(s_staticPropProgram, "u_parityBaseVertex");
    mc2::fmt::print_err("[GPUPROPS-DIAG] loadProgramsIfNeeded OK prog={:d} loc_parityWrite={:d} loc_parityVertsPerType={:d} loc_parityBaseVertex={:d}\n", mc2::fmt::as_u32(s_staticPropProgram), mc2::fmt::as_i32(s_loc_u_parityWrite), mc2::fmt::as_i32(s_loc_u_parityVertsPerType), mc2::fmt::as_i32(s_loc_u_parityBaseVertex));

    // Plan v3.8 Step 7.4 — populate s_locsLegacy. GLSL string literals are
    // mixed-prefix (terrainMVP has NO u_ prefix; the rest do); reproduce
    // them exactly — verified against existing flush() upload sites
    // (terrainMVP at static_prop.vert:69; u_mvp,
    // u_fogValue, u_debugAddrMode, u_maxLocalVertexID, u_materialFlags,
    // u_packetID at the corresponding upload sites in flush()).
    // The existing flush() upload sites still call glGetUniformLocation
    // inline; this cache is a forward-compat addition for the coalesce
    // branch's per-frame upload path. Step 7's "Legacy path keeps working"
    // guardrail: do NOT redirect the existing legacy uploads to read from
    // s_locsLegacy here — that's a follow-up cleanup, not a Step 7 edit.
    s_locsLegacy.terrainMVP        = glGetUniformLocation(s_staticPropProgram, "u_worldToClipGL");
    s_locsLegacy.mvp               = glGetUniformLocation(s_staticPropProgram, "u_mvp");
    s_locsLegacy.fogValue          = glGetUniformLocation(s_staticPropProgram, "u_fogValue");
    s_locsLegacy.debugAddrMode     = glGetUniformLocation(s_staticPropProgram, "u_debugAddrMode");
    s_locsLegacy.maxLocalVertexID  = glGetUniformLocation(s_staticPropProgram, "u_maxLocalVertexID");
    s_locsLegacy.materialFlags     = glGetUniformLocation(s_staticPropProgram, "u_materialFlags");
    s_locsLegacy.packetID          = glGetUniformLocation(s_staticPropProgram, "u_packetID");
    // V-AMBIENT-STATIC-1: hemisphere ambient strength uniform (default 0.0 = OFF).
    s_locsLegacy.ambientV1Strength = glGetUniformLocation(s_staticPropProgram, "u_ambientV1Strength");
    s_locsLegacy.propBaseLightDirStrength = glGetUniformLocation(s_staticPropProgram, "u_propBaseLightDirStrength");
    // V-MATERIAL-DEBUG-1: per-fragment material debug view mode (default 0 = OFF).
    s_locsLegacy.debugMaterialMode = glGetUniformLocation(s_staticPropProgram, "u_debugMaterialMode");
    s_locsLegacy.pathTint          = glGetUniformLocation(s_staticPropProgram, "u_pathTint");
    // GREYBEARD-DIFFUSE-TEST: terrain sun (raw MC2) for forced-diffuse probe.
    s_locsLegacy.terrainSunMC2     = glGetUniformLocation(s_staticPropProgram, "u_terrainSunMC2");
    s_locsLegacy.isNight            = glGetUniformLocation(s_staticPropProgram, "u_isNight");
    s_locsLegacy.nightFactor        = glGetUniformLocation(s_staticPropProgram, "u_nightFactor");
    s_locsLegacy.nightEmissiveScale = glGetUniformLocation(s_staticPropProgram, "u_nightEmissiveScale");
    s_locsLegacy.fixtureEmissiveScale = glGetUniformLocation(s_staticPropProgram, "u_fixtureEmissiveScale");
    s_locsLegacy.fixtureGlowStrength  = glGetUniformLocation(s_staticPropProgram, "u_fixtureGlowStrength");
    // TERRAIN-DECAL-FILL-1: cliff-decal shadow-side ambient floor (flag-gated in frag).
    s_locsLegacy.terrainDecalFill  = glGetUniformLocation(s_staticPropProgram, "u_terrainDecalFill");
    // TERRAIN-DECAL-COLORBLEND-1: colormap sampler + atlas UV params + blend (flag-gated in frag).
    s_locsLegacy.terrainColormap        = glGetUniformLocation(s_staticPropProgram, "u_terrainColormap");
    s_locsLegacy.decalAtlasTLX          = glGetUniformLocation(s_staticPropProgram, "u_decalAtlasTLX");
    s_locsLegacy.decalAtlasTLY          = glGetUniformLocation(s_staticPropProgram, "u_decalAtlasTLY");
    s_locsLegacy.decalAtlasOOW          = glGetUniformLocation(s_staticPropProgram, "u_decalAtlasOOW");
    s_locsLegacy.terrainDecalColorBlend = glGetUniformLocation(s_staticPropProgram, "u_terrainDecalColorBlend");
    // BUILDING-GLASS-1: dither-glass toggle (flag-gated in frag).
    s_locsLegacy.glassDither            = glGetUniformLocation(s_staticPropProgram, "u_glassDither");
    s_locsLegacy.farBackdropLit         = glGetUniformLocation(s_staticPropProgram, "u_farBackdropLit");
    s_locsLegacy.farBackdropNear        = glGetUniformLocation(s_staticPropProgram, "u_farBackdropNear");
    s_locsLegacy.farBackdropFar         = glGetUniformLocation(s_staticPropProgram, "u_farBackdropFar");
    s_locsLegacy.farBackdropStrength    = glGetUniformLocation(s_staticPropProgram, "u_farBackdropStrength");
    s_locsLegacy.farBackdropTint        = glGetUniformLocation(s_staticPropProgram, "u_farBackdropTint");
    // V-IBL-STATIC-1: SH-L2 coeffs + strength (default strength 0.0 = OFF).
    s_locsLegacy.iblSh             = glGetUniformLocation(s_staticPropProgram, "u_iblSh");
    s_locsLegacy.iblShStrength     = glGetUniformLocation(s_staticPropProgram, "u_iblShStrength");
    // V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel + power-lobe specular (default strength 0.0 = OFF).
    s_locsLegacy.pbrV1Strength     = glGetUniformLocation(s_staticPropProgram, "u_pbrV1Strength");
    s_locsLegacy.pbrV1RoughnessOverride = glGetUniformLocation(s_staticPropProgram, "u_pbrV1RoughnessOverride");
    // s_locsLegacy.drawIDBase / texArr stay -1 (coalesce-only; legacy
    // shader has no such uniforms).

    // Plan v3.8 Step 7.5 — second program, gated on extension + env. Link
    // failure of the coalesce program does NOT poison the legacy path:
    // s_staticPropProgramCoalesce stays 0; IsCoalesceEnabled() catches
    // it via the (s_staticPropProgramCoalesce == 0) check.
    if (s_hasShaderDrawParams && !s_coalesceEnvDisabled) {
        // Distinct program name — shader_builder.cpp:611-614 cache rejects
        // duplicate names across calls.
        glsl_program* coalesceObj = glsl_program::makeProgram(
            "static_prop_coalesce",
            "shaders/static_prop.vert",
            "shaders/static_prop.frag",
            coalescePrefix.c_str());
        if (coalesceObj && coalesceObj->is_valid()) {
            s_staticPropProgramCoalesce = coalesceObj->shp_;

            // Step 7.6 — populate s_locsCoalesce. Legacy-only uniforms
            // (u_materialFlags / u_maxLocalVertexID / u_packetID) are
            // removed by the MC2_COALESCE preprocessor branch in
            // static_prop.frag (Step 8.5); glGetUniformLocation returns
            // -1 for them, ProgramLocs default-init keeps them -1, and
            // Step 11.7.d's upload helper skips -1 locations.
            s_locsCoalesce.terrainMVP        = glGetUniformLocation(s_staticPropProgramCoalesce, "u_worldToClipGL");
            s_locsCoalesce.mvp               = glGetUniformLocation(s_staticPropProgramCoalesce, "u_mvp");
            s_locsCoalesce.fogValue          = glGetUniformLocation(s_staticPropProgramCoalesce, "u_fogValue");
            s_locsCoalesce.debugAddrMode     = glGetUniformLocation(s_staticPropProgramCoalesce, "u_debugAddrMode");
            s_locsCoalesce.drawIDBase        = glGetUniformLocation(s_staticPropProgramCoalesce, "u_drawIDBase");
            s_locsCoalesce.texArr            = glGetUniformLocation(s_staticPropProgramCoalesce, "u_texArr");
            s_locsCoalesce.materialGpuSample = glGetUniformLocation(s_staticPropProgramCoalesce, "u_materialGpuSample");
            // V-AMBIENT-STATIC-1: hemisphere ambient strength uniform (default 0.0 = OFF).
            s_locsCoalesce.ambientV1Strength = glGetUniformLocation(s_staticPropProgramCoalesce, "u_ambientV1Strength");
            s_locsCoalesce.propBaseLightDirStrength = glGetUniformLocation(s_staticPropProgramCoalesce, "u_propBaseLightDirStrength");
            // V-MATERIAL-DEBUG-1: per-fragment material debug view mode (default 0 = OFF).
            s_locsCoalesce.debugMaterialMode = glGetUniformLocation(s_staticPropProgramCoalesce, "u_debugMaterialMode");
            s_locsCoalesce.pathTint          = glGetUniformLocation(s_staticPropProgramCoalesce, "u_pathTint");
            // GREYBEARD-DIFFUSE-TEST: terrain sun (raw MC2) for forced-diffuse probe.
            s_locsCoalesce.terrainSunMC2     = glGetUniformLocation(s_staticPropProgramCoalesce, "u_terrainSunMC2");
            s_locsCoalesce.isNight            = glGetUniformLocation(s_staticPropProgramCoalesce, "u_isNight");
            s_locsCoalesce.nightFactor        = glGetUniformLocation(s_staticPropProgramCoalesce, "u_nightFactor");
            s_locsCoalesce.nightEmissiveScale = glGetUniformLocation(s_staticPropProgramCoalesce, "u_nightEmissiveScale");
            s_locsCoalesce.fixtureEmissiveScale = glGetUniformLocation(s_staticPropProgramCoalesce, "u_fixtureEmissiveScale");
            s_locsCoalesce.fixtureGlowStrength  = glGetUniformLocation(s_staticPropProgramCoalesce, "u_fixtureGlowStrength");
            // TERRAIN-DECAL-FILL-1: cliff-decal shadow-side ambient floor (flag-gated in frag).
            s_locsCoalesce.terrainDecalFill  = glGetUniformLocation(s_staticPropProgramCoalesce, "u_terrainDecalFill");
            // TERRAIN-DECAL-COLORBLEND-1: colormap sampler + atlas UV params + blend (flag-gated in frag).
            s_locsCoalesce.terrainColormap        = glGetUniformLocation(s_staticPropProgramCoalesce, "u_terrainColormap");
            s_locsCoalesce.decalAtlasTLX          = glGetUniformLocation(s_staticPropProgramCoalesce, "u_decalAtlasTLX");
            s_locsCoalesce.decalAtlasTLY          = glGetUniformLocation(s_staticPropProgramCoalesce, "u_decalAtlasTLY");
            s_locsCoalesce.decalAtlasOOW          = glGetUniformLocation(s_staticPropProgramCoalesce, "u_decalAtlasOOW");
            s_locsCoalesce.terrainDecalColorBlend = glGetUniformLocation(s_staticPropProgramCoalesce, "u_terrainDecalColorBlend");
            // BUILDING-GLASS-1: dither-glass toggle (flag-gated in frag).
            s_locsCoalesce.glassDither            = glGetUniformLocation(s_staticPropProgramCoalesce, "u_glassDither");
            s_locsCoalesce.farBackdropLit         = glGetUniformLocation(s_staticPropProgramCoalesce, "u_farBackdropLit");
            s_locsCoalesce.farBackdropNear        = glGetUniformLocation(s_staticPropProgramCoalesce, "u_farBackdropNear");
            s_locsCoalesce.farBackdropFar         = glGetUniformLocation(s_staticPropProgramCoalesce, "u_farBackdropFar");
            s_locsCoalesce.farBackdropStrength    = glGetUniformLocation(s_staticPropProgramCoalesce, "u_farBackdropStrength");
            s_locsCoalesce.farBackdropTint        = glGetUniformLocation(s_staticPropProgramCoalesce, "u_farBackdropTint");
            // V-IBL-STATIC-1: SH-L2 coeffs + strength (default strength 0.0 = OFF).
            s_locsCoalesce.iblSh             = glGetUniformLocation(s_staticPropProgramCoalesce, "u_iblSh");
            s_locsCoalesce.iblShStrength     = glGetUniformLocation(s_staticPropProgramCoalesce, "u_iblShStrength");
            // V-MATERIAL-PBR-2: per-vertex Schlick-Fresnel + power-lobe specular (default strength 0.0 = OFF).
            s_locsCoalesce.pbrV1Strength     = glGetUniformLocation(s_staticPropProgramCoalesce, "u_pbrV1Strength");
            s_locsCoalesce.pbrV1RoughnessOverride = glGetUniformLocation(s_staticPropProgramCoalesce, "u_pbrV1RoughnessOverride");
            // STATICPROP-MATERIAL-ORM-1: ORM sampler + sample-enable. The shader
            // does not declare these yet (separate later task) so both resolve to
            // -1 here; the ProgramLocs default-init keeps them -1 and every upload
            // site below is `if (loc >= 0)`-guarded → no-op until the shader lands.
            s_locsCoalesce.ormTexArr       = glGetUniformLocation(s_staticPropProgramCoalesce, "u_ormTexArr");
            s_locsCoalesce.ormSampleEnable = glGetUniformLocation(s_staticPropProgramCoalesce, "u_ormSampleEnable");

            // M3 fix: if both gates are ON and the uniform is absent, log an error.
            // This can only happen if the shader wasn't recompiled with v3 changes.
            // The if(loc >= 0) guard in flush() already handles -1 safely (no-op),
            // but the error makes the failure observable without a debugger.
            if (s_materialGpuEnabled && s_materialGpuSampleEnabled &&
                s_locsCoalesce.materialGpuSample < 0) {
                MC2_DIAG_FPUTS("[MATERIAL_GPU v4] ERROR uniform_missing name=u_materialGpuSample\n",
                           stderr);
            }

            // s_locsCoalesce.materialFlags / maxLocalVertexID / packetID
            // stay -1 (legacy-only; removed under MC2_COALESCE).

            // Step 7.6 (out-MAJ-5 / spec §6 sampler hygiene) — bind
            // sampler2DArray uniform u_texArr to texture unit 0 once
            // here so the per-frame draw branch only needs to bind the
            // texture handle, not re-issue glUniform1i.
            if (s_locsCoalesce.texArr >= 0) {
                // INIT-ONLY: bind sampler uniform once here so per-frame flush()
                // only needs to bind the texture handle. Not a flush-path state switch.
                glUseProgram(s_staticPropProgramCoalesce);
                glUniform1i(s_locsCoalesce.texArr, 0);
                glUseProgram(0);
            }
            // STATICPROP-MATERIAL-ORM-1: bind the ORM sampler uniform to kOrmTexUnit
            // once (loc-guarded; -1 until the shader declares u_ormTexArr).
            if (s_locsCoalesce.ormTexArr >= 0) {
                glUseProgram(s_staticPropProgramCoalesce);
                glUniform1i(s_locsCoalesce.ormTexArr, static_cast<GLint>(kOrmTexUnit));
                glUseProgram(0);
            }
            mc2::fmt::print_err("[GPUPROPS-DIAG] static_prop_coalesce program={:d} loc_drawIDBase={:d} loc_texArr={:d}\n", mc2::fmt::as_u32(s_staticPropProgramCoalesce), mc2::fmt::as_i32(s_locsCoalesce.drawIDBase), mc2::fmt::as_i32(s_locsCoalesce.texArr));

            // v1: coalesce path is active — overwrite PipelineRegistry with
            // the coalesce program so applyPipeline() binds the right handle.
            // Both IDs share the same coalesce program object (alpha distinction
            // is texture-array + shader discard, not a separate program).
            RenderCore::bindProgram(RenderCore::PipelineId::StaticPropOpaque,    s_staticPropProgramCoalesce);
            RenderCore::bindProgram(RenderCore::PipelineId::StaticPropAlphaTest, s_staticPropProgramCoalesce);
            // Re-cache parity uniform locations from the coalesce program.
            // s_loc_u_parity* were initially cached from s_staticPropProgram, but
            // applyPipeline() now binds s_staticPropProgramCoalesce — the locations
            // are program-specific, so calling glUniform1i with a location from the
            // wrong program yields GL_INVALID_OPERATION (0x502).
            s_loc_u_parityWrite        = glGetUniformLocation(s_staticPropProgramCoalesce, "u_parityWrite");
            s_loc_u_parityVertsPerType = glGetUniformLocation(s_staticPropProgramCoalesce, "u_parityVertsPerType");
            s_loc_u_parityBaseVertex   = glGetUniformLocation(s_staticPropProgramCoalesce, "u_parityBaseVertex");
        } else {
            // Compile/link failure — leave handle zero. Legacy path is
            // unaffected; finalizeGeometry's Step 5.5 will see
            // !s_hasShaderDrawParams resolve via IsCoalesceEnabled()
            // instead, but the simpler check here is that
            // s_staticPropProgramCoalesce stays 0 and IsCoalesceEnabled
            // catches it on every frame.
            mc2::fmt::print_err("[GPUPROPS] failed to compile/link static_prop_coalesce — coalesce path disabled for this session; legacy path unaffected\n");
            s_staticPropProgramCoalesce = 0;
        }
    }

    // FOLIAGE-STATICPROP-DEPTH-PREPASS-1: depth-prepass program (variant-matched).
    // Built AFTER the coalesce color program so it uses the SAME shader variant
    // (and thus the same vertex transform + alpha-flag derivation) as the program
    // that applyPipeline() will actually bind for the static-prop COLOR pass —
    // the load-bearing precondition for the later GL_EQUAL depth test.
    //
    // The active color program is the coalesce one (s_staticPropProgramCoalesce)
    // exactly when it linked above (lines just before this rebound StaticPropOpaque/
    // AlphaTest to it). In that case use coalescePrefix (with MC2_COALESCE); the
    // depth frag's #ifdef MC2_COALESCE branch then derives materialFlags/tex_color.a
    // via the PerDrawEntry SSBO + u_texArr + u_drawIDBase exactly like the color
    // frag. Otherwise the legacy program is active, so use legacyPrefix (no
    // MC2_COALESCE; u_materialFlags + u_tex path). Same MC2_OBJECT_ID_BUFFER /
    // MC2_USE_VIEW_UNIFORMS / MC2_STATICPROP_PBR_SLOTS defines are already baked
    // into both prefixes above, so this matches the color program byte-for-byte.
    //
    // This is a DISTINCT program object, so we capture its OWN u_drawIDBase
    // location (uniform locations are per-program). Link failure disables the
    // prepass only; the color path is untouched (s_staticPropDepthProgram stays 0
    // → flushDepthPrepass() no-ops).
    {
        const bool depthUsesCoalesce = (s_staticPropProgramCoalesce != 0);
        const char* depthPrefix =
            depthUsesCoalesce ? coalescePrefix.c_str() : legacyPrefix.c_str();
        glsl_program* depthObj = glsl_program::makeProgram(
            "static_prop_depth",
            "shaders/static_prop.vert",
            "shaders/static_prop_depth.frag",
            depthPrefix);
        if (depthObj && depthObj->is_valid()) {
            s_staticPropDepthProgram = depthObj->shp_;
            RenderCore::bindProgram(RenderCore::PipelineId::StaticPropDepth,
                                    s_staticPropDepthProgram);
            // VERTEXLAYOUT-AUTHORITY-1: depth-prepass reuses static_prop.vert →
            // same 40B VAO. Record it into the pipeline-key path.
            RenderCore::recordPipelineVertexLayout(RenderCore::PipelineId::StaticPropDepth,
                                                   RenderCore::VertexLayoutId::StaticProp40B);
            // u_drawIDBase exists ONLY in the coalesce variant; legacy returns -1
            // (and the prepass never uploads it on the legacy path).
            s_locsDepthCoalesce.drawIDBase =
                glGetUniformLocation(s_staticPropDepthProgram, "u_drawIDBase");
            // GL_EQUAL-parity (review CRITICAL-1/IMPORTANT-1): capture the depth
            // program's OWN u_materialGpuSample + u_debugAddrMode so the prepass
            // can upload the SAME values the color pass uploads to its program.
            // u_materialGpuSample is coalesce-only (legacy returns -1, harmless);
            // u_debugAddrMode exists in both variants. Per-program locations.
            s_locsDepthCoalesce.materialGpuSample =
                glGetUniformLocation(s_staticPropDepthProgram, "u_materialGpuSample");
            s_locsDepthCoalesce.debugAddrMode =
                glGetUniformLocation(s_staticPropDepthProgram, "u_debugAddrMode");
            // The coalesce frag samples u_texArr via a sampler2DArray uniform; bind
            // it to texture unit 0 once (matches the color program's init-only bind
            // at the coalesce block above) so the per-draw prepass loop only binds
            // the texture handle, not re-issue glUniform1i.
            if (depthUsesCoalesce) {
                const GLint depthTexArrLoc =
                    glGetUniformLocation(s_staticPropDepthProgram, "u_texArr");
                if (depthTexArrLoc >= 0) {
                    glUseProgram(s_staticPropDepthProgram);
                    glUniform1i(depthTexArrLoc, 0);
                    glUseProgram(0);
                }
            }
            mc2::fmt::print_err("[GPUPROPS-DIAG] static_prop_depth program={:d} variant={} loc_drawIDBase={:d} loc_materialGpuSample={:d} loc_debugAddrMode={:d}\n", mc2::fmt::as_u32(s_staticPropDepthProgram), mc2::fmt::safe(depthUsesCoalesce ? "coalesce" : "legacy"), mc2::fmt::as_i32(s_locsDepthCoalesce.drawIDBase), mc2::fmt::as_i32(s_locsDepthCoalesce.materialGpuSample), mc2::fmt::as_i32(s_locsDepthCoalesce.debugAddrMode));
        } else {
            mc2::fmt::print_err("[GPUPROPS] static_prop_depth program failed to compile/link — depth-prepass disabled this session (color path unaffected)\n");
            s_staticPropDepthProgram = 0;   // flushDepthPrepass() must no-op when 0
        }
    }
}

// --- Step 7.7: strengthened IsCoalesceEnabled() body (plan v3.8). ---
// Returns true only when EVERY precondition for the coalesce draw branch
// is satisfied. False on any precondition lets flush()'s gate fall back
// to the legacy per-type/per-packet loop with no other state change.
// Cheap (all field reads + a few comparisons); safe to call per-frame.
bool IsCoalesceEnabled() {
    coalesce_resetEnvOnce();  // idempotent — primary call is in loadProgramsIfNeeded
    if (s_coalesceEnvDisabled)              return false;
    if (!s_hasShaderDrawParams)             return false;
    if (!s_geometryFinalized)               return false;
    if (!s_coalesceLayoutReady)             return false;
    if (!s_coalesceEnabled)                 return false;
    if (!s_coalesceArmed)                   return false;
    if (s_staticPropProgramCoalesce == 0)   return false;
    if (s_coalesceInstanceSsbo == 0)        return false;
    if (s_perDrawSsbo == 0)                 return false;
    if (s_permutationSsbo == 0)             return false;
    if (s_alphaOffCount == 0 && s_alphaOnCount == 0) return false;
    if (gos_object_parity::IsParityCheckEnabled())   return false;
    return true;
}

// Layer B fallback: types we failed to register (logged once, fall back to CPU path).
std::unordered_map<const TG_TypeShape*, bool> s_failedTypes;

void ensureRingCapacity(size_t neededInstances, size_t neededColorEntries) {
    const bool needGrow =
        s_instanceSsbo == 0 ||
        neededInstances > s_instanceCapacity ||
        neededColorEntries > s_colorCapacity;
    if (!needGrow) return;

    // Wait for all in-flight frames before resizing.
    for (uint32_t i = 0; i < RING_FRAMES; ++i) {
        if (s_fence[i]) {
            MC2_GL_ClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }
    // VK-SHADOW-RING-OWNER-1: invalidate owner record + registry slot on destroy.
    if (s_instanceSsbo) {
        glDeleteBuffers(1, &s_instanceSsbo); s_instanceSsbo = 0; s_instanceMap = nullptr;
        s_instanceSsboOwner.backendHandle = 0;
        RenderCore::RenderResourceDesc invalid; invalid.id = RenderCore::RenderResourceId::StaticPropInstanceRingSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    if (s_colorSsbo) {
        glDeleteBuffers(1, &s_colorSsbo); s_colorSsbo = 0; s_colorMap = nullptr;
        s_colorSsboOwner.backendHandle = 0;
        RenderCore::RenderResourceDesc invalid; invalid.id = RenderCore::RenderResourceId::StaticPropColorRingSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }

    s_instanceCapacity = std::max(neededInstances,
        s_instanceCapacity ? s_instanceCapacity * 2 : INITIAL_INSTANCES_PER_FRAME);
    s_colorCapacity    = std::max(neededColorEntries,
        s_colorCapacity    ? s_colorCapacity    * 2 : INITIAL_COLORS_PER_FRAME);

    // SSBO-BIND-ALIGN: the legacy (non-coalesce) per-type draw binds at
    // slotInstByteBase = s_frameSlot * s_instanceCapacity * sizeof (and the color
    // base likewise). Those offsets must be multiples of
    // GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT or ring slots 1+ bind misaligned on
    // NVIDIA -> GL_INVALID_VALUE -> SSBO unbound -> props invisible. This is NOT
    // dormant: the coalesce path disarms on NVIDIA (the guard trips because
    // sizeof(GpuStaticPropInstance)=112 is not alignment-aligned), so the LEGACY
    // path is the active one there. Round capacity up to an alignment-multiple
    // (elements) so cap*sizeof is aligned; the alloc and the slot base both derive
    // from capacity, so this aligns every slot consistently.
    {
        const unsigned long long a = (unsigned long long)gpuSsboOffsetAlignment();
        s_instanceCapacity = (size_t)gpuAlignUp((unsigned long long)s_instanceCapacity, a);
        s_colorCapacity    = (size_t)gpuAlignUp((unsigned long long)s_colorCapacity,    a);
    }

    const GLbitfield storageFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const GLbitfield mapFlags     = storageFlags;

    // VK-SHADOW-RING-OWNER-1: now OWNED (GpuBufferOwner) — TIER2-EXCLUDED:ring removed.
    glGenBuffers(1, &s_instanceSsbo);
    s_instanceSsboOwner.backendHandle = s_instanceSsbo;  // mirror handle into owner
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_instanceSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                    static_cast<GLsizeiptr>(RING_FRAMES * s_instanceCapacity * sizeof(GpuStaticPropInstance)),
                    nullptr, storageFlags);
    s_instanceMap = MC2_GL_MapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(RING_FRAMES * s_instanceCapacity * sizeof(GpuStaticPropInstance)),
                    mapFlags);
    {
        // VK-SHADOW-RING-OWNER-1: register the slot (observe-only metadata; never read by the draw path).
        RenderCore::RenderResourceDesc d;
        d.id        = s_instanceSsboOwner.id;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = s_instanceSsboOwner.lifetime;
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = s_instanceSsboOwner.debugName;
        d.glName    = s_instanceSsbo;
        d.sizeBytes = static_cast<uint64_t>(RING_FRAMES * s_instanceCapacity * sizeof(GpuStaticPropInstance));
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    // VK-SHADOW-RING-OWNER-1: now OWNED (GpuBufferOwner) — TIER2-EXCLUDED:ring removed.
    glGenBuffers(1, &s_colorSsbo);
    s_colorSsboOwner.backendHandle = s_colorSsbo;  // mirror handle into owner
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_colorSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                    static_cast<GLsizeiptr>(RING_FRAMES * s_colorCapacity * sizeof(uint32_t)),
                    nullptr, storageFlags);
    s_colorMap = MC2_GL_MapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                    static_cast<GLsizeiptr>(RING_FRAMES * s_colorCapacity * sizeof(uint32_t)),
                    mapFlags);
    {
        RenderCore::RenderResourceDesc d;
        d.id        = s_colorSsboOwner.id;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = s_colorSsboOwner.lifetime;
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = s_colorSsboOwner.debugName;
        d.glName    = s_colorSsbo;
        d.sizeBytes = static_cast<uint64_t>(RING_FRAMES * s_colorCapacity * sizeof(uint32_t));
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (!s_instanceMap || !s_colorMap) {
        mc2::fmt::print_err("[GPUPROPS] persistent map failed; disabling GPU path\n");
        s_fatalRegistrationFailure = true;
    }
}
ObjBatcherCounters s_counters;

// LIGHTROW-FRESHNESS-1 gate. DEFAULT-ON kill-switch; MC2_STATIC_PROP_LIGHT_FRESHNESS=0
// restores the pre-slice behaviour (trust cachedGpuLightIndex_ with no frame check)
// bit-for-bit, which is the only way to grade a suspected regression against it.
bool staticPropLightFreshnessEnabled() {
    static const bool s_on = mc2::gate::get<"MC2_STATIC_PROP_LIGHT_FRESHNESS">();
    return s_on;
}
bool s_objbatcherTrace     = false;
bool s_objbatcherTraceInit = false;
bool s_atexitRegistered    = false;

// [SPOTLIGHT_REAL_TRACE v1] (E) Stage 0 / T0.1 baseline. Counts submitMultiShape
// events whose child has isSpotlight==true. Separate state (not in
// ObjBatcherCounters) so the schema stays self-contained and easy to retire.
// First-hit emits one-line stderr always-on so an operator without the env still
// sees confirmation that SpotLight_ children are observed at all.
uint64_t s_spotlightReal_window  = 0;  // resets every 600 frames
uint64_t s_spotlightReal_mono    = 0;  // monotonic since process start
bool     s_spotlightReal_firstHit = false;

std::unordered_map<std::string, uint32_t> s_lateRegisterCounts;
std::unordered_set<std::string> s_lateRegisterAllowlist;
bool s_lateRegisterAllowlistLoaded = false;

// Slice 2, Stage 2.A — late-registration recovery signal.
// Set true inside the late-reg branch of submitMultiShape; cleared to false
// at the top of every submitMultiShape call. Read by the public method
// wasLastFailureLateRegistration() so callers (Stage 2.B) can set
// needsFullBakeNextFrame on the owning actor without adding a return-value
// channel to submitMultiShape.
bool s_lastSubmitWasLateReg = false;

// Monotonic counter of times submitMultiShape returned false on the
// late-registration branch (across all frames since process start).
// Added to the [OBJBATCHER v1] summary line.
uint64_t s_late_register_recovery_skips = 0;

// Slice 2 (Stage 2.D.3) — "currently-submitting multishape" pointer used to
// gate per-leaf snapshot capture in submit() to only the sampled actor.
//
// Set at the top of submitMultiShape (after the registered-types check), used
// inside submit() to compare against the sampler's current pick, and reset to
// nullptr at the bottom of submitMultiShape so a stray submit() call from a
// non-multi path can't leak gating state. In SampledOnly arm mode, only
// submit() calls whose enclosing multishape == s_currentSampledShape will
// record an instance snapshot — the GPU parity SSBO still receives writes
// from every type's draw (those bytes are ignored on compare because the
// snapshot map only carries entries for the sampled actor's types).
const TG_MultiShape* s_currentSubmittingMulti = nullptr;

// TREE-POP-TAG-1: population of the actor whose leaves submit() is currently
// pushing. Written ONLY while MC2_TREE_DIAG_TRACE is armed (see
// submitMultiShape) and read ONLY by the [TREE_DIAG] record, so the unarmed
// hot path is unchanged. See popShortName() for why this field had to exist.
GpuStaticPropPopulation s_currentSubmitPop = GpuStaticPropPopulation::Legacy;

// 2026-05-10 actor-center fix: parent multishape's world position captured
// at the top of submitMultiShape and read by submit() when populating the
// substrate-record worldCenter. Every leaf of one multishape shares the same
// cull-side center so the GPU frustum sphere-test accepts/rejects all leaves
// together (matches the original one-record-per-actor model). Reset to zero
// at every entry to submitMultiShape.
//
// Coordinate convention: stored in raw MC2 world coords already unswapped
// from Stuff/MLR (.x=east, .y=north, .z=elev), so the consumer in submit()
// can copy directly into rec.worldCenter without re-unswapping.
float s_currentSubmittingActorCenter[3] = {0.0f, 0.0f, 0.0f};
bool  s_currentSubmittingActorCenterValid = false;

// Slice 2 (Stage 2.D.3) — late-registration EVENT counters split by
// allowlist disposition. Incremented in the late-reg branch of
// submitMultiShape() per occurrence (NOT once-per-type — the dedup
// printf logic is unchanged, but every event ticks one of these so the
// parity 600-frame summary surfaces actual frequency).
//
//   s_lateReg_allowed_events:    nodeId IS in objbatcher_late_register_allowlist.txt
//   s_lateReg_disallowed_events: nodeId is NOT (or empty)
//
// Read by GpuStaticPropBatcher::getAllowedLateRegEventCount() and
// getDisallowedLateRegEventCount(); consumed by gos_object_parity::ParityFrameTick.
uint64_t s_lateReg_allowed_events    = 0;
uint64_t s_lateReg_disallowed_events = 0;

// 2026-05-10 diag: per-typeID submit() histogram. MC2_SUBMIT_TYPEHIST=1 to
// enable. Answers H1 from
// docs/superpowers/plans/2026-05-10-substrate-coalesce-detail-and-perf-followup.md:
// "do detail-leaf typeIDs (Litwin_*, Door*, roof tiles) actually reach the
// per-leaf submit() under coalesce, or does the eligibility filter at
// submitMultiShape:2212-2231 strip them first?". Cross-reference the dump's
// non-zero typeIDs against the typeID→name table emitted by [STATIC_PROP_REG]
// at mission_load_byarr. Mirrors the [REGFLUSH_TYPEHIST v1] pattern.
bool                       s_submitTypeHistEnabled = false;
bool                       s_submitTypeHistInit    = false;
std::array<uint64_t, 1024> s_submitTypeHist{};
uint64_t                   s_submitTypeHistCalls   = 0;

void emitSubmitTypeHistDump(std::string_view trigger) {
    if (!s_submitTypeHistEnabled) return;
    // Build a typeID → name lookup by walking s_typeIndex once. n is ≤ a few
    // hundred types; quadratic walk is trivial for a 3-dump cadence.
    mc2::fmt::print_err("[SUBMIT_TYPEHIST v1] trigger={} calls={:d} non-zero buckets:\n",
        std::string(trigger).c_str(), mc2::fmt::as_u64((unsigned long long)s_submitTypeHistCalls));
    for (size_t t = 0; t < s_submitTypeHist.size(); ++t) {
        if (s_submitTypeHist[t] > 0) {
            const char* name = "<unknown>";
            for (const auto& kv : s_typeIndex) {
                if (kv.second == t && kv.first) {
                    // const_cast: getNodeId is non-const in tgl.h but returns
                    // a pointer to a char[TG_NODE_ID] field — we only read it.
                    name = const_cast<TG_TypeShape*>(kv.first)->getNodeId();
                    break;
                }
            }
            // alphaClass tells us which coalesce group this type lives in.
            // packets / indices: if a type has 0 packets or 0 indices, its
            // glMultiDrawElementsIndirect command has cmd.count=0 → no
            // triangles drawn even when instanceCount>0 (H3 of plan 2026-05-10).
            uint32_t aClass = 0xFFu, packets = 0u, indices = 0u, vertCount = 0u;
            if (t < s_types.size()) {
                const auto& ty = s_types[t];
                aClass    = ty.alphaClass;
                packets   = ty.packetCount;
                vertCount = ty.vertexCount;
                for (uint32_t p = 0; p < ty.packetCount; ++p) {
                    indices += s_packets[ty.firstPacket + p].indexCount;
                }
            }
            mc2::fmt::print_err("[SUBMIT_TYPEHIST v1] typeID={:d} name={} count={:d} alphaClass={:d} packets={:d} verts={:d} indices={:d}\n", mc2::fmt::as_size(t), mc2::fmt::safe((name && *name) ? name : "<empty>"), mc2::fmt::as_u64((unsigned long long)s_submitTypeHist[t]), mc2::fmt::as_u32(aClass), mc2::fmt::as_u32(packets), mc2::fmt::as_u32(vertCount), mc2::fmt::as_u32(indices));
        }
    }
    std::fflush(stderr);
}

void emitSubmitTypeHistAtExit() { emitSubmitTypeHistDump("atexit"); }

void emitFinalSummaryAtExit() {
    accumulateMonotonicAndMaybeEmit(/*forceEmit=*/true);
}

void accumulateMonotonicAndMaybeEmit(bool forceEmit) {
    s_counters.frame_count++;
    for (int p = 0; p < kPopCount; ++p) {
        s_counters.mono_eligible_actors_by_pop[p]     += s_counters.eligible_actors_by_pop[p];
        s_counters.mono_submitted_instances_by_pop[p] += s_counters.submitted_instances_by_pop[p];
        s_counters.mono_cpu_fallback_by_pop[p]        += s_counters.cpu_fallback_by_pop[p];
    }
    s_counters.mono_submitted_children  += s_counters.submitted_children;
    s_counters.mono_skipped_children    += s_counters.skipped_children;
    s_counters.mono_gpu_drawn_instances += s_counters.gpu_drawn_instances;

    const bool periodic = (s_counters.frame_count % 600 == 0
                           && s_counters.frame_count > 0);
    if (s_objbatcherTrace || periodic || forceEmit) {
        // Slice-1 fallback rate uses ONLY Building+Tree+Generic
        // populations (Legacy excluded — it's the prior killswitch path).
        uint64_t slice1_eligible = 0, slice1_fallback = 0;
        for (int p = 0; p <= 2; ++p) {  // Building, Tree, Generic
            slice1_eligible += s_counters.mono_eligible_actors_by_pop[p];
            slice1_fallback += s_counters.mono_cpu_fallback_by_pop[p];
        }
        const double fb_rate = (slice1_eligible > 0)
            ? (double)slice1_fallback / (double)slice1_eligible
            : 0.0;

        uint64_t total_submitted = 0;
        for (int p = 0; p < kPopCount; ++p) {
            total_submitted += s_counters.mono_submitted_instances_by_pop[p];
        }

        // COUNTER-HONESTY-1: authoritative registry census, so a reader can never
        // again conclude "no trees in this scene" from submit_trees=0.
        //
        // submit_* counts DYNAMIC submitMultiShape successes only. The moment an
        // actor registers a recipe it switches to the persistent-static replay
        // path (submitCachedInstance, ~line 9525) which deliberately does NOT tick
        // these counters. Steady state for a fully-registered scene is therefore
        // submit_trees=0 with hundreds of trees drawing every frame. That zero has
        // already been misread at least twice in committed status docs, so the
        // line now carries both the honest denominator and an explicit note.
        uint32_t popCensus[5] = {0, 0, 0, 0, 0};
        GpuStaticPropRegistry::getPopulationCensus(popCensus);
        // Which populations are instrumented AT ALL. Without this, a reader
        // cannot tell reg_recipes_generics=0 ("no generic props") from the truth
        // ("nothing ever tags Generic, so this field can only ever be 0" --
        // GenericAppearance has no registerStatic and mints no recipe). Same
        // trap as submit_trees; refusing to repeat it here.
        const uint8_t popMask = GpuStaticPropRegistry::getPopulationsEverTagged();
        char popTagged[24];
        mc2::fmt::to_buf(popTagged, "{}{}{}{}", mc2::fmt::safe((popMask & 1u) ? "B" : ""), mc2::fmt::safe((popMask & 2u) ? "T" : ""), mc2::fmt::safe((popMask & 4u) ? "G" : ""), mc2::fmt::safe((popMask & 8u) ? "L" : ""));
        if (popTagged[0] == '\0') {
            mc2::fmt::to_buf(popTagged, "NONE");
        }

        MC2_DIAGFE(stderr,
               "[OBJBATCHER v1] event=summary frames=%llu "
               "eligible_actors=%llu submitted_instances=%llu "
               "submitted_children=%llu skipped_children=%llu "
               "cpu_fallback=%llu gpu_drawn_instances=%llu "
               // STALE-COUNTER-RETIRE-1: gpu_drawn_instances is incremented ONLY on the
               // legacy/per-type path (line ~7011, CPU-side TypeRangeSsbo.instanceCount).
               // Under the live GPU-authority coalesce-v6 / C1b path the instance counts
               // are GPU-authored into the indirect buffer and never read back, so this
               // reads 0 there. submitted_instances is the honest live-path signal.
               "gpu_drawn_note=legacy_path_only_v6_uses_submitted "
               "fallback_rate=%.4f "
               // COUNTER-HONESTY-1: submit_* are DYNAMIC-PATH-ONLY. Registered
               // props replay via submitCachedInstance() which does not tick them,
               // so submit_trees=0 means "no dynamic tree submits", NEVER "no trees".
               // reg_recipes_* below is the authoritative live-registry census.
               // Two separate lies in one field name, both named here:
               //  - "instances" is a misnomer: this counter ticks ONCE per
               //    successful submitMultiShape (an ACTOR). Per-leaf instances
               //    are submitted_children. See the increment site (once, after
               //    the child loop) vs submitted_children (inside it).
               //  - it only ever sees the DYNAMIC path; registered props replay
               //    via submitCachedInstance() which does not tick it.
               "submit_note=actors_not_instances;dyn_path_only_registered_props_replay_uncounted "
               "submit_buildings=%llu submit_trees=%llu "
               "submit_generics=%llu submit_legacy=%llu "
               "reg_recipes_buildings=%u reg_recipes_trees=%u "
               "reg_recipes_generics=%u reg_recipes_legacy=%u "
               "reg_recipes_untagged=%u "
               // A zero for a population NOT listed here is not a measurement.
               "reg_pops_ever_tagged=%s "
               "reg_census_trustworthy=%s "
               "late_register_recovery_skips=%llu "
               // LIGHTROW-FRESHNESS-1. stale_light_rows_regathered > 0 means the
               // dynamic path was handed prior-frame light slots on this run --
               // the defect the slice fixes, measured rather than assumed. The
               // other two must read 0; non-zero is an invariant break.
               "stale_light_rows_regathered=%llu "
               "stale_light_rows_unresolvable=%llu "
               "light_rows_out_of_range=%llu\n",
               (unsigned long long)s_counters.frame_count,
               (unsigned long long)slice1_eligible,
               (unsigned long long)total_submitted,
               (unsigned long long)s_counters.mono_submitted_children,
               (unsigned long long)s_counters.mono_skipped_children,
               (unsigned long long)slice1_fallback,
               (unsigned long long)s_counters.mono_gpu_drawn_instances,
               fb_rate,
               (unsigned long long)s_counters.mono_submitted_instances_by_pop[0],
               (unsigned long long)s_counters.mono_submitted_instances_by_pop[1],
               (unsigned long long)s_counters.mono_submitted_instances_by_pop[2],
               (unsigned long long)s_counters.mono_submitted_instances_by_pop[3],
               popCensus[0], popCensus[1], popCensus[2], popCensus[3],
               popCensus[4], popTagged,
               // "no" the moment any live recipe carries the 0xFF unset default:
               // the breakdown is then incomplete and must not be read as a census.
               (popCensus[4] == 0u) ? "yes" : "no_untagged_recipes_present",
               (unsigned long long)s_late_register_recovery_skips,
               (unsigned long long)s_counters.mono_stale_light_row_regathered,
               (unsigned long long)s_counters.mono_stale_light_row_unresolvable,
               (unsigned long long)s_counters.mono_light_row_out_of_range);
        std::fflush(stderr);
    }

    // [SPOTLIGHT_REAL_TRACE v1] summary line, env-gated. Post-T3.2 the
    // increment site is gone (spotlight children are unconditionally skipped
    // at submit, the bit-2 flag is no longer emitted). Window/mono counters
    // therefore print 0 — that 0 IS the substitutive evidence (criterion #2:
    // zero static-prop draws contain spotlight-tagged packets). Demoted, not
    // deleted, per the Debug Instrumentation Rule.
    if (s_spotlightRealTrace && periodic) {
        mc2::fmt::print_err("[SPOTLIGHT_REAL_TRACE v1] event=summary site=static_prop_batcher frames={:d} window_spotlight_submits={:d} mono_spotlight_submits={:d}\n", mc2::fmt::as_u64((unsigned long long)s_counters.frame_count), mc2::fmt::as_u64((unsigned long long)s_spotlightReal_window), mc2::fmt::as_u64((unsigned long long)s_spotlightReal_mono));
        std::fflush(stderr);
    }
    if (periodic) {
        s_spotlightReal_window = 0;
    }

    for (int p = 0; p < kPopCount; ++p) {
        s_counters.eligible_actors_by_pop[p]     = 0;
        s_counters.submitted_instances_by_pop[p] = 0;
        s_counters.cpu_fallback_by_pop[p]        = 0;
    }
    s_counters.submitted_children  = 0;
    s_counters.skipped_children    = 0;
    s_counters.gpu_drawn_instances = 0;
}

void loadLateRegisterAllowlistOnce() {
    if (s_lateRegisterAllowlistLoaded) return;
    s_lateRegisterAllowlistLoaded = true;
    FILE* f = fopen("data/objbatcher_late_register_allowlist.txt", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p && *p != '\n' && *p != '\r' && *p != '#') ++p;
        *p = '\0';
        char* start = line;
        while (*start == ' ' || *start == '\t') ++start;
        char* end = start + strlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            --end;
        }
        *end = '\0';
        if (*start == '\0') continue;
        s_lateRegisterAllowlist.insert(start);
    }
    fclose(f);
}

}  // namespace gos_static_prop_batcher_detail
