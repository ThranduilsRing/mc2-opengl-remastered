---
name: mc2-amd-shader-review
description: Scan GLSL shader files for known AMD RX 7900 XTX driver violations before build. Reviews modified or specified shaders against docs/amd-driver-rules.md and reports issues with file paths, line numbers, and suggested fixes.
---

# MC2 AMD Shader Review

A pre-build review pass for GLSL shaders, scoped to the six AMD RX 7900 XTX driver rules that the MC2 engine has hit historically. Runs as a workflow skill: give it a list of shader files (or ask it to check all recently modified shaders); it reports violations file-by-line with suggested fixes.

## When to invoke

- Before building after any shader edit
- When `mc2-shader-expert` has just been consulted and the asker is about to deploy
- When `docs/amd-driver-rules.md` has been updated and existing shaders need re-validation
- As a pre-commit gate on any commit that touches `shaders/`

If the question is general GLSL syntax, uniform API, or UBO/SSBO layout, defer to `mc2-shader-expert` instead.

## Inputs

A list of GLSL files (`.vert`, `.frag`, `.tesc`, `.tese`, `.geom`, `.comp`) to review. Either:
- An explicit list (`shaders/gos_terrain.frag shaders/water.vert (removed)`)
- "All recently modified shaders" (use `git diff --name-only` + filter to shader extensions)
- "All shaders" (use `find shaders/ -name '*.vert' -o -name '*.frag' ...`)

## Rules to enforce

Read `docs/amd-driver-rules.md` for the canonical list. Then check each shader against these six critical rules:

1. **Attribute 0 must be active.** Every vertex shader must have `layout(location = 0)` AND actually use or read that attribute. A vertex shader with no attribute at location 0 produces silent draw skips on AMD.

2. **No `sampler2DArray` anywhere.** Use individual `sampler2D` on units 5-8 instead. Driver has historical issues with sampler arrays; the project rule is no arrays.

3. **Depth-only fragment shaders must write `gl_FragDepth`.** A fragment shader that writes nothing will be optimized away by the AMD driver. `gl_FragDepth = gl_FragCoord.z` is required for shadow depth passes.

4. **No texture feedback loops.** A texture bound as both a sampler (uniform) AND a framebuffer attachment in the same draw call. Flag any shader that samples from a texture that could also be the current depth/color FBO attachment (e.g., shadow map sampler active during shadow FBO render).

5. **Matrix transpose consistency.** Direct `glUniformMatrix4fv` uploads use `GL_FALSE` (row-major as-is). Shaders receiving matrices via the deferred system use `GL_TRUE`. Do not mix these in the same shader without clear documentation.

6. **No `#version` directive in shader file.** MC2 shaders must NOT have `#version` at the top. The string `"#version 430\n"` is prepended by `makeProgram()` to match the 4.3 context (required for SSBO / std430). A shader with its own `#version` causes a duplicate-version compile error.

## Workflow

1. Read each specified shader file fully.
2. Check each rule against the shader's contents.
3. For each violation, capture: file path, line number, rule violated, exact offending code, suggested fix.
4. If no violations found across all files, report `AMD driver check: PASS - N files reviewed`.
5. If violations found, report file-by-file with the format below.
6. Do NOT modify any shader file - this is review-only.

## Output format

```
=== AMD Driver Review ===

[PASS] shaders/gos_terrain.frag - no violations

[FAIL] shaders/example.vert (removed):12
  Rule: Attribute 0 must be active
  Found: layout(location = 1) in vec3 position;  (no location 0 attribute)
  Fix: Add layout(location = 0) in vec4 unused; and read it: float _dummy = unused.x;

[FAIL] shaders/sky.frag (removed):1
  Rule: No #version directive in shader file
  Found: #version 430 core
  Fix: Remove the line. makeProgram() prepends the version string.

Summary: 2 violations across 2 of N files.
```

## Cross-references

- `docs/amd-driver-rules.md` - canonical rules list with full rationale per rule
- `.claude/agents/mc2-shader-expert.md` - general GLSL / uniform API / UBO/SSBO advisor; defer non-AMD questions there
- `mclib/txmmgr.cpp` - `makeProgram()` site where `#version 430\n` is prepended (grep `makeProgram` for current line)
