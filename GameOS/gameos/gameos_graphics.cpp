// Renderer lifecycle and glue: the shader-flag gate readers, gosRenderer init/destroy/frame hooks, the renderer C-API entry points and the
// GL texture-id accessors. Mesh draws, water, terrain bridges, shadows, terrain parameters, HUD text, the gos_* C API and overlays live in
// gameos_graphics_{draw,water,bridge,shadow,tparams,hud,capi,overlay}.cpp; the classes they share are in
// gameos_graphics_material_internal.h and gameos_graphics_renderer_internal.h.


#include "gameos_graphics_material_internal.h"
#include "mc2fmt.h"
#include "vk_scene_readback.h"
#include "texture_decode.h"
#include "ui_texture.h"
#include "vulkan_context.h"
#include "vk_texture_residency.h"
#include "utils/mc2log.h"



gosRenderer* g_gos_renderer = nullptr;

static bool isAllConcreteTerrainBatch(const gos_VERTEX* vertices, int count) {
    if (!vertices || count <= 0)
        return false;

    for (int i = 0; i < count; ++i) {
        if ((vertices[i].frgb & 0x000000ffu) != 3u)
            return false;
    }

    return true;
}

// TERRAIN-NORMAL-ARRAY: save/restore all GL pixel transfer, PBO, and texture
// binding state that glGetTexImage / glTexSubImage3D are sensitive to.
// Construct once, destructor restores. Use in any function that calls
// glGetTexImage or bulk texture upload to avoid clobbering surrounding state.
// GAMEOS-GRAPHICS-SPLIT-1 slice 2: GlPixelStoreGuard moved to
// gameos_graphics_internal.h (used by gosTexture Lock/Unlock inline methods).

// Single-source gate for the sampler2DArray terrain normal path.
// Used by shader prefix injection AND all bind paths — must agree for the
// lifetime of compiled programs. Evaluated once at startup via static.
bool terrainNormalArrayEnabled() {
    // DEFAULT ON (was opt-in). The sampler2DArray path is the only one whose
    // build runs at the clean terrain/shadow bind sites; the terrain LOD chunk
    // renderer needs the array populated to apply material detail normals.
    // Visual is identical to the legacy individual-sampler path by design
    // (validated 5/5 ON). Explicit MC2_TERRAIN_NORMAL_ARRAY=0 forces the old
    // individual-sampler path.
    static const bool enabled = true;
    return enabled;
}

// STATIC-SHADOW-BANDING-1: default ON (2026-07-18, was OFF). Raises the LOD-based static-shadow PCF
// tap floor (gos_terrain.frag's `shadowTaps` ladder 16/8/4) to 16/12/8 so the
// wide adaptive-radius PCF used on flat terrain (see shadow.hglsl's
// gradient-adaptive `adaptiveScale`) doesn't undersample the Poisson disk at
// mid/far LOD. Root cause: on FLAT terrain the radius widens toward 3.2x while
// tap count drops to 8 (mid) or 4 (far) for perf — wide-radius + few-taps is
// the textbook Poisson-disk banding/ring artifact, most visible on smoothly
// varying receivers (rolling grass), NOT a shadow-map resolution or depth
// issue. Killswitch "=0" => byte-identical legacy (shadowTaps stays 16/8/4).


// STATIC-SHADOW-BANDING-2: default ON (2026-07-18, was OFF). Round 1 (BANDFIX v1) was refuted by
// field check as insufficient -- taps=16 still banded on TERRAIN (grass/dirt
// slopes; v1 verified mostly on concrete, the wrong receiver). Measurement:
// static map world-per-texel (~4.6 world units on mc2_01, 4096^2 ortho over
// the whole map) divided by tan(slope) predicts ~25-90 world-unit-spaced
// diagonal contour bands on gentle 3-10 deg slopes -- matches the user's
// "smooth discrete diagonal contour bands", not Poisson-tap noise. Root
// cause: calcShadow()'s NdotL/bias in shadow.hglsl was computed against a
// FAKE flat-up shadowN (gos_terrain.frag), so slope-scaled bias never
// adapted to real terrain tilt. v2 swaps in the height-derived slope normal
// for shadow bias ONLY (lighting normal N is untouched). Independent gate,
// typically paired with v1 (taps) but not required to be. Killswitch "=0" =>
// byte-identical legacy (shadowN stays flat vec3(0,0,1)).


