# LIGHTING HEADROOM AUDIT

**Date:** 2026-07-30
**Worktree:** `A:/Games/mc2-main` (branch `claude/nifty-mendeleev`)
**HEAD at synthesis:** `a75339bb0`
**Method:** offline arithmetic over shader source, C++ readers, mission `.fit` data,
`.ase` vertex colours and shipped `.burnin` textures. **No game launch.**
Six surface classes were audited independently, then every finding was attacked by an
independent skeptic. This report carries the **post-refutation** numbers only.

> **Line-number caveat.** Citations marked ✔ were re-verified by this synthesis at
> `a75339bb0`. Unmarked citations are inherited from the audit lanes and may drift by
> tens of lines (`mclib/camera.cpp` in particular shifted ~78 lines under
> SUN-ELEVATION-FIX-1). Constants and identifiers were checked; line offsets were not,
> except where marked.

---

## 1. THE TABLE

Headroom = the multiplicative ceiling a **lighting-only** change can reach on that
surface. `1.00x` means arithmetically immovable. Corrected values are the skeptic's
where the original finding was overturned.

| # | Surface class | Corrected headroom | Pinned? | Verdict | Survived refutation? |
|---|---|---|---|---|---|
| 1 | **Static prop — building**<br>`static_prop.{vert,frag}` | **1.005x – 2.412x**<br>(mc2_22 1.005x · mc2_09 1.049x · mc2_04 1.051x · mc2_03 1.470x · mc2_05 2.412x · mc2_10 **~1.86x**) | **Partly.** Clamp-limited on the bright day maps; free on the dark ones | MIXED | **PARTIAL** — 23-mission table survived within 0.04x; the entire mc2_10 arithmetic fell |
| 2 | **Static prop — tree**<br>(structurally different: sun deleted) | **1.000x on 9 of 24 missions**<br>(mc2_01/04/07/22/24 exactly 1.000; mc2_06/08/09/16/20 ≤1.004x)<br>up to 1.84x on mc2_05 | **YES — the one genuinely pinned class** | **PINNED** | **SURVIVED** (2 numeric corrections: mc2_09 → 1.001x, mc2_10 → 1.74x) |
| 3 | **Vehicle**<br>`gos_tex_vertex_lighted.{vert,frag}` | **≥ 1.059x on every mission**, ~1.089x on roofs<br>(vertex stage alone: 1.000x on mc2_10/22) | **NO** — vertex stage pins, fragment stage does not | NOT PINNED | **REFUTED** — "≤1.000x on EVERY mission" is false |
| 4 | **Terrain**<br>`terrain_lod_chunk.frag` | **2.16x – 20.83x to clip**<br>(mc2_01 2.16x · mc2_03 3.81x · mc2_10 9.07x · mc2_21 20.83x) | **NO ceiling pin. FLOOR-pinned** on mc2_05/10/19/21 (14–100% of fragments) | NOT PINNED | **PARTIAL** — anatomy survived; both headroom figures corrected down (up to 1.82x) |
| 5 | **Cement / pad / apron**<br>(`pureConcrete == 1` branch) | **1.016x – 6.071x**<br>(mc2_05 6.071x · mc2_23 2.477x · mc2_10 1.328x; only mc2_04/22 truly at 1.000x) | **NO** | NOT PINNED | **REFUTED** — "pinned at exactly 1.000 on all 25 missions, zero headroom" is false |
| 6 | **Water plane**<br>`gos_terrain_water_mdi.frag` | **1.000x – 4.037x**<br>(mc2_22 1.000x pinned · mc2_04 1.004x · mc2_10 1.293x · mc2_05 4.037x, pinned at the **floor**) | Ceiling-pinned on 1 of 22; floor-pinned on 1 | MIXED | **SURVIVED** — reproduced to 4 dp |
| 7 | **Shoreline — legacy screen pass**<br>`shaders/shoreline.frag` | **1.000x, all missions** | **YES** — but by a dead classifier, not a clamp | PINNED (dead) | **SURVIVED** — independently re-confirmed |
| 8 | **Shoreline — V3 foam lobe**<br>`terrain_lod_chunk.frag:2465-2469` ✔ | **1.24x – 6.53x** (typical beach 1.24x; dark beach 6.53x) | **NO** | NOT PINNED | **REFUTED** — "1.000x on all three mechanisms" is false |
| 9 | **Runway / road — unclassified**<br>(`pureConcrete == 0`) | **2.725x** on mc2_05/21 (unconditional floor pin)<br>**≤2.725x** on mc2_10/19 (not actually pinned) | Floor-pinned on **2** of 25, not 4 | NOT PINNED | **PARTIAL** — pin count halved; the mc2_10 conclusion is unresolved |

---

## 2. THE HEADLINE

**No. The "everything is pinned at the clamp ceiling" thesis does not survive.**

Both of the audit's most dramatic pinning claims were attacked and **both fell**:

- *"Cement road/runway/apron is pinned at exactly 1.000 on all 25 missions; headroom
  zero; every registered lighting knob on it is strictly darkening."* — **False.** The
  finding read `u_missionFlatLight` in its own formula and filed it under *albedo*. It
  is the sun term. `code/gamecam.cpp:451-465` ✔ computes it as
  `getLight{R,G,B}(clamp(lightDirection.z,0,1))/255` and the comment on that very line
  calls it *"the legacy per-vertex light value flat cement pads received … with
  flatIntensity = the flat-up N·L = lightDirection.z."* Its luma is below 1.0 on **22 of
  24 shipping missions** (0.1647 on mc2_05 to 1.0000 on mc2_04/22), and a **registered,
  default-ON killswitch** already realises the headroom:
  `MC2_CEMENT_MISSION_LIGHT=0` at `GameOS/gameos/gameos_graphics.cpp:10449-10454` ✔
  forces `(1,1,1)`. Real headroom **1.016x – 6.071x**, and the lever is strictly
  *brightening*.

- *"Vehicle headroom relative to the state the user complained about is ≤1.000x on EVERY
  mission."* — **False.** The finding audited `mech.vert`, found the clamp, and never
  opened `mech.frag`. The mech carries **two default-ON additive terms after that
  clamp** that the vehicle shader does not: a hemisphere ambient fill at
  `shaders/mech.frag:233` ✔ (`c.rgb += tex_color.rgb * hemi * u_mechAmbientV1Strength`,
  strength 0.15, `gos_mech_batcher.cpp:294-304` ✔ default-ON) and a Blinn glint at
  `mech.frag:284` ✔ (`gos_mech_batcher.cpp:310-312` ✔ default-ON — **despite its own
  comment at :306 saying "DEFAULT-OFF"**). I read all 85 lines of
  `gos_tex_vertex_lighted.frag` ✔: the vehicle has only `calcLightPools` and
  `apply_fog`. So ≥1.059x exists on every mission including the two "fully pinned"
  ones, reachable by copying `mech.frag:229-233` verbatim.

**What IS true, and it is narrower and more specific:**

1. **Trees are genuinely, structurally pinned** — the one class where the thesis holds.
   `TreeAppearance::update` deactivates the sun (`mclib/bdactor.cpp:7317-7318` ✔
   `light->active = false`) and recolours the *ambient* slot instead
   (`bdactor.cpp:7304` ✔), so a tree's GPU light row contains **no directional light at
   all**. The multiplier is orientation-free, and the stacked SH-IBL probe pushes it to
   a flat `1.000` on **9 of 24 missions**. On those maps the rendered tree is the
   texture, to the LSB, and no lighting change of any kind can alter one pixel.

