<!-- doc-status: current | reviewed:2026-10-11 -->
# FITINI-INMEM-HARNESS-BOOTSTRAP-RECON-1

**Status:** RECON-FIRST follow-up (no fix here). Opened because
`FITINI-INMEM-OPEN-WRAPPER-1` shipped the production seam
(`FitIniFile::open(buffer,len)`) but the **game-free harness was measured to be
non-tiny and was stopped**. This recon finds the smallest legitimate way to test
the new overload without booting the game.
**Parent:** gameos-platform-boundary-recon-1.md
(seam #1). Does NOT retire the FIT-PARSE harness deferral — that stays open until
this lands a working harness.

## The measured blocker (from the wrapper slice, 4 build iterations)
A standalone CMake linking `inifile.cpp` + `file.cpp` + `heap.cpp` + harness:
1. `gameos.hpp` → `platform_windef.h` fails to compile without `-DPLATFORM_WINDOWS`
   (root build sets it, CMakeLists:62). **Fixed by adding the define.**
2. `gameos.hpp:72` needs `utils/vec.h` → add `GameOS/gameos` include dir. **Fixed.**
3. `heap.cpp` then fails standalone: `C2601 local function definitions are illegal`
   + `C1004 unexpected EOF` — an `#if`/brace branch left open under our define set;
   heap.cpp needs **more of the engine target's exact macro config** to compile.
   **Not chased** (would mean replicating the engine build).
4. Not yet reached, but next: `file.cpp` references the FST/`gos_*`/`systemHeap`
   symbol surface → a link-time wall (the deferred FIT-PARSE RED reason).

Conclusion: an ad-hoc standalone CMake is the wrong vehicle — it diverges from the
engine target's defines/includes and re-hits the heap/FST/gos surface.

## Questions to answer (recon)
1. **Reuse the real target, not ad-hoc CMake:** can the harness be a small
   `add_executable` in the MAIN CMake that links the *already-configured* mclib
   objects/target (inheriting the exact defines/includes), instead of a separate
   project? (The compile errors were all config-divergence — this likely removes
   them.)
2. **Existing test binary:** does any current `tests/` target already link
   `file.cpp`/`inifile.cpp` + gameos with a working config we can extend? (Check
   `tests/unit/CMakeLists.txt` — `test_render_contract_3` links real engine TUs;
   is there a precedent that pulls mclib + a heap?)
3. **systemHeap bootstrap:** is there an existing test/tool path that initializes
   `systemHeap` (so we don't hand-roll it)? `bootstrap_worktree_build.py` builds
   mc2; is there a lighter init? Or a malloc-backed `UserHeap` test ctor.
4. **Micro-seam to avoid file.cpp/FST/gos:** could `FitIniFile`'s parse be tested
   against a tiny `File` test-double that only implements the in-RAM read methods
   (read/readLine/seek/eof on a buffer), avoiding linking `file.cpp` entirely?
   `File`'s read methods are `virtual` — a `MemFile : File`-like seam or a
   buffer-cursor extraction of `afterOpen`'s reads could isolate it. Assess cost
   vs. faithfulness (must exercise the REAL afterOpen/block parser, not a copy).
5. **Order vs GOS-HEADER-SPLIT-1:** would splitting `gameos.hpp` (seam #2) so
   inifile.cpp can compile against `gos_file.h`/`gos_memory.h` without the full
   `<windows.h>`/render surface unblock this more cleanly? Sequence accordingly.
6. **Is the bootstrap reusable?** the same blocker gates fit-parse, CSV-tokenizer,
   and full-objmgr harnesses. Prefer a reusable "engine-linking test fixture"
   (shared CMake fragment + systemHeap init + minimal gos stub) over a one-off.

## Exclusions
No production parser changes. No CSV tokenizer hardening. No broad heap
abstraction. No one-off fake build config that does NOT match the real engine
target's defines/includes (iteration 3 proved divergent config mis-compiles).

## Recommendation seed
Most promising = Q1+Q2 (link via the real CMake target / extend an existing
tests/ target that already compiles engine TUs with the right config), falling
back to Q4 (a buffer `File` test-double exercising the real `afterOpen`) if the
full link stays too heavy. Decide after the recon; do NOT pre-commit to standalone
CMake.
