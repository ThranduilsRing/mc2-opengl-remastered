# IBL-NIGHT-SCALE-1 — STATUS (2026-07-26)

**Verdict: SHIPPED, DEFAULT-ON. The day-sky SH-IBL probe is now night-scaled
toward a floor of 0.25 instead of applied at full strength. On mc2_10 night the
clipping building face goes from 2.49% of its pixels hard-clipped to 0.13%
(19x reduction) while the perimeter wall, turrets and bunker domes give up only
13-18% brightness — versus the 29-46% the blunt killswitch cost. Day is
byte-identical by arithmetic. Contracts 69/69, harnesses 16/16, tier1 5/5 with
DEPLOY_FINGERPRINT OK.**

- Worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`, HEAD `6c47e149`
- Commits `7dd64716` (fix) + `6c47e149` (linkage)
- Deploy `A:/Games/mc2-opengl/mc2-win64-v0.3`, manifest `src_commit 6c47e149353c`, **not dirty**
- Evidence `scratch/ibln1/`. No push, no `--amend`.

## 1. META REVIEW — the brief's premises, checked from source

| # | Inherited claim | Verified? |
|---|---|---|
| Probe is a day sky | ✅ `RenderCore/IblShCoeffs.h:5` — `DaySkyHDRI063B_4K.exr` |
| One SH set for all missions/times | ✅ `kIblShSets[]` one entry, `kMissionShMap[]` empty |
| Nothing scales the IBL by night | ✅ the `lit += ibl * u_iblShStrength` add sits outside every night branch |
| Authored night ambient is 32, C++ path stock-faithful | ✅ not re-derived; consistent |
| mc2_14 nightFactor = 0.33 | ✅ **derived independently and it lands exactly.** `LightDirPitch=20` → altitude 20 → `(25-20)/15` = **0.3333** (`camera.cpp:2209-2222`, `NIGHT_START_PITCH 25` / `NIGHT_LIGHT_PITCH 10`) |

### ★ Correction 1 — `(1 - nightFactor)` was one unrelated default away from being a silent no-op

`u_nightFactor` is **not** unconditionally uploaded. Both upload blocks
(`gos_static_prop_batcher.cpp:6700`, `:7920`) fetch real night state only when

```
s_nightTagMode != 0 || s_fixtureLightsScale > 0.0f || s_fixtureGlowStrength > 0.0f
```

and otherwise leave it at `0.0f`. `MC2_STATIC_PROP_NIGHT_TAGS` and
`MC2_STATIC_PROP_FIXTURE_GLOW` are both **default-OFF**; only
`MC2_STATIC_PROP_FIXTURE_LIGHTS` (default `1.0`) keeps that condition true. So
the brief's literal `ibl * (1 - u_nightFactor)` would have worked **by luck**,
and would have silently become a no-op the moment a user set
`MC2_STATIC_PROP_FIXTURE_LIGHTS=0`. The fix therefore calls
`gos_GetNightState()` directly and does not depend on an unrelated gate.

### ★ Correction 2 — the IBL strength is not 0.5, and the strength env is not a neutral knob

The brief (and `RendererFeatureRegistry.h`) said strength `0.5`. That is only the
bare code fallback. `data/visual_tuning.json` overrides it per mission —
defaults `0.55`, and **`0.6` on mc2_01 / mc2_10 / mc2_24**. Setting
`MC2_STATIC_PROP_IBL_SH_STRENGTH` *disables* that per-mission override
(`visual_tuning_profile.cpp:170`), so the env is **not** interchangeable with
leaving it unset. Proven empirically: env-unset and env=`0.60` are numerically
identical on every measured box on mc2_10, while env=`0.50` is not. Registry
doc and `docs/tier1_env_vars.md` corrected in `7dd64716`.

### ★ Correction 3 — mc2_05 is FULL night, not "barely involved by construction"

`LightDirPitch=270` → `lightAltitude = 180-270 = -90` → `< NIGHT_LIGHT_PITCH` →
`isNight`, `nightFactor = 1.0`. mc2_05 takes the **same full-strength night
scale as mc2_10**. The predecessor's "barely involved" is true but for a
different reason (prop sparsity, not a partial night factor), so it needed
measuring rather than assuming — §5.

Reader-sourced `nightFactor` for the whole verification set:

| mission | LightDirPitch | altitude | nightFactor |
|---|---|---|---|
| mc2_01 | 50 | 50 | **0.0 — day** |
| mc2_03 | 46 | 46 | **0.0 — day** |
| mc2_24 | 75 | 75 | **0.0 — day** |
| mc2_10 | 180 | 0 | **1.0 — full night** |
| mc2_05 | 270 | −90 | **1.0 — full night** |
| mc2_14 | 20 | 20 | **0.333 — partial** |

## 2. The curve, and why not a hard `(1 - nightFactor)`

```
strength *= 1 - (1 - floor) * nightFactor      // == mix(1.0, floor, nightFactor)
```

A hard `(1 - nightFactor)` is `floor = 0`, which at full night is *arithmetically
identical to the blunt killswitch* — the outcome the brief's own acceptance
criterion forbids. The probe's **directional structure** (sky above, ground
below) is still correct at night; only its **intensity** was wrong. So scale
toward a floor.

Floor chosen by measured sweep on mc2_10 night, emulating each candidate via the
pre-existing `MC2_STATIC_PROP_IBL_SH_STRENGTH` env on **one binary** (mc2_10's
effective full strength is 0.6, so floor *f* ≡ env `0.6f`):

| floor | bldg_left clip% | wall_a | dome_a | turret_c | turret_a |
|---|---|---|---|---|---|
| 1.00 (today) | 2.49 | 0.0% | 0.0% | 0.0% | 0.0% |
| 0.60 | 1.71 | −3.6% | −6.3% | −4.9% | −3.1% |
| 0.35 | 0.57 | −9.4% | −11.8% | −11.2% | −7.7% |
| **0.25 (chosen)** | **0.13** | **−14.1%** | **−18.4%** | **−18.3%** | **−13.0%** |
| 0.00 (== killswitch) | 0.00 | −36.9% | −45.6% | −38.0% | −29.0% |

0.25 is the knee: it removes **95% of the clipping for a 13–18% dim**, where
going to 0 costs 29–46% and reproduces the near-black-walls complaint.

**Day is byte-identical by ARITHMETIC, not by measurement.** In daylight
`nightFactor` is exactly `0.0f`; the helper early-returns `g_iblShStrength`
unchanged, so the identical float reaches the identical uniform.

## 3. Implementation

`GameOS/gameos/gos_static_prop_batcher.cpp` only — **the contended
`shaders/static_prop.vert` was deliberately not touched.** Folding the night
curve into the CPU-side strength upload makes the shader edit unnecessary, which
both avoids the conflict and makes the day-identity argument arithmetic.

- `s_iblNightFloor` — env `MC2_STATIC_PROP_IBL_NIGHT_SCALE`, clamped 0..1, default `0.25`
- `iblShStrengthNightScaled()` — applied at **all three** upload sites: GL coalesce (`:6827`), GL legacy (`:8042`), VK (`:9376`), so the backends agree
- `gos_GetNightState` declared at **global** scope (the helper is inside this file's anonymous namespace; an `extern` inside it resolves to `(anonymous)::gos_GetNightState` and fails to link — that was commit `6c47e149`)

Gate `MC2_STATIC_PROP_IBL_NIGHT_SCALE`: `gate_status` verdict **OK** — registered
(`MC2_FEATURE_STATIC_PROP_IBL_NIGHT_SCALE`), tier1-documented, smoke-allowlisted,
`check-env-registry.sh` PASS. `=1.0` is the byte-identical killswitch; `=0.0`
reproduces the blunt behaviour. **String verified present in the deployed exe.**

## 4. Measurement + noise floor

Method: env-only A/B on **one** binary, absolute-luminance metrics on static
boxes (`scratch/ibln1/measure.py`), per the predecessor's protocol.

**Published noise floor**, from an A-vs-A replicate at identical effective
strength (env-unset vs env=0.60, separate launches):

- **exactly 0.000** luma on `truck`, `crate`, `bldg_tower`, `bldg_left`, `wall_a`, `wall_b`, `turret_b`, `dome_a`, `ground_ctl2`
- `turret_a` 0.04, `turret_c` 0.34 (worst)
- **excluded as mover-contaminated:** `bldg_rear` and `ground_ctl` moved non-monotonically across a monotone strength sweep — that is the evidence for excluding them, not a judgement call

Measured effects are 15–40 luma, i.e. **~50–100× the worst noise floor**.

### mc2_10 night — headline (pre-slice vs shipped default)

| box | before | after | Δ% |
|---|---|---|---|
| bldg_left **clip%** | **2.49** | **0.13** | **−95%** |
| bldg_left max channel | 255 | 255 | (a few pixels stay saturated from other terms) |
| wall_b clip% | 0.08 | 0.00 | −100% |
| turret_a clip% | 0.11 | 0.00 | −100% |
| dome_a clip% | 0.13 | 0.07 | −46% |
| bldg_tower luma | 162.00 | 119.29 | −26% |
| wall_a luma | 105.25 | 90.37 | −14% |
| turret_a luma | 99.08 | 86.24 | −13% |
| dome_a luma | 98.90 | 80.69 | −18% |
| ground_ctl2 (**terrain control**) | 70.51 | 70.51 | **0.00** |

★ **The shipped build reproduces the calibration prediction exactly** — every
clean box in the default arm is bit-equal to the `f=0.25` emulation arm
(truck 80.93, crate 79.77, bldg_tower 119.29, bldg_left 98.83, wall_a 90.37,
wall_b 90.06, turret_a 86.24, turret_b 94.15, dome_a 80.69). Independent
confirmation the implemented curve does what was calibrated.

★ **Killswitch parity**: `=1.0` reproduces the pre-slice default exactly on
every clean box (92.28 / 101.60 / 162.00 / 136.14 / 105.25 / 110.89 / 98.90).

Verdict image: `scratch/ibln1/verdict_mc2_10_before_after.png` (before top,
after bottom) — buildings stop reading as daylit, walls/turrets stay legible.

### Truck/building contrast — reported, NOT claimed as fixed

| | truck/bldg_tower | truck/bldg_left | crate/bldg_left |
|---|---|---|---|
| before | 0.570 | 0.678 | 0.746 |
| **after** | **0.678** | **0.819** | **0.807** |
| blunt killswitch | 0.609 | 0.727 | 0.735 |

It moves further in the right direction than the killswitch did, but **does not
reach parity and this fix does not pretend to**. The truck/building gap is an
**albedo** property (truck texture ~3.3× darker), independently measured by two
prior lanes. Unchanged conclusion.

## 5. Day + partial-night + user-approved-night verification

**Day (mc2_01 9 poses, mc2_03 9 poses, mc2_24 2 poses — default vs killswitch,
same binary): 15/20 byte-identical.** The 5 differing poses were adversarially
chased rather than waved through:

- An **A-vs-A replicate** (default vs default) produced the *same class and
  magnitude* of difference on overlapping poses (max delta up to 202, changed%
  up to 0.024).
- The largest, `mc2_24_base_near` (max delta 216, 0.295%), was isolated with
  four further runs: `d_def`/`d_def2` form one cluster and `d_def3`/`d_def4`/
  `d_kill` form another — i.e. **default-config runs appear in BOTH clusters**,
  and the killswitch arm is *byte-identical* to two default runs. The pose is
  bimodal run-to-run, independent of this gate.

Combined with the arithmetic identity (§2), **day is unchanged**.

**mc2_05 — USER-APPROVED night, full nightFactor 1.0** (5 locations incl. a
no-fixture control, `scratch/pss1/bm/mc2_05.json`), killswitch vs shipped default:

| pose | pre-slice | shipped | Δ luma | clip% before → after |
|---|---|---|---|---|
| saved_205843 | 111.74 | 111.25 | −0.49 | 0.030 → 0.015 |
| loc_b | 26.04 | 25.15 | −0.90 | 0.062 → 0.025 |
| loc_c | 93.05 | 93.05 | **0.00** | 0.028 → 0.028 |
| loc_d_control | 58.44 | 58.44 | **0.00** | 0.008 → 0.008 |
| loc_e_wide | 54.49 | 54.49 | −0.00 | 0.004 → 0.004 |

Max movement **0.90 luma (−3.4%)**, two poses exactly zero, and clipping only
ever improves. **Not degraded** — and strictly gentler than the killswitch the
predecessor measured at the same ≤0.9 magnitude.

**mc2_14 — partial night, nightFactor 0.333** (5 locations incl. a no-prop
control, `scratch/ibln1/mc2_14_poses.json`). A dense tree-prop dusk map, so the
mid-range is genuinely exercised, not just the endpoints:

| pose | pre-slice | shipped | Δ luma | Δ% |
|---|---|---|---|---|
| centre_mid | 34.99 | 34.14 | −0.85 | −2.44 |
| north_mid | 35.87 | 35.02 | −0.85 | −2.36 |
| centre_wide | 63.00 | 62.60 | −0.40 | −0.64 |
| east_mid | 66.25 | 66.08 | −0.16 | −0.25 |
| loc_d_control | 59.95 | 59.81 | −0.14 | −0.23 |

Effective multiplier at dusk is `1 − 0.75×0.333 = 0.75`, producing a broad but
very slight dimming (high changed-pixel %, tiny luma delta) — the expected
signature, and nowhere near the aggressive dusk behaviour a hard `(1−n)` was
feared to cause.

## 6. Gates

- `check-contracts.sh` — **69 passed, 0 failed**
- `build_contract_harnesses.py --run` — **16/16 PASS**, 12/12 built
- tier1 — **5/5 PASS**, `DEPLOY_FINGERPRINT OK: exe sha=6c47e149353c matches worktree HEAD` on all five
- `check-env-registry.sh` — PASS (995 vars accounted)
- Tree clean (`--porcelain -uno` = 0) at commit time; deploy manifest **not** dirty

⚠ **Tier1 lane trap hit and corrected.** The documented tier1 command
auto-selected a foreign deploy and reported `DEPLOY_FINGERPRINT MISMATCH: exe
sha=8bc932f394e8` while still printing `PASS (5/5)`. `--deploy 0.3` is not a
name the smoke runner knows (valid: 0.4, 0.4c, 0.4d-rc1, 0.5.0, 0.5.1,
0.5-testing). The run above used explicit `--exe`. **A PASS line alone is not
evidence; the fingerprint line is.** This is the sixth lane to hit this.

## 7. Recommendation: DEFAULT-ON — and the case, not just the assertion

1. It fixes a defect that is **physically wrong**, not a taste call: a day-sky
   probe lighting a night scene, with no per-mission or per-time SH set.
2. **Day is untouched by arithmetic** — the whole daylight look the 2026-05-27
   flip was approved on is preserved exactly.
3. It **does not reproduce the killswitch's failure mode**: walls/turrets/domes
   dim 13–18%, not 29–46%.
4. The **user-approved mc2_05 night is verifiably not degraded** (≤0.90 luma).
5. Full gate coverage: `=1.0` restores the previous behaviour byte-identically,
   so the change is trivially revertible at runtime without a rebuild.

## 8. Honest limits / not claimed

- **Not user-verified.** Every judgement here is measured, not eyeballed by the
  user. The floor `0.25` is an evidence-picked knee, not an approved aesthetic —
  if the user wants night props brighter or darker, `MC2_STATIC_PROP_IBL_NIGHT_SCALE`
  tunes it live with no rebuild.
- **VK path is wired but not visually verified.** The same helper feeds the VK
  upload (`:9376`) so GL/VK agree by construction, but no VK capture was taken.
- **The truck/building contrast is not fixed** (§4) and this lane does not claim it.
- **A real per-night SH set remains the better long-term fix** — `kMissionShMap[]`
  is still empty. Night-scaling a day probe is the correct *cheap* fix; projecting
  an actual night-sky HDRI into a second SH set and mapping it per mission would
  be the correct *complete* one.
- `bldg_left` and `dome_a` still show a handful of ≥250 pixels; those survive
  from terms other than the IBL, so the IBL is no longer the clipping cause but
  is not the only contributor to peak brightness.

## 9. Lane hygiene

- Registered `IBL-NIGHT-SCALE-1`, deploy lane 0.3. Overlapping claims on
  `shaders/static_prop.vert` / `docs/tier1_env_vars.md` / `scripts/run_smoke.py`
  belonged to FIXTURE-GLOW-1 and PERF-HARNESS-TRUTH-1, **both DONE/RELEASED**
  with a clean tree; the shader was not touched at all.
- Captures used `--no-kill`; no foreign mc2.exe touched.
- Disk watched throughout: 14 GB → 12 GB → 14 GB free. **All 122 PNGs verified
  non-zero.**
- One dirty-tree exe was built and deployed mid-session (built before committing
  the linkage fix); it was **discarded and rebuilt clean** before any
  verification capture was taken. No measurement in this document comes from it.
