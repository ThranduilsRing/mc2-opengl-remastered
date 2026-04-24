# ASan MVP Runbook — `asan-mvp` worktree

Parallel AddressSanitizer dev build. Does not deploy to the normal install dir — runs in-place from `build64-asan/RelWithDebInfo/`.

> ## ⛔ DO NOT DEPLOY this build
>
> The ASan `mc2.exe` is intentionally confined to
> `A:/Games/mc2-opengl-src/.claude/worktrees/asan-mvp/build64-asan/RelWithDebInfo/`.
>
> **Never** `cp` / `rsync` / `Copy-Item` it into `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`
> or any other user-facing install. Reasons:
>
> - 2–3× slower and multi-GB memory footprint; useless for anyone not
>   hunting a heap bug.
> - Requires `clang_rt.asan_dynamic-x86_64.dll` sitting next to the exe;
>   a stray deploy overwrites the shippable binary with a dev-only one
>   that won't launch on a clean install.
> - `MC2_ASAN` is defined, Tracy is disabled, so Tracy captures against
>   this exe silently do nothing — a confusing perf-regression trap.
>
> **`/mc2-deploy` must not target this build.** If you need to reproduce
> an ASan finding against the deployed tree, run it in place and copy the
> log, not the binary.

## Why this build exists

Several "papered over" fixes in the main tree strongly suggest latent heap bugs:

- LZ decompress buffer bumped 263 KB → 8 MB (`mclib/txmmgr.h`)
- `MAX_STANDARD_FUNCTIONS` 256 → 512 (Carver5O stability pass)
- `ABLi_loadLibrary` heap corruption flagged for PageHeap
- Uninit `FunctionCallbackTable` (Omnitech)
- zlib bound fix

ASan gives us call stacks for exactly this class of bug. `gos_Malloc` is already
plain `malloc()`, so every `new`/`delete`/`gos_Malloc` is covered with no
allocator bypass work.

## Build

```bash
CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"

cd A:/Games/mc2-opengl-src/.claude/worktrees/asan-mvp

# One-time configure (explicit GLEW/ZLIB paths because FindGLEW/FindZLIB in
# cmake 3.31 no longer auto-resolve from CMAKE_PREFIX_PATH for this layout).
"$CMAKE" -S . -B build64-asan -G "Visual Studio 17 2022" -A x64 \
    -DCMAKE_PREFIX_PATH=A:/Games/mc2-opengl-src/3rdparty/3rdparty \
    -DGLEW_INCLUDE_DIR=A:/Games/mc2-opengl-src/3rdparty/3rdparty/include \
    -DGLEW_SHARED_LIBRARY_RELEASE=A:/Games/mc2-opengl-src/3rdparty/3rdparty/lib/x64/glew32.lib \
    -DZLIB_INCLUDE_DIR=A:/Games/mc2-opengl-src/3rdparty/3rdparty/include \
    -DZLIB_LIBRARY=A:/Games/mc2-opengl-src/3rdparty/3rdparty/lib/x64/zlibstatic.lib \
    -DZLIB_LIBRARY_RELEASE=A:/Games/mc2-opengl-src/3rdparty/3rdparty/lib/x64/zlib.lib \
    -DMC2_ASAN=ON

# Incremental builds
"$CMAKE" --build build64-asan --config RelWithDebInfo --target mc2
```

Output: `build64-asan/RelWithDebInfo/mc2.exe` plus `clang_rt.asan_dynamic-x86_64.dll` copied alongside.

## CMake changes vs. nifty

All inside `if(MC2_ASAN)` in the root `CMakeLists.txt`:

- Adds `/fsanitize=address` globally.
- Strips `/MP` (parallel compile) — incompatible with ASan on 17.x.
- Strips `/RTC1` (runtime checks) — ASan link refuses.
- Flips `/INCREMENTAL` → `/INCREMENTAL:NO`.
- Skips `TRACY_ENABLE` / `TRACY_ON_DEMAND` / `TRACY_NO_SYSTEM_TRACING` — Tracy
  hooks the allocator and races ASan's interceptors.
