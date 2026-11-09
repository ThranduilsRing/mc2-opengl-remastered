# vfx-test-portable

Owner symptom: "reads a hard-coded Windows install path A:/Games/.../mc2.fx
that does not exist on the Linux box."

Base: d63c403c9715b6a664a3513b445430e722c94c10 (nifty at assignment).
Baseline box task: 201, stock quarter1 only: reproduced REQUIRE(file.is_open())
failure at line 517 on nifty. Receipt: proofs/box/mc2-slot09/d63c403c/.tails/row1-2965166418.log.
Change: unchanged 920,265-byte tracked stock fixture, runtime repository path,
explicit doctest skip with a reason when absent; existing comparisons unchanged.

Continuation evidence: owner expanded this lane to carry the RNG determinism
fix. Compared 299ee3b1e and de00a7a8c: their two gosfx changes sequence the same
draws; took only de00a7a8c (6ab788f22) because the larger commit duplicates the
fixture and changes an unrelated load/save test. No oracle checks relaxed.
Prior precheck PID 189792 has ended. Its failure was unrelated Python tests;
see archived precheck.log. Prior proof verified Windows playback (29,822,155
assertions), missing-fixture skip, and an intentional age-oracle regression.

Next: final-tip box full units plus targeted playback and owning VFX rows;
Windows targeted playback and one production Ninja compile; final lint.
Archive final receipts and any unresolved checks in the external STATE.md.
No game runs or captures (explicit offline-only scope). Prior cleanup/release
was refused by tool policy; do not retry or bypass that refused action.
Evidence: E:/mc2-archive-2026-10/proofs/vfx-test-portable and proofs/box/mc2-slot09.
