# VULKAN-NEXT-SLICES — executable rung specs for the L6→L8 ladder (opus handoff)

Companion to `.claude/VULKAN-BACKEND-L6-LADDER-1.md` (the ratified ladder — read it first;
this doc turns each REMAINING rung into an executable spec). Worktree
`A:/Games/mc2-vulkan-backend`, branch `claude/vulkan-backend-l6`. Written 2026-07-02 after
wave-3 (ladder §5a amendment `d67eba92`, L6.1 backendHandle rename `34ade1ee`, SSAO
fixture GREEN — but the SSAO backend-compare FAILED once with a nondeterministic
corruption → N0 below; L6.2 parity remains OPEN).

**Execute top-to-bottom. Every slice: default-OFF / shipping-`build64`-byte-identical,
fail-soft, own commit, proof run BEFORE the commit message claims it.** If a proof fails,
STOP and report — do not weaken the oracle to make it pass (precedent: the frame-graph
reorder candidate was REJECTED on parity failure and the gate stayed OFF forever).

**⚠ ORDER OF WORK: ~~N0 (nondeterminism) is BLOCKING~~ — N0 RESOLVED 2026-07-02
(supervisor tolerance-floor ruling + 5/5 GREEN compare; see N0 section).
L6.4/L6.5/L6.6 are UNBLOCKED.** N1 shipped `d7fbd72f`.

---

## 0. Standing landmines (read before EVERY slice — these have each burned a session)

**LM1 — MVP row-major lockstep (the "2b bug").** The GL path uploads mat4s ROW-MAJOR with
`GL_FALSE`. Every Vulkan params UBO holding a mat4 MUST declare `layout(std140, row_major)`
and the CPU POD mirrors byte offsets with a `static_assert(sizeof(...) == N)`. Every
synthetic fixture MUST use a SHEARED viewProj whose exact inverse is also exact in binary
fractions (see `ssao_cpu_ref` / edge-fog fixture in
`GameOS/gameos/vulkan_backend_skeleton.cpp`) — a symmetric matrix CANNOT catch a transpose
bug; the shear is the detector. Do not "simplify" a fixture to identity/diagonal matrices.

**LM2 — ComputeDispatch slot-safety (terrain/water regions).**
`WaterStream::ComputeDispatchAndBindThinRecords()` (`gameos_graphics.cpp:3362`) runs
`glUseProgram(compute)` MID-FRAME and leaves binding state (slot 6 thin records) that later
draws depend on. Any region extraction near terrain/water must NOT float draws across that
dispatch, must not re-apply cached program state after it (`applyPipeline` invalidation
contract), and the Vulkan port must reproduce the dispatch→draw barrier (barrier plan B-*)
explicitly. Grep `ComputeDispatchAndBindThinRecords` and read the comments at
`gameos_graphics.cpp:10540` before touching L7.3/L7.4.

**LM3 — binding conventions are PER-FAMILY (ladder §5a is normative).** Postprocess family:
samplers at `set=0, binding=0..N-1` in GL texture-unit slot order, ONE params UBO at
`binding=N`, zero push constants, zero vertex input (`gl_VertexIndex` fullscreen triangle,
`location=0 vec2 TexCoord` only). `scripts/check-vulkan-bindings.py` enforces; extend its
expectations per region. Do NOT stretch this table over geometry regions (Shadow/Terrain/
Mech get their own table at the L7.2 recon). The already-baked `shaders/spv/*.spv` (GL
consumer artifacts) are NOT Vulkan-consumable — glslang free-numbered their samplers (both
ssao samplers decorate to binding 0, verified spirv-dis) and they target `opengl`, not
`vulkan1.1`. Never wire a Vulkan pipeline to `shaders/spv/`; Vulkan consumes
`<build>/shaders/vulkan/*.spv` compiled from `shaders/vulkan/*` (or, post-L6.5, dual-target
bakes from the single source).

**LM4 — R8_UINT stencil bridge pattern (fog + any stencil-reading pass).** GL fog frags
read scene stencil via `usampler2D stencilTex` (a `GL_STENCIL_INDEX` texture view,
SKYBOX-FOG-EXCLUDE-1). Vulkan islands bridge GL→VK by CPU readback
(`glGetTexImage(GL_DEPTH_COMPONENT, GL_FLOAT)` → staging → `VK_FORMAT_D32_SFLOAT`, see
`vulkan_postprocess_subgraph.cpp:905`). The stencil equivalent is
`glGetTexImage(GL_STENCIL_INDEX, GL_UNSIGNED_BYTE)` → staging → **`VK_FORMAT_R8_UINT`**
image sampled as `usampler2D` in the ported frag. NOT yet implemented: the vk fog shaders
carry NO stencilTex → `MC2_SKYBOX_FOG_EXCLUDE=1` + `MC2_POSTPROCESS_BACKEND=vk` silently
diverges. Slice N1 below guards it; the bridge itself is only needed if sky-exclude-under-vk
becomes a requirement.

