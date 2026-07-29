# CEMENT-PATCHY-CAUSE-1 — STATUS

**Lane:** CEMENT-PATCHY-CAUSE-1 · branch `claude/nifty-mendeleev` · 2026-07-29
**Base:** `ed1b51bd` · **Commit:** `7b9034cc`
**Deploy lane:** `mc2-win64-v0.4d-rc1` · **tier1 5/5 PASS**, `[DEPLOY_FINGERPRINT] OK: exe sha=7b9034cc00df matches worktree HEAD` on all five missions.
**Verdict:** the mechanism is found, and it is **neither the atlas nor either of the two suspects the brief named**. It is a **seam between two different shaders**.

---

## 1. The mechanism, with the receipt

A paved surface on mc2_23 is drawn **half by the terrain and half by the overlay
layer**, and the two shaders shade concrete differently. Where they abut, the
difference appears as a hard line through flat concrete — the user's *"atrocious
outlines of the concrete… concrete shouldn't magically become a different
color."*

**RenderDoc, pixel history against the pre-composite HDR scene FBO** (`resid
231`, R16G16B16A16_FLOAT — **not** the backbuffer; capture
`scratch/cpc1/rg_bm.rdc`, mc2_23 `runway_ground` bookmark replay, frame 150):

| pixel | final writer | program | what it is |
|---|---|---|---|
| `(2000,1700)` — **lighter** concrete | **eid 3678** `glDrawArrays`, 48 verts, TriangleList, opaque, depthTest+depthWrite, NoCull | VS `Shader 211` / PS **`Shader 212`** | **`shaders/terrain_overlay.frag`** (identified by SPIR-V disasm: SSBO block `TerrainCementBufOverlay { uint cementWordsPG[] }` at binding 25 is unique to that file) |
| `(2200,1700)` — **darker** concrete, 200 px away | **eid 465** `glDrawElements` | VS/PS `Shader 226/227` | terrain LOD-chunk draw (`terrain_lod_chunk.frag`) — and **eid 3678 is depth-REJECTED here** (`passed=False`) |

At `(2000,1700)` the terrain draw (`eid 408`) writes `[0.1515,0.1836,0.1495]` and
is then **overwritten** by eid 3678 with `[0.301,0.3083,0.3623]`. At
`(2200,1700)` the terrain draw writes the final value directly.

**The step is one pixel wide with no transition.** Luma profile across row 1700
of the default capture (`scratch/cpc1/base/r1/mc2_23_runway_ground.png`):

```
… 79.5 79.5 79.5 79.5 79.5 | 70.4 70.4 70.4 70.4 70.4 …
                            ^ x = 2050
```

Crop pair (beauty above, class-diag below, same crop):
`scratch/cpc1/crop_boundary.png` — one straight diagonal line between two flat
greys, and the identical line in the class map.

**This is why eleven lanes of cement-atlas work could not move it.** Roughly a
quarter of the visible "concrete" is **not drawn by the terrain shader at all**,
so no atlas lever — tone, canon, road-detect, edge-curve, ramp-word — can reach
it. It also explains why `MC2_PAVED_OVERLAY_CLASS` felt close and still made
things worse: it correctly smelled a *lighting-model split at the pavement*, but
tried to fix it from the terrain side, where half the pixels are not.

### Independent corroboration, no RenderDoc needed

Every terrain debug channel is **inert** on the lighter side and **live** on the
darker side, at the same two pixels (`scratch/cpc1/*/mc2_23_runway_ground.png`):

| capture | `x=2000` (overlay) | `x=2060` (terrain cement) |
|---|---|---|
| default | `(77,79,92)` | `(69,69,88)` |
| `MC2_TERRAIN_LOD_CHUNK_DIAG=32768` (cement class) | `(77,79,92)` — unchanged | `(255,0,0)` = class 1 solid cement atlas |
| `…=128` (terrainType) | `(77,79,92)` — unchanged | `(255,0,0)` = pureConcrete |
| `…=65536` (cement bypass) | `(77,79,92)` — unchanged | `(38,46,38)` |
| `MC2_LIGHTING_DEBUG_VIEW=albedo/sun/ambient/shadow` | `(77,79,92)` — unchanged in **all four** | responds in all four |

