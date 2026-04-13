# MC2 OpenGL Toolkit Design

**Date:** 2026-04-12  
**Goal:** Package the MC2 OpenGL modernization work into a toolkit that enables autonomous development iteration, serves as a modding guide for MC2/retro-engine enthusiasts, and is ready for public GitHub release.

## Audience

- MC2 modders/fans who want better graphics and want to tweak terrain, shaders, materials
- Retro engine modders interested in how to bolt PBR/shadows onto a 2001 codebase
- Future maintainers (including the author) picking this up later

## Deliverable 1: Validation Mode

Add a `--validate` mode to mc2.exe that enables autonomous build-run-check iteration without manual intervention.

### Command-Line Interface

```
mc2.exe --validate [options]
  --mission <name>      # mission to load (default: first solo mission)
  --frames <N>          # frames to render before exit (default: 60)
  --screenshot <path>   # save final frame as TGA
  --log <path>          # write telemetry JSON to file
  --enable <feature>    # enable feature (bloom, shadows, grass, etc.)
  --disable <feature>   # disable feature
```

### Behavior

1. Parse command-line args before `Environment.init()`
2. Skip main menu / cinematics, load specified mission directly
3. Render N frames with a fixed camera position (deterministic)
4. Each frame: collect frame time, draw call count, shader compile errors, GL errors
5. Final frame: optionally `glReadPixels` → write TGA screenshot
6. Write telemetry summary to JSON log file
7. Exit with code 0 (success) or 1 (crash / GL error / shader compile failure)

### Telemetry Output (JSON)

```json
{
  "frames": 60,
  "avg_frame_ms": 16.2,
  "max_frame_ms": 34.1,
  "gl_errors": [],
  "shader_compile_errors": [],
  "draw_calls": 1847,
  "screenshot": "validate_out.tga",
  "exit_code": 0
}
```

### Files to Modify/Create

- `GameOS/gameos/gameosmain.cpp` — arg parsing, auto-exit after N frames, skip-to-mission logic
- `GameOS/gameos/gameos_graphics.cpp` — screenshot capture (`glReadPixels`), GL error collection
- Shader compilation paths — collect compile errors into a list instead of only logging to console
- **New:** `GameOS/gameos/gos_validate.cpp` / `.h` — telemetry struct, JSON serialization, screenshot writer

### Estimated Scope

~200-300 lines of C++. The rendering infrastructure already exists; this is plumbing around it.

## Deliverable 2: Modding Guide

Organize existing documentation (architecture docs, memory files, CLAUDE.md, design docs) into a cohesive guide. ~30% new prose, ~70% curation.

### Structure: `docs/modding-guide.md`

1. **Quick Start** — build (CMake + MSVC, RelWithDebInfo only), deploy (per-file cp), run validation mode
2. **Rendering Pipeline Overview** — render order, MRT/G-buffer, shadow pipeline, post-process chain
3. **Shader Modding Guide** — file map, #version prefix rule, hot-reload, adding post-process effects, uniform API
4. **Debug Hotkeys Reference** — full RAlt+key table with descriptions
5. **Texture Upscaling Pipeline** — Python scripts, realesrgan setup, loose file overrides, buffer size gotcha
6. **Known Issues & AMD Driver Quirks** — from amd-driver-rules.md and known issues
7. **Autonomous Development Guide** — using --validate mode, Claude iteration workflow, Tracy profiler

### Source Material

- `docs/architecture.md`
- `docs/amd-driver-rules.md`
- `CLAUDE.md` (hotkeys, known issues, key files)
- Memory files (lessons_learned, shadow_coordinate_spaces, deferred_vs_direct_uniforms, etc.)
- Existing design docs in `docs/plans/`

## Deliverable 3: GitHub-Ready Packaging

### Repository Cleanup

- Top-level `README.md` — project description, before/after screenshots, feature list, build instructions, link to modding guide
- `CHANGELOG.md` — summary of all rendering features added over vanilla MC2
- `.gitignore` updates — exclude `__pycache__/`, `esrgan_models/`, `realesrgan-ncnn-vulkan/`, `.claude/worktrees/`
- License note documenting MC2 source lineage (Microsoft open-sourced)

### Release Artifacts

- Pre-built `mc2.exe` (RelWithDebInfo)
- Full shader directory
- Texture upscaling scripts (`upscale_gpu.py`, etc.)
- Sample mission data for `--validate` mode

## Implementation Order

1. Validation mode (core C++ work)
2. Modding guide (documentation curation)
3. GitHub packaging (README, CHANGELOG, .gitignore, cleanup)

## Design Decisions

- **No separate exe** — validation mode lives inside mc2.exe to avoid maintaining two codebases and duplicating the renderer
- **TGA for screenshots** — MC2 already has TGA loading code, no new dependencies needed
- **JSON for telemetry** — simple hand-written serialization, no library dependency (the output is flat enough)
- **Fixed camera** — deterministic rendering for regression comparison; camera position baked or read from mission file
