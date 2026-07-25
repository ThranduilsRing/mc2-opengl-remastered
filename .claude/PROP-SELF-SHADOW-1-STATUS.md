# PROP-SELF-SHADOW-1 — status (2026-07-25)

Lane: PROP-SELF-SHADOW-1. Worktree `A:/Games/mc2-main`, branch
`claude/nifty-mendeleev`, HEAD `05dbd1f3`.

**USER DEFECT (mc2_10 night):** *"the building has the parts in the 'sun' FULLY
lit, and the parts in the shade dark, but is also not accounting for self
shadowing at all"* — a wall facing the light reads full-bright even when it sits
behind a taller wing, in a recessed courtyard, in an inset doorway, or under an
overhang. Reads flat and cardboard-like.

**VERDICT: CONFIRMED, ROOT-CAUSED, AND ALREADY FIXABLE WITH SHIPPED CODE — NO NEW
CODE WAS NEEDED.** The remedy is two EXISTING default-OFF gates that compose:
`MC2_PROP_SHADOW_RECEIVE=1` (coarse wing-on-wing cast shadow) **+** `MC2_SSAO=1`
(the contact/recess/underside term). Both verified live this session. **No source
file was modified by this lane** — the deliverable is the diagnosis, the
empirical verification, and the review pack.

## Why props look flat — three stacked causes

1. **Props explicitly opt OUT of the dynamic self-shadow.** `static_prop.frag`
   writes `rc_gbuffer1_screenShadowEligible_noSelfShadow(N)` (the GBuffer1
   `a=0.25` class) — `shaders/static_prop.frag:610-616`. `shadow_screen.frag`
   then *deliberately skips* `sampleDynamicShadow()` for that class
   (`shaders/shadow_screen.frag:328-333`). Comment states the intent plainly:
   *"CPU never self-shadowed buildings; hard terminator on flat facets looks
   wrong."* So building-on-building and self-occlusion are switched off by
   design, not missing by accident.

2. **The baked base light is a per-LEAF constant, not per-vertex.** `v_argb`
   comes from `perVertexARGB` ← vertex attr `a_aRGBLight`
   (`shaders/static_prop.vert:428-432`, written at `:684`), fed by
   `child->GetARGBHighlight()` — **one scalar per `TG_Shape` leaf**
   (`mclib/bdactor.cpp:7135`, building path `:4259`). Every vertex of a leaf
   shares one value, so the baked seed contributes zero intra-leaf variation.
   `calc_light()` (`shaders/include/lighting.hglsl:216-297`) then adds
   `base_light + object_lights` with **no occlusion attenuation term anywhere** —
   there is no AO in this path at all.

3. **What remains is a pure per-face `max(N·L,0)` directional term** — exactly
   the "fully lit vs dark by facing, nothing in between" the user described.

Net: causes 1+2 mean nothing in the prop pipeline can darken a light-facing
surface that happens to be occluded.

## Answers to the mission's design questions

- **Are props excluded from CASTING?** **No — they already cast.** Buildings are
  in the dynamic caster set by default (`mclib/txmmgr.cpp:2940-2963`,
  `includeBldg = !s_skipBldgInDynamic`, and `s_skipBldgInDynamic` is only true
  when `MC2_STATIC_PROP_BUILDING_SHADOW` is explicitly set). The same set is
  replayed into every CSM cascade at `txmmgr.cpp:3170-3182`
  (`ZoneScopedN("RenderLists.DynShadowCSM")`). **The caster set needed no
  widening** — so the sibling lane's alpha-discard constraint was never at risk
  from this work.
- **The `setRecipeNoShadow` far/impostor exclusion is FOLIAGE-ONLY** and applies
  only to `lod > 0` tree cards (`mclib/bdactor.cpp:7259-7262`: *"far/impostor
  LODs (flat alpha cards) must NOT cast solid rectangular blob shadows"*).
  Buildings never enter that path; tree LOD0 still casts. **Untouched — no
  alpha-card foliage regression risk from this lane.**
- **Does any of this work at NIGHT?** **Yes.** `enableDynamicShadows` is not
  day/night gated — `gos_postprocess.cpp:2971-2972` sets it purely from
  "did the shadow pass produce a texture". The CSM light direction comes from the
  mission's own light vector (`gos_GetTerrainLightDir`,
  `gameos_graphics.cpp:10274-10281`) and is never zeroed at night. `MC2_NIGHT_FOG_DIM`
  is edge-fog only and does not touch shadows. Confirmed empirically below.
