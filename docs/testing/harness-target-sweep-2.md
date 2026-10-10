<!-- doc-status: current | reviewed:2026-10-11 -->
# HARNESS-TARGET-SWEEP-2

**Status:** RECON ONLY — no code, no production edits. Identifies the next
high-value contract-harness targets after the shipped set.
**Method:** 4 parallel read-only recons (deploy/asset, texture-path, data-contracts,
shader/render/runner/camera) against current nifty HEAD; verdicts verified live.
**Parent arc:** [subsystem-harness-arc-1.md](subsystem-harness-arc-1.md).

## Shipped / DONE (do not duplicate)
contract template+framework (C++ & Python) · shader contract harness · render-state
contract harness · deploy-asset Python harness (source payload lists) · objmgr
watch-policy harness · IBL registry harness + external-HDRI manifest · icon-atlas
harness · mech-import tg-dump/bone-parity/roster CLI · `tests/unit/test_hashing.cpp`
(elfHash/fst key) · `tests/smoke/test_gates.py` (smoke bucket classifier).

## Classification table

| Candidate | Verdict | Type | Production touch | Notes |
|---|---|---|---|---|
| **Render-pass static table contract** | **GREEN** | C++ header-only | none | id-by-value parity, dup ids, reads/writes range, producer/consumer closure over `RenderPassContract.h` — header self-flags field-value staleness as unguarded |
| **Release-tree deploy contract** | **GREEN** | Python filesystem | none | deployed tree runnable-shape; closes "empty shaders/ → black terrain, smoke green" + v0.4/v0.4c/v0.5 confusion |
| **Mech-import GLB asset-pack manifest** | **GREEN** | Python manifest | none | untracked MadCat/Flea GLBs (deploy-only) = IBL-HDRI class; validate `.mcasset.json` lods[].glb + `[Import] Source=` |
| **Smoke gates.py bucket top-up** | **ALREADY-COVERED + YELLOW gap** | Python (existing) | none | `test_gates.py` exists; missing `crash_silent`, `engine_reported_fail`, `heartbeat_freeze_load`, pool_null/asset_oob/missing_file |
| **GLB→MC2 texname derivation** | **YELLOW** | C++ (extraction) | YES | wrong-derived-name magenta bug; justified by active BT2018-1B milestone |
| **Logistics component-CSV tokenizer** | **YELLOW** | C++ (extraction) | YES | REAL pointer-overrun bug (see below); do only as fix+extraction+harness |
| **File/path normalizer cross-agreement** | **YELLOW** | doctest assert | YES if merged | 3 divergent lowercasing rules → Linux/Vulkan case bug; better as assertion in `tests/unit` |
| **Camera frustum/pillarbox pure math** | **GREEN (needs small extraction)** | C++ | YES (small) | confirmed unchanged at HEAD; see camera-harness-recon-1.md |
| elfHash / fst key normalize | ALREADY-COVERED | — | — | `test_hashing.cpp` |
| Smoke bucket classifier (whole) | ALREADY-COVERED | — | — | `test_gates.py` (9 tests) |
| asset_manifest.json | ALREADY-COVERED | — | — | `validate_asset_manifest.py` |
| Sound/music refs | RED | — | — | runtime-dynamic IDs → FST/FitIni |
| Movie/video (.bik) refs | RED | — | — | FST-fallback by design; names dynamic |
| Logistics purchase/variants/inventory | RED | — | — | CSVFile+PacketFile+FitIniFile |
| Pilot/profile/warrior parse | RED | — | — | per-pilot FitIniFile:File |
| Campaign/mission metadata | RED | — | — | campaign.fit → FitIniFile (NOT already-covered; `verify_campaign_assets.py` does NOT exist) |
| Save/load | RED | — | — | FitIniFile throughout; only objmgr watch-policy had a clean seam |
| FullPathFileName::init | RED | — | — | systemHeap + Win32 CharLower |
| Missing-texture magenta policy | RED | — | — | GL side-effect, not a returnable value |
| .ktx2/.tga sidecar derivation | RED | — | — | inside txmmgr upload path |
| Shader variant *compile* permutation | RED | — | — | needs GL/glslang |
| PassIdentity↔RenderPassId mapping | N/A | — | — | no mapping exists in code; not testable until one is added |

