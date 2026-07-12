# Graphics 4-Item Plans (spec → adversarial+greybeard → plan → adversarial+greybeard)

Generated 2026-06-15 via workflow wf_367b694b-211 (28 agents). Scopes: CSM=2-cascade-first,
Skybox=enable+sun-sync, Water=sky-reflection+shine only, Fog=OOB-edge only.
NOT YET IMPLEMENTED — awaiting user review of the open decisions in each item's section (F).

---

## Item 1 — Cascaded Shadow Maps (2-cascade first)

**Summary:** `MC2_SHADOW_CSM`-gated (default ON since 2026-06-18) N-cascade (default 2, clamp 1–3) dynamic shadow
path. Reuse the existing light-view × light-ortho fold per cascade, replace the dynamic depth map
with a `GL_DEPTH_COMPONENT24` `TEXTURE_2D_ARRAY`, and push ALL variant logic inside a
signature-frozen `calcDynamicShadow` so no shader call site changes.

**Phases:** P0 FBO (depth-only conversion DEFERRED — dummy color attachment is a 7900 XTX
completeness workaround, not free) → P1 2D_ARRAY resource + per-layer caster loop (N=1) with full
sampler params (COMPARE_REF_TO_TEXTURE, **GL_LEQUAL**, CLAMP_TO_BORDER, border 1.0) → P2 N matrices
(ONE basis/ONE sun, per-axis AABB + translate column, per-cascade texel snap) → P3 shader variant
inside frozen `calcDynamicShadow` (`#ifdef MC2_SHADOW_CSM` swaps sampler2DShadow→sampler2DArrayShadow;
7 consumer shaders compile both variants) → P4 inline-vs-deferred select-parity gate (BLOCKING) →
P5 debug tints + array-blit fix + far-cascade non-vacuous assert.

**Load-bearing invariants:** COMPARE_FUNC is GL_LEQUAL (not GL_LESS); one basis/one sun, z-row
identical across cascades; `calcDynamicShadow` signature frozen.

**Negative space / meta-fixes:** (1) META-FIX: freeze `calcDynamicShadow` signature — `shadow.hglsl`
is `#include`d by **7** shaders (terrain×2, screen, decal, grass, overlay, tex_vertex_lighted), not
the ~5 the naive plan assumed; call-site `#ifdef` would silently give wrong shadows on grass/decal/
overlay. (2) P0 depth-only FBO is NOT free (driver-fault slips past byte-identical gate). (3) array
needs full sampler param set + border color. (4) debug blit at :1954 reads 2D — won't show an array.
(5) 4+ bind sites use GL_TEXTURE_2D — must flip to ARRAY. (6) off-center cascades need a translate
column. (7) N=1 ≠ byte-identical (array sampler ≠ 2D sampler).

**Open decisions:** F5 N=1 diff tolerance (nonzero tolerance on `=1`, strict on `=0`?); split scheme
(λ=0.5 default?); CSM→static handoff overlap at ~8000 WU; P0 fate; N default 2 vs 1-then-widen.

---

## Item 2 — HDRI Skybox (enable + sun-sync)

**Summary:** Enable the existing `MC2_HDRI_SKY` equirect skybox, prove orientation upright/correct-
handed with a debug gradient BEFORE baking sun math, then sync its yaw to the terrain sun via an
empirically-scanned bake azimuth. Orientation eyeball is the immutable gate.

**Phases:** P0 Diagnose (NO code) — get rc1 stderr, walk the gate. **Corrected: gamecam:319 is
`if (Environment.Renderer != 3)` — sky runs when renderer is NOT 3.** Decide env_gate vs init_failed
vs path_empty (cwd-relative path may resolve against deploy dir, not worktree) → P1 (GATE) prove
orientation via `MC2_HDRI_SKY_UV_DEBUG` gradient + log worldDir → P2 sun-sync yaw: luminance-weighted
sun-centroid scan of EXR at load, `skyYaw = sunAz − bakedAz` per frame; **the real risk is a FRAME
mismatch (MC2 raw vs GL equirect), not a sign** — convert sunAz through the same swap worldToClipGL
bakes → P3 next-pass state probe (draw-buffer/FBO/attachment-count).

**Load-bearing invariants:** Phase 1 upright eyeball gates everything ("drawn" log proves only the
code path ran, not pixels). invViewRot transpose × axis-swap interaction is THE split-brain trap —
verify worldDir is genuinely GL Y-up before trusting azimuth.

**Negative space / meta-fixes:** inverted renderer gate (`!=3`); frame-mismatch ≠ sign error
(AZ_OFFSET only trims a constant); invViewRot (not the v-flip) is the real split-brain; "drawn" log
is false confidence; brightest-texel fragile → centroid; empirical bake-az survives asset swaps.
META-FIX: collapse two eyeball rounds into one (gradient + worldDir log together).

