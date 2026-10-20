// RenderCore/PipelineRegistry.cpp
//
// Static table of PipelineDesc entries, indexed by PipelineId.
// Add new rows here whenever PipelineRegistry.h gains a new PipelineId value;
// the static_assert below will fire if the table and enum fall out of sync.
//
// GL state encoded in this table (applied by pipeline_binder::applyPipeline):
//   depthTestEnable / depthWriteEnable / depthFunc=GreaterEqual (reverse-Z)
//   blend=Opaque or AlphaTest (GL_BLEND stays disabled for both)
//   cullMode=Back
// SSBO slots still bound explicitly in flush() (slot 2, 0, 1) — ssboBindingsMask
// documents them but does not drive binding; that is a future automation pass.
//
// AlphaTest note: static prop alpha is shader-discard (binary cutout).
// GL_BLEND stays disabled and GL_DEPTH_MASK stays GL_TRUE for both passes —
// the "alpha" split drives texture-array selection, not GL blend state.
//
// Object-ID note: color2 / objectIdWriteEnabled are false here. The static
// prop fragment shaders do not yet declare layout(location=2). Flip both
// flags when that shader output is added.

#include "PipelineRegistry.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <string>

namespace RenderCore {

// SSBO binding slots — bit N = slot N. See PipelineDesc.h for the full table.
static constexpr uint32_t kSsboInstances = 1u << 0;   // slot 0 — per-type instance data
static constexpr uint32_t kSsboColors    = 1u << 1;   // slot 1 — per-type color data
static constexpr uint32_t kSsboPerType   = 1u << 2;   // slot 2 — hot-color s_perTypeSsbo

static constexpr uint32_t kStaticPropSsbos = kSsboInstances | kSsboColors | kSsboPerType;

// Table indexed by static_cast<size_t>(PipelineId).
// Row 0 must be the Invalid sentinel (zeroed PipelineDesc).
static std::array<PipelineDesc, static_cast<size_t>(PipelineId::Count_)> s_descs = {{

    // [0] Invalid — zeroed sentinel; returned for bad lookups.
    PipelineDesc{},

    // [1] StaticPropOpaque
    // Renders the alpha-OFF texture-array group. Full depth, no blending.
    {
        /* blend               */ BlendMode::Opaque,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,
        /* depthFunc           */ DepthFunc::GreaterEqual, // reverse-Z throughout MC2
        /* cullMode            */ CullMode::Back,
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,           // TODO: flip when shader adds loc=2
        /* frontFace           */ FrontFace::Ccw,  // GL default; explicit per row
        /* polygonOffsetEnable */ false,           // scene passes: no polygon offset
        /* drawBuffers         */ DrawBufferSet::MainSceneMRT,
        /* ssboBindingsMask    */ kStaticPropSsbos,
    },

    // [2] StaticPropAlphaTest
    // Renders the alpha-ON texture-array group via shader discard.
    // GL blend stays disabled; depth write stays on (binary cutout writes depth).
    {
        /* blend               */ BlendMode::AlphaTest,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,            // intentional: discard ≠ blend
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::Back,
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,  // GL default; explicit per row
        /* polygonOffsetEnable */ false,           // scene passes: no polygon offset
        /* drawBuffers         */ DrawBufferSet::MainSceneMRT,
        /* ssboBindingsMask    */ kStaticPropSsbos,
    },

    // [3] MechOpaque — GPU mech batcher (GpuMechBatcher::flush). Mirrors the
    // fixed-function state the batcher previously set by hand: full depth
    // (test+write, reverse-Z GEQUAL), no blend, cull back. Color0 + GBuffer1
    // (normal/screen-shadow); object-ID (loc=2) is GLSL-macro-gated in mech.frag
    // (MC2_OBJECT_ID_BUFFER), not an applyPipeline attachment toggle, so
    // objectIdWriteEnabled stays false here (descriptive; applyPipeline does not
    // reconfigure draw buffers). ssboBindingsMask = 0: the mech batcher binds its
    // own SSBOs (instance/bone/material/lights) manually; the mask is metadata
    // and does not bind SSBOs. The mech sampler bind stays
    // manual (no PipelineDesc sampler field).
    {
        /* blend               */ BlendMode::Opaque,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,
        /* depthFunc           */ DepthFunc::GreaterEqual, // reverse-Z
        /* cullMode            */ CullMode::Back,
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,           // macro-gated in shader, not here
        /* frontFace           */ FrontFace::Ccw,  // GL default; explicit per row
        /* polygonOffsetEnable */ false,           // scene passes: no polygon offset
        /* drawBuffers         */ DrawBufferSet::MainSceneMRT,
        /* ssboBindingsMask    */ 0u,              // mech binds its own SSBOs
    },

    // [4] StaticPropDepth — camera depth-prepass. Depth-only: GEQUAL + write so
    // it lays the nearest reverse-Z depth; alpha discard in static_prop_depth.frag.
    // Color writes are masked off by the caller (glColorMask), NOT by attachment
    // changes — same FBO stays bound. Shares the static-prop SSBOs (instance /
    // per-type / per-draw) because it reuses static_prop.vert.
    {
        /* blend               */ BlendMode::AlphaTest, // discard path; GL_BLEND off
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,
        /* depthFunc           */ DepthFunc::GreaterEqual, // reverse-Z, lay nearest
        /* cullMode            */ CullMode::Back,
        // IMPORTANT-2: depth-only — no color attachments written. The caller masks
        // color via glColorMask (not attachment reconfig; same FBO stays bound), so
        // this row advertises the true write set: nothing color, depth only.
        /* colorAttachments    */ { false, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,  // GL default; explicit per row
        /* polygonOffsetEnable */ false,           // scene passes: no polygon offset
        /* drawBuffers         */ DrawBufferSet::Unspecified,
        /* ssboBindingsMask    */ kStaticPropSsbos,
    },

    // [5] TerrainOverlay — cement/perimeter overlay (terrain_overlay.{vert,frag}).
    // TERRAIN-OVERLAY-DECAL-PIPELINE-REGISTRATION-1: DESCRIPTIVE — states the truth
    // (gosRenderer::drawTerrainOverlays/drawDecalStaticBatch set this by hand) but
    // is descriptive pipeline intent. Opaque, reverse-Z GEQUAL,
    // depth-write on, cull DISABLED (overlay tiles draw both faces). frag writes
    // color0 (FragColor) + color1 (GBuffer1).
    {
        /* blend               */ BlendMode::Opaque,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ true,
        /* depthFunc           */ DepthFunc::GreaterEqual, // reverse-Z (scene-space overlay)
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,           // overlay: no polygon offset (in-material blend)
        /* drawBuffers         */ DrawBufferSet::MainSceneMRT,
        /* ssboBindingsMask    */ 0u,              // binds its own VBO/texture; no SSBO
    },

    // [6] TerrainDecal — bomb craters + mech footprints (terrain_overlay.vert +
    // decal.frag). DESCRIPTIVE; gosRenderer::drawDecals sets this by hand. The
    // FIRST AlphaBlend pipeline row: GL_BLEND on, SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    // (BlendMode::AlphaBlend), depth-test on but depth-WRITE OFF (decals must not
    // occlude), reverse-Z GEQUAL, cull disabled. frag writes color0 + color1.
    {
        /* blend               */ BlendMode::AlphaBlend, // FIRST alpha-blend row
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ false,           // decals do not write depth
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, true, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* drawBuffers         */ DrawBufferSet::MainSceneMRT,
        /* ssboBindingsMask    */ 0u,
    },
    // [7] VfxTubeAlpha — tube_ribbon, SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    {
        /* blend               */ BlendMode::AlphaBlend,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ false,
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* drawBuffers         */ DrawBufferSet::SingleColor0,
        /* ssboBindingsMask    */ 0u,
    },
    // [8] VfxTubeAdditive — tube_ribbon, ONE/ONE (DIFFERS from billboard/mesh; schema-exact)
    {
        /* blend               */ BlendMode::AdditiveOneOne, // ONE/ONE (differs from billboard/mesh)
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ false,
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* drawBuffers         */ DrawBufferSet::SingleColor0,
        /* ssboBindingsMask    */ 0u,
    },
    // [9] VfxMeshAlpha — vfx_mesh, SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    {
        /* blend               */ BlendMode::AlphaBlend,
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ false,
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* drawBuffers         */ DrawBufferSet::MainSceneMRT,
        /* ssboBindingsMask    */ 0u,
    },
    // [10] VfxMeshAdditive — vfx_mesh, SRC_ALPHA/ONE (schema-exact)
    {
        /* blend               */ BlendMode::AdditiveSrcAlphaOne, // SRC_ALPHA/ONE (same as billboard)
        /* depthTestEnable     */ true,
        /* depthWriteEnable    */ false,
        /* depthFunc           */ DepthFunc::GreaterEqual,
        /* cullMode            */ CullMode::None,
        /* colorAttachments    */ { true, false, false },
        /* objectIdWriteEnabled*/ false,
        /* frontFace           */ FrontFace::Ccw,
        /* polygonOffsetEnable */ false,
        /* drawBuffers         */ DrawBufferSet::MainSceneMRT,
        /* ssboBindingsMask    */ 0u,
    },
}};

