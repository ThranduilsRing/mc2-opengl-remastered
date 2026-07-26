# BEACON-FIXTURE-ID-1 — STATUS

Lane: BEACON-FIXTURE-ID-1. Worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`.
Base `48e95f43` → landed **`6c5c60c2`**. Deploy lane `mc2-win64-v0.4c`,
exe md5 `195baea08245`, `BUILD_FINGERPRINT sha=6c5c60c21b23 dirty=0`.

**VERDICT: the successor design is IMPLEMENTED and it works exactly as specified —
the fixture-identity discriminator is exact (4 of 773 `.ini`, zero building leakage),
buildings are provably byte-identical, and there is NO overview-altitude regression.**

**BUT the user's defect is NOT resolved, and I am not going to claim it is.** The
mechanism now correctly delivers the full stock night emission to the fixtures and
nothing else. That emission turns out to be worth about **+21/255 on the green channel
(+8 % luminance)** on a fixture that already renders green — it is not the difference
between "dark prop" and "light source". See §5. The lane that owns "make them actually
look like lights" is a *different* lane, and §6 says what it has to do.

---

## 1. META REVIEW — premises checked against source, not status docs

| Premise (from brief / prior lanes) | Verdict |
|---|---|
| Vertex colour cannot scope this; Hot Green is shared with ~730 buildings | **CONFIRMED independently.** Re-counted; `lighting.hglsl:118` names the tag "Building Base Lights". |
| The fixture set is ~24 `.ase` / 4 `.ini` | **CONFIRMED.** 24 fixture `.ase` (`{runway,red,green,blue}beaconlight` × base/x/dam/damx/l1) + `runwaypoint`; 4 `.ini`. |
| Discriminator = "the only ones whose three `Hot*RGB` are saturated primaries" | **HALF FALSE — and the false half is load-bearing. See §2.** As literally stated it leaks **503 of 773**. |
| `MC2_LIGHT_BILLBOARD_FACING` was defaulted OFF after an unbounded min-screen-size clamp | **CONFIRMED**; no clamp or billboard reintroduced here. |
| mc2_10 captures are nondeterministic; mc2_01 is clean | **CONFIRMED, and it bit me mid-lane.** See §4 — my first read of the mc2_10 evidence was wrong and a noise control overturned it. |
| Contracts 69/69 at base | **CONFIRMED** (69/69 after this lane too). |

## 2. ★ The discriminator — the brief's version is leaky; the shipped one is exact

The brief said: *fixtures are the only shapes whose three `Hot*RGB` values are
saturated primaries rather than desaturated greys.* Tested against all 773 stock
`data/tgl/*.ini` **with runtime defaults applied**, that test matches **503 of 773** —
useless.

The reason is `mclib/bdactor.cpp:813-819`: an **undeclared** slot defaults to the magic
tag itself.

```cpp
result = iniFile.readIdULong("HotGreenRGB",hotGreenRGB);
if (result != NO_ERR)
    hotGreenRGB = 0xff00ff00;      // <-- a saturated primary
```

Only 274 of 773 `.ini` declare `HotGreenRGB` at all, so ~499 buildings inherit a
saturated-green default and trip the naive test. Reading the `.ini` text alone hides
this — you only see it if you model the defaults.

**Shipped discriminator adds one clause: all three values must also be EQUAL.**

```
isEmissiveFixtureType(pink, yellow, green) :=
      (pink&0xFFFFFF) == (yellow&0xFFFFFF) == (green&0xFFFFFF)
  AND that value is a saturated primary (max>=200, min<=40, max-min>=160)
```

A fixture has *one* emission colour and declares it in all three slots; a building
declares slots *independently* with desaturated greys/olives. The three **defaults**
differ from each other, so every default-inheriting building is rejected by the
equality clause.

| Test | matches / 773 |
|---|---|
| saturated-primary only (brief's version) | **503** ❌ |
| **all-three-equal AND saturated primary (shipped)** | **4** ✅ |

The 4: `runwaylight` `0x00ff0000`, `redbeaconlight` `0x00ff0000`,
`greenbeaconlight` `0x0000ff00`, `bluebeaconlight` `0x000000ff`. **Zero buildings.**

I chose this over a hardcoded name list because it is the same size in code, it is
self-documenting about *why* these four are different, and it classifies modded
fixtures that follow the convention. A name list would have been acceptable and I would
have said so; it simply was not necessary.

**Live blast radius, measured from the runtime receipt** `[GPUPROPS] fixture-lights:`

| map | fixture types / total registered |
|---|---|
| mc2_01 | **2 / 219** |
| mc2_10 | **5–6 / 382–386** |
| mc2_03 | 0 / 463 |
| mc2_17 | 0 / 394 |
| mc2_24 | 0 / 489 |
| gaea_peaks_01 | 0 / 0 |

## 3. Implementation

`6c5c60c2`, 5 files.

- **`gos_static_prop_batcher.cpp`** — `isEmissiveFixtureType()`; the
  `MC2_STATIC_PROP_FIXTURE_LIGHTS` gate (default `1.0`, clamp `[0,16]`); classification
  at `finalizeGeometry`; the `[GPUPROPS] fixture-lights: N of M` receipt; night-state
  fetch condition widened to `s_nightTagMode != 0 || s_fixtureLightsScale > 0`; the new
  uniform uploaded at **both** the legacy and coalesce sites.
- **`shaders/static_prop.vert`** — `u_fixtureEmissiveScale`; per-vertex
  `emissiveScale = isFixture ? max(nightScale, fixtureScale) : nightScale`.
- Registry + `docs/tier1_env_vars.md` + `run_smoke.py` passthrough allowlist.

**The flag rides `ptd.hotPinkRGB.w`** — an existing std430 `vec4` padding lane that was
hardcoded to `0.0` and read nowhere (every consumer uses `.rgb`; verified). So there is
**no struct growth, no new SSBO binding, and no cpp/glsl lockstep risk.**

**Why non-fixtures are provably untouched:** the pre-existing design scales the night
*delta*, not the lighting. Non-fixtures take `emissiveScale = u_nightEmissiveScale`,
which is `0.0` by default, so `base_light = base_day + delta*0 = base_day` — bit-for-bit
the pre-fix value. This is why the gate can default ON without touching building windows,
and it is confirmed empirically in §4.

**No screen-size clamp, no billboard, no min-size floor.** The emission rides real
fixture geometry and shrinks with distance.

## 4. ★ Measurement — including a result that overturned my own first reading

Noise discipline: every mc2_10 verdict below is filtered by an **ON-vs-ON control run**
(same gate value, separate launch). Pixels that move between two identical-config runs
are masked out before any claim.

**This was not academic.** My first read of the mc2_10 mid-altitude evidence was that
pose `f_0_1` showed a strong 3624-pixel signal. The control run showed
**ON-vs-ON = 2985 px on that same pose** — it was almost entirely mover drift, and one
cluster I had zoomed into was a *building* sitting at a different position between runs,
i.e. exactly the false "buildings changed!" conclusion the noise was primed to produce.
Corrected here rather than left in the record.

| pose | ON-vs-ON (noise) | ON-vs-OFF | clean signal |
|---|---|---|---|
| mc2_10 `f_0_2` | 52531 | 52531 | **0** — pure noise |
| mc2_10 `f_0_1` | 2985 | 3624 | unusable |
| mc2_10 `f_0_0` | **0** | 24 | **24 px, green +25** |
| mc2_10 `f_1_1` | **0** | 64 | **64 px** |
| mc2_10 `lo_5` (ground) | **0** | 1676 | **1676 px, green 167→188** |
| mc2_10 `grid_2_2` (**overview, alt 6000**) | 419 | 514 | **69 px = 0.00083 % of frame** |

### Per-altitude verdicts (the mandated matrix)

- **(a) Ground level on a runway** — mc2_10 `lo_5`, pos (4200,4800) alt 350, **noise 0**.
  Green beacon: 1676 px change, green channel **167 → 188**, peak green 255,
  luminance **+8 %**. Real, correctly coloured, correctly located on the fixture. Modest.
- **(b) Mid-altitude airbase** — alt 2200: 24–64 clean px on noise-free poses; green
  beacon +25 on the green channel. Nothing on buildings.
- **(c) OVERVIEW altitude (mandatory)** — alt 6000, near top-down: clean signal **69 px,
  0.00083 % of an 8.3 Mpx frame**, longest run a handful of pixels. **No dashes, no
  streaks, no constant-size markers.** The `RUNWAY-MARKER-LIGHTS-1` regression is NOT
  reintroduced. This is the constraint that killed the previous attempt and it passes
  by ~3 orders of magnitude.
- **(d) DAY pose, no daylight glow** — mc2_01 is a day map. ON-vs-OFF over its 9 poses:
  **7 poses exactly 0.0000 %**. The one pose with any diff (`runway_overview`, 3239 px)
  is **100 % accounted for by movers**: all 3239 pixels at `x<500` where the mechs walk,
  and **0 pixels on the runway itself**. No daylight glow.
- **(e) Building windows unchanged** — every noise-free pose containing buildings is
  **exactly 0 changed pixels**. This is the whole point of the scoping and it holds.

### ★ The emission is already clamped — scale does nothing

`MC2_STATIC_PROP_FIXTURE_LIGHTS` = **1, 4, and 8 produce byte-identical frames**
(24 px / max 50 and 64 px / max 64, unchanged across all three). `get_base_light`
clamps to `1.0` internally, so the fixture emission is already saturated at scale 1.
**There is no headroom in this mechanism**, independently reconfirming
`NIGHT-TAGS-GREEN-1` §4.2.

## 5. Default recommendation: **ON** — but with the honest caveat

**Recommend shipping default-ON**, as shipped. Justification:
- Blast radius is **2–6 shape types per map**, discriminator validated at zero leakage.
- Buildings, terrain, movers: **0 changed pixels** on every noise-free pose.
- Overview altitude: **0.00083 %** of frame. It cannot look "atrociously hideous"
  because at overview it is essentially invisible.
- It is stock-correct behaviour the GPU path was silently dropping.
- `=0` is an exact byte-identical escape hatch.

**The caveat, stated plainly: this does not fix the user's complaint.** "The actual
lights are not on at ALL" describes fixtures that should read as *light sources*. What
shipped makes an already-green beacon **8 % brighter**. If the user launches with this
and looks at a runway, they will most likely still say the lights are not on. Defaulting
it ON is right because it is correct and free — not because it answers the defect.

## 6. What the successor lane must do

The remaining gap is **not** scoping and **not** the night tags — both are now solved and
measured. It is that a fixture's *emissive delta clamps at 1.0 and covers a few dozen
pixels*. Making these read as lights needs a **new visual mechanism**, and the two
obvious ones are the ones that already failed:

1. **Do NOT** reintroduce a min-screen-size clamp or billboard (`RUNWAY-MARKER-LIGHTS-1`,
   `MC2_LIGHT_BILLBOARD_FACING`). That is what produced the overview streaks.
2. The viable direction is a **bounded, distance-attenuated glow/halo** that fades to
   zero above some altitude — i.e. the opposite of an unbounded clamp — or routing
   fixtures through the existing light-cone/bloom path. Whatever it is, **it must be
   verified at overview altitude first, not last.**
3. The per-instance fixture flag this lane added (`ptd.hotPinkRGB.w`) is exactly the hook
   such a mechanism needs — it can already ask "is this a light fixture?" for free.

## 7. Gates + verification performed

- `gate_status(MC2_STATIC_PROP_FIXTURE_LIGHTS)` → **verdict OK, flags []**, default ON,
  registered ✓, tier1-documented ✓, smoke-allowlisted ✓, 3 readers.
- Gate string present in the **deployed** binary (`grep` hit in `mc2.exe`); deployed
  `static_prop.vert` contains `u_fixtureEmissiveScale`; deployed exe md5 ==
  build md5 `195baea08245`.
- `scripts/validate_shaders.py` → `[PASS] shaders\static_prop.vert`.
- `scripts/check-contracts.sh` → **69 passed, 0 failed**.
- `tools/build_contract_harnesses.py --run` → **16/16 PASS**.
- `run_smoke.py --tier tier1 --duration 30 --deploy 0.4c` → **5/5 PASS**, no
  `DEPLOY_FINGERPRINT` failure.
  - ★ **First tier1 run silently smoked lane `0.4` and HARD-FAILED the fingerprint
    check.** Re-run with explicit `--deploy 0.4c`. Recording it because the brief warned
    three lanes have already shipped conclusions off the wrong exe — the guard works,
    but only if you read its last line.
- No push, no `--amend`, no foreign-PID kills (waited out the foreign lane holding the
  0.4c exe rather than killing PID 9416/24160/26896).

## 8. Adversarial self-review

- *"Would the user still see the defect on a default launch?"* — **Partly, yes.** The
  fixtures now receive their stock night emission on a default launch, but at +8 %
  luminance they will probably still read as "not on". Stated in §5 rather than buried.
- *"Did the discriminator actually get validated, or did I trust the brief?"* — The
  brief's version was tested and **rejected** (503/773). The shipped version differs from
  what I was told to implement, and §2 shows the counting that forced the change.
- *"Is the scoping claim real or asserted?"* — Real: every noise-free pose with buildings
  shows **exactly 0** changed pixels, and the mechanism (delta × 0.0) makes it provable,
  not just observed.
- *"Is the overview verdict trustworthy given it's mc2_10?"* — It is the weakest number
  here, because mc2_10 overview carries a 419-px noise floor. I masked with an ON-vs-ON
  control, but `CAPTURE-DETERMINISM-1` warns the noise is **intermittent**, so a single
  control can understate it. The margin (69 px vs a "hideous streaks" failure mode that
  was visible across the whole frame) is large enough that this does not change the
  verdict — but I would not defend the exact figure 69.
- *"Did I fabricate a night map?"* — No. mc2_01 is day (mean luminance 51, verified
  visually); mc2_10 is night (18–30, verified visually). Only mc2_10 could produce a
  night verdict, and only 2 of 6 tier maps contain fixtures at all.
- *"Anything I'd flag against myself?"* — Two things. (i) I mis-read the mc2_10 evidence
  before running the control and would have reported a false signal; §4 keeps that.
  (ii) I could not verify a **blue** or **red** beacon emitting on-screen — only green
  (and a red/white runway light at 64 px). The other colours are covered by the same code
  path and the same 4-`.ini` classification, but I did not put eyes on them.