**LM5 — spv deploy lockstep.** Every Vulkan region loads `shaders/vulkan/*.spv` relative to
CWD (`MC2_VULKAN_SPV_DIR` overrides). `deploy_payload.py` ships
`<build64-root>/shaders/vulkan/*.spv` (fix `689623a9`) — but ONLY if you deploy with
`--build64-root build64_island`. Deploying the island exe without the spv payload =
`vk_device_init_failed` → every backend-compare exits 5 INCONCLUSIVE. Canonical island
deploy (lane `mc2-win64-v0.4c` — the ISLAND lane; v0.4 stays GL):

```powershell
py -3 scripts/deploy_payload.py A:/Games/mc2-opengl/mc2-win64-v0.4c --source-root . `
  --build-dir build64_island/RelWithDebInfo --build64-root build64_island --exe-name mc2.exe
```

After deploy, verify `cmp build64_island/RelWithDebInfo/mc2.exe <lane>/mc2.exe` (the
stale-exe gotcha is real — a mismatched size means you deployed the wrong build dir).

**LM6 — exit-5 INCONCLUSIVE is not PASS.** `run_golden_parity.py` backend-compare reads the
per-region health block (`REGION_SPEC` maps region → block name + extra env). FallbackGL →
exit 5. A compare that "passed" while the region fell back proved NOTHING. Always quote the
RESULT line + `region_impl` in the commit message.

**LM8 — deploy-lane contention (burned this session, cost ~2h of voided proofs).**
Concurrent agents lease deploy folders LRU-style and REDEPLOY THEIR OWN EXE onto them
mid-session. On 2026-07-02 a foreign agent swapped `mc2-win64-v0.4c/mc2.exe` at least
twice (02:1x-02:2x and 02:43, plus 02:48) — every capture I ran in that window silently
executed the foreign non-island exe and produced convincing-looking-but-void "parity"
results. Protocol, non-negotiable: (1) direct `run_visual_capture.py` calls BYPASS the
lease system — before any proof run, `cmp` the lane exe against your build output; (2)
verify exe identity IN THE RUN'S OWN OUTPUT: an island exe prints `[VK_EDGE_FOG]/
[VK_OOB_FOG]/[VK_PP_SUBGRAPH]/[VK_SSAO_ISLAND]` health lines at shutdown — a vk-parity
capture whose log lacks them DID NOT RUN YOUR EXE; (3) `cmp` the lane exe again AFTER the
run (bracket the window); (4) `tasklist`/`Get-Process mc2 | select Path` before deploying —
never stomp a lane with a live foreign process; (5) state dumps in the lane's
`debug_state/` are written by WHOEVER ran last with `MC2_DEBUG_STATE_DUMP=1` — always
check `latest_render_state.json` mtime + the presence of blocks only your exe emits
before attributing its contents to your run.

**LM7 — build discipline.** Island work builds `build64_island` (CMake `MC2_VULKAN_ISLAND`)
and probes build `build64_vulkan` (`run_vulkan_probe.py` owns it). Shipping `build64` must
stay byte-identical for every slice (island code is `#ifdef MC2_VULKAN_ISLAND`; the ifdef'd
TU list must match across mc2/gameos_main — ODR guard `check-gated-layout-macros.py`). Full
relink on class-layout changes. RelWithDebInfo always.

**Canonical proof commands** (run from `A:/Games/mc2-vulkan-backend`):

```powershell
# probe + fixtures (build64_vulkan, all tokens must be 1, validation_errors=0):
py -3 scripts/run_vulkan_probe.py --validation sync
# backend-compare (island lane exe; region from REGION_SPEC):
py -3 scripts/run_golden_parity.py mc2_01 --backend-compare --region <Region> --exe A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe
# tier1 defaults (island exe, all gates OFF -> proves no default-path regression):
$env:MC2_DEBUG_STATE_DUMP="1"; py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs --exe A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe
# NEVER --kill-existing / --with-menu-canary / --duration > 30.
```

---

## N0 — VK-SSAO-PARITY-FAIL-1 — **RESOLVED 2026-07-02 (supervisor oracle ruling + 5/5 GREEN)**

### RESOLUTION (2026-07-02, session 4)

**SUPERVISOR ORACLE RULING (verbatim, now encoded in `run_golden_parity.py` REGION_SPEC
comment + ladder §5b):** "transcendental-class regions may declare an EXPLICIT per-region
tolerance floor — max |diff| ≤ 1 LSB AND diff-pixel count ≤ 0.1% — in REGION_SPEC (same
family as the oobfog envelope precedent). Byte-exact remains the DEFAULT for all regions;
the floor is a declared per-region property, never global."

Encoded as `REGION_SPEC["PostprocessSSAO"]["tolerance_floor"]` +
`evaluate_region_tolerance()` (pure-stdlib PNG diff; consulted ONLY after a byte-exact
FAIL; structural culprits NEVER tolerated; missing PNGs REFUSE; RESULT line keeps the
honest `within_floor=False` next to `region_tolerance=WITHIN[stats]`). Self-test case (e)
covers within/exceeds-1LSB/exceeds-0.1%/structural-refuse/empty-refuse/missing-png-refuse.

