<!-- doc-status: current | reviewed:2026-10-11 -->
# MC2 Mod Packaging & Deployment Architecture

**Status:** Strategy / design doc (long-term), 2026-06-10.
**Scope:** How mods are authored as local projects, packaged into distributable bundles, installed/uninstalled/rolled back into a deploy, and discovered by editor + mc2.exe.
**Siblings:** `mc2-modding-toolchain-architecture.md` (tool ownership + bridges), `runtime-bridge-architecture.md` (editor↔game process model). This doc owns the **packaging/deploy lane** of that architecture.

---

## 1. North star

> **A mod is a plain folder that is simultaneously: the editable project, the runtime overlay, and (zipped) the distributable package. Packaging adds metadata and integrity, never a new format. Installing is an indexed, reversible file operation. mc2.exe stays the only authority on what a mod *means* at runtime.**

Three load-bearing consequences:

1. **`mods/<id>/` is already the runtime contract** — `mclib/file.cpp:62-67` ("Mod overlay — session-scoped, single active mod", `MC2_ACTIVE_MOD` env, priority `active mod > dependency N..0 > base data/ > FastFiles > CD`, O(1) hash lookup via `g_modIndex`). The package layer wraps this; it never replaces it.
2. **No second asset database.** The mod index (`g_modIndex` + `.modindex-cache`, `file.cpp:160-345`) and the engine's existing manifests (`mod.json`, `models.json` via `mclib/model_override_registry.cpp:162`) are the only registries. The package layer adds a *package manifest* describing files-and-versions, not assets-and-meanings.
3. **Reversible by construction.** Every install operation is "copy files into a fresh `mods/<id>/` + write a receipt"; every shared-file mutation (central `models.json`) goes through the already-shipped `.bak` + atomic-rename + round-trip-verify pattern (`tools/asset_viewer/CentralManifestMerge.cpp:58-79`).

---

## 2. Mod project folder layout (minimum viable + full)

The project folder IS the install folder IS the unzipped package. One layout, three roles.

```
mods/<modId>/
  mod.json                  # REQUIRED. Identity + deps (engine-parsed today: file.cpp:393-411
                            #   ReadModJson — id, name, dependencies[])
  package.json              # NEW, package layer only. Version/files/integrity (§3).
                            #   Engine NEVER reads this.
  data/                     # The overlay root. Mirror of base data/ — anything under
                            #   data/ shadows the base file of the same relative path
                            #   (first-wins indexing, file.cpp:74-120 IndexModData;
                            #   only "data/..." relative paths are mod-searchable,
                            #   file.cpp:60 ShouldSearchMods)
    missions/*.fit *.pak    # missions (editor save format, frozen)
    campaign/*.fit          # campaigns — auto-discovered & injected into the campaign
                            #   list by EnumerateModCampaignFiles (file.cpp:443-463)
    tgl/                    # appearance .ini/.tgl + texture tiers {128,256,512,1024}/*.ktx2
    model_overrides/        # models.json + GLB payloads (model override registry)
    effects/  sound/  art/  textures/  objects/  abl/   # same-name shadowing
  src/                      # OPTIONAL, project-role only. Uncooked sources (GLB, PSD,
                            #   .ase). NEVER indexed (not under data/), stripped from
                            #   distributable packages by default.
  out/                      # OPTIONAL. Regeneratable cook artifacts; gitignore-style
                            #   excluded from packages (matches asset-manifest-schema.md
                            #   generatedOutputs convention).
  .modindex-cache           # engine-generated, per-machine (file.cpp:161). Never packaged.
  .install-receipt.json     # NEW, written by installer at install time (§5). Never packaged.
```

**Minimum viable project = `mod.json` + `data/` with one file in it.** Everything else is additive. A folder with no `mod.json` still loads (folder name becomes the id, no deps — `file.cpp:518-521`), so hand-rolled mods keep working.

### How a mod declares each asset class

Declaration = **putting the file at the path the engine already resolves**, plus a manifest only where the engine already has one. No new declaration syntax.

