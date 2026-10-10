<!-- doc-status: current | reviewed:2026-10-11 -->
# MC2 Runtime Bridge Architecture

**Status:** Strategy / design doc (no code yet)
**Date:** 2026-06-10
**Scope:** Long-term architecture for editor ↔ mc2.exe out-of-process bridge: launch, observe, eventually control.

## Anti-goals (binding)

- **No in-process sim.** Recon established sim/render/HUD/camera coupling makes PIE-in-editor infeasible. The game always runs as a separate `mc2.exe` process.
- **No Mission/ObjectManager refactor.** The bridge taps existing identity systems (watchID, partId) and existing lifecycle hooks (Mission::init/update/destroy); it does not restructure them.
- **No runtime state mutation from the editor until observation is stable.** Phases 0–3 are strictly read-only. Control (v1 socket commands) lands only after the telemetry pipeline has proven itself across crashes, restarts, and protocol drift.

## 1. Process model

The model is **supervisor + child**, built on what already ships:

- **Editor = supervisor.** It owns the child's lifetime. Launch via the existing `EditorTaskRunner` (`editor/EditorTaskRunner.cpp`): `CreateProcessA` with `CREATE_NO_WINDOW`, combined stdout+stderr pipe (`CreatePipe`, child inherits write end), a worker thread reading 4 KB chunks and splitting lines into `HandleLine()`, exit detection via `WaitForSingleObject` + `GetExitCodeProcess`, and main-thread-only result callbacks through `PumpMainThread()` (called from `EditorInterface::update()`). This threading contract — worker thread touches only runner state under mutex, callbacks fire on the main thread — is the foundation the whole bridge inherits.
- **Game = child.** Launched as today: `"<exe>" -mission "<pak path>"` (built at `EditorInterface.cpp:5316`, parsed at `mechcmd2.cpp:2468-2506`, sets `justStartMission=true` and skips the menu). The game never knows or cares whether an editor is listening; all bridge behavior in the game is opt-in via environment variables set by the supervisor.
- **One playtest session at a time** (v0). The runner already supports multiple tasks; the bridge layer enforces a single active *playtest* session so panel state, runtime-ID maps, and the stop/restart state machine stay unambiguous. Multi-session is a non-goal until someone needs it.
- **Orphan policy.** If the editor exits while a playtest child runs, the child is killed (Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, assigned at launch). This is the one process-model addition over today's runner and prevents zombie mc2.exe instances — a known pain point (TaskStop orphaning mc2.exe children is in session lore).

A thin new layer, `PlaytestSession`, sits above `EditorTaskRunner`: it owns the session state machine (below), the telemetry decoder, and the runtime-ID table. The runner stays a dumb process executor.

```
EditorInterface (ImGui panels)
        │ main thread
PlaytestSession  ──── state machine, telemetry store, ID map
        │
EditorTaskRunner ──── CreateProcessA / pipe / worker thread
        │ stdout pipe (v0) + TCP socket (v1)
mc2.exe ─ BridgeEmitter (game-side, env-gated)
```

## 2. Auto-save / playtest lifecycle

Today the launch button requires a manual save ("Save the mission to a .pak first"). The bridge replaces this with an **auto-save-to-shadow-copy** flow so playtest never mutates the user's working file and never launches stale data:

1. **Snapshot save.** On "Playtest", the editor saves the current mission state to a shadow path: `<missionDir>/.playtest/<missionName>.pak` (+ `.fit` sidecars, via the existing `EditorData::save()` → `saveMissionFitFileStuff()` flow pointed at the shadow dir). The user's own `.pak` is untouched; unsaved edits still playtest correctly.
2. **Validate.** Run the existing Mission Validator checklist (`MissionValidation.cpp`). Hard failures (no player drop zone, no MOVE data on legacy-size maps) block launch with the checklist shown; soft warnings annotate the session panel but don't block.
3. **Launch.** `EditorTaskRunner::StartTask` with `-mission "<shadow pak>"`, working directory = game exe dir (as today), plus bridge env vars (`MC2_BRIDGE=1`, `MC2_BRIDGE_SESSION=<guid>`, later `MC2_BRIDGE_PORT`).
4. **Observe.** Telemetry flows until exit.
5. **Cleanup.** Shadow dir entries older than N sessions are pruned at editor startup. Shadow paks are also the artifact attached to crash reports (exact bytes the game saw).

