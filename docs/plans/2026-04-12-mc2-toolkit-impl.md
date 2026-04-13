# MC2 OpenGL Toolkit Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a `--validate` mode to mc2.exe for autonomous dev iteration, write a modding guide, and prepare the repo for GitHub release.

**Architecture:** The validation mode hooks into the existing `main()` loop in `gameosmain.cpp` and the existing `-mission` quick-start path in `mechcmd2.cpp`. A new `gos_validate.h/.cpp` module tracks telemetry (frame times, GL errors, shader errors) and writes JSON + TGA screenshot on exit. The modding guide curates existing documentation. GitHub packaging adds README, CHANGELOG, and .gitignore updates.

**Tech Stack:** C++ (MSVC), OpenGL 4.2, SDL2, CMake, TGA file format

---

## Task 1: Create validation telemetry module (gos_validate.h/.cpp)

**Files:**
- Create: `GameOS/gameos/gos_validate.h`
- Create: `GameOS/gameos/gos_validate.cpp`

**Step 1: Write the header**

```cpp
// gos_validate.h - Validation mode telemetry for autonomous dev iteration
#pragma once

#include <vector>
#include <string>

struct ValidateConfig {
    bool enabled;           // --validate flag present
    int maxFrames;          // --frames N (default 60)
    char screenshotPath[512]; // --screenshot path (empty = no screenshot)
    char logPath[512];      // --log path (default "validate.json")
    // Feature toggles parsed from --enable/--disable
    int bloomOverride;      // -1=no override, 0=off, 1=on
    int shadowsOverride;
    int fxaaOverride;
    int grassOverride;
};

struct ValidateTelemetry {
    int framesRendered;
    float totalFrameMs;
    float maxFrameMs;
    std::vector<std::string> glErrors;
    std::vector<std::string> shaderErrors;
    int exitCode;           // 0=success, 1=error
};

// Global access
ValidateConfig& getValidateConfig();
ValidateTelemetry& getValidateTelemetry();

// Called from main() before GetGameOSEnvironment
void validateParseArgs(int argc, char** argv);

// Called from shader_builder on compile/link error
void validateRecordShaderError(const char* msg);

// Called from draw_screen each frame
void validateRecordFrame(float frameMs);

// Called from main loop to check if we should exit
bool validateShouldExit();

// Called on exit: writes JSON log + optional TGA screenshot
void validateWriteResults(int viewportW, int viewportH);
```

**Step 2: Write the implementation**

