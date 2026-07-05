# PIPELINEKEY-RASTERSTATE-AUTHORITY-RECON-1

**Type:** RECON (no build, no code change). Scopes promoting the PipelineKey
`rasterState` axis (currently `status: MISSING` in `pipeline-key-schema.json`)
toward authoritative, the way VERTEXLAYOUT-AUTHORITY-1 promoted `vertexLayout`.

**VERDICT: PARTIAL GO — split the axis.** `frontFace` + `cullMode` are safely
promotable now (closed enums, the 4 registered pipelines are all GL-default,
zero behavior change). `polygonOffset` / `depthBias` / `polygonMode` are NOT —
their only real consumers (shadow casters, terrain decals) are **un-registered
passes**, and depth bias is a **shader** axis, not GL raster state. Defer those
to when shadow/decal passes are onboarded to `PipelineDesc`.

Scratch evidence: `.claude/RASTER-RECON-frontface-cull.md`,
`.claude/RASTER-RECON-polyoffset-depthbias.md`.

---

## Answers to the 7 recon questions

### 1. Where is frontFace set?
**Exactly ONE `glFrontFace` site:** `gameos_graphics.cpp:6038` — the shadow-pass
state *restore* (round-trips a captured value; snapshot at `:5990`/`:6021`).
**Winding is never authored** — it stays GL-default **`GL_CCW` process-wide**.
Legacy fixed-function (`gameos_graphics.cpp:5445-5450`) encodes per-shape winding
NOT via `glFrontFace` but by **flipping the culled face** (a CCW shape →
`glCullFace(GL_FRONT)`). `applyPipeline` (`pipeline_binder.cpp`) **never touches
frontFace**. → Winding is a **leaked global (always CCW)**, not pipeline-local.

### 2. Is cullMode complete in PipelineDesc?
**Yes, for cull.** All 4 registered rows are `CullMode::Back`
(`PipelineRegistry.cpp:53/68/91/108`); `applyPipeline` (`pipeline_binder.cpp:64-74`)
fully drives `glEnable/glDisable(GL_CULL_FACE)` + `glCullFace` from
`PipelineDesc.cullMode`. Neither batcher hand-sets cull *outside* applyPipeline:
mech has only a save@`gos_mech_batcher.cpp:1981` / restore-epilogue@`:2353`
around its `applyPipeline(MechOpaque)`@`:2013`; static-prop uses
`applyPipeline`@`:5138`/`:5375` with no hand cull (reset via state-cache
invalidate). **`frontFace` is the one cull-family axis applyPipeline omits.**

### 3. Where do polygon offset / depth bias live?
- **Polygon offset = SHADOW ONLY**, 4 live GL sites, **all balanced** (set→reset→
  disable, no leak): post-fx shadow `gos_postprocess.cpp:3118`/`:3141`→disable
  `:3152`; static-building shadow `gos_static_prop_batcher.cpp:7639`→reset`:7678`+
  disable`:7679`; dynamic-prop shadow `:7786`→reset`:7836`+disable`:7837`.
  Factor/units = `shadowBiasFactor_=2.0`/`shadowBiasUnits_=4.0`
  (`gos_postprocess.h:104-105`, ImGui-tunable, **runtime-mutable**, no env gate).
  No `GL_POLYGON_OFFSET_LINE`; no `glPolygonMode` (fill is implicit default).
- **Depth bias = SHADER, compile-time const**, single-sourced
  `shaders/include/terrain_depth_bias.hglsl:43-47` + C++ mirror
  `mclib/terrain_depth_bias.h:64-75` (`clip.z += K*clip.w`): TERRAIN −0.002,
  OVERLAY +0.00005, WATER_FAST −0.00375. `terrain_overlay.vert:31` notes it
  *replaced* an old `glPolygonOffset(-1,-1)`. `gl_FragDepth` bias only at
  `gos_terrain.frag:1082` (undisplaced special path). `static_prop.vert` has **no
  fudge** (true depth, wins naturally). **No runtime-uniform depth bias anywhere.**

### 4. Which registered pipelines need non-default raster state?
**None.** All 4 (StaticPropOpaque/AlphaTest/Depth, MechOpaque) = GL default
**CCW + cull-back + no polygon offset + fill**. The polygon-offset users (shadow
casters) and depth-bias users (terrain/overlay/water) are **not registered
pipelines**. `PipelineDesc.h:50-71` has **no** frontFace/polygonOffset/depthBias
field today; schema already flags `rasterState` MISSING.

### 5. Do shadow / static-prop passes depend on ad-hoc raster state?
- **Static-prop color**: NO ad-hoc raster — fully via `applyPipeline`.
- **Mech color**: cull via `applyPipeline`; a save/restore epilogue guards the
  surrounding frame but sets no raster itself.
