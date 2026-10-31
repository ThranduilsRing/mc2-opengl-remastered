Assignment: A:/Games/mc2-lane9, claude/cxx23-packet-b, base 32f03ddbe.
W94 c0eba1a0a merged as e16169570; both status sections retained.
Flip restored as 917daf779, registry retains ABLE and adds OBJS (16 entries).
Migration restored/composed as 98d54f460. Four independently verified G
archives plus verified F archives; only objects and section union changed.
Mask run bytes 30 -> 0, build 0 -> 0; no SIMID changes or live repin.
Proof root: E:/mc2-archive-2026-10/proofs/lane9/cxx23-f (f11-*).
Migration receipt green: 01=9e8aa1d7ecf718c9, 03=da539bb964a2ef94,
10=d9889ccfda30e9b8, 24=e97eb99f18e4f796.
Candidate clean HEAD: ea698ee83. Contracts PASS (6s); unit PASS (69 cases,
3041 assertions, 57s); game and asset viewer build PASS (677s).
Required all-target checkpoint FAIL (591s): mc2fx.exe and mc2fx_preview.exe
LNK2019/LNK1120, unresolved mc2::render_view::drawArena(), referenced by
gosFX::DrawNativeShape in shape.cpp.obj. Standalone projects/corpus built.
Completion monitor rejected the build receipt before deploy or game runs.
No acceptance battery ran; legacy/refusal/continuation/firstsave/SIMID/
audit/validation/current-main-CVE remain unproven for this candidate.
Per rejection rule, migration reverted as f8ced1fb5, flip as b7841a0dd.
W94 merge, counted Building codec and CVE-G fixture retained. Final masks
run 30 -> 30 (candidate 0 reverted), build 0 -> 0. SIMID pins unchanged.
Next: resolve the imported drawArena tool link closure on main, then
reapply 917daf779 and 98d54f460 and repeat clean-HEAD build/acceptance.
Sole install retained: E:/mc2-installs/claude-cxx23-packet-f, unchanged from
F10 rejected candidate 369753936. No extra stage/install created; freed 0 B.
Build output contains rejected ea698ee83, not rollback HEAD; do not deploy.
Rollback unit receipt: f11-rollback-unit.log, 68/68 cases and 3033/3033
assertions PASS (30s); helper prints PASS. PowerShell outer command reports
exit 1 while formatting expected negative-test stderr as NativeCommandError;
no test failed, and the unchanged tree was not tested again. Runtime/pins/
mask diff against merge e16169570 is empty ignoring end-of-line whitespace.
Final diff --check clean. All owned build/monitor/unit/scout children exited.
Largest avoidable cost: discovering imported tool link debt after the full
all-target compile. Fix the drawArena link closure upstream before retry.
