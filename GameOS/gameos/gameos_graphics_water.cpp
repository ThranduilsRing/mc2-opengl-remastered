// gameos_graphics: water tuning state and accessors, the water bridge and gosRenderer::renderWaterFastPath.
// Declared in the class headers; shared names in gameos_graphics_material_internal.h.
#include "gameos_graphics_material_internal.h"
#include "mdi_submit.h"                            // MDI-SUBMISSION-SCAFFOLD-1: this piece issues indirect draws





// WaterPerCmd — per-draw data for glMultiDrawArraysIndirect, indexed by gl_DrawID.
// 32 B std430-aligned; lockstep with gos_terrain_water_fast_mdi.vert binding 7.
struct WaterPerCmd {
    uint32_t textureSlot;   // reserved; texture unit selection driven by o_isWater in FS
    int32_t  isWater;       // 1 = base, 2 = detail (matches legacy isWater uniform)
    int32_t  detailMode;    // 0 = base, 1 = detail
    float    uvScale;
    float    uvOffsetX;
    float    uvOffsetY;
    uint32_t pad0_;
    uint32_t pad1_;
};
static_assert(sizeof(WaterPerCmd) == 32, "WaterPerCmd must be 32 B");

// WATER-DEBUG-VIEWS-1: fragment/material-space debug mode for the MDI water FS
// (gos_terrain_water_mdi.frag uniform u_waterDebugMode). Sentinel -1 = uninit:
// resolve once from MC2_WATER_DEBUG_MODE. ImGui (EditorInspector) may overwrite
// with a live value (>=0). Distinct from the VS geometry-space debugMode latch
// (MC2_RENDER_WATER_FASTPATH_DEBUG). 0=Final default.
int g_waterFsDebugMode = -1;
int gos_GetWaterFsDebugMode()
{
    if (g_waterFsDebugMode < 0) {
        g_waterFsDebugMode = mc2::gate::get<"MC2_WATER_DEBUG_MODE">();
    }
    return g_waterFsDebugMode;
}
void gos_SetWaterFsDebugMode(int m) { g_waterFsDebugMode = (m < 0) ? 0 : m; }

// WATER-TUNING-UI-1: runtime-tunable MDI water material params. Defaults MUST
// match the former compile-time consts in gos_terrain_water_mdi.frag EXACTLY
// (byte-identical default render). Uploaded each frame in the MDI bind block;
// driven live by Graphics Options > Water. Only the MDI FS consumes these.
float g_waterAbsorptionDensity = 0.022f;
float g_waterMaxAlpha          = 0.87f;
float g_waterRippleGain        = 0.22f;
float g_waterGlintGain         = 0.30f;
float g_waterDeepColor[3]      = { 0.03f, 0.13f, 0.20f };
float g_waterShallowColor[3]   = { 0.22f, 0.45f, 0.38f };

float gos_GetWaterAbsorptionDensity()       { return g_waterAbsorptionDensity; }
void  gos_SetWaterAbsorptionDensity(float v){ g_waterAbsorptionDensity = v; }
float gos_GetWaterMaxAlpha()                { return g_waterMaxAlpha; }
void  gos_SetWaterMaxAlpha(float v)         { g_waterMaxAlpha = v; }
float gos_GetWaterRippleGain()              { return g_waterRippleGain; }
void  gos_SetWaterRippleGain(float v)       { g_waterRippleGain = v; }
float gos_GetWaterGlintGain()               { return g_waterGlintGain; }
void  gos_SetWaterGlintGain(float v)        { g_waterGlintGain = v; }
void  gos_GetWaterDeepColor(float* rgb)     { rgb[0]=g_waterDeepColor[0]; rgb[1]=g_waterDeepColor[1]; rgb[2]=g_waterDeepColor[2]; }
void  gos_SetWaterDeepColor(float r,float g,float b)    { g_waterDeepColor[0]=r; g_waterDeepColor[1]=g; g_waterDeepColor[2]=b; }
void  gos_GetWaterShallowColor(float* rgb)  { rgb[0]=g_waterShallowColor[0]; rgb[1]=g_waterShallowColor[1]; rgb[2]=g_waterShallowColor[2]; }
void  gos_SetWaterShallowColor(float r,float g,float b) { g_waterShallowColor[0]=r; g_waterShallowColor[1]=g; g_waterShallowColor[2]=b; }

// WATER-VISUAL-FIRST-SLICE: gated camera-INDEPENDENT sky/horizon tint. Strength
// default 0.0 = exact no-op (byte-identical). Env MC2_WATER_SKYTINT=1 bumps the
// default to a small value for quick A/B; the ImGui slider is authoritative once
// touched. Sentinel -1 on strength = uninit (resolve env once). NOT fresnel /
// reflection (those stay shelved per the 2026-05-17 camera-independence ruling).
float g_waterSkyTintStrength = -1.0f;
float g_waterSkyTintColor[3] = { 0.55f, 0.70f, 0.85f };  // soft sky-blue/horizon
float gos_GetWaterSkyTintStrength()
{
    if (g_waterSkyTintStrength < 0.0f) {
        g_waterSkyTintStrength = mc2::gate::get<"MC2_WATER_SKYTINT">() ? 0.15f : 0.0f;
    }
    return g_waterSkyTintStrength;
}
void  gos_SetWaterSkyTintStrength(float v) { g_waterSkyTintStrength = (v < 0.0f) ? 0.0f : v; }
void  gos_GetWaterSkyTintColor(float* rgb) { rgb[0]=g_waterSkyTintColor[0]; rgb[1]=g_waterSkyTintColor[1]; rgb[2]=g_waterSkyTintColor[2]; }
void  gos_SetWaterSkyTintColor(float r,float g,float b) { g_waterSkyTintColor[0]=r; g_waterSkyTintColor[1]=g; g_waterSkyTintColor[2]=b; }

// WATER-SKY-REFLECTION-1: gated camera-DEPENDENT SH-L2 sky reflection on the MDI
// water FS. Default 0.12 = un-gated (sky reflection is ON by default as of
// WATER-SUN-SPEC-1; MC2_WATER_REFLECTION=0 kills it back to 0.0 byte-identical).
// Env MC2_WATER_REFLECTION=0 -> 0.0 no-op; =1 or unset -> 0.12 default.
// Slider authoritative once touched. Sentinel -1 = uninit (resolve env once).
float g_waterReflStrength = -1.0f;
float gos_GetWaterReflStrength()
{
    if (g_waterReflStrength < 0.0f) {
        // WATER-REFL-DEFAULT-ON: default ON (1.5); MC2_WATER_REFLECTION=0 kills it.
        // Higher default exposes the HDRI specular contribution at gameplay camera.
        const bool reflOff = !mc2::gate::get<"MC2_WATER_REFLECTION">();
        g_waterReflStrength = reflOff ? 0.0f : 1.5f;
    }
    return g_waterReflStrength;
}
void  gos_SetWaterReflStrength(float v) { g_waterReflStrength = (v < 0.0f) ? 0.0f : v; }
// Whether reflection is enabled (default: ON). Env MC2_WATER_REFLECTION=0 kills it.
// Resolved live each call (cheap). UI uses this to toggle gate-state label.
int   gos_GetWaterReflectionGate()
{
    return mc2::gate::get<"MC2_WATER_REFLECTION">();  // default ON; =0 disables
}

// WATER-REFLECTION-SAMPLE-1: strength of the terrain reflection RT blended OVER
// the SH sky in the MDI water FS. Gate MC2_WATER_REFLECTION_RT (same env that
// drives the C1 RT fill pass). Default 0 = no RT blend -> SH sky only ->
// byte-identical to Phase A. Sentinel -1 = uninit (resolve env once).
float g_waterRtStrength = -1.0f;
float gos_GetWaterRtStrength()
{
    if (g_waterRtStrength < 0.0f) {
        g_waterRtStrength = mc2::gate::get<"MC2_WATER_REFLECTION_RT">() ? 0.85f : 0.0f;
    }
    return g_waterRtStrength;
}
void gos_SetWaterRtStrength(float v) { g_waterRtStrength = (v < 0.0f) ? 0.0f : v; }
int  gos_GetWaterReflectionRtGate()
{
    return mc2::gate::get<"MC2_WATER_REFLECTION_RT">();
}

