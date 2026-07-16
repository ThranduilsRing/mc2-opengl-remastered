# VK-OPT-BACKLOG — ranked optimization backlog (render + upload path)

## ✅ MEASURED 2026-07-10 — VK-DRAW-PERF-1 phase 1 (GPU-timestamp breakdown, `721088fa`)
Added `MC2_VK_GPU_TIMESTAMPS` (default-OFF, `vk_gpu_timestamps.h`) — real GPU
`vkCmdWriteTimestamp` brackets on the terrain + mech island render passes. Strict
native stack (`run_vk_nointerop_strict_visible.py`, 800x600, EMA over the run):

| mission | island | patches/runs | cpu_draw_us | gpu_pass_us | gpu_setup_us | gpu_draw_us |
|---|---|---|---|---|---|---|
| mc2_01 | terrain | 20 patches / 35 draws | 1400.1 | 1031.5 | 5.5 | **1026.0** |
| mc2_01 | mech | 38 inst / 25 runs | 133.2 | 17.5 | 0.6 | **16.9** |
| mc2_24 | terrain | 32 patches / 71 draws | 1222.2 | 821.9 | 5.1 | **816.8** |
| mc2_24 | mech | 122 inst / 60 runs | 299.3 | 25.8 | 0.6 | **25.2** |

**VERDICT — terrain is GPU FRAGMENT-bound.** GPU pass = 67-73% of CPU draw_us
(so NOT CPU-submit-bound); setup/clear is negligible (5µs); the whole GPU cost is
the draw segment over only 20-32 non-overlapping patches / trivial vertex count →
fill/fragment, not vertex, not bind. At 800x600 (480k px) terrain gpu_draw≈1026µs ≈
2ns/px; scaling to 1920x1080 (~4.3×) gives ≈4400µs — **exactly the reported baseline
draw_us**, confirming resolution-linear fragment cost.

**CORRECTION — mech is NOT GPU-draw-bound.** Mech gpu_draw = 17-25µs even at 122
instances / 14814 verts. The "mech ~5000µs draw" premise does NOT reproduce as GPU
draw; mech per-frame CPU cost lives in `stage_us` (instance/bone/light SSBO build+
upload: 150µs mc2_01, 522µs mc2_24) + submit/fence-wait, not the GPU pass. The mech
draw-cost lever was a misdiagnosis.

**No parity-safe fragment fix exists.** The VK terrain frag is byte-matched to GL:
dynamic CSM 8 taps (= `terrain_lod_chunk.frag:900`), static 16 taps (= `:899`), POM
already carries GL's early-exit `break`. Cutting any of these DIVERGES from GL and
violates the parity mandate ("shading change must hold bookmark MADs"). GL pays the
same fragment cost — this is the features-not-perf tax, not a VK regression. Phase 2
correctly ships NO fix (measure-first prevented a parity-violating cut).

**Honest next lever (future, higher-risk, out of this slice):** each island does its
own `vkQueueSubmit` + `vkWaitForFences` (CPU-blocks on GPU completion) before the
next island records — a serial per-island sync (~370µs terrain, ~274µs mech CPU
submit/wait). Collapsing the direct-chain into fewer submits / async fences is a
structural multi-slice change to the ownership scheduler, not a smallest-verifiable
slice, and the "record-submit at floor" ruling stands for the submit itself.


## ✅ SHIPPED 2026-07-10 — VK-TERRAIN-STAGING-PERF-1 (`62707331`)
Strict native stack dominator was **NOT** a staging/upload cost — it was a
mis-gated fallback: `MC2_VK_SHADOW_DIRECT_CHAIN` left the VK shadow island's
producers declining (`useInterop=false`), so the terrain island fell back to a
per-frame GL CSM `glGetTexImage` readback (8192^2 x layers). Fixed by gating the
shadow producers on `(useInterop || directChain)`.
**mc2_01 before→after: stage_us 126815 → 3.0, avg_fps 7.4 → 79.0.** copy stats
static/dyn/csm all <1us. **Next island dominator = `draw_us`** (terrain ~4400us,
mech ~5000us) = GPU draw/submit, not upload. See VK-ARC-STATE.md 2026-07-10.

