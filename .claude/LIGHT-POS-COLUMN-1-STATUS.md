# LIGHT-POS-COLUMN-1 — object-path light world position never reaches the GPU

**Status:** code SHIPPED behind a default-OFF gate; measurement PARTIAL.
**Default recommendation: stays OFF.** Not because the fix is wrong — it is
provably correct — but because the measurement that would justify ON has not
been completed. See §6.

Commits (branch `claude/nifty-mendeleev`):
- `febe661a` — the fix + gate + registration + docs
- `31e1a9e8` — `RendererFeature` enum entry (`kFeatureTable` static_assert)

Gate: `MC2_LIGHT_POS_COLUMN_FIX`, default **OFF**.
`gate_status` verdict **OK**, zero flags (registered / documented /
smoke-allowlisted / reader found).

---

## 1. The bug — verified independently at source, five links

Every object-path POINT / SPOT / INFINITEWITHFALLOFF light is currently
evaluated **as if it sat at the world origin.**

1. `Stuff::AffineMatrix4D` is **3 columns × 4 rows**, indexed
   `entries[(column<<2)+row]` — `mclib/stuff/affinematrix.hpp:141`
   (`row <= W_Axis(3)`, `column <= Z_Axis(2)`).
2. `BuildTranslation` writes `(W_Axis, {X,Y,Z}_Axis)` — `affinematrix.hpp:102-108`
   — so the world translation lands at `entries[3] [7] [11]`.
3. `BuildRotation(EulerAngles)` writes **only rows 0..2**
   (`affinematrix.cpp:162-194`). The producers all call
   `BuildTranslation` *then* `BuildRotation`, so the translation genuinely
   survives. Producers: `bdactor.cpp:3435` (per-building point light),
   `bdactor.cpp:3673` (building spotlight children), `carnage.cpp:742`
   (explosion lights), `turret.cpp:917` (turret SPOT, falloff 50/450),
   `weaponbolt.cpp:1451` (weapon-bolt lights).
4. `GatherLightsParameters` (`mclib/txmmgr.cpp`) memcpys **12 floats**, landing
   translation at GPU floats `[3]/[7]/[11]` — i.e. `m[0].w / m[1].w / m[2].w`
   under a column-major `mat4` — then **explicitly zeroes `[12][13][14]`** and
   sets `[15] = 1.0f`.
5. **All seven** consumers decode the light origin as `m[3].xyz`, which *is*
   floats `[12][13][14]`:
   - `shaders/include/lighting.hglsl:246, 256, 274` (POINT / SPOT / INFINITEWITHFALLOFF)
   - `shaders/vulkan/mech_tier1.vert:110, 117, 124`
   - `shaders/lightgrid_build.comp:112`

⇒ the decode always yields `(0,0,0)`. Falloff therefore tracks
*distance-from-origin* instead of *distance-from-lamp*.

The in-code comment at `lighting.hglsl:205-208` asserting "column 3
(translation) … under column-major mat4" was simply **wrong about the CPU
type's layout**, and has been corrected in place.

## 2. Which end I fixed, and why

**Fixed the UPLOAD**, in `GatherLightsParameters`: copy `[3]/[7]/[11]` into
`[12]/[13]/[14]` when the gate is on.

- It is the **single chokepoint** feeding all seven read sites across three
  shader languages (GLSL, SPIR-V, compute) — one edit, and **no SPIR-V rebake**.
- **Double-correction is impossible.** The value shipped today is the constant
  `(0,0,0)`. A constant carries no information, so *no consumer can be
  compensating for it.* This is the check the brief asked for, and it comes back
  clean by construction rather than by survey.
- Default OFF retains the zeroing branch ⇒ upload **byte-identical** to shipped.

Deliberately untouched:
- `lightDir` is gathered separately via `GetLocalForwardInWorld` and was
  already correct.
- AMBIENT uploads Identity (translation already zero) and the GLSL AMBIENT case
  returns before reading position. INFINITE has no position term.
- The rotation 3×3 left in `[0..11]` — no consumer reads it.

**Notable:** the CPU mirror (`mclib/msl.cpp:1792`) sources POINT position from
the *different* field `TG_Light::direction` (which `carnage`/`weaponbolt`/
`bdactor:3437` stash the position into), and never reads this matrix at all.
So this gate moves the GPU path **toward** the CPU path, not away from it.
Note `bdactor.cpp:3673` building spotlights set `SetLightToWorld` but **not**
`->direction`, so those are mispositioned on the CPU path too — out of scope
here, but it is the obvious follow-up.

## 3. Build and gate provenance

- Clean attributable build: `build_fingerprint.h` sha `31e1a9e81cc0`,
  **dirty=0**, branch `claude/nifty-mendeleev`.
- Gate string genuinely present in the deployed exe, verified by byte search
  **with a negative control** (`strings` is not installed on this machine, and
  the brief warns a prior `strings`-based probe returned a plausible zero for
  every gate):
  - `MC2_LIGHT_POS_COLUMN_FIX` → 1 occurrence
  - `MC2_LIGHT_CLAMP_FIXTURE` → 2 (positive control)
  - `MC2_TOTALLY_BOGUS_GATE_XYZ` → 0 (negative control — the probe discriminates)

## 4. tier1

`--tier tier1 --duration 30 --keep-logs --exe <deployed lane exe>` (explicit
`--exe`, per the standing rule):

| mission | result | frames | fps | mover violations |
|---|---|---|---|---|
| mc2_01 | PASS | 4241 | 141 | 0 |
| mc2_03 | PASS | 4215 | 140 | 0 |
| mc2_10 | PASS | 4147 | 138 | 0 |
| mc2_17 | PASS | 4254 | 142 | 0 |
| mc2_24 | PASS | 4249 | 142 | 0 |

