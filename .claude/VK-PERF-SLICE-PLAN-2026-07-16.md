# VK Perf Slice Plan — synthesis of 5-area recon (worktree `A:/Games/mc2-vk-interop-bridge`, branch `claude/vk-interop-bridge-1` @ `06e13dfe`)

## Dedup map

The five recon lanes independently converged on the same three cost centers; the plan below dedupes them:

| Cluster | Reported by | Essence |
|---|---|---|
| **A. Submit/wait lockstep** | areas 1, 2, 3, 5 | 6-15 CPU-blocking GPU drains/frame (per-island `vkWaitForFences` inline + `vkQueueWaitIdle` in fog/posts/composite). The recorded "honest next lever" (`.claude/VK-OPT-BACKLOG.md:35-40`). |
| **B. HUD tail CPU churn** | areas 1, 2, 3, 4 | Per-draw full-atlas RGBA deep copy (~59-80/frame, >99.9% never read), getenv storm in the same loop, 4x vertex copies, mirror alloc churn. |
| **C. Island-local waste** | areas 3, 5 | Prop GL light-SSBO readback (measured ~1.5 ms block), mech double bookkeeping, prop feed re-flatten, CSM re-sort (opt-in only). |

Verified no-win / do-not-re-mine: present verify oracles (already env-gated + throttled), VK terrain fragment cost (parity-mandated), GPU-cull readback (correctly engineered; measurement debt only).

---

## Top 5 slices (ranked by expected-win / effort)

### 1. VK-HUD-GATE-HOIST-1 — hoist getenv gate reads out of the flushHUDBatch per-draw loop
- **Win/effort:** 0.1-0.5 ms/frame steady, up to several ms during weapon fire (per-triangle gosFX route multiplies the calls) / **trivial**. Highest ratio in the set.
- **Evidence:** `GameOS/gameos/gameos_graphics.cpp:8186, 8201, 8229` (3x `subGate("MC2_VK_VFX")` per draw), `:8249` (raw `getenv("MC2_VK_HUD_DRAW_DUMP")` per draw); `vk_shared_gbuffer_gate.h:73-101` (subGate un-memoized by design). ~380-450 CRT env scans/frame at field HUD density; thousands under fire.
- **Measurement recipe:** No pre-profiling needed — safe static win. Proof-of-parity: `[VK_HUD_AUDIT]` calls/textured counts and `hud_vk_*` health counters bit-identical before/after; tier1 smoke (canonical command) as regression gate. Optional: one coarse Tracy zone around the flushHUDBatch loop banked pre-change (also serves slice 2).
- **Slice shape:** Hoist the three `subGate` results into locals above the loop; make the DRAW_DUMP getenv a function-local static. **DISARM-1 semantics preserved exactly** — memoize only env tokens, never fold in `g_mc2VkSessionForcedGL`; per-flush granularity satisfies the frame-boundary disarm intent. Add a one-line comment so the next session doesn't re-fuse them. Gate-off GL path untouched.

### 2. VK-HUD-MIRRORCOPY-1 — stop deep-copying full texture RGBA mirrors into every textured HUD draw
- **Win/effort:** 0.3-2 ms/frame steady (several MB memcpy + ~80 heap alloc/free pairs per frame), more under fire (per-triangle sprite copies) / **small**.
- **Evidence:** `gameos_graphics.cpp:8191` (`draw.textureRgba = tex->getRgbaMirror()` — const-ref return at `:1185`, assignment deep-copies into `VkHudDraw`, `engine_present_vk.h:117`). Consumer `ensureHudTextureMirror` (`engine_present_vk.cpp:1258-1261`) reads bytes only on (handle,serial) change. **Waste already field-proven:** `hud_vk_textured_last=59-69`/frame vs `hud_vk_texture_uploads=10` across 900+ frames.
- **Measurement recipe:** The existing counters ARE the proof — no live profiling required. Bank the slice-1 Tracy zone number pre/post for the ms figure. Parity: `hud_vk_textured/serialized/uploads` counters unchanged; portrait/comm-video re-bake still triggers uploads (serial-bump path exercised); one banked fire-event capture pair (`MC2_VISUAL_CAPTURE_ON_FIRE`, compare vs `.claude/vfx-fire/`).
- **Slice shape:** Preferred: gosTexture holds mirror as `shared_ptr<const vector<uint8_t>>`, `VkHudDraw` stores the shared_ptr (refcount bump, lifetime-safe across mid-frame `gos_DestroyTexture`/txmmgr cache-out). Alternative: `hasMirror(handle,serial)` query on EnginePresentVk, skip copy on hit. Must keep the (handle,serial) keying and 4-frame eviction window (glyph-corruption fix) intact.