// WATER-SUN-SPEC-2 (repairs WATER-SUN-SPEC-1, c50e99a6). Blinn-Phong sun
// specular intensity on the MDI water FS. Default 0.0 = exact no-op.
//
// WHAT WAS WRONG FOR THE WHOLE LIFE OF WATER-SUN-SPEC-1, and why this comment
// block is long: the value below was uploaded to "u_waterSpecIntensity" every
// frame (see the MDI bind block), and that uniform WAS NEVER DECLARED IN ANY
// SHADER IN THIS REPO'S HISTORY -- `git log -S"u_waterSpecIntensity" --all --
// 'shaders/*'` returns zero commits. Same for the "terrainLightDir" upload two
// lines above it. glGetUniformLocation returned -1 for both and the upload
// helper skips loc < 0 silently, so this gate was arithmetically incapable of
// changing a pixel at any value on any mission. WATER-SUN-SPEC-2 declares and
// consumes both in shaders/gos_terrain_water_mdi.frag; do not delete either
// upload without deleting the shader term with it.
//
// The previous comment here also claimed "the ImGui slider is authoritative
// once touched". There is no slider: gos_SetWaterSpecIntensity() had ZERO
// callers anywhere in the tree and has been deleted, and grepping the whole
// repo case-insensitively for "shine" finds no UI control. If a Graphics
// Options control is wanted later, add the setter back WITH its caller.
//
// The value is the additive peak of the specular lobe in linear light, before
// fog. Recommended 0.2-0.5; 1.0 is an exaggerated value that makes the
// mechanism unmistakable when confirming it works. Sentinel -1 = uninit.
float g_waterSpecIntensity = -1.0f;
float gos_GetWaterSpecIntensity()
{
    if (g_waterSpecIntensity < 0.0f) {
        const std::string v = mc2::gate::str<"MC2_WATER_SHINE">();   // "" when unset or empty
        const bool on = (!v.empty() && v[0] != '0');   // canonical default-OFF opt-in
        float s = 0.0f;
        if (on) {
            const float f = (float)atof(v.c_str());
            s = (f > 0.0f) ? f : 0.25f;               // non-numeric truthy ("on", "yes") -> 0.25
        }
        g_waterSpecIntensity = s;
    }
    return g_waterSpecIntensity;
}

void gos_terrain_bridge_renderWaterFast(
    unsigned int recordCount,
    unsigned int waterGosHandle,
    unsigned int waterDetailGosHandle,
    float waterElevation,
    float alphaDepth,
    unsigned int alphaEdgeByte,
    unsigned int alphaMiddleByte,
    unsigned int alphaDeepByte,
    float mapTopLeftX,
    float mapTopLeftY,
    float frameCos,
    float frameCosAlpha,
    float oneOverTF,
    float oneOverWaterTF,
    float cloudOffsetX,
    float cloudOffsetY,
    float sprayOffsetX,
    float sprayOffsetY,
    float maxMinUV)
{
    if (!g_gos_renderer || recordCount == 0) return;
    g_gos_renderer->renderWaterFastPath(
        recordCount, waterGosHandle, waterDetailGosHandle,
        waterElevation, alphaDepth,
        alphaEdgeByte, alphaMiddleByte, alphaDeepByte,
        mapTopLeftX, mapTopLeftY,
        frameCos, frameCosAlpha,
        oneOverTF, oneOverWaterTF,
        cloudOffsetX, cloudOffsetY,
        sprayOffsetX, sprayOffsetY,
        maxMinUV);
}

#ifdef MC2_VULKAN_ISLAND
// VK-P9a WATER-INTEROP: the water island entry points (defined in
// GameOS/gameos/vulkan_water_island.cpp under the same #ifdef). File-scope
// declarations so renderWaterFastPath can call them (extern "C" must be at
// namespace scope, not inside a function body).
extern "C" int mc2_vk_water_island_enabled();
extern "C" int mc2_vk_water_island_run(
    unsigned int sceneColorTex, unsigned int sceneDepthTex, int w, int h,
    const void* thinRecords, unsigned int thinCount,
    const void* recipes, unsigned int recipeCount,
    const void* vsParams, const void* fsParams,
    unsigned int hdriGlTex);
#endif

// WATER-SHORE-EDGE-1 feather half-width K (world units), shared by the GL MDI
// bind block and the VK water island fsParams pack (VK-PARITY-WATER-SHORE-1) so
// the two paths cannot drift. MC2_WATER_SHORE_FEATHER_WU overrides; default 7.
static float mc2WaterShoreFeatherWu()
{
    static const float s_shoreFeatherWu = mc2::gate::get<"MC2_WATER_SHORE_FEATHER_WU">();
    return s_shoreFeatherWu;
}

// Shore mode 3 depth-only lift K (fraction of clip.z). Shared by GL uniform + VK UBO.
static float mc2WaterShore3DepthK()
{
    static const float s_k = mc2::gate::get<"MC2_WATER_SHORE3_DEPTH_K">();
    return s_k;
}

// WATER-HDRI-REFL-PERF-1 LOD default, shared by the GL MDI bind block and the
// VK water island fsParams pack (RUNG-6.5) so the two paths cannot drift.
// MC2_WATER_HDRI_LOD (float) overrides; else MC2_WATER_HDRI_REFL_FULL=1 -> 1.0,
// default 4.0 (256x128 mip of the 4K source — see the bind-block comment).
static float mc2WaterHdriLodDefault()
{
    static float s_lod = -999.0f;
    if (s_lod < -1.0f) {
        if (mc2::gate::isNonEmpty<"MC2_WATER_HDRI_LOD">()) {
            s_lod = mc2::gate::get<"MC2_WATER_HDRI_LOD">();
        } else {
            const bool fullRate = mc2::gate::get<"MC2_WATER_HDRI_REFL_FULL">();
            s_lod = fullRate ? 1.0f : 4.0f;
        }
    }
    return s_lod;
}

