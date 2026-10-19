// RenderCore/PipelineDesc.h
//
// Description of the fixed-function pipeline requirements a draw call
// requires. Header-only; no runtime dispatch yet.
//
// Purpose: name the implicit render contracts so future DrawPacket sorting
// and editor debug rendering have a typed handle to build cache keys from.
// Do NOT swap live render paths to use this type until DrawPacket dispatch
// is wired (a future slice).
//
// BlendMode values are intentionally identical to
// render_contract::PassStateContract::BlendMode so callers can cast freely.
// A static_assert enforcing this lives in mclib/render_contract.cpp when
// MC2_RENDER_CONTRACT_ASSERT is active.
//
// SSBO binding slots referenced by ssboBindingsMask (bit N = slot N):
//   8  = SUBSTRATE         (gpu_cull_substrate.cpp)
//   9  = DEBUG             (gpu_cull_compute.cpp)
//  14  = READBACK          (gpu_cull_readback.h)
//  16  = BASE_INSTANCE     (gos_static_prop_batcher.cpp)
//  20  = LIGHT_DATA        (gameos.hpp / lighting.hglsl)
//
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md §6

#pragma once

#include <cstdint>

namespace RenderCore {

// Values 0..3 (Opaque/AlphaBlend/AlphaTest/Additive) MUST stay value-identical to
// render_contract::PassStateContract::BlendMode (enum-parity static_asserts in
// render_contract_pipeline.h). `Additive` (value 3) is a LEGACY COARSE value
// (== glBlendFunc(ONE,ONE)) retained ONLY for the render-contract bridge; it
// cannot distinguish the two additive blend funcs the engine actually uses.
// BLENDMODE-ADDITIVE-VOCABULARY-1 adds the precise variants used by the pipeline
// registry — do NOT use the coarse `Additive` in new registered PipelineDesc rows
// (the pipeline-key checker rejects it; use AdditiveOneOne / AdditiveSrcAlphaOne).
//   AlphaBlend          = SRC_ALPHA / ONE_MINUS_SRC_ALPHA
//   AdditiveOneOne      = ONE / ONE                 (e.g. tube_ribbon)
//   AdditiveSrcAlphaOne = SRC_ALPHA / ONE           (e.g. particle_billboard, vfx_mesh)
enum class BlendMode : uint8_t {
    Opaque              = 0,
    AlphaBlend          = 1,
    AlphaTest           = 2,
    Additive            = 3,   // legacy coarse (ONE/ONE); render-contract bridge only
    AdditiveOneOne      = 4,   // ONE / ONE
    AdditiveSrcAlphaOne = 5,   // SRC_ALPHA / ONE
    Multiply            = 6,   // DST_COLOR / ZERO (post-fx darkening: shadow/AO/shoreline)
};

enum class CullMode  : uint8_t { None, Back, Front };

// Front-face winding. GL default is Ccw (GL_CCW); MC2 has used the ambient
// process-wide default and never authored it (only the shadow pass
// save/restores it). PIPELINEKEY-RASTERSTATE-FRONTFACE-AUTHORITY-1 makes it an
// explicit per-pipeline row so registered pipelines stop depending on leaked
// global state. NOTE: the legacy fixed-function path encodes winding by flipping
// the CULLED FACE (not glFrontFace) — that path is untouched by this field.
enum class FrontFace : uint8_t { Ccw, Cw };

// Depth comparison function. Matches GL_LESS / GL_GEQUAL etc. without
// pulling in GL headers. LessEqual is the conventional forward-Z default;
// GreaterEqual is the reverse-Z default used throughout MC2. Less = GL_LESS,
// used by the shadow-caster passes (forward-Z shadow maps, opposite the scene's
// reverse-Z) — see gameos_graphics.cpp:6155/6311/6400, gos_postprocess.cpp:3114.
enum class DepthFunc : uint8_t { LessEqual, GreaterEqual, Always, Equal, Less };

// Which GL_COLOR_ATTACHMENTx slots must be non-NONE in the active FBO.
// Mirrors render_contract::RequiredAttachments; kept separate to avoid an
// mclib → rendercore include cycle.
struct ColorAttachmentMask {
    bool color0;   // GL_COLOR_ATTACHMENT0 — albedo / HDR scene color
    bool color1;   // GL_COLOR_ATTACHMENT1 — GBuffer normal + post-shadow mask
    bool color2;   // GL_COLOR_ATTACHMENT2 — R32_UINT object ID (M1.5+)
};

// DRAWBUFFER-OWNERSHIP-1: the draw-buffer set this pass renders into, made an explicit
// per-pass field instead of being inherited implicitly from whatever the previous pass
// left bound (the "draw to SingleColor but assume MRT" latent risk). DESCRIPTIVE: this
// is NOT emitted by applyPipeline — the recon ruling keeps draw-buffer binding out of
// applyPipeline, and the gos_postprocess setSceneDrawBuffers() chokepoint stays the
// single live authority. check-drawbuffer-ownership.py keeps this field in lockstep with
// colorAttachments, the live call sites, and the render_pass_attachment.h contract table.
enum class DrawBufferSet : uint8_t {
    Unspecified = 0,   // not classified, or depth-only with caller-managed color mask
    MainSceneMRT,      // {COLOR0, COLOR1[, COLOR2]} — setSceneDrawBuffers(MainSceneMRT)
    SingleColor0,      // {COLOR0} only            — setSceneDrawBuffers(SingleColor)
    ShadowDepthOnly,   // depth-only shadow FBO (dummy color attachment)
    Backbuffer,        // default framebuffer (FBO 0)
};

struct PipelineDesc {
    BlendMode           blend;
    bool                depthTestEnable;
    bool                depthWriteEnable;
    DepthFunc           depthFunc;   // applied by applyPipeline(); GreaterEqual = reverse-Z
    CullMode            cullMode;