### 3. VK-PROP-LIGHTFEED-NATIVE-1 — delete the prop island's per-frame `glGetBufferSubData` light readback (port the mech item-9 pattern)
- **Win/effort:** several hundred µs up to ~1 ms/frame CPU + removes a GL↔VK ordering hazard / **small**, and it is a **proven pattern with an existing audit harness**.
- **Evidence:** `vulkan_color_prop_island.cpp:1316-1318` (per-frame GL client sync + full LightsData copy; receipts show the block region ~1.5 ms: `total_us=1813` vs `stage_inst_us=10.6` + `draw_us=273` in `.claude/props-probe/vk_receipts.json`). Mech already retired the identical readback (`vulkan_mech_island.cpp:979-1010`, native CPU feed + FEED-AUDIT byte-compare).
- **Measurement recipe:** Before/after `total_us` in the props receipts (expect ~1813 → <800). Correctness: run the ported FEED-AUDIT byte-compare (native feed vs GL SSBO readback) for N frames **before** deleting the readback; `MC2_VK_FEED_DUMP` golden feed as backstop; tier1 smoke.
- **Slice shape:** Extend `mc2_color_prop_island_get_light_feed` (`gos_static_prop_batcher.cpp:8540`, currently exports only the GL handle) to export the CPU byte source (`mcTextureManager` lightData_, already exported to the mech feed); island memcpys from CPU feed; keep readback behind a temporary audit gate for one slice, then delete.

### 4. VK-QWI-TO-FENCE-1 — replace every `vkQueueWaitIdle` with a dedicated per-submit fence; fold fog into the composite submission
- **Win/effort:** 0.3-1 ms/frame field env (more with posts armed or FIFO vsync — the composite's queue-wide drain currently transitively waits the previous frame's present blit, which gates on the acquire semaphore) / **small**. Also the risk-free stepping stone toward cluster A.
- **Evidence:** `engine_present_vk.cpp:2010-2014` (composite, every frame via `gos_postprocess.cpp:4384`), `vulkan_post_fog_native.cpp:713-721` (every frame), `vulkan_post_ssao_native.cpp:878`, `vulkan_post_hzb_native.cpp:757`, `vulkan_post_screenfx_native.cpp:957/1137`, upload helpers `vulkan_mech_island.cpp:397` / `vulkan_color_prop_island.cpp:494`, `vulkan_terrain_island.cpp:1487`. These islands have no fence only because they pass `VK_NULL_HANDLE` at submit.
- **Measurement recipe:** Add water-style `us_fence_wait` timers (pattern: `vulkan_water_island.cpp:977`) around each converted wait; expect composite wait to stop scaling with present latency under FIFO. Field FPS A/B (window focused, real input — smoke FPS is a documented artifact and must NOT be used for FPS deltas). Sync-validation layer run + tier1 smoke + strict canary.
- **Slice shape:** One island per commit. Semantics preserved: each pass's own fence still fully retires its cbuf before reuse; only the accidental waiting on unrelated queue work is removed. Same-queue submission order already guarantees composite-before-present GPU ordering (the waitIdle is not load-bearing for that). Fog fold-in (fog runs immediately before composite, 2 fullscreen triangles) removes one entire submit+drain at trivial risk. **Caveat:** the composite's own drain cannot be fully removed yet — composite and copy-2 share `s.cbuf` (`:2029/:2261`) — that is slice 5.

### 5. VK-PRESENT-SINGLESUBMIT-1 — merge composite + tail HUD/ImGui + swapchain blit into one command buffer / one submit; delete the composite drain
- **Win/effort:** 0.3-2 ms/frame (resolution-dependent: composite pass ~0.3-0.5 ms at 1707x960, 1-2 ms at 4K, pulled onto the CPU critical path today) + halves present-path submits / **medium**. Strategic value exceeds the ms figure: it is the hard **prerequisite for 2-frames-in-flight** and for the cluster-A collapse.
- **Evidence:** `engine_present_vk.cpp:2009-2014` (submit + `vkQueueWaitIdle`), `:98-102` (single-frame sync by design), `:2231-2371` (second submit for tail+blit; fence wait at `:2231` is near-free only because the drain already emptied the queue), `:2029/:2261` (shared `s.cbuf` is the root cause), barriers at `:1977-1986` and `:2271-2339` already provide GPU-side ordering across a single submission.
- **Measurement recipe:** `[VK_PRESENT]` health receipts + present_frames counters unchanged; `MC2_VK_PRESENT_VERIFY` oracle (bounded-window FNV hash, frame%60) proves pixel-identical present pre/post; timer around the deleted drain site; field FPS A/B; fire-event dump (`MC2_VK_PRESENT_DUMP_ON_FIRE`) for the tail-HUD path.
- **Slice shape:** Give the composite its own cbuf ring slot (or record composite+tail+blit into one cbuf), single `vkQueueSubmit`, keep the existing inFlight fence (waits the PREVIOUS frame) as the only CPU wait. **Re-audit required:** `presentImageVkOwned_` handshake (`:2198-2219`), strict canary ordering assumption, and the DISARM-2b fail-soft path where composite records but present declines. Do NOT bundle the copy-2 blit-elimination (compositing directly to swapchain) unless free during the same restructure — modest GPU-only win (~0.1-0.3 ms) with format-variant costs for HUD/ImGui passes and loss of the sharedImg verify tap.