static_assert(
    s_descs.size() == static_cast<size_t>(PipelineId::Count_),
    "s_descs row count must match PipelineId::Count_. "
    "Add a row for every new PipelineId value.");

// Returned for Invalid / out-of-range lookups.
static const PipelineDesc s_nullDesc{};

const PipelineDesc& getPipelineDesc(PipelineId id) {
    const auto idx = static_cast<size_t>(id);
    if (idx == 0u || idx >= static_cast<size_t>(PipelineId::Count_))
        return s_nullDesc;
    return s_descs[idx];
}

// SPIRV-MECHOPAQUE-PIPELINEKEY-INTEGRATION-1: per-PipelineId logical variant key.
static std::array<std::string, static_cast<size_t>(PipelineId::Count_)> s_variantKeys;

void recordPipelineVariantKey(PipelineId id, const char* variantKey) {
    const auto idx = static_cast<size_t>(id);
    if (idx == 0u || idx >= static_cast<size_t>(PipelineId::Count_))
        return;
    s_variantKeys[idx] = variantKey ? variantKey : "";
}

const char* getPipelineVariantKey(PipelineId id) {
    const auto idx = static_cast<size_t>(id);
    if (idx == 0u || idx >= static_cast<size_t>(PipelineId::Count_))
        return "";
    return s_variantKeys[idx].c_str();
}

