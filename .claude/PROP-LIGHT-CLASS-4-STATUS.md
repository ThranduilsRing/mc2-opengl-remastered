# PROP-LIGHT-CLASS-4 — STATUS (2026-07-25)

**Verdict: NO CODE LANDED, DELIBERATELY. Both assigned fixes are hard-blocked on
live lane contention. What this lane delivers instead is the decisive evidence
that (a)'s assigned root cause is WRONG, and the gating fence evidence for (b).**

- Worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`
- Build: **CLEAN fingerprint `ad43dab55147`, `MC2_BUILD_GIT_DIRTY 0`**
- Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.3` (my registered, uncontended lane),
  md5 exe + shaders verified identical to worktree
- Evidence: `scratch/proplightclass4/`
- No push, no `--amend`, no edits to any contended file

| Objective | Outcome |
|---|---|
| (a) fix tree-class prop directional light | ❌ **ROOT CAUSE FALSIFIED** — the mechanism does not exist as described (§3). No fix is warranted as specified. |
| (b) flip `MC2_PROP_SHADOW_RECEIVE` ON | ❌ **BLOCKED** — flip target actively owned (§2); and the promised benefit is NOT observable (§4). |
| (b) verify fence artifact gone | ✅ **CONFIRMED GONE** (§4) — `d99bd798` works. |
| `MC2_SSAO` decision | ❌ Not reached — RECEIVE must settle first. |

## 1. META REVIEW — inherited premises, corrected

| # | Inherited claim | Reality (verified from source/tooling, not status docs) |
|---|---|---|
| C1 | Base is `d99bd798` | HEAD was **`4b343e72`**, then **`ad43dab5`** — it moved *during my build*. `d99bd798` is 5+ commits back. |
| C2 | Tree is clean | `preflight` says `safe_to_touch=false`, 194 dirty. But **tracked** tree is clean; all 194 are untracked scratch dirs. |
| C3 | Prior lane blocked by dirty tree | **Blocker retired.** `cmake/GenerateBuildFingerprint.cmake:37` computes dirty with `status --porcelain -uno` — **tracked only**. Untracked scratch cannot poison a build. VEHICLE-SELFLIT-5 waited on a blocker that had already lifted. |
| C4 | `MC2_LIGHTING_DEBUG_VIEW` resolves the unknown in one frame | True, **but** `gate_status` flags it **`ENV_DROP_RISK` + UNREGISTERED**: absent from the `run_smoke.py` allowlist. Run through `run_smoke.py` it silently no-ops. Used `run_visual_capture.py` (full env copy). |
| C5 | Deploy to the leased `0.4c` | **Rejected.** `0.4c` is shared by **six** fresh lanes, three touched 0.0h ago. Deployed to my own `0.3` instead. |

**A correction against the brief's own framing:** the brief instructed me not to
re-derive (a)'s root cause and to treat it as found. Following that instruction
would have shipped a wrong fix. The one-frame test it *also* mandated is what
falsified it. The test was right; the conclusion attached to it was not.

## 2. BLOCKER — every file both fixes need is owned by a live lane

11 lanes are FRESH (<12h), all in this worktree. `check_conflict`:

| File the fix needs | Owning lane | Age |
|---|---|---|
| `mclib/bdactor.cpp` (the (a) fix site) | CONE-SPILL-COUPLING-1 — note says "ACTIVE, bdactor.cpp:3292-3312" | 9.2h |
| `shaders/static_prop.vert` / `.frag` | EDGE-LIGHT-EMISSION-1 | 9.4h |
| `mclib/genactor.cpp` | EDGE-LIGHT-EMISSION-1 | 9.4h |
| `GameOS/gameos/gos_postprocess.cpp` (**holds the RECEIVE default**) | SHADOW-SUNCLAMP-1 | **0.0h** |
| `shaders/shadow_screen.frag` | SHADOW-SUNCLAMP-1 | **0.0h** |
| `RenderCore/RendererFeatureRegistry.h` | CEMENT-ALPHA-LAND-1 + SHADOW-SUNCLAMP-1 | 9.1h / 0.0h |
| `docs/tier1_env_vars.md` | 3 lanes | — |

