# Tool Preview Rendering Architecture v2 — one preview story across all MC2 tooling

- Date: 2026-06-11
- Status: STRATEGY v2 (supersedes `tool-preview-rendering-architecture.md` v1, 2026-06-10)
- Scope: Asset Viewer (`tools/asset_viewer/`), in-engine UI previews (mechlab/logistics via `SimpleCamera`), editor preview panes, **and (new in v2) offline thumbnail generation for the editor Asset Placement Browser** (`docs/superpowers/strategy/editor-superpowers-roadmap.md` §3.6)
- Siblings: `asset-cook-pipeline-architecture.md` (cook/decode layer), `props-modernization-architecture.md` (static prop lanes), `vfx-modernization-roadmap.md` (gosFX — engine-only, no standalone preview)

## What changed vs v1 (delta summary)

v1 established the backend taxonomy (B/A/C), the asset-type matrix, the share-vs-never-share seams, the batcher preview-steal rule, and fidelity tiers F0–F3. **All of that survives unchanged and is normative here.** v2 adds/changes:

1. **North star stated explicitly** (v1 had only the "no universal renderer" principle).
2. **Thumbnail pipeline is now a first-class section** — v1 parked it as a v4 footnote ("headless render service"). The editor roadmap's Asset Placement Browser (legacy `tgaName` thumbnails loaded-but-unshown at `code/Editor/EditorObjectMgr.cpp:516`) makes it near-term: v2 defines a two-tier thumbnail plan (T1 legacy TGA reuse → T2 Backend-A batch render).
3. **Camera + lighting model unified** — v1 scattered camera facts across §3; v2 defines per-backend camera/light ownership in one table, including the IBL question (Backend-A already binds `u_iblSh[]`/`u_iblShStrength` per the shader contract — v1 never said what to feed them).
4. **Batcher-outside-game-loop contract hardened** — v1's §5 rule covered preview-steal at submit time; v2 adds the full per-frame lifecycle contract (what a batcher may assume about `renderLists()` cadence, MVP snapshot freshness, texture-manager state) learned from the `GPU-CULL-SIMPLECAM-1` stale-MVP fix in `code/simplecamera.cpp` (~line 216) and the `MechPreviewRenderScope` fix (~line 121).
5. **Picking in previews promoted** from a mech-inspector detail (`docs/asset-viewer-mech-assembly-inspector.md` Stage 2) to a cross-backend design rule.
6. **Parity contract** rewritten as an explicit promised/not-promised table per consumer (modder vs editor vs thumbnail).
7. **UV-V check status absorbed**: the inconclusive luminance smoke (`docs/asset-viewer-backend-a-uvv-check.md`) is folded into the parity contract as a known-open item; the fix (high-contrast fixture) stays slice S4.

v1's §1–§8 remain the reference for backend internals; this doc is the decision layer on top.

---

## 1. North star

> **Every MC2 asset has exactly one designated preview backend per consumption context, each backend owns 100% of its GL state and camera, and the fidelity tier of what you're looking at is always labeled.** Previews answer "is my asset right?" at the cheapest tier that can answer it — they never silently impersonate the engine.

Corollary: the preview system is a *family of small renderers with shared decode and shared contracts*, not a render abstraction. The engine is the only F2+ renderer; everything standalone is an honest approximation with a drift tripwire (compile smokes, contract doc, golden frames).

## 2. Backend recap (normative, from v1)

| Backend | Anchor | Fidelity tier | Notes |
|---|---|---|---|
| **B** — standalone Lambert | `tools/asset_viewer/MeshPreview3D.{h,cpp}` | F0 "Geometry" | GL 3.3, inline shaders, single FBO |
| **A** — shader-faithful static | `tools/asset_viewer/ModelPreviewEngineShader.{h,cpp}` + `docs/asset-viewer-backend-a-shader-contract.md` | F1 "Engine shading" | Compiles live `shaders/static_prop.{vert,frag}` (legacy lane: no `MC2_USE_VIEW_UNIFORMS`/`MC2_COALESCE`/`MC2_STATICPROP_PBR_SLOTS`), stub SSBOs (bindings 0/1/2/3/20), reverse-Z MRT FBO; fail-open to B |
| **C** — in-engine context swap | `code/simplecamera.cpp:105-230`, `Mech3DAppearance::render` | F2 "Engine context" | Engine `eye` swap + `MechPreviewRenderScope`; CPU `TG_Shape::Render` draw path |
| **T** (new) — thumbnail batch | this doc §6 | F1 (T2) / legacy (T1) | Backend-A driven headlessly via `renderToPixels()`; not a new renderer |

