# TERRAIN-VISUAL-HEIGHT-CONSUMER-1 — RECON (geometry displacement)

Worktree `A:/Games/mc2-controlmap-sample-1` @ `5ded0f19`. RECON ONLY.

## Executive summary

**The premise "READY with no consumer" is STALE.** The consumer is already built and
wired end-to-end on this branch, gated `MC2_TERRAIN_VISUAL_DISPLACE` (default OFF).
What the prompt frames as the "highest-risk slice to design" is **substantially
shipped as S1**: vertex displacement, LOD0-band gating, a 4×-finer grid with skirts,
corner-pinned crack-free edges, SSBO binding 26 with a `GpuBufferOwner`, the mission
bake loader with size validation, and a byte-identical OFF path. The remaining real
work is **verification/acceptance of the existing S1** and the **consumer-desync
follow-ups (S2/S3)** — NOT designing displacement from scratch. Recommendation below
is re-scoped accordingly: **adopt the existing implementation as S1, prove it, then
land the desync chokepoints already stubbed in `terrain_runtime`.**

## Current anatomy (verified, file:line)

Displacement path (LIVE, gated):
- `shaders/terrain_lod_chunk.vert:33-102` — `TerrainVisualHeightBuf` binding 26
  (`heightsFine[]`, V=(mapSide-1)*4+1), `u_visualDisplace`/`u_visualSide`. Displaced
  branch (`:58-101`): interior verts read `heightsFine[fx+fy*V]` (`:89`); **chunk-edge
  + skirt verts corner-pinned onto the COARSE stitch line** via `mix(h0,h1,tt)`
  (`:66-87`) using the same per-edge stride bytes as the coarse stitch — so seams are
  pixel-identical to the coarse path (no cracks). Depth fudge preserved (`:99`).
- `gos_terrain_lod_chunk.cpp:70-73` — `s_visualHeightSsbo` GpuBufferOwner
  (`RenderResourceId::TerrainVisualHeightSsbo`, binding 26).
- `:843-851` — per-frame gate resolve: `s_visualDisplaceGate` (env) AND SSBO loaded
  AND `s_visualSide>0`; binds binding 26 for whole draw.
- `:1187-1199` — **per-chunk LOD gating**: `displaceThis = active && cmd.lodStep==1`
  (near band only); displaced patch = `getOrBuildPatch(qcX*4,qcY*4,1,...)` (4×-finer
  grid + skirts built by the SAME builder); sets `u_visualDisplace` per chunk.
- `:1289` — unbinds binding 26 on restore.
- `mclib/terrain.cpp:925-993` — gate (`MC2_TERRAIN_VISUAL_HEIGHT` or `_DISPLACE`),
  path `data/missions/<stem>.beauty/visual_height_4x.r32` (override
  `MC2_TERRAIN_VISUAL_HEIGHT_FILE`), size-validates want=V*V*4, calls
  `gos_TerrainLodChunk_UploadVisualHeightFull` (`:987`). NOT-FOUND/SIZE/READ logs.

Coarse baseline (OFF path, byte-identical): `terrain_lod_chunk.vert:104-148`
(world stride 128, stitch `:113-136`, skirt `:139`). Grid gen `getOrBuildPatch` +
`makeSamplePositions` (LOD stride, per-edge skirt mask, 1-ring apron) in
`gos_terrain_lod_chunk.cpp`.

Runtime split (chokepoints stubbed, byte-identical): `mclib/terrain_runtime.h:35-69`
— `sampleGameplayHeight`, `groundElevation` (gate `MC2_TERRAIN_RUNTIME_GROUNDING`),
`sampleVisualHeight`, `decalElevation` (gate `MC2_TERRAIN_RUNTIME_DECALS`),
`sampleWaterLevel`, `sampleWaterClass`. Parity self-test `terrain.cpp:1685-1707`.
Bake tool `tools/terrain_beautify/visual_heightfield.py:48-67` (corner-pinned bilinear,
max corner error 0) + optional `reshape_visual:90-129` (corner_clamp/max_delta).

## Option matrix (grid strategy — Q1)

| Option | Stitch/skirt/crack | Perf | Verdict |
|---|---|---|---|
| **A. Corner-pinned interior subdiv 4× via existing builder** (SHIPPED) | Edges pinned to coarse line, skirts reuse coarse mask — proven crack-free by construction | ×16 verts on LOD0 band only; coarse LODs untouched | **RECOMMENDED — already implemented; adopt as S1** |
| B. GPU tessellation (add TCS/TES) | Chunk path is NOT tessellated; retrofitting inner/outer tess factors to match per-edge stitch bytes = re-deriving crack rules in a new stage | Adds pipeline; overkill | Reject — strictly harder than A, no upside |
| C. Global 4× grid everywhere | Would ripple LOD/stitch/skirt across all bands | ×16 everywhere | Reject — cost + risk |
| D. VTF displacement, same grid density | Zero new verts → **silhouette unchanged**, only per-vertex Z nudged at coarse density | Cheap | Fallback only — loses the finer silhouette that is the north star |

