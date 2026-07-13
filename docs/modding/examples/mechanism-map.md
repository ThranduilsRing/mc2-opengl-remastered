# Data-Modding Mechanism Map

Reference table of verified data-modding mechanisms in this checkout. Every anchor below was spot-checked against the source at the time of writing (grep + read, not memory) — if a line has drifted since, trust the surrounding function/symbol name over the exact line number and re-grep.

Start with [`getting-started.md`](getting-started.md) if you haven't set up a mod folder yet — this page assumes you already know what `mods/<id>/data/...` and `MC2_ACTIVE_MOD` mean.

## Mod overlay core

| Mechanism | Entry point | Gate / env | Drop path | Status |
|---|---|---|---|---|
| Mod activation | `InitModSearchPaths`, `mclib/file.cpp:632` (reads `MC2_ACTIVE_MOD` at `:668`) | `MC2_ACTIVE_MOD` (unset = base game) | — | Shipped, this checkout |
| Priority order (active > deps > base > FastFile > CD) | `mclib/file.cpp:700-720` (index build, highest-priority-first, first-wins) | — | — | Shipped |
| Dependency chain mounting | `mclib/file.cpp:724-740` | `mod.json` `"dependencies"` array | `mods/<dep-id>/data/...` | Shipped |
| `mod.json` parser (minimal JSON extractor) | `ReadModJson`, `mclib/file.cpp:524-539` | — | `mods/<id>/mod.json` | Shipped. Only reads `id`/`name`/`dependencies`; `type`/`description` are launcher-only fields, not read by the engine |
| Whole-file index build | `IndexModData`, `mclib/file.cpp:213-274` | — | recurses `mods/<id>/data/`, dot-files/dirs skipped | Shipped |
| Key normalization | `NormalizeKey`, `mclib/file.cpp:202-206` | — | — | lowercase + `\`→`/`, case/slash-insensitive matching |
| Mod index disk cache | `TryLoadModCache`/`WriteModIndexCache`/`LoadModIndexCache`, `mclib/file.cpp:344-437` | `MC2_REBUILD_MOD_CACHE` (any value forces rebuild) | `mods/<id>/.modindex-cache` (auto-generated) | Shipped. Invalidation = 2-level directory-mtime sweep (`GetModDataMtime`, `:298-333`) — see stale-cache trap in getting-started.md §6 |
| File-resolve diagnostics | `mclib/file.cpp:637-666`, `EmitResolveRecord` `:127-` | `MC2_LOG_FILE_RESOLVE` (0/1/2), `MC2_RESOLVE_TRACE_FILE` (path, level-2 only) | — | Shipped |

## Launcher / packaging tooling

| Mechanism | Entry point | Gate / env | Drop path | Status |
|---|---|---|---|---|
| Launcher mod picker | `tools/mc2_launcher/mc2_launcher.cpp` — `ScanMods` `:62-113`, `DoLaunch` `:117-140` | Sets `MC2_ACTIVE_MOD` in the child `mc2.exe` process env | scans `<launcher-dir>/mods/*/mod.json` | Shipped. Only `type:"campaign"` entries are listed; zero mods found = skip UI, launch base game |
| Mod packager (`pack`) | `tools/mc2mod/mc2mod.py`, `cmd_pack` `:156-205` | — | writes `<id>-<version>.mc2mod` | Shipped |
| Package integrity check (`verify-lite`) | `tools/mc2mod/mc2mod.py`, `cmd_verify_lite` `:224-294` | — | re-hashes every archive member vs `package.json` | Shipped |
| Installer (`install`) | `tools/mc2mod/mc2mod.py`, `cmd_install` `:363-453` | Canonical-root guard `:55-58, 372-385` (refuses `a:/games/mc2-opengl/mc2-win64-v0.4` and `-0.4c`, no override flag in v1) | `<deploy>/mods/<id>/` + `.install-receipt.json` | Shipped. Use your own deploy dir, not a project canonical root |
| Uninstaller (`uninstall`) | `tools/mc2mod/mc2mod.py`, `cmd_uninstall` `:460-555` | `--force` to override modified/missing-file check | reads `.install-receipt.json` as ground truth | Shipped |
| Offline resolution replica (for tooling, not the engine) | `tools/mod_install/resolver.py` | — | mirrors `file.cpp`'s ladder for `mc2mod check`/CI parity smoke | Shipped, tooling-only |

## Weapons / equipment (`compbas.csv`)

| Mechanism | Entry point | Gate / env | Drop path | Status |
|---|---|---|---|---|
| Master component table load | `MasterComponent::loadMasterList`, `mclib/cmponent.cpp:630-678` — called with fixed size 255 (`code/mission.cpp:1974`, `code/saveload.cpp:846`) | — | `data/objects/compbas.csv` | Shipped. **Fixed 255-row table** — you cannot add row 256; "adding" a weapon means filling a row whose `Type` column is `undefined` |
| Row parser | `MasterComponent::initEXCEL`, `mclib/cmponent.cpp:87-179` | — | — | Shipped. Column 0 = `masterID` (must match the row's own position); `Type` column `"undefined"` → row is skipped/free (`:111-114`) |
| Weapon/component display name + abbreviation | `mclib/cmponent.cpp:62-63` (`COMPONENT_NAME_START=32000`, `COMPONENT_ABBR_START=33000`), read at `:100-101`/`:288,292` | — | string resource IDs `32000+masterID` / `33000+masterID`, resolved via `cLoadString` from `data/defs/text/en_us/mc2res_legacy.fit` | **TRAP**: the CSV's own `Name` column is parsed then discarded (`:118`, `// name, ignore`) — it is never shown in-game. To rename a weapon you must edit the string-resource `.fit` file at the matching ID, not the CSV |
| Weapon FX binding | `compbas.csv` col `Special FX ID` → `MasterComponent::getWeaponSpecialEffect()` (`mclib/cmponent.h:467`) → `effects.csv` row lookup (`mclib/weaponfx.cpp` init) | — | `data/objects/compbas.csv` (col) + `data/objects/effects.csv` (row) | Shipped, pure data. Full chain documented in [`docs/gosfx-modder-dropin-path.md`](../../gosfx-modder-dropin-path.md) |

## Mechs

| Mechanism | Entry point | Gate / env | Drop path | Status |
|---|---|---|---|---|
| Variant block layout | `BattleMech::init(DWORD variantNum)`, `code/mech.cpp:1404-` — variant name at offset `23 + 97*variantNum` (`:1451`), armor block at `115 + 97*variantNum` (`:1487-1517`), inventory rows `26 + 97*variantNum + curItem` xref `masterID` via column 5 (`:1545`) | — | `<chassis>.csv` (e.g. `data/objects/atlas.csv`) | Shipped. Pure-data variant add = append a new 97-row block to the chassis CSV. New **archetypes** (`ObjectClass` enum) need engine code — out of scope for data mods |
| Mech mesh import opt-in | `[Import]` block `Source=<base>` probe, `mclib/mech3d.cpp:346-360`; LOD0-only gate `:397-404` | Compile-time `ENABLE_ASSIMP_IMPORTER` (`CMakeLists.txt:204`, default `ON` — no rebuild needed on a stock build) | `data/tgl/<mech>.ini`, `[Import]` block | Shipped. Only LOD0 uses the modern importer; every other LOD slot and every mech without an `[Import]` block takes the original `.ase` path unchanged |
| Import format probe order | `TG_TypeMultiShape::LoadFromFile`, `mclib/msl.cpp:438-479` | Trace: `MC2_ASSIMP_TRACE` | probes `<base>.glb` then `<base>.fbx`, falls back to `<base>.ase` on any failure | Shipped |
| glTF import tuning | `mclib/assimp_importer.cpp:56-61` (axis/Y-offset), `:543-545` (grounding) | `MC2_GLTF_AXIS` (0-3), `MC2_GLTF_YOFF`, `MC2_GLTF_GROUND` (0/1/2, default 2) | — | Shipped, runtime-tunable without rebuild |
| Mech animations | Gestures via legacy `Gestures0..N` `.ini` blocks | — | `data/tgl/<mech>.ini` | **Known gap**: GLB animation clips are not wired to the gesture system yet (future stage-2 work) |
| Dead/doc-drift env var | — | `MC2_ASSIMP_MECH_IMPORT` | — | **DEAD** — never read by any code in this checkout; only appears in stale docs/tooling metadata. Use `ENABLE_ASSIMP_IMPORTER`/`MC2_ASSIMP_TRACE` above instead |

## Buildings, props, trees

| Mechanism | Entry point | Gate / env | Drop path | Status |
|---|---|---|---|---|
| Render-only model override registry | `ModelOverrideRegistry::instance()`/`loadFromFile`, `mclib/model_override_registry.cpp:37-146` (singleton load at `:158-166`) | — | `data/model_overrides/models.json` | Shipped. Entry: `{"type":"model","renderOnly":true,"fallback":"stock","scale":1.0,"replaces":"<class>:<name>","source":"<file>.glb"}` — `type` must be exactly `"model"`, `renderOnly` must be `true`, `fallback` must be exactly `"stock"`, and `scale` must be exactly `1.0` (any other value drops the entry, MVP limitation, `:63-68`) |
| Class restriction | `mclib/model_override_registry.cpp:77` | — | `replaces` must be `"staticProp:<name>"` or `"tree:<name>"` | Shipped — no other override class is accepted |
| Static-prop resolve seam | `mclib/bdactor.cpp:357` (`ModelOverrideRegistry::instance().resolve("staticProp", ...)`) | — | — | Shipped. `autoGround=false` for static props (`:373-374`) — the glTF must carry its own stock-matching pivot |
| Tree resolve seam | `mclib/bdactor.cpp:4106` (`ModelOverrideRegistry::instance().resolve("tree", ...)`) | — | — | Shipped. Trees keep `autoGround=true` (base-snapped via `MC2_GLTF_GROUND`) |
| Override trace | `mclib/bdactor.cpp:198,208,354,1444,3094,3139,4103,5395,5419` | `MC2_MODOVERRIDE_TRACE` | — | Shipped, logs live appearance-name matches to stderr |
| Asset cook pipeline (Track G) | `tools/asset_cook/trackg_cook.py` | — | produces manifest entries + `manifest.json`/`materials.json` per cooked asset | Shipped tooling. Validate output with `py -3 tools/asset_cook/validate_asset_manifest.py` |

## In-game debug UI (not a data mod, but relevant while iterating)

| Mechanism | Entry point | Gate / env | Status |
|---|---|---|---|
| ImGui runtime enable | `isEnabled()`, `GuiRuntime/GuiRuntime.cpp:24-31` | `MC2_IMGUI` (default ON in dev builds; any value starting with something other than `0` counts as on; `"0"` disables) | Shipped |
| Graphics Options window | `GuiRuntime/GraphicsOptionsWindow.cpp:1738-1744` | Hotkey `Ctrl+Shift+G` (in-game, no env var) | Shipped |
| Object Inspector | `GuiRuntime/EditorInspector.cpp:103` | `MC2_IMGUI_INSPECTOR` | Shipped |
| Panel dispatch | `GuiRuntime::Render()`, `GuiRuntime/GuiRuntime.cpp:321-338` | — | Hardcoded C++ call list (`EditorInspector::drawImGui()`, `GraphicsOptionsWindow::draw()`) — **no data-driven panel registry exists** |

## Branch-only (not present on this checkout)

| Mechanism | Where it actually lives | Gate / env | Status |
|---|---|---|---|
| Equipment ADD (new equipment beyond the fixed 255-row table via a facade layer) | Local unmerged branch `claude/unit-profile-seam` (tip `59f16541`) | `MC2_EQUIPMENT_ADD` | **Does not exist on this checkout.** `grep`ing `MC2_EQUIPMENT_ADD` here returns nothing. Do not tell modders to set this env var unless they've specifically checked out that branch |
| Unit-profile data-source flip | Same branch | `MC2_UNIT_PROFILE_DATA` | **Does not exist on this checkout.** Same caveat as above |
