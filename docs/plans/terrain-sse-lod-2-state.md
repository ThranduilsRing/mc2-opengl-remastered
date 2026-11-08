# terrain-sse-lod-2 state (lane 2, Astra review p35)

Terrain chunk LOD is chosen by screen-space error, not distance. `MC2_TERRAIN_LOD_CHUNK_DIST_SCALE` and the distance path are deleted.

- Error: `mc2lod::levelErrors` / `edgeErrors` (mclib/terrain_lod_rules.h) per block from the 4x visual bake when loaded, else coarse heights; cached per height version.
- Pick: `chooseSseLevel`, 1 px (0.25 px concrete), coarsen below 0.9 x tol, live FOV/viewport (terrain_update.cpp `chooseProjectedLod`).
- Morph: per-block weight toward level+1, capped so mix(L, L+1) error stays within tol; per-corner weights (`cornerMorph`: 0 next to culled/off-map/other-level blocks, else the shared min) packed in the patch `morph` word; VS `terrainMorphH`.
- Stitch error is in the bound: a pair whose lcm-stride shared edge projects over tol refines the coarser block (`stitchErrorLevel`).
- Shader stitch now clamps the partial-edge endpoint like `mc2lod::edgeHeight` (Astra finding 1).
- Proof: tests/unit/test_terrain_lod_rules.cpp (shader-equivalent shared-edge agreement over random levels/visibility/morph, 4 partial sizes, both orientations, morph sweep).
- Unchanged: RT terrain tiles (fixed LOD), picking/LOS (render only), GL path (VK-only engine).
- Residuals: shadow pass not morphed; level skips (>1 level in a frame) or a delta-clamp release can still pop; mode-2 errors ignore the edge fade and displace-far < 1.

## Shore blob follow-up (2026-11-08)
- Assignment: slot02, claude/terrain-sse-lod-2, starting tip 041ebf919 (base 609d07294).
- Done: clean tip Ninja build/deploy; three deterministic baseline captures; two day captures each for forced LOD0, contact AO off, shadows off, shore off, static-only (debug 3) and mover-only (debug 4).
- Diagnosis: static-only contains the blob; mover-only does not. Shore/AO off do not remove it. Forced LOD0 clears morph and still contains the blob (LOD0/AO scene MAD 0.0032/0.0041). This contradicts the brief's static-shadow exclusion: the fresh bake artifact is not limited to coarse geometry.
- Candidate fix: bound the static shadow bake's Catmull-Rom surface to its cell's four actual bake heights, preventing invented shoreline pits/ridges; evaluator 2 invalidates old cooked masks. No gate or LOD feature removal.
- Next: build and capture the candidate; inspect saved_205644 crop0 and owner_lowwide crop1 before calling it fixed. Then covering shader rows, terrain LOD + shadow mask doctests, ratchet, ONE final lint and smoke-lite.
- A/B: `ab.py --compare-only <proof>/tip <proof>/fix --crop 0.18,0.16,0.34,0.23 --crop 0.18,0.35,0.34,0.43 --out-root <proof>/tip-vs-fix`; both arms fully built/deployed from slot02, same private install, no exe/shader mixing.
- Tool slowness: inherited warm build 785 s; verified seed ~26 min; initial deploy ~25 min; scout timed out at 420 s. Host captures also waited behind another lane's mod smoke; no foreign process killed or lease guard bypassed.
- Final receipts/results go in the proof folder, keeping the proved-at tip unchanged after validation.
- Candidate 647eb0931: compiled and captured all three poses; blob unchanged. REJECTED; spline clamp removed.
- CURRENT CODE IS DIAGNOSTIC ONLY: evaluator 3, resolve outputs prop visibility without terrain self-shadow. Next: capture two day poses with debug mode 3, identify prop vs terrain contribution, RESTORE normal min(props, terrain) and evaluator before final proof/hand-back. Do not merge this diagnostic state.
- Proof root: E:/mc2-archive-2026-10/proofs/terrain-sse-lod-2/blob-followup; private install: E:/mc2-installs/terrain-sse-lod-2.
