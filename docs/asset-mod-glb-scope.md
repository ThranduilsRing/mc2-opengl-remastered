# Asset Class GLB Support — Phase 0 Finding

> **SUPERSEDED (2026-07-09)** by [docs/asset-pipeline.md](asset-pipeline.md) section 1 —
> buildings/trees/props now support `[Import]` (as of merge `fdb7c470`, `bdactor.cpp`
> implements the probe); mech GLB is opt-in via `MC2_ASSIMP_MECH_IMPORT=1`. Kept for
> history.

## Summary

GLB probe support exists **only for mechs** today. All other classes use
`LoadTGMultiShapeFromASE` exclusively — no `[Import]` hook, no `LoadFromFile` call.

---

## Per-class table

| Class | Loader file | Shape container | LoadFromFile call? | GLB today? |
|---|---|---|---|---|
| **Mech** | `mclib/mech3d.cpp` | `TG_TypeMultiShape mechShape[]` | YES — via `[Import] Source=` opt-in (commit `309cbac2`) | YES (strict opt-in) |
| **Building** | `mclib/bdactor.cpp` | `TG_TypeMultiShape bldgShape[]` | NO | no |
| **Generic prop** | `mclib/genactor.cpp` | `TG_TypeMultiShape genShape` | NO | no |
| **Vehicle** | `mclib/gvactor.cpp` | `TG_TypeMultiShape gvShape[]` | NO | no |
| **Tree/foliage** | `mclib/bdactor.cpp` | `TG_TypeMultiShape treeShape[]` | NO | no |

---

## Evidence

`mclib/mech3d.cpp` (lines ~368–443):
```cpp
// ASSIMP-MECH-IMPORT-1 — STRICT OPT-IN. An optional [Import] section with
// Source= activates LoadFromFile on its LOD0 shape.
if (importSourceBase[0]) {
    mechShape[i]->LoadFromFile(importSourceBase);   // opt-in: modern import
```

`mclib/bdactor.cpp` — grep `LoadFromFile`, `ImportGeometry`, `[Import]`, `importBase`:
**zero hits**. Only `LoadTGMultiShapeFromASE` call sites.

`mclib/gvactor.cpp` — same: zero hits.
`mclib/genactor.cpp` — same: zero hits.

`TG_TypeMultiShape::LoadFromFile` in `mclib/msl.cpp` (line ~438):
probes `{tglPath}{baseName}.glb` then `.fbx`, calls `ImportGeometryFromFile`,
falls through to `LoadTGMultiShapeFromASE` on miss/failure.

---

## Implication for Phase 0–1 test asset

Phase 0–1 test asset must be a **mech** — the only class with an active GLB probe
path. The simplest mech with fewest LODs in the codebase is the **Raven** (1 LOD,
1 main shape, 6 hardpoints).

Non-mech classes need a 3-line engine addition per class (Phase 2).

---

## Phase 2 recipe (deferred)

For each non-mech class, in the class's `init()` INI parse, mirror
`mech3d.cpp:368`:
1. Read optional `[Import] Source=<base>` (strip extension)
2. At each LOD0 `LoadTGMultiShapeFromASE` call-site:
   `if (importBase[0]) shape->LoadFromFile(importBase); else shape->LoadTGMultiShapeFromASE(aseName);`

Prereq: `claude/assimp-mech-import-1` merged to nifty.
Effort: ~1 day per class (3 lines + smoke gate).
