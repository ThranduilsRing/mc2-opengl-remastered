# DROPPED-ITEMS-1 — STATUS

Lane: DROPPED-ITEMS-1 | Worktree: `A:/Games/mc2-main` | Branch: `claude/nifty-mendeleev`
Base HEAD at start: `ffc0f10a` (tracked tree CLEAN — 0 tracked modifications)
Date: 2026-07-25

**Outcome: RECON-COMPLETE, NO CODE LANDED. Three of the four items had their premise
refuted from source. The fourth has a newly-found, airtight root cause that belongs to a
live sibling lane's file.**

---

## META REVIEW — corrections to the brief

The brief and `.claude/SESSION-AUDIT-2026-07-25.md` §1 both assert *"NO CODE EXISTS for any
of them on any branch."* **That is false for items 2, 3 and 4.** Each was verified
independently (`git merge-base --is-ancestor` against HEAD, plus reading the live source):

| Item | Audit claim | Verified reality |
|---|---|---|
| 2 foliage shadow alpha-test | "never opened — no gate, no commit, no shader change anywhere" | **Shipped and default-ON since `b8f764b6` (2026-06-16)**, extended to the atlas path by `fddba31c` (2026-07-16) |
| 3 LOD impostor flash | "zero code on any branch" | **Mechanism exists** (`MC2_FAR_BACKDROP_LIT`, FAR-BACKDROP-LIT-IMPL-1) — registered, documented, smoke-allowlisted, but **default OFF** and with a **misaligned distance band** |
| 4 pool consumers | "never attempted in any form" | **Closed by `7dcbcd3a` (2026-07-22, POOL-CONSUMERS-1)**, whose own header names these two claims as its motivation |

Also corrected: the brief says the tree was clean at `1779bf1d`; actual start HEAD was
`ffc0f10a` (2 commits later). Tracked tree was genuinely clean; `preflight` reports
`dirty=true` only because of untracked `scratch/` capture dirs belonging to sibling lanes.

---

## Item 1 — World-shadow asymmetry, mc2_10 night overview — **OPEN, ROOT CAUSE FOUND**

This is the one genuine open defect. The ledger was right that the code path was never
read. It has now been read, and the mechanism is exact.

**Root cause: the shadow *matrices* are built from a sun-altitude-clamped sun, but the
screen-shadow *back-face guard* dots against the RAW unclamped sun.**

1. `mclib/camera.cpp:347` — `lightDirection.z = sin(lightPitch * DEGREES_TO_RADS)`.
2. `mc2_10.fit` authors `LightDirPitch = 180.000000` → `sin(180°) = 0.0` → the sun is
   **exactly horizontal**. Of the four verification maps, mc2_10 is the **only** one:
   mc2_01 = 50°, mc2_03 = 46°, mc2_24 = 75°.
3. `GameOS/gameos/gos_postprocess.cpp:5794` `mc2ClampShadowSunAltitude()` clamps the
   effective sun altitude to a 15° minimum (`MC2_SHADOW_MIN_SUN_ALT`, default 15). It is
   applied at **both** matrix builds — `:5867` (static full-map) and `:6291` (dynamic
   cascades). On mc2_10 `down = -fz = 0 < sin(15°) = 0.2588`, so the clamp **fires**; on
   the other three maps it is byte-identical no-op.
4. The clamp deliberately does **not** touch the shading light dir — its own comment,
   `gos_postprocess.cpp:5791`: *"Shading light dir (shadow_screen lightDir, terrain
   N-dot-L) is NOT touched."*
5. `gos_postprocess.cpp:2993-2994` therefore uploads `lightDir` from the **raw**
   `gos_GetTerrainLightDir()` — the horizontal vector.
6. `shaders/shadow_screen.frag:161` (and the non-CSM twin at `:225`):
   ```glsl
   float NdotL = max(dot(N, lightDir), 0.0);
   if (NdotL < 0.05) return 1.0;   // back-face guard
   ```

**Consequence.** With `lightDir` horizontal, `NdotL` is a purely *azimuthal* test — it has
no vertical component to lift it. Every surface whose normal leans away from azimuth
(`LightDirYaw 28.75° + 45°` = 73.75°) fails the guard and returns "fully lit", discarding
shadows the shadow map legitimately contains — because that map was rendered from a
**15°-elevated** sun that does light those surfaces. The world therefore splits into a
shadowed half and an unshadowed half along one compass axis. That is the reported
asymmetry, and the map-specificity (mc2_10 only) falls straight out of the pitch table.

