# Active campaigns

Extracted from CLAUDE.md 2026-05-24. SHIPPED slices are kept here for
archaeology; DECISIONS + steady-state pointers stay current. Add new
campaigns at top (most recent first).

For the RenderWorld arc specifically, see also:
- [docs/renderworld_arc_status.md](renderworld_arc_status.md) — steady-state
  ledger reframing M3/M4/M5 as DECISIONS (not ongoing work)
- [docs/renderworld_migration_guide.md](renderworld_migration_guide.md) —
  contributor onboarding for the arc

---

## Track V — post + grounding MVP (SHIPPED + MERGED 2026-05-29)

Merged to nifty `d8ccd032` (lane `claude/trackv-post-grounding-mvp`, off
`e109a7fc`). Clean ort merge; nifty builds mc2 exit 0, `mc2_tests --ts=RenderCore`
45/45, `check-contracts` 8/8. Full state:
`memory/trackv_post_grounding_mvp_shipped.md`. Soak/tuning guide:
`docs/trackv-post-grounding-soak-1.md`. **All gates DEFAULT-OFF** → byte-identical
shipped default (env default-OFF tier1 5/5 PASS; gate-ON full-stack 2/2 PASS, no
GL errors).

Key discovery: HDR post infra ALREADY existed (scene FBO `RGBA16F` +
`postprocess.frag` ACESFilm + `runBloom`, gated by ImGui-only member bools) — so
the HDR/bloom/tonemap slices were just env-gate wiring + per-mission tunables, no
new pipeline. SSAO was the only genuinely-new pass.

- **TRACKV-GATE-DEFAULT-OFF-TEST-1** (`500ddc99`): 4 Feature gates registered in
  RendererFeatureRegistry (COUNT 30→34) + RenderCore unit test asserting each is
  Feature-kind + default-OFF (promotion to default-ON is now a deliberate 2-file edit).
- **HDR-POST-SCAFFOLD-1** (`6feb8882`): `MC2_HDR_POST` master gate (`hdrPostEnabled_`);
  composite force-disables bloom+tonemap + `runBloom` early-returns when OFF.
- **BLOOM-MVP-1** (`f3463808`): `MC2_BLOOM` + profile keys bloomThreshold/Intensity.
- **TONEMAP-ACES-MVP-1** (`bf09789d`): `MC2_TONEMAP_ACES`; exposure via profile 'exposure'.
- **SSAO-GTAO-LITE-MVP-1** (`891954d0`): `MC2_SSAO` (+`MC2_SSAO_DEBUG` trace). New
  `shaders/ssao.frag` (16-sample world-space hemisphere; occlusion via window-depth
  ordering sky=1.0 + world-distance range check → NO camera-pos uniform, dodges the
  Stuff→MC2 axis-swap hazard) + `ssao_apply.frag` (half-res R16F, multiplicative into
  scene; sky AO=1, UI composites after → untouched). Tunables aoRadius/Strength/Bias/Power.
- **ImGui tuners** (`3fd0d87e`): Graphics Options → Post-Process → **Track V** live
  controls (master, exposure, SSAO sliders + AO-buffer debug).
- **First-soak tune** (`7d2f4d2c`): bloom intensity 0.3→0.15, threshold 0.6→1.2, ACES
  input ×0.9 (tonemap branch only — default-OFF stays byte-identical).

Deps: BLOOM + TONEMAP inert without `MC2_HDR_POST=1`. SSAO independent.

OUTSTANDING: gate-ON VISUAL quality only spot-tuned on mc2_01 (run the soak across
mc2_03/17/24); SSAO radius/bias defaults (3.0 wu / 0.0025) are first-pass guesses;
HDR-master ImGui-toggle-off observed to darken (env default-OFF path is
smoke-verified non-black — caveat in soak doc). Next: full soak → dial SSAO →
consider promoting tonemap to default-ON after visual review.

---

## Shadow lane — dynamic prop shadows working (2026-05-29)