**Open decisions:** F1 paste rc1 stderr + confirm options.cfg MC2_HDRI_SKY value + launch dir;
editor scope (does EditRel hit this path?); keep AZ_OFFSET as permanent trim knob or remove; confirm
canonical EXR is in the DEPLOY dir.

---

## Item 3 — Water (sky reflection + shine only)

**Summary:** Give MDI water a real analytic wave normal feeding sky-reflection + a sun-specular lobe,
uploading `terrainLightDir` **RAW MC2 (no swap)** exactly like the terrain path — behind strength
gates that hold Baseline-A byte-identical at zero.

**Phases:** P0 baseline + lightweight probe (is lightDir length>eps? does it move? which tier1
missions are water-bearing AND sun-lit) → P1 upload terrainLightDir RAW to water MDI (uniform bound
but unused = byte-identical) → P2a analytic wave normal, KEEP empirical skyDir byte-for-byte →
P2b re-derive skyDir cleanly (SEPARATE, orbit-gated A/B) → P3 sun-specular Blinn-Phong lobe (port
math from dead :317 lobe but NOT its hardcoded light; length-guard) → P4 real normal to GBuffer1
(gated behind strength>0; flat (0,0,1) at zero preserves Baseline-A) → P5 tune defaults.

**Load-bearing invariants:** LBI-1 terrainLightDir uploaded RAW MC2 (the camMC2 swap at :3087 exists
only because terrain_camera_pos_ is Stuff-frame; light dir is already raw — applying the swap double-
transforms). LBI-2 wave normal/vdir/specular all in MC2 Z-up; keep separate from the SH skyDir basis.

**Negative space / meta-fixes:** M1 ROOT-FIX: the draft's "apply camMC2 swap to lightDir" was
INVERTED — upload RAW, mirroring terrain verbatim; deletes the heavy frame-truth probe as a solution
to a non-problem. M3 split normal-source change (2a) from skyDir re-derivation (2b) so a failed orbit
test bisects. N1 `u_waterScreenSize` survives (still drives RT UV). N3 dead lobe carries a hardcoded
light — port math, not light. N5 vacuous-counter trap (idle fly-throughs spawn no FX/sun).

**Open decisions:** specIntensity ship default (0.0-then-chosen, e.g. 0.6?); Phase 2b adoption if
clean derivation disagrees with empirical :207 flip; editor water (length-guarded-black acceptable?);
accept Baseline-A byte-identical only at 0/0; Blinn-Phong now / GGX deferred.

---

## Item 4 — OOB Terrain-Edge Fog (chunk-path parity)

**Summary:** Port the legacy terrain edge-haze (grey-blue fade past map boundary,
`gos_terrain.frag:951-956`) onto the production default-on chunk frag, compositing into `lit` before
the final MRT write, via a shared PREC-free angle-bracket-included helper, gated by `u_halfMap > 0.0`.
No depth/blend/GBuffer changes. Preserves the long Wolfman interior view.

**Phases:** P0 (refactor-only) NEW `shaders/include/edge_haze.hglsl` with `EDGE_HAZE_SKY=(0.58,0.65,
0.75)` + `edgeHazeAmount(worldXY,halfMap)` smoothstep; refactor legacy frag to use it (algebraic
identity); leave the height-fog literal alone → P1 `#include <include/edge_haze.hglsl>` (angle
brackets, after the `#define PREC` line) into terrain_lod_chunk.frag; `mix(lit, EDGE_HAZE_SKY,
edgeHazeAmount(v_worldPos.xy, u_halfMap))` after :510 before the MRT write; gated `u_halfMap>0`.

**Load-bearing invariants:** include must be `<...>` not `"..."`; header must be PREC-free
(`#define PREC` is at :12, after includes); v_worldPos must be raw MC2 pre-swap (Chebyshev is sign-
agnostic so X-mirror can't bite magnitude — only because neither side touches the GL clip swap);
color-into-`lit` only, no GL-state perturbation (landmine f375e0ba).

**Negative space / meta-fixes:** wrong include syntax caught; PREC ordering trap caught; height-fog
explicitly NOT ported (scope); headless can't see the edge ring (four-edge symmetry visual capture is
the real gate); constant already duplicated 3× → shared (edge-haze only, no height-fog fold).

**Open decisions:** Phase 0 in (kills 3× constant drift, minimal risk) or out (copy 8 lines, zero
legacy change)? — recommendation IN-but-minimal. Confirm legacy `mapHalfExtent` == chunk `s_halfMap`
magnitude. Accept interior height-fog divergence between legacy and chunk (edge-ring parity only).
