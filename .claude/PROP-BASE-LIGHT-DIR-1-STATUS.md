# PROP-BASE-LIGHT-DIR-1 — STATUS (2026-07-26)

Lane PROP-BASE-LIGHT-DIR-1. Worktree `A:/Games/mc2-main`, branch
`claude/nifty-mendeleev`.

- Commits landed (all mine): `c2999bd7`, `02471415`, `1a727339`.
- Final verified HEAD: **`1a7273392b3e`**. Tracked tree clean (`--porcelain -uno`).
- Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.3` (NOT `0.4c` — see §6),
  `deploy_status` verdict **OK**, manifest `src_commit 1a7273392b3e`, not dirty.
- **tier1: PASS (5/5) with explicit `DEPLOY_FINGERPRINT OK` on all five
  missions** (`exe sha=1a7273392b3e matches worktree HEAD`). Contracts **70/70**.
- Gate `MC2_PROP_BASE_LIGHT_DIR`: `gate_status` verdict **OK** — registered
  (`RendererFeature::PropBaseLightDir = 51`, COUNT 52), documented
  (`docs/tier1_env_vars.md`), smoke-allowlisted (`scripts/run_smoke.py`),
  string present in the deployed exe (binary grep = 1 hit), term present in
  the deployed shader (5 hits). **Default 0.0 = OFF.**
- Evidence: `scratch/pbld1/`. All PNGs verified non-zero (0 zero-byte files).
  Disk held at ~13 GB free throughout.

| Objective | Verdict |
|---|---|
| Give prop base light a directional/orientation term | ✅ **Shipped, gated, default-OFF, proven live and monotonic.** |
| Do not brighten anything | ✅ **Held.** Mean luminance moves strictly DOWN; clipping unchanged at 0.000 %. |
| Do not flatten authored variation | ✅ Multiplicative on existing terms, not a tone remap. |
| Do not double-count with the IBL SH probe | ✅ IBL summand is never scaled. |
| **The brief's root-cause premise (`base_light`)** | ❌ **FALSIFIED BY MEASUREMENT (§1). The real term is `ambientTerm`.** |
| **ACCEPTANCE: user's night building face darker** | ❌ **NOT DEMONSTRATED (§4). No building-face pose was found.** |
| Alpha-test foliage excluded, as designed | ❌ **Exclusion does not fire (§5). Foliage IS being modulated.** |
| mc2_05 night not degraded | ❌ **NOT DONE (§7).** |
| Full 4-map × day/night × ≥3-location matrix | ❌ **PARTIAL (§7).** |
| Recommended default | **Leave OFF** (§8). |

---

## 1. ★ META REVIEW — the brief's central premise is false, and I measured it

The brief instructed: *"Do NOT re-derive"* the root cause, stating that the
building face looks lit because of `base_light`, a baked per-vertex constant
with no orientation term. That premise came from `SHADOW-DECISIONS-1` §2,
which derived it **from source reading alone and never measured it**. The
bookends rule says to check my own brief's premises from READER SOURCE. I did,
and it does not survive contact with the shipped build.

**The falsifying experiment.** I built the gate on `base_light` first, exactly
as briefed, and got **byte-identical output at every strength on every pose** —
including strength `1.0`, which multiplies the back-face seed by **zero**. That
is impossible if the term mattered. I then ruled out the boring explanations
one at a time rather than assuming:

| Hypothesis | Test | Result |
|---|---|---|
| Env dropped by the capture harness | `run_visual_capture.py:223` builds `env = os.environ.copy()` | Refuted — full passthrough |
| Capture harness insensitive / cached | Control: `MC2_SSAO_DEBUG=1` | Refuted — **100.000 % changed**, harness is live |
| Deployed GLSL not actually used | Unconditional `seed *= 0.25` edit to the DEPLOYED shader | No change — but see below |
| Wrong shader (buildings on `building_pbr`) | Unconditional `VertexLight *= 0.25` on deployed `building_pbr.vert` | No change |
| Wrong shader (legacy `gos_tex_vertex_lighted`) | Same probe on that shader | No change |
| **Static props not in frame at all** | `MC2_GPU_PROPS_DEBUG_MODE=8` (magenta "did this draw land") | **Refuted — 6.4 % prop coverage on `airport_bldgs_mid`** |

So `static_prop.vert` *is* running and *is* drawing those pixels, yet scaling
`seed` by zero changes nothing. The only consistent conclusion:

> **`base_light` is effectively ZERO on these props**, day and night. The
> baked per-vertex seed is not what lights them.

**Finding the term that does.** Probing the other orientation-free summand on
prop-dense night poses:

| Probe (mc2_10 night, prop-dense poses) | changed % | maxAbs |
|---|--:|--:|
| `seed` (base_light) × 0.25, and × 0.0 | **0.000 %** | 0 |
| **`ambientTerm` × 0.25** | **2.5 – 6.9 %** | **30 – 54** |

`ambientTerm` is the per-actor object-light AMBIENT accumulated from the
`ObjectLights` row — **orientation-free by construction**, exactly the defect
shape the brief described, just in a different summand. That is the real cure
site, and `1a727339` re-points the gate there.

**Why the prior lane's reasoning looked right but wasn't.** Its §2 argument
("a back-face has `ndl == 0` so no shadow term can help it") is *correct and
still stands* — it is the reason `MC2_PROP_SHADOW_RECEIVE` can never fix this.
Only the follow-on attribution of the residual brightness to `base_light`
was wrong, and it was never measured.

---

## 2. Mechanism as shipped

`shaders/static_prop.vert`, non-window branch:

```glsl
float propDirFactor = 1.0;
if (u_propBaseLightDirStrength > 0.0 && (inst.flags & kFlagIsAlphaTest) == 0u) {
    float wrap = 0.5 + 0.5 * dot(normalize(worldNormalMC2),
                                 normalize(u_terrainSunMC2));
    propDirFactor = mix(1.0 - u_propBaseLightDirStrength, 1.0, wrap);
}
...
lit = (seed + ambientTerm) * propDirFactor + sunColor * ndl;
```

One factor is applied to the **whole orientation-free portion**
(`seed + ambientTerm`). That covers the term that actually matters today and
stays correct if a future asset revives a nonzero baked seed.

**Trade-offs, as the brief asked me to state them:**

- **Shader vs CPU bake.** Chose the shader. Far/impostor LODs register via
  `TransformMultiShape_HierarchyOnly` and **never run the per-vertex light
  bake**, so a bake change could not reach them at all; a bake would also need
  re-baking per mission light direction. Cost of the shader route is one dot +
  one mix per vertex, on a path that already normalizes both operands for
  `ndl`. (Per-vertex cost was **not** measured — Tracy is compiled into none of
  the build trees and the brief forbids building a 30th tree at 99 % disk.)
- **Never brightens.** `propDirFactor ≤ 1.0`, equality only for a face pointing
  at the sun, and the sun term is left outside the multiply. So the term
  redistributes; it cannot add energy. This composes safely with the sibling
  `MC2_STATIC_PROP_IBL_NIGHT_SCALE` and moves in the *helpful* direction for
  the separate over-bright-at-night complaint.
- **No double-count with `MC2_STATIC_PROP_IBL_SH`.** That probe is an
  *additive* ambient irradiance summand added to `lit` further down and is
  **not** scaled here — its directional ambient is neither removed nor counted
  twice.
- **Authored variation preserved** — a multiply on existing per-vertex terms,
  not a replacement or tone remap.

---

## 3. Measured behaviour (noise floor published first)

**Noise floor.** Two identical OFF runs, mc2_01 buildings, 3 poses:
**byte-identical, 0.000 %, maxAbs 0.** mc2_10 night OFF/OFF likewise 0.000 %.
Every number below is above that floor.

**mc2_10 night, prop-dense scout poses — strength sweep (monotonic):**

| pose | 0.35 | 0.55 | 0.80 | maxAbs @0.80 |
|---|--:|--:|--:|--:|
| g_0_1 | 2.471 % | 2.476 % | 2.477 % | 49 |
| g_0_2 | 4.916 % | 4.947 % | 4.955 % | 38 |
| g_1_2 | 6.840 % | 6.879 % | 6.886 % | 28 |

**mc2_01 day buildings @0.55:** 0.392 % / 0.000 % / 0.965 %, maxAbs ≤ 22.
Day effect is smaller because the sun term dominates and is not scaled.

**Brightness / clipping guardrails (the constraint that mattered most):**

| arm | mean lum OFF → ON | clip ≥250 OFF → ON | near-black <16 OFF → ON |
|---|---|---|---|
| mc2_10 night @0.80 | 24.74 → **24.65** | 0.000 % → **0.000 %** | 34.769 % → 34.826 % |
| mc2_01 day @0.55 | 71.19 → **71.17** | 0.000 % → 0.000 % | 30.000 % → 30.000 % |

Luminance strictly **down**, clipping **unchanged at zero**, near-black
essentially flat (+0.06 pp at the strongest setting). **Nothing goes
near-black — the failure mode of the rejected blunt IBL killswitch does not
reproduce.** OFF is byte-identical to the pre-slice build.

---

## 4. ★ ACCEPTANCE NOT MET — no building-face pose was ever found

The acceptance test is the user's own frame: a night compound wall with the
light behind it reading visibly darker. **I could not demonstrate it, and I am
not going to claim it.**

- **`tests/visual/bookmarks/mc2_01_buildings.json` is misnamed.** I looked at
  the frames. `airport_bldgs_mid` is tarmac, runway, chain-link fence, parked
  aircraft and one building clipped at the bottom-left corner. The changed
  pixels in the day arm land on **a fence strip**, not a wall. (This also
  retro-explains SHADOW-DECISIONS-1 §4 finding its changed pixels "on a single
  aircraft prop" — that set has almost no buildings in it.)
- **mc2_10's prop-dense areas are forest, not the compound.** I scouted a 3×3
  grid over the map (`scratch/pbld1/mc2_10_scout.json`) under
  `MC2_GPU_PROPS_DEBUG_MODE=8` to find where props actually are. The three
  prop-dense poses (7 %, 9.8 %, 14 % coverage) are **all pine forest** — I
  looked at them (`scratch/pbld1/cmp_g_1_2.png`, `cmp_g_0_2.png`).
  `mc2_10_sunclamp` (the only existing mc2_10 set) has **0.05 % prop
  coverage** — it is a terrain set and is useless for this question.

So the mechanism is proven to work and to be safe, but **the specific defect
the user reported has not been shown fixed.** Locating a night compound pose
with a lit-from-behind wall is the first thing the next lane should do; it is
a bookmark-authoring task, not a code task.

---

## 5. ★ Open defect in my own change: the alpha-test exclusion does not fire

The design deliberately excludes alpha-test instances, because foliage/fence
cards are two-sided (the neighbouring `ndl` branch uses `abs()` for exactly
this reason) and a signed hemispherical wrap would darken whichever side
happens to be authored facing away.

**It does not work.** On the forest pose `g_1_2` the trees visibly darken with
the gate ON (`scratch/pbld1/tree_check.png` — I looked at it, canopies are
clearly darker on the ON side), and 33 053 pixels change. These tree types are
evidently **not carrying `kFlagIsAlphaTest`**, so the guard never fires and
foliage is being modulated after all.

Being honest about severity rather than rationalising it: the result *reads*
as a plausible, subtle darkening rather than an artifact, and it is well
within the brightness guardrails — but it is **not the designed behaviour**,
and "it happens to look fine" is not a verification. This needs either a
corrected predicate (find how foliage is actually flagged) or a deliberate
decision that foliage should be included. **It is a reason on its own not to
flip the default yet.**

---

## 6. Deviations from the brief's tooling instructions

- **Deployed to `0.3`, not `0.4c`.** `0.4c` currently holds the **blessed
  golden baseline exe `6c47e149`**. Deploying over it would have destroyed the
  reference the ratchet grades against. `0.3` was free (stale lock, no smoke
  lease) and is my registered lane.
- **`run_smoke.py` auto-selects lane `0.4`, not my deploy dir.** My first
  tier1 printed `PASS (5/5)` **alongside `DEPLOY_FINGERPRINT MISMATCH`
  (exe sha=8bc932f394e8)` — the exact trap the brief warned about, now for the
  eighth lane. It had smoked a foreign lane's exe. Fixed by passing
  `--exe <my deploy>/mc2.exe` explicitly. **Every tier1 claim in this document
  is from a run with explicit per-mission `DEPLOY_FINGERPRINT OK`.**