**Proof (quiet lane, LM8 bracketed: exe cmp'd before+after = build64_island output, md5
`a4d12301`; B's own capture log carries `[VK_SSAO_ISLAND]` health lines
attempted=3946 used_vulkan=3946 validation_errors=0):**

```
RESULT: PASS backend parity: GL vs Vulkan (region=PostprocessSSAO) within_floor=False
region_tolerance=WITHIN[highangle_wide:max|diff|=1(<=1) diff_px=134/480000=0.0279%(<=0.10%)]
pixel_oracle=SHARP(6 exact px fields) exit=0   A(gl)=GLInline B(vk)=VulkanSubgraph
```

4/5 bookmarks byte-IDENTICAL; highangle_wide within the declared floor → **5/5 GREEN.
L6.4 / L6.5 / L6.6 UNBLOCKED.** Fog re-proof was already GREEN byte-exact on this exe
(session 3). Remaining N0 acceptance (tier1 defaults on the island exe) run this session —
see state ledger.

---

### (history) VK-SSAO-PARITY-FAIL-1 (UNIFORM-RATIO BUG DEAD — matrix transpose fixed `ac96e31d`; residual is now ±1 LSB on 0.03% px, a perceptual-floor question for the supervisor; M / medium)

### UPDATE 2026-07-02 (session 3 — ROOT CAUSE FOUND: SSAO UBO row-major transpose)

**The uniform ~34% over-darkening is DEAD. Single diverging constant found (exactly as the
supervisor predicted — not ULP drift): the SSAO params UBO was qualified `row_major` but
the GL SSAO path uploads column-major.** `shaders/vulkan/ssao.frag` `SSAOParams` had
`layout(std140, row_major)`. The GL SSAO path (`gos_postprocess.cpp` `runSSAO()`) uploads
`inverseViewProj_`/`viewProj_` with a SINGLE `glUniformMatrix4fv(..., GL_FALSE, ...)`, and
those arrays are column-major on the CPU (index=col*4+row; `boxDecalMulColMajor` +
comment `gos_postprocess.cpp:1508`). GLSL reads a GL_FALSE upload column-major → GL SSAO
computes `M*v` with M column-major. The `row_major` VK UBO read the SAME bytes transposed
→ `transpose(M)*v` → world reconstruction collapsed, `dist` shrank, `rangeCheck`
saturated, ~every sample read `occluded` (raw AO ~0.08 vs GL ~1.0 on byte-identical
inputs). **Why fog was immune:** the GL FOG path DOUBLE-transposes (`invT =
transpose(inverseViewProj_)` then `setMat4`/GL_TRUE, `gos_postprocess.cpp:2755-2771`) →
nets a row-major read → fog's `row_major` UBO is correct. SSAO has only the single
implicit transpose → needs `column_major` (the std140 default). Fog uses ONLY
`invViewProj`; SSAO is the sole consumer of the forward `viewProj` — so fog's byte-exact
proof never exercised this convention (LM1 in action).

**Fix (`ac96e31d`):** drop `row_major` from `SSAOParams` (column_major default now matches
the GL_FALSE upload; contiguous std140 mat4 lands element (col c,row r) at byte (c*4+r)*4 =
the GL array layout). CPU pack unchanged. **Fixture correction (the LM1 trap, same
commit):** the SSAO fixture packed matrices raw AND read its CPU reference row-major, so it
validated the shader against its OWN transpose convention and could NOT catch the real-path
divergence. Now the fixture transposes its (row-major-written) literals at UBO pack time so
the column_major shader gets the same LOGICAL matrix the CPU ref computes; the x←z shear is
preserved so a regression re-diverges. `probe --validation sync`: **ssao1,
validation_errors=0.**

**Proof (mc2_01 PostprocessSSAO backend-compare, quiet lane, exe md5 `a4d12301`,
region_impl=VulkanSubgraph — genuine vk, not FallbackGL):** went from **5/5 bookmarks
failing** (69% darker, mean B/A 0.68) → **4/5 byte-IDENTICAL** (overview_center,
ridge_lowangle, edge_fog_boundary_south, edge_fog_corner_southwest all match GL SHA
exactly). Only `highangle_wide` (the one occlusion-bearing high view) still differs.

