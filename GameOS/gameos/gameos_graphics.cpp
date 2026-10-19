// Renderer lifecycle and glue: the shader-flag gate readers, gosRenderer init/destroy/frame hooks, the renderer C-API entry points and the
// GL texture-id accessors. Mesh draws, water, terrain bridges, shadows, terrain parameters, HUD text, the gos_* C API and overlays live in
// gameos_graphics_{draw,water,bridge,shadow,tparams,hud,capi,overlay}.cpp; the classes they share are in
// gameos_graphics_material_internal.h and gameos_graphics_renderer_internal.h.


#include "gameos_graphics_material_internal.h"
#include "mc2fmt.h"
#include "texture_decode.h"
#include "ui_texture.h"
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_context.h"
#include "vk_texture_residency.h"
#endif
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
    static const bool enabled = mc2::gate::get<"MC2_TERRAIN_NORMAL_ARRAY">();
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
bool mc2ShadowStaticBandFixEnabled() {
    // SHADOW-BANDING-DEFAULT-ON (2026-07-18): field bug report confirmed the
    // banding persists in the shipped default config ("this was never
    // fixed") — the fix existed but was left opt-in. DEFAULT ON now;
    // killswitch: MC2_SHADOW_STATIC_BANDFIX=0 reverts to legacy taps.
    static const bool enabled = mc2::gate::get<"MC2_SHADOW_STATIC_BANDFIX">();
    return enabled;
}

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
bool mc2ShadowStaticBandFixV2Enabled() {
    // SHADOW-BANDING-DEFAULT-ON (2026-07-18): field bug report (banding in
    // and around mech shadows on terrain) verified A/B in-engine: default
    // config shows diagonal contour striping across terrain
    // (debugview 1 = shadow factor, mc2_01 f1800: high-pass stripe
    // amplitude 3.67 -> 1.12 with V2 on). DEFAULT ON now; killswitch:
    // MC2_SHADOW_STATIC_BANDFIX_V2=0 reverts to flat-normal bias.
    static const bool enabled = mc2::gate::get<"MC2_SHADOW_STATIC_BANDFIX_V2">();
    return enabled;
}

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
bool mc2TerrainShadowGuardClampedSun() {
    static const bool enabled = mc2::gate::get<"MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN">();
    return enabled;
}

// mc2ShadowMinSunAltDeg() is defined in gos_postprocess.cpp (declared in
// gameos.hpp) so the GLSL clamp and the C++ matrix clamp can never drift apart.
//
// Emits "#define MC2_SHADOW_MIN_SUN_SIN <sin(minAlt)>" for shader prefixes whose
// program consumes shadow.hglsl with the terrain sun. Returns "" when the gate is
// off or the clamp is disabled -> shadow.hglsl's 0.0 default -> identity -> the
// compiled shader is byte-identical to legacy.
std::string mc2TerrainShadowSunDefine() {
    if (!mc2TerrainShadowGuardClampedSun()) return std::string();
    const float deg = mc2ShadowMinSunAltDeg();
    if (deg <= 0.0f) return std::string();
    char buf[80];
    mc2::fmt::to_buf(buf, "#define MC2_SHADOW_MIN_SUN_SIN {:.8f}\n", static_cast<double>(sinf(deg * 0.01745329252f)));
    return std::string(buf);
}

// VK-PARITY-SHADOW-1: single producer of the terrain shadow knob set (see gameos.hpp).
// Values are round-tripped through the SAME printf formats the #define injection uses
// (%.1f wu, %.2f max, %.8f sin) so GL (compile-time literal) and VK (UBO float) agree.
Mc2ShadowFadeKnobs mc2ShadowFadeKnobs() {
    Mc2ShadowFadeKnobs k{};
    char buf[64];
    if (mc2ShadowDistFadeWu() > 0.0f) {
        mc2::fmt::to_buf(buf, "{:.1f}", static_cast<double>(mc2ShadowDistFadeWu()));
        k.distFadeWu = static_cast<float>(atof(buf));
        mc2::fmt::to_buf(buf, "{:.2f}", static_cast<double>(mc2ShadowDistFadeMax()));
        k.distFadeMax = static_cast<float>(atof(buf));
        k.flags |= mc2ShadowFadeCont()  ? 1 : 0;
        k.flags |= mc2ShadowFadeRamp0() ? 2 : 0;
        k.flags |= mc2ShadowAltScale()  ? 4 : 0;
    }
    if (mc2TerrainShadowGuardClampedSun()) {
        const float deg = mc2ShadowMinSunAltDeg();
        if (deg > 0.0f) {
            mc2::fmt::to_buf(buf, "{:.8f}", static_cast<double>(sinf(deg * 0.01745329252f)));
            k.minSunSin = static_cast<float>(atof(buf));
        }
    }
    if (mc2ShadowStaticBandFixV2Enabled()) k.flags |= 8;
    return k;
}

