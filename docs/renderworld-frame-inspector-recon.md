# RenderWorld Frame Inspector — Recon (spec, no code)

RECON ONLY. Question: what frame-introspection already exists, and what would a
full frame inspector need (current pass, render target, draw counts by family,
visible terrain chunks, static-prop batches, mech instances, VFX counts,
depth/blend mode, FBO, viewport)?

Pairs with: [mcp-render-state.md](mcp-render-state.md),
[debug_state_schema.md](debug_state_schema.md),
[render-binding-registry.md](render-binding-registry.md),
[editor-debug-overlay-phase2.md](editor-debug-overlay-phase2.md),
the per-family `*-rv-arc-recon.md` docs, and `docs/renderpass-contract-spec.md`.

---

## (a) Existing introspection inventory

The headline finding: **most of a frame inspector already exists** — three
layers, all read-only:

### Layer 1 — RenderSnapshot (per-frame extraction, always-on)

- `GameOS/gameos/render_snapshot.h:148-265` — `struct RenderSnapshot`: frame
  index, mech packets + 8 mismatch counters, static-prop per-draw-slot packets
  (`ExtractedStaticPropPacket`, render_snapshot.h:114-122: sortedSlot,
  typeId, pipelineId, materialIdx, instanceCount, texArrayLayer), v2/v2.2/v2.3
  counters, `ok` hard gate, `TerrainPassFacts` (render_snapshot.h:128-141:
  viewId, legacyProgramId, drawCallCount, tess/overflow/viewUniforms flags).
- Extraction: `ExtractRenderSnapshot()` in `render_snapshot.cpp`, called once
  per frame between game logic and draw (`gameosmain.cpp` ~1300); per-frame
  log line gated `MC2_RENDER_SNAPSHOT_LOG=1` (`render_snapshot.cpp:671-673`).
- Pass-fact promotion happens in `gameosmain.cpp:1411-1437`
  (TERRAIN-PASS-PACKET-0): fills `snap.terrainPass` from
  `TerrainPatchStream::getLastFlush*()` accessors + `gos_get*ProgramId()`
  free functions.

### Layer 2 — EditorInspector pass spines (in-game ImGui, `MC2_IMGUI` builds)

`GuiRuntime/EditorInspector.h` — per-pass snapshot structs filled per frame in
`gameosmain.cpp` and rendered in the in-game **Object Inspector** window
(`EditorInspector::drawImGui()`, `EditorInspector.cpp:220`, called from
`GuiRuntime::Render()`; plus a **Renderer Features** window at
`EditorInspector.cpp:227-240` backed by `RenderCore/RendererFeatureRegistry.h`):

| Spine | Struct | Fill site | Contents |
|---|---|---|---|
| Terrain | `TerrainPassSnapshot` (EditorInspector.h:68-83) | gameosmain.cpp:1440-1462 | 4 program ids, bucket/vert/thinRec/recipe counts, overflow, viewId/name, tess flag |
| Shadow | `ShadowPassSnapshot` (EditorInspector.h:97-115) | gameosmain.cpp:1466+ (SHADOW-SPINE-0) | 3 shadow program ids, shadowsEnabled, map sizes, mech+staticProp caster types/instances drawn |
| VFX | `VfxPassSnapshot` (EditorInspector.h:121-149) | gameosmain.cpp (VFX-SPINE-0) | particle program id, gates, SSBO capacity/budget, overflow, process-lifetime emit/flush totals, debugMode |
| Selection | `StaticPropInspectorData` / `MechInspectorData` / `TerrainInspectorData` (EditorInspector.h:28-64) | ctrl-shift-click pick bridge (EditorInspector.h:88-91) | per-object drilldown incl. per-packet rows (gameosmain.cpp:1370-1407 `DrawPacketPropRow`) |

### Layer 3 — MC2_DEBUG_STATE_DUMP JSON + mc2-render-state MCP