Backend-T is deliberately **not** a fourth renderer: it is Backend-A invoked in batch mode. The headless `renderToPixels(int w, int h, std::vector<uint8_t>&)` path already exists on both `MeshPreview3D.h:30` and `MaterialPreviewPBR.h:32` and is proven by the UV-V smoke (256×256 PPM dumps).

## 3. Preview backend decision — share game lanes vs dedicated, per asset class

The recurring question: should previews ride the game's modern render lanes (mech material-GPU path, static prop batcher, terrain chunk) or a dedicated lightweight backend?

**Decision rule (unchanged from v1, restated with the lanes named):**
- Appearance is a pure function of (mesh, materials, shader files) → **dedicated Backend-A** referencing the live shader files. Do NOT link the game lane.
- Appearance depends on engine-resident mutable state (animation pose, paint cache, damage detach, FX sim) → **Backend-C inside the engine process**, riding the **CPU draw path**, never the batched lane.
- Only geometry/UV sanity needed → Backend-B.

Per asset class:

| Asset class | Game's modern lane | Preview decision | Why |
|---|---|---|---|
| Static props (.tgl) | GPU static-prop batcher + coalesce + PBR slots (`props-modernization-architecture.md`) | **Dedicated (Backend-A)**, shares only shader *files* + binding contract | Lane is batched/deferred — exactly the lifecycle that breaks outside the frame loop (§5). Stub SSBOs proven sufficient (contract doc, compile PASS 2026-06-10) |
| Mechs/vehicles/turrets | `GpuMechBatcher::submitActor` → flush in `MC_TextureManager::renderLists()` (`mclib/txmmgr.cpp:2721`) | **Backend-C with the lane explicitly bypassed** (`MechPreviewRenderScope`, `gos_mech_killswitch.h:44-48`; check at `mclib/mech3d.cpp:2683`) | Paint cache, rigid-part hierarchy, gestures live in engine pools (`asset-viewer-mech-assembly-inspector.md`); standalone reimplementation = guaranteed drift. The lane itself is the proven hazard |
| Terrain | chunk/GPU-only post-8z (`mc2TerrainLodChunkEnabled()`) | **No per-asset preview.** Editor world view IS the preview | Chunk renderer needs the full dispatch/cull substrate; meaningless per-asset |
| VFX (gosFX) | engine FX pipeline (`vfx-modernization-roadmap.md`) | Backend-C only; thumbnail = captured frame, never simulated standalone | No standalone contract exists or should |
| Textures/materials | n/a | ImGui image / `MaterialPreviewPBR` (local PBR sphere, `LocalPbrMaterialBackend`) | Already shipped; not a scene renderer |

**Explicit anti-decision:** we will NOT build a "preview mode" into the static-prop batcher or mech batcher (per-submission MVP capture, preview buckets). That is v1's v4 territory and stays gated on the same triggers (CPU mech path retirement, or measured multi-actor preview perf need).

## 4. Camera + lighting model

### Camera ownership (one owner per backend, never routed across)

| Backend | Camera | Projection | Owner |
|---|---|---|---|
| B | Tool orbit camera (PreviewSurface params) | standard-Z perspective | `MeshPreview3D` |
| A | Tool orbit camera, same params | **reverse-Z infinite-far** to match engine `u_worldToClipGL` semantics | `ModelPreviewEngineShader` |
| C | `SimpleCamera` swaps global `eye`, recalculates projection constants + `TG_Shape::SetViewport` for its viewport (`code/simplecamera.cpp:135-150`) | engine projection | `SimpleCamera::render()` |
| T | Fixed framing rule (below) | same as A | thumbnail driver |

`SimpleCamera` ownership rule (v2 formalization): SimpleCamera is the **only** legitimate way to render an engine object under a non-world camera. Anything it depends on at render time it must set itself — it already restores `eye`, pushes/pops render states, and (post `GPU-CULL-SIMPLECAM-1`) refreshes the terrain/cull MVP before `renderLists()` so GPU cull doesn't run with the stale GameCamera matrix. Any new global the render spine snapshots per-frame (view UBOs, cull matrices) gets a line item in SimpleCamera's render — this is the in-engine mirror of the "set ALL state you depend on" rule.

Thumbnail framing rule: camera distance = `k × boundingRadius` along a fixed 3/4 orbit (yaw 45°, pitch −20°), look-at = AABB center. Deterministic per asset → thumbnails are cache-stable and diffable.

