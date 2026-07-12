# MECH-NORMALS-FIX-1 — ship state

Fixes the corrupted mech normals diagnosed in `docs/mech-normals-audit.md`
(the shared TGL ASE loader averages per-corner normals by vertex index,
destroying hard-edge splits → "too smooth" + "rainbow not following geometry").

## What shipped

A **mech-local** normal recompute in the GPU mech batcher
(`GameOS/gameos/gos_mech_batcher.cpp`). The shared `LoadTGShapeFromASE`
(`mclib/tgl.cpp`) is deliberately **untouched** — it is used by all
props/buildings (high blast radius); the fix lives entirely in the mech
geometry build, on the triangle-soup verts.

- `recomputeMechNodeNormals(...)` runs per mech node (never blends across nodes
  = separate rigid pieces):
  - **mode 1 (Faceted):** geometric face normal (cross of triangle edges) to all
    3 corners. Flat shading; correct per-face. Used to confirm the path.
  - **mode 2 (Smoothed):** group corners by exact source position, accumulate
    area-weighted face normals only from faces within `smoothDeg` of each other
    → smooth within panels, **hard edges preserved**. Oriented to the cooked
    normal hemisphere (winding-agnostic).
- `s_stagingVbo` stays **pristine cooked**; the recompute is applied to a
  transient copy at VBO upload, so mode 0 (default) is **byte-identical** to the
  old path and modes can be re-dialed without re-registering.

## Controls

- Gate: `MC2_MECH_NORMALS_MODE` (0=Cooked default / 1=Faceted / 2=Smoothed),
  default **0** (OFF — no visual/perf change).
- `MC2_MECH_NORMALS_SMOOTH_DEG` (1..179, default 60) — mode-2 angle threshold;
  **lower = more hard edges** (sharpen the cockpit). Env sets startup default.
- **Live ImGui** (mech inspector → "Mech Normals (experimental)", shown when a
  mech is picked): Normal Mode combo + Smooth Angle slider. Changing either
  calls `batcher_rebuildMechNormals()`, which recreates the immutable geometry
  VBO from the pristine staging with the current mode/angle (occasional stall;
  default path never triggers it). Lets you dial without env vars or restart.

## Validation

- Build RelWithDebInfo clean (`MC2_IMGUI=ON`). env-registry PASS.
- Tier1 default (mode 0) 5/5 — byte-identical, no regression (one run showed a
  cross-session `--kill-existing` frame-0 false FAIL on a non-mech mission;
  the active-mech mc2_24 passed clean).
- Visual (mc2_24, `MC2_MECH_FRAG_DEBUG=4` normal debug): mode 1 faceted
  (per-face flat colors following geometry — confirms path), mode 2 follows
  geometry with hard edges preserved (vs mode 0's rainbow garbage). User-
  confirmed; cockpit fan-faces soften near 60° → lower the angle to sharpen.

## Out of scope / next

- Shared TGL loader fix (would fix props/buildings too) — separate approval,
  high blast radius.
- Picking a default mode/angle to flip ON, and MECH-AMBIENT-1 — both now
  unblocked (normals are correct under mode 2) but need separate approval.
- Possible: rebuild-on-slider-release instead of per-drag (minor UX).
