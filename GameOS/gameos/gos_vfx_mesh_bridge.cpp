//==========================================================================//
// File:    gos_vfx_mesh_bridge.cpp                                          //
// Contents: GameOS-side GL bridge for the VFX mesh substrate                //
//           (MC2_VFX_ORACLE_SHAPE slice — gosFX::Shape only).               //
//                                                                           //
// Persistent-mesh + per-instance-transform model (the GPU mech-batcher      //
// shape, NOT the per-frame CPU vertex expansion of the billboard/tube path).//
// Each distinct MLRShape's model-space verts/UVs/indices are uploaded to a  //
// GL VBO/EBO ONCE, cached by 64-bit mesh id. Per frame the bridge issues one //
// indexed glDrawElements per instance with per-instance {matrix, scale,     //
// color, texture, blendMode}. Translucent: depth-test ON, depth-write OFF.  //
// No object-ID writes (single color attachment).                            //
//                                                                           //
// GL state save/restore + InvalidateRenderStateCache mirror the particle    //
// bridge (gos_particle_bridge.cpp). This file owns ALL GL.                  //
//==========================================================================//

#include "gos_vfx_mesh_bridge.h"

#include <gameos.hpp>
#include <GL/glew.h>
#include "utils/shader_builder.h"
#include "../../RenderCore/PipelineRegistry.h"  // VFX-APPLYPIPELINE-ROUTING-1
#include "pipeline_binder.h"                    // applyPipeline — VFX per-instance blend

#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include "Gates/gate.h"
#include "mc2fmt.h"

// terrainMVP — same world->GL-clip matrix the particle bridge and terrain use
// (row-major direct upload, GL_FALSE; see memory/terrain_mvp_gl_false.md).
extern const float* gos_GetTerrainMVPMat4();
// MLR pool index / GOS handle -> GL texture name (gameos_graphics.cpp:8850).
// gameos.hpp already declares this.
// unsigned int gos_GetGLTextureName(DWORD handle);
extern void gos_RendererRebindVAO();