## GREEN/YELLOW detail (the actionable ones)

### Render-pass static table contract — GREEN, zero-touch (best new target)
- **Source:** `RenderCore/RenderPassContract.h` (`kRenderPassContracts[]` L133-205, `RenderPassId` L47-55), `RenderResourceRegistry.h` (`RenderResourceId` L10-21).
- **Bug class:** the header's `static_assert` (L210) checks array *length* only; L131-132 explicitly warns it does NOT catch field-value staleness. A row with an out-of-order/duplicate `id`, a `reads[]`/`writes[]` resource out of range or a non-Unknown after the Unknown terminator (silent truncation), or a read resource with **no writer** (the CONTRACT-3 "missing writer" condition, provable statically) all compile clean.
- **Smoke is blind:** these tables feed the editor inspector / closure-audit docs, never validated at game runtime. A stale row mis-reports closure forever; tier1 renders fine.
- **Real not fake-green:** `#include`s the real constexpr headers, iterates the real tables (cheapest IBL tier — no .cpp, no GL, no glew link, unlike render-state).
- **Type:** new standalone C++ header-only harness `tools/render_pass_table_harness`.
- **Tests:** `id == RenderPassId(i+1)` parity; no dup ids; every enum mapped once; reads/writes `< Count` and no mid-array Unknown; every read resource has a writer; required strings (`name`/`ownerSubsystem`/`inspectorSectionId`) non-null (`killSwitchEnv` nullable).
- **Risk:** LOW. ~6 tests, <1s. **Acceptance:** all green from real headers; registered in run_contract_tests.py.

### Release-tree deploy contract — GREEN, zero-touch
- **Source:** the deployed tree (e.g. `A:/Games/mc2-opengl/mc2-win64-v0.4`) validated against `scripts/deploy_payload.py` real constants (`FFMPEG_DLLS` L103-109, `LAUNCHER_NAME` L74, `SHADER_EXTS` L56) + `.deployed_manifest.csv`.
- **Bug class:** shipped-but-broken install — missing exe/launcher/DLLs, **empty/partial `shaders/`** (documented black-terrain trap: dropped `.comp` compiles-fail non-fatally, smoke still PASSES), missing data/`*.fst`, multi-tree confusion (5 candidate trees live).
- **Smoke is blind:** smoke runs the worktree build, never looks at the deployed tree; empty shaders/ passes smoke.
- **Real not fake-green:** imports `deploy_payload` constants (no duplicated DLL/launcher lists), stats real files.
- **Type:** Python `--target-dir <path>`; default suite informational when target absent (IBL strict precedent) so CI without a release tree stays green.
- **Tests:** exe+launcher present; ffmpeg DLLs; shaders nonempty (>~80); data dirs+fst; support scripts; no-stale-v04c; optional manifest drift via `verify_only`.
- **Risk:** LOW, no game/GL. **Acceptance:** points at v0.4 → exit 0; delete a shader/DLL → specific FAIL; absent target → informational pass (strict only under `--target-dir`).

### Mech-import GLB asset-pack manifest — GREEN, zero-touch
- **Confirmed gap:** deployed trees ship `Flea.glb`, `MadCat.glb`, marauder/madcat atlases, but `git ls-files data/tgl/` tracks only Hangar/Quonset GLBs — same untracked-large-asset class as the 7/8 HDRIs.
- **Source:** `.mcasset.json` `lods[].glb` + `[Import] Source=` in `data/tgl/*.ini` (read by `mclib/bdactor_sidecar.cpp:429`, `mclib/mech3d_type.cpp:50`). Parse the single `Source=` key with a trivial regex — NOT the FitIni parser.
- **Bug class:** a `Source=`/mcasset GLB ref neither tracked nor declared = dangling import (silent ASE fallback or GPU-path crash).
- **Smoke is blind:** import gated `MC2_ASSIMP_MECH_IMPORT=1`, off in tier1.
- **Type:** Python, new `docs/testing/mech_import_glb_pack.txt` (IBL pipe-delimited format) + `every_glb_tracked_or_declared` + strict-local mode.
- **Risk:** LOW-MED (cap scope to single-key regex, do not link parser). **Acceptance:** Hangar/Quonset in-repo, MadCat/Flea declared-external; undeclared GLB ref FAILS default; strict FAILS when a declared GLB absent locally.

