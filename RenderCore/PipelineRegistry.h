// RenderCore/PipelineRegistry.h
//
// Named pipeline IDs and their PipelineDesc lookup.
// Not consumed by live render paths yet — exists so DrawPacket emitters
// can carry a typed identity instead of pipelineId=0.
//
// Lifecycle:
//   1. Engine reads getPipelineDesc(id) for static GL-state contracts
//      (blend, depth, cull, attachments, ssboBindingsMask) at any time.
//   2. Renderer calls bindProgram(id, glName) once during GL init to wire
//      the actual program object into the table.
//   3. DrawPacket::pipelineId = static_cast<uint32_t>(PipelineId::Foo).
//
// Extending: add enum values before Count_, then add a matching row to
// the s_descs initializer in PipelineRegistry.cpp. The static_assert in
// that file will catch any mismatch at compile time.

#pragma once

#include <cstdint>
#include "PipelineDesc.h"

namespace RenderCore {

enum class PipelineId : uint32_t {
    Invalid = 0,
    StaticPropOpaque = 1, // opaque geometry, alpha-off group
    StaticPropAlphaTest = 2, // alpha-tested geometry, alpha-on group (shader discard)
    MechOpaque = 3, // GPU mech batcher opaque pass (reverse-Z, cull back)
    StaticPropDepth = 4, // camera depth-prepass (depth-only, alpha discard)
    TerrainOverlay = 5, // terrain cement/perimeter overlay (terrain_overlay.frag, opaque)
    TerrainDecal = 6, // bomb craters + mech footprints (decal.frag, alpha blend, depth-write OFF)
    VfxTubeAlpha = 7, // tube_ribbon, SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    VfxTubeAdditive = 8, // tube_ribbon, ONE/ONE  (differs from billboard/mesh)
    VfxMeshAlpha = 9, // vfx_mesh, SRC_ALPHA/ONE_MINUS_SRC_ALPHA
    VfxMeshAdditive = 10, // vfx_mesh, SRC_ALPHA/ONE
    Count_ = 11,
};

// VERTEXLAYOUT-AUTHORITY-1: stable repo-owned vertex-input layout identities —
// the VAO half of a PSO key. Each value names a fixed, finite vertex layout used
// by a registered pipeline. The canonical layout-name token in the trailing
// "// layout: <name>" comment is AUTHORITATIVE: scripts/check-pipeline-key.py
// parses it and cross-checks it against the pipeline-key schema's per-pipeline
// vertexLayoutId/vertexLayout. Renaming a value or its layout string without
// updating the schema (or vice versa) FAILS the pipeline_key gate.
enum class VertexLayoutId : uint32_t {
    Invalid          = 0,
    StaticProp40B    = 1,   // layout: static_prop_40B (gos_static_prop_batcher.cpp:2310-2323)
    MechGpuVertex48B = 2,   // layout: mech_GpuMechVertex_48B (gos_mech_batcher.cpp:1376-1382)
    Count_           = 3,   // sentinel — do not use as an ID
};

// Canonical layout-name string for id (the same token recorded in the
// pipeline-key schema). Returns "" for Invalid / out-of-range.
const char* vertexLayoutName(VertexLayoutId id);

// Return the static GL-state contract for id.
// glProgramName is 0 until bindProgram() is called for that id.
// Out-of-range or Invalid returns a zeroed sentinel (all false, all 0).
const PipelineDesc& getPipelineDesc(PipelineId id);

// Wire in the actual GL program object name from the renderer at GL init.
// Must be called before any DrawPacket using this id is dispatched.
// No-op (with assert) if id is Invalid or out-of-range.
void bindProgram(PipelineId id, uint32_t glProgramName);

// SPIRV-MECHOPAQUE-PIPELINEKEY-INTEGRATION-1: record the selected logical
// shader-variant identity (the canonical define-set key, e.g.
// "mech|MC2_OBJECT_ID_BUFFER=1;MC2_USE_VIEW_UNIFORMS=1") for this pipeline id,
// into the pipeline-key path. Derived from the program PREFIX, so it is the SAME
// whether the stages were GLSL-compiled or SPIR-V-specialized (the SPIR-V branch
// lives per-stage BELOW the bound program and cannot fork this accounting).
// Empty until recorded; safe to call once at program build.
void recordPipelineVariantKey(PipelineId id, const char* variantKey);
const char* getPipelineVariantKey(PipelineId id);

// VERTEXLAYOUT-AUTHORITY-1: record the vertex-input layout identity bound for
// this pipeline id, into the pipeline-key path. Promotes vertexLayout from a
// descriptive doc string to a recorded + checked axis. Pure metadata — does NOT
// touch GL state, the VAO, or any draw. The MechOpaque PipelineKey is now
// (shaderVariantId + vertexLayoutId). Invalid until recorded.
void recordPipelineVertexLayout(PipelineId id, VertexLayoutId layout);
VertexLayoutId getPipelineVertexLayout(PipelineId id);

} // namespace RenderCore
