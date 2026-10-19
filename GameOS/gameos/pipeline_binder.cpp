// GameOS/gameos/pipeline_binder.cpp
//
// GL implementation of pipeline_binder::applyPipeline().
// This is the only site that translates RenderCore::PipelineDesc fields
// into live GL calls. Keep it thin — no caching, no state tracking.
// A future "diff-apply" pass can add redundant-call elision without
// changing the interface.

#include "pipeline_binder.h"

#include <GL/glew.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Gates/gate.h"
#include "mc2fmt.h"

namespace pipeline_binder {

// SHADOW-CASTER-APPLYPIPELINE-ROUTING-1: MC2_PIPELINE_BIND_TRACE (default OFF).
static bool pipelineBindTraceEnabled() {
    static int s_on = -1;
    if (s_on < 0) {
        s_on = mc2::gate::get<"MC2_PIPELINE_BIND_TRACE">();
    }
    return s_on != 0;
}

void applyPipeline(const RenderCore::PipelineDesc& desc, const char* dbgName) {
    // --- Program ---
    // 0 means not yet registered (bindProgram not called). Skip rather than
    // binding program 0, which would silently fall back to fixed-function.
    if (desc.glProgramName != 0u)
        glUseProgram(static_cast<GLuint>(desc.glProgramName));

    // --- Depth ---
    if (desc.depthTestEnable) glEnable(GL_DEPTH_TEST);
    else                       glDisable(GL_DEPTH_TEST);
    glDepthMask(desc.depthWriteEnable ? GL_TRUE : GL_FALSE);
    {
        GLenum glDepthFn = GL_LEQUAL;
        switch (desc.depthFunc) {
            case RenderCore::DepthFunc::LessEqual:    glDepthFn = GL_LEQUAL;  break;
            case RenderCore::DepthFunc::GreaterEqual: glDepthFn = GL_GEQUAL;  break;
            case RenderCore::DepthFunc::Always:       glDepthFn = GL_ALWAYS;  break;
            case RenderCore::DepthFunc::Equal:        glDepthFn = GL_EQUAL;   break;
            case RenderCore::DepthFunc::Less:         glDepthFn = GL_LESS;    break;
        }
        glDepthFunc(glDepthFn);
    }

    // --- Blend ---
    // GLSTATE-BLEND-RESTORE-1: always set glBlendFunc regardless of enable/disable.
    // NVIDIA retains stale blend factors when GL_BLEND is disabled; any subsequent
    // glEnable(GL_BLEND) without a matching glBlendFunc inherits them. Resetting to
    // GL_ONE/GL_ZERO for Opaque/AlphaTest ensures a predictable neutral baseline.
    switch (desc.blend) {
        case RenderCore::BlendMode::Opaque:
            glDisable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ZERO);
            break;
        case RenderCore::BlendMode::AlphaTest:
            // Alpha-tested geometry uses shader discard; GL_BLEND stays off.
            glDisable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ZERO);
            break;
        case RenderCore::BlendMode::AlphaBlend:
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case RenderCore::BlendMode::Additive:        // legacy coarse == AdditiveOneOne
        case RenderCore::BlendMode::AdditiveOneOne:
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            break;
        case RenderCore::BlendMode::AdditiveSrcAlphaOne:
            // BLENDMODE-ADDITIVE-VOCABULARY-1: pre-multiplied-ish additive used by
            // particle_billboard + vfx_mesh (alpha-scaled add), distinct from the
            // ONE/ONE tube additive. applyPipeline can now express BOTH.
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            break;
        case RenderCore::BlendMode::Multiply:
            // BLENDMODE-MULTIPLY-1: DST_COLOR/ZERO — multiplicative DARKENING used by
            // the post-fx screen-shadow / cloud-shadow / shoreline / SSAO-apply passes
            // (scene *= mask). Distinct from every additive/alpha mode.
            glEnable(GL_BLEND);
            glBlendFunc(GL_DST_COLOR, GL_ZERO);
            break;
    }

    // --- Cull ---
    switch (desc.cullMode) {
        case RenderCore::CullMode::None:
            glDisable(GL_CULL_FACE);
            break;
        case RenderCore::CullMode::Back:
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            break;
        case RenderCore::CullMode::Front:
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT);
            break;
    }

    // --- Front-face winding ---
    // PIPELINEKEY-RASTERSTATE-FRONTFACE-AUTHORITY-1: make winding explicit per
    // pipeline instead of relying on the ambient process-wide default. All
    // registered pipelines are Ccw (GL default), so this is a behavioral no-op
    // today — it stops the registered set from depending on leaked global state.
    // The legacy fixed-function face-flip path is independent of this call.
    glFrontFace(desc.frontFace == RenderCore::FrontFace::Cw ? GL_CW : GL_CCW);

    // --- Polygon offset (ENABLE/DISABLE only) ---
    // SHADOW-CASTER-APPLYPIPELINE-ROUTING-1: drive GL_POLYGON_OFFSET_FILL from the
    // pipeline row. The MAGNITUDE (glPolygonOffset factor/units) is intentionally
    // NOT set here — it stays owned by the call site (runtime/ImGui shadowBias),
    // paired with that site's existing teardown glDisable. Only ShadowStaticProp
    // sets this true; for all others this disables (a no-op where offset is
    // already off — e.g. the shadow brackets and scene passes).
    if (desc.polygonOffsetEnable) glEnable(GL_POLYGON_OFFSET_FILL);
    else                          glDisable(GL_POLYGON_OFFSET_FILL);

    if (dbgName && pipelineBindTraceEnabled()) {
        const char* df =
            desc.depthFunc == RenderCore::DepthFunc::Less         ? "Less" :
            desc.depthFunc == RenderCore::DepthFunc::LessEqual    ? "LessEqual" :
            desc.depthFunc == RenderCore::DepthFunc::GreaterEqual ? "GreaterEqual" :
            desc.depthFunc == RenderCore::DepthFunc::Equal        ? "Equal" : "Always";
        const char* cm =
            desc.cullMode == RenderCore::CullMode::None ? "None" :
            desc.cullMode == RenderCore::CullMode::Back ? "Back" : "Front";
        const char* ff = desc.frontFace == RenderCore::FrontFace::Cw ? "Cw" : "Ccw";
        // VFX-VISUAL-GATE-1: emit BlendMode so the visual gate confirms at runtime
        // that the additive cases are NOT collapsed — tube = ONE/ONE
        // (AdditiveOneOne), billboard/mesh = SRC_ALPHA/ONE (AdditiveSrcAlphaOne).
        const char* bm =
            desc.blend == RenderCore::BlendMode::Opaque              ? "Opaque" :
            desc.blend == RenderCore::BlendMode::AlphaBlend          ? "AlphaBlend" :
            desc.blend == RenderCore::BlendMode::AlphaTest           ? "AlphaTest" :
            desc.blend == RenderCore::BlendMode::Additive            ? "Additive" :
            desc.blend == RenderCore::BlendMode::AdditiveOneOne      ? "AdditiveOneOne" :
            desc.blend == RenderCore::BlendMode::AdditiveSrcAlphaOne ? "AdditiveSrcAlphaOne" :
            desc.blend == RenderCore::BlendMode::Multiply            ? "Multiply" : "?";
        mc2::fmt::print_err("[PIPELINE_BIND] {} depth={} cull={} frontFace={} polygonOffset={} blend={}\n",
            mc2::fmt::safe(dbgName), mc2::fmt::safe(df), mc2::fmt::safe(cm), mc2::fmt::safe(ff), mc2::fmt::safe(desc.polygonOffsetEnable ? "true" : "false"), mc2::fmt::safe(bm));
        std::fflush(stderr);
    }
}

} // namespace pipeline_binder
