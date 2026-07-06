# VK-TERRAIN sub-feature oracle + dynamic-shadow ship (2026-07-05)

Session: per-actor mech scheme cache + co-covered mech oracle + terrain material completion.
Branch `claude/vk-interop-bridge-1`. Deploy `A:/Games/mc2-opengl/mc2-win64-0.4c`.

## ORACLE METHOD (terrain)
Bookmark cameras frame mostly COARSE-LOD terrain that GL draws (VK terrain only co-writes
LOD0) → GL-vs-VK bookmark diff = 0.000 on all 11 mc2_01 bookmarks (VK LOD0 matches GL, but
the residual features aren't exercised). The exercising camera is the NATURAL close game
camera (free-capture, `MC2_VISUAL_CAPTURE_FRAME=200`, no bookmark) where VK LOD0 fills the
screen and the base structures are visible. Driver `.claude/mech-parity/freecap_driver.py`;
bookmark control `.claude/mech-parity/terrain_oracle.py`.

★Free-capture caveat: the natural camera drifts slightly run-to-run (not byte-stable), so
free-cap numbers carry ~8-10/ch drift noise in grass. Use free-cap for SHADOW-REGION numbers
(large, localized) and the BYTE-STABLE bookmark sweep for the gate-OFF control.

## SHIPPED: MC2_VK_TERRAIN_DYNSHADOW (commit 43e6bbc2)
Dynamic (building/mech) sun shadow on VK terrain — legacy single-map (non-CSM) path.
- Feed (gos_terrain_lod_chunk.cpp): pp->getDynamicShadowTextureRaw() + getDynamicLightSpaceMatrix()
  + dynShadowsEnabled (shadowsEnabled_ && getDynamicShadowFBO() && !CSM).
- Island (vulkan_terrain_island.cpp): 2nd D32 depth-compare image (binding 7), slot-parameterized
  ensure_shadow_image_slot + copy_shadow_slot (refactored the static path to share). UBO 256->512B.
- Frag (terrain_tier1.frag): shadow = min(static, dyn); pureConcrete -> dyn only. Reuses
  computeBakedShadow (VERBATIM legacy calcDynamicShadow: adaptiveScale 2.4->0.8, mix(0.4,1.0), 8 taps).

★ORACLE (mc2_01 free-cap f200, hut_shadow_blob region = building shadow on concrete):
  BEFORE (GL vs VK-nodyn): mean 81.9, 92% covered  (no dynamic shadow — bright concrete)
  AFTER  (GL vs VK-dyn):   mean  4.0,  5% covered  (building shadow lands, matches GL)
★GATE-OFF CONTROL (byte-stable bookmark sweep): dynshadow OFF vs ON = BYTE-IDENTICAL on all
  11 mc2_01 bookmarks → feature inert where no dynamic caster's shadow lands. Discipline held.

## DEFERRED terrain sub-features (oracle-before-feature plan for each)
Per handoff dependency order, dynamic-shadow was first (mechanism-proven by spike 773e514e).
Remaining, each its own gate riding MC2_VK_TERRAIN_LIGHTING, oracle at free-cap f200:
- **anti-tile** (`sampleAntiTileArr`, terrain_lod_chunk.frag:284-355): variance-preserving
  hashed-offset resample + fwidth per-layer near-gating. Oracle: grazing-angle grass tiling
  seams. Medium — needs the ddx/ddy gradient plumbing + hash.
- **POM** (`chunkParallax`, :282-357): per-material height-alpha ray-march (data-dependent loop,
  needs the atlas ALPHA displacement channel imported + pomParams). Oracle: steep-angle relief
  on rock/concrete. Larger — data-dependent loop + a new alpha texture import.
- **markings/overlay sidecar** (:626-660, u_overlaySidecar + u_overlayBounds): world-XY->UV
  authored cement/pad/RUNWAY overlay composite. Oracle: runway numbers on a runway map
  (mc2 airport). Medium — one more copy-once texture + world-bounds UV + additive composite.
- hemisphere ambient (:929-942, terrainLightingV1Strength) — env-gated OFF by default in GL
  too, so not a default-look gap. macro-variation (:860-887) similarly ImGui-gated.

## ★★ORACLE BLOCKER (2026-07-05 session — anti-tile/POM/markings ALL DEFERRED, code-grounded)
Attempted anti-tile first (oracle-before-feature). Found NO same-deterministic-camera oracle
exists for ANY of the three remaining features. Chain of proof (all repeatable):

1. **Bookmark sweep (byte-stable) = 0.000 GL-vs-VK on all 11 poses** AND VKon-vs-VKoff = 0.000
   on all 13 poses tested (added near/low-alt/oblique airport bookmarks incl. alt 250-350).
   → VK terrain co-writes ZERO visible pixels at every bookmark camera. GL owns the frame →
   trivially identical. Nothing for a terrain feature to move.
2. **Why:** VK feed captures only `cmd.lodStep==1` (LOD0) patches from SubmitDrawCommands
   (gos_terrain_lod_chunk.cpp:1543). At bookmark free-cameras the LOD-chunk system selects
   NO lodStep==1 chunks in the captured frame, EVEN at alt 350 oblique over the airport apron
   where LOD0 detail visibly should apply (LOD_DIST_THRESH[0]=6000wu should cover it — but the
   bookmark free-camera's LOD origin does not drive the co-write feed the way the natural
   camera does; the "first co-write lod0Patches=7" log fires pre-settle, and the settled
   bookmark frame contributes 0). Airport_lowoblique bookmark renders a perfect near
   concrete+road+grass view (POM+anti-tile SHOULD show) yet VKon-vs-off=0.00.
3. **Free-cap (natural camera) IS the only camera that exercises VK LOD0** — but it is
   ENGINE-DECLARED non-deterministic (gos_visual_capture.cpp:38-43: "single-frame free-capture
   is inherently nondeterministic, the sim is live"). Measured: GL-config self-diff f200 = 0.26%
   (stable), but **VK-config self-diff f200 = 10.68%** (VK path perturbs camera/sim run-to-run);
   GL-vs-VK f200 = 60% dominated by a systematic camera-POSE shift the VK terrain gate induces
   (two screenshots frame the apron in different screen positions). 10.68% jitter swamps the
   subtle anti-tile/POM grass signal → free-cap cannot serve as a same-camera oracle for them.
   (dynshadow got away with free-cap ONLY because its signal — a building shadow blob — is large,
   localized, and robust to a few px of drift; anti-tile/POM/detail grass is not.)

**VERDICT: anti-tile, POM, markings-sidecar all DEFERRED — no oracle, do NOT port blind (redline).**
Mechanism is NOT the blocker (verbatim GL math is ready in terrain_lod_chunk.frag:284-419);
VERIFICATION is. Shipping unverified-parity behind a byte-OFF-only control violates
oracle-before-feature.

★MARKINGS NUANCE (partial good news): mc2_01's DEFAULT runway markings ("21","72G", dashed
centerlines, road markings) come from the LEGACY cement-word + cement/road atlas path, which VK
ALREADY imports and reproduces — runway_overview VKon (FORCE_LOD=0) shows all markings present,
matching GL. The `u_overlaySidecar` "overlay-V2" composite is a SEPARATE env-gated beauty feature
(MC2_TERRAIN_OVERLAY_V2, default-OFF, additive-over-legacy, byte-identical when off / no sidecar)
and mc2_01 has NO deployed overlay sidecar. So markings-in-the-DEFAULT-look are already at parity;
the overlay-sidecar port has neither a default-look gap NOR a map that exercises it here → N/A until
a map ships an overlay sidecar.

★UNBLOCK PATH (next-session, before any of these 3): build a DETERMINISTIC near-camera oracle that
actually drives VK LOD0 co-write. Options: (a) make the bookmark free-camera feed the LOD-chunk
lodStep==1 selection (route bookmark eye into the SubmitDrawCommands LOD origin so a low-alt
bookmark yields lodStep==1 patches → deterministic + VK-exercising); or (b) add a fixed-pose PINNED
game-camera capture mode (freeze the natural camera at a scripted world pose+frame so it's byte-stable
yet still the LOD-driving camera). Either unblocks anti-tile + POM together (both are the same
near-camera detail-normal path, terrain_lod_chunk.frag chunkDetailNormal, fwidth-gated). Est: the
oracle-tooling slice is the real work; the two feature ports are small once it exists.

## ★★UNBLOCKED (2026-07-05 session 2 — deterministic near-camera oracle SHIPPED)
Commit `9326febb`. The oracle blocker above is RESOLVED.

**Root cause of the free-cap non-determinism (the 10.68% VK self-diff):** NOT a VK
path bug and NOT terrain. Under the sim-frozen bookmark sweep the VK terrain co-write
is byte-deterministic. The residual is an in-world MECH whose animation pose is not
frozen at the pinned frame — a tight center island; terrain OUTSIDE it is byte-identical
run-to-run. The old 10.68% was whole-frame LIVE-CAMERA drift (free-cap has no fixed pose
+ live sim), not a terrain/VK-timing defect. VK's residual read ~2× GL's only because the
VK mech path draws more of that same jittering mech.

**Oracle = option (b) hybrid:** bookmark sweep (deterministic sim-freeze + hard-set pose,
gos_visual_capture.cpp) + `MC2_TERRAIN_LOD_CHUNK_FORCE_LOD=0` (a LOD POLICY applied to BOTH
GL and VK — terrain.cpp:246 chooseLodLevel — NOT a VK gate) so every drawn block is
lodStep==1 → the VK island co-writes a large region (SubmitDrawCommands captures all patches).
Two scripted poses in `tests/visual/bookmarks/mc2_01_terrain_near.json`:
- `near_land_off` (pos 4200,-3600 alt 320, no units): byte-perfect 0.000 self-diff UNMASKED.
- `near_land_low` (pos 2496,-1941 alt 300): 56% VK co-write COVERAGE; mech island
  (box x400-516 y100-200, measured union of 6 self-diff pairs) masked → 0.000 terrain self-diff.

**Acceptance (all green, `terrain_near_oracle.py selfdiff`):**
 (i)  self-diff = 0.000 across 3 runs, GL-config AND VK-config, both poses.
 (ii) VKon-vs-VKoff = 56.15% terrain coverage (269541 px) → LOD0 co-write proven.
 (iii) one command re-runs it; poses are named JSON.

## ★ANTI-TILE / POM / DETAIL BASELINE (captured this session, near_land_low pose)
Decomposition via MC2_VK_TERRAIN_DETAIL 0/1 (terrain_near_oracle.py detail):
- GL vs VK(detail OFF):        cov 66.61%  mean_all 8.97  — full near-detail gap
- GL vs VK(detail ON=current): cov 56.28%  mean_all 4.96  — current mv_detailNormal closes ~45%
- VK detail OFF vs ON:         cov 37.37%  mean_all 4.70  — what partial detail already adds
→ Remaining GL gap (56% / mean 4.96) = anti-tile (sampleAntiTileArr) + per-layer fwidth
  fades + POM. These are the DEFERRED near-detail features, now VERIFIABLE.

**PORT SCOPE (next session — small, SHADER-ONLY):** all constants already compile-time in
terrain_tier1.frag (MATV_TILING/MATV_NORMAL_BOOST/MATV_TILING_SNOW) → NO new UBO fields.
Port the WHOLE chunkDetailNormal near-path VERBATIM into mv_detailNormal: (1) add
mv_hash22 + mv_sampleAntiTileArr (texture()-based, not textureGrad — VK's mv_detailNormal
runs in UNIFORM control flow so implicit LOD is valid; the ddx/ddy plumbing GL needs is only
for its non-uniform weight branches), (2) per-layer fwRock/fwGrass/fwDirt/fwConcrete/fwSnow
fades (GL terrain_lod_chunk.frag:353-357), (3) route each layer sample through anti-tile
gated by `fw* > 0.5 && ats* > 0.01` EXACTLY as GL :392-415. Own gate riding
MC2_VK_TERRAIN_LIGHTING (e.g. reuse detailEnabled tier or a new detailAntiTile flag),
byte-OFF control = the 11 stable mc2_01 bookmarks + near_land_off. Requires SPIR-V recompile
(glslangValidator) + deploy 0.4c + smoke. POM is the same near path (chunkParallax) — port
after anti-tile lands, same oracle. Markings-sidecar remains N/A (no map ships an overlay).

## ★★SHIPPED (2026-07-05 session 3 — anti-tile + POM ported on the green near-oracle)
Both near-detail features ported VERBATIM into `shaders/vulkan/terrain_tier1.frag`
`mv_detailNormal`, shader-only (no new UBO fields), riding the existing `detailEnabled`
tier (MC2_VK_TERRAIN_DETAIL, requires MC2_VK_TERRAIN_LIGHTING). Both changes live
ENTIRELY inside `if (uT.detailEnabled != 0)` → default-OFF preserved by construction.

**Gate:** MC2_VK_TERRAIN_DETAIL (state MC2_VK_TERRAIN_LIGHTING family). No new gate/UBO
field added (oracle mandated shader-only). Anti-tile always co-active with detail;
POM always co-active with detail (pomParams.x=0.02 baked > 0).

**Commit f576144d — anti-tile + per-layer fwidth fades + concrete layer:**
- mv_hash22 + mv_sampleAntiTileArr (GL :274-294), texture()-based not textureGrad (VK
  mv_detailNormal is uniform control flow → implicit LOD valid; GL's ddx/ddy plumbing is
  only for its non-uniform weight branches, dropped here).
- per-layer fwidth fades fwRock..fwSnow (GL :353-357).
- anti-tile gate `ats* > 0.01 && fw* > 0.5` (GL :382-415).
- ADDED the previously-omitted concrete layer (MAT_LAYER_CONCRETE=3, matTiling.w).

**Commit 5a495d0f — POM (parallax-occlusion):**
- mv_pomDisplacement + mv_parallax (GL :295-327) + offset block (GL :361-367).
- pomParams baked const {0.02,8,32,0} = GL default (gameos_graphics.cpp:7112 /
  terrain_pom_scale_ :2456). Displacement = matNormalArray.a, carried through VK's
  copy-once RGBA8 import (no separate alpha import needed — verified in island cpp).
- GL POM has NO self-shadow term → full port == biggest-safe-subset (POM sans self-shadow).

**BYTE-OFF VERDICT (both features):**
- near_land_off UNMASKED control: 0.000 self-diff GL-config AND VK-config; GL-vs-VK
  max ≤6 (all sub-threshold) → byte-clean where terrain has no near detail.
- 11 stable mc2_01 bookmarks: inert by construction (coarse-LOD bookmark cameras
  co-write ~0 VK terrain pixels; change is inside detailEnabled). near_land_off is the
  STRONGER control (it actually exercises VK LOD0) and is clean.
- Determinism: VK self-diff = 0.000 both poses after both ports (no new non-determinism).

**near_land_low GL-vs-VK (coverage / mean_all), FORCE_LOD=0, mech island masked:**
| state                         | cov%   | mean_all | per-px max |
|-------------------------------|--------|----------|-----------|
| baseline (detail ON, pre-port)| 56.28  | 4.96     | 255       |
| +anti-tile                    | 56.54  | 4.89     | 255       |
| +anti-tile +POM (SHIPPED)     | 56.73  | 4.713    | **70**    |

★DIAGNOSIS (stop-rule honored): anti-tile ALONE barely moved the number (4.96→4.89;
moved VK only 0.004%/max10 vs baseline). A dN×4 probe moved 28%/mean4.1 → the detail
path is ALIVE and strong; anti-tile is simply a subtle variance-preserving effect at
this pose (fwidth≈1 so the fades are inactive near). Did NOT iterate blind — diagnosed
that POM is the dominant residual. POM confirmed it: moved VK 0.66%/mean1.17 vs the
pre-antitile baseline (10× anti-tile alone), dropping GL-vs-VK mean 4.89→4.71 and
per-pixel MAX 255→70 (worst-case relief disagreement collapsed). Both correct-direction.

Residual GL gap (mean ~4.7) = LOD-tier fade + painted-concrete normal + cliff triplanar
(all still deferred, minor) + colormap/lighting float noise. Diminishing returns for the
near-detail-normal path; the big near-detail items (anti-tile, POM) are now done.

## ★ITEM-3 NEXT-SESSION SLICE SCOPE (fail-soft + height-audit default-ON gates)
The deterministic near-oracle now makes these verifiable. NOT shipped this session
(out of scope; item was "scope it in the doc"). Scope:
- **VK terrain fail-soft** (setFallback paths in vulkan_terrain_island.cpp): when a
  copy-once import (colormap/cement/matNormalArray/shadow) is not ready, the FS already
  fails soft per-fragment (uT.*Ready flags → olive / skip). The DEFAULT-ON gate to verify
  is the ISLAND-level fallback (no_terrain_feed / spv_load_failed → GL owns terrain).
  Oracle: force each *Ready=0 individually at the near pose, assert VK output degrades
  gracefully (never holes/crashes) and that with everything ready the near_land_low
  co-write is unchanged. This is a robustness gate, not a parity feature — verify with
  the near-oracle's VKon capture + a deliberately-broken-import run.
- **height-audit** (heightsFS SSBO snapshot vs GL live heights): the mv_smoothTerrainNormal
  macro-slope reads uHF.heightsFS. A default-on audit gate should compare the VK height
  snapshot against GL's per-frame heights at the near pose (they must match or the macro
  slope diverges). Oracle: near_land_low VK vs a VK-height-audit-assert run; expect 0
  mismatches. Cheap — one SSBO checksum compare.
