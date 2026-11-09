# Explosion whiteout lane

Assignment: slot12, claude/explosion-whiteout, base d63c403c9715b6a664a3513b445430e722c94c10.
Merged lighting-hud-residuals (13041f465) before changes.

Inspection: TG_Light normalizes RGB bytes at GPU upload, and uses source RGB
with separate intensity exactly once. Terrain adds the pool after ground
lighting, including concrete. Pool compression does not reserve ground headroom.
Stock explosion creation has no destroyed-object size input; most death callers
pass null owners. Do not change damage ownership to obtain visual size.

Next: instrument source/pool numbers and add a fixed-frame explosion debug
command; capture baseline on box; add class/size scaling and soft compression
with doctests; final box build/unit/contracts/lint/smoke and day/night captures;
one Windows Ninja build (mc2 + viewer); inspect montages; cleanup and handback.
No proofs run yet. No installs or scratch created by this lane yet.
