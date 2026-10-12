<!-- doc-status: current | reviewed:2026-10-11 -->
# SUBSYSTEM-HARNESS-ARC-1 — cheap contract/edge-case harness layer

**Status:** SHIPPED — HARNESS-TEMPLATE-1, SHADER-CONTRACT-HARNESS-1,
RENDER-STATE-CONTRACT-HARNESS-1, DEPLOY-ASSET-CONTRACT-HARNESS-1 (first Python),
OBJMGR-CONTRACT-HARNESS-1 (parallel lane), IBL-REGISTRY-CONTRACT-HARNESS-1
(+external-HDRI manifest), ICON-ATLAS-HARNESS-1 (+8-site row-count bug fix),
RENDER-PASS-TABLE-HARNESS-1 (static table invariants over RenderPassContract.h:
id parity, dup ids, reads/writes range, no mid-array terminator, static
producer/consumer closure — header self-flags field-value staleness),
DEPLOY-RELEASE-TREE-CONTRACT-HARNESS-1 (Python; validates a DEPLOYED tree's
runnable shape; explicit `--release-root`/`MC2_RELEASE_ROOT`, non-blocking when
unconfigured), MECH-IMPORT-GLB-PACK-MANIFEST-1 (Python; JSON manifest
`docs/testing/mech_glb_external_pack.json` declares imported data/tgl GLBs with
tracked/external/generated source types; Flea.glb = external/deploy-only; strict
via `MC2_MECH_GLB_PACK_STRICT=1`). DEFERRED — FIT-PARSE (file subsystem),
STATIC-PROP (classification entangled, extraction-gated like objmgr was). Next
targets ranked in [harness-target-sweep-2.md](harness-target-sweep-2.md). Ten
harnesses; runner green.

### deploy_release_tree_contract_harness usage
`py -3 tools/deploy_release_tree_contract_harness/deploy_release_tree_contract_harness.py --release-root <deployed-tree>`
(or set `MC2_RELEASE_ROOT`). Unconfigured → all tests PASS as no-ops with a
"not configured" diagnostic, so the aggregate runner stays green on a fresh
checkout. `MC2_RELEASE_TREE_STRICT=1` (or `--test strict_full_runtime_payload`)
additionally requires every FFmpeg DLL + the launcher. Imports `deploy_payload.py`
constants — no duplicated lists. Validated against a known-good v0.4 tree
(87 shaders, 8 .fst, all DLLs + launcher present).

## Python harnesses (since DEPLOY-ASSET-CONTRACT-HARNESS-1)

When a contract's **source of truth is Python** (e.g. `scripts/deploy_payload.py`
constants), a C++ harness would be fake-green by construction (it would duplicate
the lists). Such harnesses are written in Python against the same contract:
- `tools/contract_harness_common/contract_harness.py` — Python mirror of the C++
  framework (`Harness`/`Ctx`, `--list/--test/--json/--seed`, same JSON shape,
  exit 0/1/2, stdout-owned-by-framework / diagnostics-to-stderr, `in_default`).
- `run_contract_tests.py` has TWO explicit registries (no discovery):
  `REGISTERED_HARNESSES` (native exes, found across build dirs) and
  `PY_HARNESSES` (repo-relative `.py`, invoked via the current interpreter).
**Branch / worktree:** `claude/contract-harnesses-1` @ `A:/Games/mc2-contract-harnesses`
**Base:** nifty HEAD `4c177ea7`.

## Problem

Today every C++ correctness fix (UAF, OOB, watchID exhaustion, allocator
overflow, parser overrun) is first proved by the full 5-mission tier1 smoke.
Smoke is slow (~150s), launches the game, and is a *bad* way to force a rare
edge case — many crash paths are hard to hit naturally in a 30s mission.

**Goal:** a set of small standalone harness executables that link production
code where practical and force exact edge cases **without launching a mission**.
Smoke becomes the final integration gate, not the only proof.

