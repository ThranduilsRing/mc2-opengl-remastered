# Deploy reuse

Assignment: slot09, claude/deploy-reuse; starting HEAD eef4eddb4.
Baseline passed: 318.111 s; hash/verify 22.259 s, copy 38.603 s,
stock stamps 71.218 s, kit prepare/cook 175.944 s (693 + 9 GLBs).
Implemented metadata/hash receipts (including NTFS ChangeTime), retained stock
stamps with native FNV, input-keyed shared recipes, and verified seed kit reuse.
Targeted tests: 100 passed/1 skipped initially, then 51 passed including combined
zero-copy/zero-cook and shared-write fault cases. Added normalized-sidecar case.
Next: final-tip Ninja build, targeted pytest/contracts, one lint, retained deploy
timings, cleanup. No engine change, C++ doctest or game run needed.
Proofs and final completion/next-step receipt:
E:/mc2-archive-2026-10/proofs/deploy-reuse/STATE.md.
