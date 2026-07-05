# REDUNDANT-PASS-HUNT-1 — ranked ledger + opus work queue

Slice: **REDUNDANT-PASS-HUNT-1** (lane `claude/controlmap-sample-1`, worktree
`A:/Games/mc2-controlmap-sample-1`). Follows the static-light win (9.6 MB/frame
PCIe re-upload eliminated via cost-split counters). User directive: *"see if there
are any other dumb redundant passes we are doing… lightweight, modern C++, Vulkan
render driven."*

Method = evidence-first hunt across four redundant-work classes: **re-upload**,
**re-render**, **re-compute**, **dead-pass**. This doc is the opus work queue.

## TL;DR — verdict

The frame is **already well-optimized**. The obvious big fish are dead:

- **Re-upload class**: the light SSBO (the static-light-win subsystem) is fully
  handled — grow-once + per-frame SubData + prefix/suffix split (`LIGHT-GROW-ONCE-
  SUBDATA-1`, `LIGHTSSBO-ORPHAN-1`, `gos_LightDataSsbo_UploadSplit`,
  `gameos_graphics.cpp:9039-9160`). Terrain height texture uploads once per mission
  (`gos_terrain_height_tex.cpp:102`, cache + re-upload only on ImGui factor change).
  Static-prop / mech batchers dirty-gate their SSBO writes (materialFlags change
  detection with "when changed", `gos_static_prop_batcher.cpp:5948-5985`). HDRI sky
  is loaded + sun-scanned once at mission load (`gos_hdri.cpp`).
- **Re-render class**: the static-caster terrain **shadow map is already built
  once per mission**, NOT per frame (`gameos_graphics.cpp:6388`
  "`static shadow build is once-per-mission (gos_StaticLightMatrixBuilt-gated)`").
  Water "reflection" is a static HDRI cubemap sample, not a mirrored-scene RT — no
  per-frame reflection render exists. Only the *dynamic* object shadow pass runs
  per frame, and it only rasterizes things that actually move (mechs/vehicles).
- **Re-compute class**: this is where the remaining fish were. Two landed (below).
  `Camera::inverseProject` matrix-inverse is mouse-pick-only (`camera.cpp:992`,
  called from `:604`), not per-frame — the "inverseProject fallback" the directive
  named is already fixed.
- **Dead-pass class**: `dead_gate_scan` finds only gate-registry cleanup (13 TIER_A
  gates, advisory, human-gated deletion) — no dead *render passes*. Separate lane.

## Wins LANDED (this slice)