> **Hard principle.** Every crash/corruption fix whose edge case is hard to hit
> naturally in tier1 smoke should get a cheap contract harness or forced
> micro-test.

## Model: `tools/mech_import_harness`

The existing `tools/mech_import_harness` is the proven pattern (do not rewrite):

- **Standalone CMake project** (`project(...)`, its own `build64-harness/` dir).
  It is **NOT** wired into the root `CMakeLists.txt`. Building a harness never
  touches `build64/` and never requires vcpkg.
- **Minimal linkage.** 1A linked only `assimp::assimp`; 1C added exactly one
  mclib TU (`mech_skel_import.cpp`) that includes only Assimp + std. *The linker
  is the proof it stays game-free.*
- **Reporting by exit code + printf.** `FAIL: ...` lines, a summary line, exit
  0 = pass / nonzero = fail. Mode dispatch off `argv[1]`.
- **No game launch, no GL, no gameos, no mission data.**

This arc generalizes that pattern with a shared header and a uniform CLI.

## Harness template convention

### Folder layout

```
tools/contract_harness_common/contract_harness.h   # header-only shared infra
tools/<name>_harness/<name>_harness.cpp            # one harness per subsystem
tools/<name>_harness/CMakeLists.txt                # standalone project
tools/<name>_harness/fixtures/                      # optional test data files
tools/run_contract_tests.py                        # discovers + runs harnesses
```

### CMake pattern

Copy `tools/contract_smoke_harness/CMakeLists.txt`. Each harness is a top-level
`project()` built into its own dir (`build64-contract`, or a per-harness dir).
It adds `tools/contract_harness_common` to its include path and links the
**smallest** production TU subset it needs (often zero). Do **not** add harnesses
to the root `CMakeLists.txt` — keeping them standalone is what guarantees "no
production behavior change."

### CLI convention (uniform, provided by `Harness::run`)

| Flag | Meaning |
|---|---|
| `--list` | list test names (human or `--json`) |
| `--test <name>` | run only the named test (overrides the default suite; can run an `inDefault=false` demo test) |
| `--json` | emit machine-readable JSON instead of human text |
| `--seed <n>` | seed for any randomized test (`TestCtx::seed`) |
| `-h`/`--help` | usage, exit 2 |

### Output format

Human:
```
  [PASS] foo (0 ms)
  [FAIL] bar (1 ms) - file.cpp:42 CH_CHECK(x == y)
my_harness: FAIL (2 tests, 1 failure, 1 ms)
```

JSON (per the arc spec):
```json
{"harness":"contract_smoke_harness",
 "tests":[{"name":"pass","status":"PASS","ms":1}],
 "status":"PASS","elapsed_ms":1}
```

### Exit-code policy

- `0` — all selected tests passed.
- `1` — at least one test failed.
- `2` — usage / unknown arg / no such test.

`run_contract_tests.py` treats the **exit code as authoritative** and also
honors `status:"FAIL"` in JSON.

### Writing a harness

```cpp
#include "contract_harness.h"
using namespace contract_harness;
static bool test_x(TestCtx& t) { CH_CHECK(t, cond); return true; }
int main(int argc, char** argv) {
    Harness h("x_harness");
    h.add("x", test_x);
    h.add("demo_fail", test_demo, /*inDefault=*/false); // only via --test
    return h.run(argc, argv);
}
```

`CH_CHECK` records `file:line` and the expression, does not early-return (so all
checks in a test report). A test fails by returning false or recording any
`CH_CHECK`/`t.fail()`. Exceptions are caught and reported as failures.

**stdout/stderr contract:** the framework owns stdout (report / JSON). Tests must
write human diagnostics to **stderr** (`std::fprintf(stderr, ...)`). Writing to
stdout corrupts `--json`. `run_contract_tests.py` reads each harness's stdout as
JSON; harness diagnostics on stderr are passed through to the console.

**Repo-relative harnesses** (those that scan source/assets, like the shader
harness) bake the repo root via CMake `target_compile_definitions(...
MC2_REPO_ROOT="${MC2_ROOT}")`, overridable at runtime with env
`MC2_CONTRACT_REPO_ROOT`. This needs no new CLI flag, so the shared `Harness`
stays generic.

