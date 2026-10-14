<!-- doc-status: current | reviewed:2026-10-13 -->
# Lane 2 midday session cleanup review (38a248bf5..daa328fe0, 12 own commits)

Reviewer: read-only Sonnet pass, worktree `A:/Games/mc2-lane2`, branch `claude/lane2-s3`, no builds, no launches. Verified with `git show`, the code, the committed
artifacts and these re-runs (all green): pytest on the 8 touched Python test files (74 passed), `cxx17_ratchet --check`, `tools/callers.py --check`,
`check-background-launch.py`, `log_tag_census --check`, `capture_index.py --check`, `check-capture-resolution.py --current` and `--selftest`, `bless_selftest.py`,
`check-golden-coverage.py`.

## Verdict: ACCEPT-WITH-FIX

No defect in a shipped code path. The fixes are one design gap that the next Vulkan batches will hit (M1), two proof gaps (M2, M3) and small accuracy / hygiene items.

## Findings

### Medium
- **M1 the context keeps no record of what it enabled.** `vulkan_context.cpp:347-381` builds the feature chain from the local `verdict`, then drops it; only
  `bufferDeviceAddress_` (private, `vulkan_context.h:109`) survives. R1 batches 4-6 must "never call an unavailable bit" but no island can ask whether
  dynamic rendering / sync2 / timeline / descriptor indexing is enabled or by which route. Today this is harmless (no consumer, see the list below); the
  first consumer will have to re-derive it. Fix: keep `profile::Evaluation` (or a small `EnabledFeatures`) in `VulkanContext`, expose `const profile::Evaluation& profile() const`, and
  test that a consumer reads `featureEnabled()` before a call. Do it before batch 4.
- **M2 the refusal path and the final VK build were never run.** The unit tests cover `evaluate()` (required-missing, wanted-missing, every limit, 1.1 device,
  1.1 instance, swapchain only when presenting, descriptor-indexing group, the exact line); nothing drives the context through a refusal. Reading the code the
  path is sound (`fail()` -> `destroy()` tears down instance/messenger, `profileLine` survives, a second `init()` returns false without retrying, the reason names the
  missing items), but it is unrun. Also the only live VK evidence (profile line, VK tier1 5/5 `11-01-55`) is from exe 4f91ac2d8; `69f421440` and `3634a9ac1`
  changed `vk_device_profile.h`/`vulkan_context.cpp` afterwards and the final tier1 (`11-50-22`, exe 9c876ecb7) was GL only. Fix: one VK-umbrella mission at HEAD, and one scratch build with
  `kFloorApi` = 1.4 (not committed) to watch refusal -> GL with the log line; record both in the state file.
- **M3 `capture_index.py` rewrites the record from what is on disk.** `build()` lists the directories under `.claude/l2cap`; the lane's own open item (e) deletes
  the 11 GB of PNG sets, and the next regeneration then erases every VOID flag the index exists to keep (`capture_index.py:93-97`). `--check` is also STALE on any clone (the sets are untracked).
  Fix: before deleting anything, make the tool keep lines of vanished sets (marked `gone`) or freeze the file and stop regenerating it.

### Low
- **L1 refused count.** Commit c5214e23e and the state file say "15 refused"; `golden.json` has `n_regions 25`, `n_refused 14` (13 `unstable` + 1 `degenerate`, the 167-byte
  `distant_ridge_base` crop). The 4K set is 24 blessed + 15 refused (14 + 1). Same class, same 39 regions, one region (blessed at 1080p) moved. Correct the number.
- **L2 VOID count.** The index has 22 `**VOID**` lines, not 23 (`grep -c VOID` counts the header sentence). `probe-ab` has no `capture_run.json`, prints `unrecorded` and is
  not flagged, while the header says VOID covers "the sidecars say nothing". Spot checks agree with the rule: `f21-after-vk`, `p3a-vk`, `w2base-vk` declare only
  `MC2_RENDER_BACKEND=vulkan` (no umbrella) with `readback_source gl_fbo`; `probe-vk-noumbrella` records `drew gl`; the `-vk-day/-night` and `-vkarm` sets are `vk_present`, not flagged.
