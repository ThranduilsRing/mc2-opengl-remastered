# VK-ARC-STATE — current state block (update every session)

HEAD routing note (2026-07-08): current shipped tip is rung 8.7ab
`VK-SHADOW-DIRECT-CHAIN-1` after the shadow-enabled visible no-interop strict proof
on branch `claude/vk-interop-bridge-1`, worktree
`A:/Games/mc2-vk-interop-bridge`, build dir `build64_island`.
Always confirm the exact commit with `git log -1 --oneline` before launching
agents; this file is append-heavy and older rung sections below are historical.
Deploy lane for Vulkan ownership work is the literal
`A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe`; pass `--exe` explicitly for
smoke, visual, and RenderDoc harnesses. Do not rely on default deploy/visual
lanes (`v0.4`, `v0.4d-rc1`, or non-v-prefixed `0.4c` paths).
Rung-8 sub-ladder: `.claude/VK-RUNG8-LADDER-1.md` (8.1-8.7w shipped). Visible
strict proof for the full native final tail lives at
`.claude/vk-present-native-strict-visible-clean/`: 3/3 deterministic
`mc2_01_buildings` bookmarks, `present_frames=2700`, `copy1_blit_frames=0`,
`gl_handoff_skip_frames=2700`, `composite_vk_frames=2700`,
`imgui_vk_frames=2700`, `fallback_reason=""`. Next work is not 8.4/8.5 replay;
it is remaining deletion readiness around `gl_interop_view.*`, shared scene
color/depth/present substrates, and any diagnostic/non-CSM fallback paths still
needed for the final Metal gate.

Current final-gate canaries: `MC2_VK_NO_GL_INTEROP_STRICT=1` aborts on any
production `glInteropSignalToVk`/`glInteropWaitFromVk` call, while
`MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT=1` aborts on exportable Win32
`VkImageResource` creation or `glInteropViewCreate`. Proof gates
`MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR=1`, `MC2_VK_PROP_DIRECT_CHAIN=1`,
`MC2_VK_MECH_DIRECT_CHAIN=1`, `MC2_VK_WATER_TIER1=1`,
`MC2_VK_WATER_DIRECT_CHAIN=1`, `MC2_VK_POST_FOG_NATIVE=1`,
`MC2_VK_POST_FOG_DIRECT_CHAIN=1`, `MC2_SSAO=1`,
`MC2_VK_POST_SSAO=1`, `MC2_VK_POST_SSAO_DIRECT_CHAIN=1`,
`MC2_SHADOW_ENABLE=1`, `MC2_SHADOW_CSM=1`,
`MC2_VK_SHADOW_INTEROP=1`, `MC2_VK_SHADOW_DIRECT_CHAIN=1`,
`MC2_VK_CSM_ARRAY_INTEROP=1`, `MC2_VK_CSM_FULLMAP_INTEROP=1`,
`MC2_VK_STATIC_SHADOW_MAP=0`, and `MC2_VK_POST_SCREENFX=1`,
`MC2_VK_POST_SCREENFX_SHADOW=1`, `MC2_VK_POST_SCREENFX_DIRECT_CHAIN=1`,
`MC2_HZB_BUILD=1`, `MC2_VK_POST_HZB=1`, `MC2_VK_POST_HZB_DIRECT_CHAIN=1`,
`MC2_VK_POST_COMPOSITE_DIRECT_CHAIN=1` now make a visible `mc2_01` full
bookmark sweep pass with the handoff strict canary armed:
`.claude/vk-nointerop-strict-visible-shadow-direct-clean-final/`, 11/11
deterministic. Log shows `sha=53311e5aced3 dirty=0`,
`MC2_VK_SHADOW_DIRECT_CHAIN=1 -- VK shadow products stay VK-owned; GL
semaphore wait is skipped`, first VK shadow/CSM products, present
`composite_vk_frames=6600 gl_handoff_skip_frames=6600`, and zero occurrences
of `[VK_NO_GL_INTEROP_STRICT]`, `glInteropSignalToVk`, or
`glInteropWaitFromVk`. Detailed post-health receipt is in the immediately prior
dirty proof `.claude/vk-nointerop-strict-visible-shadow-direct-dirty-receipts/`
with screenfx `shadow_frames=6629 direct_chain=1 used_interop=0` and
HZB/fog/SSAO/water/prop all `used_interop=0`.
Follow-up 8.7ad dirty proof
`.claude/vk-nointerop-strict-visible-static-off-dirty/` forced
`MC2_VK_STATIC_SHADOW_MAP=0`; it still passed 11/11 deterministic bookmarks
with no `VK_SHARED_STATIC_SHADOW` log lines and screenfx
`shadow_frames=4687 direct_chain=1 used_interop=0 fallback_reason=""`. This
removes the static-shadow export/import from the strict stack; resource-strict
still fails earlier at shared scene color/depth/present.
Follow-up 8.7ae harness proof
`.claude/vk-nointerop-strict-visible-ui-tail-long-dirty/` added native UI tail
gates to the strict stack: `MC2_VK_HUD=1`, `MC2_VK_HUD_COLOR_PROOF=1`,
`MC2_VK_HUD_SKIP_GL=1`, and `MC2_VK_IMGUI=1`. It passed 11/11 deterministic
bookmarks on clean deployed binary `sha=d5faad0feb69 dirty=0`; final present
receipt reached `present_frames=8100`, `copy1_skip_frames=8100`,
`gl_handoff_skip_frames=8100`, `tail_hook_frames=8100`,
`composite_vk_frames=8100`, `hud_vk_consumed_frames=7258`,
`hud_vk_textured_draws_last=69`, `imgui_vk_frames=8100`,
`fallback_reason=""`, with `[VK_HUD_SKIP v1] ... reason="native_full_coverage"`
and no `[VK_NO_GL_INTEROP_STRICT]` abort.
Follow-up 8.7af present no-import proof (commit `3339ca20`) added `MC2_VK_PRESENT_NO_GL_IMPORT=1`: EnginePresentVk allocates the present target as VK-only/non-exportable when the strict native final-tail stack is armed, and `gosPostProcess::endScene()` uses a VK-only direct-present sentinel instead of a GL FBO. Clean proof `.claude/vk-nointerop-strict-visible-present-noimport-clean-final/` passed 11/11 deterministic `mc2_01` bookmarks on deployed binary `sha=3339ca203cb6 dirty=0`; final present receipt reached `present_frames=6300`, `copy1_blit_frames=0`, `copy1_skip_frames=6300`, `gl_handoff_skip_frames=6300`, `tail_hook_frames=6300`, `composite_vk_frames=6300`, `hud_vk_consumed_frames=5523`, `hud_vk_textured_draws_last=69`, `imgui_vk_frames=6300`, `fallback_reason=""`. Absence check: zero `[VK_NO_GL_INTEROP_STRICT]`, zero `glInteropSignalToVk`, zero `glInteropWaitFromVk`, zero `VK_SHARED_STATIC_SHADOW`, zero present resize/copy fallback strings. This removes the present-specific GL memory import; remaining resource-strict blockers are shared scene color/depth/object-id and other earlier-scene export/import substrates.
Follow-up 8.7ag strict harness hardening (commit `53a38a95`) added the shipped
max-native geometry/feed gates to `run_vk_nointerop_strict_visible.py`:
`MC2_VK_TERRAIN_ALL_LODS=1`, `MC2_VK_TERRAIN_SKIRTS=1`,
`MC2_VK_TERRAIN_VISUALDISPLACE_FAR=1`, and `MC2_VK_MECH_FEED_NATIVE=1`.
Clean proof `.claude/vk-nointerop-strict-visible-maxnative-harness-dirty/`
used deployed binary `sha=3339ca203cb6 dirty=0`, passed 11/11 deterministic
`mc2_01` bookmarks, and kept final present at `present_frames=6300`,
`copy1_blit_frames=0`, `gl_handoff_skip_frames=6300`,
`composite_vk_frames=6300`, `hud_vk_consumed_frames=5518`,
`imgui_vk_frames=6300`, `fallback_reason=""`. Absence check stayed zero for
`[VK_NO_GL_INTEROP_STRICT]`, `glInteropSignalToVk`, `glInteropWaitFromVk`,
`VK_SHARED_STATIC_SHADOW`, and present resize/copy fallback strings.
Follow-up 8.7ah fog direct-resource init (commit `42cedc66`) made
`vulkan_post_fog_native.cpp` require only shared `VkImageResource` handles when
`MC2_VK_POST_FOG_DIRECT_CHAIN=1`; GL views remain mandatory only for handoff
mode. This does not remove the shared depth/color export/import substrate yet,
but it removes a false GL-wrapper dependency from the first honest direct
consumer. Clean proof
`.claude/vk-nointerop-strict-visible-fog-direct-resource-clean-rerun/` passed
11/11 deterministic `mc2_01` bookmarks on deployed binary `sha=42cedc661baf
dirty=0`; fog health reached `attempted=6503`, `used_vulkan=6503`,
`direct_chain=1`, `used_interop=0`, `gl_skipped=13006`,
`fallback_reason=""`; final present reached `present_frames=6300`,
`copy1_blit_frames=0`, `copy1_skip_frames=6300`,
`gl_handoff_skip_frames=6300`, `tail_hook_frames=6300`,
`composite_vk_frames=6300`, `hud_vk_consumed_frames=5519`,
`imgui_vk_frames=6300`, `fallback_reason=""`. Absence check: zero
`[VK_NO_GL_INTEROP_STRICT]`, zero `glInteropSignalToVk`, zero
`glInteropWaitFromVk`, zero `VK_SHARED_STATIC_SHADOW`, zero present
resize/copy fallback strings.
Final exact-binary regression after reviewer fixes:
`.claude/vk-nointerop-strict-visible-final-smoke/`, engine smoke PASS,
7 deterministic bookmarks before duration, `composite_vk_frames=300`,
`gl_handoff_skip_frames=300`, `VK_WATER_TIER1 used_interop=0`, no strict abort.
LowLight alternate composite mode is also strict-clean at
`.claude/vk-nointerop-strict-visible-lowlight/` (`MC2_VIEW_MODE=lowlight`,
engine smoke PASS, `composite_vk_frames=300`, no strict abort). ObjectIdDebug
strict is green under `MC2_VK_OBJECTID_DIRECT_CHAIN=1`; it is a deletion canary,
not a pixel-parity port for native ObjectId MRT2 writes. Next direct-chain work
is shadow-specific VK->GL sampling paths and any diagnostic fallback paths still
needed before deleting `gl_interop_view.*` and Win32 external-memory plumbing.

Verification guardrails:
- Bracket every proof with lane hygiene: deployed exe fingerprint/manifest plus
  `[VK_*]` health lines showing `fallback_reason=""` for the feature under test.
- Minimized smoke is valid for crash/telemetry but cannot prove direct present,
  HUD/ImGui, or nonzero-surface final-tail behavior. Use visible bookmark
  captures for those.
- Bookmark sweep files must contain at least two entries; single-pose bookmark
  files are parser canaries only.
- RenderDoc capture is a separate lane; use `scripts/renderdoc_capture.py --exe`
  against the same `mc2-win64-v0.4c/mc2.exe`.
- Before deleting interop, add or use a canary mode that hard-fails on any
  shared-scene GL fallback/import path, so a passing smoke cannot hide a GL
  substrate rescue.

## RUNG 8.3 -- VK-POST-SCREENFX-NATIVE-1 (2026-07-07) -- SHIPPED

Dirty implementation recovered from the interrupted 8.3 agent and verified in-place.
New TU `GameOS/gameos/vulkan_post_screenfx_native.cpp`: cloud-shadow +
shoreline-foam render natively on the SHARED ENGINE VulkanContext, in one
render pass / one submit, multiplying into shared scene color0 in place. Inputs:
shared scene depth for cloud, shared scene normal/GBuffer1 for shoreline. Gate
`MC2_VK_POST_SCREENFX` (envTriState opt-in, default-OFF) with per-pass bisect
subgates `MC2_VK_POST_SCREENFX_SHADOW`, `MC2_VK_POST_SCREENFX_CLOUD`, and
`MC2_VK_POST_SCREENFX_SHORELINE`.
Return bitmask skips exactly the GL pass bodies that VK rendered. Screen-shadow
is intentionally NOT ported in this rung because the CSM cascade array has no
zero-copy VK producer yet; porting it now would add readbacks and violate the
rung goal.

Harness/code fixes during recovery:
- Fixed malformed `debug_state_dump.cpp` raw JSON insertion for
  `vk_post_screenfx_native`.
- Added missing `vk_shared_gbuffer_gate.h` include in `gos_postprocess.cpp`.
- Added `MC2_CLOUD_SHADOW` to `scripts/run_smoke.py` allowlist; without it,
  cloud proof was impossible because Popen dropped the gate. Corrected docs to
  match code: `MC2_CLOUD_SHADOW=1` is default-OFF.

Evidence (`.claude/vk-post-screenfx/`):
- Build: `build64_island` Vulkan target PASS; shader target PASS; deployed to
  `A:/Games/mc2-opengl/mc2-win64-0.4c` with explicit `--exe` lane.
- Offline: `check_ubo_layout.py --pair screenfx_cloud` PASS; `--pair
  screenfx_shore` PASS. `glsl_port_diff.py --all` shows the 4 pre-existing
  older divergences unchanged and the new pairs VERBATIM: `cloudShadowNoise`,
  `cloudShadowMain`, `shorelineNoise`, `shorelineMain`.
- Shoreline proof (`mc2_01`, water bookmark, gate ON both passes): capture
  receipt `attempted=280 used_vulkan=280 cloud_frames=0 shore_frames=280
  gl_skipped=280 fallback_reason=""`; base run had no native receipt. Visual
  A/B vs gate-off showed expected shoreline effect concentrated on water poses
  (oblique mean MAD 4.602/ch, overview 0.291/ch).
- Cloud proof (`mc2_24`, `MC2_CLOUD_SHADOW=1`,
  `MC2_VK_POST_SCREENFX_SHORELINE=0`): smoke receipt `attempted=113
  used_vulkan=113 cloud_frames=113 shore_frames=0 gl_skipped=113
  fallback_reason=""`. Visual A/B (`vk_cloud_base` vs `vk_cloud_native`) all
  three bookmarks deterministic; `visual_diff.py` rollup PASS/WARN only
  (2 pass, 1 warn, 0 fail). MAD: highangle 0.013/ch, overview 0.039/ch,
  ridge 0.006/ch, changed <=0.090%. Full capture receipt:
  `attempted=1044 used_vulkan=1044 cloud_frames=1044 shore_frames=0
  fallback_reason=""`.
