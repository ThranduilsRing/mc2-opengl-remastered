# asset-io-offmain-1 continuation 1, launch 2 of 3

Assigned PC worktree slot11; box slot4. PC editing/git/read-only lint only.
Revised scope: smoke-lite (two starts) and two 4K diagnostic travel captures;
no baseline rebuild, runtime fix, or additional game starts. Limit 150 calls.
Old launch receipts remain in the parent proof directory and are not repeated.
Archived stale ORCHESTRATOR-STEER.md in the cont1 proof directory.

RECEIPT S0 PASS a271cb2588d1181728fd745e990618d8160106f7 E:/mc2-archive-2026-10/proofs/asset-io-offmain-1/cont1/merge.log

Next: PC ratchet/lint; serialized lean install; sync/build/selected units and
vk_shaders contracts. Deploy currently lacks --no-example-mods: recheck after
offline work, then BLOCKED-ON-FLAG if still absent. No mods may reach the box.
Box failure: one diagnostic only, then BLOCKED-ON-BOX. Archive receipts, report,
and release PC slot11. Keep SHA-linked evidence; local commits only.

PC ratchet PASS (exit 0); lane_merge lint PASS (exit 0, 46 s), no inherited
failure. First SSH had a duplicated root@ prefix; the single diagnostic with
the corrected address succeeded. Lean install then completed alone, exit 0:
28416 private files, disk 38%. Sync verified fb3db8b0574fc9209a2a167b3fdb049ccde0aa50.
Build session 62951 queued behind slot0 and slot1; no game starts or deploy.
The original unit filter is missing from the old PowerShell-truncated receipt;
preserve covering audio/texture source-file scope and report the actual count.

RECEIPT S1 FAIL fb3db8b0574fc9209a2a167b3fdb049ccde0aa50 E:/mc2-archive-2026-10/proofs/asset-io-offmain-1/cont1/contracts.log
Build exit 0 (1300 steps). Owning audio/texture files: 83 cases / 267375
assertions, all PASS, exit 0. Expanded scope because the prior exact 61-case
filter was truncated; do not claim the identical selection. Shader contracts
exit 1: five Slang sources report "slangc and spirv-cross required (Vulkan SDK)".
Hard stop: BLOCKED-ON-BASE. No tooling repair or second test pass attempted.
RECEIPT S2 BLOCKED fb3db8b0574fc9209a2a167b3fdb049ccde0aa50 E:/mc2-archive-2026-10/proofs/asset-io-offmain-1/cont1/flag-check.log
Secondary blocker: --no-example-mods absent at final check. No deploy/parity,
fingerprint or smoke this launch. Zero of four authorized game starts used.
RECEIPT S3 BLOCKED fb3db8b0574fc9209a2a167b3fdb049ccde0aa50 E:/mc2-archive-2026-10/proofs/asset-io-offmain-1/cont1/contracts.log
Two 4K travel captures NOT RUN; no CPU/GPU ms/frame or source/phase claim.
S4 excluded by the review override: no baseline rebuild or runtime fix.

Resume only after orchestrator supplies missing shader tools and promotes the
deploy flag. In Git Bash from A:/Games/mc2-ops/vast (box slot4):
1. Complete `ssh <BOX settings> 'bash -s -- 4' < lean_install.sh` alone first.
2. `bash slot_sync.sh 4 <continuation-tip> A:/Games/mc2-slot11`
3. `bash slot.sh 4 build`
4. `bash slot.sh 4 unit '--source-file=*test_audio_backend.cpp,*test_texture_sources.cpp,*test_texture_decode.cpp,*test_texture_service.cpp'`
5. `bash slot.sh 4 bash scripts/check-contracts.sh --rows vk_shaders --serial`
6. Only if green and flag exists: `bash slot.sh 4 python3 scripts/deploy_payload.py /workspace/slot4/install --ninja --exe-name mc2 --allow-new-target --classic-assets --no-example-mods`
7. Verify v04-assets.sha256 first, extract /workspace/v04-assets.tar only on
   mismatch, reverify. Then `bash slot.sh 4 --game python3 scripts/run_smoke.py --lite --keep-logs --exe /workspace/slot4/install/mc2`.
8. Two separate runs: `bash slot.sh 4 --game env TRACY_TOOLS_DIR=/workspace/toolchains/tracy python3 tools/tracy_capture_compare.py capture --install /workspace/slot4/install --mission mc2_10 --seconds 20 --free-timestep --fps-cap 0 --pan --env MC2_WIDTH=3840 --env MC2_HEIGHT=2160 --env MC2_VK_DEVICE=NVIDIA --csv -o /workspace/slot4/proofs/<fresh-run>/t.tracy`.
Export CPU/GPU unwrap CSV separately, list every Main asset event over 5 ms
with source/phase, divide summed exec ns by complete Frame count / 1e6.
No new timing evidence supersedes launch 1. No final-tip executable proof is
claimed for the later documentation-only handback commit.

Archive: cont1/RESULT.html and SHA-linked receipts. Crash scan empty; disk 42%.
Cleanup planned after archive: mark remote run .pulled, remove only slot4
build64-ninja/build64-tests/install/tmp, retain source and proof marker, release
PC slot11. No PC scratch, copied executable, or game process created.