**RESIDUAL characterization (highangle_wide, final scene, non-debug, GL vs VK):
134/480000 px changed = 0.03%; MAX |diff| = 1 (ONE 8-bit level); mean luminance ratio
1.0000 overall / 0.9929 on the 134 changed px. Deterministic (stable hash across 2 VK
reruns).** This is the ±1-LSB `sin/cos/fract/pow` rounding drift between the AMD GL driver's
GLSL frontend and glslang SPIR-V flipping the occasional `occluded` boolean — i.e. exactly
the sub-1% residual the supervisor said "earns a perceptual-floor discussion" once the
uniform-ratio bug was dead. **ESCALATION: the byte-exact oracle now blocks ONLY on this
±1-LSB residual. This is an ORACLE-DESIGN decision for the supervisor** (per the
reorder-candidate precedent I did NOT self-weaken). Options: (a) accept a per-region
tolerance floor of ≤1 LSB / ≤0.05% px for PostprocessSSAO (transcendental-bearing region,
same class as oobfog's structural-envelope oracle); (b) keep byte-exact and treat SSAO as
permanently 4/5 with a documented known-diverging bookmark. **L6.4/6.5/6.6 remain BLOCKED
pending that ruling** — but the plumbing/math is now proven sound.

### UPDATE 2026-07-02 (session 2 — instrumented, quiet-lane, LM8 bracketed)

**Root cause of the GROSS corruption (raw AO ≈0.08 / center-X): the island ran on
frames it should have SKIPPED.** `MC2_VK_SSAO_DUMP_INPUTS=1` (new, default-OFF)
proved it: on the island's FIRST executed frame the depth readback is ALL-ZERO and
`inverseViewProj_`/`viewProj_` are ALL-ZERO — because those matrices are populated
ONLY by `setTerrainMVP()` during terrain draw, and are zero-initialized otherwise.
GL's `runSSAO()` self-guards on `ssaoEnabled_ && sceneHasTerrain_` (+ program/FBO)
and no-ops on such frames; the island had NO guard, so it reconstructed garbage AO
from a zero matrix + zero depth. On the first IN-MISSION (depth-signal) frame the
same dump showed **correct** depth (min/max/mean plausible), unit-length normals,
non-zero matrices, and a **healthy** effective AO ratio (mean out.g/in.g = 0.66–0.96
across the run). So the "depth/normal readback CONTENTS" prime suspect was WRONG —
the readback is correct in-mission; the corruption was the missing enable-guard.

**Three plumbing bugs fixed (commits below):**
1. **BUG-1 enable-guard** (`f902b2ef`) — `runSSAORegionVulkanBackend` now declines
   (`ssao_gate_off`) when `!ssaoEnabled_ || !sceneHasTerrain_ || program/FBO invalid`,
   mirroring GL. `runSSAORegionThroughBackend` treats that decline as a SKIP (not
   FallbackGL) so a trailing no-terrain frame can't stamp the shutdown health
   FallbackGL (harness exit-5). This is ALSO "the vk route ignores MC2_SSAO gate".
2. **BUG-2 LINEAR apply sampler** (`a38a610e`) — `occSampler` NEAREST→`VK_FILTER_LINEAR`
   to match GL `ssaoColorTex_` GL_LINEAR (the 4-tap box relies on HW bilinear).
3. **BUG-3 occlusion format** (`a38a610e`) — `R8_UNORM`→`VK_FORMAT_R16_SFLOAT` to match
   GL's GL_R16F precision (R8's 256 levels alone exceeded the pixel-exact floor).
