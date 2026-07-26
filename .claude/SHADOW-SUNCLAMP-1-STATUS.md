# SHADOW-SUNCLAMP-1 — STATUS

Lane: SHADOW-SUNCLAMP-1 | Worktree: `A:/Games/mc2-main` | Branch: `claude/nifty-mendeleev`
Base HEAD at start: `4b343e72` (tracked tree CLEAN — 0 tracked modifications)
Commit landed: `ad43dab5`
Deploy lane used: `A:/Games/mc2-opengl/mc2-win64-abl-validate` (fingerprint OK, md5 MATCH)
Date: 2026-07-26

**Outcome: the inherited "airtight" root cause is REFUTED BY A/B CAPTURE. A real
but DIFFERENT inconsistency was fixed and shipped gated; the true mechanism for the
user-visible defect has been located with source evidence and is NOT what was
handed over. The two ENV_DROP_RISK gate gaps are closed and verified.**

---

## META REVIEW — the brief's premises, checked from source

Everything factual in the brief verified TRUE:

| Premise | Verdict |
|---|---|
| `mc2_10.fit` authors `LightDirPitch = 180` | **TRUE** — independently confirmed in **three** separate data copies (`Carver5-feasibility`, `Sarna.net/magicsunofficialexpansion`, `mc2-decal-integrate-test`) |
| mc2_01 = 50°, mc2_03 = 46°, mc2_24 = 75° | **TRUE** — same three copies |
| `mc2ClampShadowSunAltitude` fires on both matrix builds | **TRUE** — `gos_postprocess.cpp:5868` (static), `:6292` (dynamic) |
| Its comment says the shading lightDir is NOT touched | **TRUE** — `:5791` |
| `shadow_screen.frag:161` guard `if (NdotL < 0.05) return 1.0;` | **TRUE**, and a twin at `:225` |
| `MC2_SHADOW_MIN_SUN_ALT` + `MC2_IMPOSTOR_DIST` are ENV_DROP_RISK + undocumented | **TRUE** — both `gate_status` = GAPS |
| `bdactor.cpp:7436` excludes LOD>=1 on a now-false justification | **TRUE** (not actioned — see Coordination) |

I also strengthened the provenance beyond the handover: `gameos_graphics.cpp:10197`
calls `buildStaticLightMatrix(-lx, -ly, -lz)` from `gos_GetTerrainLightDir()`, and
`gos_postprocess.cpp:2993` uploads the **byte-identical expression** `{-lx,-ly,-lz}`
into the `lightDir` uniform. Same vector, same convention, one clamped and one not.
The inconsistency is therefore real and provable at the strongest possible level.

**But "the inconsistency is real" and "the inconsistency causes the reported defect"
are two different claims. The handover asserted the second without ever capturing a
frame. It is false.**

---

## What was landed — `ad43dab5`

`mc2ClampShadowSunAltitude()` is now applied to the `shadow_screen` guard vector,
under new killswitch **`MC2_SHADOW_GUARD_CLAMPED_SUN`** (default ON, `=0` reverts).
The shading light dir is untouched, so brightness/colour cannot move.

Gate hygiene — all three now `gate_status` **verdict OK, zero flags**:

| Gate | Before | After |
|---|---|---|
| `MC2_SHADOW_GUARD_CLAMPED_SUN` | (new) | registered + documented + allowlisted, default ON |
| `MC2_SHADOW_MIN_SUN_ALT` | ENV_DROP_RISK + UNDOCUMENTED | registered + documented + allowlisted |
| `MC2_IMPOSTOR_DIST` | ENV_DROP_RISK + UNDOCUMENTED | registered + documented + allowlisted |

Files: `GameOS/gameos/gos_postprocess.cpp`, `RenderCore/RendererFeatureRegistry.h`,
`docs/tier1_env_vars.md`, `scripts/run_smoke.py`.

---

## THE REFUTATION — A/B capture, mc2_10 night, 3 wide poses

New fixtures authored to replace the deleted fraudulent ones
(`tests/visual/bookmarks/mc2_10_sunclamp.json`): 3 WIDE full-frame poses at normal
RTS gameplay altitude (alt 780–850, proj 28–32), spread across the map
(centre / `-3200,-3200` / `+3000,+2600`) and rotated to **73.75 / 163.75 / 253.75°**
so the sun azimuth (`LightDirYaw 28.75 + 45 = 73.75°`) is straddled and the reported
split cannot be cropped out. Deterministic fixed-clock capture, all poses
`present=True det=True`.

