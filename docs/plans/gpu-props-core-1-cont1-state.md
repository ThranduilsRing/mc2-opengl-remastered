# gpu-props-core-1 continuation, launch 2

Assigned branch claude/gpu-props-core-1; box slot 1; PC editing/git/lint only.
M = da78fd2e27e963307046fbbb210a75123c5e0ebb (clean merge of play).
RECEIPT S0 PASS da78fd2e27e963307046fbbb210a75123c5e0ebb

Proof root: E:/mc2-archive-2026-10/proofs/gpu-props-core-1/cont1/.
PC cxx17 and lint PASS at M. Serialized LEAN install PASS, disk 36%.
Sync M PASS; build M PASS with explicit exit 0. Prop-cache unit PASS:
6 cases, 245 assertions. Pipeline-layout row PASS (10 tests).
Shader row initially lacked Slang on PATH; only that failed row was retried
with /workspace/toolchains/slang/bin prepended: 79 shaders and 9 tests PASS.
RECEIPT S1 PASS da78fd2e27e963307046fbbb210a75123c5e0ebb

Required --no-example-mods is absent from scripts/deploy_payload.py at M.
No deployment, baseline revert, capture, pixel verdict, or smoke has run.
Candidate e3a779752 remains UNACCEPTED; prior timing is not current proof.
Do not bypass the no-mods requirement or report an older exe as final proof.
RECEIPT S2 BLOCKED da78fd2e27e963307046fbbb210a75123c5e0ebb BLOCKED-ON-FLAG
RECEIPT S3 BLOCKED da78fd2e27e963307046fbbb210a75123c5e0ebb prerequisite S2
RECEIPT S4 BLOCKED da78fd2e27e963307046fbbb210a75123c5e0ebb no pixel verdict
RECEIPT S5 BLOCKED da78fd2e27e963307046fbbb210a75123c5e0ebb no deploy/smoke
Final box source clean at M; disk42%; no crash files or proof directories.
No .pulled marker is owed: all logs were streamed directly to the PC archive.
Retain slot1 build/install/tmp/source for the third launch: proof still owed;
no box captures or copied baseline executable exist. No files deleted manually.
Release PC slot10 after this documentation commit; record release in archive.
No current CPU/GPU timing, pixel identity, or runtime success is claimed.

Next: finish S1; if flag remains absent, BLOCKED-ON-FLAG. A subsequent
authorized play merge must supply it before S2. Use SLOTS.md commands:
lean_install.sh 1 (already complete); slot_sync.sh 1 <commit> A:/Games/mc2-slot10;
slot.sh 1 build; slot.sh 1 unit '--test-case=prop_record_cache:*'; scoped
vk_shaders_compile/vk_pipeline_layout contracts. Deploy only with --exe-name mc2
--no-example-mods --classic-assets, then verify v0.4 checksums before capture.
S2 revert e3a779752, build/deploy exact R, baseline captures and save exe/shaders.
S3 revert R; require empty git diff M HEAD -- shaders GameOS; repeat proofs.
S4 exact pixel identity for all 12 owner day poses plus night; otherwise revert.
S5 exact-tip offline checks/build/deploy/smoke, archive embedded RESULT.html.
Tracy CPU/GPU separately: summed zone ns / complete CPU Frame count / 1e6.
Cleanup only own files after archival; release slot10; no push.
