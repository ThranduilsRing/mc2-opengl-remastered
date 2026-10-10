<!-- doc-status: stale | reviewed:2026-10-11 -->
# GPU-TEXTURE-RESOURCE-MANAGER-SEAM-RECON-1 — deep texture-seam recon → **GO, but QUALIFIED (default-on, behavior-preserving, low value)**

**Arc:** VULKAN-CONTRACT-MANIFEST-ARC · recon only, no code (this doc only) · 2026-06-22
**Builds on:** `docs/render-backend-seams/gpu-resource-manager-seam-recon-1.md` (textures = minimal first seam) and `sampler-unit-occupancy.md` (SHADER-SAMPLER-BINDING-MANIFEST-1).
**Built against:** nifty-mendeleev HEAD `bc000039` — re-grep line numbers before trusting (cheap to drift).

> **Foreign-WIP hazard (live this session):** `mclib/mech3d.cpp`, `mclib/txmmgr.h`, `tests/visual/golden-sets.json` are dirty/foreign. This recon reads `txmmgr.cpp` only and proposes a future slice that touches the **`gameos_graphics.cpp` + `gl_utils.cpp` side only** — `txmmgr.h` stays read-only.

---

## TL;DR — the key question, answered

**Q: Can we introduce a GL-only `GpuTextureManager` behind the existing `gos_*` texture APIs WITHOUT changing handles, shaders, samplers, or any callsite outside the renderer?**

**A: YES.** The `gos_*` texture API + opaque `gosTextureHandle` DWORD + the M6 firewall already form a hard boundary. A GL-only texture manager is a *pure internal re-housing* of `gosRenderer`'s texture half (`textureList_` / `gosTexture` / the `gl_utils.cpp` GL primitives) behind the unchanged `gos_NewTexture*` / `gos_DestroyTexture` / `gos_LockTexture` API. `mclib/` and `code/` need **zero** changes; shaders, sampler units, and handle format are untouched.

**Verdict: GO — but qualified.** It is genuinely the lowest-risk first true seam *and* it earns only **modest** Vulkan-prep value, because the encapsulation is **already ~80% delivered** by the existing handle indirection + firewall. The honest framing: this is a *consolidation/clarity* slice (one named owner instead of three scattered layers), not a *capability-unlocking* slice. **Recommend GO only if banked as an explicit "name the owner so the Vulkan device-creation port has one file to fork" step — otherwise DEFER**, because it costs a full GameOS relink for clarity, not new behavior.

---

## Texture OWNERSHIP TABLE

| Layer | Owns | Where | Notes |
|---|---|---|---|
| **A. Slot/cache (game-facing, NO raw GL)** | `MC_TextureNode[4096]` (`masterTextureNodes`), the protected `DWORD gosTextureHandle`, lazy cache-in/out | `mclib/txmmgr.cpp` (alloc `:420`, free `:547`), `get_gosTextureHandle()` `:4250` | Hands out / re-resolves the **opaque** `gosTextureHandle` DWORD. Never sees a `GLuint`. Read-only for the seam. |
| **B. Handle registry (the gosTexture owner)** | `std::vector<gosTexture*> textureList_` | `gameos_graphics.cpp` field `:2157`, `addTexture` `:1534`, `getTexture` `:1622`, `deleteTexture` `:1638`, teardown `:5273` | Hands out the **handle** = index into the vector. Owns the `gosTexture*` lifetime. **This is the seam's primary surface.** |
| **C. GL object (the GLuint owner)** | `GLuint` inside `Texture tex_` (`gosTexture::tex_.id`) | `gosTexture` class `:1005`, ctors `:1007/:1038/:1069`, `createHardwareTexture()` `:1259` | Wraps a `Texture` struct whose `.id` is the live `GLuint`. dtor `:1090` → `destroyTexture(&tex_)`. |
| **D. Raw GL primitive layer** | `glGenTextures` / `glTexImage2D` / `glTexSubImage2D` / `glGetTexImage` / `glDeleteTextures` | `GameOS/gameos/utils/gl_utils.cpp`: `create2DTexture` `:166`, `updateTexture` `:270`, `getTextureData` `:312`, `destroyTexture` `:157` | **Layer D was NOT broken out in the prior high-level recon.** It is the actual GL-call site and the truest "GL-only" extraction target. |

**Who hands out handles:** `addTexture()` returns `textureList_.size()-1` (a monotonically-growing index, never reused — slots are nulled on delete, never compacted; see `deleteTexture`). That index IS the `gosTextureHandle` stored in `MC_TextureNode`.

