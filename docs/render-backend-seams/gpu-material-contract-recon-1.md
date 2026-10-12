<!-- doc-status: current | reviewed:2026-10-11 -->
# GPU-MATERIAL-CONTRACT-RECON-1 — material binding model recon → **DEFER**

**Arc:** VULKAN-CONTRACT-MANIFEST-ARC · recon only, no code (this doc only) · 2026-06-22
**Built against:** nifty-mendeleev HEAD `9a602915` — re-grep line numbers before trusting (cheap to drift).
**Builds on:** `gpu-resource-manager-seam-recon-1.md` (§materials split-brain), `gpu-texture-resource-manager-seam-recon-1.md` (batchers pack via `gos_GetGLTextureId`), `binding-slot-occupancy.md` (slot multiplexing), docs/material-abi-unification-recon.md (archived 2026-10-11, E:/mc2-archive-2026-10/docs/cull-wave1-docs-2026-10-11.tar.gz) (the M0–M7 ABI plan), `docs/material-m0-contract.md` (M0 pin), `RenderCore/MaterialGpu.h` + `shaders/include/material_gpu.hglsl` + `scripts/check-material-gpu-mirror.sh` (the live mirror gate).

> **Foreign-WIP hazard (live this session):** `mclib/mech3d.cpp`, `mclib/txmmgr.h`, `tests/visual/golden-sets.json` are dirty/foreign. This recon reads only — touches none of them — and proposes no code.

---

## TL;DR

The "split-brain" is **real but it is NOT a binding-5-vs-7 problem.** It is a **three-table, three-semantic** divergence where only ONE table is shader-actionable today, and the binding-5/7 numbers the prior recon called out are largely a **red herring**:

- **Binding 5** is owned by static props and is genuinely the live, shader-sampled material table.
- **Binding 7** (`gos_materials.cpp`) is a global mech-surface *profile* table that is **bound but read by NO shader** — mech.frag samples its normal/ORM via plain `sampler2D` uniforms (`u_normalTex`/`u_ormTex`) fed raw GL ids CPU-side. The SSBO upload + `bindMaterialTable()` are effectively vestigial GPU-side; the table's data reaches the shader as loose samplers, not as an indexed SSBO record.
- A **third** table — mech *per-actor* at **binding 2** (`s_mechMaterialSsbo`) — also holds `MaterialGpu` records, is `compare-only`, and is likewise **not sampled by mech.frag**.

So two of the three `MaterialGpu` SSBOs are not consumed as material records by any shader. The struct *shape* is unified (one `MaterialGpu`, mirror-gated). What is NOT unified is **texture identity semantics** (4 meanings) and **who actually samples the record** (only static props). The D-material-unify debt is correctly scoped as a *future implementation milestone* (M2/M4 in the ABI plan), gated on the mech texture-model decision — exactly the kind of pass-specific, not-yet-settled state where a binding-contract checker would encode a model that is about to change. **VERDICT: DEFER.**

---

## 1. Material REPRESENTATIONS — the full set