- **L3 option column is relative to HEAD's table** (`capture_index.py:option_differences`), not to the defaults of the exe that drew the set. After today's flip a pre-e9e3f19d6 set that ran PBR ON
  (the default then) shows `StaticPropPbr=true`, and `t1-oldexe-pbr0-gl` (old exe whose default was ON, forced OFF) shows `defaults`. Fix: say "differences from the defaults at index time (HEAD)" in the header, or print the exe's own default.
- **L4 stale doc.** `.claude/VK-PARITY-GAPS.md:40` (row 22, edited in 4d95c4334) still says "both default ON since 559714587".
- **L5 the `[VK_PROFILE v1]` line is not the full enabled set and the extension list is read twice.** `extensions=` holds only profile-selected extensions; `VK_EXT_memory_budget`
  (`vulkan_context.cpp:337-345`, enabled) and `VK_KHR_swapchain` are absent, because the line is printed before `devExts` is complete; the live line says `extensions=-` although the budget extension is on, and
  `tests/smoke/test_vk_profile.py:11` fixes a line with `extensions=VK_EXT_memory_budget` that the engine never prints. The context also re-enumerates device extensions for the budget bit
  while `gather()` already holds them ("query once"). Fix: use `caps.hasExtension()`, make memory budget a table row (wanted) or print all enabled extensions, align the fixture.
- **L6 `[VK_PROFILE v1]` is invisible to the log-tag census** (header-built string): not in `tests/unit/log_tag_baseline.json`, so a rename passes unseen. The lane wrote the lesson down (state, lesson 4) and did not register it. Register it by hand or teach the census `vk_device_profile.h`.
- **L7 stale comments:** `vulkan_context.cpp:183` and `:328` still say "(apiVersion 1.1)"; the instance now asks for 1.2/1.3.
- **L8 simplification / ratchet.** LG06 +1 is avoidable in code written this session: `ItemResult::id`/`route` are `const char*` and the `add` lambda takes `const char* id` (`vk_device_profile.h:71-76,116`);
  `std::string_view` (literal-backed) drops the site. Routes are also tested by first character (`timeline[0]=='c'`, `dr[0]=='e'`, `:124-151`) and by `strcmp(...,"core")`
  (`vulkan_context.cpp:369-370`): an `enum class Route { Core, Ext, Limit, None }` removes both. LG11 +1 (escaped quote read as a path backslash) and LG20 +1 (launcher JSON key names are not env reads) are scanner false positives: justified, nothing contorted.
- **L9 process comment in code:** `tests/unit/test_gate.cpp` (`#include "vk_device_profile.h"` line) says "the one unit file the capture lane may edit". That is a process rule, not a WHY of the test; it will rot. Drop it.
- **L10 launcher test guard is mtime-based** (`test_launcher_legacy_env.py:28-34`, autouse): any tree where `Gates/render_options*.h` or `mc2_launcher.cpp` is newer than the built exe fails the whole file. Intended, but a merge into nifty turns pytest red until `mc2_launcher` is rebuilt.
- **L11 VRAM adapter choice.** `gpu_memory_report.cpp:74-103` picks the DXGI adapter with the most dedicated memory, not the one the GL/VK context uses (no LUID match). Right on this host; wrong on a two-dGPU host. Document it or match the LUID. VK `load_end` reads usage 0 (lazy islands): known, compare `run_end`.
- **L12 baselines.** 688f8a38c says "re-recorded"; the diff only adds 5 `tier1` entries (none existed before) and `perf` is write-only (nothing reads it), so `perf.resolution` is informational. Harmless; the wording overstates.

