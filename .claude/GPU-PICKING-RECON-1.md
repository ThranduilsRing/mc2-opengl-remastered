# GPU-PICKING-RECON-1

**Mode:** RECON ONLY — no source changes. Worktree `A:/Games/mc2-controlmap-sample-1` (branch `claude/controlmap-sample-1`, HEAD `5be735a7`).
**Question (user):** "is there a better way to do picking — GPU-side? I think there is a reason [we don't], but not sure." Find the reason, then the design.

**One-line verdict:** Yes, a unified GPU picking service is the right end-state, and the substrate + house async pattern already exist — but the "reason we don't do it universally" is real and specific: the **ground-position pick runs every frame for hover feedback consumed in the SAME frame** (passability red-X, LOS, drag-select anchor, precision move-order Z), and a naive 1-frame-lagged GPU readback makes that feedback visibly lag the cursor. The correct design is same-frame synchronous single-pixel readback (already the shipped pattern) for the cursor, NOT the async ring — with async reserved for region/rect reads. This recon replaces "there's a mysterious reason" with a precise, addressable constraint.

---

## 1. Ground-position pick (the 84%-of-Interface hotspot)

**Path:** `Camera::inverseProject` (`mclib/camera.cpp:992`), CPU brute-force.

- Structure (post-VPL-retirement): a two-stage forward-projection scan over the whole terrain quadList (`land->getQuadList()`, `getNumQuads()`), `camera.cpp:1139`+. Each admitted quad projects 4 corners via `eye->projectForSelectionPicking()` (`:1238`) and screen-containment tests. This is O(quads) — the cost class that dominates `MissionInterfaceManager::update` (the "84% of Interface" hotspot; perf snapshot `docs/render-perf-snapshot.md` bucket "picking independent (`Camera::inverseProject`)").
- `MC2_LOWCAM_PICK` (default ON, `camera.cpp:1199`) — single-pass, no 100-tile cap, frontmost-containing-tile tiebreak; fixed grazing-pitch mis-land. `MC2_TERRAIN_RAYCAST_PICK` — heightfield raycast picker (`:1016`), **structurally broken** (getPosition()=ground-focus not eye; worldToClipGL inverse collapses X), NOT used; legacy forward-projection is canonical even under `MC2_TERRAIN_LOD_CHUNK`.
- **Caller:** SOLE production caller is `MissionInterfaceManager::update` (`code/missiongui.cpp:1129` per-frame hover + `:1961`/`:2315` per-click precision). Guarded by a mandatory per-frame delta-cache (`missiongui.cpp:1100-1135`, `inverseProjectCacheValid`): skips the quad walk when neither cursor pixel nor camera world→clip matrix changed. So the O(quads) cost only lands on cursor-move / camera-move frames — but at wolfman zoom on large maps that is most frames of active play.
- Two precisions in play: fast per-frame preview vs. the precise per-click `inverseProject` for the terrain-surface/placement-Z (`missiongui.cpp:1950-1962` comment: "we need the real terrain-surface hit … move-orders land on the correct elevated cell"). `screenToGroundPlaneApprox` (a z=0 ground-plane O(1) unproject, `gos_postprocess.cpp:4938`) is REJECTED for the click path because it drifts ~elevation in XY on hilly terrain and shares the broken worldToClipGL inverse.

**KEY:** `RenderWorld::lookupAtPixel` **already computes worldX/Y/Z** by unprojecting the depth sample through the per-frame inverse-VP (`RenderWorld.cpp:942-964`, reverse-Z aware). The GPU ground-position primitive already exists inside the substrate; nobody currently consumes its `worldPosValid`/world coords for ground picks.

## 2. Object pick