### Lighting

| Backend | Light model | IBL |
|---|---|---|
| B | Single hard-coded Lambert key | none |
| A | Engine shader lighting with **neutral preview scene**: stub `LightsData` (binding 20) = one white directional key + filler; `g_scene` fog disabled; `u_ambientV1Strength` small constant | `u_iblSh[]` (active uniform per contract) fed a **constant neutral-gray SH** (L0-only, ~0.18 irradiance), `u_iblShStrength` matching engine default. Never zero — zeroing it is what collapsed the UV-V luminance contrast (delta 1.2, `asset-viewer-backend-a-uvv-check.md`) |
| C | `SimpleCamera` white key + 196/196/196 ambient for components (`simplecamera.cpp:124-132`), fog off | whatever the engine lane does — not configurable, that's the point |
| T | Same as A; the neutral scene IS the thumbnail look | same as A |

v2 rule: **the Backend-A neutral scene values are part of the shader contract doc** — one canonical block, used by interactive A and thumbnails identically, so a thumbnail and the A pane never disagree.

Mission-profile presets (load real mission ambient/fog into the stub UBO) remain v4 — nice-to-have, not parity-bearing.

## 5. Batcher-outside-game-loop contract

GPU batchers (`GpuMechBatcher`, static-prop batcher, any future one) assume a **game-frame lifecycle**: submit during scene walk → flush once inside `MC_TextureManager::renderLists()` under the world MVP snapshot → per-frame buffer reset. Every preview bug in this family came from violating one of those assumptions. The binding contract:

1. **Submit-time diversion (v1 §5 rule, unchanged):** any batched path checks preview depth at *submit* time (`g_mechPreviewRenderDepth`) and diverts to an immediate path honoring the active camera. Flush-time fixes forbidden — camera context is gone by flush.
2. **MVP snapshot freshness:** any out-of-band `renderLists()` call (SimpleCamera does one per render) must first refresh every matrix the flush/cull consumes — the stale-GameCamera mis-cull in the intro pan is the precedent (`simplecamera.cpp` GPU-CULL-SIMPLECAM-1 comment).
3. **No mid-frame second flush assumption:** batchers may not assume exactly one flush per frame. SimpleCamera renders mean N+1 flushes; per-frame buffers must tolerate it (or the preview path must not feed them — preferred, per rule 1).
4. **State block ownership:** the batcher's flush sets ALL GL state it needs (depth func/mask, blend, cull) — terrain-chunk transparency saga (`f375e0ba`) is the canonical lesson; previews leave arbitrary state behind.
5. **Ship-enabled checklist item:** "preview-scope guard present + trace event (`MC2_MECH_PREVIEW_TRACE`-style) + SimpleCamera smoke" before any new batcher ships enabled.
6. **Standalone tools never link a batcher.** Asset Viewer links no `eye`, no `mcTextureManager`, no TGL pools, no batcher (v1 §3 anti-coupling, re-affirmed).

Generalize `MechPreviewRenderScope` → `PreviewRenderScope` (one counter, all batchers) when the second batcher needs it — slice S2.

## 6. Thumbnail pipeline (offline batch for the asset browser)

Consumer: editor Asset Placement Browser (`editor-superpowers-roadmap.md` §3.6) — filter box + category tree + **thumbnail grid**.

**T1 — ship now (editor roadmap Phase A, zero new rendering):** reuse the legacy per-asset TGA thumbnails already loaded at `EditorObjectMgr.cpp:516` (getter `EditorObjectMgr.h:372`, currently unshown). Lazy TGA→GL upload cache keyed by name, main thread only, `ImGui::Image` grid. Covers base assets; mod-added assets without TGAs get a placeholder.