2. **The project has repeatedly shipped lighting fixes that could not move anything** —
   this half of the thesis is fully supported. Fifteen such fixes survived refutation
   (section 3). But **clamp saturation is only one of five failure modes, and it is not
   the most common one.** The observed modes, by count:

   | Failure mode | Count | Example |
   |---|---|---|
   | Gate is arithmetically dead at its shipped default, or zeroed on exactly the missions it targets | 6 | `MC2_TERRAIN_AMBIENT_FLOOR` dayGate == 0.0000 on the 5 darkest maps |
   | Uniform / flag never reaches the shader at all | 3 | `MC2_WATER_SHINE` uploads to a uniform that has **never existed** |
   | `max()` or `mix()` placed under a band that never reaches it | 3 | tree floor `max(litRgb, 0.5)` — dead on 21 of 24 |
   | Clamp ceiling eats the change (the actual thesis) | 2 | `V-AMBIENT-STATIC-1` lands pre-clamp; +0.4% on mc2_22 |
   | Premise false — the thing being fixed does not exist in the data | 1 | `MC2_RUNWAY_TONE_MATCH`: dark_cells = 0 on stock atlases |

   > **CORRECTION 2026-07-31 (LIGHTING-INCAPABLE-BATCH-2).** "Fifteen" and the
   > *"Uniform / flag never reaches the shader at all"* row are **both overcounted by
   > two.** Entries **8 (`MC2_LEGACY_LIT_MC2_FRAME`) and 9 (`MC2_GV_LIGHTSOUT_CLEAR`)
   > are retracted** — both flags do reach live shading at HEAD. That row's surviving
   > member is `MC2_WATER_SHINE` alone (count **1**, not 3), and the survivor total is
   > **13**, not 15. The audit's own thesis is unharmed — if anything it sharpens, since
   > the retracted pair failed by a **sixth** mode the table does not list:
   > **the finding was stale, not the fix.** Entry 8's premise was killed by a commit
   > that is an ancestor of this audit's own synthesis HEAD.

**The most alarming single finding is not a ceiling.** It is
`MC2_TERRAIN_AMBIENT_FLOOR` / TERRAIN-AMBIENT-FLOOR-2 (`96ffc35d`, default-ON at 0.35):
its day gate is **exactly 0.0000** on mc2_05, mc2_10, mc2_19, mc2_21 and mc2_23
(`GameOS/gameos/utils/terrain_daylight.h`, kNightFullDeg 10 / kNightStartDeg 25).
Those five are precisely the missions with the **lowest** terrain multipliers
(0.3670–0.4814) and the **highest** floor-pin fractions (14.3–100%). The shipped
default-ON cure for "terrain is too dark" is switched off on exactly the five maps where
terrain is darkest. Two independent lanes reproduced this table to four decimals.

**Bottom line:** there is substantial lighting headroom almost everywhere. The wasted
work was real but was overwhelmingly caused by **gates that are inert where they are
needed**, not by a saturated renderer.

---

## 3. THE INCAPABLE-FIX LIST

Only entries that **survived independent refutation**. Each: what it tried, why it could
not have worked.

> ### ⚠ POST-AUDIT VERIFICATION IS IN PROGRESS — DO NOT DELETE A GATE ON THIS LIST WITHOUT RE-VERIFYING IT FIRST
>
> "Survived one refutation" is not "proven". A per-batch re-verification sweep against
> HEAD started 2026-07-31. **Entries 8 and 9 have already been RETRACTED** — see the
> inline blocks below. Entry 8 in particular names a gate that is **default-ON and
> correctness-critical**, and deleting it on this list's authority would ship
> confidently-wrong vehicle lighting.
>
> **The systematic defect this list has, and that a reader must correct for:** the
> synthesis pinned its citations at `a75339bb0` but several findings were *derived* on
> older trees, and at least one — entry 8 — was **invalidated by a commit that is an
> ancestor of the synthesis HEAD itself** (`5c04048c9`, landed 14 h 21 m earlier).
> Past-tense evidence ("the row **was** all-zero") got filed under a present-tense
> "cannot work" heading. **Before acting on any entry here, re-read its readers at
> HEAD and check `git log` for a later slice that moved the premise.**

### Trees / static props

1. **`ef1c9fcb` — tree lighting floor `max(litRgb, vec3(0.5))`**
   (`shaders/static_prop.frag:413` ✔).
   *Tried:* stop tree cards reading black on "the light-facing falloff side".
   *Could not work:* the computed tree multiplier never falls below 0.5 on **21 of 24
   missions** (binds only on mc2_05 73.1%, mc2_23 50.7%, mc2_10 65.1%). On mc2_01 the
   tree **minimum** is 1.026 — twice the floor. Its premise was wrong in kind: trees have
   no falloff side, because `bdactor.cpp:7317-7318` ✔ removes the directional light.
   *(Corrected from "22 of 24" — the skeptic found mc2_10's ambient is weather-divided,
   so the floor does bind there.)*

2. **`33a2b524` — PROP-SHADOW-RECEIVE-1 / `MC2_PROP_SHADOW_RECEIVE`.**
   *Tried:* let props receive shadows.
   *Could not work on trees:* an **exact no-op on every tree, every mission, every
   normal, every time of day.** It modulates only `sunColor * ndl`, and a tree's row has
   no directional light, so `sunColor` is identically `vec3(0)`
   (`bdactor.cpp:7317-7318` ✔ + the `->active` filter at `mclib/txmmgr.cpp:2877`).
   There is no sun term for a shadow to attenuate.

3. **`589503cd` — V-MATERIAL-PBR-3 / `MC2_STATIC_PROP_PBR_V1`.**
   *Tried:* per-fragment specular on static props.
   *Could not work on trees:* the whole block sits behind `if (v_pbrV1SunFound != 0)`
   (`static_prop.frag:483`), and that flag is set only when the vertex scan finds a
   `TG_LIGHT_INFINITE` entry. A tree's row never has one. An entire visual feature that
   is **identically zero on the darkest class in the game** (tree median albedo
   L = 58.0).

4. **`3e32d41a` — TERRAIN-DECAL-FILL-1, `max(litRgb, fill)`.**
   *Tried:* lift the shadow side of cliff decals.
   *Could not work:* the shipped code records its own verdict at
   `static_prop.frag:415-421` — *"the old `max(litRgb, fill)` was a visual no-op — the
   baked per-vertex light … never drops below the slider's operative range on visible
   faces, so `max()` returned `litRgb` every time."* Identical failure shape to #1.
   `7de56d52` (v2) had to move it after the texture multiply to escape.

5. **`99779f70` — "bake IBL SH default strength = 0.5" (V-IBL-STATIC-1-TUNE).**
   *Tried:* enshrine a user-eyeball tuning pass at 0.5.
   *Could not work:* overwritten on **every mission load** by `data/visual_tuning.json`
   (`staticPropIblStrength` 0.55 default; 0.60 on mc2_01/10/24), applied with no gate via
   `visual_tuning_profile.cpp:294-334`, called unconditionally from
   `code/mission.cpp:3874`, and the file ships (`scripts/deploy_payload.py:118`). The C++
   default and its comment are unreachable on any real deploy. **Any tuning decision
   recorded against 0.5 was measured on a value the game does not use** — and
   `tools/class_light_band_harness` still hardcodes `kIblStrengthDefault = 0.5`.