- **Shadow passes** (the static-building & dynamic-prop shadow casters at
  `gos_static_prop_batcher.cpp:7639/7786` and post-fx `gos_postprocess.cpp:3118`):
  **YES, ad-hoc** — they set polygon offset by hand (balanced) and are **not in
  the PipelineId registry**. This is the real raster-state surface, and it's
  outside the promotable set until those passes get `PipelineDesc` rows.

### 6. RasterStateId vs explicit fields?
**Explicit fields, NOT an interned `RasterStateId`.** Rationale:
- The promotable axes are tiny closed enums (`FrontFace{Ccw,Cw}`, `CullMode`
  already exists) — same shape as the existing explicit `blend`/`cullMode`/
  `depthFunc` fields. An intern-table buys nothing when all 4 pipelines share one
  trivial raster state.
- **Size is free:** `PipelineDesc` is exactly 20 B with **3 padding bytes at
  offsets 13–15** (between `objectIdWriteEnabled`@12 and the 4-byte-aligned
  `ssboBindingsMask`@16). A `FrontFace` (uint8) + `polygonOffsetEnable` (bool)
  drop into that padding with **zero struct growth** — the
  `static_assert(sizeof(PipelineDesc) <= 20)` still holds.
- A full `glPolygonOffset(factor,units)` would need 8 B (two floats) and would
  blow the budget — **don't add it**; the registered set is offset-OFF, so a
  single `polygonOffsetEnable=false` bool fully describes them. Reconsider a
  `RasterStateId` intern-table only when shadow casters (with real factor/units,
  and runtime-mutable ImGui bias) are registered — that's the first true raster
  diversity.

### 7. Can a checker catch missing raster state without changing GL behavior?
**Yes — check-time only, identical to the `pipeline_desc` / `vertexLayout`
pattern.** Extend `scripts/check-pipeline-key.py` to (a) require each registered
pipeline declare `frontFace` + `cullMode` in the schema, (b) cross-check them
against the `PipelineDesc`/`PipelineRegistry.cpp` rows (parse the table), and
(c) FAIL on missing/stale/drift. Optionally a C++ `static_assert` that every
registered row's `frontFace == Ccw` (current invariant). The checker never
issues a GL call, so it cannot change rendering.

---

## Recommended build-slice shape (PIPELINEKEY-RASTERSTATE-AUTHORITY-1)

1. Add `enum class FrontFace : uint8_t { Ccw, Cw };` + a `FrontFace frontFace;`
   field (+ optional `bool polygonOffsetEnable;`) to `PipelineDesc` — into the
   free padding; verify `sizeof<=20` still asserts.
2. Set all 4 registry rows `frontFace = Ccw`, `polygonOffsetEnable = false`
   (current behavior, explicit).
3. `applyPipeline`: add `glFrontFace(frontFace==Cw?GL_CW:GL_CCW)`. **Behaviorally
   a no-op** (global is already CCW), but makes winding pipeline-local.
4. Schema: `rasterState` MISSING → **PARTIAL** (frontFace + cullMode authoritative;
   residual gap = `polygonOffset`/`depthBias`/`polygonMode`, owned by unregistered
   passes). Add per-pipeline `frontFace`/`cullMode` to `registered_pipelines[]`.
5. Checker: enforce frontFace/cull present + matching the C++ rows; missing/drift
   FAIL (mirror the VERTEXLAYOUT-AUTHORITY-1 checker block).
6. Gate: build green + `pipeline_key`/`pipeline_desc` PASS + adversarial FAILs.
   **Smoke recommended this time** (unlike vertexLayout) because step 3 issues a
   real `glFrontFace` — confirm no winding regression (mc2_24 mech + shadow).

## Hazards / DO-NOT
- **DO NOT touch the legacy face-flip trick** (`gameos_graphics.cpp:5445-5450`).
  Legacy fixed-function encodes winding via culled-face flip; it must keep
  working independently of the new `frontFace` field.
- **DO NOT pull polygon offset factor/units into PipelineDesc** (8 B, blows the
  20 B budget; consumers unregistered; ImGui-runtime-mutable → would force a
  mutable key field). Leave it as the unregistered-shadow-pass gap.
- **DO NOT model depth bias as raster state** — it's a single-sourced **shader**
  axis (`terrain_depth_bias.hglsl` + C++ mirror); it has its own lockstep.
- Only runtime-variable raster in the whole engine = `shadowBias*` (ImGui). Any
  future shadow-pass key field must treat it as runtime-mutable, not a frozen id.

## Deferred (out of this slice)
Shadow-caster / terrain-decal `PipelineDesc` onboarding (prereq for promoting
polygonOffset); `polygonMode` (only a future DebugWireframe pipeline needs it);
a `RasterStateId` intern-table (only once raster diversity is real).
