# T6 exit ownership correction

## Rebase and final proof run (2026-11-06)

Rebased onto 560feb6a8; three conflicts were baseline.json head metadata only.
Nifty's ABL replace_filename/generic_string include path and shadow push fix
are preserved. Restored the ownership inventory's true explicit-teardown counts:
SpecLibrary delete=1, TG_TypeMultiShape new=5, apprtype untyped deletes=3,
fastfile untyped deletes=1; each is at or below nifty's corresponding row.
Added FstRoundtrip doctest covering borrower close before explicit archive
teardown, repeated shutdown, and registry reinitialization. Sensor-shape and
SpecLibrary shutdown are not linked into the headless unit target: they require
appearance/TGL renderer state and gosFX heaps/object registries respectively.
Mocking those would not prove real static destruction order; the reversion
removes automatic destruction, and the final engine smoke covers normal exit.
No existing tests removed. Local proofs copied to the E: topic archive under
local-pre-rebase; automatic approval review rejected deletion (blocked by policy),
so local proofs remain and deletion will not be retried or bypassed.
Checkpoint: source/test candidate 2d9ca738d1470f288c268c00dfcb4ca23b4f2a4f.
Cxx17 ratchet PASS; own_inventory PASS (294 types, zero rises/falls);
file_size PASS (inherited terrtxm.cpp +1 warning); diff whitespace PASS.
Box proof submission attempted twice, both failed before queue submission:
SCP connection closed, rc=255. Separate lint submission failed identically.
Cleanup attempted: SSH connection refused at 14.227.95.149:30598. No box task,
build, game, install or scratch directory was created by this run. The bundle
was removed by box_run's finally block; removed this lane's stale local box-run
ref. Other lanes' refs left alone. Remote state cannot be inspected/cleaned
while the endpoint refuses SSH; preserve any existing crash evidence before
cleanup when it returns. Logs retained in the E: topic archive /2d9ca738/.
Do not treat earlier proof receipts as proof of this rebased candidate.
Next after box recovery: run against the final HEAD (including this note)
`py -3 A:/Games/mc2-main/tools/box_run.py --repo A:/Games/mc2-cxxt6 --gpu auto -- build unit unit-san smoke-lite determinism:mc2_03 saveload`,
then the same command with `-- lint`. Retry a lease-blocked row once later;
retain exact final-SHA receipts, archive crash evidence, then run
`box_run.py --cleanup claude/w122-cxx-t6`. Keep the assigned worktree.

Assignment: A:/Games/mc2-cxxt6, claude/w122-cxx-t6, base ef86f849e0b269ee239537487a97453385c339b8.

Audit of base..HEAD found five automatic static owners added by T6: three sensor
shapes, the FastFile owner vector, and the SpecLibrary singleton. All reverted
to raw storage and explicit teardown. Local/instance RAII remains; FastFileInit
keeps its local failure-path owner and releases only into the explicit registry.

Review list: mech3d.cpp SensorSquareShape; gvactor.cpp SensorTriangleShape and
SensorCircleShape (destroySensorShapes); fastfile.cpp registry (FastFileFini);
particles/spec_library.cpp singleton (SpecLibrary::Shutdown). Direct std::exit
must not destroy these after the function-local registries or engine heaps.

LG03 mclib baseline honestly restored from 324 to 334 (+10 raw allocation/delete
sites needed for explicit teardown); no exemptions or regenerated inventories.
TraceManager's static owner already exists at the assignment base and was not
introduced by T6; its local buffer/stream changes do not add a static owner.

Next: check ratchet/diff, commit locally; run
box build unit unit-san smoke-lite determinism:mc2_03 determinism:mc2_24 at the
committed final tip, then one box lint. Archive SHA-tagged receipts and crash
evidence, clean box lane. Do not delete pre-existing local proofs/: earlier
automatic review denied that deletion. No local install created.
