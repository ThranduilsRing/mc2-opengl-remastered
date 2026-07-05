# SHADOW-CASTER-PIPELINE-REGISTRATION-RECON-1

**Type:** RECON (no build, no code change). Decides how the shadow-caster draw
passes become registered in the RenderCore PipelineId registry — the prerequisite
for modeling `polygonOffset` authoritatively in the PipelineKey (deferred here by
PIPELINEKEY-RASTERSTATE-FRONTFACE-AUTHORITY-1).

**VERDICT: GO, but as a DESCRIPTIVE-ONLY registration + an enable-bool — NOT an
applyPipeline reroute.** Register 3 new shadow PipelineIds with `PipelineDesc`
rows that state the truth (`pipelineDescRegistered=false`, like `StaticPropDepth`
today), add a 1-byte `polygonOffsetEnable` field, and check it. **Do not** route
shadow draws through `applyPipeline` and **do not** put offset factor/units in
the key — both are separate, higher-risk follow-ups. This first slice is
**zero live-GL change, zero visual risk** (metadata + checker only).

Scratch evidence: `.claude/SHADOWREG-RECON-passes.md`,
`.claude/SHADOWREG-RECON-datamodel.md`.

---

## The shadow-caster surface (what would be registered)

Caster draw functions set **NO** depth/cull/colormask — the **pass bracket**
(begin/end) owns those. Only **polygon offset** is set per-caster. **No
`glFrontFace` anywhere**; **cull is DISABLED** in the bracket; FBO is **pure
depth** (`DEPTH_COMPONENT24`, plus a dummy R8 color attachment for AMD FBO
completeness that is never written).

| Pass | Draw site | Program | Polygon offset | Depth | Cull | Color |
|---|---|---|---|---|---|---|
| **A** terrain→static map | `gameos_graphics.cpp:6120/6171` (`beginShadowPrePass`/`drawShadowBatchTessellated`) | `shadow_terrain` | **none** | test on, **func LESS**, write on | **disabled** | masked off (depth-only) |
| **B** static building→static map | `gos_static_prop_batcher.cpp:7588` (`drawStaticBuildingShadows`, gate default OFF) | `shadow_static_prop` | **2.0 / 4.0** (`:7639`→reset`:7678`+disable`:7679`) | same | disabled | depth-only |
| **C** dynamic prop→dyn map | `gos_static_prop_batcher.cpp:7723` (`drawDynamicPropShadows` / `:7388` flushShadow) | `shadow_static_prop` | **2.0 / 4.0** (`:7787`→reset`:7836`+disable`:7837`) | same | disabled | depth-only |
| **D** mech→dyn map | `gos_mech_batcher.cpp:865` (`GpuMechBatcher::flushShadow`) | `shadow_mech` | **none** | same | disabled | depth-only |
| ~~E legacy CPU blob~~ | `mclib/mech3d.cpp:3480` (`mechShadowShape->RenderShadows`, TGL) | — | — | — | — | **OUT OF SCOPE** (legacy fixed-function; foreign-WIP file) |

The legacy bracket `gos_postprocess.cpp:beginShadowPass:3118`/`:3141`→disable`:3152`
also sets the 2.0/4.0 offset. Bias magnitudes = `shadowBiasFactor_=2.0`/
`shadowBiasUnits_=4.0` (`gos_postprocess.h:104-105`, **ImGui-runtime-mutable**).

**CSM is NOT a pipeline axis:** the cascade loop (`mclib/txmmgr.cpp:2806-2814`,
count `mc2ShadowCsmCount()`, gate `MC2_SHADOW_CSM` default OFF) changes only
layer/viewport/matrix — same program + same state → **one PipelineId per caster
type, not per cascade.**

### Proposed PipelineId set (3 new)
- `ShadowTerrain` — `shadow_terrain`, offset OFF.
- `ShadowStaticProp` — `shadow_static_prop`, offset **ON** (covers B + C; same
  program + state, both enable offset).
- `ShadowMech` — `shadow_mech`, offset OFF.

`polygonOffsetEnable` is the **only** raster axis that differs across them — which
is exactly the authoritative fact worth capturing.

---

## Data-model decision (both recon agents converge)

**Model `polygonOffsetEnable` as a single `bool` in `PipelineDesc`; keep
factor/units OUT of the key (dynamic state).** This is the Vulkan
`VK_DYNAMIC_STATE_DEPTH_BIAS` analog and matches how `depthFunc`/`cull`/`frontFace`
are already modeled.

