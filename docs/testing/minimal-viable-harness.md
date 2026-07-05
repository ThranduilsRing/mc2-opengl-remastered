# MINIMAL-VIABLE-HARNESS-1 — engine TUs under unit test, game-free

**Status:** SHIPPED. The real mclib file stack (`file.cpp`, `inifile.cpp`,
`packet.cpp`, `ffile.cpp`) now compiles, links, and runs inside
`tests/unit/mc2_tests` with **no game boot, no GL context, no GameOS runtime**.
This closes the harness blocker measured in
[fitini-inmem-harness-bootstrap-recon-1.md](fitini-inmem-harness-bootstrap-recon-1.md)
and executes slice #1 (HEAP-TEST-BOOTSTRAP-1) of `ARCH-REVIEW-MCLIB-CORE-1`.

The point of this document is **repeatability**: the goal is separable engine
parts, not one-off tests. Follow the recipe below to bring the NEXT engine TU
under test.

---

## What shipped

| Piece | Path | Role |
|---|---|---|
| Heap/GameOS shim | `tests/unit/heap_shim.cpp` | malloc-backed `UserHeap`/`systemHeap` + `Environment`, `InternalFunctionStop/Pause`, window-mode and crashbundle stubs |
| File-stack tests | `tests/unit/test_file_inifile.cpp` | FitIni disk parse, FitIni in-memory `open(buffer,len)` (FITINI-INMEM first runtime proof), PacketFile round-trip, loose-disk `File::open` |
| Fixture | `tests/unit/fixtures/harness_smoke.fit` | on-disk .fit exercised by disk-parse + loose-resolve tests |
| CMake registration | `tests/unit/CMakeLists.txt` | engine TU list + zlib link + `MC2_TEST_FIXTURE_DIR` define |

Build + run (standalone; never touches `build64/`):

```bat
cmake -S tests/unit -B build64-tests -G "Visual Studio 17 2022" -A x64
cmake --build build64-tests --config RelWithDebInfo --target mc2_tests
build64-tests\RelWithDebInfo\mc2_tests.exe            :: full suite
build64-tests\RelWithDebInfo\mc2_tests.exe -ts=FileStack   :: just this slice
```

(Use the full VS CMake path from CLAUDE.md when `cmake` is not on PATH.)

---

## The measured dependency frontier (the core insight)

Linking `file.cpp` + `inifile.cpp` demands exactly this closure — nothing more:

```
file.cpp  inifile.cpp            <- targets
├── packet.cpp                   <- File::open(parent,...) casts to PacketFilePtr
├── ffile.cpp                    <- FST branch of File::open / read / readLine
│   ├── fastfile.cpp             <- FastFileFind + fastFiles registry
│   ├── lzdecomp.cpp             <- LZDecomp (compressed FST/packet reads)
│   ├── lzcomp.cpp               <- LZCompress (FastFile::writeFast)
│   └── zlib.lib                 <- compress2/uncompress (3rdparty/lib/x64, static)
├── err.cpp                      <- Fatal/Assert used by inifile error paths
├── GameOS/src/platform_str.cpp  <- S_strlwr / S_stricmp / S_strnicmp
├── mclib/fst_hash.cpp           <- elfHash/key normalize (already in target)
└── heap_shim.cpp                <- everything below
```

Symbols the shim supplies instead of linking their real (heavyweight or
uncompilable-standalone) TUs:

| Symbol | Real owner | Why shimmed |
|---|---|---|
| `UserHeap`/`HeapManager`/`HeapList` methods, `systemHeap`, `globalHeapList` | `mclib/heap.cpp` | heap.cpp does not compile standalone (C2601/C1004, recon blocker #3); allocator is swappable by design (`USE_GOS_HEAP` precedent) |
| `Environment` | `GameOS/gameos/gameos.cpp` | drags the whole platform layer; zero-init means `checkCDForFiles=0` |
| `InternalFunctionStop/Pause` | `GameOS/gameos/gameos_debugging.cpp` | STOP aborts loudly in tests; PAUSE logs and continues |
| `EnterWindowMode/EnterFullScreenMode/ExitGameOS` | gameos graphics | CD-missing dialog path in `File::open`; no display in tests |
| `crashbundle_append/init` | `gos_crashbundle.cpp` | SEH/minidump machinery; `FastFile::readFast` traces through it |

Empirical method that produced this list (reuse it): add the target TU to
`target_sources`, build, and let `LNK2019` enumerate the frontier. Each
unresolved symbol gets ONE of two treatments, in this order of preference:
1. **Link the real TU** if it is leaf-ish (no GL/SDL/gos runtime state) —
   e.g. `lzcomp.cpp`, `platform_str.cpp`.
2. **Stub it in `heap_shim.cpp`** if the real owner drags the platform layer
   AND the stub semantics are safe for tests (no-op or fail-loud).

Never treatment #3 (modify the engine TU to compile differently for tests) —
that's how divergent-config miscompiles happen (recon iteration 3).

## Shim contract (rules when extending `heap_shim.cpp`)

- CRT-plain allocation semantics: `Malloc(0)` returns a real pointer,
  `Free(NULL)` is a no-op, every `Malloc/calloc` result is `free`-able.
- `heap.cpp` must NEVER be added to the target while the shim is linked (ODR).
- Stubs are no-op or fail-loud only; no engine behavior re-implemented in stubs.
- Only define symbols the linker demands; delete stubs that lose their last
  referencing TU.

## Recipe: adding the NEXT engine TU to mc2_tests

1. **Preflight**: `py -3 tools/repo_query.py slice-preflight ...` if the work is
   recon-derived (docs/disciplines.md).
2. Add the TU to `target_sources(mc2_tests ...)` in `tests/unit/CMakeLists.txt`
   with a one-line comment naming the slice.
3. Build; triage `LNK2019`s per the two-treatment rule above. Record any new
   frontier TUs as comments in the CMake list (the list IS the frontier map).
4. Compile defines: the target already carries `LINUX_BUILD`,
   `PLATFORM_WINDOWS`, `USE_ASSEMBLER_CODE=0`, matching the engine build. Do
   NOT add TU-specific defines that diverge from the main build.
5. Fixtures go in `tests/unit/fixtures/` and are addressed via the
   `MC2_TEST_FIXTURE_DIR` compile define. Scratch output goes to cwd with an
   `mvh_scratch_` prefix and is removed by the test.
6. New tests: one `TEST_SUITE` per subsystem (`FileStack` is the file-stack
   one). Match doctest style of the existing `test_*.cpp` files.
7. Run the FULL suite (`mc2_tests.exe`) — all pre-existing tests must stay
   green. Then run the smoke gate if any engine (non-test) file changed.

Note the API conventions that bit the first tests: `writePacket` returns
**bytes written** (not `NO_ERR`); `seekBlock` returns `BLOCK_NOT_FOUND` as an
unsigned constant; `File::open` **lowercases** paths (harmless on Windows,
relevant for fixture naming).

## Relationship to the registered contract harnesses

`tools/build_contract_harnesses.py` builds standalone per-contract executables
(`tools/<name>/` → `build64-h-<name>`). That tier is for contracts that need
their own `main`/GL-adjacent linkage. `tests/unit/mc2_tests` is the cheaper
tier: doctest, one binary, no GL — prefer it whenever the subject TU fits
through the shim. A TU that fits neither tier is itself a finding (name the
blocker in a recon doc, as fitini-inmem did).

The doctest tier is CLI-runnable through the same builder (MINIMAL-VIABLE-
HARNESS-2), but on its own code path (it is NOT in `REGISTERED_HARNESSES` — it
has no per-harness `--json`/`status` contract and lives at `tests/unit/`, not
`tools/<name>/`, so registering it there would report MISSING/FAIL by
construction):

```
py -3 tools/build_contract_harnesses.py --unit-only --run   # doctest tier only
py -3 tools/build_contract_harnesses.py --run --unit        # both tiers
```

Exit code is authoritative (doctest returns non-zero on any failed assertion),
so it composes cleanly with a gate.

**CI / slice_gate proposal (not yet wired):** add `py -3
tools/build_contract_harnesses.py --unit-only --run` as a fast, GL-free
pre-merge check — it needs no game boot, no GL context, and no deploy, so it is
a cheap always-on guard for the engine-logic TUs already under the shim. (Note:
the file/mod/FST tests write scratch files to CWD with an `mvh_scratch_` prefix
and self-clean; run the binary from a writable working directory.)

## Next 3 targets — SHIPPED (MINIMAL-VIABLE-HARNESS-2)

1. **`mclib/csvfile.cpp`** (`CSVFile : File`) — DONE. Zero new link frontier as
   predicted. `tests/unit/test_csvfile.cpp` parses a Buildings.csv-shaped
   fixture (`fixtures/buildings_smoke.csv`, CRLF), and locks two real dialect
   findings: the parser is NOT RFC-4180 (splits on every comma, no quote
   awareness → an embedded comma shifts later columns) and `countCols()`
   returns the COMMA count, so the final column (index == column count) is
   unreachable via `seekRowCol` and reads back 0.
2. **FST round-trip test** — DONE. `tests/unit/test_fst_roundtrip.cpp` authors a
   compressed `.fst` (`create`→`reserve`→`writeFast`→`close`), registers it via
   `FastFileInit`, and proves the ladder end to end: **packed serves when loose
   absent; loose overrides packed** (plus `fileExists` tier-1-vs-tier-2).
3. **`mclib/paths.cpp`** (leaf path globals) — DONE, linked as a leaf TU.
   `tests/unit/test_paths_mod.cpp` also exercises the `mods/` resolution order
   (`InitModSearchPaths`/`TryModOpen`, which live in the already-linked
   file.cpp): an active-mod override wins over loose base data, stock mode falls
   back, and `ShouldSearchMods` gates out absolute/`..` paths.

Deferred/blocked: `heap.cpp` itself (by design — shim replaces it here),
`utilities.cpp` (drags `txmmgr.h`/`mclib.h`; needs a leaf-TU carve first,
fst_hash-style), `userinput.cpp` (hard gos coupling; see
gameos-platform-boundary-recon-1.md).