// TERRAIN-SHADOW-GUARD-1 (MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN, DEFAULT ON,
// "=0" kills). The shadow MATRICES are built from an altitude-clamped sun
// (mc2ClampShadowSunAltitude, gos_postprocess.cpp, 15deg min) but shadow.hglsl's
// back-face guard `if (NdotL < 0.05) return 1.0;` dotted the RAW authored sun.
// Under MC2_SHADOW_STATIC_BANDFIX_V2 (default ON) `normal` is reset to flat-up
// immediately before that guard, so on terrain it reduces to
// `terrainLightDir.z < 0.05`. mc2_10 authors LightDirPitch=180 -> z = 0 ->
// EVERY terrain pixel early-outs fully lit while props/mechs keep their shadows
// via the unguarded path. Feeding the guard the same clamped sun the matrices
// used restores them. No-op wherever sin(authored altitude) >= sin(15deg)
// (mc2_01=50deg, mc2_03=46deg, mc2_24=75deg -> byte-identical).


// mc2ShadowMinSunAltDeg() is defined in gos_postprocess.cpp (declared in
// gameos.hpp) so the GLSL clamp and the C++ matrix clamp can never drift apart.
//
// Emits "#define MC2_SHADOW_MIN_SUN_SIN <sin(minAlt)>" for shader prefixes whose
// program consumes shadow.hglsl with the terrain sun. Returns "" when the gate is
// off or the clamp is disabled -> shadow.hglsl's 0.0 default -> identity -> the
// compiled shader is byte-identical to legacy.


// VK-PARITY-SHADOW-1: single producer of the terrain shadow knob set (see gameos.hpp).
// Values are round-tripped through the SAME printf formats the #define injection uses
// (%.1f wu, %.2f max, %.8f sin) so GL (compile-time literal) and VK (UBO float) agree.


// WATER-BIAS-LINEARIZE-1: bias-mode selector for the FAST water regime.
// Default 0 = distance-proportional bias (clip.z += F_REL*clip.z; world error
// ~ linear in distance, kills the far-field water-over-ridge z-fight).
// MC2_WATER_BIAS_LEGACY=1 -> 1 = legacy constant NDC offset
// (clip.z += FUDGE*clip.w). Consumed by both fast water VS variants and the
// gpu_driven_water.comp pzOk admission gate (all three MUST agree; see
// mclib/terrain_depth_bias.h). Exported via gameos.hpp for the water-stream TU.
int mc2WaterBiasLegacy() {
    static const int mode = false;
    return mode;
}

gosRenderer* getGosRenderer() {
    return g_gos_renderer;
}

;

const std::string gosRenderer::s_Foreground = std::string("Foreground");

// ──────────────────────────────────────────────────────────────────────────


// GAMEOS-GRAPHICS-SPLIT-1 slice 5: the HUD-scale / UI-canvas state globals
// moved to gameos_graphics_params.cpp (extern-declared in
// gameos_graphics_internal.h; flushHUDBatch et al. still reference them
// directly).


void gosRenderer::init() {
    ZoneScopedN("gosRenderer::init");
    initRenderStates();

    // x = 1/w; x =2*x - 1;
    // y = 1/h; y= 1- y; y =2*y - 1;
    // z = z;
    projection_ = mat4(
            2.0f / static_cast<float>(width_), 0, 0.0f, -1.0f,
            0, -2.0f / static_cast<float>(height_), 0.0f, 1.0f,
            0, 0, 1.0f, 0.0f,
            0, 0, 0.0f, 1.0f);

	graphics::get_drawable_size(win_h_, &Environment.drawableWidth, &Environment.drawableHeight);

    // setup viewport
    setupViewport(true, 1.0f, true, 0, 0.0f, 0.0f, 1.0f, 1.0f);

    {
        ZoneScopedN("gosRenderer::init meshes");
        text_ = gosMesh::makeMesh(4024 * 6);
        gosASSERT(text_);
    }




    pendingRequest = false;

    num_draw_calls_ = 0;
    num_draw_calls_to_draw_ = 0;
    break_on_draw_call_ = false;
    break_draw_call_num_ = 0;

    // add fake texture so that no one will get 0 index, as it is invalid in this game
    DWORD tex_id;
    { ZoneScopedN("gosRenderer::init dummyTexture"); tex_id = gos_NewEmptyTexture( gos_Texture_Solid, "DEBUG_this_is_not_a_real_texture_debug_it!", 1); }
    (void)tex_id;
    gosASSERT(tex_id == INVALID_TEXTURE_ID);

	fog_color_ = vec4(1.0f, 1.0f, 1.0f, 1.0f);

    gos_TerrainLodChunk_Init();
}