## Runtime targets

- Individual harness: **< 1 s**.
- Whole non-GL harness suite: **< 10 s**.
- Keep linked production code minimal — slow harnesses defeat the purpose.

## Production linkage map (planned per harness)

| Harness | Links directly | Globals to stub | Entanglement risk |
|---|---|---|---|
| `contract_smoke_harness` (shipped) | none | none | none — pure template |
| `objmgr_contract_harness` | watch-list math from `objmgr` if extractable | ObjectManager singletons, GameObject ctor chain | **HIGH** — full ObjectManager pulls game globals. First slice may use a narrow extracted helper for watch-list math (no silent duplication). |
| `txmmgr_bounds_harness` | `getVertexBlock`/`getBlock` allocator from `txmmgr` | GL handles, texture upload, global tex manager init | **MED** — allocator cursor math may be extractable from GL. |
| `fit_parse_harness` | `.fit`/`.mdf`/inifile parser TU | file IO globals, FST archive | **MED** — parser may need temp fixture files only. |
| `shader_contract_harness` (shipped) | none (filesystem/source scan) | none | **LOW** — scans engine source for `"shaders/X"` refs + enumerates shaders/, no GL. |
| `render_state_contract_harness` (shipped) | `mclib/render_contract.cpp` + `RenderCore/RenderResourceRegistry.cpp` (the real TUs) | none | **LOW** — scope/order paths are pure-CPU. Link-time deps glew32+opengl32 (header types only, no GL context). Needs `3rdparty/lib/x64/glew32.lib` (unzip `3rdparty.zip` in the worktree). `getPassScopeViolationCount()` is cumulative process-wide → tests assert DELTAS. |

## How this fits with smoke

- **Harness** proves the *forced edge case* deterministically and cheaply.
- **tier1 smoke** proves *integration only* (real load, real GL, real frames).
- A correctness fix should land with a harness test for the edge case **and**
  pass tier1 for integration. The harness is the first proof; smoke is the gate.

## Risks (explicit)

1. **Over-stubbing hides bugs.** A stub that returns a "nice" value can mask the
   very corruption being tested. Prefer linking the real TU; stub only globals
   that are pure environment (no logic under test).
2. **Linking too much engine code makes harnesses slow.** Pulling a full
   subsystem (and its transitive game globals) blows the <1s budget and the
   game-free guarantee. Extract the smallest unit instead.
3. **Duplicating logic instead of calling it.** A reimplemented watch-list/
   allocator/parser in the harness tests the *copy*, not production. If a unit
   can't be linked, extract it into a shared TU that *both* the harness and
   production call — never silently duplicate.
4. **Fake tests that don't exercise the real bug path.** A test must drive the
   actual production code path of the fix. A green harness that doesn't touch the
   fixed code is worse than none.

## Seams: ready vs. needs-recon

- **Ready (low entanglement):** shader inventory/deploy-manifest checks;
  RenderPassContract nesting (mock-only, no GL).
- **Needs recon first (entangled):** `objmgr` watch-list (ObjectManager pulls
  game globals); `txmmgr` allocator (GL-coupled); `.fit`/`.mdf` parser (file IO
  + FST). For each, the first slice must determine whether the unit links
  directly, needs a small extracted shared helper, or is too entangled to test
  cheaply yet.

## Queue (scoped; not started — see Phase 3 in the arc brief)

1. `OBJMGR-CONTRACT-HARNESS-1` — watchID exhaustion, save-loop clamp, corrupt
   watchSave index rejection, `getByWatchID` range checks.
2. `TXMMGR-BOUNDS-HARNESS-1` — exact-fit vs overflow for `getVertexBlock`/
   `getBlock` (cursor unchanged on overflow), untextured add boundary cases.
3. `FIT-PARSE-HARNESS-1` — malformed `.fit`/`.mdf` fail-safe, OOB inventory
   index, `readIdIntArray`/`readIdFloatArray` capacity, oversize multiline.