    ColorAttachmentMask colorAttachments;
    // True when the fragment shader declares layout(location=2) out uint
    // for object-ID. Distinct from colorAttachments.color2: a pass can
    // require color2 bound without writing it (terrain does this).
    bool                objectIdWriteEnabled;

    // Front-face winding for this pipeline (applied by applyPipeline() via
    // glFrontFace). All registered pipelines are Ccw (the GL default) today;
    // the field exists so the row states the truth instead of relying on
    // ambient global state. Occupies one of the 3 padding bytes before
    // ssboBindingsMask — no struct growth.
    FrontFace           frontFace;

    // Whether GL_POLYGON_OFFSET_FILL is enabled for this pipeline. ENABLE/DISABLE
    // only — the factor/units MAGNITUDES are intentionally NOT modeled here: they
    // are runtime-mutable (ImGui shadowBiasFactor_/Units_, gos_postprocess.h) and
    // would blow the 20-byte budget, so they remain dynamic state (the Vulkan
    // VK_DYNAMIC_STATE_DEPTH_BIAS analog). Only the shadow-caster prop pipeline
    // sets this true today. SHADOW-CASTER-PIPELINE-REGISTRATION-1. Occupies a
    // padding byte before ssboBindingsMask — no struct growth.
    bool                polygonOffsetEnable;

    // DRAWBUFFER-OWNERSHIP-1: declared draw-buffer set (see DrawBufferSet above).
    // Descriptive metadata; occupies the last remaining padding byte before
    // ssboBindingsMask — no struct growth.
    DrawBufferSet       drawBuffers;

    // Bit N set → SSBO binding slot N is required. Covers slots 0-31.
    // See binding table in the file header above.
    uint32_t            ssboBindingsMask;
};

// P9 removed the unused driver program name. The remaining fields describe
// render intent; native pipelines own their executable shader handles.
static_assert(sizeof(PipelineDesc) <= 20,
              "PipelineDesc must stay small; it lives in hot-path cache entries.");

} // namespace RenderCore