- **Which term produces the full-bright faces?** The per-vertex `calc_light`
  N·L on top of the flat per-leaf baked seed — cause 2+3 above.

## The remedy (both gates already shipped, both default-OFF)

| Gate | Default | What it contributes |
|---|---|---|
| `MC2_PROP_SHADOW_RECEIVE=1` | OFF | Opts the `a=0.25` prop class back INTO `sampleDynamicShadow` (`shadow_screen.frag:333`). Gives **coarse** wing-on-wing / prop-on-prop cast shadow. Floor 0.4, `min()`-combined with the static map (no double-darken). Commit `33a2b524`. |
| `MC2_SSAO=1` | OFF | Half-res screen-space AO over scene depth + GBuffer1 normals, which props already write. Gives the **fine** contact term: recesses, inside corners, undersides, inset doorways. (`MC2_FEATURE_SSAO`, `gos_postprocess.cpp:910`.) |

Neither alone is sufficient: RECEIVE is too coarse to resolve a doorway reveal
(the cascades are map-sized), and SSAO alone cannot put one wing's shadow on
another. **Recommend both ON together.**

## Evidence

All captures via `scripts/run_visual_capture.py`, engine glReadPixels readback,
3840x2160, `MC2_SMOKE_FIXED_TIMESTEP=1`, **every arm deterministic
(`det=True`, `nondeterministic=False`)**. Artifacts in `scratchpad/pss1/`.

**Capture vehicle (honest):** the deployed `0.4` install
(`A:/Games/mc2-opengl/mc2-win64-v0.4`), exe fingerprint `16c2244a`. HEAD is
`05dbd1f3`, so `deploy_status` reports FINGERPRINT-MISMATCH. This is
**deliberate and valid here**: `git diff 16c2244a..HEAD` is **empty** for
`shadow_screen.frag`, `render_contract.hglsl` and `static_prop.frag`, and the
`gos_postprocess.cpp` delta contains **no `propShadowReceive` hunk** (verified).
The gate's entire implementation is byte-identical between the deployed build
and HEAD, and the gate commit `33a2b524` is an ancestor of `16c2244a`. **No
build or deploy was performed by this lane** — nothing was rebuilt, so no lane
contention and no manifest churn.

### DAY, mc2_01 airport buildings — quantitative (clean, no animation)

vs the same gate-OFF baseline, per bookmark:

| Pose | RECEIVE only | RECEIVE + SSAO |
|---|---|---|
| airport_bldgs_close | 0.25% changed, **100.0% darker** | 14.70% changed, **14.62% darker** |
| airport_bldgs_low | 0.72% changed, **100.0% darker** | 1.54% changed, **1.49% darker** |
| airport_bldgs_mid | 0.73% changed, **100.0% darker** | 4.62% changed, **4.30% darker** |

**Structural proof:** with RECEIVE alone, **every single changed pixel is
DARKER — zero brighter pixels**, max darkening ~128 (consistent with the 0.4
floor). A pure occlusion term can only darken; a lighting bug or animation drift
could not produce a one-sided distribution. Adding SSAO multiplies coverage
**20-60x** while staying ~99% darker (the 0.05-0.32% brighter is AO dither /
bilateral-upsample noise). This is the quantitative pass the mission asked for.

### NIGHT, mc2_10 walled compound — categorical