- **Movers (mechs/vehicles):** `GameObjectManager::findObjectByMouse` / `findMoverByMouse` (`code/objmgr.cpp:3877` / `:3941`). Dispatcher `findObjectByMouse(mouseX,mouseY)` (`:4280`) tries movers first (4 commander/skip permutations), then non-movers. **Movers are RECT-ONLY** — screen-space AABB test on `objAppearance->upperLeft/lowerRight` (`objmgr.cpp:3967-3975`), explicit comment `:3979` "Movers are NOT per poly!!". Non-movers additionally require `objAppearance->PerPolySelect(mouseX,mouseY)` (`:3917`/`:3926`).
- **Why PerPolySelect is excluded for movers (memory question answered):** the capability EXISTS for movers — `Mech3DAppearance::PerPolySelect` (`mclib/mech3d.cpp:1867`) and `GVAppearance::PerPolySelect` (`mclib/gvactor.cpp:3209`) both forward to `TG_Shape::PerPolySelect`. It is deliberately NOT called on the mover path because PerPolySelect's contract (`mclib/tglpp.cpp:10-33`) requires the leaf to have been transformed **last turn exactly**: all six pool pointers non-null AND `lastTurnTransformed == turn-1` (in-game) / `== turn` (editor). Movers re-transform and settle every frame; a per-poly test against a mid-settle / stale-transform leaf silently returns false (contract-safe but flaky), so a moving mech would drop in and out of selectability. The screen AABB (`upperLeft/lowerRight`, maintained every frame) is robust to that motion. Rect-only is the correct trade for things that move; per-poly is reserved for static-ish scenery where the leaf transform is stable.
- **Target consumer:** `updateTarget(bRawGui)` (`missiongui.cpp:1168`, runs EVERY frame) sets `target = findObjectByMouse(...)` (`:1771`) for hovered-unit name/targeting reticle — a per-frame hover consumer, not click-gated.
- **Editor picks:** already migrated off `inverseProject`. `EditorObjectMgr::getObjectAtScreenPosition` (`editor/EditorObjectMgr.cpp:986`) forward-projects each object's world pos and matches by screen pixel-distance (`EditorObjectMgr_ConsiderScreenPick`, tolerance `pixelFloor=12px`), with PerPolySelect at `:213`. Comment `:843`: "This replaces the old inverseProject". BuildingBrush / cell-snap use `land->worldToCell(pos,...)` on a world point.

## 3. The existing GPU substrate (RenderWorld arc)

**Doc:** `docs/renderworld_arc_status.md` (arc reached steady state 2026-05-24).

- **ObjectID G-buffer:** MRT color attachment-2, `GL_R32UI`, created only when `MC2_OBJECT_ID_BUFFER=1` (`gos_postprocess.cpp:975-998`; NEAREST filter, added to scene draw buffers). Env-OFF = exactly zero FBO cost.
- **RenderObjectKind** (`RenderWorld.h`): `StaticProp=0` (SHIPPED writer), `Mech=1` (SHIPPED writer), `Terrain=2` (RESERVED, **no writer**, tripwire-protected), `Vfx=3` (RESERVED, **CI-PROHIBITED** writer). Handle = 20-bit index + generation; partitioned bases (`kMechHandleBase=0x10000`).
- **`lookupAtPixel(screenX,screenY)`** (`RenderWorld.cpp:823`): binds the scene FBO read buffer, does **synchronous single-pixel `glReadPixels`** of attachment-2 (`:863`, GL_RED_INTEGER/GL_UNSIGNED_INT) **plus a depth-component readPixels** (`:866`), then generation-checks the record under a mutex and unprojects depth→world (`:942-964`). Comment `:853`: "Synchronous single-pixel readback … stalls the GPU until prior-frame attachment-2 writes are visible." One pixel — the stall is bounded, not a full-buffer BAR read.
- **Consumers of `lookupAtPixel` today (count = 3 gated paths, all default-OFF):**
  1. **Shift+click static-prop pickup** (`M1.6`, `MC2_STATIC_PROP_PICK`) — via `tryGameplayPick` (`code/gameplay_pick.cpp:78`) called from `tryStaticPropPick` (`missiongui.cpp:2224`).
  2. **Ctrl+Shift static/mech inspect** (`M2.6`, `MC2_MECH_PICK`) — also via `tryGameplayPick` (`missiongui.cpp:2135`).
  3. **★ GPU hover pick for mechs** — `MC2_GPU_PICK_HOVER=1` ("GPU_PICK_HOVER_DYNAMIC-1", `docs/tier1_env_vars.md:142`), inline in `updateTarget` (`missiongui.cpp:1680-1770`). Mouse-move cache (no readPixels when cursor still), watch-ID liveness validation, **only `kind==Mech` uses the GPU result**; StaticProp/miss fall through to CPU `findObjectByMouse`. This is the incremental prior art — the first real "GPU pick replaces a CPU per-frame hover" slice, already shipped default-OFF.