```cpp
// gos_validate.cpp
#include "gos_validate.h"
#include <cstdio>
#include <cstring>
#include <GL/glew.h>

static ValidateConfig s_config = {};
static ValidateTelemetry s_telemetry = {};

ValidateConfig& getValidateConfig() { return s_config; }
ValidateTelemetry& getValidateTelemetry() { return s_telemetry; }

void validateParseArgs(int argc, char** argv) {
    memset(&s_config, 0, sizeof(s_config));
    s_config.maxFrames = 60;
    s_config.bloomOverride = -1;
    s_config.shadowsOverride = -1;
    s_config.fxaaOverride = -1;
    s_config.grassOverride = -1;
    strncpy(s_config.logPath, "validate.json", sizeof(s_config.logPath));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--validate") == 0) {
            s_config.enabled = true;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            s_config.maxFrames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            strncpy(s_config.screenshotPath, argv[++i], sizeof(s_config.screenshotPath) - 1);
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            strncpy(s_config.logPath, argv[++i], sizeof(s_config.logPath) - 1);
        } else if (strcmp(argv[i], "--enable") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "bloom") == 0) s_config.bloomOverride = 1;
            else if (strcmp(argv[i], "shadows") == 0) s_config.shadowsOverride = 1;
            else if (strcmp(argv[i], "fxaa") == 0) s_config.fxaaOverride = 1;
            else if (strcmp(argv[i], "grass") == 0) s_config.grassOverride = 1;
        } else if (strcmp(argv[i], "--disable") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "bloom") == 0) s_config.bloomOverride = 0;
            else if (strcmp(argv[i], "shadows") == 0) s_config.shadowsOverride = 0;
            else if (strcmp(argv[i], "fxaa") == 0) s_config.fxaaOverride = 0;
            else if (strcmp(argv[i], "grass") == 0) s_config.grassOverride = 0;
        }
    }
}

void validateRecordShaderError(const char* msg) {
    if (s_config.enabled) {
        s_telemetry.shaderErrors.push_back(msg);
        s_telemetry.exitCode = 1;
    }
}

void validateRecordFrame(float frameMs) {
    s_telemetry.framesRendered++;
    s_telemetry.totalFrameMs += frameMs;
    if (frameMs > s_telemetry.maxFrameMs) s_telemetry.maxFrameMs = frameMs;
}

bool validateShouldExit() {
    return s_config.enabled && s_telemetry.framesRendered >= s_config.maxFrames;
}

static void writeScreenshotTGA(const char* path, int w, int h) {
    unsigned char* pixels = new unsigned char[w * h * 3];
    glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, pixels);

    FILE* f = fopen(path, "wb");
    if (!f) { delete[] pixels; return; }

    // TGA header (uncompressed RGB)
    unsigned char header[18] = {};
    header[2] = 2; // uncompressed true-color
    header[12] = w & 0xFF; header[13] = (w >> 8) & 0xFF;
    header[14] = h & 0xFF; header[15] = (h >> 8) & 0xFF;
    header[16] = 24; // bits per pixel

    fwrite(header, 1, 18, f);
    fwrite(pixels, 1, w * h * 3, f);
    fclose(f);
    delete[] pixels;

    fprintf(stderr, "VALIDATE: Screenshot saved to %s\n", path);
}

void validateWriteResults(int viewportW, int viewportH) {
    if (!s_config.enabled) return;

    // Screenshot first (while GL context is still active)
    if (s_config.screenshotPath[0]) {
        writeScreenshotTGA(s_config.screenshotPath, viewportW, viewportH);
    }

    // Check for accumulated GL errors
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        char buf[64];
        snprintf(buf, sizeof(buf), "GL_ERROR: 0x%04X", err);
        s_telemetry.glErrors.push_back(buf);
        s_telemetry.exitCode = 1;
    }

    float avgMs = s_telemetry.framesRendered > 0
        ? s_telemetry.totalFrameMs / s_telemetry.framesRendered : 0.0f;

    // Write JSON
    FILE* f = fopen(s_config.logPath, "w");
    if (!f) {
        fprintf(stderr, "VALIDATE: Failed to write log to %s\n", s_config.logPath);
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"frames\": %d,\n", s_telemetry.framesRendered);
    fprintf(f, "  \"avg_frame_ms\": %.1f,\n", avgMs);
    fprintf(f, "  \"max_frame_ms\": %.1f,\n", s_telemetry.maxFrameMs);

    fprintf(f, "  \"gl_errors\": [");
    for (size_t i = 0; i < s_telemetry.glErrors.size(); i++) {
        fprintf(f, "\"%s\"%s", s_telemetry.glErrors[i].c_str(),
                i + 1 < s_telemetry.glErrors.size() ? ", " : "");
    }
    fprintf(f, "],\n");

    fprintf(f, "  \"shader_errors\": [");
    for (size_t i = 0; i < s_telemetry.shaderErrors.size(); i++) {
        fprintf(f, "\"%s\"%s", s_telemetry.shaderErrors[i].c_str(),
                i + 1 < s_telemetry.shaderErrors.size() ? ", " : "");
    }
    fprintf(f, "],\n");

    if (s_config.screenshotPath[0])
        fprintf(f, "  \"screenshot\": \"%s\",\n", s_config.screenshotPath);

    fprintf(f, "  \"exit_code\": %d\n", s_telemetry.exitCode);
    fprintf(f, "}\n");
    fclose(f);

    fprintf(stderr, "VALIDATE: Results written to %s (exit_code=%d, %d frames, %.1fms avg)\n",
            s_config.logPath, s_telemetry.exitCode, s_telemetry.framesRendered, avgMs);
}
```

**Step 3: Add to CMakeLists.txt**

Find the `mc2` target source list and add `GameOS/gameos/gos_validate.cpp`.

Run: Search for existing `gos_postprocess.cpp` in CMakeLists.txt to find the right source list, add `gos_validate.cpp` adjacent.

**Step 4: Commit**

```bash
git add GameOS/gameos/gos_validate.h GameOS/gameos/gos_validate.cpp CMakeLists.txt
git commit -m "feat: add validation telemetry module (gos_validate)"
```

---

## Task 2: Wire validation into main loop (gameosmain.cpp)

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp`

**Step 1: Add includes and arg parsing**

At top of file, add:
```cpp
#include "gos_validate.h"
```

In `main()`, before the `GetGameOSEnvironment(cmdline)` call at line 315, add:
```cpp
    // Parse validation args before GameOS consumes the command line
    validateParseArgs(argc, argv);
```

**Step 2: Add frame telemetry and auto-exit to main loop**

In the main loop (line 391-415), after `frameRate = 1000.0f / (float)dt;` (line 414), add:
```cpp
        // Validation mode: record frame and check exit condition
        if (getValidateConfig().enabled) {
            validateRecordFrame(1000.0f / frameRate);
            if (validateShouldExit()) {
                validateWriteResults(Environment.drawableWidth, Environment.drawableHeight);
                break;
            }
        }