Branch `claude/shadow-lane` (9 commits ahead of nifty, ready to merge).
Full state: `memory/shadow_dynamic_projection_and_caster_feed_fixed.md`.

- **SHADOW-LANE PHANTOM** (SHIPPED `2764cb65`): terrain objects invisible w/
  MC2_SHADOW_ENABLE=1 was a flush-ORDER bug (registry flush after the SSBO slot
  lock). Registry flushed before flushShadow. See
  `memory/shadow_enable_terrain_object_invisibility_resolved.md`.
- **SHADOW-TERRAIN-COMBINE-MIN-1** (SHIPPED `7ea32b83`): terrain shadow combine
  is `min(static,dynamic)` not multiply → no double-darken on overlap.
- **SHADOW-BOUNDED-NEAR-FIT-1** (SHIPPED `d7e95a7f` + center `8428805e`): gate
  MC2_SHADOW_BOUNDED_NEAR_FIT (default OFF) caps the dynamic fit radius for crisp
  near shadows; box centered on screen-center ground-focus ray.
- **SHADOW-STATIC-BUILDINGS-2** (SHIPPED `657d671d`): gate
  MC2_STATIC_PROP_BUILDING_SHADOW (default OFF) replays ALL buildings (registry,
  visibility-independent) into the world-fixed static map; dynamic pass skips
  building types to avoid a fuzzy double-shadow (`8428805e`).
- **SHADOW-DYNAMIC-PROJECTION-FIX-1** (SHIPPED `a365e6ad`): dynamic frustum-fit
  unprojected GL-NDC corners through `inverse(getWorldToClip())` (D3D pixel-homog,
  Y-down) → box MIRRORED, shadows only at certain camera angles. Fixed: invert
  `worldToClipGL()` (GL-NDC + axis-swap baked); drop manual swizzle + w<0 negate.
- **SHADOW-DYNAMIC-PROP-CASTERS-1** (SHIPPED `054ca335`; **DEFAULT-ON flip `ef6192ad`**):
  MC2_SHADOW_DYNAMIC_PROP_CASTERS now **DEFAULT ON** (kill-switch `=0`) feeds the
  dynamic caster pass from the registry (visibility-independent) instead of the
  camera-visible s_typeRanges → every prop casts, not just near-camera ones. Takes
  effect only when MC2_SHADOW_ENABLE is set. Buildings excluded when the static
  building map is active, else included (no bare-SHADOW_ENABLE regression).
  Validated mc2_01: default-on inst=1010 incl buildings / 733 when building-static
  on; kill-switch reverts to flushShadow; 0 GL err.
- **DEBT:** no light-box cull yet (draws all map props/frame; **HZB planned**);
  camZ=0 focus shim; stale building shadow on destroy; foliage alpha-discard.

## MaterialGpu arc — static-prop complete, Mech-1 substrate done (2026-05-26)

- **MaterialGpu static-prop arc v4–v7** (SHIPPED 2026-05-26): Static props fully
  table-driven for albedo by default. Four commits: v5 (c38c8426) default-ON upload,
  v6 (06962919) docs/invariants locked, v7 (ae2152cd) default-ON shader sampling.
  MC2_MATERIAL_GPU=0 disables upload/bind/compare; MC2_MATERIAL_GPU_SAMPLE=0 disables
  shader sampling and falls back to texArrayLayer. texArrayLayer retained as compare
  authority. Log tag: `[MATERIAL_GPU v4]`. Tier1 5/5 PASS all gates.
  Handoffs: `memory/HANDOFF_2026_05_26_material_gpu_static_prop_complete.md` (v4-v7),
  `memory/HANDOFF_2026_05_26_material_gpu_arc_checkpoint.md` (full arc).

- **MaterialGpu Mech-1** (SHIPPED 2026-05-26): `GpuMechInstance.materialIdx` at byte 52
  (replaces `_pad1`), compare invariant proven (mismatches=0), `[MECH_MATERIAL_GPU v1]`
  log tag. HEAD c2dd0a33. Shader sampling NOT yet wired — pending Mech-2 decision.

