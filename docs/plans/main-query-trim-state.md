# main-query-trim state

Assignment: A:/Games/mc2-slot04, claude/main-query-trim; base 8fc0c6c89.

Implemented GUI-covered hover suppression after lifetime validation/cleanup,
one-pass priority/depth/handle pick selection (stable exact ties), and nearest-ray
LOS early-out before batching the rest. No persistent LOS cache or cadence change.
Added offline pick sort oracle, hover cleanup/freshness test, randomized LOS
any-hit/full-batch equivalence and an early-out evaluation counter.

Next: final diff review, targeted contracts/doctests, compile, box headless SIMID
mc2_01/03/10/24 and smoke-lite, final lint. Record receipts and clean scratch.
No installs or scratch created yet. Savings measurement belongs to weekly batching.