`RenderCore::MaterialGpu` (`RenderCore/MaterialGpu.h:120-155`, 32 B std430, `static_assert`'d, mirror `shaders/include/material_gpu.hglsl:46-55`) is the single GPU-visible record *shape*. It is the authority for **record layout**. It is **not** the authority for material *identity/content* — each lane owns its own CPU table and writes records with lane-specific field semantics. There is also a per-pass legacy uniform path (mech) and a CPU sidecar/inventory family. Full set:

| # | Representation | Where | Authoritative for | Notes |
|---|---|---|---|---|
| 1 | `RenderCore::MaterialGpu` (32 B std430) | `RenderCore/MaterialGpu.h:120` | GPU record **layout/ABI** (the mirror contract) | one struct, three live producers, three semantics |
| 2 | Static-prop table `s_materialGpuTable` → `s_materialGpuSsbo` | `gos_static_prop_batcher.cpp:709`, upload `:3872-3877` | static-prop material content (the ONE shader-sampled table) | `TextureArrayLayer` semantic |
| 3 | Mech per-actor table `s_mechMaterialTable` → `s_mechMaterialSsbo` | `gos_mech_batcher.cpp:513,516`, build `:1786-1814` | mech per-actor albedo handle | `TextureManagerSlot`, **compare-only**, not shader-sampled |
| 4 | Mech profile table `s_profiles[].gpu` → `s_ssbo` | `gos_materials.cpp:56,58`, upload `:68-88` | global mech surface profiles (Metal061B etc.) | `RawGlId`, bound@7 but **no shader reads it** |
| 5 | Mech legacy draw-state uniforms | `gos_mech_batcher.cpp` `u_materialFlags` + `u_pbrNormalTex`/`u_pbrOrmTex` (`:2032-2034`) | what mech.frag *actually* consumes | loose `sampler2D` + flag uniform; the live mech material path |
| 6 | CPU sidecar/inventory (`StaticPropMaterialInventoryEntry`, `StaticPropTypeMaterialCache`) | `gos_static_prop_batcher.h:514`, `gos_static_prop_registry.h:180` | editor/inspector views | *views* of table 2, not a parallel ABI |
| 7 | Asset-viewer `MaterialSlotTextures` + loose PBR uniforms | `tools/asset_viewer/MaterialRenderBackend.h:8` | viewer-local preview | `RawGlId`, never reaches runtime SSBO |
| 8 | Terrain splat palette (5 fixed layers + loose uniforms) | `shaders/gos_terrain.frag:45-55`, `mclib/terrain.h:83` | terrain surface | **distinct ABI** — per-pixel classified, no per-draw `materialIdx`; explicitly out of `MaterialGpu` |

**Authority answer (Q1):** `MaterialGpu` is the authority for the *record layout* only (and the mirror gate enforces just that). It is **not** a single source of material *truth*: identity/content is owned per-lane, and the meaning of the texture fields is lane-specific. There is no separate "CPU `TG_Material` that is authoritative" — the legacy per-draw mech material state is loose uniforms (table 5), not a struct.

---

## 2. WHERE each field lives (record vs texture vs sampler vs uniform)

| Field | In the SSBO record? | Actual GPU residence | Notes |
|---|---|---|---|
| `albedoTex` | yes (uint32) | static props: it IS a `GL_TEXTURE_2D_ARRAY` *layer index* sampled by `static_prop.frag:213` via `texture(u_texArr, vec3(uv, layer))`. mech-per-actor: it is a `mcTextureManager` slot — never sampled; CPU resolves slot→gos handle→GL tex, binds to `u_tex`. | the texture *object* is bound to a sampler unit by the consumer; the record holds only an *index/slot/id*, never the GL object |
| `normalTex` / `metallicRoughnessTex` | yes (uint32) | static props: `kMaterialTexAbsent` today (ORM read at `static_prop.frag:263,396` but unwired in live data). mech profile: a **raw GL id** (`gos_materials.cpp:303-308`) — but consumed as `u_normalTex`/`u_ormTex` *samplers* (`GameOS/gameos/gos_mech_batcher_flush.cpp:267-277`), **not** by reading the SSBO | RawGlId-in-a-record is the most divergent case; the value is a GL name, not an index |
| `emissiveTex` | yes (uint32) | `kMaterialTexAbsent` for all live entries | unwired everywhere |
| `flags` | yes (uint32) | static props: read from record. mech: pushed as a plain `u_materialFlags` uniform (`material-abi-unification-recon.md` §a.3), not from the SSBO | same bits, two delivery paths |
| `baseColorFactor`/`metallicFactor`/`roughnessFactor` | yes (float) | static props: read from record (`static_prop.frag:390-391`). mech/profile/viewer: loose uniforms or defaults | M0 pinned roughness default to 1.0 across sites (`material-m0-contract.md` §1) |
| sampler **unit** | no | chosen by the consumer per its program manifest (`sampler-unit-occupancy.md`); orthogonal to the record | the record never names a unit — same separability the texture-seam recon found |

The load-bearing structural fact: **a `MaterialGpu` record never holds a GL texture object — only an index/slot/id whose meaning is the lane's `MaterialTextureSemantic`.** The texture object itself is always bound to a sampler unit by the consumer (legacy resolve or batcher raw-id). This is the Vulkan-relevant seam: in a descriptor world the record's texture field becomes a descriptor index; today it is one of four incompatible things.

---

## 3. WHY are the bindings split (5 / 7 — and the un-mentioned 2)?

**Root cause: DEBT, not a genuine per-pass semantic difference — but the debt is one of *ownership/history*, and "unify" is blocked on a separate, unsettled design decision (the mech texture model), which is what makes it more than a mechanical renumber.**

The full picture (the prior recon's "5 vs 7" undercounted):

| Binding | Buffer | Owner / file | Draw hint | Lifetime | Semantic | Shader-sampled? |
|---|---|---|---|---|---|---|
| **5** | `s_materialGpuSsbo` | `gos_static_prop_batcher.cpp` (bind `:5150`,`:5841`; upload `:3872`) | `GL_STATIC_DRAW` | map/mission | `TextureArrayLayer` | **YES** — `static_prop.frag`, `static_prop_depth.frag`, `building_pbr.frag` |
| **7** | `s_ssbo` | `gos_materials.cpp` (`kMechMaterialTableBinding=7`, bind `:503`, upload `:77-83`) | `GL_STATIC_DRAW` | process (global profiles) | `RawGlId` | **NO** — bound by `bindMaterialTable()` (`GameOS/gameos/gos_mech_batcher_flush.cpp:506`) but no GLSL block reads binding 7; mech.frag uses `u_normalTex`/`u_ormTex` samplers |
| **2** | `s_mechMaterialSsbo` | `gos_mech_batcher.cpp` (bind `:1941`, upload `:1810-1812`) | `GL_DYNAMIC_DRAW` | mission (persists, grows) | `TextureManagerSlot` | **NO** — compare-only; mech shader never reads it |

**The D-material-unify note (verbatim), `gos_materials.cpp:61-63`:**
> ```
> // Binding 7: mech material profile table (temporary; binding 5 owned by static-prop batcher).
> // Debt: D-material-unify — unify static-props and mechs under shared gos_materials table on binding 5.
> constexpr GLuint kMechMaterialTableBinding = 7;
> ```
And `gos_materials.h:6-13`: *"the buffer block must use binding=7 ... to avoid collision with static-prop materialTable_ at binding=5 ... This table lives at binding 7 (binding 5 is owned by the static-prop batcher; see D-material-unify debt to consolidate later)."*

So binding 7 was chosen **purely to dodge the static-prop binding-5 occupancy** — a historical/ownership accident, not a semantic requirement. The *record layout* is the **same** `MaterialGpu` struct in all three tables (tables 2/3/4 in §1). The differences are: (a) texture-identity semantic, (b) draw hint / lifetime, (c) whether any shader samples it.

**Why unify is more than renumbering:** unifying mechs onto the static-prop table requires mechs to become *shader-actionable* (a `TextureArrayLayer` or `DescriptorIndex` semantic), which is the explicit M4 gate in `material-abi-unification-recon.md` — `docs/superpowers/specs/2026-05-26-mech-material-gpu-mech2-decision.md`. Until that decision lands, the three tables legitimately differ in semantic, and binding 7's table is not even consumed as an SSBO. A contract that froze "binding 7 = mech material table" would be encoding a vestigial, about-to-change arrangement.

---

## 4. Shader CONSUMERS (which `.frag` reads which binding / struct / semantic)

| Shader | Declares `MaterialTable` @ binding 5? | Reads what | Texture semantic | Notes |
|---|---|---|---|---|
| `static_prop.frag` | yes (`:100-102`) | `materials[materialIdx].albedoTex` as array layer (`:213`); ORM/factors (`:258-396`) | `TextureArrayLayer` | the only fully shader-actionable lane; guarded by `u_materialGpuSample` |
| `static_prop_depth.frag` | yes (`:60-62`) | `materials[materialIdx].albedoTex` (`:97`) for alpha-test in the depth/shadow caster | `TextureArrayLayer` | shares binding 5; alpha cutout only |
| `building_pbr.frag` | yes (`:10-12`) | `materials[0]` — **fixed index 0**, factors only (`:57`) | `TextureArrayLayer` (degenerate) | reads the singleton entry; normal/ORM come from `u_normalTex`/`u_ormTex` samplers, not the record |
| `mech.frag` | **NO** | nothing — no `buffer`/`binding=` block at all (grep-confirmed 0 matches) | n/a (consumes `u_pbrNormalTex`/`u_pbrOrmTex` samplers) | binding 2 + binding 7 SSBOs are bound but **never read here** |
| `material_gpu_contract.frag` (fixture) | yes (`:14-16`) | `materials[0]` | — | reflect-golden ABI fixture, not a render pass |

`MaterialTextureSemantic` (`RenderCore/MaterialGpu.h:86-92`) confirmed per consumer:
`TextureArrayLayer`=static props (shader-actionable), `TextureManagerSlot`=mech per-actor (compare-only), `RawGlId`=asset-viewer + the mech profile table. `DescriptorIndex`/`BindlessHandle` are future-only.

**Key consumer finding:** the binding-5 table has **three** shader consumers; the binding-7 and binding-2 tables have **zero** shader consumers. The split is asymmetric — one real consumer surface (binding 5) and two upload-only tables.

---

## 5. Batcher PRODUCERS (who packs / uploads, static vs dynamic)

| Producer | Builds | Upload site | Hint | When |
|---|---|---|---|---|
| `gos_static_prop_batcher.cpp` | `s_materialGpuTable` (array-layer dedup at `finalizeGeometry`) | `:3872-3877` `MC2_GL_BufferData(..., GL_STATIC_DRAW)` | **static** | once per map; GL-error-guarded (deletes buffer → sampling auto-disabled on failure) |
| `gos_mech_batcher.cpp` | `s_mechMaterialTable` (texHandle→idx, persists per mission) | `:1810-1812` `glBufferData(..., GL_DYNAMIC_DRAW)` | **dynamic** | rebuilt per flush, re-uploaded only when the table grows (`tableDirty`) |
| `gos_materials.cpp` | `s_profiles[].gpu` (global named profiles) | `:77-83` `glBufferData(..., GL_STATIC_DRAW)` | **static** | once at `init()`; profiles selected by `MC2_MECH_SURFACE_MATERIAL` |

All three pack the same `sizeof(RenderCore::MaterialGpu)` stride. Static-prop + profile tables are immutable-per-map/process (→ a static descriptor set in Vulkan terms); the mech per-actor table is grow-only within a mission (still effectively static at steady state — append-only, re-upload on growth).

---

## 6. STATIC-per-material vs PER-DRAW vs PER-PASS

| Class | Data | Vulkan shape |
|---|---|---|
| **Immutable per material** (→ static descriptor set / read-only SSBO) | the `MaterialGpu` record itself: texture index/slot/id, flags, PBR factors. All three tables are map/mission/process-lifetime, never mutated in place. | a per-lane material-table SSBO bound once; descriptor stable for the map |
| **Per-draw / per-instance** (→ push-constant or per-instance attribute) | `materialIdx`: static-prop `PerDrawEntry.materialIdx`, mech `GpuMechInstance.materialIdx` (byte 52, `GameOS/gameos/gos_mech_batcher.h:230`). This selects WHICH record, it is not material content. | per-instance vertex/SSBO field, already exists |
| **Per-pass** (→ pass-level uniform, NOT per-material) | IBL participation (`u_iblSh`), fog, the mech surface-material *selection* (`s_mechSurfaceMaterialIdx` chooses one global profile per pass), `u_buildingPbrControls`, `u_pbrTileScale` | pass uniform block; correctly NOT in the record |

The record/index split is already clean and Vulkan-shaped. The thing that is NOT clean is the texture *field*: in three lanes it means three things, and only one is an index a shader can use.

---

## 7. Can ONE manifest/contract describe the material bindings without runtime change?

**Partially — and that partial is exactly the trap.** A static checker *could* assert, at CI time:
- the three `MaterialGpu` SSBO bindings (5 / 2 / 7) and that each C++ producer's `sizeof(RenderCore::MaterialGpu)` matches the GLSL block stride (the mirror gate already does the field-order half);
- the per-consumer `MaterialTextureSemantic` (which lane is homogeneous in which semantic);
- which shaders declare a `MaterialTable` block at binding 5 (occupancy).

But it **cannot** describe the live model honestly without encoding three things that are *in flight*:
1. binding 7's table is bound-but-unread (a checker asserting "binding 7 = mech material table" documents a vestige);
2. mech material reaches the shader via **samplers**, not the SSBO — a "material binding contract" that lists binding 2/7 would imply a GPU consumer that does not exist;
3. the unify (M2/M4) will **move** mechs onto binding 5 with a different semantic, invalidating any frozen 5/2/7 map.

So a contract written now would be **descriptive of a transitional state about to change**, not of a stable seam. That is churn-shaped (cf. the rejected flat binding-enum: the same "encode a model that's actually about to shift" failure mode).

### Smallest checkable artifact (if anything ships)
The **only** non-transitional, genuinely-stable surface worth checking today is already mostly covered:
- **Extend `scripts/check-material-gpu-mirror.sh`** (or add a sibling `check-material-bindings.py`) to additionally assert, from the existing files, that **every C++ producer of a `MaterialGpu[]` SSBO and every GLSL `MaterialTable { MaterialGpu ... }` block agrees on the 32-byte stride / 8-field layout** — i.e. catch a producer/consumer drift in the *struct*, which is the part that is stable and load-bearing. The binding *numbers* should be left to `check-binding-slots.py` (already WARNs on binding-5 occupancy and slot-7 multiplexing), NOT duplicated into a material-specific contract that would harden a vestigial layout.
- A `docs/material-binding-occupancy.{md,json}` could be emitted, but it would largely restate `binding-slot-occupancy.md` slots 5/2/7 plus the M0 semantic table — low marginal value, and it would need rewriting the moment M2/M4 lands.

**Net:** the smallest worthwhile artifact is a *struct-stride cross-check* extension to the existing mirror gate — NOT a new binding contract. The binding map itself is not yet stable enough to freeze.

---

## DESCRIPTOR-SHAPED contract proposal (descriptive only)

If/when the mech texture-model decision (M4) lands and mechs move to a shader-actionable semantic, the natural Vulkan shape is:

- **One descriptor set per lane** (props / mechs / VFX), each binding a read-only `MaterialGpu[]` SSBO as a **static** material table (set updated once per map; mirrors today's `GL_STATIC_DRAW` lifetime). The current binding 5 / 2 / 7 numbers collapse into "binding 0 of each lane's material set" — the cross-pass-multiplexing model already documented in `binding-slot-occupancy.md`.
- **`materialIdx`** stays a per-instance push-constant / instance attribute selecting the record (already exists).
- **Texture identity** becomes a uniform `DescriptorIndex` (into a bindless/indirection table) across all lanes — the M4 target semantic — which is the single change that would actually make ONE material contract describable.
- The mech profile table (binding 7) either folds into the per-lane mech set as real records (and mech.frag starts reading the SSBO instead of `u_normalTex` samplers) **or** is retired in favor of the sampler path. Until that is decided it should not be contract-frozen.

**A single unified material contract is achievable only post-M4** (uniform texture semantic + mech actually sampling its record). Pre-M4, "one contract for both tables" would require runtime changes (rewiring mech to read an SSBO) — i.e. it is NOT achievable "without runtime change" today, which is the core question and the core reason to DEFER.

---

## VERDICT — **DEFER**

The split-brain is real, but recon shows it is **less ready to contract than the prompt's premise assumed**:

1. **It is debt, not a stable semantic split** — `gos_materials.cpp:61` says so explicitly ("temporary"; "consolidate later"); binding 7 was picked only to dodge binding-5 occupancy.
2. **But the debt is gated on an unsettled design decision** (mech texture model, M4) — so it is not a mechanical renumber a checker could safely pin.
3. **Two of the three `MaterialGpu` SSBOs (binding 2 and binding 7) are bound but read by no shader** — mech material reaches the shader through `sampler2D` uniforms, not the record. A "material binding contract" listing 5/2/7 would document a consumer that does not exist and a vestige about to move.
4. **A contract written now describes a transitional state about to change** under M2/M4 → it would be churn, the exact failure mode of the rejected flat binding-enum.
5. **The one stable, checkable surface** (struct layout 32 B / 8 fields across producers and consumers) is **already ~covered** by `check-material-gpu-mirror.sh` + `check-binding-slots.py`; the only worthwhile increment is a small **struct-stride cross-check** extension to the mirror gate, not a new binding contract.

**Not GO:** the model is too pass-specific and too in-flight to contract cleanly — one material contract covering both tables requires runtime rewiring (mech → SSBO sampling) that does not exist yet, so it fails the "without runtime change" bar.
**Not STOP:** the work is not blocked on a missing pass-DAG; it is blocked on a *named, scheduled* decision (M4 mech texture model) already tracked in `material-abi-unification-recon.md`. Once M4 lands and texture identity is uniform, revisit as **GO** — at that point a single per-lane descriptor-set contract becomes describable and worth a checker.

**Prerequisite to re-open as GO:** complete M2 (ABI bump) + M4 (mech moves to a shader-actionable / `DescriptorIndex` texture semantic and mech.frag samples its material SSBO). Until then, the bankable deliverable is this recon + (optionally) the mirror-gate stride-check extension.

---

### EXCLUSIONS (respected)
No material refactor, no texture-manager impl, no descriptor abstraction, no Vulkan code, no shader edits (stale comments flagged in-doc only, not edited), no batcher rewrite, no unifying binding 5/7 (that is M2/M4 impl, not this recon). Did not touch `mclib/mech3d.cpp`, `mclib/txmmgr.h`, `tests/visual/golden-sets.json`.
