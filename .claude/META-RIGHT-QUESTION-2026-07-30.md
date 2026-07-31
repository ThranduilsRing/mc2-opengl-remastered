# META-RIGHT-QUESTION-2 — is "dark props" the right question? (2026-07-30)

Lane META-RIGHT-QUESTION-2. Thinking + static-reading only: no launch, no build,
no code, no gate flips. Every claim below was re-verified at reader source in
`A:/Games/mc2-main` this session unless explicitly marked UNVERIFIED. On a
default launch the user still sees all three defects; this document changes
zero pixels. Its only value is redirecting the next lane's spend.

---

## 0. First: two of the four "refutations" in this lane's own brief are stale relays

Checked against the tree before relying on them — and the brief itself
demonstrates the relay problem it warns about:

| Brief's claim | Actual state in the tree |
|---|---|
| "Bake-order 3× ambient collapse — refuted (twice)" | TRUE, and now **refuted at source with an executable proof**: `PROP-BAKE-SUNWINDOW-1` (2026-07-30, `mclib/bdactor.cpp:3149-3177`) — the tree sun-off window is confined in extent (single set/restore, no early return, serial updates), and the "collapse" step is a no-op because `GatherLightsParameters` COMPACTS (`txmmgr.cpp` num_lights advances only in the `active` branch), proven by `tools/object_light_row_harness ::decompose_slot0_is_a_noop_after_gather`, which goes RED when compaction is removed from the model. |
| "Static recipe frozen-snapshot staleness — investigated, not established" | **STALE.** `PROP-BAKE-TIME-1` (2026-07-30, same comment block, `bdactor.cpp:3222-3330`) **confirms** the bake-TIME mechanism to the bit: the mutator is WEATHER (`code/weather.cpp:362` divides `eye->ambientRed` by `rainLightLevel`; verified at source this session). mc2_10 is a permanent storm; BRIGHT 0.7529 = 192/255 = the pre-weather boot ambient frozen at frame 1, DIM 0.2471 = 63/255 = trunc(192/3.048) one tick into the ramp, LIVE settles at 48/255. Executable proofs exist (`::ambient_state_changes_the_baked_row`, `::mc2_10_storm_ambient_is_not_mission_constant`, `::non_rainy_missions_are_immune_to_bake_time` — all present in `object_light_row_harness.cpp:840-1027`). **Crucial inversion: the frozen rows are BRIGHTER than the live scene (+15 to +144/255). Staleness cannot explain "props too dark" — it explains props too bright, and fixing it would DARKEN mc2_10 props.** The fix (generation counter on `mc2StaticLightEnvironmentGeneration`) is designed in-tree, deliberately unshipped, user call. |
| "Albedo theory — refuted" | **No receipt exists on either side.** See §4 — this is the refutation to re-open. |
| "LOD decimation as the mc2_17 mechanism — refuted" | **No artifact found in the tree.** `grep -il decimat .claude/*.md` hits only three terrain-recon docs; no mc2_17 dark-prop LOD doc exists. A refutation with no tree artifact is unauditable and — given the relay error rate — must be treated as *unresolved in either direction*, not as refuted. Whoever cites it next must produce the receipt. |

Also verified from the brief's fact list: `kAmbientFloor = 0.35` + clamp
(`mech.vert:204,222-224`); `static_prop.vert` has zero `TG_LIGHT_POINT/SPOT`
handling (grep: 0 hits; comment "Point/spot lights are omitted"); vehicles'
missing `CacheGpuLightData` fix + `MC2_LEGACY_LIT_MC2_FRAME` promotion
(`RendererFeatureRegistry.h:669-673`); `updateDaylight` unreachable in-mission
(`camera.cpp:2238` commented; `Camera::activate` gated on `ready && active`;
`GameCamera::activate` overrides without chaining, `gamecam.cpp:1060` commented
— full dispatch chain documented at `bdactor.cpp:3256-3269`). NOT verified this
session: vehicles-cast-no-shadows; turret ground-snap absence; terrain-LOD
horizontal-only. Those three are used below only where marked.

---

