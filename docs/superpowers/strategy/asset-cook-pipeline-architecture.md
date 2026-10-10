<!-- doc-status: current | reviewed:2026-10-11 -->
# MC2 Asset Cook Pipeline Architecture

**Status:** Strategy / design doc (long-term), 2026-06-10.
**Scope:** What stays source data vs becomes cooked runtime data; who owns cooking, validation, and consumption; manifest formats; cache layout; fallback policy; rollout order.
**Siblings:** `mod-packaging-deploy-architecture.md` (packaging/deploy lane), `telemetry-oracle-cockpit-architecture.md` (observability), `mc2-modding-toolchain-architecture.md` (tool ownership). This doc owns the **cook lane**: source → validated → cooked → loaded.
**Grounding:** `docs/asset-pipeline.md` (canonical inventory), `docs/asset-manifest-schema.md` (shape gate), `docs/asset-modernization-recon.md` (compression landscape), `docs/ibl-plan.md` (V-IBL-STATIC-0), `tools/asset_cook/` (Track G cook driver), `tools/mc2texcook/` (texture cooks incl. `burnin_jpeg.py`).

---

## 1. North star

> **Legacy files stay the source of truth and always load. A cook is a reversible, regeneratable acceleration sidecar — discovered by stem-probe, validated before export, described by a manifest that lives next to the asset. mc2.exe prefers the cooked record when present and valid, and silently falls back to the legacy path otherwise. There is exactly one asset database: the filesystem plus the manifests already shipped.**

Load-bearing consequences:

1. **No flag-day conversion.** Every cooked format ships as a sidecar probe next to a working legacy path. This pattern is already proven three times: `.ktx2` same-stem sidecar in `mclib/txmmgr.cpp:3426-3521` (BC7 → RGBA8 fallback), terrain colormap probe chain in `mclib/terrtxm2.cpp` (`<name>.burnin.jpg` → `.burnin.ktx2` BC7 via `gos_terrain_indirect.cpp` `BuildColormapAtlas` (~:873, vkFormat 145/146, gate `MC2_COLORMAP_KTX2`) → `.tga`), and `.tgl` compiled from `.ase` (`mclib/msl_load.cpp:365 LoadTGMultiShapeFromASE`). New cooks copy this shape; they never replace a loader.
2. **Cook ≠ authoring.** Cooked artifacts are derived, deterministic, and deletable — deleting `out/` or a `.ktx2` tier must never lose information. Hand-authored data (`.ase`, `.fit`, `.pak`, source GLB, PSD) is never overwritten by a cook.
3. **The Asset Viewer is the cook front-end, not the cook.** Cooking logic lives in headless Python (`tools/asset_cook/trackg_cook.py`, `tools/mc2texcook/*`); the Viewer/Workbench drives it, previews it, and gates export on validation. The editor and mc2.exe only *consume*.

---

## 2. Source vs cooked taxonomy

Three tiers, per asset. "Source" = hand-authored truth, versioned/archived. "Cooked" = derived runtime-optimal record. "Generated cache" = machine-local, never shipped.

