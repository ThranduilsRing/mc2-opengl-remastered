# INFO-IMGUI-SLICE-2 SPEC (2026-07-15)

Worktree: A:/Games/mc2-ui-phase1, branch claude/ui-phase1-integration, base HEAD 5073aff3.
Predecessor: INFO-IMGUI-SLICE-1 (`c54b4bf8` + `bf0e4e0e`) — MC2_UI_IMGUI_INFO=1 renders the
in-mission unit-Info panel as ImGui (header + name + health bar + loadout child). Read
code/infowindow.cpp (both legacy render and the ImGui path) BEFORE coding.

## Goal (three deferred items from slice-1)
1. **Mech paperdoll icon**: legacy draws it via the unit icon (`icon->renderUnitIcon` — find the
   exact call in InfoWindow::render / related; the ImGui panel currently leaves that area blank).
   Reproduce in the ImGui path: either call the same legacy draw into the panel rect (it draws
   via gos quads — may compose fine under ImGui since HUD z-order already works), or sample the
   icon texture into an ImGui Image. Choose the smallest-diff approach that pixel-matches
   `.claude/info_window_legacy_ref.png` (paperdoll schematic region).
2. **Font match**: legacy panel text is bitmap-font ALL-CAPS. ImGui default font is mixed-case
   sans. Minimum: uppercase all strings in the ImGui path. Better (if cheap): load the legacy
   bitmap font into ImGui atlas — ONLY if an existing helper exists (grep for ImGui font loading
   in the repo, e.g. gos_imgui*.cpp); do NOT build a font importer from scratch this slice.
3. **Pilot/skill rows**: legacy InfoWindow shows pilot name/skills rows (see legacy render +
   the ref PNG). Add to the ImGui panel with content parity — if legacy uses a shared gather
   helper, reuse; if it draws inline, factor a shared gather like gatherWeaponRows was (BOTH
   paths call it — never duplicate enumeration logic).

## Hard rules (verbatim from lane discipline)
- COMMIT FIRST → build → deploy. Never deploy dirty (exe embeds sha+dirty).
- Build: `& "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo --target mc2` from A:/Games/mc2-ui-phase1. RelWithDebInfo always.
- Deploy: `py -3 scripts/deploy_payload.py A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0 --allow-new-target --source-root . --build-dir build64/RelWithDebInfo --exe-name mc2.exe` (check exact arg shape against script --help; --build-dir points at RelWithDebInfo per lane note).
  After EVERY deploy: `cp .claude/run-with-log.bat.deploy-backup A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0/run-with-log.bat`.
- New env vars (should not need any) register in RenderCore/RendererFeatureRegistry.h; run scripts/check-env-registry.sh.
- ONE game process: `Get-Process mc2 -ErrorAction SilentlyContinue` must be EMPTY before launch; kill by exact PID the moment the screenshot lands; zero-count after.
- EYEBALL PIXELS: acceptance = a PNG you Read, not "it built". Convert TGA→PNG via tools/dev_shell/shot.py png.
- Gate-off byte-identical: MC2_UI_IMGUI_INFO unset ⇒ legacy path unchanged. Then tier1 smoke gate-OFF:
  `$env:MC2_DEBUG_STATE_DUMP="1"; $env:MC2_DIAGNOSTIC_TRACE_FILE="debug_state/diagnostic_trace.jsonl"; $env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE"; py -3 A:\Games\mc2-main\scripts\run_smoke.py --tier tier1 --duration 30 --keep-logs`
  (run from A:/Games/mc2-ui-phase1; NEVER --kill-existing, NEVER --with-menu-canary, NEVER duration>30).
- DO NOT TOUCH A:/Games/mc2-main working tree (read-only OK, e.g. run_smoke.py path above).

## Verify recipe (headless capture, gate ON)
Env: MC2_UI_IMGUI_INFO=1 MC2_BOOT_INFO=700 MC2_SCREENSHOT_AT_FRAME=4200
MC2_SCREENSHOT_SOURCE=backbuffer MC2_NO_LAUNCHER=1 MC2_LOG=1, run from the 0.5.0 install
(run-with-log.bat or mc2.exe direct). Screenshot TGA lands per MC2_SCREENSHOT_AT_FRAME
convention (grep gameosmain.cpp for the output path). Compare vs `.claude/info_window_legacy_ref.png`:
paperdoll present, name+health unchanged, loadout rows unchanged, pilot rows present, text ALL-CAPS.
Also capture gate-OFF same recipe minus MC2_UI_IMGUI_INFO → legacy unchanged.

## Deliverables
- Commits on claude/ui-phase1-integration (feat + any fix), pushed nowhere (local only).
- Capture PNGs in .claude/ (info_imgui_slice2.png, info_legacy_gateoff_slice2.png).
- Status block appended to .claude/HANDOFF-ui-phase1-bugs-2026-07-05.md (match existing style, terse).
- Final report: what shipped, commit shas, capture paths, tier1 verdict, anything deferred.
