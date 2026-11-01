# Light contract 2 (clean base) running note

Branch claude/light-contract2, base nifty 946818e33. Goal: the prop light contract as the ONLY change, then
prove pixel identity vs the e5cf6ba9c exe.

- Port: 30 files from claude/light-contract, contract only. Left out: bake latch, INERT classification, objmgr_misc,
  appear.h, present-active wakes, invalidation generation, oracle, all docs of p1r.
- Taken wholesale (contract-only vs nifty): UBO + instance ABI, color_prop_tier1.vert, camera, gamecam,
  bdactor_lightbake (mc2PropSlotI + LIGHT_CONTRACT check), batcher.h/submit/seam/shadow, prop_light_contract.h + unit test.
- Hand-merged: registry (staticPropSetActorTint), island (UBO copy), live.h (feed struct), bdactor_render/static/tree/tree_static/update,
  logparse (strict rules), test_gpu_abi_gen (368), tests/unit CMake.