// WATER-BIAS-LINEARIZE-1: bias-mode selector for the FAST water regime.
// Default 0 = distance-proportional bias (clip.z += F_REL*clip.z; world error
// ~ linear in distance, kills the far-field water-over-ridge z-fight).
// MC2_WATER_BIAS_LEGACY=1 -> 1 = legacy constant NDC offset
// (clip.z += FUDGE*clip.w). Consumed by both fast water VS variants and the
// gpu_driven_water.comp pzOk admission gate (all three MUST agree; see
// mclib/terrain_depth_bias.h). Exported via gameos.hpp for the water-stream TU.
int mc2WaterBiasLegacy() {
    static const int mode = mc2::gate::get<"MC2_WATER_BIAS_LEGACY">();
    return mode;
}

gosRenderer* getGosRenderer() {
    return g_gos_renderer;
}

GLenum getGLVertexAttribType(gosVERTEX_ATTRIB_TYPE type) {
	GLenum t = -1;
	switch (type)
	{
	case gosVERTEX_ATTRIB_TYPE::BYTE: return GL_BYTE;
	case gosVERTEX_ATTRIB_TYPE::UNSIGNED_BYTE: return GL_UNSIGNED_BYTE;
	case gosVERTEX_ATTRIB_TYPE::SHORT: return GL_SHORT;
	case gosVERTEX_ATTRIB_TYPE::UNSIGNED_SHORT: return GL_UNSIGNED_SHORT;
	case gosVERTEX_ATTRIB_TYPE::INT: return GL_INT;
	case gosVERTEX_ATTRIB_TYPE::UNSIGNED_INT: return GL_UNSIGNED_INT;
	case gosVERTEX_ATTRIB_TYPE::FLOAT: return GL_FLOAT;
	default:
		gosASSERT(0 && "unknows vertex attrib type");
	}

	return t;
};

static const char* const g_shader_flags[] = {
    "ALPHA_TEST",
    "IS_OVERLAY"
};

const std::string gosRenderMaterial::s_mvp = std::string("mvp");
const std::string gosRenderMaterial::s_fog_color = std::string("fog_color");

const std::string gosMesh::s_tex1 = std::string("tex1");

const std::string gosRenderer::s_Foreground = std::string("Foreground");

// ──────────────────────────────────────────────────────────────────────────