This is not theoretical: **SHADOW-SUNCLAMP-1 landed `ad43dab5` (a
`shadow_screen.frag` change) while my build was running.** Editing the RECEIVE
default under that lane is a guaranteed collision, and any shadow evidence I
captured would be against a shader mid-rewrite.

Also live: **VEHICLE-SELFLIT-4** (9.3h) is tasked on defect (a) itself.

## 3. ★ Defect (a) — the assigned root cause is FALSIFIED

The brief's mechanism: `BldgAppearance::update` writes the terrain light sample
to slot 0 (SUN) while `TreeAppearance::update` writes it to slot 1 (AMBIENT) and
deactivates the sun ⇒ tree-class props get **no directional light** while
buildings keep it.

**The source half is real** — I confirmed both sites verbatim
(`mclib/bdactor.cpp:3638` sets slot 0; `:6706` sets slot 1; `:6719-6720` sets
`light->active = false`).

**The rendering half is false.** Four debug-view captures at the mc2_10 night
compound, all deterministic, all hashes distinct (probe armed):

`MC2_LIGHTING_DEBUG_VIEW=sun` (`v_dbgSun`) → **every static prop is black,
buildings included.** Not just tree-class. There is no building-vs-truck
directional differential to explain the defect, because *no prop* has a
meaningful directional term at this night pose.

Mean luminance, `compound_close`, fixed boxes:

| channel | truck | crate | bldg_tower | bldg_left |
|---|---|---|---|---|
| albedo (raw texture, no lighting) | **41.6** | 48.5 | **136.6** | 98.9 |
| final render | 96.3 | 101.5 | 138.9 | 121.9 |
| ambient | 57.7 | 70.7 | 196.4 | 143.1 |
| sun | 49.0 | 43.6 | 16.0 | 29.2 |

Two things kill the theory outright:

1. **The truck's own sun term (49.0) is HIGHER than the building's (16.0).**
   The exact opposite of the predicted deficit.
2. **Lighting gain (final ÷ albedo): truck 2.31×, buildings 1.02–1.23×.** The
   truck receives *more* lighting benefit than the buildings do. Lighting
   *narrows* the gap — truck/building luminance ratio goes from **0.305 in
   albedo to 0.694 in the final render**.

**The truck is not under-lit. It is dark because its authored texture is 3.3×
darker than the buildings' textures.** The albedo-channel capture is visually
near-identical to the final render for these objects.

This also independently corroborates VEHICLE-SELFLIT-5 (the objects ARE static
props) and refutes VEHICLE-NIGHT-LIT-2's "terrain is the sole writer / trucks are
baked" hypothesis — which BAKED-TRUCK-3 had already killed from the texture side.
Those two lanes contradicted each other; **SELFLIT-5 is the correct one**, and
NIGHT-LIT-2's result was almost certainly a third instance of the shader-probe
false-negative trap it documented against itself.

### What this means for the user's complaint

The complaint ("trucks near-black beside lit buildings") is **real and visible**
— reproduced in `scratch/proplightclass4/base/`. But it is an **art/albedo**
property, not a lighting-class bug. The candidate framings now are:

- accept as stock-correct (dark brown truck asset), or
- the buildings are **over-bright** at night (their ambient reads 196/255, near
  saturation) — i.e. fix the buildings, not the trucks, or
- an asset-level albedo lift for the specific dark scenery props.

**Do not "fix" `TreeAppearance::update`.** On this evidence it would change
nothing visible and would perturb foliage for no benefit. (The brief asked why
trees got that treatment — that question is now moot for this defect, and I did
not spend the budget on it.)

## 4. Defect (b) — fence blocker CLEARED, but the benefit is not observable

Env-only A/B, `MC2_PROP_SHADOW_RECEIVE` 0 vs 1, clean `ad43dab5` install.
Cross-check: the gate-unset scout run's hash equals the `=0` run's hash, so the
default really is OFF and captures are deterministic.

| pose | mission | changed px |
|---|---|---|
| saved_205644 (wide day) | mc2_01 | 0.10% |
| saved_220655 (wide day) | mc2_01 | 0.02% |
| base_close_a (close day) | mc2_01 | 0.085% |
| base_close_b (close day) | mc2_01 | 0.006% |
| base_close_c (close day) | mc2_01 | 0.003% |

