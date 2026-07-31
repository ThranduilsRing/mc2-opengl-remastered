# INCAPABLE-GATE REMEDIATION — synthesis of 4 batches + 4 independent refutations

Date: 2026-07-30. Worktree `A:/Games/mc2-main`, branch `claude/nifty-mendeleev`.
Source: `.claude/LIGHTING-HEADROOM-AUDIT.md` §3 ("the incapable-fix list"), 21 entries,
of which 19 were assigned to four batches. Each batch was then attacked by an
independent skeptic with no stake in its result.

**Nothing in this document was verified by looking at a rendered frame.** Engine
launches are hard-blocked in every lane that produced it. Every claim below is
source arithmetic, a checker exit code, or a capture taken by an earlier lane and
cited as such. Where a skeptic corrected a batch, **the corrected version is what
appears here**, and the correction is named.

---

## 1. THE LEDGER

| # | Gate / subject | Verified? | Action taken | Survived refutation? |
|---|---|---|---|---|
| **Batch 1 — static props / buildings** | | | | **BATCH REFUTED — see §6** |
| #4 | `MC2_TERRAIN_DECAL_FILL` — `max(litRgb, fill)` | Claimed CONFIRMED INCAPABLE | documented only | **NO** — the replacement premise is self-contradicting (0.13 < 0.20) and night-blind; the *conclusion* survives on a different argument |
| #5 | `MC2_STATIC_PROP_IBL_SH_STRENGTH` — "bake default 0.5" | CONFIRMED INCAPABLE | repaired (comment) | **Yes** — skeptic re-derived it independently and it holds |
| #6 | `MC2_PROP_BASE_LIGHT_DIR` | NOT INCAPABLE (audit refuted) | documented only | **Yes** — and *strengthened* by the skeptic's night finding |
| #7 | `MC2_STATIC_PROP_AMBIENT_V1` | NOT INCAPABLE (audit refuted) | documented only | **Yes** — same |
| **Batch 2 — vehicle diffuse** | | | | **BATCH SURVIVED** |
| #8 | `MC2_LEGACY_LIT_MC2_FRAME` (default **ON**) | NOT INCAPABLE — audit was **stale** | audit entry retracted, no code touched | **Yes** |
| #9 | `MC2_GV_LIGHTSOUT_CLEAR` | NOT INCAPABLE — audit read the branch backwards | repaired (smoke allowlist) — **uncommitted** | **Yes**, with the repair's delivery flagged as the weak point |
| #10 | `MC2_SUN_AXIS_FIX` | NOT INCAPABLE (narrow claim correct) | left alone | **Yes** |
| **Batch 3 — terrain / cement / wet lobe** | | | | **BATCH SURVIVED** |
| #11 | `MC2_TERRAIN_AMBIENT_FLOOR` (default **ON**, 0.35) ★ | **CONFIRMED INCAPABLE on 5 of 24 missions** | new gate `MC2_TERRAIN_AMBIENT_FLOOR_NIGHT`, default OFF | **Yes** — skeptic re-derived the mission pitches from the `.fit` files |
| #12 | `MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN` (default **ON**) | NOT INCAPABLE — audit **mis-filed a correctness fix** | documented only; docs blast radius corrected 1 → 5 missions | **Yes** — skeptic called this the most consequential call in the sweep |
| #13 | `MC2_CEMENT_EDGE_CURVE` | CONFIRMED INCAPABLE — **shipped at the identity** | **DELETED** (10 sites) | **Yes** — see §5 for the one flagged caveat |
| #14 | `MC2_CEMENT_LIT` | CONFIRMED INCAPABLE at stock | documented only | **Yes** |
| #15 | `MC2_RUNWAY_TONE_MATCH` family | **UNVERIFIABLE** (needs a running engine) | left alone | Yes, but it is the batch's weakest disposition |
| #16 | `MC2_TERRAIN_LIGHTING_V1` / `V2` | NOT INCAPABLE — *mis-targeted*, not dead | documented only | **Yes** |
| #21 | V3 wet/foam lobe | CONFIRMED INCAPABLE (wet lobe only) | documented only | **Yes** |
| §5.11 | `PavedOverlayClassMode()` "returns 1 when unset" | **FALSE at source** | repaired at 4 sites | **Yes** |
| **Batch 4 — water / shoreline** | | | | **BATCH SURVIVED** |
| #17 | `MC2_WATER_SHINE` — uniforms never declared | CONFIRMED INCAPABLE | **repaired** (uniforms + sun lobe) | **Yes**, with two undisclosed defects found (§4) |
| #18 | legacy screen-space shoreline pass | CONFIRMED INCAPABLE **and a live artifact** | default flipped to suppress — **uncommitted** | **Yes**, but flagged as an ungraded default flip |
| #19 | `MC2_WATER_SHORE_FEATHER` / `_MEANDER` | NOT INCAPABLE — parked at 0 by measured decisions | left alone | **Yes** |
| #20 | `MC2_WATER_SHORE_MASK` (default **ON**) | NOT INCAPABLE — **starved of data** (2 of 24 cooked) | documented only | **Yes** |