## 1. The reframed question

**"Props are too dark" is a statement about a RATIO reported as if it were a
statement about one class.** The eye compares a prop against the mech, terrain,
and pavement pixels beside it. Those references are not neutral: every one of
them is lit by a *different, port-era-invented* model. The five multiplier
bands, each from reader source:

| Surface | Lit multiplier model | Source | Band (floor..ceiling) |
|---|---|---|---|
| Terrain | `mix(0.35, 1.20, diffuse) * shadow` — mission light COLORS not in the multiplier at all | `terrain_lod_chunk.frag:1872,2005` | **[0.35 .. 1.20]**, hardcoded, mission-independent |
| Cement/apron atlas | flat-lit, exemption | `:1878-1891`, `u_cementLit=0` default | **{1.0}** exactly |
| Mech | `clamp(0.35 + A + S·ndl, 0, 1)` | `mech.vert:204,218-224` | **[0.35+A .. 1.0]** — saturates to a degenerate {1.0} on bright missions |
| Vehicle (post-`5c04048c`) | `A + S·ndl` — no floor, no clamp | `gos_tex_vertex_lighted` via `calc_light` | **[A .. A+S]** |
| Static prop | `seed(≈0) + A + S·ndl + 0.6·SH_IBL(n)` | `static_prop.vert:823,845-848` | **[A+ibl .. A+S+ibl]** — the ONLY class with the SH-IBL additive (default-ON, `RendererFeatureRegistry.h:456`) |

Where A = ambient/255, S = sun-color/255 for the row actually gathered.
Instantiated: mc2_03 day (authored A=0.122, S=0.675): mech shadow-face 0.47 vs
prop shadow-face ~0.12+ibl vs terrain floor 0.35 vs cement 1.0. mc2_10 live
storm (A settles 48/255≈0.19): mech ≈1.0 flat vs prop ≈0.19+S·ndl+ibl.
(The mech-vs-vehicle instance of this was already computed by
`VEHICLE-CLASS-TRUTH-1` §6; the five-surface matrix, the terrain/cement bands,
and the IBL asymmetry are assembled here for the first time.)

So the reframing, concretely:

> **The defect is between-class variance of five independently-invented
> multiplier bands, and the reference classes are the ones that moved.**
> Stock MC2 lit mechs, vehicles and props through ONE CPU kernel
> (`tgl.cpp`); the 0.35 mech floor, the mech clamp, the terrain [0.35,1.20]
> band, the cement exemption, and the prop-only IBL term are all port-era
> additions. Four lanes hunted for "the prop bug" inside the prop half of a
> ratio whose other half had been rebuilt. That is why four mechanisms
> refuted cleanly: each was a real mechanism, and none of them was the ratio.

Questions nobody has asked, each now crisply decidable:

1. **"Which class deviates from the stock kernel, and by how much?"** — never
   measured. The stock CPU kernel is still in the tree. See §2.
2. **"On which missions does the structural model PREDICT the complaint?"** —
   prop floor < terrain floor exactly when authored ambient < 0.35·255 ≈ 89.
   mc2_03/12/13/17/18 author ambient 31 (`VEHICLE-CLASS-TRUTH-1` §5). If the
   user reports dark props on a mission where the model predicts parity, the
   structural explanation is falsified. A falsifiable per-mission prediction
   table has never been produced.
3. **"In which frame is `ObjectLights.light_dir`, really?"** — three in-tree
   texts give three answers: `mech.vert:270-276` says Stuff-space (and swaps it
   for specular); `RendererFeatureRegistry.h:673` says the rows are MC2-frame
   and mech's diffuse feed is consistent; `static_prop.vert:75-79` found the
   row sun "~90deg off" and substituted `u_terrainSunMC2` instead of resolving
   it. Mech diffuse (no swap) and mech specular (swap) consume the same bytes —
   **at most one of them is right**, and on bright missions the diffuse error
   would be invisible under the saturated clamp. Each lane patched its own
   consumer locally; nobody asked the one question that settles all four
   consumers at once.

---

## 2. The discriminating measurement — one artifact, offline, no launch