Both are **re-compute-class** eliminations, **byte-identical** (env vars are
immutable per process ⇒ identical uniform values), **no gate** (pure cleanup,
matching the file's own established `static const … = [](){…}()` pattern), and
**build-verified** (`mc2.exe` built RelWithDebInfo, exit 0).

| # | Commit | Fix | Evidence |
|---|--------|-----|----------|
| 1 | `a54238be` | `gos_TerrainLodChunk_SubmitDrawCommands` resolved **~11 `getenv()` per frame** for mission-constant terrain debug/tuning gates (FORCE_COLOR, DIAG, CEMENT_DIAG_CONNECT, SLOPE_BIAS[_STRENGTH], CLIFF_TRIPLANAR[_STRENGTH], MACRO_VARIATION[_STRENGTH], EDGE_FEATHER[_STRENGTH]). Now resolved once at file scope. | `gos_terrain_lod_chunk.cpp` had the *correct* cached pattern 2 lines away (`s_v1Env`/`s_v2Env`) — these were the inconsistent outliers. `getenv` on Windows takes a CRT lock + linear-scans the environment block. |
| 2 | `b183a68c` | `mc2LightingDebugMode()` did `getenv()` + up to 11 `strcmp` **per call, called per frame from 5 hot draw sites** (terrain chunk, static-prop batcher ×2, gameos_graphics ×2). Now resolves once into a function-local `static`. | No runtime setter exists (`grep` found no `setenv`/`putenv`/ImGui path for `MC2_LIGHTING_DEBUG_VIEW`) ⇒ immutable ⇒ safe to cache. `gameos_graphics.cpp:6859`. |

**Cost note**: these are µs-scale per-frame savings (getenv ≈ 0.5–2 µs each under the
CRT lock; ~11–16 calls/frame eliminated ⇒ order ~10–30 µs/frame on the terrain +
lighting-debug paths). Not headline like the 9.6 MB PCIe win, but they are pure
redundant recompute, trivially safe, and remove the last per-frame `getenv` sites
from the hot terrain/object draw paths (bringing them in line with the disciplined
majority). **Recommended final gate**: tier1 smoke (deploy to a spare release dir,
NEVER `--kill-existing`) — deferred here to avoid contending with the concurrent
nifty mc2.exe; change is byte-identical by construction so smoke is a formality.

## Ledger — top ranked remaining candidates (opus specs)

Ranked by (value × confidence ÷ risk). Items 1–2 are structural and need real
measurement + parity gates; items 3+ are either already-good (documented for the
record) or low-ROI.

1. **[MEDIUM value / MEDIUM risk] Dynamic shadow pass — cull static-but-currently-
   still movers.** The per-frame dynamic shadow pass (`beginDynamicShadowPass`,
   `gameos_graphics.cpp:6602`; `txmmgr.cpp:2728`) rasterizes every dynamic object's
   shadow every frame. Movers that are *idle this frame* (not moving, not animating,
   sun static) re-render an identical shadow contribution. **Spec**: add a per-mover
   "shadow dirty" bit set on transform/pose/sun change; skip the depth draw for
   clean movers by reusing their prior shadow-map region — OR, cheaper and safer,
   fold idle-long-enough movers into the once-per-mission static shadow bake and
   promote back to dynamic on first motion. **Parity risk**: HIGH — sun drift, LOD
   pops, and the `markTerrainDrawn-revives-dead-passes` landmine (MEMORY) mean this
   needs a pixel-parity gate (mc2_24 combat, noise-floor-aware ≤3× baseline) before
   default-on. Measure first with a cost-split counter on `Shadow.DynObjectDirect`.

2. **[LOW-MEDIUM value / LOW risk] Coalesce per-packet alpha-test re-resolve loop.**
   `gos_static_prop_batcher.cpp:5948-5985` walks every packet each frame OR-reducing
   `materialFlags` and doing per-entry `glBufferSubData` only "when changed"
   (~10 µs/frame in mc2_10 per its own comment). The *scan* is per-frame even when
   nothing ever changes after the first destruction event. **Spec**: gate the whole
   scan behind a coarse "any destruction/texture-alpha event since last flush" flag
   (bdactor `setObjStatus` already knows when a damage texture loads); when the flag
   is clear, skip the packet walk entirely. Byte-identical while clean. Cheap,
   contained, no visual surface. Add a counter to confirm the scan is usually a no-op.

3. **[LOW value / LOW risk] `MC2_LIGHTING_DEBUG_VIEW` re-audit for a runtime toggle.**
   Win #2 cached it assuming immutability (verified: no setter today). If a future
   ImGui live-toggle for lighting debug views is wanted, add a `batcher_set…` setter
   + invalidation rather than reverting to per-frame getenv. Doc-only note.

4. **[INFO] Re-upload class is closed.** Documented above — light SSBO, terrain
   height, batchers, HDRI all dirty-gated / grow-once / load-time. No further
   re-upload wins found. Listed so opus does not re-hunt this class.

5. **[INFO] Re-render class is closed.** Static shadow bake is once-per-mission;
   no reflection RT. Only genuinely-moving geometry re-renders. Listed to prevent
   re-hunting.

6. **[LOW value] `debug_renderer.cpp` / `gpu_cull_readback.cpp` per-frame getenv.**
   Both have a few uncached `getenv` but are diagnostic-only TUs (behind `_TRACE`/
   `_DEBUG` gates that are default-OFF and rarely on). Not hot in default config.
   Cache-once if touching the file for another reason; not worth a standalone slice.

7. **[LOW value] SceneData UBO per-frame upload (`txmmgr.cpp:2469`).** Genuinely
   camera-dependent (cam_pos, fog vary per frame) and tiny (`sizeof(TG_HWSceneData)`).
   NOT redundant — do not "optimize". Listed to prevent a false-positive slice.

8. **[INFO] getenv discipline is otherwise excellent.** terrain_indirect
   (`g_lcsInit` cache-once with the exact comment "getenv per-call is slow"),
   mech_batcher (46 getenv, ALL file-scope `static const` one-time), postprocess
   (76 getenv, ALL in `init()`/`createFBOs()` — load/resize-time, not per-frame),
   static_prop_batcher (all behind `s_*Init` one-time flags). The two fixed sites
   were the only per-frame stragglers in the hot draw path.

9. **[SEPARATE LANE] Gate-registry cleanup (`dead_gate_scan` TIER_A, 13 gates).**
   `MC2_ABL_TRACE` (in `#if 0`), `MC2_DRAW_PACKET_STATIC_PROP_V6` (doc-only, no
   reader), `MC2_SHADOW_CSM_FULLMAP_LAST` (doc-only), `MC2_TERRAIN_LOD_CHUNK_BRIDGE_
   OBJBLOCK` (doc-only), plus ledger-REMOVED gates with lingering readers
   (`MC2_BLOOM`, `MC2_HDR_POST`, `MC2_TONEMAP_ACES`, etc.). This is source hygiene,
   not a per-frame perf win — belongs in a gate-cleanup lane, human-gated deletion.

10. **[INFO / Vulkan-prep] The two fixes reduce per-frame CRT/OS surface** (no
    per-frame `getenv` on the terrain/object draw paths), which is aligned with the
    "Vulkan render driven" direction — a native command-buffer recording path wants
    zero per-frame environment queries. No further action; noted for direction.

## Biggest single finding

Not a new win — a **confirmation**: the frame's obvious redundant-work classes
(static-light re-upload, static-shadow re-render, reflection RT) are **already
eliminated** by prior arcs. The static shadow map being built once-per-mission
(`gameos_graphics.cpp:6388`) is the load-bearing fact — it means the only remaining
re-render redundancy is *idle dynamic movers* (ledger item 1), which is a
measurement-gated slice, not a slam-dunk. The re-compute class held the last easy
fish (the two getenv sites), now landed.

## Commits

- `a54238be` — perf(terrain): cache per-frame terrain getenv gates once
- `b183a68c` — perf(render): cache mc2LightingDebugMode env resolve

Both `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