A pixel that ignores the terrain shader's own debug bitmask **and** the unified
lighting-debug channel is not a terrain fragment.

## 2. Effect size (two-run floor published first)

Floor, `run_visual_capture --runs 2`, mc2_23 default env:
**`runway_ground` / `runway_mid` / `runway_overview` = STABLE (0.0000 %)**;
`beacons_ground`, `beacons_mid`, `both_fields_overview` **DRIFT ⇒ not used**.
mc2_24 `base_near` / `water_near` **STABLE**. mc2_10 not used.

Seam step, `cement − overlay`, sampled 8 px either side of every class-1↔overlay
horizontal transition, restricted to low-chroma pavement on **both** sides:

| pose | overlay px | cement px | seam n | median step | %\|step\|>6 |
|---|---|---|---|---|---|
| `runway_ground` | 2,544,192 | 5,282,717 | 21,355 | **−4.95 L** | 69 % |
| `runway_mid` | 1,779,079 | 6,241,722 | 14,006 | **−16.93 L** | 87 % |
| `runway_overview` | 1,813,727 | 5,456,447 | 23,782 | **−14.81 L** | 92 % |

*Mask honesty:* "overlay" here = pixels inert under both terrain diag captures.
That set also contains buildings/props; the low-chroma + luma-band filter removes
most, and the RenderDoc rows above pin the specific boundary. Treat the table as
the scale of the effect, not a pure overlay/cement census.

## 3. What was refuted along the way

- **Suspect 1 — per-tile `pureConcrete` variation.** Refuted. On the apron,
  `MC2_TERRAIN_LOD_CHUNK_DIAG=128` shows `pureConcrete` saturated (`(255,0,0)`)
  across **every** terrain-drawn apron pixel; `=32768` shows the apron is
  **uniformly class 1** (solid cement atlas) — 65.8 % of the overview frame, with
  class 2 at 0.49 %. There is no patchwork of cement classes and no partial
  lighting exemption. The `[CEMENT_TRANSITION_DROP]` "dropped tile falls back to
  bare colormap" story does not fire on these poses.
- **Suspect 2 — road-tileset piece placement.** Not the visible defect. The
  large-scale tone field inside the cement apron (concrete background only, road
  texels excluded) is flat at `spread ≈ 12 L` on `runway_ground`/`runway_mid`;
  the `75 L` spread on `runway_overview` is **runway light pools**, visible as
  discrete blobs in `scratch/cpc1/field_overview.png`. No stub/mismatch signature.
- **The cement path is not making it worse — it is masking it.** Under
  `MC2_TERRAIN_LOD_CHUNK_DIAG=65536` (all cement overrides bypassed) the boundary
  step *grows* from −12.7 to −27.5 L on `runway_overview`. The cement atlas is
  pulling the terrain side **toward** the overlay, not away from it.
- CEMENT-INTRACELL-1's finding stands and is confirmed: the atlas concrete
  background is one colour; the dark shapes are an authored road tileset.

## 4. What was shipped — and its honest limit

`MC2_OVERLAY_CEMENT_SHADOW_MATCH=1` (**default OFF**, `7b9034cc`).

The two shaders were found to disagree on a specific, nameable term:
`terrain_lod_chunk.frag` drops the **static (baked) shadow** on concrete
(`shadow = mix(shadow, dynS, pureConcrete)`, line ~1937) and forces
`normalLight = 1.0`; `terrain_overlay.frag` does `c.rgb *= staticShadow *
dynamicShadow` with no cement exemption at all. When armed, an overlay fragment
whose own 128 wu tile carries a **VALID cement word** (bit 31 — read from the
SSBO already bound at binding 25 for `PAD-AREA-GLOW-1`, so no new binding and no
new texture fetch) drops `staticShadow` and keeps `dynamicShadow`, matching the
chunk frag exactly.