---

## Texture handle LIFECYCLE (create → cache-in/out → bind → destroy → reset)

```
CREATE (cold load, e.g. mech/terrain texture)
  mclib caller → gos_NewTextureFromMemory(fmt, name, bytes, size, hints)   [GameOS/gameos/gameos_graphics_hud.cpp:321]
    → new gosTexture(...)                                                  [ctor :1007]
    → gosTexture::createHardwareTexture()                                  [:1259]
        → create2DTexture()  → glGenTextures + glTexImage2D (+mip)         [gl_utils.cpp:166]
    → gosRenderer::addTexture(ptex) → returns handle = index              [:1534]
  handle stored in MC_TextureNode.gosTextureHandle                         [txmmgr.cpp]

CACHE-OUT (mid-frame eviction, MC_TextureManager::flushCache / update)
  txmmgr.cpp :872/:900 → gos_DestroyTexture(handle)                        [→ deleteTexture]
  node.gosTextureHandle = CACHED_OUT_HANDLE (0xFFFFFACE)                   [:875/:902/:928]
  (GL object freed; the textureList_ slot is nulled)

CACHE-IN (lazy, on next access)
  get_gosTextureHandle() sees CACHED_OUT_HANDLE                            [mclib/txmmgr_render_passes.cpp:752]
    → (maybe flushCache to free a slot)                                    [:4269]
    → gos_NewTextureFromMemory(...) re-uploads → NEW handle               [:4447]
    → node.gosTextureHandle = new handle
  *** INVARIANT: a CACHED_OUT node MUST resurrect via the gos_NewTexture* path.
      The static-render-bug class lives here (see Risks). ***

BIND / USE (legacy MLR / fixed-function path)
  caller → gos_SetRenderState(gos_State_Texture, handle)                   [txmmgr.cpp 20+ sites :2405..:3647]
  applyRenderStates() resolves handle → GL                                 [gameos_graphics.cpp:5544-5569]
    → getTexture(handle)->getTextureId() → glActiveTexture+glBindTexture
    (caches cachedResolvedTexId_[i] for the rs-equality early-out :5419)

BIND / USE (GPU-driven batcher path)
  batcher → gos_GetGLTextureId(handle) → raw GLuint                        [GameOS/gameos/gameos_graphics_capi.cpp:875]
    → packed into a material SSBO / bound directly (mech + static-prop)

DESTROY (explicit)
  gos_DestroyTexture(handle) → deleteTexture(handle)                       [:8124 → :1638]
    → delete textureList_[h]; textureList_[h] = 0   (~gosTexture → destroyTexture → glDeleteTextures)

RESET / TEARDOWN (renderer shutdown — see Reset section)
  ~gosRenderer / destroy: for each textureList_ entry → delete             [:5273-5276]
```

---

## CREATE / DESTROY callsites (complete)

**Create (public API entry points), `gameos_graphics.cpp`:**
- `gos_NewEmptyTexture` `:8027` → ctor `:1038` (w/h, no data → magenta-fill or dynamic)
- `gos_NewTextureFromMemory` `:8045` → ctor `:1007` (TGA-in-memory; the cache-in workhorse)
- `gos_NewCompressedTexture2D` `:8066` → glGenTextures+`glCompressedTexImage2D` **inline** `:8076/:8090`, then wrap via prebuilt-ctor `:1069` (BC7 .ktx2 sidecars)
- `gos_NewTextureFromFile` `:8115` → ctor `:1007` (`is_from_memory=false`) → `createHardwareTexture` loads from disk
- `addTexture` `:1534` (registry insert, called by all of the above)

**GL primitive create (the real glGenTextures sites):**
- `create2DTexture` `gl_utils.cpp:166`, `createDynamicTexture` `:204`, `create3DTextureF` `:229`, `createPBO` `:254`
- inline magenta fallback `GameOS/gameos/gameos_graphics_overlay.cpp:74` (`getOverlayFallbackTexture`, a permanent 1×1, NOT in `textureList_`)
- inline BC7 `:8076`; terrain normal array `:2023`; misc inline `:8076/:9385`

**Destroy:**
- `gos_DestroyTexture` `:8124` → `deleteTexture` `:1638` (nulls slot, runs `~gosTexture` → `destroyTexture` → `glDeleteTextures`)
- `~gosTexture` `:1090` → `destroyTexture(&tex_)`
- `destroyTexture` `GameOS/gameos/utils/gl_utils.cpp:152` (the only `glDeleteTextures` for `textureList_` textures)
- teardown loop `:5273-5276`
- cache-out callers in `txmmgr.cpp`: `:872`, `:900`, `:927` (all via `gos_DestroyTexture`)

