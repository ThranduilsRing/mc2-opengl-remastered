# Docs Index

## Start here

- [docs/STARTER-GUIDE.md](STARTER-GUIDE.md) — orientation for new contributors/agents
- [../README.md](../README.md) — project overview
- [../BUILD-WIN.md](../BUILD-WIN.md) — build, deploy, first smoke test

## For modders

- [docs/modding-guide.md](modding-guide.md)
- [docs/beta/README_modder_beta.md](beta/README_modder_beta.md)
- [docs/modding-effects.md](modding-effects.md)
- [docs/modding-animations.md](modding-animations.md)
- [docs/asset-pipeline.md](asset-pipeline.md)
- [docs/GLOSSARY.md](GLOSSARY.md)

## Engine reference

- [docs/critical_inline_rules.md](critical_inline_rules.md)
- [docs/tier1_env_vars.md](tier1_env_vars.md)
- [.planning/codebase/ARCHITECTURE.md](../.planning/codebase/ARCHITECTURE.md) — codebase map
- [docs/render-contract.md](render-contract.md)
- [docs/cxx17-coding-rules.md](cxx17-coding-rules.md)
- [docs/disciplines.md](disciplines.md)
- [docs/known_issues.md](known_issues.md)

## Ledgers

- [docs/active_campaigns.md](active_campaigns.md)
- [docs/renderworld_arc_status.md](renderworld_arc_status.md)
- [docs/trackv-status.md](trackv-status.md)
- [docs/render-perf-snapshot.md](render-perf-snapshot.md)

## Tooling / Dev Harness Index

Core references:

- [docs/dev-shell.md](dev-shell.md) — live-edit loop (shader/UI hot-reload, one-shot commands, screenshots) without relaunching
- [docs/tier1_env_vars.md](tier1_env_vars.md) — every `MC2_*` gate: defaults, polarity, the add-a-gate checklist
- [docs/modding-guide.md §8](modding-guide.md#8-autonomous-development) — `--validate` headless validation mode + Tracy profiling
- [docs/release-checklist.md](release-checklist.md) — the zips-only release gate
- [docs/RELEASE-PROCESS.md](RELEASE-PROCESS.md) — end-to-end release runbook (version bump → package → gate → publish)
- [../CONTRIBUTING.md](../CONTRIBUTING.md) — branch/PR flow, mandatory build config, the tier-1 smoke gate

Fixtures shipped in the v0.5 cycle (all in-tree; one-liners — see
[docs/tier1_env_vars.md](tier1_env_vars.md) for full semantics):

- **Camera bookmarks** — the ImGui inspector's "Save Camera Bookmark" button
  (`code/gamecam.cpp`) writes a schema-valid bookmark JSON
  (`MC2_BOOKMARK_SAVE_DIR=<dir>`, default `saved_bookmarks/`);
  `MC2_VISUAL_BOOKMARK_CAPTURE=<bookmark.json>` replays the poses and captures
  frames at each one — the deterministic "see what I see" loop.
- **`MC2_CAMERA_PARK=x,y,rot,proj,alt[@frame]`** — one-shot camera pose pin at frame
  N (default 300), sim keeps running; stable captures without input scripting
  (`GameOS/gameos/gos_visual_capture.cpp`).
- **`MC2_DEBUG_VIEW=N`** — boot straight into a global full-scene debug render
  view (1=shadow 2=depth 3=albedo 4=overdraw 5=normals 6=chunk); the command
  channel's `debugview N` verb switches it at runtime.
- **`MC2_UI_COMMAND_FILE=<path>`** — headless UI driver: the engine consumes
  commands appended to the file (`toggle_pause`, `press pause.<BTN>`,
  `press dialog.YES|NO`, `debugview N`, `screenshot <name>`) and acks each
  line to `<path>.ack` (`code/missiongui.cpp`).
- **`MC2_BOOT_QUICKSAVE_AT=N` / `MC2_BOOT_QUICKLOAD_AT=M`** — one-shot
  quicksave/quickload at frame N/M via the same flags the pause-menu buttons
  set; M > N gives a deterministic save-then-load cycle in one headless run.
- **`tools/tracy_capture_compare.py`** — capture + diff Tracy profiles between
  two runs (zone-level attribution; landed with the Vulkan perf arc).

## Everything else

Point-in-time recon/slice reports and superseded docs live under [docs/archive/](archive/).