- Defines `MC2_ASAN` preprocessor symbol for any future source-level gating
  (e.g. TGL vertex-pool bypass).
- POST_BUILD copies `clang_rt.asan_dynamic-x86_64.dll` from the MSVC bin dir
  into the output directory.

## Run: standalone mission smoke

MSVC ASan runtime honors `ASAN_OPTIONS`. Use a non-halting config so a single
session surfaces multiple bugs:

```bash
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:halt_on_error=0:print_stacktrace=1:symbolize=1"

cd build64-asan/RelWithDebInfo
./mc2.exe -mission mc2_01 2>&1 | tee asan-mc2_01.log
```

Flag meanings:
- `detect_leaks=0` — the engine leaks by design at shutdown; noise we can't
  action yet.
- `halt_on_error=0` — keep running after a report so we inventory bugs per run.
- `abort_on_error=1` — when we *do* exit (e.g. uncaught exception), raise so
  the debugger catches the exact frame.
- `print_stacktrace=1:symbolize=1` — stack with file/line via PDB.

### First-run target sequence

1. **Boot smoke** — launch to main menu, exit via ESC. Validates the build
   plumbing (ASan runtime loaded, symbol resolution works).
2. **mc2_01 base game** — 60 s with `-mission mc2_01`. Any hit here is a
   decades-old engine bug, not a mod-compat issue.
3. **Carver5O corebrain** — `magic_corebrain_only` profile. Tests the
   `ABLi_loadLibrary` / `FunctionCallbackTable` predictions.
4. **Exodus 1.1** — `exodus_1_1`. Different content path, exercises the
   `mc2x-import` component-ID area.

## Run: via smoke harness (preferred)

The existing tier1 harness iterates the 5 canonical missions and collects
artifacts. Point it at the ASan exe instead of the deployed one:

```bash
py -3 A:/Games/mc2-opengl-src/.claude/worktrees/asan-mvp/scripts/run_smoke.py \
    --tier tier1 --duration 60 --fail-fast \
    --exe A:/Games/mc2-opengl-src/.claude/worktrees/asan-mvp/build64-asan/RelWithDebInfo/mc2.exe
```

(Check `scripts/run_smoke.py --help` — if `--exe` isn't supported, copy
`mc2.exe` + `clang_rt.asan_dynamic-x86_64.dll` to the deploy dir temporarily,
or add exe-override support as a follow-up.)

## Deliberately **not** doing in MVP

- **TGL vertex pool bypass.** `TG_GOSVertexPool` is the only non-malloc slab
  left. Skipping unless ASan stays quiet while known pool-exhaustion events
  fire. Annotate with `__asan_poison_memory_region` then.
- **`windows_hook_rtl_allocators=1`.** Would cover AMD GL driver allocations
  and produce noise we can't fix. Enable only if a suspected bug is invisible
  without it.
- **Deploying to `mc2-win64-v0.1.1/`.** ASan exe stays in the build tree so it
  can't shadow the production binary.

## Expected early failures (priors)

Ordered by confidence:

1. `ABLi_loadLibrary` heap-buffer-overflow or use-after-free.
2. LZ decompress overrun inside `txmmgr.cpp`.
3. Global/stack-buffer-overflow on the pre-bump `MAX_STANDARD_FUNCTIONS` array.
4. Downstream deref from uninit `FunctionCallbackTable` (ASan won't catch the
   uninit read itself — MSan not on Windows — but the resulting bad-pointer
   deref is in scope).
5. Classic `char buf[N]` overflows in `mission*.cpp` / `ablmc2.cpp` loaders.

If the first mission load hits #1 or #2, don't fix-then-rerun — finish all
three target missions first with `halt_on_error=0`, then triage. Fixing one
bug can mask or shift the next.

## Known limitations

- ASan is ~2–3× slower than RelWithDebInfo and blows up memory. Fine for
  mission-load and 60 s smoke, not for perf tuning.
- MSVC ASan cannot detect uninitialized reads (no MSan on Windows).
- GL driver allocations are invisible unless `windows_hook_rtl_allocators=1`,
  which is deliberately off.