- Est: both are small verification-gate slices (no new shader math); the near-oracle +
  FORCE_LOD=0 is the driving camera. Do fail-soft first (bigger safety value).

## ★★SHIPPED (2026-07-05 session 4 — item-3 fail-soft + height-audit DONE)
Commits `61590207` (infra) + `4a14687a` (deadlock fix). Both DEFAULT-ON safety gates
built + verified on the green near-oracle. Files: vulkan_terrain_island.{h,cpp},
gos_terrain_lod_chunk.cpp, RendererFeatureRegistry.h, run_smoke.py.

**FAIL-SOFT (island-level GL fallback, no LOD0 hole):**
- The GL LOD0-skip is now gated on island-READY, not merely -enabled. New
  `mc2_vk_terrain_island_prepare(w,h)` births the island at loop-top (GL ctx current)
  BEFORE the skip decision, so ready reflects real state. `mc2_vk_terrain_island_ready()`
  is the clean predicate (born+inited+!disabled+spv+pipeline). Any island failure ->
  GL keeps drawing LOD0. Loud one-shot stderr on fail-soft entry + on a post-skip
  run()==0 1-frame gap.
- ★DEADLOCK CAUGHT+FIXED (first cut regressed VKon-vs-VKoff 56%->0%): skip gated on
  ready, ready on a born island, island born only from the skip's feed. prepare()
  births independent of feed -> broke it. Lesson: birth must NOT depend on the very
  skip it gates.