**Recommendation: Option A (as shipped). Staged fallback = D** (flip
`getOrBuildPatch(qcX*4…)` back to `qcX` and let the vert sample `heightsFine` at coarse
positions) if the ×16 near-band cost regresses frame budget — keeps slope shading,
drops the finer silhouette.

## Data (Q2)
SSBO (binding 26, R32F-equivalent float[]) — **already chosen, correct**: matches
binding-23 height precedent, avoids a texture unit (chunk uses units up to 11;
`kChunkTexUnitTransitionMask=11` — unit 11 IS taken, so R32F-texture route would have
needed a free unit; SSBO sidesteps this). **Corner-pin property preserved exactly**:
bake guarantees `visual[coarse vertex] == elev` (max corner error 0,
`visual_heightfield.py:48-67`); vert corner-pins edges independently, so coarse corners
are EXACT on both sides. No change needed.

## LOD interaction (Q3) — already correct
Displacement fires only on `lodStep==1` (near band); coarser chunks stay coarse. Mixed
displaced/undisplaced neighbours are safe because the displaced patch's shared edge is
corner-pinned onto the SAME coarse stitch line the neighbour samples. **Open nicety:**
no distance fade WITHIN the LOD0 band (hard on/off at the LOD0→LOD1 boundary). The
boundary chunks already agree on the shared edge (both pinned to coarse), so no crack —
but interior slope detail pops at the band edge. S2 candidate: alpha-fade `heightsFine`
toward coarse across the last row of LOD0 (also mitigates far z-fight, but far already
undisplaced so low priority).

## Consumer desync ledger (Q4) + mitigations

| Consumer | Uses | Displaced? | Desync | Mitigation |
|---|---|---|---|---|
| Terrain self-shadow | **screen-space** post-process (`gos_postprocess.cpp:2905`), samples frag depth | Yes (frag has displaced depth) | None — reads displaced silhouette for free | none needed |
| Object→terrain shadow | shadow map + world Z, chunk binds `getShadowTexture()` (`:922-927`) | No (objects on gameplay Z) | Object shadow lands on gameplay surface, not visual → small offset on slopes | bounded by corner_clamp; accept v1 |
| Water fast path | flat plane `sampleWaterLevel` (pos-ignored) | No | Shoreline contact: visual terrain rises/dips vs flat waterline → wet/dry band shifts | keep water on gameplay; corner_clamp bounds shoreline drift |
| Decals/overlays (roads) | flat triangles on coarse grid; "roads vanish on slopes" landmine | No | Road/decal geometry flat while terrain now bumpier → z-fight/float on slopes | route decal Z through `decalElevation` (chokepoint EXISTS, `terrain_runtime.h:56`, gate `MC2_TERRAIN_RUNTIME_DECALS`) → S3 |
| Object grounding | `groundElevation` chokepoint (`:45`) == gameplay today | No | Units float/sink vs visual surface (p99 6–15wu at full strength per audit) | **corner_clamp keeps coarse corners exact → interior-only divergence, bounded**; near-unit fade unimplemented — spec S3 |
| Mine overlay | overlay on terrain height | No | z-fight on height divergence | decalElevation route (S3) |
| Picking / cursor | gameplay height (CPU MapData) | No | Cursor sits on gameplay surface, ~corner_clamp off visual | accept (gameplay-authoritative by design) |

**Grounding-fade spec (S3):** within R world-units of any live unit, lerp displaced
Z→coarse Z in the vert (needs unit positions in a UBO/SSBO — new plumbing) OR clamp
bake corner_clamp small enough (current safe default 0.0 = pure bilinear = ZERO
divergence). **v1 ships corner_clamp=0 → NO grounding drift**; reshape (nonzero clamp)
is deferred behind the drift probe.

## Gate / byte-identity (Q5)
Established pattern is correct: `MC2_TERRAIN_VISUAL_DISPLACE` default OFF; load also
gated (`_VISUAL_HEIGHT` or `_DISPLACE`); **sidecar-present requirement** enforced
(bake missing → logged, `visualDisplaceActive=false`, coarse path). OFF path is the
untouched `:104-148` branch — byte-identical (passthrough pattern, like control map).
Uniform locations guarded `>=0`; binding 26 only bound when active.

