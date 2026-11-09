# box-wait-rc

Assignment: mc2-slot07, claude/box-wait-rc, base 8fc0c6c8957673ab0be789403a10b91b852c59b2.
Root cause confirmed from live Pueue metadata: tasks 67, 87, 97 have
`status={"Done":"Success"}`. The parser expected Done.result, synthesizing rc=1.
Task 81 has `status={"Done":{"Failed":2}}`; its real rc was also lost.
Initial two-noop repro: task 113 (sh:true, sh:exit 0) finished with both rows
rc0; original --wait exited1 and printed state=Done rc=1 rows=2/2 verdict=FAIL.

Implemented: actual/legacy Pueue result parsing; distinct infra rc77;
named row table on final wait line; requested-row completeness; copy-back
failure detection and stale evidence removal. NO_BASELINE retains exit4,
LOCK_TIMEOUT exit76; row failures exit1. Targeted fault-injection tests added.
Initial targeted pytest: 51 passed. Owning row: gate_farm.
Next: finish gate_farm contract + cxx ratchet; commit; real green/red tasks
at tip and final lint. Final receipts will be in
E:/mc2-archive-2026-10/proofs/box-wait-rc/. No engine/game changes.
No PC install or scratch created; archive proof receipts retained.
