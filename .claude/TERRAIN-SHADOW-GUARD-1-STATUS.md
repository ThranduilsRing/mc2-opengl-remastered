# TERRAIN-SHADOW-GUARD-1 — STATUS

Lane: TERRAIN-SHADOW-GUARD-1 | Worktree: `A:/Games/mc2-main` | Branch: `claude/nifty-mendeleev`
Base HEAD at start: `6772323e` (tracked tree CLEAN)
Commits landed: `1b23a82b` (fix), `c63fec74` (shader-permutation inventory row)
Deploy lane used: `A:/Games/mc2-opengl/mc2-win64-abl-validate` (DEPLOY_FINGERPRINT OK, `c63fec7406e0`)
Date: 2026-07-26

**Outcome: the handed-over root cause is CONFIRMED — by capture, not by source argument.
The terrain shadow-factor buffer on mc2_10 night was a flat, saturated 1.0 across the
entire frame with the guard on the raw sun, and resolves into real relief-following
terrain shadowing with the guard on the clamped sun. Fix landed gated, default ON.**

---

## META REVIEW — premises re-checked from source (not from status docs)

| Premise | Verdict |
|---|---|
| `gos_terrain.frag:958/960` pass raw `terrainLightDir.xyz` to `calcShadow`/`calcDynamicShadow` | **TRUE** (read directly) |
| `shadow.hglsl` back-face guard `NdotL = max(dot(normal,lightDir),0); if (NdotL < 0.05) return 1.0;` | **TRUE** |
| `shadow.hglsl` **resets `normal = vec3(0,0,1)`** immediately before that guard | **TRUE**, inside `#ifdef MC2_SHADOW_STATIC_BANDFIX_V2` |
| `MC2_SHADOW_STATIC_BANDFIX_V2` is default **ON** | **TRUE** (`gameos_graphics.cpp:259`, `docs/tier1_env_vars.md:217`) |
| `mc2ClampShadowSunAltitude` clamps the MATRIX sun only, 15° default | **TRUE** (`gos_postprocess.cpp:5844`, comment at `:5791`) |
| Clamp engages only where `sin(pitch) < 0.2588` → mc2_10 alone | **TRUE** — and now *empirically* confirmed: 14 control poses on mc2_01/03/24 are byte-identical across the A/B |
| `bdactor.cpp:~7449` excludes LOD>=1 from casting on a now-false justification | **TRUE** — see Deferred |

**Correction to the handover:** the brief described the guard as affecting only
`gos_terrain.frag`. It is broader — `decal.frag`, `gos_grass.frag`,
`terrain_overlay.frag` and `terrain_lod_chunk.frag` all call the same two functions
with `terrainLightDir`. The fix was therefore placed **inside `calcShadow` /
`calcDynamicShadow`** rather than at the terrain call site, so every world-surface
consumer is cured at one chokepoint.

---

## What was landed — `1b23a82b`

`shadow.hglsl` gains `mc2ShadowClampSunAltitude()`, an exact GLSL mirror of the C++
clamp, applied to `lightDir` at the top of the three primary shadow entry points
(`calcShadow`, both `calcDynamicShadow` variants). The clamp magnitude arrives as a
**compile-time define**, `MC2_SHADOW_MIN_SUN_SIN`, because the minimum altitude is a
process constant — this avoids plumbing a new uniform through the ~8 `terrainLightDir`
upload sites, which is what stalled the previous attempt.

`mc2TerrainShadowSunDefine()` (`gameos_graphics.cpp`) emits it at **every** terrain-family
program prefix: the main terrain variations, thin, surface, mask-solid, overlay+decal,
and — critically — `gos_terrain_lod_chunk.cpp` (base + tess, which reuses `prefix`).
Routing that file was the stated blocker; its lanes were stale and it was reclaimed.

`shadow.hglsl` defaults the macro to `0.0`, making the function the identity, so any
program that does **not** inject it (props, mechs) compiles byte-identical.
`mc2ShadowMinSunAltDeg()` is now exported from `gos_postprocess.cpp` so the GLSL clamp
and the C++ matrix clamp cannot drift apart.

### Which cure was chosen, and why

Three options were on the table. **Feeding the guard the clamped vector** was chosen:
the shadow map *content* is rendered from the clamped sun, so the guard must test
against that same sun or it rejects surfaces the map genuinely has occlusion for.
Making the guard merely "robust to a degenerate light" would have suppressed the
back-face rejection wholesale and reintroduced the self-shadow acne it exists to stop.
Clamping the **shading** light dir was rejected outright: it would move scene
brightness on mc2_10, which is a visual change the user did not ask for. Measured
consequence of that choice — **0.00 % of pixels brightened** in every A/B pose.

### Gate hygiene — `gate_status` verdict **OK, zero flags**

| Gate | State |
|---|---|
| `MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN` | registered (`MC2_FEATURE_…`) + documented (`docs/tier1_env_vars.md`) + smoke-allowlisted, **default ON**, `=0` reverts |

Gate string verified present in the **deployed** binary (`grep` of `mc2.exe`, count 1).

---

## CAPTURE CONFIRMATION — mc2_10 night, 3 wide poses

Fixtures: `tests/visual/bookmarks/mc2_10_sunclamp.json` (wide, alt 780–850, proj 28–32,
spread centre / `-3200,-3200` / `+3000,+2600`, rotations 73.75 / 163.75 / 253.75° so the
sun azimuth is straddled). Deterministic fixed clock; all poses `present=True det=True`.

