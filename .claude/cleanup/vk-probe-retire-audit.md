# D7 — VK probe fixture retirement audit (2026-10-09, read-only at HEAD d628e1374; verdict DELETE all, phase 1 ≈ −9k lines)

Read from HEAD `d628e1374`; nothing was edited, built or run. **Verdict: DELETE all eleven fixtures plus the `vulkan_shader_probe` harness. Keep nothing.** Phase 1 removes about 9.0k lines. The one thing to carry over is the ~47-line SPIR-V loader, and that only because `vulkan_present_object.cpp` still calls it.

## 1. Who runs the probes today

**Nobody has, for months.**

- **Arming:** `MC2_VULKAN_PROBE` (presence-gated) arms `mc2_vulkan_probe_if_env()`, called at `gos_render.cpp:677`. `MC2_VULKAN_SWAPCHAIN_PRESENT` and `MC2_VULKAN_SWAPCHAIN_PRESENT_HIDDEN` add the swapchain-present probe. The standalone `vulkan_shader_probe` exe calls the probes directly.
- **Scripts and smoke:** `scripts/run_vulkan_probe.py` is the only driver. The only mention of the env vars in `scripts/run_smoke.py` is the Popen allowlist at lines 2871-2880; no smoke profile, capture profile, or `check-contracts.sh` row sets them or calls the driver.
- **Contract rows:** `check-contracts.sh` has none for the probe.
- **Last maintained:** 2026-07-02. `git log` for `run_vulkan_probe.py` and `vulkan_shader_probe_main.cpp` ends at `4d30d93e5` (WATER-MVP-ORACLE-1) that day. The last substantive skeleton change is the same date; the last touch of any kind is a 2-line edit in `de2ec973b` (2026-07-08, "retire fog islands").
- **Last recorded result:** `git log -S"VK_PROBE"` finds only archive/doc commits, never a probe result. The only recorded probe usage I found is the 2026-07-01 lib-track handoff in memory.
- **Already broken:** the edgefog fixture's `EdgeFogParams` is `static_assert == 100` bytes (skeleton ~:1990). The shader block `shaders/vulkan/edge_fog.frag` is 112 bytes since `96bc2957a` (2026-10-08, horizon-clamp fields). `FogOobParams` is 84 bytes against 96 (from the split plan). Any run would have failed or hit validation errors, and nobody noticed.
- **Build cost:** `vulkan_shader_probe` (root `CMakeLists.txt:154-198`) is not `EXCLUDE_FROM_ALL` and `MC2_VULKAN` defaults ON. Every full build compiles the 7.6k-line skeleton twice (once in `gameos`, once in the probe exe), plus `vulkan_shadow_island`, `swapchain_*`, `present_object`, `VkBootstrap`, `volk` and `vma_impl` again.

## 2. What each fixture guards that nothing else does

Fixture line ranges are in the 7640-line skeleton. None of the `*_cpu_ref` helpers is used elsewhere; they sit in anonymous namespaces, and the `*_cpu_ref` grep over `tests/`, `scripts/` and `tools/check_vk_shaders.py` returns nothing.

| Fixture (lines) | Shader | Live path now | Offline cover |
|---|---|---|---|
| shaders/triangle/descriptors/sampled_image/caps (187-285, 364-1648, 7471-7592) | `fullscreen.*` | Every VK island; `VulkanContext` init | `vk_shaders_compile`, `spirv_*`, `vk_pipeline_layout` |
| edgefog (1950-2650) | `edge_fog.*` | `vulkan_post_fog_native.cpp` loads the same `.spv` | `vk_ubo_layout` rows `postfog_edge` and `postfog_oob` check the live structs |
| oobfog (2651-3385) | `fog_oob.*` | same file | same rows |
| ssao (3386-4140) | `ssao.*` | `vulkan_post_ssao_native.cpp` | `ssaonat_occ` and `ssaonat_apply` UBO rows |
| hzb (4141-4882) | `hzb_reduce.*` | `vulkan_post_hzb_native.cpp` | `tests/unit/test_depth_hzb.cpp` pins the CPU convention |
| comparesampler (4883-5514) | `compare_shadow.*`, used by no live code | Live shadow sampling is `lighting_shadow_vk.glsl`, `screen_shadow_csm.frag` and the terrain-frame `sampler2DShadow`s | none specific |
| csmarray (5515-6178) | `csm_array_shadow.*`, used by no live code | same | none specific |
| shadowdepth (6217-6808) | `shadow_depth.*`, built by `vulkan_shadow_island.cpp` (202 lines, called only from this probe) | The live island is `vulkan_shadow_island_live.cpp` with `shadow_prop.*` | `vk_ubo_layout` row `shadow_live` and `check-vk-pipeline-layout` cover the live pipeline |
| watermvp (6843-7470) | `water_mvp.*`, a standalone oracle shader that the live water island does not use (it uses `water_geom`) | `vulkan_water_island.cpp` | `watervs` and `waterfs` UBO rows |

