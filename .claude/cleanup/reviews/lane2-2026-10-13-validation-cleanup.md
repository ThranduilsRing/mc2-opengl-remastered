<!-- doc-status: current | reviewed:2026-10-13 -->
# Lane 2 afternoon session cleanup review (daa328fe0..45c19e661, 18 own commits, the nifty merge cc05eaf6d skipped)

Reviewer: read-only Sonnet pass in `A:/Games/mc2-lane2` (branch `claude/lane2-s3`), no builds, no launches. Verified with `git show`, the code, the committed smoke artifacts
and these re-runs (all green): pytest on `test_vk_replay`, `test_capture_index`, `test_vk_profile`, `test_vk_validation` (42 passed), `cxx17_ratchet --check` (0 fewer sites than the baseline,
i.e. tight), `callers.py --check`, `check-background-launch.py`, `capture_index.py --check`, `git diff --check`, a NUL scan of every touched file, and a re-run of `vk_replay.compare` on the
committed scratch capture (replay frame 23 = application frame 462 = the live sidecar's `frame`, hash 7238bc8ab474a01f, 31 distinct hashes in 31 shots).

## Verdict: ACCEPT-WITH-FIX

No defect in a shipped default path: the profile, the refusal, the grading and the proofs do what the state file says. The fixes are one stale planning document that contradicts the
owner ruling (M1), one fallback that does not fall back (M2), and small accuracy items. The two remaining validation ids are located below.

## Findings

### Medium
- **M1 the best-practices review still plans per-facility fallbacks.** `.claude/cleanup/vk-best-practices-review-2026-10-12.md:153-160` has a "Fallback" column for dynamic rendering, sync2, timeline,
  descriptor table and BDA; rank 1's proof says "absent-extension fallbacks exercised" (`:173`), rank 4 "legacy paths" (`:176`), rank 5 "identical digests on a 1.2 extension device and 1.2 fallback device"
  (`:177`), owner question 1 (`:184`) is unmarked. The ruling (b2e3b05dc, `docs/vk-device-profile.md:7-11`) says an island has NO per-facility alternative. Batches 4-6 will be planned from this file.
  Fix: a "superseded by the ruling of 2026-10-13" note at the top, the Fallback column struck, the proofs reworded (a 1.2 device proves the KHR route, not a fallback), question 1 marked answered.
- **M2 the LUID fallback is dead and its label is wrong.** `GameOS/gameos/gpu_memory_report.cpp:121-137`: with a LUID, `takeIt = sameLuid`, so when the LUID names no DXGI adapter (remote session, WARP, a driver that
  reports a LUID DXGI does not list) nothing is taken, `adapter` stays null, the function returns null and sets `g_adapterRule = "most_dedicated"` although no adapter was chosen; the comment says "fall back rather than report
  nothing". Diagnostic only, never a crash. Fix: on `haveLuid && !matched`, run the old most-dedicated pass (loop again with `takeIt = DedicatedVideoMemory >= best`), then label the rule.

### Low
- **L1 comment count.** `vulkan_context.cpp:349` "Seven are required by the owner ruling": the ruling is five facilities = EIGHT bits (timeline 1, BDA 1, descriptor indexing 4, dynamic rendering 1, sync2 1); independentBlend is the ninth.
  The doc (`vk-device-profile.md`) and 5d591f936 say eight/nine correctly.
- **L2 a pre-1.2 device mis-reports `independent_blend`.** `vk_device_profile.cpp:41` returns before any feature read, so a 1.1 GPU that has the core 1.0 feature `independentBlend` is printed as missing it.
  `tests/smoke/test_vk_profile.py:15-19` (`OLD_GPU`) lists six `required_missing` ids; the engine would print seven for the same device (`independent_blend` last). Fix: read `vkGetPhysicalDeviceFeatures` before the early
  return; add the id to the fixture and its `len(...) == 6`.
- **L3 the instance cap and the invariant behind it.** `vulkan_context.cpp:171` requests `min(loader, kUseApi13)`: deliberate and not a bug (nothing in the engine uses 1.4 functionality; VMA is told
  `usableApi()`; a 1.4 device runs at 1.3). It is why the M2 scratch floor of 1.4 refused: the proof used a constant the engine could never meet. Missing guard: `static_assert(kFloorApi <= kUseApi13)` next to
  `vk_device_profile.h:32-33` (and the id `api_1_2` is a literal, so raising the floor makes it lie). In the loader-too-old path (`vulkan_context.cpp:174`) `instance_api=` prints the LOADER's version while everywhere
  else it is the created instance's; they are equal in every reachable case (loader < 1.2), so cosmetic.
- **L4 the 388063c80 wording was not run live.** The report `2026-10-13T12-49-20` shows the old, self-contradicting text ("loader reports 1.4.350, below the 1.4 floor"); the fix is text-only and unit-reachable only with a scratch floor.
- **L5 `vk_replay.py proof` ignores `--frames` for the replay.** `:219,222` pass `frames=None` to `run_replay`, which then screenshots replay frames 1-31 always (`:192`). `--frames 440-500` records 61 frames and replays 31:
  a live frame in the tail reports DIFFERENT. Fix: `f"1-{last - first + 1}"` from `parse_frames(args.frames)`.
- **L6 the frame mapping is a label, not a check.** `compare` computes `app_frames` from the capture's first frame (`:141-142`) and never reads the live sidecar's `frame`. The hash match is the proof (decoded RGB + size, `:117-123`);
  on a static scene several shots could match and the label would be ambiguous. Fix: read `<live>.json["frame"]` and make `equal` require it to be in `app_frames` (it is here: 462). Also `find_live_png` takes `sorted()[0]`
  (`x.png` beats `x_dup.png` only by ASCII order, `:157`), and `captured_file` takes the largest `.gfxr`, not the newest (`:148`).
- **L7 `capture_index --refresh` can downgrade a row.** `merge()` (`:159-174`): a set whose PNG/JSON files were partly deleted but whose directory remains is re-described with `poses_present 0`, `drew []` and VOID
  "no pose has a sidecar"; VOID is kept (correct) but the good data is lost. Guard: skip the refresh when the new row has fewer sidecars than the old one. Whole-directory deletion is safe (tested; `--check` OK).
- **L8 the Buildings.csv self-compare covers the blessed regions only.** 25 of 39 regions (18 + 7, all PASS 0.0); the 14 refused ones (13 unstable, 1 degenerate) are exactly where a difference would be invisible. The ledger
  notes (`gold1080-nocsv`, `gold1080-nocsv-01`) and the state file say "every blessed region", which is accurate; say "25 of 39" next to "nothing changed".
- **L9 validation tier gaps.** `vk_validation.py:grade()` has no production caller (the red comes from the marker bucket; only tests call it). A run that sets `MC2_VULKAN_VALIDATION` but never creates the VK context
  (GL fallback with no refused line, a GL-only mission) prints no `layer_enabled` and is not red: `describe()` only appends "(layer not confirmed on)". Make the runner red on "requested and not confirmed". The callback's
  `++validationErrors` (`vulkan_context.cpp:85`) is a plain increment from a driver-callable function (benign race).
- **L10 messages carry raw handles.** No `vkSetDebugUtilsObjectNameEXT` anywhere: `VkImage 0x310000000031` has to be decoded by creation order. Name the shared scene images, the light-feed ring and the HUD buffers first.

## The two remaining validation ids: likely sites (report `tests/smoke/artifacts/2026-10-13T15-59-39/mc2_01.log`, lines 299-381)
**VUID-vkCmdDraw-None-09600 (image in UNDEFINED where an attachment / sampled layout is declared).** Class: the shared G-buffer images are created `UNDEFINED` (`vk_image_resource.cpp:71`, called from
`vulkan_shared_scene_color.cpp:~118` for colour / normal / object-id and `vulkan_shared_scene_depth.cpp:~96` for depth) and NOTHING gives them their first layout; every consumer declares a layout from the start.
Evidence in the log: two command buffers, images `0x16` COLOR_ATTACHMENT (scene colour, once), `0x1f` DEPTH_STENCIL_ATTACHMENT (depth and stencil aspects) and `0x28` COLOR_ATTACHMENT (normal), each reported ONCE, and `0x31`
SHADER_READ_ONLY reported on every later submit. Handles 22 / 31 / 40 / 49 are 9 apart, which is the creation order colour, depth, normal, object-id (`gos_postprocess_fbo.cpp:182,248,306,345`; the run logs
`objectId(MRT2) active`). Sites:
 1. `GameOS/gameos/engine_present_vk.cpp:1983-1991` and `:2035-2043` (composite command buffer: barrier `oldLayout = COLOR_ATTACHMENT` on the scene and object-id images) and `:736-741` (descriptor 1 declared
    SHADER_READ_ONLY on the object-id view even when `needsObjectId` is false, `:1929-1931`). The first composite submit precedes the first terrain pass, so scene colour is still UNDEFINED (once).
 2. `GameOS/gameos/vulkan_terrain_island_island.cpp:454,460,488` (the first-owner CLEAR pass declares COLOR / DEPTH_STENCIL_ATTACHMENT_OPTIMAL as `initialLayout` over images that were never transitioned): normal and depth
    are reported at that submit (once).
 3. Object-id (`0x31`) is never written by any Vulkan island: `MC2_SHARED_SCENE_OBJECT_ID` appears only in `vulkan_shared_scene_color.*`, `gos_postprocess_fbo.cpp` and `engine_present_vk.cpp`, and no island render pass
    has a third colour attachment. So the composite samples an image of undefined content in view modes 1 and 3, and it stays UNDEFINED for the whole session (hence the repeats).
 Fix (one place, confidence high for the class, medium for the 0x31 identity): after `vkImageResourceCreate` in the two shared-scene Create functions, one one-shot submit: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL for colour / normal /
 object-id (clear object-id to 0, nothing else initialises it), UNDEFINED -> DEPTH_STENCIL_ATTACHMENT_OPTIMAL for depth (both aspects). Then every declared layout is true from frame 1. Confirm with object names (L10).
**VUID-vkDestroyDevice-device-05137 (three VkBuffers leaked).** Site: `GameOS/gameos/vk_shared_light_feed.cpp:25-27,86-110`: a ring of `kSlots = 3` lazily created VMA storage buffers (`vmaCreateBuffer`, `:105`) and no release
function anywhere (grep for Release / Shutdown / Destroy finds nothing); `gameosmain.cpp:1369-1400` tears down eight islands and the present path but never this ring; `VulkanContext::destroy()` then destroys the allocator
and the device with the three buffers alive. Three leaked buffers = three slots (handles 306 / 481 / 482: slot 0 re-created on growth, slots 1 and 2 together). The other raw `vkCreateBuffer` sites
(`engine_present_vk.cpp:968,994,2493`) all have a destroy in `EnginePresentVk::destroy()` (`:2641,2650-2653,2680`). Fix: `mc2vkSharedLightFeedRelease(device)` (wait idle, `vmaDestroyBuffer` per slot, reset `g_alloc`/`g_slot`)
called in `gameosmain.cpp` beside `mc2_vk_mech_island_destroy()`; also the "allocator changed" branch (`:57-68`) drops slots without destroying them.

## Judged items
1. **Required-facility flip: correct.** `vk_device_profile.h:136-193`: api, limits, timeline, BDA, descriptor indexing (umbrella + 3 bits), dynamic rendering, sync2, independent_blend are `Need::Required`; the swapchain only with a present
   window; only `memory_budget` is optional. A refused device gets `enabledFeatures` and `deviceExtensions` cleared (`:188-191`). No per-facility fallback arm exists anywhere (grepped cpp/h/docs; none of the five has a consumer yet);
   the rule "the batch that adopts a facility deletes the older arm" is written in `docs/vk-device-profile.md:11-12` and the header; the only stale statement is M1. `fail()` -> GL with the profile line, the `FAILED` line and
   `[RENDER_BACKEND] fail-soft`, no crash path (two lines plus the render-backend line, not one). Doctest: `test_gate.cpp:1613-1636` drops each of the six rows from a full device and checks `missingRequired`, the line, and
   `enabled=- extensions=-`; swapchain, limits, api, descriptor-indexing bits have their own cases. Parser/grade: `test_vk_profile.py:51-58` for the six rows. **M2 live proofs are real**: `2026-10-13T12-49-20`
   (floor 1.4: `verdict=refused ... required_missing=api_1_2`, GL for 1952 frames, bucket `vk_profile_refused`) and `12-51-45` (limits refused on the 7900 XTX, GL for 4045 frames); both read here.
2. **M1 / L5-L7 / L10 / L11.** The context keeps the `Evaluation` (`vulkan_context.h:95,104-105,128`) and resets it to not-accepted in `destroy()`. Enabled bits: the eight of the ruling (required, NOT consumed by any island:
   BDA only reaches VMA's `BUFFER_DEVICE_ADDRESS_BIT`, `:410`) and `independentBlend` (consumed today by the terrain / water / VFX / legacy-shape pipelines); the extension `VK_EXT_memory_budget` is consumed by the VRAM report.
   One device-extension enumeration, `extensions=` is the full list, `[VK_PROFILE v1]` is in `log_tag_baseline.json:11625`, no stale "1.1" comments (only the WHY at `vk_device_profile.h:9`). L10: `ninja -t deps` of the launcher
   object returned the three repo headers in 0.3 s here, falls back to the four-file mtime check when ninja, the module or the record is missing, and skips when the exe is absent: robust on a fresh tree (a branch switch that
   touches a header turns it red until the launcher is rebuilt, by design; the fixture runs ninja once per test, module scope would save ~3 s). L11: `GL_DEVICE_LUID_EXT = 0x9599` and `glGetUnsignedBytevEXT` are right, the VK
   LUID comes from `VkPhysicalDeviceIDProperties` (`vk_device_profile.cpp:23-32`, 1.1+), `memcmp` against `LUID` bytes is the correct layout; M2 above is the only defect.
3. **Validation grading: correct.** Layer on only when `MC2_VULKAN_VALIDATION` is set (`vulkan_context.cpp:100`); the launcher never asks (no hit in `tools/mc2_launcher`), no smoke default sets it. `[VK_VALIDATION v1] severity= id= text=`
   (`vk_validation_line.h`, one snprintf, id whitespace-free, text quote-free and capped at 900); errors = marker bucket `vk_validation_error`, warnings counted and listed; `layer_unavailable` / `messenger_unavailable` are errors.
   The callback still counts `validationErrors` and runs while `vkDestroyDevice` does, which is how 05137 surfaces. Fix 1: `independent_blend` row + feature bit (minimal, `vk_device_profile.h:177-180`, `vulkan_context.cpp:366`).
   Fix 2: the composite object-id descriptor uses a NEAREST sampler (`engine_present_vk.cpp:663-674,741`, destroyed at `:2668`); correct, minimal. Note it still binds the float scene view in that slot when no object-id image
   exists (`:738`): a usampler over a float view, which no layer can flag.
4. **Replay tool: sound.** Layer variables match the SDK script (`VK_INSTANCE_LAYERS`, `VK_LAYER_PATH`, `GFXRECON_CAPTURE_FILE/FRAMES`, timestamp off); replay is `--swapchain offscreen --screenshots a-b --screenshot-format png`; the
   `--wsi headless` failure is in the docstring (`:101`) and `docs/host-tools.md`; the capture goes through `capture.py` (nofocus, host lease), the replay holds the lease; 14 pytest cases cover the argument building, the
   hash (pixels, size, channel order), the match and the dry run. Findings L5, L6.
5. **Capture ledger: correct.** One entry per set (`ledger.json`, 147 sets, 23 VOID, `--check` OK on this tree and defined for a clone); VOID is carried through a refresh; `--note` is kept through a refresh and rejects an
   unknown set; the Buildings.csv self-compare is recorded in two notes. L7, L8.
6. **Proof completeness.** 8cbb7e306 is not a net loosening: it restored the three midday sites that the take-nifty's-side conflict resolution had erased; two of them were then removed (3cbadd0bb LG06 -1, 568352d34 LG11 -1), so the
   session ends with LG20 +1 (launcher JSON key names, a scanner false positive accepted at midday) over nifty's counts; the reasons are in the commit message and the baseline is tight. Regenerated files are named in the
   commit trailers (`log_tag_baseline.json`, `callers.json`, `gated-layout-macros.md`, `background-launch-inventory.md`, `default_build_quiet_baseline.json`). No deploy from a failed build: the two `C1041` builds died before the exe link;
   the scratch floor builds ran with `MC2_SMOKE_REQUIRE_FINGERPRINT=0` and are named; the final deploy is exe `1678068bbe9a`, fingerprint OK. Both tier1 flakes are in `tier1-flake-log.md:20-21`, each with a clean rerun.

## Next lane 2 session: do first
1. `git merge claude/nifty-mendeleev` (tip `cb43bc927`, 37 commits ahead of the merge base `f6ef2a4be`); `git merge-tree` shows ONE conflict, `tools/cxx17_ratchet/baseline.json` (take nifty's, `--check --tighten`, re-add only real sites).
   Regenerate `callers.py`, `log_tag_census --write`, `check-background-launch.py --write`; rebuild `mc2_launcher` before pytest; `[VIDEO v1]` arrives in the quiet baseline with the merge.
2. The two validation ids (sites above) and object names (L10); then a VK tier1 with `MC2_VULKAN_VALIDATION=1` must report `errors=0` with `layer_enabled`; then make the layer run a named tier.
3. M1 (the review document), M2, L2, L3 (`static_assert`), L5, L6, L7 as one small commit set.
4. Shader-perturbation "first differing draw" for the replay oracle (rank 2 remainder) and the night pose.

## Files likely to conflict with lane 1 at merge (changed on both sides since `f6ef2a4be`)
`tools/cxx17_ratchet/baseline.json` (real conflict, regenerable); auto-merged cleanly today but regenerate after the merge: `tests/callers.json`, `tests/callers_allowlist.json`, `scripts/background-launch-allow.json`,
`tests/contract/default_build_quiet_baseline.json`, `tests/unit/test_gate.cpp` (different hunks). Lane 2 only (nifty has not touched them): `tests/unit/log_tag_baseline.json`, `docs/background-launch-inventory.md`,
`docs/render-backend-seams/gated-layout-macros.md`, `GameOS/gameos/{vk_device_profile.*,vulkan_context.*,engine_present_vk.cpp,gpu_memory_report.cpp}`.