- **Readback semantics:** all consumers are **synchronous same-frame** (single pixel). NONE is lagged. The `tryGameplayPick` header (`gameplay_pick.h:73-77`) documents "ONE synchronous glReadPixels … main thread only."
- **The house async pattern (for reference, NOT currently used by picking):** `gpu_cull_readback.*` (`GameOS/gameos/gpu_cull_readback.h`) — 3-slot persistent-mapped ring + per-frame `glFenceSync`, three-tier non-blocking `tryConsume` (N-1 / N-2 / conservative), `readback_isActorVisibleLagged(actorId)` O(1) query that is **fail-open** (returns visible when no good slot). Used by the cull path (`terrobj.cpp` recalcBounds gate, default-ON). This is the template for any lagged region pick.

## 4. WHY NOT UNIVERSAL — the actual reason(s)

Ranked by load-bearing weight:

1. **★ Same-frame hover-feedback contract (the primary reason).** The ground pick is NOT click-only — it runs every frame in `MissionInterfaceManager::update` to drive: (a) passability check → red-X "dead area" cursor (`missiongui.cpp:1137-1149` `IsGameSelectTerrainPosition` + `worldToCell`), (b) team line-of-sight (`:1147`), (c) the drag-select box anchor (`dragStart = wPos`), (d) hovered-unit name/reticle via `updateTarget→findObjectByMouse` (`:1168`/`:1771`). All consumed **in the same frame the cursor moved**. A 1-frame-lagged GPU result would make the red-X, the hovered name, and the select box trail the cursor by a frame — visibly wrong during fast pans. This is why the shipped `MC2_GPU_PICK_HOVER` path is **synchronous same-frame** (single-pixel `glReadPixels`, accepting the small stall) and NOT built on the async ring. The reason we don't universally async-GPU-pick is: the cursor loop is a same-frame consumer.

2. **Precision / sub-pixel Z for move-orders.** The per-click move-order needs the exact terrain-surface elevation (`missiongui.cpp:1950-1962`); `screenToGroundPlaneApprox` (z=0 plane) is explicitly rejected as XY-drifting on slopes. GPU depth-unproject (`lookupAtPixel:942`) reads the actual rendered depth of the terrain fragment under the cursor — this is MORE precise than the approx AND correct on slopes (it's the true surface), so GPU depth-unproject is a genuine precision *upgrade* here, not a regression — provided reverse-Z precision at distance holds (see risks).

3. **Buffer availability during pause / menus / cutscenes / editor-ortho.** `lookupAtPixel` guards on `getGosPostProcess()` and a live scene FBO/tex (`RenderWorld.cpp:835-851`) and returns `NO_POSTPROCESS`/`NO_FBO_OR_TEX` gracefully. If the scene FBO isn't re-rendered (pause without redraw, some menu overlays, editor ortho preview modes), attachment-2/depth are stale or absent → GPU pick must fall back to CPU. Editor already uses a forward-projection screen-match picker (§2) independent of the objectID buffer, so editor integration is additive, not a replacement.

4. **Occlusion semantics — GPU pick = visible-surface only.** The objectID buffer is last-fragment-wins depth-tested; you can only pick what's visibly on top. VFX is deliberately click-through (M4 DECISION: VFX must NEVER write attachment-2 — GL integer attachments don't blend, translucent fragments would clobber the ID underneath; `renderworld_arc_status.md:47-57`). No gameplay path found that requires picking an *occluded* object (e.g. selecting a mech hidden behind a building) — CPU rect-pick could in principle select through scenery, but the visible-surface semantics of GPU pick are actually the desired behavior for the cursor. Trees are already excluded from targeting (`objmgr.cpp:3915`,`:3923`).

