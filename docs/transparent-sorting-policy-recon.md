# Transparent / Additive Sorting Policy — Recon

**Slice:** TRANSPARENT-SORTING-POLICY-RECON-0 · **Date:** 2026-06-11 · **Status:** recon only, no code changed.
Worktree `nifty-mendeleev`. Line numbers grep-confirmed at recon time; re-grep before quoting.

Pairs with: `docs/render-pipeline-map.md` (spine map), `docs/vfx-rv-arc-recon.md` §4 (VFX blend matrix),
`docs/vfx-overdraw-audit.md` (additive cost), `docs/water-reflection-pass-plan.md`,
`docs/renderpass-contract-spec.md` (descriptive pass registry — NOT a scheduler).

---

## 1. Current ordering model — two different worlds

### 1.1 Legacy MLR sorter (`mclib/mlr/`)

The only path with a *real* transparency sort. `MLRSorterByOrder`
(`mclib/mlr/mlrsortbyorder.cpp`) maintains **16 priority buckets**
(`MLRState::PriorityCount = 1<<4`, `mclib/mlr/mlrstate.hpp:317-351`;
`DefaultPriority=0`, `AlphaPriority=2`). Per frame:

- Non-alpha buckets: drawn in bucket order, optionally **texture-sorted** within
  a bucket (`gEnableTextureSort`, `mlrsortbyorder.cpp:255,475`).
- `AlphaPriority` bucket: per-primitive `SortAlpha` records carrying a camera
  `distance`, **back-to-front shell-sorted** (`mlrsortbyorder.cpp:561-583`,
  comparator `(*alphaArray)[jj-hh]->distance < tempSortAlpha->distance`), then
  drawn via immediate `gos_DrawTriangles/Quads/Lines` (`mlrsorter.cpp:55,185,221`).
- GL state per primitive comes from `MLRState::SetUsedState`
  (`mclib/mlr/mlrstate.cpp:326-350`): `OneZeroMode` (opaque), `OneOneMode`
  (additive), `AlphaInvAlphaMode` (alpha blend + alphaTest 1), `OneInvAlphaMode`.

This sorter now serves only the **unrouted gosFX leaves** flushed at
`theClipper->RenderNow()` ("Draw the FX", `code/gamecam.cpp:~394`):
PertCloud/ShapeCloud/Shape/DebrisCloud/PointLight — and Shape/ShapeCloud/Debris
are *already dead by default* (`MC2_DISABLE_GOSFX` gate, `mlr_gate.cpp`
`kDefaultDisabled=true`; see `docs/vfx-3d-mesh-substrate-recon.md`). So the one
correct back-to-front sorter in the engine is sorting an almost-empty list.

### 1.2 Legacy `renderLists()` bucket walk (`mclib/txmmgr.cpp:1955+`)

NOT a sort — a hardcoded sequence of flag-filtered linear scans over
`masterVertexNodes[]` (submission order = texture-node allocation order; no
depth key anywhere):

| # | Bucket (Tracy zone) | Filter | Order within |
|---|---|---|---|
| 1 | `Render.3DObjects` solids | `MC2_DRAWSOLID` (`txmmgr.cpp:2071,2537`) | node index |
| 2 | shadow pre-passes / GPU batcher flushes | — | fixed |
| 3 | `Render.TerrainOverlays` / `Render.Decals` | GL-direct batches (`:2748,2780`) | push order |
| 4 | `RenderLists.TerrainAlphaWaterLoops` | `ISTERRAIN+DRAWALPHA` (`:2801-2864`) | alphaTest-off then -on; node index |
| 5 | `Render.NoUnderlayer` | `ISTERRAIN+GPUOVERLAY` (`:2879-2927`) | node index |
| 6 | `RenderLists.ShadowBlobs` | `ISSHADOWS+DRAWALPHA` (`:2959-3000`) | node index |
| 7 | `RenderLists.NonTerrainAlphaLoops` | `!ISTERRAIN+DRAWALPHA` (`:3015-3063`) | alphaTest split; node index |
| 8 | `RenderLists.VfxHudSubmit` effects | `MC2_ISEFFECTS` (`:3093-3133`) | node index |
| 9 | spotlights `MC2_ISSPOTLGT` (`:3137`), compass `MC2_ISCOMPASS` (`:3185`) | node index |

