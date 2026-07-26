# FOGLIGHT-LATCH-1 — STATUS

Branch `claude/nifty-mendeleev`. Probe shipped at `7c797480`; graded at HEAD
`0888bbcd`. Tier1 **PASS 5/5** with `[DEPLOY_FINGERPRINT] OK: exe sha=0888bbcd997c
matches worktree HEAD` on all five missions (lane `0.4d-rc1`).

## VERDICT: the `fogLightSet` latch is REFUTED. It is not the dark-prop defect.

No fix shipped. **On a default launch the user still sees the defect.** The last
standing candidate from DARK-PROP-LIGHTING-6 is now dead, by measurement.

## 1. The premise, verified from source (not from the brief)

Confirmed exactly as handed over — `mclib/bdactor.cpp`:

- `:1517`, `:6033` clear `fogLightSet` in the constructors.
- `BldgAppearance::update` (`:3275`) samples `land->getTerrainLight(position)`
  once inside `if (!fogLightSet)` (`:3654`), builds `lightRGB` (`:3665`), and
  sets the latch at `:3699`.
- `:3702` `eye->setLightColor(0, lightRGB)` — slot 0 is `TG_LIGHT_INFINITE`, the
  **sun**, for `BldgAppearance` — runs every frame from the latched value.

So `lightRGB` really is a one-shot per-actor latch feeding the sun slot forever.
The mechanism is real. It just isn't what's darkening the props.

## 2. What the measurement says

Probe: `MC2_PROP_LIGHT_LATCH_DIAG=1` (registered `Trace`, default OFF,
tier1-documented, `run_smoke.py` passthrough-allowlisted — `gate_status` verdict
`OK`, no `ENV_DROP_RISK`). Emits `[PROPLATCH v1]` `event=latch` (frame, terrain
intensity, latched RGB, nightFactor, position) and a throttled `event=compare`
that re-samples what the same actor at the same position would get *now* and
prints `MATCH`/`STALE`. Pure observation — it never writes `lightRGB`.

**Engagement confirmed** (2455 lines). Note the trap: `MC2_DIAGF` is additionally
gated on `MC2_DIAG_VERBOSE`, so the first run produced **zero** probe output while
appearing to succeed — the same silent-no-op class as `MC2_LIGHTING_DEBUG_VIEW`.
Any future probe using `MC2_DIAGF` must set `MC2_DIAG_VERBOSE=1` too.

Pose: `tests/visual/bookmarks/mc2_10_building_faces.json`, mc2_10 night, all five
bookmarks present + deterministic.

### Latched vs current, mc2_10 night

| actor | class | terrain intensity | latched RGB | current RGB | state |
|---|---|---|---|---|---|
| `JeepProp` | vehicle prop | 0.0012 | `3F3F3F` | `3F3F3F` | MATCH |
| `s_barracks` | building | 0.0000 | `3F3F3F` | `3E3E3E` | STALE by 1 |
| `s_admin_entry` | building | 0.0341 | `414141` | `3D3D3D` | STALE by 4 |
| `guntower` | building | 0.0291 | `414141` | `3E3E3E`–`3F3F3F` | STALE by 2–3 |
| `crateswithtarp` | crate prop | 0.0124 | `404040` | `404040` | MATCH |
| `VehicleHangar` | building | 0.0284 | `404040` | — | — |
| `rock_tclump2` | rock | 0.3164 | `595959` | `585858` | STALE by 1 |

Across all 41 latching actor types the latched value spans only `3E3E3E`–`595959`,
and it tracks terrain intensity monotonically — i.e. it is behaving correctly.

## 3. Why this refutes the hypothesis — three independent ways

1. **The classes agree.** `JeepProp` latches `3F3F3F`; `s_barracks`,
   `s_admin_core`, `s_admin_wing` latch `3F3F3F`; `guntower` and `s_admin_entry`
   latch `414141`. Vehicle-class props sit *inside* the building band, not below
   it. There is no dark-vs-light split to explain the user's picture.
2. **Where the latch is stale, it is stale by 1–7/255 — and it is the BUILDINGS
   that go stale, not the props.** `guntower` 796/848 compares STALE; `JeepProp`
   0/1. The drift is the wrong size *and the wrong direction* to matter.
3. **There is no cold-table window on this map.** Every single `event=latch`
   line, including the ones at `frame=0`, reports `nightFactor=1.000`. Mission
   night lighting is already fully applied before the first prop latches, so the
   "latched while the light table was cold" scenario simply does not occur here.

The single most decisive datum: **`s_barracks` latches at terrain intensity
`0.0000` and gets the identical `3F3F3F` that `JeepProp` gets.** The user reports
barracks as correctly lit and vehicle props as near-black. Identical latched sun
colour, opposite visual outcomes ⇒ the differentiator is downstream of `lightRGB`.

## 4. Where this points the next lane

`lightRGB` at mc2_10 night is effectively a **flat ~`0x3F` ambient floor for every
static prop regardless of class** — at these intensities `Camera::getLightRed`
(`mclib/camera.cpp:4334`) is returning essentially `ambientRed`, because
`lightRed * intensity` is negligible. So:

- The whole `getTerrainLight → lightRGB → setLightColor(0) → fogLightSet` chain
  is **exonerated**. Do not re-open it.
- The brightness difference must arise downstream, where two shapes receiving the
  *same* light value diverge: the per-leaf normal-dependent gather in
  `CacheGpuLightData`, the per-recipe bake (`mc2SetBakedStaticLight` is keyed by
  `recipeIndex`, i.e. **shared across all instances of a recipe**), or the shader's
  ambient/base-light split. This is consistent with PROP-BASE-LIGHT-DIR-1's
  finding that `base_light` is ~zero and only `ambientTerm` lights these props.

## 5. Also found during meta review (not this lane's to fix)

`mclib/bdactor.cpp:3118-3147` — `LIGHTBAKE-REBAKE-ON-READY-1` already documents
and fixes a *closely related* frozen-bake defect (recipes baked during boot
keeping a day-bright row), default-ON, killswitch
`MC2_LIGHTBAKE_REBAKE_ON_READY=0`. It addresses the opposite polarity (too
bright) and is orthogonal to this finding.

## 6. Evidence hygiene

- **Measured** two-run noise floor at this pose: all 5 captures **byte-identical**
  between the diag-OFF and diag-ON runs (`e608adef3e789c24`, `b32aebc328b6f65f`,
  `258815452d20661e`, `1b5766e78c3a41cb`, `0a31b8cb3544a84b`) — 0.000%, measured
  not recorded. This doubles as proof the probe does not perturb rendering.
- PNGs verified non-zero (24,887,323 bytes each) — the A: zero-disk hazard.
- No before/after grading exists because **no behavioural change was made**.

## 7. Honest caveat

mc2_10's named truck fits (`truckprop`, `cargotruckprop`, …) did **not** appear in
this run — the only vehicle-class prop that ever latched was `JeepProp`. So this
is not literally "the user's dark truck vs a lit building". The refutation does
not depend on it: argument 3 above (`s_barracks` at intensity 0.0000 latching the
same `3F3F3F`) is class-independent, and the latch band is uniform across all 41
types observed. But a lane wanting the by-name instance proof for the user's exact
trucks still needs to decode `mc2_10.pak`.
