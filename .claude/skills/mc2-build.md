---
name: mc2-build
description: Build mc2.exe in the current worktree (or main repo) using CMake + MSVC RelWithDebInfo
---

# MC2 Build

Build mc2.exe from the current worktree or main repository.

## Steps

1. **Detect worktree**: Check if CWD is inside a worktree under `.claude/worktrees/`. If so, use that worktree root. Otherwise use `A:/Games/mc2-opengl-src`.

2. **Run CMake build**:
```bash
CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
cd "<worktree_root>" && "$CMAKE" --build build64 --config RelWithDebInfo --target mc2
```

3. **Report result**: Show success or failure. On failure, show the last 30 lines of output to capture error messages.

## Important
- ALWAYS use `--config RelWithDebInfo`, never Release or Debug
- ALWAYS use `--target mc2`
- Build directory is always `build64/` relative to worktree root
- Output exe lands at `build64/RelWithDebInfo/mc2.exe`
