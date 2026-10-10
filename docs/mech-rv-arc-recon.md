<!-- doc-status: stale | reviewed:2026-10-11 -->
# Mech R→V Lane — Arc Recon (MECH-RV-ARC-RECON-0)

Read-only end-to-end map of the mech render lane, modeled on the
StaticPropOpaque (completed R→V reference) and Terrain (audited) lanes.
Goal: surface the authority chain, material/texture model, shader/pass
inventory, debug/inspector/capture gaps, risks, and the next safe slices.

All line numbers grep-confirmed against the `nifty-mendeleev` worktree at
recon time (HEAD `cf7c6bbd`). Re-grep before quoting; this is a 5139-line
`mech3d.cpp` and lines drift.

Sibling recon docs: `docs/shadow-rv-arc-recon.md` (shadow lane — separate;
referenced here read-only), `docs/terrain-rv-arc-recon.md`.

---

## 1. Authority chain

The mech render data flows R→V as:

```
BattleMech (game AI/state)
  └─ Mech3DAppearance::render()            mclib/mech3d.cpp:2483   (entry; enqueuer)
       ├─ mechShape->SetTextureHandle(0, localTextureHandle)  :2486 (slot0 = per-actor paint)
       ├─ GPU-cull gate (readback_isActorVisibleLagged)        :2496
       ├─ fills GpuMechSubmitDesc                              :2567-2619
       └─ GpuMechBatcher::submitActor(desc)
              GameOS/gameos/gos_mech_batcher.cpp:990
                ├─ typeLod lookup s_typeLodIndex {mechType,lod}  :990  (late-reg fails here)
                ├─ stage bone matrices listOfShapes[i].shapeToWorld → PendingSubmit::bones :1016
                ├─ resolve per-packet texHandles (slot0 = desc; slot1+ = type) :1041
                └─ push s_pendingSubmits
       GpuMechBatcher::flush()  gos_mech_batcher.cpp:1077
         ├─ Step 2.5 build s_mechMaterialTable / s_mechHandleToMaterialIdx :1204-1232
         ├─ write instance SSBO (binding 0) + bone SSBO (binding 1)
         ├─ [gated] extract ExtractedMechPacket → s_mechExtractPersist :1600-1627
         └─ glDrawElementsInstancedBaseVertex per bucket
```

**Ownership table:**

| Datum | Owner | Site |
|---|---|---|
| mesh geometry | `Mech3DAppearanceType::mechShape[lod]` (TG_TypeMultiShape from .ase) | `mclib/mech3d_type.cpp:32` |
| per-actor pose (bones) | `TG_MultiShape::listOfShapes[i].shapeToWorld` | staged at `gos_mech_batcher.cpp:1016` |
| slot0 texHandle (paint) | mcTextureManager **slot index** (NOT gos handle) on `Mech3DAppearance` | `mech3d.cpp:2486` |
| objectIdRaw | **RenderWorld** handle (`kMechHandleBase=0x10000`) | `RenderWorld.cpp:971`, stored `mech3d.h:478` |
| materialIdx | `s_mechHandleToMaterialIdx[slot0TexHandle]` → `s_mechMaterialTable`; sentinel `0xFFFFFFFF` | `GameOS/gameos/gos_mech_batcher_geometry.cpp:36` |
| skinning mode | `u_skinningMode` uniform (0=rigid, !=0=4-bone blend) | `mech.vert:73` |

**RenderWorld relationship — route-only.** `MechRenderAdapter` (Slice M2,
`GameAdapters/MechRenderAdapter.{h,cpp}`) registers a mech at spawn
(`RenderWorld::registerMech`, `RenderWorld/RenderWorld.cpp:1002`) and stores the handle
back on the appearance (`setRenderWorldHandleForAdapter`,
`MechRenderAdapter.cpp:111`). RenderWorld is **not** authoritative for mech
draw — it is an object-ID/handle ledger only. The adapter is explicitly
temporary (`MechRenderAdapter.h:13`).

**Snapshot relationship — observational only.** `ExtractedMechPacket`
(`render_snapshot.h:28`) is written in `flush()` before the pending list is
cleared, gated `MC2_SNAPSHOT_MECH_EXTRACT=1` (default OFF). It carries
`objectIdRaw, instanceIdx, materialIdx, texHandle, typeLodIdx, renderFlags`.
Consumers: `batcher_getMechPendingEntry()`, `batcher_compareMechSnapshot()`
(compare-against-live), and the inspector panel. **Unlike StaticProp's
v3 flip, mech snapshot is NOT a dispatch authority** — the live batcher
remains the sole draw path. `RenderPassContract.h:117` records
`snapshotRowAuthoritative=true` (rows match) but
`pipelineDescRegistered=false` and `viewUniformsBound=false`.

---

## 2. Current material/texture chain