**T2 — Backend-A batch render (fills the gaps + retires hand-authored TGAs):**
- New `mc2_asset_viewer.exe --thumbnails <manifest> <outDir>` mode: enumerate `.tgl` props (later bind-pose mechs once Backend-A-mech lands), render each via `ModelPreviewEngineShader::renderToPixels()` at 256×256 with the §4 framing + neutral scene, encode PNG.
- **Cache key = hash(.tgl bytes, albedo .ktx2 bytes, shader-contract version, framing constants).** Re-cook only on change → incremental, CI-able.
- Output lands beside cooked assets (`data/thumbs/<name>.png`); editor browser prefers T2 thumb, falls back T1 TGA, then placeholder. Mod assets get thumbnails for free at mod-cook time (ties into `asset-cook-pipeline-architecture.md` and `mod-packaging-deploy-architecture.md`).
- Failure policy mirrors interactive fail-open: A-compile failure → render with B (and watermark the thumb corner so an F0 thumb is distinguishable) → smoke red.
- **Not** a game-exe `--render-asset` mode (v1's v4 idea). The asset viewer already has the headless context, the loader, and Backend-A; the game exe adds Mission bootstrap weight and the batcher hazards of §5 for zero fidelity gain at F1.

F2 thumbnails (true engine context) are explicitly NOT a goal — see parity contract.

## 7. Picking in previews

One rule across backends: **picking is ID-buffer-based and on-demand, never per-frame.**

- **Standalone (A/B, mech inspector Stage 2):** R32UI attachment writing `shapeIndex+1` (or submesh id), rendered only on click, single-pixel readback, map id→tree node (`asset-viewer-mech-assembly-inspector.md` §Selection — engine precedent `MC2_OBJECT_ID_BUFFER`, `gos_mech_batcher.h:42-46`). Fallback: CPU ray vs per-part AABB.
- **Backend-C panes:** reuse the same pattern if/when needed; do NOT reuse the world picker (`findTerrainObjectByMouse`-class scans) — wrong camera, wrong object set, and it's already a known 3.6ms hotspot in-game.
- **Anti-rule:** no `inverseProject`-style analytic unproject pickers in tools — the editor's 13–34s freeze lineage (Camera::inverseProject on hot paths) is the cautionary tale. Preview picking is click-driven readback, full stop.
- Highlight direction (tree→preview) stays `SetARGBHighLight` in-engine / tint pass standalone.

## 8. Parity contract — promised vs explicitly not promised

| Property | Promised at | NOT promised |
|---|---|---|
| Geometry, winding, scale, part hierarchy | F0+ (all backends; shared `TglMeshLoader` + Stuff→GL transform) | — |
| UV orientation matches in-game | F0+ **intended**; currently OPEN — UV-V smoke INCONCLUSIVE (delta 1.2 < 3.0), closure = S4 high-contrast fixture + one user visual check | — |
| Albedo texture content/tier | F0+ | texture *filtering/aniso* parity |
| Material/shading behavior (the real frag math) | F1 (Backend-A, legacy lane) | pixel-exactness; mission light/fog/shadow; modern-lane defines until S5 |
| Paint scheme, pose, damage states | F2 only (Backend-C) | any standalone backend; thumbnails |
| Shadows, fog, IBL-as-in-mission, post-process, bloom | F3 only (place in world) | F0–F2 previews and ALL thumbnails |
| Thumbnail ≡ Backend-A pane | promised (same code, same neutral scene block) | thumbnail ≡ in-game |
| Animation/FX playback | nothing below F2; gosFX F2-only | standalone forever |

UI mechanics (v1 §6): tier label always visible, fail-open transitions bannered, lower tier never impersonates higher. Thumbnails carry the F0-watermark rule (§6) as their version of the banner.

## 9. Anti-goals

All v1 §7 anti-goals stand: no universal renderer; no shader forks/copies; no engine globals in standalone tools; no pixel-exactness below F2; no preview-aware batcher complexity until measured need; no standalone effects/terrain preview; no inherited GL state. v2 adds:

- **No game-exe headless thumbnail mode** (asset viewer owns Backend-T).
- **No per-frame picking** or analytic unproject pickers in any preview.
- **No F2 thumbnails** — thumbnail fidelity ceiling is F1 by design.
- **No second neutral-scene definition** — one canonical block in the contract doc.

## 10. Risks

| Risk | L | Mitigation |
|---|---|---|
| Engine lane migration (view-uniforms/coalesce/PBR-slots default-on) orphans Backend-A legacy lane | High over time | S5 modern-lane compile smoke; contract doc tracks omitted defines; smoke reds when legacy lane deleted |
| New batchers reintroduce preview-steal | Med | §5 contract + `PreviewRenderScope` + default-on checklist + SimpleCamera smoke |
| Thumbnail cache staleness (asset changed, thumb didn't) | Med | content-hash cache key (§6); cook-time regeneration hook |
| Thumbnail batch nondeterminism across drivers (AA, mip selection) → noisy diffs | Med | fixed framing, no MSAA, explicit mip bias; compare luminance-band not pixel in any thumb-parity smoke |
| Neutral-scene values drift between interactive A and Backend-T | Low | single constant block in contract doc, both consume it |
| UV-V flip ships into thumbnails before S4 closes | Med | S4 ordered before T2 default-on; UV-V is a thumbnail gate |
| Backend-A-mech (bind pose) underestimates engine state | High | scope to bind-pose + paint only; animation F2 forever (v1 §8) |
| SimpleCamera's CPU draw dependency retired by a future 8z-style cleanup | Low now | F2 preview smoke pins it; retirement = v4 trigger |

## 11. Phased roadmap

- **P1 — Honesty + guard rails (now):** tier labels + fail-open banner; `PreviewRenderScope` generalization + batcher checklist; F2 SimpleCamera pixel/trace smoke; UV-V high-contrast fixture.
- **P2 — Thumbnails T1 (editor Phase A):** legacy TGA grid in AssetBrowser (no new rendering; lands with the editor roadmap's quick win #3).
- **P3 — Thumbnails T2:** `--thumbnails` batch mode on Backend-A, hash cache, editor pickup with T1 fallback, cook-pipeline hook for mods.
- **P4 — Lane keep-alive:** Backend-A modern-lane compile smoke (S5); golden-frame A-vs-engine luminance-band parity smoke.
- **P5 — Mech F1:** Backend-A-mech bind-pose spike (identity part transforms + paint colors); if it survives, mech thumbnails join T2; editor embeds an F1 prop pane.
- **v4 triggers unchanged:** preview-aware batching only on CPU-path retirement or measured multi-actor preview need; mission-profile lighting presets opportunistic.

## 12. First 5 implementation slices

1. **S1 — Fidelity labels + fail-open banner** (`AssetViewerApp`/sidebar UI only; persist backend choice; banner with log path on A→B fallback).
2. **S2 — `PreviewRenderScope`** (hoist from `gos_mech_killswitch.h:44-48`; mech batcher adopts; checklist comment in `gos_mech_batcher.h`; SimpleCamera uses the generic scope).
3. **S3 — F2 preview smoke** (SimpleCamera mech render → non-empty viewport pixels + `gpu_submit=0 cpu_draw=1` trace assert; wire into tier gates).
4. **S4 — UV-V high-contrast fixture** (synthetic checker-with-arrow albedo on a quad prop; closes `asset-viewer-backend-a-uvv-check.md`; gates T2).
5. **S5 — Thumbnail T1 + T2 skeleton** (editor TGA grid; `--thumbnails` mode rendering 5 fixture props to PNG with hash cache; defer editor T2 pickup until S4 green).

(S6 Backend-A modern-lane smoke and S7 Backend-A-mech spike follow, per v1 ordering.)

## 13. Follow-up prompts (for Opus/Codex)

1. *"Implement S5-T2: add a `--thumbnails <propListFile> <outDir>` batch mode to `tools/asset_viewer/main.cpp` driving `ModelPreviewEngineShader::renderToPixels()` at 256×256 with the canonical framing (yaw 45°, pitch −20°, dist = k·boundingRadius) and the neutral-scene block from `docs/asset-viewer-backend-a-shader-contract.md`; PNG encode (stb_image_write), content-hash cache key over (.tgl bytes, albedo .ktx2 bytes, contract version, framing constants); fail-open to `MeshPreview3D` with a corner watermark; exit nonzero if any asset fell open. Cite §6 of `docs/superpowers/strategy/tool-preview-rendering-architecture-v2.md`."*
2. *"Implement S2: generalize `MechPreviewRenderScope` (`GameOS/gameos/gos_mech_killswitch.h:44-48`) into `PreviewRenderScope` with one depth counter; update the check at `mclib/mech3d.cpp:2683` and `code/simplecamera.cpp` usage; add the §5 batcher-default-on checklist as a header comment in `GameOS/gameos/gos_mech_batcher.h`; grep for any other `g_mechPreviewRenderDepth` consumers. Behavior-preserving rename + doc; tier1 inner-loop smoke."*
3. *"Design-review then implement S4: create a high-contrast UV-V fixture (procedural 256×256 albedo: top half white with a down-pointing black arrow, bottom half mid-gray) cooked to .ktx2, applied to a simple quad/box .tgl test prop; rewire `--smoke-backend-a-uvv` to use it and tighten the reliability threshold so INCONCLUSIVE becomes impossible on the fixture; document the verdict in `docs/asset-viewer-backend-a-uvv-check.md` and flip the parity-contract OPEN item in tool-preview-rendering-architecture-v2.md §8."*