5. **Multi-select rect** needs a region read of the ID buffer, not a single pixel — fine on GPU (glReadPixels of a rect region), and the async ring is the right tool there since rect-select tolerates a frame of latency (it's a drag gesture, not a hover).

## 5. DESIGN — unified GPU picking service

A single `RenderWorld::PickService` (extends existing `lookupAtPixel` + `ScreenPick`) with three query shapes:

- **`pickCursor(glX,glY) → {objectHandle?, kind, worldPos}`** — synchronous single-pixel readback (attachment-2 + depth), same-frame. This is the merge of the existing `lookupAtPixel` (already returns world pos). Consumers: the per-frame hover loop (passability/LOS/red-X/hovered-name) and per-click move-orders. **Kills the `Camera::inverseProject` cost class entirely** for the ground pick (no quad walk; one pixel readback + one matrix-vector unproject). Mover object pick uses the ID lookup (mech writer shipped); static-prop uses ID lookup (writer shipped); ground position uses depth-unproject.
- **`pickRegion(rect) → set<handle>`** — for rect multi-select; async via the `gpu_cull_readback` ring pattern (1-frame lag acceptable for a drag gesture), OR a synchronous region read if the rect is small.
- **CPU fallback shim** — any query returns a `usedCpuFallback` flag; the caller keeps the existing CPU path (`inverseProject` / `findObjectByMouse` / editor screen-match) live behind it for: buffer-not-available (pause/menu/ortho), env-OFF, kind not yet GPU-covered (matches the shipped `MC2_GPU_PICK_HOVER` mech-only-then-fall-through shape exactly).
- **Editor integration:** additive. Editor keeps its forward-projection screen-match (§2) as the default; GPU pick opt-in via `MC2_EDITOR_MODE + MC2_OBJECT_ID_BUFFER` (already documented `tier1_env_vars.md:158`). If M3.1 terrain identity ever ships it plugs in here.
- **Vulkan-forward (coordinate with vulkan-l6 ladder):** both backends read the SAME attachments (R32_UINT color + depth). The pick service should express readback as an abstract "read attachment N at pixel/rect" op so the L6 backend-parity work can implement it once per backend. The single-pixel synchronous readback maps cleanly to a Vulkan `vkCmdCopyImageToBuffer` + fence-wait (bounded, like the GL stall); the async region path maps to the existing ring/fence pattern already prototyped in the Vulkan islands. FLAG for vulkan-l6: attachment-2 + depth must be present in the native subgraph's exported intermediate for pick to work under Vulkan.

## 6. Slice ladder (sizes + acceptance; house parity oracle)

**Parity oracle (house pattern):** over a tier1 smoke run with `MC2_PICK_PARITY=1`, at every cursor-move/click frame compute BOTH the GPU pick and the CPU pick for the same screen coords, log the delta, and RETURN what the active mode would return (mirrors the existing `MC2_TERRAIN_PICK_PARITY` diagnostic at `camera.cpp:1017-1021` and `MC2_LIGHTBAKE_PARITY`). Acceptance = ground-pos delta within terrain-cell tolerance and object-handle identical for the sampled frames; 0 unexplained mismatches.

| Slice | Size | Content | Acceptance |
|---|---|---|---|
| **S0 (this doc)** | recon | Inventory + reason + design | — |
| **S1 GPU-GROUND-PICK-PARITY** (first slice) | S | Wire a **parity-only** `pickCursorGroundPos` reading `lookupAtPixel().worldX/Y/Z` alongside the existing `inverseProject` in `missiongui.cpp` hover+click paths, gate `MC2_GPU_GROUND_PICK_PARITY` (default-OFF, no behavior change — logs delta, returns CPU result). | tier1 5/5 GL-clean; parity log shows GPU-vs-CPU ground-pos delta distribution; confirm depth-unproject precision on slopes at wolfman + at distance (reverse-Z). |
| **S2 GPU-GROUND-PICK-ADOPT** | S | Flip the hover ground pick to consume the GPU world pos (synchronous, same-frame) behind `MC2_GPU_GROUND_PICK` (default-OFF); CPU fallback when `!worldPosValid` / buffer unavailable. | Parity clean over full run; red-X/LOS/drag-anchor visually identical; `Camera::inverseProject` walk retired from the hover path when armed (substitutive Tracy proof: `MIF.InvProj` zone → ~0). |
| **S3 PICK-SERVICE-UNIFY** | M | Extract `RenderWorld::PickService` merging the 3 existing consumers + S2 into one API; move-order click path adopts precise GPU depth-unproject (default-OFF gate). | Move-orders land on correct elevated cell (parity vs precise `inverseProject:1961`); all 3 legacy consumers route through the service. |
| **S4 GPU-RECT-SELECT** | M | `pickRegion` for drag multi-select via the `gpu_cull_readback` ring pattern (fail-open, 1-frame lag). | Rect select parity vs CPU AABB set; lag imperceptible on drag. |
| **S5 VULKAN-PICK-PARITY** | M | Coordinate with vulkan-l6: implement the attachment-read op on the Vulkan backend; pick-parity oracle runs cross-backend. | GL vs Vulkan pick pixel-parity on the islands where attachment-2+depth are exported. |

**First slice = S1** (parity-only ground pick; zero behavior change; measures the precision claim before adopting).

## 7. Risks

- **R1 — same-frame stall.** Single-pixel synchronous `glReadPixels` stalls the GPU until prior-frame attachment-2/depth are visible (`RenderWorld.cpp:853`). Bounded (1 pixel) and the shipped `MC2_GPU_PICK_HOVER` already eats it with a mouse-move cache — but must confirm at wolfman it beats the `inverseProject` quad-walk it replaces (net win = removing an O(quads) CPU scan for a bounded GPU stall + mouse-move cache). The delta-cache on the CPU side already means both are skipped on still-cursor frames, so the comparison is per cursor-move frame.
- **R2 — reverse-Z depth precision at distance.** Depth-unproject world pos (`lookupAtPixel:948-962`) may lose XY precision at far terrain under reverse-Z; S1 parity must sample distant clicks specifically. If insufficient, keep precise `inverseProject` for the click move-order and use GPU only for hover.
- **R3 — buffer liveness (pause/menu/ortho).** Must default to CPU when the scene FBO isn't freshly rendered; the graceful `NO_FBO_OR_TEX`/`NO_POSTPROCESS` guards exist but the *fallback wiring* must be exercised in the parity slice (not just assumed).
- **R4 — terrain has no GPU identity (M3 deferred).** Object *identity* over terrain stays CPU (worldToTile); GPU covers only ground *position* + StaticProp/Mech identity. Do NOT resurrect M3 terrain writers as a dependency — the depth-unproject gives ground position without a Terrain objectID (matches `renderworld_arc_status.md:43` "GPU substrate would add no new info" for terrain identity).
- **R5 — mover pick stays rect-only by design.** Do NOT switch movers to GPU per-poly under the illusion it's "better" — the visible-surface ID lookup already gives per-pixel mover picking (better than rect), so mover GPU pick is an *upgrade over rect*, but the PerPolySelect-last-turn-transform fragility (§2) is why CPU movers are rect not per-poly; the GPU path sidesteps that entirely (no leaf-transform dependency). Confirm the shipped `MC2_GPU_PICK_HOVER` mech path already validates this.
- **R6 — Vulkan attachment export.** vulkan-l6 native subgraph must export attachment-2 + depth for pick to work under Vulkan; flag as an explicit L6 dependency, not an afterthought.

---

## 8. S1 RESULT (GPU-GROUND-PICK-PARITY-1) - measured 2026-07-02

**Branch** `claude/gpupick-1` (lane `A:/Games/mc2-gpupick`), commits `6a13ecc9` (oracle) -> `5264dcf3` (smoke allowlist) -> `edbabaa4` (throttle+reason breakdown) -> `0fe43a7b` (center probe). Gate `MC2_GPU_GROUND_PICK_PARITY` **default OFF**. Wired parity-only at 3 sites in `code/missiongui.cpp`: hover quad-walk branch (`inverseProject(mouseXY)`), L-click move, R-click move. GPU result NEVER consumed. Emits `[GPU_PICK_PARITY]` (delta hist + timing + fallback reasons) at process exit. Accessor `RenderWorld::IsGpuGroundPickParityEnabled()`.

**Soak:** mc2_01 + mc2_24 + gaea_peaks_01 (displaced), 30 s each, gate-ON, deploy v0.4c then v0.4 (spare contention forced the hop). All runs **3/3 PASS, GL-clean, +0 destroys**.

### The load-bearing finding (S2-blocking)
`lookupAtPixel` **does not return a ground position for bare-terrain pixels.** Control flow (`RenderWorld.cpp:868-916`): it reads BOTH objectID (`raw`) and `depthSample`, then at `:877` `if (raw==0u) return BACKGROUND_PIXEL` and at `:893-915` returns invalid on generation-mismatch / dead-slot -- **all before** the depth->world unproject at `:943-964`. So `worldPos` is computed **only for pixels covered by a live StaticProp/Mech**; a terrain-only pixel (objectID cleared to 0, or a stale recycled non-zero ID left in the last-write-wins integer attachment) never reaches the unproject and returns `worldPosValid=false`. Empirically: across all three missions the center-screen probe (looking straight at terrain) got **compared=0, gpu_invalid=all** (slot_dead -- the center pixel read a stale non-zero objectID, not 0), and the parked-cursor path likewise `compared=0`. **The depth-unproject the whole GPU-ground-pick premise rests on is currently unreachable for ground picks.** S2 MUST lift the unproject out from behind the objectID-validity gate (see S2 below). This is the single most important S1 output -- it converts "adopt the GPU ground pos" from a wiring task into a `lookupAtPixel` contract change.

### R1 - stall vs quad-walk (per-camera-motion-frame, wolfman-class maps)
| mission | CPU inverseProject avg / max (us) | GPU readback avg / max (us) |
|---|---|---|
| mc2_01 | 477-530 / 813-2012 | 602-1320 / 760-3755 |
| mc2_24 | 686-712 / 1343-1787 | 994-1159 / 2523-5082 |
| gaea_peaks_01 | 678-725 / 1157-1710 | 714-898 / 3113-3487 |

The single-pixel synchronous readback is **comparable-to-slower** than the CPU quad-walk on stock-size maps, and much **spikier** (tail 3-5 ms throttled). **Un-throttled it froze mc2_24** (`heartbeat_freeze_play` at 37.4 s; gate-OFF control PASSes) -- worst-case readback hit **1.27 s** when fired every camera-motion frame, because the prior-frame attachment write was not yet visible and the readback fully stalled the pipeline. **R1 verdict: the every-frame synchronous readback is NOT a win on stock maps and is a hazard.** The GPU path only wins where the CPU quad-walk explodes (the oversized 1K map). Adoption REQUIRES the mouse-move cache the shipped `MC2_GPU_PICK_HOVER` uses (readback only on cursor-move), NOT an every-frame call. S1 default sample rate is now every-30th-site (knob `MC2_GPU_GROUND_PICK_PARITY_SAMPLE`, `_CENTER` for center probe) so the oracle itself cannot freeze the game.

### R3 - buffer availability
`buffer_unavailable=0` across all runs -- the objectID buffer stayed live through play; no pause/menu/ortho frames were sampled by the fly-throughs. R3 fallback wiring is present (`NO_FBO_OR_TEX`/`NO_POSTPROCESS`/`OID_BUFFER_DISABLED` -> counted as fallback) but was **not exercised** by unattended smoke. S2 must exercise it deliberately (pause the game with the gate on).

### Displaced-terrain (gaea_peaks_01) parity
**Not measurable in S1** for the same reason as the load-bearing finding: the center pixel over displaced terrain returns invalid (no objectID writer for terrain), so no GPU worldPos, so no delta. The slope-parity claim (recon sec 5: "GPU better on slopes") **cannot be tested until S2 makes `lookupAtPixel` return worldPos for terrain pixels.** gaea_peaks_01 ran clean (140 fps, 4214 frames) -- the gate is stable on displaced maps; the parity number is just gated behind the S2 contract fix.

---

## 9. Execution queue (S2-S5, opus-ready)

Ordering unchanged from sec 6, but S2 is **re-scoped** by the S1 finding: adoption is blocked on a `lookupAtPixel` contract change, not just consumer wiring.

### S2 - GPU-GROUND-POS-UNPROJECT (was GPU-GROUND-PICK-ADOPT) - size S/M
**Blocker resolved first, then adopt.** The S1 finding: `lookupAtPixel` discards a valid `depthSample` for terrain/background pixels.
- **Files:** `RenderWorld/RenderWorld.cpp` (`lookupAtPixel` `:868-964`), `RenderWorld/RenderWorld.h` (`LookupResult` doc), `code/missiongui.cpp` (hover ground-pos consume behind new gate).
- **Change:** compute the depth->world unproject **regardless of objectID validity** -- move the `:943-964` block to run whenever `depthSample>0` (and `ivp`/vw/vh present), BEFORE/independent of the `raw==0`/generation returns. Keep `isValid`/`kind`/`handle` object-identity semantics exactly as-is (a terrain pixel stays `isValid=false` for OBJECT identity but now carries `worldPosValid=true`). This is additive: existing object consumers (`EditorInspector`, `MC2_GPU_PICK_HOVER`) still gate on `isValid`; only ground-pos consumers read `worldPosValid`. Add a `depthOnly` fast path if the caller only wants ground pos (skip the record mutex + generation work).
- **Then adopt:** flip the hover ground pick to consume `res.worldX/Y/Z` (synchronous, same-frame) behind `MC2_GPU_GROUND_PICK` (default OFF), **with the mouse-move cache** (no readback when cursor still -- mirror `s_hover*` cache in the `MC2_GPU_PICK_HOVER` block `missiongui.cpp:1706-1720`). CPU fallback when `!worldPosValid` / buffer unavailable.
- **Acceptance:** (a) S1 parity oracle now shows `compared>0` with a real delta distribution over terrain incl. gaea_peaks_01 (the slope-parity claim finally testable); (b) red-X / LOS / drag-anchor visually identical gate-ON vs OFF; (c) `Camera::inverseProject` walk retired from the hover path when armed (Tracy `MIF.InvProj` -> ~0 on cursor-still frames via the cache); (d) R3 fallback exercised: pause the game gate-ON, confirm `buffer_unavailable`>0 and CPU takes over with no visual glitch; (e) tier1 5/5 GL-clean, no `heartbeat_freeze` (the cache prevents the every-frame stall).

### S3 - PICK-SERVICE-UNIFY - size M
- **Files:** new `RenderWorld/PickService.{h,cpp}` (or extend `RenderWorld.cpp`), `code/gameplay_pick.cpp`, `code/missiongui.cpp` (route hover+click+shift-pick through the service), `GuiRuntime/EditorInspector.cpp` / `EditorBridge/EditorRenderBridge.cpp` (editor pick routes through service).
- **Change:** `pickCursor(glX,glY) -> {objectHandle?, kind, worldPos, usedCpuFallback}` merging the 3 existing consumers (`MC2_STATIC_PROP_PICK`, `MC2_MECH_PICK`, `MC2_GPU_PICK_HOVER`) + S2 ground pos. Move-order click path (`missiongui.cpp` L/R-move `inverseProject`) adopts precise GPU depth-unproject behind the gate. Keep the CPU path live behind `usedCpuFallback` per sec 5.
- **Acceptance:** move-orders land on the correct elevated cell (parity vs precise `inverseProject` at the click sites, incl. distant clicks for the R2 reverse-Z precision check -- sample clicks at far terrain and confirm delta within terrain-cell tolerance); all 3 legacy consumers route through the service with byte-identical behavior gate-OFF; editor pick unchanged (additive).

### S4 - GPU-RECT-SELECT - size M
- **Files:** `RenderWorld/PickService.{h,cpp}` (`pickRegion`), `GameOS/gameos/gpu_cull_readback.*` (reuse the 3-slot ring), `code/missiongui.cpp` (drag-select box -> `pickRegion`).
- **Change:** `pickRegion(rect) -> set<handle>` for drag multi-select via the `gpu_cull_readback` ring pattern (fail-open, 1-frame lag acceptable for a drag gesture -- NOT the same-frame cursor path). Region `glReadPixels` of the objectID attachment over the rect; dedup handles.
- **Acceptance:** rect-select parity vs the CPU AABB set (`objmgr.cpp` mover screen-AABB) over a scripted drag; lag imperceptible; no per-frame stall (async ring, not synchronous).

### S5 - VULKAN-PICK-PARITY - size M (coordinate with vulkan-l6)
- **Files:** Vulkan backend attachment-read op (island subgraph export of attachment-2 + depth), `RenderWorld/PickService.cpp` (abstract "read attachment N at pixel/rect").
- **Change:** implement the single-pixel synchronous readback as `vkCmdCopyImageToBuffer` + fence-wait (bounded, like the GL stall); the async region path maps to the existing ring/fence Vulkan islands. FLAG (hard L6 dependency): the native subgraph MUST export attachment-2 + depth in its intermediate.
- **Acceptance:** GL vs Vulkan pick pixel-parity on the islands where attachment-2+depth are exported; the S1 parity oracle runs cross-backend with 0 unexplained mismatches.

**Cross-cutting for all of S2+:** the synchronous readback MUST stay behind a mouse-move / dirty cache (R1 -- proven a freeze hazard when fired every frame). Never call `lookupAtPixel` unconditionally per frame.