- `GameOS/gameos/debug_state_dump.cpp` (374 lines) — writes
  `debug_state/latest_render_state.json` frame 1 then every 300 frames, plus
  8-slot history ring (`maybeWriteRenderState`, debug_state_dump.cpp:344-369).
  Schema `MC2_DEBUG_STATE_V1` ([debug_state_schema.md](debug_state_schema.md)):
  mission, build, feature gates, engineView (viewId, viewMode, **viewport**),
  renderSnapshot ok+counters, staticPropOpaque visual globals, mech section
  with up to 32 per-instance packets (debug_state_dump.cpp:241-300).
- MCP server `mc2-render-state` ([mcp-render-state.md](mcp-render-state.md)):
  `get_render_state/health/feature_gates/visual_settings/history/frame_info`,
  `validate_state`, plus capture-baseline wrappers. Validator
  `scripts/check-debug-state-json.py`.

### Per-family counters/telemetry (existing accessors)

| Family | Counter source | file:line |
|---|---|---|
| Terrain patch stream | `getLastFlushBucketCount/VertCount/ThinRecCount/RecipeCount`, `wasLastFlushOverflowed` | `GameOS/gameos/gos_terrain_patch_stream.h:157-160` |
| Terrain chunk path (default-ON) | `[TerrainLOD prod]` printf: frame, objBlocks, objVerts, solidWindow | `mclib/terrain.cpp:3432-3435`; chunk draw `gos_TerrainLodChunk_SubmitDrawCommands` (`gos_terrain_lod_chunk.h:29`) — **no programmatic visible-chunk accessor exposed** (s_blockMeta internal) |
| Static props | `batcher_getDrawSlotCount`, `batcher_getPacketDrawInfo`, `batcher_getPacketMaterialFlags` (used at gameosmain.cpp:1380-1396); per-slot instanceCount in `ExtractedStaticPropPacket`; shadow caster counts in `ShadowPassSnapshot` |
| Mechs | `batcher_getLastFlushSubmitCount()` (`gos_mech_batcher.h:218`); per-instance `mech.packets[]` in dump (objectIdRaw, texHandle, materialIdx, renderFlags) when `MC2_SNAPSHOT_MECH_EXTRACT=1` |
| VFX | anonymous-namespace lifetime counters in particle batcher surfaced via `VfxPassSnapshot` (emitTotal, flushTotal, recordsFlushedTotal, recordsPerFlushMax, trail counts) — **no per-frame counts** (EditorInspector.h:133-135 comment); `MC2_FX_COUNT_LOG` per-site GOSFX counter (`mclib/mech3d.cpp:65-69`) |
| Frame timing | `MC2_HITCH_TRACE` (`GameOS/gameos/mc2_hitch_trace.cpp:31`) + MISSION/MIF/GEOM_PHASE splits; Tracy zones per pass (`gameos_graphics.cpp:2738,3303,3523,3869,4065,4200,4313`) |
| Pass identity (static) | `RenderCore::RenderPassId` enum + `kRenderPassContracts` (`RenderCore/RenderPassContract.h:46-141`): StaticPropOpaque, Terrain, MechOpaque, Shadow, VFX — **compile-time contract table, no runtime "current pass" tracker** |
| View/viewport | `RenderCore::getCurrentView()` EngineView (viewId, viewMode, viewport[4], ViewUniforms binding=3) — already in dump |
| Editor overlay | `editor/EditorDebugOverlay.{h,cpp}` — chunk/superchunk grid + stats panel (editor-only, [editor-debug-overlay-phase2.md](editor-debug-overlay-phase2.md)) |

---

## (b) Gap table — desired field vs what exists