The "sort key" of the legacy world is therefore **the bit-flag** (`txmmgr.h:52-64`)
plus the hardcoded bucket sequence. Transparent objects within a bucket are
unsorted (no per-node depth), relying on submit order + depth-test luck.

### 1.3 Modern GPU paths

Each GPU family is a self-contained immediate pass at a fixed point in the
frame loop (`code/gamecam.cpp:284-402`); ordering between families is the
**call sequence**, not a key. Within a family, order is batch/group order
(static-prop two-group split, particle group map). No cross-family transparent
sort exists; correctness rests on (a) opaque-first call order, (b) depth-test
with depth-write OFF for everything blended.

---

## 2. Current-state table per draw family

ZComp values: gos `1` = test-on (reverse-Z GEQUAL), `2` = no-write compare, `0` = off.

| Family | Pass / call site | Depth test | Depth write | Blend | Sort | Batched? |
|---|---|---|---|---|---|---|
| Terrain solid (GPU indirect) | `Render.TerrainSolid` → `DrawIndirect()` (`txmmgr.cpp:2467,2514`) | GEQUAL | **ON** (explicit, see chunk-driver lesson in MEMORY 10.3) | OFF | GPU cull order | MDI |
| Static props opaque | `Render.GpuStaticProps` alpha-OFF group (`gos_static_prop_batcher.cpp:6170`) | GEQUAL | ON | OFF | type/packet sort (`s_sortedPacketOrder`) | MDI |
| Foliage / alpha-test props | same pass, alpha-ON group (`:6191`; `STATIC_PROP_FLAG_ALPHA_TEST` `:2129`) | GEQUAL | **ON** | OFF (frag `discard`) | drawn **after** opaque group | MDI |
| Mechs (GPU batcher) | `Render.GpuMechs` (`txmmgr.cpp:2719`) | GEQUAL | ON | OFF | bucket order | instanced |
| Vehicles / CPU fallback (Spine A solid) | `Render.3DObjects` (`txmmgr.cpp:2071`) | ZComp 1 | ON (`:1973,1989`) | `OneZero` | node index | per-node `glDrawElements` |
| Terrain overlays / mines (runway, cement) | `Render.TerrainOverlays` (`gameos_graphics.cpp:8700-8712`) | GEQUAL | (mask as set by caller; static decal path restores TRUE `:8732`) | **OFF** (`glDisable(GL_BLEND)` `:8701`) | push order | small batch |
| Decals (craters, footprints) | `Render.Decals` → `drawDecals()` (`gameos_graphics.cpp:8764-8768`) | GEQUAL | **OFF** | SRC_ALPHA/ONE_MINUS | push order | batch, cleared per frame |
| Legacy terrain alpha + water (MLR nodes) | `TerrainAlphaWaterLoops` (`txmmgr.cpp:2794,2807`) | ZComp 1 | inherits (write ON from preamble) | AlphaInvAlpha | node index; non-water layer **skipped** (`:2815-2819`) | per-node |
| Water (GPU fast path) | `renderWaterFastPath()` after renderLists (`gamecam.cpp:354`, `gameos_graphics.cpp:2402`) | GEQUAL | **OFF** | SRC_ALPHA/ONE_MINUS | GPU cull order; base then detail layer | MDI |
| Shadow blobs (legacy) | `ShadowBlobs` (`txmmgr.cpp:2941-2953`) | ZComp 2 | **OFF** | AlphaInvAlpha | node index | per-node |
| Non-terrain alpha (legacy transparent props/FX geom) | `NonTerrainAlphaLoops` (`txmmgr.cpp:3007-3011`) | ZComp 1 | **ON** ⚠ (`:3011`) | AlphaInvAlpha | node index, **no depth sort** | per-node |
| Lasers/beams (`MC2_ISEFFECTS`) | `VfxHudSubmit` (`txmmgr.cpp:3087-3089`) | ZComp 1 | OFF (`:3089`) | **OneOne additive forced** for whole bucket | node index | per-node |
| Spotlight cones | `:3135-3139` | ZComp 1 | **ON** (`:3135`) ⚠ | inherits OneOne | node index | per-node |
| Compass / world HUD | `:3179-3187` | **OFF** (ZComp 0) | OFF | AlphaInvAlpha + alphaTest | node index | per-node |
| GPU particles (alpha) | post-renderLists `Batcher::Flush` (`gamecam.cpp:388`; `gos_particle_bridge.cpp:303-307`) | GEQUAL | OFF | SRC_ALPHA/ONE_MINUS | **group (texture/blend) order only — no depth sort** | SSBO, 1 draw/group |
| GPU particles (additive) | same (`gos_particle_bridge.cpp:388-392`) | GEQUAL | OFF | SRC_ALPHA/ONE (order-independent) | group order | same |
| GPU trails | same bridge | GEQUAL | OFF | group blend | ring order | same |
| Unrouted gosFX (MLR clipper) | `theClipper->RenderNow()` (`gamecam.cpp:~394`) | per MLRState | per MLRState | per MLRState | **true back-to-front** in AlphaPriority bucket (`mlrsortbyorder.cpp:561-583`) | immediate |
| Weather | `gamecam.cpp:~398` | — | — | alpha | submit order | immediate |
| HUD batch | replay post-composite (`gameosmain.cpp:622`) | z=0.9999 reverse-Z near | — | alpha | submit order | batched |
| ImGui | last (`gameosmain.cpp:626`) | OFF | OFF | alpha | submit order | batched |