### Beauty frames — the gate moves default pixels

| Pose | gate ON (default) | gate OFF (`=0`) |
|---|---|---|
| `sunclamp_center_wide` | `a69191ebe106979e` | `ea1688351a3d60a7` |
| `sunclamp_across_azimuth` | `48d2c2882e3f1e43` | `ffdb61004031bef9` |
| `sunclamp_relief_ne` | `b0b66c1ca78e723e` | `ed4a2643816f659f` |

The gate-OFF hashes **reproduce the SHADOW-SUNCLAMP-1 lane's recorded values exactly**,
which independently confirms arm B is unmodified legacy and arm A is this change's delta.

| Pose | pixels changed | darkened | brightened |
|---|---|---|---|
| `sunclamp_center_wide` | 36.6 % | 36.4 % | **0.00 %** |
| `sunclamp_across_azimuth` | 11.7 % | 11.5 % | **0.00 %** |
| `sunclamp_relief_ne` | 14.1 % | 13.8 % | **0.00 %** |

Purely subtractive — the exact signature of "shadows that were being discarded have
returned", with shading untouched.

### Shadow-factor debug view — the decisive evidence

`MC2_LIGHTING_DEBUG_VIEW=shadow` renders the combined terrain PCF shadow factor
(1 = lit, 0 = occluded), which removes the night-ambient masking:

| Pose | fraction of frame at shadow factor > 250/255 |
|---|---|
| | gate OFF → gate ON |
| `sunclamp_center_wide` | **70.0 % → 2.9 %** |
| `sunclamp_relief_ne` | **69.4 % → 28.1 %** |

Gate OFF, the terrain region of the frame is a **featureless saturated white** — not
"few shadows", but the literal `return 1.0` early-out firing for every terrain pixel,
exactly as predicted. Gate ON, relief-following occlusion structure appears.
Side-by-sides: `scratch/tsg1/dbg_cmp_*.png`, beauty comparisons `scratch/tsg1/cmp_*.png`.

### Controls — the falsifiable prediction holds

All **14** poses **byte-identical** across the A/B, as predicted for maps whose authored
sun clears the 15° minimum:

| Map | Authored pitch | Poses | Result |
|---|---|---|---|
| mc2_01 | 50° | 3 (`mc2_01_sunclamp_control`) | byte-identical |
| mc2_03 | 46° | 9 (`mc2_03_scout`) | byte-identical |
| mc2_24 | 75° | 2 (`mc2_24_terrain_near`) | byte-identical |

Day (mc2_01/03/24) and night (mc2_10) both covered; mc2_10's authored sun is a mission
constant, not time-animated.

---

## ADVERSARIAL SELF-REVIEW

- **Would the user see this on a default launch?** Yes. Arm A was captured with **no env
  override at all** and differs from arm B at all three poses. The gate defaults ON in
  both the code and the registry.
- **Could the delta be something other than this change?** No. Arm B reproduces hashes
  recorded by a different lane on a different commit, and both arms are `det=True`.
- **Could non-terrain surfaces have been disturbed?** No. The define is appended only to
  the terrain-family prefixes; prop/mech programs compile the `0.0` identity. Nothing
  brightened anywhere.
- **Honest limitation.** In the *beauty* frame the night magnitude is modest (max channel
  delta 4–14 / 255) because night ambient dominates the sun term. The mechanism is proven
  and terrain shadowing is restored, but whether the user reads the reported
  "shadowed half / unshadowed half" as fully resolved is a **user click-through item** —
  asserting that from these captures would repeat the overclaim that sank the previous
  attempt.
- **Harness caught a real gap.** `check-shader-injectors.py` correctly failed the new
  ungoverned macro; fixed in `c63fec74` rather than waived.

---

## GATES

| Gate | Result |
|---|---|
| Contracts | **68 / 68** |
| Contract harnesses | **16 / 16** |
| tier1 smoke | **5 / 5 PASS**, `DEPLOY_FINGERPRINT OK` on all five (`c63fec7406e0`) |
| Tree | clean (tracked); committed before build; never pushed |

---

## DEFERRED / HANDED OVER

**`mclib/bdactor.cpp:~7449` — LOD>=1 excluded from casting shadows.**
NOT actioned: the file was claimed by the **live** lane `CONE-SPILL-GRADE-1` (touched
minutes before this session). Assessment for whoever picks it up:

- The justification comment ("shadow depth pass has no alpha discard") is **false**, and
  more completely false than the brief assumed. `b8f764b6` added `shadow_static_prop.frag`
  with `discard a < 0.5`, and the **atlas/coalesced** variant is also compiled
  (`gameos_graphics.cpp:5284`, `#define MC2_SHADOW_PROP_ATLAS 1`). So both the legacy and
  the coalesced texture paths alpha-test. The original blocker is gone on both.
- **Benefit:** distant trees regain leaf-shaped shadows — a genuine candidate cure for the
  missing-distant-tree-shadow complaint.
- **Cost:** far/impostor LODs are numerous flat cards; admitting them grows the shadow
  caster set at distance (`[SHADOW_CULL]` currently 2466 → 1816). Needs measurement.
- **Recommendation:** its own gated slice (default OFF until graded), not a drive-by.
