<!-- doc-status: stale | reviewed:2026-10-11 -->
# MODEL-OVERRIDE — Lighting + LOD perf-architecture recon

**Status:** READ-ONLY recon. No code changed. Input to a spec→review→plan→review→execute pipeline.
**Branch:** `claude/model-override-system-recon-1` (repo `A:/Games/mc2-model-override-recon`).
**HEAD at recon:** `313df6aa` (GPU-INSTANCE-SKIP-POOLS-1 landed — pool churn already mitigated; lighting + LOD are the remaining axes).
**Question:** A model-override FOREST (≥150 instances of a 706k-tri override tree) is slow; the perf snapshot claims a scalable static-light path already shipped (`b41baec` SSBO ceiling lift, `2db2a04`/`38d8720` persistent baked table). WHY does lighting still degrade, and what is the actual overflow site? Plus the LOD axis (trees pinned to one LOD; LOD swap blacks props).

Every symbol below is grep-verified `file:line` against the working tree on this branch. Where an in-source comment contradicts the code, that is called out explicitly — it is the single most load-bearing finding here.

---

## 0. TL;DR verdict (read this first)

1. **There is NO 32-slot light cap, NO UINT32_MAX overflow sentinel, and NO per-frame full-bake re-arm from light contention.** The inline comment at `mclib/bdactor_static.cpp:117-120` ("addLightDataStructureWithPerActorColor, 32-slot UBO returns UINT32_MAX under forest contention → render() invalidates and re-arms the full-bake latch perpetually") is **STALE / FACTUALLY WRONG against the current code**. The C++ light table (`MC_TextureManager::lightData_`) is an **unbounded, dynamically-grown array** (`txmmgr.cpp:1300-1307`, `+128` chunks), the shader SSBO is **unbounded** (`shaders/include/lighting.hglsl:43-56`, `buffer LightsData { ObjectLights light[]; }`), and the slot allocator **never returns a sentinel** — it always returns a valid grown slot. `b41baec`'s SSBO conversion **did** cover this consumer.

2. **The real `0xFFFFFFFF` sentinel is `TG_MultiShape::cachedGpuLightIndex_`** (`mclib/msl.h:346`), meaning "not yet cached this lifetime / gather did not run". It is set to a valid slot by `mc2CacheOrBakeStaticGpuLight` and only *stays* sentinel when the light gather genuinely could not run (e.g. `g_useGpuObjects && g_useGpuMechs` both off — `msl.cpp:2043`, or no SHAPE_NODE leaf found). It is **not** an overflow signal. The `== UINT32_MAX → invalidateStaticRegistration()` guard at `bdactor.cpp:4219` therefore almost never fires for a normal forest.

3. **The lighting cost on the override forest is NOT a per-frame full bake.** With `MC2_LIGHTBAKE` default-ON (`mclib/txmmgr_lights.cpp:271`), a *registered* tree (`recipeIndex >= 0`) takes the persistent-table O(1) path every frame after the first (`mc2CacheOrBakeStaticGpuLight` HIT → `EmitBakedGpuLightData` = a pure pointer assignment, `msl.cpp:2096-2109`). The `38d8720`/`2db2a04` machinery **does reach override trees** — IF they register. So gap #1 ("override types off the static path") is **NOT** the dominant problem on the verified path.

4. **What the forest actually pays on the lighting axis is small and bounded** — see §3.4. The 2.29s `Render.GpuStaticProps` GPU zone the greybeard pinned is **GPU raster, not CPU lighting**. The full-bake-per-frame premise in the task is built on the stale `:4586` comment; the code does not do it for a registered forest. **The dominant cost is LOD/overdraw (GPU), and the lighting "fix" is mostly a documentation/correctness cleanup, not a perf unlock.** Fix sequence: **LOD first.**