| Asset class | Declared by | Resolved by |
|---|---|---|
| Models (overrides) | `data/model_overrides/models.json` records (`{"overrides":[...]}`, key = `class:appearanceName`, `renderOnly`, `fallback:"stock"`) + GLB next to it | `mclib/model_override_registry.cpp:162` |
| Textures | shadow file at same relative path: `data/tgl/<tier>/*.ktx2`, `data/textures/...` | mod index first-wins → `txmmgr`/`KtxLoader` (unchanged) |
| Appearances | `data/tgl/*.ini` (+ `.tgl`) shadowing or adding entries | `bdactor.cpp`/`msl.cpp` loaders via the same file open path |
| Missions | `data/missions/<name>.fit` + `<name>.pak` (PacketFile — format frozen, MOVE packet 4 rules per `editor/EditorData.cpp`) | `code/mission.cpp` |
| Campaigns | `data/campaign/<name>.fit` | `EnumerateModCampaignFiles` (`mclib/file.cpp:644`) |
| Effects | `data/effects/mc2.fx` shadow (whole-file; finer-grain split is engine work, out of scope here) | gosFX loader |
| Sounds | `data/sound/*.wav` shadow | `gameos_sound.cpp` |
| Scripts | `data/missions/*.abl` (incl. `nop.abl` stub pattern proven in mc2x-compat) | ABL loader |

Per-asset *authoring* manifests (the `assetId/capabilities/materials` schema in `docs/asset-manifest-schema.md`) live under `src/` as cook inputs — they are workbench/validator artifacts, **never** consumed by mc2.exe.

---

## 3. Package manifest proposal (`package.json`)

The distributable bundle = `<modId>-<version>.zip` of the project folder, minus `src/`, `out/`, dotfiles. `package.json` is generated at pack time by the packer tool. **mc2.exe never reads it** — runtime identity stays `mod.json` (anti-split-brain: one file per consumer).

```json
{
  "schema": "mc2-mod-package/1",
  "id": "mc2x-pbr",                      // MUST match mod.json id
  "version": "1.2.0",                    // semver; install/rollback key
  "name": "MC2X PBR Pack",
  "authors": ["..."],
  "engineMin": "0.4",                    // advisory; installer warns, never blocks hard
  "dependencies": [                      // mirrors mod.json deps, adds version ranges
    { "id": "mc2x-compat", "version": ">=1.0" }
  ],
  "files": [                             // every packaged file: relPath + size + sha256
    { "path": "data/tgl/512/abuilding.ktx2", "size": 174904, "sha256": "..." }
  ],
  "overridesBase": ["data/effects/mc2.fx"],   // declared shadows of BASE files (computed
                                              // at pack time vs a base-file index) — the
                                              // conflict detector's fast path (§6)
  "providesMissions": ["m1", "m2"],           // derived from data/missions/*.fit
  "providesCampaigns": ["mycampaign"],
  "signature": null                           // future: detached sig over the files[] hashes
}
```

Notes:
- `files[].sha256` gives checksums now and is the substrate for future signing (sign the manifest, manifest pins the files) — no format change later.
- The pack tool *derives* `providesMissions`/`overridesBase` from the folder; the modder never hand-maintains them (generated section, like `models.generated.json` from `BundleExport.h`).
- Validation = a Python validator alongside `tools/validate_asset_manifest.py`, same shape-only philosophy.

---

## 4. Install / deploy flow

**Deploy targets are real game dirs** (`A:/Games/mc2-opengl/mc2-win64-v0.4/`, `0.4c/`). One installer tool (CLI first, UI later — see §7), `tools/mod_install/` (Python, like the cook/smoke tooling):

```
mc2mod install <bundle.zip|projectDir> --deploy <gameDir> [--link]
mc2mod uninstall <modId> --deploy <gameDir>
mc2mod list / verify / rollback <modId> --to <version>
```

1. **Read + validate** `package.json` (schema, id==mod.json id, hashes of zip contents).
2. **Conflict scan** (§6) against already-installed `mods/*/package.json`. Report; require `--force` to proceed on hard conflicts.
3. **Stage** to `mods/.staging/<id>-<version>/`, verify hashes, then atomic-rename to `mods/<id>/` (prior version, if any, renamed to `mods/.trash/<id>-<oldver>-<timestamp>/` until commit).
4. **Write receipt** `mods/<id>/.install-receipt.json`: bundle hash, version, install time, file list, plus any *shared-file mutations* performed (e.g. central `models.json` merge entries) so uninstall knows exactly what to reverse.
5. **Playtest install (`--link`, local dev):** instead of copying, write `mods/<id>/` as a junction/symlink to the working project folder. Engine sees a normal folder; `MC2_REBUILD_MOD_CACHE=1` (`file.cpp:288`) is set by dev launch scripts so the `.modindex-cache` never goes stale against live edits. This is the editor's one-click-playtest path: zero copy, instant iteration.

