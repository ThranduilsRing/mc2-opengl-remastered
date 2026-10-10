<!-- doc-status: stale | reviewed:2026-10-11 -->
# GAMEOS-PLATFORM-BOUNDARY-RECON-1

**Status:** RECON ONLY — no code. Maps GameOS/platform deps blocking portability,
harnessability, Vulkan/Linux readiness, and clean subsystem tests.

> **SCOPE HONESTY:** this is the **GameOS/platform boundary** only. It is NOT the
> broader cross-subsystem audit (shader/material, texture/asset policy,
> EditorBridge, editor runtime, smoke/deploy/release ops are OUT OF SCOPE here).
> Do not cite this as `CROSS-SUBSYSTEM-AUDIT-RECON-1`.
>
> **CONFIDENCE on the headline findings** (advisor calibration):
> - gameos.hpp pulls no GL — **PROVEN** (header has no GL include; only `<windows.h>`/shim).
> - 143-file transitive reach — **PLAUSIBLE** (grep-derived; reproduce before trusting as exact).
> - `File::open(buffer,len)` exists, inRAM-aware, zero callers — **PROVEN** (file.cpp:1355 cited).
> - FIT/CSV harness "~3 lines away" — **PLAUSIBLE, downgraded from GREEN**: the
>   wrapper is tiny but the test `systemHeap` bootstrap, buffer lifetime, and
>   malformed/empty-buffer error behavior can bite. Treat as YELLOW-small.
> - Threading portable — **GREEN but SCOPED** to frame_jobs/txmmgr/mclib (not a
>   global platform conclusion).
> - gosRenderer RED, crash-handler additive — **PROVEN/RED** and **YELLOW** resp.
> - Header split "mechanical/zero churn" — **PLAUSIBLE, not proven** (splits expose
>   include-order assumptions; worth doing, not "free").
**Method:** 4 parallel read-only recons (File/FST/heap/path · GL/window/input/timing ·
crash/diag/threading · gameos.hpp surface+callers). All cites live-verified at HEAD.

## ★ Headline corrections / findings
- **`gameos.hpp` does NOT include any GL/D3D/GLEW header.** The "drags the world"
  cost is `<windows.h>` + API breadth + the transitive chain: `mclib/stuff/stuff.hpp:46`
  and `mclib/utilities.h:9` both `#include<gameos.hpp>`, and those two are pulled into
  ~all of mclib → **143 files transitively reach gameos.hpp** (28 include it directly).
  This makes a header split materially cheaper than feared.
- **`File::open(const char* buffer, int len)` ALREADY EXISTS** (`mclib/file.cpp:1355`,
  decl file.h:168) and every `File::read*` is inRAM-aware. It has ZERO live callers.
  A ~3-line `FitIniFile::open(buffer,len)` wrapper + a test `systemHeap` bootstrap
  unblocks the deferred FIT-PARSE / CSV in-memory harness with no disk/FST.
- **Threading is already portable** — `frame_jobs.cpp` + `txmmgr.cpp` are pure
  `std::thread/mutex/atomic/condition_variable`; **zero** `CreateThread`/`CRITICAL_SECTION`
  in mclib. The GREEN anchor.
- **The renderer is the one monolithic RED wall:** `gosRenderer` (gameos_graphics.cpp:1388)
  is a concrete class with **0 virtuals**, a single `g_gos_renderer` global (246 refs),
  and **1111 `gl*` calls** in that one TU. No backend seam exists; out of scope.
- **Crash handler is the only other true RED** (SEH + DbgHelp + MiniDumpWriteDump,
  `gos_crashbundle.cpp`), but a `#ifndef _WIN32` stub seam + clean C-API already exist
  (engine links on Linux today with zero crash diagnostics → additive backend, not a
  blocking refactor).

## Ranked top 10 seams

