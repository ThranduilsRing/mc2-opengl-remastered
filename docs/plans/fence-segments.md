# Fence segments — lane state

Assignment: slot12, claude/fence-segments, requested base f269b312d.
Initial HEAD 8fc0c6c89 includes the two required Linux build fixes above base.
Budget: <=150 tool calls; inspect ~30, implement ~50, proof ~40, hand-back ~20.

Root: color_prop_tier1.frag wrapped UVs AFTER scaling into a zero-padded texture
array subregion. Stock type 440 wirefence repeats U twice; type 441 wirefencediag
repeats U three times. Later repeats sampled transparent padding. Objects exist;
the fragment sampling/coverage stage drops parts of their meshes. HZB-off capture
reproduces the gaps. Fix both alpha groups: fract(vUv) * vUvScale; continuous
derivatives retain correct mip choice (textureGrad on the alpha-OFF branch).
Blame identifies 985ddc79bbb870276a5184ebb730fb41b5ac1cda as the wrap-order change.

Mission record IDs (zero-based packet indices, not runtime handles) and meshes
are archived under E:/mc2-archive-2026-10/proofs/fence-segments:
mc2_01: 67 fences (440:4, 441:63), front diagonal records 139,141..154;
mc2_02: 50 (440:47,441:3), airfield straight records 137 onward;
mc2_03: 11 type 440, records 412 onward. Shared shader fix covers all types/maps.
Current saved-view, close-ups and HZB-off captures reproduce the problem.

Golden comparison BLOCKED: isolated lean golden exe d662802367d8 is refused by
exe_guard because it predates MC2_WINDOW_OFFSCREEN/f0ea013bd. Never bypass this
guard. Golden itself was never launched or modified. Do not claim a golden A/B
or bisect. Temporary tree .golden-capture must be deleted after crash inspection.

Initial compiled-SPIR-V tests: 5 passed (10.53s). Added fault-injection variant
to reject scale-before-wrap on both array branches. Final receipts go into the
proof folder (not another commit, so tested SHA remains the tip).
Next: commit; Windows Ninja compile; box build/unit/smoke-lite/lint; shader
contracts including the regression test; covering shader_contract_harness;
mc2_01 and mc2_02 before/after montages; cleanup lane scratch and golden clone.
Existing lane install E:/mc2-installs/pc-lane-12 is retained for the orchestrator.