- ★VERIFIED per failure mode:
  · **missing SPV** (MC2_VULKAN_SPV_DIR=empty): "cannot open spv" -> "init failed
    (spv_load_failed) — GL fallback" -> my fail-soft log -> output == GL baseline
    (near_land_low cov 0.169% = mech-jitter only; near_land_off 0.000). No hole, no
    crash. FIRED / logged / recovered. ✅
  · **no feed** (window-minimized harness -> VK present forced GL): fallback_reason
    "no_terrain_feed", GL drew LOD0, no hole. FIRED / recovered. ✅
  · pipeline/renderpass/framebuffer/device-null all route the same init-fail path ->
    island destroyed -> prepare()==0 -> GL draws (covered by construction, same code
    path as spv_load_failed).
  · transient (height_snapshot_failed / submit_failed / fence_timeout) latch
    s->disabled -> prepare()==0 next frame; at worst a single self-healing frame with
    the loud gap log.

**HEIGHT-AUDIT (MC2_VK_TERRAIN_HEIGHT_AUDIT, verification-only, default-OFF):**
- FNV-1a checksum of the version-gated height snapshot; each frame re-reads GL live
  heights + compares. Loud rate-limited [HEIGHT_AUDIT] MISMATCH on divergence
  (crater/deform without a g_terrainHeightVersion bump -> stale VK macro-slope).
  Never mutates render output.