Session state machine (owned by `PlaytestSession`):

```
Idle → Saving → Validating → Launching → Loading → Running ⇄ Paused
                                   │           │        │
                                   └──── Failed ◄── Crashed
Running/Paused → Stopping → Exited → Idle
Exited → (Restart) → Saving   (fresh snapshot, new session GUID)
```

- `Loading` = between process start and `[TIMING v1] event=mission_ready`.
- `Running` ⇄ `Paused` is driven by telemetry (game reports its own pause state), not by editor assumption.
- Every transition is timestamped and logged to the session panel.

## 3. stdout protocol v0

v0 rides the **existing pipe and existing line discipline** — `HandleLine()` already parses line-by-line. The smoke instrumentation (`gos_smoke.cpp`) proves the pattern: versioned bracketed prefix, `key=value` pairs, one event per line. v0 generalizes it:

```
[BRIDGE v0] ev=<event> t=<turn> ts=<scenarioTime> <key>=<value> ...
```

Rules:

- **One event per line, ≤ 512 bytes**, ASCII, no embedded newlines. Values with spaces are forbidden in v0 (use IDs, not names; names resolve editor-side via the partId map).
- **Versioned prefix.** `[BRIDGE v0]` is the namespace; unknown `ev=` values are ignored by the editor (forward compatibility), unknown prefix lines pass through to the raw log view (smoke markers, `[TerrainLOD prod]`, FATAL lines all still visible).
- **`t=` is the mission turn counter** (`Mission::update` `turn++`, code/mission_update.cpp:473) and `ts=` is `scenarioTime` — every event carries both so panels can order and align events without wall-clock games.
- **Rate discipline.** stdout is a shared, blocking, line-buffered channel also carrying smoke/diag output. v0 events are budgeted: lifecycle events are unthrottled (they're rare); periodic snapshots (object census, perf) emit at ≤ 2 Hz; per-entity events (damage, death) are unthrottled but the emitter drops to a `ev=overflow dropped=<n>` line if a frame would emit > 64 events. **No per-frame-per-object streaming on stdout — that's what v1 is for.**
- **Emission point.** A single game-side module, `BridgeEmitter` (new file, pattern of `gos_smoke.cpp`): env-gated by `MC2_BRIDGE=1`, zero-cost when off, called from a handful of existing seams (mission lifecycle, end-of-`Mission::update`, object death notification). No scattered printf — all bridge output goes through one choke point so rate limiting and versioning are enforceable.

Why stdout first: the transport, the reader thread, the line parser, and the main-thread pump **all already exist and are battle-tested by the smoke harness**. v0 needs only an emitter in the game and a decoder in `PlaytestSession::HandleLine`. That gets observation panels live in days, and everything learned about event taxonomy carries to v1 unchanged.

v0 limitations (accepted): one-way only (no control), blocking writes if the editor stalls reading (mitigated by the existing dedicated reader thread + 64 KB log trim), text overhead.

## 4. Socket protocol v1

When observation outgrows stdout (entity position streaming, control commands), add a **localhost TCP socket**, loopback only:

- **Handshake.** Editor opens a listening socket on an ephemeral port before launch, passes `MC2_BRIDGE_PORT=<port>` + `MC2_BRIDGE_SESSION=<guid>` in the child environment. Game connects out (child-connects-to-parent avoids firewall prompts for listening game processes) and sends a `hello` frame echoing the session GUID and its protocol version. Editor rejects GUID mismatches (stale game from a previous session).
- **Framing.** Length-prefixed JSON frames: `uint32 length` + UTF-8 JSON body. JSON over a binary scheme because event volume is modest (hundreds/sec, not hundreds of thousands), debuggability is paramount in a modding tool, and schema evolution is free. If entity-transform streaming ever needs it, add a single binary frame type (`type:"blob"` + typed payload) later — don't pre-optimize.
- **Channels.**
  - `telemetry` (game→editor): same event taxonomy as v0, JSON-shaped, unthrottled lifecycle + configurable-rate streams (editor sends a `subscribe` frame: which streams, at what Hz).
  - `command` (editor→game): pause/resume/stop, and later, gated mutation commands. Every command carries a sequence number; game replies `ack`/`nack` with the seq, so the editor's UI can show command state truthfully.
- **Coexistence.** stdout v0 stays on permanently as the crash-resilient channel: socket buffers die with the process, but the pipe's last lines survive to the editor and are the primary crash forensics. Lifecycle events emit on **both** channels; high-rate streams are socket-only.
- **Game-side threading.** Socket I/O on a dedicated game thread; inbound commands land in a lock-free queue drained once per frame at a single point in the main loop (top of `Mission::update`). The sim never blocks on the network, and commands apply at a deterministic frame boundary.

## 5. Telemetry event taxonomy

Namespaced `category.event`. v0 ships the starred subset; the rest are v1/stream-tier.

**session.\*** — process and protocol lifecycle
- ★ `session.hello` (version, build id, exe path, session GUID echo)
- ★ `session.goodbye` (reason: quit | mission_end | stop_requested)

**mission.\*** — mission lifecycle (hooks: `Mission::init/start/update/destroy`)
- ★ `mission.loading` (pak path), ★ `mission.ready` (turn 0, object counts by class)
- ★ `mission.result` (scenarioResult: win/lose/draw, terminationResult, final turn/time)
- ★ `mission.objective` (objective index, old→new status)
- ★ `mission.paused` / `mission.resumed` (source: hotkey | command)

**object.\*** — entity lifecycle (hooks: ObjectManager create/destroy seams, death pipeline)
- ★ `object.census` (periodic ≤ 0.5 Hz: counts by class, alive/dead movers per team)
- ★ `object.destroyed` (rid, class, partId if any, killer rid if known)
- `object.damaged` (rid, location, amount) — v1, subscribable
- `object.spawned` (runtime-spawned entities: salvage, elementals)

**mover.\*** — per-mover streams, v1 socket-only, subscribable at N Hz
- `mover.transform` (rid, x, y, z, rot), `mover.state` (pilot status, weapon state, move order class)

**perf.\*** — frame health
- ★ `perf.sample` (≤ 1 Hz: fps, frame ms p50/p99 — reuse the `[PERF v1]` ring-buffer machinery)
- ★ `perf.hitch` (frame ms over threshold, with the H1a hitch-attribution tag when available)

**warn.\*** — anomalies worth surfacing in-editor
- ★ `warn.script` (ABL error: module, line), ★ `warn.asset` (missing texture/tgl/fit fallback hit)
- ★ `warn.oob` (object off MOVE grid — the existing guarded accessor hits, now reported instead of silent)

This taxonomy is the bridge's real product: each `warn.*` event turns a silent in-game degradation into an actionable editor diagnostic pointing at an editor-selectable object.

## 6. Stable runtime IDs

The correlation problem: an editor placement must map to a runtime entity and back, across the whole session.

- **`rid` (runtime ID) = watchID.** `GameObject::watchID` is allocated append-only into `watchList[]` (objmgr.cpp:591-595), never reordered, never reused within a mission — exactly the stability needed. Every telemetry event referencing an entity carries `rid=<watchID>`.
- **Editor correlation = partId.** `partId` derives from the editor's Part index and persists through `.fit` `[PartN]` blocks; at mission load, `parts[i].objectWID` links part → watchID (mission.cpp:~1010). `mission.ready` is followed by a one-shot **`object.manifest`** dump: `(rid, partId, objectTypeNum, class, team)` per editor-placed object. `PlaytestSession` builds the bidirectional map once; thereafter every event with a `rid` resolves to an `EditorObject` (and clicking telemetry selects the object in the viewport).
- **Runtime-spawned entities** (no partId) get `partId=0` and are listed by class/position only.
- **Restart invalidates everything.** rids are scoped to a session GUID; the editor map is rebuilt from each new manifest. No ID persistence across runs is attempted — that's what partId is for.
- **Terrain/static props** are out of scope for v0 identity (block/vertex indexed, not part-indexed); if needed later, the manifest grows a `static.manifest` section keyed by block:vertex.

## 7. Pause / stop / restart semantics

- **Pause/resume.** Game already has `gamePaused` + `MissionInterfaceManager::togglePauseWithoutMenu()` (missiongui.h:269-272). v0: pause is observed only (game reports `mission.paused` when the user hits the hotkey). v1: editor sends `command.pause` / `command.resume`, applied at the frame-boundary command drain, acked, and confirmed by the same `mission.paused` telemetry — the panel state always reflects what the game *said*, never what the editor *asked*.
- **Stop (graceful).** Editor touches the runner's existing `cancelFile` mechanism (or v1 `command.stop`); `BridgeEmitter` polls it once per frame and triggers the game's normal quit path → `session.goodbye reason=stop_requested` → clean exit 0. Grace window 5 s.
- **Stop (hard).** After the grace window, `TerminateProcess` (already in `CancelTask`, editor/EditorTaskRunner.cpp:372). Session marked `Killed`, last 64 KB of log retained.
- **Restart.** Always **full process restart with a fresh snapshot save** — never an in-process mission reload. Rationale: re-snapshot picks up edits made while watching (the actual iteration loop modders want), and process death is the only reliable way to reset global state in this engine. Sequence: Stop → wait Exited → Saving → … One button in the panel.
- **Exit-code interpretation:** 0 = clean; 2 = argument/smoke parse failure (config error, show command line); other nonzero or pipe-EOF-without-goodbye = crash → `Crashed` state, crash workflow (§9).

## 8. Editor panels consuming telemetry

All ImGui, following the existing `renderMissionToolsImGui()` pattern (method on `EditorInterface`, registered in the toolbar render loop, auto-docked into the right column under `MC2_EDITOR_AUTODOCK`). All read from `PlaytestSession`'s main-thread-safe store; panels never touch the pipe or socket.

1. **Playtest Control** (v0, replaces the current launch button): exe path, state-machine status with timestamps, Launch / Stop / Restart buttons, exit code + failure reason, mission-validator warnings.
2. **Session Log** (v0): scrolling raw line view (everything, including non-bridge output), filter chips by prefix (`BRIDGE`/`SMOKE`/`FATAL`/`GL_ERROR`), auto-scroll, copy-to-clipboard. This is the crash-forensics surface.
3. **Mission Status** (v0): turn/scenarioTime, objective list with live status, team alive/dead counts from `object.census`, win/lose banner from `mission.result`.
4. **Runtime Objects** (v0 manifest + events): table of manifest entries — partId, class, team, alive/dead, kill attribution. **Row click selects the corresponding `EditorObject` in the viewport** (the partId map's payoff). Dead-object rows accumulate a casualty report for balance tuning.
5. **Warnings** (v0): aggregated `warn.*` events, deduped with counts, each row resolving to an object/asset where possible. The "your mission has problems" panel.
6. **Perf Strip** (v0 minimal): fps + p99 sparkline from `perf.sample`, hitch markers from `perf.hitch`.
7. **Live Map Overlay** (v1, needs `mover.transform` stream): mover positions drawn on the editor's tacmap — observe pathing behavior against the terrain being edited.

## 9. Failure / recovery behavior

- **Crash (pipe EOF, no goodbye, nonzero exit).** Session → `Crashed`. Editor retains: last 64 KB stdout, exit code, last telemetry event (turn/time of death), shadow pak snapshot. One-click "Save crash bundle" zips these. The last `[HEARTBEAT]`-style turn marker bounds the crash to a frame range.
- **Hang.** Editor-side watchdog: if `Running` and no event (heartbeat tier included) for 10 s, panel shows `Unresponsive` with a Kill button. No auto-kill — the user may be alt-tabbed at a debugger.
- **Protocol garbage.** Unparseable `[BRIDGE ...]` lines increment a counter, raw line goes to the log, decoder never throws. > 5% malformed → banner warning (version skew: stale exe vs new editor).
- **Version skew.** `session.hello` carries protocol + build id; editor compares against its supported range, downgrades gracefully (unknown events ignored by design), shows a "game build older than editor" notice.
- **Socket failure (v1).** Socket death while the process lives ≠ session death: bridge degrades to stdout-only (lifecycle events still flow), panel shows degraded badge, control disabled. One reconnect attempt; the session GUID makes reconnection unambiguous.
- **Editor crash.** Job Object kills the child. Shadow pak survives on disk for post-mortem.
- **Launch failure** (`CreateProcessA` fails, bad exe path): immediate `Failed` with the OS error string — already surfaced by the runner.

## 10. Security / safety boundaries

- **Loopback only.** v1 socket binds `127.0.0.1`, never `0.0.0.0`. No remote control surface, ever, in this design.
- **Session GUID as auth.** The GUID passed via child environment must be echoed in `hello`; connections without it are dropped. This defeats the "some other local process connects to the editor's port" case — sufficient for a single-user modding tool, and explicitly *not* a hostile-multi-user security boundary.
- **Read-only by construction until Phase 4.** The game-side command dispatcher ships with an allowlist containing exactly: `pause`, `resume`, `stop`, `subscribe`. Mutation commands aren't "disabled" — they don't exist in the dispatcher. Adding one is a deliberate code change with its own review, after observation is stable (anti-goal honored structurally, not by policy).
- **Bridge fully env-gated.** `MC2_BRIDGE` unset → `BridgeEmitter` compiles to early-return stubs, no socket code runs, no behavior change for normal players. Smoke gates (tier1) run with the bridge off to prove zero regression.
- **No editor-controlled code execution.** Commands are enum-dispatched with typed parameters. No "eval ABL string from socket" command, ever — that's the line between a debug bridge and a remote-code-execution hole in a shipped exe.
- **Filesystem.** Game reads only the pak path given on its command line (as today); the bridge adds no file-write commands. Shadow paks live under the mission directory the user already owns.

## 11. Implementation phases

Each phase ends green on tier1 smokes (5/5) with the bridge env-gated off, and is independently shippable.

- **Phase 0 — Supervisor hardening (editor only, no game changes).** `PlaytestSession` state machine over `EditorTaskRunner`; Job Object kill-on-close; Playtest Control + Session Log panels consuming raw lines + existing `[SMOKE v1]`/`[TIMING v1]` markers (the game already emits these — the editor just starts understanding them). Auto-save-to-shadow + validator gate. *Exit: launch/observe-raw/stop/restart loop is solid; crashes produce a retained log + exit code.*
- **Phase 1 — stdout protocol v0 (game emitter + editor decoder).** `BridgeEmitter` module (pattern of `gos_smoke.cpp`); `session.*`, `mission.*` lifecycle events; `object.manifest` + partId↔rid map; Mission Status panel. *Exit: editor shows live turn/time/objectives; clicking a manifest row selects the editor object.*
- **Phase 2 — Observation depth.** `object.census/destroyed`, `warn.*`, `perf.sample/hitch`; Runtime Objects, Warnings, Perf Strip panels; rate limiting + overflow accounting proven on a dense mission (metropolitan, 2600+ objects). *Exit: a full playtest of a tier1 mission yields a usable casualty + warning report with zero measurable game-side frame cost when idle.*
- **Phase 3 — Socket v1, observation only.** Connect-back handshake, framing, `subscribe`; lifecycle dual-emitted, `mover.transform` socket-only; Live Map Overlay; degradation to stdout on socket death. *Exit: tacmap shows live movers; killing the socket mid-run degrades cleanly; crash forensics still work via stdout.*
- **Phase 4 — Control (gate: observation stable across ≥ a few weeks of real editor use).** Command channel with ack/nack + frame-boundary drain; allowlist `pause/resume/stop` only; panel buttons wired to acked state. *Exit: pause from editor is reflected by game telemetry, not UI optimism.*
- **Phase 5 — Mutation (separate future design).** Candidates: teleport camera to editor viewport position, respawn/heal unit for iteration, objective force-complete. Each command gets its own safety review. Explicitly out of scope here; listed only so the dispatcher's allowlist architecture is designed for it.

## Decision log (why, briefly)

| Decision | Why |
|---|---|
| stdout v0 before socket | Pipe + reader thread + line parser + main-thread pump already exist and are smoke-proven; days to first panel |
| Keep stdout after v1 | Pipe survives crashes; socket buffers don't. stdout = forensics channel forever |
| Game connects to editor (v1) | Avoids firewall prompts on a listening game exe; editor already owns lifecycle |
| JSON frames, not binary | Modding-tool debuggability > throughput at expected rates; binary blob frame type reserved |
| watchID as rid | Append-only, never reused in-mission (objmgr.cpp:591) — free stability |
| partId for editor correlation | Already round-trips editor→.fit→runtime (`parts[i].objectWID`) |
| Restart = new process + re-snapshot | Only reliable global-state reset; picks up live edits — the actual iteration loop |
| Control allowlist starts at 4 entries | Anti-goal "no mutation until observation stable" enforced structurally |
| Job Object kill-on-close | Known orphaned-mc2.exe pain; supervisor death must reap children |