| Asset class | Source of truth | Cooked runtime record (sidecar) | Generated cache (never shipped) |
|---|---|---|---|
| Terrain colormap (burnin) | `<map>.burnin.tga` (or 4× upscale archives, `release_assets/0.3/mc2-burnins-4x-*`) | `<map>.burnin.jpg` (shipped; `tools/mc2texcook/burnin_jpeg.py`, decoded `mclib/burnin_jpeg_decode.cpp`) → target `<map>.burnin.ktx2` stored-BC7 (loader exists: `BuildColormapAtlas`; **cook unfinished**, §8.1) | merged colormap atlas GL texture (built per mission load) |
| Terrain detail/water/overlay | `data/textures/{tier}/*.tga` | per-tier BC7 `.ktx2` sidecars (same `txmmgr` probe) | runtime cement/overlay atlas (`GameOS/gameos/gos_terrain_indirect_dispatch.cpp:302`) |
| Heightmap | mission `.pak` quad verts | none — procedural (`gos_terrain_height_tex.cpp:38`) | R32F GL texture |
| Static prop / tree geometry | `.ase` (2,947 in `mc2srcdata/tgl/`) + compiled `.tgl`/`tgl.fst`; override source = authored `.glb` | cooked override GLB + per-asset `manifest.json` + `models.generated.json` (`tools/asset_cook/trackg_cook.py stage/assemble`; 2,015 props proven via `cook_all_stock.py`) | meshdump JSONs (workbench `--export-tgl-meshdump-all`) |
| Static prop textures | 128² originals + upscale archives (`mc2-tgl.zip`) | `data/tgl/{128,256,512,1024}/*.ktx2` BC7 tiers (`cook_tgl_tiers.py`; shipped 2026-06-02) | — |
| Mech/vehicle geometry+tex | `.ini`+`.ase` + `.txm` paint-hash sources | same manifest+GLB shape as props (via `[Import] Source=` opt-in, `docs/asset-modernization-recon.md` §1); textures: bucketed `.ktx2` | `.tgl`/`.agl` compile cache |
| IBL | `data/hdr/DaySkyHDRI063B_4K.exr` (+license sidecar) | **target:** prefiltered specular cubemap mips + irradiance SH/cube + BRDF LUT as `.ktx2` sidecars next to the `.exr` (`<name>.specular.ktx2`, `<name>.irradiance.ktx2`, shared `brdf_lut.ktx2`) | none (cook offline; current runtime loads raw EXR each boot, `gos_hdri.cpp:12`) |
| VFX meshes/cards | `data/effects/mc2.fx` (#XFG, gosFX) + procedural particle SSBOs | none near-term; long-term: billboard atlas `.ktx2` for MLR tex pool | GPU particle buffers |
| UI/HUD art | `data/art/` TGA/PNG + upscale archives | BC7 `.ktx2` tiers (blocked on GameOS compressed-upload path, recon §3) | font atlases (`gos_font.cpp` rasterized) |
| Missions/FIT/PacketFile | `.fit`/`.pak` — **format frozen, never cooked** | none | `.modindex-cache` (mod overlay index, `mclib/file.cpp:161`) |

**Rule of thumb:** anything a human edits is source; anything `py -3 tools/...` can reproduce byte-for-byte from source + tool version is cooked; anything the engine rebuilds at load is cache. A file may move tiers only downward (e.g. a one-off upscale that gets a reproducible recipe becomes cooked).

---

## 3. Ownership map

| Stage | Owner | Must NOT own |
|---|---|---|
| Source authoring (GLB, PSD, EXR, `.ase` edits) | Human / DCC tools; archived in `release_assets/` or `src/` of a mod project | — |
| Import + preview + **validation** + export gating | **Asset Viewer / Workbench** (`tools/asset_viewer/`, `ModWorkbench.*`, `BundleExport.*`) | the cook implementation itself; mission data; deploy |
| Cook execution (deterministic, headless, CI-runnable) | Python cook tools: `tools/asset_cook/trackg_cook.py`, `tools/mc2texcook/{mc2texcook,batch_cook,cook_tgl_tiers,burnin_jpeg}.py`, future `cook_ibl.py` | UI; validation policy (it *enforces* the validator, doesn't define it) |
| Manifest shape + coherence gate | `tools/asset_cook/validate_asset_manifest.py` + `asset_manifest.schema.json` (and the scaffold `tools/validate_asset_manifest.py` per `docs/asset-manifest-schema.md`) | file-existence/bake checks beyond `--check-files` |
| Mission authoring; consuming validated assets | **Editor** — places props, saves `.pak`/`.fit`; reads through the same loaders as the game | cooking; manifest writes; texture compression |
| Runtime resolution + fallback | **mc2.exe** — sidecar probes in `txmmgr.cpp`/`terrtxm2.cpp`/`KtxLoader`/`model_override_registry.cpp` | reading authoring manifests (`src/` manifests are tool-only, per mod-packaging §2) |
| Packaging/deploy of cooked output | `mc2mod` lane (sibling doc) | cook logic — always shells to the cook tools |

The Viewer drives, Python cooks, the validator gates, the engine decides. Any predict/actual divergence between validator and engine loader is a tool bug (same authority rule as the packaging doc §9).

---

## 4. Manifest formats

**Per-asset manifests, not a central database.** Extend the two formats that already exist; invent nothing parallel (explicit instruction in `docs/asset-manifest-schema.md` §"For the future asset-pipeline lane").

1. **`mc2-asset-manifest-v1`** (`tools/asset_cook/asset_manifest.schema.json`) — one per cooked model asset, sits next to the cooked GLB. Identity (`assetId`, `replaces = '<class>:<appearanceName>'`, lowercase class), geometry stats, `materials[]` (name/shader/alphaMode/alphaTestThreshold/doubleSided/pbr factors), `textureRefs[]` (slot ∈ {albedo, normal, orm, emissive, mask} + colorSpace convention: albedo/emissive=srgb vk43, normal/orm/mask=linear vk37; BC7 cooked = 145/146), `capabilities{}` (mapped to `RenderCore/RenderObjectDesc.h ArchetypeFlags`), `lods[]`, `provenance{tool, toolVersion, generatedAt, sourceHash}`, `generatedOutputs[]`.
2. **`models.generated.json` → central `models.json`** — the *runtime-facing projection* (registry subset only), consumed by `mclib/model_override_registry.cpp:162`. Promotion into central `models.json` is a separate reviewed merge through `CentralManifestMerge.cpp` (`.bak` + atomic rename + round-trip verify). The engine never reads the full authoring manifest.

**Minimum material schema (the floor every cooked model must satisfy):**

```
material: { name, shader, alphaMode: opaque|alphaTest|blend,
            alphaTestThreshold?[0,1], doubleSided: bool,
            pbr: { baseColorFactor, metallicFactor=0.0, roughnessFactor=1.0 } }
textureRef: { slot: albedo (REQUIRED) | normal|orm|emissive|mask (optional),
              path, colorSpace per slot table, cooked?: {format, vkFormat, mips, dims} }
cross-rules: normal slot ⇒ capabilities.hasTangents; 'a_' name prefix ⇔ alphaTest
             (resolver convention, asset_cook README); dims %4==0 for BC7.
```

Albedo-only is a *valid* cooked material — normal/ORM for static props is explicitly shelved in-game (`docs/asset-pipeline.md` §6 decision, 2026-06-02); the slots exist so Viewer authoring/preview and any future lighting-model change need no schema rev.

**New manifest kinds (additive, same philosophy):**

- **Texture-set sidecar** `cook.json` per cooked texture directory (e.g. `data/tgl/512/cook.json`): tool+version, preset, source archive hash, tier policy ("never upscale"). Lets `mc2mod verify` and CI prove a tier is reproducible without re-cooking. Engine never reads it.
- **IBL manifest** `<hdri>.ibl.json` next to the `.exr`: source hash, sample counts, mip count, output files. Same provenance block.

---

## 5. Cook cache layout

```
<repo>/release_assets/...            # archived hand-authored + upscale sources (immutable)
<mod or workdir>/src/                # uncooked sources + authoring manifests (never indexed)
<mod or workdir>/out/                # cook scratch: staged.json, materials.json, meshdumps
                                     #   — regeneratable, gitignored, stripped from packages
<deploy>/data/tgl/{128,256,512,1024}/*.ktx2 + cook.json     # shipped texture tiers
<deploy>/data/missions/<map>.burnin.{jpg|ktx2}              # shipped colormap cooks (stem-probe)
<deploy>/data/hdr/<name>.{specular,irradiance}.ktx2 + brdf_lut.ktx2 + <name>.ibl.json
<deploy>/.../model_overrides/<id>/{model.glb, manifest.json}; central models.json
```

Conventions (all already in force, now binding):
- **Sidecar stem-probe is the discovery mechanism** — no lookup tables. Cooked file = `<source-stem>.<cooked-ext>` in the same directory (or numeric tier sibling dirs `{64..1024}/`, the tier-ladder convention from `docs/asset-pipeline.md` §5).
- **`out/` is the only cook scratch dir**; anything there can be deleted at any time (`generatedOutputs[]` points into it).
- **Cooked artifacts in deploy carry provenance** via the directory `cook.json` / per-asset manifest, never via filename mangling.
- Mod projects follow the identical layout under `mods/<id>/data/` (packaging doc §2) — the cook lane is mod-agnostic.

---

## 6. Validation stages

Four gates, increasingly expensive, each with an existing precedent:

1. **Shape gate (offline, CI, no binaries):** `validate_asset_manifest.py` schema + cross-field coherence (slot==index, class lowercase, alphaClass⇔`a_` prefix⇔flags, LOD ascension, bounds ordering). Runner: `tools/asset_cook/tests/run_tests.py` (golden passes, 5 broken fixtures fail *for cause*) + `scripts/check-asset-manifests.py`.
2. **Cook-output gate (post-cook, automatic):** vkFormat ∈ {145,146} verified after `ktx.exe` (already done in `trackg_cook.py textures`); BC7 dims %4; mip-chain completeness; for IBL, energy-conservation sanity (max mip ≈ irradiance average). `--check-files` existence pass.
3. **Texture-resolution gate (Viewer, pre-export — NEW):** every `textureRefs[].path` must resolve through the same name-sanitization the runtime resolver uses; **missing texture = export blocked with a named warning** (`[asset-validate] MISSING_TEX slot=albedo material=<m> wanted=<path>`), not a silent untextured manifest. Today `cook_all_stock.py` quietly produces "386 untextured geometry-only" manifests — fine for stock bulk-dump, wrong for authored exports. The Viewer's export button greys out on errors; warnings (e.g. no normal map) export with a recorded waiver in the manifest.
4. **Engine oracle (smoke):** `MC2_RENDER_PATH=1` descriptive log (`isOverride`, `path=override_multidraw|legacy_static`) + gl-clean `--validate` run, the proven G-E2E bigbox pattern. The screenshot remains the real gate for geometry conventions — **the extents oracle is mirror/rotation-blind** (asset_cook README calibration story; 4 wrong axis maps passed offline, all caught visually).

Stage 3 findings feed back as warnings the Viewer surfaces *live* during authoring, so the export gate is never the first time a modder hears about a missing texture.

---

## 7. Runtime fallback policy

Binding rules for every cooked format (the contract that keeps legacy content loading forever):

1. **Probe order = cheapest-acceptable first, legacy last, never fail on absence.** Colormap: `.burnin.jpg` → `.burnin.ktx2` (BC7) → `.tga` (terrtxm2.cpp probe chain). Textures: `.ktx2` sidecar → RGBA8 → legacy TGA (`mclib/txmmgr_render_phases.cpp:480`). Models: override registry hit → `LoadTGMultiShapeFromASE`.
2. **Invalid cooked record = log + fall back, never abort.** A corrupt `.ktx2` or schema-stale manifest behaves like an absent one, with one `SPEW`-class line naming the file and reason (feeds the telemetry-oracle cockpit lane).
3. **Env-gated kill switches per format** during rollout (`MC2_COLORMAP_KTX2`, `MC2_TEXMGR_COMPRESSED_UPLOAD` pattern); default flips ON only after a tier1 smoke + visual confirm, and the opt-out stays for one release after.
4. **No cooked format may change game semantics** — cooks affect memory/bandwidth/load-time only. Anything that changes gameplay-visible data (mission `.pak`, FIT) is out of the cook lane by definition.
5. **Mods override cooks the same way they override sources** — the overlay (`mclib/file.cpp` first-wins index) resolves the cooked sidecar path like any other; a mod can ship either a cooked or a legacy file and both work.

---

## 8. First asset classes to cook (priority order)

1. **Terrain colormaps → stored-BC7 KTX2 (finish the started cook).** Loader is *already shipped and default-on* (`BuildColormapAtlas`, `MC2_COLORMAP_KTX2`); only the cook is missing. Blocker is tooling: `ktx.exe create` rejects raw `BC7_UNORM_BLOCK` (only basis-lz/uastc) — route = **texconv/nvtt → raw BC7 blocks → `ktx create --raw`** (per the 2026-06-08 handoff), packaged as `tools/mc2texcook/cook_burnin_bc7.py`. Win: stays GPU-compressed in VRAM (the q95 JPEG decodes to full RGBA8 RAM — 67 MB for the 4096² 1K-map case); per-map disk ~16 MB vs 75 MB TGA. Highest leverage per line of new code in the project.
2. **IBL prefilter cook** (`V-IBL-STATIC-0`, `docs/ibl-plan.md`). Offline `cook_ibl.py`: EXR → equirect-to-cube → GGX prefiltered specular mips + irradiance + BRDF LUT, all RGBA16F (or BC6H later) `.ktx2`. Runtime gains a sidecar probe in the `gos_hdri.cpp`/`GameOS/gameos/gos_postprocess_gates.cpp:108-135` init (gate shape = `MC2_STATIC_PROP_AMBIENT_V1` precedent); absent sidecars → current raw-EXR sky path unchanged. This is the *only* item where a cook unlocks a feature (real IBL ambient) rather than just shrinking one.
3. **Static prop/model manifests at authoring quality.** The bulk machinery exists (2,015 stock props cooked, 0 errors); the work is the §6 stage-3 texture-resolution gate + Viewer export wiring, converting "386 untextured" from silent to waived-or-blocked. Mech/vehicle classes follow via the `[Import] Source=` recipe (recon §1) once `claude/assimp-mech-import-1` merges.
4. **UI/HUD + mech texture BC7** — blocked on the GameOS/`MC_TextureManager` compressed-upload path (recon §3/§4 TEXMGR-COMPRESSED-UPLOAD); sequenced after 1–3 because it needs engine work, not cook work.
5. **VFX meshes/cards — last.** gosFX `mc2.fx` is a monolith with CPU-legacy consumers; cooking it buys little until the GPU particle path fully owns rendering. Near-term: nothing. Long-term: atlas the MLR texture pool to BC7 once (4) lands.

**glTF's place:** glTF/GLB is the **interchange + override-source format**, full stop. It is what humans author in, what the Viewer imports/exports, and what cooked model overrides ship as (the importer already consumes it). It is **not** a runtime replacement for `.tgl` across the stock library and not a new archive format — stock `.ase`→`.tgl` keeps loading verbatim; GLB enters only through the opt-in override registry. A future "all props are GLB" flag-day is explicitly out of scope (anti-goal §9).

---

## 9. Anti-goals (binding)

- **No second asset database.** Filesystem layout + sidecar stem-probes + the two existing manifest families are the only registries. No SQLite, no global asset GUID table, no content-addressable store. (Mirrors packaging doc §9.)
- **No flag-day format conversion** — every cook is per-asset, sidecar, reversible; legacy loaders are never deleted as part of this lane.
- **No engine parsing of authoring manifests** — mc2.exe reads `models.json` projections and cooked binaries only.
- **No cook logic in C++ tools or the editor** — Viewer/editor shell out to the Python cooks; one implementation per cook.
- **No PacketFile/FIT/FST cooking** — frozen formats; mods ship loose files, the overlay outranks FastFiles already.
- **No upscaling inside the cook** — cooks downsample/transcode only; upscales are a separate, archived, human-curated source step (`release_assets/`).
- **No supercompression (Basis/ETC1S) until the GameOS compressed-upload path exists** and a transcoder is vendored; stored-BC7 (and BC6H for HDR) is the only shipped GPU format until then.

## 10. Risks

| Risk | Mitigation |
|---|---|
| BC7 raw-block toolchain (texconv/nvtt + `ktx create --raw`) produces KTX2 the `KtxLoader`/`BuildColormapAtlas` rejects (level byteLength, typeSize) | golden round-trip test: cook one burnin, load through the engine probe with `MC2_COLORMAP_KTX2=1`, assert atlas built + visual confirm; keep `.jpg` fallback in place |
| Cook determinism drift (tool upgrades change bytes → `verify` noise) | `provenance.toolVersion` + `cook.json` source hash; verify compares source-hash+recipe, not output bytes |
| Manifest schema bifurcation (scaffold `tools/validate_asset_manifest.py` vs `tools/asset_cook/validate_asset_manifest.py`) | declare `asset_cook`'s schema canonical; fold the scaffold's material-authoring rules in; one validator family in CI |
| Silent untextured exports normalized by the bulk stock cook | §6 stage-3 gate: authored exports block on missing albedo; bulk stock runs pass `--allow-untextured` explicitly |
| IBL cook correctness invisible offline (same trap as axis calibration) | screenshot gate + a grey-furnace test scene in the Viewer; offline energy checks are necessary-not-sufficient |
| VRAM/disk regression from over-eager tiers | tier cooks never upscale; smoke counters (telemetry lane) track texture-memory deltas per flip |
| Deploy split-brain (v0.4 game vs 0.4c editor — known trap) | cooks deploy via `mc2mod`/deploy scripts with explicit `--deploy`; never hand-copy into one target |

## 11. Phased roadmap

- **P0 — Codify + converge (no behavior change):** this doc; merge the two manifest validators; add `cook.json` provenance writer to `cook_tgl_tiers.py`/`batch_cook.py`.
- **P1 — Colormap BC7 cook:** `cook_burnin_bc7.py` (texconv→raw→`ktx create --raw`), golden engine round-trip, cook + deploy stock maps and `1kbasicmap`; `.jpg` stays as fallback.
- **P2 — IBL cook + runtime probe:** `cook_ibl.py`, sidecar probe in the HDRI init, env-gated, default-OFF until visual confirm; static-prop ambient consumes it (ibl-plan Batch 2 close-out).
- **P3 — Viewer validation gates:** stage-3 texture-resolution gate, missing-texture warnings, export blocking + waivers; wire validator family into the Viewer export path and CI.
- **P4 — Class breadth:** vehicle/prop/tree `[Import]` hooks (post assimp-merge); authored-override happy path Viewer→cook→mod project→playtest end-to-end.
- **P5 — Engine-gated compression:** GameOS compressed-upload path → UI/mech BC7 tiers; evaluate BC6H for IBL and ETC1S-transcode for burnins only after.

## 12. First 5 implementation slices

1. **Validator convergence:** make `tools/asset_cook/validate_asset_manifest.py` + schema the single canonical gate; port the scaffold's MATERIAL-AUTHORING rules (colorSpace/slot table, tangent cross-check) + missing `invalid/material_fail_*.json` fixtures; one CI runner. Gate: golden + all broken fixtures behave; both fixture corpora pass under one tool.
2. **`tools/mc2texcook/cook_burnin_bc7.py`:** `.burnin.tga|jpg` → BC7 blocks (texconv or nvtt, sRGB) → `ktx create --raw` → `<name>.burnin.ktx2`, verify vkFormat 145/146 + level sizes against `RenderCore/KtxLoader` constraints. Gate: engine loads it via the existing probe (`MC2_COLORMAP_KTX2=1`), tier1 single-mission smoke clean, screenshot parity vs `.jpg`.
3. **`cook.json` provenance sidecars:** emit from `cook_tgl_tiers.py`, `batch_cook.py`, `cook_burnin_bc7.py` (tool, version, preset, source hash, file list); `validate_cook_json.py` shape gate; teach `mc2mod verify` to read it.
4. **Viewer export gate (stage 3):** texture-resolution pass over `textureRefs[]` using the runtime resolver's sanitization rules; export blocked on missing albedo, waiver recorded for optional slots; `cook_all_stock.py` gains explicit `--allow-untextured`. Gate: a fixture with a missing albedo fails export with the named warning; bigbox golden still exports.
5. **`tools/mc2texcook/cook_ibl.py` (offline only):** EXR → prefiltered specular mips + irradiance + BRDF LUT as RGBA16F `.ktx2` + `<name>.ibl.json`; offline checks (mip count, energy sanity) + a Viewer preview mode. No engine change in this slice — runtime probe is the next slice, per ibl-plan gating.

## 13. Follow-up prompts (Opus/Codex)

1. *"Implement slice 2 of `docs/superpowers/strategy/asset-cook-pipeline-architecture.md` in worktree `.claude/worktrees/nifty-mendeleev`: write `tools/mc2texcook/cook_burnin_bc7.py` producing stored-BC7 `.burnin.ktx2` via texconv (or nvtt) raw blocks + `A:/Games/mc2-tools/ktx/ktx.exe create --raw`. It must satisfy `RenderCore/KtxLoader.cpp` constraints (vkFormat 145/146, typeSize=1, level byteLength = ceil(w/4)*ceil(h/4)*16) and the probe in `mclib/terrtxm2.cpp` / `gos_terrain_indirect.cpp BuildColormapAtlas`. Gate: cook one stock burnin, run a 30s single-mission smoke with `MC2_COLORMAP_KTX2=1 --keep-logs`, assert the atlas-built log line and no FATAL. Do not delete the `.jpg`/`.tga` fallbacks."*
2. *"Implement slices 1+4 (validator convergence + Viewer export gate) of asset-cook-pipeline-architecture.md: fold `docs/asset-manifest-schema.md` MATERIAL-AUTHORING-VALIDATION-1 rules and the missing `invalid/material_fail_*.json` fixtures into `tools/asset_cook/validate_asset_manifest.py`; add a texture-resolution pass (mirror the runtime resolver's name sanitization from `trackg_cook.py textures`) that fails on missing albedo and warns on missing optional slots; add `--allow-untextured` to `cook_all_stock.py`. Python 3 stdlib only, no engine changes. Gate: `tools/asset_cook/tests/run_tests.py` green, new broken fixtures fail for cause."*
3. *"Implement slice 5 of asset-cook-pipeline-architecture.md: `tools/mc2texcook/cook_ibl.py` cooking `data/hdr/DaySkyHDRI063B_4K.exr` into GGX-prefiltered specular cube mips, Lambert irradiance cube, and a BRDF LUT as RGBA16F KTX2 + `<name>.ibl.json` provenance, per `docs/ibl-plan.md`. Offline-only — no shader or engine edits this slice. Include mip-count/energy sanity checks and a small render of each output face to `out/` for visual review. Note `ktx.exe` limitations; emitting via `--raw` is acceptable."*
