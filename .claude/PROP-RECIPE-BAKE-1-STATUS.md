# PROP-RECIPE-BAKE-1 — STATUS

Branch `claude/nifty-mendeleev`. Probe shipped at `4b7aad66` + type-name
resolution at HEAD. Lane deploy `0.4d-rc1`.

## VERDICT

**No fix shipped. On a default launch the user still sees the defect.**

Three more candidates refuted (two of them the brief's own top-ranked ones), and
one new mechanism identified with strong measured support but **not yet A/B
proven**. Also found a measurement contaminant that has been silently corrupting
every prior lane's captures on this machine.

## 0. READ THIS FIRST — the stale env var that invalidates prior runs

This machine's shells carry `MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE=1` (plus
`MC2_GPU_CULL_STATIC_FROZEN_RECORDS=1`, `MC2_STATIC_POP_SPLIT=1`). That gate is
**default-OFF in the game the user launches.** It makes `registerStatic()` seed
every prop with a flat `0.3,0.3,0.3` ambient fallback (`bdactor.cpp:4677-4688`).

Measured, same pose, same exe, only the env differing:

| env | `zeroLightSlots` |
|---|---|
| shell as-inherited (`eager=1`) | **0** |
| `eager` unset — the user's real config | **35** |

So ~35 props on mc2_10 render from a never-baked all-zero light row on a default
launch, and the stale var hides it. It also changed one capture hash
(`control_no_buildings` `0a31b8cb3544a84b` → `9430f9f09c5ed307`). **Any lane
running captures from these shells must `env -u` all three.**

## 1. Refuted this lane

1. **"`mc2SetBakedStaticLight` is keyed by `recipeIndex`, shared across all
   instances of a recipe" — FALSE.** `registerRecipe()`
   (`gos_static_prop_registry.cpp:607`) is called **per actor**: one recipe = one
   `RecipeRange` = one multi-shape instance with its **own** model matrix
   (`staticPropGetModelMatrix(recipeIndex)`, `reserve(15000)`). Measured: 2538
   alive recipes on mc2_10. There is no cross-instance recipe sharing to poison.
2. **The `:4687` fallback bake — never fires by default.** Gated on
   `MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE`, default OFF (see §0).
3. **The Tree/Bldg sun-off asymmetry is correctly confined to real foliage —
   now proven BY NAME, closing DARK-PROP-LIGHTING-6's honest gap.** Every prop
   type that bakes a sunless row is foliage, and nothing else is:

   | sunless type | instances | alphaClass |
   |---|---|---|
   | `Pinetree` (×2 types) | 1353 | 1 |
   | `Pine2` | 649 | 1 |
   | `Pine1dead` | 25 | 1 |
   | `birch1` | 12 | 1 |
   | `Pine3dead` / `Pine2dead` / `birch2` | 17 | 1 |

   8 types sunless, 61 sunlit, **zero types straddle**. `crateswithtarps01`,
   `Barrels`, `burningtrashbarrel` are all in the **sunlit** set — confirming
   lane 6's by-class finding with per-instance data.

## 2. The mechanism — bake-order leakage of a GLOBAL sun-enable

Among **sunlit** props the ambient term splits into two populations, and the
split is **per instance, not per type**:

| type | BRIGHT (`amb≈0.7529`) | DIM (`amb≈0.2471`) |
|---|---|---|
| `Mesh01` | 28 | 132 |
| `Box01` | 12 | 22 |
| `Barrels` | 2 | 8 |
| `CorrigatedMetalBarricade` | 2 | 12 |

**Same type, same textures, same code path, 3× difference in the orientation-free
term that PROP-BASE-LIGHT-DIR-1 already proved is what actually lights these
props.** `0.7529 = 192/255`, `0.2471 = 63/255 = 0x3F` — and `0x3F` is exactly the
value FOGLIGHT-LATCH-1 measured `JeepProp` and `crateswithtarp` latching. The two
lanes' numbers now join up.

**Why it happens.** `addLightDataStructureWithPerActorColor` →
`decomposeFirstActiveLightColor` (`txmmgr.cpp:1355`) writes the per-actor terrain
colour into row slot **[0]** — the *first active* light. `GatherLightsParameters`
packs active lights in the same order, so row[0] is normally the **sun**
(`worldLights[0]`, `TG_LIGHT_INFINITE`) and the scene ambient (0.7529) survives
untouched → BRIGHT.

