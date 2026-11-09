# Contact picking fixes — lane state

Assignment: `A:/Games/mc2-slot01`, `claude/contact-pick-fixes`, base `f269b312d8328aaafccec0b53a15f23ce5775efb`.

Implemented: quality-aware pure hover naming shared by hover and mech/vehicle sensor text;
sensor-only mover model admission; front-to-back terrain march and bisection using
the accepted 4x CPU visual bake (simulation-height fallback), preserving object picking.
Production cursor already used pickTerrainRay; removed its nearest-vertex/cell snap.
Click inverseProject and anchored zoom now use the same surface picker in perspective.

Proof pending: targeted doctests/contracts, box build/unit/smoke-lite and
headless mc2_01 determinism/saveload; final Windows Ninja compile and lint.
No PC install, capture, or performance measurement planned.

Next: review diff, run proofs, record receipts and exact failure/skip details here.
