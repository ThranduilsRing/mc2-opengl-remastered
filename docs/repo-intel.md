<!-- doc-status: current | reviewed:2026-11-08 -->
# Repo intel tools (moved from CLAUDE.md, 2026-11-08)

Use these before grepping blind. Route by question:

Shader iteration: `py -3 scripts/deploy_payload.py <lane-install> --ninja --allow-new-target --shaders-only` rebuilds only Vulkan shaders, verifies copies, preserves unrelated manifest rows, and tags smoke/capture provenance `shader-dirty` until a full committed build/deploy.

- literal string, env key or filename: `rg`
- C++ declaration, references, types, overloads: clangd (LSP plugin; compile DB from `py -3 scripts/configure_analysis.py`, re-run after CMake changes)
- how subsystems connect: graphify
- gates, env vars, bindings, slices: `repo_query.py`

"Zero references" means deletion *candidate* only: check data, scripts and other configs too.

## repo_query and friends

- `py -3 tools/repo_intel/repo_query.py preflight`: session-start sanity check
- `py -3 tools/repo_intel/repo_query.py env --domain <shadow|terrain|...>` / `env --all`: which `MC2_*` vars exist, defaults, read sites
- `py -3 tools/repo_intel/repo_query.py binding --conflicts` / `binding <N>`: shader binding-slot index
- `tools/repo_intel/grep_tool.py`: `grep_source()` / `symbol_lookup()` (import it; lexical, not AST)
- `tools/repo_intel/gate_status.py`, `dead_gate_scan.py`: gate defaults and dead-gate candidates (gate amnesty)

## Visual A/B (ab.py)

`py -3 tools/visual_verify/ab.py --bookmarks a,b (--install-a DIR --install-b DIR | --exe-a EXE --exe-b EXE [--install DIR]) [--crop x0,y0,x1,y1] [--env K=V]`

Vulkan-only before/after from two builds (the engine has no GL arm): montage, crops, diff heatmap and a MAD table. `--compare-only DIR_A DIR_B` reuses captures. Always A/B a day AND a night map (`mc2_01_saved_205644`, `mc2_10_saved_204523`). MAD is NOT a pass signal: mc2_10 with every building missing scored MAD 0.62. Look at the montage. Captures exit as soon as the PNG and sidecar are complete; the duration is only a timeout. See also [visual-verify.md](visual-verify.md).

A/B captures default to 3840x2160; `--res 1080` opts out, and explicit `--env MC2_WIDTH=... --env MC2_HEIGHT=...` overrides remain supported. Each invocation stages in a unique directory under `MC2_LANE_SCRATCH`, or under `<install>/../_ab-stage` when unset; `--stage-root DIR` overrides the parent. Cleanup removes only that invocation's stages and private directory, retaining the parent; `--keep-stage` retains the stages too.

## graphify

AST-only graph of owned code: `graphify-out/graph.json`, report `graphify-out/GRAPH_REPORT.md`. Run from the repo root: `graphify query "<question>"`, `graphify path "A" "B"`, `graphify explain "gosRenderer"`. Community labels are generic.

- Rebuild: the file list is `graphify-out/.scoped_files.json` (GameOS mclib code RenderCore GuiRuntime shaders scripts tools). `graphify update` rescans the whole root including 3rdparty, so re-run the scoped script instead.
- Use it for call-graph and coupling questions (who calls X, cross-directory hubs). INFERRED edges are heuristic: verify with grep.
- Subagent briefs: name the relevant memory files and these tools up front.

## Release / fallback build (VS tree)

Fresh tree: `py -3 scripts/bootstrap_worktree_build.py --build`; then
`"$CMAKE" --build build64 --config RelWithDebInfo --target mc2` and deploy without `--ninja`.
CMake: `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`.