**Cumulative top-5 estimate:** ~1.5-6 ms of the ~10.7 ms VK-vs-GL frame gap (40 vs 70 FPS), all low-to-medium risk, before touching the structural lever.

---

## Follow-on (NOT in top 5 — needs its own arc + profiling first)

- **VK-ISLAND-PIPELINE-ARC (cluster A full collapse)** — defer each island's fence wait to next reuse of its parity slot / batch islands into 1-2 submits. Expected 3-8 ms/frame (the dominant share of the gap). **Blocked on:** slice 5 (cbuf split), per-parity staging rings (water vsUbo/fsUbo, shadow instAlloc/uboAlloc, terrain patch UBOs are single-instance and rewritten under the inline-wait guarantee), and the incidental GL↔VK seam ordering the drains currently provide. **Profile first:** roll water's `us_fence_wait` telemetry to all islands + `MC2_VK_GPU_TIMESTAMPS` to bank per-island CPU-wait vs GPU-pass numbers; do one island at a time under tier1 + sync-val + field-check gates. Coordinate with the frame-graph / GPU-BUFFER-OWNER-RECON-1 lane owner — same fence-wait family.
- **VK-VFX-COALESCE-1 (gosFX per-triangle draws)** — enqueue-side append-coalescing in the drawTris IsHUD branch (`mlrsortbyorder.cpp:589-609` → `gameos_graphics.cpp:6438-6452`). Est. 1-5 ms during fire. **Measure first:** bank a fire-event draw-count capture (`MC2_VK_HUD_DRAW_DUMP` + `.claude/vfx-fire/` pairs) to size triangle counts; paint order preserved by append-only-to-previous-call, so risk is low once sized. Slices 1+2 shrink this cost proportionally — re-measure after they land.

## Measure-before-coding (do not write code until the counter says so)

| Item | Gate/recipe | Threshold to act |
|---|---|---|
| Mech lights static-prefix skip (`vulkan_mech_island.cpp:984-990`) | add stage_us sub-split via existing timers (`:1037/:1172`) | memcpy share is a meaningful fraction of 150-522 µs stage_us |
| HUD gosMesh orphan churn (`gl_utils.cpp:435-445`) | read the existing `[GPUBUF v1]` Hud owner tally from one field log | meaningful per-frame bytes |
| HUD mirror alloc recycle / `[VK_HUD_MIRROR]` churn | grep an existing long field log | counts climb during sessions |
| GPU-cull readback (backlog #3) | user-driven Tracy at wolfman only | measurement debt — never re-plumb |
| CSM 5x re-sort/re-stage (`vulkan_shadow_island_live.cpp`) | zero field win (gates opt-in); needs GL-shadow-consumer receipt before cutting the double render | only when default-arming the shadow island |

## Do-not-touch (verified no-win — recorded to prevent re-audit)

- Present verify/readback oracles: correctly env-gated + windowed + throttled; zero cost in field config (`gameosmain.cpp:1953-1955`).
- VK terrain fragment cost: byte-matched to GL by the parity mandate; any cut breaks the bookmark-MAD gate by construction. Requires a user-level perf-over-parity ruling first.
- GL semaphore/EXT_external_objects traffic, hot-path glFinish/glFlush, GL glMemoryBarrier centralization: already deleted/optimal per area-2 scope verification.

## Conflicts / sequencing constraints

1. **HUD-tail correctness currently depends on the composite drain** (single HUD vertex buffer, in-place mirror updates, 4-frame eviction window are race-free only because of it). Slices 4/5 preserve single-frame-in-flight so they are safe as specified — but any later frames-in-flight change must ship together with per-frame HUD vertex buffers, mirror double-buffering, and a re-derived eviction window. Document the dependency in-code during slice 5.
2. Slices 4 and 5 both touch the `engine_present_vk.cpp` submit tail — sequence them (4 then 5), never parallel-lane.
3. Copy-2 blit elimination is only worth taking inside slice 5's restructure (acquire must move before composite recording); standalone it is churn.
4. Slice 2's shared_ptr choice constrains nothing later; the fetch-by-handle variant would interact with mid-frame texture destruction — prefer shared_ptr.
5. All slices behind the existing umbrella gates, gate-off GL path byte-identical, tier1 smoke (canonical command, never `--kill-existing`) per slice, field FPS measured with a focused window only.