## Wanted facilities used unconditionally elsewhere in the VK islands: NONE
Grepped the whole tree (cpp, h, shaders, no `build*`, `3rdparty`) for `vkCmdBegin/EndRendering`, `VkRenderingInfo`, `PipelineRenderingCreateInfo`, `vkCmdPipelineBarrier2`, `vkQueueSubmit2`,
`VkSubmitInfo2/DependencyInfo/*Barrier2`, `VK_SEMAPHORE_TYPE_TIMELINE`, `VkTimelineSemaphoreSubmitInfo`, `vkWaitSemaphores/vkSignalSemaphore`, `vkGetBufferDeviceAddress`, any `*DEVICE_ADDRESS*`,
`VK_DESCRIPTOR_BINDING_*`, `DescriptorSetLayoutBindingFlagsCreateInfo`, `UPDATE_AFTER_BIND`, `nonuniformEXT`, `GL_EXT_buffer_reference`, `VK_KHR_(dynamic_rendering|synchronization2|timeline|buffer_device)`:
the only hits are the profile and the context. The only `#extension` in `shaders/vulkan` is `GL_GOOGLE_include_directive`. `vkCreateDevice` and `vmaCreateAllocator` exist once. The fallbacks are therefore not a lie; they are vacuous today (no consumer), which is what M1 is about. Note all 8 feature bits are enabled although nothing uses them.

## Confirmed correct
1. **Profile table = the review** (`vk-best-practices-review-2026-10-12.md:141-165`): 1.2 floor on device and instance, KHR route for dynamic rendering / sync2 on 1.2, core 1.3 when both have it, timeline and BDA core 1.2, descriptor indexing = umbrella + `runtimeDescriptorArray` + `shaderSampledImageArrayNonUniformIndexing` + `descriptorBindingPartiallyBound`, BDA also told to VMA. Limits and swapchain are the lane's additions (limits 4 sets / 128 B push are spec minimums, so they never refuse; islands use at most 2 sets). Doc table, header and tests agree.
2. **Gather once at device creation** per physical device; feature structs chained only where valid (`usableApi() >= 1.2`, `1.3` struct only on core 1.3); no `pEnabledFeatures` alongside the features2 chain; `devExts` strings outlive the call; VMA gets `min(device, instance)`; discrete-first pick with a refused-device line when none qualifies.
3. **Refusal and grading:** `fail()` -> GL, line and reason logged, no crash path found; `logparse` `vk_profile_refused` is a red bucket, `vk_profile.grade` reds a refused or self-contradicting line, parser skips truncated lines; a mission with a refused line goes red (`test_vk_profile.py`).
4. **PBR flip:** `Gates/render_options.h` false, launcher row "(OFF)" with `NULL` offVal like the other default-OFF cfg rows (`SaveCfgRows` writes TRUE/FALSE from the checkbox, so offVal is not used), `test_gate.cpp` (defaults, override, JSON `"StaticPropPbr": false`), `test_render_options_pin.py`, matrix split into `pbr_on` + `ambient_off` (no stale id left), `tier1_env_vars.md` text, GraphicsOptionsWindow text, batcher comment all say OFF; Ambient stays ON everywhere; the golden, tier1 `report.json` and the `t1-*` sidecars record PBR false. Default-equality: 25 px vs a 20 px floor is stated honestly in the ledger and state (2 clusters; the montage was viewed); the number itself is consistent with the `t1-*` sets (exe 98fc6705b with `StaticPropPbr=0` vs e9e3f19d6).
5. **Capture review fixes:** `check_backends` runs in `compare` after `check_options` (exit 1, "VK arm UNPROVEN"), covers a missing sidecar and a GL-expected set that drew VK; `drew` is a sorted list end to end (`capture.py` `_as_list`, `bless.py:327`, manifest) with no str-expecting reader left; `fail_reasons` in the manifest; candidate set records backend and options; launcher write is tmp + `FlushFileBuffers` + `MoveFileEx` (`mc2_launcher.cpp:824-836`) and a failed rewrite logs `migrate_failed` and keeps the keys; `CountUnknownEnvKeys` counts a repeated key once, ignores values, caps names at 8 with `+N_more`; headless `--sync-options` calls `GfxSetDefaults`. Tests exist for each (gl-requested/vk_present branch, `capture.main` and `run_visual_capture.main` exit codes single and multi run, dropped keys, failed rewrite).
6. **Re-bless:** `CURRENT_GOLDEN`, `verify.py --golden current`, `check-capture-resolution.py --current` resolve to the same set and agree (1920x1080); `golden.json` carries the 17 options (PBR false), `declared_env` (`MC2_SMOKE_FREEZE_ANIM=1`), backend `gl/gl`, exe e9e3f19d6 clean; `bless.py` refuses missing/different options and a backend that is not the requested one (selftest T13-T16 fire); `verify.py` refuses an options mismatch unless `--vary`. The 14 refusals are the 4K set's class (unstable run-to-run, one degenerate crop). tier1 is pinned in the one place the runner builds the env (`run_smoke.py:1391`, `tier_size_env`), the log line `[RESOLUTION] Pre-window: 1920x1080` is demanded, and the report header says so; `--mission`/adhoc runs stay unpinned by design.
7. **VRAM:** GL = `GL_TEXTURE_FREE_MEMORY_ATI` (4 ints, KiB; first two used) and `GL_NVX_gpu_memory_info`, extension lists probed once, GL error queue drained; VK = `vmaGetHeapBudgets` summed over device-local heaps with `VK_EXT_memory_budget` enabled when present; DXGI `QueryVideoMemoryInfo(LOCAL)` is the process counter. One line at `load_end` and one at `run_end` (smoke or `MC2_DIAG_VERBOSE` only), Tracy plot every 120 frames, parser/report/baseline tests, census baseline gained `VRAM`. Final tier1 `11-50-22` carries the VRAM line (mc2_01 1624 -> 3782 MiB).
8. **Proof:** commit trailers name the regenerated files; `callers.json`, the background-launch inventory, the census, `gated-layout-macros.md` all re-check green at HEAD; tier1 `11-50-22` manifest says binary `9c876ecb757d`, `dirty false`, `verdict reconciled`, HEAD match; the cxx17 baseline check is green and its head stamp is the last code commit; a failed build was not deployed in this session. Edited regions carry WHAT+WHY comments (only L7/L9 are off).