All six saved mc2_10 night bookmarks are distant overviews (alt 4954-6045) that
cannot resolve intra-building occlusion. Composed four close poses over the
tower/compound complex (~3500,4600, per LIGHT-CONE-MODELS-1's `lc_close`);
archived as `08_prop_self_shadow/mc2_10_compound.bookmark.json`.

- `compound_low` gate-OFF **reproduces the user's screenshot exactly**: a tall
  control tower stands immediately beside a large multi-wing building and casts
  **nothing** onto it; every face uniformly bright.
- OFF vs both-ON on that building (review image 01): contact darkening appears at
  the **rooftop penthouse / roof-deck junction**, under the **parapet lip**, in
  the **recessed window bays**, in the **inside corner where the near wing meets
  the rear block**, and in the **garage doorway recess**. The building reads
  three-dimensional instead of cardboard. **This is the user's requested outcome.**
- Wall/dome cluster (review image 02, RECEIVE only): dome flanks and the crevice
  between adjacent domes darken; buttress slopes darken.

**MEASUREMENT CAVEAT — stated plainly, as instructed.** The night pixel diff is
**not trustworthy** and I did not rely on it. `compound_mid` shows 6.66% changed
of which **5.19% is BRIGHTER** — that is the sibling LIGHT-REALISM-1 sweeping
searchlight animation, not this gate. The night verdict above is **categorical
(a named recess that was flat is now visibly darker)**, per the mission's
instruction. The clean quantitative pass is the day table. Note the day poses
independently corroborate the mechanism with a zero-brighter distribution.

**Exaggeration:** the shadow-classification debug view exists
(`shadow_screen.frag` `debugMode==1`, blue=shadowed) but is **key-toggled only**
(`gameosmain.cpp:369-376`), with no env var, so it is unreachable headless.
Verify-by-exaggeration was therefore done by **stacking** the two gates rather
than by over-driving a strength constant (neither gate exposes one; the 0.4 floor
is hard-coded).

## RESIDUAL / BLOCKER for ever defaulting RECEIVE ON

**Alpha-test lattice props self-shadow as solid slabs.** On the mc2_01 day poses
the densest diff under RECEIVE-only is the **chain-link fence**, which darkens
**uniformly across the whole lattice** (review image 05). Mechanism: the shadow
depth pass has **no alpha discard** (the constraint the sibling lane documented),
so a fence writes its depth as a *solid* slab into the cascades; with RECEIVE ON
the fence then samples that solid slab and shadows itself everywhere. Foliage is
protected from this by `setRecipeNoShadow`, but **fences/railings are not
foliage and are not excluded.**

This is cosmetically minor at gameplay zoom but it is a genuine wrong-physics
artifact, and it is the reason I do **not** recommend flipping
`MC2_PROP_SHADOW_RECEIVE` to default-ON in this release.

Fixing it properly needs a design call I deliberately did **not** make
unverified: the clean fix is a distinct GBuffer1.a class for alpha-test props so
they can be excluded from *receive* — but that edits the render-contract mask
encoding shared with `shadow_screen.frag`, which is too wide a blast radius to
land without its own verified slice. The tempting cheap fix (mark alpha-test prop
recipes `noShadow`) is **wrong** — it would also stop fences casting onto
*terrain*, a visible regression. Logged as follow-up `PROP-ALPHA-SHADOW-CLASS-1`.

## Gate / merge safety

- **Zero source files modified.** Nothing to revert; `git status` shows only the
  new status doc + review-pack assets. Default behavior byte-identical by
  construction (both gates untouched and still default-OFF).
- No new env var; no registry, tier1-doc or smoke-allowlist change needed —
  `MC2_PROP_SHADOW_RECEIVE` and `MC2_SSAO` are both already registered,
  documented and smoke-allowlisted (`gate_status` verdict OK for both).
- No build, no deploy, no push. No `--amend`.
- Lane conflict honored: `shaders/static_prop.vert` (claimed by
  RUNWAY-MARKER-LIGHTS-1, and carrying the LIGHT-BILLBOARD-GPU-1 flag-bit-4
  billboard work) was **read only, never edited**.

## Perf cost if props join the shadow pass

**Caster-side: zero.** Props already render into the CSM cascades unconditionally
(`txmmgr.cpp:3170-3182`); this lane adds no geometry to the depth pass.
**Receive-side:** one extra `sampleDynamicShadow()` (the existing PCF ledger) for
prop pixels only, inside the already-running fullscreen `shadow_screen` pass —
i.e. it costs the same per-pixel work mechs already pay, over the prop pixel
coverage (0.25-0.73% of frame at these poses). SSAO is the larger of the two: a
half-res AO pass plus composite, which is its own documented default-OFF
trade-off. **Not separately profiled this session — no Tracy capture was taken;
treat the above as an argued bound, not a measurement.**

## Open / user-gated

1. **Taste grade the combination** (`MC2_PROP_SHADOW_RECEIVE=1 MC2_SSAO=1`) live —
   especially the 0.4 shadow floor and SSAO radius/strength.
2. **Fence slab artifact** — decide whether it blocks, and whether to fund
   `PROP-ALPHA-SHADOW-CLASS-1`.
3. Whether to ship either gate default-ON (recommendation: **not yet**, per the
   residual above).
