---
name: mc2-deploy
description: Deploy mc2.exe and all shaders from current worktree to runtime directory, with diff verification
---

# MC2 Deploy

Deploy the built exe and all shader files from the current worktree to the runtime directory, verifying every file.

## Paths
- **Source worktree**: Auto-detect from CWD (or `A:/Games/mc2-opengl-src`)
- **Deploy target**: `A:/Games/mc2-opengl/mc2-win64-v0.1.1`

## Steps

1. **Detect worktree**: Same logic as mc2-build — find worktree root from CWD.

2. **Deploy exe**:
```bash
cp -f "<worktree>/build64/RelWithDebInfo/mc2.exe" "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
```

3. **Deploy FFmpeg DLLs** (copy each DLL individually using `cp -f`, NEVER `cp -r`):
```bash
for dll in avcodec-61.dll avformat-61.dll avutil-59.dll swscale-8.dll swresample-5.dll; do
    cp -f "<worktree>/build64/RelWithDebInfo/$dll" "A:/Games/mc2-opengl/mc2-win64-v0.1.1/$dll"
    diff -q "<worktree>/build64/RelWithDebInfo/$dll" "A:/Games/mc2-opengl/mc2-win64-v0.1.1/$dll"
done
```

4. **Deploy shaders**: Copy EVERY shader file individually using `cp -f` (NEVER `cp -r` — it silently fails to overwrite on Windows/MSYS2):
   - All `*.vert`, `*.frag`, `*.tesc`, `*.tese` from `<worktree>/shaders/`
   - All files from `<worktree>/shaders/include/` to deploy `shaders/include/`
   - Create `shaders/include/` in deploy target if it doesn't exist

5. **Verify ALL deployed files**: Run `diff -q` on every DLL and shader file (source vs deployed). Report:
   - Files that were deployed successfully (match confirmed)
   - **ANY mismatches — flag loudly as errors**
   - Files that exist in source but not in deploy (new files that need copying)
   - Files that exist in deploy but not in source (stale files from old branches)

## Critical Rules
- **NEVER use `cp -r`** — it does not overwrite existing files on Windows/MSYS2. This has caused hours of debugging from stale shaders.
- **ALWAYS verify with `diff -q`** after copying
- Deploy shaders ONE FILE AT A TIME with `cp -f`
- Report stale files in deploy dir that don't exist in source
