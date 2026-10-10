# C++23 step 1 -- the clang-tidy safe set

WHAT: mechanically apply the rewrites whose result cannot change behaviour, and prove each translation unit (TU)
unchanged by comparing object code before and after. WHY: the ratchet shows 4835 `NULL` (LG01) and 815 `typedef`
(LG02); a reviewer cannot read thousands of edits, but identical object sections prove each one was a no-op.

## Checks
| check | rewrite | why it is safe | what the proof can still catch |
|---|---|---|---|
| `modernize-use-nullptr` | `NULL`/`0` as a null pointer -> `nullptr` | same value in pointer context | `NULL` passed to `...` (int vs pointer width) or to an overload set: object differs, TU reverted |
| `modernize-use-using` | `typedef X Y;` -> `using Y = X;` | identical type alias | none expected |
| `modernize-use-override` | add `override` to overriding virtuals | no codegen; turns a silent signature mismatch into a compile error | a compile error means a real latent bug: reported, not fixed here |
| `readability-redundant-casting` | drop a cast to the expression's own type | same type, same value | value-category cases (e.g. a cast that forces an rvalue): object differs, reverted |

OFF, with reason: `modernize-use-default-member-init` (moves initialisation, can reorder it against constructor
bodies), `modernize-loop-convert` (changes iteration expressions and sometimes copies), `modernize-use-auto`,
`modernize-use-trailing-return-type`, `modernize-pass-by-value`, anything `cppcoreguidelines-*` (C-style casts need a
human choice of `static_cast` vs `reinterpret_cast`: that is LG09, step 2). Each gets its own proof plan before it is enabled.

## Tool: `tools/tidy/run_tidy_safe.py`
1. Select TUs: `.cpp` present in `build64-ninja/compile_commands.json` (`scripts/configure_ninja.py` already exports it with
   `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`), not excluded, no uncommitted changes.
2. Hash current objects with `tools/objhash.py` (factored out of `tools/comment_guard.py`, which now imports it). Refuses to
   start unless every selected object is newer than its source (clean base build).
3. `clang-tidy -p build64-ninja --checks=-*,<safe set> --header-filter=^$ --fix` per TU, in parallel. The header filter keeps
   fix-its inside the main file; headers are phase 2 because one header edit rebuilds every includer.
4. One `ninja` run builds the changed objects (under vcvars, `-j` capped so a concurrent smoke is not starved).
5. Compare each object: identical COFF code/data/relocation/symbol sections (timestamps and `.debug*` ignored) -> keep;
   otherwise `git checkout` that file and print the differing sections. A clang-tidy compile error discards that TU's fixes.
6. Print the kept files as a pathspec list. The tool never commits.

## Excluded (the owner's comment pass; a fix-it would collide with it)
`code/missiongui*`, `code/mission*`, `mclib/txmmgr*`, `mclib/bdactor*`, `mclib/mech3d*`, `code/mech*`,
`GameOS/gameos/gos_postprocess*`, `gos_terrain_indirect*`, `gos_terrain_lod_chunk*`, `gameos_graphics*`,
`gos_static_prop_batcher*`; also `3rdparty/`, `Gates/`, build trees. They join when the owner declares the pass done.

## Batches and ratchet
- 25 TUs per batch, sorted by directory (`mclib`, `code`, `GameOS`, `GuiRuntime`, `gui`, `editor`, `Viewer`, `tools`).
- After a batch: build `mc2` + doctest, `cxx17_ratchet --check --tighten` (LG01 and LG02 baselines drop by the sites removed),
  commit the kept files by pathspec. A batch whose totals do not fall by the sites tidy reported is investigated, not committed.
- Phase 2 (headers): rewrite a header only together with a full rebuild of its includers and the same object proof for each.

## Risks
- clang-tidy parses MSVC command lines in `cl` driver mode; macros that differ between the MSVC and clang front ends can hide a
  site (it is then simply not rewritten) or skip a `#if` arm. The object proof covers only the arm the build compiles.
- Host load: the proof rebuild is serial and CPU heavy. Run it only when no smoke holds the lease.

## First run (after lane 2's tier1 is done)
`py -3 tools/tidy/run_tidy_safe.py --dry-run mclib/crater.cpp`, then without `--dry-run`. Report: diff size, objhash verdict,
LG01/LG02 delta. Go/no-go for the 25-TU batches rests on that one TU being identical.
