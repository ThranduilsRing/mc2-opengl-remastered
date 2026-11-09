# ABL stack overflow lane

Assignment: mc2-slot04, claude/abl-stack-overflow, base 609d072946c4256da454185255879b59da285a12.

Implemented: all five primitive pushes check capacity before incrementing tos.
runtimeError preserves the softfail log but throws on stack overflow, including
when a fatal callback returns. Audited ablrtn.cpp: entry frame and parameter
pushes delegate to these primitives; no independent unchecked increment exists.
Regression in the engine-linked doctest seam exercises each real primitive at
the boundary with both diagnostic flags on, checks the sentinel bytes beyond
storage, preserves the last valid value, and asserts execution cannot continue.

Next: commit; Ninja mc2 + engine-linked seam; ABL doctests and contract rows;
final lint; isolated deploy and smoke-lite; mc2_03 baseline determinism if feasible.
No proof has run yet. Keep evidence under E:/mc2-archive-2026-10/proofs/abl-stack-overflow.
