<!-- doc-status: current | reviewed:2026-10-11 -->
# Pipeline-key schema — PIPELINE-KEY-SCHEMA-1

> **Schema + checker only. No Vulkan, no SPIR-V consumer, no PSO/cache impl, no
> shader/runtime/material change.** Data: [`pipeline-key-schema.json`](pipeline-key-schema.json).
> Gate: `scripts/check-pipeline-key.py` (in `check-contracts.sh` as `pipeline_key`).

## VERDICT: **GO (schema)** · **DEFER (implementation)**

The first repo-owned logical `PipelineKey` model. Every registered pipeline can
produce a **stable PARTIAL key** from already-banked contracts. A real keyed PSO
cache waits on a SPIR-V consumer (next arc).

## The PipelineKey model + field authority

| Field | Status | Source (banked contract) | Vk mapping |
|---|---|---|---|
| `shaderVariantId` | DESCRIPTIVE | SHADER-PERMUTATION-INVENTORY-1 OFFLINE_VARIANT set + runtime `glProgramName` + `unique_name_suffix_` | `pStages` (SPIR-V) |
| `specializationParams` | DESCRIPTIVE | inventory SPECIALIZATION_CONSTANT (`MC2_SHADOW_CSM_MAX:int`, `READBACK_SSBO_BINDING:int`) | `VkSpecializationInfo` |
| `vertexLayout` | DESCRIPTIVE | per-shape VAO (recon §3): static_prop 40B, mech 48B | `pVertexInputState` |
| `renderPassCompat` | **AUTHORITATIVE** | RENDER-PASS-DAG-CONTRACT-1 (`RenderPassContract.h`) | `renderPass`/`subpass` |
| `materialLayout` | PARTIAL | sampler/binding-occupancy + material-gpu-mirror | `VkPipelineLayout` (descriptors) |
| `depthState` | **AUTHORITATIVE** | `PipelineDesc.depth{Test,Write,Func}` | `DepthStencilState` |
| `blendState` | PARTIAL | `PipelineDesc.blend` (factor) | `ColorBlendAttachmentState` |
| `cullState` | PARTIAL | `PipelineDesc.cullMode` | `RasterizationState.cullMode` |
| `rasterState` | **MISSING** | (not modeled) | `RasterizationState` (frontFace/polyOffset/polyMode) |
| `colorMaskAttachmentMask` | PARTIAL | `PipelineDesc.colorAttachments` | RT attachments + `colorWriteMask` |

**Authoritative now (safe to key on):** `depthState`, `renderPassCompat`,
`materialLayout` ABI (binding/sampler/material — individually banked),
blend *factor*, cull *mode*, attachment mask.
**Descriptive (finite but no repo-owned id yet):** `shaderVariantId`,
`specializationParams`, `vertexLayout`, per-pipeline descriptor grouping.
**Missing (must add to `PipelineDesc` before a real PSO):** `rasterState`
(`frontFace`, `polygonOffset`), `blendEquation`, per-channel `colorWriteMask`.

## Partial keys for the 4 registered pipelines

| Pipeline | variant macros | depth | blend | cull | attach (0/1/2) |
|---|---|---|---|---|---|
| `StaticPropOpaque` | COALESCE, OBJECT_ID, USE_VIEW_UNIFORMS, PBR_SLOTS | GEQUAL T/T | Opaque | Back | T/T/F |
| `StaticPropAlphaTest` | + ALPHA_TEST (discard) | GEQUAL T/T | AlphaTest | Back | T/T/F |
| `MechOpaque` | OBJECT_ID, USE_VIEW_UNIFORMS | GEQUAL T/T | Opaque | Back | T/T/F |
| `StaticPropDepth` | COALESCE, PBR_SLOTS | GEQUAL T/T | AlphaTest | Back | F/F/F |

All variant macros are governed by the shader inventory (the checker enforces it).

## Answers to the slice questions
- **Can every registered pipeline produce a stable logical key?** YES — partial:
  the authoritative core (depth / cull-mode / blend-factor / attachment-mask /
  render-pass-compat) + a finite, inventory-governed `shaderVariantId`.
- **Authoritative vs descriptive?** See the status column above.
- **Excluded draw families?** The 9 ad-hoc raster passes (Terrain, Shadow, VFX,
  Water, PostProcess, VegetationCards, TerrainDecal, TerrainOverlay, UI) +
  2 SSBO/`gl_VertexID`-driven families (particle/VFX-trail, GPU-indirect terrain
  solid). They set FF state ad-hoc and have no `PipelineDesc` row — must be
  onboarded to the registry before they can carry a key.
- **Smallest pilot family for eventual PSO validation?** **`MechOpaque`** — one
  program, one fixed VAO (`GpuMechVertex` 48B), already `pipelineDescRegistered`,
  already in `reflect.py` SHADER_VARIANTS, smallest variant set (`OBJECT_ID ×
  USE_VIEW_UNIFORMS` = 4). (Runner-up: `StaticPropOpaque` — reflected but ≤16 variants.)

## Checker (`check-pipeline-key.py`)
Cross-validates schema ↔ shader-inventory ↔ `PipelineRegistry.h`:
1. every per-pipeline variant macro exists in the shader inventory (no phantom variant);
2. every specialization param is typed;
3. every key field declares an allowed status (missing PSO fields are explicit, not silent);
4. schema's pipeline set == non-Invalid `PipelineId` enum (no stale/missing row).

Current tree: **PASS** (10 fields, 4 pipelines, 0/0). Planted phantom-macro,
untyped-spec-param, and missing-pipeline all **FAIL** (proven).

## Next (not started)
`SPIRV-CONSUMER-PILOT-RECON-1` or `OFFLINE-SHADER-VARIANT-BUILD-1` — the schema is
banked; the remaining wall is the runtime `makeShader` having no precompiled-SPIR-V
path. Bank a SPIR-V consumer + offline variant bake before promoting the descriptive
fields (`shaderVariantId`, `vertexLayout`) to authoritative and building a real cache.

## Exclusions honored
No Vulkan, no SPIR-V consumer, no shader edits, no `makeShader` change, no material
M4 unify, no pipeline-cache impl, no RenderDevice. Foreign WIP untouched.