5. **One genuine residual lighting gap remains (gap #1, narrow form):** the very first frame after each `registerStatic`, and after every LOD/shape-swap or damage invalidation, `needsFullBakeNextFrame` is re-armed (`bdactor.cpp:4908`, `:4281`, `:4256`) — by design one frame. For trees this is benign because trees **never LOD-swap** (§4). It only churns for *buildings* hovering at a LOD distance boundary. For the override forest it is a one-time mission-load transient, not a per-frame cost.

---

## 1. The light path map (submit → lightDataIndex → allocator), file:line

Per static-prop / override-tree instance, per frame:

| # | Stage | Site | What happens |
|--:|---|---|---|
| 1 | Appearance update, GPU-eligible branch | `mclib/bdactor_static.cpp:82` (tree gpuEligible), `:2329` (bldg) | `gpuEligible = g_useGpuObjects && !needsFullBakeNextFrame && isMultiShapeEligibleForGpuObjects(treeShape)` (`:4544-4548`). |
| 2 | Cache-or-bake the per-actor light | `mclib/bdactor.cpp:4555` (tree), `:2336` (bldg) → `mc2CacheOrBakeStaticGpuLight()` `mclib/bdactor_lightbake.cpp:186` | If `MC2_LIGHTBAKE` on AND `registered` AND `recipeIndex >= 0`: table **HIT** → `EmitBakedGpuLightData` (O(1) pointer assign, `mclib/msl_transform.cpp:634`); **MISS** (frame 1 / post-invalidate) → `CacheGpuLightData()` real gather, then persist via `mc2SetBakedStaticLight` + `mc2WriteStaticLightSlot` (`bdactor.cpp:1929-1936`). Else (unregistered / bake off / mech) → `CacheGpuLightData()` legacy gather (`:1912`). |
| 3 | Real gather (MISS only) | `TG_MultiShape::CacheGpuLightData()` `mclib/msl_transform.cpp:545` → first SHAPE_NODE leaf `TG_Shape::GatherGpuObjectLightDataOnly()` `mclib/tgl_transform.cpp:1364` | Sets `cachedGpuLightIndex_` from the allocator. |
| 4 | **Slot allocator** | `MC_TextureManager::addLightDataStructureWithPerActorColor()` `mclib/txmmgr_lights.cpp:624` → `addLightDataStructure()` `mclib/txmmgr_lights.cpp:586` | FNV+memcmp dedup (`:1285-1297`); on unique data, **grows the array** (`:1300-1307`, `capacity += 128`) and returns `count++`. **No cap, no sentinel return.** A per-frame slot-cache repoint (`s_lightSlotByActorKey`, `:1378-1398`) skips even the FNV/memcmp on a hit. |
| 5 | Per-instance capture | `mclib/bdactor.cpp:4557` (tree), `:2340` (bldg) | `staticReg.lightDataIndex = treeShape->getCachedGpuLightIndex()` (the slot from step 3/2). |
| 6 | Per-leaf broadcast into instance record | `GameOS/gameos/gos_static_prop_batcher.cpp:4344-4350` (submit path) ; registry replay `bdactor.cpp:4226` `markVisible(recipeIndex, lightDataIndex, …)` | `GpuStaticPropInstance.lightDataIndex` ← `multi->cachedGpuLightIndex_` (`batcher:4346`), with a `0xFFFFFFFF` fallback to a fresh `GatherGpuObjectLightDataOnly()` (`:4348-4350`). |
| 7 | GPU consume | `shaders/static_prop.vert:214` `calc_light(int(inst.lightDataIndex), …)` indexing `shaders/include/lighting.hglsl:43-56` SSBO `LightsData light[]` | Per-vertex lit from the indexed `ObjectLights` record. |
| 8 | Whole-buffer SSBO upload | `mclib/txmmgr.cpp:347-358` (capacity-sized upload) | Binding=20, sized to `lightDataStructuresCapacity` (grows with the table). No fixed-32 anywhere. |

**Static-replay (touch) path** (registered + stable, the steady state): `IsStaticNow()` true (`bdactor.cpp:4207`, def `:4751`) → `touch()` (`:4758`) → bake-HIT `EmitBakedGpuLightData` (`:4777`) or legacy `ResubmitCachedGpuLightData` (`:4779`) → `markVisible(...)` (`:4226`). **This is O(1) per instance per frame** and is what a registered forest runs every frame after frame 1.

---

## 2. The overflow site + the bound hit — RESOLVED: there is none on this path

The task's premise was that overflow returns `UINT32_MAX` and re-arms a per-frame bake. **Grep-verified refutation:**

- **`addLightDataStructure` (`txmmgr.cpp:1278-1331`)** — the only allocator. It grows (`:1300-1307`) and returns `count++` (`:1310`). It contains **no `0xFFFFFFFF`, no `MAX_LIGHT`, no cap comparison**. (Confirmed: the only `0xFFFFFFFF` literals in `txmmgr.cpp` are `firstActiveLightSourceIndex`'s "no active light" return `:1003/1011/1017`, the per-actor-color `actorLightSource == 0xFFFFFFFFu` *no-light* branch `:1344`, and `s_sceneLightTemplateFrame` init `:947/1426` — none is an overflow ceiling.)
- **Shader SSBO (`lighting.hglsl:43-56`)** — `layout(binding=20, std430) buffer LightsData { ObjectLights light[]; };` — runtime-sized array; the audit doc `docs/static-prop-lighting-audit.md:81-108` confirms the `[LIGHTSSBO v1]` UBO→SSBO migration that lifted the 64-slot ceiling. **The proposal `docs/model-override-gpu-instancing-proposal.md:28,146` claim of `LightsData[32]`/`[64]` UBO is STALE** — it predates / mis-cites the SSBO conversion.
- **The genuine sentinel** `cachedGpuLightIndex_ = 0xFFFFFFFFu` (`msl.h:346`) means *"not yet cached"*, set valid by every successful gather/bake. The `== UINT32_MAX` guards (`bdactor.cpp:4219` tree, `:1412` bldg) only fire when the gather **could not run** (both GPU kill-switches off `msl.cpp:2043`, or no SHAPE_NODE leaf). For a normal forest with `g_useGpuObjects=true` (default, `GameOS/gameos/gos_static_prop_batcher_internal.h:55`) it does not fire.

**Conclusion: the 32-slot overflow / UINT32_MAX-driven per-frame full-bake described in the task and in `mclib/bdactor_static.cpp:117` does not exist in the current code.** This is the load-bearing correction the spec must absorb before designing a lighting fix, or it will "fix" a non-bug.

---

## 3. Which of the 3 lighting gaps — ranked

### Gap #1 — override/tree types off the static O(1) path — **PARTIAL / TRANSIENT only (rank: low)**
The static O(1) path is gated on `registered && recipeIndex >= 0` (`bdactor.cpp:1911`). Override trees **do** register: `TreeAppearance::registerStatic` (`bdactor.cpp:4796`) registers the **render** shape (`getTreeRenderShape(i)`, `:4812`) with the `_treeRegIsOverride` flag, builds the recipe (`:4833,4855`), and on `regIdx >= 0` sets `staticReg.registered/recipeIndex` (`:4897-4900`). So a successfully-registered override tree reaches the persistent baked table exactly like a stock tree. **The only residual is the one-frame `needsFullBakeNextFrame=true` re-arm at registration (`:4908`)** and after invalidation (`:4256` late-reg failure, `:4281` post-registration, `:4244` shape-swap). For trees this is a mission-load transient (trees never LOD-swap — §4), not a per-frame forest cost.

### Gap #2 — SSBO conversion missed this consumer — **REFUTED (rank: n/a)**
`b41baec` covered this consumer. Both the C++ table and the shader array are unbounded (§2). No cap remains for the static-prop lane.

### Gap #3 — slot-pool density / a ~150-instance forest exceeding a permanent-slot pool — **N/A on lighting (rank: n/a)**
The permanent static light slots are keyed by **monotonic registry `recipeIndex`** (one per *recipe* = per type×LOD, not per instance), and `lightData_` grows on demand (`bakeStaticLightSlot` `txmmgr.cpp:1436-1464`, `+128` chunks). A 150-instance forest of 6 types consumes ~6 recipe slots, not 150. There is no per-instance light-slot pool to overflow. (The density wall the proposal found at ~90–148 instances is the **TGL geometry frame-pools** — `mission.cpp` `vertexPool`/`facePool` — already addressed by GPU-INSTANCE-SKIP-POOLS-1 `313df6aa`, not a lighting pool.)

**Ranked lighting gaps:** #1 (transient registration re-bake, low impact, trees benign) ≫ #2 (refuted) = #3 (n/a). **Net: lighting is essentially already O(1) for a registered override forest. The lighting "fix" is correctness/cleanup, not a perf unlock.**

---

## 4. LOD axis

### 4.1 Trees pinned to a single LOD
Trees only ever build the per-instance render shape from **LOD index 0** and never swap it at runtime:
- `currentLOD = 0` hardcoded at init (`bdactor.cpp:3834`).
- `setObjStatus(NORMAL)` builds `treeShape = appearType->getTreeRenderShape(0)->CreateFrom()` (`bdactor.cpp:3999`); init builds `getTreeRenderShape(0)` (`:3856`).
- `registerStatic` transforms LOD-0 geometry (`:4833`).
- There is **no distance-driven `currentLOD` reassignment for trees** anywhere in `TreeAppearance` — contrast buildings, which have `currentLOD` swap blocks (`bdactor.cpp:3025, 3092, 3556` and the LOD-distance reads `:274`). Tree `lodDistance[]` is read at load (`:3634-3639`) but never used to swap the render shape.

So the override-tree forest renders the **full-detail 706k-tri mesh at every distance** — no LOD reduction. (The task's "LOD 1" = this single loaded LOD; the code index is 0.) This is the GPU cost driver.

### 4.2 The "LOD-swap → props/trees go black" mechanism
Root-caused in `docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md:78-87`, fix annotated in code:
- After a LOD/shape swap (buildings) or any re-register, the **new `TG_MultiShape`'s `lightData_` is default-zero** — it has never had a full update, only `TransformMultiShape_BuildRecipe` (positions only) ran (`bdactor.cpp:4902-4904`).
- Under static-replay, `touch()` → `ResubmitCachedGpuLightData()` re-ships that **all-zero** `lightData_` → `addLightDataStructure(&zeros)` returns the zero-light slot → registry flush emits the actor with **all-zero lighting → black prop/tree** (spec `:80-85`).
- **The cure already in the code:** every registration site sets `needsFullBakeNextFrame = true` (`bdactor.cpp:4908` tree mission-load, `:4281` tree per-frame re-register, `:1508/:1476` bldg, `:2893` bldg damage `→ all-zero lighting slot → black actor` comment) so the **next** frame forces a full `update()` that populates `lightData_` *before* any `touch()` re-ships it. This is exactly the "one extra full bake per LOD swap" the spec's Fix 2 prescribes (`:97-101`).
- **The trap the eventual LOD slice must avoid:** if a real per-distance LOD swap is added for trees (or any new shape-swap), it MUST re-arm `needsFullBakeNextFrame` at the swap site, or it resurrects the 2026-05-05 black-tree class. (Note `docs/known_issues.md:54`: the Stage-0.5 §4 repoint was reverted partly because it "resurrected the 2026-05-05 black-tree class" — same mechanism, confirming it is live and easy to re-trip.) A texture-handle/layer angle is **secondary**: the override-untextured `layerForPacket=-1` cull (`mclib/bdactor_sidecar.cpp:432`, fixed for overrides in `4171be63`) is a *registration*-time concern, not a per-LOD one, but a per-LOD shape would need its textures resolved per LOD too (`LoadOverrideRenderShapeTextures`, mvp-notes Slice 5).

### 4.3 Recommended LOD model
The spec's **Fix 3 (per-LOD pre-registration)** (`lod-swap-churn spec:105-115`) is the right target: pre-register one `staticReg` recipe per LOD at mission load; a "LOD swap" becomes a registration-state lookup (which recipe is active for `currentLOD`) instead of an invalidate+re-register cycle — and each per-LOD recipe carries its own permanent baked light slot, so no black-frame and no per-frame bake. For override trees this also means authoring/importing 2–3 decimated LODs of the 706k mesh and registering each.

---

## 5. GPU-vs-CPU reconciliation (the 2.29s `Render.GpuStaticProps`)

| Cost | Where it lives | Verdict |
|---|---|---|
| **2.29s `Render.GpuStaticProps`** | GPU timeline (the multidraw raster zone; snapshot row `docs/render-perf-snapshot.md:32`) | **GPU raster of the no-LOD full-detail 706k-tri mesh × ~148 instances + alpha-test leaf-card overdraw.** There is **no depth pre-pass** for static props (the .frag writes color + `GBuffer1` directly, `static_prop.frag:190-202`; alpha-test discard `:193-194`), so leaf cards pay full overdraw. This is the dominant, GPU-bound cost. |
| **Per-frame lighting** | CPU | For a *registered* forest this is **O(1)/instance** (`EmitBakedGpuLightData` pointer assign, §1 touch path) — bounded and small. The whole-buffer SSBO re-upload (`txmmgr.cpp:347-358`) is sized to the table capacity (≈ #recipes, not #instances), so it is **NOT** churned proportionally to the forest. The full-bake is a one-frame mission-load transient (§3 gap #1), **not** a per-frame cost. |

**Does the lighting cost also show on the GPU timeline?** Only via the one whole-buffer SSBO upload per frame, which is recipe-count-sized (tiny) — it does **not** scale with the forest and does **not** materially feed the 2.29s zone. The 2.29s is overwhelmingly **raster + overdraw**, i.e. the LOD/overdraw axis.

**So: is BOTH a lighting-O(1) fix AND a LOD fix needed, and in what order?**
- A lighting-O(1) fix is **already shipped** for the registered path (LIGHTBAKE table). The remaining lighting work is **correctness/cleanup** (delete the stale `:4586` "32-slot UBO" comment; confirm the one-frame re-arm is the only residual), not a perf slice.
- The 2.29s is a **LOD + overdraw (GPU)** problem. **LOD first** — it is where the time is. A depth-pre-pass / alpha-test-aware sort is the secondary overdraw lever.

**Recommended FIX SEQUENCE: LOD first; lighting is a cleanup, not a perf gate.**

---

## 6. Sequenced fix plan (phases — recon + sequencing only, NOT implementation)

**Phase 0 — Correctness reconciliation (cheap, unblocks accurate spec).**
Delete/repair the stale `mclib/bdactor_static.cpp:117-120` annotation (no 32-slot UBO, no UINT32_MAX overflow); correct `docs/model-override-gpu-instancing-proposal.md:28,146` `LightsData[32]` to the unbounded SSBO reality. No code-behavior change. Output: the spec stops chasing a non-bug.

**Phase 1 — LOD for override trees (the real 2.29s win). Largest ROI.**
1. Author/import 2–3 decimated LODs of the override mesh (extend `tree_export_lush.py` leaf-card-preserving thinning per LOD; resolve textures per LOD via `LoadOverrideRenderShapeTextures`).
2. Implement the spec's **Fix 3 per-LOD pre-registration** (`lod-swap-churn spec:105-115`): `staticReg` array keyed by `currentLOD`; LOD selection = active-recipe lookup, not invalidate+re-register.
3. Add distance-driven `currentLOD` selection to `TreeAppearance` (trees are currently pinned to LOD 0, §4.1) using the already-loaded `lodDistance[]` (`bdactor.cpp:3634`).
4. **Guard rail (load-bearing):** any new shape-swap site MUST set `needsFullBakeNextFrame=true` (§4.2) or it resurrects the black-tree class (`known_issues.md:54`). Bake the regression test into the slice.

**Phase 2 — Overdraw (secondary GPU lever).**
Static-prop depth pre-pass or front-to-back / alpha-test-aware ordering to cut leaf-card overdraw (no pre-pass today, §5). Measure after Phase 1 — Phase 1 LODs may already reduce leaf-card count enough.

**Phase 3 — Lighting cleanup (correctness, not perf; can run anytime after Phase 0).**
Confirm via Tracy that `addLightDataStructure scan` is dynamic-only for a registered override forest (the still-PENDING `[LIGHTBRIDGE v1]` substitutive proof, snapshot `:59,:61`). If a residual per-frame static call survives, it is the gap #1 transient re-arm — bound it to genuinely one frame. No new ceiling work; the SSBO path is already unbounded.

**Sequencing rationale:** Phase 0 is a prerequisite (prevents fixing a phantom). Phase 1 is where the 2.29s lives. Phase 2 is incremental on top of Phase 1. Phase 3 is correctness/verification, not on the critical perf path.

---

## 7. Evidence index (file:line)

- **No light cap / grow-on-demand allocator:** `mclib/txmmgr.cpp:1278-1331` (esp. grow `:1300-1307`, return `:1310`); per-actor wrapper `:1333-1401`; whole-buffer SSBO upload `:347-358`; bake enable default-ON `:1201`.
- **Persistent baked table (38d8720/2db2a04):** `s_bakedStaticLight` map `mclib/txmmgr_lights.cpp:734`; `bakeStaticLightSlot` `:1436-1464`; `resetLightData` rebase `:1409-1427`; `mc2CacheOrBakeStaticGpuLight` `mclib/bdactor.cpp:1903-1939`; `EmitBakedGpuLightData` O(1) `mclib/msl.cpp:2096-2109`; `CacheGpuLightData` real gather `:2035-2064`.
- **Real sentinel (not overflow):** `cachedGpuLightIndex_ = 0xFFFFFFFFu` `mclib/msl.h:346`; guards `mclib/bdactor.cpp:4219` (tree), `:1412` (bldg), `GameOS/gameos/gos_static_prop_batcher_finalize.cpp:1035-1039`.
- **STALE/WRONG comment (the load-bearing correction):** `mclib/bdactor_static.cpp:117-120`.
- **Shader unbounded SSBO:** `shaders/include/lighting.hglsl:43-56`; per-instance index `shaders/static_prop.vert:214`; audit confirmation `docs/static-prop-lighting-audit.md:81-108`.
- **Proposal stale UBO claim:** `docs/model-override-gpu-instancing-proposal.md:28,146`.
- **Trees pinned to LOD 0:** `mclib/bdactor.cpp:3834` (`currentLOD=0`), `:3856,:3999` (`getTreeRenderShape(0)`), `:4833`; bldg LOD swap (contrast) `:3025,:3092,:3556`; tree `lodDistance` read-but-unused `:3634-3639`.
- **LOD-swap-black mechanism + cure:** spec `docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md:78-115`; re-arm sites `mclib/bdactor_static.cpp:438,:4281,:4256,:2893,:1508,:1476`; live-regression note `docs/known_issues.md:54`.
- **Override-tree registration reaches the static path:** `TreeAppearance::registerStatic` `mclib/bdactor.cpp:4796-4910` (render shape `:4812`, recipe `:4833/4855`, register `:4897-4900`); eligibility `gos_static_prop_batcher.cpp:6933-6951`; `g_useGpuObjects` default true `:47`.
- **GPU raster zone (2.29s) + no depth pre-pass:** `docs/render-perf-snapshot.md:32`; `shaders/static_prop.frag:190-202` (color+GBuffer1 direct), alpha-test discard `:193-194`.
- **TGL geometry-pool wall (already fixed, NOT a light pool):** GPU-INSTANCE-SKIP-POOLS-1 `313df6aa`; `mclib/bdactor_static.cpp:102-105,:4602-4606`; proposal §3 `docs/model-override-gpu-instancing-proposal.md:157-180`.