void gosRenderer::renderWaterFastPath(
    unsigned int recordCount,
    unsigned int waterGosHandle,
    unsigned int waterDetailGosHandle,
    float waterElevation,
    float alphaDepth,
    unsigned int alphaEdgeByte,
    unsigned int alphaMiddleByte,
    unsigned int alphaDeepByte,
    float mapTopLeftX,
    float mapTopLeftY,
    float frameCos,
    float frameCosAlpha,
    float oneOverTF,
    float oneOverWaterTF,
    float cloudOffsetX,
    float cloudOffsetY,
    float sprayOffsetX,
    float sprayOffsetY,
    float maxMinUV)
{
    ZoneScopedN("renderWaterFastPath");
    TracyGpuZone("renderWaterFastPath");
    if (!water_fast_prog_ || !water_fast_prog_->shp_ || recordCount == 0) return;
    // WATER-SAME-ORDER-VALIDATE-1: top-level validate-only wrapper (gate MC2_FRAMEGRAPH_EXECUTOR).
    // No-op when gate unset (byte-identical). PIN INVARIANT: additive only — no GL state change,
    // no reorder. Body sets its own state UNCHANGED between begin and end. Placed AFTER the
    // early-return guards so begin only fires on real draws; the RAII end fires on all exit paths.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::Water,
                                              "gosRenderer_renderWaterFastPath");
    struct TopLevelGuard_ {
        ~TopLevelGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::Water,
                                                    "gosRenderer_renderWaterFastPath");
        }
    } _tlGuard;
    // Water now HAS an AmbientContract row (depthFunc=GEQUAL, depthWrite=On, colorMask/blend
    // Inherit) and an fbo_ledger MainColor row -> ambient/fbo guards validate (no longer skip).
    render_contract::noteRenderPass(render_contract::PassIdentity::Water,
                                    "gosRenderer::renderWaterFastPath");
    GLuint prog = water_fast_prog_->shp_;

    // Lazy-compile the MDI variant program (MDI VS + MDI FS) the first time it's needed.
    static GLuint s_waterMdiProg = 0;
    static GLuint s_perCmdSsbo   = 0;
    if (s_waterMdiProg == 0 && gpu_driven::IsWaterEnabled()) {
        // ARB_shader_draw_parameters in prefix (same pattern as static_prop batcher).
        // gl_DrawIDARB is used in the VS; the unsuffixed gl_DrawID is core only in 4.6.
        static const char* kWaterMdiPrefix =
            "#version 430\n"
            "#extension GL_ARB_shader_draw_parameters : require\n";
        glsl_program* mdi = glsl_program::makeProgram(
            "gos_terrain_water_mdi",
            "shaders/gos_terrain_water_fast_mdi.vert",
            "shaders/gos_terrain_water_mdi.frag",
            kWaterMdiPrefix);
        if (mdi && mdi->shp_) {
            s_waterMdiProg = mdi->shp_;
            MC2_DIAGF("[WATER_MDI v1] event=prog_compiled prog=%u\n", (unsigned)s_waterMdiProg);
            fflush(stdout);
        } else {
            // Permanently disable MDI path so we don't attempt re-compile every frame.
            // Use a sentinel GLuint of UINT_MAX (not a valid GL object name).
            s_waterMdiProg = (GLuint)~0u;
            MC2_DIAGFE(stderr, "[WATER_MDI v1] event=prog_compile_fail — MDI path disabled\n");
            fflush(stderr);
        }
    }
    if (s_perCmdSsbo == 0 && gpu_driven::IsWaterEnabled()) {
        glGenBuffers(1, &s_perCmdSsbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_perCmdSsbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 2 * (GLsizeiptr)sizeof(WaterPerCmd), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    GLint savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
    GLboolean savedBlend = glIsEnabled(GL_BLEND);
    GLint savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB, &savedSrcRGB);
    GLint savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB, &savedDstRGB);
    GLint savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK, &savedDepthMask);

    // VAO must be bound before any glDrawArrays — AMD silently drops draws
    // when VAO is 0 (memory:projectz_overlay_findings.md). The fast path
    // runs INSIDE renderWater after Object/HUD passes that may have left
    // VAO unbound. Same fix the overlay path uses.
    GLint savedVAO = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
    extern void gos_RendererRebindVAO();
    gos_RendererRebindVAO();

    glUseProgram(prog);

    // --- One-time uniform binds. setMat4Direct = GL_FALSE (row-major direct,
    // for terrainMVP per terrain_mvp_gl_false.md: GL_FALSE + row-major
    // cancels to the right math). setMat4Std = GL_TRUE (transpose to GL
    // column-major, the canonical convention for `mvp`/`projection_` —
    // see orchestrator "GL_FALSE → GL_TRUE for thin VS projection_" entry).
    auto setMat4Direct = [&](const char* name, const float* v) {
        GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, v);
    };
    auto setMat4Std = [&](const char* name, const float* v) {
        GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_TRUE, v);
    };
    auto setVec4 = [&](const char* name, const float* v) {
        GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0) glUniform4fv(loc, 1, v);
    };
    auto setVec2 = [&](const char* name, float a, float b) {
        GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0) glUniform2f(loc, a, b);
    };
    auto setF = [&](const char* name, float a) {
        GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0) glUniform1f(loc, a);
    };
    auto setI = [&](const char* name, int a) {
        GLint loc = glGetUniformLocation(prog, name);
        if (loc >= 0) glUniform1i(loc, a);
    };

    const float* wMvpWaterNonMdi =
        gos_terrain_indirect::IsFrameSolidArmed()
            ? gos_terrain_indirect_getDispatchMvp16()  // TERRAIN-PHASE-RAW-MVP-OK
            : gos_GetTerrainMVPMat4();
    if (!wMvpWaterNonMdi) wMvpWaterNonMdi = gos_GetTerrainMVPMat4();  // safety: pre-arm/first frame
    setMat4Direct("u_worldToClipGL", wMvpWaterNonMdi);
    setMat4Std   ("mvp",             (const float*)&projection_);

    // Debug-mode override gated by MC2_RENDER_WATER_FASTPATH_DEBUG=N.
    // 0 = normal, 1 = magenta solid, 2 = green, 3 = yellow.
    static int s_debugMode = -1;
    if (s_debugMode < 0) {
        s_debugMode = mc2::gate::get<"MC2_RENDER_WATER_FASTPATH_DEBUG">();
    }
    setI("debugMode", s_debugMode);
    setF   ("waterElevation",  waterElevation);
    setF   ("alphaDepth",      alphaDepth);
    // Alpha-band byte uniforms are `int` in the shader because `uniform uint`
    // crashes the project's shader_builder (memory: uniform_uint_crash.md).
    // The values are 0..255 and round-trip safely through int.
    setI   ("alphaEdgeByte",   (int)alphaEdgeByte);
    setI   ("alphaMiddleByte", (int)alphaMiddleByte);
    setI   ("alphaDeepByte",   (int)alphaDeepByte);
    setVec2("mapTopLeft",      mapTopLeftX, mapTopLeftY);
    setF   ("frameCos",        frameCos);
    setF   ("frameCosAlpha",   frameCosAlpha);
    setF   ("maxMinUV",        maxMinUV);
    setI   ("tex1",            0);
    // WATER-BIAS-LINEARIZE-1: bias mode for the FAST VS (0 = proportional
    // default, 1 = legacy constant). MC2_WATER_BIAS_LEGACY=1 reverts.
    setI   ("u_waterBiasLegacy", mc2WaterBiasLegacy());

    // Fragment-shader-side fog_color + time uniforms (gos_tex_vertex.frag).
    // 2026-04-30: fog_color was hardcoded to zeros, which disables the FS
    // fog branch (`if (fog_color.x>0 || ...)`). The legacy water flush goes
    // through gosRenderMaterial::apply()→setFogColor(fog_color_), so legacy
    // gets per-pixel atmospheric mixing that softens shoreline alpha-band
    // transitions. Without fog mixing, the 3-band classifier's tile-aligned
    // staircase is fully visible at the shore.
    //
    // CINEMATIC-WATER-WHITE-1: do NOT use the cached fog_color_ here.
    // fog_color_ holds the last value written by applyRenderStates(), which
    // may be a stale sky/atmosphere white from the most-recent terrain batch.
    // By water-render time, renderLists() has finished and every render path
    // resets gos_State_Fog to 0 — so read the current render-state value
    // directly. This gives vec4(0) (fog branch disabled) and fixes the
    // "water white in cinematic" regression. (time uses the SmokeMode
    // fixed-timestep clock when active, for deterministic capture.)
    {
        const vec4 waterFog = uint32_to_vec4(renderStates_[gos_State_Fog]);
        setF   ("time", SmokeMode::fixedTimestepEnabled()
                            ? (float)SmokeMode::fixedClockSeconds()
                            : (float)((double)(timing::get_wall_time_ms() - timeStart_) / 1000.0));
        setVec4("fog_color", (const float*)&waterFog);
    }

    // GPU-driven path: dispatch compute cull/pack + cmd-patch if enabled.
    // This MUST happen before EnsureRecipeBufferUploaded so the thin records
    // are ready and slot 6 is correctly restored to thin data (M2 fix).
    const bool gpuArmed = WaterStream::ComputeDispatchAndBindThinRecords(frameCos);

    // Ensure the recipe buffer is uploaded (idempotent, mission-static).
    GLuint recipeBuf = (GLuint)WaterStream::EnsureRecipeBufferUploaded();
    if (recipeBuf == 0) {
        glDepthMask(savedDepthMask);
        if (!savedBlend) glDisable(GL_BLEND);
        glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
        glUseProgram((GLuint)savedProgram);
        return;
    }

    // Alpha-blend overlay state.
    // 2026-04-30 shoreline fix: explicitly set depth test ON + LEQUAL.
    // The legacy water flush goes through applyRenderStates which sets these
    // from gos_State_ZCompare/gos_State_ZWrite. The bridge originally only
    // disabled depth-write, leaving depth test inheriting whatever the prior
    // pass left. Without depth test, water fragments draw over above-water
    // land tiles, producing a tile-aligned staircase at the shoreline. With
    // depth test ON + LEQUAL, land's higher Z occludes water's lower Z so
    // water only renders on actually-submerged pixels — matching the legacy
    // result where the shore looks effortlessly smooth.
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC, &savedDepthFunc);
    // CINEMATIC-WATER-CULL-1: the water fast path is a flat overlay mesh and
    // must NOT be subject to the caller's backface-cull state. The SimpleCamera
    // intro/cinematic path runs default_state.SetBackFaceOn() (simplecamera.cpp),
    // leaving GL_CULL_FACE enabled with a winding that culls EVERY water
    // triangle -> water silently vanishes on the cinematic pan. (RenderDoc pixel
    // history: water frags PASSED=no FLAGS=backfaceCulled.) GameCamera gameplay
    // leaves a cull state that lets water through, which is why this only bit the
    // cinematic. Neutralize cull for the water draw and restore after — same
    // discipline as the blend / depth-mask / depth-func state this path already
    // saves+restores below.
    GLboolean savedCullFace = glIsEnabled(GL_CULL_FACE);
    // WATER-ARMED-APPLYPIPELINE-ROUTING-1: drive base FF state from the WaterArmed
    // row (AlphaBlend SRC_ALPHA/ONE_MINUS_SRC_ALPHA, depth-test on, reverse-Z
    // GEQUAL, cull None) — byte-identical to the old hand-set (+ no-op
    // frontFace(CCW)/offset-disable). glProgramName=0 -> applyPipeline SKIPs
    // program (the glUseProgram(prog) above stays). depthMask is NOT owned here:
    // the debug-gated glDepthMask(s_waterNoDepthWrite?...) below sets it (and
    // overrides applyPipeline's depthWriteEnable=true), preserving the
    // MC2_WATER_NO_DEPTH_WRITE A/B gate. The existing save block + restore
    // epilogue still own teardown.
    //
    // APPLY-STATE-WATER-1: when MC2_FRAMEGRAPH_EXECUTOR is ON the executor APPLIES the
    // WaterArmed pipeline here (the body's live draw site, AFTER the compute dispatch at
    // :3232 — a begin-seam pre-apply would be clobbered by glUseProgram(compute)). The
    // dispatch sets waterStateAppliedByExecutor_; the body applyPipeline below is then
    // skipped (one-shot reset). Gate OFF -> flag stays false -> body applies as before ->
    // byte-identical.
    if (render_contract::isTopLevelExecutorEnabled() &&
        RenderCore::framegraph::findTopLevelStateDesc(RenderCore::RenderPassId::Water) != nullptr) {
        executorApplyWaterState();
    }
    if (!waterStateAppliedByExecutor_) {
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(RenderCore::PipelineId::WaterArmed), "WaterArmed");
    }
    waterStateAppliedByExecutor_ = false;
    render_frame_plan::trace(render_frame_plan::Phase::Water, "WaterFastPath",
        render_frame_plan::PathKind::ApplyPipeline, 1, "WaterArmed");
    // OOB-FOG-WATER-DEPTH-1: water must write depth so runFogOob() (which
    // fires on rawDepth==0 far-plane pixels) skips water-covered pixels.
    // Without depth writes, OOB fog classifies water pixels as empty sky and
    // overlays near-opaque white clouds over them.  MC2_WATER_NO_DEPTH_WRITE=1
    // restores the old broken behaviour for A/B bisect.
    static const bool s_waterNoDepthWrite = mc2::gate::get<"MC2_WATER_NO_DEPTH_WRITE">();
    glDepthMask(s_waterNoDepthWrite ? GL_FALSE : GL_TRUE);
    // Invalidate after direct GL state set so subsequent applyRenderStates()
    // calls between here and the draw cannot short-circuit on stale cache.
    invalidateRenderStateCache();

    // Install a REPEAT/LINEAR sampler on unit 0 for the water draw. The
    // patch-stream's bucket sampler (CLAMP_TO_EDGE) may still be bound from
    // the prior terrain pass; CLAMP would reduce the world-scale water UVs
    // (0..MaxMinUV ≈ 0..8) to texture-edge samples, making water invisible.
    // The legacy water path runs gos_State_TextureAddress = gos_TextureWrap,
    // matching REPEAT.
    static GLuint s_waterFastSampler = 0;
    if (s_waterFastSampler == 0) {
        glGenSamplers(1, &s_waterFastSampler);
        glSamplerParameteri(s_waterFastSampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glSamplerParameteri(s_waterFastSampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glSamplerParameteri(s_waterFastSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_waterFastSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    GLuint savedSampler = 0;
    {
        GLint q = 0;
        glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &q);
        savedSampler = (GLuint)q;
    }
    glBindSampler(0, s_waterFastSampler);

    GLuint baseTex   = (GLuint)gos_terrain_bridge_glTextureForGosHandle(waterGosHandle);
    GLuint detailTex = (waterDetailGosHandle != 0xffffffffu)
                       ? (GLuint)gos_terrain_bridge_glTextureForGosHandle(waterDetailGosHandle)
                       : 0u;

    const bool mdiValid = gpuArmed
                          && s_waterMdiProg != 0 && s_waterMdiProg != (GLuint)~0u
                          && s_perCmdSsbo  != 0
                          && baseTex       != 0;

    if (mdiValid) {

        // Atlas accessors: defined only in gos_terrain_indirect.cpp (no header).
        // Precedent: sibling terrain-solid functions declare identical extern blocks
        // (gameos_graphics.cpp:2584-2588, :2877-2881, :3100).
        extern GLuint gos_terrain_indirect_getAtlasGLTex();
        extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
        extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
        extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();

        WaterPerCmd cmds[2];
        cmds[0] = { 0u, 1, 0, oneOverTF,      cloudOffsetX, cloudOffsetY, 0u, 0u };
        cmds[1] = { 1u, 2, 1, oneOverWaterTF, sprayOffsetX, sprayOffsetY, 0u, 0u };
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_perCmdSsbo);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)sizeof(cmds), cmds);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        glUseProgram(s_waterMdiProg);

        auto setMF = [&](const char* name, float a) {
            GLint loc = glGetUniformLocation(s_waterMdiProg, name);
            if (loc >= 0) glUniform1f(loc, a);
        };
        auto setMI = [&](const char* name, int a) {
            GLint loc = glGetUniformLocation(s_waterMdiProg, name);
            if (loc >= 0) glUniform1i(loc, a);
        };
        auto setMMat4Direct = [&](const char* name, const float* v) {
            GLint loc = glGetUniformLocation(s_waterMdiProg, name);
            if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, v);
        };
        auto setMMat4Std = [&](const char* name, const float* v) {
            GLint loc = glGetUniformLocation(s_waterMdiProg, name);
            if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_TRUE, v);
        };
        auto setMVec4 = [&](const char* name, const float* v) {
            GLint loc = glGetUniformLocation(s_waterMdiProg, name);
            if (loc >= 0) glUniform4fv(loc, 1, v);
        };
        auto setMVec2 = [&](const char* name, float a, float b) {
            GLint loc = glGetUniformLocation(s_waterMdiProg, name);
            if (loc >= 0) glUniform2f(loc, a, b);
        };
        auto setMVec3 = [&](const char* name, const float* v) {
            GLint loc = glGetUniformLocation(s_waterMdiProg, name);
            if (loc >= 0) glUniform3fv(loc, 1, v);
        };

        const float* wMvpWaterMdi =
            gos_terrain_indirect::IsFrameSolidArmed()
                ? gos_terrain_indirect_getDispatchMvp16()  // TERRAIN-PHASE-RAW-MVP-OK  // TERRAIN-PHASE-RAW-MVP-OK
                : gos_GetTerrainMVPMat4();
        if (!wMvpWaterMdi) wMvpWaterMdi = gos_GetTerrainMVPMat4();  // safety: pre-arm/first frame
        setMMat4Direct("u_worldToClipGL", wMvpWaterMdi);
        setMMat4Std   ("mvp",             (const float*)&projection_);
        setMI         ("debugMode",       s_debugMode);
        setMI         ("u_waterDebugMode", gos_GetWaterFsDebugMode());  // WATER-DEBUG-VIEWS-1 (FS material-space)
        // WATER-TUNING-UI-1: runtime MDI water material params (default-seeded
        // to the former consts -> byte-identical default; live via Graphics
        // Options > Water). loc<0 (uniform optimized out) is silently skipped.
        setMF         ("ABSORPTION_DENSITY", g_waterAbsorptionDensity);
        setMF         ("WATER_MAX_ALPHA",    g_waterMaxAlpha);
        setMF         ("RIPPLE_GAIN",        g_waterRippleGain);
        setMF         ("GLINT_GAIN",         g_waterGlintGain);
        setMVec3      ("DEEP_COLOR",         g_waterDeepColor);
        setMVec3      ("SHALLOW_COLOR",      g_waterShallowColor);
        // WATER-VISUAL-FIRST-SLICE: gated sky tint (strength 0 default = no-op).
        setMF         ("u_waterSkyTintStrength", gos_GetWaterSkyTintStrength());
        setMVec3      ("u_waterSkyTintColor",    g_waterSkyTintColor);
        // WATER-SKY-REFLECTION-1: gated SH-L2 sky reflection (strength 0 = no-op).
        setMF         ("u_waterReflStrength",    gos_GetWaterReflStrength());
        // WATER-SHORE-MASK-1 (batch-6 item 3): feather the water shore band
        // along the cooked smoothed-contour distance field. Texture unit 4
        // (0/1 base/detail, 2 refl RT, 3 HDRI). Killswitch
        // MC2_WATER_SHORE_MASK=0 or no sidecar -> u_hasShoreMask=0 ->
        // byte-identical.
        // WATER-SHORE-EDGE-1: mode selection is resolved ONCE engine-wide by
        // gos_WaterShoreMode() (0=legacy ramp, 1=mask feather, 2=per-fragment
        // signed height vs the hi-res visual_height_4x sidecar; default 2).
        // Mode 2 with no height sidecar falls back to mode-1 behaviour in the
        // FS; mode >=1 with no mask sidecar falls back to legacy. Texture
        // units: 0/1 base/detail, 2 refl RT, 3 HDRI, 4 shore mask,
        // 5 shore height (R32F).
        {
            extern unsigned int gos_TerrainLodChunk_ShorelineMaskTex();
            extern const float* gos_TerrainLodChunk_ShorelineMaskBounds();
            extern int          gos_WaterShoreMode();
            extern unsigned int gos_TerrainLodChunk_WaterShoreHeightTex();
            extern const float* gos_TerrainLodChunk_WaterShoreHeightBounds();
            const int shoreMode = gos_WaterShoreMode();
            setMI("u_shoreMode", shoreMode);
            const GLuint slTex = (shoreMode >= 1)
                ? (GLuint)gos_TerrainLodChunk_ShorelineMaskTex() : 0u;
            setMI("u_hasShoreMask", slTex != 0 ? 1 : 0);
            if (slTex != 0) {
                setMVec4("u_shoreMaskBounds", gos_TerrainLodChunk_ShorelineMaskBounds());
                setMI("u_shoreMask", 4);
                glActiveTexture(GL_TEXTURE0 + 4);
                glBindTexture(GL_TEXTURE_2D, slTex);
                glActiveTexture(GL_TEXTURE0);
            }
            const GLuint shTex = (shoreMode >= 2)
                ? (GLuint)gos_TerrainLodChunk_WaterShoreHeightTex() : 0u;
            setMI("u_hasShoreHeight", shTex != 0 ? 1 : 0);
            if (shTex != 0) {
                setMVec4("u_shoreHeightBounds", gos_TerrainLodChunk_WaterShoreHeightBounds());
                setMI("u_shoreHeightTex", 5);
                glActiveTexture(GL_TEXTURE0 + 5);
                glBindTexture(GL_TEXTURE_2D, shTex);
                glActiveTexture(GL_TEXTURE0);
            }
            // K: feather half-width in world units (shore ramp spans -K..+K/2
            // about the height zero-crossing). Tunable via
            // MC2_WATER_SHORE_FEATHER_WU; default 7.
            setMF("u_shoreFeatherWu", mc2WaterShoreFeatherWu());
            setMI("u_shoreFlat", (shoreMode == 3 && shTex != 0) ? 1 : 0);
            if (shoreMode == 3) {
                // Depth-only lift of the flat plane (see vert): terrain triangles poking above
                // the plane where the bicubic says water must not occlude the water.
                setMF("u_shoreDepthK", mc2WaterShore3DepthK());
            }
        }
        setMF         ("waterElevation",  waterElevation);
        setMF         ("alphaDepth",      alphaDepth);
        setMI         ("alphaEdgeByte",   (int)alphaEdgeByte);
        setMI         ("alphaMiddleByte", (int)alphaMiddleByte);
        setMI         ("alphaDeepByte",   (int)alphaDeepByte);
        setMVec2      ("mapTopLeft",      mapTopLeftX, mapTopLeftY);
        setMF         ("frameCos",        frameCos);
        setMF         ("frameCosAlpha",   frameCosAlpha);
        setMF         ("maxMinUV",        maxMinUV);
        // WATER-EDGE-FEATHER-1: dissolve the hard map-edge water rim into the
        // fog'd horizon (companion to TERRAIN-EDGE-FEATHER-1). Gate
        // MC2_WATER_EDGE_FEATHER default-ON (reader: gameos_graphics.cpp:3427,
        // `(e && e[0]=='0') ? 0 : 1` -- a kill-switch); =0 uploads 0 -> frag block
        // skipped (byte-identical vs pre-feature). This line said default-OFF until
        // ENV-DROP-CLOSE-1 (2026-07-30), i.e. it described the OPPOSITE of what a
        // default launch renders.
        // Strength MC2_WATER_EDGE_FEATHER_STRENGTH (default 1.0).
        // halfMap = the canonical WORLD-unit half-extent (gosPostProcess::
        // getMapHalfExtent), the SAME value the legacy terrain edge-haze + post-fx
        // edge_fog use against WorldPos.xy (raw MC2 world units). Origin-centered.
        {
            // WATER-EDGE-FEATHER-DEFAULT-ON (2026-07-18, Sym4 recon): flip
            // default ON — the hard navy map-edge water band at grazing camera
            // angles is a stock-visible artifact; the feather dissolves it into
            // the horizon haze. Killswitch MC2_WATER_EDGE_FEATHER=0 reverts.
            // NOTE split: only the MDI frag (gos_terrain_water_mdi.frag)
            // declares the feather uniforms; the non-MDI fallback water path
            // draws with gos_tex_vertex.frag (shared by other passes) and
            // stays un-feathered — acceptable because the non-MDI path is a
            // fail-soft fallback (mdiValid false), not the shipped default.
            static const int s_waterEdgeFeather = mc2::gate::get<"MC2_WATER_EDGE_FEATHER">();
            static const float s_waterEdgeFeatherStr = mc2::gate::get<"MC2_WATER_EDGE_FEATHER_STRENGTH">();
            gosPostProcess* ppW = getGosPostProcess();
            setMI("u_waterEdgeFeather",         s_waterEdgeFeather);
            setMF("u_waterEdgeFeatherStrength", s_waterEdgeFeatherStr);
            setMF("u_halfMap",                  ppW ? ppW->getMapHalfExtent() : 0.0f);
        }
        // SHORELINE-GEOMETRY-1 (MC2_WATER_SHORE_MEANDER): world-space fBm
        // perturbation of the shore iso-contour, so the coastline silhouette
        // reads as an organic curve instead of the terraced heightfield's
        // tile-lattice staircase. Amplitude 0 (default) -> the FS skips the
        // term entirely -> byte-identical. Same accessors the CPU/GPU tile
        // admission uses (WaterStream::EffShoreExt), so geometry and contour
        // stay in lockstep.
        setMF("u_shoreMeanderWu",   WaterStream::ShoreMeanderWu());
        setMF("u_shoreMeanderFreq", WaterStream::ShoreMeanderFreq());
        // CINEMATIC-WATER-WHITE-1: same fix as non-MDI path above — read current
        // render state, not stale fog_color_. See comment block above. (time uses
        // the SmokeMode fixed-timestep clock when active, for deterministic capture.)
        {
            const vec4 waterFog = uint32_to_vec4(renderStates_[gos_State_Fog]);
            setMF         ("time",  SmokeMode::fixedTimestepEnabled()
                                        ? (float)SmokeMode::fixedClockSeconds()
                                        : (float)((double)(timing::get_wall_time_ms() - timeStart_) / 1000.0));
            setMVec4("fog_color", (const float*)&waterFog);
        }
        // WATER reflection frame fix: terrain_camera_pos_ is the Stuff/MLR eye
        // (.x=left, .y=elevation, .z=forward); the water FS consumes cameraPos in
        // RAW MC2 (matching WorldPos: .x=east, .y=north, .z=up). Apply the
        // documented Stuff->MC2 swap MC2=(-Stuff.x, Stuff.z, Stuff.y) here so the
        // reflect vdir + Fresnel + waveLOD respond to camera PITCH/zoom, not just
        // pan. (Other terrain/lighting consumers swap shader-side; water did not.)
        vec4 camMC2;
        mc2ToRender(camMC2, terrain_camera_pos_);
        camMC2.w = 1.0f;
        setMVec4      ("cameraPos", &camMC2.x);  // water-v1 reflection/Fresnel (raw MC2)
        // WATER-SUN-SPEC-2: these two uploads were DEAD from c50e99a6 until
        // WATER-SUN-SPEC-2 declared the matching uniforms in
        // shaders/gos_terrain_water_mdi.frag (neither name had ever existed in
        // any shader in this repo -- see gos_GetWaterSpecIntensity above).
        // They are load-bearing now.
        // WATER-SUN-SPEC-1: upload terrainLightDir RAW MC2 (no camMC2 swap).
        // The camMC2 swap above is for cameraPos only (Stuff->MC2 frame fix).
        // Light dir is already raw MC2 from gamecam.cpp:314 ("NOT swizzled — MC2 Z-up");
        // terrain uploads it raw at :6055/6177/6354. Applying the swap would double-
        // transform -> sun on wrong axis. The shader uses it directly in MC2 Z-up.
        setMVec4      ("terrainLightDir", (const float*)&terrain_light_dir_);
        setMF         ("u_waterSpecIntensity", gos_GetWaterSpecIntensity());
        setMI         ("tex1",  0);
        // WATER-BIAS-LINEARIZE-1: same mode as the non-MDI branch above.
        setMI         ("u_waterBiasLegacy", mc2WaterBiasLegacy());
        setMI         ("tex2",  1);

        // WATER-REFLECTION-SAMPLE-1: bind the terrain reflection RT (filled by
        // the Phase C1 RenderWaterReflectionPass) to unit 2 and upload the
        // sample uniforms. Replaces the old dead terrain-colormap atlas bind.
        // u_waterRtStrength 0 (gate MC2_WATER_REFLECTION_RT off) -> the FS skips
        // the RT sample and falls back to the SH sky -> byte-identical to Phase A.
        gosPostProcess* ppRefl = getGosPostProcess();
        GLuint reflRtTex = ppRefl ? (GLuint)ppRefl->getWaterReflectionTexture() : 0u;
        const bool bindRefl = (reflRtTex != 0u);
        setMI         ("u_waterReflRT",     2);
        setMF         ("u_waterRtStrength", gos_GetWaterRtStrength());
        setMVec2      ("u_waterScreenSize",
                       ppRefl ? (float)ppRefl->getWidth()  : 1.0f,
                       ppRefl ? (float)ppRefl->getHeight() : 1.0f);
        // WATER-ASPECT-CORRECT-2: the FS's dFdx(nz) aspect compensation
        // (WATER-ASPECT-CORRECT-1) was written assuming the perspective
        // frustum is ALWAYS 4:3-locked while the viewport is native-res —
        // true before UI-ASPECT-ANCHOR-1, but that fix (mclib/camera.cpp,
        // default ON) now builds the frustum at the REAL drawable aspect,
        // so there is usually no stretch left to compensate for. Pass the
        // actual frustum aspect the FS should un-stretch against: native
        // aspect (== viewport aspect, so the FS's _ac correction becomes a
        // no-op) by default, or the legacy 4.0/3.0 only when the frustum
        // really is still 4:3-locked (MC2_CAMERA_ASPECT_NATIVE=0). Fixes
        // "water HDR clipped at odd resolutions" (docs/known_issues.md) —
        // the FS was over-correcting a stretch that no longer exists,
        // growing worse the further the window aspect departs from 4:3.
        extern bool g_cameraProjAspectNative;
        const float waterVpAspect = ppRefl && ppRefl->getHeight() > 0
            ? (float)ppRefl->getWidth() / (float)ppRefl->getHeight() : (4.0f / 3.0f);
        setMF         ("u_waterProjAspect",
                       g_cameraProjAspectNative ? waterVpAspect : (4.0f / 3.0f));

        // WATER-HDRI-REFL-1: bind HDRI equirect on unit 3 for direct specular
        // sampling (sun disk + sky color, LOD-filtered via mipmaps added in gos_hdri.cpp).
        // u_waterHdriLod < 0 signals no HDRI -> FS falls back to SH-L2.
        // MC2_WATER_HDRI_LOD env (float) overrides the default.
        //
        // WATER-HDRI-REFL-PERF-1: the per-fragment HDRI path (waterEvalHdri:
        // atan2+asin + textureLod against the live equirect) measured ~10ms/frame
        // GPU at LOD 1.0 (2K mip of the 4K source) across a full-map water quad
        // (451c9e49). The HDRI reflection is a heavily blurred, low-frequency sky
        // read by design (Reinhard-toned broad color, not a sharp sun disk) — a
        // much coarser mip costs the same ALU per fragment but a fraction of the
        // texture bandwidth/cache pressure, with no visible quality loss at the
        // gameplay camera. Default bumped 1.0 -> 4.0 (256x128 mip of a 4K source;
        // recon's own "keep-cheap" recommendation). MC2_WATER_HDRI_REFL_FULL=1
        // restores the old full-rate LOD 1.0 for A/B or regression checks.
        {
            // LOD env resolve hoisted to mc2WaterHdriLodDefault() (file scope) so
            // the RUNG-6.5 VK island fsParams pack shares the exact same value.
            const bool hdriAvail = (ppRefl && ppRefl->isHdriReady());
            setMI("u_hdri", 3);
            setMF("u_skyYaw", hdriAvail ? ppRefl->getSkyYaw() : 0.0f);
            setMF("u_waterHdriLod", hdriAvail ? mc2WaterHdriLodDefault() : -1.0f);
        }

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, recipeBuf);
        // Slot 6 (thin records) was already bound by ComputeDispatchAndBindThinRecords.
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, s_perCmdSsbo);

        // Bind textures: base to unit 0, detail (or base) to unit 1.
        // Save+restore the unit-1 sampler: prior passes (e.g. patch-stream bucket)
        // may have left CLAMP_TO_EDGE on unit 1. The detail UV range (0..MaxMinUV ≈
        // 0..8) would collapse to edge-texel smear under CLAMP. Bind s_waterFastSampler
        // (REPEAT/LINEAR) for the duration of the MDI draw, then restore.
        GLuint savedSampler1 = 0;
        {
            GLint q = 0;
            glGetIntegeri_v(GL_SAMPLER_BINDING, 1, &q);
            savedSampler1 = (GLuint)q;
        }
        glBindSampler(1, s_waterFastSampler);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, baseTex);
        glActiveTexture(GL_TEXTURE1);
        GLuint detailOrBase = (detailTex != 0) ? detailTex : baseTex;
        glBindTexture(GL_TEXTURE_2D, detailOrBase);
        glActiveTexture(GL_TEXTURE0);

        // WATER-REFLECTION-SAMPLE-1: bind the reflection RT on unit 2 (mirrors the
        // unit-1 save/restore idiom). When the RT is unavailable, unit 2 is left
        // untouched; the FS only samples it when u_waterRtStrength>0 (gate ON).
        GLuint savedSampler2 = 0;
        if (bindRefl) {
            GLint q = 0;
            glGetIntegeri_v(GL_SAMPLER_BINDING, 2, &q);
            savedSampler2 = (GLuint)q;
            glBindSampler(2, 0);   // use the RT texture's own LINEAR/CLAMP_TO_EDGE params
            glActiveTexture(GL_TEXTURE0 + 2);
            glBindTexture(GL_TEXTURE_2D, reflRtTex);
            glActiveTexture(GL_TEXTURE0);
        }

        // WATER-HDRI-REFL-1: bind HDRI equirect on unit 3 (with mipmaps for LOD).
        const bool bindHdri = (ppRefl && ppRefl->isHdriReady() && ppRefl->getHdriTex() != 0u);
        if (bindHdri) {
            glActiveTexture(GL_TEXTURE0 + 3);
            glBindTexture(GL_TEXTURE_2D, ppRefl->getHdriTex());
            glActiveTexture(GL_TEXTURE0);
        }

        // VK-WATER-TIER1: when the shared-GBuffer water island is armed, render the
        // BASE layer in Vulkan in-place and skip the GL MDI draw. Feed remains the
        // CPU-visible thin-record/readback path used by the island. On any fail-soft
        // (returns 0), fall through to the GL draw. Gate OFF -> GL MDI draw as before
        // (byte-identical).
        bool vkWaterHandled = false;
