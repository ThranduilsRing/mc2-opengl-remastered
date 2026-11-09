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
Baseline code b113e2fa1400 compiled on box. Fixed-frame building probe reports
RGB 16777215 (white), maxIntensity 1, inner 150, outer 500, radiance [1,1,1].
Byte normalization and the single intensity multiply are correct. Terrain uses
the legacy display-color light contract; postprocess multiplies exposure (default
1) and grades it, rather than tone-mapping the added flash. Do not introduce an
isolated sRGB decode into that pipeline.

Implemented: class constants and geometry-radius scale; rational intensity and
reach shoulders; core limited to 5% of reach; same transform on every decay frame.
For the reference building, peak becomes 0.149333 (pool compression <0.22),
outer 273.596, core 13.680. Mine and other unrelated effects remain unchanged.
Source size is presentation-only; older saves use the class reference radius.
Doctests cover class/size ordering, decay, overdrive, bounds and invalid input.
Preliminary cxx17 ratchet PASS after replacing diagnostic functional casts.

Baseline capture batch on box (b113e2fa): day/night building flash at render
triggers 182/186/190, plus vehicle/turret at 182. Debug flash spawns at 180;
capture freezes sim at the requested trigger, then snapshots the settled pose.
Archive root E:/mc2-archive-2026-10/proofs/box/mc2-slot12/<sha8>/.
day.json and night.json proof camera inputs are in proofs/explosion-whiteout and
the box lane tmp directory. Bookmarks are gitignored, so fixtures stay in proofs.

Next: commit fix, final box build/unit/covering contracts/lint/smoke and matching
captures, one Windows Ninja compile, inspect montages, archive logs and clean
lane-owned scratch. No PC install created. Box install is lane-owned/reused.

Checkpoint: box disconnected (SSH reset then connection refused) during the
remaining baseline captures and before final box proof. First baseline day
capture and building diagnostic retained. At ad0be26c the focused doctest
passed 3 cases / 45 assertions, covering contracts passed 3 rows, lint passed.
Windows compile found the legacy Windows `far` macro colliding with the helper's
local variable; renamed it to outerRadius. Resume the failed build and the
focused final-tip checks. Current receipts and exact remaining capture rows:
E:/mc2-archive-2026-10/proofs/explosion-whiteout/state.md.