**Launching:** the launcher (bat file today, launcher UI later) sets `MC2_ACTIVE_MOD=<id>` and spawns mc2.exe — exactly today's mechanism (`file.cpp:491`). Single-active-mod-plus-dependencies stays the runtime model; "load order" UIs are just choosing the active mod and editing its `dependencies` list. Multi-active composition is an engine decision deferred until needed (the dep chain already gives layered priority).

**Editor discovery:** editor enumerates `mods/*/mod.json` at the deploy root, shows a mod picker, and when a mod project is active: saves missions into `mods/<id>/data/missions/`, lists mod assets in the Asset Browser (it already reads through the same `file.cpp` open path when launched with `MC2_ACTIVE_MOD`), and launches playtests via `EditorTaskRunner` with `MC2_ACTIVE_MOD` in the child env (launch seam at `editor/EditorInterface.cpp:5301-5326`). No in-process coupling — the mod folder is the only shared state.

**Asset Viewer feed:** the Workbench's `exportBundle` (`tools/asset_viewer/ModWorkbench.h:37`, `BundleExport.h`) already writes `<out>/<id>/{model.glb, models.generated.json}`. The seam: point `<out>` at `mods/<id>/data/model_overrides/` and use the S5 central-merge (`CentralManifestMerge.cpp` — `.bak`, atomic temp→rename, round-trip verify, auto-rollback on verify failure) against the **mod's** `models.json`, never the base game's. Viewer exports land *in the project*; the project flows to the package; nothing is exported "into the game" directly.

---

## 5. Rollback model

Three nested layers, all file-grained:

1. **Whole-mod**: uninstall = delete `mods/<id>/` + reverse receipt-listed shared-file mutations. Rollback-to-version = keep N prior versions in `mods/.trash/` (or re-install an archived bundle); rename-swap back. Because the engine resolves through `mods/<id>/data/` *at file-open time* with zero persistent registration, removing the folder is a complete logical uninstall — there is nothing else to unwind. `.modindex-cache` self-invalidates on mtime (`file.cpp:269-279`).
2. **Shared-file mutations** (the only writes outside `mods/<id>/`): central manifest merges use the shipped `.bak` + verify + auto-restore pattern (`CentralManifestMerge.cpp:58-79`). The receipt records each merged record key so uninstall removes *those records*, not the whole file.
3. **Base game is never touched.** The installer refuses to write outside `mods/` (same spirit as `ShouldSearchMods` rejecting `..` and absolute paths, `file.cpp:60-66`). Therefore "rollback to vanilla" = unset `MC2_ACTIVE_MOD`. This is the invariant that makes saves safe: save data (`data/missions/save.fit`) stays in base `data/`, never inside a mod.

---

## 6. Conflict model

Two tiers, both computed from `package.json.files[]` + `overridesBase[]` — no engine involvement:

- **Mod↔base** (informational): mod shadows a base file. This is the *point* of mods; surfaced in the install report, never blocking.
- **Mod↔mod** (the real problem): two installed mods that could appear in one dependency chain both provide the same `data/...` relative path. Detector intersects `files[].path` sets across `mods/*/package.json`. Severity:
  - **Identical sha256** → benign, dedupe note.
  - **Different content, both in the active chain** → WARN with the resolution that *will* happen (dependency order is deterministic: active mod wins, then dep[0]..dep[N] — `file.cpp:547`, mirrored at runtime by the `[mod-dup]` first-wins log, `file.cpp:108-112`). Show winner/loser exactly as the engine will pick them.
  - **Semantic conflicts** (two mods adding override records for the same `class:appearanceName` key in their own `models.json` files) → detected by parsing the manifests, same key-intersection logic. WARN.
- **Runtime is the oracle**: `MC2_LOG_FILE_RESOLVE=1` already prints `[mod-hit]/[mod-dup]` winners (`file.cpp:70, 415-421`). The installer's static prediction must match the engine's dynamic behavior; a smoke check can diff them.

