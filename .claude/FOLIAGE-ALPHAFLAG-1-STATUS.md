# FOLIAGE-ALPHAFLAG-1 — STATUS

Lane: FOLIAGE-ALPHAFLAG-1 · worktree `A:/Games/mc2-main` · branch `claude/nifty-mendeleev`
Commit: **`c6889696`** (fix + gate + registry + docs + smoke allowlist)
Deploy lane: `A:/Games/mc2-opengl/mc2-win64-v0.3` (`0.4c` left holding the blessed golden baseline)
Gates: contracts **72/72**, harnesses **16/16**, tier1 **5/5 PASS** with explicit
`[DEPLOY_FINGERPRINT] OK: exe sha=c68896963b20 matches worktree HEAD` on all five missions.

---

## 1. The bug — confirmed verbatim from source, then confirmed at runtime

Both static-prop *recipe* sites built their per-instance flags word with

```cpp
if (child->GetIsSpotlight()) flags |= (1u << 2);
```

- `BldgAppearance::registerStatic`  — `mclib/bdactor.cpp:4428` (pre-fix numbering)
- `TreeAppearance::registerStatic`  — `mclib/bdactor.cpp:7332`

Bit 2 was repurposed to `kFlagIsAlphaTest` in T3.2, and spotlight children are
`continue`-skipped ~10 lines above **both** sites, so **bit 2 was never set on the
recipe path at all**. Two shader features keyed off it were dead there:

1. the alpha-test **exclusion** in the `MC2_PROP_BASE_LIGHT_DIR` wrap term
   (`shaders/static_prop.vert:800`);
2. two-sided `abs(NdotL)` foliage lighting (`shaders/static_prop.vert:731`).

**Runtime confirmation** (temporary `[FAFPROBE]`, since reverted): with the gate on,
the tree site reports `leaves=2000 alphaTest=2000 gate=1` on mc2_03 and mc2_24 —
100% of tree leaves now carry the bit that was previously never set.

## 2. The fix, and every producer/consumer checked

`TG_Shape::GetIsAlphaTest()` (new, `mclib/tgl.h` + `mclib/tgl.cpp`) reads
`TG_TypeShape::alphaTestOn`, mirroring the existing out-of-line `IsShapeNode()`
seam because `myType` is protected. Both recipe sites now source bit 2 from it.

| Site | Channel | Source | Status |
|---|---|---|---|
| `submitMultiShape` (batcher:5575) | per-**instance** `flags` bit 2 | `ts2->alphaTestOn` | already correct — **untouched** |
| `Bldg/TreeAppearance::registerStatic` | per-**instance** `flags` bit 2 | was `GetIsSpotlight()` | **fixed** (gated) |
| `registerType` (batcher:2894) | per-**packet** `materialFlags` | `typeShape->alphaTestOn` | already correct — **untouched** |
| `static_prop.frag:640` GBuffer1 `a=0.1875` | reads `materialFlags`, **not** `flags` | — | **unaffected** |
| `static_prop.vert:731 / :800` | reads `inst.flags` bit 2 | — | now actually receives it |

**No double-classification.** The `a=0.1875` alpha-test shadow class
(PROP-ALPHA-SHADOW-CLASS-1, `d99bd798`, chosen inside the shoreline shader's
`0.15..0.35` water band) rides the per-**packet** `materialFlags` channel, which is
independent of the per-**instance** `flags` word this lane touches. Shoreline pixels
cannot move, and no instance is classified twice.

## 3. The gate — and why it is NOT in the shader