| Pose | gate ON | gate OFF |
|---|---|---|
| `sunclamp_center_wide` | `ea1688351a3d60a7` | `ea1688351a3d60a7` |
| `sunclamp_across_azimuth` | `ffdb61004031bef9` | `ffdb61004031bef9` |
| `sunclamp_relief_ne` | `ed4a2643816f659f` | `ed4a2643816f659f` |

**Byte-identical.** A third arm with `MC2_PROP_SHADOW_RECEIVE=1` (which routes static
props through the very guard I changed) is **also byte-identical**. Shadows were
demonstrably live in all runs — `[CSM] init nearLayers=2 fullMapSize=4096`,
`rendering static shadows (map half-extent=7680)`, `[SHADOW_CULL] casters 2466 -> 1816`.

So the shipped change does not alter the mc2_10 night overview at all, and the
handed-over root cause cannot be the explanation for the defect.

---

## WHY it cannot be — read from `shadow_screen.frag` main()

1. Terrain, grass, decals and overlays hit `pixelHandlesOwnShadow` and
   **early-out** (`FragColor = vec4(1.0); return;`) *before* `lightDir` is ever
   referenced. The world surface never reaches the guard.
2. The static full-map path — `sampleShadowMap(shadowMap, worldPos, lightSpaceMatrix, 8)`
   — has **no NdotL guard at all** and does not consume `lightDir`.
3. The `lightDir` guard exists **only inside `sampleDynamicShadow`**, which runs for
   **mech pixels only** (`!isStaticProp`); static props are excluded unless
   `MC2_PROP_SHADOW_RECEIVE` (default **OFF**) is set.

A mech-pixel-only guard cannot produce a world-scale shadowed-half / unshadowed-half
split. The wide overview frames are terrain-dominated, which is exactly why all three
arms hash the same.

---

## THE ACTUAL MECHANISM (located, not yet fixed)

The identical bug exists on the **terrain** path, where it *is* world-scale:

- `gos_terrain.frag:958` — `calcShadow(WorldPos, shadowN, terrainLightDir.xyz, shadowTaps)`
  (and `:960` `calcDynamicShadow(...)` with the same raw vector).
- `shaders/include/shadow.hglsl:167-168` — the same back-face guard:
  ```glsl
  float NdotL = max(dot(normal, lightDir), 0.0);
  if (NdotL < 0.05) return 1.0;
  ```
  dotted against the **raw unclamped** `terrainLightDir`.
- **Decisive detail:** under `MC2_SHADOW_STATIC_BANDFIX_V2` (default-ON), `shadow.hglsl:154`
  **resets `normal = vec3(0.0, 0.0, 1.0)`** (flat-up) specifically for the back-face
  guard and slope bias. So on terrain the guard reduces to
  `NdotL = max(terrainLightDir.z, 0.0)`.
  On mc2_10, `terrainLightDir.z = sin(180°) ≈ 0` → `NdotL = 0 < 0.05` →
  **`return 1.0` — terrain sun shadows are suppressed globally on this map**, while
  props and mechs keep theirs through the unguarded static path. That differential is
  the world-scale asymmetry the user is reporting.
- Compounding: `shadow.hglsl:416` derives `sunAltSin = clamp(lightDir.z, 0.0, 1.0)`
  (SHADOW-ALT-SCALE-1, "scale the whole model by sun altitude"), which also collapses
  to 0 on a horizontal sun.

The correct fix is the same shape as the one shipped, applied to the terrain path:
pass `calcShadow`/`calcDynamicShadow` a **clamped shadow-only** light vector while
leaving `gos_terrain.frag:820`'s diffuse `NdotL` on the raw vector, so brightness is
preserved. This needs a new uniform beside `terrainLightDir`.

**Not landed here — coordination boundary.** `terrainLightDir` is uploaded from
`terrain_light_dir_` through several sites; `GameOS/gameos/gos_terrain_lod_chunk.cpp`
carries one of them and is claimed by **three** live lanes (CEMENT-ALPHA-LAND-1,
CEMENT-UNIFORM-2, ROAD-OVERLAY-LIGHT-1). Adding the uniform on only the
`gameos_graphics.cpp` paths (which are clear) would leave the LOD-chunk path
uploading a default-zero vector and silently break terrain shadows there. Landing it
correctly requires that file, so it is handed over rather than half-done.