## Acceptance per sub-slice (Q7)
- **S1 (adopt+prove existing):** `terrain_workbench.py` cross-section of `heightsFine`
  vs coarse at chunk edges (corner error == 0); static-cam before/after screenshots on
  a mission WITH a bake (visual: finer slopes, identical silhouette at chunk seams);
  `slice_gate` smoke ramp tier1 gate-ON vs gate-OFF (OFF must be byte-identical);
  confirm `[VISUAL_HEIGHT v1] LOADED` + `[TerrainLOD v1] FIRST SUBMIT`.
- **S2 (band fade / optional denser):** re-run seam cross-section; screenshot LOD0→LOD1
  boundary for pop.
- **S3 (consumer fixes):** grounding-drift probe (the `terrain.cpp:1685` parity
  self-test is the tooling seam — extend it to compare `groundElevation` vs
  `sampleVisualHeight` under displace; TERRAIN-GROUNDING-AUDIT probe exists as this
  parity harness); decal-on-slope screenshot (roads don't z-fight).

## Recommended staging (re-scoped)
- **S1 = VERIFY-AND-ADOPT** the shipped displacement (no new displacement code; write
  the workbench/screenshot/smoke acceptance; ship corner_clamp=0 bake). Risk: LOW —
  code exists, gated, OFF byte-identical.
- **S2 = band-edge slope fade** (optional; solves LOD0-boundary pop / far z-fight).
- **S3 = consumer chokepoint activation**: route decals + mine overlay through
  `decalElevation`; add grounding-drift probe; only then consider nonzero corner_clamp
  reshape. Risk lands last, incrementally.

## Landmines
1. **markTerrainDrawn revives dead passes** — chunk draw calls `pp->markTerrainDrawn()`
   (`:1278`); unchanged by displace, but any grid/count edit that early-returns before
   it silently kills cloud-shadow/shoreline/godrays.
2. **Roads/decals/mines flat vs displaced** — on slopes they z-fight/float the moment
   `_DISPLACE` is on. corner_clamp=0 keeps corners exact but interiors still bump —
   decal route (S3) is mandatory before nonzero clamp.
3. **Grounding drift** — units float/sink vs visual surface; bounded ONLY while
   corner_clamp small. Do NOT ship reshape before the drift probe (S3).
4. **Skirt/tex-unit correctness** — displaced patch skirts come from the 4×builder and
   must keep the per-edge skirt mask; unit 11 is taken (`kChunkTexUnitTransitionMask`)
   so SSBO (not R32F texture) is the right data channel — don't "modernize" to a
   texture and collide.

## Wrong facts found (vs prompt)
- **"bake READY with no consumer" / "design displacement" — FALSE.** Consumer is
  implemented and wired (vert branch, LOD0 gating, 4× grid+skirts, SSBO owner, bake
  loader), gated `MC2_TERRAIN_VISUAL_DISPLACE` default OFF. This is verify-not-design.
- **"vertex Z from height SSBO binding 23, chunk vert ~:45"** — sampleH helper is at
  `:44-48`; main coarse read `:106`. Correct SSBO/binding, line ~off.
- **"unit 11 taken on chunk path? verify"** — YES, `kChunkTexUnitTransitionMask=11`
  (`:209`). Visual height correctly uses SSBO binding 26, not a texture unit.
- **"CSM depth pass uses which terrain draw?"** — MC2 terrain shadow is **screen-space
  post-process**, not a geometry CSM depth pass; terrain self-shadow consumes the
  displaced silhouette automatically. Object→terrain shadows use the shadow map+world Z.
- **"3 load-bearing rules"** — depth fudge is TERRAIN_DEPTH_FUDGE=**0** now
  (`vert:16`, net 0, not -0.004 — that band was removed per
  TERRAIN-DEPTH-BIAS-OWNERSHIP-1); the displaced branch preserves whatever the const is.

## Open user rulings
1. **Adopt-and-verify vs re-design?** Recommend adopt (code exists, gated, tested-OFF).
   Confirm the intent isn't a from-scratch rewrite.
2. **corner_clamp for v1 bake: 0 (zero grounding drift, no reshape) or small nonzero
   (nicer terrain, needs S3 drift probe first)?** Recommend 0 for S1.
3. **S3 decal/overlay reroute scope** — decals+mines only, or also water shoreline
   contact this arc?
4. **Band-edge fade (S2)** — ship it, or accept the LOD0→LOD1 slope pop for v1?