Live-path runtime checks exist, but outside `check-contracts`: `run_golden_parity.py --backend-compare`, `run_vk_field_check.py`, and the visual golden `reset-50331d5`. Two of the four SPIR-V checks in the "Offline cover" column catch layout and reflection drift only, not majorness (see section 5).

No unit test creates a VK device. `grep` for `vkCreateInstance`, `vkCreateDevice` and `volkInitialize` under `tests/` returns nothing. The three vulkan-named unit tests (`test_vulkan_contract`, `test_vulkan_layout_chain`, `test_subgraph_contiguity`) are pure offline table checks. The device-creating code under `tools/` is manual spikes (`tools/vk_gl_interop_spike`, `vk_hwnd_present_probe`, `vk_interop_transport_test`). A headless doctest would need an ICD in CI, which this repo does not have.

## 3. Does production depend on the probe section?

- **Real production dependents:** `mc2_vulkan_load_spv` and `mc2_vulkan_free_shader` (skeleton 140-186), used by `vulkan_present_object.cpp:383-384,749-750`.
- **The only path into those callers is test scaffolding:**
  - `vulkan_present_object` → `vulkan_swapchain_present.cpp` → the probe, or → `mc2_vulkan_composite_present_scaffold` (called from `gos_postprocess.cpp:4507`).
  - The scaffold runs only under `#ifdef MC2_VULKAN_ISLAND` and `MC2_VK_COMPOSITE_PRESENT` (default off, one-shot test pattern; call site `gos_postprocess.cpp:4604-4625`).
  - `engine_present_vk.cpp` has its own loader.
  - If the composite-present scaffold is also retired, the loader has zero users.
- **Other non-static symbols are probe-only:**
  - `mc2_vulkan_probe*` is called only from `gos_render.cpp:677`, `tools/vulkan_shader_probe_main.cpp`, and `mc2_vulkan_probe()` in the skeleton itself.
  - `mc2_vulkan_probe_swapchain` (in `vulkan_swapchain_probe.cpp`, 280 lines) is called only from the probe.
  - `mc2_vulkan_build/destroy_shadow_depth_pipeline` is called only from the shadowdepth fixture.
- **Not probe-only:** `mc2_vulkan_composite_present_scaffold` is declared in the skeleton header (`:237`) and defined in `vulkan_swapchain_present.cpp:162`. Its declaration must move to the slimmed header.
- **Anonymous-namespace helpers that the loader uses:** `log` (the `[VULKAN_SKELETON]` prefix) and volk. `ensure_volk_initialized` and the pipeline-cache helpers die with the probe.

## 4. Deletion plan

**Phase 1, mechanical, one commit:**