**Fix shape (NOT landed — see Coordination).** Feed the back-face guard the *same*
clamped sun the matrices used, via a separate uniform (e.g. `shadowLightDir`), leaving the
shading `lightDir` untouched so brightness/N·L are unaffected. Gate: new killswitch
`MC2_SHADOW_GUARD_CLAMPED_SUN`, default ON (matches the clamp's own default-ON
convention), =0 reverts to today's raw-sun guard.

**Why it is not landed here:** `shaders/shadow_screen.frag` is claimed by lane
**PROP-NIGHT-LIGHT-3**, which was live 25 s before I checked. Editing it would be the
double-implementation failure the lane registry exists to prevent. **Handed over.**

---

## Item 2 — Palm-vs-pine foliage shadows — **PREMISE REFUTED; one stale-comment residual**

The alpha-tested shadow depth path the brief asks me to "implement" **already exists**:

- `shaders/shadow_static_prop.frag` — SHADOW-PROP-ALPHA-1. Performs `if (tex_color.a < 0.5)
  discard;` guarded by `u_materialFlags & ALPHA_TEST_BIT`, explicitly byte-matched to the
  colour pass. Atlas variant SHADOW-PROP-ALPHA-ATLAS-1 under `#define MC2_SHADOW_PROP_ATLAS`.
- Programs built at `gameos_graphics.cpp:5221` (`shadow_static_prop_alpha`) and `:5248`
  (`..._atlas`); selected by `resolvePropShadowProgram()`,
  `gos_static_prop_batcher.cpp:8222`, which falls back atlas → legacy-alpha → empty-frag.
- Gates `MC2_SHADOW_PROP_ALPHA` and `MC2_SHADOW_PROP_ALPHA_ATLAS` are **both default ON**.
  `gate_status(MC2_SHADOW_PROP_ALPHA)` → `verdict OK`, documented, smoke-allowlisted.

The brief's premise ("shadow depth pass writes depth with NO ALPHA DISCARD") is true only
of `shaders/shadow_depth.frag` (`void main(){}`) and the shared `shadow_instanced.frag` —
neither of which is the prop/foliage path any more.

**Therefore the palm-vs-pine difference is most likely STOCK-CORRECT.** With alpha-test
working, a palm (sparse fronds, edge-on to a low sun) genuinely casts a thin, broken
shadow while a pine (dense cone) casts a solid one. That is the physically right answer,
not a defect. *Stated as a strong evidenced hypothesis, not a closed verdict* — see
Verification gap.

**Real residual (small, genuine):** `mclib/bdactor.cpp:7430-7436` still excludes **every**
LOD ≥ 1 from casting:
```cpp
// SHADOW-FOLIAGE: far/impostor LODs (flat alpha cards) must NOT cast solid
// rectangular blob shadows (shadow depth pass has no alpha discard). ...
GpuStaticPropRegistry::setRecipeNoShadow(regIdx, true);
```
**That justification is now factually stale** — the depth pass *does* alpha-discard. The
exclusion outlived its reason. Re-enabling far-LOD casting is the correct follow-up, and
is exactly the second half of the brief's item 2. Mirror stale comment at
`GameOS/gameos/gos_static_prop_registry.cpp:267`. `mclib/bdactor.cpp` is claimed by live
lane **CONE-SPILL-COUPLING-1** — coordinate before editing.

---

## Item 3 — LOD impostor crossfade white flash — **PREMISE REFUTED; fix is a defaults change**

Confirmed as described: `TreeAppearance::selectActiveLOD()` (`mclib/bdactor.cpp:6985`) is a
**hard flip with no crossfade** — one `recipeIndex` in, one out
(`bdactor.cpp:6385 markVisibleChecked`). Hysteresis is a deadband on the integer LOD:
`_far = dist*1.08`, `_near = dist*0.92` (`bdactor.cpp:7004-7005`) → **736 / 864** at the
default `MC2_IMPOSTOR_DIST = 800`.