6. **`c2999bd7` + `1a727339` — `MC2_PROP_BASE_LIGHT_DIR` (PROP-BASE-LIGHT-DIR-1).**
   *Tried:* darken building faces the sun is behind — the user's literal complaint.
   *Could not work where it matters:* it scales `(seed + ambientTerm)` and deliberately
   does **not** scale the SH-IBL term (`static_prop.vert:783-785`, "NO DOUBLE-COUNT").
   Any shadow-side face where IBL alone already reaches 1.0 is immovable at **any**
   strength up to 1.0. Corrected immovable share of the `ndl≤0` hemisphere at strength
   1.0: **mc2_14 52.7%, mc2_11 52.5%, mc2_24 51.5%, mc2_22 49.5%, mc2_04 49.5%,
   mc2_07 44.4%, mc2_01 44.1%** — five missions above half, not seven.
   *(Skeptic's numbers; the original table ran 3–8 pp high.)*

7. **`19e85517` — V-AMBIENT-STATIC-1 / `MC2_STATIC_PROP_AMBIENT_V1`.**
   *Tried:* additive hemisphere fill for flat-reading props.
   *Could not work where needed:* it lands **before** the clamp
   (`static_prop.vert:838` vs the clamp at `:912` ✔), so the ceiling eats it on precisely
   the missions that read flat. Sphere-mean gain at **full** strength: +0.5% mc2_22,
   +2.7% mc2_04, +3.3% mc2_09 — versus +36.2% mc2_05 and +21.3% mc2_23. **8–70x more
   effective on the missions nobody complained about.** *(Skeptic reproduced within
   0.5 pp.)* This is a true clamp-ceiling casualty.

### Vehicles

8. **`b01a7a96` — VEHICLE-NIGHT-LIT-1 / `MC2_LEGACY_LIT_MC2_FRAME`.**
   > ## ⛔ RETRACTED 2026-07-31 (LIGHTING-INCAPABLE-BATCH-2). **THIS ENTRY IS FALSE AT HEAD, AND ACTING ON IT WOULD BREAK THE GAME.**
   > This gate is **DEFAULT-ON** and is a **correctness requirement** of the
   > `gvactor.cpp` gather. **Do not delete it. Do not demote it. Do not "clean it up
   > as inert."** The source says so in three places
   > (`mclib/txmmgr.cpp:2381-2384`, `RendererFeatureRegistry.h:699`,
   > `docs/tier1_env_vars.md:410`) and this entry contradicts all three.
   >
   > **Why the original text was true and then stopped being true.** The whole
   > refutation rests on one clause — *"the vehicle row **was** all-zero"* — written
   > in the past tense and then filed under a present-tense "cannot work" list.
   > `VEHICLE-LIGHTING-PATH-1` (`5c04048c9`, **2026-07-30 08:34:43 -0500**) populates
   > that row. This audit was synthesised at `a75339bb0`, **2026-07-30 22:57:51 -0500**
   > — **14 h 21 m later, with the fix already an ancestor of its own HEAD**
   > (`git merge-base --is-ancestor 5c04048c9 a75339bb0` → true).
   >
   > **The chain that is live at HEAD, every link read from the C++/GLSL that runs:**
   > 1. `mclib/gvactor.cpp:125` — `s_vehicleLitPath = gvEnvFlagDefaultOn("MC2_VEHICLE_LIT_PATH")`, **default ON** (`:110-116`).
   > 2. `mclib/gvactor.cpp:2316-2317` — `if (s_vehicleLitPath) gvShape->CacheGpuLightDataAllLeaves();`
   > 3. `mclib/msl.cpp:2062-2064` — guard `if (!g_useGpuObjects && !g_useGpuMechs) return;` **passes**: `g_useGpuObjects = true` (`gos_static_prop_batcher.cpp:59`), `g_useGpuMechs = envFlagDefaultOn("MC2_GPU_MECHS")` (`gos_mech_batcher.cpp:134`, default ON).
   > 4. `mclib/msl.cpp:2075` → `TG_Shape::GatherGpuObjectLightDataOnly()` (`tgl.cpp:3123-3127`) → `addLightDataStructureWithPerActorColor(&lightData_)`.
   > 5. `mclib/txmmgr.cpp:2062+` — **that function calls `GatherLightsParameters` itself** (`:2081` on the no-per-actor-light path; `:2088` into the template, then `*light_data = it->second.data` at `:2096`). **`lightData_` is filled.** The original entry checked only the two `GatherLightsParameters` call sites inside `tgl.cpp` (`:1898`, `:2869`, both genuinely behind `bShadersDrawPathEnabled == false`) and missed this third one, which is the one the live path uses.
   > 6. ⇒ `numLights.x != 0`, so the `lighting.hglsl:277-282` early-return **does not fire** and `normal` / `vertex_world_pos` / `light_dir` **are** read.
   > 7. `mclib/txmmgr.cpp:2404-2411` — reader is **default-ON**; `u_legacyLitMc2Frame = 1` selects the MC2-frame feed at `shaders/gos_tex_vertex_lighted.vert:197-207`.
   >
   > **Verdict: NOT INCAPABLE. Headroom is not 1.000x; the frame is load-bearing.**
   > `gate_status` → `verdict OK`, default ON, registered, documented, smoke-allowlisted.
   > `da3a5b78`'s self-disproof was correct **for the tree it was measured on** and is
   > now obsolete: the gate was *unreachable*, never *wrong*.

   *Original text, retained for the record:*
   *Tried:* feed `calc_light` MC2-frame normals/positions.
   *Could not work:* `calc_light`'s `numLights == 0` early-return
   (`shaders/include/lighting.hglsl:277-282`) fires **before** `normal`,
   `vertex_world_pos` or `light_dir` are read, and the vehicle row was all-zero
   (`tgl.h:339` zero-init; the only other gathers sit behind
   `bShadersDrawPathEnabled == false`, `mclib/tgl.cpp:221`). Correcting the *frame* of
   inputs that are never read cannot change one byte. Headroom exactly 1.000x.
   Self-documented as disproven at `da3a5b78`.

9. **`7c059585` — VEHICLE-NIGHT-LIT-2 / `MC2_GV_LIGHTSOUT_CLEAR`.**
   > ## ⛔ RETRACTED 2026-07-31 (LIGHTING-INCAPABLE-BATCH-2). **Both stated reasons are wrong. The latch has three live consumers.**
   >
   > **(a) is true but not load-bearing.** `shaders/gos_tex_vertex_lighted.vert:175-176`
   > does pass the literal `false` for `get_base_light`'s `lightsOut`. That proves only
   > that the *shader* does not re-apply the latch — because **the CPU already baked it
   > into the `aRGBLight` vertex attribute the shader consumes.**
   >
   > **(b) is false, and it is backwards.** `drawOldWay` is `false`
   > (`mclib/tgl.cpp:193`), so `if (drawOldWay)` at `:3289` is the **dead** arm. The
   > **`else` at `:3385-3426` is the LIVE arm**, and it is the one that calls
   > `mcTextureManager->addVertices(...)`. `gVertex[i].argb = tri.aRGBLight[i]` is set
   > at `:3268-3271`, **above** the `if/else`, so it reaches both.
   >
   > **The primary consumer the entry missed entirely:** `TG_Shape::MultiTransformShape`
   > (`tgl.cpp:1868`) reads `lightsOut` at **`:1926`** (forces `isNight = false`,
   > `nightFactor = 0`) and at **`:2129`** (`if (!lightsOut)` suppresses the per-vertex
   > `startVLight` colour), then writes `listOfVertices[j].argb` at **`:2481`** and
   > `listOfTriangles[j].aRGBLight[i]` at **`:2776`**. That kernel is **live in this
   > build**: `useVertexLighting = true` (`mclib/terrain.cpp:353`) and
   > `Environment.Renderer = 0`, not 3 (`code/mechcmd2.cpp:2974`) — which is exactly
   > what this repo's own `RendererFeatureRegistry.h:774` (LIGHT-ROW-PURE-1) already
   > certifies in writing. Vehicles reach it via `gvactor.cpp:2690`
   > `gvShape->TransformMultiShape(...)` → `msl.cpp:1930`.
   >
   > **Third consumer:** `tgl.cpp:3227` in `TG_Shape::Render` — `lightsOut` forces
   > `isNight = false`, which makes the `isSpotlight` early-return at `:3229-3231` fire
   > and suppresses a vehicle's searchlight-cone geometry.
   >
   > **Verdict: NOT INCAPABLE.** The shipped `docs/tier1_env_vars.md:411` description
   > — *"latent-correctness fix; **not** the cause of the reported all-vehicles-dark-at-night
   > defect"* — was and remains accurate. "Latent" ≠ "impossible", and this entry
   > promoted the former into the latter.
   >
   > **Why it had never been measured, now fixed:** `MC2_GV_LIGHTSOUT_CLEAR` was
   > **absent from the smoke passthrough allowlist** (`gate_status` → `ENV_DROP_RISK`;
   > parked in `scripts/registered-gate-passthrough-debt.txt`), so every A/B that set
   > `=1` had it dropped at `Popen` and graded OFF against OFF. Closed by this lane.

   *Original text, retained for the record:*
   *Tried:* clear a stuck lights-out latch on ground vehicles.
   *Could not work,* for two independent reasons: (a)
   `shaders/gos_tex_vertex_lighted.vert:176-177` passes the **literal `false`** for
   `get_base_light`'s `lightsOut` parameter — the shader cannot read the latch at all;
   (b) the only other consumer feeds the legacy `addVertices` queue gated by
   `drawOldWay == false` (`mclib/tgl.cpp:193`, guard `:3289`).

10. **`MC2_SUN_AXIS_FIX` (`166a6e456`, default-OFF) — sold as a brightness improvement.**
    *Could not brighten:* **exactly zero delta on mc2_10 and mc2_22** (both pinned at the
    vertex stage), and elsewhere it *permutes* which faces are lit rather than lifting
    them — worst visible face unchanged on 20 of 24 missions, roof deltas two-sided
    (+2.03x mc2_03, but 0.731x mc2_12). A correctness fix, not a brightness fix. The
    commit's own Stage D reaches the same conclusion.
    > **CONFIRMED, with one scope correction (LIGHTING-INCAPABLE-BATCH-2, 2026-07-31).**
    > The arithmetic stands and the gate is healthy: readers `mclib/txmmgr.cpp:2438-2440`
    > and `GameOS/gameos/gos_mech_batcher.cpp:2465-2468`, both in the canonical
    > DEFAULT-OFF opt-in idiom `(v && v[0] && v[0] != '0')`; `gate_status` → `verdict OK`,
    > registered, documented, smoke-allowlisted.
    >
    > **But "sold as a brightness improvement" is no longer true of HEAD, and the entry
    > should not be read as an open action item.** `RendererFeatureRegistry.h:794` and
    > `docs/tier1_env_vars.md:1395` already state this entry's own conclusion verbatim —
    > *"it is NOT MONOTONE, so it cannot be justified as 'the user wanted brighter
    > mechs'"* — already name **mc2_10 and mc2_22 as the zero-delta saturated pair**, and
    > already give a flip criterion pointing at **unsaturated** missions (mc2_03, mc2_13,
    > mc2_19 at 97.0% of the normal sphere). That is the anti-saturation recipe
    > discipline, already applied. **Nothing to delete, nothing to repair.**
    >
    > **Scope correction — the blast radius widened after this row was written.** The
    > vehicle half of `MC2_SUN_AXIS_FIX` is applied *only* on the
    > `u_legacyLitMc2Frame == 1` branch (`gos_tex_vertex_lighted.vert:197-207`). When
    > SUN-AXIS-FIX-1 shipped, `MC2_LEGACY_LIT_MC2_FRAME` was default-**OFF**, so
    > `MC2_SUN_AXIS_FIX=1` moved **mechs only**. `VEHICLE-LIGHTING-PATH-1` (`5c04048c9`)
    > flipped it default-**ON**, so `=1` now moves **mechs *and* ground vehicles**. Any
    > A/B graded against the original per-mission table is measuring a wider change than
    > that table covers. Re-measure the vehicle class before flipping the default.

### Terrain and cement

11. **`96ffc35d` / `ea7ef347` — TERRAIN-AMBIENT-FLOOR-2 / `MC2_TERRAIN_AMBIENT_FLOOR`
    (DEFAULT-ON at 0.35). ★ THE HEADLINE ENTRY.**
    *Tried:* raise dark terrain.
    *Could not work where needed:* its day gate is **exactly 0.0000** on mc2_05, mc2_10,
    mc2_19, mc2_21, mc2_23 (`GameOS/gameos/utils/terrain_daylight.h`), which are exactly
    the five lowest-multiplier, highest-floor-pin maps. Those frames are **byte-identical
    to before it landed.** It is live and useful (+22.9% to +31.6%) on the other 19 — a
    fix that by construction cannot reach the worst cases. Reproduced to 4 dp by two
    lanes.

12. **TERRAIN-SHADOW-GUARD-1 / `MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN` (DEFAULT-ON).**
    *Could not help:* engages only where `terrainLightDir.z < sin(15°) = 0.2588`, which is
    **the same five maps**, where it multiplies an already floor-pinned 0.3670 by
    `shadow ≤ 1`. Net effect of the two default-ON terrain lighting changes on the five
    darkest missions: `+0.0000` and `×(≤1)`. **The only default-ON movement available to
    terrain there is downward.**

13. **`81d9ace8` / `d931fdbb` — CEMENT-EDGE-CURVE-1 / `MC2_CEMENT_EDGE_CURVE`.**
    *Tried:* soften the cement/terrain tone step via the exponent `E` in
    `pow(pureConcrete, E)`.
    *Could not work:* on road and runway pixels `pureConcrete` is either **0** (no cement
    word) or **exactly 1** (forced by `if (cementHit) pureConcrete = 1.0;`,
    `terrain_lod_chunk.frag:1454`). **0 and 1 are fixed points of `pow(x,E)` for every
    `E` in [0.05, 1.0].** A mathematical no-op on every runway and road pixel on every
    mission; its authority lives entirely inside a one-tile ramp band. Its own status doc
    measured the residual at 4–5/255. *Independently derived by two lanes and confirmed
    by both skeptics.*

14. **`6b94443e` — RUNWAY-APRON-TONE-2 / `MC2_CEMENT_LIT`.**
    *Tried:* close the runway/apron tone gap.
    *Could not work on the runway, and only darkens the apron:* (a) on unclassified
    runway tiles the effect is multiplied by `pureConcrete == 0` — a literal no-op, and
    its own lane captured the mc2_10 runway **bit-identical** across all four strengths
    (66.51 L at t = 0, 0.35, 0.6, 1.0). (b) On classified cement it can only move the
    multiplier between `normalLight` and 1.0, and flat-ground `normalLight ≤ 0.35 +
    0.85/√2 = 0.95104` for **every possible sun pitch** (because
    `mclib/camera.cpp` builds `lightDirection` with unit-length xy and normalizes
    *after*, capping `L.z` at 1/√2). So `MC2_CEMENT_LIT` is **monotonically darkening**
    on this surface. Its advertised "24% gap closure" was achieved entirely by dimming
    the apron (90.43 → 84.63) while the runway did not move.

15. **`MC2_RUNWAY_TONE_MATCH` / `_TARGET` / `_STRENGTH`.**
    *Tried:* lift dark asphalt cells toward concrete tone.
    *Could not work:* the `[CEMENT_CELL_TONE v1]` census measured **`dark_cells = 0`** on
    mc2_10 (19/19) and mc2_24 (110/110). The dark asphalt family the gate exists to lift
    **does not exist in the shipping atlases.** The premise, not just the lever, was
    false.

16. **`MC2_TERRAIN_LIGHTING_V1` / `V2` (both default-OFF).**
    *Tried:* hemisphere fill to lift terrain.
    *Cannot move the ratio being complained about:* `hemiContrib` is an **absolute**
    additive term, not albedo-scaled and **not masked by `flatLitShare`** (the pad
    exemption), so it lifts the cement pads by the same number of nits as the ground. It
    can move terrain's absolute value but not the terrain-to-cement relationship, and it
    flattens albedo variation while doing so.

### Water and shoreline

17. **`c50e99a6` — WATER-SUN-SPEC-1 / `MC2_WATER_SHINE` / the Graphics Options "water
    shine" slider. ★ THE CLEANEST ENTRY IN THE SET.**
    *Tried:* sun glint on water.
    *Could not work:* `GameOS/gameos/gameos_graphics.cpp:3477-3478` uploads
    `terrainLightDir` and `u_waterSpecIntensity` to the MDI water program **every
    frame**. Neither uniform is declared in `gos_terrain_water_mdi.frag` or
    `gos_terrain_water_fast_mdi.vert` — and `git log -S"u_waterSpecIntensity" --all`
    proves **neither has ever existed in any shader in the repo's history.** The upload
    lambda skips `loc < 0` silently. Headroom 1.000x at every slider position, on every
    mission. The shader half was reverted per session review; the C++ half, the env var
    and the UI control shipped anyway. *Both the finding and the skeptic grepped
    independently: zero hits.*
    **Consequence beyond the dead knob:** the water fragment shader has **no sun
    direction at all**. A "the sun does not glint off the water" complaint cannot be
    fixed by any lighting tuning, because there is no sun term to tune.

18. **`781e6b85` — the legacy screen-space shoreline foam pass,
    `shaders/shoreline.frag`.**
    *Tried:* animated foam at the waterline.
    *Could not work:* its water classifier is `myAlpha > 0.15 && myAlpha < 0.35` on
    GBuffer1.a (`shoreline.frag:22`). Under the default renderer stack **no water pixel
    can satisfy it** — water writes alpha **0.0**
    (`rc_gbuffer1_screenShadowEligible`, `shaders/include/render_contract.hglsl:27-29`)
    and terrain writes **1.0** (`rc_gbuffer1_shadowHandled_flatUp`, 21 call sites).
    The only shader that ever wrote a continuous ~0.25 there was the **legacy** terrain
    renderer, superseded by the LOD-chunk cutover on 2026-06-09. It **still runs on every
    default frame** (`gos_postprocess.cpp:769` `shorelineEnabled_(true)`,
    `:4626-4631` unconditional; suppression is opt-in via `MC2_SHORELINE_LEGACY_SUPPRESS`,
    default false).
    **Live artifact, not just a dead pass:** the pixels that *do* fall in (0.15, 0.35)
    are **static props (0.25)** and **alpha-test lattice props (0.1875)** — so the pass is
    currently painting an animated up-to-1.4x foam rim on **building and fence
    silhouettes** adjacent to terrain. `scripts/check-render-contract-gbuffer1.sh` passes
    while blind to exactly this.

19. **`a8ea6726` (SHORELINE-STAIRCASE-1, `MC2_WATER_SHORE_FEATHER`) and `4be51440`
    (SHORELINE-GEOMETRY-1, `MC2_WATER_SHORE_MEANDER`).**
    *Tried:* fix the shoreline staircase.
    *Not running:* both are **inert at their shipped defaults** —
    `gos_terrain_water_stream.cpp:66-79` returns 0 (identity) and `:131-139` returns 0
    after WATER-MEANDER-CLAMP-1 (`67b25f47`) reverted 75 → 0. The only two shipped
    attempts at the staircase and neither is live. Four missions (mc2_06, 09, 12, 15)
    author `AlphaDepth = 0`, giving a 1.0 wu ramp across a 128 wu tile — the hard
    staircase, and entirely geometric.

20. **`3c864eac` — WATER-SHORE-MASK-1 and the V3 mask modulator.**
    *Could not work on most missions:* `u_hasShoreMask` / `u_hasShorelineMask` require a
    cooked `shoreline_mask.png` sidecar. In the 0.4 deploy lane **only mc2_01 has one**;
    mc2_03 and mc2_10 carry `control_map.png` only.

21. **V3 wet lobe — any attempt to make the wet band "pop" via terrain lighting**
    (`terrain_lod_chunk.frag:2447-2448` ✔).
    *Could not work:* `wetSand = vec3(1.16,1.06,0.78) * (litLuma * 0.90)`, so
    `mix(lit, wetSand, w)` is **homogeneous of degree 1 in `lit`**. Scale terrain
    lighting by k and the band scales by exactly k — the band's **contrast against the
    ground it sits on is invariant**. *Narrowed by the skeptic:* this holds for the wet
    lobe's chroma only, and only where `brightGround = smoothstep(0.08, 0.26, litLuma)`
    (`:2432` ✔) has saturated. It does **not** extend to the foam lobe (see §5).

---

## 4. WHERE THE NEXT FIX SHOULD GO

Per surface, the channel with actual authority.

| Surface | Go here | Why |
|---|---|---|
| **Tree** | **ASSET** (albedo), or **COMPOSITING** (post-clamp) | The multiplier is a constant 1.000 on 9 of 24 missions — the rendered tree *is* the texture. Tree median albedo L = 58.0 (p10 28.1, min 22.4 `willowdead3`). The only lighting-side levers that escape the clamp are the **post-clamp** night light-pool add (`static_prop.frag:457`, `MC2_TERRAIN_LIGHT_POOLS` default-ON) and `v_fixtureGlow` (`:624`). Do **not** add another pre-clamp term. |
| **Static prop — building** | **SHADER**, but only on the dark maps; **COMPOSITING** elsewhere | Real headroom exists (1.47x mc2_03, 2.41x mc2_05) but is ≤1.05x on mc2_22/09/04. The SH-IBL probe alone saturates the clamp on 33.0% (strength 0.55) / 35.2% (0.60) of the normal sphere **with mission lighting set to zero** — reducing `staticPropIblStrength` in `data/visual_tuning.json` is the highest-leverage single edit, since it un-pins the clamp so every other knob starts working again. Note `RenderCore/IblShRegistry.h:52-56`: all 24 missions share **one day-sky probe**, including the five `nightFactor == 1.0` ones. |
| **Vehicle** | **SHADER — a 5-line copy** | Port `shaders/mech.frag:229-233` ✔ into `gos_tex_vertex_lighted.frag`, post-multiply. Default-ON on the mech since 2026-05-28, so it is not speculative. Buys ≥1.059x on the sides and ~1.089x on the roof on **every** mission, including mc2_10 and mc2_22 where the vertex stage is fully pinned. This also closes the actual truck-vs-mech gap the user reported: 6–9% of albedo plus the glint, on top of the albedo ratio (cargotruck painted L 93.08 vs mech class median 112.0). |
| **Terrain** | **SHADER** — but fix the *gate*, not the constants | Terrain is never ceiling-pinned; it is floor-pinned on mc2_05/10/19/21 (14–100% of fragments at `diffuse == 0.02`). The single highest-value change is making `MC2_TERRAIN_AMBIENT_FLOOR`'s day gate non-zero on the five night maps — the fix already exists and is switched off exactly where it is needed. **Do not** expect `MC2_SUN_ELEVATION_FIX` to help: on mc2_05/10/19/21 (pitches 270/180/0/270) the corrected N·L is −1, 0, 0, −1, all still below the 0.02 pre-clamp, delivering **exactly 1.000x**. |
| **Cement / pad** | **SHADER (a flag flip)**, then **ASSET** | `MC2_CEMENT_MISSION_LIGHT=0` (`gameos_graphics.cpp:10449-10454` ✔) is a registered, default-ON killswitch delivering 1.016x–6.071x with no new code. `MC2_PAD_AREA_GLOW` (default 0.12, max 4.0, `gos_terrain_lod_chunk.cpp:4036-4046` ✔) is additive at full mask on pad interiors (`terrain_lod_chunk.frag:2121-2122` ✔) and is the largest single upward lever. After lighting convergence the entire residual is **albedo** — cement atlas tone. |
| **Water plane** | **SHADER CONSTANTS = the albedo** | Unique in the corpus: water has **no albedo asset**. `waterCol = mix(DEEP_COLOR(0.03,0.13,0.20), SHALLOW_COLOR(0.22,0.45,0.38), trans)` — both are uniforms, already live-tunable from Graphics Options > Water. Peak output over 24 missions is 0.849 green into an RGBA16F target with tonemap off. **Even on the two ceiling-pinned missions a shader change works**, because you change the constant that *is* the albedo. The lighting-vs-asset dichotomy does not apply here. |
| **Shoreline — waterline appearance** | **SHADER (foam lobe) + GEOMETRY** | The foam lobe is **not** lighting-invariant (§5) and has 1.24x–6.53x. But the dominant limit is geometric: `slopeAtten = clamp((N.z-0.940)/0.038, 0, 1)` is **exactly 0** on 87.9% of mc2_23's shoreline vertices, 79.9% of mc2_03's, 66.5% of mc2_22's. **No lighting change can lift a multiplicative zero.** Widen the slope window or fix the shore geometry first. |
| **Shoreline — legacy pass** | **DELETE or REPOINT** | Not a tuning problem. Either flip `MC2_SHORELINE_LEGACY_SUPPRESS` to default-ON, or repoint the classifier at a value a live shader actually writes. Leaving it as-is keeps an animated foam rim on building and fence silhouettes. |
| **Runway / road — unclassified** | **CLASSIFICATION (geometry/data)**, not lighting | 58.0% of mc2_10's paved-overlay tiles and 81.0% of mc2_03's carry no cement word. The two populations are drawn by different code paths with different lighting models, and the tone step is the seam between them. `MC2_PAVED_OVERLAY_CLASS=1` is arithmetically capable (2.725x on mc2_05/21) but its revert rests on real evidence — **9.2–21.7% hard-step pixels, 20–29/255 max delta**. Fix the classifier coverage, not the multiplier. |

---

## 5. REFUTED FINDINGS

Listed honestly. **Nothing in this section is presented as fact anywhere else in this
document.**

### 5.1 "Cement is pinned at exactly 1.000 on 100% of fragments on 100% of missions; upward headroom exactly 1.00x; every registered lighting knob on it is strictly darkening"
**Fell three ways.**
- `u_missionFlatLight` was misfiled as albedo. It is the sun term for flat pads
  (`code/gamecam.cpp:451-465` ✔, whose own comment says so). Its luma is below 1.0 on
  **22 of 24** missions. ✔ verified.
- `MC2_PAD_AREA_GLOW` is **default-ON at 0.12**, bounded at 4.0
  (`gos_terrain_lod_chunk.cpp:4036-4046` ✔), and `padGlowMask` returns **exactly 1.0**
  inside a pad (`terrain_lod_chunk.frag:341-359`, applied `:2121-2122` ✔). Cement is
  already **above** 1.000 on every night mission at stock.
- `MC2_TERRAIN_LIGHTING_V1` adds to pads unmasked. **The finding stated this itself in
  its own incapable-fix list and then asserted the opposite in its headroom field** — a
  self-contradiction, not a subtlety.
**Corrected:** 1.016x – 6.071x (table §1 row 5).

### 5.2 "Vehicle headroom relative to the complained-of state is ≤1.000x on EVERY mission" and "the registry's premise IS FALSE FOR THE MECH"
**Fell to `mech.frag`, which the finding never opened.** Two default-ON post-clamp
additive terms (`mech.frag:233` ✔ hemisphere fill at 0.15; `:284` ✔ Blinn glint at 0.05)
mean the mech's effective multiplier is **not** 1.000. The registry justification at
`RendererFeatureRegistry.h:721` is **true** at the fragment stage. Notably the finding
*caught* the equivalent post-clamp term on the vehicle side (`calcLightPools`) and then
did not run the same check on the class it was comparing against.
**Corrected:** ≥1.059x sides, ~1.089x roof, every mission.
*Also refuted:* "NOT pinned: mc2_12, mc2_17" — a flat-ground artifact; on a sun-facing
slope `I → 1`, `S = 1.0`, peak 1.47 → pinned.
*Also surfaced:* `MC2_MECH_SPECULAR_V1`'s reader is **default-ON**
(`gos_mech_batcher.cpp:310-312` ✔) despite its own comment at `:306` saying "DEFAULT-OFF"
— an eighteenth documented-vs-actual lie, verified here.

### 5.3 "On mc2_10 the static-prop roof computes 0.9952, 67.8% pinned, headroom 1.005x" — and everything downstream
**Fell to weather.** `code/weather.cpp:361-368` ✔ divides `eye->ambientRed` **and**
`eye->lightRed` by `rainLightLevel` every frame; `Weather::update` early-returns on
`!weatherActive` and `weatherActive` is false when `MaxRainDrops == 0`
(`weather.cpp:163-174` ✔). **mc2_10 is the only shipping mission with active weather**
(MaxRainDrops 500, ChanceOfRain 100 → `rainLevel` ramps to the 4.0 cap and stays).
Prop light rows are **frozen per recipe** (`mclib/bdactor.cpp:3383-3446`), so they capture
a weather-divided ambient, not the authored 192. Corroboration the skeptic did not have to
trust: PROP-RECIPE-BAKE-1's measured DIM constant **0.2471 is exactly 63/255**, and its
instance split was 28 bright / 132 dim on Mesh01.
**Corrected for mc2_10:** buildings ~1.86x (not 1.05x), trees ~1.74x (not 1.11x), roof
multiplier 0.489 (not 0.9952), building pinned **0.0%** (not 67.8%), and the tree
`max(litRgb,0.5)` floor **binds on 65.1%** of the normal sphere. The correct outlier set
is **mc2_05, mc2_10, mc2_23** — not just mc2_05 and mc2_23.
**This also invalidated the finding's own "CORRECTION 2"**, which overturned an inherited
claim using a number that was itself wrong.

### 5.4 "Max rendered luminance == albedo exactly; truckprop 29.0% of white, permanently"
**Fell to the present path.** `shaders/postprocess.frag:104-133` ✔ applies, unconditionally:
`* exposure` (per-mission in `data/visual_tuning.json` — 1.0 base, **1.05 mc2_10, 1.1
mc2_24**, the same file the finding read `staticPropIblStrength` from), then
`color *= vec3(1.05, 1.01, 0.94)`, a luminance grade, a vignette (0.82–1.0), and a
top-of-screen warm glow. Screen-centre red gain ~1.10x baseline, ~1.16x mc2_10, ~1.21x
mc2_24. The `static_prop.vert:912` ✔ clamp bounds the **multiplier**, not the pixel.

### 5.5 "Terrain has 10x–38x headroom to clip on the dark maps"
**Fell because the burnin texel is not `baseColor`.** `terrain_tint_strength_scale_`
defaults to **1.0f** (`gameos_graphics.cpp:2237` ✔), so
`terrain_lod_chunk.frag:1546-1563` ✔ mixes **18–50% of a ~0.40-luma `materialTint`** into
every non-cement terrain fragment. The shader's own comment says *"baseColor is NOT raw
albedo."* Overstated by up to **1.82x** on exactly the dark maps the argument led with.
**Corrected:** mc2_19 20.51x (not 36.73x), mc2_21 20.83x (not 37.99x), mc2_05 18.16x (not
28.65x), mc2_23 13.32x (not 20.30x), mc2_10 9.07x (not 10.28x), mc2_03 3.81x (not 4.57x).

### 5.6 "Terrain in-shader headroom 1.28x–3.27x from a geometrically correct N·L, no new constants"
**Fell: the named mechanism yields nothing on the four missions carrying the headline.**
A geometrically correct N·L never reaches 1.0 on any shipping mission. The fix exists
(`MC2_SUN_ELEVATION_FIX`, default-OFF) and delivers mc2_24 1.245x, mc2_01 1.155x, mc2_03
1.136x — but on mc2_05/10/19/21 (pitches 270/180/0/270) the corrected N·L is −1, 0, 0, −1,
all below the 0.02 pre-clamp, so the delivered gain is **exactly 1.000x**, not 3.27x.
`1.20/nl_median` is a valid statement of the `mix()` band ceiling; reaching it requires new
constants, which the sentence explicitly denied.

### 5.7 "The shoreline is pinned at 1.000x for lighting on all three mechanisms; a lighting change cannot move the waterline's appearance at all"
**Fell to the foam lobe, nine lines below the block the finding analysed.** It proved
homogeneity on the **wet** lobe (`:2447-2448` ✔) and generalised to the whole band. The
**foam** lobe at `terrain_lod_chunk.frag:2465-2469` ✔ is
`lit = mix(lit, vec3(0.92), foamAmt * (1 - pureConcrete))` — **`vec3(0.92)` is a
constant**, not scaled by `litLuma`, so `mix(lit, 0.92, F)` is **not** homogeneous. Foam
contrast is `F·(0.92 − lit)` and moves under any uniform lighting change; past 0.92 it
inverts to a dark rim. Default-ON at full strength. Compounding: `F` itself scales with
`brightGround = smoothstep(0.08, 0.26, litLuma)` (`:2432` ✔), so lighting drives foam
*amount* as well as contrast — and the finding's own unknowns concede the beach `litLuma`
distribution was never measured, making "1.00x on every mission" unsupported by its own
evidence.
**Corrected:** 1.24x typical beach, 1.74x bright, **6.53x** dark beach — and the optimum
is reachable by lighting alone, since `normalLight` spans 0.367–1.20 (a 3.27x sweep).
*Note:* the legacy pass (§3 #18) and the water-FS geometry **are** genuinely 1.000x —
those two thirds of the claim survived.
*Citation drift:* the `slopeAtten` constants were cited at `:2532-2536`; the actual lines
are `:2415-2417`.

### 5.8 "Runway/road population B is floor-pinned on 4 missions (mc2_05, 10, 19, 21); no sun-direction change can move it"
**Half fell.** On population B, `pureConcrete == 0`, so the detail normal is **not**
suppressed (`terrain_lod_chunk.frag:1483-1484`, `:1513-1515`, detail strength default
1.0). On **mc2_10 and mc2_19 the sun is exactly horizontal** (`L.z = 0`), so
`NdotL = (Nx·Lx + Ny·Ly)/|pert|` is driven **entirely** by the horizontal normal —
maximally sensitive, not dead. Only mc2_05 and mc2_21 (`L.z = −0.70711`) pin
unconditionally. **The two it loses are exactly the two its conclusion depended on.**

### 5.9 "Flat lighting on mc2_10 reaches parity and slightly OVERSHOOTS the apron"
**Fell as unsupported — verdict UNRESOLVED, not reversed.** The back-solve
`base·shadow ≈ 177–181 L` divides the measured 65.0 L by `normalLight = 0.3670`, which
(per 5.8) is a per-texel **lower bound**, not the surface mean. So the true flat-lit
landing point is **below** 177–181 L, and the error runs in exactly the direction that
flips it back to undershoot against the 145–171 L apron.
**What survives:** the finding is **right** that CONCRETE-DELIVER-2's cited ceiling
"L~24 vs concrete L~60-87" was **transplanted from mc2_23's one-tile ramp band**
(`CEMENT-EDGE-CURVE-1-STATUS.md` §3 is an mc2_23 table) into a justification bounding
**mc2_10's runway**. The revert's stated reason is unsound; its **surviving** reason is
the independent hard-step evidence (9.2–21.7% hard-step pixels, 20–29/255 max delta).
No promotion is recommended either way.

### 5.10 Systematic numeric drift (direction: overstates saturation)
Every static-prop pinned-fraction figure ran **1.5–3 pp high** (mc2_22 90.8 → 89.3,
mc2_04 78.7 → 76.0, mc2_10 67.8 → 65.4, mc2_19 21.6 → 18.3; zero-mission-light 34.8%/37.0%
→ **33.0%/35.2%**). Headroom agreed to ≤0.04x, so this is a loose pin threshold, not a
model error. The `MC2_PROP_BASE_LIGHT_DIR` immovable table ran **3–8 pp high**.
One internal inconsistency: mc2_09 tree pinning was reported at 95.1% *and* headroom
1.06x — incompatible (95.1% at 1.000 bounds headroom at 1.052x). Actual **1.001x**.

### 5.11 Two false claims found live in HEAD (both confirmed by two independent lanes)
- `shaders/terrain_lod_chunk.frag:1915` asserts `PavedOverlayClassMode()` "returns 1 when
  unset". The reader at `GameOS/gameos/gos_terrain_indirect.cpp:1100` says
  `if (!v || !v[0]) return 0;`. **It returns 0.** Bit 22 (`kCementPavedFlatBit`) is
  therefore never baked, and both `pavedOverlayFlat` branches are **dead code at stock**.
- `RenderCore/RendererFeatureRegistry.h:865` repeats the same false assertion **as the
  stated justification** for TERRAIN-AMBIENT-FLOOR-2's design. It was already false when
  written: CONCRETE-DELIVER-2 reverted the default to 0 at 11:15:09 on 2026-07-30;
  TERRAIN-AMBIENT-FLOOR-2 landed at 12:43:48. No pixel impact at stock (both arms reduce
  to the same expression), but the documented rationale is unsound.

### 5.12 Albedo census convention trap
`tools/albedo_masked_audit.py` reports `L_srgb` from the **unpainted** sum (`:634-636`);
for mech and vehicle that texture is a paint **mask**, not a colour, producing a spurious
blue cast on every mech/vehicle row. `--no-paint` produces byte-identical console output
to the default. Class medians differ sharply by convention: unpainted mech 47.0 / vehicle
48.4 / tree 58.0 / static_prop 103.9; painted mech 112.0 / vehicle 96.9. An inherited
claim of "mech 120.7, vehicle 113.8, trees are by far the darkest class" reproduces under
**neither** convention. Under either one, **trees are not the darkest class**.

---

## 6. OPEN UNKNOWNS

Ranked by how much each would change this report.

1. **★ Do props inherit a sibling actor's light row?**
   ZERO-LIGHT-ROW-1 (`478a114b`) documents that `BldgAppearance::render` and
   `TreeAppearance::render` **validate** `shape->getCachedGpuLightIndex()` but **forward**
   `staticReg.lightDataIndex`. If a tree lands on a building's row it acquires a sun term
   and the entire tree model here is wrong for that actor.
   **Settled by:** running the existing `[GUARD_FORWARD v1]` census and reporting
   `would_change`. It exists and has never been run. **This is the single measurement
   that would most change this report.**

2. **What fraction of the mc2_10 apron sample rectangle is actually chunk-cement?**
   The shader model says a fully cement-classified mc2_10 region must fall to **36.70%**
   at `MC2_CEMENT_LIT=1.0`; ROAD-OVERLAY-LIGHT-1 measured only **93.6%** (90.43 → 84.63).
   A ~10% cement fraction inside the rectangle predicts the observed −6.4% exactly, and is
   consistent with CEMENT-PATCHY-CAUSE-1's RenderDoc proof that one apron is split across
   two shaders.
   **Settled by:** `MC2_TERRAIN_LOD_CHUNK_DIAG=32768` at the identical pose, reporting the
   cement-class pixel fraction inside the exact sampling rectangle. No new instrument.

3. **Cement atlas albedo.** The catalog atlas is stitched at mission load, so it could not
   be measured offline. After lighting convergence the residual on every cement surface is
   albedo, and that number is currently UNKNOWN (bounded only at 2.07–3.64x from mc2_23's
   measured pad L 70–123).
   **Settled by:** dumping the built atlas at load and running the same luma census used
   on the burnins. No launch needed if the cook path is invokable standalone.

4. **Beach `litLuma` distribution.** Determines whether `brightGround` and `beachMat`
   hard-zero the shoreline band. Exact bounds: colormap luma < 0.0667 → hard-zero
   regardless of lighting; > 0.708 → hard-one regardless. Lighting has authority only in
   between.
   **Settled by:** a masked luminance census of each mission's baked colormap restricted
   to the shoreline vertex ring, plus the minimap-LUT material classifier's `beachMat`
   histogram over the same ring. No launch.

5. **Screen-area weighting.** Every headroom figure integrates a **uniform** normal sphere
   or equal-weighted box faces. MC2's camera favours roofs. If the user is mostly looking
   at roofs and sun-facing walls, pinned fractions are **higher** than reported; at shadow
   sides, lower.
   **Settled by:** a per-pixel normal histogram from an existing capture pose.

6. **Is water's fog mix live?** The finding asserts water's fog is disabled because
   `renderStates_[gos_State_Fog]` is 0 at water-draw time (`gameos_graphics.cpp:3183`).
   That is a runtime fact and could not be settled offline. If fog is live, every
   "lighting controls 97%/92%/64%/45% of the water pixel" figure is an overstatement at
   distance.
   **Settled by:** a one-line state dump at water bind, or a frame capture.

7. **Detail-normal scatter on floor-pinned terrain.** Pin percentages use the **macro**
   normal; `detailNormalStrength.x` defaults to 1.0. On the `L.z ≈ 0` maps this pulls the
   pin fraction toward ~50% rather than the reported 61.5%, and it is the plausible
   mechanism for "the runway changes tone along its own length". It does not move the
   median off the floor.
   **Settled by:** sampling the material normal maps at the terrain UV rate offline.

8. **Terrain shadow distribution.** All figures assume `shadow = 1`. The shadowed extreme
   is exact and worth stating: at `shadow = 0`, terrain is `0.1183 × albedo` on day
   missions and **exactly 0.0** on the five night ones — fully-shadowed night terrain
   renders black apart from pool lights and pad glow. What fraction of each map is
   actually shadowed is unknown offline.
   **Settled by:** the static shadow bake.

9. **Runway/road population split beyond 5 missions.** Tile-level unclassified fractions
   exist only for mc2_01 (29.4%), mc2_03 (81.0%), mc2_05 (40.6%), mc2_10 (58.0%),
   mc2_24 (33.8%). The other 20 are unmeasured, and a third sub-population — road art
   baked **into** solid cement atlas cells (CEMENT-INTRACELL-1: 78 of 110 mc2_24 cells) —
   has no per-mission coverage number at all.
   **Settled by:** extending the `[CEMENT_PAVEMENT_GAP v1]` census to all 25 missions,
   counting pixels rather than tiles.

10. **In-situ magnitude of the post-clamp channels.** The night light-pool add
    (`static_prop.frag:457`) and fixture glow (`:624`) are the only prop lighting levers
    **not** ceiling-limited, so a fix routed through them is exempt from every number in
    this report — but their realised magnitude is unmeasured.
    **Settled by:** dumping `calcLightPools(MC2WorldPos)` at a known night pose, or an
    A/B with `MC2_LIGHT_POOL_OBJECTS=0`.

11. **Direction of desire on mc2_10.** The arithmetic says a flat exemption *brightens*
    the runway. The user's words were *"grey here, black on the concrete"* — plausibly a
    request to make the runway **darker** relative to concrete. Headroom existing and
    headroom pointing the right way are different questions; this audit can only answer
    the first.
    **Settled by:** the click-through on mc2_10 `saved_204937` that the originating lane
    named and never got.

---

## APPENDIX — corrected reference tables

**Static prop, building path** — headroom = `1/mean(clamp(lit,0,1))` over the
equal-solid-angle normal sphere, red channel, shipped defaults (skeptic's independent
recomputation; agrees with the finding within 0.04x on all 23 non-weather missions):

| Mission | Headroom | Mission | Headroom | Mission | Headroom |
|---|---|---|---|---|---|
| mc2_22 | 1.005x | mc2_01 | 1.139x | mc2_20 | 1.315x |
| mc2_09 | 1.049x | mc2_06 | 1.179x | mc2_18 | 1.328x |
| mc2_04 | 1.051x | mc2_21 | 1.180x | mc2_13 | 1.383x |
| mc2_07 | 1.089x | mc2_19 | 1.214x | mc2_12 | 1.398x |
| mc2_14 | 1.102x | mc2_15 | 1.219x | mc2_03 | 1.470x |
| mc2_24 | 1.110x | mc2_16 | 1.221x | mc2_17 | 1.498x |
| mc2_11 | 1.136x | mc2_02 | 1.253x | mc2_23 | 1.594x |
| **mc2_10** | **~1.86x** ⚠ | mc2_08 | 1.283x | mc2_05 | 2.412x |

⚠ mc2_10 computed at the post-rebake weather-divided ambient `A = S = 63/255 = 0.2471`
(§5.3). If the live settled value 48/255 reaches the row instead: ~2.16x.

**Cement / pad** — headroom to a `(1,1,1)` flat light, realisable today via
`MC2_CEMENT_MISSION_LIGHT=0`:

| Mission | luma(MFL) | Headroom | Mission | luma(MFL) | Headroom |
|---|---|---|---|---|---|
| mc2_05 | 0.1647 | **6.071x** | mc2_13 | 0.6353 | 1.574x |
| mc2_23 | 0.4037 | 2.477x | mc2_03 | 0.6700 | 1.492x |
| mc2_12/17 | 0.4784 | 2.090x | **mc2_10** | 0.7529 | **1.328x** |
| mc2_19 | 0.5020 | 1.992x | mc2_08 | 0.8206 | 1.219x |
| mc2_21 | 0.5333 | 1.875x | mc2_20 | 0.8334 | 1.200x |
| mc2_14 | 0.5544 | 1.804x | mc2_06 | 0.8486 | 1.178x |
| mc2_02 | 0.5725 | 1.747x | mc2_09 | 0.8588 | 1.164x |
| mc2_18 | 0.6078 | 1.645x | mc2_01 | 0.8814 | 1.135x |
| mc2_11 | 0.6176 | 1.619x | mc2_07 | 0.8877 | 1.127x |
| mc2_15 | 0.6275 | 1.594x | mc2_16 | 0.8981 | 1.113x |
| | | | mc2_24 | 0.9843 | 1.016x |
| | | | mc2_04, mc2_22 | 1.0000 | **1.000x** |

**Terrain, corrected clip headroom at the median fragment** (tint-blend corrected, §5.5):
mc2_01 2.16 · mc2_09 2.52 · mc2_13 2.98 · mc2_06 3.24 · mc2_20 3.34 · mc2_24 3.37 ·
mc2_07 3.44 · mc2_08 3.63 · mc2_03 3.81 · mc2_22 4.04 · mc2_04 4.05 · mc2_16 4.23 ·
mc2_18 4.35 · mc2_17 4.48 · mc2_02 4.57 · mc2_12 5.13 · mc2_15 5.41 · mc2_11 6.04 ·
mc2_14 6.15 · **mc2_10 9.07** · mc2_23 13.32 · mc2_05 18.16 · mc2_19 20.51 ·
**mc2_21 20.83**.

---

*No game was launched. No repo file outside this one was modified. Scratch scripts were
kept out of the repository.*