**Read-only recon, 2026-07-03.** Worktree `A:/Games/mc2-vk-interop-bridge` @ `19cf4fb8`
(branch `claude/vk-interop-bridge-1`). Every item below is parity-gateable against the golden
bookmarks: `tests/visual/bookmarks/*.json` + `scripts/run_golden_parity.py` +
`tests/visual/golden-sets.json` (blessed sets `baselineA-head-*`). Diff floor per
`docs/render-backend-seams/visual-tolerance-policy.json`.

## CRITICAL CONTEXT — what is ALREADY shipped (do not re-mine)

Per `docs/render-perf-snapshot.md` (refreshed 2026-06-04) the big terrain/static-prop/light
wins are **DONE**: static-prop snapshot Fill dirty-only (−97%, `cf654080`), static light upload
split (224µs→7µs, `STATICPROP-PERMANENT-INSTANCE-LIGHTS-1`), registry-flush cached-blob
(`b9d20373`), dynamic-shadow dirty-only (192µs→40µs, `b4dbaf3c`), rain batch (448µs→1 draw,
`12ee1205`), quadSetupTextures retire (1.01ms→0.6µs, `6ecace0a`), water GPU full-recipe cull
(`b988e415`), VertexProjectLoop retirement, mech MaterialGpu persistent + dirty-gated
(`26974734`), thin-canary readback gated off. **Those are closed — the backlog below is the
*remaining* live surface, which is smaller and more specialized.**

The largest un-retired CPU masses (`MapData::makeLists` ~0.24–0.5ms, `slimReduce` ~0.48ms,
`GameLogic.Units.TerrainObjects` ~1.43ms) are the **camera-sliding-window terrain cull** and are
explicitly ruled **hands-off until the GPU-cull lifecycle port** (`MC2_GPU_CULL_LIFECYCLE`,
paused lane) — they are per-frame by nature (`memory/terrain_quadlist_is_camera_sliding_window.md`).
They appear below as the one large strategic item, not a quick win.

---

## RANKED — most ROI first

### ⭐ #1 QUICK WIN — Particle tube-ribbon: per-frame CPU vec4 expansion + 4 unfenced full uploads
- **Category:** 2 (CPU loop → GPU/shader) + 1 (redundant per-frame upload) + 4 (class-D no fence)
- **Evidence:** `GameOS/gameos/gos_particle_bridge.cpp:811-817` (per-frame `pos3→vec4` scalar
  expansion loop over `numVerts`, `std::vector<float>` alloc **every frame per record**),
  then `:819-831` four back-to-back `glBufferSubData` (pos/col/uv/idx) with **no fence, no ring,
  no dirty-gate** — buffers are class-D `s_tubePosSsbo`/`s_tubeColSsbo`/… in
  `buffer-lifetime-ledger.md` (IMPLICIT-SYNC, flagged "freshest highest-touch risk").
  **Tracy zone:** `gos.TubeRibbon.FlushDeferred` (`gos_particle_bridge.cpp:702`).
- **Why wasteful:** (a) the vec4 widen is fixed-function CPU work + a heap alloc per record per
  frame; the shader already reads std430 vec4 — feed vec3 and widen in the VS, or keep a persistent
  scratch. (b) full re-upload of geometry that is often unchanged between frames (same ribbon
  record). (c) class-D implicit-sync = NVIDIA stall risk (the exact stall the light path fixed with
  orphaning at `gameos_graphics.cpp:9084`).
- **ROI:** Hot on rain/effect-heavy missions (mc2_17). Cheap. Substitutive.
- **Parity gate:** `mc2_17.json` (rain/effects) + `mc2_01_combat.json` (weapon FX ribbons). Geometry
  identical → byte-exact expected; fence/orphan is a no-op visually.
- **Effort:** S. **Path:** GL now (ships pre-VK); the persistent-buffer + explicit-barrier shape is
  also the Vulkan-native model. **Automatable:** yes — a per-frame upload-bytes counter on the
  tube SSBOs + a "same-record-hash re-upload" detector guards the dirty-gate fix.