No lockfiles, no priority numbers, no registry: conflict *resolution* is "edit the dependency list or remove a mod", which keeps mc2.exe the sole authority on layering.

---

## 7. Tool ownership map (packaging lane)

| Stage | Owner | Must NOT own |
|---|---|---|
| Asset import/cook/validate, override records, GLB→bundle export, central-manifest merge | Asset Viewer Workbench (`tools/asset_viewer/ModWorkbench.*`, `BundleExport.*`, `CentralManifestMerge.*`) + headless `trackg_cook.py` | mission data; deploy; package versioning |
| Mission/campaign authoring, save to `mods/<id>/data/missions/`, playtest launch | Editor (`EditorData.cpp` save path, `EditorTaskRunner` launch) | asset cooking; manifest merges; package install |
| Pack / install / uninstall / rollback / verify / conflict scan | NEW `tools/mod_install/` CLI (later thin launcher UI) | asset meaning, file-format knowledge beyond "path + hash"; never parses .pak/.tgl |
| Runtime resolution, layering, campaign injection, save/load | mc2.exe (`mclib/file.cpp` overlay, `model_override_registry`, PacketFile) | nothing new — it already owns it |

**What the package layer must NEVER own or duplicate:** asset semantics (no parsing PacketFiles, TGL, FIT beyond reading `mod.json`/`models.json` keys); the file-resolution algorithm (predict-and-warn only — the engine's first-wins index is truth); an asset database (file paths + hashes only); save-game data; FastFile contents (FST stays a base-game distribution format; mods ship loose files — the overlay already outranks FastFiles); cook logic (always shells to `trackg_cook.py`).

---

## 8. Bridge seams (all file-based, per the no-IPC constraint)

1. **Viewer → project:** `exportBundle(outRoot=mods/<id>/data/model_overrides/, ...)` + S5 merge into the mod's `models.json`. Existing code; only the default target changes.
2. **Editor → project:** "Active mod project" setting = a mod id; save/load paths and asset browser rooted accordingly; `Build Mod Package` button (`EditorInterface.cpp:5330-5376`, currently rough) re-pointed to shell out to `mc2mod pack`.
3. **Editor → game:** `EditorTaskRunner` spawn with `MC2_ACTIVE_MOD` + `-mission <pak>`; telemetry back via stdout/`debug_state` per `runtime-bridge-architecture.md`.
4. **Installer → engine:** none at runtime. The only handshake is folder layout + `mod.json` + env var. `MC2_REBUILD_MOD_CACHE=1` for linked dev installs.
5. **CI:** `mc2mod verify` + manifest validators + the smoke harness running `MC2_ACTIVE_MOD=<id>` tier1 — the mc2x-compat precedent (6/6 campaign smokes) is the template for "mod CI".

---

## 9. Anti-goals (binding)

- **No engine-wide asset database** — manifests + folder layout only.
- **No PacketFile / FST format changes**; no repacking base archives. (`mclib/packet.cpp`, `fastfile.cpp` untouched.)
- **No in-process editor/game/viewer integration** — process spawn + files, period.
- **No Steam Workshop / online assumptions** — zip-over-any-channel; signing optional and detached.
- **No multi-active-mod composition UI** until a real need; the dependency chain is the composition mechanism.
- **No installer writes outside `mods/`** (except receipt-tracked central-manifest merges, which are themselves inside the deploy's data tree and `.bak`-reversible).
- **mc2.exe is runtime authority** — tools predict, the engine decides; any predict/actual divergence is a tool bug.

## 10. Risks

| Risk | Mitigation |
|---|---|
| `.modindex-cache` staleness on linked dev installs (2-level mtime sweep, `file.cpp:170-204`, misses deep edits) | dev launch scripts set `MC2_REBUILD_MOD_CACHE=1`; `mc2mod verify` re-hashes |
| Deploy-target split-brain (game runs v0.4, editor 0.4c — known trap from memory) | installer takes explicit `--deploy`; `mc2mod list` shows per-deploy state; never "install everywhere" implicitly |
| `mod.json` parsed by a minimal hand extractor (`file.cpp:349-390`) — fancy JSON (nested objects in deps) would silently misparse | package validator enforces the *flat* subset the engine parses; versioned deps live only in `package.json` |
| Whole-file shadowing of monoliths (`mc2.fx`, big `.fit`s) makes mods collide coarsely | acknowledged; finer granularity is future engine work, conflict detector at least makes it visible |
| Receipt loss (user deletes `.install-receipt.json`) | uninstall degrades gracefully: folder delete always works; central-manifest cleanup falls back to scanning records whose `sourceRelPath` points into the removed mod |
| Mission .pak invariants (MOVE packet 4, `moveSide ≤ 720`) violated by hand-made mods | not the installer's job; editor validator + runtime synthesis fallback (already shipped in `mission.cpp`) own it |

## 11. Phased roadmap

- **P0 — Codify the contract (docs + validator).** Folder-layout spec, `package.json` schema + `tools/validate_mod_package.py`, fixture mod. No behavior change.
- **P1 — `mc2mod` CLI: pack/install/uninstall/verify.** Receipts, staging, hash verify, `--link` dev installs. mc2x-compat becomes the first packed fixture.
- **P2 — Conflict scan + `list`/`rollback`.** Static winner/loser prediction, `[mod-dup]` parity smoke.
- **P3 — Tool integration.** Viewer export targets a mod project by default; editor mod-picker + save-into-mod + playtest env; `Build Mod Package` shells to `mc2mod pack`.
- **P4 — Distribution polish.** Launcher-side install UI, version pinning, optional detached signatures over `files[]` hashes.

## 12. First 5 implementation slices

1. **`docs/mod-package-format.md` + `tools/validate_mod_package.py` + fixture** `tests/fixtures/mods/minimal-mod/` (mod.json + package.json + one shadow file); wire into the existing check-script family.
2. **`mc2mod pack`**: project folder → zip + generated `package.json` (hashes, derived `providesMissions`/`overridesBase`), excludes `src/`, `out/`, dotfiles. Round-trip test: pack mc2x-compat, validate.
3. **`mc2mod install/uninstall`** with staging + receipt + trash-based version retention; smoke: install fixture into a temp deploy, run `MC2_ACTIVE_MOD` tier1 single-mission, uninstall, verify byte-identical deploy.
4. **`mc2mod install --link`** + dev launch script (`run-mod.bat` setting `MC2_ACTIVE_MOD` + `MC2_REBUILD_MOD_CACHE=1`).
5. **Conflict scanner** (`mc2mod check`): path-set + sha256 intersection across installed mods + `models.json` key intersection; assert prediction matches `MC2_LOG_FILE_RESOLVE=1` `[mod-dup]` output in a scripted run.

## 13. Follow-up prompts (Opus/Codex)

1. *"Implement slice 1+2 of `docs/superpowers/strategy/mod-packaging-deploy-architecture.md` in worktree `.claude/worktrees/nifty-mendeleev`: write `docs/mod-package-format.md`, `tools/validate_mod_package.py` (shape-only, mirroring `tools/validate_asset_manifest.py` style), fixture `tests/fixtures/mods/minimal-mod/`, and `tools/mod_install/mc2mod.py pack`. Constraints: Python 3 stdlib only, no engine changes, exclude src/ out/ dotfiles from packs, sha256 every packed file. Gate: pack the fixture and `mods/mc2x-compat` from deploy `A:/Games/mc2-opengl/mc2-win64-0.4c/`, both validate exit 0."*
2. *"Implement `mc2mod install/uninstall/verify/--link` per §4-5 of mod-packaging-deploy-architecture.md: staging dir, atomic rename, `.install-receipt.json`, trash-based version retention, junction-based `--link`. Write a smoke that installs the fixture into a temp deploy tree, runs nothing (filesystem-only assertions), uninstalls, and asserts the tree is byte-identical to before. Do NOT touch mclib/file.cpp."*
3. *"Build the conflict scanner (`mc2mod check`) per §6: intersect `files[].path`+sha256 across `mods/*/package.json` and override-record keys across mods' `data/model_overrides/models.json`; print predicted winner/loser using the engine's ordering (active > dep[0..N], first-wins per mclib/file.cpp:547). Add a parity test that launches mc2.exe with `MC2_LOG_FILE_RESOLVE=1` and two overlapping fixture mods and diffs `[mod-dup]` lines against the prediction."*