`tools/class_light_band_harness`, imitating `object_light_row_harness`
verbatim (no engine, no GL, no Stuff; `glsl_port_diff.py` + `PORT-BEGIN`
markers keep the GLSL↔C++ mirrors honest — `calc_light` is already marked).

**Stage A — the band matrix.** Pure functions for the five models above (source
lines as contracts). Inputs: authored `.fit` values per mission (AmbientRGB,
DayLight*, pitch, Weather block — parsing idiom already exists in the
polarity/vehicle lanes' scratch parsers) plus mc2_10's weather ramp variants
{192, 63, 48}. Output: per-mission × per-class M(ndl) at ndl ∈ {0,¼,½,¾,1} +
worst pairwise band ratio + the §1.2 prediction column. ~25 assertions pin the
current state so any future lighting change diffs visibly.

**Stage B — the frame test.** Port the ~10 lines of
`camera.cpp:1857-1862` (EulerAngles → lightToWorld) and the
`GatherLightsParameters` row extraction; for authored pitches, dot the row's
`light_dir` against canonical normals under both frame conventions and against
`gos_GetTerrainLightDir`. One test decides which of the four sun consumers
(mech diffuse, mech specular, prop substitute, vehicle converted) is wrong.

**Stage C — the stock reference.** Port the legacy CPU per-vertex kernel
arithmetic (`tgl.cpp` ~1880-2225) as ground truth and assert each GPU object
class's deviation from it. (Terrain is excluded from the stock-parity claim —
stock terrain lighting was always a separate path; the port-era invention claim
is made only for the three object classes plus the cement exemption.)

**What each outcome means:**

- **prop == stock, mech > stock** → "dark props" is really "brightened
  references"; the fix is ONE policy knob (give props the mech floor, or
  retire/shrink the mech floor), an aesthetics decision for the user — the bug
  hunt is over.
- **prop < stock** → a genuine prop regression exists, with its magnitude and
  term attribution measured exactly; hunt continues with a number.
- **both == stock** → the complaint is stock look under modern clarity; pure
  tuning, user call.
- Stage B mismatch on mech diffuse → the reference class is lit by a
  wrong-axis sun currently masked by saturation, and every band comparison
  involving mech NdotL gets a correction before anyone trusts it.

This is one artifact that separates the hypotheses *simultaneously* instead of
eliminating them one at a time — the property the four sequential refutations
lacked.

---

## 3. The cheapest things never tried (ranked by cost)

1. **Masked albedo measurement — minutes, pure Python, settles §4.** Decode the
   actual prop and building textures from the FST/srcdata (extraction scripts
   exist: `scratch/treesreopen1/fst_extract.py`), compute per-texture mean over
   OPAQUE texels only, truck body vs building wall. No lane has ever produced a
   masked number; both sides of the albedo dispute rest on unmasked rect means
   vs an eyeball read.
2. **Stage A of the band harness — a day.** The prediction table alone
   (§1.2) is falsifiable against the user's existing reports without any
   launch.
3. **Add `Camera::ambient*/light*` + a per-slot `worldLights[]` snapshot to
   `MC2_DEBUG_STATE_DUMP`.** I read the live dump this session
   (`get_render_state`): it carries pass/resource state but **zero lighting
   scalars**. One struct extension makes every future "what was the ambient
   really" question answerable from the user's own runs, retroactively killing
   the entire class of frozen-vs-live disputes that consumed three lanes.
4. **When launches resume (user-present):** the already-built `[BAKEORD v1]`
   run (needs `MC2_DIAG_VERBOSE=1`) to settle the one remaining unknown of the
   weather story — the cohort partition (`bdactor.cpp:3305-3311` says
   explicitly: one gated run, no more reading); and TREES-REOPEN's D1
   (`MC2_LIGHTING_DEBUG_VIEW=lightcount/sun/ambient`) for the tree rows.

---

## 4. The refutation to re-open: ALBEDO — the only one with no receipt

Ranking the four by proof quality:

- **Bake-order:** REFUTED, gold standard — source reading + an executable
  harness test that was verified RED under the injected regression. Do not
  re-open.