GLuint gVAO = 0;
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


    { ZoneScopedN("gosRenderer::init vao"); glGenVertexArrays(1, &gVAO); }

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

    // Terrain tessellation extra VBO
    terrain_extra_capacity_ = 1024 * 120;  // large buffer for extended view distance
    {
        ZoneScopedN("gosRenderer::init terrainExtra");
        terrain_extra_data_ = new gos_TERRAIN_EXTRA[terrain_extra_capacity_];
        terrain_extra_vb_ = makeBuffer(GL_ARRAY_BUFFER, nullptr,
            sizeof(gos_TERRAIN_EXTRA) * terrain_extra_capacity_, GL_DYNAMIC_DRAW);
    }
    mc2::fmt::print_out("[TESS] Extra VBO created: capacity={:d} vb={:d}\n", mc2::fmt::as_i32(terrain_extra_capacity_), mc2::fmt::as_u32(terrain_extra_vb_));

    // Load terrain tessellation material (TCS/TES shaders)
    // ISSUE-34: MC2_TERRAIN_TESS=0 (default ON) skips the load — same engine
    // state as a TCS/TES compile failure — so the no-tess config is testable
    // headlessly. Shadow/GPU-transform behavior is keyed on
    // gos_IsModernShadowPathActive(), not on this material.
    {
        ZoneScopedN("gosRenderer::init terrainMaterial");
        if (!mc2::gate::get<"MC2_TERRAIN_TESS">()) {
            terrain_material_ = nullptr;
            mc2::fmt::print_out("[TESS] MC2_TERRAIN_TESS=0 — terrain tessellation material load skipped (kill-switch)\n");
        } else {
            mc2::fmt::print_out("[TESS] About to load terrain shader...\n"); fflush(stdout);
            gosMaterialVariationHelper terrainHelper;
            gosMaterialVariation mvar;
            terrainHelper.getMaterialVariation(mvar);
            terrain_material_ = gosRenderMaterial::load("gos_terrain", mvar);
            if (terrain_material_) {
                materialList_.push_back(terrain_material_);
                mc2::fmt::print_out("[TESS] Terrain material loaded successfully\n");
            } else {
                mc2::fmt::print_out("[TESS] WARNING: Terrain material failed to load — tessellation disabled\n");
            }
        }
        fflush(stdout);
    }

    // Load thin-record terrain program (gos_terrain_thin.vert + gos_terrain.frag, no tess).
    // Used by PatchStream M1g to draw thin records via GL_TRIANGLES, avoiding tessellation overhead.
    {
        ZoneScopedN("gosRenderer::init thinTerrainProg");
        std::string kThinPrefix = "#version 430\n";
        if (terrainNormalArrayEnabled())
            kThinPrefix += "#define TERRAIN_NORMAL_ARRAY\n";
        if (mc2ShadowStaticBandFixEnabled())
            kThinPrefix += "#define MC2_SHADOW_STATIC_BANDFIX 1\n";
        if (mc2ShadowStaticBandFixV2Enabled())
            kThinPrefix += "#define MC2_SHADOW_STATIC_BANDFIX_V2 1\n";
        kThinPrefix += mc2TerrainShadowSunDefine();   // TERRAIN-SHADOW-GUARD-1
        thin_terrain_prog_ = glsl_program::makeProgram(
            "gos_terrain_thin",
            "shaders/gos_terrain_thin.vert",
            "shaders/gos_terrain.frag",
            kThinPrefix.c_str());
        if (!thin_terrain_prog_ || !thin_terrain_prog_->shp_)
            mc2::fmt::print_err("[THIN_TERRAIN] WARNING: failed to compile thin terrain shader — thin draw path disabled\n");
        else
            mc2::fmt::print_out("[THIN_TERRAIN] Thin terrain shader loaded: prog={:d}\n", mc2::fmt::as_u32(static_cast<unsigned>(thin_terrain_prog_->shp_)));
        fflush(stdout);
    }

    if (!TerrainPatchStream::init()) {
        // init_fail path lives in Task 2; for now this is dead code.
        MC2_DIAGFE(stderr, "[PATCH_STREAM v1] event=init_fail reason=task1_skeleton_returned_false\n");
        fflush(stderr);
    }

    gos_TerrainLodChunk_Init();
}