**⚠ Measured effect: none.** ON vs OFF is **0.0000 % changed, max Δ = 0** on all
three floor-valid mc2_23 poses. mc2_23 is a **night** map, where the static
shadow term is already ~1 on the apron, so the one term I could align is the one
term that is inert there. The mc2_24 `base_near` day pose showed 1.98 % changed —
**but the diff is a moved vehicle**, not the gate (`scratch/cpc1/m24_triple.png`:
the diff is black across all concrete and road, and magenta on the vehicle); the
`water_near` no-pavement control is exactly 0.0000 %. **I do not claim this gate
cures anything.** It removes a real latent inconsistency that will bite the
moment a static-shadow caster lands on an overlay-covered apron; it is not the
cure for the user's screenshot.

- OFF is **byte-identical**: post-build OFF vs the pre-build default capture =
  **0.0000 % on all three poses**.
- `gate_status` verdict **OK** (registered / documented / smoke-allowlisted /
  default OFF / 2 readers). `check-env-registry.sh` **PASS** (1010 vars).
  `check-contracts.sh` **74/74**.
- Hard-step delta on mc2_24 `base_near`: **+0.005 pp** (bar was ≤0.62 pp) — and
  even that is the vehicle.

## 5. What is left, and what I recommend

The *operative* difference at the seam is **not** the static-shadow term. Having
excluded it by measurement, what remains is the **albedo/light path itself**: the
terrain side is `cement atlas texel × u_missionFlatLight`, flat-lit by
construction; the overlay side is `vertex Color × decal texture`, carrying its own
baked vertex lighting and fog. Two different art sources and two different
lighting models for one physical surface. Closing that is a **design decision, not
a tuning knob** — either the overlay adopts the cement atlas + flat light on
cement tiles, or the cement terrain adopts the overlay's shading there. Both are
visible, blast-radius changes that should be a user-present lane with a §19-style
before/after, not something I pick unilaterally after this lane already spent its
budget proving where the boundary is.

**REVIEW §19 path:** I did **not** add an Arm D. §19 arms are *user choices*
between visibly different renders; a gate measured at 0.0000 % is nothing to look
at, and putting it there would misrepresent it. What belongs in §19 from this lane
is **evidence, not a choice**: the `crop_boundary.png` pair and the class-diag
overlay, captioned with the two eids — so the next reader sees in one image why
the atlas was never the lever. That is a pack edit, and `release_assets/` is
user-gated, so it is left staged here rather than written.

## 6. Process / deviations

- **Isolated build:** reused `A:/Games/mc2-cac2` (the predecessor lane's tree),
  checked out **detached at `7b9034cc`**, clean tracked tree, full relink. Not a
  30th worktree — disk is tight.
- **Deploy lane deviation, disclosed:** `mc2-win64-v0.4d-rc1`, same lane the
  predecessor used. Not in the forbidden set (v0.3 / v0.4 / v0.4c /
  `refbuild-cf97ed52`). Lane lock and smoke lease were both free at claim time
  (`deploy_status` verdict OK).
- `run_smoke` **refused to start** with `MC2_STATIC_POP_SPLIT` /
  `MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE` / `MC2_GPU_CULL_STATIC_FROZEN_RECORDS`
  ambient — the ENV-DROP guard fired correctly; re-run with them unset.
- `preflight` reported `safe_to_touch=false` from **42 untracked `scratch/*` dirs
  left by sibling lanes**. No tracked file outside my claim was touched; the
  commit is exactly 5 files.
- A `MC2_TERRAIN_DEBUG_MODE` probe of the overlay's own `surfaceDebugMode 30/31`
  (dynamic vs combined shadow) changed **0.0000 %** — that selector does not
  reach the overlay program on this path (the known "thin-path-forgot-uniform"
  trap). Noted, not chased.
- Artifacts under `A:/Games/mc2-main/scratch/cpc1/` (captures, `.rdc`, pixel
  histories, comparison strips). The `.rdc` and the 24 MB PNG sets are prunable.

## 7. Open / user-gated

- **The design call in §5** — which shader wins on a cement tile. This is the
  real cure and it needs the user's eye.
- Whether to keep `MC2_OVERLAY_CEMENT_SHADOW_MATCH` at all, given it is currently
  a measured no-op (my recommendation: keep, default OFF — it is correct, free,
  and prevents the inconsistency from becoming visible later).
- The §19 evidence insert (pack write is user-gated).