- **Frozen-snapshot:** CONFIRMED-WITH-INVERSION (weather), with three
  executable tests and bit-exact derivations of both field constants. Closed —
  and its direction (frozen = brighter) actively *removes* it as a dark-prop
  cause. Do not re-open.
- **LOD-decimation/mc2_17:** no artifact; never actually refuted as far as the
  tree can show. Unresolved, not closed.
- **Albedo: WEAKEST — re-open, offline, this week.** The 3.3× figure
  (`PROP-LIGHT-CLASS-4`: truck 41.6 vs building 136.6 in debug-view 40) was
  measured over PNG *rectangles*; `DARK-PROP-LIGHTING-6` §3 identified the
  contaminant (a truck fills a fraction of its rect, surrounded by dark
  ground/shadow — that alone can manufacture a 3× gap) and explicitly marked
  it **not yet tested**. The "refutation" is the user's direct read of the
  source `.tga`. So: claim = unmasked rect means; refutation = eyeball. Item
  §3.1 settles it for the cost of a lunch break.

**And note the trap that made this dispute ill-posed:** albedo and the §1 band
gap COMPOSE — final pixel ≈ albedo × band(ndl) (+emissive +IBL −shadow). A
1.5× albedo gap times a 3× band gap is the user's picture. Arguing which one
"is" the cause was the wrong question; the right question is the split, which
the masked measurement (numerator) plus Stage A (denominator) delivers as two
independent numbers.

---

## 5. The other two defects, same treatment, briefly

### 5a. Black quads + sunk turrets, mc2_03

- The frozen-snapshot hypothesis is now structurally DEAD for mc2_03 color
  variation: on the 45 non-rainy missions the ambient is constant from init to
  mission end, so *no bake moment can produce a divergent row there*
  (`bdactor.cpp:3272-3283`, weather is the only default-launch mutator and
  mc2_10 the only mission that can rain). Stop charging this hypothesis.
- **Black** has a measured standing candidate that is not a color-variation
  mechanism at all: the all-zero light row — `zeroLightSlots = 35` on a
  default launch (PROP-RECIPE-BAKE §0, hidden by this machine's stale
  `MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE=1` shell env — env-hygiene trap for
  every future capture), and the source's own "black actor" comment
  (`bdactor.cpp:4655`). The right question is not "why are these quads black"
  but **"why does a zero row ever reach the screen"** — an invariant, and the
  fail-loud pattern (magenta placeholder class, already shipped elsewhere in
  this port) is the fix *shape*. Cheapest: read `[STATIC_LIGHT_ZERO v1]` at an
  mc2_03 pose on the next permitted run — zero new code.
- **Sunk turrets:** the ground-snap-absence claim is UNVERIFIED here (no tree
  receipt found). But there is an offline decisive check nobody has run:
  **parse `mc2_03.pak` packet 1 turret records and compare placed z against
  terrain elevation at (x,y) from packet 0** — both parsers already exist
  (`scratch/treesreopen1/objlist.py`, the packet-0 PostcompVertex read). That
  answers "sunk or not, by how much, for all 11 turrets" without a launch.
- The two symptoms may be ONE defect: `BUILDING-FACE-POSE-1` sighted "housing
  renders as ground shadows with **no building geometry above them**" on this
  map, and the caster replay is visibility-independent and default-ON
  (`MC2_SHADOW_DYNAMIC_PROP_CASTERS`, TREES-REOPEN rank 3 receipts). If
  geometry is sunk/missing while its shadow still draws, the screen shows dark
  quads. Discriminator D5: one A/B with the caster kill-switch — if the black
  quads vanish, this was never a lighting question; it is an **elevation**
  question, and the offline z-vs-terrain parse above becomes the whole
  investigation.

### 5b. Runway colour

