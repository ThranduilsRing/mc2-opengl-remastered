# W96 smoke count ceilings

Owner-authorized test-only adjustment: remove walk.* ceilings, whose run means
depend on run length and host load (mc2_10 walk.update ~125 vs 79.56).
Other ceilings use twice the original observed count (old ceiling / 1.25),
with draws.props using twice the reported maximum, conservatively bounded by
old ceiling x1.08 for the observed 5–8% overrun. No ratchet baseline changes.
Counts and walk counters remain present in logs; only smoke ceilings change.
Window proof/resume state: E:/mc2-archive-2026-10/proofs/merge96/STATE.md.
