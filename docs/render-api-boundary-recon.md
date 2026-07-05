# Render API / RHI Boundary Recon — 2026-06-11

**Status:** recon only, no code changes. Branch `claude/nifty-mendeleev` worktree.
**Question answered:** what is the actual renderer API boundary today, and what should
it become — where the goal is NOT a Vulkan/DX12 port, but: no random GL outside owned
render modules, explicit pass state, explicit resources, explicit submit points.

Companion docs (read first, all still accurate where cited):
`docs/render-contract.md` (submission-space contract),
`docs/render-binding-registry.md` (UBO/SSBO/unit map),
`docs/renderpass-contract-spec.md` + `RenderCore/RenderPassContract.h` (5-lane descriptive registry),
`docs/engine-standalone-seams.md` (B1–B7 blockers, host-services proposal),
`docs/engine-closure-audit.md` (validator inventory),
`.claude/engine-lane-separation-strategy.md` (ELS A0–A4 ownership lanes),
`docs/known_issues.md:33` (GlStateGuard meta-fix, deferred),
`docs/modernization-roadmap-2026-06-09.md` (GlStateGuard = roadmap item G/#8).

---

## (a) GL-call-site census by module

Method: `grep -rEo "\bgl[A-Z][A-Za-z0-9]+\(" --include=*.cpp --include=*.h <dir>`
(function-call anchor; same pattern family as `scripts/check-no-raw-gl-from-game.sh`).
Counts are raw textual hits; comments/false positives noted per row.

| Directory | Raw gl* hits | Verdict |
|---|---:|---|
| `GameOS/gameos/` | **3990** | The GL backend. This is where GL is SUPPOSED to live. |
| `tools/` | 495 | Standalone hosts (asset_viewer 354, rendercore spike 25, mc2fx_preview 4) — own GL contexts, legitimately raw. |
| `editor/` | 68 | MFC editor host glue (EditorGameOS.cpp 30, AssetThumbnailCache 9, EditorGosRender 9, MapGeneratorDialog 9, EditorGpuTimer 5, EditorInterface 5). Outside every lint scope today. |
| `mclib/` | 16 | All diagnostic or false-positive: `txmmgr.cpp:2248,2680-2692` (env-gated probes, allowlisted), `render_contract.cpp:437,493,502` (assert-mode state queries, allowlisted), `camera.cpp` hits are `glToMC2()` name collisions + comments. |
| `GuiRuntime/` | 7 | ImGui runtime: `GuiRuntime.cpp:349-350` `glGetIntegerv` diagnostics + viewport-texture plumbing. Mostly benign reads. |
| `RenderWorld/` | **6** | `RenderWorld.cpp:829-836` — object-ID pick readback (`glBindFramebuffer`/`glReadPixels`). The ONE production GL leak in the "GL-free" module; sibling of blocker B1 (`RenderWorld.cpp:28` includes `gos_postprocess.h`). |
| `RenderCore/` | 1 | Comment only (`RendererFeatureRegistry.h:555`). Genuinely GL-free. |
| `code/` (game) | **0** | Clean. Enforced by `scripts/check-no-raw-gl-from-game.sh` (M6 firewall). |
| `gui/`, `EditorBridge/`, `GameAdapters/`, `Viewer/`, `netlib/`, `mc2res/` | 0 | Clean. |

GameOS/gameos per-file top offenders (these ARE the owned render modules):

| File | gl* | Role |
|---|---:|---|
| `gameos_graphics.cpp` | 1229 | renderer core: legacy draw lanes, terrain bridges, masks, uniform cache |
| `gos_static_prop_batcher.cpp` | 499 | static-prop GPU lane (incl. welded-in material table, B6) |
| `gos_postprocess.cpp` | 461 | FBOs, bloom, shadows, post |
| `gos_terrain_lod_chunk.cpp` | 197 | chunk terrain (default-on since `a7b090be`) |
| `gpu_cull_compute.cpp` | 177 | GPU cull |
| `gos_mech_batcher.cpp` | 172 | mech GPU lane |
| `gos_terrain_indirect.cpp` | 164 | indirect terrain / colormap atlas |
| `gos_terrain_water_stream.cpp` | 134 | water fast path |
| `gos_terrain_lighting.cpp` | 114 | terrain lighting compute |
| `gos_particle_bridge.cpp` | 110 | GPU particles |
| `gos_terrain_patch_stream.cpp` | 102 | patch stream |
| `debug_renderer.cpp` | 62 | debug draw |
| (remaining ~15 files) | <40 each | readback, substrate, height tex, hdri, imgui backend, validate, screenshot |

**Bottom line:** the boundary already holds at the *directory* level — game (`code/`) is
zero, `mclib/` is diagnostics-only-allowlisted, RenderCore is pure. The two real holes are
(1) `RenderWorld.cpp:829-836` pick readback + the B1 downward include, and (2) `editor/` +
`GuiRuntime/`, which are in no lint scope at all. The *intra-GameOS* problem is different:
~25 TUs each own raw GL with **implicit cross-pass state inheritance** (the 10.3
transparency saga; explosion-card cull bug; water vanish — see `docs/known_issues.md:33`).

---

## (b) Current API layers

```
                          game code (code/, mclib game-side)
                                      |
        +------------------+---------+-----------------+------------------+
        |                  |                            |                  |
   gos_* immediate    mcTextureManager            RenderWorld          Camera/eye
   (gameos.hpp)       enqueue->renderLists        (handles, POD)       (matrix producer,
   gos_DrawQuads/     dual master-node queues     upsertStaticProp     gamecam.cpp:192-204
   Tris/Lines/Text    (txmmgr.cpp:1331 flush)     registerMech         packs ViewUniforms)
   gos_RenderIndexed  legacy + hardware arrays    markVisible/pick           |
   gos_PushTerrain-        |                           |                     v
   Overlay/Decal           |                           |              RenderCore (GL-FREE)
   gos_SetRenderState      |                           |              ViewUniforms/EngineView
   (cached; single         |                           |              PipelineDesc/Registry
   stateCacheValid_ bit)   |                      [LEAK: own glReadPixels    MaterialGpu, KtxLoader
        |                  |                       RenderWorld.cpp:829]      RenderPassContract
        +---------+--------+--------------+------------+--------------------+
                  |                        |
                  v                        v
        GameOS/gameos GL backend  <--- the ONLY legitimate raw-GL zone (3990 calls)
        gameos_graphics.cpp (legacy lanes + gos-state cache applyRenderStates)
        gos_*_batcher / gos_terrain_* / gpu_cull_* / gos_postprocess (GPU-direct lanes,
            each sets raw GL state, must manually gos_InvalidateRenderStateCache —
            13 call sites today, comment-and-discipline enforced)
        pipeline_binder.cpp (PipelineDesc -> GL apply)
        view_uniforms_gl.cpp (UBO binding 3)
        utils/shader_builder.cpp (load_shader + include splicer, version-prefix at compile)
```

Key structural facts:
- **Render functions are enqueuers, not submitters** (audit doc §MISSING): `XXX::render()`
  enqueues into `masterVertexNodes`/`masterHardwareVertexNodes`; actual `glDraw*` happens
  in `MC_TextureManager::renderLists()` (`mclib/txmmgr.cpp:1331`) at `gos_RendererEndFrame`.
  MLR immediate-draw is the exception. GPU-direct lanes hook AFTER `renderLists()`.
- **Two state systems coexist:** the gos render-state cache
  (`gos_SetRenderState`/`applyRenderStates`, invalidated via
  `gos_InvalidateRenderStateCache` — 13 sites) and raw GL set by GPU-direct passes.
  The seam between them is the active bug factory.
- **Pass identity is descriptive only:** `RenderCore/RenderPassContract.h` (5 lanes) and
  `mclib/render_contract.cpp` (GBuffer1/shadow contract) describe but do not schedule.
- **GlStateGuard slice 1 = NOT STARTED.** Zero code symbols anywhere
  (grep `GlStateGuard` hits only docs). Design exists: `docs/known_issues.md:33`
  (RAII snapshot/restore + cache-invalidate on dtor, ~14 wrap sites),
  roadmap `docs/modernization-roadmap-2026-06-09.md:277` (slice 1 = RAII struct +
  `MC2_GLSTATEGUARD` env gate + state-delta counter), sequenced AFTER Baseline A.

---

## (c) Proposed allowed-GL zones vs forbidden zones

**Allowed (raw GL legitimate, by ownership):**
1. `GameOS/gameos/**` — the backend. Within it, each pass-owner TU owns its GL but must
   (target state) acquire pass state through GlStateGuard, not inheritance.
2. `tools/**` standalone hosts (asset_viewer, spikes) — own contexts, own rules; keep the
   `PreviewSurface` seam so they never reach into game GL state.
3. Diagnostic-only allowlisted TUs (`scripts/check-no-raw-gl-from-game.allowlist`):
   `mclib/render_contract.cpp`, `mclib/txmmgr.cpp` env-gated probes. Read-only queries only.

**Forbidden (already clean — keep enforced):**
4. `code/`, `mclib/` (non-allowlisted), `gui/`, `GameAdapters/`, `EditorBridge/` — M6
   firewall, active.
5. `RenderCore/` — GL-free by include-firewall (`scripts/check-include-firewall.sh`).

**Forbidden (currently leaking — work items):**
6. `RenderWorld/` — must become GL-free in *both* directions: re-home the pick readback
   (`RenderWorld.cpp:829-836`) behind a backend call, and break the B1 include
   (`RenderWorld.cpp:28` → `gos_postprocess.h`) with a backend interface.
7. `editor/` + `GuiRuntime/` — 75 raw calls in no lint scope. Editor host glue
   (context creation, GPU timer, thumbnail FBO) should route through a small
   `EditorRenderBridge`/gos surface; add both dirs to a relaxed allowlist-style lint
   (state-query reads OK, draws/binds forbidden) rather than the strict zero rule.

---

## (d) Bridge APIs needed

1. **`GlStateGuard` RAII** (designed, unbuilt): snapshot depth/blend/cull/mask/program/
   VAO on ctor, restore + `gos_InvalidateRenderStateCache()` on dtor; inner guards for
   sub-passes; `MC2_GLSTATEGUARD` gate + state-delta counter. Collapses the 13-site
   manual invalidate list. This IS the "explicit pass state" mechanism.
2. **`gos_ReadObjectIdPixel(x, y, &id, &depth)`** (or a RenderCore-declared,
   GameOS-implemented `IPickReadback`) — removes RenderWorld's only raw GL.
3. **Backend interface for B1**: the handful of `gos_postprocess.h` symbols RenderWorld
   uses become a narrow `RenderBackend` header declared in RenderCore/RenderWorld and
   implemented in GameOS (same direction as `pipeline_binder.cpp` and
   `view_uniforms_gl.cpp`, which are already the model: RenderCore declares POD/desc,
   GameOS owns the GL).
4. **Material-table service extraction (B6)**: `gos_static_prop_batcher.cpp:3109-3260`
   build/upload/bind moves to a RenderCore-described, GameOS-implemented service so the
   asset viewer (Slice 4) and mech lane can share it.
5. **Host services (Slice 1, DONE)**: `HostServices/` already exists — `IConfig` is the
   landing zone for B3's per-draw env gates becoming explicit render-config fields.

## (e) Immediate APIs that survive (HUD/tools only)

Keep, screen-space-authoritative per render-contract Bucket C1:
- `gos_TextDraw*` family (`gameos.hpp:1026-1030`) and font APIs.
- `gos_DrawPoints/Lines/Triangles/Strips/Fans/Quads` (`gameos.hpp:2228-2254`) and
  CPU-array `gos_RenderIndexedArray` (`gameos.hpp:2265-2267`) — HUD, menus, selection
  brackets, editor gizmos, debug overlays. Per the editor-discipline rule
  (`docs/critical_inline_rules.md`), 2D immediate UI is explicitly NOT in the
  GPU-only-path ban.
- `debug_renderer.cpp` debug draw.

Survive as *world bridges with exit plans* (render-contract D2/A3): `gos_PushTerrainOverlay`
/ `gos_PushDecal` / `gos_DrawTerrainOverlays` (`gameos.hpp:2371-2380`) until the typed
world-space overlay batch ships. New world-geometry features must NOT route through any
immediate API — that is the containment rule, not deletion.

## (f) What RenderWorld should own

Today (verified `RenderWorld/RenderWorld.h`): object handles + lifecycle
(`upsertStaticProp`/`destroy`/`registerMech`/`destroyMech`), `markVisible`, record/explain
views (`getObjectRecordView`, `fillStaticPropSlots`), pick state, frame banner. It is a
scene/handle registry, not a frame owner.

Target ownership (aligned with ELS lanes + roadmap item 9 "RenderWorld facade"):
- **Scene records**: static prop + mech (+ eventually terrain-object) render records, POD
  in, no game types (firewall already enforces).
- **Visibility results**: consume one cull truth (post-A3/F1), publish `visibleIds`;
  draw lanes consume, never invent visibility (A4/M2b rule).
- **Submit points**: named, ordered pass invocations (`RenderWorld::renderOpaque()` etc.)
  that *call into* GameOS pass owners — explicit submit points without becoming a render
  graph (RenderPassContract stays descriptive; this is sequencing, not scheduling).
- **Light records + dirty generations** (ELS A2 LightBridge).
- It should NOT own: GL calls, FBOs, shader compilation, GL state — those stay GameOS,
  reached through the backend interface from (d).

## (g) Enforcement mechanism options

1. **Extend the existing lint (cheapest, do first).** `check-no-raw-gl-from-game.sh` is
   proven (pattern, comment-strip, allowlist, negative-test). Add scopes: `RenderWorld`
   (strict zero after the readback re-home), `GuiRuntime` + `editor/` (allowlist per-TU
   with justification), keep `RenderCore` under the include firewall. Wire into pre-commit
   like the other `scripts/check-*.sh` validators (closure-audit §2 table).
2. **GlStateGuard expansion (runtime).** Slice 1 = RAII + counter; slice 2 = wrap the ~14
   GPU-direct passes; slice 3 = assert mode (`MC2_GLSTATEGUARD=2`: glGet snapshot diff at
   pass exit, red-log any dirty slot) — converts the implicit-state bug class into a loud
   smoke failure. Sequenced after Baseline A per roadmap.
3. **Link-time seams (heaviest, defer).** The rendercore_standalone_spike already proves
   the link-level seam empirically (links only `view_uniforms_gl`, `pipeline_binder`,
   `PipelineRegistry` + SDL/GLEW). A per-dir static-lib split (`RenderCore` lib with no
   `opengl32`/GLEW in its link interface) would make violations a link error — but the
   ELS doc explicitly warns against the giant-modularization branch; only do this if the
   lint keeps regressing.
4. **Registry drift guards (already exist, keep).** binding-registry doc-update rule,
   `shader_reflect` goldens, `shader_schema`, `RenderPassContract` static_assert.

## (h) Migration order

1. **Lint widening** — add `RenderWorld` to the raw-GL check scope with a temporary
   allowlist entry for the pick readback; add `GuiRuntime`/`editor` in report-only mode.
   Zero behavior change. (Now; independent of Baseline A.)
2. **Pick-readback re-home** — `gos_ReadObjectIdPixel` in GameOS; RenderWorld calls it;
   delete the allowlist entry. Small, isolated.
3. **GlStateGuard slice 1** (RAII + env gate + delta counter) — after Baseline A freeze
   per roadmap; then slice 2 wrap chunk-terrain + static-prop passes, counter == 0 gate;
   then remaining GPU-direct passes; then assert mode in tier1.
4. **B1 backend interface** — break `RenderWorld.cpp:28` → `gos_postprocess.h` with the
   narrow backend header. Gates Ring-2 standalone work; do when a tool needs RenderWorld.
5. **B6 material-table extraction** — unblocks asset-viewer Slice 4 and mech material lane.
6. **B3 env-gates → render-config fields** via HostServices `IConfig` (mechanical).
7. **Explicit submit points on RenderWorld** — name and order the pass invocations
   (depends on nothing above, but most useful after GlStateGuard so each named pass is
   also state-clean).
8. **F1/A3 one-projection unification** — separate campaign, after GlStateGuard +
   Baseline A (roadmap dependency table `docs/modernization-roadmap-2026-06-09.md:474-478`).

Non-goals (restated): no Vulkan/DX12 port, no render graph/scheduler, no file
restructure, no removal of immediate HUD APIs, no forcing water/UI/picking out of their
intentional projected/screen spaces.
