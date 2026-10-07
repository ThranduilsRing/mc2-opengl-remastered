---
name: mc2-validate
description: Run mc2.exe in validation mode - renders N frames, captures telemetry + screenshot, exits with status code
---

# MC2 Validate

Run the deployed mc2.exe in validation mode for autonomous build-test iteration.

## Prerequisites

Build and deploy first using `/mc2-build-deploy`.

## Steps

1. **Run validation**:
```bash
cd "A:/Games/mc2-opengl/mc2-win64-v0.4" && ./mc2.exe --validate --frames 60 --log validate.json --screenshot validate.tga 2>validate_stderr.txt
```

2. **Check exit code**: `$?` should be 0 for success.

3. **Read telemetry**: Read `validate.json` for:
   - `exit_code`: 0=success, 1=error
   - `shader_errors`: array of compile/link errors
   - `gl_errors`: array of GL errors
   - `avg_frame_ms` / `max_frame_ms`: performance
   - `frames`: number rendered

4. **Check stderr**: Read `validate_stderr.txt` for VALIDATE: messages and any crash output.

5. **Check screenshot**: If `--screenshot` was used, verify file exists and has non-zero size.

## Options

- `--validate` — enable validation mode (required)
- `--frames N` — number of frames to render (default 60)
- `--screenshot path` — save final frame as TGA
- `--log path` — telemetry JSON output (default validate.json)
- `--enable feature` — force enable: bloom, shadows, fxaa, grass
- `--disable feature` — force disable: bloom, shadows, fxaa, grass
- `-mission name` — which mission to load (default mis0101)

## Interpreting Results

- **exit_code 0 + empty error arrays** = clean run
- **exit_code 1 + shader_errors** = shader compilation failed (check the error messages)
- **exit_code 1 + gl_errors** = GL runtime errors (usually bad state or missing resources)
- **No JSON file** = crash before validation could write (check stderr for crash info)
- **avg_frame_ms > 50** = performance regression worth investigating