- **Two builds were needed per code change** because `build_fingerprint.h`
  stamps the sha at build time: committing after building leaves the exe
  stamped at the parent commit and `deploy_status` reports
  FINGERPRINT-MISMATCH. Commit → build → deploy, strictly.
- **Lane overlap noted, not ignored.** `FIXTURE-GLOW-1` claims
  `shaders/static_prop.vert` and `PERF-HARNESS-TRUTH-1` claims
  `docs/tier1_env_vars.md` / `scripts/run_smoke.py`. Tracked tree was clean at
  start and end; my edits are self-contained additive blocks. No push, no
  `--amend`, no foreign-process kills.
- **One self-inflicted build break**, recorded rather than hidden: the first
  registry entry quoted the user's report inline, terminating the C++ string
  literal and producing 100+ errors. Fixed in `02471415`.

---

## 7. Not done / not claimed

- **mc2_05 night non-degradation — NOT DONE.** The brief requires proving the
  USER-APPROVED mc2_05 night lighting is not degraded. There is **no mc2_05
  bookmark set** in `tests/visual/bookmarks/`, and I did not author one. Since
  the gate is default-OFF and OFF is byte-identical, mc2_05 is unaffected on a
  default launch — but the ON-state claim is **unverified**, and this is a
  hard blocker on any default flip.