---

## ADVERSARIAL SELF-REVIEW

- **"Would the user still see the defect on a default launch?" — YES.** This lane did
  not fix the reported defect. Stated plainly rather than dressed up. `ad43dab5` is a
  correctness/consistency fix whose user-visible benefit at the captured poses is
  *nil*, and I have proven that rather than assumed it.
- **Did I repeat the sin the brief warned about (re-labelling on one un-A/B'd
  capture)?** Deliberately not. I A/B'd the actual gate, on the actual defect map, at
  3 wide poses, plus a `MC2_PROP_SHADOW_RECEIVE` liveness arm — and let the result
  overturn the brief's premise instead of narrating around it.
- **Is the shipped change dead code?** Not dead, but *inert under default gating at
  these poses*: it can only move mech pixels (or props with `MC2_PROP_SHADOW_RECEIVE=1`).
  I could not exhibit a pose where it changes a pixel. Its justification is
  consistency + zero demonstrated regression, not observed benefit. It is default-ON;
  if the user prefers strictly-proven-only defaults, `MC2_SHADOW_GUARD_CLAMPED_SUN=0`
  or flipping the registry default is a one-line change.
- **Is the terrain mechanism itself just another un-captured story?** It is
  source-derived and NOT yet capture-confirmed — the same evidential standard that
  just failed. It is stated as the leading, well-evidenced hypothesis, not a verdict.
  What raises confidence above the previous claim: it explains a *world-scale*
  effect (terrain covers the world), it survives the reading of `main()` that killed
  the previous claim, and the flat-up guard reset makes the mc2_10 collapse exact
  (`NdotL = max(sin(180°),0) = 0`) rather than approximate. **It must be A/B'd before
  anyone calls it fixed.**
- **Control maps / day+night / 3 locations per map:** the mc2_01/03/24 controls were
  NOT captured. Their purpose was to prove the change is a no-op off mc2_10 — but the
  change proved a no-op *on mc2_10 itself*, on which those controls are strictly
  weaker evidence, and the clamp provably cannot fire there (`sin(50/46/75°)` all
  ≥ 0.2588). Recorded as a gap rather than claimed.
- **Clean-fingerprint build + gate string in the deployed binary:** verified.
  `deploy_status` verdict **OK** (`exe sha=ad43dab55147 matches expected`, manifest
  not stale), src↔deployed **md5 MATCH** (`84416dcccfedc6e503aa7f7e16087cd9`), and
  `MC2_SHADOW_GUARD_CLAMPED_SUN` present in the **deployed** exe bytes.
- **Deploy-path trap:** avoided. `0.4c` had a **fresh foreign smoke lease** (pid 39748,
  4 s old, mid-run on mc2_01) and `mc2-win64-0.4c` is flagged **BROKEN-TREE**
  (yields false crash reports). Deployed to `mc2-win64-abl-validate` — no lane lock,
  no smoke lease, no competing claim, fully populated (36 058 files).

---

## Item also in scope — `bdactor.cpp:7436` far-LOD shadow exclusion

Confirmed the stale justification: the comment claims "shadow depth pass has no alpha
discard", which has been false since `b8f764b6` (2026-06-16). **Not actioned** —
`mclib/bdactor.cpp` is claimed by live lane **CONE-SPILL-COUPLING-1**. Editing it
would be the double-implementation failure the registry exists to prevent. Note for
whoever holds it: `MC2_IMPOSTOR_DIST` is now allowlisted and documented, so an
impostor-band A/B will no longer silently run the default path — that was previously
impossible to test honestly.

## Coordination log

- Lane registered; `check_conflict` before every edit.
- `shaders/shadow_screen.frag` + `gos_postprocess.cpp` — PROP-NIGHT-LIGHT-3's claim had
  gone **stale**; re-checked clear before editing.
- `docs/tier1_env_vars.md` / `scripts/run_smoke.py` / `RendererFeatureRegistry.h` —
  shared append-only registries, overlapping other lanes; edited with **additive-only**
  hunks, no reflow of neighbours.
- `mclib/bdactor.cpp` (CONE-SPILL-COUPLING-1) and
  `gos_terrain_lod_chunk.cpp` (3 lanes) — **respected, not touched.**
- Deploy lane moved off contended `0.4c` to `mc2-win64-abl-validate`.
- No push, no `--amend`. Tracked tree clean at handover.