### #2 — Light-data whole-range SubData under grow-once (residual per-frame upload)
- **Category:** 1 (redundant per-frame upload)
- **Evidence:** `gos_LightDataSsbo_UploadGrowOnce` / `gos_LightDataSsbo_UploadSplit`
  (`GameOS/gameos/gameos_graphics.cpp:9101-9152`). Under `LIGHT-GROW-ONCE-SUBDATA-1` the split
  optimization is **subsumed** (`:9114`) and the whole `usedBytes` live range is `glBufferSubData`'d
  every frame (`:9128`/`:9145-9149`). The static prefix `[0..S)` is immutable (proven permanent
  slots, `STATICPROP-PERMANENT-INSTANCE-LIGHTS-1`) yet is re-shipped when grow-once is the active
  path. **Tracy zone:** `RenderLists.LightDataUpload`.
- **Why wasteful:** the split path (prefix uploaded once/dirty-only) already proved the prefix is
  static — but grow-once *disables* the prefix skip (`:9138` comment: "prefixDirty skip is
  disabled"). Two default-on optimizations partially cancel; the immutable prefix rides every frame.
- **ROI:** Moderate — prefix can be the majority of `usedBytes` on building-dense maps; the split
  already recovered ~217µs so the residual is the un-skipped prefix under grow-once. Measure which
  path is actually default-active at wolfman before sizing.
- **Parity gate:** `mc2_01.json` + `mc2_24.json` (building-dense) — light values unchanged → byte-exact.
- **Effort:** M (reconcile the two upload strategies so prefix stays skipped under grow-once — needs
  persistent-store + dirty-range, not orphan). GL now. **Automatable:** yes — the existing
  `MC2_LIGHTBAKE_PARITY`/`MC2_LIGHTBAKE_STABILITY` counters already prove prefix stability; add a
  "prefix bytes uploaded/frame" counter to catch the redundant ship.

### #3 — GPU-cull readback R-A-W stall (open hazard) + C2 readback SSBO
- **Category:** 4 (sync stall) / 3 (data round-tripping CPU that could stay GPU)
- **Evidence:** `gpu_cull_compute.cpp` readback SSBO slot 14 (`buffer-lifetime-ledger.md` /
  `gpu-buffer-owner-recon-1.md` §"GPU cull compute"), `glGetNamedBufferSubData` consumer. MEMORY
  flags an **open "island readback R-A-W sync hazard"** (HANDOFF vulkan L5) and the snapshot lists
  `MC2_GPU_CULL_READBACK` default-ON with **substitutive proof still PENDING**. Consumer:
  `readback_isActorVisibleLagged()` gating `recalcBounds()` in `code/terrobj.cpp`.
- **Why wasteful:** BAR/PCI-E readback of visibility bits every frame to drive a CPU cull gate; the
  same visibility already lives GPU-side (`s_actorVisBuf` atomicOr). This is the classic "data that
  could live on GPU but round-trips CPU." The prior GOM.readbackSnapshot BAR-read was already killed
  (SHADOW-COPY-1, 3.6ms→360ns) — this is the sibling that wasn't.
- **ROI:** Potentially high (BAR reads are the worst stalls) but **the substitutive win is unproven**
  — needs a user-driven Tracy at wolfman to confirm the readback is actually costing, per the
  snapshot's own honesty constraint. Size before committing.
- **Parity gate:** `mc2_24.json` (many actors) — cull decision must stay identical; the lagged gate
  already tolerates staleness so parity is on *pixels*, not timing.
- **Effort:** M/L. **Path:** VK-native leaning (the interop bridge's fenced readback is the right
  home); a GL shadow-copy interim (mirror the SHADOW-COPY-1 pattern) ships now. **Automatable:**
  partially — a readback-bytes/frame counter + the existing motion_tolerance counter.

### #4 — Class-D implicit-sync buffers: terrain-mask indirect cmd bufs (Vulkan-blocking + stall risk)
- **Category:** 4 (missing barrier/fence) — correctness + VK-prep
- **Evidence:** `buffer-lifetime-ledger.md` class-D rows: `s_indirectCmdBuf` /
  `s_waterIndirectCmdBuf` (terrain-mask) in `gameos_graphics.cpp`, `s_ssbo`/`s_tubePosSsbo`
  (particle). All "IMPLICIT-SYNC" — no fence, no ring, no explicit barrier. The checker
  (`scripts/check-buffer-lifetime-ownership.py`) already enforces they stay *declared* but not fixed.
- **Why wasteful:** on NVIDIA these are latent CPU-block-until-GPU stalls (same class the light
  orphan fixed) and they are hard blockers for the Vulkan backend (implicit sync doesn't exist in
  VK). Not a frame-cost win on AMD today, but a **correctness + VK-enablement** item.
- **ROI:** Low frame-cost on the dev AMD box; **high VK-unblock value.** Bundle with #1 (particle is
  the same class).