**Authoritative material datum for a mech = the live slot0 texHandle**
(mcTextureManager slot index → resolved per-frame to a live gos handle →
GL texture id). Mech textures are **live, per-draw-bound `GL_TEXTURE_2D`
objects — NOT array layers**, confirmed in code:

- `mech.frag:24` declares `uniform sampler2D u_tex;` (single 2D, not array).
- `GameOS/gameos/gos_mech_batcher_geometry.cpp:286-345`: per draw call, `dc.texHandle` (slot index,
  comment "NOT a gos handle") → `get_gosTextureHandle()` (`:1459`) →
  `gos_GetGLTextureId()` (`:1461`) → bound at GL_TEXTURE0 (`:1513`).
- `mech.frag:64` forces `textureLod(u_tex, v_uv, 0.0)` — AMD RX 7900 XTX
  auto-LOD strict-fail workaround on paint-scheme textures.

**`materialIdx` is meaningful only as an identity/compare token, not a
sampling key.** Mech-1 wired `s_mechHandleToMaterialIdx` (one MaterialGpu
entry per unique texHandle, `albedoTex=texHandle`), uploaded to an SSBO at
binding=2 (`gos_mech_batcher.cpp:1204-1231,1346`). **The shader never
declares or samples that SSBO** — `mech.vert`/`mech.frag` have no
`binding=2` material buffer. This matches the Mech-2 "Option A
(identity/compare only)" decision: mech textures are not array-able without
a dedicated cook arc, so `materialIdx` stays a compare/substrate field.
Gate `MC2_MATERIAL_GPU` (default ON) controls the upload; compare/mismatch
logged at `GameOS/gameos/gos_mech_batcher_geometry.cpp:137-155`.

**Team color / paint scheme = CPU texture recolor at load, NOT a shader
feature.** `Mech3DAppearance::setPaintScheme()` (`mech3d.cpp:1725-1836`):
locks the gos texture, runs a **dominant-channel classifier** (`:1772-1806`,
`kRatio=3.0`) that maps R/G/B-dominant texels to paint slots 0/1/2, then
writes `tintColor*shade` blended by a soft `mix` confidence back into the
texel buffer. Paint RGB from `paintSchemata.fit` (`:1686-1721`). Re-applied
on team assignment / load (`resetPaintScheme()` `:1879`). **No
`u_teamColor`/`u_paintScheme` uniform exists** — the baked texture is what
the GPU sees, so team color is invisible to any debug view that samples
post-bake albedo.

**Damage mask — none in the GPU path.** No damage texture/mask in
`mech.vert`/`mech.frag`/`gos_mech_batcher.cpp`. Damaged-state meshes load
via the legacy `TG_Shape` path (`bdactor.cpp:261`) and are not wired into
the batcher shader.

**Normal / spec / roughness / palette / LUT — all absent.** `mech.frag`'s
only sampler is `u_tex`. Normal comes solely from the vertex attribute
`a_normal` through the bone matrix; written to `GBuffer1` for screen-shadow
eligibility (`mech.frag:87`) but never used for normal-mapped shading.
`a_tangentOct` (`mech.vert:26`, GL_SHORT) is declared but zero-filled and
unused — the ABI slot exists for a future normal-map slice.

---

## 3. Current shader / pass inventory

| Shader | Role | View transport |
|---|---|---|
| `shaders/mech.vert` | instanced+skinned mech vertex; reads instance SSBO (b0) + bone SSBO (b1); per-vertex `calc_light()` when `u_lightingMode=1`; Stuff→MC2 axis swap inline | **legacy `uniform mat4 u_worldToClipGL`** (`:61,133`) |
| `shaders/mech.frag` | single `u_tex` sampler; alpha-test; fog mix; **9 debug modes already present**; writes FragColor + GBuffer1 (+ objectId attachment under `MC2_OBJECT_ID_BUFFER`) | — |
| `shaders/shadow_mech.vert` | depth-only mech shadow (mirrors bone math) | `uniform mat4 lightSpaceMatrix` (`:16`) — **shadow lane, read-only here** |

No mech geometry/tessellation shader. **View transport is legacy flat
uniform**, NOT the ViewUniforms UBO (binding=3). CPU uploads via
`gos_GetTerrainMVPMat4()` → `glUniformMatrix4fv(s_loc_terrainMVP,...)`
(`GameOS/gameos/gos_mech_batcher_geometry.cpp:268`). The legacy `cameraToClip` consumer noted in
memory is `tgl.cpp:1620,1624` (TG_Shape path); the batcher reads the same
composed matrix.

**Pass contract:** `RenderPassContract.h:117` `MechOpaque` /
`GpuMechBatcher` — `viewUniformsBound=false`, `pipelineDescRegistered=false`,
`snapshotRowAuthoritative=true`, `killSwitchEnv=MC2_SNAPSHOT_MECH_EXTRACT`.