| # | Seam | Class | Source | Why it matters | Smallest slice | Type |
|---|---|---|---|---|---|---|
| 1 | **In-memory FitIniFile** (wire `File::open(buf,len)` into `FitIniFile::open`) | **GREEN** | file.cpp:1355, inifile.cpp:767 | unblocks the DEFERRED fit/CSV harness (no disk/FST) | `FitIniFile::open(buf,len)` + test systemHeap bootstrap | runtime+tool |
| 2 | **gameos.hpp split** (gos_time.h/gos_file.h/gos_util.h out of the umbrella) | **YELLOW** | gameos.hpp §TIME 1625/§FILE 1046/§UTILITY 1907 | subsystem tests compile w/o `<windows.h>`; highest harnessability multiplier | extract 3 GREEN sections, umbrella re-includes | header-only |
| 3 | **Path normalizer unify** (`fst_fold_key` = ASCII-lower+slash) | **YELLOW** | file.cpp:202 + :980, cident.cpp:75 | THE Linux case-sensitivity hazard (3 divergent folders) | add to fst_hash.{h,cpp}, route both + doctest | runtime+tool |
| 4 | **diagnostic_trace portability + test** | **YELLOW** | diagnostic_trace.cpp (only `GetCurrentThreadId` + uncond `<windows.h>`) | pure JSONL writer, 1 dep from portable + harnessable | swap tid, drop windows.h, JSONL doctest | runtime+test |
| 5 | **frame_jobs `parallelForRange` doctest** | **GREEN** | frame_jobs.cpp (pure std::) | validates the worker-safety invariant the FRAME-JOBS arc leans on; untested | doctest: serial==parallel, full-range coverage | test |
| 6 | **Decouple stuff.hpp/utilities.h from gameos.hpp** | **YELLOW (bigger)** | stuff.hpp:46, utilities.h:9 | the 143→far-fewer transitive-reach amplifier; the real harnessability unlock | narrow their include to the split sub-headers (after #2) | header-only |
| 7 | **NETWORK/DirectPlay dead removal** | **DEAD-LEGACY** | gameos.hpp 1721–1884 | pure header-surface shrink (single-player remaster; no live caller) | caller sweep → delete/`#if 0` the section | header-only |
| 8 | **Timing direct-caller consolidation** | **YELLOW** | 11 `timeGetTime`/QPC callers in code/+mclib | `gos_GetHiResTime` is already portable (has Linux branch); these 11 bypass it | route them through gos_GetHiResTime | runtime |
| 9 | **INPUT VK-map extract + harness** | **YELLOW** | gameos_input.cpp handleKeyEvent (SDL→Win32-VK table) | input already SDL; pure state machine testable w/ synthetic SDL_Events | extract table+`gos_GetKeyStatus` to pure unit | tool+test |
| 10 | **systemHeap test bootstrap** | **YELLOW (test-only)** | heap.{h,cpp} (287 hits/48 files; already `USE_GOS_HEAP`-seam'd) | enables linking the fit/CSV harness (pairs with #1) | minimal malloc-backed UserHeap for test fixture | tool |

### RED / out-of-scope (noted, not ranked)
- **gosRenderer monolith** (1111 gl*, 0 virtuals, 246-ref global) — Vulkan backend = large arc, no existing seam. Do NOT attempt a renderer-interface slice.
- **Crash handler portable backend** (SEH/DbgHelp/minidump → signal/libunwind) — real RED port; stub seam already drawn so it's additive, not blocking.
- **FullPathFileName `CharLower`** (cident.cpp:75, Win32) — folds into seam #3 as a follow-on micro-slice.

### DEAD-LEGACY inventory
- NETWORK/DirectPlay section (gameos.hpp 1721–1884); legacy Environment texture-heap + net fields (302–321, self-flagged "will be deleted"); pure-D3D vertex/state enums (D3DVERTEX variants, clip/lighting states).
- Two UNREGISTERED crash filters: `gameosmain.cpp:52` (cast-to-void) + `code/dw.cpp:87` (Watson, `#ifndef LINUX_BUILD`) — confirm no double-registration, then delete.
- `mclib/ffile.cpp:670` `_open("d:/ffile.bin")` dead debug stub.

## Recommended next 3 slices

1. **FITINI-INMEM-OPEN-1** (runtime+tool, tiny) — add `FitIniFile::open(const char* buf,
   int len)` (mirror inifile.cpp:767 + call `afterOpen()`) and a test `systemHeap`
   bootstrap. **Retires the standing FIT-PARSE deferral** — the parser becomes
   harnessable game-free (the seam, `File::open(buf,len)`, already exists). Highest
   immediate payoff. tier1-gated (touches FitIniFile). Run slice-preflight first.
2. **GOS-HEADER-SPLIT-1** (header-only, mechanical, reversible) — extract the 3 GREEN
   sections (gos_time.h / gos_file.h / gos_util.h, ~6 functions) into standalone
   headers; `gameos.hpp` becomes an umbrella re-including them. Zero behavior change,
   zero caller churn; lets file/time/util harnesses compile without `<windows.h>`.
   Enforce via the existing `scripts/check-include-firewall.allowlist`. The
   harnessability multiplier.
3. **PATH-FOLD-KEY-UNIFY-1** (leaf, Linux portability) — add `fst_fold_key` (ASCII
   lower + `\\`→`/`) to `mclib/fst_hash.{h,cpp}`, route file.cpp:202 + :980 through it,
   add doctest cases (mixed-case / backslash / non-ASCII passthrough) to test_hashing.cpp.
   Top Linux case-sensitivity seam; `CharLower` shim is a follow-on micro-slice.

All three are small, low-risk, and avoid the renderer/crash RED walls. #1 and #3 are
recon-derived production touches → **run `repo_query.py slice-preflight` first** (per
the DEV-EFFICIENCY-BOOTSTRAP-1 rule) and gate on tier1.

## Cluster verdict recap
- **GREEN anchors:** threading (frame_jobs/txmmgr), in-memory File seam, gos TIME/FILE/UTILITY sections.
- **YELLOW (cheap adapters/extractions):** path normalizers, diagnostic_trace, timing callers, input VK-map, header split, systemHeap test bootstrap.
- **RED walls (large arcs, out of scope):** gosRenderer/GL core, crash-handler portable backend.
- **DEAD-LEGACY (pure shrink):** NETWORK/DirectPlay, legacy env fields, D3D enums, 2 unregistered crash filters.