void gosRenderer::initHeadless() {
    pendingRequest = false;
    num_draw_calls_ = 0;
    num_draw_calls_to_draw_ = 0;
    break_on_draw_call_ = false;
    break_draw_call_num_ = 0;
    DWORD tex_id = gos_NewEmptyTexture( gos_Texture_Solid, "DEBUG_this_is_not_a_real_texture_debug_it!", 1);
    (void)tex_id;
    gosASSERT(tex_id == INVALID_TEXTURE_ID);
    fog_color_ = vec4(1.0f, 1.0f, 1.0f, 1.0f);
}

void gosRenderer::destroy() {
    gos_TerrainLodChunk_Destroy();

    gosMesh::destroy(text_);

    // delete fonts before textures, because they refer them
    for(auto & fontElem : fontList_) {
        while(gosFont::destroy(fontElem)) {};
    }
    fontList_.clear();

    for(auto & textureElem : textureList_) {
        delete textureElem;
    }
    textureList_.clear();


}

// META-FIX INFRA-1: diagnostic mirror sink for mc2AuditLine. mclib's audit
// emitter must not link mc2_diag directly (tools that skip
// diagnostic_trace.cpp still link mclib), so GameOS registers this sink once
// the JSONL trace is up. Mirrored events land under tag "AUDIT" (add AUDIT to
// MC2_DIAG_TAGS to capture them).
static void mc2AuditDiagSink(const char* line) {
    if (!mc2_diag::enabled()) return;
    char esc[1024];
    size_t o = 0;
    for (const char* p = line; *p && o + 8 < sizeof(esc); ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\') { esc[o++] = '\\'; esc[o++] = static_cast<char>(c); }
        else if (c < 0x20) {
            o += static_cast<size_t>(mc2::fmt::to_buf_n(esc + o, sizeof(esc) - o, "\\u{:04x}", mc2::fmt::as_u32(static_cast<unsigned>(c))));
        } else {
            esc[o++] = static_cast<char>(c);
        }
    }
    esc[o] = '\0';
    char buf[1200];
    mc2::fmt::to_buf(buf, "{{\"line\":\"{}\"}}", mc2::fmt::safe(esc));
    mc2_diag::writeEvent("AUDIT", 1, 0, buf);
}

void gos_CreateRenderer(graphics::RenderWindowHandle win_h, int w, int h) {
    ZoneScopedN("gos_CreateRenderer");

    g_gos_renderer = new gosRenderer(win_h, w, h);
    g_gos_renderer->init();

    gosPostProcess* pp = new gosPostProcess();
    pp->init(w, h);
    if (!mc2vk::sceneReadbackSelfTest()) {
        mc2::fmt::print_err("[VK_SCENE_READBACK] selftest result=FAIL\n");
        std::abort();
    }

    // Initialize diagnostic JSONL trace. Session id shared with debug_state_dump
    // so MCP can correlate JSONL events with render-state snapshots by session_id.
    mc2_diag::init(mc2_debug_state::getSessionId(), _getpid());

    // META-FIX INFRA-1: route gated audit marker lines into the JSONL trace.
    mc2AuditSetSink(&mc2AuditDiagSink);

    // BUILD — one-shot build metadata; fires immediately after init().
    {
        char buf[512];
        mc2::fmt::to_buf(buf,
            "{{\"commit\":\"{}\","
            "\"dirty\":{},"
            "\"branch\":\"{}\","
            "\"build_time\":\"{}\","
            "\"config\":\"{}\","
            "\"platform\":\"windows\"}}",
            mc2::fmt::safe(MC2_BUILD_GIT_SHA), MC2_BUILD_GIT_DIRTY ? "true" : "false",
            mc2::fmt::safe(MC2_BUILD_GIT_BRANCH), mc2::fmt::safe(MC2_BUILD_TIME_ISO), mc2::fmt::safe(kDiagBuildConfig));
        mc2_diag::writeEvent("BUILD", 1, 0, buf);
    }

    // Report the actual native device selected by the shared owner.
    {
        const char* name = "unavailable";
        if (mc2vk::engineVulkanContextValid())
            name = mc2vk::getOrCreateEngineVulkanContext().health().deviceName;
        char buf[512];
        mc2::fmt::to_buf(buf, "{{\"backend\":\"vulkan\",\"device\":\"{}\"}}", mc2::fmt::safe(name));
        mc2_diag::writeEvent("DEVICE", 2, 0, buf);
    }

}