---

## 3. Observed conflicts / bugs

1. **`NonTerrainAlphaLoops` blends with depth-write ON** (`txmmgr.cpp:3011`):
   alpha-blended geometry writes Z in node order. A near transparent surface
   submitted early occludes a far one submitted later, and punches holes in the
   later GPU water/particle passes' depth tests. Classic legacy bug, latent
   because this bucket is mostly empty now (vehicles/CPU-fallback only).
2. **Spotlight cones depth-write ON while additive** (`:3135`) — same class.
3. **No depth sort anywhere except the near-dead MLR AlphaPriority bucket.**
   GPU particle *alpha* groups draw in arbitrary group order with no
   back-to-front (additive groups don't care; alpha groups do). Smoke-over-smoke
   from two different textures can pop order per frame.
4. **Whole-bucket blend coercion:** the `MC2_ISEFFECTS` bucket forces
   `gos_Alpha_OneOne` for every node (`:3087`) regardless of the node's intent —
   any alpha-blend effect routed there silently becomes additive.
5. **Cross-family ordering is positional, fragile:** water draws after particles
   would break; particles before renderLists would break (no depth). The order
   is encoded only in `gamecam.cpp` call sequence + comments ("trap #6",
   `vfx-rv-arc-recon.md` §4). The chunk-terrain transparency saga (MEMORY 10.3,
   `f375e0ba`) and the particle-bridge cache-invalidate hazard are both
   "inherited GL state across positionally-ordered passes" failures.
6. **Terrain overlays draw blend-OFF before legacy water but after solids**, so
   water tints them (correct), but decals (blend ON, depth-write OFF) draw
   *before* water yet sample no water depth — submerged craters render fully
   then get water blended over: acceptable, but only by accident of order.
7. **Two transparency regimes for the same content class:** routed gosFX leaves
   = GPU bridge (group-ordered), unrouted leaves = MLR (distance-sorted). The
   same explosion can have its CardCloud sorted differently than its PertCloud.
8. **Mixed compare-state encodings:** legacy buckets use gos ZComp 0/1/2,
   GL-direct passes use raw `glDepthFunc(GL_GEQUAL)` + cache invalidation. Every
   new pass re-derives reverse-Z by hand (decal `:8766` comment "was GL_LEQUAL").

---

## 4. Proposed unified layer policy

A single explicit **layer enum** (extend `RenderCore/RenderPassContract.h`
descriptively first), every draw family tagged. Draw order = layer order;
within a layer, order = sort key (§5). Target sequence:

| Layer | Contents | Depth test | Depth write | Blend |
|---|---|---|---|---|
| 0 Opaque world | terrain solid, static props (alpha-OFF), mechs, vehicles, animated buildings | GEQUAL | ON | OFF |
| 1 Alpha-test (masked) | foliage cards, alpha-ON prop group, (future foliage depth-prepass slots here) | GEQUAL | ON | OFF + discard |
| 2 World overlays on opaque | terrain overlays/mines (blend OFF today — keep), decals/craters/footprints | GEQUAL | OFF | per family |
| 3 Transparent surfaces | water base + detail, future water reflection blend, legacy water nodes | GEQUAL | OFF | alpha |
| 4 VFX alpha | alpha-blend particle groups, alpha MLR leaves, shadow blobs, weather | GEQUAL | OFF | alpha, **back-to-front within layer** |
| 5 VFX additive | additive particle groups, lasers/beams, trails, spotlight cones | GEQUAL | OFF | SRC_ALPHA/ONE (order-free, batch by texture) |
| 6 Screen/world HUD | compass, VTOL markers | OFF or near-plane | OFF | alpha |
| 7 UI | HUD batch replay, ImGui | OFF | OFF | alpha |

Rules:
- **Depth-write ON is illegal at layer ≥ 2** (fixes §3.1/3.2 by construction).
- Additive after alpha (alpha needs ordering, additive doesn't; additive last
  also concentrates bloom input).
- Each layer-owner sets ALL state it depends on and invalidates the gos cache
  (the chunk-terrain lesson, generalized).
- Layer membership is data (the contract registry), not `gamecam.cpp` position;
  the frame loop eventually iterates layers instead of hand-calling passes.

## 5. 64-bit sort key sketch

For the (eventual) unified submission list; high bits sort first:

```
bits 63-60  layer        (4b)  — table in §4
bits 59-56  viewport/view (4b) — main scene, water-reflection RT, shadow, RTT editor
bits 55-54  translucency class (2b) — 0 opaque, 1 masked, 2 alpha, 3 additive
bits 53-30  depth        (24b) — opaque/masked: FRONT-to-back (Hi-Z/early-Z win);
                                 alpha: BACK-to-front (invert: ~d);
                                 additive: ignored, repurposed as batch-stability salt
bits 29-14  material/pipeline id (16b) — program + blend + texture-array group
                                 (for opaque this dominates *after* coarse depth
                                  bucketing; consider swapping depth/material for
                                  opaque if state-change cost > overdraw cost)
bits 13-0   instance/packet id (14b) — stable tiebreak (deterministic frames)
```

Depth quantization: normalized view-space distance / far, 24-bit fixed point —
matches the MLR sorter's float `distance` semantics. GPU families don't need
the key per-instance (cull shaders own intra-pass order); the key orders
**packets/groups** at the CPU submission level, replacing both the bit-flag
buckets and the positional call sequence.

## 6. Migration order (lowest risk first)

1. **State-hygiene fixes (no reorder):** flip `NonTerrainAlphaLoops` and
   spotlight depth-write to OFF (`txmmgr.cpp:3011,3135`) behind a default-ON
   env with opt-out; per-node blend honored in the EFFECTS bucket. Tier1 gate.
2. **Descriptive layer tagging:** add `layer` field to `RenderPassContract`
   rows + the legacy buckets; inspector shows the de-facto order. No behavior.
3. **Particle alpha-group sort:** sort alpha groups (and optionally particles
   within a group) back-to-front by group centroid distance at
   `Batcher::Flush` — small CPU cost (~8 groups worst case, audit §3). Additive
   groups untouched.
4. **Decal/overlay/water layer assert:** runtime check (env-gated) that
   depth-write is OFF whenever blend is ON — catches §3-class regressions.
5. **Bucket consolidation:** route the EFFECTS/spotlight/shadow-blob legacy
   buckets through the layer table (same draws, table-driven state).
6. **Sort-key submission list (the real unification):** introduce the 64-bit
   key for the CPU-side packet list; legacy buckets become key emitters; retire
   `MC2_*` bit-flag scan loops. Per-pass kill-switches throughout.
7. **Retire the MLR AlphaPriority sorter** once the remaining unrouted gosFX
   classes are routed (3D-mesh substrate arc) — its back-to-front semantics
   move into layer-4 ordering.

---

**Verdict:** there is no unified transparent ordering model today — one true
sorter (MLR, nearly dead), one flag-bucket walk (legacy), and positional call
order (GPU paths). The two live correctness bugs are depth-write-ON blended
buckets; everything else is fragility, not breakage. Fix state first, tag
layers second, sort-key last.