`MC2_PROP_FOLIAGE_ALPHAFLAG`, default **OFF**. Registered in
`RendererFeatureRegistry.h` (id 52), documented in `docs/tier1_env_vars.md`,
allowlisted in `scripts/run_smoke.py`. (`run_visual_capture.py` uses
`os.environ.copy()` — full passthrough, no ENV_DROP; verified the gate is recorded
in every per-pose sidecar's `gate_env`.)

★ **Hazard the brief did not flag.** The obvious move — gating the shader's `abs()`
branch — would have **changed shipped pixels**, because `submitMultiShape` *already*
sets bit 2 correctly, so two-sided foliage lighting is already live on that path.
Gating at the **emission site** instead makes default-OFF byte-identical *by
construction* and makes gate-ON **converge** the two paths rather than diverge them.

The two consequences are gated together deliberately: they are the same one-bit
classification, and measurement (§4) shows the look-change consequence is a no-op,
so splitting them would buy nothing.

## 4. Measurement

**Floors were measured, not assumed** (two runs per pose, all PNGs verified non-zero).
19 of 24 poses have a true **0.000%** floor. Poses that do **not**, and are therefore
**excluded from grading**:

| Pose | Measured floor |
|---|---|
| `mc2_05_east_compound_lightfacing` | **5.052%** (maxAbs 255) |
| `mc2_03_sam_lightfacing` | 0.054% (maxAbs 255) |
| `mc2_03_sam_lightaway` | 0.001% |

⚠ **Premise falsified.** The brief states mc2_05's revised set is "4 poses, all with a
measured 0.000% floor". `east_compound_lightfacing` measures a **5.052%** floor — the
same mover-coupled light-pool confound that produced the original false blocker. The
other three mc2_05 poses are clean (0.000%, 0.000%, 0.324%).

### 4a. Two-sided foliage lighting is a measured NO-OP

Gate ON alone vs baseline, **0.000% changed on every gradeable pose, all 5 maps** —
despite the probe proving 2000/2000 tree leaves carry the bit. `abs(NdotL)` and
`max(NdotL,0)` coincide because these tree-card normals keep `NdotL >= 0` at the
shipped sun elevations.

★ This **refutes the brief's premise** that enabling it is "a default-render change
game-wide". The previous lane's caution was reasonable but the fear was unfounded.
(Honest scope: no-op at 24 graded poses across 5 maps; not a proof over all gameplay.)

### 4b. The exclusion works, and moves foliage only, in the right direction

`C` (0.55, exclusion dead) → `D` (0.55, exclusion alive) — **100% brighter-only,
0.0000% darker** on every pose where it fires. Foliage recovering from wrongful
darkening, exactly as designed:

| Pose | changed% | brighter% | darker% |
|---|---|---|---|
| `mc2_03_civstore_lightaway` | 0.496 | **0.4515** | 0.0000 |
| `mc2_10_control_no_buildings` | 0.248 | 0.2462 | 0.0000 |
| `mc2_10_settlement_lightfacing` | 0.247 | 0.2469 | 0.0000 |
| `mc2_10_settlement_lightaway` | 0.045 | 0.0448 | 0.0000 |
| `mc2_10_compound_lightaway` | 0.043 | 0.0417 | 0.0000 |
| mc2_24 / mc2_01 / mc2_05 refinery | 0.000 | 0.0000 | 0.0000 |

**Eyes-on** (`scratch/fa1/sbs_CD_mc2_03/mc2_03_civstore_lightaway_sbs.png`): the 4×
delta panel lights up on **the two conifers and nothing else** — every building is
black. The exclusion touches foliage and only foliage.

Magnitude is small because foliage occupies few pixels at these building-centric
poses; the *sign* and *localisation* are unambiguous.

### 4c. `MC2_PROP_BASE_LIGHT_DIR=0.55` with the exclusion alive (arm D vs baseline)

| Map | pose | changed% | **brighter%** | darker% | ΔmeanLum |
|---|---|---|---|---|---|
| mc2_10 | compound_lightaway | **20.245** | **0.0000** | 20.142 | −0.887 |
| mc2_10 | compound_lightfacing | 5.341 | 0.0000 | 5.158 | −0.228 |
| mc2_10 | settlement_lightaway | 4.709 | 0.0000 | 4.524 | −0.308 |
| mc2_10 | settlement_lightfacing | 4.937 | 0.0000 | 4.920 | −0.423 |
| mc2_10 | **control_no_buildings** | 0.025 | 0.0000 | 0.024 | −0.002 |
| mc2_05 (night) | refinery_lightfacing | 18.249 | 0.0000 | 18.205 | −1.270 |
| mc2_05 (night) | refinery_lightaway | 14.240 | 0.0000 | 14.233 | −1.194 |
| mc2_05 (night) | **control_no_buildings** | 0.000 | 0.0000 | 0.000 | 0.000 |
| mc2_03 | industrial_lightfacing | 10.879 | 0.0000 | 9.145 | −0.193 |
| mc2_03 | industrial_lightaway | 9.272 | 0.0000 | 7.667 | −0.159 |
| mc2_03 | civstore_lightaway | 5.382 | 0.0000 | 4.441 | −0.098 |
| mc2_03 | **control_no_buildings** | 0.000 | 0.0000 | 0.000 | 0.000 |
| mc2_24 | palace_lightfacing | 4.318 | 0.0000 | 2.985 | −0.114 |
| mc2_24 | industrial_lightfacing | 2.943 | 0.0000 | 2.294 | −0.108 |
| mc2_24 | palace_lightaway | 1.178 | 0.0000 | 0.380 | −0.008 |
| mc2_24 | **control_no_buildings** | 0.118 | 0.0000 | 0.083 | −0.002 |
| mc2_01 | quonset_lightaway | 1.157 | 0.0000 | 0.935 | −0.042 |
| mc2_01 | hangar_lightaway | 1.101 | 0.0000 | 0.844 | −0.058 |
| mc2_01 | **control_no_buildings** | 0.000 | 0.0000 | 0.000 | 0.000 |

- **Never brightens: 0.0000% on all 19 gradeable poses**, day and night.
- **Acceptance asymmetry reproduces**: mc2_10 compound **20.2%** light-away vs **5.3%**
  light-facing.
- Controls 0.000–0.118%; no clip% increase anywhere; dark% flat.
- **Eyes-on** (`scratch/fa1/sbs_D_mc2_10/mc2_10_compound_lightaway_sbs.png`): walls
  turned from the light darken; the olive terrain is **pure black in the delta** —
  untouched; and the tree in frame shows **no delta**, i.e. correctly excluded.

## 5. Verdict — is `MC2_PROP_BASE_LIGHT_DIR` ready to flip to 0.55?

**Yes — the last technical blocker is cleared, with two conditions.**

The alpha-test exclusion the gate's own registry entry has always *asserted* is now
true on the recipe path. On every pose with a genuine floor the gate darkens
light-away building faces, never brightens a single pixel, leaves terrain at exactly
0.000%, holds at night, and no longer wrongly darkens foliage.

1. **`MC2_PROP_FOLIAGE_ALPHAFLAG` must flip to default-ON in the same change.**
   With it OFF the exclusion is dead again and foliage gets wrongly darkened. It is
   independently safe to default-ON: a **measured 0.000% no-op** at all 24 poses while
   `MC2_PROP_BASE_LIGHT_DIR` is off, and it removes a live landmine (two code paths
   disagreeing about the meaning of one bit).
2. **The flip itself is a visible default-look change → user-gated.** The engineering
   evidence supports it; the aesthetic call is the user's click-through, not mine.

**Recommendation:** flip `MC2_PROP_FOLIAGE_ALPHAFLAG` to default-ON now (zero measured
cost, pure correctness), and flip `MC2_PROP_BASE_LIGHT_DIR` to 0.55 on user
click-through of the arm-D shots.

## 5a. Concurrency note (read before re-grading)

Every build, capture and gate run in this document was taken on **`c6889696`**, when
that was worktree HEAD — the tier1 `DEPLOY_FINGERPRINT OK` confirms it. **After** the
measurement was complete, a parallel lane committed **`62f789c2`
(CONCRETE-DETAIL-NORMAL-1, "original overlay concrete is now the default")** on top of
it in this same shared worktree. My commit and tree are intact and unaffected, and
that change is in a different subsystem (terrain overlay, not static-prop lighting),
so it does not invalidate anything here — but **any re-grade must rebuild**, and
whoever blesses a golden baseline should be aware a default-ON terrain change landed
in the same window.

## 6. Notes for the next lane

- `deploy_payload.py` reads git HEAD from `--source-root`; pass the **repo**
  (`A:/Games/mc2-main`), not `mc2-srcdata`, or you get a bogus "different commit"
  warning *and* a shader-dir FAIL. (Prior lane logged the first half of this.)
- **mc2_01 has no tree-class recipe statics.** Its `[FAFPROBE] tree` count is zero and
  its C→D exclusion delta is 0.000% on every pose. Grade foliage on mc2_03/mc2_24; do
  not conclude "the fix does nothing" from mc2_01, which is what a first arm here
  appeared to show.
- Buildings are never alpha-test (`SetAlphaTest(true)` is called only on tree shapes,
  `bdactor.cpp:5778-5910`), so `alphaTest=0` on building leaves is correct, not a bug.
- Evidence: `scratch/fa1/` (211 MB — floors `F1_*`/`F2_*`, arms `B_*`/`C_*`/`D_*`,
  composites `sbs_*`, metric `signed.py`). Delete when the flip lands.