// --headless (docs/sim/headless-load.md): a renderer object with no window, no meshes and no device, so the texture list, font list and
// screen-mode state that mission load fills exist. Mirrors gosRenderer::init minus every device call.
void gos_CreateRendererHeadless(int w, int h) {
    g_gos_renderer = std::make_unique<gosRenderer>(nullptr, w, h).release();
    Environment.drawableWidth = w;
    Environment.drawableHeight = h;
    g_gos_renderer->initHeadless();
}

void gos_DestroyRenderer() {

    // Write shutdown state dump and flush diagnostics BEFORE any resource teardown.
    // Both functions read render/postprocess state — must run while it is still valid.
    mc2_debug_state::writeShutdownState();
    mc2_diag::shutdown();

    gosPostProcess* pp = getGosPostProcess();
    if (pp) { pp->destroy(); delete pp; }

    g_gos_renderer->destroy();
    delete g_gos_renderer;
}

void gos_RendererBeginFrame() {
    gosASSERT(g_gos_renderer);
    // F3 CPU projection cost-baseline: idempotent first-call init, then
    // reset per-frame bucket accumulators. No-op when env OFF.
    ::mc2_cpu_proj_cost::init_from_env();
    ::mc2_cpu_proj_cost::frame_begin();
    { g_gos_renderer->beginFrame(); }
}

void gos_RendererEndFrame() {
    gosASSERT(g_gos_renderer);
    { g_gos_renderer->endFrame(); }
    // F3 CPU projection cost-baseline: commit per-frame samples; print
    // window stats every 500 frames. No-op when env OFF.
    ::mc2_cpu_proj_cost::frame_end();
    // [B1 C16] gosFX heap + child counter tick; env-gated; no-op when OFF.
    ::gosFX::DiagFrameTick();
}

void gos_RendererFlushHUDBatch() {
    gosASSERT(g_gos_renderer);
    g_gos_renderer->flushHUDBatch();
}

// pp->endScene() leaves VAO 0 bound. Any caller that needs to issue
// gos_Draw* calls after endScene (e.g. projectz_overlay_render) must
// rebind gVAO first, same as flushHUDBatch() does internally.


// GAMEOS-GRAPHICS-SPLIT-1 slice 5: logical-canvas size shims for extraction
// TUs (gameos_graphics_params.cpp) that must not see the gosRenderer class.
// Return 0 when the renderer does not exist yet (callers treat 0 as "no-op").
int gosRendererLogicalWidth()  { return g_gos_renderer ? g_gos_renderer->getWidth()  : 0; }
int gosRendererLogicalHeight() { return g_gos_renderer ? g_gos_renderer->getHeight() : 0; }

void gos_RendererHandleEvents() {
    gosASSERT(g_gos_renderer);
    g_gos_renderer->handleEvents();
}

// ── End world-space overlay batch API ─────────────────────────────────────────

// Resolve a gosTextureHandle to the underlying raw GL texture name for
// consumers outside this TU (gos_static_prop_batcher). Returns 0 on
// invalid handle — caller should treat 0 as "unbind / default white".
GosTexels gos_GetTextureTexels(uint32_t gosHandle) {
    GosTexels out;
    if (gosHandle == INVALID_TEXTURE_ID || !g_gos_renderer) return out;
    if (gosHandle >= g_gos_renderer->getTextureListSize()) return out;
    gosTexture* tex = g_gos_renderer->getTexture(gosHandle);
    if (!tex) return out;
    gosTextureInfo info;
    tex->getTextureInfo(&info);
    out.width = static_cast<uint32_t>(info.width_);
    out.height = static_cast<uint32_t>(info.height_);
    if (auto blocks = tex->getBlocks()) {
        out.vkFormat = tex->getBlockVkFormat();
        out.bytes = std::move(blocks);
    } else if (tex->hasRgbaMirror()) {
        out.vkFormat = mc2tex::kVkFormatRgba8Unorm;
        out.bytes = tex->getRgbaMirror();
    } else {
        static int s_noTexelLogs = 0;
        if (++s_noTexelLogs <= 8)
            MC2_LOG(Core, Warn, "[TEXTURE_RGBA v1] handle {:d} has no CPU texels; the importers draw it untextured",
                    mc2::fmt::as_u32(gosHandle));
        out.status = GosTexels::Status::Unavailable;
        return out;
    }
    out.status = GosTexels::Status::Ready;
    return out;
}