---

## BIND / USE callsites (grouped by subsystem)

The sampler manifest (`sampler-unit-occupancy.md`) counts **~224 `glActiveTexture` occurrences across 15 GameOS files** (note: prior recon's "~201" figure has drifted up). For the **seam**, only the *handle→GL resolution* sites matter — and there are exactly **two binder families**, both renderer-internal:

**Family 1 — legacy render-state resolve (the canonical handle→GL bind):**
- `applyRenderStates()` `gameos_graphics.cpp:5544-5569` — resolves `gos_State_Texture{,2,3}` → `getTexture(h)->getTextureId()` → `glActiveTexture`+`glBindTexture` (units 0/1/2). rs-equality early-out caches `cachedResolvedTexId_[]` `:5419-5424`.
- Driven by **~24 `gos_SetRenderState(gos_State_Texture, get_gosTextureHandle())` sites in `txmmgr.cpp`** (`:2405,:2416,:2485,:2489,:3218..:3647`) — the MLR/fixed-function shape render path. These pass the **opaque handle**; they never see GL.

**Family 2 — GPU-driven batcher raw-id resolve (`gos_GetGLTextureId`):**
- `gos_GetGLTextureId` def `:9991`; `gos_GetTextureGLId` `:8160`; `gos_GetGLTextureName` `:10006` (particle bridge).
- Consumers (ALL inside `GameOS/gameos/`): `GameOS/gameos/gos_mech_batcher_flush.cpp:475`; `GameOS/gameos/gos_static_prop_batcher_mapload.cpp:313,:7131,:7518,:7811`; particle bridge. These pack the raw `GLuint` into material SSBOs / bind for shadow-alpha packets.
- Terrain bridge resolves via `getTexture(gosHandle)->getTextureId()` (`GameOS/gameos/gos_terrain_bridge.h:35`).

**Both families call `getTexture()` / `getTextureId()` — i.e. they go through the registry (Layer B).** A `GpuTextureManager` that *is* Layer B keeps both families working unchanged.

---

## SAMPLER relationship — separable, the seam must NOT touch samplers

The sampler **unit** is chosen by the *consumer* (the binder picks `GL_TEXTURE0+unit` per its program's manifest entry; see `sampler-unit-occupancy.md`). The **texture object** (the `GLuint`) is what the manager owns. These are orthogonal: `applyRenderStates` binds the resolved id to a unit it computes locally (`:5548`), and the batchers bind the raw id to a unit from their own pass manifest. **A `GpuTextureManager` only owns create/store/resolve/destroy of the `GLuint`; it never picks a unit.** Confirmed separable — the seam does NOT touch the sampler namespace, and `check-sampler-bindings.py` stays the authority there.

---

## RAW-GL-TEXTURE-LEAK verdict: **NO LEAK (firewall holds, with one nuance)**

- **`code/` and `mclib/`: ZERO** references to `gos_GetGLTextureId` / `gos_GetTextureGLId` / `->getTextureId()` / `glBindTexture` (repo_grep over `code/*.{cpp,h}` + `mclib/*.{cpp,h}` → 0 matches). Game logic deals only in the opaque `gosTextureHandle` DWORD.
- **`check-no-raw-gl-from-game.sh`** scope = `code mclib`, bans raw `gl*()` calls. `gos_GetGLTextureId` is a `gos_*` function (allowed by design — it is the abstraction, not a raw GL call). So the resolver is *not even in firewall scope* and *not used* in scope anyway.
- **Nuance (not a leak):** raw `GLuint`s DO cross **inside the renderer** — `gos_GetGLTextureId` deliberately hands the GL name to the GPU-driven batchers (`gos_mech_batcher`, `gos_static_prop_batcher`) so they can pack it into material SSBOs. This is **GameOS-internal**, by design, and out of firewall scope. A `GpuTextureManager` should *keep* exposing this internal resolver (it is the bindless/SSBO seam, and is exactly what a Vulkan port will need as `VkImageView`/handle).

**Conclusion:** the "external leak the seam was meant to encapsulate does not exist for textures." The firewall + handle indirection already deliver it. (This is the prior recon's caveat, now verified at the callsite level.)

---

## RESET / DESTRUCTION mechanism today

- **Mission unload:** `MC_TextureManager` flushes non-`neverFLUSH` nodes (`txmmgr.cpp:597-664`), each via `MC_TextureNode::destroy()` → `gos_DestroyTexture` → `deleteTexture` → `~gosTexture` → `glDeleteTextures`. Nodes flagged `neverFLUSH`/`uniqueInstance`/`pinRefCount>0` survive.
- **Mid-frame cache pressure:** `flushCache` / `update()` (`:834-928`) evicts to `CACHED_OUT_HANDLE`; lazy `get_gosTextureHandle` resurrects.
- **Renderer shutdown / context teardown:** `~gosRenderer` (the destroy path at `:5263-5290`) deletes fonts first (they reference textures), then walks `textureList_` deleting every `gosTexture*` (`:5273-5276`), plus the few inline-owned GL textures (`terrain_normal_array_tex_` `:5283`).
- **Device/context loss:** there is **no D3D9-style "lost device → rebuild all" path** in this GL port. The `gos_RebuildFunction pFunc` parameter on `gos_NewTexture*` is **always 0** (`gosASSERT(pFunc==0)` `:8047`) — the rebuild-callback mechanism is vestigial. Context loss is not handled; teardown is full-destroy only. **A `GpuTextureManager` need not implement device-loss** to be behavior-preserving — but it is the natural future home if Vulkan/EGL ever needs it.

---

## EXACT files/functions a GL-only `GpuTextureManager` WOULD touch

**WOULD touch (all inside `GameOS/gameos/`, renderer-internal):**
- `gameos_graphics.cpp`: move `gosTexture` class (`:1005-1204`), `createHardwareTexture` (`:1259`), `textureList_` + `addTexture`/`getTexture`/`deleteTexture`/`getTextureListSize` (`:1534-1644`), the teardown loop (`:5273-5276`) into a `GpuTextureManager` (new TU, e.g. `gos_texture_manager.{h,cpp}`). The `gos_NewTexture*` / `gos_DestroyTexture` / `gos_Lock/UnLockTexture` / `gos_GetGLTextureId` entry points become thin forwarders to it.
- `gl_utils.cpp` (Layer D): may remain as-is (the manager calls it) **or** the 2D helpers move in. Prefer **leave gl_utils.cpp alone** to minimize churn — it is shared with RT/FBO code.
- The two binder families keep calling `getTexture()`/`gos_GetGLTextureId()` — now methods on the manager. No signature changes.

**MUST NOT touch (the boundary):**
- `mclib/txmmgr.{cpp,h}` — the slot/cache layer. **`txmmgr.h` is foreign-WIP-dirty; keep it read-only.** Do not alter `MC_TextureNode`, `gosTextureHandle`, `get_gosTextureHandle`, `CACHED_OUT_HANDLE`.
- The `gos_*` API **signatures** in `gameos.hpp` — handle format (`DWORD` index) is the contract.
- Any shader, any GLSL `layout(binding=)`, any sampler-unit literal — out of scope.
- `code/` — out of scope entirely (already clean).
- `RenderResourceRegistry` lifetime — see below.

---

## RenderResourceRegistry — OBSERVE, do not OWN (confirmed)

`RenderResourceRegistry.h:9` is explicit: "descriptive only; owners retain GL lifetimes," `glName` is debug-only, populated by RT owners (postprocess, terrain-height). A `GpuTextureManager` should, if anything, **publish descriptors INTO** the registry (like the postprocess RTs do) — never extend the registry to own texture lifetime. Extending it for ownership would break its descriptive-only invariant. **The `MaterialGpuBuffer` slot is declared-but-unpopulated; a texture manager could optionally populate a texture-descriptor view, but that is additive and not required for the seam.**

---

## SAFE FIRST SEAM recommendation + default-on / behavior-preserving

**Recommended seam:** house Layers B+C (`textureList_` + `gosTexture` + `createHardwareTexture`) into a single `GpuTextureManager` owned by `gosRenderer`, with the `gos_*` texture API as unchanged forwarders. Leave Layer D (`gl_utils.cpp`) and Layer A (`txmmgr.cpp` cache) untouched.

**Default-on & behavior-preserving: YES, trivially.** This is a pure mechanical re-housing — the same objects, same handle indices, same GL calls, same order. There is **no gate to flip**: it is not an alternate path, it is the *only* path, just relocated. (Contrast with the gpu-offload slices that ship a default-OFF killswitch around a *new* path.) Byte-identical output is the acceptance bar, not an A/B.

---

## Test / smoke gates that would PROVE safety

1. **tier1 5/5** (`mc2_01 03 10 17 24`, 30s) — exit 0. Catches mission-load + teardown.
2. **Byte-identical visual gate** — capture a golden set BEFORE the move, re-capture AFTER; require pixel-identical (the move changes no GL state). Mech (`mc2_24`) + terrain (`mc2_17`) baselines already exist under `tests/visual/baselines/`.
3. **`MC2_GL_DEBUG_FATAL=1`** clean run — no GL errors introduced by the re-housing (esp. around `createHardwareTexture` / BC7 / lock-unlock readback `GlPixelStoreGuard`).
4. **Cache-eviction stress (the load-bearing one):** force cache churn (low `cache_Threshold` / long mission) and confirm CACHED_OUT→resurrect still works. **This is the static-render-bug class** (`07a1f8ac` R2b lineage + the `CACHED_OUT_HANDLE` protocol): if the manager mishandles slot-null-on-delete vs index-reuse, evicted textures resurrect to the wrong slot → black/wrong textures. A `mc2_24` long-soak with texture-leak trace (`MC2_TXM_LEAK_TRACE`) + visual check is mandatory.
5. **Full GameOS relink** — the move is a class-layout + TU change; delete the `.obj` + `mc2.exe` (per CLAUDE.md full-relink rule).

---

## RISKS / caveats (carried forward + verified)

- **The seam is ~80% already done.** Handle indirection + M6 firewall already deliver the GL-only encapsulation *value*. The remaining 20% is "one named owner file" for Vulkan-port clarity. **This is the central GO-vs-DEFER tension.** It is churn-shaped unless explicitly justified as Vulkan-device-port prep.
- **Index stability is load-bearing.** `addTexture` indices are never reused; `deleteTexture` nulls in place. The cache layer (`txmmgr.cpp`) stores raw indices and resurrects via NEW indices. A `GpuTextureManager` MUST preserve "monotonic index, null-on-delete, no compaction" exactly, or the CACHED_OUT protocol + every `get_gosTextureHandle` consumer breaks.
- **Four constructor flavors** must all be covered: from-memory TGA (`:1007`), from-file (`:1007`, `is_from_memory=false`), prebuilt/BC7 (`:1069`), and empty/dynamic w-h (`:1038`, magenta-fallback path). Plus the inline `gos_NewCompressedTexture2D` glGen at `:8076`.
- **Lock/Unlock does GL readback+upload** (`gosTexture::Lock` `:1109` / `Unlock` `:1154`) with a `GlPixelStoreGuard` and BGRA swizzle — the mech-paint recolour path. The manager must keep Lock/Unlock as `gosTexture` methods (do not relocate the swizzle/guard logic).
- **`gl_utils.cpp` is shared** with RT/FBO/3D-texture code — do NOT pull it into the manager; the manager *calls* it.
- **Foreign-WIP collision:** `txmmgr.h` is dirty. Run `slice-preflight --symbols get_gosTextureHandle,gosTextureHandle,addTexture,deleteTexture,getTexture` before any future code slice; extract strictly on the `gameos_graphics.cpp` side.

---

## VERDICT

**GO — qualified.** A GL-only `GpuTextureManager` behind the unchanged `gos_*` API is **feasible, low-risk, and provably default-on/behavior-preserving** (answer to the key question = **YES**, with **no raw-GL leak** into game code). It is the correct *first* true backend ownership seam — most centralized, firewall-clean, sampler-separable.

**But it earns only modest keep.** The encapsulation it formalizes is already ~80% delivered by the existing handle indirection + firewall, so the slice is *consolidation for Vulkan-port clarity* (give the device-creation port one file to fork), not new capability. **Spend the relink only if the Vulkan arc is committed to actually forking a device layer next**; otherwise **DEFER** — banking this recon is the deliverable, and the contract-manifest arc slices remain the higher-value bankable work. Do **not** rubber-stamp it as a standalone "modernization" win; it is a deliberate down-payment on a port that must follow, or it is churn.

**Next if GO:** scope `GPU-TEXTURE-MANAGER-GL-ONLY-1` as a no-gate mechanical move (Layers B+C → new TU), acceptance = byte-identical tier1 + visual + GL-debug-fatal + cache-eviction soak, full GameOS relink.

---

### EXCLUSIONS (respected by this recon)
No Vulkan code, no descriptor abstraction, no sampler rewrite, no material refactor, no atlas changes, no handle-format change, no broad `RenderDevice`/`IRenderBackend` interface, no mech-import involvement.