```

**Step 3: Apply feature overrides**

In `draw_screen()`, after the post-process `pp` is obtained (line 145), add:
```cpp
    // Apply validation mode feature overrides
    if (pp && getValidateConfig().enabled) {
        ValidateConfig& vc = getValidateConfig();
        if (vc.bloomOverride >= 0) pp->bloomEnabled_ = vc.bloomOverride;
        if (vc.shadowsOverride >= 0) pp->shadowsEnabled_ = vc.shadowsOverride;
        if (vc.fxaaOverride >= 0) pp->fxaaEnabled_ = vc.fxaaOverride;
    }
```

**Step 4: Set exit code on normal termination**

After the main loop, before `Environment.TerminateGameEngine()`, ensure validation results are written if we haven't already (handles the case where the game exits via SDL_QUIT or gosExitGameOS before frame limit):
```cpp
    if (getValidateConfig().enabled && !validateShouldExit()) {
        // Game exited before frame limit — still write results
        validateWriteResults(Environment.drawableWidth, Environment.drawableHeight);
    }
```

**Step 5: Build and verify compile**

Run: `/mc2-build`

**Step 6: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat: wire validation mode into main loop with auto-exit and feature overrides"
```

---

## Task 3: Hook shader error collection into shader_builder

**Files:**
- Modify: `GameOS/gameos/utils/shader_builder.cpp`

**Step 1: Add include**

At top of file, add:
```cpp
#include "gos_validate.h"
```

**Step 2: Record shader compile errors**

In `get_shader_error_status()` (line 62), after `fprintf(stderr, "SHADER COMPILE ERROR: %s\n", buf);` (line 77), add:
```cpp
            validateRecordShaderError(buf);
```

**Step 3: Record shader link errors**

In `get_program_error_status()` (line 87), after `fprintf(stderr, "SHADER LINK ERROR: %s\n", buf);` (line 102), add:
```cpp
            validateRecordShaderError(buf);
```

**Step 4: Build and verify**

Run: `/mc2-build`

**Step 5: Commit**

```bash
git add GameOS/gameos/utils/shader_builder.cpp
git commit -m "feat: collect shader compile/link errors for validation telemetry"
```

---

## Task 4: Wire -mission into validate mode for auto-start

**Files:**
- Modify: `code/mechcmd2.cpp`

**Step 1: Add include and default mission for validate mode**

At top of file (near other includes), add:
```cpp
#include "gos_validate.h"
```

In `ParseCommandLine()` at the end (after the while loop, around line 2530), add:
```cpp
    // If --validate was passed but no -mission, default to first solo mission
    if (getValidateConfig().enabled && !justStartMission) {
        justStartMission = true;
        strcpy(missionName, "mis0101");
    }
```

This ensures `--validate` always loads a mission even without `-mission` flag.

**Step 2: Disable sound in validate mode**

In `GetGameOSEnvironment()`, after `ParseCommandLine(CommandLine)` (line 2643), add:
```cpp
    // Force sound off in validate mode (no audio device needed)
    if (getValidateConfig().enabled) {
        useSound = false;
    }
```

**Step 3: Build and verify**

Run: `/mc2-build`

**Step 4: Commit**

```bash
git add code/mechcmd2.cpp
git commit -m "feat: auto-start mission and disable sound in validate mode"
```

---

## Task 5: Deploy and test validation mode

**Step 1: Deploy**

Run: `/mc2-deploy`

**Step 2: Test basic validation**

Run the game in validate mode from the deploy directory:
```bash
cd "A:/Games/mc2-opengl/mc2-win64-v0.1.1" && ./mc2.exe --validate --frames 30 --log validate_test.json --screenshot validate_test.tga
```

Wait for it to exit (should take ~30 seconds).

**Step 3: Check results**

Read `validate_test.json` and verify:
- `frames` is 30
- `exit_code` is 0
- `shader_errors` is empty
- `avg_frame_ms` is reasonable (10-60ms)

Check that `validate_test.tga` exists and has non-zero size.

**Step 4: Commit validation skill**

Create `.claude/skills/mc2-validate.md`:
```markdown
---
name: mc2-validate
description: Run mc2.exe in validation mode — renders N frames, captures telemetry, exits
---

# MC2 Validate

Run the deployed mc2.exe in validation mode for autonomous testing.

## Steps

1. **Run validation**:
```bash
cd "A:/Games/mc2-opengl/mc2-win64-v0.1.1" && ./mc2.exe --validate --frames 60 --log validate.json --screenshot validate.tga
```

2. **Check results**: Read `validate.json` for exit_code, shader_errors, frame timing.

3. **Check screenshot**: If screenshot was requested, verify file exists and has content.

## Options
- `--frames N` — number of frames (default 60)
- `--screenshot path` — save final frame as TGA
- `--log path` — telemetry JSON output (default validate.json)
- `--enable feature` / `--disable feature` — toggle bloom/shadows/fxaa/grass
- `-mission name` — which mission to load (default mis0101)
```

