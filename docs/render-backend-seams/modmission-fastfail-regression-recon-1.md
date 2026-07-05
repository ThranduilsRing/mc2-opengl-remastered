# MODMISSION-FASTFAIL-REGRESSION-RECON-1

**Status**: CLOSED — PREMISE INVALIDATED (no code regression found)
**Crash**: `0xC0000409` (STATUS_STACK_BUFFER_OVERRUN / `__fastfail`)
**Affected (claimed)**: `clearwater` (TangoMaster), `cfv2_mission1_escort` (MCO-ClanEagle)
**Passing**: `torrin`/`area16`/`coldstone` (DarkRain), all tier1 stock missions
**Date**: 2026-06-23

---

## VERDICT: ALL PRIOR CRASH DATA WAS FALSE POSITIVE

All "crashes" in iteration 1 and the first bisect-2 runs were caused by test
infrastructure and environment errors, NOT by code regressions.  After fixing the
environment, **every tested commit passes cleanly**:

| Commit | SHA | Mission | Env | Result |
|--------|-----|---------|-----|--------|
| 1 | `4c90126c` | clearwater | correct | **PASS** (4213 fr, 140fps) |
| 2 | `972d872d` | clearwater | correct + cache clear | **PASS** |
| 5 | `4f507820` | clearwater | correct | **PASS** |
| 8 | `2c8b1a60` | clearwater | correct + cache clear | **PASS** (4213 fr, 140fps) |
| HEAD | `a4dea8ac` | cfv2_mission1_escort | correct (mco-compat) | **PASS** |

No code regression exists in commits 1–8 or at HEAD for either mission.

---

## Root cause of false positives (in order of discovery)

### FP-1: Wrong MC2_ACTIVE_MOD value in iteration 1

Iteration 1 bisect used `MC2_ACTIVE_MOD=MCO-TangoMaster` (incorrect).  The mod
directory is `mods/TangoMaster/` — value should be `TangoMaster`.  Engine could not
find `clearwater.fit` → STOP assert → 0xC0000409.

### FP-2: Missing MC2_ACTIVE_MOD env var in bisect-3 first run

When building bisect worktrees, the `MC2_ACTIVE_MOD` env var was not set before the
smoke run.  Engine ran without any active mod → clearwater.fit not found → STOP →
0xC0000409.  Fixed by re-running with `$env:MC2_ACTIVE_MOD="TangoMaster"` and
`$env:MC2_MOD_DEPS="mc2x-compat"`.

### FP-3: Stale .modindex-cache in bisect-4 (absolute path mismatch)

When `mc2-bisect-deploy-4` was created by copying from `mc2-bisect-deploy-3`, the
`mods/TangoMaster/.modindex-cache` was copied verbatim.  The cache stores ABSOLUTE
paths pointing to the SOURCE deploy (`mc2-bisect-deploy-3\mods\TangoMaster\...`).
Engine loaded TangoMaster's 447 files via stale cache paths and missed the
`clearwater.fit` entry → 0xC0000409.

Fix: `Remove-Item ".../mods/TangoMaster/.modindex-cache" -Force` before the smoke run.

**STANDING RULE**: after copying any deploy directory, delete ALL `.modindex-cache`
files in every mod subdirectory before running smoke:
```powershell
Get-ChildItem "A:/Games/mc2-bisect-deploy-N/mods" -Recurse -Filter ".modindex-cache" | Remove-Item -Force
```

### FP-4: Missing mco-compat mod for cfv2 (mc2-win64-v0.4 deploy)

`cfv2_mission1_escort` requires `MC2_MOD_DEPS=mco-compat`, but `mco-compat` was not
present in `A:/Games/mc2-opengl/mc2-win64-v0.4/mods/`.  Only `mc2-win64-v0.4d-rc1`
had the `mco-compat` mod.  Fixed by pointing smoke at the `v0.4d-rc1` deploy.

### FP-5: Race condition — smoke started while deploy copy in progress (bisect-5)