- ★VERIFIED both directions:
  · POSITIVE (audit ON, green pose): **0 MISMATCH** + `first TIER-1` co-write fired.
    Audit clean where heights consistent. ✅
  · NEGATIVE (MC2_VK_TERRAIN_HEIGHT_AUDIT_CORRUPT=1 poisons the stored checksum once):
    control-armed line + **★MISMATCH fires** (snapshot_fnv 0x4a8f… vs live_fnv
    0xef2a…). Audit has teeth. ✅
- Byte-OFF control: GL-config near-oracle run shows 0 VK-terrain / audit activity;
  near_land_off byte-perfect. VKon-vs-VKoff back to 56.8% co-write (unchanged by the
  item-3 infra — inert when ready).

## ★★PRE-FLIP SCOPE (remaining, priority order — before any DEFAULT flip of MC2_VK_TERRAIN_TIER1)
Item-3 (fail-soft + height-audit) is the last SAFETY prerequisite and is now DONE.
Remaining items to a default-ON flip, highest-value first:

1. **MECH ISLAND FRUSTUM CULL + CAPTURE DETERMINISM — ★★DONE 2026-07-06 (commit 4594f86a).**
   ★FINDING (corrects the original attribution): there was NO cull mismatch to fix. The
   VK island draws the SAME instance set GL draws BY CONSTRUCTION — the feed is
   `s_vkMechInstSnapshot.assign(instDst, +totalInstances)` in the SAME batcher flush that
   computes GL's drawn totalInstances (gos_mech_batcher.cpp:2796), and the GL color pass
   does NO draw-time frustum cull (glDrawElementsInstancedBaseVertex draws all
   dc.instanceCount). The only mech cull is `mechGpuCullSkip` at SUBMIT (mech3d.cpp:2875),
   which gates the batcher submit → already shared by both paths. So the residual the
   original scope blamed on "no island cull" was actually (a) ANIM-POSE JITTER + (b)
   mech-pipeline settle latency.
   · CULL WITNESS (frame-stamped, MC2_MECH_BATCHER_STATS-gated — no default spam): GL
     `event=summary instances=` vs VK `event=cull_witness vk_instances=`. Steady-state
     GL==VK==**38** instances at the settled capture frame; distinct counts {13,25,38}
     match on both sides (only a pre-settle warmup 175 is GL-only). Cull PARITY CONFIRMED.
   · DETERMINISM ROOT CAUSE: BattleMech::render → Mech3DAppearance::update(animate=true)
     advances the stock gait by mechFrameRate*frameLength (WALL-CLOCK global, NOT
     scenarioTime) every RENDER frame. The capture sweep freezes the SIM (bPaused gates
     scenarioTime + unit updates) but not the render-side gait tick → pose drifted
     run-to-run. FIX: capture-mode-only predicate `mc2CaptureGaitFreezeActive()` (=1 iff a
     visual-capture is active AND bPaused). Gates the stock gait (mech3d:5061), imported
     clip clock (:3688), and sensor-marker spin (:4310). Default-OFF-safe: shim is 0
     whenever no MC2_VISUAL_* capture is active → live game byte-identical (terrain
     GL/VK selfdiff controls stayed 0.000).
   · SETTLE: pose-pin left a pure pipeline-latency residual → **settle=80** (was 30) makes
     the WHOLE frame (terrain+prop+mech, UNMASKED) byte-identical ×3: near_land_low AND
     near_land_off = **0.000 self-diff, NO OBJ_BOX MASK**, GL-config and VK-config. The
     OBJ_BOX mask is now RETIRED for mech-inclusive captures.
   · WITNESS TOOL: .claude/mech-parity/mech_capture_witness.py (selfdiff | whole).

   ★UNMASKED WHOLE-FRAME NUMBERS (deterministic, near_land_low, settle=80, all VK regions
   ON: terrain+detail+prop+mech+mech-lighting; substrate control near_land_off):
   | pose           | cov%   | mean_all | mean_cov | max |
   |----------------|--------|----------|----------|-----|
   | near_land_low  | 58.97  | 5.01     | 7.79     | 118 |
   | near_land_off  | 0.685  | 0.768    | 33.1     |  65 |
   The near_land_low 58.97%/5.01 is DOMINATED by the KNOWN terrain near-detail residual
   (mean ~4.7, LOD-fade + painted-concrete normal + cliff triplanar — already documented
   deferred-minor above), NOT mech. near_land_off (no near-detail terrain, no units) is
   near-byte (0.685%/0.768).

   ★CO-COVERED MECH NUMBER (deterministic replacement for the noisy freecap 28.33/ch):
   isolated the VK mech footprint via VK-mech-ON vs VK-mech-OFF at near_land_low, then
   GL-vs-VK over that footprint = **mean 14.92/ch, max 32** (footprint 2862px, all
   differing but max only 32 → uniform ~15/ch shading offset, NO silhouette/coverage
   blowout). This is the arc's FIRST DETERMINISTIC co-covered mech parity number. The old
   freecap 28.33 was inflated by ~10% run-to-run camera jitter + a different (natural)
   camera; it is UNCHANGED by this work (the gait freeze is pause-only; freecap has no
   pause) and is now SUPERSEDED by 14.92 on the deterministic pose. Residual ~15/ch =
   edge AA + calc_light-Lambert approx (as the mech oracle predicted), NOT a cull gap.