But if the sun is **inactive** at bake time, first-active becomes
`worldLights[1]` = `TG_LIGHT_AMBIENT`, so the terrain colour overwrites the
**ambient** instead → ambient collapses 0.7529 → 0.2471 → **DIM**, and it is
frozen forever by the per-recipe bake.

The only thing that deactivates `worldLights[0]` is the **TreeAppearance** bake
window: `bdactor.cpp:6783-6784` sets `getWorldLight(0)->active = false`, restored
at `:6900`. That is **global camera state**. Any non-foliage prop whose bake runs
inside that window inherits the sun-off state and bakes a permanently dimmed row.
mc2_10 has **2002 tree instances** interleaved with the compound props — abundant
opportunity.

This predicts precisely the reported picture: scattered near-black props beside
correctly-lit ones of the *same type*, stable across frames, textures normal,
unaffected by every downstream lighting gate tried so far — and it is consistent
with every prior lane's refutation, including the datum that killed FOGLIGHT
(`s_barracks` and `JeepProp` latch the identical `3F3F3F`: the latched value is
the same, but for a DIM-baked prop it *replaces* the 0.7529 ambient instead of
*adding* to it).

**Status: not A/B proven.** The correlation (per-instance, within-type, exact 3×,
matches both prior lanes' constants) is strong; the causal step "these specific
DIM instances baked inside a tree window" has not been directly instrumented.

## 3. Suggested next slice (do NOT skip the proof step)

1. Instrument: log `getWorldLight(0)->active` at each `mc2CacheOrBakeStaticGpuLight`
   call and correlate with the BRIGHT/DIM outcome per recipe. That is the missing
   causal link and it is one probe.
2. If confirmed, the fix is to stop leaking global light state across actors —
   scope the tree sun-off to that actor's own bake (save/restore is already there
   at `:6900`; the leak is that *other actors bake inside the window*), or bake
   foliage from a private light list. Discriminate on foliage-ness with
   **FOLIAGE-ALPHAFLAG-1's existing `alphaClass`/`GetIsAlphaTest()`** — the probe
   added this lane already surfaces it per type. **Do not build a second
   discriminator.**
3. Separately, the 35 never-baked all-zero slots (§0) are a real default-launch
   defect worth its own slice; the source itself calls the outcome a "black
   actor" (`bdactor.cpp:4655`).

## 4. What shipped

Diagnostic only, read-only, **default OFF**, no behavioural change:

- `mclib/txmmgr.cpp` — `mc2PeekStaticLightSlotSummary()`: summarizes a permanent
  static light slot the way `static_prop.vert:720-729` consumes it (ambient sum
  vs first-infinite sun colour).
- `GameOS/gameos/gos_static_prop_batcher.cpp` — `mc2PeekStaticPropTypeInfo()`:
  typeID → shape node name + alphaClass.
- `GameOS/gameos/gos_static_prop_registry.cpp` — `[PROPROW v1]` one-shot
  per-recipe dump, hung on the **already-registered**
  `MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE` gate (no new env var: `run_smoke.py` is
  claimed by PERF-HARNESS-TRUTH-1 and FIXTURE-GLOW-1, so no allowlist edit was
  possible).

## 5. Evidence hygiene

- **Measured** two-run floor: runs 2 and 3 produced **byte-identical** hashes on
  all 5 bookmarks (`e608adef3e789c24`, `b32aebc328b6f65f`, `258815452d20661e`,
  `1b5766e78c3a41cb`, `9430f9f09c5ed307`) — 0.000%, and proof the probe does not
  perturb rendering.
- Probe engagement **confirmed before trusting results**: 2538 `[PROPROW v1]`
  lines. The first run emitted **zero** — because `deploy_payload.py` had
  silently failed (wrong `--build-dir`, then wrong `--source-root`) and the old
  exe was still in the lane. Verified by exe size/mtime, not by assumption.
  Uses `fprintf(stderr)`, so the `MC2_DIAGF`/`MC2_DIAG_VERBOSE` trap does not apply.
- No before/after grading exists because **no behavioural change was made**.