void gosRenderer::destroy() {
    gos_TerrainLodChunk_Destroy();
    TerrainPatchStream::destroy();

    gosMesh::destroy(text_);

    for(size_t i=0; i<materialList_.size(); i++) {
        gosRenderMaterial::destroy(materialList_[i]);
    }
    materialList_.clear();

    // delete fonts before textures, because they refer them
    for(size_t i=0; i<fontList_.size(); i++) {
        while(gosFont::destroy(fontList_[i])) {};
    }
    fontList_.clear();

    for(size_t i=0; i<textureList_.size(); i++) {
        delete textureList_[i];
    }
    textureList_.clear();

    // Terrain tessellation cleanup
    delete[] terrain_extra_data_;
    terrain_extra_data_ = nullptr;
    if (terrain_extra_vb_) { glDeleteBuffers(1, &terrain_extra_vb_); terrain_extra_vb_ = 0; }
    if (terrain_material_) { gosRenderMaterial::destroy(terrain_material_); terrain_material_ = nullptr; }
    if (terrain_normal_array_tex_ != 0) {
        glDeleteTextures(1, &terrain_normal_array_tex_);
        terrain_normal_array_tex_ = 0;
    }

    glDeleteVertexArrays(1, &gVAO);

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

void gos_CreateRenderer(graphics::RenderContextHandle ctx_h, graphics::RenderWindowHandle win_h, int w, int h) {
    ZoneScopedN("gos_CreateRenderer");

    g_gos_renderer = new gosRenderer(ctx_h, win_h, w, h);
    g_gos_renderer->init();

    gosPostProcess* pp = new gosPostProcess();
    pp->init(w, h);

    // Initialize diagnostic JSONL trace. Session id shared with debug_state_dump
    // so MCP can correlate JSONL events with render-state snapshots by session_id.
    mc2_diag::init(mc2_debug_state::getSessionId(), _getpid());

    // META-FIX INFRA-1: route gated audit marker lines into the JSONL trace.
    mc2AuditSetSink(&mc2AuditDiagSink);

    // BUILD — one-shot build metadata; fires immediately after init().
    {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"commit\":\"%s\","
            "\"dirty\":%s,"
            "\"branch\":\"%s\","
            "\"build_time\":\"%s\","
            "\"config\":\"" MC2_DIAG_BUILD_CONFIG "\","
            "\"platform\":\"windows\"}",
            MC2_BUILD_GIT_SHA, MC2_BUILD_GIT_DIRTY ? "true" : "false",
            MC2_BUILD_GIT_BRANCH, MC2_BUILD_TIME_ISO);
        mc2_diag::writeEvent("BUILD", 1, 0, buf);
    }

    // DEVICE — one-shot GL device info; GL context is valid (g_gos_renderer->init() ran above).
    {
        const char* vendor   = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* version  = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* glsl     = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        char buf[1024];
        mc2::fmt::to_buf(buf, "{{\"gl_vendor\":\"{}\",\"gl_renderer\":\"{}\",\"gl_version\":\"{}\",\"glsl_version\":\"{}\"}}", mc2::fmt::safe(vendor   ? vendor   : "<unavailable>"), mc2::fmt::safe(renderer ? renderer : "<unavailable>"), mc2::fmt::safe(version  ? version  : "<unavailable>"), mc2::fmt::safe(glsl     ? glsl     : "<unavailable>"));
        mc2_diag::writeEvent("DEVICE", 1, 0, buf);
    }
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
void gos_RendererRebindVAO() {
    glBindVertexArray(gVAO);
}

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
uint32_t gos_GetGLTextureId(uint32_t gosHandle) {
    if (gosHandle == INVALID_TEXTURE_ID || !g_gos_renderer) return 0;
    // Reject uninitialized / sentinel handles (seen in the wild on
    // TG_TinyTexture entries that were never loaded: 0xFFFFFFFF).
    // Bound by the current texture list size to avoid the hard assert
    // inside gosRenderer::getTexture(texture_id >= list size).
    if (gosHandle >= g_gos_renderer->getTextureListSize()) return 0;
    gosTexture* tex = g_gos_renderer->getTexture(gosHandle);
    return tex ? tex->getTextureId() : 0;
}

GosTexels gos_GetTextureTexels(uint32_t gosHandle) {
    GosTexels out;
    if (gosHandle == INVALID_TEXTURE_ID || !g_gos_renderer) return out;
    if (gosHandle >= g_gos_renderer->getTextureListSize()) return out;
    gosTexture* tex = g_gos_renderer->getTexture(gosHandle);
    if (!tex || tex->getTextureId() == 0) return out;   // not loaded yet: the caller retries next frame
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

GosTexels gos_GetTextureTexelsByGlName(uint32_t glName) {
    if (glName == 0 || !g_gos_renderer) return {};
    if (GosTexels pv = gos_GetCameraPreviewTexels(glName); pv.status == GosTexels::Status::Ready) return pv;
    const uint32_t h = g_gos_renderer->findTextureHandleByGlName(glName);
    return h == INVALID_TEXTURE_ID ? GosTexels{} : gos_GetTextureTexels(h);
}

// P1-1: FX-GPU-1 Phase 1 — narrow GL-name resolver for the GPU particle
// bridge. Declared in GameOS/include/gameos.hpp and called at flush time.
// Delegates to gos_GetGLTextureId; the DWORD parameter matches the GOS
// texture handle convention used by gosFX/MLR.
uint64_t gos_GetUiTextureId(DWORD handle) {
#ifdef MC2_VULKAN_ISLAND
    if (!mc2vk::uiTextureServiceReady()) return gos_GetGLTextureId(handle);
    const auto source=gos_GetTextureTexels(handle);
    if (source.status!=GosTexels::Status::Ready) return 0;
    return mc2vk::getOrCreateEngineVulkanContext().textures().legacyUiId(handle,source.width,source.height,
        mc2vk::texelFormat(source.vkFormat),source.bytes);
#else
    return gos_GetGLTextureId(handle);
#endif
}
uint64_t gos_GetMaterialTextureId(DWORD handle) {
#ifdef MC2_VULKAN_ISLAND
    if (!mc2vk::uiTextureServiceReady()) return 0;
    const auto texels=gos_GetTextureTexels(handle);
    if (texels.status!=GosTexels::Status::Ready || !texels.bytes) return 0;
    return mc2vk::getOrCreateEngineVulkanContext().textures().legacyUiId(handle,texels.width,texels.height,
        mc2vk::texelFormat(texels.vkFormat),texels.bytes,false,g_gos_renderer->getTexture(handle)->wantsMipmaps());
#else
    return 0;
#endif
}


unsigned int gos_GetGLTextureName(DWORD handle) {
    return gos_GetGLTextureId(static_cast<uint32_t>(handle));
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
