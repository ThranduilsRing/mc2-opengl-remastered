<!-- doc-status: current | reviewed:2026-10-11 -->
# HZB / Reverse-Z Depth Convention

Status: **convention locked** (HZB-DEPTH-CONVENTION-TESTS-1). No runtime HZB
exists yet. This doc + `tests/unit/test_depth_hzb.cpp` are the contract the
future runtime HZB slice (`TRACKRV-HZB-VISIBILITY-OPUS-1`) must match.

## Depth regime (recon-verified)

| Property            | Value                                  | Evidence |
|---------------------|----------------------------------------|----------|
| Depth direction     | **Reverse-Z**: near→1.0, far→0.0       | `gameosmain.cpp` `glClearDepth(0.0f)`; `gameos_graphics.cpp` `glDepthFunc(GL_GREATER)`; `appear.h` `HUD_DEPTH 0.9999f` |
| Clip / NDC depth    | **ZERO_TO_ONE** (mandatory, fail-closed) | `gameosmain.cpp` `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` |
| Closer means        | **Larger** depth value                 | `terrain_depth_bias.h` "GEQUAL: the LARGER NDC z wins" |
| Sky / far sentinel  | depth **== 0.0** (clear value)         | `ssao.frag`, `shadow_screen.frag`, `particle_billboard.frag` all guard `depth >= 1.0`==near, far==0 |

Depth-reconstruction shaders (`ssao.frag`, `shadow_screen.frag`,
`particle_billboard.frag`) all consume window depth **raw** through
`inverseViewProj * vec4(uv*2-1, depth, 1)` — no linearization helper exists,
and all three agree on the reverse-Z ordering. No inconsistency found.

## HZB reduction convention: **MIN pyramid** (store the FARTHEST occluder)

The scene depth buffer holds the nearest surface per pixel = the *largest*
reverse-Z value. A coarse HZB texel must store the **minimum** child depth =
the farthest-back (weakest) occluder in the tile, so the conservative cull
test never hides a visible object:

```
hzbTexel       = min over the tile's child depths          // farthest occluder
objClosestDepth = max reverse-Z depth over object footprint // object's nearest point
cull  iff  objClosestDepth < hzbTexel
```

Forward-Z (near=0/far=1) stores **MAX**. Reverse-Z **flips this to MIN**.
Storing MAX here would keep the *nearest* occluder and over-cull objects that
are visible behind a thin foreground sliver → intermittent invisible-object
bugs. That inversion is the trap this convention guards against.

## Mip dimension ladder: round **UP** (ceil), min-reduce, custom pass

- Each level halves each axis independently, **ceil**, floored at 1:
  `next = max(1, (d + 1) / 2)`. Examples: `8→4→2→1`, `7→4→2→1`, `5→3→2→1`,
  `1→1` (terminal, idempotent).
- Round-up guarantees the coarse level fully covers the finer one with a
  clamped 2×2 fetch — **no source texel dropped** on an odd extent, no per-level
  3×3 edge taps.
- `glGenerateMipmap` is **unusable**: it (a) *averages* (HZB needs `min`) and
  (b) *floors* the size. The runtime HZB needs a **custom reduction pass**
  (compute or full-screen) implementing the above.

## What the runtime HZB MUST honor

1. Reverse-Z ordering (larger = closer); far/sky = 0.0.
2. MIN reduction (farthest occluder), not MAX.
3. Cull test `objClosestDepth < hzbTexel`.
4. Ceil mip sizing + clamped 2×2 fetch; terminal 1×1 stable.

Any runtime deviation should update `tests/unit/test_depth_hzb.cpp` in the same
change, with the re-derived rationale.
