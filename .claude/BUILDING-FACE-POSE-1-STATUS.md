# BUILDING-FACE-POSE-1 — STATUS (2026-07-26)

Lane BUILDING-FACE-POSE-1. Worktree `A:/Games/mc2-main`, branch
`claude/nifty-mendeleev`.

- Commits landed (all mine): `63596bd9` (pose sets), plus this status doc.
- Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.3` (NOT `0.4c` — it holds the
  blessed golden baseline exe).
- Evidence: `scratch/bfp1/`. All PNGs verified non-zero. Disk held at ~13 GB
  free throughout; no build tree added (Tracy still compiled in none).
- **tier1: PASS (5/5) with explicit `DEPLOY_FINGERPRINT OK` on all five
  missions** (`exe sha=0745a13bda31 matches worktree HEAD`), run with an
  explicit `--exe` — `run_smoke.py` auto-selects a lane otherwise, which is how
  eight previous lanes got a PASS printed next to a MISMATCH.
  MOVER-COHERENCE 0 violations on all five. Contracts **70/70**.
- Contract harnesses: 14 PASS via `tools/run_contract_tests.py`;
  `shader_contract_harness` and `ibl_registry_contract_harness` are reported
  FAIL by that runner but **both exit 0 with all tests passing when executed
  directly**, so the runner verdict is a harness-runner artifact;
  `timing_math_harness` / `job_system_harness` are unbuilt (MISSING). None of
  this is attributable to this lane — **the whole change set is 5 JSON pose
  files and one Markdown document, no C++, no shaders** (`git diff --name-only
  b8211bce..HEAD` returns no code file).
- **★ Nothing about the DEFAULT RENDER changed. The user's building face still
  looks exactly as it did on a default launch** — this lane made the fix
  gradeable and found what blocks enabling it; it did not enable it.

| Objective | Verdict |
|---|---|
| **Author honest building-face poses** | ✅ **Done.** 5 tracked sets, sites derived from mission object data, every frame inspected, 5 candidates rejected on inspection or measurement. |
| **Re-grade `MC2_PROP_BASE_LIGHT_DIR` on a real building face** | ✅ **Done, and it WORKS at night** (§3). First demonstration against the actual complaint. |
| **Acceptance: light-away wall visibly darker than lit faces** | ✅ **MET on mc2_10 night** — walls darken 20–27 %, roofs ~9 %, sun-facing faces ~0–5 %, ground 0 % (§3). |
| **Foliage discriminator** | ✅ **Root cause found and named** (§5). Not a wrong predicate — the flag is never set on this code path. |
| **mc2_05 night not degraded** | ❌ **FAILS. The gate BRIGHTENS mc2_05 props by up to +71 %** (§4). This is new, measured, and contradicts the design's central safety claim. |
| **Recommended default** | **Leave OFF** (§7) — on §4 alone, not on doubt about the mechanism. |

---

## 1. ★ META REVIEW — what I checked before trusting the brief

The brief told me not to re-derive the root cause. I did check the load-bearing
premise it rests on, because the whole slice is void without it:

- **"Buildings are static props"** — CONFIRMED from source. `BldgAppearance::
  registerStatic` (`mclib/bdactor.cpp:4224`) registers buildings into the same
  GPU static-prop registry the shader serves. Had this been false,
  `MC2_PROP_BASE_LIGHT_DIR` could never touch the user's wall at all.
- **`ambientTerm` is the term that matters** — accepted, and consistent with
  what I measured (§3): the effect is large at night, tiny by day, which is the
  signature of scaling an ambient term while leaving the sun term alone.
- **`base_light` cannot go negative** — I checked `get_base_light`
  (`shaders/include/lighting.hglsl:70-140`); every branch returns a
  non-negative colour. This matters because it is what makes §4 a genuine
  contradiction rather than an obvious arithmetic slip.

**Correction to the inherited brief.** It states the gate's factor "can only
redistribute, never brighten". **That is false as shipped** — see §4. The claim
is true of the vertex expression in isolation and I could not fault that
algebra; it is not true of the rendered frame.

---

## 2. The pose sets — how the sites were chosen, and what I threw away

The blocker was real: `mc2_01_buildings.json` frames tarmac, runway, fence and
parked aircraft. I did not hunt for buildings by flying the camera around.

**Sites come from mission data.** Each mission `.pak` packet 1 is a flat object
list — `int32 count`, then per object `int32 objTypeNum; float x,y,z; float
rotation; ...` (`code/objmgr.cpp:1276-1305`). Type numbers resolve against
`data/objects/object2.pak`, whose `[ObjectClass] ObjectTypeNum` is the
`ObjectTypeClass` enum (`code/objtype.cpp:355-376`); BUILDING / TREEBUILDING /
TURRET / GATE / KLIEG_LIGHT / BRIDGE are building-class. Clustering those gives
each map's real compounds. Extractor: `scratch/bfp1/extract_buildings.py`.
Sanity check: no object fell outside any map's known half-extent.

This found the user's own screenshot. The 53-object cluster at
mc2_10 `(-2371, 2890)` is the two-storey stone barrack block with roof vents,
chain-link fencing, a wrecked vehicle and a lookout tower — at night.

**Five candidates were rejected, each for a stated reason:**

| Rejected | Why |
|---|---|
| mc2_10 depot `(3093,5026)` | **Measured**: two identical gate-OFF runs differ on **66.6 % / 69.8 %** of pixels, maxAbs 255 — movers cross it. An A/B there grades traffic. |
| mc2_05 spotlight yard `(-236,448)` | **Measured**: OFF-vs-OFF **84–91 %**. The animated cone glow never resolves twice. Removed; recorded as a known gap. |
| mc2_03 village `(546,3676)` | **Inspected**: the housing renders as ground shadows with **no building geometry above them**. |
| mc2_03 control `(4200,-4200)` | **Inspected**: off-map blackness. An empty frame passes every A/B trivially. |
| mc2_01 control `(-5200,-3200)` | **Inspected**: open water, no props. |

Shipped: `tests/visual/bookmarks/mc2_{01,03,05,10,24}_building_faces.json`.
Each site is captured at two rotations 180° apart (one frame holds the wall the
light falls on, the other the wall turned away); each set carries a no-building
control; all pass `check-visual-bookmark-honesty.py`. **mc2_05 had no pose set
at all before this**, which is why its night lighting had never been checked.

**mc2_01 is recorded as a weak site rather than dressed up.** Of 1000 placed
objects only 44 are building-class and all are low-rise Quonsets and domes — it
has no multi-storey flat wall. It is coverage and a guardrail arm, not the
acceptance test, and its file says so.

---

## 3. ★ NOISE FLOOR FIRST, THEN THE VERDICT

**Noise floor (two identical gate-OFF runs, per pose):**

| poses | changed % | maxAbs |
|---|--:|--:|
| mc2_10 compound ×2, control | **0.000 %** | **0** |
| mc2_10 settlement ×2 | **0.000 %** | **0** |
| mc2_03 industrial ×2, civstore, control | **0.000 %** | **0** |
| mc2_03 sam ×2 | 0.026 / 0.044 % | 255 (movers) |
| mc2_05 liao ×2, refinery, control | **0.000 %** | **0** |
| mc2_01 controls | **0.000 %** | **0** |
| *(rejected)* mc2_10 depot ×2 | **66.6 / 69.8 %** | 255 |
| *(rejected)* mc2_05 spotlight yard | **84–91 %** | 183 |

Every number below is on a pose whose floor is 0.000 %.

**mc2_10 NIGHT — the acceptance test, on the user's own compound:**

| pose | strength | changed % | maxAbs | mean lum OFF→ON |
|---|--:|--:|--:|---|
| compound_lightaway | 0.35 | 20.25 % | 19 | 34.59 → 34.02 |
| compound_lightaway | 0.55 | 20.29 % | 30 | 34.59 → 33.70 |
| compound_lightaway | 1.0 | 20.30 % | 55 | 34.59 → 32.99 |
| compound_**lightfacing** | 0.55 | **5.34 %** | 21 | 31.54 → 31.31 |
| control (no buildings) | 0.55 | 0.273 % | 16 | 35.31 → 35.30 |

**The asymmetry is the result.** Viewed from the light-away side the gate moves
20.3 % of pixels; viewed from the lit side, 5.3 %. Per-pixel darkening at 0.55
on the light-away view: median 9.6 %, p95 21.9 %, max 27.1 %. On the lit-side
view: max 16.0 %.

**And it lands on the right surfaces.** I rendered the per-pixel darkening as an
image (`scratch/bfp1/crops/ratio_compound.png`) and looked at it:
**vertical walls 20–27 %, roofs ~9 %, sun-facing roof-vent faces ~0–5 %, terrain
and water exactly 0 %.** With mc2_10's horizontal sun that ordering is what a
correct orientation term must produce. Side-by-side
(`scratch/bfp1/crops/verdict_wall.png`): the wall now reads as a distinctly
darker plane than its own roof, where before the two sat at nearly the same
value. **This is the first time the reported defect has been shown improving.**

A prediction of mine failed and is recorded: I expected wall:roof darkening
near 2:1 and hand-picked crops gave 6.7 % vs 5.9 %. The hand-picked boxes were
bad (mixed faces), not the mechanism — the per-pixel map, which needs no
hand-drawn mask, showed the true separation. I kept the failed measurement
rather than deleting it.

**Guardrails at 0.55:** mean luminance strictly **down** on every pose;
clipping (≥250) **unchanged**; near-black (<16) **+0.41 pp** on the worst pose.
At 1.0 near-black is **+1.71 pp** and max darkening 45.9 % — too close to the
failure mode that got the blunt IBL killswitch rejected.

**Day is nearly a no-op.** mc2_03 at 0.55: 5–11 % of pixels, maxAbs ≤50, mean
luminance −0.1 to −0.2, near-black flat, darkening mode 0–2 %. mc2_01: ≤1.2 %.
mc2_24: ≤4.3 %. Expected — the sun term dominates by day and is not scaled.

---

## 4. ★ THE BLOCKER — the gate BRIGHTENS mc2_05, and that map is user-approved

mc2_05 at strength 0.55, against a pose whose OFF-vs-OFF floor is **0.000 %**:

| pose | changed % | mean lum OFF→ON |
|---|--:|---|
| liao_compound_lightaway | 36.3 % | **83.20 → 90.62 (+8.9 %)** |
| liao_compound_lightfacing | 44.3 % | **76.67 → 81.90 (+6.8 %)** |
| refinery_lightaway | 14.2 % | 72.11 → 70.92 (down, correct) |

Signed map: **31.9 % of pixels BRIGHTER, 3.3 % darker; max brighten +92.7
luminance.** The building props themselves darken correctly; the brightening is
a large diffuse glow region. A flat surface inside it measures **+71.4 %**
(`scratch/bfp1/crops/g_480,60,560,110.png` — I looked at it).

**I tried to make this go away and could not:**

| Alternative explanation | Test | Result |
|---|---|---|
| mc2_05's day/night cycle landed the runs at different times | terrain patches outside the glow, OFF vs ON | **byte-identical** (Δ = +0.00) — no time-of-day shift |
| The run is just noisy | OFF vs a second OFF | **0.000 %**, maxAbs 0 |
| Merely *setting* the env var perturbs the run | `MC2_PROP_BASE_LIGHT_DIR=0.0` vs unset | **0.000 %** — exact no-op, so the shading term is the cause |
| The seed can go negative, so ×factor brightens | read `get_base_light` | **Refuted** — every branch returns non-negative |

So: reproducible, caused by the gate's shading term, and **not explained by the
vertex expression**, which cannot increase `lit`. I am not inventing a mechanism
to close the gap. **The distinguishing condition is that mc2_05 is the only tier
map with `DayToNightTime` set** (864000; every other tier map is −1), so it is
the only one running an intermediate `nightFactor` — and the night-keyed
consumers downstream of `lit` (`MC2_STATIC_PROP_IBL_NIGHT_SCALE`, default-ON;
the FAR-BACKDROP mix) are the place to look. mc2_10, a night map without a
cycle, shows no brightening. **That is the next lane's first job, and it is a
hard blocker on any default flip**, because mc2_05's night lighting is
explicitly user-approved territory.

---

## 5. ★ The foliage exclusion — root cause, and it is worse than a wrong predicate

The exclusion does not fire because **`kFlagIsAlphaTest` is never set on the
code path these props take.** There are two flag-word builders:

- `GpuStaticPropBatcher::submitMultiShape` (`gos_static_prop_batcher.cpp:5567-
  5576`) sets bit 2 from `TG_TypeShape::alphaTestOn` — correct, but this is the
  **dynamic fallback** path.
- `TreeAppearance::registerStatic` (`mclib/bdactor.cpp:7329-7332`) builds its
  own flags word and bit 2 is still `GetIsSpotlight()` — the **retired**
  meaning. Spotlight children are `continue`-skipped ten lines above
  (`:7326`), so **this path emits bit 2 exactly never.** The identical stale
  line exists for buildings at `bdactor.cpp:4428`.

On-screen forest trees go down the static-registry path, so they carry
`flags` bit 2 = 0 and the guard cannot fire. `SetAlphaTest(true)` *is* called on
every tree LOD (`bdactor.cpp:5737/5771`) and does propagate
(`msl.h:214`) — the flag is set on the type, it is simply never read here.

**This is not only my slice's bug.** The same bit drives the two-sided
`abs(NdotL)` foliage lighting at `static_prop.vert:731`, which therefore also
never fires for statically-registered trees. One root cause, two dead features.

**Why I did not land the one-line fix.** Adding the `alphaTestOn` read at
`bdactor.cpp:7332` would simultaneously switch on two-sided foliage lighting for
every statically-registered tree in the game — a **default-render change**,
since that branch is not behind my gate. That needs its own gate, its own
before/after on foliage, and its own click-through. It is a clean, well-scoped
slice and it is written up here rather than rushed in behind an unrelated one.

**Is the foliage darkening acceptable meanwhile?** On measurement it is small —
the no-building control moves 0.273 % of pixels at maxAbs 16, and the delta
image shows the leak is *only* pine silhouettes, with terrain and water at
exactly zero. But it is undesigned behaviour, and it is a second reason not to
flip the default.

---

## 6. Other findings worth a successor's time

- **Props render as shadow-only.** At several poses the geometry is absent while
  its ground shadow still draws: mc2_03 civilian housing, mc2_01 palms (present
  in one capture of the same pose, absent in another), two of four mc2_01
  Quonsets. It is capture-order dependent, which points at prop registration or
  LOD residency rather than culling. Not investigated.
- **`deploy_payload.py` reads git HEAD from `--source-root`.** Passing
  `--source-root A:/Games/mc2-srcdata` makes it compare the exe fingerprint
  against the *srcdata* repo's HEAD and print a scary, meaningless
  "built from a DIFFERENT commit" warning. Cosmetic but it costs a lane's
  attention every time.
- **The gate's registry doc string is stale.** `RendererFeatureRegistry.h` and
  `docs/tier1_env_vars.md` still describe it as acting on the `base_light`
  seed; `1a727339` re-pointed it at `ambientTerm`. Not corrected here — it sits
  in a sibling lane's claimed file.

---

## 7. Recommendation

**Keep `MC2_PROP_BASE_LIGHT_DIR` default-OFF.**

This is a *different* recommendation from the previous lane's, for a different
reason. The mechanism is no longer unproven — §3 shows it does exactly what the
user asked for, on the user's own compound, at night, with the correct
surface ordering and every brightness guardrail intact. If §4 did not exist I
would be proposing **0.55** as the default: it is where the wall/roof separation
is clearly readable (walls 20–27 %, roofs ~9 %) while near-black is only
+0.41 pp, and the effect saturates in reach by 0.55 while maxAbs keeps climbing,
so higher settings buy contrast on already-changed pixels rather than reach.

**§4 is what blocks it.** A term that is supposed to be incapable of brightening
raises a large region of mc2_05 by up to 71 %, on the one map whose night look
the user has already signed off. Shipping that as a default would trade a fix
the user asked for against a regression in territory they already approved.

Ordered next steps:

1. **Explain and fix the mc2_05 brightening** (§4). Start at the night-keyed
   consumers downstream of `lit`; mc2_05's intermediate `nightFactor` is the
   distinguishing condition. Until this is understood the gate cannot default ON.
2. **Land the `kFlagIsAlphaTest` fix as its own gated slice** (§5) — it revives
   two-sided foliage lighting as well, so it needs its own before/after.
3. **Then flip to 0.55**, with the §3 evidence as the justification.
4. Optional: make a stable mc2_05 spotlight-fixture pose so the approved
   fixtures themselves are gradeable (§2).

## 8. Not done / not claimed

- **Perf — NOT MEASURED.** Tracy is compiled in none of the build trees and the
  brief forbids adding one at 99 % disk. One dot + one mix per vertex is
  asserted cheap, not measured.
- **Golden regions not graded, no re-bless proposed.** No release artifacts
  touched (`release_assets/`, `releases/` are user-gated).
- **mc2_05 spotlight fixtures unverified** — pose removed as ungradeable (§2).
- **mc2_24 fortress pose is weak** — mostly pavement with one wall segment; kept
  for coverage, it carries little of the map's verdict.
- **The mc2_05 mechanism is unexplained**, deliberately (§4).
- **The sun's in-frame direction was inferred, not independently measured.**
  The claim "walls turned away from the light darken most" rests on the shader
  dotting against `u_terrainSunMC2`, which is the mission light direction by
  construction. What is *convention-free and measured* is the orientation
  dependence itself: the same buildings from two 180°-opposed rotations move
  20.3 % vs 5.3 % of pixels, and the per-pixel map separates walls from roofs
  from sun-facing faces. If someone wants the absolute sense nailed down, that
  needs a light-direction debug overlay, which I did not build.
- No push, no `--amend`, no foreign-process kills.