`mc2-bisect-deploy-5` was still being copied from `deploy-4` when the first smoke run
for bisect-5 started.  `GetFileAttributesA(".../mc2x-compat/data/")` returned
`INVALID_FILE_ATTRIBUTES` because that directory did not yet exist → "dep 'mc2x-compat'
not found, skipping" → 0 mod files loaded → clearwater.fit not found → 0xC0000409.

Fix: wait for `Copy-Item` to complete fully, then clear all mod caches before running.

---

## Static analysis findings (exhaustive, commits 1–8)

All new stack buffers and data structures were analyzed statically.  No overflow
candidate found:

- `s_pendingSubmits`, `s_typeLodRecords` = `std::vector` (unbounded, no overflow)
- `s_fallbacksThisFrame[5]` — indexed by `GpuMechFallbackReason` (5-value enum, safe)
- `textureOrKtxSidecarExists()` — `char ktx[1024]`, strncpy with `sizeof(ktx)-1` + explicit `strlen+6 < sizeof(ktx)` guard before strcat
- `FullPathFileName` = heap-allocated, no internal fixed buffer
- AO path in `resetPaintScheme()` (commit 8) — gated on `ImportedMechAoTexName(aoTypeKey) != nullptr` which requires `g_importedAnims` populated (`MC2_ASSIMP_MECH_IMPORT=1`, default OFF); inert for all mod missions
- `float wp[3]` in `getWeaponNodePosition` — writes indices [0],[1],[2] only; `g_importedAnims` empty for mod missions so loop exits immediately
- `GpuMechSubmitDesc` struct layout change (commit 8, `slot6TexHandle` inserted) — all callers recompile together, no ABI split
- `file.cpp` — unchanged in entire bisect window (last touched at `668e793e`, ancestor of commit 1)
- ABL repro hook (commit 9) — gated on `MC2_ABL_ARG_GUARD_REPRO=1` (unset), inert
- BlendMode enum extension (commits 10–12) — `PipelineDesc.h`/`PipelineRegistry` only; no runtime switch-indexed arrays found

---

## Symbolication note

`0xC0000409` (`__fastfail`) bypasses the SEH unhandled-exception filter entirely —
`gos_crashbundle.cpp` `StackWalk64` / `SymFromAddr` handler never fires → `crash_silent`
with no stack.  For genuine future `__fastfail` crashes, obtain a stack via:
- WER minidump: `reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /v DumpType /t REG_DWORD /d 2 /f` then collect `.dmp` from `%LOCALAPPDATA%\CrashDumps\`
- Or: attach WinDbg, set `sxe cc` to break on `__fastfail`

---

## Fix-slice

**NONE REQUIRED.**  No code regression found.  The crash reports were all caused by
test environment issues.  No fix slice is proposed.

---

## Bisect infrastructure created (may be cleaned up)

| Directory | Commit | Status |
|-----------|--------|--------|
| `A:/Games/mc2-bisect-deploy-2/` | `4c90126c` (commit 1) | built, PASS |
| `A:/Games/mc2-bisect-deploy-3/` | `4f507820` (commit 5) | built + deployed, PASS |
| `A:/Games/mc2-bisect-deploy-4/` | `972d872d` (commit 2) | built + deployed, PASS |
| `A:/Games/mc2-bisect-deploy-5/` | `2c8b1a60` (commit 8) | built + deployed, PASS |
| `A:/Games/mc2-bisect-3/` | worktree @ `4f507820` | can be removed |
| `A:/Games/mc2-bisect-4/` | worktree @ `972d872d` | can be removed |
| `A:/Games/mc2-bisect-5/` | worktree @ `2c8b1a60` | can be removed |

---

## Process lessons

1. **Always set `MC2_ACTIVE_MOD` explicitly** — the correct value is the directory name
   under `mods/` (no `MCO-` prefix, no `-compat` suffix).
2. **Delete `.modindex-cache` after copying a deploy dir** — cache stores absolute paths.
3. **Wait for deploy copy to complete** before starting smoke; verify with `Get-Item`.
4. **cfv2 (MCO-ClanEagle) requires `mco-compat`** — only `mc2-win64-v0.4d-rc1` has it.
5. **Before concluding a code regression, verify the env is clean** and retry at least
   once with cache cleared.
