# Dev Shell — live-edit playbook

The edit→see loop for MC2, no relaunch, no smoke run. Any agent (or human)
should be able to go from "here's a screenshot of what I want changed" to a
verified on-screen change using ONLY this page.

## The loop, end to end

```powershell
# 1. Launch straight into the screen you're iterating on (example: Mech Bay)
cd A:\Games\mc2-opengl\<deploy-lane>
$env:MC2_DEV_SHELL="1"; $env:MC2_NO_LAUNCHER="1"
$env:MC2_BOOT_TO_BAY="campaign"; $env:MC2_BOOT_TO_MISSION="mc2_03"; $env:MC2_BOOT_TO_SCREEN="bay"
.\mc2.exe
# (missions instead: .\mc2.exe -mission mc2_01 — skips all menus)

# 2. In a second shell: auto-reload on save (shaders + front-end .fit layouts)
py -3 <repo>\tools\dev_shell\watch.py A:\Games\mc2-opengl\<deploy-lane>

# 3. Edit files. Save. Look at the game. That's it.
```

## One-shot commands (`tools/dev_shell/mc2_cmd.py`)

| Command | What |
|---|---|
| `ping` | liveness + frame counter |
| `reload_shaders [--force] [--name PROG]` | recompile changed (or all) shader programs; reply carries the FULL GLSL error text; failed reload keeps the old program live |
| `screenshot --name X [--source backbuffer]` | TGA to `<deploy>/dev_shell_out/`. `backbuffer` for menus/front-end (they never touch sceneFBO), default `scene` for missions |
| `last_screenshot` | confirm the capture landed |
| `ui_reload` | re-read the ACTIVE front-end screen's `.fit` from `data/art/` and rebuild it in place |
| `texture_refresh --name SUBSTR` | re-read edited loose `.tga`s whose path contains SUBSTR from disk and re-upload (reply: `refreshed` + `skipped_no_file`; FST-only textures skip — drop a loose copy to override) |
| `framegraph [--collect true]` | pass graph JSON: contract passes, derived edges, per-pass GL state + draw counts + GPU ms. `--collect true` first, wait a second, call again |
| `get_gate/set_gate --name MC2_X [--value V]` | env peek/poke (honest: init-read gates need restart) |
| `quit` | clean exit |

## Direct boot targets

- Front-end grid: `MC2_BOOT_TO_BAY=<campaign>` + `MC2_BOOT_TO_MISSION=<mission>` + `MC2_BOOT_TO_SCREEN=purchase|bay|loadout|launch`. Campaign mission 1 skips logistics — always pass a later mission (mc2_03 works stock).
- Mission: `mc2.exe -mission <stem>` (quickstart, no logistics).

## What reloads live vs what doesn't

| Asset | Live? | How |
|---|---|---|
| Shaders (`shaders/**`) | YES | watcher or `reload_shaders`; errors come back in the reply |
| Front-end layout `.fit` (`data/art/mcl_*.fit`) | YES | loose file overrides FST; watcher or `ui_reload` |
| UI `.tga` pixels | YES | `texture_refresh --name <substr>` re-reads the loose file and re-uploads; node ids stay stable. FST-only textures: place a loose copy first (loose overrides FST on next refresh-load) |
| Gates read per-frame | YES | `set_gate` |
| Gates read once at init | NO | restart (the reply tells you) |

## Extending the shell

Upper layers register commands without linking games back into gameos:

```cpp
#include "../GameOS/gameos/gos_dev_shell.h"
static bool myCmd(const char* paramsJson, std::string& dataOut, std::string& errOut) { ... }
gos_dev_shell::registerCommand("my_cmd", myCmd);   // at init; inert when gate off
```

Built-ins live in `GameOS/gameos/gos_dev_shell.cpp`. Protocol: line-JSON in,
`{ok,error,version,data}` out, port 9877 (`MC2_DEV_SHELL_PORT`), localhost
only, main-thread execution at the frame-loop poll point.

## Verify discipline

This tooling replaces tier1 smoke for iteration work (user ruling 2026-07-04):
verify = boot → command → screenshot → look. Full tier1 only for merge gates
and load-bearing engine changes.
