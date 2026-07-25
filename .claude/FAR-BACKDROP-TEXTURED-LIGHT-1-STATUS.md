# FAR-BACKDROP-TEXTURED-LIGHT-1 — status 2026-07-23

Branch `claude/nifty-mendeleev`, worktree A:/Games/mc2-main, HEAD `9f8c8086`.
Lane FAR-BACKDROP-TEXTURED-LIGHT-1 (bdactor.cpp/.h claimed, no conflicts).
Deploy lane `0.4` (also claimed by lows-cluster-1 — coordinate before any deploy).

## Headline
Confirmed the **fullbright-impostor mechanism from shader source** — this
resolves the deferred team's standing open question ("why do impostors render
fullbright *despite* sharing LOD0's light slot?", DEFERRED-BATCH-8C Item B).
**No code shipped**: the sound fix is a shader-level `base_light` change on the
hot static-prop path, which is regression-sensitive and, per the mission's own
verification bar (day + 3 night poses + LOD sweep + letterbox), MUST be A/B
verified before landing. Shipping it unverified would risk repeating the
already-A/B-disproven peer no-op `73755184`. Attribution advanced; fix designed
and ready to implement + verify in a session that can hold the 0.4 lane.

## Root cause (source-grounded, NOT re-derived speculation)

The static-prop / tree GPU draw computes final vertex light in
`shaders/include/lighting.hglsl::calc_light(lights_index, normal, worldPos, base_light)`:

```
if (0 == ld.numLights.x)
#ifdef MC2_STATIC_PROP_LIGHTING
    return base_light;          // <-- empty slot returns base_light untouched
#endif
...
return final + ambient;         // final starts = base_light, lights added on top
```

`static_prop.vert:332` / `gos_tex_vertex_lighted.vert:75`:
`base_light = get_base_light(perVertexARGB, ...)` — i.e. **base_light is the
per-vertex baked light color** carried in the immutable per-type VBO.

The chain that produces fullbright/black far geometry:

1. Tree impostor / far LOD is registered into the immutable per-type VBO via the
   **positions-only** hierarchy walk (`TransformMultiShape_HierarchyOnly`,
   bdactor.cpp:6517/6557; GPU-INSTANCE-SKIP-POOLS-1). This path does **NOT** run
   the per-vertex full light bake — so the recipe's `perVertexARGB` is a
   **stale / mission-load (day) value**, never re-baked for night.
2. The per-frame shared-slot patch (`staticReg[activeLOD].lightDataIndex =
   LOD0 slot`, bdactor.cpp:6494-6495 / 6569-6570) DOES give the impostor draw a
   night-correct **object-light** slot — that part works.
3. But `calc_light` returns `base_light + object_lights`. Because `base_light`
   (`perVertexARGB`) is the stale day/near-white value, the sum clamps bright →
   **fullbright impostor at night**, and flashes bright at the LOD pop (the
   moment the recipe swaps to the never-night-baked far LOD).
4. The far-terrain-ridge **black band** (Item A) is the same defect with the
   opposite baked seed: that geometry's `perVertexARGB` bakes to ~black
   (lightsOut / dark day bake), so at night it stays PURE BLACK. This is
   consistent with DEFERRED-BATCH-8B RenderDoc (eid 12756 tonemap composite
   samples a scene FBO already dark in the far region) and batch-7
   ("real geometry drawn PURE BLACK", albedo debug view does not repaint it —
   the albedo view reskins terrain chunks, not the static-prop VBO path).

**So the shared light *slot* was a red herring for both symptoms — the object
light is fine; the stale per-vertex `base_light` is the fault.** This is why
peer `73755184` (flooring the untextured gos_vertex path) was A/B-proven a no-op
(DEFERRED-BATCH-8C): it touched neither `perVertexARGB` nor the textured
static-prop program.

## Gated fix design (implement + verify next; gate MC2_FAR_BACKDROP_LIT, default-OFF)

Two viable seams; prefer (A):

**(A) Shader base_light substitution (minimal, cheap).** Add a per-instance flag
(e.g. `kFlagFarBackdropLit`, set in bdactor.cpp when `activeLOD == _maxAvailLOD`
and the gate is on) OR a global uniform `MC2_FAR_BACKDROP_LIT`. In
`static_prop.vert` / `gos_tex_vertex_lighted.vert`, when set AND the slot has
`numLights.x > 0`, **derive base_light from the object-light ambient/night term
instead of the stale perVertexARGB** — i.e. pass a night-neutral base (or
`vec3(0)` and rely on the object-light ambient + fog) so the impostor takes the
mission night shade rather than its day-baked vertex color. Byte-identical when
off (flag never set / uniform 0 → existing `get_base_light(perVertexARGB,...)`
path unchanged).

**(B) CPU per-vertex night re-bake for the far LOD recipe.** Under the gate, run
the full `TransformMultiShape` (not HierarchyOnly) for the impostor recipe so
`perVertexARGB` is night-correct. Sound but costlier (defeats the pool-skip perf
win) and touches the immutable-VBO registration — higher blast radius. Only if
(A) proves visually insufficient.

The night ambient/terrain-light color is already computed in
`TreeAppearance::update` (bdactor.cpp:6403-6407, `lightRGB` from
`eye->getLightRed/Green/Blue(getTerrainLight(position))`) — reuse it as the
floor source; do NOT invent a constant.

## Verification plan (NOT yet run — requires a build + the 0.4 lane free)
- Verify-by-exaggeration first (force gate strength high; confirm ridge lifts +
  impostors darken toward neighbors), then dial to plausible night shade.
- Night poses 205952 / 204907 / 204611 via MC2_CAMERA_PARK + visual_verify
  FULL-ZOOM crops: band gone (horizon ridge = plausible night shade), impostors
  matched to lit LOD0 neighbors.
- LOD zoom sweep (MC2_IMPOSTOR_DIST / MC2_FORCE_LOD): white flash gone at the pop.
- Day-pose regression: byte-identical / no darkening with gate on in daylight.
- Cinematic/movie check: letterbox black bars UNCHANGED (they are intentional and
  are NOT drawn by this path — must stay untouched).

## Residual risk / handoff notes
- **Item A (black band) draw-path caveat:** the mechanism above is the strongest
  source-grounded hypothesis and matches all batch-7/8 evidence, but the far
  *ridge* pixel program was never RenderDoc-confirmed to be the static-prop VBO
  path specifically (batch-8C's flagged NEXT step). Fix (A) should be A/B'd on
  the ridge crop directly; if the ridge does not lift while impostors do, the
  ridge is a distinct far-terrain/backdrop blit needing its own seam and the
  RenderDoc program read must be done first.
- **Wave-3 note:** deferred `73755184` (`MC2_BACKDROP_NIGHT_LIGHT`, default-ON on
  branch `claude/deferred-sweep-1`) is an A/B-verified no-op on the band —
  recommend it be **reverted or flipped default-OFF** so a dead gate doesn't mask
  the real fix. (It lives on the deferred branch, not nifty.)
- Preflight `safe_to_touch=false` was solely 3 untracked scratch dirs
  (debug_state/ dev_shell_out/ scratchpad/) — no source contention.

## Status: attribution CONFIRMED (source-grounded), fix DESIGNED, NOT implemented, NOT verified.
