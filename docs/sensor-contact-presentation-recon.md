<!-- doc-status: stale | reviewed:2026-10-11 -->
# Sensor / contact presentation — recon (SENSOR-CONTACT-PRESENTATION-RECON-1)

Status: **DEFER. No renderer-only implementation is possible today.** This doc
records the recon verdict (Subagent E, 2026-05-29) and the minimal bridge a
future lane would need. No code shipped in this opus.

## The data exists — but only on the gameplay side

Contact state is fully structured and updated every frame in `code/`:

- `code/dcontact.h` — `ContactStatus` enum: `CONTACT_NONE`,
  `CONTACT_SENSOR_QUALITY_1..4`, `CONTACT_VISUAL`.
- `code/contact.h` — `ContactInfo::contactStatus[MAX_TEAMS]`, `SensorSystem`,
  `TeamSensorSystem::getContactStatus(MoverPtr, bool)`; global `SensorManager`.
- `code/mover.h` — `Mover::conStat` (int32_t), a per-frame cached copy of the
  home-team contact status (refreshed in `mech.cpp` / `gvehicl.cpp`, then fed to
  the **legacy** `appearance->setSensorLevel(...)` path only).

## The renderer cannot see it

- `RenderWorld` / `GameOS` / GPU mech batcher have **zero** contact access.
  `RenderMechDesc` carries `mechTypeId` / `gameObjectId` / `debugCookie` only.
- `setSensorLevel()` drives the legacy CPU MLR appearance
  (`sensorTriangleShape`/`sensorSquareShape`), not the GPU batcher.
- The `RenderWorld.h` firewall forbids game-side includes, so the renderer
  cannot reach `conStat` without an adapter bridge.
- Net: **no renderer-only presentation is possible now** — any attempt is
  either a no-op (no data) or a firewall violation.

## Minimal bridge (future, gameplay-collaborator + adapter lanes)

1. **Adapter call (small):** `RenderWorld::updateMechContactStatus(handle, uint8_t)`
   driven from the same sites that call `appearance->setSensorLevel()`
   (`mech.cpp`, `gvehicl.cpp`). Translate `CONTACT_*` → compact renderer enum.
2. **Storage:** `uint8_t contactStatus` in `RenderObjectRecord` (CPU-side only;
   no GPU impact).
3. **Consume:** either CPU-side in the GPU batcher (gate submit / set a
   per-draw uniform — lowest risk) or pass as an instance attribute for
   per-tier shader visuals (sensor-quality 1..4 distinct looks).

## Boundary (must hold)

```
GAMEPLAY (code/)                         RENDERER (RenderWorld/GameOS/mclib)
ContactInfo::contactStatus[]             RenderObjectRecord::contactStatus  [MISSING]
Mover::conStat (per-frame)         →→→   [needs adapter bridge — GameAdapters only]
SensorSystem / TeamSensorSystem          (gameplay-owned; must never cross)
gametacmap drawBlip/drawSensor           (code/-side 2D; not a renderer object)
```

## Deferred to gameplay/collaborator lanes
- All detection logic (range, ECM/ECCM, LOS) — already correct in `code/`.
- **Stale "last known position"** ghost contacts — MC2 does not track a last-seen
  position today; needs new gameplay-side data.
- Minimap/radar overlay modernization (`code/gametacmap.cpp`).

## Recommendation
Name the `updateMechContactStatus` API and land the adapter call FIRST (gameplay
collaborator + small adapter lane). Only then start a renderer visual lane.
Starting renderer-side now yields a no-op or a firewall breach.

## Re-confirmed under GAMEADAPTERS-VISUAL-STATE-BRIDGE (2026-05-29)

Slice 4 of `GAMEADAPTERS-VISUAL-STATE-BRIDGE-OPUS-1` re-ran this analysis and
the **DEFER verdict holds**. No contact state shipped. Two facts re-verified
against `claude/nifty-mendeleev`:

- `Mover::conStat` is already the post-filter, *player-visible* value (the home
  team's resolved status, refreshed at `mech.cpp:6327` / `gvehicl.cpp:3750`).
  It never exposes other teams' rows of `ContactInfo::contactStatus[]`. So the
  single scalar is safe to mirror — but only that scalar, via an adapter.
- `ShowMovers` (the reveal/debug cheat used in the visibility gates) is an
  existing intentional reveal; preserve it, don't treat it as a new leak.

**Hard leak stop conditions (any one → STOP):** bridging the full
`contactStatus[MAX_TEAMS]` array or any non-home `teamId`; calling
`getContactStatus(teamId,…)` for a non-home team on the renderer path; sending
ANY enemy transform/position to the renderer when `conStat == CONTACT_NONE`
(must be filtered gameplay-side before submit, as `mech.cpp:6332` does today);
exposing `SensorSystem`/`TeamSensorSystem`/`SensorManager` or detection inputs;
inventing a stale "last-known-position" ghost (no such data exists today).

**Precedent now exists.** Slice 1 (`MECH-VISUAL-STATE-BRIDGE-1`) established the
sanitized game→renderer pattern: a pure-POD `RenderCore::MechVisualState` filled
game-side and copied by value, no game-pointer chasing. A future
`SENSOR-VIEW-MVP-1` should mirror it — a `ContactVisualState`
(`enum uint8_t { Hidden, Sensor1..4, Visual }`) pushed through
`RenderWorld::updateMechContactStatus(handle, uint8_t)` from the same sites that
call `setSensorLevel()`. Renderer owns *how* a tier looks; gameplay owns
*whether* the player may see it.

## Cross-references
- `RenderCore/MechVisualState.h` (the sanitized-bridge precedent — Slice 1)
- `docs/thermal-ir-design.md` (same firewall constraint for heat data)
- `docs/thermal-view-mech-heat-mvp-defer.md` (Slice 2 defer + split path)
- `docs/viewmode-capture-matrix.md` (the presentation framework a future
  sensor mode would plug into as ViewMode::TacticalOverlay / a new mode)