// P1-1: FX-GPU-1 Phase 1 — narrow GL-name resolver for the GPU particle
// bridge. Declared in GameOS/include/gameos.hpp and called at flush time.
// Delegates to gos_GetGLTextureId; the DWORD parameter matches the GOS
// texture handle convention used by gosFX/MLR.
uint64_t gos_GetUiTextureId(DWORD handle) {
    if (!mc2vk::uiTextureServiceReady()) return 0;
    const auto source=gos_GetTextureTexels(handle);
    if (source.status!=GosTexels::Status::Ready) return 0;
    return mc2vk::getOrCreateEngineVulkanContext().textures().legacyUiId(handle,source.width,source.height,
        mc2vk::texelFormat(source.vkFormat),source.bytes);
}
uint64_t gos_GetMaterialTextureId(DWORD handle) {
    if (!mc2vk::uiTextureServiceReady()) return 0;
    const auto texels=gos_GetTextureTexels(handle);
    if (texels.status!=GosTexels::Status::Ready || !texels.bytes) return 0;
    return mc2vk::getOrCreateEngineVulkanContext().textures().legacyUiId(handle,texels.width,texels.height,
        mc2vk::texelFormat(texels.vkFormat),texels.bytes,false,g_gos_renderer->getTexture(handle)->wantsMipmaps());
}


const float* gos_GetProj2ScreenMat4() {
    if (!g_gos_renderer) return nullptr;
    return reinterpret_cast<const float*>(&g_gos_renderer->getProj2Screen());
}

const float* gos_GetTerrainMVPMat4() {
    const bool rendererOk = (g_gos_renderer != nullptr);
    const bool valid      = rendererOk && g_gos_renderer->isTerrainMVPValid();
    const float* p        = valid ? reinterpret_cast<const float*>(&g_gos_renderer->getTerrainMVP()) : nullptr;

    // [MVP_DIAG v1] S2.7 — log get_mvp entry, throttled. If valid,
    // also dump row0 from the cache to compare against set_mvp row0.
    if (g_mvpDiagFrame == 1 || g_mvpDiagFrame == 5 ||
        g_mvpDiagFrame == 30 || g_mvpDiagFrame == 120) {
        static long s_lastLogged = -1;
        if (s_lastLogged != g_mvpDiagFrame) {
            s_lastLogged = g_mvpDiagFrame;
            MC2_DIAGFE(stderr,
                    "[MVP_DIAG v1] event=get_mvp frame=%ld renderer=%p valid=%d ptr=%p\n",
                    g_mvpDiagFrame, static_cast<void*>(g_gos_renderer), valid ? 1 : 0, static_cast<void*>(const_cast<float*>(p)));
            if (p) {
                MC2_DIAGFE(stderr,
                        "[MVP_DIAG v1] event=get_mvp_row0 frame=%ld row0=[%g %g %g %g]\n",
                        g_mvpDiagFrame, p[0], p[1], p[2], p[3]);
            }
            fflush(stderr);
        }
    }
    return p;
}

// TERRAIN-SHORELINE-MASK-1: shared render-shader clock, same computation as
// the water fast-path's "time" uniform (SmokeMode fixed-timestep override for
// deterministic capture, else wall-time since renderer init). Consumed by the
// chunk terrain frag's shoreline wet/foam band (f(worldPos,time) ONLY — no
// camera dependence, matching the water FS's camera-independence ruling).
float gos_GetShaderClockSeconds() {
    return g_gos_renderer ? g_gos_renderer->getShaderClockSeconds() : 0.0f;
}

// gos_GetTerrainTeseProgram removed in Task 7b (UBO pivot).
// Per-program upload approach retired; UnifiedProjectionUBO at binding=0
// reaches all material-variant programs by binding point.

#include "gameos_graphics_debug.cpp"