4. ~~`SHADER-CONTRACT-HARNESS-1`~~ — **SHIPPED.** referenced-shaders-exist
   (60 refs resolve), removed-postfx-absent (bloom/godray), shader inventory
   (83 runtime shaders), #ifdef-symbol classification for the SPIR-V seam.
5. ~~`RENDER-STATE-CONTRACT-HARNESS-1`~~ — **SHIPPED.** Links the real
   `render_contract.cpp`. Scope subsystem: balanced begin/end, end-without-begin,
   owner-mismatch, nested-ok, missing-end-at-frame-boundary, violation-counter.
   Order-audit (CONTRACT-3): correct-order, missing-writer. 8 tests, <1s.
6. ~~`DEPLOY-ASSET-CONTRACT-HARNESS-1`~~ — **SHIPPED (first Python harness).**
   Imports real `scripts/deploy_payload.py` constants; asserts source-tracked
   payload exists (SUPPORT_SCRIPTS / EDITOR_SUPPORT_TREES / GAME_COOK_TOOLS /
   BUILDING_PBR_PAYLOAD), no stale v0.4c target, repo-relative/no-escape paths.
   Build artifacts (FFMPEG_DLLS/launcher/exe) excluded. 6 tests, <1s.
7. ~~`OBJMGR-CONTRACT-HARNESS-1`~~ — **SHIPPED via a parallel lane** (`8f670a0a`,
   standalone watch-policy contract harness). The deferral was lifted when that
   lane did the helper extraction.
8. ~~`IBL-REGISTRY-CONTRACT-HARNESS-1`~~ — **SHIPPED.** Cheapest tier: links only
   the header-only constexpr registries (`RenderCore/IblHdriRegistry.h` +
   `IblShRegistry.h`), no .cpp/GL/3rdparty. Integrity: index0==default, every
   sky-map name resolves (typo guard), all sky 1-21 mapped, out-of-range→default,
   no duplicate set names, SH names resolve. Asset existence SEPARATED: default
   `hdri_assets_inventory` (informational, never fails) + `hdri_assets_exist_strict`
   (via `--test` or `MC2_IBL_ASSET_STRICT=1`). 8 default tests, <1s.
   **STATIC-PROP deferred:** classification predicates (stableLightSkipEligible,
   proxy/registration, currentness guards) are methods on the appearance/registry
   classes — same entanglement class as objmgr; extraction-gated. See
   lighting-staticprop-harness-recon-1.md.
   **Open (separate from this harness):** 7/8 registry HDRIs are not git-tracked
   — see IBL-HDRI-ASSET-PACK-RECON-1 (deploy-only vs gap; the strict test catches
   drift without blocking clean checkouts).

**FIT-PARSE deferred:** parser is `FitIniFile : File`; tested path is I/O-coupled
(`File::open` needs systemHeap + FST + gosASSERT) → dragging the file subsystem /
over-stubbing risk. See fit-parse-harness-1-recon.md.

**OBJMGR deferred:** OBJMGR-CONTRACT-HARNESS recon (off current nifty `3da176d4`)
found the watch-list edge cases already hardened (OBJMGR-WATCHID-BOUNDS-1 +
WATCHID-LOAD-GUARD-1). The only no-fake-green path is extracting
`code/objmgr_watch_policy.h` — a tier1-gated production touch whose sole
immediate value is testability of just-fixed code. **Deferred until a real
objmgr watch/save change justifies the extraction.** See
[objmgr-contract-harness-1-recon.md](objmgr-contract-harness-1-recon.md).

**Recommended next:** `OBJMGR-CONTRACT-HARNESS-1` (per advisor ruling — order is
template → shader → objmgr). The template is now proven on a real subsystem;
objmgr is the high-value target but will hit ObjectManager/runtime global
coupling, so its first slice must decide: link the real watch-list unit, extract
a shared helper both call, or declare it too entangled to test cheaply yet.