- **MaterialGpu Mech-2** (BLOCKED — texture model decision required): texHandle is not
  shader-actionable in the current mech pipeline. Decision doc:
  `docs/superpowers/specs/2026-05-26-mech-material-gpu-mech2-decision.md`.

- **Static-prop pipeline binder** (STARTED 2026-05-26, commit 9f958536):
  `applyPipeline` wired in `GpuStaticPropBatcher::flush()`. PipelineDesc v1
  (depthFunc/reverse-Z encoding, DrawPacket pipelineId wiring) is the next slice.

---

## DrawPacket arc — v7+v7.1 SHIPPED, default-ON (2026-05-26)

- **DrawPacket v7** (SHIPPED 2026-05-26): Default-ON flip. `s_v6Enabled` true by
  default. Kill-switch: `MC2_STATIC_PROP_LEGACY_DISPATCH=1` reverts to legacy
  `glMultiDrawElementsIndirect`. `run_smoke.py` propagation tuple updated. Tier1
  5/5 PASS both gates. Log tag `[DRAW_PACKET_V6]` fires by default — NOT opt-in.

- **DrawPacket v7.1** (SHIPPED 2026-05-26, HEAD `f780949f`): Dead-code removal.
  Deleted: `StaticPropOpaquePacketView` struct, `batcher_setOpaqueDispatchCandidates()`,
  `s_opaqueDispatchCandidates`/`s_opaqueDispatchCandidateCount`/`s_v4TypeDrawCount` statics,
  v4A flush block (~90 lines), v4A legacy-path suppression guard + `[DRAW_PACKET_SUPPRESS v1]` log,
  v4B coverage-compare block (~80 lines), v4C slot-coverage block (~70 lines),
  deprecated `MC2_DRAW_PACKET_STATIC_PROP_V6` gate plumbing. Net -291 lines.
  `s_v6Enabled` lambda simplified to kill-switch only. Dispatch hierarchy table
  added to `docs/tier1_env_vars.md`. Tier1 5/5 PASS post-cleanup.
  Handoff: `memory/HANDOFF_2026_05_26_drawpacket_v7_shipped.md`.

- **DrawPacket v7.2** (PENDING — 1+ week soak): Remove `MC2_DRAW_PACKET_COALESCE_V5`
  gate plumbing after confirming no disarms. Not yet started.

- **DrawPacket v8** (DEFERRED): Shadow-pass packet dispatch, GPU-cull count
  integration, sortKey population, mech/terrain packets, log-tag normalization
  `[STATIC_PROP_PACKET_DISPATCH v1]`. No spec yet.

---

## Terrain colormap modernization arc (ALL STEPS SHIPPED 2026-05-31)

All 4 steps merged to nifty (deploy mc2-win64-v0.3/v0.4):
- **Step 1 (0b2b3a95)**: COLORMAP-TILES-RETIRE-1 — skip 400-tile GL upload
- **Step 2 (f9c12b53)**: COLORMAP-CPU-RETIRE-1 — free cpuColorMap after atlas upload
- **Steps 3+4 (3a21fba0)**: COLORMAP-BC7-KTX2-1 — BC7 KTX2 atlas sidecar
  (~81 MB VRAM savings/mission; Pillow handles BGRA→RGBA; cook: `bake_colormap_ktx2.py`)

Remaining open debt:
- Delete dead helpers in mapdata.cpp (cpu_sampleColormap + 4 others)
- Soak .burnin.ktx2 then delete .burnin.tga/.burnin.jpg from game data (3.6 GB)

Full design rationale: `docs/terrain-colormap-modernization-debt.md`.

---

## In flight / ready to execute

