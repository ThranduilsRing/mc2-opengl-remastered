# Editor Task Runner — Phase 1 (async terrain gen)

Branch `claude/terrain-gen-pcg`. Commits: EditorTaskRunner infra + MapGeneratorDialog async migration.

## What shipped
- `editor/EditorTaskRunner.{h,cpp}` — reusable worker-thread runner. CreateProcess +
  combined stdout/stderr pipe, `PROGRESS <pct> <stage> <msg>` parse, bounded log tail,
  cancel (cancel-file touch + TerminateProcess). Callbacks fire only from
  `PumpMainThread()` (main thread, called in `EditorInterface::update`). `RenderImGui()`
  = Task Monitor window (progress bar, log tree, Cancel).
- MapGeneratorDialog Preview/Generate now START a task and return — no UI freeze.
  Python launched `py -3 -u` (unbuffered → live PROGRESS). Terrain applied
  (`generateFromDialogParams`) ONLY in the success callback, main thread. Cancel/fail
  never applies. Generate camera/UI reseat behind `TakePostGenerateApplied()`.
- LoadPreset unchanged (synchronous file copy, no Python).

## Automated smoke (regression, ran 2026-06-09)
`py -3 scripts/run_editor_smoke.py --exe "<v0.4>/Mission Editor.exe" --keep-logs` → **6/6 PASS**
(gen_map_basic, foliage_present/missing/garbage, gen_save_load gen+save / load).
NOTE: `-gen-map` exercises the LEGACY MFC auto-gen path, not the ImGui dialog async
path — so this is a no-regression gate, not coverage of the new code. The async path
needs a real mouse click and is manual-only below.

## Manual smoke (the new async path)
Launch editor: `A:/Games/mc2-opengl/mc2-win64-v0.4/Mission Editor.exe`.
1. Tools toolbar → **Generate Map** → dialog opens.
2. Click **Preview (~3s)**. EXPECT: status "Running preview in background…", a
   **Task Monitor** window appears with a progress bar advancing, Preview/Generate
   buttons greyed while running. Viewport keeps redrawing (pan camera = responsive).
   On finish: thumbnail loads, Task Monitor shows "succeeded".
3. Click **Generate**. EXPECT: progress bar, responsive UI. On success the dialog
   closes, terrain applies, camera re-seats above the new surface, a sound plays.
4. Click **Generate** again, then **Cancel** in the Task Monitor mid-run. EXPECT:
   process dies, status "Generate cancelled (map unchanged)", the previous terrain
   is untouched (no partial apply).
5. Force a failure: rename/remove `tools/terrain_gen/terrain_gen.py`, click Generate.
   EXPECT: Task Monitor shows "FAILED" + exit code, expandable log tail, map unchanged.

## Known limitations / risks
- **Temp-dir atomicity TODO.** Generator writes straight into `terrain_gen_out/`. A
  cancel mid-write can leave partial files there; they are never APPLIED (no success
  callback) and the next good run overwrites. Future: generate to temp dir → atomic
  rename on success.
- One task at a time (Preview/Generate disabled while busy) — intentional for Phase 1
  (apply ordering would be ambiguous with overlapping runs).
- Async path is manual-smoke-only until the headless harness (other session) can drive
  ImGui clicks or a `-smoke-dialog-generate` CLI hook is added.