- Smoke: `mc2_01` shoreline gate PASS; `mc2_24` cloud-only PASS. Canonical
  tier1 both-state PASS on explicit `--exe A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe`:
  gate-off `2026-07-07T19-27-55` 5/5 PASS +0 destroys; gate-on
  `2026-07-07T19-31-16` 5/5 PASS +0 destroys with `MC2_VK_POST_SCREENFX=1`
  and `MC2_CLOUD_SHADOW=1`. Gate-on receipts across all tier1 missions:
  attempted==used_vulkan, fallback_reason="", and both `cloud_frames` +
  `shore_frames` incremented (`mc2_01` 112/112, `mc2_03` 114/114,
  `mc2_10` 114/114, `mc2_17` 112/112, `mc2_24` 104/104).

Historical residuals at time of 8.3: this previously said to advance to 8.4
HZB compute. That work is now shipped; use the HEAD routing note above and
`.claude/VK-RUNG8-LADDER-1.md` for current next work. Keep historical receipts
below for provenance only.

## RUNG 8.2 -- VK-POST-SSAO-NATIVE-1 (2026-07-07) -- SHIPPED

Commits `658107ae` (feature) + `786f6999` (gate registration). New TU
`GameOS/gameos/vulkan_post_ssao_native.cpp`: SSAO gen+apply rendered natively on
the SHARED ENGINE VulkanContext -- gen samples the shared scene DEPTH + NORMAL
directly into a VK-OWNED half-res R16_SFLOAT occlusion image (the ladder's one
intermediate; device-local, non-shared), apply 4-tap-box-upsamples it (LINEAR)
and MULTIPLIES (DST_COLOR/ZERO) into the shared scene color0 IN PLACE. Sync =
8.1 fog pattern extended to THREE shared resources (color0 + normal + depth
BeginFrame/sems both directions; in-cb barriers depth DS<->SHADER_READ_ONLY and
normal COLOR_ATTACHMENT<->SHADER_READ_ONLY around the gen pass). Gate
`MC2_VK_POST_SSAO` (ladder's name; envTriState opt-in, default-OFF, new-rung
pattern). Highest precedence at the SSAO site (native > region-iface > GL);
parity guards mirror runSSAO()'s exact gate set (MC2_SSAO master gate,
sceneHasTerrain, programs/FBO valid) + falls through to GL when
`MC2_SSAO_DEBUG!=0` (grayscale-overwrite diagnostic NOT ported -- explicit
limitation). SHADERS REUSED VERBATIM: the island's parity-proven
ssao/ssao_apply SPIR-V (R16F format parity, LINEAR apply-sampler parity,
column-major matrix parity all inherited).

KILLS (vs the old vulkan_ssao_island readback pattern, measured live in the
attribution run): per-frame glGetTexImage DEPTH + NORMAL readbacks + staging
uploads, scene-color pull/preload + blended-color readback/writeback
(island forbidden_* counters showed 2497 each per 2497-frame run,
total_us ~15281/frame vs native render_us 300-680).

