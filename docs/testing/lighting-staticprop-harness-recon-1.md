<!-- doc-status: stale | reviewed:2026-10-11 -->
# LIGHTING + STATIC-PROP HARNESS TARGET RECON-1

**Status:** RECON ONLY — no code changes. Surveys lighting and static-prop for
cheap game-free harness targets.
**Verdicts:** LIGHTING → **GREEN** (strong target, recommend next). STATIC-PROP →
**DEFER** (classification logic entangled, like objmgr).
**Branch / worktree:** `claude/lighting-staticprop-harness-recon-1` @
`A:/Games/mc2-lighting-harness-recon`, off nifty `97b80c1a`.
**Parent arc:** [subsystem-harness-arc-1.md](subsystem-harness-arc-1.md).

## LIGHTING — GREEN: `IBL-REGISTRY-CONTRACT-HARNESS-1`

### What links game-free
`RenderCore/IblHdriRegistry.h` and `RenderCore/IblShRegistry.h` are **header-only,
constexpr, "no GL, no game-side headers"** (their own firewall comments; include
only `<cstddef>`/`<cstring>` + sibling `IblShCoeffs.h`). A C++ harness `#include`s
them and tests the **real tables** — zero duplication, the cleanest tier yet
(no .cpp link, no glew, unlike render-state).

`RenderCore/SceneLighting.h` is a **pure POD** (float arrays, no logic) → nothing
to assert; not a target. `GameOS/gameos/ibl_sh_runtime.h` is an extern decl only.

### Bug-prone invariants — invisible to smoke, instant for a harness
`lookupHdriForSkyNumber` (IblHdriRegistry.h:145) silently returns `kIblHdriSets[0]`
("default") when a `kSkyNumberHdriMap[].hdriSetName` **does not resolve**. So a
single-character typo in a mood name (e.g. `"day_clr"`) degrades that sky to the
default with **no error** — a 30s smoke never notices (sky just looks slightly
off). Candidate tests (all over the real header):
- `index0_is_default` — `kIblHdriSets[0].name == "default"` (load-bearing fallback).
- `every_skymap_name_resolves` — every `kSkyNumberHdriMap[].hdriSetName` is found
  by `findHdriSetByName` (**the typo guard**).
- `all_sky_numbers_1_21_mapped` — sky numbers 1..21 each map (or intentionally
  default); flags an accidentally-dropped sky.
- `lookup_out_of_range_is_default` — `lookupHdriForSkyNumber(0/22/-1/INT_MAX)`
  returns `kIblHdriSets[0]`.
- `no_duplicate_set_names` — `kIblHdriSets[].name` unique (lookup-by-name is
  first-match; a dup silently shadows).
- Same shape for `IblShRegistry.h` (keyed on mission name → SH set; index 0 default).

### Asset-existence contract (same harness, real table = no duplication)
Each `kIblHdriSets[].exrPath` is the asset the loader uses
(`gos_postprocess.cpp:291  newPath = hdriSet.exrPath`). The harness iterates the
real table and checks the asset exists on disk.

> **★ LIVE FINDING (verify before acting).** Of the 8 distinct HDRIs in
> `kIblHdriSets[]`, **only `DaySkyHDRI063B_4K` is git-tracked**; the other 7
> (`pizzo_pernice/citrus_orchard/belfast_sunset/mud_road/qwantani_night` 16k +
> `NightSkyHDRI007/008` 16k) are **absent from the repo** — no `.exr`, no `.ktx2`,
> not even LFS pointers (`.gitattributes` LFS-tracks `*.exr`, but these files were
> never committed). In a clean checkout only sky→`default` resolves to a present
> asset; every other sky number references a missing file and the loader falls
> back silently. This is either an intended "assets deployed out-of-band" setup or
> a real source-asset gap. **An asset-existence harness flags it immediately** —
> which is the point. Owner should decide: commit the 7 HDRIs (LFS), or mark them
> deploy-only and scope the test to source-tracked entries.

### Implementation shape (next slice, if GO)
- `tools/ibl_registry_contract_harness/` — standalone CMake, links **nothing**
  (header-only include of `RenderCore/IblHdriRegistry.h` + `IblShRegistry.h`),
  uses `contract_harness.h`. Add `MC2_REPO_ROOT` (like shader harness) for the
  asset-existence checks. Register in `REGISTERED_HARNESSES`.
- Lookup-invariant tests are pure (no filesystem). Asset-existence test scoped to
  source-tracked entries (or all, with the live finding as the first red flag).
- Cheapest harness in the arc: no .cpp link, no GL, no 3rdparty.

## STATIC-PROP — DEFER (classification logic entangled)

The bug-prone static-prop logic is **classification predicates** —
`stableLightSkipEligible`, proxy-eligibility, static-registration state,
currentness guards (STATIC-REGISTRY-CURRENTNESS-GUARD-2, R2B touch-preserve). These
live as **methods on the appearance / registry classes**: `code/bldng.cpp`,
`mclib/appear.h`, `mclib/bdactor.cpp`, `GameOS/gameos/gos_static_prop_registry.cpp`,
`GameAdapters/StaticPropRenderAdapter.h`. They are coupled to the `GameObject` /
appearance hierarchy and the registry's GL-adjacent state — **same entanglement
class as objmgr**: no clean game-free link, and a no-fake-green harness would need
a production helper extraction (pull the predicate math into a pure header both
production and harness call).

**Verdict: DEFER** — mirror the objmgr ruling. Do the extraction only when a real
static-prop classification bug or a planned registry change justifies the
tier1-gated production touch. (`gos_static_prop_registry.cpp` may be the most
self-contained piece — worth a *targeted* recon if static-prop becomes a priority,
to see whether the registry's index/coverage bookkeeping is separable from the
appearance hierarchy.)

## Recommendation
**Next harness = `IBL-REGISTRY-CONTRACT-HARNESS-1`** (lighting). It is the
lowest-cost target found so far (header-only, no link, no GL), guards a real
silent-degradation class (mood-name typos → default sky), and its asset-existence
test already surfaced a concrete gap (7/8 HDRIs not source-tracked). Static-prop
stays deferred behind the same extraction gate as objmgr.

## Recon hygiene
Ran against current nifty HEAD `97b80c1a`; verified header purity, the loader call
site, and asset/LFS state live (not from memory).