- **Full matrix — PARTIAL.** Covered: mc2_01 day (3 poses), mc2_10 night (9
  scout + 3 sunclamp). **Missing: mc2_03 entirely, mc2_24 entirely, night on
  mc2_01/mc2_03/mc2_24, and the required no-buildings control pose.**
- **Golden regions — not graded, no re-bless proposed.** No release artifacts
  touched (`release_assets/`, `releases/` are user-gated).
- **Perf — NOT MEASURED.** Tracy is in none of the 29 build trees; the brief
  forbids building a 30th at 99 % disk. Per-vertex cost of one dot + one mix
  is asserted small but **not measured**.
- **`ambientTerm`'s CPU origin not traced.** I proved it is the dominant term
  by measurement but did not follow it back to the per-actor light gather to
  confirm *why* it is orientation-free. Worth doing before a default flip.

---

## 8. Recommendation

**Keep `MC2_PROP_BASE_LIGHT_DIR` default-OFF (0.0).**

The mechanism is real, correctly gated, safe on every guardrail the user
cares about (never brightens, no clipping, nothing near-black, authored
variation preserved), and byte-identical when off. But three things must land
before a default flip is defensible:

1. **A building-face pose that shows the user's defect and its fix** (§4).
   Right now we have a mechanism with no demonstration against the complaint
   that motivated it.
2. **The alpha-test exclusion must actually fire, or be deliberately dropped**
   (§5).
3. **mc2_05 night must be shown undegraded** (§7) — it is user-approved
   territory.

If asked for a value once those clear, the evidence points to **0.55**: the
effect saturates by ~0.55 (2.476 % vs 2.477 % at 0.80 on g_0_1) while maxAbs
keeps climbing, so higher settings buy contrast on already-changed pixels
rather than reach, and 0.55 keeps maxAbs ≤ 34 at night.

**The most valuable output of this lane is §1**: the root cause the next lane
should work from is `ambientTerm`, not `base_light`, and that correction is
now measured rather than reasoned.