**Skinning:** `GpuMechVertex` (48B, `gos_mech_batcher.h:15`) carries
`boneIndices[4]`/`boneWeights[4]`. `u_skinningMode==0` (default) = rigid
single-bone; `!=0` = weighted 4-bone (Track D/Assimp imports). Stock assets
ship `(1,0,0,0)` so both modes are byte-identical for stock — **do not
touch skinning; gameplay/animation invariant.**

---

## 4. Current debug / inspector / capture gaps

**RenderDebugView registry** (`RenderCore/RenderDebugView.h`): enum has
Final/Albedo/Normal/Roughness/Metallic/LightingOnly/IblOnly/SpecularOnly/
MaterialIdx/TexArrayLayer (`_Count=10`). **`kDebugViewMask_Mech = 0u`** —
mech lane registers ZERO views (placeholder; `test_rendercore.cpp:151`
asserts zero). StaticProp registers 7.

**BUT mech.frag already implements 9 internal debug modes** (agent-confirmed)
— they exist in the shader but are **not wired to the RenderDebugView
registry, not exposed in the inspector, and the driving uniform is
unaudited.** This is the central slice-3 opportunity: surface what already
exists rather than build new shading. (Slice-3 recon must read mech.frag's
debug-mode block + find its uniform + map modes to the registry enum.)

**Inspector** (`GuiRuntime/EditorInspector.cpp`): two mech panels already
exist —
- "Mech" identity panel (`:785`): variant/long name, chassis class, team ID,
  pilot, status, armor, structure (via `setMechData()`).
- "Mech Snapshot" panel (`:829`, gated `MC2_SNAPSHOT_MECH_EXTRACT`): rows,
  mat_valid/sentinel, 5 mismatch counters, mech+shadow program IDs,
  annotates **"PipelineDesc: legacy (not on registry)"** (`:864`), and
  per-selected-mech `objectIdRaw / texHandle (+ texture name) / materialIdx /
  typeLodIdx / renderFlags`.

So **material inventory is ~70% already present** for the selected mech —
the gaps are (a) it's per-pick only, not a fleet list; (b) it's gated; (c)
team-color/damage facts are not surfaced; (d) **no machine-readable export.**

**Debug-state JSON** (`debug_state_dump.cpp`, schema `MC2_DEBUG_STATE_V1`):
has `engineView`, `registeredViews[]`, `renderSnapshot` (staticProp only),
`renderPasses`, `staticPropOpaque{}` (15 fields), `renderResources[]`.
**No mech section at all** — no mech snapshot counters, no mech pass state.
This is the cleanest, lowest-risk slice-2 target (read-only emit, mirrors
the existing `staticPropOpaque{}` block).

**EngineView:** only `MainScene` registered (`gamecam.cpp:206`). Mechs do
not register/use an EngineView.

**RenderResourceRegistry** (`RenderResourceRegistry.h`): IDs Unknown/
MainColor/MainDepth/ShadowStaticMap/TerrainHeightTexture/MaterialGpuBuffer/
ShadowDynamicMap. **No mech instance/bone/material resource registered.**

**PipelineDesc** (`PipelineRegistry.cpp`): 3 rows (Invalid, StaticPropOpaque,
StaticPropAlphaTest). **No mech PipelineId.**

**Capture/baselines** (`tests/visual/baselines/`): staticprop + terrain
variants only. **Zero mech baselines.**

### Gap table vs StaticProp lane

| Axis | StaticProp | MechOpaque |
|---|---|---|
| RenderDebugView mask | 7 modes | `0u` (but 9 modes live in mech.frag, unwired) |
| Inspector shader-mode picker | yes | none (identity + snapshot panels only) |
| PipelineDesc registered | yes | no (legacy) |
| ViewUniforms UBO bound | yes (b3) | no (legacy `u_worldToClipGL`) |
| Debug-state JSON section | `staticPropOpaque{}` | **none** |
| Visual capture baselines | ~50+ | **zero** |
| RenderResourceRegistry entry | MaterialGpuBuffer | none |
| Snapshot role | dispatch authority (v3 flip) | observational only |

---

## 5. Risk list

- **R1 — Team color is CPU-baked, not shader-side.** Any future albedo/
  lighting debug view samples the *post-paint* texture; a "team color" debug
  view cannot recover the pre-paint slots from the GPU side. A TeamColor
  debug mode would have to read `PaintSchemata`/classifier state from the
  CPU, not the shader. Don't promise a shader TeamMask view that the data
  model can't back. (HIGH for honesty of debug labels.)
- **R2 — Skinning is gameplay/animation-load-bearing.** `u_skinningMode` and
  the bone SSBO are shared with Track D imports and `PerPolySelect` picking
  (`gpu_mech_aware_mouse_pick_queued.md`). No touching bone math, vertex
  format, or skinning paths. (HIGH.)