All LODs **do** share LOD0's light slot (`bdactor.cpp:7422`, `:7529-7531`) — confirmed — so
the actor-level SSBO light is *not* the source of the step. The step is in the authored
per-vertex seed `base_light`, copied once at type registration from `TG_TypeVertex::aRGBLight`
(`gos_static_prop_batcher.cpp:2646`); impostor cards carry a bright day-ish seed.

**A cure was already written for exactly this** — FAR-BACKDROP-LIT-IMPL-1,
`static_prop.vert:612-623`, which mixes `base_light` toward the night ambient so both LODs
converge. It is registered (`RendererFeatureRegistry.h:614`), documented, smoke-allowlisted
— and **default OFF** (`gos_static_prop_batcher.cpp:808-811`).

**Additional finding — the band is misaligned, and my correction differs from the recon's.**
Defaults are `_NEAR = 700.0`, `_FAR = 960.0` (`:812-819`) while flips occur in [736, 864].
For no visible pop the two LODs must shade identically *everywhere a flip can occur*, i.e.
across the whole hysteresis band — which requires the substitution to be **fully converged
(t = 1) by 736**, the lowest flip distance. So the correct alignment is
`_FAR ≈ 0.92 × MC2_IMPOSTOR_DIST` (= 736) with `_NEAR` below it (e.g. 600) — **not**
`NEAR=736 / FAR=864`, which would put `t = 0` exactly at the near flip (worst case).
Today `t` is only ~14 % at the 736 flip and ~63 % at the 864 flip, leaving the residual
step that reads as the flash.

**Fix shape (NOT landed):** derive `_NEAR`/`_FAR` from `s_impostorDist` in code instead of
hardcoded 700/960, then flip `MC2_FAR_BACKDROP_LIT` default ON. Touches
`gos_static_prop_batcher.cpp` + `RendererFeatureRegistry.h`.

---

## Item 4 — Movers/props receiving pool light — **CLOSED: both claims FALSE at HEAD**

Fixed by `7dcbcd3a` (2026-07-22, POOL-CONSUMERS-1) — verified an ancestor of HEAD. The
include's own header (`shaders/include/light_pools.hglsl:7-9`) names these exact claims:
> "Movers didn't receive their own headlight pools and props near pools stayed unlit: the
> pool loop only existed in the terrain shaders."

- Props: `shaders/static_prop.frag:453` `c.rgb += tex_color.rgb * calcLightPools(v_worldPos);`
- Mechs: `shaders/mech.frag:400` — identical line.
- Both under default-ON gates (`MC2_TERRAIN_LIGHT_POOLS`, `MC2_LIGHT_POOL_OBJECTS`).
- **No self-exclusion exists**: `calcLightPools` (`light_pools.hglsl:46-88`) is purely
  positional — no owner id / actor handle in the struct or the loop. A mover *is* lit by
  its own light, subject only to the `_plPad1` mover-dim (0.25).

**Residual caveat (fidelity, not darkness):** vehicles never reach the GPU mech batcher
(`mclib/gvactor.cpp` has zero `submitActor` refs); they take the legacy MLR path and get
the *same* `TG_Light` set via CPU per-vertex `SetLightList` (`gvactor.cpp:2605`). So
vehicles are lit, but Gouraud per-vertex rather than per-fragment, and are not subject to
the pool knobs. That is **VEHICLE-SELFLIT-\*** lane territory, not this one.

**Confound worth recording:** the pool cap was 64 until `5213d257` (2026-07-24) raised it to
256. Any "props/movers unlit" observation recorded before that date is confounded by lights
being silently dropped by pack order and should not be trusted as evidence.

**Handover to PROP-NIGHT-LIGHT-3:** pool consumption is *not* that lane's defect (it is
already closed and default-ON). The genuinely open prop-lighting hole is
`MC2_STATIC_PROP_NIGHT_TAGS`, which returns 0 unset
(`gos_static_prop_batcher.cpp:792-795`). Also: `static_prop.frag:453` is load-bearing —
anyone editing that file must not drop it.

---

## Verification gap — stated plainly

**No visual verification was performed, and no code was landed.** Per the STANDING CAPTURE
RULE this lane owes wide full-frame verdict shots at ≥3 locations across
mc2_01/03/10/24, day+night, plus a zoom sweep for item 3 and a same-frame palm/pine
comparison at low sun for item 2. None of that was run, because:

- A clean `mc2.exe` **was** built from `ffc0f10a` (exit 0, tracked-clean tree) — that part
  is done and attributable.
- But `deploy_payload.py` restricts deploys to six canonical folders, and the only one with
  a populated ~5 GB install is `mc2-win64-v0.4d-rc1`, which is claimed by **three** live
  lanes (REVIEW-ALLPOSES-1 — actively building a review pack — FORCE-ALL-VISIBLE-1, and
  PROP-NIGHT-LIGHT-3). Deploying there would have destroyed an in-flight review artifact.
- Its current exe is independently unusable as a baseline anyway:
  `deploy_status` → `FINGERPRINT-MISMATCH`, embedded sha `98ba41555e3f` vs expected
  `ffc0f10adcca`, manifest `6aaba3373ac5-dirty`.

Landing item 1 or 3 unverified would violate this lane's own adversarial-review bookend
("would the user still see this on a default launch?", "CLEAN-build evidence", "gate string
present in the deployed binary"). Recon findings are reported instead; the fixes are
specified precisely enough to be executed by whoever holds the contended files.

---

## ADVERSARIAL SELF-REVIEW

**Refuting my own claims:**

- *"Item 2/3/4 premises are refuted"* — could I be reading a sibling lane's uncommitted
  work? **No.** `git status --porcelain` showed **0 tracked modifications**, and each
  commit (`b8f764b6`, `fddba31c`, `7dcbcd3a`) was confirmed an ancestor of HEAD by
  `git merge-base --is-ancestor`. These are committed, in-HEAD facts.
- *"Item 1 root cause"* — is it just re-labelling, the exact sin the ledger caught last
  time? **No.** It is derived purely from source with no capture involved, it names a
  specific line (`shadow_screen.frag:161`) and a specific uniform provenance
  (`gos_postprocess.cpp:2993`), and it makes a **falsifiable map-specific prediction** that
  the data confirms: the clamp fires only where `sin(pitch) < 0.2588`, which across the
  four verification maps is mc2_10 **alone** (180° vs 50/46/75°). That is why the user sees
  it on mc2_10 and nowhere else. A re-labelling could not have predicted that.
- *"Would the user still see these on a default launch?"* — Item 1: **YES** (no gate
  involved; it is the default path on mc2_10). Item 2 palm/pine: yes, but probably
  correctly so. Item 2 far-LOD exclusion: **YES**, unconditional. Item 3: **YES** —
  `MC2_FAR_BACKDROP_LIT` is default OFF, so the cure never runs. Item 4: **NO** — closed
  and default-ON.
- *"Gate registered + smoke-allowlisted?"* — **N/A, no gate was added.** Recorded gap for
  whoever lands item 1: `MC2_SHADOW_MIN_SUN_ALT` itself is currently **ENV_DROP_RISK +
  UNDOCUMENTED** (`gate_status` → GAPS; read at `gos_postprocess.cpp:5797`, absent from the
  `run_smoke.py` passthrough allowlist and from `docs/tier1_env_vars.md`). A gate-ON smoke
  of that var today silently runs the gate-OFF path. Same flags on `MC2_IMPOSTOR_DIST`.
  Both should be fixed alongside.
- *"CLEAN-build evidence?"* — build yes (exit 0 from tracked-clean `ffc0f10a`); deploy and
  binary string-grep **no**, and I have said so rather than implying otherwise.

**Weakest point:** item 2's palm-vs-pine verdict is a source-derived hypothesis, not a
pixel verdict. It should not be closed until someone captures palms and pines in one frame
at a low sun angle. Item 1's mechanism, by contrast, is solid on source alone.

## Coordination log

- Lane registered; deploy lane `0.4d-rc1` flagged as clashing with 3 lanes → **not used**.
- `check_conflict` before any edit: `shaders/shadow_screen.frag` → PROP-NIGHT-LIGHT-3;
  `mclib/bdactor.cpp` → CONE-SPILL-COUPLING-1. **Both respected; zero files edited.**
- Tree left exactly as found: 0 tracked modifications, HEAD `ffc0f10a`, no commits, no push.