### Smoke gates.py bucket top-up — YELLOW gap in ALREADY-COVERED, trivial
- `tests/smoke/test_gates.py` (9 tests) exists but does NOT assert `crash_silent` (the single most important crash bucket — a classifier regression mislabeling it as pass makes the whole smoke gate silently blind), `engine_reported_fail`, `heartbeat_freeze_load`, `pool_null`/`asset_oob`/`missing_file`. **Add ~4 tests to the existing file** (not a new harness). Zero production touch, trivial risk.

### YELLOW extraction-gated (do ONLY with a paired fix)
- **Logistics component-CSV tokenizer** (`code/logisticscomponent.cpp:188` `extractString`): no bounds check during scan (release `gosASSERT` is a no-op → `memcpy` overrun on over-long field) and `pFileLine += i+1` walks **one byte past the NUL** on a short/truncated line (~20 sequential extracts → OOB heap read). **Real bug.** Mod CSVs routinely malformed. Land as fix+extraction(`logistics_csv` leaf)+harness, else defer per objmgr rule.
- **GLB→MC2 texname derivation** (`mclib/assimp_importer.cpp:283` `DeriveMC2TextureName`/`:162` `skelMeshDropped`): wrong derived atlas name → magenta mech (256-clamp off-by-one, missed `a_` alpha prefix). Extraction has forward value (active BT2018-1B milestone). Production touch + visual confirm required.
- **File/path normalizer divergence:** `NormalizeKey` (mclib/file.cpp:215, `tolower`) vs `File::open` (`S_strlwr`+loop, L980) vs `FullPathFileName` (Win32 `CharLower`) — three rules, a real Linux/Vulkan case-sensitivity hazard. Best as a cross-agreement assertion in `tests/unit` doctest; merging the three is its own production refactor.

## Ranked recommendation — next 3 (all GO)

1. **`RENDER-PASS-TABLE-HARNESS-1` — GO.** Strongest pure win: zero production
   touch, header-only (cheapest tier), strongly fake-green-resistant, and it
   guards a hazard the header *itself* declares unguarded (field-value staleness +
   static producer/consumer closure). ~1 hour, <1s runtime.
2. **`DEPLOY-RELEASE-TREE-CONTRACT-HARNESS-1` — GO.** Already next in the arc
   queue; zero touch; closes the operationally painful "deployed tree wrong /
   empty shaders → black terrain, smoke green / v0.4-vs-v0.4c" class. Reuses the
   deploy-asset import-the-constants + IBL strict-split patterns.
3. **`MECH-IMPORT-GLB-PACK-MANIFEST-1` — GO.** Zero touch; closes the confirmed
   untracked-MadCat/Flea-GLB gap with the proven IBL external-pack manifest
   pattern. Pairs naturally with the existing mech-import CLI gates.

**Trivial side-GO:** add the 4 missing buckets (esp. `crash_silent`) to
`tests/smoke/test_gates.py` — highest value/effort ratio, hardens the smoke gate's
own classifier.

**Conditional (NO-GO as pure harness; GO only as fix+extraction):**
`LOGISTICS-COMPONENT-CSV` (has a real overrun bug — worth a dedicated fix slice),
`GLB-TEXNAME-DERIVE` (ride the BT2018-1B milestone). Both require a production
touch and are only justified paired with a real fix, per the objmgr precedent.

**Stay deferred:** all FitIni/FST-coupled data (sound, movie, logistics
purchase/variants, pilot, campaign, save/load), txmmgr/GL paths (magenta policy,
.ktx2 sidecar), shader-compile permutations, FullPathFileName. PassIdentity↔
RenderPassId mapping is not testable until a mapping is added to code.

## Live findings surfaced during the sweep (not harness work)
- **Untracked deploy-only GLBs** (Flea/MadCat) — manifest target #3 addresses.
- **compbas.csv tokenizer overrun** (`logisticscomponent.cpp:182/203`) — real latent bug on malformed CSV.
- **3 divergent path normalizers** — latent Linux/Vulkan case bug.
- **`test_gates.py` has no `crash_silent` coverage** — smoke classifier blind spot.
- **`scripts/verify_campaign_assets.py` does not exist** — an earlier assumption it covered campaign metadata is wrong; campaign metadata stays RED (FitIni).