## tests/unit outside the allowed line
Lane-2 commits touch two files under `tests/unit`: `test_gate.cpp` (e9e3f19d6: default-flip cases; 4f91ac2d8: +155 lines, 8 `vk profile:` cases) and the generated `log_tag_baseline.json` (487584705). `test_mc2str.cpp`, `test_objective_text.cpp`, `own_inventory_baseline.json` in the range are lane 1's via the nifty merge, not this lane's. test_gate.cpp is not changed on nifty since 5d29cecb1, so no conflict today.

## Next lane 2 session: do first
1. `git merge claude/nifty-mendeleev` (tip at review time 21354e71f), regenerate the five files below, rebuild `mc2_launcher` before pytest (L10).
2. M2: VK-umbrella mission run at HEAD (profile line, `Vulkan profile:` report line) and the scratch refusal build; note the results in the state file.
3. M1 (keep the evaluation in the context) and M3 (index preservation) before any PNG is deleted or batch 4 starts; then L1-L9 as one small commit set (L8 is the only C++ change besides L5/L7 and M1).
4. Still open for the owner: question 1 (REQUIRE the five wanted facilities).

## Likely conflicts with lane 1 at merge (changed on both sides since 5d29cecb1)
`tools/cxx17_ratchet/baseline.json`, `tests/callers.json`, `tests/callers_allowlist.json`, `tests/unit/log_tag_baseline.json`, `docs/background-launch-inventory.md` - all regenerable (`lane_merge.py` auto-resolves the baselines; run `callers.py`, `log_tag_census --write`, `check-background-launch.py --write` at the merged tree). Watch `tests/unit/test_gate.cpp` if lane 1's logging/step-3 work touches it, and `GuiRuntime/GraphicsOptionsWindow_terrain.cpp` (one comment here).