**✅ The fence artifact is GONE.** `scratch/proplightclass4/fence_zoom.png`
(OFF top / ON bottom, 4× on the apron chain-link run): with RECEIVE **ON** the
alpha-test fence shows **no solid-slab self-shadow**. `d99bd798`'s 0.1875
GBuffer1.a class does its job. The sole named blocker to the flip is genuinely
retired.

**❌ But I could not demonstrate the promised benefit, and I will not claim it.**
The brief's acceptance criterion is "the user's building face going dark with
light from behind". At every pose I captured, RECEIVE ON changes **0.003–0.10%**
of pixels and produces **no visible building self-shadowing** — the tower faces
are indistinguishable OFF vs ON. I also saw **no fence shadow cast onto the
ground** in either state, which the brief expects as a YES.

So the honest state is: the *blocker* is cleared, but the *benefit* is
undemonstrated at these poses. Flipping a default-ON for a change that moves
0.006% of pixels and shows no visible improvement is not justified on this
evidence. Either better poses exist (low sun + tall isolated face) or the
feature is doing less than its description claims — and `ad43dab5` just changed
that shader, so this needs re-testing *after* SHADOW-SUNCLAMP-1 settles.

**`MC2_SSAO` was not measured.** It is downstream of a RECEIVE decision that did
not resolve, and Tracy timing against a shader another lane is actively editing
would be worthless.

## 5. Verification honestly scoped

- **Build PASS**, clean fingerprint, md5-verified deploy — the one thing prior
  lanes in this family could never get. Recorded so the next lane can trust it.
- **Captures: 8 runs, all `engine_capture_fired=True`, `nondeterministic=False`.**
- **NOT DONE and not claimed:** contracts 67/67, harnesses 16/16, tier1 5/5.
  No repo-content change was made, so there is nothing for them to gate; and
  `check-contracts.sh` is contended by CONTRACT-HARNESS-REPAIR-1.
- **NOT DONE:** the full 4-map × day/night × ≥3-locations matrix. It gates a
  code change that (§2) I am not permitted to make and (§3) should not be made
  as specified. Producing the full matrix to justify a falsified fix would have
  been expensive theatre.
- **mc2_03 / mc2_24 not captured** — same reason.

## 6. Recommended successor sequence

1. **Re-scope (a) as an ART/albedo question, not a lighting one.** The lighting
   path treats truck and building identically; the truck's texture is 3.3×
   darker. Decide: stock-correct, building over-brightness, or asset albedo lift.
   Tell VEHICLE-SELFLIT-4 before it spends another lane on the lighting path.
2. **Re-test RECEIVE after SHADOW-SUNCLAMP-1 lands and releases**
   `shadow_screen.frag` + `gos_postprocess.cpp`. Hunt a pose where a tall face
   turns away from a low sun. If the benefit still doesn't show, the honest
   action is to question the feature, not to default it ON.
3. **Fix the `MC2_LIGHTING_DEBUG_VIEW` ENV_DROP_RISK** — it is unregistered and
   missing from the `run_smoke.py` allowlist, so it silently no-ops through the
   standard harness. This is a live evidence-integrity hazard for every lane that
   reaches for it. (Not fixed here: `run_smoke.py` is contended.)
4. **Correct the record on VEHICLE-NIGHT-LIT-2 §3** — its "baked into terrain"
   conclusion is disproven twice over.
5. Consider the lane-registry pressure itself: 11 fresh lanes in ONE worktree,
   contending on the same ~8 render files, is why three consecutive lanes in this
   family have now delivered evidence instead of fixes.

## 7. Lane hygiene

- Registered `PROP-LIGHT-CLASS-4`, deploy lane `0.3`, zero file conflicts.
- Repo-content edits: **none**. This status doc + untracked `scratch/proplightclass4/`.
- Captures used `--no-kill` (own-children reap only) so no other lane's run was killed.
- Did not touch `0.4c` (six fresh lanes) or any contended source file.
- `d99bd798`'s malformed `@` subject line left alone as instructed.
