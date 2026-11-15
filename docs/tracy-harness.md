<!-- doc-status: current | reviewed:2026-10-11 -->
# Tracy harness — capture, compare, baseline, regress

`tools/tracy_capture_compare.py` is the canonical way to profile MC2 with
[Tracy](https://github.com/wolfpld/tracy). It drives a headless smoke run,
records a `.tracy` trace, and turns traces into per-zone delta tables and
release-gate baselines. It is agent-runnable and process-safe (it refuses to
launch while any `mc2.exe` is already running, and only ever kills processes it
spawned).

## Prerequisites — the Tracy binaries (not committed)

The harness shells out to two Tracy command-line tools:

- **`tracy-capture.exe`** — the headless recorder.
- **`tracy-csvexport.exe`** — turns a `.tracy` file into per-zone-stats CSV.

These are large third-party binaries and are **deliberately not committed to
this repo**. Download them from the Tracy releases page
(<https://github.com/wolfpld/tracy/releases> — the "Windows binaries" archive
contains `tracy-capture.exe` and `tracy-csvexport.exe`), or build them from the
Tracy source tree.

Point the harness at wherever you unpacked them with the **`TRACY_TOOLS_DIR`**
environment variable:

```sh
export TRACY_TOOLS_DIR="C:/Users/<you>/Downloads/TracyProfiler"
```

If unset, the harness looks in `C:/Users/Joe/Downloads/TracyProfiler` (the
original author's location). `TRACY_TOOLS_DIR` is a **tooling** env var — it is
not an `MC2_*` runtime gate and is intentionally outside
check-env-registry.sh (retired in G8; the gate table is the registry) scope; its only home is this doc.

## Why this works headless

Every non-ASAN MC2 build compiles with `TRACY_ENABLE` + `TRACY_ON_DEMAND`
(`CMakeLists.txt`): the Tracy client buffers **nothing** until a recorder
connects. So the harness starts `tracy-capture.exe` first (it binds port 8086),
then launches the game — the game streams zones to the already-listening
recorder.

## Measurement hygiene (read before trusting a number)

- **NEVER fixed-timestep for FPS / frame-count comparison.**
  `MC2_SMOKE_FIXED_TIMESTEP=1` advances the sim a fixed 1/30s per frame, which
  decouples frame count from wall-clock render cost — great for deterministic
  golden-image capture, useless for "is it faster?". `baseline`/`regress` run
  **free-timestep**; `capture` opts in with `--free-timestep`.
- **Uncapped = `MC2_FPS_CAP=0`.** `baseline`/`regress` set it so the frame rate
  is render-bound, not limiter/vsync-bound.
- **NEVER `MC2_VK_PRESENT_VERIFY=1`.** It is a diagnostic readback that taxes the
  present path ~25% and skews every VK number. The harness never sets it.
- **VK arm needs a visible, non-minimized window.** A minimized window gives the
  VK present path a zero surface extent and it fail-softs to GL — you would then
  profile the GL present path while thinking you measured VK.
- **Pin the pose.** `baseline`/`regress` set `MC2_CAMERA_PARK` so a baseline
  and a later re-run measure the exact same scene.
- **The drawable is pinned to 1920x1080** (`MC2_WIDTH`/`MC2_HEIGHT` from `scripts/smoke_lib/render_resolution.py`; `--env MC2_WIDTH=... --env
  MC2_HEIGHT=...` overrides one run). Before 2026-10-12 a trace took whatever the install's options.cfg said, which is 3840x2160 on every install
  on this host, so traces and `release_assets/tracy-baselines` taken before that date are 4K and are **not comparable** with a new trace: re-take
  the baseline before a `regress`.
- **The render options are an input.** Each trace gets a `<trace>.options.json` (the install's options.cfg render options plus
  `MC2_OPTIONS_OVERRIDE`, `scripts/smoke_lib/render_options_pin.py`); `baseline.json` keeps them with the measurement. `compare` and `regress` refuse two runs whose
  options differ (`StaticPropPbr` on one side only is a different frame cost, not a regression) and refuse a run with no record; `--vary Key,Key`
  names the options that differ on purpose.
- **GPU memory is recorded next to the frame rate.** The engine plots the process's dedicated video memory to Tracy as `VRAM.Process.MB` (DXGI
  `QueryVideoMemoryInfo`, every 120 frames) and prints `[VRAM v1]` lines at the end of mission load and at the end of a smoke run
  (`GameOS/gameos/gpu_memory_report.cpp`: on VK the VMA budget/usage, on GL `GL_TEXTURE_FREE_MEMORY_ATI` on AMD or `GL_NVX_gpu_memory_info` on NVIDIA,
  and the DXGI process counter on both; -1 = that counter does not exist on the driver). `baseline.json` keeps the plot summary and the log samples per
  arm (`vram`), `summary.md` has a GPU memory table, `tools/perf_baseline_report.py` prints a GPU memory line per trace, the smoke report carries a
  `VRAM:` line per mission (`scripts/smoke_lib/vram.py`), and the debug-state dump has a `gpu_memory` object. A trace taken with an exe that predates the
  plot says so instead of staying silent.

## The arm

One arm, `vk`; the GL arm was retired on 2026-10-16 (`game_env` refuses it) because GL is deleted after VK parity.

| arm | env applied | measures |
|---|---|---|
| `vk` | `MC2_RENDER_BACKEND=vulkan`, `MC2_VK_SHARED_GBUFFER=1`, `MC2_VK_SHADOW_INTEROP=0` | the Vulkan present path + shared-gbuffer island umbrella, the user's real config |

The `vk` arm reflects the user's former config (backend + umbrella, shadow
interop OFF). Since 2026-10-15 the VK shadow producers are ON by default under
the umbrella (`MC2_VK_SHADOW_INTEROP` is now a kill switch), so this arm measures
the frame without shadows; `--env MC2_VK_SHADOW_INTEROP=1` measures the default
frame (the tool's own `=0` default is `tools/tracy_capture_compare.py`, not changed here).

> **Scope note:** the VK interop islands (water/shadow/terrain) have **no Tracy
> zones**. Their per-island timings come from `[VK_*_PERF]` log receipts parsed
> by `.claude/fence-defer/measure.py` — out of scope for this tool. The default
> zone allowlist covers the CPU frame zones shared by both backends: `DrawScreen`, `renderWaterFastPath`,
> `SwapWindow.SDL`, `Camera.UpdateRenderers`, `Terrain::IndirectDraw`.

## Zones and the frame-count proxy

`tracy-csvexport` reports per-zone stats (`total_ns`, `counts`, `mean_ns`, …).
Frame count is taken from the **`DrawScreen`** zone (`gameosmain.cpp`, the
top-level per-frame draw present in both backends): its `counts` over a fixed
recording window is the FPS proxy (`fps = frames / seconds`).

## Off-screen mid-mission UI changes

Declare `MC2_UI_COMMAND_FILE=<absolute path>` with `--env` (Tracy) or `--env-declare` (visual capture).
`MissionInterfaceManager::processUiCommandChannel` (`code/missiongui_uicmd.cpp:157`) polls each mission frame,
reads newline-separated commands, and **truncates in place**; no Renamed event occurs.
Wait for an empty file, write a complete batch, then wait with a deadline for new bytes in `<path>.ack`.
Receipts are `[f=N] <command> -> ok|blocked|unknown`; save the previous ack byte offset before sending.
Send `toggle_pause`, then `press pause.OPTIONS`, checking each ack and resulting state/screenshot.
This opens legacy pause options, not ImGui Graphics Options; there is no generic setting setter.
`debugview 3` changes the live view; `screenshot after_change` needs `MC2_DEV_SHELL=1`.
For settings without a verb, use `MC2_OPTIONS_OVERRIDE` before launch; mid-mission hot-apply remains unproven.

## Subcommands

### `capture` — one trace

```sh
py -3 tools/tracy_capture_compare.py capture \
    --install A:/Games/mc2-opengl/mc2-win64-0.4c --arm vk \
    --mission mc2_01 --seconds 30 -o vk_before.tracy
# add --free-timestep --fps-cap 0 --pose "x,y,rot,proj,alt@600" for FPS work
```

### `compare` — per-zone delta table of two traces

```sh
py -3 tools/tracy_capture_compare.py compare before.tracy after.tracy \
    [--zones DrawScreen,SwapWindow.SDL]
```

### `baseline` — capture the VK arm at a pose → dated baseline dir

Records `vk` (the only arm; the GL arm was retired with GL deletion step 1) at the canonical pose, uncapped
free-timestep, and writes a baseline directory with the trace, a machine-readable `baseline.json`, and
a human `summary.md` table.

```sh
py -3 tools/tracy_capture_compare.py baseline \
    --install A:/Games/mc2-opengl/mc2-win64-0.4c
# defaults: --mission mc2_10, the mc2_10 corner-park pose, 15s, arm vk,
#           --name <UTC date>, out-dir release_assets/tracy-baselines/<name>
```

Default pose (`MC2_CAMERA_PARK`): `-6656,-6656,150.625,10.9094,6045.2358@600`
— a busy corner of the largest map (`mc2_10`), parked at frame 600 so the scene
is settled before the 15s measurement window.

Output lands under `release_assets/tracy-baselines/<name>/` (that tree is
gitignored, so the multi-MB traces are never committed). For a **release**
baseline, pass `--out-dir release_assets/<version>/tracy` — see
[RELEASE-PROCESS.md](RELEASE-PROCESS.md) §3a.

### `regress` — re-capture + compare against a baseline

Re-captures the VK arm with the baseline's exact settings and diffs against it.

```sh
py -3 tools/tracy_capture_compare.py regress \
    --baseline release_assets/tracy-baselines/2026-07-23 \
    --install A:/Games/mc2-opengl/mc2-win64-0.4c
```

Thresholds (on frame-count **drop**):

| drop | verdict | exit |
|---|---|---|
| ≤ 5% | PASS | 0 |
| > 5% | WARN | 1 |
| > 10% | FAIL | 2 |

Tune with `--warn-pct` / `--fail-pct`. The report (`regress.md` in the output
dir) also carries the per-zone mean deltas, so a WARN/FAIL can be
attributed to a specific zone.

## Lane / safety notes

- Run a lane-check (`mc2-repo-intel` `deploy_status`) before capturing against a
  deploy lane, exactly as for a smoke run — you are launching the game.
- Keep runs short (the 15s default respects the standing short-run rule).
- The harness aborts if an `mc2.exe` is already running; it never kills a
  process it did not spawn.