Harness gap also fixed (`5cd3cc65`): `MC2_SSAO`/`MC2_SSAO_DEBUG` added to the
`run_smoke.py` allowlist (the compare's structural half was running SSAO OFF).

**RESIDUAL — STILL OPEN (N0 NOT closed, L6.4/6.5/6.6 STAY BLOCKED).** With ALL three
fixes the `--region PostprocessSSAO` backend-compare **still FAILs** (within_floor=False,
5/5 bookmarks, region_impl B=VulkanSubgraph so NOT a FallbackGL — a genuine vk run).
Direct PNG diff (A=gl vs B=vk, quiet lane, both `[VK_SSAO_ISLAND]`-verified):
**B is UNIFORMLY DARKER than A** — B-darker 69% / B-brighter 0%, mean B/A luminance
ratio ≈0.68 (median 0.66), ~68% of pixels changed, mean magnitude 4.5–17.6/chan.
gl-vs-gl floor is byte-IDENTICAL (0 px), so the oracle is sound and byte-exact IS the
bar. VK **over-darkens ~34%** — a systematic occlusion-computation divergence, NOT the
plumbing. This over-darkening PRE-DATES this session (the pre-fix baseline B also
failed 5/5). Live suspects, in order:
- **Vulkan clip-Y flip — RULED OUT analytically this session.** SSAO is two passes
  (occlusion→apply), but the flip cancels: VK writes occlusion top-row from TexCoord.y=0
  = depth image v=0 = GL's bottom depth row (glGetTexImage is bottom-up); the apply pass
  samples that same flipped occlusion and rasterizes flipped again, and the readback
  `glTexSubImage2D` writes row 0 to the GL-bottom texel — the double flip cancels and the
  AO lands on the correct scene pixel. (Fog's single pass cancels the same way; fog is
  byte-exact.) Not the Y-flip.
- **half-res sampling GRID OFFSET — RULED OUT analytically.** Both GL (`quadVAO_` 6-vert +
  `postprocess.vert`) and VK (`ssao.vert` 3-vert `gl_VertexIndex` tri) interpolate TexCoord
  to identical pixel-center UVs `(px+0.5)/halfW,(py+0.5)/halfH`; NEAREST-sampling full-res
  depth at that UV picks the same texel in both. No offset.
- **D32_SFLOAT vs DEPTH24 precision — RULED OUT analytically.** `glGetTexImage(DEPTH24→
  GL_FLOAT)` yields floats quantized to 2^24 levels; D32_SFLOAT stores those losslessly and
  returns them exactly on sample = exactly GL's sampled `.r`. No redistribution in practice.
- **LEADING HYPOTHESIS — now CORROBORATED by the fog re-proof: hard-branch transcendental
  amplification.** ★ The PostprocessFog backend-compare on this SAME fixed exe
  (`da788127`, quiet lane, exe_md5-verified in-run) went **GREEN: within_floor=True,
  culprit_bookmark=(none), exit 0, SHARP(6 exact px fields)** — byte-exact. Fog and SSAO
  share the ENTIRE bridge (glGetTexImage depth readback, D32_SFLOAT depth, RGBA16F color
  round-trip, Vulkan clip-Y). Fog reaching byte-exact through that bridge EMPIRICALLY
  confirms the bridge + round-trip + Y-flip cancellation + depth precision are all sound —
  so the SSAO residual is NOT plumbing. The one structural difference is that SSAO's
  occlusion pass accumulates 16 samples, each gated by a HARD boolean
  `storedDepth > sampleProjDepth + aoBias` and weighted by `smoothstep`, over
  `sin/cos/fract/pow/normalize` math. GL compiles GLSL text via the AMD GL driver; VK
  consumes glslang-baked SPIR-V — different frontends/optimizers. Sub-ULP differences in the
  transcendentals FLIP whole-sample `occluded` booleans, and 16 accumulated flips compound
  into the observed ~6%/±34% AO divergence. Fog has NO hard-branch accumulation, which is
  why fog reaches byte-exact and SSAO may NOT. **If confirmed, SSAO cannot meet the
  exact-SHA oracle** and needs a per-region tolerance floor (an ORACLE-DESIGN decision for
  the supervisor — do NOT self-weaken; precedent: reorder candidate rejected, gate stayed
  OFF).
**NEXT (to confirm): isolate occlusion-pass-only** — add a default-OFF debug env that forces
the apply pass to passthrough so the readback carries the raw occlusion buffer, diff GL
`ssaoColorTex_` vs the VK occlusion. If the raw AO already differs with identical inputs
(which the input-dump proves), the branch-amplification hypothesis is confirmed; escalate the
tolerance-floor question. Do NOT weaken the oracle unilaterally; L6.4/6.5/6.6 STAY BLOCKED
until PostprocessSSAO is genuinely GREEN ×5 (or the supervisor rules a floor is acceptable).

**Instrumentation retained** for this: `MC2_VK_SSAO_DUMP_INPUTS=1`.

---

**Finding (2026-07-02, forensically corrected — read LM8 first):** the first SSAO
backend-compare (`run_golden_parity.py mc2_01 --backend-compare --region PostprocessSSAO`)
**FAILED** — the B(vk) process produced a corrupted frame on ALL 5 bookmarks: scene
darkened (b/a ratio ~0.6 where diff, 63-69% of pixels), film-grain noise + a moiré "X"
converging at frame center; the diff does NOT blur away (systematic, not dither). B
genuinely ran the vk island (B's own state dump: `render_backend_ssao_region.region_impl=
VulkanSubgraph`, vk_draws=2, routed_frames=1161 — and the dump CONTAINS the ssao block,
which only the island exe emits). **This is the ONLY genuine vk-SSAO pixel capture in
existence.** Earlier session notes claiming "4 clean byte-identical reruns / nondeterminism"
are VOID: forensics showed a concurrent agent swapped the v0.4c lane exe (LM8) between the
parity run and every rerun — those reruns executed a foreign NON-island exe (their capture
logs have zero `[VK_*]` shutdown health lines; their state dump lacked the ssao block).
The corruption may therefore be a plain DETERMINISTIC island bug. Diagnosis restarts on a
quiet lane.

**Evidence paths:** corrupt B =
`A:/Games/mc2-opengl/mc2-win64-v0.4c/debug_state/golden_parity/mc2_01/B/viscap/*.png`
(diff vs `A/viscap`; A = gl backend, same exe, pixel-clean). Reproduce on a QUIET lane:
deploy island build per LM5, then `run_visual_capture.py --mission mc2_01 --exe
<lane>/mc2.exe` + `MC2_SSAO=1 MC2_RENDER_BACKEND_REGION_IFACE=1 MC2_POSTPROCESS_BACKEND=vk
MC2_DEBUG_STATE_DUMP=1`, and VERIFY the run's own log carries `[VK_SSAO_ISLAND]` health
lines (exe-identity fingerprint) before trusting any pixels.

**CONFIRMED bugs found during diagnosis (island exe, valid evidence):**
1. **Missing enable-guard on the vk SSAO route:** with `MC2_RENDER_BACKEND_REGION_IFACE=1`
   + `MC2_POSTPROCESS_BACKEND=vk` and **MC2_SSAO UNSET**, the island still ran every frame
   (guard_on capture log, 2026-07-02 02:41: `[VK_SSAO_ISLAND] attempted=2268
   used_vulkan=2268`). GL's `runSSAO()` self-guards on `ssaoEnabled_` && `sceneHasTerrain_`;
   `runSSAORegionVulkanBackend`/the island do NOT — vk runs a pass GL would skip (also in
   menus/no-terrain frames). Fix: mirror both guards before entering the island (decline →
   the GL wrapper's own guards then no-op). Suspect this also interacts with the corrupt B:
   the island can run against SSAO resources GL considers not-ready.
2. **NEAREST-vs-LINEAR apply sampler:** `shaders/vulkan/ssao_apply.frag` header rationalizes
   a NEAREST sampler for `ssaoTex`; GL samples the half-res AO with LINEAR filtering under
   the same 4-tap box (`shaders/ssao_apply.frag` — "linear filtering already smooths
   between"). The vk 4-tap-NEAREST is NOT the same filter. A moiré-against-the-pixel-grid
   artifact is exactly what a broken half-res upsample produces — this is a PRIME suspect
   for the corrupt B's "X" pattern. Fix in `vulkan_ssao_island.cpp` (LINEAR sampler for the
   apply set) + correct the shader comment.
3. **Likely-but-reverify (observed on the foreign exe, same shader family):** mc2_01
   bookmarks yield AO≈1.0 everywhere (MC2_SSAO_DEBUG raw-AO frames all-white) → even a
   passing mc2_01 compare would prove the ROUND-TRIP, not the AO math. Add an
   occlusion-bearing bookmark set (urban poses, mc2_03/mc2_17) to the compare.

**Also re-run once the lane is quiet (evidence voided by LM8):** fog backend-compare
post-`34ade1ee`-rename (fog parity was last genuinely proven BEFORE the rename), and tier1
defaults on the island exe (the 2026-07-02 02:34 tier1 ran the foreign exe — its 4/5 PASS +
mc2_01 crash_silent tell us nothing about this branch; the crash_silent also coincided with
foreign lane activity).

**UPDATE 2026-07-02 03:0x — corruption is DETERMINISTIC and localized to pass-1 inputs:**
- Quiet-window rerun (island exe verified via `[VK_SSAO_ISLAND]` lines in the run's OWN
  log, LM8 protocol): bookmark hashes **bit-identical to the corrupt parity-B hashes**
  (831d5ecd/52a229eb/e8527982/28e98749/8afd8d51). Deterministic, reproducible.
- Island raw-AO capture (`MC2_SSAO_DEBUG=1`, island exe): the occlusion field is
  **ao≈0.08 nearly everywhere** (mean 19.5/255, p99=64) with a bright X/hourglass
  converging at frame center — while GL's AO on the same scene is ≈1.0. Same shader math,
  supposedly same inputs → the island's INPUTS are not what GL samples.
- Ruled out by inspection tonight: UBO layout/fill (matches shader + fixture, 152B,
  full-res screenSize, same matrices), descriptor writes (depth→0, normal→1, UBO→2, occ
  apply→0), pass-1 viewport/scissor (half-res), Y-orientation (conventions cancel;
  fog parity proves the bridge orientation), param value sources.
- PRIME remaining suspects, in order: (a) **depth readback contents** —
  `glGetTexImage(GL_DEPTH_COMPONENT, GL_FLOAT)` on `sceneDepthTex_` while it may still be
  ATTACHED to the bound scene FBO (feedback/undefined-read hazard; the fog subgraph reads
  the SAME depth — but fog only thresholds depth, so subtly-wrong values can pass fog
  parity while destroying SSAO's position reconstruction); instrument by dumping
  depthScratch to a file and comparing against a GL-side readback taken at a known-good
  point; (b) normal readback contents (same instrumentation); (c) pass-1 pipeline state
  not yet inspected (front-face/rasterizer irrelevant for fullscreen tri, but check
  depth-clamp/sample-shading defaults); (d) the NEAREST depth sampler vs GL's
  sceneDepthTex_ filter state (check `glTexParameteri` on creation — LINEAR depth
  filtering would smooth reconstruction; NEAREST vs LINEAR alone should not produce
  ao 1.0→0.08 though).

**Order of work inside N0:** (1) instrument the bridge: dump depthScratch/normalScratch
(first 64 floats + a checksum) behind an env; compare vs GL truth — this names suspect
(a)/(b) in one run; (2) land the enable-guard fix (bug 1) + LINEAR apply sampler (bug 2);
(3) fix the named input bug; (4) rerun raw-AO debug capture (expect ≈white), then 5
consecutive backend-compare exit-0 runs + fog compare re-proof post-rename + tier1 on the
island exe (all under LM8 protocol).
**Until N0 closes, do NOT start L6.4/L6.5/L6.6 — do not grow the family on an unproven
template** (the fog subgraph shares the same bridge code pattern).

## N1 — VK-FOG-SKYEXCLUDE-GUARD-1 — **SHIPPED `d7fbd72f` (2026-07-02)**

`runFogRegionVulkanBackend()` (`gos_postprocess.cpp`) now declines with
`fallback_reason="sky_exclude_not_ported"` when `skyboxFogExcludeEnabled()` — the vk fog
subgraph has no stencil input (LM4), so gate-ON + vk would silently drop sky exclusion.
Island-ifdef'd TU only → shipping build64 byte-identical. Proven on the island exe
(guard_on capture log: `[VK_PP_SUBGRAPH] attempted=0` with exclude ON while
`[VK_SSAO_ISLAND]` ran in the same process → the guard, not a dead iface, stopped it).
REMAINING ACCEPTANCE (queue with N0 step 4, quiet lane): a dump-instrumented capture
showing `render_backend_region.region_impl=="FallbackGL"` + the reason string, and the
default fog compare exit 0 non-fallback.

## L6.4 — VK-REGION-HZB-1 (HZB-reduce region; M / medium)

**GL shape (read first):** `gos_postprocess.cpp:1698-1770` — gated `MC2_HZB_BUILD`
(default OFF), R32F reverse-Z pyramid, SEPARATE texture per mip level (`hzbLevelTex_[]`,
one FBO re-attached per level), `shaders/hzb_reduce.frag` with `uReduce` 0/1 (level 0 =
copy from scene depth, level k = 2x2 min-reduce of level k-1), `uSrcTexel` per level.
Diagnostic probe (`MC2_HZB_PROBE`) does CPU cross-check via `glGetTexImage` — that CPU
reference is REUSABLE as the fixture oracle.

**Files:** follow the SSAO region precedent EXACTLY (commit `af5f9cdc` is the template):
- `RenderCore/RenderRegionId.h` — `PostprocessHzb` enum + `PostprocessHzbRegionContext`
  (RenderResourceId depth in / HzbPyramid out is already a RenderResourceId via the FBO
  ledger; dims + mip count; NO handles) + string case.
- `RenderCore/IRenderBackend.h` — `runRegion` overload for the new context (ruling R3:
  overload-per-region).
- `GameOS/gameos/gos_postprocess.cpp` — extract `runHzbRegionGL()` (pure code-move of the
  1698-1770 loop), route via `runHzbRegionThroughBackend()` (copy the SSAO selector block),
  health `render_backend_hzb_region`.
- `GameOS/gameos/vulkan_hzb_island.cpp` — NEW island TU. Port hzb_reduce per §5a (1 sampler
  → binding 0, params UBO @1). Per-mip chain: ONE VkImage with N mip levels + N framebuffer
  views is the natural Vulkan shape, with a per-mip `VkImageMemoryBarrier`
  (COLOR_ATTACHMENT_WRITE → SHADER_READ, mip k before mip k+1 draws — barrier plan B13);
  or mirror GL's separate-images if simpler — either is fine, the barrier is the point.
- Fixture in `vulkan_backend_skeleton.cpp` + header + `tools/vulkan_shader_probe_main.cpp`
  + `SUCCESS_TOKENS` in `run_vulkan_probe.py` ("hzb_fixture"): synthetic 64x64 depth
  (known pattern), CPU min-reduce reference (exact — min() of floats IS bit-portable, no
  transcendental caveat: use the BIT-TIGHT oracle, tolerance 0, unlike ssao/fog),
  validate every mip level via per-level readback.
- `scripts/run_golden_parity.py` — REGION_SPEC entry: `"PostprocessHzb": {"health_block":
  "render_backend_hzb_region", "extra_env": {"MC2_HZB_BUILD": "1"}}`.
- `GameOS/gameos/debug_state_dump.cpp` — health block (copy ssao block).
- `docs/tier1_env_vars.md` — note vk coverage of MC2_HZB_BUILD.

**Watch:** HZB output feeds nothing else yet (probe is diagnostic) — the pixel oracle
won't see HZB content in MainColor. The BINDING proof is therefore the fixture (bit-tight)
+ the backend-compare only proves no-regression of the visible frame with the gate ON +
`region_impl=VulkanIsland` non-fallback. State that honestly in the commit.

**Acceptance:** probe all-1 validation 0 (sync preset) incl. new hzb_fixture; tier1
defaults 5/5; backend-compare `--region PostprocessHzb` exit 0 non-fallback; `build64`
byte-identical.

## L6.5 — VK-SPV-SINGLE-SOURCE-FOLD-1 (retire hand-mirrors; S→M / low)

**Goal:** ONE GLSL source per pass, dual-target bake (`--target-env opengl` for the GL
SPIR-V consumer, `--target-env vulkan1.1` for the backend); delete
`shaders/vulkan/{edge_fog,fog_oob,ssao,ssao_apply}.{vert,frag}` hand-mirrors.

**Precondition (per ladder §5a bake plan):** each GL source gains (a) explicit
`layout(binding=)` on samplers per the family table, (b) loose uniforms gathered into the
params UBO. That is a GL-SIDE change (glUniform* → UBO upload in `gos_postprocess.cpp`)
— prove it byte-identical with the plain GL golden oracle (`run_golden_parity.py` in
feature-gate mode is NOT applicable; use two GL captures pre/post change → identical
pixel_shas) BEFORE flipping any Vulkan consumer to the new bake.

**Order within the slice:** (1) convert ONE pass (edge_fog) GL source + GL upload path →
GL parity proof; (2) add the vulkan1.1 bake target to the CMake shader step
(`GameOS/gameos/CMakeLists.txt:193` region) emitting to `<build>/shaders/vulkan/`;
(3) point the island loader at the baked artifact, delete the hand-mirror, re-run fog
backend-compare byte-identical; (4) repeat for fog_oob, ssao, ssao_apply; (5) extend
`check-vulkan-bindings.py` to parse the SINGLE source. STOP at any parity failure —
partial fold (some passes folded, some hand-mirrored) is an acceptable landing state, but
say so in the commit.

**Watch:** the GL sources have `u_skyExcludeEnabled` + `usampler2D stencilTex` (LM4) that
the hand-mirrors lack — folding edge_fog/fog_oob means the baked vk frag now DOES declare
stencilTex → either bridge it (R8_UINT pattern) or spec-constant it out
(`constant_id` for the exclude branch, and keep the N1 guard). Decide with the supervisor
if unclear; do not silently sample an unbound descriptor (validation will catch it — sync
preset is mandatory here).

## L6.6 — VK-REGION-COMPOSITE-PRESENT-1 (L / medium — SUPERVISED, get a go first)

First "Vulkan writes the presented image". Couples the region path to the L5 engine-owned
swapchain (`vulkan_swapchain_present` machinery from the lib track). Region =
Composite/backbuffer blit (FBO-0 edge, `colorFinalLayout=Present` already declared in the
frame-graph contract).

**Shape:** `RenderRegionId::CompositePresent` + context (MainColor in, swapchain image
out — the resolver cannot return a GLuint for a swapchain image; extend `RegionOutput` or
the resolver contract minimally, do NOT put a VkImage in the neutral context). Vulkan impl:
acquire → fullscreen blit/draw of the bridged MainColor → present, reusing the L5
present+resize+semaphore-per-image discipline (present-semaphore-reuse bug already burned
once — reuse the L5 pattern verbatim). Fail-soft = GL swap path untouched.

**Proof:** present 16 consecutive frames validation-clean (sync preset), backend-compare
on the full-frame region (pixel oracle reads FBO pre-present; ALSO capture a swapchain
readback if feasible), tier1 defaults 5/5, and a 30s gate-ON soak on mc2_01+mc2_24.
**Do not attempt unsupervised** — this rung changes who presents.

## L7.x / L8.x — geometry ladder (specs deferred BY DESIGN)

L7.1 (SSBO:2 + slot-14/slot-20 binding splits) → L7.2 Shadow → L7.3 Terrain → L7.4 Water →
L7.5 StaticProp+Mech → L7.6 descriptor consolidation → L8.0 UI liveness → L8.1 default
flip. Each REQUIRES its own recon slice first (descriptor tables per family, buffer
ownership JIT per ladder §3, LM2 for terrain/water). Do NOT start L7.1 without checking
concurrent terrain-v2 branches (ladder §8 ruling 6 — coordination checkpoint). The ladder
doc §6 table remains the authority on order and proof method.

---

## State ledger (2026-07-02, wave-3 close)

- Regions live through `runRegion`: PostprocessFog (subgraph, 2 draws) — parity last
  genuinely proven PRE-rename; post-rename re-proof queued in N0 (LM8 voided the 07-02
  attempt); PostprocessSSAO (island, 2 passes) — **parity FAILED on its only genuine run**
  (N0). Both fail-soft, health-dumped, REGION_SPEC-registered.
- Probe fixtures green (sync validation, 0 errors): edgefog, oobfog, ssao (class-based
  oracle — sky/floor/wall bit-tight, envelope band + dual liveness).
- tier1 defaults on the ISLAND exe: NOT yet validly run (LM8) — queued in N0 step 4.
- Known divergences: fog vk vs `MC2_SKYBOX_FOG_EXCLUDE=1` (→ N1 guard, SHIPPED
  `d7fbd72f`); ssao vk runs without `MC2_SSAO` (→ N0 bug 1, open).
- Island deploy lane: `A:/Games/mc2-opengl/mc2-win64-v0.4c` (LM5 command).
- `GpuBufferOwner.backendHandle` is 64-bit, GL-free record (L6.1) — geometry regions
  inherit it; do not reintroduce `glName`.