**5/5 PASS — but `DEPLOY_FINGERPRINT` reported MISMATCH, so per the standing
rule this is NOT reported as a clean tier1 PASS.** Honest accounting: the exe
carries `31e1a9e81cc0` (mine) while worktree HEAD had advanced to `5e8aa80e`
because a sibling lane committed mid-run. `git diff --stat 31e1a9e8 HEAD` is
**one `.md` file, zero engine code**, so the runs do exercise my commit's
engine — but the gate printed MISMATCH and I am not going to launder that.

## 5. Measurement — what moved (nothing, so far), and why

Method: gate-OFF vs gate-ON **A/B inside one exe**, `--runs 3 --warmup 1`
(two-run byte floor per pose). Doing the A/B within a single binary also
sidesteps the compromised reference installs entirely — nothing is graded
against `v0.3` / `v0.4` / `v0.4c`. `MC2_TERRAIN_LIGHT_POOLS=0` to avoid the
known mover-dim nondeterminism. `MC2_STATIC_POP_SPLIT`,
`MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE`, `MC2_GPU_CULL_STATIC_FROZEN_RECORDS`
unset; effective env confirmed present in every manifest
(`identity.mc2_env.effective`).

### mc2_05 `building_faces` (night; the USER-APPROVED set)

Floor: all 4 poses byte-stable, **0.000%**.

| pose | gate OFF | gate ON | delta |
|---|---|---|---|
| refinery_lightaway | `24243032e8c5` | `24243032e8c5` | **byte-identical** |
| refinery_lightfacing | `3027ee6c40d7` | `3027ee6c40d7` | **byte-identical** |
| east_compound_lightfacing | `3bfc79f0778f` | `3bfc79f0778f` | **byte-identical** |
| control_no_buildings | `8046c8d90790` | `8046c8d90790` | **byte-identical** |

Three of these carry the `mc2_05_night_approved` cover tag. ⇒ **the
user-approved mc2_05 night lighting is provably NOT degraded** — it is not
merely "close", it is unchanged to the byte.

### The null is real, not an ENV-DROP

The brief's central trap. Positive control: re-running with
`MC2_TERRAIN_LIGHT_POOLS=1` **did** move pixels
(`24243032e8c5`→`274518b83a43`, `3027ee6c40d7`→`1172c86bd270`) and reproduced
the documented mover-dim drift on `east_compound`. So `--env-declare` demonstrably
reaches the exe, and the gate-ON run really did run gate-ON.

### Why nothing moved: there were no POINT/SPOT lights to reposition

`MC2_OBJECT_PARITY_TRACE=1` census of the object-light row:

```
iLight=0 type=1  (TG_LIGHT_INFINITE)   dir=(0.4997,-0.0349,-0.8655)
iLight=1 type=0  (TG_LIGHT_AMBIENT)    dir=(0,0,1)
numLights=2
```

(`tgl.h:163-168`: AMBIENT=0, INFINITE=1, INFINITEWITHFALLOFF=2, POINT=3, SPOT=4.)

**Zero POINT, zero SPOT.** The corrected code path is never exercised on these
poses, so byte-identical output is the *expected and correct* result — it
confirms the gate is inert where there is nothing to fix, and it is exactly
why the change is safe.

## 6. Honest limits — what I did NOT measure

This is the part that keeps the default OFF.

1. **Only mc2_05 was A/B'd.** mc2_01 / mc2_03 / mc2_10 / mc2_24, and the
   day/night breadth the standing capture rule requires, were **not** run.
2. **The parity trace is once-per-session**, firing on the first
   `submitMultiShape`. It may sample *before* `bdactor.cpp:3462` registers
   building spotlights (which requires `eye->isNight` and shapes with
   `GetIsSpotlight()` children). So "no POINT/SPOT lights" is proven for the
   sampled draw, **not** for the whole session.
3. **The most likely place this fix actually bites was never tested.** The
   `carnage.cpp` explosion lights, `weaponbolt.cpp` bolt lights and
   `turret.cpp` SPOT (falloff 50/450 — a 450-unit outer radius, far larger than
   the 80-unit fixture radius the sibling lane measured against) are **stock,
   always-on, dynamic** lights that carry real positions. They only exist during
   combat, and every pose used here is a static bookmark with no weapon fire.
   **A combat pose is the next experiment**, and it is the one that could
   plausibly show a large, visible change.
4. Therefore: **this lane does NOT touch the dark-props complaint**, and I make
   no claim that it does. The sibling measurement (props 169 units from the
   nearest fixture vs an 80-unit radius) still stands, and correcting the light
   *position* does not by itself bring those props inside the radius.

## 7. Recommendation

- **Keep `MC2_LIGHT_POS_COLUMN_FIX` default OFF** pending §6.1 and §6.3.
- The fix itself is correct and I would defend it on the source reading alone;
  what is missing is evidence of *benefit*, not evidence of *correctness*.
- Next experiment, in priority order:
  1. A **combat pose** with active weapon fire / explosions — the case with real
     light positions and large radii.
  2. Re-run the census late in a night mission to settle §6.2.
  3. Fix `bdactor.cpp:3673` to also set `->direction`, so building spotlights are
     positioned correctly on the **CPU** path too.
- The radii question the brief anticipated is now sharper: it is not only "are
  the radii too small", it is "**do any object-path POINT/SPOT lights exist at
  all in the poses we grade on**". On mc2_05 building faces, the answer was no.

## 8. Pre-existing issue noticed (not mine, not touched)

`scripts/check-env-registry.sh` FAILS on `MC2_CEMENT_ATLAS_DUMP`
(unregistered), which belongs to the CEMENT-ATLAS-CELL-2 lane. My gate is clean
on all four registration surfaces.