namespace {

// ── One cached GPU mesh (uploaded once, keyed by meshId) ──────────────────
struct CachedMesh {
    GLuint vao        = 0;
    GLuint vbo        = 0;   // interleaved [pos.xyz, uv.xy]
    GLuint ebo        = 0;
    GLsizei indexCount = 0;
};

std::unordered_map<uint64_t, CachedMesh> s_meshCache;

GLuint s_sampler = 0;   // CLAMP_TO_EDGE + LINEAR
GLuint s_whiteTex = 0;  // 1x1 white fallback (complete texture for untextured prims)
const ::glsl_program* s_prog = nullptr;
bool s_initFailed = false;

// Cached uniform locations (-2 = unqueried, -1 = absent, >=0 = valid).
GLint s_loc_worldToClipGL = -2;
GLint s_loc_modelToWorld  = -2;
GLint s_loc_scale         = -2;
GLint s_loc_instanceColor = -2;
GLint s_loc_uAtlas        = -2;
GLint s_loc_hasTexture    = -2;

// Lazy env gate for verbose per-mesh diagnostics (MC2_VFX_ORACLE_SHAPE_LOG).
bool s_log_initialized = false;
bool s_log_value       = false;
bool logEnabled() {
    if (!s_log_initialized) {
        s_log_value = mc2::gate::get<"MC2_VFX_ORACLE_SHAPE_LOG">();
        s_log_initialized = true;
    }
    return s_log_value;
}

// ── Bridge-side counters (process lifetime; dumped under LOG every 240) ────
unsigned long long g_instancesSubmitted = 0;
unsigned long long g_drawsIssued        = 0;   // instances actually drawn
unsigned long long g_missingTexture     = 0;   // gosTexHandle!=0 but GL name 0
unsigned long long g_flushCalls         = 0;
bool g_firstHarvestLogged = false;

void ensureInitialized() {
    if (s_initFailed) return;
    if (s_prog != nullptr && s_sampler != 0 && s_whiteTex != 0) return;

    if (s_sampler == 0) {
        // TEX-CLASS: per-pass-rebind -- vfx-mesh sampler object
        glGenSamplers(1, &s_sampler);
        glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    if (s_whiteTex == 0) {
        // 1x1 opaque white so the sampler is always bound to a COMPLETE
        // texture even for untextured primitives (an incomplete/0 texture
        // bound to an active sampler trips GL_INVALID_OPERATION on AMD).
        const unsigned char px[4] = {255, 255, 255, 255};
        GLint prevTex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        // TEX-CLASS: asset-pool -- 1x1 white VFX fallback content texture
        glGenTextures(1, &s_whiteTex);
        glBindTexture(GL_TEXTURE_2D, s_whiteTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    }
    if (s_prog == nullptr) {
        static const char* kPrefix = "#version 430\n";
        s_prog = ::glsl_program::makeProgram(
            "vfx_mesh",
            "shaders/vfx_mesh.vert",
            "shaders/vfx_mesh.frag",
            kPrefix);
        if (!s_prog || !s_prog->shp_) {
            s_initFailed = true;
            mc2::fmt::print_err("[VFX_MESH v1] event=prog_compile_fail — bridge disabled\n");
            std::fflush(stderr);
            s_prog = nullptr;
            return;
        }
        mc2::fmt::print_err("[VFX_MESH v1] event=prog_compiled prog={:d}\n", mc2::fmt::as_u32((unsigned)s_prog->shp_));
        std::fflush(stderr);
        s_loc_worldToClipGL = glGetUniformLocation(s_prog->shp_, "u_worldToClipGL");
        s_loc_modelToWorld  = glGetUniformLocation(s_prog->shp_, "u_modelToWorld");
        s_loc_scale         = glGetUniformLocation(s_prog->shp_, "u_scale");
        s_loc_instanceColor = glGetUniformLocation(s_prog->shp_, "u_instanceColor");
        s_loc_uAtlas        = glGetUniformLocation(s_prog->shp_, "uAtlas");
        s_loc_hasTexture    = glGetUniformLocation(s_prog->shp_, "u_hasTexture");
    }
}

// Upload one mesh into a fresh VAO/VBO/EBO. Interleaves pos(3)+uv(2) so a
// single VBO drives both attributes. Returns true on success.
bool uploadMesh(const GosVfxMeshUpload& up, CachedMesh& out) {
    if (up.vertexCount == 0 || up.indexCount == 0 ||
        up.positions == nullptr || up.indices == nullptr) {
        return false;
    }
    // Build interleaved buffer: 5 floats per vertex.
    const unsigned vc = up.vertexCount;
    std::vector<float> interleaved;
    interleaved.resize((size_t)vc * 5u);
    for (unsigned i = 0; i < vc; ++i) {
        interleaved[i*5+0] = up.positions[i*3+0];
        interleaved[i*5+1] = up.positions[i*3+1];
        interleaved[i*5+2] = up.positions[i*3+2];
        if (up.uvs) {
            interleaved[i*5+3] = up.uvs[i*2+0];
            interleaved[i*5+4] = up.uvs[i*2+1];
        } else {
            interleaved[i*5+3] = 0.0f;
            interleaved[i*5+4] = 0.0f;
        }
    }

    glGenVertexArrays(1, &out.vao);
    glGenBuffers(1, &out.vbo);
    glGenBuffers(1, &out.ebo);

    glBindVertexArray(out.vao);
    glBindBuffer(GL_ARRAY_BUFFER, out.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(interleaved.size() * sizeof(float)),
                 interleaved.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(up.indexCount * sizeof(uint16_t)),
                 up.indices, GL_STATIC_DRAW);

    // location 0 = pos (3 floats), location 1 = uv (2 floats), stride 5 floats.
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void*)(3 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    out.indexCount = (GLsizei)up.indexCount;
    return true;
}

void maybeDumpSummary() {
    if (!logEnabled()) return;
    if ((g_flushCalls % 240ull) != 0ull) return;
    mc2::fmt::print_err("[VFX_MESH v1] event=summary flushCalls={:d} instancesSubmitted={:d} drawsIssued={:d} missingTexture={:d} meshesUploaded={:d}\n", mc2::fmt::as_u64(g_flushCalls), mc2::fmt::as_u64(g_instancesSubmitted), mc2::fmt::as_u64(g_drawsIssued), mc2::fmt::as_u64(g_missingTexture), mc2::fmt::as_u32((unsigned)s_meshCache.size()));
    std::fflush(stderr);
}

}  // namespace

extern "C" void gos_vfx_mesh_flush(const GosVfxMeshUpload*   uploads,
                                   unsigned int              numUploads,
                                   const GosVfxMeshInstance* instances,
                                   unsigned int              numInstances) {
    ++g_flushCalls;
    if (numInstances == 0 || instances == nullptr) {
        maybeDumpSummary();
        return;
    }

    ensureInitialized();
    if (s_initFailed || s_prog == nullptr || s_prog->shp_ == 0) {
        maybeDumpSummary();
        return;
    }

    // ── Ingest any not-yet-cached mesh uploads (upload ONCE per meshId) ────
    for (unsigned i = 0; i < numUploads; ++i) {
        const GosVfxMeshUpload& up = uploads[i];
        if (s_meshCache.find(up.meshId) != s_meshCache.end()) continue;  // cache hit
        CachedMesh cm;
        if (uploadMesh(up, cm)) {
            s_meshCache.emplace(up.meshId, cm);
            if (logEnabled()) {
                mc2::fmt::print_err("[VFX_MESH v1] event=mesh_upload meshId={:d} verts={:d} indices={:d} cacheSize={:d}\n", mc2::fmt::as_u64((unsigned long long)up.meshId), mc2::fmt::as_u32(up.vertexCount), mc2::fmt::as_u32(up.indexCount), mc2::fmt::as_u32((unsigned)s_meshCache.size()));
                std::fflush(stderr);
            }
        }
    }

    if (logEnabled() && !g_firstHarvestLogged) {
        g_firstHarvestLogged = true;
        mc2::fmt::print_err("[VFX_MESH v1] event=FIRST_HARVEST instances={:d} uploads={:d}\n", mc2::fmt::as_u32(numInstances), mc2::fmt::as_u32(numUploads));
        std::fflush(stderr);
    }

    g_instancesSubmitted += numInstances;

    // ── GL state save (mirror gos_particle_bridge) ────────────────────────
    GLint savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
    GLint savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
    GLint savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB, &savedSrcRGB);
    GLint savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB, &savedDstRGB);
    GLboolean savedBlend = glIsEnabled(GL_BLEND);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC, &savedDepthFunc);
    GLint savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK, &savedDepthMask);
    GLint savedSampler   = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &savedSampler);
    GLint savedActiveTex = 0; glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTex);
    GLint savedTex2D0    = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex2D0);
    GLboolean savedCullFace = glIsEnabled(GL_CULL_FACE);

    // ── Program + frame-constant uniforms ─────────────────────────────────
    glUseProgram(s_prog->shp_);
    {
        const float* mvp = gos_GetTerrainMVPMat4();
        if (mvp && s_loc_worldToClipGL >= 0)
            glUniformMatrix4fv(s_loc_worldToClipGL, 1, GL_FALSE, mvp);
    }
    if (s_loc_uAtlas >= 0) glUniform1i(s_loc_uAtlas, 0);
    glBindSampler(0, s_sampler);

    // ── Translucent state: depth-test ON, depth-write OFF, blend ON ───────
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);   // reverse-Z (matches particle/water bridges)
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // gosFX Shape meshes (blast rings etc.) are authored single-winding but
    // mirror through the X-flip axis swap; render double-sided like the
    // billboard path so they never backface-cull away.
    glDisable(GL_CULL_FACE);

    // ── Per-instance indexed draws (effect-phase / submission order) ──────
    for (unsigned ii = 0; ii < numInstances; ++ii) {
        const GosVfxMeshInstance& inst = instances[ii];
        auto it = s_meshCache.find(inst.meshId);
        if (it == s_meshCache.end()) continue;  // mesh upload failed/absent
        const CachedMesh& cm = it->second;
        if (cm.vao == 0 || cm.indexCount == 0) continue;

        // Per-instance uniforms. modelToWorld is supplied COLUMN-MAJOR (GL
        // native) by mclib (the Stuff row-vector affine already folded into a
        // column-vector matrix), so upload GL_FALSE.
        if (s_loc_modelToWorld  >= 0)
            glUniformMatrix4fv(s_loc_modelToWorld, 1, GL_FALSE, inst.modelToWorld);
        if (s_loc_scale         >= 0) glUniform1f(s_loc_scale, inst.scale);
        if (s_loc_instanceColor >= 0) glUniform4fv(s_loc_instanceColor, 1, inst.rgba);

        // Texture resolve (gosTexHandle already a GOS handle from mclib).
        GLuint glTex = 0;
        if (inst.gosTexHandle != 0)
            glTex = (GLuint)gos_GetGLTextureName(inst.gosTexHandle);
        if (inst.gosTexHandle != 0 && glTex == 0) {
            ++g_missingTexture;  // resolve failed — draw untextured-white
        }
        if (s_loc_hasTexture >= 0) glUniform1i(s_loc_hasTexture, glTex != 0 ? 1 : 0);
        // Always bind a COMPLETE texture (1x1 white fallback when unresolved)
        // so the active sampler never references an incomplete texture.
        glBindTexture(GL_TEXTURE_2D, glTex != 0 ? glTex : s_whiteTex);

        // VFX-APPLYPIPELINE-ROUTING-1: per-instance blend via VfxMesh row
        // (additive = AdditiveSrcAlphaOne = SRC_ALPHA/ONE; alpha = SRC_ALPHA/ONE_MINUS_SRC_ALPHA).
        // Same GL state as the old hand-set; program(0)=skip keeps the vfx_mesh program.
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(inst.blendMode == 1
                ? RenderCore::PipelineId::VfxMeshAdditive
                : RenderCore::PipelineId::VfxMeshAlpha),
            inst.blendMode == 1 ? "VfxMeshAdditive" : "VfxMeshAlpha");

        glBindVertexArray(cm.vao);
        glDrawElements(GL_TRIANGLES, cm.indexCount, GL_UNSIGNED_SHORT, (const void*)0);
        ++g_drawsIssued;
    }

    // ── Restore state ─────────────────────────────────────────────────────
    if (savedCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex2D0);
    glBindSampler(0, (GLuint)savedSampler);
    if (savedActiveTex != GL_TEXTURE0) glActiveTexture((GLenum)savedActiveTex);
    glDepthMask((GLboolean)savedDepthMask);
    glDepthFunc((GLenum)savedDepthFunc);
    if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    if (!savedBlend) glDisable(GL_BLEND);
    glUseProgram((GLuint)savedProgram);
    glBindVertexArray((GLuint)savedVAO);

    // Raw GL calls bypassed the gos render-state cache; force re-sync so the
    // next gos_SetRenderState(sameValue) is not a stale no-op (same guard the
    // particle + mech batchers use).
    extern void __stdcall gos_InvalidateRenderStateCache();
    gos_InvalidateRenderStateCache();

    maybeDumpSummary();
}