- **Unified-projection F1** (design + plan complete; ready for execution):
  Spec v2.8 greybeard-signed at
  `docs/superpowers/specs/2026-05-22-unified-projection-v2-f1-atomic-design.md`.
  Plan v1.1 codex-signed at
  `docs/superpowers/plans/2026-05-22-unified-projection-v2-f1-atomic-plan.md`.
  Handoff at
  `~/.claude/projects/A--Games-mc2-opengl-src/memory/HANDOFF_2026_05_22_unified_projection_F1_ready_for_execution.md`.
  Correctness-only (CPU budget already met per F3 ~55us total post-A2).
  Collapses inline `axisSwap*worldToClip` at `gamecam.cpp:165-187` into
  `Camera::worldToClipGL()`; renames `terrainMVP` → `u_worldToClipGL`
  across 14 vert + 3 compute/frag + 10 CPU bind sites; deletes SSAO
  runtime entirely; Stage A-pre parity probe + Stage A atomic
  single-commit flip. 21 tasks across 4 phases. Execute via
  `superpowers:subagent-driven-development` skill.

---

## Infrastructure / platform

- **C++17 Standard Flip** (SHIPPED 2026-05-24): root CMakeLists.txt now
  sets `CMAKE_CXX_STANDARD 17` (was implicit C++14 from MSVC default;
  three modern modules already C++17 via `target_compile_features`).
  12-line CMake edit, zero compile fixes, tier1 5/5 PASS, no runtime
  behavior change. Allowed-feature rules: `docs/cxx17-coding-rules.md`.
  Audit recon:
  `docs/superpowers/explorations/2026-05-24-cxx17-upgrade-recon.md`.
  Stabilization gauntlet:
  `docs/superpowers/explorations/2026-05-24-cxx17-stabilization-gauntlet.md`
  (GREEN). First useful cleanups: inline constexpr handle-base
  constants, std::optional in offline tools, structured bindings in
  tools/tests. NOT runtime renderer churn.

- **Render contract Phase 2** (SHIPPED 2026-05-24): `render_contract.h`
  gains `RequiredAttachments` (which `COLOR_ATTACHMENTx` a pass needs in
  the active draw-buffer list) + `ShaderOutputContract` (per-pass FS
  `layout(location=N)` uniqueness check) + `attachmentCount` in
  `PassStateContract` (expected `glDrawBuffers` arg). New runtime
  asserts gated by `MC2_RENDER_CONTRACT_ASSERT=1` — emit `[RENDER_CONTRACT
  v2] assert mode ACTIVE` on startup; compare live GL state to
  `render_contract::stateContractFor()` declarations and warn on
  mismatch. Tier1 5/5 PASS both modes; zero violations observed at HEAD.

---

## RenderWorld arc — STEADY STATE reached 2026-05-24

See [docs/renderworld_arc_status.md](renderworld_arc_status.md) for the
full ledger with decisions, handle-range partitioning, CI enforcement
layer, and what's NOT an upcoming slice. Quick summary:

- **M1, M1.5, M1.6, M2-pre, M2, M2.5, M2.6, M6** SHIPPED
- **M3** SHIPPED (DECISION: terrain deferred indefinitely; CPU
  `worldToTile` canonical)
- **M4** SHIPPED (DECISION: VFX click-through by design; CI-grep
  prohibits attachment-2 writes)
- **M5** DEFERRED INDEFINITELY (overlay had 7 in-tree meanings without
  identity-needing consumer)

Future arc work is opt-in only: M3.1 if editor consumer emerges, M2.7
mech-select-on-click if desired, or a new named slice consuming existing
substrate.

### Per-slice archaeology (verbose entries, kept for grep)

- **RenderWorld Slice M1** (SHIPPED 2026-05-23): static-prop adapter
  routes 5 audited call sites (`mclib/bdactor.cpp:1471,2802,4273,4859`,
  `code/warrior.cpp:7593`) through
  `GameAdapters::StaticPropRenderAdapter` ->
  `RenderWorld::upsertStaticProp` -> `GpuStaticPropRegistry::registerRecipe`.
  Three new modules at repo root: `RenderCore/`, `RenderWorld/`,
  `GameAdapters/`. New virtual `Appearance::getStaticRecipeIndex()`
  enables m5 late-spawn handle adoption. `[RENDER_WORLD v1]` banner
  emits per mission. Firewall: `scripts/check-include-firewall.sh`.
  Greybeard PATCH (justified) — adapter is TEMPORARY per spec section 10.
  Tier1 5/5 PASS; objects counts: mc2_01=997, mc2_03=2552, mc2_10=2611,
  mc2_17=1521, mc2_24=2641. Spec:
  `docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md`.

