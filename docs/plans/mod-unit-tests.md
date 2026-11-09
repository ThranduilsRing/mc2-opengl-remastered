# Mod unit regressions — 2026-11-08

Assignment: slot11, claude/mod-unit-tests, base d63c403c9.
Added seven synthetic, game-free doctests to the existing unit target, including
the production model registry TU (previously tested only by a separate CTest).

History/class map:
- VFS/model mount precedence and additive provider-relative model merge: 02c884e25.
- Manifest schema validation: b7f7c1211.
- Retail FST mixed case/backslash keys: f31f7a0df.
- Loose missing/changed asset fallback: 02c884e25 (later art-layout fixes d12205cd2).
- Invalid render-only override data: model registry validation; additive merge 02c884e25.
- Save compatibility with changed mod data: save archive fingerprint rejection
  (existing save_archive unit coverage); canonical bolt saves fixed by 02c884e25.
  The new test covers the archive codec, not complete engine save reconstruction.

Reviewed strategy docs: mod-virtual-filesystem-design and data-ownership-registry-strategy.
Latest battery: E:/mc2-archive-2026-10/proofs/mods-nightly-20261108.
Run1 has invalid fingerprints; run2 reference-exe timeouts; run3 CVE startup timeout.
These are not evidence of a specific mod content regression. No ORCHESTRATOR NOTE supplied.
Fixtures model the six supported legacy mods' common shapes, not copies of their assets.

Next: build focused tests on box; demonstrate all seven red via temporary synthetic
input mutations; restore fixtures, commit final tip, run box unit/owning contracts/lint
and one Windows Ninja compile. Record receipts and runtime here. No deploy/game needed.