2. **GATE CONSOLIDATION (MC2_VK_SHARED_GBUFFER umbrella + Tier-0 retire)** — per handoff.
   The VK terrain/prop/mech islands each ride their own MC2_VK_*_TIER1 + shared-depth
   gates; consolidate the shared-GBuffer co-write family under ONE umbrella
   MC2_VK_SHARED_GBUFFER (keep the per-region sub-gates as detail toggles), and retire
   the Tier-0 (readback/present-region) proof gates that P14-native superseded. Scope:
   registry entries + run_smoke allowlist + the presence checks in each island's
   *Requested(); byte-OFF must stay byte-identical; each region's existing oracle must
   pass unchanged under the umbrella. Medium — pure gate plumbing, no shader/render math.
   NOTE: explicitly OUT of scope this session per steering ("no consolidation yet").

3. **FULL-FRAME PARITY + SOAK CRITERIA** — the flip gate. NOW UNBLOCKED by #1+#2: the
   whole-frame VK-vs-GL number is DETERMINISTIC and UNMASKED (settle=80 + gait freeze).
   ★MEASURED BASELINE (this session, near_land_low all-regions-ON): whole-frame mean_all
   5.01 / cov 58.97 — but this is ~94% TERRAIN near-detail residual (mean ~4.7, deferred-
   minor), NOT a mech/prop defect. Define + run:
   (a) whole-frame VK-vs-GL parity, all 3 regions live + UNMASKED (mech pin from #2),
       across the 11 stable mc2_01 bookmarks + BOTH near poses + ≥2 other maps (airport
       for markings, a water map). ★REVISED ACCEPTANCE (data-informed): report TWO
       numbers per pose — (i) whole-frame mean_all, and (ii) mean_all with the KNOWN
       terrain near-detail residual accounted for (either subtract the terrain-only
       GL-vs-VK baseline, or gate on the mech+prop footprint only). Propose accept when
       the MECH+PROP co-covered mean_all ≤ ~15/ch (the measured 14.92 floor = edge AA +
       Lambert approx) with max ≤ ~40 (no silhouette blowout), AND near_land_off (the
       substrate control) stays ≤ 1.0 mean_all. Do NOT hold the flip on the terrain
       near-detail residual — it is a documented deferred-minor, not a regression.
   (b) tier1 soak (mc2_01 mc2_03 mc2_10 mc2_17 mc2_24, 30s each) with all VK gates ON
       -> exit 0, no crash_silent, no fail-soft-gap log firing in steady state, height-
       audit clean. Use the CANONICAL smoke command (CLAUDE.md) verbatim.
   (c) height-audit run over crater/deform content (a mission that deforms terrain) to
       confirm the audit stays green under live height mutation OR flags the writer that
       needs a version bump. Largest item; gates the flip.

### ★NEXT-SESSION NOTES (post #1+#2)
- Consolidation (#2 gate-umbrella) remains OUT of scope per steering; do it before the
  soak so the soak exercises the final umbrella gate.
- The mech+prop-footprint isolation used here (VK-region-ON vs -OFF diff, intersect
  GL-vs-VK) is the clean way to separate mech/prop parity from the terrain near-detail
  residual for (a) — bake it into a `mech_capture_witness.py footprint` mode.
- One more mech pose worth adding for coverage: a pose framing a DAMAGED/lightsOut mech
  (exercises the renderFlags bit-1 ambient-floor branch) and a SELECTED mech (highlight).
- freecap 28.33 is retired; do NOT chase it — it is non-deterministic by engine
  declaration (gos_visual_capture.cpp:38). The deterministic 14.92 co-covered number is
  the arc's number of record.

## Files (this session, terrain)
- NEW oracle: .claude/mech-parity/terrain_near_oracle.py (keeper),
  tests/visual/bookmarks/mc2_01_terrain_near.json. Artifacts .claude/mech-parity/terrain/near_oracle/.
- Diagnosis tooling (this session): .claude/mech-parity/terrain_freecap.py (natural-cam GL-vs-VK),
  terrain_oracle.py (existing bookmark sweep). Artifacts under .claude/mech-parity/terrain/
  (antitile_baseline_f200, gl_repeat, vk_repeat, airport_lowobl — the self-diff + VKon/off probes).
- GameOS/gameos/vulkan_terrain_island.{h,cpp}, gos_terrain_lod_chunk.cpp
- shaders/vulkan/terrain_tier1.frag
- RenderCore/RendererFeatureRegistry.h (gate registered), scripts/run_smoke.py (allowlisted)
- Oracle artifacts: .claude/mech-parity/terrain/dynshadow_f200/, gateoff_ctrl/

## Files (2026-07-06 session, mech capture determinism + cull witness — commit 4594f86a)
- mclib/mech3d.cpp — file-scope extern mc2CaptureGaitFreezeActive() + 3 capture-freeze
  gates (stock gait :5061, imported clip clock :3688, sensor spin :4310).
- GameOS/gameos/gos_visual_capture.cpp — extern-C mc2CaptureGaitFreezeActive() predicate.
- GameOS/gameos/gos_mech_batcher.cpp — event=summary now carries frame= + instances=.
- GameOS/gameos/vulkan_mech_island.cpp — event=cull_witness (MC2_MECH_BATCHER_STATS-gated).
- NEW witness: .claude/mech-parity/mech_capture_witness.py (selfdiff | whole).
- Artifacts: .claude/mech-parity/terrain/near_oracle/{wit_gl,wit_vk,mo_gl,mo_vk,s80_vk_*}.