**Score.** 19 entries. **6 confirmed incapable** (#4, #5, #13, #14, #17, #21, plus #11
incapable-on-its-own-target). **9 refuted** — the audit was wrong that they could not
work (#6, #7, #8, #9, #10, #12, #16, #19, #20). **1 unverifiable** (#15). **1 false
claim in HEAD repaired** (§5.11). Of the four batches, **three survived** independent
attack and **one was refuted**.

**The audit's own hit rate on §3 was under 50%.** Two of the entries it invited the
deletion of — #8 and #12 — are **default-ON and live on every launch today**;
executing the list as written would have shipped confidently-wrong vehicle lighting
and removed terrain shadows outright.

---

## 2. WHAT THE OWNER GAINS

You asked two questions. Here are both answers, from this data only.

### "Why are my complaints not fixed?"

**Because six of the seven blockers were not clamp saturation. They were a
multiplicative predecessor sitting between the knob and the pixel, which nobody
checked on the mission the knob was aimed at.**

The audit's own framing was that clamp saturation was the story. It is not — it
explains 2 of 21 cases. The dominant mode, 6 cases, is **a gate that is arithmetically
zero on exactly the missions its author was trying to fix**. Concretely, from this
sweep:

- `MC2_TERRAIN_AMBIENT_FLOOR` ships **default-ON at 0.35** and is worth +22.9% to
  +31.6% on 19 missions. On mc2_05, mc2_10, mc2_19, mc2_21 and mc2_23 — the five
  **darkest** maps, the ones the complaint is about — its daylight gate composes to
  **exactly 0.0000**, so the shader block is skipped entirely and those frames are
  **byte-identical to the build before the fix landed**
  (`GameOS/gameos/utils/terrain_daylight.h:102` `kNightFullDeg = 10.0f`, `:123` returns
  −1 for any authored pitch >180, `:130` returns exactly 0 for altitude ≤ 10; the
  authored `LightDirPitch` on those five is 270/180/0/270/9).
- `MC2_TERRAIN_DECAL_FILL` presents as default-ON at 0.20 but reaches **zero
  fragments** on a default launch, because the flag bit it needs is only ever set
  behind the default-OFF `MC2_TERRAIN_DECAL` (`mclib/bdactor.cpp:4961`, sole producer).
- `MC2_WATER_SHINE` uploaded two uniforms **that no shader had ever declared** —
  `git log -S"u_waterSpecIntensity" --all -- 'shaders/*'` returns zero commits.
  `glGetUniformLocation` returned −1, silently, for the life of the gate.
- `MC2_WATER_SHORE_MASK` is default-ON and works — but only **2 of 24 missions** have
  the sidecar it consumes, and the failure is 100% silent at runtime.
- `MC2_CEMENT_LIT` cannot reach the runway at all at stock, and on the cement it *does*
  reach it is **monotonically darkening** (flat-ground ceiling 0.95104 < 1.0, so raising
  it can only dim). The advertised "24% gap closure" was closure by dimming the apron.
- `MC2_CEMENT_EDGE_CURVE` **shipped at the identity exponent 1.0** — `pow(x, 1.0) == x`
  — so at its default it moved zero pixels on every mission for its entire life.

Two more reasons your complaints outlived their fixes, both structural:

- **The audit you were handed is itself unreliable.** Entry #8 was invalidated by a
  commit (`5c04048c9`, 08:34) that was already an **ancestor of the audit's own HEAD**
  (`a75339bb0`, 22:57) — 14 hours earlier, same day. The premise was stale when written.
  Four lanes overturned four entries independently on the same day. §3 should be treated
  as unverified claims, not a settled list.
- **A checker you rely on is blind to a whole gate class.** Injection-proven this
  session: `scripts/check-gate-default-agreement.py` does not verify the default of any
  *float-valued* gate, because `classify_statement()` sees `atof()` and returns
  "value-read, not a boolean gate". That covers `MC2_TERRAIN_AMBIENT_FLOOR` (shipped
  default-ON at 0.35), `MC2_CEMENT_LIT`, `MC2_PAD_AREA_GLOW`, `MC2_WATER_SHINE` and every
  other numeric tunable. A registry lie about any of their defaults ships green today.

### "Is it all needed? I don't want bloat."

**Honest answer: this remediation made the tree bigger, not smaller.**

| | lines |
|---|---|
| Added across 3 commits + working tree | ≈ **+780** |
| Removed | ≈ **−125** |
| **Net** | **≈ +655** |
| Of which **executable** code removed (non-comment) | **≈ 29 lines** |

Essentially all of the +780 is `//` comments, registry doc strings and
`docs/tier1_env_vars.md` rows. Commit `158e8e9e6` (+192/−15) is **provably 100%
comment-only** — machine-verified twice, by its author and by its skeptic: zero
non-comment lines added or removed across four files. `ef12fbb45` (+118/−0) is one
markdown file.

The bloat you actually asked about — dead gates — went down by **exactly one gate**:

- `MC2_CEMENT_EDGE_CURVE` gone from all 10 of its sites (reader, uniform, `GLint`,
  `glGetUniformLocation`, per-frame upload, header decl, registry row, smoke
  passthrough, docs row, shader expression).
- `gos_SetWaterSpecIntensity()` gone — it had **zero callers and no header
  declaration** tree-wide.

Everything else that *looked* dead turned out to be load-bearing (#8, #12), measurably
live (#6, #7, #16, #19), or default-OFF with a documented purpose (#10, #15). **Deleting
on the audit's authority would have broken the game.** That is the honest justification
for the ratio: the expensive part of this work was *proving what not to delete*.

If you want the line count to actually go down, the one bloat item with a real
payoff left standing is the **legacy screen-space shoreline pass** (§3): it is
mis-aimed and could be deleted outright along with its five registry entries, but that
touches `ExecutorIslandId`, `PipelineId`, `ApplyPassId`, the VK screenfx bitmask and
frame-graph island-order assertions in `tests/unit/test_frame_graph.cpp` — it needs a
lane that can run those tests.

### Your actual visual complaints, one by one

| Complaint | Moved? | Why |
|---|---|---|
| **Concrete transition tiles** ("concrete shouldn't magically become a different colour") | **Diagnosis now firm; no fix landed** | Proven at source that *every* lighting-side lever is structurally incapable — the ring is an **albedo-source discontinuity**: the one-tile ramp band carries no cement word, so the shader keeps the dark baked colormap as `base` there. One dead lighting lever (`MC2_CEMENT_EDGE_CURVE`) deleted so nobody burns another day on it. The live lever is **`MC2_CEMENT_RAMP_WORD`** (`RenderCore/RendererFeatureRegistry.h:2683`), albedo-side, default-OFF. |
| **Runway colour** | **No — and two blind alleys closed** | `MC2_CEMENT_LIT` proven monotonically *darkening* and unable to reach the runway at stock; `MC2_RUNWAY_TONE_MATCH` is user-rejected and its "inert" premise is **unverifiable without a launch** (§6 caveat). Same albedo answer as above. |
| **Vehicle lighting** | **Yes, closer — and a disaster averted** | The audit said the vehicle light row was all-zero and the gate could not work. It is **populated**: the third `GatherLightsParameters` call, inside `addLightDataStructureWithPerActorColor` (`mclib/txmmgr.cpp:2081`/`:2088`, stored `:2096`), which the audit missed. `MC2_LEGACY_LIT_MC2_FRAME` is **default-ON and live on every launch today**. Magnitude still unmeasured. |
| **mc2_17 waterline** | **Partly — one new lever, one hazard confirmed** | mc2_17 is one of only three missions with a cooked `shoreline_mask.png`, so mask-side work *can* land there. `MC2_WATER_SHORE_MEANDER` at 75 **deletes 11.2 percentage points of frame water** on the `shelf_east_wide` pose (one-way, because `if (shore <= 0.0) discard;` is not zero-mean) — that is why it is parked at 0, and it is a measured hazard, not neglect. New: `MC2_WATER_SHINE` now actually reaches a shader and mc2_17 is one of the missions where it can show. |
| **Trees** | **Not addressed** | No batch owned the tree class. Sibling lane TREE-SUN-RESTORE-1 / Stage E (`290a3d1ae`) holds it. |
| **Sunk turrets** | **Not addressed** | Geometry/placement, outside the lighting sweep entirely. |
| **Black quads** | **Not addressed** | Not in the audit's §3, not in any batch. |
| **Dark buildings / props** | **A new candidate mechanism, not a fix** | See below — this is the most valuable single output of the sweep. |

### The one finding worth more than the 19 entries

**The SH-IBL ambient probe on every building is never rotated to the mission sun.**

`kIblShSets[]` has exactly one entry and `kMissionShMap[]` is empty
(`RenderCore/IblShRegistry.h:38`, `:51`), so all 24 missions share one coefficient table
projected from **one day-sky HDRI with that HDRI's own sun baked into it**.
`shaders/static_prop.vert:652` computes `worldNormal = a_normal * mat3(inst.modelMatrix)`
and feeds it **raw** to `evalShL2` (`:262`) — there is no rotation anywhere. From the
shipped coefficients the L1 band puts the bright axis at Stuff **+X**, azimuth ≈0°, with
a **7.71 : 1** swing between +X (irradiance 4.624) and −X (0.600). The sun the same
shader uses two lines later is the per-mission `u_terrainSunMC2` (`:732-734`), entirely
unrelated.

So on any mission whose sun is not near Stuff +X, **the dominant ambient term on every
building puts its brightest light on a side the sun is not on** — and no other prop knob
can cancel it, because they are all bounded by the same probe. This finding was raised
by Batch 1 and **independently confirmed by its skeptic** ("TRUE and valuable") and
re-verified for this report. It was deliberately *not* implemented: both repairs (rotate
the SH about world-up to the mission sun azimuth, or drop L1/L2 and keep only the
isotropic L00 term) change the default render on every mission and need a gate with an
argued default and a graded A/B — which no lane in this sweep could take.

---

## 3. REPAIRED AND NOW LIVE

**Exactly one change in the entire sweep alters a default launch.**

### `MC2_SHORELINE_LEGACY_SUPPRESS` → default **ON** (Batch 4 #18)

**Status: EDITED IN THE WORKING TREE, NOT COMMITTED.** See §6 note on delivery.

`GameOS/gameos/gos_terrain_lod_chunk.cpp:4053-4059` — the reader went from
`(v && v[0] == '1')` (default-OFF, and not even one of the two canonical idioms) to the
canonical default-ON killswitch `!(v && v[0] == '0')`. The `&&` with
`gos_TerrainLodChunk_IsShorelineMaskActive()` was removed from the call site
(`:3485`), so `MC2_TERRAIN_SHORELINE=0` no longer silently restores the artifact.

**What you will see change.** `shaders/shoreline.frag:22` is
`bool isWater = (myAlpha > 0.15 && myAlpha < 0.35);`. A GBuffer1.a producer census shows
water writes **0.0**, terrain writes **1.0**, and the *only* producers inside that band
are **static props at 0.25 and alpha-test lattice props at 0.1875**. The pass has
therefore never once touched water. What it has been doing on every default frame of
every mission is painting an **animated fbm foam brighten** — scrolled by `time`, pulsed
by `sin(time*2.0)`, up to **1.4× multiplicative** — onto **building and fence
silhouettes** within ~6 screen texels of terrain. With the flip, that stops.
`shaders/include/render_contract.hglsl` documents this collision itself, calling the
0.1875 class "load-bearing" *specifically because* `shoreline.frag:22` misclassifies the
0.25 prop class as water.

**Where to look — this is a temporal artifact, a still frame understates it.**
Use a **moving** camera on **mc2_01** or **mc2_03**, framing buildings or fences whose
silhouette meets the ground. Every mission with props near ground is affected; there is
no saturation trap here because the removed term was multiplicative on prop edges, not
additive into a clamped sum.

**The honest cost.** Those prop edges lose up to 1.4× of brightness *while a
dark-props complaint is open*. `MC2_SHORELINE_LEGACY_SUPPRESS=0` restores the shipped
behaviour exactly, and under `MC2_TERRAIN_LOD_CHUNK=0` nothing changes at all (the legacy
terrain path genuinely needs the pass and keeps it).

**This flip overturns SHORELINE-GLOW-1's explicit decision to defer this to a graded
capture.** Its skeptic did not refute the reasoning but named this the weakest action in
the batch: an ungraded default flip on every mission, decided from source by an agent
that cannot render a frame. The argument for overturning is that the brightness being
removed is *animated* — a shimmer crawling along the silhouette of a static building is
not a brightness judgement call. **You should grade it.**

Nothing else is live. `MC2_WATER_SHINE` stays at 0.0, the new terrain night floor stays
at 0.0, and everything else in the sweep is comments and docs.

---

## 4. GATED, AWAITING YOUR VERDICT

| Gate | Default | Flip criterion — **and where a delta can actually appear** |
|---|---|---|
| **`MC2_TERRAIN_AMBIENT_FLOOR_NIGHT`** *(new)* | **0.0 = OFF = byte-identical** | Grade **mc2_05** and **mc2_10** from a **ground-level** pose carrying *both* bare terrain and a concrete pad, **0 vs 0.30**. Expect terrain up, pads delta **0.00%**. **Saturation checked first:** the term is *additive* and terrain headroom to clip on these maps is **9.07× (mc2_10) to 20.83× (mc2_21)** — nothing saturates, so this recipe cannot come back empty for the wrong reason. **A non-zero pad delta is a STOP** — it means the `flatLitShare` exemption mask broke. Reader `GameOS/gameos/gos_terrain_lod_chunk.cpp:2956`, registry `RenderCore/RendererFeatureRegistry.h:892`, docs `docs/tier1_env_vars.md:772`. Default is OFF *by argument, not laziness*: the predecessor slice was rejected at merge review for a +59.1% mc2_10 ground lift, and you have explicitly praised how mc2_05 currently looks. This is a taste call. |
| **`MC2_WATER_SHINE`** | **0.0 = OFF = exact no-op** | Grade on **mc2_17** (bookmark `shelf_east_wide`) or **mc2_01**. **Do NOT use mc2_05/10/19/21** — the term is multiplied by `terrainLightDir.z` and skipped when ≤0, and their authored pitches (270/180/0/270) make it **exactly 0.0**. **The pose recipe currently in the registry and docs is wrong and should be ignored:** its skeptic computed the lobe and the operative condition is *camera elevation ≈ atan(sin(LightDirPitch)) with the sun ahead of the camera* — at the default `projectionAngle` of 30 the spec reaches 1.0, and at 88 (top-down) it collapses to **0.012**. Follow the docs recipe and you will see nothing and wrongly re-file the gate as dead. **Also undisclosed: this is GL-only.** `shaders/vulkan/water_geom.frag` declares neither uniform, so the gate is an exact no-op on Vulkan while live on GL, and no registry or docs row says so. |
| **`MC2_GV_LIGHTSOUT_CLEAR`** | OFF | Now **measurable for the first time** — it was absent from the smoke passthrough allowlist, so every prior A/B setting `=1` had it dropped at `Popen` and graded OFF against OFF. The mechanism is proven live end-to-end (`mclib/tgl.cpp:1926` / `:2129` → `:2481` / `:2776` → `addVertices` → shader). **What is NOT proven** is that a stock mission ever produces a ground vehicle that latches `lightsOut` and then legitimately needs it cleared. "Latent-correctness" remains the honest label. |
| **`MC2_PROP_BASE_LIGHT_DIR`** | 0.0 = OFF | Real and measured: on mc2_10 night `compound_lightaway` (OFF-vs-OFF noise floor **0.000%**, maxAbs 0) it changes **20.29%** of pixels at 0.55, walls −20..27%, roofs ~9%, terrain and water **exactly 0%** — versus only 5.34% from the light-facing viewpoint. That asymmetry is what a correct orientation term must produce. **BLOCKED on an unexplained result:** it also *brightens* mc2_05 by up to +71.4% in a large diffuse region, contradicting its own monotone-darkening algebra. Time-of-day and run noise were excluded byte-exactly and nobody can explain it. Do not flip until that is understood. |
| **`MC2_STATIC_PROP_AMBIENT_V1`** | OFF | **Mis-targeted, not dead.** Full strength buys +0.5% (mc2_22) / +2.7% (mc2_04) / +3.3% (mc2_09) sphere-mean on flat-reading maps but **+36.2% on mc2_05** and **+21.3% on mc2_23**. The delivered gain is the *residual headroom*, not the strength. The correct lever for a flat-reading map is the **probe strength in `data/visual_tuning.json` going DOWN**, which un-pins the clamp and makes every other knob start working. |
| **`MC2_SUN_AXIS_FIX`** | OFF | Left alone — the grievance was how it was *sold*, and `RenderCore/RendererFeatureRegistry.h:794` and `docs/tier1_env_vars.md:1395` already state its non-monotonicity verbatim and give an anti-saturation flip criterion pointing at mc2_03/mc2_13/mc2_19. **One staleness recorded:** its per-mission table was derived on the mech path, but `MC2_LEGACY_LIT_MC2_FRAME` is now default-ON, so `=1` today moves **ground vehicles too**. Re-measure the vehicle class before flipping. |
| **`MC2_WATER_SHORE_FEATHER` / `_MEANDER`** | 0.0 both | Left alone. Both are live tunables parked at 0 by *measured* decisions, not defects. `_MEANDER` at 75 deletes 11.2 pp of frame water on mc2_17 `shelf_east_wide`. One registration nit for someone: `_FEATHER` is documented and allowlisted but **not in `RendererFeatureRegistry`**, while its sibling is. |
| **`MC2_WATER_SHORE_MASK`** | **ON** | Not a flip decision — a **data** decision. Only **mc2_17 and mc2_24** have a git-tracked cooked `shoreline_mask.png`; the gate is a silent no-op on the other 22. Cooking the rest changes ~20 maps on a default launch and needs your grading. |

---

## 5. DELETED

**Total executable code removed: ≈29 lines. One gate, one dead function, two ledger lines.**

| What went | Why it is safe | Skeptic flag | Adjudication |
|---|---|---|---|
| **`MC2_CEMENT_EDGE_CURVE`** — reader `gos_TerrainCementEdgeCurve()`, uniform `u_cementEdgeCurve`, the `GLint`, the `glGetUniformLocation` lookup, the per-frame upload, the header decl, the `kAuxEnvVars` registry row, the smoke passthrough entry, the docs row, and the shader expression | **It shipped at the identity exponent.** The reader returned `1.0f` when unset and `pow(x, 1.0) == x`, so at its default it moved **zero pixels on every mission for its entire life** — meaning it has no killswitch or bisection value either, because there is no behaviour of its to turn off. Its row was in `kAuxEnvVars`, which is explicitly **not** enum-indexed, so nothing shifted. Tree-wide grep for all three symbols now returns **only tombstone comments** (`GameOS/gameos/gos_terrain_lod_chunk.cpp:4172`, `gos_terrain_lod_chunk.h:239`, `RenderCore/RendererFeatureRegistry.h:2666`, `shaders/terrain_lod_chunk.frag:307`). `scripts/check-env-registry.sh` exits **0**. | Flagged: one **documented historical A/B combination** is now non-reproducible — `.claude/CEMENT-WORD-RAMP-1-STATUS.md:142` records a measured `MC2_CEMENT_RAMP_WORD=1 + MC2_CEMENT_EDGE_CURVE=0.25` run that can no longer be re-derived on a current build. | **Accept the deletion.** This is archival-reproducibility loss on a *superseded* lever, not functional loss, and the docs tombstone states plainly that the var no longer exists and setting it does nothing — so it cannot become a new documented-vs-actual lie. |
| **`gos_SetWaterSpecIntensity()`** (`GameOS/gameos/gameos_graphics.cpp`) | Zero callers and no header declaration anywhere in the tree — only the definition existed. Cannot break the link; had no killswitch role because nothing could invoke it. | None. | Accept. |
| **`MC2_WATER_SHINE`** line from the `ALLOWLIST` in `scripts/check-env-registry.sh` | Replaced by a real `kAuxEnvVars` registry row — a strict upgrade from "excused" to "registered". | Flagged: **must land in the same commit as the registry row** or the intermediate commit is CI-red. | Accept, with the coupling honoured. |
| **`MC2_GV_LIGHTSOUT_CLEAR`** line from `scripts/registered-gate-passthrough-debt.txt` | Ratchet in the correct direction: paired with adding the var to the smoke allowlist, converting it from "permanently excused" to "guarded". Injection-proven both ways, with a byte-identical restore (sha256 `2cb62d8b…` before and after). | Flagged: the debt file carries **no other lane's hunks**, so if a sibling commits the harness alone the allowlist entry is scooped while the debt line stays — producing a loud, one-line-fix FAIL. | Accept; the failure is loud, not silent. |
| **The `&&` with `gos_TerrainLodChunk_IsShorelineMaskActive()`** at the suppression call site | Not load-bearing: `setShorelineSuppressedByTerrainMask` has one call site and the flag defaults false, so the `MC2_TERRAIN_LOD_CHUNK=0` path genuinely keeps the pass. | Flagged: that accessor now has **zero callers** while remaining defined (`gos_terrain_lod_chunk.cpp:3990`) and declared (`gos_terrain_lod_chunk.h:204`), and the preserved registry text still asserts it "is the ONLY input" to a setter it no longer feeds. | **Half-deletion, real but minor.** Retire the accessor in place or delete it with its declaration. Do not leave it orphaned. |
| **Two comment lines** in `shaders/static_prop.vert` cross-referencing `MC2_STATIC_PROP_IBL_NIGHT_SCALE` | — | Flagged as the **only** deletion the skeptics called genuinely harmful. | **Upheld as harmful — see §6.** No code was deleted, but removing the sole pointer to the night-scale mechanism *in the same hunk* that installed a day-only saturation claim is what makes the resulting error hard to catch. Restore it. |

**No enum-index damage anywhere.** `kFeatureTable` spans `RendererFeatureRegistry.h:308-806`
and is closed by a `static_assert` at `:808`; `kAuxEnvVars` begins at `:818` and the file
annotates it as not indexed by `RendererFeature`. Both the deleted row and the new
`MC2_TERRAIN_AMBIENT_FLOOR_NIGHT` row (`:892`) live in `kAuxEnvVars`. The one
`kFeatureTable` edit (row 58, the shoreline default) changed a `bool` and a doc string
in place; ordinals are untouched and `tests/unit/test_rendercore.cpp:335` still holds.

---

## 6. REFUTED BATCHES

### Batch 1 — static props / buildings: **REFUTED**

Not because a working gate was destroyed. **Nothing was deleted, and commit `158e8e9e6`
is provably 100% comment-only and behaviorally inert** — machine-verified independently
by both its author and its skeptic: 0 non-comment lines added, 0 removed, 0 block-comment
delimiters, across all four files. Blast radius on your pixels: **zero**.

It fell because **a batch whose entire deliverable was removing false claims from source
comments introduced a materially false claim and broke a contract checker doing it.**

**Finding 1 — it turned a green checker red, and it is the one checker whose entire
subject is its own job.** Re-verified for this report:

```
$ py -3 scripts/check-gate-default-agreement.py   →  EXIT 1
  [COMMENT_DISAGREEMENT] MC2_TERRAIN_DECAL
      GameOS/gameos/gos_static_prop_batcher.cpp:453 states default ON
      but the reader implements default OFF.
```

That line is `//      MC2_TERRAIN_DECAL is enabled, not a shipped default-on value.`,
**added by this commit** — `git show 158e8e9e6~1:…` shows the bare token did not exist in
that file before. The file is clean in `git status`, so no sibling lane can be blamed, and
it is none of the three declared known-reds. The batch listed three checkers it ran and
`check-gate-default-agreement.py` was not among them.

**Finding 2 — every new saturation claim is night-blind.** `IBL-NIGHT-SCALE-1` is live
and was live in the parent commit: `GameOS/gameos/gos_static_prop_batcher.cpp:1108`
defaults `s_iblNightFloor` to **0.25**, and `:1129` `iblShStrengthNightScaled()` — which
is what **both** static-prop upload sites push (`:7031`, `:8251`, `:9585`) — returns
`g_iblShStrength * (1 - (1 - 0.25) * nFactor)`. So the shipped **effective** probe
strength on a full-night mission is **0.1375** (from 0.55) or **0.15** (from 0.60), not
0.55/0.60. At those strengths the probe's maximum contribution anywhere on the sphere is
0.673 / 0.734 — **it cannot reach the [0,1] clamp at all**, and the clamped share is
**0.00%**.

The batch nonetheless committed, at **four** shader sites, the unconditional claim that
"the probe alone clamps 30.7% (0.55) / 33.5% (0.60) of the normal sphere". Worse, one of
those notes enumerates "strength 0.60 (mc2_01/10/24)" and then in the next sentence cites
measurements taken on **mc2_10 night**, where the effective strength is 0.15 and the
stated immovable set is **empty**. The bound is wrong on precisely the pose used as its
own evidence. And the same hunk **deleted the only in-file cross-reference to the night
scale**. That text is now already being cited as the reason other knobs "cannot work".

**Finding 3 — #4's `CONFIRMED_INCAPABLE` rests on a self-contradicting premise.** The
committed replacement reads: "…an away-facing normal lands around **0.13** at the
absolute worst… Because `litRgb` therefore essentially never fell below **0.20**, the v1
form `max(litRgb, 0.20)` returned `litRgb` every time." **0.13 < 0.20** — the sentence's
own worst case is below the floor it claims never bound. With the night scale applied the
true worst case is `0.232 × 0.55 × 0.25 = 0.032`, an order of magnitude under 0.20, on
exactly the night missions under complaint. **#4's practical conclusion still survives —
but via the argument the batch treated as secondary:** the flag bit's sole producer
(`mclib/bdactor.cpp:4961`) sits behind the default-OFF `MC2_TERRAIN_DECAL`, so **zero
fragments carry the bit on a default launch regardless of any lighting arithmetic.**
That is the version this report uses; the 0.13/0.20 sentence should be dropped.

**What survived Batch 1, per its own skeptic's independent re-derivation** (and therefore
usable as fact): the comment-only property; the SH arithmetic reproducing **exactly**
(irradiance 0.2324..4.896 R, saturated share 30.73%/33.53% at 0.55, +X/−X ratio 7.71:1);
#5's `CONFIRMED_INCAPABLE`; the #6 and #7 refutations, which the night finding makes
*stronger*, not weaker; and the unrotated-lobe finding. The skeptic also caught one
inaccuracy in the batch's own report: `mclib/bdactor.cpp:4826`'s
`getenv(x) != nullptr && getenv(x)[0] != '0'` is **not** the canonical opt-in idiom — it
returns true on an empty string.

**Required follow-ups, all one-liners:** (1) fix or allowlist
`gos_static_prop_batcher.cpp:453` so the checker returns 0; (2) qualify the four
"30.7%/33.5%" claims as **DAY-ONLY** and restore the `MC2_STATIC_PROP_IBL_NIGHT_SCALE`
cross-reference; (3) restate #4 on the flag-gating argument alone.

### Delivery, across all four batches — the other casualty

Three of the four batches **could not commit their work**, and said so rather than
sweeping. The shared worktree carries concurrent lanes' uncommitted hunks in
`GameOS/gameos/gos_terrain_lod_chunk.cpp`, `RenderCore/RendererFeatureRegistry.h`,
`docs/tier1_env_vars.md`, `scripts/run_smoke.py` and
`tools/class_light_band_harness/class_light_band_harness.cpp`, and
`git commit -- <path>` commits **working-tree** content. Committing would have scooped
another lane's work; in one case it would have swept an **ungraded default flip** into an
unrelated commit under someone else's message.

Consequences you should act on:

- The single user-visible change in the whole sweep (§3) is **uncommitted**, and its two
  halves live in **different** uncommitted files — the reader flip in
  `gos_terrain_lod_chunk.cpp`, the registry `defaultOn` in `RendererFeatureRegistry.h`.
  **They must land together** or `check-gate-default-agreement.py` goes DISAGREE.
- Batch 4's extracted hunks are saved at
  `C:/Users/Joe/AppData/Local/Temp/claude/A--Games-mc2-opengl-src/f4bbf866-7ac2-41e3-b73b-ee6e568b2228/scratchpad/BATCH4-PENDING/`
  — a **session-scoped** directory. That is the only backup.
- **This needs a serialised commit window**, with the shoreline default flip split out so
  it is attributable.

Two pre-existing reds, neither from this sweep: `MC2_CPU_PHONG_RETIRE` in
`check-registered-gate-passthrough.py` (from `a75339bb0`, spawned as a separate task), and
`check-shader-schema.sh` on `GpuMechInstance` fields `visualDamage01`/`visualFlags`.

---

## 7. THE PATTERN — and the practice that would have caught it

**Clamp saturation explains 2 of 21. The dominant mode, 6 of 21, is a gate that is
inert on exactly the missions it targets.** Every one of those six had a
**multiplicative predecessor** sitting between the reader and the fragment, at a value of
zero, on the target mission, at shipped defaults:

| Gate | The predecessor that was zero | Where |
|---|---|---|
| `MC2_TERRAIN_AMBIENT_FLOOR` | `dayGate` = 0.0000 on the 5 night maps | `terrain_daylight.h:130` |
| `MC2_TERRAIN_DECAL_FILL` | flag bit 3 never set (`MC2_TERRAIN_DECAL` off) | `mclib/bdactor.cpp:4961` |
| `MC2_CEMENT_LIT` (paved branch) | bit 22 never baked (`PavedOverlayClassMode()` = 0) | `gos_terrain_indirect.cpp` |
| `MC2_WATER_SHORE_MASK` | sidecar absent on 22 of 24 missions | data, not code |
| `MC2_WATER_SHINE` | uniform location = −1, silently | never declared in any shader |
| `MC2_CEMENT_EDGE_CURVE` | exponent = 1.0, the identity | its own default |

Note the variety: a day gate, a flag bit, a class mask, a data sidecar, a missing uniform
declaration, and an identity default. **The common structure is not lighting at all — it
is an unchecked multiplicative predecessor on the target mission.** A saturation review
would have caught none of these.

### The practice: an ARMING PROOF, on the named target mission, at shipped defaults

Every lighting-adjacent slice already declares the complaint it is fixing. Make it also
declare, **before review**:

1. **The target mission, by name.** Not "night maps" — `mc2_05`.
2. **The full multiplicative chain** from the reader to the fragment, with **each factor's
   numeric value on that mission at shipped defaults**: env reader, every gate/mask/flag
   bit, every day/night curve, every uniform, any required data sidecar, and the
   `glGetUniformLocation` result. Any factor that is **0** is a **STOP** — the slice does
   not merge until either the predecessor is armed or the target mission changes.
3. **A measured non-zero delta on that mission**, against a stated OFF-vs-OFF noise floor,
   at the *shipped* default of every other gate — not at whatever the author had exported.
4. **A flip criterion naming a mission where a delta can appear.** This repo already does
   this well in places (`RendererFeatureRegistry.h:794` is a model), and the failure mode
   it prevents is real: a recipe pointing at a saturated mission comes back empty and the
   gate gets re-filed as dead. `MC2_WATER_SHINE`'s current recipe is a live example of the
   inverse error — it points at the pose where the effect collapses.

Three mechanical checks would enforce most of it cheaply:

- **Shader-side arming**: fail the build when C++ uploads a uniform no shader in the
  package declares. `MC2_WATER_SHINE` uploaded to nothing for months and every checker
  stayed green.
- **Float-gate default coverage**: `check-gate-default-agreement.py` currently skips any
  reader containing `atof()` as "not a boolean gate". Injection-proven blind this session
  on a registry default that was deliberately falsified. Extend it to numeric readers.
- **Data-sidecar arming**: `check-beauty-sidecars.py` checks cooked→installed in one
  direction only, so *installed-with-no-cook* and install-to-install regression are both
  outside its contract. That gap concealed a real one: **mc2_01 has silently lost four
  beauty sidecars** (`shoreline_mask.png`, `overlay_v2.png`, `control_map.png`,
  `visual_height_mips.r32`) relative to the 0.4 build, and none of the four has a
  git-tracked cook behind it anywhere in history. **If mc2_01 water or ground looks
  different from 0.4, that is a data question, not a lighting one.**

And one process rule, from the failure mode that is not even in the audit's five-mode
table: **re-read a finding's premise at HEAD before acting on it.** Audit entry #8 was
invalidated by a commit that was already an ancestor of the audit's own synthesis HEAD.
Two lanes independently overturned four entries in a single day. §3 of
`.claude/LIGHTING-HEADROOM-AUDIT.md` is a list of **unverified claims**, not a work order.

---

*Sources: `.claude/LIGHTING-HEADROOM-AUDIT.md` §3 and §5; commits `158e8e9e6`,
`ef12fbb45`, `9411c1687`; the uncommitted working tree of `A:/Games/mc2-main` as of
2026-07-30. Four remediation batches and four independent refutations. No frame was
rendered in the production of this document.*
