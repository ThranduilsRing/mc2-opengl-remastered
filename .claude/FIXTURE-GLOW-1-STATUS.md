# FIXTURE-GLOW-1 — STATUS

Lane: FIXTURE-GLOW-1. Worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`.
Base `5c33fb48` → landed **`5dd46496`** (5 commits). Deploy lane `mc2-win64-v0.4c`,
`DEPLOY_FINGERPRINT OK: exe sha=5dd46496b31c matches worktree HEAD`.

**VERDICT: the glow mechanism is BUILT, BOUNDED and VERIFIED — and I am shipping it
DEFAULT-OFF, because it does not close the user's defect and I will not claim it does.**

The bounding problem is solved and proved at overview altitude (including under 4×
exaggeration). What is *not* solved is that at play scale the glow makes an
already-green beacon **paler**, not lit — and the fixtures the user actually named,
runway edge lights, do not exist on any tier map. §6 and §7 are the important sections.

---

## 1. META REVIEW — premises checked against source, not status docs

| Premise (from brief) | Verdict |
|---|---|
| `get_base_light` clamps at 1.0, so emission scale 1/4/8 are byte-identical | **CONFIRMED**, and I found a **second, worse** reason — §2. |
| The fixture flag rides `ptd.hotPinkRGB.w` and is free to reuse | **CONFIRMED**; used as-is, no struct growth. |
| A min-screen-size clamp caused the overview streaks; do not reintroduce | **CONFIRMED** (`static_prop.vert:437-447`, `k = clamp(kLbMinNdc/sil, 1.0, kLbMaxScale)` — the cap was added *after* the regression). |
| "Colour per fixture type: **white** runway lights, and red/green/blue beacons" | **FALSE.** `runwaylight.ini` declares `0x00ff0000` — **red**, byte-identical to `redbeaconlight`. There is **no white fixture** in stock data. Only three distinct colours exist. §7. |
| Overview altitude ~6000 on mc2_10 | True as an *altitude*, but **misleading as a distance** — MC2 changes altitude by scaling the projection, not moving the eye. This broke my first design. §3. |
| mc2_01 is clean, mc2_10 is noisy | **CONFIRMED**, and mc2_01 gave the cleanest result in the lane (exactly 0 px × 9 poses). |
| Contracts 69/69, harnesses 16/16 | **CONFIRMED** at base and after. |

## 2. ★ The real reason the lights "are not on at ALL"

The predecessor found the 1.0 clamp. There is a **second** mechanism, and it is the
one that actually matters:

```
static_prop.frag:436   vec4 c = tex_color * vec4(litRgb, v_argb.a);
```

The lighting is **MULTIPLIED by the albedo**. A fixture's lens texel is dark, so even a
fully saturated emissive seed comes out dim. **A light source is not expressible as a
multiplier on a dark texture.** Every emissive-side attempt — four lanes of them — was
pushing on a term that gets multiplied by a small number immediately afterwards.

This is why the mechanism here is a **separate additive term applied after the albedo
multiply and after fog**, next to `FragColor = c`. That is how it escapes both the 1.0
clamp and the multiply. Nothing else in the design is novel; this is.

## 3. ★ A design I shipped, measured, and had to throw away

My first attenuation window was on **view depth (`clip.w`)**: full glow below 2600,
arithmetically zero beyond 4600, "overview is 6000, so it is zero by construction".
That reasoning was wrong and the capture caught it.

MC2's camera changes **altitude by scaling the PROJECTION**, not by moving the eye
(`mclib/camera.cpp`, `newScaleFactor = 1 - (alt - AltitudeMinimum)/testMax`). Measured
with the existing debug view 50 (depth = eye distance / 6000):

| pose set | altitude | median eye distance |
|---|---|---|
| `mc2_10_lo` (ground) | 350 | **~659** |
| `mc2_10_grid` (overview) | 6000 | **~800** |

Essentially flat. **A `clip.w` window never engages, so what I had actually shipped was
an unattenuated glow at every altitude** — the exact unbounded shape this lane was told
not to ship. Fixed in `9ae2d9e1`.

**The correction** drives the fade off the fixture's **apparent NDC silhouette**,
`sil = kLbRefRadius * ndcPerUnit` — the *same quantity* the failed
`RUNWAY-MARKER-LIGHTS-1` clamp used, **inverted**:

| | RUNWAY-MARKER-LIGHTS-1 (failed) | FIXTURE-GLOW-1 |
|---|---|---|
| response as `sil` falls | `max()` grows the geometry | `smoothstep` fades the glow |
| bound | **none** (`max(1.0, k/sil)`) | `[0,1]` by construction, exactly `0.0` below `sil=0.0009` |
| touches geometry/size | yes | **never** |

Window calibrated against the predecessor's measured 4K pixel counts (1 px ≈ 0.000926
NDC at 2160 rows): ground `sil≈0.021` → full; mid `sil≈0.0035` → near full; overview
sub-pixel → zero. Compile-time, **deliberately not env-tunable**, so the bound cannot be
configured away.

## 4. ★ Measurement — overview FIRST, and two near-miss false passes

**Noise discipline.** mc2_10 additive noise is severe and I publish the floor with every
number. Two noise classes, one of which I had not seen documented:

* **Mover drift** — large, chromatically neutral, spatially compact.
* **★ Frame-wide ±1 LSB drift** — on some launches an entire frame differs by exactly
  ±1/255 across ~120k pixels (`p99 delta = 1`, 1783 distinct columns). A naive
  "ON ≥ OFF" additive mask counts all of it as signal. **All metrics below use a
  ≥8/255 gain threshold** to clear this floor. Without it I would have reported a
  121,693-px "overview glow" that was an exposure shift.

**Detector**: `ON ≥ OFF on every channel AND max channel gain ≥ 8`, minus an ON-vs-ON
control mask. Directional, because **a purely additive glow cannot darken a pixel** —
that single property invalidated my first overview reading, where the "clean signal"
had a *negative* mean delta of −14.

### (a) OVERVIEW altitude 6000 — verified FIRST, as mandated

| pose | ON-vs-ON noise | GLOW px | % of 8.3 Mpx frame |
|---|---|---|---|
| `grid_2_2` (**the fixture-bearing pose**) | 289 | **2** | 0.00002 % |
| `grid_0_0` | 62335 | 308 | unusable (signal < noise) |
| other 7 poses | 0 | **0** | **exactly zero** |

**★ And at 4× strength it does not move**: `grid_2_2` = **2 px** (noise floor 232), 7 of
9 poses still exactly 0. The one large cluster at `grid_0_0` (13703 px) is
**achromatic** — delta saturation p95 = 2 — i.e. one mover rendered brighter, not a
coloured fixture. **Quadrupling the strength produces no overview change, which is the
bound holding, not a measurement that happened to come out small.**

### (b) GROUND level on a runway — alt 350, `mc2_10 lo_5`, **noise floor 0**

**2560 px, mean gain +82, max +183, 3 compact clusters** (759/769/1032 px).
Beacon colour `(50,210,89) → (130,255,167)`. Real, correctly located, correctly
coloured, and **localised — no streaks, no dashes**.

### (c) MID altitude — alt 2200, `mc2_10 f_*`, noise floor 0 on the usable poses

`f_0_0` = 51 px, `f_1_1` = 115 px, mean gain ~+36; clusters of 15–17 px.
A small bright speck — the intended shape.

### (d) DAY pose, no daylight glow — `mc2_01`, all 9 poses

**Exactly 0 changed pixels on every pose.** The term is multiplied by the night factor,
so it is identically zero in daylight. This is the cleanest result in the lane (mc2_01
is the low-noise map).

### (e) Buildings unchanged — provable, not merely observed

`mc2_03` (0 of 463 types) and `mc2_24` (0 of 489 types) register **zero fixtures**, so
the glow is `vec3(0.0)` for every vertex on those maps by construction. Their ON-vs-OFF
diffs are therefore a **pure capture-noise measurement**, and a useful published floor:
`mc2_24 base_near` = 7588 px (0.09 %), `mc2_03` = 396 px, both with **no** ON-vs-ON
control applied. On mc2_01 (2 fixture types) every pose is exactly 0.

### ★ Two false passes I nearly reported

1. The `clip.w` design above — "zero at overview by construction" was arithmetically
   true and physically vacuous.
2. An overview run reported `TOTAL 0` at strength 4 — **because the baseline directory
   had been deleted** and the script globbed zero files. A vacuous pass that looked like
   the best result of the lane. Re-captured; the real number is the 2 px above.

## 5. Implementation

`7758c938` → `5dd46496`. Files: `shaders/static_prop.vert`, `shaders/static_prop.frag`,
`GameOS/gameos/gos_static_prop_batcher.cpp`, `RenderCore/RendererFeatureRegistry.h`,
`docs/tier1_env_vars.md`, `scripts/run_smoke.py`.

- Vert computes `v_fixtureGlow`, frag adds it after fog. **Tint is the fixture's own
  night emission delta**, which is exactly `vec3(0)` on the non-emissive post/base
  geometry — so the glow lands on the **lens only** and every fixture gets its correct
  colour with no extra plumbing. Whitening pushes toward the tint's own peak channel
  (not toward `vec3(1)`), so `0 → 0` and the lens-only property survives it.
- Scoping is inherited free from `BEACON-FIXTURE-ID-1` via `ptd.hotPinkRGB.w`. **No
  struct growth, no new binding, no cpp/glsl lockstep risk.**
- The emissive assignment stays behind its **original** condition, so enabling the glow
  cannot perturb `base_light` by an ulp.
- Receipt extended to **name** classified types (`4a1f...`/`5dd46496`) — the bare count
  hid the fact that a map can register five fixture types that are nearly all one
  colour, which is why five lanes only ever looked at green.

## 6. ★ Default recommendation: **OFF**, and why that is the honest call

Everything in §4 says the mechanism is safe: bounded at overview under exaggeration,
exactly zero in daylight, a mathematical no-op on 2 of 4 maps, no geometry touched.
Safety is not the problem.

**The problem is that it does not close the defect.** At strength 1.0, on the only
fixtures I could observe on screen, a vivid green cone becomes a *paler* green-white
cone. At true 1:1 scale the two frames are nearly indistinguishable
(`scratch/fg1_verdict/B_ground_mc2_10.png`). That is not "the lights are on".

And it cannot be fixed by turning the knob up, for a structural reason worth recording:
**the fixture lens already renders at channel max, so the only headroom left in an LDR
target is the OTHER channels.** Any added glow *necessarily* desaturates toward white.
Physically that is what a hot light core does — but it is an aesthetic call, and this
user has already rejected one fixture-lighting attempt as *"atrociously hideous"*.

Shipping this default-ON would repeat the predecessor's outcome exactly: a safe,
well-scoped, default-ON change that does not answer the complaint. So it ships **OFF**,
byte-identical on a default launch, with the strength knob calibrated and documented:
`1` = baseline, `3`–`4` = the "clearly a light" range. **The one thing this needs is the
user's eye on a strength**, which is a five-minute click-through, not another lane.

## 7. ★ What the successor (or the user) needs to know

**The user's named subject may not exist.** From the new named receipt:

| map | fixture types registered |
|---|---|
| mc2_10 | 2 × blue `0x0000FF`, 2 × red `0xFF0000` (`Redbeaconlight02`), 1 × green |
| mc2_01 | 2 × red (`Redbeaconlight03`) |
| mc2_03 | **0** of 463 |
| mc2_24 | **0** of 489 |

Every classified type across all four tier maps is a **beacon**. Not one runway or
taxiway edge light appears. Combined with the data finding that `runwaylight` is **red,
not white**, the brief's picture of the fixture population is wrong in two ways. If the
user is looking at runway edge lights and seeing nothing, the live hypothesis is no
longer "their emission is too weak" — it is **"those props are not reaching the static-
prop batcher at all"**, which is a different bug in a different subsystem, and no amount
of glow tuning will touch it. **That is where I would point the next lane.**

Second gap: **red and blue were never observed glowing.** A 16-pose ground sweep across
mc2_10 at alt 600 found **zero** saturated fixture clusters outside one exposure-drifted
pose. Green is the only colour any of the last five lanes has put eyes on. The red/blue
types are classified and go through the identical code path, but that is an argument,
not an observation, and I am labelling it as such.

## 8. Gates + verification performed

- `gate_status(MC2_STATIC_PROP_FIXTURE_GLOW)` → **verdict OK, flags []**, default OFF,
  registered ✓, tier1-documented ✓, smoke-allowlisted ✓, 3 readers.
- Gate string present in the **deployed** exe (`grep -a` hit); deployed
  `shaders/static_prop.vert` contains `u_fixtureGlowStrength` (3 hits).
- `scripts/validate_shaders.py` → `[PASS] static_prop.vert`, `[PASS] static_prop.frag`
  (the 53 `shaders/vulkan/*` FAILs are pre-existing and unrelated).
- `scripts/check-contracts.sh` → **69 passed, 0 failed**.
- `tools/build_contract_harnesses.py --run` → **16/16 PASS**.
- `run_smoke.py --tier tier1 --duration 30 --deploy 0.4c` → **5/5 PASS**, with
  `[DEPLOY_FINGERPRINT] OK: exe sha=5dd46496b31c matches worktree HEAD`.
- No push, no `--amend`, no foreign-PID kills (all captures `--no-kill`).
- Disk watched throughout (A: 7.1 → 3.5 GB); every PNG checked non-zero before analysis;
  intermediate arms deleted after metric extraction.

## 9. Adversarial self-review

- *"Would the user still see the defect on a default launch?"* — **Yes, entirely.** The
  gate is default-OFF and a default launch is byte-identical. I am not claiming the
  defect is fixed. What shipped is a verified, bounded mechanism plus a calibrated knob.
- *"Did you verify at overview first?"* — Yes, and it **overturned the design**: the
  first metric was measured, found vacuous, and replaced mid-lane (§3). That is the
  single most load-bearing thing in this report.
- *"Is the overview bound real or asserted?"* — Real, and tested the hard way: at **4×
  strength** the fixture-bearing overview pose still shows 2 px against a 232 px noise
  floor. A live unbounded term would have exploded under 4×.
- *"Did you show all four colours?"* — **No, and I could not.** There is no fourth
  colour (no white fixture exists in stock data), and red/blue were never caught on
  screen despite a dedicated 16-pose hunt. Green only. §7.
- *"Is 'buildings unchanged' observed or proved?"* — Proved on mc2_03/mc2_24 (zero
  fixture types → the term is `vec3(0)` for every vertex), observed as exactly 0 px on
  mc2_01.
- *"Where are you weakest?"* — Three places. (i) The aesthetic judgement in §6 is
  subjective and I changed my mind about it between a NEAREST-upscaled zoom (looked
  clearly worse) and the true-scale frame (looked nearly identical) — the user should
  overrule me freely. (ii) `mc2_10 lo_2` and `grid_0_0` are unusable at any strength;
  their noise exceeds their signal and I excluded rather than explained them. (iii) I
  broke the build once with a literal newline inside a format string and only caught it
  by grepping the compiler output — the preceding build's `tail -2` had hidden the
  error, and I had already deployed a stale exe off it (`fix(build)` commit records it).
- *"Anything self-serving in the framing?"* — The §2 albedo-multiply finding is the
  strongest result here and it is a *diagnosis*, not a fix. I would rather this lane be
  remembered for §2 and §7 than for the glow, which is the part that did not land.
