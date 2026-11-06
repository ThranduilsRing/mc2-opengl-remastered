# W122c T1 exact-tip proof

Assignment: A:/Games/mc2-cxxt1, claude/w122-cxx-t1.
Rebased onto 446d52859; candidate e7a7b14480ae3d874aae4d01bd9b94b3fe7b813d.
Only rebase conflicts were ratchet baseline head metadata; nifty metadata kept.
No global, namespace or static raw owner was converted to unique_ptr.
No untracked proofs/ directory was present. Receipts go to
E:/mc2-archive-2026-10/proofs/cxx23-t1/.

Configure PASS (RelWithDebInfo, existing shared compile cache).
MSVC build PASS: both mc2 and the default asset viewer linked successfully,
BelowNormal, CPUs 8-15, -j6. No compile fixes were needed.
This state-note commit changes documentation only; compiled source is identical.
Next: run unit contracts, C++ ratchet, file-size check and exactly one final-tip
lint. Exact final SHA and verdicts will be recorded in the proof root's
exact-tip-state.md so receipt updates do not change the proven commit.
No game or deploy is authorized or needed in this run.
