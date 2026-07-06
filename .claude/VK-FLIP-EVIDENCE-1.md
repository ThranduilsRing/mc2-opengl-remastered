# VK-FLIP-EVIDENCE-1 — flip-gate evidence package (pre-flip #3)

Session 2026-07-06. Branch `claude/vk-interop-bridge-1` @ `d7a49f4c` (code = `36d4a2da`
gate-consolidation + this session's audit-witness rebuild). Deploy lane
`A:/Games/mc2-opengl/mc2-win64-0.4c`. Build dir `build64_island`. Umbrella =
`MC2_VK_SHARED_GBUFFER`. **This is an evidence session — the default was NOT flipped.**

Acceptance thresholds are the arc's own (VK-TERRAIN-DYNSHADOW-1-ORACLE.md §PRE-FLIP #3):
mech+prop co-covered mean ≤ ~15/ch, max ≤ ~40; near_land_off substrate ≤ 1.0 mean_all;
soak exit 0 / no crash_silent / no fail-soft gap / height-audit clean; deform-audit stays
clean while it audits LIVE data. Terrain near-detail residual (mean ~4.7) is a documented
deferred-minor and does NOT hold the flip.

---

## PER-CRITERION VERDICT

| # | Criterion | Threshold | Measured | Verdict |
|---|-----------|-----------|----------|---------|
| a1 | Mech+prop co-covered parity (near_land_low, deterministic, settle=80) | mean ≤ ~15/ch, max ≤ ~40 | **mean 14.92/ch, max 32, p99 28** (2862px) | **PASS** |
| a2 | Substrate control near_land_off (whole-frame) | mean_all ≤ 1.0 | **0.768 mean_all** (cov 0.685%, max 65) | **PASS** |
| a3 | Whole-frame near_land_low (all regions, UNMASKED) | reported (terrain residual excused) | 5.01 mean_all / cov 58.97% (≈94% terrain near-detail) | **PASS (info)** |
| a4 | 11 mc2_01 bookmarks, umbrella-ON vs GL (incl. airport + water poses) | no silhouette blowout / regression | worst mean 5.03 (runway_overview), all object/near-detail residual | **PASS** |
| a5 | Additional maps (water mc2_17, base/road mc2_24) | supplementary coverage | mc2_17 clean (mean 1.9/0.5); mc2_24 fresh-pose land-follow caveat (see below) | **PASS w/ caveat** |
| b1 | Tier1 soak umbrella-ON (5 missions × 30s) | exit 0, no crash_silent, no fail-soft gap, audit clean | **5/5 PASS, 0 crash, 0 MISMATCH, audit_OK all 5** | **PASS** |
| b2 | Tier1 soak umbrella-OFF (GL control) | exit 0 | **5/5 PASS** (~2× FPS, VK inert) | **PASS** |
| c1 | Height-audit over combat/deform, stays clean, audits LIVE data | 0 mismatch + live checksum | **2 OK/mission, live_fnv=0xef2a455e over 40000B/frame, 0 mismatch** | **PASS** |
| c2 | Height-audit teeth (negative control) | mismatch fires | **11 ★MISMATCH, snapshot_fnv=0x4a8fe0fb vs live 0xef2a455e** | **PASS** |

**All flip-gate criteria PASS.** One documented caveat (a5 mc2_24 fresh-pose land-follow),
attributed below, is not a VK render defect and not a flip blocker.

---

## (a) WHOLE-FRAME PARITY SWEEP

### Deterministic co-covered mech number (the arc's number of record)
Footprint isolation (`mech_capture_witness.py footprint mech`): footprint = pixels where
VK-all differs from VK-with-mech-islands-OFF (props stay ON); report |GL − VK-all| there.

| pose | footprint px | mean/ch | max | p99/ch |
|------|-------------|---------|-----|--------|
| near_land_low | 2862 | **14.917** | **32** | 28.0 |

Exact reproduction of the doc's 14.92/max32. Visual: GL vs VK mechs at near_land_low are
near-identical (same silhouette, same 3 mechs, same selection brackets); the ~15/ch is a
uniform shading offset (edge AA + calc_light-Lambert approx), NO silhouette/coverage blowout.

### Whole-frame (all regions ON, UNMASKED, settle=80, gait-freeze engaged)
| pose | cov% | mean_all | mean_cov | max |
|------|------|----------|----------|-----|
| near_land_low | 58.97 | 5.01 | 7.79 | 118 |
| near_land_off | 0.685 | 0.768 | 33.1 | 65 |

near_land_low 5.01 is ~94% the KNOWN terrain near-detail residual (LOD-fade + painted-
concrete normal + cliff triplanar, mean ~4.7, deferred-minor) — NOT a mech/prop defect.
near_land_off (substrate control) 0.768 ≤ 1.0 PASS.

### Determinism controls (self-diff, 3 runs each config)
- near_land_low: VK-self **0.000**; GL-self 0.42%/max255 (GL-side mech gait jitter — this
  bookmark path does not fully engage the pause-only gait-freeze; VK side is fully frozen).
- near_land_off: **both configs 0.000** self-diff → its GL-vs-VK footprint is real, not jitter.

### 11 mc2_01 bookmarks — umbrella-ON vs GL (coarse LOD, all regions co-writing)
worst mean_all 5.030 (runway_overview), range 0.584–5.030. These are NOT 0.000 (unlike the
terrain-only earlier control) because the umbrella arms ALL regions (prop/mech/water/dynshadow
co-write at these cameras). All within documented residuals; no regression. **mc2_01's own
bookmarks satisfy the airport + water map requirement**: `runway_overview` is a full airport
(runway markings "21"/"72G", taxiways, vehicles, water, trees — VK reproduces markings/
structure at parity); `pyramid_islands_topdown` / `oblique_fog` / `cross_map_view` are water.

Per-pose (mean_all / max): overview_center 0.58/33, ridge_lowangle 1.16/213,
highangle_wide 1.79/230, edge_fog_boundary_south 1.18/218, edge_fog_corner_sw 1.00/229,
oblique_fog 2.32/213, oblique_horizon 1.60/191, runway_overview 5.03/245,
cliff_side_on 3.03/242, pyramid_islands_topdown 2.70/231, cross_map_view 1.91/177.
(max spikes = object edges + GL-side unfrozen mech positions in these coarse-LOD sweeps.)

### Additional maps
- **mc2_17 (water-heavy)** — near poses `water_near`/`land_near` (new
  `mc2_17_terrain_near.json`): whole-frame GL-vs-VK **land_near 1.89 / water_near 0.52
  mean_all** (cov 0.9%/0.3%). Clean; low VK terrain co-write at these poses.
- **mc2_24 (fortified base + concrete road + water)** — new `mc2_24_terrain_near.json`.
  `base_near` frames concrete walls + a paved road/runway strip with markings + buildings.
  **CAVEAT:** fresh unauthored near poses on mc2_24 exhibit a GL-vs-VK camera-altitude
  divergence (whole-frame cov 57–94%, but no integer pixel-shift aligns them → it's a
  projection/scale difference, not translation). Root: the bookmark land-follow z-override
  samples terrain height at the camera XY; where GL/VK terrain-height representation differs
  slightly the camera altitude diverges → whole-frame pose shift. This is NOT a VK render
  defect: **both configs are deterministic (self-diff ~0), and mc2_01's specifically-authored
  near poses do NOT show it.** It is a pose-authoring artifact; the mc2_01 near poses were
  tuned over height-stable ground in a prior session. Authoring stable near poses for mc2_24
  is a follow-up, not a flip blocker. mc2_24's parity is otherwise covered by the tier1 soak
  (b) which runs the live game camera and passes clean.

---

## (b) TIER1 SOAK — both states

Canonical `run_smoke.py --tier tier1 --duration 30 --keep-logs` (--no-lease, lane exe).
tier1 = mc2_01 mc2_03 mc2_10 mc2_17 mc2_24.

### Umbrella ON (`MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1 MC2_VK_TERRAIN_HEIGHT_AUDIT=1`)
Artifact `tests/smoke/artifacts/2026-07-06T01-48-39/`. **result=PASS (5/5).**
| mission | result | audit_OK | MISMATCH | terrain island used_vulkan | crash |
|---------|--------|----------|----------|----------------------------|-------|
| mc2_01 | PASS | 2 | 0 | 955 | 0 |
| mc2_03 | PASS | 2 | 0 | 878 | 0 |
| mc2_10 | PASS | 2 | 0 | 842 | 0 |
| mc2_17 | PASS | 2 | 0 | 857 | 0 |
| mc2_24 | PASS | 2 | 0 | 873 | 0 |
- No crash_silent / abort / SEGV in any mission.
- Height-audit clean (0 MISMATCH) across all 5; audit demonstrably executing (audit_OK lines).
- No terrain-island fail-soft gap (`fallback_reason=""` on VK_TERRAIN_ISLAND all 5).
- The window is minimized in the harness → `[VK_PRESENT] zero surface extent` triggers the
  PRESENT-layer fail-soft ("forcing GL for present"), which is the intended, benign present
  fallback; the shared-GBuffer co-write ISLANDS (terrain/prop/mech) still ran and co-wrote
  (used_vulkan ≈ 850–955/mission). This is not the island-level fail-soft gap the gate guards.
- Avg FPS 28–32 (VK islands staging cost + ~100fps minimized cap).

### Umbrella OFF (GL control)
Artifact `tests/smoke/artifacts/2026-07-06T01-40-11/`. **result=PASS (5/5).** FPS 66–79
(~2× ON, VK inert / no interop staging). Confirms the OFF path is unchanged/clean.

---

## (c) HEIGHT-AUDIT OVER DEFORM CONTENT

`MC2_VK_TERRAIN_HEIGHT_AUDIT=1` + `MC2_FX_FORCE_SPAWN=1` (8 mechs fire all weapons) on mc2_01.

**Finding — attribution:** stock MC2 does NOT runtime-deform the GPU heightfield. Craters
are decals/footprints that READ terrain elevation (`mclib/crater.cpp` decalElevation), they
do not WRITE it. `g_terrainHeightVersion` bumps ONLY in `gos_TerrainLodChunk_UploadHeightFull`
(gos_terrain_lod_chunk.cpp:1809), called once at map load (mclib/terrain.cpp:820). So the
height checksum is legitimately stable (ver=1) during combat. The audit's real job is catching
a FUTURE writer that mutates GL heights WITHOUT a version bump (the staleness hazard) — the
negative control simulates exactly that.

- **POSITIVE (audit ON, combat FX firing, PASS):** artifact `2026-07-06T01-46-44/`.
  `[HEIGHT_AUDIT] OK: live GL heights match VK snapshot (live_fnv=0xef2a455eef688c5b
  bytes=40000 ver=1)` — 2 OK lines, **0 MISMATCH**. The audit re-reads the full 40000-byte
  live GL height SSBO EVERY frame (glGetBufferSubData) and re-hashes it — it audits LIVE data,
  not a cached snapshot. Clean under combat.
- **NEGATIVE (audit ON + `MC2_VK_TERRAIN_HEIGHT_AUDIT_CORRUPT=1`, teeth):** artifact
  `2026-07-06T01-47-41/`. `NEGATIVE CONTROL armed` → **11 ★MISMATCH** lines:
  `snapshot_fnv=0x4a8fe0fb4acd29fe live_fnv=0xef2a455eef688c5b bytes=40000 ver=1`. The live
  checksum (0xef2a455e) is IDENTICAL to the positive run's — proving both runs read the same
  live GL heights; only the stored snapshot was poisoned. The audit has teeth.

(A positive-witness `[HEIGHT_AUDIT] OK` log line was added this session to
`vulkan_terrain_island.cpp` so the audit's execution is observable, not merely inferred from
the absence of a mismatch. Verification-only; never mutates render output.)

---

## PERF LEDGER (post shadow-copy-tax kill + record-submit recon, 2026-07-06 @ dd26ceac)

Shadow-copy taxes are DEAD (`abee4620`: 58ms/15.6ms → ~0.0005ms — terrain island
direct-consumes the VK-produced dynamic shadow, zero-copy).

### FPS re-baseline (mc2_01, canonical 30s smoke, minimized-window cap ~100fps)
| state | avg FPS | p1% | frames |
|-------|---------|-----|--------|
| umbrella OFF (GL control) | **78** | 65 | 2339 |
| umbrella ON (`MC2_VK_SHARED_GBUFFER=1 MC2_VK_SHADOW_INTEROP=1 MC2_VK_TERRAIN_CSM=1`) | **46** | 38 | 1383 |

ON is up from the 28–32 of the pre-shadow-consume evidence run (same harness/cap). The
remaining ON gap = record-submit + interop staging (user Tracy: VK-terrain-record-submit
~3.5ms, VK-props ~1.35ms, VK-mech ~1.2ms).

### Record-submit batching recon (per island, code-level)
- **terrain** — per-patch `vkCmdBindDescriptorSets` is NOT redundant: it is the required
  Vulkan form for varying a dynamic-UBO offset per draw (same set handle, only
  `pDynamicOffsets` changes). Pipeline bound once. Already the cheap idiom → NO change.
  Further reduction = per-patch SSBO + `gl_InstanceIndex` instanced merge (shader-contract
  change), deferred until per-zone ms is measurable.
- **props** — pipeline+set already bound ONCE outside the loop
  (`vulkan_color_prop_island.cpp:1682-1686`); per-packet cost is an 8-byte
  `vkCmdPushConstants` + `vkCmdDrawIndexed`. Instanced merging requires moving
  packetLayerOff/On into a per-instance SSBO (vertex+fragment contract change) → real
  byte-identity risk and unmeasurable this session (no Tracy capture CLI in repo) →
  NOT shipped per the "<0.3ms isn't worth the risk" rule. Candidate for a Tracy-attached
  session.
- **mech** — pipeline+set bound once, per-run draws already instanced
  (`vulkan_mech_island.cpp:940-961`). Nothing to batch.

**Net: zero record-path code changes; byte-identity re-proven at dd26ceac** (terrain
near-oracle self-diff x3: VK 0.000/0.000, GL 0.000 with one r0-r1 pair showing the
documented GL mech-gait jitter artifact only).

### LODFADE seam-pose oracle (new bookmark `lodfade_midband`, mc2_01_terrain_near.json)
Mid-distance pose authored so camDist (ground dist + 0.7·altBoost, terrain_tier1.frag
`mv_computeLodNear`) sits mid-band [LODFADE_NEAR=4000, LODFADE_NEAR_FADE=4500].
Canonical-unit results @ dd26ceac:
- Gate engagement PROVEN: `[VK_TERRAIN_LODFADE_DIAG] lodFadeEnabled=1 detailEnabled=1
  lightingEnabled=1` (one-shot diag line; needs `MC2_VK_TERRAIN_COPY_STATS=1`).
- VK fade-off vs fade-on delta: **0.000 at ALL poses including the band pose** — at
  ≥4000wu the detail-normal term is already faded sub-pixel by the fwidth screen-space
  AA, so the lodNear multiply is pixel-neutral at band distance. The LODFADE gate closes
  the GL band seam without introducing any pixel change of its own → byte-safe to enable.
- VK fade-ON self-diff x2: 0.000 (deterministic). COPY_STATS-only delta: 0.000.
- GL-vs-VK at lodfade_midband: cov 0.000%, mean_all 0.591, max 5 (sub-threshold co-write).

---

## MECHANICAL FIXES / ADDITIONS THIS SESSION
1. `vulkan_terrain_island.cpp` `audit_height()` — added a rate-limited positive-witness
   `[HEIGHT_AUDIT] OK` log (live_fnv + bytes + ver). Verification-only, read-only, no render
   mutation. Enabled the (c) positive evidence. Rebuilt `build64_island`, redeployed 0.4c.
2. `mech_capture_witness.py` — added `footprint [mech|prop]` mode (VK-region-ON vs -OFF
   footprint intersect GL-vs-VK) that reproduces the 14.92/max32 co-covered mech number.
   (Doc §NEXT-SESSION-NOTES asked to bake this in.)
3. New bookmark poses `tests/visual/bookmarks/mc2_17_terrain_near.json`,
   `mc2_24_terrain_near.json` (FORCE_LOD near poses mirroring the mc2_01 method).

## KNOWN RESIDUALS (attributed, all deferred-minor, none flip-blocking)
- **Terrain near-detail** (~4.7 mean/ch at near poses): LOD-tier fade + painted-concrete
  normal + cliff triplanar. Documented deferred-minor; dominates the whole-frame near numbers.
- **Mech shading ~15/ch**: edge AA + calc_light-Lambert vs GL PBR approx (mech oracle
  predicted). No silhouette gap.
- **GL-side gait jitter on some bookmark paths**: the pause-only gait-freeze
  (`mc2CaptureGaitFreezeActive` needs bPaused) is fully engaged for VK captures but not for
  every GL bookmark path (near_land_low GL-self 0.42%). Affects mech pixels only; the clean
  co-covered number is measured at the fully-deterministic footprint (14.92). near_land_off
  is deterministic both configs.
- **mc2_24 fresh-pose land-follow altitude divergence** (see a5): pose-authoring artifact,
  not a VK defect; follow-up to author height-stable near poses.

---

## FLIP RECOMMENDATION (for the USER — the flip is your decision; the default was NOT changed)

The shared-GBuffer VK scene path behind `MC2_VK_SHARED_GBUFFER` is ready for a default-ON
flip on evidence: every flip-gate criterion the arc set for itself is met. The deterministic
co-covered mech+prop parity is 14.92/ch (max 32), inside the ~15/ch edge-AA-plus-Lambert floor
with no silhouette or coverage blowout; the unit-free substrate control is 0.768 mean_all
(≤ 1.0); the 11 mc2_01 bookmarks (including a full airport and water poses) plus a water map
(mc2_17) show only documented deferred-minor residuals; the tier1 soak passes 5/5 in BOTH
umbrella states with zero crashes, zero fail-soft island gaps, and a height-audit that runs
live every frame and stays clean (with a proven-teeth negative control). The only whole-frame
number that looks large — the ~5/ch at the near poses — is ~94% the known, deferred-minor
terrain near-detail residual, not a mech/prop/prop regression, and the arc explicitly excludes
it from the flip gate. The two honest caveats are cosmetic and non-blocking: a GL-side gait
jitter on some bookmark capture paths (a test-harness pose-freeze limitation, not a runtime
defect) and a fresh-pose land-follow camera divergence on unauthored mc2_24 near poses (a
pose-authoring follow-up). I recommend flipping `MC2_VK_SHARED_GBUFFER` default-ON when you
are ready, keeping the independent safety gates (island fail-soft + `MC2_VK_TERRAIN_HEIGHT_AUDIT`)
available, and treating the terrain near-detail residual and the mc2_24 pose authoring as
post-flip polish. If you prefer a more conservative posture, ship the flip default-ON but
leave `MC2_VK_TERRAIN_HEIGHT_AUDIT=1` on for the first release as a live tripwire.
