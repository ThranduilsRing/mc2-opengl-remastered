# Animated shoreline

Assignment: A:/Games/mc2-slot11, claude/shoreline-animated, base d63c403c9715b6a664a3513b445430e722c94c10.

## Design

The base already samples a bicubic cooked shore field for coverage and tint,
recovers horizontal shore distance with screen derivatives, and draws a ring
whose reach oscillates when the water quality permits. The existing ring has
no independent scrolling breakup, no wash alpha or normal response, and uses
the coverage field rather than the actual 4x visual bake for its depth.

Use the terrain visual-height SSBO through water's reduced lighting set (set 1,
binding 10). Water replaces the unused cement-word binding 4 with the visual
bake binding 10, keeping the four-storage-buffer Vulkan floor and leaving other
lighting consumers' descriptor sets unchanged. Bilinearly sample its 32 wu grid.
Use water plane minus bake height for the animation; retain the cooked field's
coverage and missing-bake fallback. Fade foam to zero above the physical plane
and in deep water. Keep all derivatives ahead of coverage discard.

A separate shader include owns a five-second advancing/retreating broken crest,
a weaker trailing line, and a shore-side wash. Scroll one procedural value-noise
mask in world space. Drive every term from the existing frame-time uniform;
no persistent state or simulation changes. Apply the wash normal to diffuse
lighting independently of the reflection normal. Wash alpha stays multiplied
by existing shore coverage. Foam receives the water's vertex lighting, night
factor, static and mover shadows, local illumination, and fog; no emissive floor.

Finding 15: the existing vertex depth lift can cover above-water rocks. Preserve
the other lane's vertex height/lift code; ensure this material tests at its actual
WorldPos surface depth in the fragment stage. Do not edit sky fallback, reflection
sampling, water_reflection.comp, or the shore-lift height calculation.

Cost: four cached SSBO height reads and one procedural noise octave per fragment,
plus a few scalar wave/coverage operations; no extra draw, texture, render target,
dispatch, descriptor allocation, or CPU work per shoreline. Physical fragment
depth replaces the biased depth test and can reduce early-depth efficiency.
No timing claim.

## Proof and current state

Inspection/design and implementation complete. The first AMD capture rejected
the missing bake binding in water's reduced lighting set; the corrected layout
replaces water's unused cement descriptor and has a covering descriptor-plan test.
All 79 Vulkan shaders compile locally. Next: final-tip production build,
shader contract rows, covering doctest, box build/unit/lint/smoke-lite, day low shore
and rock plus night A/B sequences (three frames per pose), montage inspection,
and one final Windows Ninja build of mc2 and mc2_asset_viewer. Commit locally only.
Use existing water-shore-lift vehicles_north_low/reverse poses and mc2_10 water
poses. Proofs live under E:/mc2-archive-2026-10/proofs/shoreline-animated.