- **R3 — mech textures mutate every frame** (`mc2_texture_handle_is_live.md`):
  store slot index, never cache the gos handle at registration. Any
  inventory readback must resolve live per-frame, not snapshot a handle.
  (MED — affects inventory correctness.)
- **R4 — mech.frag's 9 debug modes are unaudited.** Wiring them to the
  registry without reading the shader's mode block risks mislabeling modes or
  toggling a path that mutates default output. Slice-3 must audit the driving
  uniform and confirm mode 0 = byte-identical default. (MED.)
- **R5 — Shadow lane firewall.** `shadow_mech.vert` + dynamic/static shadow
  split are a separate lane. Read-only observation only; no edits. (HIGH if
  crossed.)
- **R6 — Legacy view transport.** Mechs on `u_worldToClipGL` means a future
  MECH-VIEWUNIFORMS slice is a real shader+reflect change (golden drift),
  not a no-op. Defer; not in Batch 1. (MED, future.)
- **R7 — MaterialGpu mech SSBO is substrate-only.** Don't let a debug view
  read `materialIdx` as if it indexes real material data — it's a
  per-texHandle identity token. A MaterialIdx palette view is truthful only
  as "distinct-texHandle coloring," label it that way. (MED.)
- **R8 — Late type registration.** `submitActor` fails if `{mechType,lod}`
  absent from `s_typeLodIndex` (`gos_mech_batcher.cpp:990`); inventory walking
  all mechs must tolerate not-yet-registered types. (LOW.)

---

## 6. Recommended next slices

Ordered for lowest-risk-first, each gated default-OFF, no default visual
change, mirroring the StaticProp/Terrain lane pattern.

1. **MECH-MATERIAL-INVENTORY-1 (Batch 1, slice 2).** Lowest risk. Add a
   **mech section to the debug-state JSON** (`debug_state_dump.cpp`) mirroring
   the `staticPropOpaque{}` block: per-mech `objectIdRaw / texHandle / texture
   name / materialIdx (+sentinel flag) / typeLodIdx / renderFlags`, plus
   frame-level counts and the 5 mismatch counters. Read-only emit; reuses
   `batcher_getMechPendingEntry()` + existing inspector data. Optionally also
   widen the inspector "Mech Snapshot" panel from per-pick to a small fleet
   list. **No shader change, no texture-load change.** Validate: build, tier1
   5/5, mc2_24 active-mech, JSON shows sane mech rows.

2. **MECH-DEBUG-VIEWS-1 (Batch 1, slice 3).** Recon mech.frag's existing 9
   debug modes + driving uniform FIRST. Then set `kDebugViewMask_Mech` to the
   subset that is *truthful with available data* (Final, Albedo, Normal from
   GBuffer1, MaterialIdx-as-distinct-texHandle palette; **omit** TeamMask/
   DamageMask/Roughness/Metallic — data not present per R1/R3/R7). Wire to
   RenderDebugView registry + inspector combo, preserving any legacy numeric
   mode behavior. shader_reflect hygiene if a uniform/golden drifts;
   env_registry if a new var. Mode 0 must be byte-identical. Gated, no
   default change.

3. **MECH-BASELINE-0 (Batch 2, slice 4).** Add mc2_24 (+mc2_17) mech capture
   presets: default + the slice-3 debug modes. Metadata: mech debug mode,
   inventory mode, gates, mission/commit. First mech entries in
   `tests/visual/baselines/`.

4. **MECH-LIGHTING-PLAN-0 (Batch 2, slice 5, doc only).** Plan the first safe
   visual improvement. Open questions already answered by this recon: mechs do
   NOT consume ViewUniforms; MaterialGpu is identity-only; lighting today is
   per-vertex `calc_light()` (`u_lightingMode=1`); no normal/spec data; team
   color is CPU-baked. The plan must weigh ViewUniforms migration (R6, shader
   change) vs ambient/IBL-lite vs honest team-color surfacing, and pick a
   gate/env name + required capture evidence. **Do not implement without
   separate approval.**

Deferred / not authorized: MECH-VIEWUNIFORMS-1 (R6), MECH-TEAMCOLOR-DEBUG-1
(R1 — needs CPU-side classifier data), MECH-AMBIENT-1, MECH-IBL-1,
MECH-PBR-PRE-1 (no normal data), MECH-DAMAGE-MASK-1 (no GPU damage path).

**Verdict:** mech lane is at R-stage observational. Slices 2–3 bring it to
the StaticProp/Terrain debug-parity stage with zero default-visual risk.
A first *visual* slice is NOT yet justified — it needs MECH-LIGHTING-PLAN-0
and capture baselines first.