- **RenderWorld Slice M1.5** (SHIPPED 2026-05-23): substrate-only
  ObjectID buffer. `MC2_OBJECT_ID_BUFFER=1` env-gated `R32_UINT` MRT
  attachment at `GL_COLOR_ATTACHMENT2` on the main scene FBO; static-prop
  fragment writes `Handle.raw()` via `layout(location=2) out uint
  v_objectId`. RenderWorld API extension: `s_objectRecords`
  always-populated table indexed by `handle.index()` +
  `lookupAtPixel(x,y) -> LookupResult` synchronous readback (generation
  + alive check). C1 META-FIX: `setSceneDrawBuffers(SceneDrawBufferMode,
  bool objectIdAttachmentReady)` helper in `gos_postprocess.cpp`
  centralizes scene-FBO draw-buffer policy across 5 sites. Greybeard:
  META-FIX. Tier1 5/5 PASS env-OFF AND env-ON. Spec:
  `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md`.

- **RenderWorld Slice M1.6** (SHIPPED 2026-05-23): static-prop pick
  wiring. `MC2_STATIC_PROP_PICK=1` + `MC2_OBJECT_ID_BUFFER=1` enables
  Shift+left-click inspect-only static-prop selection. User-driven
  canary on mc2_03: 26 hits + 11 diagnostic misses. **Note:** Log schema
  `[STATIC_PROP_PICK v1]` and `StaticPropSelectionDebugState` were
  renamed to `[GAMEPLAY_PICK v1] kind=StaticProp` and
  `GameplaySelectionDebugState` in M2.6 META-FIX (commit `ca08d0c`).
  Archaeology: grep `[GAMEPLAY_PICK v1]`. Spec:
  `docs/superpowers/specs/2026-05-23-renderworld-slice-m1-6-staticprop-pick-spec.md`.

- **RenderWorld Slice M2-pre** (SHIPPED 2026-05-23): preemptive META-FIX
  refactor of M1.6 gameplay-pick machinery. New TU
  `code/gameplay_pick.{h,cpp}` hosts shared types + `tryGameplayPick(req)`
  dispatcher (env-substrate gate + 4 gesture gates + mover-first
  short-circuit + viewport query + bounds + coord scale +
  lookupAtPixel) + pure `screenToFboPixel(...)` coord transform. New
  automated validator `RunGameplayPickSelfTest()` gated by
  `MC2_GAMEPLAY_PICK_SELFTEST=1`. Greybeard: META-FIX. Spec:
  `docs/superpowers/specs/2026-05-23-renderworld-slice-m2-pre-gameplay-pick-extraction-spec.md`.

- **RenderWorld Slice M2** (SHIPPED 2026-05-23): route-only
  `MechRenderAdapter`. Every live `Mech3DAppearance` instance now has a
  `RenderObjectHandle` stored on it. `GameAdapters/MechRenderAdapter.{h,cpp}`
  bridges `BattleMech::init/destroy` to
  `RenderWorld::registerMech/destroyMech`. Mechs allocate from the
  unified `s_objectRecords` table at `kMechHandleBase=0x00010000`.
  Tier1 5/5 PASS.

- **RenderWorld Slice M2.5** (SHIPPED 2026-05-23): mech object-ID
  substrate. `GpuMechInstance` (std430 SSBO) grows 48B->64B with
  `objectIdRaw` field. `mech.vert` + `mech.frag` write per-instance
  `objectIdRaw` to attachment-2. Always-on per-mission counters split:
  `[MECHBATCHER v1] event=mech_id_summary gpu_mech_id_writes=N` +
  `[MECHBATCHER v1] event=mlr_mech_summary mlr_mech_draws=M`.
  Per-mission `gpu_mech_id_writes`: mc2_01=63836, mc2_03=19230,
  mc2_10=53872, mc2_17=407061, mc2_24=1232. MLR gap empirically rare:
  all 5 tier1 missions show `mlr_mech_draws=0`.