1. Create `GameOS/gameos/vulkan_spv_loader.cpp`, about 60 lines, holding the two functions with a private `log`. This is the home for the D5 de-dup of the 13 hand-copied `spv_dir()/load_spv()` helpers. Slim `vulkan_backend_skeleton.h` from 253 lines to about 25 (the loader pair plus the composite-present decl). Rename it to `vulkan_spv_loader.h` if desired.
2. Delete `vulkan_backend_skeleton.cpp`, `tools/vulkan_shader_probe_main.cpp` (65), `scripts/run_vulkan_probe.py` (415) and `vulkan_shadow_island.cpp` (202). Delete `vulkan_swapchain_probe.cpp` (280) and the `mc2_vulkan_probe_swapchain_present` function in `vulkan_swapchain_present.cpp`.
3. Remove the `mc2_vulkan_probe_if_env()` call and its `#include` from `gos_render.cpp:16,674-678`.
4. CMake edits:
   - Root `CMakeLists.txt`: drop the `vulkan_shader_probe` target (154-198).
   - `GameOS/gameos/CMakeLists.txt`: drop lines 105-111 (skeleton, shadow_island, swapchain_probe) and add `vulkan_spv_loader.cpp`.
   - `GameOS/gameos/CMakeLists.txt`: drop `compare_shadow`, `csm_array_shadow`, `shadow_depth` and `water_mvp` from `MC2_VK_SHADER_BASENAMES` (line 360) and the baking comments (324-338).
   - Fix the stale probe-target comment in `vma_impl.cpp`.
5. Delete the 8 fixture-only shader files, 283 lines: the vert and frag of `compare_shadow`, `csm_array_shadow`, `water_mvp` and `shadow_depth`. Keep `fullscreen.*`, which `vulkan_present_object` uses.
6. Remove these registry and doc rows (generated files are regenerated, not hand-edited):
   - Rows to remove: `MC2_VULKAN_PROBE`, `MC2_VULKAN_CACHE_DIR` (skeleton-only reader), `MC2_VULKAN_SWAPCHAIN_PRESENT` and `_HIDDEN`.
   - `check-env-registry.sh` allowlist lines 850, 855, 857, and the `RendererFeatureRegistry.h:3502` entry for `SWAPCHAIN_PRESENT`.
   - `flags.auto.yaml` (~9470-9530), `gate_table.seed.json` (~32433-32623), `gate-reader-conflicts.json` (skeleton rows ~7032/7039), `gate-history.md:1391`.
   - `tier1_env_vars.md:107-111`, and the `run_smoke.py:2871-2880` allowlist.
   - `check-split-path-contracts.py` ISLANDS rows at ~218-225 and ~312-315 for the four deleted shader pairs.
   - **KEEP** `MC2_VULKAN_SPV_DIR` (13 live readers) and `MC2_VULKAN_VALIDATION` (`vulkan_context.cpp`, `vulkan_present_object.cpp`). Fix the wrong "only consulted when PROBE" sentence at `tier1_env_vars.md:110`.
