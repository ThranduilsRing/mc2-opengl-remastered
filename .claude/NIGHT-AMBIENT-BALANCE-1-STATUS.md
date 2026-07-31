# NIGHT-AMBIENT-BALANCE-1 — STATUS (2026-07-26)

**Verdict: the night ambient IS inflated, and the mechanism is named, measured
and non-stock — a DAY-SKY image-based-lighting probe applied at full strength
at night with no night scale, default-ON. It causes literal white clipping on
building faces at night. It is NOT, however, the cause of the truck-vs-building
contrast the user complained about: killing it darkens everything roughly
proportionally and barely moves the ratio. NO DEFAULT FLIP LANDED — deliberately
(§5). A registered killswitch already exists and works today.**

- Worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`, HEAD `5c33fb48`
- Deploy `A:/Games/mc2-opengl/mc2-win64-v0.3` (my lane), manifest `src_commit 5c33fb48e1b8`
- Evidence `scratch/nightamb1/`
- Repo-content edits: **none**. No push, no `--amend`.

## 1. META REVIEW — premises checked from source

| # | Inherited claim | Verified? |
|---|---|---|
| Objects are static props, not GVAppearance | ✅ not re-derived; consistent with everything below (ground/terrain boxes are invariant, prop boxes are not) |
| Night ambient reads ~196/255 | ✅ **but the label was wrong.** `static_prop.vert:242` documents `v_dbgAmbient = ambientTerm + hemisphere-fill + SH-IBL`. The 196 is NOT the authored ambient — it is authored ambient **plus two port-added fill terms**. Prior lanes read it as if it were the mission's ambient. |
| Authored night ambient | ✅ `NightAmbientRed/Green/Blue = 32` (mc2_01 stock `.fit`); code default 32 (`camera.cpp:363`). Night path is `fAmbientRed = nightAmbientRed` verbatim (`camera.cpp:2029`) — **stock-faithful, no inflation in the C++ ambient path.** |
| A hardcoded night floor | ❌ falsified. `mc2NightAmbientFloor()` (`camera.cpp:4320`) defaults to **0** — `MC2_NIGHT_AMBIENT_FLOOR` unset = no floor. Not a default-on inflator. |

## 2. ★ Where the inflation actually comes from

`shaders/static_prop.vert:640-670`:

```
lit = seed + ambientTerm + sunColor * ndl;
lit += ambient_v1;                       // hemisphere fill — default OFF
if (u_iblShStrength > 0.0) lit += evalShL2(normal) * u_iblShStrength;   // SH-IBL
```

- **Hemisphere fill** (`MC2_STATIC_PROP_AMBIENT_V1`) — default **OFF**. Not implicated.
- **SH-IBL** (`MC2_STATIC_PROP_IBL_SH`) — default **ON** since 2026-05-27, strength **0.5**.

Three facts make it a port defect, not a taste call:

1. **The probe is a day sky.** `RenderCore/IblShCoeffs.h`:
   `Source HDRI: data/hdr/DaySkyHDRI063B_4K.exr`.
2. **There is exactly one SH set for every mission and every time of day.**
   `kIblShSets[]` has one entry (`"default"`); `kMissionShMap[]` is empty.
3. **Nothing scales it by night.** The shader has `u_isNight` / `u_nightFactor`
   and uses them in the base_light night-tag path (`:559`) — but the IBL add is
   outside that. The CPU upload is `s_iblShEnabled ? g_iblShStrength : 0.0f`,
   with no night term. **A missing night scale — one of the three mechanisms
   the brief asked me to look for.**

Stock MC2 (2001) had no image-based lighting. This term did not exist.

And it is **multiplicative into albedo** — `static_prop.frag:440`
`c = tex_color * vec4(litRgb, ...)` — so a fixed additive lift to `lit`
delivers proportionally more absolute brightness to a bright texture than a
dark one. (FIXTURE-GLOW-1 independently documents this same clamp/modulate
trap in its own registry entry.)

## 3. Measurement — mc2_10 night, `compound_close`

Env-only A/B on ONE binary. `MC2_STATIC_PROP_FIXTURE_GLOW=0` in **both** arms
(§6). Both runs `engine_capture_fired=True nondeterministic=False`.
Boxes documented in `scratch/nightamb1/measure.py`.

| box | IBL ON (default) | IBL OFF | %chg |
|---|---|---|---|
| truck | 95.80 | 66.43 | +44.2% |
| crate | 103.71 | 67.24 | +54.2% |
| bldg_tower | 162.45 | 109.03 | +49.0% |
| bldg_left | 141.03 | 91.44 | +54.2% |
| bldg_rear | 136.15 | 74.51 | +82.7% |
| **ground_ctl (terrain)** | **73.84** | **73.84** | **0.00%** |
| **ground_ctl2 (terrain)** | **70.51** | **70.51** | **0.00%** |

**Noise floor for this comparison is exactly 0.000** — the two terrain control
boxes are bit-identical, and pose `tower_area_204611` is byte-identical between
arms (hash `b4afb9538b0ccf57` both). The deltas above are 100% signal.

**Blow-out is literal, not perceptual:**

| | IBL ON | IBL OFF |
|---|---|---|
| `bldg_left` max channel | **255** | 220 |
| `bldg_left` px ≥250 | **2.76%** | **0.00%** |
| frame px luma>200 | 1.139% | 0.337% |

At night, a day-sky probe drives building faces into **hard 255 clipping**.

## 4. ★ But it does NOT explain the user's complaint

| ratio | IBL ON | IBL OFF |
|---|---|---|
| truck / bldg_tower | 0.590 | 0.609 |
| truck / bldg_left | 0.679 | 0.727 |
| truck / bldg_rear | 0.704 | 0.892 |
| crate / bldg_tower | 0.638 | 0.617 |
| crate / bldg_left | 0.735 | 0.735 |

Killing the IBL scales the whole prop population down together. Contrast
narrows on some pairs, widens on others, and never approaches parity. **This
confirms PROP-LIGHT-CLASS-4's conclusion rather than overturning it: the
truck-vs-building gap is an ALBEDO property.** The IBL is a separate, real,
independently-fixable defect that happens to sit on the same surface.

`scratch/nightamb1/verdict_compound_close.png` (ON top / OFF bottom) shows the
counter-risk plainly: with IBL off the compound reads convincingly as night and
the buildings stop glowing — **but the perimeter wall and turrets go near-black**,
which is the *same class of complaint the user already raised*. Trading blown
buildings for black walls is not obviously a win.

## 5. Why no default flip landed

1. **It would not fix the reported defect** (§4). The brief's acceptance test —
   contrast narrowing from the bright side — is not met.
2. **It creates new dark props** (§4 image).
3. **mc2_05's night is USER-APPROVED and I proved it is barely involved** —
   killing IBL there costs almost nothing, so it is neither a regression risk
   *nor* a justification. 5 locations, `nondeterministic=False`:

| mc2_05 pose | luma ON | luma OFF | Δ | changed px |
|---|---|---|---|---|
| saved_205843 | 111.74 | 111.19 | 0.55 | 2.93% |
| loc_b | 25.99 | 25.08 | 0.91 | 9.43% |
| loc_c | 93.05 | 93.07 | −0.02 | 2.34% |
| loc_d_control | 58.44 | 58.44 | −0.00 | 0.54% |
| loc_e_wide | 54.49 | 54.51 | −0.02 | 1.55% |

   mc2_10 is prop-dense (airbase); mc2_05 is not. The defect is concentrated
   where props are dense.
4. **The tree is not mine to flip tonight** (§6).
5. **The user already has the toggle.** `MC2_STATIC_PROP_IBL_SH=0` is
   registered, tier1-documented, smoke-allowlisted (`gate_status` verdict `OK`),
   and present 6× in the deployed exe. No code needed to try it.

**The correct fix, when the user wants it, is not the killswitch — it is to
multiply the IBL term by `(1 - nightFactor)`** (or author a night SH set into
the now-empty `kMissionShMap`). That keeps the day look the 2026-05-27 flip was
approved on, and removes only the physically-wrong night contribution. That is
a one-line shader change plus a gate, and it is a *default-behaviour* change on
a surface a sibling lane is actively rewriting — a deliberate slice, not a
drive-by.

## 6. ⚠ Build attributability — disclosed

`git status --porcelain -uno` was clean at session start. During my build HEAD
moved to `19e8be13` and back to `5c33fb48`, and the tree became **dirty with
FIXTURE-GLOW-1's uncommitted edits** to `shaders/static_prop.{vert,frag}`,
`gos_static_prop_batcher.cpp`, `RendererFeatureRegistry.h`. My exe therefore
carries that lane's WIP and `MC2_BUILD_GIT_DIRTY=1`.

Handled, not hidden:
- Their new gate `MC2_STATIC_PROP_FIXTURE_GLOW` is **DEFAULT-ON**, so I set it
  to `0` in **both** arms — their feature is fully neutralised on both sides.
- Both arms are the **same binary**, so every delta in §3–§5 is attributable to
  my one env variable regardless of the tree state.
- I did **not** stash, revert, commit, or otherwise disturb their working tree.

What this does mean: the **absolute** luminances above are from a tree carrying
a sibling's WIP. The **deltas and ratios** are sound. Re-measure absolutes after
FIXTURE-GLOW-1 commits.

## 7. Verification honestly scoped

- Build PASS; deploy PASS (manifest `src_commit 5c33fb48e1b8`, PDB hash match).
- 4 captures, all `engine_capture_fired=True`, all `nondeterministic=False`.
- Measurement noise floor **0.000** (bit-identical terrain controls + one
  byte-identical pose across arms) — stronger than the mc2_10 27.3% pose noise
  the brief warned about, because the A/B is env-only on one binary.
- **NOT DONE, NOT CLAIMED:** contracts 69/69, harnesses 16/16, tier1 5/5. No
  repo-content change was made, so there is nothing for them to gate.
- **NOT DONE:** mc2_01 / mc2_03 / mc2_24, and day captures. They gate a default
  flip that §5 says should not happen yet. Producing them to justify a change I
  am recommending against would be theatre.

## 8. Successor sequence

1. **Decide the night-IBL question with the user.** Show them
   `verdict_compound_close.png`. The honest framing: "your buildings are
   white-clipping at night because a day-sky probe is applied at night; here is
   it off; note the wall goes black — do you want it night-scaled rather than
   killed?"
2. **If yes → NIGHT-IBL-SCALE-1:** `lit += ibl * u_iblShStrength * (1.0 - u_nightFactor)`
   behind a killswitch, after FIXTURE-GLOW-1 releases the shader. Full 4-map
   day+night matrix then, because it IS a default change.
3. **The truck/building gap remains an ASSET question** — unchanged from
   PROP-LIGHT-CLASS-4. Two lanes have now independently measured it as albedo.
   Stop assigning it to lighting lanes.
4. **Correct the "196/255 night ambient" figure wherever it is quoted.** It is
   not the mission ambient (authored 32); it is ambient + port-added fill. That
   mislabel is what made this look like a C++ ambient bug for two lanes.

## 9. Lane hygiene

- Registered `NIGHT-AMBIENT-BALANCE-1`, deploy lane `0.3`, zero file conflicts
  at registration.
- Captures used `--no-kill`; no foreign mc2.exe touched.
- Disk watched throughout: 7.1 GB → 5.7 GB free; all PNGs verified non-zero.
- `docs/tier1_env_vars.md` is claimed by PERF-HARNESS-TRUTH-1 — I did not edit it
  (no new gate to document).
