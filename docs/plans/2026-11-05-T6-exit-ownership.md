# T6 exit ownership correction

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