```bash
git add .claude/skills/mc2-validate.md
git commit -m "feat: add mc2-validate skill for autonomous validation"
```

---

## Task 6: Write modding guide

**Files:**
- Create: `docs/modding-guide.md`

**Step 1: Write the guide**

Create `docs/modding-guide.md` with 7 sections. Content is curated from:
- CLAUDE.md (build rules, key files, hotkeys, known issues)
- Memory files (lessons_learned, shadow_coordinate_spaces, deferred_vs_direct_uniforms, terrain_texture_tuning, water_rendering_architecture)
- Design docs in `docs/plans/` (recover from nifty-mendeleev branch if needed)

Sections:
1. **Quick Start** — prerequisites (MSVC 2022, CMake, GLEW, SDL2), build command, deploy steps, run validation
2. **Rendering Pipeline** — render order diagram, MRT setup, shadow pipeline (static 4096x4096 + dynamic 2048x2048), post-process chain (bloom threshold → blur → shadow pass → FXAA → tonemap)
3. **Shader Modding** — file map table (each .frag/.vert and what it does), #version prefix rule, hot-reload behavior, uniform API (setFloat/setInt before apply()), how to add a post-process effect
4. **Debug Hotkeys** — table: RAlt+F1 bloom, RAlt+F2 shadow debug, RAlt+F3 shadows, RAlt+F5 FXAA, RAlt+4 screen shadows, RAlt+5 grass, RAlt+6 god rays, RAlt+7 shorelines, RAlt+9 SSAO, F6-F12 tess params, [/] shadow softness
5. **Texture Upscaling** — realesrgan-ncnn-vulkan setup, upscale_gpu.py usage, loose file override system, MAX_LZ_BUFFER_SIZE fix
6. **Known Issues & Driver Quirks** — post-process on HUD, shadow banding, AMD sampler2DArray crash, attribute 0 rule, gl_FragDepth precision
7. **Autonomous Development** — validation mode usage, Claude workflow (edit → build → validate → read JSON → iterate), Tracy profiler connection

Keep each section focused and practical. Code examples where helpful.

**Step 2: Commit**

```bash
git add docs/modding-guide.md
git commit -m "docs: add comprehensive modding guide"
```

---

## Task 7: GitHub packaging — README

**Files:**
- Create: `README.md`

**Step 1: Write README**

Include:
- Project title and one-line description
- Feature list (PBR terrain, tessellation, shadows, bloom, FXAA, tonemapping, skybox, texture upscaling, validation mode)
- Build instructions (brief, link to modding guide for details)
- Quick start (build → deploy → run)
- Validation mode example
- Link to modding guide
- License/attribution note (original MC2 source by Microsoft/FASA, this fork by [author])
- Credit for rendering additions

**Step 2: Commit**

```bash
git add README.md
git commit -m "docs: add README for GitHub release"
```

---

## Task 8: GitHub packaging — CHANGELOG and .gitignore

**Files:**
- Create: `CHANGELOG.md`
- Modify: `.gitignore`

**Step 1: Write CHANGELOG**

Summarize all rendering features from git log. Group by category:
- Terrain: PBR splatting, normal mapping, POM, tessellation
- Lighting: shadow maps (static + dynamic), post-process shadow pass
- Post-Processing: bloom, FXAA, tonemapping, skybox
- Effects: grass (geometry shader), cloud shadows, height fog, triplanar cliffs
- Tools: texture upscaling pipeline, validation mode, Tracy profiling, debug hotkeys
- Infrastructure: G-buffer MRT, shader hot-reload, loose file overrides

**Step 2: Update .gitignore**

Append to existing .gitignore:
```
# Claude worktrees and cache
.claude/worktrees/
__pycache__/

# AI upscaling models and tools (large binaries)
esrgan_models/
realesrgan-ncnn-vulkan/

# Upscaled texture output
mc2srcdata/

# Validation output
validate.json
validate*.tga
```

**Step 3: Commit**

```bash
git add CHANGELOG.md .gitignore
git commit -m "docs: add CHANGELOG, update .gitignore for GitHub release"
```

---

## Task 9: Final verification

**Step 1: Full build-deploy-validate cycle**

Run: `/mc2-build-deploy`
Then: Run validation mode and confirm JSON output is clean.

**Step 2: Review all new/modified files**

Check that nothing was accidentally broken. Review the git log for this session.

**Step 3: Final commit if needed**

Any fixups from verification.
