# actor-pool-2

Owner request: "object ownership: one owning factory/pool per actor family".
Assigned base: b26fc9bfe90a94a60a163d3bc5e2399ab5cbd16d.

Gates chosen: fixed mission/save slots mirror turrets; no vehicle spare-slot
reuse and less gameplay surface than buildings. Reuses ActorPool unchanged.
Mission setup and save load share spawnGate; gateStore and manual retirement
removed. The unused GateType raw factory now refuses unowned construction.
Save records, slot/handle order, and existing fixtures are unchanged.

Offline coverage: existing ActorPool creation/stale-ID/graveyard doctests plus
new ActorPool gate-fixture reload test (production GateRecord codec, same bytes,
slot handles, stale session rejected, destruction waits for render packet).
This test uses a record holder; engine Gate construction needs runtime globals.

Next: box build/unit, sim_include_firewall, ratchet then lint; determinism and
saveload mc2_10/mc2_03 baseline checks; smoke-lite; one Windows Ninja compile.
Record receipts and any base failures here. No baselines may be re-recorded.
