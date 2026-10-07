---
name: mc2-build
description: Build mc2.exe in A:/Games/mc2-main using CMake + MSVC RelWithDebInfo
---

# MC2 Build

Build mc2.exe in `A:/Games/mc2-main` (the only build tree; the root `A:/Games/mc2-opengl-src` is stale -- never build there). Commit first: the exe fingerprint embeds the git sha.

## Steps

1. **Bootstrap if unconfigured**: if `A:/Games/mc2-main/build64/CMakeCache.txt` does NOT exist, run the bootstrap instead of hand-discovering CMake flags:
   ```bash
   py -3 A:/Games/mc2-main/scripts/bootstrap_worktree_build.py --build
   ```
   Then STOP. (Drop `--build` to configure only.)

2. **Run CMake build**:
```bash
CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
cd "A:/Games/mc2-main" && "$CMAKE" --build build64 --config RelWithDebInfo --target mc2 mc2_launcher
```

3. **Report result**: Show success or failure. On failure, show the last 30 lines of output to capture error messages.

## Important
- ALWAYS use `--config RelWithDebInfo`, never Release or Debug
- Vulkan is compiled into `build64` by default; `build64_island` is obsolete
- Output exe lands at `build64/RelWithDebInfo/mc2.exe`