7. Doc edits:
   - Mark D7 / opus-consistency A7 and item 9 done in `ORCHESTRATOR-HANDOFF.md` and `opus-consistency-review.md`.
   - In `giant-file-split-plan.md`, strike §1's 5-way fallback and the §13 alternative, and update `file-size-baseline.md` row 10.
   - Update `docs/ARCHITECTURE-MAP.md:53`, `cxx17-strangler-plan` LG09 (398) and `narrowing-baseline.md:103` (the skeleton's one C4244 row).
   - Add a "historical" banner to `VULKAN-NEXT-SLICES.md`, whose canonical commands (lines 109, 404) call `run_vulkan_probe.py`.
8. Before deleting, make a local tag (for example `archive/vk-probe-fixtures`) so the oracles can be resurrected with `git show`. No push.

**Phase 2, optional, separate decision:** retire the composite-present scaffold: `vulkan_swapchain_present.cpp` (187), `vulkan_present_object.{cpp,h}` (775 + 149), the `gos_postprocess.cpp:4488-4530` and `4604-4625` blocks, and `fullscreen.*`. The loader then has zero users, so `vulkan_spv_loader.cpp` is unnecessary unless D5 adopts it.

**Vulkan flip checklist:** no item references probes or fixtures (grep for probe, fixture, skeleton and oracle finds only unrelated text). Items 5, 9, 11 and 12 rely on live captures and are unaffected. No edit needed.

## 5. What is lost

- **Generic triangle, descriptors, sampled-image, caps:** a broken VK basic-capability path would show up as every island failing under validation (smoke runs with `MC2_VULKAN_VALIDATION`).
- **edgefog / oobfog:**
  - Both CPU-reference oracles are stale: 100 against 112 bytes, 84 against 96.
  - The fixture's unique value was catching an `invViewProj` transpose (`ab8cfc7b5`). Today that is caught only by oblique-fog bookmark A/B (backend-compare), not by `check-contracts`.
  - `check_ubo_layout` verifies size but not column or row majorness, and no script lints `row_major` in `shaders/vulkan` (the only mention is the fixture-token comment in `run_vulkan_probe.py`).
- **ssao:** this is the largest real loss. It was a pixel-class math oracle (bit-tight classes plus an enveloped wall-adjacent band). If `ssao.frag` regresses, we would catch it instead via GL-vs-VK A/B on the SSAO bookmarks, which is eyes-on and not in `check-contracts`.
- **hzb:** a bit-tight min-reduce oracle. If it breaks, we would catch it via `test_depth_hzb.cpp` (CPU convention only) and over-culling in backend-compare.
- **comparesampler / csmarray:** the spike shaders are not live. If the sampler compareOp setup or CSM array select breaks, we would catch it via terrain/prop shadow parity pixels, not an isolated check.
- **shadowdepth:** it tested the dead `vulkan_shadow_island.cpp` pipeline, so nothing live is lost. The vertex-stride class is covered by `check-vk-pipeline-layout` (`good_shadow_stride` and `bad_shadow_stride_fallback` fixtures).
- **watermvp:** its proof (column-major plus reverse-Z fudge under an asymmetric matrix) was recorded in `4d30d93e5` and is rediscoverable through the tag. If water MVP regresses, we would catch it via the `watervs` UBO row and water A/B.

**KEEP list: none.** No fixture lacks a live path. Ssao and edgefog have the weakest substitutes, so retain their commit SHAs and the pre-delete tag in the doc trail.

## 6. Verdict per fixture

| Fixture | Verdict |
|---|---|
| shaders, triangle, descriptors, sampled_image, caps | DELETE |
| edgefog | DELETE (already stale) |
| oobfog | DELETE (already stale) |
| ssao | DELETE |
| hzb | DELETE |
| comparesampler | DELETE |
| csmarray | DELETE |
| shadowdepth | DELETE, with `vulkan_shadow_island.cpp` |
| watermvp | DELETE |

MOVE-TO-TEST is not viable today: no ICD in unit tests, and no precedent.

## Impact

- **Lines removed (phase 1):**
  - The skeleton goes from 7640 to 0, which is -7640 plus about 60 for the new loader file.
  - Probe main -65, `run_vulkan_probe.py` -415, `vulkan_shadow_island.cpp` -202, `vulkan_swapchain_probe.cpp` -280, header about -225, shaders -283.
  - Net about -8.8k to -9.1k lines.
  - Phase 2 would remove about 1.2k more, plus the scaffold in `gos_postprocess.cpp`.
- **`file_size_ratchet.baseline.json`:**
  - `files_over_1500` goes 100→99 and `files_over_3000` goes 37→36.
  - The `GameOS` per-dir counts go 19→18 and 9→8.
  - Max lines stay at 11819 (`gameos_graphics.cpp`).
  - The skeleton leaves the `top` list; `files_over_1500` and `top` drop its 7640 entry.
  - The new loader is under 1500 lines, so it needs no entry. Run `py -3 tools/file_size_ratchet.py --check --tighten`.
- **`dup_lines_ratchet.baseline.json`:** the skeleton entry (line 66) is 2878 duplicated lines of 74844, so about -3.8% of the total. `vulkan_shadow_island.cpp`'s internal clones go too; its baseline entry was not measured.
- **Build time:** removes the double compile of the skeleton and its companions in every full build.

## Phase 2 provenance (2026-10-09) — verdict DELETE; owner rule applied (functionality exists in engine_present_vk; never parked for a bug)

### Provenance: VK composite-present scaffold (`MC2_VK_COMPOSITE_PRESENT`)

#### Recommendation: DELETE (D7 phase 2 is safe)

The scaffold was never switched off for a bug. It was built as a deliberate, permanent test-only skeleton for a feature that later shipped through a different file, `engine_present_vk.cpp`. Neither of your tests for keeping it applies:

- **Functionality exists elsewhere:** the real present path is live, and it covers more than the scaffold ever did.
- **Post-processing thing parked for a bug:** nothing in the history, docs or memory says that.

Two things to handle when deleting, both below: the extra deletion surface, and one shader-file correction to the audit.

#### Timeline

| Date | SHA | Event |
|---|---|---|
| 2026-06-30 | `06946a87b` | `fullscreen.vert/frag` created in VULKAN-SHADER-TOOLCHAIN-1. Described as a "trivial" shader, "Not yet rendered anywhere". |
| 2026-07-01 | `8241e5916` | VULKAN-SWAPCHAIN-PRESENT-1 (L5). Throwaway probe in `vulkan_swapchain_present.cpp` (914 lines). Opens its own separate SDL Vulkan window, presents 16 frames, tears down. Sync validation caught a shared render-finished semaphore re-signal, fixed with one semaphore per swapchain image. |
| 2026-07-02 | `94d4cc8eb` | L6.6-lite COMPOSITE-PRESENT-SCAFFOLD, the only commit that ever touched the scaffold files. It extracted the probe into `VulkanPresentObject`. It added the `CompositePresent` region contract in RenderCore, the `gos_postprocess.cpp` test region, and the `MC2_VK_COMPOSITE_PRESENT` gate. |
| 2026-07-03 | `3e737f565`, `19cf4fb87` | P13 T3/T13b. `engine_present_vk.cpp` presents the real game frame, first on a hidden window and then on the real game window. |
| 2026-07-08 to 07-16 | (many) | The live path gains native composite, HUD/ImGui tail, resize hardening and single-submit present. |
| 2026-07-08 | `2b678e537` | Docs-only touch of the gate name ("register imgui gate"). |
| 2026-07-10/11 | `354f9b6f5`, `60b1ac8ac` | Rung 8 deletes the GL-to-VK handoff and exportable present image, so the "L7 interop wall" no longer exists. |
| 2026-10-09 | `4a4a72989` | D7 audit proposes phase 2 deletion. `ORCHESTRATOR-HANDOFF.md:47` lists it as "pending answer". |

The scaffold files have not changed since `94d4cc8eb` (2026-07-02). `fullscreen.*` has not changed since 06-30.

#### What the scaffold does

- **Test pattern only.** `acquireAndPresentOneFrame` clears to teal and draws the `fullscreen.*` triangle, which outputs UV as colour. It has its own `inFlight_` fence and a `vkWaitForFences` per frame.
- **Separate window and device.** It creates its own SDL Vulkan window, instance, surface, device and swapchain. It never touches the game window, FBO-0 or the GL swap.
- **Run-once call site.** `gos_postprocess.cpp:~4604-4625` runs it once, only under `#ifdef MC2_VULKAN_ISLAND` and when the env var is set. It presents 16 frames with a resize at the midpoint.
- **MainColor is never sampled.** The code says: "The composited MainColor is NOT sampled here (the fullscreen-triangle test pattern stands in)". The same file says bridging the real game image is "L7 interop, ledgered".
- **Not a post chain.** It is not a GL-to-VK interop present, and not a VK-rendered post chain.

#### Does `engine_present_vk.cpp` subsume it? YES

- **Live path:** `engine_present_vk.cpp` is 2767 lines. It presents the real composited frame through a swapchain on the real game window. It has native composite/FXAA/lowlight/object-id, HUD and ImGui tail, a single submit, and resize with `oldSwapchain` chaining and bounded retry.
- **Device ownership:** it borrows the shared engine device from `VulkanContext`. `vulkan_context.h:5-11` says the present object must not own a device on the engine path.
- **Code relationship:** `engine_present_vk.cpp` shares no code with `VulkanPresentObject`. The only mention is the comment on line 9, "mirrors ... per-image render-semaphore discipline". A grep finds no other user of `VulkanPresentObject` outside the scaffold and the L5 probe.
- **Own loader and shaders:** `engine_present_vk.cpp` has its own `loadSpv`, and has no references to `fullscreen`. It uses its own composite and blit shaders.
- **Nothing missing:** the semaphore-per-image fix, OUT_OF_DATE/SUBOPTIMAL rebuild and resize recovery are all in the live path, in hardened form. The scaffold's only unique content is the teal triangle and the 16-frame self-test.

#### Switched off for a bug? NO

- The commit message and `VULKAN-NEXT-SLICES.md:451-480` present it as the intended end state of L6.6-lite. Key phrases:
  - "build the reusable machinery + contract ONLY"
  - "banks the present machinery + contract so L7 has something real to plug into"
- There is no "parked", "reverted", "temporarily off" or bug wording anywhere. The default-off, ISLAND-only gating is by design, to keep the shipping build byte-identical.
- The only bug mentioned is the L5 semaphore hazard, and it was fixed in 8241e5916 and carried into the live path.
- **Gate registry text:** `MC2_VK_COMPOSITE_PRESENT` is unregistered. `flags.auto.yaml` has `registered: no`, no doc and no feature id. `gate_table.seed.json` has `doc: ""`, `retireBy: ""` and one reader. `gate-history.md` has no entry. The only statement of intent is the code comment: "opt-in ... must NOT run per-frame".
- **Open items:** nothing open references it. The present-wait perf item (`opus-modernization-review.md` §4.1, S5) cites `engine_present_vk.cpp:1972`, the `[QWI-WAIT]` log and the `VKIsl.Present.CompositeWait` zone. The scaffold would not be the base for that work; `engine_present_vk.cpp` is.

#### Corrections and extra surface for the delete

1. **`fullscreen.*` is deletable too.** The audit says to keep `fullscreen.*` for `vulkan_present_object`. Phase 2 deletes `vulkan_present_object` (and the skeleton, which also loads `fullscreen.*` at `vulkan_backend_skeleton.cpp:257,509`, goes in phase 1), so nothing then uses it. Two other places mention it:
   - `scripts/check-split-path-contracts.py:204-205` has allowlist rows for it, to remove.
   - `tools/vk_interop_transport_test/CMakeLists.txt:63` uses its own `shaders/fullscreen.vert` (a different file), so it is unaffected.
2. **The audit's list misses the rest of the `CompositePresent` region contract.** If you want it fully gone:
   - `RenderCore/RenderRegionId.h`: enum value, string case, `CompositePresentRegionContext`, and the `RegionOutput.presented` comment.
   - `RenderCore/IRenderBackend.h`: the `runRegion` overload.
   - `gos_postprocess.cpp`: about 28 references, including the GL backend adapter and the extern "C" `mc2_render_backend_composite_present_*` entry points, plus `gos_postprocess.h` (7 refs).
   - `debug_state_dump.cpp`: 6 refs.
   - `vulkan_backend_skeleton.h:230+`: the declaration.
   - **Open question:** whether to remove the contract or leave it as inert additive code.
3. **Housekeeping:**
   - Remove the CMake entries in the root `CMakeLists.txt:162` and `GameOS/gameos/CMakeLists.txt` (+4 lines).
   - Remove the `MC2_VK_COMPOSITE_PRESENT` row from the gate seed and regenerate `flags.auto.yaml`.
   - Fix `docs/ARCHITECTURE-MAP.md:59`, which still lists `vulkan_swapchain_present.cpp` and `vulkan_present_object.cpp` as present files.
   - After phase 2, `vulkan_spv_loader.cpp` is unnecessary (the audit already says so).
   - `MC2_VULKAN_VALIDATION` is also read by `vulkan_context.cpp`, so keep it.
4. **Resurrection:** the audit's plan to tag `archive/vk-probe-fixtures` before deleting covers this too. The L5 and L6.6 commits (`8241e5916`, `94d4cc8eb`) also survive in git history.

#### Confidence

Provenance is solid for creation and intent: one commit with a detailed message, plus a supervisor-ruling paragraph in `VULKAN-NEXT-SLICES.md`. It is thin for any later discussion, because the scaffold was never touched again and no docs or memory reference it beyond the D7 audit. I found no hint of a parked bug. I did not search the memory `rendering/` and `process/handoffs/` folders beyond one archived handoff hit (`HANDOFF-2026-07-02-session-end.md:74`, a list mention of "L6.6 present").

Orchestrator decision: phase 2 GO including removal of the CompositePresent region contract (RenderRegionId/IRenderBackend/gos_postprocess/debug_state_dump refs) and fullscreen.* — no inert code kept.
