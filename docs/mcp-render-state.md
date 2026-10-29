<!-- doc-status: current | reviewed:2026-10-11 -->
# MC2 Render-State MCP Server

Runtime bridge P2/P3: `get_runtime_observation()` reads the latest dump's mission
zone timings, brain/mover census, light pool and registry statistics.
`get_runtime_socket_state()` reads the harness's last v1 socket capture, including
session identity, timestamp, dropped samples and mover transforms.
Collect with `py -3 scripts/run_smoke.py --mission mc2_01 --runtime-bridge --exe <lane-install>/mc2.exe`.
Normal harness runs explicitly select no socket. No control commands exist.
Setting `MC2_BRIDGE=1` on the harness is equivalent to `--runtime-bridge` (for
determinism A/B runs); the harness always generates its own port and GUID.

Socket v1 uses a four-byte **big-endian** unsigned payload length (1..1048576),
then a UTF-8 JSON object. Every message has `version:1`, `type`, `session`.
The harness binds only 127.0.0.1 on an ephemeral port and supplies `MC2_BRIDGE=1`,
`MC2_BRIDGE_PORT`, `MC2_BRIDGE_SESSION` (fresh GUID); the game connects out.
Game `hello` includes `read_only:true` and `build_id` (git SHA). The authenticated parent sends
`subscribe` with `seq`, `streams:["runtime.observation","mover.transform"]`,
`hz` in 0.25..30. Game replies `ack` or `nack` with `seq`; unsupported commands
are always rejected. Wrong version/session or invalid framing closes the socket.
Telemetry contains `stream`, `data`, cumulative `dropped`. The state stream
carries the full render snapshot, even when file dumping is disabled. Transforms
carry part_id/handle/team/position/rotation, capped at 256 with explicit truncation;
these IDs are scoped to this session's current mission, not persistent save IDs.
One pending sample replaces older samples; main-thread publication never waits
for I/O. The worker makes at most one reconnect and falls back to existing stdout
and file observation on failure. The harness records `latest_runtime_bridge.json`
on completion; its timestamp describes the last sample, not current liveness.
P4 remains deferred: the design requires weeks of stable observation first.

Read-only MCP bridge from Claude agents to the running MC2 engine and smoke
artifacts. No engine mutation, no command queue, no networking inside mc2.exe.

## Quick start

**Step 1 — enable the state dump and launch the game**

```bat
set MC2_DEBUG_STATE_DUMP=1
set MC2_DEBUG_STATE_DUMP_HISTORY=1
cd A:\Games\mc2-opengl\mc2-win64-v0.4
mc2.exe
```

The engine writes `debug_state/latest_render_state.json` every 300 frames and
8 rolling history slots (`history_0.json` … `history_7.json`) when history is
enabled. The dump gate is **off by default** — no disk traffic without the env
var.

**Step 2 — open a Claude Code session in the worktree**

```
claude   (from A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\)
```

The project `.mcp.json` at the worktree root auto-registers `mc2-render-state`.
No manual `/mcp add` needed.

**Step 3 — confirm the server is connected**

In the Claude Code chat, type `/mcp`. You should see `mc2-render-state` listed
with status `connected`.

## Available tools

### State read tools (require game running with MC2_DEBUG_STATE_DUMP=1)

| Tool | Purpose |
|---|---|
| `get_render_state` | Full latest snapshot as JSON |
| `get_render_health` | Compact health summary — ok flag + all mismatch counters |
| `get_feature_gates` | All MC2_* feature gate states at last snapshot |
| `get_visual_settings` | IBL/SH ambient, PBR specular, dispatch path, material debug |
| `get_history(slot)` | One history ring slot (0=oldest, 7=newest); needs HISTORY=1 |
| `validate_state` | Schema validation — PASS or FAIL with specific errors |
| `get_frame_info` | Frame index, mission name, build config — lightweight orient |

### Capture and artifact tools (MC2-MCP-WRAPPER-2)

| Tool | Purpose |
|---|---|
| `run_capture_baseline(mission, duration)` | Single-mission smoke run; blocks ~duration+15s; returns report summary |
| `list_capture_sets` | All artifact dirs newest-first with per-mission results |
| `summarize_latest_capture` | Detailed report for the newest artifact dir |
| `get_latest_artifact_paths` | All files + sizes in the newest artifact dir |

`run_capture_baseline` wraps `run_smoke.py --tier adhoc --kill-existing --keep-logs`.
It terminates any running mc2.exe before starting, so do not call it while
manually running the game and expecting it to stay alive.

## Example workflow — quick health check

```
get_frame_info         # orient: which mission, what build
get_render_health      # check ok flag and mismatch counters
get_visual_settings    # inspect IBL/PBR strengths and dispatch path
validate_state         # full schema validation pass/fail
```

## Example workflow — regression check after a code change

```
run_capture_baseline("mc2_01", 30)   # run one mission smoke
summarize_latest_capture             # read the report
get_latest_artifact_paths            # locate log files if result != PASS
```

## Example workflow — IBL tuning session

1. Launch game with `MC2_DEBUG_STATE_DUMP=1`.
2. Adjust IBL strength via ImGui slider in-game.
3. Wait ~5s for next dump (300 frames at 60fps).
4. `get_visual_settings` — confirm new strength visible in snapshot.
5. `get_render_health` — confirm ok=true, no mismatch counters raised.

## Configuration reference

| Env var | Default | Effect |
|---|---|---|
| `MC2_DEBUG_STATE_DUMP` | off | Enable periodic JSON dump |
| `MC2_DEBUG_STATE_DUMP_DIR` | `debug_state` | Output directory (relative to mc2.exe CWD) |
| `MC2_DEBUG_STATE_DUMP_HISTORY` | off | Enable rolling 8-slot history ring |
| `MC2_DEPLOY_DIR` (MCP server) | `A:/Games/mc2-opengl/mc2-win64-v0.4` | Override deploy dir for state file reads |

`MC2_DEPLOY_DIR` is read by the MCP server process, not by mc2.exe. Set it in
`.mcp.json` `env` block if your deploy path differs from the default.

## Schema

Full field reference: `docs/debug_state_schema.md`

Validator: the `validate_state` MCP tool (the standalone check-debug-state-json.py script was retired 2026-10-12, cull wave 1).