Why not the alternatives:
- **Grow PipelineDesc for float factor/units (+8 B):** blows the `<=20 B`
  `static_assert`, AND the magnitudes are **ImGui-runtime-mutable** → they cannot
  be a *static* key field anyway. Rejected.
- **Side-table (RasterExtra) keyed by PipelineId:** more machinery than a 1-byte
  bool buys; defer until there's real per-pipeline factor/units diversity.
- **Bool only (CHOSEN):** ~2 padding bytes remain in `PipelineDesc` after
  `frontFace`; a `bool polygonOffsetEnable` fits → **`sizeof<=20` still holds**.

### New enum gaps to resolve in the build slice
- **`DepthFunc::Less` is MISSING.** Shadow casters use **forward-Z `GL_LESS`**
  (scene is reverse-Z `GL_GEQUAL`); the `DepthFunc` enum today is
  `{LessEqual, GreaterEqual, Always, Equal}`. **Verify GL_LESS vs GL_LEQUAL at
  the bracket; if truly `GL_LESS`, add `DepthFunc::Less`** so the shadow rows
  state the truth. (Do not approximate with `LessEqual`.)
- `CullMode::None` already exists (shadow cull is disabled) — no new value.
- `colorAttachments = {false,false,false}` (depth-only), exactly like
  `StaticPropDepth`.

---

## RenderPass integration
`RenderPassId::Shadow = 4` **already exists** (`RenderCore/RenderPassContract.h:213`,
`pipelineDescRegistered=false`, already FIRST in `kFramePassOrder`). The 3 new
shadow PipelineIds map their `renderPassCompat` to `Shadow`. Flip
`pipelineDescRegistered=true` for Shadow only once a row is actually registered.

## Scope split (do these as SEPARATE slices, in order)

**Slice 1 — SHADOW-CASTER-PIPELINE-REGISTRATION-1 (recommended first, low risk):**
- Add `ShadowTerrain`/`ShadowStaticProp`/`ShadowMech` to `PipelineId` (+ `s_descs`
  rows, `static_assert` count), `pipelineDescRegistered=false` (DESCRIPTIVE).
- Add `bool polygonOffsetEnable` to `PipelineDesc` (fits padding; bump the
  `static_assert` comment, NOT the value).
- Add `DepthFunc::Less` if verified.
- `bindProgram` the 3 shadow programs at their link sites (metadata only).
- Schema: move `rasterState` `polygonOffset` from `gaps` → `authoritative_subaxes`
  (enable-bool only); add the 3 shadow pipelines with `polygonOffsetEnable`.
- Checkers: extend `check-pipeline-key.py` (mirror the frontFace block — each
  shadow pipeline declares `polygonOffsetEnable`, cross-check vs C++ row) and
  **`check-pipeline-desc.py` (add `polygonOffsetEnable` to `PIPELINE_DESC_FIELDS`
  in struct order — same trap that bit the frontFace slice).**
- **No `applyPipeline` change. Zero live-GL change.** Gate = build + checkers +
  adversarial; smoke optional (no behavior change).

**Slice 2 (LATER, HIGHER RISK) — SHADOW-APPLYPIPELINE-ROUTE-1:** actually drive
shadow fixed-function state through `applyPipeline`. **Requires smoke + visual
gate** (shadow acne, peter-panning, shadow disappearance on mc2_24 + mc2_01).
This is where the GLSTATE leak hazard lives (offset must be disabled after the
pass) and where `applyPipeline`'s "thin, no state tracking" charter is stressed.

## Hazards / DO-NOT
- **Do not route shadow draws through `applyPipeline` in slice 1** — that changes
  live GL and needs a visual gate. Slice 1 is descriptive registration only.
- **Do not put bias factor/units in `PipelineDesc`** — 8 B over budget + ImGui
  runtime-mutable. They stay dynamic state owned by the existing balanced hand-set
  sites (all reset to `(0,0)` + disable — no leak today).
- **Do not touch `mech3d.cpp`** (legacy CPU shadow path E; foreign-WIP file).
- **Verify `DepthFunc::Less` vs `LessEqual`** before adding — don't guess.
- Remember `check-pipeline-desc.py`'s positional field list — any new
  `PipelineDesc` field must be added there in struct order or `pipeline_desc`
  FAILs (the frontFace-slice merge trap).

## Deferred
applyPipeline rerouting (slice 2); bias factor/units modeling; legacy CPU shadow
(path E); vegetation shadow (none found — trees ride the prop registry); Vulkan.