#ifdef MC2_VULKAN_ISLAND
        if (mc2_vk_water_island_enabled()) {
            // Feed (STEP 0 revised): read back the thin records the armed GPU compute
            // ACTUALLY packed this frame (ground truth) — the CPU pack yields 0 on the
            // full-recipe-authoritative path (no CPU projection state). ReadbackGpuThinForVk
            // reads the bucket-header count + the packed ring slot into a CPU copy after
            // the compute's final barrier. No fenced GL buffer crosses into VK.
            const void*    vkThinPtr   = nullptr;
            // MC2_VK_WATER_PERF: time the GL thin-buffer readback (2x glGetBufferSubData
            // = blocking sync on the GL water cull/pack compute). Reported alongside the
            // island run()'s [VK_WATER_PERF] line so the full per-frame cost is visible.
            static int s_vkWaterPerf = -1;
            if (s_vkWaterPerf == -1) {
                s_vkWaterPerf = mc2::gate::get<"MC2_VK_WATER_PERF">();
            }
            const auto tThinRb0 = std::chrono::steady_clock::now();
            const uint32_t vkThinCount = WaterStream::ReadbackGpuThinForVk(&vkThinPtr);
            if (s_vkWaterPerf) {
                const double us = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - tThinRb0).count();
                std::fprintf(stderr, "[VK_WATER_PERF] thin_readback_us=%.1f thin=%u\n",
                             us, vkThinCount);
                std::fflush(stderr);
            }
            const void*    vkRecipes   = (const void*)WaterStream::GetRecipes();
            const uint32_t vkRecipeN   = WaterStream::GetRecipeCount();
            gosPostProcess* ppVk = getGosPostProcess();
            if (vkThinCount > 0 && vkThinPtr && vkRecipes && vkRecipeN > 0 && ppVk) {
                // Pack VS/FS UBO params LOCKSTEP with water_geom.{vert,frag}.
                struct VkWaterVsP { float m[16]; float p0[4],p1[4],p2[4],p3[4]; } vsP{};
                struct VkWaterFsP { float fog[4],f0[4],f1[4],f2[4],f3[4],deep[4],shallow[4],f4[4],f5[4],f6[4],f7[4],f8[4],f9[4]; } fsP{};
                memcpy(vsP.m, wMvpWaterMdi, 16 * sizeof(float));
                vsP.p0[0]=waterElevation; vsP.p0[1]=alphaDepth; vsP.p0[2]=mapTopLeftX; vsP.p0[3]=mapTopLeftY;
                vsP.p1[0]=frameCos; vsP.p1[1]=maxMinUV; vsP.p1[2]=oneOverTF; vsP.p1[3]=cloudOffsetX;
                // WATER_DEPTH_FUDGE_FAST — LOCKSTEP with mclib/terrain_depth_bias.h
                // (mc2depth::WATER_DEPTH_FUDGE_FAST) + shaders/include/terrain_depth_bias.hglsl
                // (WATER_DEPTH_FUDGE_FAST). The VS applies clip.z += this * clip.w.
                vsP.p2[0]=cloudOffsetY; vsP.p2[1]=0.00025f;
                vsP.p2[2]=(float)alphaEdgeByte; vsP.p2[3]=(float)alphaMiddleByte;
                vsP.p3[0]=(float)alphaDeepByte;
                vsP.p3[2]=mc2WaterShore3DepthK();   // shore mode 3 depth lift K (p3[1] flat flag set by the VK island)
                {
                    const vec4 waterFog = uint32_to_vec4(renderStates_[gos_State_Fog]);
                    memcpy(fsP.fog, &waterFog, 4*sizeof(float));
                }
                fsP.f0[0]= SmokeMode::fixedTimestepEnabled()
                             ? (float)SmokeMode::fixedClockSeconds()
                             : (float)((double)(timing::get_wall_time_ms() - timeStart_) / 1000.0);
                fsP.f0[1]=camMC2[0]; fsP.f0[2]=camMC2[1]; fsP.f0[3]=camMC2[2];
                fsP.f1[0]=alphaDepth; fsP.f1[1]=g_waterAbsorptionDensity;
                fsP.f1[2]=g_waterMaxAlpha; fsP.f1[3]=g_waterRippleGain;
                fsP.f2[0]=g_waterGlintGain;
                // RUNG-6.5 WATER-REFLECTION PORT: feed the reflection inputs with
                // the EXACT values the GL MDI bind block uploads (gate parity:
                // MC2_WATER_REFLECTION=0 -> strength 0.0 -> FS block skipped both
                // sides; !isHdriReady -> lod -1 -> SH-L2 fallback both sides).
                {
                    const bool vkHdriAvail = ppVk->isHdriReady();
                    fsP.f2[1] = gos_GetWaterReflStrength();                       // u_waterReflStrength
                    fsP.f2[2] = vkHdriAvail ? ppVk->getSkyYaw() : 0.0f;           // u_skyYaw
                    fsP.f2[3] = vkHdriAvail ? mc2WaterHdriLodDefault() : -1.0f;   // u_waterHdriLod
                    fsP.f3[0] = (float)ppVk->getWidth();                          // u_waterScreenSize
                    fsP.f3[1] = (float)ppVk->getHeight();
                }
                memcpy(fsP.deep,    g_waterDeepColor,    3*sizeof(float));
                memcpy(fsP.shallow, g_waterShallowColor, 3*sizeof(float));
                // VK-PARITY-WATER-SHORE-1: WATER-SHORE-EDGE-1 inputs, same producers
                // as the GL bind block (gos_WaterShoreMode / accessor bounds / K).
                // f4.y/f4.z (hasHeight / hasMask) are set by the island, which owns
                // the VK copies of the GL shore textures.
                {
                    extern int          gos_WaterShoreMode();
                    extern const float* gos_TerrainLodChunk_ShorelineMaskBounds();
                    extern const float* gos_TerrainLodChunk_WaterShoreHeightBounds();
                    fsP.f4[0] = (float)gos_WaterShoreMode();     // u_shoreMode
                    fsP.f4[3] = mc2WaterShoreFeatherWu();        // u_shoreFeatherWu
                    memcpy(fsP.f5, gos_TerrainLodChunk_WaterShoreHeightBounds(), 4*sizeof(float)); // u_shoreHeightBounds
                    memcpy(fsP.f6, gos_TerrainLodChunk_ShorelineMaskBounds(),    4*sizeof(float)); // u_shoreMaskBounds
                    fsP.f7[0] = waterElevation;                  // waterElevation
                }
                // VK-PARITY-F3: GL-default water features the VK FS lacked, same producers as
                // the GL MDI bind block (edge feather killswitch/strength, map half extent,
                // authored sky tint, frustum aspect).
                {
                    static const int s_vkWEdgeFeather = mc2::gate::get<"MC2_WATER_EDGE_FEATHER">();
                    static const float s_vkWEdgeFeatherStr = mc2::gate::get<"MC2_WATER_EDGE_FEATHER_STRENGTH">();
                    extern bool g_cameraProjAspectNative;
                    const float vkVpAspect = ppVk->getHeight() > 0
                        ? (float)ppVk->getWidth() / (float)ppVk->getHeight() : (4.0f / 3.0f);
                    fsP.f8[0] = (float)s_vkWEdgeFeather;
                    fsP.f8[1] = s_vkWEdgeFeatherStr;
                    fsP.f8[2] = ppVk->getMapHalfExtent();
                    fsP.f8[3] = gos_GetWaterSkyTintStrength();
                    memcpy(fsP.f9, g_waterSkyTintColor, 3*sizeof(float));
                    fsP.f9[3] = g_cameraProjAspectNative ? vkVpAspect : (4.0f / 3.0f);
                }
                vkWaterHandled = (1 == mc2_vk_water_island_run(
                    (unsigned int)ppVk->getSceneColorTexture(),
                    (unsigned int)ppVk->getSceneDepthTexture(),
                    ppVk->getWidth(), ppVk->getHeight(),
                    vkThinPtr, vkThinCount, vkRecipes, vkRecipeN, &vsP, &fsP,
                    ppVk->isHdriReady() ? (unsigned int)ppVk->getHdriTex() : 0u));
            }
        }