| Desired field | Exists? | Gap |
|---|---|---|
| Current pass | PARTIAL | `RenderPassId` enum exists but nothing sets a runtime "current pass" — need a thread-local `RenderCore::beginPass(RenderPassId)` marker |
| Current render target / FBO | NO | FBOs are private members of gosPostProcess (`gos_postprocess.cpp:542,938,1044…` sceneFBO_/bloomFBO_/ssaoFBO_/hzbFBO_); no tracker. Need a `currentFbo` shadow var set at each glBindFramebuffer site (or glGet query at snapshot points) |
| Draw counts by family | PARTIAL | terrain buckets ✓, mech submits ✓, static-prop slots ✓; no unified per-frame `{family → drawCalls, instances}` rollup struct |
| Visible terrain chunks | NO (printf only) | `[TerrainLOD prod]` objBlocks printed (terrain.cpp:3432) but not accessor-exposed; chunk path needs `gos_TerrainLodChunk_GetLastFrameStats()` (drawn chunks, LOD histogram, skirts) |
| Static prop batches | YES | `staticPropPackets` span + counts in snapshot; per-slot detail in inspector |
| Mech batch instances | YES (gated) | `MC2_SNAPSHOT_MECH_EXTRACT=1`, capped at 32 packets in dump |
| VFX counts (per-frame) | NO | only lifetime aggregates; needs per-frame emit/flush counters (noted out-of-scope in VFX-SPINE-0) |
| Depth/blend mode | NO | no GL state mirror; the 10.3 transparency saga (chunk driver inheriting glDepthMask) is exactly the failure class this would catch. GlStateGuard is the planned-but-not-started owner |
| Viewport | YES | `engineView.viewport` in dump + inspector |
| Current view mode | YES | `engineView.viewMode` + `MC2_VIEWMODE_FRAMEWORK` ([viewmode-capture-matrix.md](viewmode-capture-matrix.md)) |

---

## (c) Delivery modes compared

| Mode | Pros | Cons |
|---|---|---|
| 1. In-game ImGui panel (extend Object Inspector / new "Frame Inspector" window in `EditorInspector.cpp`) | Live, interactive, all spine plumbing exists; works in game AND editor (GuiRuntime shared) | Human-only; not agent-checkable; needs MC2_IMGUI build |
| 2. stdout `[RENDERWORLD_FRAME v1]` line (pattern of `MC2_RENDER_SNAPSHOT_LOG`, `[TerrainLOD prod]`) | grep-able in smoke logs; cheapest; tier1-gateable | per-frame spam, no structure, no history; another ad-hoc log grammar |
| 3. Artifact JSON via MC2_DEBUG_STATE_DUMP (add `framePasses` section to `MC2_DEBUG_STATE_V1`→`V2` or additive) | Agent-checkable via existing mc2-render-state MCP tools + validator; history ring free; integrates with capture baselines | 300-frame cadence (not per-frame); schema/validator must be updated in lockstep |

**Recommendation: 3 + 1 from one collector.** Define a single
`FramePassStats` struct filled per pass; the JSON dump and the ImGui panel are
both downstream consumers (exactly the TERRAIN-PASS-PACKET-0 pattern where the
inspector consumes `snap.terrainPass`). Skip mode 2 except for a single
optional env-gated summary line for smoke-gate use
(`MC2_RENDERWORLD_FRAME_LOG=1`), reusing the collector.

---

## (d) Data collection points per pass (where the hook lives)

A `RenderCore::beginPass(RenderPassId)/endPass()` pair (thread-local current
pass + per-pass stats slot, indexed by `RenderPassId`, reset each frame in
`ExtractRenderSnapshot`) inserted at:

| Pass | Insertion site |
|---|---|
| Shadow | gosPostProcess shadow render (`gos_postprocess.cpp` shadow FBO bind ~:542 region) + `flushShadow()` lanes in `gos_mech_batcher.cpp` / `gos_static_prop_batcher.cpp` (caster counts already harvested by SHADOW-SPINE-0) |
| Terrain (chunk) | `gos_TerrainLodChunk_SubmitDrawCommands` (`gos_terrain_lod_chunk.cpp:443`) — also where explicit depth/blend state is set (10.3 fix), so capturing depth/blend here is one glGet-free read of what we just set |
| Terrain (patch/water/mask streams) | `TerrainPatchStream::flush` (counts already exist, gos_terrain_patch_stream.h:157-160); water fast path `gameos_graphics.cpp:2738`; mask passes `:3869/:4065`; indirect `:3523` |
| StaticPropOpaque | `gos_static_prop_batcher.cpp` flush/dispatch (snapshot build v6 already counts slots/instances) |
| MechOpaque | `gos_mech_batcher.cpp` flush (`batcher_getLastFlushSubmitCount`, gos_mech_batcher.h:218) |
| VFX | particle bridge flush (`gos_particle_bridge.cpp`, SSBO binding 14) — add per-frame records-flushed counter beside the lifetime ones |
| Postprocess/composite | each `glBindFramebuffer` in `gos_postprocess.cpp` (:542 scene, :630 bloom, :661 ssao, :684 godray, :719 waterRefl, :1044 hzb, :794/:1095 default) — record `{passId, fboId, fboDebugName}` |

Snapshot assembly stays in the existing seam: `gameosmain.cpp:1411+` after
`ExtractRenderSnapshot()`, alongside TERRAIN/SHADOW/VFX-SPINE fills.

---

## (e) Integration with visual regression + telemetry cockpit

- **Visual baselines** ([visual-baseline-howto.md](visual-baseline-howto.md),
  `scripts/capture_baseline.py`): a baseline PNG whose sidecar JSON also embeds
  the `framePasses` block gives "pixels + pass stats" pairs — a pixel diff can
  be triaged instantly (did drawCallCount/chunk count change?). The dump dir is
  already read by `run_capture_baseline` / `summarize_latest_capture` MCP tools.
- **MCP cockpit**: add one MCP tool `get_frame_passes` (reads the new section);
  `get_render_health` gains optional informational fields, NOT in ok gate
  (matches TerrainPassFacts precedent, render_snapshot.h:131 "NOT included in
  ok gate").
- **Tier1 smokes** ([tier1_env_vars.md](tier1_env_vars.md)): optional
  `MC2_RENDERWORLD_FRAME_LOG` line lets the smoke gate assert e.g.
  "terrain chunks drawn > 0", replacing bespoke FASTPATH_DROP-style probes.
- **ViewMode matrix** ([viewmode-capture-matrix.md](viewmode-capture-matrix.md)):
  per-mode captures get pass-stat deltas for free (e.g. ObjectIdDebug should
  not change draw counts).

---

## (f) Build order

1. **`FramePassStats` substrate** — struct + per-frame array in
   `render_snapshot.h/.cpp` (informational, outside ok gate);
   `RenderCore::beginPass/endPass` + thread-local current pass on
   `RenderPassContract.h`'s existing enum. No consumers yet.
2. **Cheap fills first (accessors already exist)** — terrain patch stream,
   mech submit count, static-prop slot count, shadow caster counts: wire into
   the new struct at the gameosmain.cpp:1411+ seam.
3. **Chunk-terrain stats accessor** — `gos_TerrainLodChunk_GetLastFrameStats()`
   (chunks drawn, culled, LOD histogram) replacing printf-only telemetry;
   capture depth/blend booleans where the driver sets them.
4. **FBO/render-target tracking** — debug-name registry + current-FBO shadow
   var at the gos_postprocess.cpp bind sites (defer full GL state mirror to
   GlStateGuard, which memory says is intentionally NOT started yet — do not
   front-run it; record only what each pass explicitly sets).
5. **JSON dump section** — `framePasses` in debug_state_dump.cpp + schema doc
   + `check-debug-state-json.py` update (additive, keep `MC2_DEBUG_STATE_V1`
   or bump with validator in same commit).
6. **ImGui "Frame Inspector" tab** in EditorInspector.cpp consuming the same
   struct (per-pass table: pass, FBO, draws, instances, depth/blend).
7. **MCP tool + smoke line** — `get_frame_passes`; optional
   `MC2_RENDERWORLD_FRAME_LOG` summary line; baseline sidecar embed.
8. **Per-frame VFX counters** — last (needs a new counter in the particle
   batcher; explicitly deferred by VFX-SPINE-0).