// VERTEXLAYOUT-AUTHORITY-1: canonical layout-name table, indexed by
// VertexLayoutId. MUST stay lockstep with the enum's "// layout:" comments and
// the pipeline-key schema (cross-checked by scripts/check-pipeline-key.py).
static constexpr const char* kVertexLayoutNames[] = {
    /* [0] Invalid          */ "",
    /* [1] StaticProp40B    */ "static_prop_40B",
    /* [2] MechGpuVertex48B */ "mech_GpuMechVertex_48B",
};
static_assert(
    sizeof(kVertexLayoutNames) / sizeof(kVertexLayoutNames[0]) ==
        static_cast<size_t>(VertexLayoutId::Count_),
    "kVertexLayoutNames must have one entry per VertexLayoutId value.");

const char* vertexLayoutName(VertexLayoutId id) {
    const auto idx = static_cast<size_t>(id);
    if (idx == 0u || idx >= static_cast<size_t>(VertexLayoutId::Count_))
        return "";
    return kVertexLayoutNames[idx];
}

// Per-PipelineId vertex-layout identity. Value-initialized to Invalid (0).
static std::array<VertexLayoutId, static_cast<size_t>(PipelineId::Count_)> s_vertexLayouts;

void recordPipelineVertexLayout(PipelineId id, VertexLayoutId layout) {
    const auto idx = static_cast<size_t>(id);
    if (idx == 0u || idx >= static_cast<size_t>(PipelineId::Count_))
        return;
    s_vertexLayouts[idx] = layout;
}

VertexLayoutId getPipelineVertexLayout(PipelineId id) {
    const auto idx = static_cast<size_t>(id);
    if (idx == 0u || idx >= static_cast<size_t>(PipelineId::Count_))
        return VertexLayoutId::Invalid;
    return s_vertexLayouts[idx];
}

} // namespace RenderCore