- **RenderWorld Slice M2.6** (SHIPPED 2026-05-23): mech pickup
  integration (inspect-only v1). Closes the RenderWorld arc's first
  user-visible loop. Shift+LMB on a hostile mech visible to sensors
  emits `[GAMEPLAY_PICK v1] hit kind=Mech ...`. Three new env vars
  (`MC2_MECH_PICK`, `MC2_MECH_PICK_DEBUG`, `MC2_MECH_PICK_PIERCE_FOG`).
  Handle→BattleMech reverse-lookup via linear scan over `ObjectManager`
  movers (Option B; partId-cookie rejected). META-FIX retired per-kind
  log+state pattern: `[STATIC_PROP_PICK v1]` → `[GAMEPLAY_PICK v1]
  kind=X`. Latent post-M2.5 mislabel bug fixed simultaneously. Tier1
  5/5 PASS env-OFF AND env-ON. Gate 6 substitutive-proof grep: zero
  hits for retired symbols.

- **RenderWorld Slice M6** (SHIPPED 2026-05-24): firewall audit script
  — no raw GL from game side. Codifies the empirical finding that
  `code/` has ZERO raw GL calls and `mclib/` has 3 diagnostic-only
  gated hits in `render_contract.cpp`. New
  `scripts/check-no-raw-gl-from-game.sh` (function-level grep, NOT
  include-level) with allowlist of exactly one TU. Turns the arc from
  "discipline by memory" to "discipline enforced by script."

- **RenderWorld Slice M3** (SHIPPED 2026-05-24; **DECISION: deferred
  indefinitely**): terrain reservation/deferral. `RenderObjectKind::Terrain
  = 2` + `kTerrainHandleBase = 0x40000` + defensive `lookupAtPixel`
  tripwire. No shaders edited, no adapter, no env var, no consumer.
  Forward-compat: if M3.1 ever ships per-quad terrain identity
  (editor-driven), use `subKind = Base/Water/Decal/Mine` payload (NOT
  separate enum values).

- **RenderWorld Slice M4** (SHIPPED 2026-05-24; **DECISION: never write
  attachment-2**): VFX prohibition + scaffold. `RenderObjectKind::Vfx
  = 3` + `kVfxHandleBase = 0x00080000u` + NEW
  `scripts/check-vfx-no-objectid.sh` firewall grep gate. Additive
  blending + R32_UINT last-write-wins would clobber M2.6 mech-pick
  under particles. Source-game-object lookup stays in game logic.

- **RenderWorld Slice M5** (DEFERRED INDEFINITELY 2026-05-24): "overlay"
  had 7 in-tree meanings without identity-needing consumers. Enum slot
  un-reserved (comment-only deferral note). If a future use case
  emerges, ship as a NEW NAMED slice (HoverKindIndicator /
  RenderWorldDebugOverlay / M5-perf overlay-decal GPU port — NOT as
  "M5 Overlay").

---

## Track S — Substance Painter Terrain Integration (ROADMAP — HIGH PRIORITY)

Shortest path from Substance Painter to visible MC2 terrain: export bitmaps → validate manifest →
wire one gated terrain slot. No `.sbsar` runtime, no Blender/plugin flow, no Substance graph
support in the terrain system.

**First slice: `TERRAIN-SUBSTANCE-BITMAP-PROBE-1`**

Scope:
- Export one known terrain material from Substance Painter as loose bitmaps
- Add a fixture asset manifest (`kind: terrainMaterial`, `shader: terrainPBR`)
- Validate manifest through existing asset-manifest validator
- Wire one terrain material slot under gate `MC2_TERRAIN_SUBSTANCE_PROBE=1`
- No runtime terrain architecture changes, no `.sbsar`, no cook pipeline dependency, no default behavior change

