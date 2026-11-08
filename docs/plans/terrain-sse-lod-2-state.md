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
- Both candidate fixes rejected by inspected crops; final runtime code restored exactly to 041ebf919. This follow-up is NOT merge-ready: the original blob remains.
- A/B: `ab.py --compare-only <proof>/tip <proof>/fix --crop 0.18,0.16,0.34,0.23 --crop 0.18,0.35,0.34,0.43 --out-root <proof>/tip-vs-fix`; both arms fully built/deployed from slot02, same private install, no exe/shader mixing.
- Tool slowness: inherited warm build 785 s; verified seed ~26 min; initial deploy ~25 min; scout timed out at 420 s. Host captures also waited behind another lane's mod smoke; no foreign process killed or lease guard bypassed.
- Final receipts/results go in the proof folder, keeping the proved-at tip unchanged after validation.
- Candidate 647eb0931: compiled and captured all three poses; blob unchanged. REJECTED; spline clamp removed.
- Prop-only diagnostic fc2fcfdd1 removes exactly the blob in both day poses: terrain self-shadow is responsible. Full bake and original evaluator restored.
- Candidate 3f7e06d49: static-shadow underwater fade samples fine bake height instead of drawn LOD Z. Built, fully deployed, three-pose A/B in receiver-fix/ and tip-vs-receiver-fix/; both inspected blob crops unchanged. REJECTED and reverted.
- Exact next steps: inspect terrain-march inputs at the blob world position (sun vector, receiver height, occluding height/distance). Distinguish a real distant occluder from self-intersection/height-bake features before changing bias or softness. The static mask XY lookup is responsible even at forced LOD0; AO, movers, wet band and underwater receiver fade are excluded. Do not repeat those checks or ship a generic shadow attenuation workaround.
- Final checks and cleanup receipts are recorded in external RESULTS.md; requested corrected-tip A/B is NOT fulfilled, because no successful fix exists yet. All temporary prop-only/evaluator edits are gone.
- Proof root: E:/mc2-archive-2026-10/proofs/terrain-sse-lod-2/blob-followup; private install: E:/mc2-installs/terrain-sse-lod-2.