- **Parity gate:** all tier1 bookmarks (`mc2_01/03/10/17/24.json`) — adding a barrier/orphan is
  visually inert → byte-exact.
- **Effort:** S each. **Path:** VK-native prep (LIGHT-BUFFER-BARRIER-VERIFY-1 /
  PARTICLE-BUFFER-LIFETIME-1 already queued in the ledger's "Open" section). **Automatable:** yes —
  the ledger checker is the guard; extend it to require an explicit barrier/orphan for class-D.

### #5 — Terrain camera-window CPU cull (makeLists + slimReduce) → GPU-cull lifecycle port
- **Category:** 2 (large per-vertex CPU loops → compute) + 3 (window rebuilt CPU-side each frame)
- **Evidence:** `MapData::makeLists` (~0.24–0.5ms, snapshot row) rebuilds vx/vy + pVertex over
  ~16–40K verts/frame; `Terrain::geometry slimReduce` (`terrain.cpp:1607`, ~0.48ms) per-vertex
  `onScreenR` angular cull over ~40K verts → the load-bearing object cull gates.
  **Tracy zones:** `MapData::makeLists`, `Terrain::geometry slimReduce`.
- **Why wasteful:** ~0.7–1ms combined per-vertex CPU work feeding cull gates that a compute pass
  could produce; the terrain GPU dispatch is already world-indexed and does NOT consume the window.
- **ROI:** Largest remaining CPU mass — **but explicitly ruled hands-off until `MC2_GPU_CULL_LIFECYCLE`
  (paused lane).** Not a quick win; a strategic multi-slice port. `hazeFactor` writes are still
  load-bearing (`gos_terrain_lighting.cpp:53-55`) so no cheap cut exists.
- **Parity gate:** `mc2_01_terrain_solid.json` + `mc2_24.json` (mech-canary cull) — the offload must
  reproduce the object-visibility gates byte-exact (CRIT-1).
- **Effort:** L (the paused Lifecycle-gate lane). **Path:** GL compute now, VK-native later.
  **Automatable:** the cull-gate parity is already counter-guarded (motion_tolerance, mech canary).

### #6 — HUD / gosMesh per-draw orphan-realloc churn (untracked, not hitch-accounted)
- **Category:** 1 (redundant per-draw upload) / 4 (draw churn)
- **Evidence:** `updateBuffer` 5-arg overload `GameOS/gameos/utils/gl_utils.cpp:431` (decl
  `utils/gl_utils.h:120`) — the real per-draw orphan-realloc for all gosMesh VBO/IBO (HUD quads/
  tris/lines/points/text, 6 pairs, `gpu-buffer-owner-recon-1.md` §HUD). **Not routed through the
  `MC2_GL_BufferData` hitch macro → invisible to Tier-1 telemetry.**
- **Why wasteful:** immediate-mode HUD re-specs a buffer per draw-batch every frame; batchable and
  the buffers are small + mostly static (text atlas geometry changes rarely).
- **ROI:** Unknown — **needs measurement first** (it's telemetry-dark). Likely modest but it's the
  cleanest Tier-0 wrapper-adoption target (recon §3 adoption order: lod_chunk → HUD → postprocess).
- **Parity gate:** `mc2_01.json` (HUD in frame) — pixels identical.
- **Effort:** M. **Path:** GL now (GpuBuffer adoption slice B, TIER0-HUD). **Automatable:** yes — first
  step is *adding* the hitch accounting so the churn becomes visible (that IS the automatable guard).

---

## Full list (secondary / lower-ROI / watch)

| # | Title | Cat | Evidence | ROI | Effort | Path |
|---|---|---|---|---|---|---|
| 7 | Mech material SSBO (`s_mechMaterialSsbo`, slot 2) **unfenced** persistent | 4 | `gpu-buffer-owner-recon-1.md` §Mech ("none (un-fenced)"); already dirty-gated (`26974734`) so cost is closed — residual is the **missing fence** (VK-prep only) | Low frame / VK-unblock | S | VK-prep |
| 8 | DSA absent everywhere (`glGenBuffers` bind-to-edit, never `glCreateBuffers`) | 5-ish | `gpu-buffer-owner-recon-1.md` finding #5 | Micro per-bind; big VK-semantic alignment | L (sweep) | VK-prep |
| 9 | 4 diagnostic buffers masquerade as live (`g_thinCanarySSBO` slot7 written/frame **never read**, `g_solidBucketHeaderSsbo`, water `s_spikeThin`/`s_spikeHeader`) | 1 | recon §4; canary already default-OFF (`26974734`) | Tiny (gate the rest) | S | GL now |
| 10 | Terrain handle LUT full SubData/frame (`gos_terrain_indirect.cpp:2376`) | 1 | per-frame `MC2_GL_BufferSubData` of `kLutBytes` | **VERIFIED LIVE, SAFE-BY-DESIGN — NOT dead-path** (corrects both this row's original MOOT claim and the 2026-07-16 thin-canary residual audit's OPEN status). Handle-stability recon (VK-ARC-STATE.md 2026-07-16 EVE): `gosTextureHandle` index is never reused (nulled-not-recycled) so no wrong-texture aliasing is possible; `tex_resolve()`'s per-frame memo window self-heals any cache-out race by construction (frame-scoped reset); NO VK-bridge-mirror consumes this data (`vulkan_terrain_island.cpp`: 0 refs to terrainHandle/thSlot/LUT), so the `(handle,serial)` VK-PERF-1 precedent doesn't apply. Live `MC2_MODERN_TEX_RESOLVE_VALIDATE=1` soak (17,027 frames, 1.1M resolves) = 0 mismatches/oob. Separate OPEN question (perf, not stability, not sized here): `ComputeDispatch()` still runs unconditionally every frame though its SOLID-draw consumer is retired (LOD-chunk owns terrain) and its only other consumer (water reflection) is off by default. | S | n/a |
| 11 | Water thin-ring fence | 4 | recon finding #2 → **CLOSED** (WATER-THINRING-FENCE-1, `gos_terrain_water_stream.cpp:2154`/`:105`) | done | — | — |
| 12 | Terrain solid thin-record ring: 10ms fence wait (`g_thinRecordSSBO` ring-3) | 4 | recon §terrain-solid "fence + 10ms wait" | intentional ring-depth wait; watch, don't cut | — | VK |
| 13 | Particle main `s_ssbo` class-D (separate from tube) | 4 | ledger class-D `gos_particle_bridge.cpp` | bundle w/ #1/#4 | S | VK-prep |
| 14 | `s_surfaceVB/IB/TB` epoch-tracked glBufferData (slot 20/21/22) | 1 | recon §postprocess ("epoch"); already epoch-gated so mostly static | Low (verify epoch fires rarely) | S | GL |

---

## Method notes for the executor
- **Substitutive, not additive** (`memory/feedback_offload_must_be_substitutive_not_additive.md`):
  a fix only counts when the retired zone goes to ~0 in a *fresh* user-driven Tracy at wolfman zoom
  AND total frame drops. Smoke FPS is deceptive (no input). Several snapshot wins are still
  "PENDING substitutive proof" — do not double-count.
- **Every fix gates through `scripts/run_golden_parity.py`** against the blessed
  `baselineA-head-*` sets; byte-exact for value-preserving changes, within-floor
  (`visual-tolerance-policy.json`) only where explicitly justified.
- **Cost-split instrumentation is observer-effect-dominated**
  (`memory/cost_split_instrumentation_is_observer_effect_dominated.md`) — trust clean non-COST_SPLIT
  Tracy, not per-element RDTSC.
- **Do the quick win (#1) first:** cheap, hot on mc2_17, parity-safe, and it doubles as the
  class-D fence template for #4/#13.