Export naming convention (loose bitmaps first, KTX2 after first visual proof):
```
terrain_substance_test_basecolor.png   (sRGB)
terrain_substance_test_normal.png      (linear)
terrain_substance_test_mra.png         (metallic/roughness/AO packed, linear)
```

Acceptance:
- Gate OFF: terrain byte-identical
- Gate ON: one terrain layer visibly uses the Substance material
- Normal map orientation verified (watch OpenGL vs DirectX Y-flip — "inside out" lighting = flip green)
- Roughness/metallic/AO not obviously inverted
- No GL errors, screenshot captured

**Sequence after bitmap probe works:**

| Slice | Description |
|---|---|
| `TERRAIN-SUBSTANCE-BITMAP-PROBE-1` | First visual proof (loose PNGs, one slot, gated) |
| `TERRAIN-SUBSTANCE-KTX2-BAKE-1` | Run KTX2 bake probe after bitmap path is proven |
| `TERRAIN-MATERIAL-MANIFEST-1` | Generalise manifest to cover multiple terrain material slots |
| `TERRAIN-MATERIAL-BROWSER-0` | Browse/validate terrain material manifests |
| `BLENDER-SUBSTANCE-MC2-EXPORT-PRESET-1` | Export preset / output template for MC2 terrain |
| `.sbsar` offline bake support | Optional, later |

**Key watch item:** Normal map convention. Substance exports are configurable; verify with a simple
known pattern before judging material quality.

---

## Track G/E — Authoring Loop (ROADMAP, not yet started)

Blender authoring plugin + standalone MC2 asset viewer.

**Architecture decision:** Blender = authoring/export/sync client. MC2 = rendering authority.
Do NOT embed MC2 renderer inside Blender viewport — it would fight Blender's draw manager
and drift from engine parity.

Agreed sequence:

| Slice | Name | Description |
|---|---|---|
| 1 | `MC2-ASSET-VIEWER-0` | Standalone exe: load asset manifest + mesh (glTF/GLB or native), cook texture refs, render with MC2 material/pipeline conventions; show Visual / ObjectIdDebug / Thermal / normal/albedo/material debug; validation summary + optional screenshot |
| 2 | `BLENDER-MC2-EXPORTER-0` | Blender add-on: select object → export .glb + MC2 manifest; map Blender material nodes to MC2 material fields; validate manifest; "Open in MC2 Viewer" button |
| 3 | `BLENDER-MC2-MATERIAL-MAPPER-1` | Deeper material node graph translation |
| 4 | `BLENDER-MC2-LIVE-SYNC-1` | Plugin watches changes, pushes deltas; viewer hot-reloads |
| 5 | `MC2-ASSET-VIEWER-VIEWMODES-1` | Full ViewMode preview (thermal/class/damage) |
| 6 | `BLENDER-MC2-LOD-AUTHORING-1` | LOD naming convention + assignment UI |
| 7 | `BLENDER-MC2-IMPOSTOR-PREVIEW-1` | Impostor billboard preview in viewer |