#endif

        const GLsizei drawCount = (detailTex != 0) ? 2 : 1;
        if (!vkWaterHandled) {
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, WaterStream::GetIndirectCmdBuffer());
            glMultiDrawArraysIndirect(GL_TRIANGLES, nullptr, drawCount, 0);
            mdi_submit::trace("WaterFastPath", "MultiDrawArraysIndirect", (int)drawCount,
                              "GPU", "WaterStreamIndirect", false);
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
        }

        if (bindRefl) {
            glActiveTexture(GL_TEXTURE0 + 2);
            glBindTexture(GL_TEXTURE_2D, 0);    // force-clear; mirrors unit-1 post-draw
            glActiveTexture(GL_TEXTURE0);
            glBindSampler(2, savedSampler2);    // restore sampler only
        }

        if (bindHdri) {
            glActiveTexture(GL_TEXTURE0 + 3);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE0);
        }

        // Restore unit 1 — don't leave a texture bound on unit 1 for the legacy path.
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindSampler(1, savedSampler1);

        // Parity check: when MC2_GPU_DRIVEN_PARITY=1, runs CPU pack and compares
        // against the GPU compute output byte-for-byte. No-op when disabled.
        WaterStream::ComputeDispatchParity_Check();

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, 0);
        glBindSampler(0, savedSampler);
        glDepthMask(savedDepthMask);
        glDepthFunc((GLenum)savedDepthFunc);
        if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
        if (savedCullFace) glEnable(GL_CULL_FACE);
        if (!savedBlend) glDisable(GL_BLEND);
        glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
        glUseProgram((GLuint)savedProgram);
        glBindVertexArray((GLuint)savedVAO);
    } else {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, recipeBuf);
        const uint32_t thinCount = WaterStream::UploadAndBindThinRecords();
        if (thinCount == 0) {
            glBindSampler(0, savedSampler);
            glDepthMask(savedDepthMask);
            glDepthFunc((GLenum)savedDepthFunc);
            if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
            if (savedCullFace) glEnable(GL_CULL_FACE);
            if (!savedBlend) glDisable(GL_BLEND);
            glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
            glUseProgram((GLuint)savedProgram);
            glBindVertexArray((GLuint)savedVAO);
            return;
        }

        static bool s_fastDiagPrinted = false;
        if (!s_fastDiagPrinted) {
            s_fastDiagPrinted = true;
            while (glGetError() != GL_NO_ERROR) {}
            GLint curVAO=0, curFBO=0, curVP[4]={0};
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &curVAO);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &curFBO);
            glGetIntegerv(GL_VIEWPORT, curVP);
            GLboolean depthTest = glIsEnabled(GL_DEPTH_TEST);
            GLboolean cullFace  = glIsEnabled(GL_CULL_FACE);
            GLint curDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC, &curDepthFunc);
            fprintf(stderr,
                    "[WATER_FAST v1] event=first_draw prog=%u recipeBuf=%u "
                    "thin=%u baseGosH=%u baseGLTex=%u detailGosH=%u detailGLTex=%u "
                    "VAO=%d FBO=%d viewport=[%d,%d,%d,%d] depthTest=%d cullFace=%d depthFunc=0x%x\n",
                    (unsigned)prog, (unsigned)recipeBuf,
                    (unsigned)thinCount,
                    (unsigned)waterGosHandle, (unsigned)baseTex,
                    (unsigned)waterDetailGosHandle, (unsigned)detailTex,
                    curVAO, curFBO, curVP[0], curVP[1], curVP[2], curVP[3],
                    depthTest, cullFace, (unsigned)curDepthFunc);
            fflush(stderr);
        }

        const GLsizei drawVerts = (GLsizei)(thinCount * 6u);

        if (baseTex != 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, baseTex);

            setI("isWater",    1);
            setI("detailMode", 0);
            setF("uvScale",    oneOverTF);
            setVec2("uvOffset", cloudOffsetX, cloudOffsetY);

            glDrawArrays(GL_TRIANGLES, 0, drawVerts);
            {
                GLenum err = glGetError();
                if (err != GL_NO_ERROR) {
                    static bool s_errPrinted = false;
                    if (!s_errPrinted) {
                        s_errPrinted = true;
                        fprintf(stderr, "[WATER_FAST v1] event=base_draw_gl_err err=0x%x verts=%d\n",
                                (unsigned)err, (int)drawVerts);
                        fflush(stderr);
                    }
                }
            }
        }

        if (detailTex != 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, detailTex);

            setI("isWater",    2);
            setI("detailMode", 1);
            setF("uvScale",    oneOverWaterTF);
            setVec2("uvOffset", sprayOffsetX, sprayOffsetY);

            glDrawArrays(GL_TRIANGLES, 0, drawVerts);
        }

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, 0);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, 0);
        glBindSampler(0, savedSampler);
        glDepthMask(savedDepthMask);
        glDepthFunc((GLenum)savedDepthFunc);
        if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
        if (savedCullFace) glEnable(GL_CULL_FACE);
        if (!savedBlend) glDisable(GL_BLEND);
        glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
        glUseProgram((GLuint)savedProgram);
        glBindVertexArray((GLuint)savedVAO);
    }

    // WATER-THINRING-FENCE-1: fence the thin-record ring slot just drawn (both the
    // armed-MDI and legacy-CPU branches reach here after their draw). The two
    // early-returns above (recipeBuf==0, legacy thinCount==0) intentionally skip
    // this — no draw consumed the slot in those cases.
    WaterStream::EndThinRingFrameFence();

    // RENDER_STATES v1: water fast path bound textures directly on unit 0; the
    // applyRenderStates cache is now stale. Force a full re-apply on next call.
    invalidateRenderStateCache();
}