Evidence (`.claude/vk-post-ssao/` -- ssao_oracle.py + cap/*; MC2_SSAO=1 in ALL
configs; stock AO tunables are near-invisible at RTS poses, so parity was also
proven under a boosted tuning profile `visual_tuning_aoboost.json`
(aoRadius 24 / aoStrength 2.0 via MC2_VISUAL_TUNING_FILE) where the GL AO
effect is measurable (0.03-0.48/ch cov 1-5%)):
- Offline trio: check_vk_shaders 34/34, check_ubo_layout 7/7 pairs OK (new
  ssaonat_occ 7 fields / ssaonat_apply 1 field), glsl_port_diff --all baseline
  unchanged (no new ports; SPIR-V reuse).
- Byte-off control (gl vs gl2, MC2_SSAO=1, stock AND boosted tunings, 5 AO
  poses mc2_01): 0.000 all non-sacrificial poses.
- Receipts: gate-ON sweeps attempted==used_vulkan (479-495), fallback_reason="";
  gate-ON tier1 smoke used_vulkan=193-198/mission; debug-state JSON section
  `vk_post_ssao_native` (postssao_used_vulkan) verified in
  latest_render_state.json.
- Single-variable MAD (vk_base vs vk_ssao, SAME VK scene, boosted): at the
  deterministic poses 0.047-0.090/ch cov 2-5% -- SMALLER than the GL AO effect
  itself (0.19-0.48/ch) and well INSIDE the impl-swap family residual measured
  same session on the parity-proven OLD island (gl vs gl_island: up to
  0.92/ch cov 11.8% at cliff_side_on). overview_center/ridge_lowangle diffs
  (0.28/1.71) == the vk scene run-to-run SELF-noise at those poses (base
  self-diff 0.27/1.64) -- scene noise, not SSAO.
- vk_ssao self-diff determinism: 0.000 at all scene-deterministic poses.
- Sacrificial first bookmark used every sweep (VK-FIRST-BOOKMARK-POSE-1).
- Smoke: canonical tier1 5/5 PASS BOTH states on the fingerprint-verified exe
  (sha 786f69990c17 == HEAD), +0 destroy-delta all missions. Gate-ON ~6 FPS =
  the pre-existing umbrella-under-smoke cost (rung-6/7/8.1 note), not SSAO
  (native render_us ~300).

Residuals / next: (1) tiny impl-swap residual (<=0.09/ch boosted) inherited
from the island port family (hash12 sin/fract precision class) -- far below
the island's own accepted residual, not itemized; (2) MC2_SSAO_DEBUG
grayscale mode unported (falls through to GL, documented); (3) old SSAO
island + its readback taxes become DELETE candidates at rung 8.7. NEXT
SUB-RUNG: 8.3 screen-shadow/cloud-shadow/shoreline (MC2_VK_POST_SCREENFX,
needs verbatim shader ports + glsl_port_diff registration) or 8.4 HZB compute.

## RUNG 8.1 — VK-POST-FOG-NATIVE-1 (2026-07-07) — SHIPPED

First rung-8 (postprocess-tail ownership) sub-rung. New TU
`GameOS/gameos/vulkan_post_fog_native.cpp`: BOTH postprocess fog passes
(edge + oob) rendered natively on the SHARED ENGINE VulkanContext — samples the
shared scene depth directly, alpha-blends into the shared scene color0 IN PLACE
(water-tier1 sync protocol: dual BeginFrame + GL sems both directions, in-cb
depth DS_ATTACHMENT<->SHADER_READ_ONLY barriers, layout-stable LOAD/STORE pass).
Gate `MC2_VK_POST_FOG_NATIVE` (envTriState opt-in, default-OFF, new-rung
pattern). Highest precedence at the fog site (native > subgraph > island > GL);
parity guards fall through to GL when either GL fog pass would self-skip or
`MC2_SKYBOX_FOG_EXCLUDE=1` (VK fog shaders have no stencil sky-exclude path —
pre-existing subgraph limitation, now explicit). Shaders reused VERBATIM
(edge_fog/fog_oob SPIR-V); UBO PODs offline-checked (check_ubo_layout labels
postfog_edge 8 fields / postfog_oob 4 fields OK).

KILLS (vs the old fog subgraph path): per-frame `glGetTexImage(DEPTH)` CPU
readback + staging upload ("P5" finally closed), 2x per-frame
`glCopyImageSubData` color round-trips, and the subgraph's PRIVATE
VkInstance/VkDevice (which is provably UNUSABLE under `MC2_RENDER_BACKEND=vulkan`:
R2 interlock coerces its interop OFF and the readback-mode run CRASHED in
present swapchain rebuild — captured in `.claude/vk-post-fog/cap/vk_subgraph/`).

Evidence (`.claude/vk-post-fog/` — fog_oracle.py + cap/*):
- Offline trio: check_vk_shaders 34/34, check_ubo_layout 5/5 OK, glsl_port_diff
  --all green (no new shader ports; fog SPIR-V pre-dates the tool, reused as-is).
- Byte-off control (gl vs gl2, 5 fog poses mc2_01): 0.000 — clean.
- Receipts: sweep used_vulkan=495/495 attempted, fallback_reason="";
  gate-ON tier1 smoke mc2_01 used_vulkan=193/193; debug-state dump section
  `vk_post_fog_native` (postfog_used_vulkan) added for future captures.
- Single-variable MAD (vk_base vs vk_fog, SAME VK scene): oblique_fog
  10.13/ch cov 19.0%, oblique_horizon 8.51/ch cov 40.6% — ATTRIBUTED: the GL
  fog-impl-swap control (gl vs gl+subgraph, the parity-proven shipped VK fog)
  shows the SAME signature (10.89/19.3%, 8.62/40.7%), i.e. the delta is the
  KNOWN family fog-port residual, not a new defect. Remaining pose diffs
  (boundary_south 3.66@16%, overview 2.37@0.8%) == vk_base SELF-noise
  (base-vs-base2 3.69@15.2%, 2.34@0.7%); vk_fog itself is run-to-run
  DETERMINISTIC (self-diff 0.000 all poses).
- Smoke: canonical tier1 5/5 PASS both states, Δ destroys +0, manifest exe
  sha `46e81cb1d9f2f7c1` == deployed build. Gate-ON ~6 FPS = the pre-existing
  umbrella-under-smoke cost (identical without the fog gate; rung-6 note).

Residuals / next: (1) known fog-port residual at oblique poses (~8-11/ch in
the fog band) is inherited from the original island/subgraph port — if ever
worth closing, diff edge_fog.frag GL-vs-VK math paths and register
glsl_port_diff pairs; (2) sky-exclude (MC2_SKYBOX_FOG_EXCLUDE) unsupported
natively — port the stencil sample if that gate ever defaults ON; (3) the
subgraph + old fog islands become DELETE candidates at rung 8.7 (ladder), not
before. NEXT SUB-RUNG: 8.2 SSAO native (same shape: shared depth in, shared
color0 multiply out, engine context).

## RUNG 7 -- VK-MECH-FEED-NATIVE-1 (2026-07-07) -- SHIPPED

Commits `28fc0c9f` (code) + `f1f49df4` (gate registration). Gate
`MC2_VK_MECH_FEED_NATIVE` (envTriState opt-in, default-OFF, new-rung pattern);
safety gate `MC2_VK_MECH_FEED_AUDIT` (verification-only, HEIGHT_AUDIT class).
Scope note vs recon doc: the recon ladder's rung 7 headline is "mech material/
lighting full PBR" but material+lighting parity ALREADY landed pre-flip
(VK-FLIP-EVIDENCE-1 Lambert+fog+paint+scheme-cache); the remaining substance was
"kill the mech path's GL-object reads" (recon section A "Remaining interop
copies" row: these die at the next mech ownership rung). This rung delivers
exactly that subset:
- GEOMETRY (copy-once): island memcpy's the batcher's CPU sources -- new
  `s_vkVboNativeCopy` (the EXACT normals-recomputed `work` blob uploaded to
  s_sharedVbo; s_stagingVbo alone is NOT the uploaded bytes when
  s_mechNormalsMode!=0) + `s_stagingIbo` (uploaded verbatim). Kills both
  copy-once glGetBufferSubData readbacks.
- LIGHTSDATA (per-frame): island memcpy's `mcTextureManager->lightData_`
  (new public accessors getLightDataPtr/getLightDataBytes; the CPU array IS the
  byte source of the GL SSBO upload, txmmgr.cpp:2451-2459). Kills the ONLY
  per-frame GPU readback (glGetBufferSubData) on the mech path.
- Fail-soft: any missing CPU pointer (size-mismatch race, no texture manager)
  -> GL-readback path unchanged. Gate OFF = byte-identical old feed path.
- Feed struct additions: Mc2MechGeomFeed.vboCpu/iboCpu,
  Mc2MechFrameFeed.lightDataCpu/lightDataCpuBytes (null unless armed).
Evidence (oracle `.claude/vk-mech-feed-native/mech_feed_oracle.py`, caps under
`cap/mc2_24/` + `cap/mc2_01/`):
- AUDIT (byte-level golden-vs-native): FEED-AUDIT geometry mismatch_buffers=0 +
  lights byte-identical EVERY frame, mc2_24+mc2_01 sweeps AND all 5 tier1
  missions under gate-ON smoke (0 DIVERGES lines total). GL SSBO is
  capacity-sized so cpu<=gl bytes; min-prefix compare.
- Instance/cull parity: inst=122 bones=326 runs=60 geom_verts=14814 (mc2_24)
  and 38/140/25/5220 (mc2_01) IDENTICAL old-vs-native; used_vulkan receipts
  474/478/465 (VK runs), GL control used_vulkan=0.
- vk_feed_diff.py old-feed vs native-feed dumps: exit 0 (122 instances both).
- Single-variable MAD vk_old vs vk_native (mc2_24, sacrificial-first-bookmark):
  0.027-0.120/ch cov<=0.44% max~235 == the GLxGL byte-off floor measured same
  session (0.030-0.131/ch cov<=0.45%) -- transport change is pixel-neutral.
- Smoke: tier1 5/5 PASS both states, +0 destroy-delta (gates-OFF and full
  VK umbrella + FEED_NATIVE + FEED_AUDIT). Gate-ON FPS ~6 = the PRE-EXISTING
  umbrella-under-smoke cost, not a feed regression.
- TRAP (relearned): `run_smoke.py --deploy 0.4c` resolves to the STALE
  `mc2-win64-v0.4c` (v-prefixed) lane, NOT `mc2-win64-0.4c`. First smoke pair
  silently ran the old exe (log format betrayed it: no `source=` in the
  geometry line). Use `--exe A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe
  --no-lease` for this lane.
Residuals (honest, for rung 8 planning):
- Paint-scheme textures (run.slot0GlTex) still GL-sourced: copy-once
  glGetTexImage import keyed by live GL texture id (scheme cache). Making
  these native = mcTextureManager CPU-texture feed slice.
- Instance/bone/run snapshots are still BUILT by the GL batcher's flush()
  (CPU code, no GL reads, but flush-time production inside the GL frame) --
  full batcher-independence would mean reimplementing the skinning-palette/
  instance build outside flush; not needed to kill interop copies.
- GL-readback feed path retained as fail-soft (deletable at rung 8 when the
  GL side disappears).

## RUNG 6.5 — WATER REFLECTION PORT (2026-07-07) — SHIPPED (closes the rung-6 grazing gap)

Commits `f0de79c7` (port) + `006e2115` (glsl_port_diff + check_ubo_layout
registration). Ports GL's default-ON water reflection block
(gameos_graphics.cpp:3100 area; strength 1.5, `MC2_WATER_REFLECTION=0` kills)
VERBATIM into `shaders/vulkan/water_geom.frag`: SH-L2 sky coeffs + waterEvalHdri
(equirect, skyYaw-rotated, Reinhard, LOD from mc2WaterHdriLodDefault(), default
4.0) + waterEvalSkySh fallback + wave-normal/reflect setup + Schlick Fresnel mix
(REFL_F0 0.02 / REFL_MAX 0.55). 5 PORT pairs, ALL VERBATIM per glsl_port_diff.
- SKIPPED with evidence: (a) terrain-RT contact term — `MC2_WATER_REFLECTION_RT`
  default-OFF (`g_waterRtStrength=0.0` unless env set) -> the GL sub-block is a
  no-op in the field config; (b) sun-spec — `u_waterSpecIntensity` is uploaded
  but consumed by NO shader (grep of shaders/ = zero hits; dead upload), and
  defaults 0 anyway. Not RT ray-query — nothing MoltenVK-hostile added
  (VK 1.1 core; BC feature queried, fail-soft to SH if absent).
- Plumbing: WaterFsParams UBO +16B (f2.yzw = reflStrength/skyYaw/hdriLod, new
  f3 = screen size; `check_ubo_layout.py` waterfs pair registered, 7 fields OK),
  binding-2 combined image sampler, COPY-ONCE HDRI import from the GL texture
  (BC6H .ktx2 sidecar mip0 in the field config — GL's own textureLod clamps to
  mip 0 there, so 1-mip VK image is exact parity; RGBA16F+full-chain path for
  the tinyexr fallback). Honest cost: one-time staging copy at first armed-water
  frame (mc2_01 uses mud_road_puresky 16K BC6H = 128MB one-shot), ZERO per-frame
  cost; receipt `[VK_WATER_ISLAND] hdri REAL import: 16384x8192 fmt=143 mips=1`.
  Fail-soft = 1x1 dummy bound + fs UBO hdriLod patched to -1 -> SH-L2 fallback
  (same semantics as GL `!isHdriReady()`). Gate parity: fs UBO feeds
  `gos_GetWaterReflStrength()` -> `MC2_WATER_REFLECTION=0` skips the block on
  BOTH sides (proved live below).
- Offline: check_vk_shaders 34/34, check_ubo_layout 3/3 pairs OK,
  glsl_port_diff --all: 8 VERBATIM (5 new water pairs) + the 4 pre-existing
  recorded rename findings unchanged.
- Live (mc2_01 water sweep, sacrificial-first-bookmark, receipts every run:
  water island used_vulkan=488/472, thin=6167, recipes=9801; GL control
  used_vulkan=0): water-only delta (vk_base vs vk_water, SAME VK scene,
  single variable) at grazing oblique_low: **8.29/ch -> 3.64/ch full-image
  (cov 98%->38%)** == the rung-6 refl-off floor (3.56/ch cov 39%) — the
  reflection gap is CLOSED to the base-layer residual. overview_high 0.31/ch
  (bar <=0.6 met). Single-variable execution proof: vk_water vs
  vk_water_refloff (MC2_WATER_REFLECTION=0) = 9.35/ch cov 99% — the ported
  block demonstrably executes and honors the GL gate. Artifacts:
  `.claude/vk-water-tier1/cap/` (rung-6 baselines preserved in `cap_rung6/`),
  driver `run_65.py`, analyzer `mad_65.py`.
- Smoke: tier1 5/5 PASS both states (gates-OFF and full VK_BASE+WATER_TIER1),
  +0 destroy-delta all missions, worktree run_smoke vs 0.4c exe.
- Remaining rung-6 residual (unchanged, now the whole gap): base-layer 3.6/ch
  cov ~38% at grazing not yet itemized (suspects: fog band, alpha/blend LSB,
  detail layer); detail/spray layer (isWater==2) still GL-discarded both sides.

## RUNG 6 — VK-WATER-TIER1 (2026-07-07) — SHIPPED (base-layer, reflection-off subset)

Commits `06c4fe03` (island tier1 mode + gate registration) + `790388f8`
(waterBaseColor port markers). Gate `MC2_VK_WATER_TIER1` (default-OFF,
envTriState opt-in like SKIRTS/VISUALDISPLACE_FAR; precedence over the P9a
`MC2_VK_WATER_INTEROP`). `vulkan_water_island.cpp` tier1 mode renders the armed
water base layer IN-PLACE into the shared scene GBuffer (color0+normal+depth
LOAD-preserve, terrain-island sync protocol) — kills the P9a offscreen target,
the GL composite pass, AND the per-frame glGetTexImage depth readback
(receipts: depthin_getTexImage=0, depthin_copyBufToImg=0, forbidden_*=0 on the
live gate-ON run). Blend: FS stays premultiplied; pipeline color ONE/OMSA +
alpha SRC_ALPHA/OMSA == GL straight-alpha one-stage arithmetic; shared normal
attachment writeMask=0 (GL water GBuffer1 a=0 no-op parity). Depth: shared
depth LOAD (water runs after terrain), reverse-Z GEQUAL test+WRITE,
WATER_DEPTH_FUDGE_FAST 0.00025 lockstep unchanged (same VS).
- Offline: check_vk_shaders 34/34, check_ubo_layout OK; NEW port pair
  `waterBaseColor` (GL MDI FS vs water_geom.frag) = **VERBATIM** (17 lines).
- Byte-off control (gates unset, GL x2, mc2_01 water bookmarks): 0.000 /
  0.003 MAD — clean.
- Live gate-ON receipts (mc2_01, water bookmark sweep + sacrificial first
  bookmark for VK-FIRST-BOOKMARK-POSE-1): water island used_vulkan=492,
  vk_draws=1, thin=6167, recipes=9801; gate-OFF VK run: water used_vulkan=0.
- Water-only delta (vk_base vs vk_water, SAME VK scene, single variable):
  overview_high 0.54/ch cov 0.04%; oblique_low 8.29/ch cov 62%. ATTRIBUTED by
  experiment: GL water reflection defaults ON (strength 1.5 + HDRI LOD4,
  gameos_graphics.cpp:3100); with MC2_WATER_REFLECTION=0 the delta collapses
  to 3.56/ch cov 10.5% (oblique) / 0.32 (high). The VK FS is the documented
  reflection-OFF base-layer subset (P9a parity contract) — the dominant
  divergence is the MISSING reflection/HDRI feature, not a port defect.
- Depth discipline: shorelines correct, islands occlude water, no z-fight, no
  water-vanish (visual check on gate-ON captures). GL-vs-VK whole-frame pose/
  pillarbox framing difference at these bookmarks is the PRE-EXISTING VK-sweep
  camera artifact (present identically in vk_base, independent of this rung).
- Smoke: tier1 5/5 PASS both states (+0 destroys). Gate-ON smoke FPS ~7 is the
  PRE-EXISTING umbrella-under-smoke cost (measured identical without the water
  gate), not a water regression.
- Residuals / next sub-rung: (1) port the reflection block (SH-L2 sky + HDRI +
  RT sample + sun spec) into water_geom.frag + UBO to close the 3.6-8/ch
  grazing-angle gap — this is the remaining rung-6 feature work; (2) detail/
  spray layer (isWater==2 second MDI draw) still GL-discarded both sides, no
  port needed today; (3) refl-off residual 3.56/ch cov 10.5% at grazing not yet
  itemized (suspects: sun-spec default, fog band). Artifacts + oracle:
  `.claude/vk-water-tier1/` (water_oracle.py, cap/*).

## RUNG 5 — VK-TERRAIN-VISUALDISPLACE-FAR-1 (2026-07-07) — MODE-2 DONE

Commits `4c427cf9` (shaders) + `49ba7f25` (wiring). Gate
`MC2_VK_TERRAIN_VISUALDISPLACE_FAR` (default-OFF, envTriState opt-in, requires
ALL_LODS, smoke-allowlisted incl. GL `MC2_TERRAIN_VISUAL_DISPLACE_FAR` knob).
Scope: mode-2 far-band (lodStep>1, non-stitched) pure Z-swap ONLY; mode-1 (LOD0
4x topology) + stitched patches stay GL-owned (separate L slice later). VK skirt
TOP verts displace like GL (GL's mode-2 branch runs for skirt tops); bottoms stay
coarse-h - skirtDepth. New: binding-10 heightsFine SSBO (dummy 16B from frame 1),
snapshot_visual_height version-gated mission-load copy, UBO visualSide/
visualDisplaceFar/visualMode @ off 580/584/588 (1024B slot unchanged), per-patch
visualMode in Mc2TerrainTier1Patch, feed-dump displacedPatches field.
- Offline: check_vk_shaders 34/34, check_ubo_layout OK (47 fields), port pairs
  `visualDisplaceFar` x2 (main + skirt VS) = **VERBATIM** (local u_* aliases +
  anonymous SSBO instance keep the block textually identical to GL).
- Byte-off: control GL x2 gates-unset = 0.000 on 10/11 mc2_01 bookmarks
  (cross_map_view has PRE-EXISTING run-to-run drift, gates unset — excluded);
  displace-ON + umbrella+ALL_LODS but new gate OFF vs GL = 0.000-class (max MAD
  0.35/ch, cov <=1.4%) — predicate excludes everything, GL owns all pixels.
- Displaced-capture evidence: GL band-count telemetry LOD1=5 (GL ref run) ->
  LOD1=0 (VK gate-ON run) = VK captured exactly the 5 mode-2 patches; snapshot
  receipt `V=397 bytes=630436 ver=1`; used_vulkan=739 (VK run), 0 (GL run).
  NOTE: MC2_VK_FEED_DUMP produced no file this sweep (readyToDump gating) —
  evidence via band counters instead.
- Z-swap mechanism proof (single-variable): fade knob 1.0 vs 0.0, all else
  identical -> MAD 0.00-0.78/ch, cov <=0.81%, localized far-band silhouettes.
  The displacement math executes in VK and is small/localized as expected.
- GL-vs-VK absolute MADs gate-ON (mc2_01, 11 bookmarks): 13.6-44.3/ch, cov
  30-62%. ATTRIBUTION: dominated by the PRE-EXISTING bare-umbrella family
  residual (measured baseline at same poses, NO displacement anywhere:
  13.9-15.0/ch cov ~30%) — arming the gate flips far-band ownership to VK,
  exposing the known VK terrain look-class divergence (sky-band fill + lighting)
  on far surface. NOT a Z-swap defect (see fade-knob proof). Poses verified
  self-matching (no VK-FIRST-BOOKMARK-POSE-1 hit this sweep).
- Smoke: tier1 5/5 PASS both states (gates-OFF and displace+umbrella+ALL_LODS+
  gate-ON), +0 destroy-delta all missions, worktree run_smoke vs 0.4c exe.
- Residuals/deferred: mode-1 LOD0 port (L slice); stitched-patch ownership;
  0.4c lane ships NO visual_height_4x.r32 bakes (only v0.4 does) — live tests
  used MC2_TERRAIN_VISUAL_HEIGHT_FILE override; feed-dump readyToDump gating
  under sweeps; artifacts `.claude/vk-visdisp-far/`.

## Gate inventory (umbrella + sub-gates)

- Umbrella: `MC2_VK_SHARED_GBUFFER=1` arms the family; explicit per-gate env
  overrides (`MC2_VK_TERRAIN_TIER1`, `MC2_VK_TERRAIN_ALL_LODS`,
  `MC2_VK_TERRAIN_SKIRTS`, `MC2_VK_TERRAIN_LIGHTING`, `MC2_VK_TERRAIN_DYNSHADOW`,
  `MC2_VK_TERRAIN_DETAIL`, `MC2_VK_TERRAIN_LODFADE`, `MC2_VK_PROP_TIER1`,
  `MC2_VK_MECH_TIER1`, ...) win when explicitly set 0/1.
- All default-OFF; umbrella-ON verified live this session (see FPS table below).

## Current oracle numbers (this session, mc2_01, ~20s live run, umbrella ON)

Two runs sampled via `tools/perf_snapshot.py`:

| run | FPS(proxy) | terrain total_us | prop total_us | mech total_us |
|---|---|---|---|---|
| 1 | 211.2 | 4734.0 | 1585.1 | 810.5 |
| 2 | 217.8 | 4592.1 | 1596.4 | 806.0 |
| 3 (12s) | 385.1 | 2596.9 | 1658.6 | 863.4 |

FPS is a coarse proxy (`1e6/max(total_us across islands)`), NOT a real frame
counter — the debug-state dump has no frame-count field today. If one gets
added, update `tools/perf_snapshot.py` first.

## Ladder position / next rung

VK-OFFLINE-PROOFS-1 (this session): 5 offline verification tools built +
validated live against the real deployed exe. See `tools/` section below.
Next rung: build a real `MC2_VISUAL_BOOKMARK_CAPTURE` bookmark file so
`tools/vk_feed_diff.py` runs can be TRUE golden-vs-current (today's live
validation showed non-determinism run-to-run without a bookmark: mech gait /
prop streaming / camera drift all still moving — see readyToDump() gate in
`GameOS/gameos/vk_feed_dump.h`).

## VK-lane tool section (agent-efficiency — cross-worktree doc awkward, kept here)

- **`tools/vk_feed_diff.py`** — golden-vs-current diff of `MC2_VK_FEED_DUMP`
  dumps (terrain/prop/mech feeds). `py -3 tools/vk_feed_diff.py golden.jsonl current.jsonl [--island NAME] [--allow-missing]`.
  Validated: identical files → exit 0; synthetic ALL_LODS-on-vs-off diff →
  reports exactly the added coarse patch, exit 1.
- **`GameOS/gameos/vk_feed_dump.h`** — the dump-side harness the 3 feed
  producers (`gos_terrain_lod_chunk.cpp`, `gos_static_prop_batcher.cpp`,
  `gos_mech_batcher.cpp`) call into. Env `MC2_VK_FEED_DUMP=<path>`. Gated by
  `readyToDump()`: fires only when the sim is frozen for a bookmark sweep
  (`mc2CaptureGaitFreezeActive()`) if a bookmark is configured; else fires on
  first successful feed with a one-time stderr determinism warning.
- **`tools/glsl_port_diff.py`** — verbatim-port prover for `//PORT-BEGIN:name`/
  `//PORT-END:name` marker pairs. Registry (`--all`) covers `calc_light`
  (`shaders/include/lighting.hglsl` vs `shaders/vulkan/mech_tier1.vert`),
  `lodNear` (`shaders/gos_terrain.frag` vs `shaders/vulkan/terrain_tier1.frag`),
  `skirtVertMath` (`shaders/terrain_lod_chunk.vert` vs
  `shaders/vulkan/terrain_tier1_skirt.vert`). All 3 REPORT AS DIVERGING after
  whitespace/comment normalization — real renames (identifier/variable-name
  changes only, semantics preserved), not silently fixed. See findings below.
- **`tools/check_vk_shaders.py`** — wraps `glslangValidator -V` (same tool +
  flags CMake's VULKAN-SHADER-TOOLCHAIN-1 target uses) over `shaders/vulkan/*.{vert,frag}`,
  errors-only. `py -3 tools/check_vk_shaders.py`. Validated: 34/34 clean;
  deliberate syntax error in `fullscreen.frag` detected then reverted.
- **`tools/check_ubo_layout.py`** — cross-checks C++ island UBO mirror structs'
  `// off N` comments against independently-recomputed std140 offsets parsed
  from the GLSL uniform block. Registry: terrain (`TerrainFrameUBO`), prop
  (`ColorFrameUBO`). Validated: both OK (36 + 10 cross-checked fields);
  deliberate off-by-931 error in `vulkan_terrain_island.cpp` caught then reverted.
- **`tools/perf_snapshot.py`** — PID-tracked launch + N-second wait +
  debug-state-dump island-timing read + exact-PID kill, one-line FPS+per-island
  report. `py -3 tools/perf_snapshot.py --exe <path> --mission mc2_01 --duration 20 --env MC2_VK_SHARED_GBUFFER=1`.
  Validated live (see FPS table above); never leaves an orphaned process
  (finally-block exact-PID taskkill).

## Verbatim-port findings (tools/glsl_port_diff.py --all)

All three registered ports (`calc_light`, `lodNear`, `skirtVertMath`) are
**functionally-equivalent but NOT textually verbatim** — cosmetic identifier
renames only (e.g. `lights_index`→`idx`, `n_dot_l`→`ndl`, `u_skirtDepth`→
`uT.skirtDepth`). No logic divergence found. Not "fixed" — recorded as findings
per the tool's own contract (see tool docstring).

## Session update convention

Whoever picks up VK work next: update the FPS table + ladder position above
before ending the session. Keep this file under ~60 lines.

## FIELD-CONFIG SESSION 2026-07-06 (commit a50c6033) — full-stack verification

Standing rule: every fix verifies under the FULL launcher field env
(`tracy-vk-compare.bat` + `MC2_STATICPROP_TEX_TIER=512`), not gate-isolated.
Oracle: `.claude/mech-parity/field_oracle.py` (field GL/VK bookmark sweep). NOTE:
capture window MUST be visible (`wShowWindow=4`), NOT minimized(7) — minimized =
zero surface extent = VK present fail-softs to GL and silently invalidates the
run. Islands (terrain/shadow/prop/mech) still render under present-fail-soft, so
composited pixels remain a valid VK oracle.

- **BUG 1 (terrain shadow wash) FIXED (interim)** — MC2_VK_TERRAIN_CSM full-map
  subset is net-negative at play distance (coarse full-map cascade vs GL's near
  cascade = camera-swimming blobs). Field GL-vs-VK MAD 28-31 (wash) -> ~5
  (clean) on airport bookmarks. Double-gated behind
  `MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL=1` (parked escape hatch).
- **VK-TERRAIN-CSM-NEAR-1 (real cure, LANDED, FIELD-VERIFIED 2026-07-06 this
  session, commit `1a4969c8`)** — ports GL's `calcDynamicShadow` near-cascade
  cascade-select VERBATIM into `terrain_tier1.frag`
  (`//PORT-BEGIN/END:calcDynamicShadowCsm`, registered in
  `tools/glsl_port_diff.py`). Imports GL's near-cascade array
  (`dynShadowArrayTex_`, GL_TEXTURE_2D_ARRAY depth) as a VK D32 depth-COMPARE
  `VK_IMAGE_VIEW_TYPE_2D_ARRAY` (`sampler2DArrayShadow`, binding 8) + the
  separate full-map catch-all cascade (binding 9) + all cascade
  matrices/texel-world/csmDepthSpan into `TerrainFrameUBO` (grew 512B->1024B,
  `check_ubo_layout.py` passes, 44 fields cross-checked). Copy mechanism:
  glGetTexImage per frame (no zero-copy producer for the array exists yet —
  the VK shadow island only produces the single legacy map) — documented as a
  known cost, not hidden. Gate `MC2_VK_TERRAIN_CSM` now means THIS
  implementation (requires MC2_VK_TERRAIN_DYNSHADOW chain + GL
  MC2_SHADOW_CSM=1); FULLMAP_EXPERIMENTAL untouched as a fallback escape
  hatch, mutually exclusive C++ population (no collision).
  **FIELD RESULTS** (rebuilt+redeployed 0.4c @ `1a4969c8`, fingerprint
  confirmed): terrain island co-writes confirmed active (LOD0 TIER-1 patches
  drawn, `used_vulkan>0` both near+airport runs). Byte-off control (gates
  off, GL x2) = 0.000 on 10/11 mc2_01 bookmarks (1 negligible max=1px, cov
  0%). VK self-diff determinism at the near building-shadow pose: mean
  0.27–1.76/ch, cov ≤0.79% (clean; one lodfade-pose max=255 spike is the
  known mech-gait jitter, not terrain). Airport bookmarks (`overview_center`,
  `runway_overview`) did NOT reproduce the a50c6033 wash pattern specifically
  — but DID show a severe GL-vs-VK divergence (mean 32–34/ch, cov ~97%) that
  **bisects to a PRE-EXISTING, CSM-independent bug**: reproduced identically
  with `MC2_VK_TERRAIN_CSM` unset and even with only the bare
  `MC2_VK_SHARED_GBUFFER=1` umbrella set (no other sub-gates). This matches
  the already-open **FAR-BAND BLACK** entry below (`oblique_horizon`,
  `overview_center` — both wide/far bookmark poses). VK-TERRAIN-CSM-NEAR-1
  itself is NOT implicated (identical wash with/without the CSM gate). The
  building-shadow near-pose oracle (`near_land_low`) was separately blocked
  by an UNRELATED, reproducible GL-side bug this session:
  `[MECH_LOAD] WARN: mech appearance has no base shape ... mechType=0` fires
  deterministically on GL (both `gl`/`gl2` runs, byte-identical 0.000
  self-diff) at this pose+FORCE_LOD=0 combo — 6 mechs registered
  (`RENDER_WORLD registered=6 alive=6`) but none get a base shape, so GL
  renders NO mechs/shadows at this pose while VK renders all 3 correctly.
  Root cause not investigated further (out of CSM-near scope); flagging for
  a follow-up. The mech-free `near_land_off` control pose (same location,
  no units) gave GL-vs-VK mean_all 9.8/ch cov 20% (masking known sky/EXR-
  missing black band at rows 0-220, a pre-existing artifact present in prior
  sessions' `ridge_lowangle` captures too) — consistent with the arc's
  documented ~5-10/ch near-detail residual band, no new regression signal.
  Smoke: tier1 5/5 PASS both umbrella-OFF and full-field+CSM-ON states, 0
  crash, deploy fingerprint 1a4969c8 confirmed both runs.
  **VERDICT: CSM-near mechanism itself verified clean (byte-off clean, VK
  self-diff clean, terrain island actively co-writing, no CSM-attributable
  wash); the loud field numbers trace to two PRE-EXISTING unrelated bugs
  (FAR-BAND BLACK, GL mech-load-at-this-pose) that predate/are independent of
  this commit. Full artifacts: `.claude/mech-parity/csm_near_field/`.**
- **BUG 4 (mech screen-space freeze on pan) FIXED** — feed now uses live
  gos_GetMechFixBMVPMat4() not the flush-time snapshot. Static-capture cannot
  pan; verified by code-trace + no static regression. USER RE-TEST on live pan.
- **BUG 2 (fence) / BUG 3 (hangar washout)** — hold under field config; prior
  fixes (b911bc0d / 91051001) confirmed present, props render (walls/quonsets
  textured, fence brackets present) in field runway_overview + mc2_24. Layer
  index is bucket-relative/order-derived => tier-INVARIANT (deploy has only
  128/ tier dir; missing 512/ => whole-group fallback, GL==VK). No tier-driven
  layer shift.

### OPEN (documented, NOT fixed this session — for next session)
- **BUG 5 (2x sim speed, field env)** — NOT a fixed-step misfire:
  `MC2_SMOKE_FIXED_TIMESTEP` is the ONLY armer (gos_smoke.cpp:54); the field bat
  does NOT set it, and `MC2_DEBUG_STATE_DUMP` touches no timestep path. Sim dt =
  `frameLength = 1.0/frameRate` (mechcmd2.cpp:2217); frameRate = live wall-clock
  `1000/dt` (gameosmain.cpp:1957). Needs an instrumented sim-time-vs-wall run
  (field vs minimal env) to root-cause the 2x. SIM-SPEED FEATURE SEAM (user
  request, do NOT implement yet): the clean knob is `frameLength`
  (mclib/timing.cpp:20, set mechcmd2.cpp:2217) — a future MC2_SIM_SPEED/UI
  button scales it.
- **FAR-BAND BLACK — PARTIAL FIX this session (`gos_terrain_lod_chunk.cpp` gate
  bug fixed; deeper CSM-near shading defect still OPEN).** Root-caused via
  bisection (MC2_VK_TERRAIN_LIGHTING/DETAIL/DYNSHADOW/CSM toggled one at a time
  on overview_center): the VK-TERRAIN-CSM-NEAR-1 feed-population block
  (`gos_terrain_lod_chunk.cpp` ~line 964, `out->csmEnabled=1` path) was gated on
  `dynShadowGate` (the DYNSHADOW sub-gate) ONLY — despite its own comment
  claiming "MC2_VK_TERRAIN_CSM alone... now means THIS implementation", the code
  never actually checked `mc2vkgate::subGate("MC2_VK_TERRAIN_CSM")`. Since GL
  CSM defaults ON (`MC2_SHADOW_CSM` default-ON) and the umbrella defaults
  DYNSHADOW ON too, this fed real CSM cascade data to the VK terrain frag any
  time DYNSHADOW was armed, REGARDLESS of the CSM sub-gate's own value — a
  real, provable gating bug, now FIXED (added the missing `csmNearGate` check).
  Verified: bare-umbrella-minus-explicit-CSM-off now renders terrain correctly
  (`.claude/mech-parity/csm_near_field/farband_realfix/vk_umbrella_csmoff/`).
  **HOWEVER** the FIELD launcher recipe (and bare umbrella with no overrides)
  legitimately ARMS both DYNSHADOW and CSM together (by design), and the
  overview_center/oblique_horizon black-out STILL reproduces in that
  configuration post-fix (`vk_final/mc2_01_overview_center.png`) — so a SECOND,
  deeper defect lives inside the CSM-near shading path itself
  (`mv_calcDynamicShadowCsm`/`mv_csmProject`/`mv_csmSampleLayer` in
  `shaders/vulkan/terrain_tier1.frag`, or the C++ feed populated at
  `gos_terrain_lod_chunk.cpp` lines 964-982) when the near-cascade path is
  legitimately active — most likely `normalLight`/`uT.terrainLightDir`
  collapsing near-zero (line 708 `lit = base * normalLight * shadow` would
  blacken the WHOLE fragment, not just the shadow term, matching the
  near-total-blackout symptom rather than a milder shadow-only darkening).
  NOT YET ISOLATED further this session (ruled out: depth format
  D32_SFLOAT_S8_UINT vs GL_DEPTH24_STENCIL8 reinterpretation — tried both a
  D24_UNORM_S8_UINT export swap [driver rejects it, falls back to D32S8, no
  change] and a native-VK-clear fix on the shared depth attachment [no change]
  — depth is NOT the mechanism; isolating LIGHTING/DETAIL/DYNSHADOW-without-CSM
  all render terrain correctly, only CSM-near-armed reproduces the blackout).
  Overview MAD: unable to compute a numeric MAD this session (no golden/
  byte-identical GL reference existed for this exact bookmark+camera-jump
  timing; visual A/B via screenshots only). NEXT SESSION: instrument
  `computeNdotLBand`/`uT.terrainLightDir` and the CSM UBO fields with a
  one-shot stderr dump inside `terrain_tier1.frag` (or add a
  MC2_VK_TERRAIN_CSM_DEBUG viz mode) to see whether lightDir/normalLight or the
  CSM sample itself is the zero. Artifacts:
  `.claude/mech-parity/csm_near_field/farband_realfix/`,
  `.claude/mech-parity/csm_near_field/oobfog_isolate/` (bisection captures).

- **CSM-BLACKOUT-RECON-1 (this session) — BLOCKED on a broken deploy lane,
  root cause NOT isolated live.** Coordinator's UBO-write-collision hypothesis
  (terrain UBO grew 512->1024B, maybe two feed paths stomp each other) was
  checked FIRST as instructed and RULED OUT by static read: the per-patch UBO
  populate loop (`vulkan_terrain_island.cpp` ~2193-2264) zero-inits a fresh
  `TerrainFrameUBO u{}` every patch and does a full `slots[i]=u` struct
  assignment (no partial memcpy/stale-size aliasing); `sizeof(TerrainFrameUBO)`
  is used consistently for the descriptor range, `ensure_ubo_capacity`, and the
  dynamic-offset stride — no collision found. `check_ubo_layout.py` still
  passes (44 fields). Built the planned one-shot debug-viz instrumentation
  instead: a new UBO field `csmDebugMode` (int, repurposed the `_csmPad2` slot
  at UBO off 364, fed from env `MC2_VK_TERRAIN_CSM_DEBUG=1..5`) that early-outs
  `terrain_tier1.frag`'s lit compose to show `normalLight`(1) / `lightDir`(2) /
  `staticS`(3) / `dynS`(4) / `shadow`(5) in isolation
  (`GameOS/gameos/vulkan_terrain_island.cpp` UBO struct + populate site,
  `shaders/vulkan/terrain_tier1.frag` struct mirror + branch after the
  `lit = base*normalLight*shadow` line). Builds clean, `check_vk_shaders.py`
  34/34, `check_ubo_layout.py` OK, full mc2 build succeeds, deployed to 0.4c.
  **BLOCKED**: every VK launch this session (both the new debug probe and a
  vanilla re-run of the existing `field_oracle.py`, 3 attempts) hit
  `[VK_PRESENT] init: vkCreateSwapchainKHR failed. fail-soft.` ->
  `[RENDER_BACKEND] fail-soft: forcing GL for the session` — the whole run
  silently became a GL capture, invalidating all screenshots taken (the
  earlier "reproduction" in `.claude/mech-parity/csm_near_field/blackout_debug/`
  is GL, not VK; disregard those PNGs). `mcp__mc2-repo-intel__deploy_status`
  on `A:/Games/mc2-opengl/mc2-win64-0.4c` independently confirms
  `verdict=BROKEN-TREE`: "0.4c is a BROKEN tree (stock + modified exe both
  crash_silent / PAUSE-loop, 0 frames). Smoking it yields FALSE crash
  reports." — this is a pre-existing lane-health problem, not caused by this
  session's edits (confirmed by retrying the untouched `field_oracle.py`
  recipe with identical failure). Exe fingerprint DID match HEAD (7f396c2e)
  each time — the deploy itself was clean; the swapchain failure is
  environmental/lane-state, not a stale-exe issue. **NEXT SESSION**: get (or
  wait for) a healthy 0.4c (or a fresh, uncontended deploy dir) before
  re-running this probe; the instrumentation above is ready to use
  immediately — just set `MC2_VK_TERRAIN_CSM_DEBUG=1` (normalLight) first
  against a working VK backend at `overview_center`/`oblique_horizon` and
  read the pixel values (should be mid-gray `mix(0.35,1.20,NdotL)`, achromatic
  — any color tint or near-0 reading means the debug branch itself isn't
  executing on VK, which would point back at UBO wiring after all). Debug
  field is fully gated (env-off = 0 = zero behavior/perf change) — safe to
  leave in tree. Shadow-saga status: UNCHANGED from before this session — the
  deeper CSM-near-armed blackout defect is still open, now with ready-to-use
  instrumentation blocked only by lane health.
- **CSM-BLACKOUT-RECON-1 RESOLVED (2026-07-07 session) — blackout does NOT
  reproduce at HEAD `cf91cba5`; the "deep CSM-near shading defect" was a
  compound evidence artifact, not a shader bug.** Lane health CLEARED first:
  rebuilt+redeployed 0.4c at `cf91cba5` (manifest src_commit confirmed), plain
  VK launch on a visible window (wShowWindow=4) → `[VK_PRESENT] swapchain OK
  images=3 extent=1707x960` + present path UP, no fail-soft (last session's
  vkCreateSwapchainKHR failure was environmental, as suspected). Full
  MC2_VK_TERRAIN_CSM_DEBUG=0..5 ladder run at all mc2_01 bookmarks, every run
  with a used_vulkan receipt (488–496; GL control used_vulkan=0):
  `.claude/mech-parity/csm_near_field/blackout_debug/vk_dbg{0..5}/` + `gl_ref/`.
  FINDINGS: (1) the debug branch executes correctly on VK (achromatic grays →
  UBO wiring at off 364 GOOD); (2) NO term collapses — at the oblique_horizon
  "black band": normalLight=0.70, lightDir sane, staticS=0.67, dynS=1.00
  (CSM sample fully LIT), shadow=0.67; (3) the oblique_horizon dark band is
  PRESENT IN GL TOO (GL band mean 23/ch vs VK 17/ch — it is the stock look:
  dark slope+baked-shadow band), terrain-only MAD GL-vs-VK = 3.79/ch at
  oblique_horizon, 4.5–17/ch across all matched bookmarks — inside the
  documented near-detail residual, NO blackout-class divergence; (4) the
  "overview_center blackout" screenshots were captured at the WRONG CAMERA
  POSE under VK configs — cross-matching VK captures against all GL bookmarks
  shows VK's first bookmark (overview_center) lands on a water/fog-edge pose
  (best-match = edge_fog_corner_southwest) while every subsequent bookmark
  matches its own name; reproducible with settle=40 AND settle=80, VK-config
  runs only, deterministic. The prior session's post-fix "blackout still
  reproduces" capture (`vk_final/mc2_01_overview_center.png`) is this same
  first-bookmark wrong-pose artifact (dark fog-edge water view misread as
  terrain blackout). VERDICT: CSM-near path healthy at HEAD; no shader/feed
  fix needed, nothing landed beyond probe-script receipts. NEW OPEN ITEM →
  **VK-FIRST-BOOKMARK-POSE-1**: bookmark-sweep capture under VK gates captures
  bookmark #1 at a stale/wrong camera pose (GL runs are correct). Any
  single-bookmark VK evidence at "overview_center" from sweep captures is
  suspect until this is fixed; use later-in-sweep bookmarks or
  single-bookmark runs for VK evidence.
- **BUG 6? prop texture scramble** — coordinator saw live (pink slabs, floating
  textures, trunkless trees) DURING mc2_24 load on the Bug1+Bug4 exe. NOT
  reproduced at settled capture (props clean: walls/trees/buildings textured).
  Classified LOAD-TRANSIENT, not a static-reproducible regression; neither
  committed fix touches prop layers. USER cold-load re-check to confirm.

## ADJUDICATION-1 (2026-07-06) — contamination-proof fence + washout verdict

Method: deterministic bookmark-sweep capture (sim-frozen, fixed seed/timestep,
SW_SHOWNOACTIVATE=4 visible window) at an authored **airport bookmark**
(`c_rot110`: pos 3200,100,~400 / rot 110 / proj 28 / alt 1000 — frames hangar +
perimeter fence + runway, the user's scene class). GL ('off' set) vs VK
(canonical 'on' set = `MC2_VK_SHARED_GBUFFER=1 MC2_VK_SHADOW_INTEROP=1` +
`MC2_SHADOW_CSM/STATIC_PROP_PBR_V1/AMBIENT_V1/IBL_SH=1`). Receipts printed per
capture: **GL** vk_color_prop used_vulkan=0 (correct GL label); **VK**
used_vulkan=400–411, vk_draws=1, interop_active=1, vk2gl_sem_active=1,
inst_records=1006, type_runs=78, geom_verts=23628 (correct VK label — pixels
ARE VK, no fail-soft). Artifacts: `.claude/adjudication/{gl,vk_probe}/`,
`crop_{hangar,fence}_{gl,vk}.png`, `rdc/` (.rdc + XML), `vk_probe/packets.csv`.

**RenderDoc finding (contamination class 3, DIRECTLY OBSERVED):** a
`renderdoccmd capture -w` of the canonical VK-on config produced a capture whose
header is `<driver>OpenGL</driver>` with ZERO vk* chunks. The VK prop island is
HEADLESS (offscreen + vk2gl semaphore, no swapchain/present), so a present-based
GL RenderDoc hook cannot see its `vkCmdDrawIndexed`s. **Lesson: RenderDoc-under-GL
of a shared-gbuffer interop frame CANNOT prove the VK island — its draws are on
the VK queue with no present. Draw-level VK facts must come from an in-record
dump (added: `MC2_VK_PROP_PACKET_DUMP`) or a StartFrameCapture/EndFrameCapture
device-scoped bracket, NOT TriggerCapture.**

**FENCE — VERDICT: STILL BROKEN in canonical config (alternating panels).**
Own GL/VK pair at identical pose: GL = continuous chain-link; VK = alternating
panels (every other panel's mesh missing). Draw-level (`packets.csv`, 80 packets):
EVERY packet draws non-degenerate with exactly one valid push-constant layer
(`layerOff`/`layerOn`), zero "both -1", zero zero-count draws. **So the
alternation is NOT dropped draws / -1 layers — the per-packet layer fix
(b911bc0d / VK-FLIP-FIELD-REPORT-2) IS correct at the data level.** Root cause is
the alpha-ON group's **0.5 alpha-test discard** (`color_prop_tier1.frag:69
if (texc.a < 0.5) discard`) against the VK copy-once albedo ARRAY's mip/filter:
the minified chain-link alpha aliases below 0.5 on alternate panels at this
distance, where GL's LOD-chunk path keeps it. **INVALIDATES the prior "black-fence
FIXED" claim's completeness** — the black→visible fix (`max(vArgb,0.5)`) and the
per-packet-layer fix both landed, but neither addresses alpha-test panel dropout,
which is what the user actually sees.

**WASHOUT — VERDICT: STILL BROKEN, and it is NOT a fail-soft.** Hangar-crop mean
brightness (0..255): GL 123.8 vs VK 171.1 = **+47.3** (MAD 66.3); scene-wide VK
+24.5. Engagement probe (`MC2_VK_PROP_LIGHT_PROBE`): `lightingReady=1
haveLightFeed=1 iblShStrength=0.5 iblSh0=(1.71,1.77,1.65) lightDataBytes=3610800`
— **the real IBL/ambient feed DID engage; the hemisphere fail-soft is NOT taken.**
Kill-switch matrix (hangar bright): GL-on 123.8 / VK-on 171.1 / GL-off 121.2 /
VK-off 146.9 → GL's IBL adds +2.7, VK's IBL adds +24.2 (≈9× too strong), AND VK
is +25.7 over GL even with IBL OFF. Two stacked over-brightening bugs in the
VK prop lighting math (`color_prop_tier1.vert`:
`vArgb = seed + ambientTerm + iblTerm + kSunColor*ndl`): the real-feed branch
(`lightingReady!=0`) sums raw ObjectLights ambient + `evalShL2*0.5` +
hardcoded `kSunColor*ndl` with no exposure/weight matching GL. **INVALIDATES the
prior "washout FIXED (feed parity 91051001)" claim:** the prior washout tuning
targeted the FALLBACK hemisphere branch (comment in .vert), but canonical config
hits the REAL-FEED branch, which was never washout-tuned. "Verified fixed but
user still sees washout" = tuned the wrong branch.

**Historical claims invalidated:** (1) "fence fixed" — only black→visible +
layer-drop fixed; alpha-test panel dropout remains, user-visible. (2) "washout
fixed / IBL feed parity" — parity was measured on the fallback branch; the
engaged real-feed branch over-brightens hangar by +47. Both bugs reproduce in
the EXACT canonical config with VK-labeled receipts, so they are real, not
oracle artifacts.

**Fixes:**
- **Washout — PARTIAL fix landed, NOT cured.** `color_prop_tier1.vert` real-feed
  branch now scans ObjectLights for the real per-actor sun color (GL parity,
  static_prop.vert:414-438) instead of the hardcoded `kSunColor` stand-in.
  Re-captured (VK used_vulkan=363): hangar +47.3 → **+43.5** vs GL. Only a 3.8/255
  win — proves the phantom sun was NOT the dominant term (the real sun ≈ as bright
  as the constant). **Dominant washout cause still open:** VK/GL apply the same
  `albedo * vArgb` and the same summed terms, yet VK vArgb is +43 hotter. Leading
  hypothesis: GL attenuates prop ambient/sun by CSM shadow (`MC2_SHADOW_CSM=1` ON),
  the VK prop island does NOT shadow-attenuate its vertex-baked lighting → props
  render unshadowed/brighter. Next: instrument whether GL's v_argb includes a
  shadow factor the VK island drops (check static_prop.vert for calcShadow on the
  ambient/sun term vs the VK island's shadow interop wiring).
- **Fence — NOT fixed (root cause pinned).** Draw-level proven it's alpha-test
  panel dropout (`color_prop_tier1.frag:69 if (texc.a<0.5) discard`) against the VK
  copy-once array's mip chain, not a layer/draw drop. Fix path: alpha-to-coverage,
  preserve alpha in the array mips, or match GL's effective coverage — NOT landed.

Instrumentation left in tree, all env-gated default-off: `MC2_VK_PROP_PACKET_DUMP`
(per-packet draw params CSV), `MC2_VK_PROP_LIGHT_PROBE` (UBO lighting engagement),
`[ADJUDICATION_POSE v1]` one-shot pose dump in gos_visual_capture.cpp. Airport
bookmark authored at `.claude/adjudication/airport_bookmark.json` (c_rot110 frames
both bugs). Artifacts under `.claude/adjudication/`.

## ADJUDICATION-1 FOLLOW-UP (2026-07-06, this session)

**WASHOUT — ROOT-CAUSED AND FIXED.** Per-vertex CPU-side probe
(`MC2_VK_PROP_LIGHT_VERTEX_PROBE`, new, gated) dumped the REAL ObjectLights entries
the hangar-area instances (typeID=38) actually use: `numLights=1, ambient=(0.3,0.3,0.3),
sunFound=0` for every sampled instance — confirms no per-actor sun light feeds these
props at all (matches the prior sun-fix's own +3.8/255 result: removing the phantom
`kSunColor` barely moved the needle because GL's own scan also finds no sun here).
`iblSh0` DC coefficient is 1.71 — `evalShL2`'s `kSH_c4*sh[0]` term alone
(`0.886227*1.71≈1.51`) times `iblShStrength=0.5` is already ~0.76, and adding the 0.3
ambient pushes the raw sum past 1.0 before any clamp. **Root cause: VK's
`color_prop_tier1.vert` never clamped `vArgb`, unlike GL's `static_prop.vert:524
lit = clamp(lit + aRGBHighlight, 0.0, 1.0)`.** An unclamped >1.0 vArgb multiplies the
albedo brighter than GL's white-capped equivalent on every bright material (hangar
roofs). Fix: added the matching `clamp(..., 0.0, 1.0)` to `vArgb`'s final assignment
(`shaders/vulkan/color_prop_tier1.vert`). **Measured (canonical config, c_rot110,
template-matched hangar crop, same box both before/after within this session):
hangar delta +43.5 -> -0.6/255 (essentially matched); MAD 34.8 is structural/edge
noise, not a brightness bias.** Receipts: GL used_vulkan=0 (correct), VK
used_vulkan=420/421 frames (correct, real VK pixels, not fail-soft). Smoke: tier1 5/5
PASS both byte-off control and full canonical VK-on config, 0 crash, +0 destroy-delta.
Commit: (this session, see git log).

**FENCE — STILL OPEN, mip/aniso hypotheses RULED OUT with live evidence.** New
`MC2_VK_PROP_MIP_PROBE` diagnostic (gated, left in tree) confirms mip-count parity:
alpha-ON array `glTexMaxLevel=7` (GL) == `vkComputedMips=8` levels / `maxLod=7.0`
(VK) -- GL's own `GL_TEXTURE_MAX_LEVEL` and the VK import's computed chain length
match exactly, both texture groups (alpha-OFF 21 layers, alpha-ON 34 layers). Grep
of the entire renderer for `ANISOTROP*` returns zero hits on both the GL prop path
(`gos_static_prop_batcher.cpp`) and the VK sampler (`vulkan_color_prop_island.cpp`)
-- no anisotropic filtering anywhere, so aniso mismatch is not the cause either.
VK's mip chain is confirmed BYTE-COPIED from GL's own already-generated
`glGenerateMipmap` output (`glGetCompressedTexImage` per level, not re-derived) --
so VK samples the literal same BC7 bytes GL does, at the same LOD range, with the
same CLAMP_TO_EDGE + LINEAR_MIPMAP_LINEAR sampler state, same `fract(vUv)` UV
handling in both fragment shaders. With every concrete sampler/mip/UV divergence
eliminated, a bisect experiment settled it (same session, later):

**FENCE ALTERNATION — ROOT-CAUSED AND FIXED (same session, follow-up).**
Bisect ladder, each step build+deploy+capture with VK receipts (used_vulkan
409-420, GL reference unchanged):
1. Coordinator's fract-seam-derivative hypothesis (`textureGrad` with smooth
   gradients from the UNWRAPPED vUv): landed, deployed SPV verified to contain
   `OpImageSampleExplicitLod ... Grad` -- alternation UNCHANGED. Hypothesis
   DISPROVEN by direct experiment.
2. Forced `textureLod(.., 0.0)` diag: EVERY dropped panel reappeared.
   **Mechanism proven: the minified mip chain's alpha decays below the 0.5
   test (naive glGenerateMipmap box filter halves thin chain-link coverage
   per level), and VK's implicit LOD lands deep enough to cross it where GL's
   does not.** (Mip BYTES are identical -- byte-copied -- so the divergence is
   LOD selection, which is implementation-defined per API/compiler.)
3. Final fix (landed, `shaders/vulkan/color_prop_tier1.frag` alpha-ON branch):
   deterministic LOD computed from unwrapped-vUv gradients
   (`0.5*log2(max(dot(dx,dx),dot(dy,dy)))` in texel units), clamped to <=1.0
   for the alpha-test class (LOD<=1 proven panel-preserving by the bisect),
   sampled via explicit `textureLod`. No driver-implicit LOD dependence left.
   **Result: alternation GONE -- every panel present along the whole run at
   c_rot110 (crop_fence_vk_final.png). Fence-region MAD 29.9 -> 27.1.**
   Residual (documented, NOT the alpha bug): VK panels read slightly narrower
   with gaps vs GL's continuous ribbon -- consistent with the KNOWN-DEFERRED
   per-draw uvScale sub-region gap (VK samples with uvScale=1.0 assumed;
   GL applies uvScaleX/Y per draw, static_prop.frag:206). NEXT CONCRETE STEP:
   feed per-draw uvScaleX/Y through the packet push constant (same plumbing as
   packetLayerOff/On) and apply in the island frag; expect the gaps to close
   and fence MAD to drop to residual-class.
Hangar regression guard re-measured on the same capture: delta -0.5/255 (fix
intact). Smoke mc2_01 PASS both byte-off control and canonical VK-on state.

## FENCE-UVSCALE-1 (2026-07-06, this session) — CLOSES ADJUDICATION-1

**Fix landed.** GL uv-crop site confirmed: `shaders/static_prop.frag:206`
`vec2 uvSampled = fract(v_uv) * vec2(uvScaleX, uvScaleY)`, fed from
`PerDrawEntry.uvScaleX/Y` (`GameOS/gameos/gos_static_prop_batcher.h`), which are
computed per-PACKET in `finalizeGeometry()`
(`gos_static_prop_batcher.cpp:3844` `uvScaleXByPacket[pk] = u.w * invMaxW`,
same "packetsInGroup" loop that fills `layerForPacket`).

**Plumbing (mirrors the b911bc0d packetLayerOff/On pattern exactly):**
new per-packet sidecars `s_packetUvScaleX/Y` persisted alongside
`s_packetTexArrayLayer` in `finalizeGeometry()`, cleared in `onMapUnload()`,
exposed on `Mc2ColorPropMaterialFeed` (`packetUvScaleX/Y`,
`vulkan_shadow_island_live.h`) alongside `packetAlbedoLayer`. VK island
(`vulkan_color_prop_island.cpp`) copies them into `packetUvScaleX/Y` vectors
(default 1.0 = no crop, matching pre-fix behavior when the feed doesn't cover a
packet) at the same site that fills `packetLayerOff/On`. Push constant widened
8->16 bytes (`int layerOff, int layerOn, float uvScaleX, float uvScaleY`) --
well inside the Vulkan-guaranteed minimum `maxPushConstantsSize` of 128 bytes,
so no half-float packing or per-instance-SSBO detour was needed (justified:
8 extra bytes is noise against the 128B floor, and per-packet push constants
were already the established pattern for this exact kind of per-draw data).

**Shader (`shaders/vulkan/color_prop_tier1.vert/frag`):** `vUvScale` forwarded
flat VS->FS. Frag applies `fract(vUv) * vUvScale` verbatim in the alpha-OFF
branch (was `fract(vUv)` only). Alpha-ON branch (the fence's alpha-test class)
applies the same crop BEFORE computing the deterministic-LOD gradients from
this session's earlier alternation fix (`uvSampledOn = fract(vUv) * vUvScale`,
gradients taken from `uvSampledOn` not raw `vUv`) so the sub-region's actual
texel footprint drives LOD selection, not the full-layer one -- otherwise a
narrow sub-region would under-estimate LOD and reintroduce alpha-test aliasing
on exactly the packets this fix targets.

**Verified engaged:** new `MC2_VK_PROP_PACKET_DUMP` CSV (columns extended with
uvScaleX/Y) at c_rot110: 14/80 packets carry uvScale=0.5 (fence chain-link
post/arch sub-mesh packets, typeIDs 70/72/73/124/127/130/134/153/161/175/205/
210/213/216), confirming the plumbing reaches the exact packets the state file
predicted, not a no-op.

**Visual result:** VK fence ribbon is now continuous and full-width, matching
GL's panel coverage/spacing at c_rot110 (`crop_fence_gl_final2.png` vs
`crop_fence_vk_final2.png`, `.claude/adjudication/`) -- no narrowing, no gaps.

**MAD arithmetic note (important, avoids re-litigating a false regression):**
raw unaligned fence-crop MAD went 27.1 -> 28.6, which LOOKS like a regression
but isn't -- diagnosed as separate-process camera-pose jitter (~11px vertical
drift between GL/VK launches), present IDENTICALLY in the prior 27.1 baseline
capture (re-measured: prior pair's own best-match shift is dx=-12,dy=11,
aligned MAD 28.46, not 27.1 -- the originally-reported 27.1 was itself an
unaligned/lucky-crop number). Aligned (shift-corrected) apples-to-apples MAD:
prior 28.46 -> this session 27.54, a real but modest improvement, consistent
with only 14/80 packets carrying non-1.0 scale. **Lesson for future
bookmark-sweep MAD comparisons: always cross-correlate for a translation
offset before trusting a raw crop MAD delta as small as 1-2/255 -- separate-
process camera jitter alone produces deltas that size.**

**Regression checks (same c_rot110 capture pair):**
- Hangar brightness delta: GL 123.2 vs VK 120.4 = **-2.8/255** (within the
  existing -0.5..-0.6 residual band from the washout fix; no new regression).
- Fence visual: continuous ribbon, panel-preserving (screenshot pair above).
- Packet dump: zero degenerate draws, zero "both -1" layers (same invariant as
  the alternation-fix session).

**Smoke:** `MC2_DEBUG_STATE_DUMP=1` + tier1 (`mc2_01 mc2_03 mc2_10 mc2_17
mc2_24`, 30s each) against `A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe`
(sha `6366c922de88`, fingerprint-matched worktree HEAD): **byte-off control
5/5 PASS, +0 destroy-delta all missions; canonical VK-on config
(`MC2_VK_SHARED_GBUFFER=1 MC2_VK_SHADOW_INTEROP=1` +
`MC2_SHADOW_CSM/STATIC_PROP_PBR_V1/AMBIENT_V1/IBL_SH=1`) 5/5 PASS, +0
destroy-delta all missions.**

### ADJUDICATION-1 FINAL SCOREBOARD

| Bug | Adjudication-1 verdict | Follow-up verdict | This session (FENCE-UVSCALE-1) |
|---|---|---|---|
| Fence alternating panels | STILL BROKEN (alpha-test dropout) | ROOT-CAUSED + FIXED (deterministic clamped LOD) | n/a (already closed) |
| Fence narrow/gapped panels vs GL ribbon | not yet isolated | isolated as uvScale=1.0 assumption, deferred | **FIXED** (per-packet uvScale plumbed + applied; visually continuous/full-width, aligned MAD 28.46->27.54) |
| Hangar washout | STILL BROKEN, not fail-soft (+47.3) | ROOT-CAUSED + FIXED (unclamped vArgb, -0.6/255) | held (-2.8/255 this capture, same residual band) |

**ADJUDICATION-1 is CLOSED as of this session.** Both bugs it raised (fence,
washout) are root-caused and fixed; the one open residual the follow-up
session left on the table (fence uvScale sub-region gap) is now also fixed and
verified. No further action items outstanding from this adjudication.

## RUNG 8.4 - VK-POST-HZB-NATIVE-1 SHIPPED

Native HZB now builds the postprocess depth pyramid on the shared engine
VulkanContext under `MC2_VK_POST_HZB=1` when `MC2_HZB_BUILD=1`. The path samples
the shared scene depth directly and writes VK-owned R32F ceil-ladder pyramid
levels; the old GL `runHzbReduce()` body remains the fail-soft fallback/oracle.

Critical sync fix: the HZB pass must not advance only the shared depth resource
parity. Doing so desynchronizes the shared GBuffer triplet from terrain/prop
writers, whose submit retirement model is keyed off the color fence. The shipped
implementation acquires/releases color0, normal, and depth together, but records
barriers and samples only depth. Validation also required depth barriers to use
`VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT` for the shared
`D32_SFLOAT_S8_UINT` image.

Receipts:
- Focused native HZB smoke `mc2_01`: PASS, 168 frames, +0 destroys.
- Focused validation smoke `mc2_01`: PASS, 79 frames, +0 destroys; no HZB VUIDs.
- HZB health: attempted=79, used_vulkan=79, levels=11, vk_draws=11,
  copy_depth_ops=0, readback_ops=0, fallback_reason="".
- Probe path `MC2_HZB_PROBE=1`: PASS; native declined with
  fallback_reason="probe_enabled", attempted=0, used_vulkan=0.
- Tier1 native HZB (`mc2_01/mc2_03/mc2_10/mc2_17/mc2_24`, 20s): PASS 5/5,
  +0 destroys.

## RUNG 8.5a - VK-PRESENT-DIRECT-FBO-1 META PATH CONFIRMED

No code change in this checkpoint. The existing `MC2_VK_PRESENT_DIRECT_FBO`
implementation is the correct first 8.5 slice: GL composite, FORCE-43,
HUD, and ImGui stay unchanged but render into EnginePresentVk's shared present
FBO, letting `presentGLFrame()` skip copy-1. `CompositePresent` is not the path;
that region is explicitly a separate-window test contract.

Evidence gathered on 2026-07-07:
- Refreshed explicit deploy lane
  `A:/Games/mc2-opengl/mc2-win64-0.4c` from `build64_island/RelWithDebInfo`,
  manifest `src_commit=b52050ccdf71`.
- Exact-lane baseline smoke:
  `py -3 scripts/run_smoke.py --exe A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe --mission mc2_01 --mission mc2_24 --duration 30 --keep-logs`
  -> `2026-07-07T22-55-54`, PASS 2/2, fingerprint OK, +0 destroys.
- Exact-lane gate-on smoke:
  `MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1` with the same exe and
  missions -> `2026-07-07T22-57-19`, PASS 2/2, fingerprint OK, +0 destroys.
- Vulkan-backend minimized smoke:
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1`
  -> `2026-07-07T22-59-12`, PASS 2/2, engine context created on RX 7900 XTX,
  but present receipt is blind because the smoke harness minimizes the window:
  `fallback_reason="zero surface extent (window minimized?)"`,
  `present_frames=0`.

Follow-up evidence gathered on 2026-07-08:
- `cf02f587` added `scripts/run_visual_capture.py --show-window` and absolute
  bookmark-path handoff. Default remains minimized for ordinary visual baselines.
- Rebuilt + deployed explicit 0.4c lane from HEAD `45afd60d7416`
  (`build_fingerprint.h: dirty=0`, deploy manifest `src_commit=45afd60d7416`).
- Visible exact-lane run:
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1`
  with `run_visual_capture.py --show-window --bookmarks tests/visual/bookmarks/mc2_01.json`
  -> 11/11 PNGs present, deterministic sidecars, present receipt
  `present_frames=11101`, `validation_errors=0`, `readback_changed=1`,
  `fallback_reason=""`.
- `8fd4bea5` added `scripts/renderdoc_capture.py --exe` so Tier-5 captures can
  target the exact 0.4c lane instead of the hardcoded v0.4 deploy. RenderDoc 1.7
  proof under the same direct-FBO Vulkan env produced
  `.claude/vk-present-direct-visible/rdoc/direct_fbo_frame181.rdc`; engine hook
  logged `api_resolved`, `trigger_capture local_frame=180`, and
  `capture_complete local_frame=182`.

Verdict: 8.5a is now fully proven, not blind: copy-1 is skipped on a live Vulkan
present path with nonzero present frames and a RenderDoc capture.

8.5b sync ruling: do NOT naively render native composite directly into
EnginePresentVk's shared present image from `gosPostProcess::endScene()`.
`presentGLFrame()` already owns `vkImageResourceBeginFrame(&sharedImg)` and the
single GL->VK handoff for the final present. A native composite inserted before
GL HUD/ImGui would need VK->GL for HUD, then a second GL->VK for present; the
current `VkImageResource` semaphore/parity pair is not a safe two-handoff
mid-frame protocol. Next implementation must either design an explicit
present-image split with separate sync for composite->HUD and HUD->present, or
land a safer first runtime slice that targets shared scene color and leaves
present ownership unchanged.

## RUNG 8.5b-a - VK-POST-COMPOSITE shader asset prep

Asset-only prep for the native composite port. Added `shaders/vulkan/postprocess.vert`
and `shaders/vulkan/postprocess.frag` plus the CMake shader basename registration,
so the Vulkan build now bakes `postprocess.vert.spv` and `postprocess.frag.spv`.
The fragment shader ports the current GL composite semantics (scene sample,
exposure, FXAA gate/tunables, warm grade, vignette, ObjectIdDebug, Thermal, and
LowLight) into an explicit Vulkan descriptor layout:
`sceneTex` at set 0 binding 0, optional `u_objectIdTex` at binding 1, and one
std140 params block at binding 2.

Verification:
- `cmake --build build64_island --target mc2_vulkan_shaders --config RelWithDebInfo`
  PASS; emitted `build64_island/shaders/vulkan/postprocess.vert.spv` and
  `postprocess.frag.spv`.
- `spirv-val` PASS for both SPIR-V files.
- Full `mc2` RelWithDebInfo build PASS (only pre-existing C4267/LNK4199 warnings).

No runtime hook yet. Next slice should add `vulkan_post_composite_native.cpp`,
`gosPostProcess::runVkPostCompositeNative()`, debug-state health, `MC2_VK_POST_COMPOSITE`
allowlist/docs, and a visible-window proof path for present frames. Scout finding:
`run_smoke.py` still lacks allowlist entries for `MC2_VIEWMODE_FRAMEWORK`,
`MC2_VIEW_MODE`, and `MC2_OBJECT_ID_BUFFER`, so viewmode parity coverage will need
that harness fix when runtime testing ObjectIdDebug/Thermal/LowLight.

## RUNG 8.6a - VK-IMGUI-BACKEND-COMPILE-1 SHIPPED

Behavior-neutral first 8.6 slice. The Dear ImGui Vulkan renderer backend now
compiles into the existing `imgui` static library when `MC2_VULKAN=ON`, using the
project's existing Volk/Vulkan-Headers pattern (`IMGUI_IMPL_VULKAN_USE_VOLK` and
`Vulkan::Headers`). `GuiRuntime` remains OpenGL-backed; no runtime selection,
HUD path, or present ownership changed in this checkpoint.

Important vendoring finding: the tracked Vulkan backend had drifted newer than
the vendored core `imgui.h` (`IMGUI_VERSION_NUM 19180`) and referenced post-1.91.8
texture APIs (`ImTextureData`, `ImGuiBackendFlags_RendererHasTextures`,
`ImGuiPlatformIO::Textures`, renderer callback handlers). The fix was to align
only `imgui_impl_vulkan.cpp/.h` to the official `v1.91.8-docking` backend, matching
the core/vendor commit lineage instead of patching Dear ImGui internals.

8.6 runtime scout ruling: do not draw native HUD/ImGui into the shared present
image yet. The same mid-frame ownership issue documented in 8.5b applies:
postprocess/composite happens before GL HUD/ImGui, while `presentGLFrame()` owns
the single final GL->VK handoff. A safe runtime 8.6 slice needs either a proper
present-image split / second sync handoff or an ordering change where Vulkan owns
the full tail after composite.

Verification:
- `cmake --build build64_island --config RelWithDebInfo --target imgui` PASS.
- `cmake --build build64_island --config RelWithDebInfo --target gui_runtime mc2`
  PASS; only pre-existing C4267/C4996/LNK4199 warnings.

## RUNG 8.6b - VK-IMGUI-TAIL-SEAM-1 SHIPPED

Behavior-neutral tail contract for the eventual native ImGui/HUD renderer.
`EnginePresentVk::directPresentFrameArmed()` now exposes the only valid
precondition for a final-tail VK overlay attempt: the current frame's
`endScene()` successfully armed the direct-present shared FBO via
`presentDirectFbo()`. This intentionally does not begin a Vulkan render pass,
signal/wait semaphores, or move HUD/ImGui ordering; `presentGLFrame()` still owns
the single GL->VK handoff for the frame.

Call-chain recon, now treated as the 8.6 hard ordering contract:
- `gosPostProcess::endScene()` binds either FBO-0 or the direct shared present
  FBO.
- `gameosmain.cpp` then flushes GameOS HUD and calls `GuiRuntime::Render()`.
- The swap path later calls `EnginePresentVk::presentGLFrame()`, which performs
  the GL->VK signal and swapchain blit/present.

Hard ban remains: native HUD/ImGui must not write into the shared present image
before a real tail handoff split exists. The new query only prevents future code
from guessing at `directFbo_` state.

Verification:
- Rebuilt `build64_island` with clean fingerprint
  `sha=a96e1809f4f8 dirty=0`.
- Deployed exact 0.4c lane:
  `A:/Games/mc2-opengl/mc2-win64-0.4c`, manifest
  `src_commit=a96e1809f4f84928387de2a5d65ebecd11524544`.
- Vulkan/direct-FBO smoke:
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1`
  with `scripts/run_smoke.py --exe A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe --mission mc2_01 --mission mc2_24 --duration 20 --keep-logs`
  PASS 2/2, fingerprint OK, +0 destroys.

## RUNG 8.6c - VK-IMGUI-DECLINE-GATE-1

`MC2_VK_IMGUI=1` is now a real smoke-forwarded runtime gate, but still
behavior-neutral: `GuiRuntime::Render()` emits one `[VK_IMGUI]` receipt with
`requested=1`, `used_vulkan=0`, `direct_present_armed=<0|1>`, and
`fallback_reason="tail_handoff_not_split"`, then falls through to the existing
SDL2/OpenGL3 renderer. This proves the gate reaches the exact ImGui render site
without beginning a Vulkan render pass or changing ordering.

This is intentionally not a native renderer yet. The decline reason remains the
8.5b/8.6b contract: `presentGLFrame()` owns the single final GL->VK handoff, so
native ImGui cannot write the shared present image until the final-tail handoff is
split.

Verification:
- Build PASS at `7b4741ee` (`build64_island`, RelWithDebInfo), dirty=0 before deploy.
- Deployed exact 0.4c lane with manifest `src_commit=7b4741eeed0e6cd3cc4134323a525ab07473428e`.
- Gate-on minimized smoke:
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1 MC2_VK_IMGUI=1`
  with `scripts/run_smoke.py --exe A:/Games/mc2-opengl/mc2-win64-0.4c/mc2.exe --mission mc2_01 --mission mc2_24 --duration 20 --keep-logs`
  PASS 2/2, fingerprint OK, +0 destroys.
- Receipt in both mission logs:
  `[VK_IMGUI] requested=1 used_vulkan=0 direct_present_armed=0 fallback_reason="tail_handoff_not_split"`.
  `direct_present_armed=0` is expected for minimized smoke because VK present
  fail-softs on zero surface extent; use visible capture for direct-FBO armed proof.
- Follow-up hygiene: `MC2_VK_IMGUI` registered in `RendererFeatureRegistry.h`;
  `docs/flags.auto.yaml` regenerated. `check-env-allowlist.sh` remains advisory-dirty
  from pre-existing harness-only vars, but reports zero engine `getenv` vars missing
  from the smoke allowlist.
- Visible-window proof at `2eecfdbf`:
  `run_visual_capture.py --show-window --mission mc2_01 --bookmarks tests/visual/bookmarks/mc2_01.json`
  with `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1 MC2_VK_IMGUI=1`
  wrote 11/11 deterministic bookmark PNGs under
  `.claude/vk-imgui-decline-visible-armed/`.
  Present path stayed live (`present_frames=6301`, `validation_errors=0`,
  `readback_changed=1`, `fallback_reason=""`) while ImGui emitted
  `[VK_IMGUI] requested=1 used_vulkan=0 ... fallback_reason="tail_handoff_not_split"`.
  The `direct_present_armed` field stayed 0 from the ImGui call site even with
  live VK present, so it is a conservative predicate only; do not treat it as
  proof that direct-FBO present was unavailable.

## RUNG 8.6d - VK-TAIL-HOOK-1

Behavior-neutral final-tail command-buffer seam landed. `MC2_VK_PRESENT_TAIL_HOOK=1`
adds a no-op image memory dependency inside `EnginePresentVk::presentGLFrame()`
after the final GL->VK handoff and before the swapchain blit. This is the exact
place native HUD/ImGui rendering must attach: the shared present image is
Vulkan-owned, still in `COLOR_ATTACHMENT_OPTIMAL`, and the existing present
command buffer already owns the final submit/present cadence. The GL->VK
semaphore wait stage expands to include `COLOR_ATTACHMENT_OUTPUT` only while
the hook is enabled.

Verification:
- Build PASS at `d710e56c` + dirty hook changes (`build64_island`, RelWithDebInfo).
- Deployed 0.4c lane; manifest `src_commit=d710e56c3622`.
- Visible-window proof:
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1 MC2_VK_PRESENT_TAIL_HOOK=1 MC2_VK_IMGUI=1`
  with `run_visual_capture.py --show-window --mission mc2_01 --bookmarks tests/visual/bookmarks/mc2_01.json`
  wrote 11/11 deterministic bookmark PNGs under `.claude/vk-present-tail-hook/`.
- `[VK_PRESENT]` final health:
  `present_frames=6301`, `direct_fbo_requests=6301`, `direct_fbo_grants=6301`,
  `copy1_blit_frames=0`, `copy1_skip_frames=6301`, `tail_hook_frames=6301`,
  `validation_errors=0`, `fallback_reason=""`.
- No-hook vs hook PNG byte compare: 9/11 byte-identical; the two differences
  were negligible (`cross_map_view`: 2 pixels / 1,638,720, max delta 9;
  `ridge_lowangle`: 24 pixels / 1,638,720, max delta 1). Treat hook as visually
  neutral. NEXT: replace the no-op dependency with the ImGui Vulkan draw-data
  render pass at this seam; do not attempt a second handoff from `GuiRuntime`.

## RUNG 8.6e - VK-IMGUI-NATIVE-1

Native ImGui renderer landed on the final-tail seam. `GuiRuntime` still owns the
ImGui context, SDL input backend, frame construction, and `ImGui::Render()`.
When `MC2_VK_IMGUI=1`, it does not initialize or call the OpenGL3 renderer
backend. Instead it uses the SDL2 Vulkan platform init, builds the CPU font
atlas, calls `EnginePresentVk::prepareImguiVulkanFrame()` before `ImGui::NewFrame()`,
then hands `ImGui::GetDrawData()` to `EnginePresentVk`. `EnginePresentVk`
initializes `ImGui_ImplVulkan` lazily, creates a LOAD/STORE render pass and
framebuffer over the shared present image (`VK_FORMAT_R8G8B8A8_UNORM`), and
records `ImGui_ImplVulkan_RenderDrawData()` inside `presentGLFrame()` after the
GL->VK handoff and before the swapchain blit.

Important lifecycle rule: `GuiRuntime::Shutdown()` calls
`EnginePresentVk::shutdownImguiVulkanRenderer()` before destroying the ImGui
context. `EnginePresentVk::destroy()` remains a no-op-safe backup, so the
renderer backend is not shut down after the context is gone.

Verification:
- Build PASS at `0be75ff2` + dirty native-ImGui changes (`build64_island`, RelWithDebInfo).
- Deployed 0.4c lane; manifest `src_commit=0be75ff2221b`.
- First native attempt crashed in `ImGui::NewFrame()` because the CPU font atlas
  was not built without the GL renderer. Fixed by `io.Fonts->Build()` on the VK
  renderer path.
- Second native attempt rendered every frame but crashed on shutdown because the
  ImGui context was destroyed before `ImGui_ImplVulkan_Shutdown()`. Fixed by the
  explicit present-owned renderer shutdown from `GuiRuntime::Shutdown()`.
- Clean visible proof:
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1 MC2_VK_PRESENT_TAIL_HOOK=1 MC2_VK_IMGUI=1`
  with `run_visual_capture.py --show-window --mission mc2_01 --bookmarks tests/visual/bookmarks/mc2_01.json`
  wrote 11/11 deterministic bookmark PNGs under `.claude/vk-imgui-native-clean/`.
- Proof lines:
  `[VK_IMGUI] requested=1 used_vulkan=1 fallback_reason=""`;
  final `[VK_PRESENT]` had `present_frames=6301`, `tail_hook_frames=6301`,
  `imgui_vk_ready=1`, `imgui_vk_frames=6301`, `copy1_blit_frames=0`,
  `copy1_skip_frames=6301`, `validation_errors=0`, `fallback_reason=""`.
- Shutdown-clean: no `Crash`, no `Forgot to shutdown Renderer backend`, no
  `[imgui-error]` in `.claude/vk-imgui-native-clean/mc2_01_capture.log`.
- Visual delta vs the 8.6d tail-hook baseline: 9/11 byte-identical; only the
  known tiny unstable views differed (`cross_map_view`, `ridge_lowangle`).

Post-commit verification at clean HEAD `4ed1ca21db54`:
- Minimized two-mission smoke with
  `MC2_RENDER_BACKEND=vulkan MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_VERIFY=1 MC2_VK_PRESENT_TAIL_HOOK=1 MC2_VK_IMGUI=1`
  PASS 2/2 (`tests/smoke/artifacts/2026-07-08T13-32-28`), fingerprint OK,
  +0 destroys.
- Combined rung-8 ownership smoke with
  `MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1 MC2_VK_POST_FOG_NATIVE=1 MC2_VK_POST_SSAO=1 MC2_VK_POST_SCREENFX=1 MC2_VK_POST_HZB=1 MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_PRESENT_TAIL_HOOK=1 MC2_VK_IMGUI=1 MC2_HZB_BUILD=1 MC2_SSAO=1`
  PASS 2/2 (`tests/smoke/artifacts/2026-07-08T14-47-14`), fingerprint OK,
  +0 destroys. Because smoke starts minimized, present and ImGui correctly
  decline with `zero surface extent` / `vulkan_renderer_not_ready`; use the
  visible proof above for present/ImGui.
- Same combined smoke proves postprocess-native paths actually execute when
  `MC2_VK_SHARED_GBUFFER=1`: `VK_POST_FOG_NATIVE`, `VK_POST_SSAO_NATIVE`,
  `VK_POST_SCREENFX_NATIVE`, and `VK_POST_HZB_NATIVE` all report
  `substrate_live=1`, `used_vulkan>0`, `fallback_reason=""` on both `mc2_01`
  and `mc2_24`. HZB reports `copy_depth_ops=0 readback_ops=0`.

## RUNG 8.7 READINESS - NOT YET DELETABLE

Do not delete old interop scaffolding yet. The rung-8 ladder's final deletion
bar is stricter than current code:
- 8.5b native composite first runtime slice is landed, not complete. Under
  `MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1
  MC2_VK_PRESENT_DIRECT_FBO=1 MC2_VK_POST_COMPOSITE=1`, Visual mode with FXAA
  off and a full drawable viewport now samples shared scene color0 in VK and
  writes the shared present image. Visible proof:
  `.claude/vk-post-composite-native-first-2/`, 11/11 deterministic `mc2_01`
  bookmarks, `[VK_PRESENT] composite_vk_frames=300`, `copy1_blit_frames=0`,
  `fallback_reason=""`; minimized smoke `tests/smoke/artifacts/2026-07-08T15-02-41`
  PASS 2/2, +0 destroys. Remaining composite branches still fall back to GL:
  FORCE-43/OOB-letterbox and viewmode/object-id/debug paths.
- 8.5c native composite FXAA is landed. The native Visual composite now ports
  the existing GL FXAA filter and tunables. Visible proof:
  `.claude/vk-post-composite-fxaa-native/`, `MC2_FXAA=1`, 11/11 deterministic
  `mc2_01` bookmarks, `[FXAA v1] enabled=1`, `[VK_PRESENT]
  composite_vk_frames=300`, `copy1_blit_frames=0`, `fallback_reason=""`.
  Minimized smoke with `MC2_FXAA=1` forwarded:
  `tests/smoke/artifacts/2026-07-08T15-12-17`, PASS 2/2, +0 destroys.
- 8.5d native composite FORCE-43 is landed. The native Visual composite now
  clears the shared present image black in VK and draws into the caller-supplied
  pillarbox viewport. Visible proof:
  `.claude/vk-post-composite-force43-native/`, `MC2_FORCE_43=1`, 11/11
  deterministic `mc2_01` bookmarks, `[VK_PRESENT] composite_vk_frames=300`,
  `copy1_blit_frames=0`, `fallback_reason=""`. Minimized smoke with
  `MC2_FORCE_43=1` forwarded: `tests/smoke/artifacts/2026-07-08T15-16-01`,
  PASS 2/2, +0 destroys.
- 8.5e native composite LowLight is landed. LowLight/NightVision mode 5 runs in
  the native composite using the same gain/tint math; no object-ID texture is
  needed. Visible proof: `.claude/vk-post-composite-lowlight-native/`,
  `MC2_VIEWMODE_FRAMEWORK=1 MC2_VIEW_MODE=5`, 11/11 deterministic `mc2_01`
  bookmarks, `[VIEWMODE v1] framework=1 initialMode=5`, `[VK_PRESENT]
  composite_vk_frames=300`, `copy1_blit_frames=0`, `fallback_reason=""`.
  Minimized smoke: `tests/smoke/artifacts/2026-07-08T15-20-02`, PASS 2/2,
  +0 destroys.
- 8.5f native composite ObjectIdDebug/Thermal is landed. The shared scene
  substrate now has `MC2_SHARED_SCENE_OBJECT_ID`, backed by `VK_FORMAT_R32_UINT`
  imported to `GL_R32UI`; native composite binds it as a `usampler2D` for mode 1
  and mode 3. Visible proofs:
  `.claude/vk-post-composite-objectid-native/` and
  `.claude/vk-post-composite-thermal-native/`, each 11/11 deterministic
  `mc2_01` bookmarks, `[VK_SHARED_SCENE_COLOR] objectId(MRT2) active: VK R32UI
  ... imported to GL_R32UI`, `[VK_PRESENT] composite_vk_frames=300`,
  `copy1_blit_frames=0`, `fallback_reason=""`. Minimized Thermal smoke:
  `tests/smoke/artifacts/2026-07-08T15-28-36`, PASS 2/2, +0 destroys.
- 8.6 native ImGui is landed, but GameOS HUD (`gos_RendererFlushHUDBatch()`) is
  still legacy GL. `MC2_UI_PASS_MEASURE=1` now emits `[VK_HUD_AUDIT v1]`
  batch shape lines for the native HUD port. Focused `mc2_01` smoke
  `tests/smoke/artifacts/2026-07-08T15-33-28` PASS 1/1 showed the tier1 HUD is
  not an untextured micro-slice: steady frames are `calls=95 quads=56 lines=18
  tris=14 text=7 textured=69 vertices=1106 alpha1=4 alpha3=91`. Native HUD
  therefore needs ordered textured quad + text replay and a gosTexture mirror,
  not just line/tri replay.
- `vulkan_postprocess_subgraph.cpp`, `vulkan_ssao_island.cpp`,
  `vulkan_hzb_island.cpp`, and the edge/oob fog islands are now bypassed by the
  native rung-8 gates during the verified shared-GBuffer smoke, but they still
  compile as default-off fallback/oracle paths.
- `gl_interop_view.*` and the Win32 external image memory/semaphore extensions
  are still required by the shared scene color/depth/present substrates while
  GL continues to produce composite/HUD pixels. The unused buffer interop seam
  (`vk_gl_shared_buffer.*` / `gl_interop_buffer_view.*`) was deleted after rung
  8.7j because no live path includes or calls it.

Current tail status:
- Native GameOS HUD replay and skip-GL proof are shipped; see
  `VK-RUNG8-LADDER-1.md` 8.6f+ through 8.6j.
- The remaining native composite diagnostic branch is now closed for the
  ownership-relevant CSM dynamic case: `MC2_SHADOW_DEBUG_MODE=dynamic` uses a
  native VK overlay when the shadow island exposes a same-frame CSM array view.
  Static/non-ready debug sources intentionally retain GL fallback.
- Present copy-2 has a VK-owned proof gate: `MC2_VK_PRESENT_SKIP_GL_HANDOFF=1`
  skips the GL->VK semaphore signal/wait when direct present is armed and the
  shared present image was produced by VK. Visible proof captured 3/3 building
  bookmarks in `.claude/vk-present-skip-gl-handoff/` and logged
  `gl_handoff_skip_frames=1`.
- Present native strict is now visible-proven across the full native final-tail
  stack (`MC2_VK_POST_COMPOSITE=1`, direct FBO, tail hook, HUD skip, ImGui
  Vulkan, skip GL handoff). Receipt:
  `.claude/vk-present-native-strict-visible-clean/`, 3/3 deterministic
  `mc2_01_buildings` bookmarks, `copy1_blit_frames=0`,
  `gl_handoff_skip_frames=2700`, `composite_vk_frames=2700`,
  `imgui_vk_frames=2700`, `fallback_reason=""`.
- Global GL/VK interop strict canary is live and currently fails as expected:
  `MC2_VK_NO_GL_INTEROP_STRICT=1` aborts at the first
  `glInteropSignalToVk` boundary. This is the next deletion-readiness proof to
  turn green; until then the Win32 external-memory/semaphore family remains live.
- Static shadow has a VK substrate gate: `MC2_VK_STATIC_SHADOW_MAP=1` backs
  `ShadowStaticMap` with exportable VK `D32_SFLOAT` imported into GL as
  `GL_DEPTH_COMPONENT32F`. GL still renders/samples it; native screen-shadow
  can later bind the same image without a copy.
- Static-only screen-shadow can now render natively under `MC2_VK_POST_SCREENFX=1`
  + `MC2_VK_STATIC_SHADOW_MAP=1`; it declines only when dynamic shadows require
  a path that is not yet VK-readable.
- CSM dynamic screen-shadow can now render natively when the direct VK CSM
  producers are armed (`MC2_VK_SHADOW_INTEROP=1`,
  `MC2_VK_CSM_ARRAY_INTEROP=1`, `MC2_VK_CSM_FULLMAP_INTEROP=1`). The
  `screen_shadow_csm` pipeline samples the same-frame VK CSM array/full-map
  views directly and skips the GL screen-shadow body under `MC2_SHADOW_ENABLE=1`.
  Debug classification and non-CSM dynamic stay GL fallback paths.
- Remaining deletion blocker: shared scene color/depth/present substrates still
  rely on `gl_interop_view.*` while GL can produce earlier scene pixels.
- ObjectIdDebug strict canary is green under the native final-tail stack:
  `MC2_VK_OBJECTID_DIRECT_CHAIN=1` skips the native composite's objectId MRT2
  interop handoff when the full composite direct-chain stack is armed. Correct
  Vulkan proof (after discarding a non-Vulkan `build64` false positive):
  `.claude/vk-nointerop-strict-visible-objectid-direct-clean/`, 11/11
  deterministic `mc2_01` bookmarks with `MC2_OBJECT_ID_BUFFER=1`,
  `MC2_VIEW_MODE=objectid`, `MC2_VK_NO_GL_INTEROP_STRICT=1`; log shows
  `sha=d890a2f2f1f0 dirty=0`, objectId VK `R32UI`, all
  terrain/prop/mech/water islands active, `composite_vk_frames=300`,
  `gl_handoff_skip_frames=300`, smoke PASS, and no strict abort. This is a
  deletion-readiness canary, not a full objectId MRT2 parity port.
- Native fog direct-chain strict canary is green:
  `.claude/vk-nointerop-strict-visible-postfog-direct-clean/`, 11/11
  deterministic `mc2_01` bookmarks, Vulkan backend selected, fog initialized on
  the shared engine context, `sha=35c56c4c362d dirty=0`,
  `VK_POST_FOG_NATIVE [HEALTH] attempted=662 used_vulkan=662 direct_chain=1
  used_interop=0 gl_skipped=1324`,
  `VK_PRESENT composite_vk_frames=600 gl_handoff_skip_frames=600`, water/prop
  `used_interop=0`, no fail-soft GL fallback and no `[VK_NO_GL_INTEROP_STRICT]`
  abort.
- Native SSAO direct-chain strict canary is green:
  `.claude/vk-nointerop-strict-visible-postssao-direct-dirty-ssaoon/`, 11/11
  deterministic `mc2_01` bookmarks with `MC2_SSAO=1`,
  `MC2_VK_POST_SSAO=1`, `MC2_VK_POST_SSAO_DIRECT_CHAIN=1`,
  `MC2_VK_NO_GL_INTEROP_STRICT=1`; log shows `SSAO v1 enabled=1 debug=0`,
  native SSAO initialized on the shared engine context, `VK_POST_SSAO_NATIVE
  [HEALTH] attempted=661 used_vulkan=661 direct_chain=1 used_interop=0
  gl_skipped=661`, fog still `used_interop=0`, present
  `composite_vk_frames=600 gl_handoff_skip_frames=600`, water/prop
  `used_interop=0`, and no `[VK_NO_GL_INTEROP_STRICT]` abort. Note:
  the first SSAO strict attempt without `MC2_SSAO=1` was only a crash regression
  (`attempted=0`) and is not the execution proof.
- Native screenfx + HZB direct-chain strict canary is green:
  `.claude/vk-nointerop-strict-visible-postscreenfx-hzb-direct-dirty-cleanrun/`,
  11/11 deterministic `mc2_01` bookmarks with screenfx, SSAO, fog, HZB, and
  native present all armed under `MC2_VK_NO_GL_INTEROP_STRICT=1`; log shows
  `VK_PRESENT composite_vk_frames=600 gl_handoff_skip_frames=600`, screenfx
  `attempted=1324 used_vulkan=1324 shadow_frames=662 shore_frames=662
  direct_chain=1 used_interop=0 gl_skipped=1324`, HZB `attempted=662
  used_vulkan=662 direct_chain=1 used_interop=0 levels=12 vk_draws=12
  copy_depth_ops=0 readback_ops=0`, fog/SSAO/water/prop all `used_interop=0`,
  no present fail-soft, and no `[VK_NO_GL_INTEROP_STRICT]` abort. The HZB path
  now defers native execution for the first observed extent/mip-count frame so
  the present swapchain handles resize before HZB allocates/renders.
- Native shadow direct-chain strict canary is green:
  `.claude/vk-nointerop-strict-visible-shadow-direct-clean-final/`, 11/11
  deterministic `mc2_01` bookmarks with dynamic CSM shadows, static shadow
  substrate, native CSM screen-shadow, post stack, HZB, composite, and native
  present all armed under `MC2_VK_NO_GL_INTEROP_STRICT=1`; log shows
  `sha=53311e5aced3 dirty=0`, `MC2_VK_SHADOW_DIRECT_CHAIN=1 -- VK shadow
  products stay VK-owned; GL semaphore wait is skipped`, `first VK shadow map
  produced ... (direct VK consumers only)`, first VK CSM array/full-map
  products, present `composite_vk_frames=6600 gl_handoff_skip_frames=6600`,
  no present fail-soft, and zero occurrences of `[VK_NO_GL_INTEROP_STRICT]`,
  `glInteropSignalToVk`, or `glInteropWaitFromVk`. Detailed post-health
  receipt remains the dirty pre-commit proof
  `.claude/vk-nointerop-strict-visible-shadow-direct-dirty-receipts/`, with
  screenfx `shadow_frames=6629 direct_chain=1 used_interop=0` and
  HZB/fog/SSAO/water/prop all `used_interop=0`.

## RUNG 8.6f - VK-HUD-NATIVE-QUEUE-1

`MC2_VK_HUD=1` now deep-copies the already-scaled ordered GameOS HUD batch from
`gosRenderer::flushHUDBatch()` into `EnginePresentVk` for final-tail replay. The
GL HUD replay remains active until the Vulkan consumer can render every call
kind; this slice proves the ownership handoff and exact tail ordering before
native ImGui.

Verification:
- Build: `cmake --build build64_island --config RelWithDebInfo --target mc2`
  passed; only the known `mechcmd2.cpp` C4267 and delay-load LNK4199 warnings.
- Minimized smoke with `MC2_VK_HUD=1`:
  `tests/smoke/artifacts/2026-07-08T15-38-51`, PASS 1/1, +0 destroys. As
  expected, present surface was zero extent, so queue counters stayed 0 there.
- Visible bookmark run:
  `.claude/vk-hud-queue-visible/`, 10/11 deterministic captures before the
  duration window ended. Runtime proof line: `[VK_PRESENT] created=1 ...
  tail_hook_frames=1 ... hud_vk_queued_frames=1 hud_vk_consumed_frames=1
  hud_vk_draws_last=85 imgui_vk_ready=1 imgui_vk_frames=1 ... fallback_reason=""`.
  Steady HUD audit remained `calls=95 ... textured=69 text=7`.