Interchange format: glTF/GLB (Blender's built-in glTF 2.0 add-on is enabled by default).

What viewer/exporter validates:
- **Mesh:** triangle count, bounds, normals, tangents if normal map present, UV0/UV1, LOD naming
- **Materials:** baseColor sRGB, normal linear, metallic/roughness/AO packing, alpha mode, KTX2/cook status, MC2 shader model compat
- **Capability flags:** castsShadow, supportsObjectId, supportsThermal, supportsDecalReceiver, hasLods, hasImpostor

Explicitly deferred (not MVP): bidirectional sync, editing missions inside Blender, MC2 renderer
inside Blender viewport, full material node graph translation, live hot-reload into full game runtime.

Longer-term vision: viewer answers "what material/pipeline/viewmode would this asset use?" —
RenderWorld as query engine for asset authoring, not just debugging.

---

## Track H — Mod Tools Suite (ROADMAP, not yet started)

UE-inspired modder/editor tooling, translated into MC2-shaped tools. NOT cloning Unreal Editor —
borrowing the *concepts* (content browser, material instances, packaging, tags, data tables) and
building BattleTech-RTS-semantics-aware versions.

**Priority order:**

| # | Slice | Description |
|---|---|---|
| 1 | `MC2-ASSET-VIEWER-0` | (shared with Track G/E) Standalone truth preview |
| 2 | `MC2-MOD-PACKAGER-0` | CLI: `mc2mod validate/cook/pack/smoke MyMod` → `.mc2mod` + manifest + cooked/ + reports/ + screenshots/ |
| 3 | `MC2-ASSET-BROWSER-0` | Browse manifests, dependencies, validation state, cook status, "what uses this?", open in viewer |
| 4 | `MC2-MATERIAL-INSTANCE-EDITOR-0` | Constrained PBR editor: texture slots, baseColor/normal/metallic-roughness-AO/emissive/alpha/shadow/objectId/thermal flags + preview + validator |
| 5 | `BLENDER-MC2-EXPORTER-0` | (shared with Track G/E) |
| 6 | `MC2-TAGS-AND-DATA-EDITOR-0` | Hierarchical tag editor (`unit.mech.assault`, `weapon.lrm`, `sensor.ecm`, `terrain.forest`, etc.) + CSV/JSON-backed UnitDef/WeaponDef/MaterialDef/SensorDef with schema validation |
| 7 | `MC2-VFX-PRESET-EDITOR-0` | Preset-based particle authoring: flipbook, color/alpha/size over lifetime, additive/alpha mode, soft/lit/bloom toggles, test-scene preview |
| 8 | `MC2-MISSION-TUNING-EDITOR-0` | Per-mission visual/tactical profile editor: lighting, post stack, VFX intensity, water/terrain tuning, tactical overlay defaults |

**Mod validation dashboard** (part of Packager or standalone): asset errors, texture color-space
errors, missing tangents, invalid tags, broken dependencies, unsupported shaders, stale generated
outputs, smoke/capture failures. HTML report or simple GUI.

**Tactical overlay authoring** (MC2-specific advantage over UE): weapon range rings, firing arcs,
sensor ranges, ECM fields, heat danger overlays, command mode preview, threat field visualization.
UE has generic tools; MC2 should have BattleTech RTS authoring tools.

**UE concepts borrowed and their MC2 equivalents:**
- Content Browser / Asset Manager → MC2 Asset Browser + manifest index + dependency graph
- Material Editor / Material Instances → MC2 Material Instance Editor (no node graph yet)
- Editor Utility Widgets → MC2 Tool Panels (Python/JSON-driven validators, batch importers)
- Gameplay Tags → MC2 Tags (hierarchical, semantic, RTS-battlefield-aware)
- Data Tables / Data Assets → CSV/JSON-backed defs with schema validation
- Niagara → VFX Preset Editor (preset-based, not node graph)
- Packaging / cook / publish → `mc2mod` CLI

**Explicitly deferred:** full Blueprint clone, full node material editor, full Niagara clone,
full level editor, marketplace backend, live multi-user editor, visual gameplay scripting,
in-editor terrain sculpting.

**The MC2 modding advantage:** tooling that understands BattleTech RTS semantics — not just
"is this mesh valid" but "does this mech have valid thermal/sensor metadata?", "does this weapon
show correct range bands?", "does this material work across all ViewModes?", "what will this look
like in mc2_24 with the real post stack?"

---

## Pending durable artifacts (write-when-ready)

- **Render contract document** at `docs/render-contract.md` (or
  `.planning/codebase/RENDER-CONTRACT.md`) — enumerate at
  function/symbol level: who enqueues into which master array, who
  flushes when, what state is inherited at each hook point, what each
  Track A/B/C/coalesce slice consumes/emits. Currently we burn context
  re-deriving this every render slice. Captured 2026-05-14 from
  codebase-architecture mapping; promote to real artifact when next
  render slice plan lands.
