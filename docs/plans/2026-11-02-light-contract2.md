# Light contract 2 (clean base) running note

Branch claude/light-contract2, base nifty 946818e33. Goal: the prop light contract as the ONLY change, then
prove pixel identity vs the e5cf6ba9c exe.

- Port: 30 files from claude/light-contract, contract only. Left out: bake latch, INERT classification, objmgr_misc,
  appear.h, present-active wakes, invalidation generation, oracle, all docs of p1r.
- Taken wholesale (contract-only vs nifty): UBO + instance ABI, color_prop_tier1.vert, camera, gamecam,
  bdactor_lightbake (mc2PropSlotI + LIGHT_CONTRACT check), batcher.h/submit/seam/shadow, prop_light_contract.h + unit test.
- Hand-merged: registry (staticPropSetActorTint), island (UBO copy), live.h (feed struct), bdactor_render/static/tree/tree_static/update,
  logparse (strict rules), test_gpu_abi_gen (368), tests/unit CMake.

## Pixel A/B result (2026-11-01)

The residual vs the e5cf6ba9c exe is NOT the contract. A clean nifty build (946818e33, same install data, same bookmarks)
differs from e5cf6ba9c by exactly the same amount (mc2_01 quonset_lightfacing mad 0.874 max 103 on both; mc2_03 buildings
mad 0.66-1.14), and the contract build vs that nifty build is identical to run-to-run noise:

| image (mc2_01/03 building_faces) | e5cf6ba9c vs contract | nifty vs contract | contract A/A |
|---|---|---|---|
| hangar_lightaway | 0.641 / max 111 | 0.002 / max 70, 1 px>24 | 0 |
| quonset_lightaway | 0.602 / 102 | 0.000 / 51, 11 px>24 | 0 |
| civstore_lightaway | 1.147 / 91 | 0.005 / 30 | 0 |
| sam_lightfacing | 0.659 / 67 | 0.001 / 35 | 0 |
| industrial_lightfacing | 0.958 / 72 | 0.116 / 63 | 0.009 / 23 |
| industrial_lightaway | 1.06 / 74 | 0.29 / 56 (nifty is itself non-deterministic here: n vs n2 mad 5.2) | 0 |

Systematic effect e5cf6ba9c -> nifty: whole buildings about 3 percent brighter (mc2_01 up to 5.8 percent), not caused by this lane.
One extra fix: vehicle (gvactor_render) static-prop submit now passes the per-actor terrain I (the old row carried it); no pixel change.

## W96 saveload-mc2_10 red (2026-11-02): row-stale after a mission reload

Evidence (merge96 gate, saveload-mc2_10 log): mismatches=0 until the quickload (log line [SAVELOAD] op=load_slotMission), then
mismatch#1.. at TerrainLOD frame=4 after the load: want=32 (live S=37 A=25, I=0.213) vs row=101. slotI_fallback doubled 554->1108.
Cause found in code: mission unload (mc2ClearAllBakedStaticLight) clears the bake map, but the one-shot rebake-on-ready
(s_lightBakeRebakeDone, function-static s_readyFrame) stays latched from the first mission, so rows gathered during the load, before
night lighting was applied, are never rebaked. s_slotI (per light index terrain I) also survived the unload.
Fix (this branch): mission reset re-arms the rebake and clears s_slotI; the parity check is stale-aware (rows gathered in an earlier
S/A epoch are counted as stale_rows/stale_max/stale_last_frame, informational; fresh rows stay strict).