**"Make the runway lighter" has already been answered — by the user, twice, in
the negative** ("after is WAY worse", both albedo swaps). Treating the third
attempt as a tone problem repeats a refuted experiment. What the measurements
actually established (`RUNWAY-APRON-TONE-2`: cement census `dark_cells=0` on
both airbase maps; the runway is baked-colormap terrain under full terrain
lighting next to a lighting-EXEMPT cement atlas): this is the §1 defect again —
**two surfaces the player reads as one object, on two different lighting
contracts**. Flat lighting provably cannot close the residual because the
residual is the *albedo* term of a product whose *lighting* term is what
diverges with time of day.

The right question: **"which surfaces must share one lighting contract?"** —
and there are two unification directions, both already landed as gates, that
have never been compared: pull pavement UP into cement's exemption
(`MC2_PAVED_OVERLAY_CLASS=1`, captured: uniformity better on 13/15 poses,
mean-tone mixed-sign) vs pull cement DOWN into terrain lighting
(`MC2_CEMENT_LIT=1`, landed `6b94443e`, **never captured** — blocked by a dirty
tree, not by a decision). The cheapest decisive act on this defect is two
captures and a side-by-side of those two directions at `saved_204937` + one
night pose, then a user click-through. If the user still wants full
convergence after unification, the remainder is an asset reconciliation
(colormap vs atlas art) — a content decision only the user can make, and no
shader lane should accept it as a bug again.

---

## 6. Adversarial self-review

**What would the four prior agents say?**
(a) *"The band table is VEHICLE-CLASS-TRUTH §6 generalized."* For mech-vs-
vehicle, yes — credit given. New here: the terrain [0.35,1.20] and cement {1.0}
bands in the same matrix, the prop-only IBL additive (default-ON — which means
§6's own prop/vehicle numbers UNDERSTATE prop brightness; my correction cuts
against my own headline and is included anyway), the per-mission falsifiable
prediction table, the stock-CPU ground truth, and the three-way frame
contradiction. (b) *"Your harness measures models, not pixels."* Correct, and
it is the point: it attributes DIRECTION and SIZE per term; it cannot rule on
beauty. Band parity would not prove perceived parity — texture, shadow,
emissive tags, fog are outside it. (c) The PROP-BAKE-TIME author would say the
ambient axis is closed and the bands "were always like this" — no: the floor,
clamp, exemption and IBL are port-era by commit history, and Stage C makes
"always like this" a testable claim instead of a shrug.

**Would the user still see this on a default launch?** Yes — all three
defects, unchanged. This lane produced a document the user will never see on
screen. It is worth having only if the next lane runs §3.1 + §2 Stage A
instead of hunting a fifth mechanism, and if §5b stops the third tone
experiment before it is built.

**Most likely ambiguous result, and the sharpening:** the masked albedo
measurement — prop textures mix body/glass/team-color regions and a single
mean can straddle. Sharpen: per-texture opaque-texel HISTOGRAMS, per-node
breakdown, compare like regions (truck body vs building wall), exclude
hot-color-tagged verts' texels. Second risk: Stage B's lightToWorld port could
diverge from runtime if the matrix is rebuilt elsewhere mid-mission; mitigate
by porting the exact `setSunPitch` path and testing several authored pitches,
and cross-checking against the `u_terrainSunMC2` upload value.

**My own relay exposure:** three brief facts used but not re-verified are
marked UNVERIFIED where used (vehicle shadows, turret ground-snap, terrain LOD
metric); the mc2_17 LOD refutation could not be located and is treated as
unresolved rather than trusted in either direction.

**Process finding (the meta-defect under all three defects):** refutations
without tree artifacts and mechanisms promoted to fact before proof are how
this defect consumed nine-plus lanes: the bake-order mechanism was copied
forward "as fact" into three files before being refuted at source
(`bdactor.cpp:3150-3151`), the polarity lane's wrong `updateDaylight` mechanism
propagated until `DAYLIGHT-GATE-RECHECK-1` corrected it in `camera.h:486`, and
this lane's own brief carried two stale relays (§0). The corrective already
exists in this tree and worked twice today: **a refutation is citable only
with a tree artifact, and where possible an executable check that goes RED
when the refuted mechanism is re-injected** — `object_light_row_harness` is
the template. §2 extends the same template from refutations to the standing
question itself.
