# Track V — Status Ledger

**Last update:** 2026-05-29
**Branch:** `claude/nifty-mendeleev`
**Scope:** Visual / presentation track (Track V). Track R (engine-convergence
API: handles, PipelineDesc, snapshot, EngineView, visibility) is **CLOSED** —
see [track-r-closeout-opus.md](track-r-closeout-opus.md).

This is a **status ledger**, not a plan. It records what Track V has actually
shipped, what is explicitly deferred, and what the next slice owners are. It is
maintained by docs/scripts hygiene passes; runtime behavior is never changed by
editing this file.

---

## Shipped

| Item | Where | Notes |
|------|-------|-------|
| Track R closeout | [track-r-closeout-opus.md](track-r-closeout-opus.md) | Engine-as-API spine stable; Track R closed |
| Post / grounding MVP | [trackv-post-grounding-soak-1.md](trackv-post-grounding-soak-1.md) | HDR + bloom + ACES tonemap post stack, soak-validated |
| Lighting consistency | [trackv-lighting-consistency-opus-1.md](trackv-lighting-consistency-opus-1.md) | Cross-surface lighting reconciliation |
| VFX payoff | [trackv-vfx-payoff-opus-1.md](trackv-vfx-payoff-opus-1.md), [vfx-payoff-capture-matrix.md](vfx-payoff-capture-matrix.md) | Soft particles, lit particles, bloom participation (gates default-OFF) |
| Tactical presentation | [tactical-presentation-visual-state-capture.md](tactical-presentation-visual-state-capture.md) | ViewMode framework + ObjectIdDebug / Thermal-placeholder / LowLight / tactical-arc overlay |
| GameAdapters visual-state bridge | [renderworld_arc_status.md](renderworld_arc_status.md) | Static-prop + Mech render adapters, ObjectID substrate |
| Validation scaffold / checks | `scripts/check-contracts.sh`, `scripts/check-smoke-matrices.py`, `scripts/check-toolchain-bom.py` | Cheap static contract + matrix + toolchain gates |
| Asset-manifest scaffold | `tools/asset_cook/validate_asset_manifest.py`, [asset-manifest-schema.md](asset-manifest-schema.md) | Shape gate + negative-fixture regression guard (`scripts/check-asset-manifest-fixtures.py`) |
| Capture no-kill tooling | `scripts/run_smoke_matrix.py`, `scripts/check-smoke-matrices.py` | Repeated `--mission`, never `--kill-existing`; concurrency-safe lock |
| HZB depth-convention contract | [hzb-depth-convention.md](hzb-depth-convention.md), `tests/unit/test_depth_hzb.cpp` | Reverse-Z / MIN-reduce contract LOCKED (no runtime yet) |

### Capture matrices (definitions; no runs in CI)
`tests/smoke/matrices/`: `staticprop`, `terrain`, `vfx`, `viewmode`, `water`,
plus `shadow`, `ssao` (this batch) and a `hzb` placeholder. All pass
`check-smoke-matrices.py` (registered env vars, known missions, no
`--kill-existing` / `--missions`).

---

## Deferred / future (NOT built)

These are named so nobody mistakes a placeholder or a stand-in for a shipped
system. Each becomes a slice when its owner picks it up.

| Item | State |
|------|-------|
| Real sensor contacts (LOS / radar / ECM) | deferred — ViewMode "Sensor" frame is not wired to real sensor data |
| Vehicle / mech thermal (real heat channel) | deferred — Thermal ViewMode is a **luminance placeholder**, not real IR (see thermal-ir-design notes) |
| HZB runtime / GPU occlusion cull | deferred — contract locked, **no runtime**; owner `TRACKRV-HZB-VISIBILITY-OPUS-1` |
| Asset import / cook runtime (KTX2 bake, meshopt/CDAG, gltf) | deferred — manifest is a SHAPE gate only; owner `TRACKG-ASSET-PIPELINE-PROBE-OPUS-1` |
| Semantic visibility | deferred — vision-stage concept, no implementation |
| Zoom-continuous presentation ladder | deferred — vision-stage concept |
| Tactical field renderer (threat / cover / path gradients) | deferred — tactical-arc overlay is a single-unit MVP, not a field |
| Real culling (beyond camera-frustum) | deferred — depends on HZB runtime |

---

## Next-gen RTS vision alignment

The thesis in [2026-05-26-next-gen-rts-engine-vision.md](2026-05-26-next-gen-rts-engine-vision.md)
("render the commander's understanding of the world", not just the world) is
**post-roadmap context**, not a backlog. Track V's shipped work is the
*foundation* the vision builds on, not the vision itself:

- **ViewMode framework** → the seed of "perception-first rendering" (Visual /
  Sensor / Command / Damage as first-class frames). Today only Visual is real;
  Thermal/LowLight/ObjectIdDebug are presentation experiments.
- **GameAdapters + RenderSnapshot + ObjectID** → the substrate a future
  "RenderWorld as a query engine" would query.
- **Tactical-arc overlay** → the smallest precursor to "tactical fields"
  (envelopes, threat gradients, safe paths).

Translate the vision into **slices**, do not implement it wholesale. Each future
slice should name one perception layer or one query and ship it gated, the same
way the foundation above shipped.

---

## Recommended next Opus slices
- `TRACKRV-HZB-VISIBILITY-OPUS-1` — runtime HZB against the locked depth contract.
- `TRACKG-ASSET-PIPELINE-PROBE-OPUS-1` — asset import/cook probe against the manifest shape gate.
