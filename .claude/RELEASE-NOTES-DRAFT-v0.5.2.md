# MC2 OpenGL v0.5.2 — release notes

Patch release over v0.5.1. Folds in the final deferred-sweep-1 batch
(shadow-quality + spotlight-cone fixes) plus the nifty session fixes that
landed after the v0.5.1 cut. No gameplay or data changes — engine/shaders only.

## World rendering
- **Shadow distance-fade, seamless (SHADOW-FADE-RAMP0-1, default ON)** — tree
  and object shadows now fade continuously from the contact point instead of
  through a windowed onset that drew a visible crisp/soft seam. `MC2_SHADOW_FADE_RAMP0=0`
  restores the old windowed onset.
- **Sun-altitude-scaled shadow softness (SHADOW-ALT-SCALE-1, default ON)** —
  grazing (low) sun keeps the aggressive soft dissolve; high sun renders
  mostly-crisp with tip softening only. `MC2_SHADOW_ALT_SCALE=0` restores the
  fixed low-sun gains at all altitudes.
- **Spotlight ghost-wedge removal (GHOST-CONE-RECIPE-SKIP-1, default ON)** —
  spotlight cone meshes are no longer baked into static prop recipes and their
  SpotLight_ TYPE geometry registers empty, so the opaque tan/cream 3D ghost
  wedges over roads/compounds are gone (the additive light glow is unaffected).
  Killswitch `MC2_STATICPROP_RECIPE_SPOTLIGHT_SKIP=0` restores the old bake.
- Carried from the post-0.5.1 nifty line (default ON): two-sided beacon/runway
  edge-light glow cards (`MC2_LIGHT_BILLBOARD_TWOSIDED`) and grazing-angle water
  sky reflection (WATER-GRAZE-REFL-1). These are why the v0.5.1 visual golden no
  longer byte-matches — see the golden re-bless note in MERGE-WAVE-3-LOG.md.

## Fixtures / tooling (modder- and dev-facing)
- New diagnostics from the batch: `MC2_LIGHT_CONE_DIAG`, `MC2_LIGHT_CONE_AIM_FLIP`
  (cone-aim A/B probe), cone-leaf-name invariant. All default OFF / trace-only.

## Not in this release
- **FAR-BACKDROP-NIGHT-LIGHTING-1** (deferred `73755184`, gate
  `MC2_BACKDROP_NIGHT_LIGHT`) — reverted during the WAVE-3 merge: it was an
  A/B-verified no-op once `MC2_NIGHT_FOG_DIM` (v0.5.1) already lifts the far
  horizon above its proposed floor. Removed to keep the default byte-identical
  and avoid a redundant gate. See MERGE-WAVE-3-LOG.md §2.
- Vulkan backend is not compiled into this build (islands fail-soft to GL).

## Provenance
- Built at commit `fa3a5304` (claude/nifty-mendeleev), clean tree.
- Tier1 smoke 5/5, contracts 65/65, Tracy GL baseline 91.8 fps @ mc2_10 canonical
  pose (release_assets/0.5.2/tracy/).
