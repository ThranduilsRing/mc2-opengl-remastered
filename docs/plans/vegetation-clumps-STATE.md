# Vegetation clumps — 2026-11-08

Owner: "It is sparse and ugly. We need: variety, some bushes and small trees,
and more clumping. Like small tree plus some bushes plus some grass all on top
of one another and not all spread out."

Base d63c403c9715b6a664a3513b445430e722c94c10; slot04, claude/vegetation-clumps.
Hypothesis: grass-only palette and uniform jitter hide vertical layering.
Offline proof: VegClumps doctests (seed, blue-noise separation, layers, masks,
recipe validation), veg-card cook pytest, deployment payload tests.
Implemented: bounded blue-noise clump centres, per-map integer seed, layered
saplings/2–5 bushes/grass skirt, moisture-like noise, slope and static exclusion
masks; four procedural woody cards, per-biome JSON recipes, payload wiring.
Existing instancing, distance fade and texture-array layout are retained.
Sample inspected: E:/mc2-archive-2026-10/proofs/vegetation-clumps/woody-sample-sheet.png;
revised from open twig silhouettes to fuller crowns. In-game appearance unproven.
Cost by construction: same eight 512x512 BC7 layers and ten mips (~2.67 MiB);
16-byte persistent instances, two eight-vertex strips (12 triangles) per survivor,
same population/visible caps. Larger crowns may increase overdraw: NOT measured.
Receipts live in E:/mc2-archive-2026-10/proofs/vegetation-clumps/.
Next: finish native placement tests, selected shader/art/mod contract rows,
Windows Ninja compile (both targets), final lint/precheck; release slot.
Targeted Python runner rejected shortened module names (unknown module); no
tests ran in that invocation. Do not count it as a green receipt.
Tool gap: this base's box_run.py hardcodes -j16; JOBS is 6. Do not run it above
the owner limit or change unrelated runner infrastructure.
Continuation prerequisite: a runner supporting JOBS=6, then box build/unit/
smoke-lite; final-tip data/shader deploy (box runner here copies only shaders,
not changed vegetation data). Obtain A/B captures of mc2_01 meadow, mc2_02 snow
road, mc2_10 night, inspect montages, and measure dense-pose GPU cost there.
No runtime, capture, measured cost, or GTX 1050 Ti performance claim is proven.
