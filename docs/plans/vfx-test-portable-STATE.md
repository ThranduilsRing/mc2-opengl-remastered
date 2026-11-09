# vfx-test-portable

Owner symptom: "reads a hard-coded Windows install path A:/Games/.../mc2.fx
that does not exist on the Linux box."

Base: d63c403c9715b6a664a3513b445430e722c94c10 (nifty at assignment).
Baseline box task: 201, stock quarter1 only: reproduced REQUIRE(file.is_open())
failure at line 517 on nifty. Receipt: proofs/box/mc2-slot09/d63c403c/.tails/row1-2965166418.log.
Change: unchanged 920,265-byte tracked stock fixture, runtime repository path,
explicit doctest skip with a reason when absent; existing comparisons unchanged.

Next: collect baseline failure; commit; run box targeted/full units and VFX
contract rows; Windows targeted unit and one production Ninja compile; verify
missing-fixture skip and intentional oracle regression; lint/precheck; release.
Evidence: E:/mc2-archive-2026-10/proofs/vfx-test-portable and proofs/box/mc2-slot09.
