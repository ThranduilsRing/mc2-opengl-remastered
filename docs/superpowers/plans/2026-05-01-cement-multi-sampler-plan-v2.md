# Cement Multi-Sampler Implementation Plan v2.2 (Stage 4 — indirect-terrain PR2)

> **Revision history:**
> - **v2.0** stop-the-lined at [adversarial review](../specs/2026-05-01-cement-multi-sampler-plan-v2-review.md) (3 CRITICAL + 3 MAJOR + 5 MINOR) and advisor pass (2 CRITICAL + 3 MAJOR + 3 MINOR; one CRITICAL overlap).
> - **v2.1** addressed all v2.0 CRITICAL+MAJOR findings; stop-the-lined again at [v2.1 re-review](../specs/2026-05-01-cement-multi-sampler-plan-v2.1-review.md) for one new CRITICAL (`MC_MAXTEXTURES = 4096`, NOT 3000 — node-index space conflated with the resident-texture cap) + 2 MAJOR (counter explodes per-non-cement-quad; TES early-outs skip RecordIdx assignment) + 4 MINOR.
> - **v2.2** (this version) addresses all v2.1 findings inline. Net architectural changes vs v2.1: (a) cement node-index array sized `MC_MAXTEXTURES = 4096` (mclib/txmmgr.h:44); silent-skip becomes traced-skip; (b) `RecordIdx = 0u;` moved to TOP of TES `main()` so both `tessDebug.x < -2.5` and `tessDebug.x < -1.5` early-outs don't leave the varying undefined; (c) `g_cementPackUnmappedCount` only increments when the quad's `_wp0` material bytes are ALL Concrete (3) — i.e., the quad genuinely expects to find a cement layer.

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Verification Appendix at the end — every cited symbol grep-verified at write-time including its enclosing control flow AND the function signature/return type when the cited site is a function call (the V_L → V21 lesson learned).

**Goal:** Pure cement quads in the indirect-terrain SOLID draw render with their authored CONCRETE catalog texture (matching legacy M2's per-bucket binding). A cement-catalog atlas is bound at sampler unit 3 (`tex3`). The fragment shader selects the cement atlas vs the colormap atlas using `useCementAtlas` + the cement-layer-valid bit in `thinRecs[RecordIdx].control.w` (= `_pad0`). Default-on flip happens in this same commit (Q6 bundle): `MC2_TERRAIN_INDIRECT` becomes default-on; killswitch `=0` opt-out preserved.

**Architecture:** Multi-sampler approach (Q2 / option c). No `TerrainQuadRecipe` schema growth (frozen at 144 B / 9 vec4). Cement pixel data sourced via **GPU readback** from already-resident catalog textures (B1 = path (a)). Per-quad cement layer-index (low byte) plus a validity bit (bit 31) live in `TerrainQuadThinRecord._pad0`. The frag reads `_pad0` directly from the thin-record SSBO at binding 2, indexed by a new `flat uint RecordIdx` varying emitted by the thin VS (and by the legacy TES with default `0u` for linker compat). Multi-layer generalization slot is `_pad0` middle bits (8..30) reserved for future layers.

**Commit tag:** `PR2` (PR1 = `f221570`+`a29ff83`+`c8fa5df`, PR2 = this slice).

**Tech Stack:** GL 4.3 (SSBO/std430), C++17, MSVC RelWithDebInfo, AMD RX 7900 XTX driver rules.

---

## Source documents (read-first for executor)

- **Plan v2.0 review (subagent, code-grounded):** `docs/superpowers/specs/2026-05-01-cement-multi-sampler-plan-v2-review.md`
- **Plan v2.0 advisor pass:** in-conversation 2026-05-01 (filed in commit message of plan v2.1).
- **Plan v1 (superseded):** `docs/superpowers/plans/2026-05-01-cement-multi-sampler-plan.md`
- **v1 adversarial review:** `docs/superpowers/specs/2026-05-01-cement-multi-sampler-plan-review.md`
- **Brainstorm (Q1–Q9 frozen):** `docs/superpowers/brainstorms/2026-05-01-cement-multi-sampler-scope.md`
- **Parent plan:** `docs/superpowers/plans/2026-04-30-indirect-terrain-draw-plan.md`
- **Parent design:** `docs/superpowers/specs/2026-04-30-indirect-terrain-draw-design.md`
- **Memory:** `memory/water_ssbo_pattern.md`, `memory/gpu_direct_renderer_bringup_checklist.md`, `memory/mc2_texture_handle_is_live.md`, `memory/mc2_argb_packing.md`
- **Worktree CLAUDE.md** for build/deploy/smoke rules.

---

## Delta from v2.0 (preamble — read before any task)

| Topic | v2.0 | v2.1 |
|---|---|---|
| **Cement-layer map key (subagent C1-v2 / advisor C1)** | `g_cementLayerIndexBySlot[]` keyed by `textures[]` slot. Packer used `q.terrainHandle` as the slot — but `q.terrainHandle` is `mcTextureNodeIndex` (returned by `getTextureHandle` per [`mclib/terrtxm.h:281-288`](mclib/terrtxm.h:281)), NOT the slot. Lookup would miss for every cement quad. | Map renamed to `g_cementLayerIndexByNodeIdx[]`, sized `MC_MAX_TERRAIN_TXMS = 3000` (same upper bound for both indices). Atlas-build stores at `[peekTextureHandle(slot)]`; packer indexes with `q.terrainHandle` directly. No `isCement` call needed at packer — the map IS the cement filter. |
| **Layer-0 vs not-cement disambiguation (advisor C2)** | `_pad0 = layer_index_or_0`. Layer 0 indistinguishable from "not cement". Alpha-cement boundary fragments could interpolate `TerrainType ≥ 2.999` while `_pad0 = 0` → erroneously sample atlas slot 0 instead of staying on the colormap path. | `_pad0` bit 31 = `CEMENT_LAYER_VALID` (`0x80000000u`). Bits 7:0 = layer index. Bits 30:8 reserved for future layers. Frag gate becomes `useCementAtlas != 0 && (cementWord & 0x80000000u) != 0u` — TerrainType is no longer the only gate. |
| **Legacy frag varying source (subagent C2-v2)** | Plan added `flat out uint RecordIdx` to `gos_terrain.vert`. Wrong: legacy frag receives varyings from `gos_terrain.tese` (TES re-emits the bare frag-input names per [`shaders/gos_terrain.tese:11-16`](shaders/gos_terrain.tese:11)); TCS strips VS-only outputs. Plan B.2 was dead code on the legacy path. | Add `flat out uint RecordIdx; ... RecordIdx = 0u;` to `shaders/gos_terrain.tese` only. NO change to `gos_terrain.vert` or `.tesc`. |
| **`BuildCementCatalogAtlas` GL state hygiene (advisor M2)** | Saves unit-0 `GL_TEXTURE_BINDING_2D` and `GL_PACK_ALIGNMENT`; calls `glActiveTexture(GL_TEXTURE0)` but never restores `GL_ACTIVE_TEXTURE`. Mission-load state leak. | Save and restore `GL_ACTIVE_TEXTURE` via `glGetIntegerv(GL_ACTIVE_TEXTURE, ...)` at function entry/exit. Also save/restore `GL_UNPACK_ALIGNMENT` for hygiene around the upload. |
| **Mip strategy (advisor M3)** | Implicit: `GL_LINEAR` min/mag, no mipmaps. Could shimmer at distance/oblique angles. | Explicit no-mip choice documented in B.3 Step 2 comment: cement atlas has no inter-cell gutters, so `glGenerateMipmap` would bleed neighboring cells. Gate A expanded to include a near-airfield screenshot AND a distance/oblique screenshot — shimmer regression visible there. If shimmer fails Gate A, escalate to a per-cell mipmap pass with gutters in a follow-up slice (out of this slice's scope). |
| **Bridge accessor open ❓2 (subagent V6)** | `gos_terrain_bridge_glTextureForGosHandle` deferred to executor as ❓2 ("confirm implemented"). | Closed: implementation lives at [`GameOS/gameos/gameos_graphics.cpp:1775-1781`](GameOS/gameos/gameos_graphics.cpp:1775). Subagent V6 confirmed via grep. ❓2 retracted. |
| **Uniform-location lifecycle warning (subagent M2-v2)** | Bridge silently skips `glUniform1i` when `glGetUniformLocation` returns -1 (e.g., shader compile failure). | One-time `[TERRAIN_INDIRECT v1] event=cement_uniform_missing loc=<name>` print on first miss when `cementAtlasReady` is true. Surfaces silent shader-link failures. |
| **Per-quad cement variant (subagent M1-v2)** | Implicit: per-quad single-handle (matches legacy M2). Boundary mixed-cement quads sample only the primary corner's variant. | Documented in Out of scope: "Per-corner cement variant resolution — per-quad single-handle matches legacy M2 semantics; not a regression." |
| **255-cap silent truncation (advisor minor)** | `if (cementSlots.size() >= 255) break;` silently caps. | V27: encoding widened to 16 bits (max 65535); atlas budget cap raised to 1024 (gridSide=32, 2048×2048 = 16 MB). Trace warning `event=cement_catalog_truncated count=1024` printed on cap; Gate A treats this trace event as FAIL. |
| **Trace prefix versioning (advisor minor)** | All cement events use the existing `[TERRAIN_INDIRECT v1]` prefix to keep one grep pattern across the slice. | Kept at `v1`; the version covers the schema, not the slice number. Documented here. |
| **Lookup-miss instrumentation (subagent D3)** | None. C1-class bugs (every cement quad sampling slot 0) would only surface via visual inspection. | New per-mission counter: `cement_quad_packed_but_unmapped_count`. Printed in the `cement_catalog_built` event payload. Non-zero → known cement quad whose nodeIdx isn't in the atlas → bug. |
| **V21 retraction + V22..V25 added** | V21 conflated nodeIdx and slot. | V21 retracted. New entries V22 (nodeIdx-vs-slot semantics), V23 (validity-bit encoding), V24 (`GL_ACTIVE_TEXTURE` save/restore), V25 (TES is the legacy frag-input source) document the semantics that were missing. |

**Net surface area changed in v2.1 vs the legacy non-thin VS chain:** ONE new `flat out uint RecordIdx; RecordIdx = 0u;` declaration+assignment in `shaders/gos_terrain.tese` (not VS, not TCS). NO other legacy shader changes.

---

## User decisions (FROZEN — do not relitigate)

| Q | Decision | Source |
|---|---|---|
| Q1 | Pure cement only. Alpha-cement base correct on PR1; overlay stays legacy. | Brainstorm |
| Q2 | Single sampler at unit 3 (`tex3`), repurpose existing declaration. | Brainstorm |
| Q3 | Dynamic enumeration via `isCement()` walk over `[0, nextAvailable)`. | Brainstorm |
| Q4 | Per-quad cement layer-index in `TerrainQuadThinRecord._pad0` bits 15:0 (V27; was low byte / bits 7:0 in v2.1). | Brainstorm |
| Q5 | Measure at runtime; trace prints catalog count. | Brainstorm |
| Q6 | This slice IS Stage 4. Cement fix + default-on flip in one commit. | Brainstorm |
| Q7 | Visual canary at mc2_01 airport tarmac. | Brainstorm |
| Q8 | M2 fast-path safe via `useCementAtlas == 0` gate. | Brainstorm |
| Q9 | Atlas memory negligible (~1-2 MB). | Brainstorm |
| Q-B1 | **GPU readback** (path (a)) for cement pixel data. | This session |
| Q-B6 | **Frag-side SSBO fetch** (option c). RecordIdx travels via `flat uint` varying because `gl_PrimitiveID` restarts per sub-draw under `glMultiDrawArraysIndirect`. | This session |
| **Q-V** | **Validity bit at `_pad0` bit 31** for layer-0 disambiguation. Reserve bits 30:8 for future layers. | v2.1 (advisor C2) |

---

## Out of scope (explicit rejection list)

- Alpha-cement overlay path (overlay stays legacy).
- Runway markings / decals.
- `TerrainQuadRecipe` field growth (frozen at 144 B / 9 vec4; V_T).
- New sampler declaration in `gos_terrain.frag` (repurpose `tex3`; V_X / V15).
- M2 fast-path changes (Q8).
- `sampler2DArray` (Canary B not run; V_Z2).
- Per-bucket draw for cement quads (defeats indirect; not Option C).
- Baking cement art into `cpuColorMap` (Option B; not selected).
- More than one atlas layer in this slice (architecture generalizes via `_pad0` bits 30:8 + frag SSBO read).
- **Per-corner cement variant resolution** — per-quad single-handle matches legacy M2 semantics; mixed-cement quads sample the primary corner's variant. NOT a regression vs legacy.
- **Per-cell atlas mipmaps with gutters** — out of scope; if shimmer at distance fails Gate A, follow-up slice.
- Physical deletion of legacy SOLID path (post-soak follow-up).
- Mod content validation (`memory/feedback_offload_scope_stock_only.md`).
- **Atlas above 1024 layers** (gridSide > 32, atlas > 2048 px square, > 16 MB). _pad0 encoding (16 bits) supports 65535 layers, but the 1024 cap is the practical atlas-memory budget. If anyone hits 1024, revisit the atlas budget — don't blindly raise the cap.

---

## File structure (touched in this slice)

| File | Stage | Responsibility |
|---|---|---|
| `mclib/terrtxm.h` | A | Add public `getNextAvailableSlot()` accessor. |
| `mclib/quad.cpp` | A | Add `case 20:` to `terrainTypeToMaterial` Concrete group (B4 parity). |
| `GameOS/gameos/gos_terrain_indirect.cpp` | A, B, C | `BuildCementCatalogAtlas()` via GPU readback; `g_cementAtlas*` statics; `g_cementLayerIndexByNodeIdx[]`; thin-record `_pad0` populate (with validity bit); bridge accessors; reset hook; `terrainTypeToMaterialLocal` `case 20:` (B4 parity); `cement_quad_packed_but_unmapped_count` counter. Default-on flip in `IsEnabled()`. |
| `GameOS/gameos/gameos_graphics.cpp` | B | `gos_terrain_bridge_drawIndirect`: bind cement atlas at unit 3 (after `glBindSampler(3,0)`), set `useCementAtlas` + `atlasCementGridSide` + `atlasCementWorldUnitsPerTile` uniforms (lifecycle warning on missing locs), save/restore unit-3 binding + sampler. Reset `useCementAtlas` after draw. |
| `shaders/gos_terrain_thin.vert` | A | Add `flat out uint RecordIdx; ... RecordIdx = recordIdx;`. |
| `shaders/gos_terrain.tese` | A | Add matching `flat out uint RecordIdx; ... RecordIdx = 0u;` for linker compat with the legacy chain. **NOT** the VS, **NOT** the TCS — the legacy frag's varying source is the TES. |
| `shaders/gos_terrain.frag` | B | Declare thin-record SSBO at binding 2 (`readonly`); add `flat in uint RecordIdx`; add `uniform int useCementAtlas; uniform int atlasCementGridSide; uniform float atlasCementWorldUnitsPerTile;`. Cement-atlas branch overrides `texColor` when `useCementAtlas != 0` AND the validity bit is set in `thinRecs[RecordIdx].control.w`. |
| `memory/indirect_terrain_solid_endpoint.md` (NEW) | C | Slice closeout. |
| `memory/MEMORY.md` | C | Index entry for new memory. |

---

## Stage A: Catalog enumeration, atlas build via GPU readback, thin-record wiring

**Scope:** Add `getNextAvailableSlot()`. Build cement-catalog atlas at `BuildDenseRecipe()` time via GPU readback. Build dense `g_cementLayerIndexByNodeIdx[]` lookup. Populate `tr._pad0` (with validity bit) in `PackThinRecordsForFrame()`. Patch `case 20:` parity drift in both files. Add `RecordIdx` flat varying to thin VS.

### Task A.1 — Add `getNextAvailableSlot()` accessor (C2 / B2 fix)

**Files:** Modify `mclib/terrtxm.h:234`.

- [ ] **Step 1:** Open [`mclib/terrtxm.h`](mclib/terrtxm.h) and locate the public-section line `long getNumTypes() const { return numTypes; }` at line 234.

- [ ] **Step 2:** Insert immediately after it:

```cpp
// Public accessor for nextAvailable (protected static long, terrtxm.cpp:59).
// Used by gos_terrain_indirect::BuildCementCatalogAtlas to walk slots
// [0, getNextAvailableSlot()) and filter via isCement(slot).
long getNextAvailableSlot() const { return nextAvailable; }
```

- [ ] **Step 3:** Build:

```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -20
```

Expected: clean build.

- [ ] **Step 4:** Commit:

```bash
git add mclib/terrtxm.h
git commit -m "feat(terrtxm): public getNextAvailableSlot() accessor for cement atlas walk"
```

### Task A.1.bis — Patch terrainType-20 → Concrete in BOTH map files (B4)

**Files:** Modify `mclib/quad.cpp:142-145` and `GameOS/gameos/gos_terrain_indirect.cpp:244-245`.

- [ ] **Step 1:** In [`mclib/quad.cpp:142-145`](mclib/quad.cpp:142):

Existing:
```cpp
        case 10: // Concrete
        case 13: case 14: case 15: case 16: // Cement 2-5
        case 17: case 18: case 19:          // Cement 6-8
            return 3; // Concrete
```

Replace with:
```cpp
        case 10: // Concrete
        case 13: case 14: case 15: case 16: // Cement 2-5
        case 17: case 18: case 19:          // Cement 6-8
        case 20:                            // END_CEMENT_TYPE (terrtxm.h:44)
            return 3; // Concrete
```

- [ ] **Step 2:** In [`GameOS/gameos/gos_terrain_indirect.cpp:244-245`](GameOS/gameos/gos_terrain_indirect.cpp:244):

Existing:
```cpp
        case 10: case 13: case 14: case 15: case 16:
        case 17: case 18: case 19:                    return 3; // Concrete
```

Replace with:
```cpp
        case 10: case 13: case 14: case 15: case 16:
        case 17: case 18: case 19: case 20:           return 3; // Concrete
```

- [ ] **Step 3:** Build, then commit (one bundled commit; the two files describe the same conceptual fix and `_wp0` parity check requires both at once):

```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
git add mclib/quad.cpp GameOS/gameos/gos_terrain_indirect.cpp
git commit -m "fix(terrain): map END_CEMENT_TYPE(20) to Concrete material in both type-map sites"
```

### Task A.2 — Add cement-atlas statics and reset hook in `gos_terrain_indirect.cpp`

**Files:** Modify [`GameOS/gameos/gos_terrain_indirect.cpp`](GameOS/gameos/gos_terrain_indirect.cpp).

- [ ] **Step 1:** Inside the existing anonymous namespace at line 370 (right after the `g_atlasGLTex` block ending at line 377), insert:

```cpp
// ---------------------------------------------------------------------------
// Cement catalog atlas — single GL_TEXTURE_2D, packed grid of N cement tile
// textures.  Built once per mission at BuildDenseRecipe() time via GPU
// readback from already-resident catalog textures (textureData[0] in
// tileRAMHeap is dead in stock gameplay — quickLoad gates the RAM path at
// terrtxm.cpp:561).  Bound at unit 3 by gos_terrain_bridge_drawIndirect.
//
// LAYER MAP: keyed by mcTextureNodeIndex (NOT textures[] slot).
//   q.terrainHandle returned by quad.cpp:546 (getTextureHandle) is the
//   nodeIdx, NOT the slot — see V22 in Verification Appendix.
// ---------------------------------------------------------------------------
static GLuint  g_cementAtlasGLTex          = 0;
static int     g_cementAtlasGridSide       = 0;   // cells per row/col (power of 2)
static int     g_cementAtlasTileCount      = 0;   // distinct cement entries enumerated
static bool    g_cementLayerMapReady       = false;
static int     g_cementCatalogTruncated    = 0;   // 1 if N>=1024 cap hit (Gate A FAIL); was 255 pre-V27

// Dense lookup: mcTextureNodeIndex → atlas layer-index (0..N-1).
// Sized MC_MAXTEXTURES = 4096 (mclib/txmmgr.h:44) — node-index space.
// NOT MC_MAX_TERRAIN_TXMS (3000): that's the textures[] slot cap, a different
// space.  NOT MAX_MC2_GOS_TEXTURES (3000): that's the resident-cap usage
// counter.  Memory note `texture_handle_cap.md` documents the gosHandle
// space; nodeIdx is the masterTextureNodes[] index, sized MC_MAXTEXTURES.
// 0xFFFF = "not cement / not in atlas".
//
// REQUIRES: #include "mclib/txmmgr.h" at the top of this file (or a fwd-
// constant declaration).  If not already included, add it in the headers
// section before this static.
static uint16_t g_cementLayerIndexByNodeIdx[MC_MAXTEXTURES];

// Per-frame counter — incremented when the packer sees a quad whose
// q.terrainHandle is non-zero AND maps to no cement layer.  A non-zero count
// after Stage A.4 is wired indicates an enumeration miss (debug discipline).
static uint32_t g_cementPackUnmappedCount = 0;
```

- [ ] **Step 2:** Locate `ResetDenseRecipe()` at [`gos_terrain_indirect.cpp:479`](GameOS/gameos/gos_terrain_indirect.cpp:479). After the existing `g_atlasGLTex` teardown (lines 497-501), add:

```cpp
// Cement catalog atlas teardown — mirror g_atlasGLTex pattern.
if (g_cementAtlasGLTex != 0) {
    glDeleteTextures(1, &g_cementAtlasGLTex);
    g_cementAtlasGLTex = 0;
}
g_cementAtlasGridSide    = 0;
g_cementAtlasTileCount   = 0;
g_cementLayerMapReady    = false;
g_cementCatalogTruncated = 0;
g_cementPackUnmappedCount = 0;
memset(g_cementLayerIndexByNodeIdx, 0xFF, sizeof(g_cementLayerIndexByNodeIdx));

if (traceOn()) {
    printf("[TERRAIN_INDIRECT v1] event=cement_catalog_reset\n");
    fflush(stdout);
}
```

- [ ] **Step 3:** Build, commit:

```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
git add GameOS/gameos/gos_terrain_indirect.cpp
git commit -m "feat(terrain-indirect): cement-atlas static storage + reset hook (nodeIdx-keyed)"
```

### Task A.3 — Implement `BuildCementCatalogAtlas()` via GPU readback

**Files:** Modify [`GameOS/gameos/gos_terrain_indirect.cpp`](GameOS/gameos/gos_terrain_indirect.cpp).

- [ ] **Step 1:** Add an `extern` declaration inside the anonymous namespace, after the static declarations from Task A.2:

```cpp
// Bridge accessor: gosHandle → GLuint texture name.  Implemented at
// GameOS/gameos/gameos_graphics.cpp:1775-1781 (subagent V6).
// Declared in gos_terrain_bridge.h:47.
extern unsigned int gos_terrain_bridge_glTextureForGosHandle(unsigned int gosHandle);
```

- [ ] **Step 2:** Insert the function after `BuildColormapAtlas()` (between its closing brace at line 419 and the closing brace of the anonymous namespace at line 421):

```cpp
// BuildCementCatalogAtlas — GPU readback path (B1 of plan v2.1).
// Runs at BuildDenseRecipe() time.  Walks textures[0..nextAvailable-1],
// filters by isCement(slot), reads each cement tile via glGetTexImage,
// blits into a packed grid atlas, uploads as a single GL_TEXTURE_2D.
//
// quickLoad-safe: does not depend on tileRAMHeap textureData[0] (which
// is NULL in stock gameplay per terrtxm.cpp:561 enclosing gate).
//
// NO MIPMAPS: cement atlas cells are packed without inter-cell gutters,
// so glGenerateMipmap would bleed neighboring cells.  Sampler is GL_LINEAR
// min/mag; potential shimmer at distance/oblique angles is accepted —
// Gate A includes a distance/oblique screenshot to surface this.  Per-cell
// mip generation with gutters is a follow-up slice.
void BuildCementCatalogAtlas() {
    ZoneScopedN("Terrain::IndirectCementAtlasUpload");

    if (!Terrain::terrainTextures) {
        if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_terrainTextures\n");
        return;
    }
    auto* tt = Terrain::terrainTextures;

    const int txmSize = TERRAIN_TXM_SIZE;  // extern int, typically 64 (terrtxm.cpp:51)
    const long lastSlot = tt->getNextAvailableSlot();
    if (lastSlot <= 0) {
        if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_slots\n");
        return;
    }

    // Pass 1: enumerate cement slots, resolve each to (nodeIdx, GLuint).
    std::vector<DWORD>  cementNodeIndices;  // values for the layer map (KEY)
    std::vector<GLuint> cementGLTextures;   // resolved GL texture names (per-tile readback source)
    cementNodeIndices.reserve(64);
    cementGLTextures.reserve(64);
    bool truncated = false;

    for (long slot = 0; slot < lastSlot; ++slot) {
        if (!tt->isCement((DWORD)slot)) continue;
        const DWORD nodeIdx = tt->peekTextureHandle((DWORD)slot);
        if (nodeIdx == 0xffffffffu) continue;
        if (nodeIdx >= (DWORD)MC_MAXTEXTURES) {
            // Out-of-range nodeIdx: not silently ignored.  Surfaces a real bug
            // (heap corruption / out-of-bounds nodeIdx) rather than producing
            // a "cement renders as colormap" symptom with no log signal.
            if (traceOn()) {
                printf("[TERRAIN_INDIRECT v1] event=cement_atlas_nodeidx_oob "
                       "slot=%ld nodeIdx=%u cap=%d\n",
                       slot, (unsigned)nodeIdx, (int)MC_MAXTEXTURES);
                fflush(stdout);
            }
            continue;
        }
        const DWORD gosHandle = tex_resolve(nodeIdx);
        if (gosHandle == 0u || gosHandle == (DWORD)INVALID_TEXTURE_ID) continue;
        const GLuint glTex = gos_terrain_bridge_glTextureForGosHandle((unsigned)gosHandle);
        if (glTex == 0) continue;
        cementNodeIndices.push_back(nodeIdx);
        cementGLTextures.push_back(glTex);
        // 1024 cap (V27): atlas-budget cap (gridSide=32, 2048×2048 = 16 MB).
        // _pad0 layer-index lives in bits 15:0 (encoding cap = 65535);
        // CEMENT_LAYER_VALID takes bit 31; bits 30:16 reserved for future layers.
        if (cementNodeIndices.size() >= 1024) { truncated = true; break; }
    }

    const int N = (int)cementNodeIndices.size();
    if (N == 0) {
        if (traceOn()) printf("[TERRAIN_INDIRECT v1] event=cement_atlas_skip reason=no_cement_tiles count=0\n");
        return;
    }

    // Build nodeIdx → layer-index map.
    memset(g_cementLayerIndexByNodeIdx, 0xFF, sizeof(g_cementLayerIndexByNodeIdx));
    for (int k = 0; k < N; ++k) {
        g_cementLayerIndexByNodeIdx[cementNodeIndices[k]] = (uint16_t)k;
    }

    // Grid: smallest power-of-2 side fitting N cells in a square.
    int gridSide = 1;
    while (gridSide * gridSide < N) gridSide <<= 1;
    const int atlasPixelSide = gridSide * txmSize;

    // CPU buffers.  BGRA8 matches BuildColormapAtlas precedent at line 398.
    std::vector<uint32_t> atlasBuf((size_t)atlasPixelSide * atlasPixelSide, 0u);
    std::vector<uint32_t> tileBuf((size_t)txmSize * txmSize, 0u);

    // Save GL state (V24): GL_ACTIVE_TEXTURE, unit-0 GL_TEXTURE_BINDING_2D,
    // GL_PACK_ALIGNMENT, GL_UNPACK_ALIGNMENT.
    GLint savedActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActive);
    glActiveTexture(GL_TEXTURE0);
    GLint savedTex0Binding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex0Binding);
    GLint savedPackAlign = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &savedPackAlign);
    GLint savedUnpackAlign = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedUnpackAlign);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    for (int k = 0; k < N; ++k) {
        glBindTexture(GL_TEXTURE_2D, cementGLTextures[k]);

        // Readback mip 0.  Cement tiles are uncompressed RGBA8 (verified via
        // gl_utils.cpp:53-58,165-184 path); BGRA matches storage.
        // glGetTexImage stalls until prior GL work completes — acceptable at
        // mission load (one-time, ~9-30 tiles).
        glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, tileBuf.data());

        const int col  = k % gridSide;
        const int row  = k / gridSide;
        const int dstX = col * txmSize;
        const int dstY = row * txmSize;
        for (int py = 0; py < txmSize; ++py) {
            const uint32_t* srcRow = &tileBuf[(size_t)py * txmSize];
            uint32_t*       dstRow = &atlasBuf[(size_t)(dstY + py) * atlasPixelSide + dstX];
            memcpy(dstRow, srcRow, (size_t)txmSize * sizeof(uint32_t));
        }
    }

    // Upload packed atlas at unit 0 (will restore unit-0 binding below).
    if (g_cementAtlasGLTex == 0) glGenTextures(1, &g_cementAtlasGLTex);
    glBindTexture(GL_TEXTURE_2D, g_cementAtlasGLTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 atlasPixelSide, atlasPixelSide, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, atlasBuf.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // no mips — see header comment
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);   // contract: bridge clears unit-3 sampler
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // Restore state (V24).
    glPixelStorei(GL_PACK_ALIGNMENT, savedPackAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex0Binding);
    glActiveTexture((GLenum)savedActive);

    g_cementAtlasGridSide    = gridSide;
    g_cementAtlasTileCount   = N;
    g_cementCatalogTruncated = truncated ? 1 : 0;
    g_cementLayerMapReady    = true;

    if (traceOn()) {
        printf("[TERRAIN_INDIRECT v1] event=cement_catalog_built tile_count=%d "
               "atlas_size=%dx%d grid_side=%d gltex=%u truncated=%d "
               "unmapped_pack_count=%u\n",
               N, atlasPixelSide, atlasPixelSide, gridSide,
               (unsigned)g_cementAtlasGLTex,
               g_cementCatalogTruncated,
               g_cementPackUnmappedCount);
        if (truncated) {
            printf("[TERRAIN_INDIRECT v1] event=cement_catalog_truncated count=1024\n");
        }
        fflush(stdout);
    }
}
```

- [ ] **Step 3:** Add bridge accessors after the existing `BuildColormapAtlas` accessors at line 428 (just before `// Stage 2 public API`):

```cpp
GLuint gos_terrain_indirect_getCementAtlasGLTex()    { return g_cementAtlasGLTex; }
int    gos_terrain_indirect_getCementAtlasGridSide() { return g_cementAtlasGridSide; }
bool   gos_terrain_indirect_isCementAtlasReady()     { return g_cementLayerMapReady && g_cementAtlasGLTex != 0; }
```

- [ ] **Step 4:** Wire `BuildCementCatalogAtlas()` into `BuildDenseRecipe()` at [`gos_terrain_indirect.cpp:476`](GameOS/gameos/gos_terrain_indirect.cpp:476) (immediately after the existing `BuildColormapAtlas();` call):

```cpp
// Build cement catalog atlas via GPU readback (textureData[0] is NULL in
// stock gameplay; see plan v2.1 §C1/B1).
BuildCementCatalogAtlas();
```

- [ ] **Step 5:** Build, smoke (trace only — frag not yet wired):

```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
MC2_TERRAIN_INDIRECT=1 MC2_TERRAIN_INDIRECT_TRACE=1 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 20 --keep-logs
```

Expected: in mc2_01 (airport) trace, an `event=cement_catalog_built tile_count=N` line where `N >= 1` and `truncated=0`. If `N == 0` on mc2_01, GPU-readback resolved zero cement gosHandles — investigate `tex_resolve` / `gos_terrain_bridge_glTextureForGosHandle` results before proceeding. Other missions may print `cement_atlas_skip reason=no_cement_tiles` (legitimate for non-cement biomes).

- [ ] **Step 6:** Commit:

```bash
git add GameOS/gameos/gos_terrain_indirect.cpp
git commit -m "feat(terrain-indirect): cement-catalog atlas build via GPU readback (nodeIdx-keyed)"
```

### Task A.4 — Populate `tr._pad0` (with validity bit) in `PackThinRecordsForFrame()`

**Files:** Modify [`GameOS/gameos/gos_terrain_indirect.cpp:991`](GameOS/gameos/gos_terrain_indirect.cpp:991).

- [ ] **Step 1:** Locate the line `tr._pad0         = 0u;` at line 991.

- [ ] **Step 2:** Replace with:

```cpp
// Cement layer-index lookup, keyed by mcTextureNodeIndex (NOT slot).
// q.terrainHandle is the un-resolved nodeIdx returned by getTextureHandle
// at quad.cpp:546 (V22).  The map g_cementLayerIndexByNodeIdx is populated
// at atlas-build time keyed by nodeIdx, so direct indexing is correct.
//
// Encoding (V23, widened in V27):
//   bit 31     = CEMENT_LAYER_VALID — disambiguates "layer 0" from "not cement"
//   bits 30:16 = reserved for future layers (decals, scorch)
//   bits 15:0  = cement atlas layer index (0..65535 encoding cap;
//                practically capped at 1024 by atlas budget)
constexpr uint32_t kCementLayerValidBit = 0x80000000u;
uint32_t cementWord = 0u;
if (g_cementLayerMapReady) {
    const DWORD nodeIdx = (DWORD)q.terrainHandle;
    if (nodeIdx < (DWORD)MC_MAXTEXTURES) {
        const uint16_t idx = g_cementLayerIndexByNodeIdx[nodeIdx];
        if (idx != 0xFFFFu) {
            cementWord = kCementLayerValidBit | ((uint32_t)idx & 0xFFFFu);
        } else {
            // Lifecycle counter: only count quads that EXPECT a cement layer
            // (i.e., all 4 corner materials in recipe._wp0 are Concrete=3).
            // The recipe was built by buildRecipeSlot at primeMissionTerrainCache
            // time and packs 4×8-bit material indices into _wp0
            // (gos_terrain_indirect.cpp:340-346).  Reading here is safe — the
            // recipe is mission-stable.  Without this gate the counter
            // explodes per-non-cement-quad and Gate A's "low count"
            // criterion is meaningless (subagent v2.1 MAJOR).
            if (rec) {
                uint32_t tpacked = 0u;
                memcpy(&tpacked, &rec->_wp0, 4);
                const bool allConcrete =
                    ((tpacked        & 0xFFu) == 3u) &&
                    (((tpacked >> 8) & 0xFFu) == 3u) &&
                    (((tpacked >>16) & 0xFFu) == 3u) &&
                    (((tpacked >>24) & 0xFFu) == 3u);
                if (allConcrete) ++g_cementPackUnmappedCount;
            }
        }
    }
}
tr._pad0         = cementWord;
```

- [ ] **Step 3:** Build, smoke, confirm trace:

```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
MC2_TERRAIN_INDIRECT=1 MC2_TERRAIN_INDIRECT_TRACE=1 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 15 --keep-logs
```

Verify: `cement_catalog_built ... unmapped_pack_count=<N>` where N is small or zero. Large N → enumeration miss.

- [ ] **Step 4:** Commit:

```bash
git add GameOS/gameos/gos_terrain_indirect.cpp
git commit -m "feat(terrain-indirect): _pad0 carries cement layer-index + validity bit (nodeIdx lookup)"
```

### Task A.5 — Thin VS: add `flat out uint RecordIdx`

**Files:** Modify [`shaders/gos_terrain_thin.vert`](shaders/gos_terrain_thin.vert).

- [ ] **Step 1:** Locate the `out` declarations block at lines 27-35. After `out float UndisplacedDepth;` at line 35, add:

```glsl
flat out uint RecordIdx;  // index into thinRecs[] — frag reads thinRecs[RecordIdx]._pad0
                          // for cement layer-index + validity bit when useCementAtlas != 0.
                          // Legacy chain emits the matching declaration in gos_terrain.tese
                          // (NOT gos_terrain.vert — TCS strips VS outputs not consumed).
```

- [ ] **Step 2:** At the end of `main()` (after `UndisplacedDepth = ...` at line 169):

```glsl
RecordIdx = recordIdx;
```

- [ ] **Step 3:** Build, commit:

```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
git add shaders/gos_terrain_thin.vert
git commit -m "shader(thin-vs): emit flat RecordIdx varying for frag-side cement layer lookup"
```

---

## Stage B: Legacy TES varying, frag SSBO read, bridge bind

### Task B.1 — Legacy TES: add matching `flat out uint RecordIdx`

**Files:** Modify [`shaders/gos_terrain.tese`](shaders/gos_terrain.tese).

- [ ] **Step 1:** Open `shaders/gos_terrain.tese`. Locate the `out` block at lines 11-16 (currently emits `Color`, `Texcoord`, `TerrainType`, `WorldNorm`, `WorldPos`, `UndisplacedDepth`).

- [ ] **Step 2:** Add immediately after `out float UndisplacedDepth;` (line 16):

```glsl
flat out uint RecordIdx;  // matches gos_terrain_thin.vert; legacy chain has no thin-record
                          // context, so emit a constant 0u.  Frag's cement-atlas branch
                          // is gated on useCementAtlas != 0 which the legacy bridge never
                          // sets, so the SSBO read is dead on the legacy path.
```

- [ ] **Step 3:** In `main()`, set `RecordIdx = 0u;` AT THE TOP of the function, BEFORE either of the `tessDebug.x < -2.5` (line ~38) or `tessDebug.x < -1.5` (line ~74) early-out paths — both of those `return` without assigning per-fragment outputs. Place the assignment immediately after the existing `vec3 bary = gl_TessCoord;` line (~line 36):

```glsl
RecordIdx = 0u;  // legacy chain has no thin-record context; set BEFORE early-outs.
```

Verify after editing: grep `RecordIdx` in the TES file should show exactly two matches — the `out` declaration and the assignment near the top of `main`. There should be NO `RecordIdx` assignments inside or after either `tessDebug` early-out block.

- [ ] **Step 4:** Verify NO modification to `shaders/gos_terrain.vert` or `shaders/gos_terrain.tesc`. Grep to confirm:

```bash
grep -n "RecordIdx" A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_terrain.vert A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_terrain.tesc
```

Expected: zero matches in either file.

- [ ] **Step 5:** Build, then smoke the LEGACY chain (this verifies no shader-link regression):

```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
MC2_TERRAIN_INDIRECT=0 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 15 --keep-logs
```

Expected: tier1 5/5 PASS under the legacy path. "Transparent terrain through to skybox" symptom = silent linker fail on the legacy chain — re-verify that `RecordIdx` is in the TES `out` block AND `main()`.

- [ ] **Step 6:** Commit:

```bash
git add shaders/gos_terrain.tese
git commit -m "shader(legacy-tese): emit RecordIdx=0u for frag linker compat with thin chain"
```

### Task B.2 — Frag: declare thin-record SSBO + cement-atlas branch (with validity bit)

**Files:** Modify [`shaders/gos_terrain.frag`](shaders/gos_terrain.frag).

- [ ] **Step 1:** After `uniform sampler2D tex3;` at [`gos_terrain.frag:35`](shaders/gos_terrain.frag:35), update the comment:

```glsl
uniform sampler2D tex3;  // cement-catalog atlas (was: legacy detail displacement, unused with per-material POM).
                         // Bound at unit 3 by gos_terrain_bridge_drawIndirect when useCementAtlas != 0.
                         // Texture-object wrap = GL_REPEAT; the bridge clears unit-3 sampler with
                         // glBindSampler(3, 0) so the texture-object wrap is the contract.
                         // No mipmaps (cement atlas cells lack gutters; bleed risk).
```

- [ ] **Step 2:** After the existing `useAtlasColormap` uniform block (around line 60), add:

```glsl
// --- Cement catalog atlas (Stage 4 / PR2) ---------------------------------
// Bound at sampler unit 3 (tex3) by the indirect bridge.  Per-quad layer
// index + validity bit live in TerrainQuadThinRecord._pad0 (control.w),
// read from binding-2 SSBO indexed by the flat RecordIdx varying.
//
// _pad0 encoding (plan v2.1 V23, widened in V27):
//   bit 31     = CEMENT_LAYER_VALID
//   bits 30:16 = reserved for future layers
//   bits 15:0  = cement atlas layer index (0..65535 encoding cap;
//                practically capped at 1024 by atlas budget)
//
// C++ struct: GameOS/gameos/gos_terrain_patch_stream.h:103-111 (8×uint32, 32 B).
// std430 packs identically to 2×uvec4.  control.x=recipeIdx, .y=terrainHandle,
// .z=flags, .w=_pad0.  lightRGBs.{x,y,z,w}=lightRGB{0..3} (BGRA-packed).
uniform int   useCementAtlas;          // 0 = M2 / legacy, 1 = indirect cement-armed
uniform int   atlasCementGridSide;     // cells per row/col of cement atlas
uniform float atlasCementWorldUnitsPerTile;  // = Terrain::worldUnitsPerVertex (128.0)

flat in uint RecordIdx;

struct TerrainQuadThinRecord_Frag {
    uvec4 control;    // x=recipeIdx, y=terrainHandle, z=flags, w=_pad0(cement word)
    uvec4 lightRGBs;
};
layout(std430, binding = 2) readonly buffer ThinRecordBufFrag {
    TerrainQuadThinRecord_Frag thinRecsFrag[];
};
```

- [ ] **Step 3:** Locate the `texColor` assignment block at [`gos_terrain.frag:230-237`](shaders/gos_terrain.frag:230). Existing:

```glsl
PREC vec2 colormapUV;
if (useAtlasColormap != 0) {
    colormapUV.x = (WorldPos.x - atlasMapTopLeftX) * atlasOneOverWorldUnits;
    colormapUV.y = (atlasMapTopLeftY - WorldPos.y) * atlasOneOverWorldUnits;
} else {
    colormapUV = Texcoord;
}
PREC vec4 texColor = texture(tex1, colormapUV);
```

Replace with:

```glsl
PREC vec2 colormapUV;
if (useAtlasColormap != 0) {
    colormapUV.x = (WorldPos.x - atlasMapTopLeftX) * atlasOneOverWorldUnits;
    colormapUV.y = (atlasMapTopLeftY - WorldPos.y) * atlasOneOverWorldUnits;
} else {
    colormapUV = Texcoord;
}
PREC vec4 texColor = texture(tex1, colormapUV);

// Cement-catalog override: gated on useCementAtlas (M2/legacy never set this)
// AND the validity bit in the per-quad cement word.  TerrainType is NOT a
// gate — alpha-cement boundary fragments can interpolate TerrainType near 3.0
// while their _pad0 has no validity bit set, and we must NOT sample cement[0]
// for those (advisor C2).  When validity bit is set, the quad is genuinely
// pure-cement and TerrainType is exactly 3.0 across all corners.
if (useCementAtlas != 0) {
    uint cementWord  = thinRecsFrag[RecordIdx].control.w;
    bool cementValid = (cementWord & 0x80000000u) != 0u;
    if (cementValid) {
        uint layerIdx = cementWord & 0xFFFFu;  // V27: was 0xFFu pre-widening
        int  gridSide = atlasCementGridSide;
        if (gridSide < 1) gridSide = 1;
        int  cCol = int(layerIdx) % gridSide;
        int  cRow = int(layerIdx) / gridSide;
        PREC vec2 cTileUV = fract(vec2(WorldPos.x, -WorldPos.y) / atlasCementWorldUnitsPerTile);
        PREC vec2 cAtlasUV = (vec2(float(cCol), float(cRow)) + cTileUV) / float(gridSide);
        texColor = texture(tex3, cAtlasUV);
    }
}
```

- [ ] **Step 4:** Build, commit:

```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
git add shaders/gos_terrain.frag
git commit -m "shader(terrain-frag): cement-atlas branch via SSBO _pad0 read with validity-bit gate"
```

### Task B.3 — Bridge: bind cement atlas at unit 3, set uniforms, save/restore

**Files:** Modify [`GameOS/gameos/gameos_graphics.cpp`](GameOS/gameos/gameos_graphics.cpp).

- [ ] **Step 1:** In `gos_terrain_bridge_drawIndirect()` near the existing colormap-atlas externs at line 2295, add:

```cpp
extern GLuint gos_terrain_indirect_getCementAtlasGLTex();
extern int    gos_terrain_indirect_getCementAtlasGridSide();
extern bool   gos_terrain_indirect_isCementAtlasReady();
```

- [ ] **Step 2:** After the existing colormap-atlas bind block ends at [`gameos_graphics.cpp:2323`](GameOS/gameos/gameos_graphics.cpp:2323), add:

```cpp
// ---- Bind cement catalog atlas at unit 3 (Stage 4 / PR2) ---------------
// Sampler-object override safety: clear unit 3's sampler with
// glBindSampler(3, 0) so the texture-object wrap (GL_REPEAT) is the binding
// (M1/B3 of plan v2.1).
GLint  savedTex3Binding = 0;
GLuint savedTex3Sampler = 0;
const bool cementAtlasReady = gos_terrain_indirect_isCementAtlasReady();
if (cementAtlasReady) {
    glActiveTexture(GL_TEXTURE3);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex3Binding);
    { GLint q = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 3, &q); savedTex3Sampler = (GLuint)q; }
    glBindSampler(3, 0);
    glBindTexture(GL_TEXTURE_2D, gos_terrain_indirect_getCementAtlasGLTex());
    glActiveTexture(GL_TEXTURE0);
}

{
    const GLint locTex3   = glGetUniformLocation(prog, "tex3");
    const GLint locUCA    = glGetUniformLocation(prog, "useCementAtlas");
    const GLint locGSide  = glGetUniformLocation(prog, "atlasCementGridSide");
    const GLint locWUPT   = glGetUniformLocation(prog, "atlasCementWorldUnitsPerTile");

    // Lifecycle warning (subagent M2-v2): if any uniform location is missing
    // when the atlas is ready, the frag failed to compile with the new
    // uniforms — print once per process so the failure is visible in the log.
    static bool s_warnedCementUniforms = false;
    if (cementAtlasReady && !s_warnedCementUniforms &&
        (locTex3 < 0 || locUCA < 0 || locGSide < 0 || locWUPT < 0)) {
        printf("[TERRAIN_INDIRECT v1] event=cement_uniform_missing "
               "tex3=%d useCementAtlas=%d gridSide=%d wupt=%d\n",
               locTex3, locUCA, locGSide, locWUPT);
        fflush(stdout);
        s_warnedCementUniforms = true;
    }

    if (locTex3  >= 0) glUniform1i(locTex3, 3);
    if (cementAtlasReady) {
        if (locUCA   >= 0) glUniform1i(locUCA,   1);
        if (locGSide >= 0) glUniform1i(locGSide, gos_terrain_indirect_getCementAtlasGridSide());
        if (locWUPT  >= 0) glUniform1f(locWUPT,  Terrain::worldUnitsPerVertex);
    } else {
        if (locUCA   >= 0) glUniform1i(locUCA,   0);
    }
}
```

- [ ] **Step 3:** After the existing `useAtlasColormap` reset at [`gameos_graphics.cpp:2364-2367`](GameOS/gameos/gameos_graphics.cpp:2364), add:

```cpp
// Reset useCementAtlas so the M2 fast path doesn't inherit the cement flag.
{
    const GLint locUCA = glGetUniformLocation(prog, "useCementAtlas");
    if (locUCA >= 0) glUniform1i(locUCA, 0);
}
// Restore unit 3 (texture binding + sampler).
if (cementAtlasReady) {
    glActiveTexture(GL_TEXTURE3);
    glBindSampler(3, savedTex3Sampler);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex3Binding);
    glActiveTexture(GL_TEXTURE0);
}
```

- [ ] **Step 4:** Build, deploy, smoke mc2_01 with indirect ON:

```bash
cmake --build build64 --config RelWithDebInfo --target mc2 2>&1 | tail -10
sh .claude/skills/mc2-deploy.md  # or per-file cp -f + diff -q per CLAUDE.md
MC2_TERRAIN_INDIRECT=1 MC2_TERRAIN_INDIRECT_TRACE=1 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 25 --keep-logs
```

Expected: mc2_01 trace shows `cement_catalog_built tile_count >= 1`, `unmapped_pack_count=0` (or small), no `cement_uniform_missing` event. Visual: airport tarmac renders concrete tiles. Failure modes per Gate A acceptance criteria.

- [ ] **Step 5:** Side-by-side compare with legacy:

```bash
MC2_TERRAIN_INDIRECT=0 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 25 --keep-logs
```

- [ ] **Step 6:** Commit:

```bash
git add GameOS/gameos/gameos_graphics.cpp
git commit -m "bridge(indirect): bind cement atlas at unit 3 with sampler clear + state restore + uniform-loc lifecycle warn"
```

---

## Stage C: Default-on flip + 5-gate validation + slice closeout

### Task C.1 — Flip `IsEnabled()` to default-on

**Files:** Modify [`GameOS/gameos/gos_terrain_indirect.cpp:40-46`](GameOS/gameos/gos_terrain_indirect.cpp:40).

- [ ] **Step 1:** Replace `IsEnabled()` body:

Existing:
```cpp
bool IsEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return s;
}
```

Replace with:
```cpp
bool IsEnabled() {
    static const bool s = []() {
        const char* v = getenv("MC2_TERRAIN_INDIRECT");
        // Stage 4 default-on: literal "0" opts out; absent or anything else = on.
        if (v && v[0] == '0' && v[1] == '\0') return false;
        return true;
    }();
    return s;
}
```

- [ ] **Step 2:** Update `gos_terrain_indirect.h:56-57` comment to reflect "Stage 4 flip shipped (commit hash filled at C.7)".

- [ ] **Step 3:** Build (commit folded into C.7).

### Task C.2 — Gate A: visual canary at mc2_01 airport tarmac (NEAR + DISTANCE)

**Acceptance criteria:**

- Default-on (`MC2_TERRAIN_INDIRECT` unset OR `=1`) renders mc2_01 starting-area tarmac (large grey/tan rectangular concrete-tile pattern, ~200-400 units N of starting position) with visible concrete tile texture.
- **Near screenshot:** side-by-side at fixed tarmac camera vs `MC2_TERRAIN_INDIRECT=0` (legacy M2): perceptual diff ≤5%. No grass-bleed, no uniform-grey wash, no obvious tile misalignment.
- **Distance/oblique screenshot (advisor M3):** zoom out to RTS distance OR rotate camera oblique to the tarmac. Compare indirect vs legacy. Shimmer/aliasing is acceptable IF it matches legacy's behavior. If indirect is visibly worse than legacy (additional shimmer that legacy doesn't have), Gate A FAILS — escalate to per-cell mip+gutter follow-up.
- **Trace check:** `cement_catalog_built ... truncated=0 unmapped_pack_count=<low>` AND no `cement_uniform_missing` events in the run.
- Specific failure modes that = FAIL:
  - Grass/dirt visible through tarmac → atlas not bound or `useCementAtlas` not set.
  - Uniform grey, no tile pattern → atlas UV math broken (likely `worldUnitsPerVertex` mismatch).
  - Black or magenta sub-tiles → SSBO read out-of-bounds or RecordIdx propagation broken.
  - Tile-aligned rectangular artifacts at non-tarmac biome boundaries → cement applied to non-cement quads (validity-bit gate broken).
  - `truncated=1` in trace → `>=1024` cement variants enumerated (V27 atlas-budget cap; pre-V27 was 255); cap silently truncated atlas.
  - `unmapped_pack_count` in 4+ digits → enumeration miss in `BuildCementCatalogAtlas`.

- [ ] **Step 1:** Capture near + distance/oblique screenshots both modes; note camera positions in commit message.
- [ ] **Step 2:** Document PASS/FAIL.

### Task C.3 — Gate B: Tracy delta on `Terrain::SetupSolidBranch`

- [ ] **Step 1:** Run mc2_01 default-on, attach Tracy, capture 60 s.
- [ ] **Step 2:** Confirm `Terrain::SetupSolidBranch` ≤ 20 % of Stage 1 baseline.
- [ ] **Step 3:** `Terrain::IndirectCementAtlasUpload` appears once per mission load, <50 ms for 9-30 tiles.
- [ ] **Step 4:** No regression on `Terrain::ThinRecordPack` (≤ +10 % vs Stage 1).

### Task C.4 — Gate C: parity check tier1 5/5

- [ ] **Step 1:**

```bash
MC2_TERRAIN_INDIRECT_PARITY_CHECK=1 \
  py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 30 --keep-logs
```

Expected: zero `event=mismatch` lines across 5 missions. (Validates the new `case 20:` patches via `_wp0` comparison.)

### Task C.5 — Gate D: tier1 5/5 quintuple (N4)

- [ ] **Step 1:**

```bash
for i in 1 2 3 4 5; do
  py -3 scripts/run_smoke.py --tier tier1 --with-menu-canary --kill-existing --duration 20
done
```

All 5 PASS or do not flip default-on.

### Task C.6 — Gate D2: mission-restart lifecycle (B7)

- [ ] **Step 1:** Run a 3-mission warm-boot pass:

```bash
py -3 scripts/run_smoke.py --tier tier1 --kill-existing --duration 30 --keep-logs
```

- [ ] **Step 2:** Grep:

```bash
LATEST=$(ls -t tests/smoke/artifacts/ | head -1)
grep -c "event=cement_catalog_built" tests/smoke/artifacts/$LATEST/*/console.log
grep -c "event=cement_catalog_reset" tests/smoke/artifacts/$LATEST/*/console.log
```

Expected: each `built` matches a `reset` (or `n_built = n_reset + 1` if final mission did not exit via `Mission::destroy` by capture end). Peak RSS at last mission ≤ peak at first mission + 50 MB headroom (sample via Process Explorer or equivalent).

- [ ] **Step 3:** RSS climbs unbounded → cement atlas teardown is broken; re-examine Task A.2 Step 2.

### Task C.7 — Slice closeout: memory + index + Stage C bundled commit

- [ ] **Step 1:** Write `memory/indirect_terrain_solid_endpoint.md`:

```markdown
---
name: indirect-terrain SOLID endpoint
description: PR1+PR2 architecture, default-on date, Tracy delta summary, queued follow-ups
type: project
---

## Status (2026-05-01)
- **PR1:** f221570 + a29ff83 + c8fa5df (SOLID indirect draw, colormap atlas, dual-UV via WorldPos).
- **PR2:** <commit-hash-from-C.7> (cement multi-sampler + default-on flip).
- **Default-on flipped:** 2026-05-01.

## Architecture
[2-3 paragraphs covering: SSBO recipe + thin-record split, colormap atlas at unit 0, cement
catalog atlas at unit 3 via GPU readback, RecordIdx flat varying for frag-side SSBO read,
validity bit at _pad0 bit 31 for layer-0 disambiguation, killswitch path.]

## Tracy delta (Gate B)
- `Terrain::SetupSolidBranch`: <pre-PR2 ms> → <post-PR2 ms> (<pct%> reduction)
- `Terrain::IndirectCementAtlasUpload`: <ms> per mission load
- `Terrain::ThinRecordPack`: <pre> → <post> (<pct%> delta)

## Catalog count (Gate A trace)
- mc2_01: <N> cement tiles
- mc2_03 / mc2_10 / mc2_17 / mc2_24: <N each, often 0>

## Queued follow-ups
- Target 2 brainstorm: multi-layer overlays/decals (alpha-cement overlay, runway markings).
- Per-cell atlas mipmaps with gutters (if Gate A distance shimmer is unacceptable).
- Post-soak: physical deletion of legacy SOLID path.
```

- [ ] **Step 2:** Add to `memory/MEMORY.md` under "Rendering / shaders":

```markdown
- ⭐ [Indirect-terrain SOLID endpoint shipped](indirect_terrain_solid_endpoint.md) — PR1+PR2 default-on 2026-05-01; cement multi-sampler via GPU readback + frag SSBO RecordIdx + validity-bit _pad0
```

- [ ] **Step 3:** Stage C bundled commit:

```bash
git add GameOS/gameos/gos_terrain_indirect.cpp \
        GameOS/gameos/gos_terrain_indirect.h \
        memory/indirect_terrain_solid_endpoint.md \
        memory/MEMORY.md
git commit -m "$(cat <<'EOF'
feat(terrain-indirect): Stage 4 — cement multi-sampler + default-on flip (PR2)

Pure-cement quads (airport tarmac, runways, concrete pads) now render
their authored CONCRETE catalog texture under the indirect SOLID draw,
matching the legacy M2 per-bucket binding.  Cement-catalog atlas is
built at mission load via GPU readback (textureData[0] is NULL in
stock gameplay — quickLoad gates the RAM path at terrtxm.cpp:561) and
bound at sampler unit 3.  Per-quad cement layer-index + validity bit
live in TerrainQuadThinRecord._pad0 (bit 31 = valid; bits 7:0 = layer);
the frag reads it directly from the thin-record SSBO at binding 2,
indexed by a flat RecordIdx varying emitted by the thin VS (and =0u
emitted at the top of legacy gos_terrain.tese main() — before the
debug early-outs — for linker compat).

Default-on flip: IsEnabled() now returns true unless MC2_TERRAIN_INDIRECT=0.
Killswitch preserved.  Quintuple N4 gate: tier1 5/5 × 5 PASS.  Gate A
(visual): mc2_01 tarmac concrete tiles match legacy at near AND distance/
oblique angles.  Gate D2: mission restart lifecycle clean (cement_
catalog_built/reset paired, no leak).

Also patches END_CEMENT_TYPE(20) → Concrete in both terrainTypeToMaterial
sites (quad.cpp + gos_terrain_indirect.cpp) for parity correctness.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## 5-Gate Ladder

| Gate | Condition | Measured by |
|---|---|---|
| **A — Visual** | mc2_01 tarmac concrete tiles render at NEAR and DISTANCE/OBLIQUE camera, ≤5% perceptual diff vs legacy. No failure-mode artifacts. Trace shows `truncated=0`, low `unmapped_pack_count`, no `cement_uniform_missing`. | Manual screenshot pairs + log grep. |
| **B — Perf** | `Terrain::SetupSolidBranch` ≤ 20 % of Stage 1 baseline. `Terrain::ThinRecordPack` ≤ +10 % delta. | Tracy capture, mc2_01, 60 s. |
| **C — Parity** | `MC2_TERRAIN_INDIRECT_PARITY_CHECK=1` tier1 5/5: zero `event=mismatch`. | Smoke run, 30 s/mission. |
| **D — Regression** | tier1 5/5 PASS × 5 sequential runs (N4 quintuple). Menu canary clean. | `run_smoke.py --tier tier1 --with-menu-canary` × 5. |
| **D2 — Lifecycle** | Cross-mission warm-boot run: `cement_catalog_built` and `cement_catalog_reset` paired; RSS flat across boundaries (≤50 MB drift). | Single 3-mission smoke + grep + RSS sampling. |

All 5 gates must PASS before the Stage C commit lands.

---

## Verification Appendix (V1..V25)

> Discipline per CLAUDE.md "Documentation Discipline": every cited symbol grep-verified at write-time INCLUDING enclosing control flow AND function signature/return type when the cited site is a function call. The V_L → V21 lesson learned twice: V21 in v2.0 conflated `mcTextureNodeIndex` (return of `getTextureHandle`) with `textures[]` slot (input arg). v2.1 retracts V21 and adds V22 with the corrected semantics.

| ID | Claim | Evidence | Status |
|---|---|---|---|
| **V1** | `quickLoad = true` is the stock-gameplay default; pixel-data RAM block is dead | [`mclib/terrtxm.cpp:561`](mclib/terrtxm.cpp:561) `if (InEditor || !quickLoad)` encloses lines 562-584. | ✅ (B1 fix: GPU readback) |
| **V2** | `getNextAvailableSlot()` is a one-line public accessor following the `getNumTypes()` pattern | New in this plan; mirror of [`mclib/terrtxm.h:234`](mclib/terrtxm.h:234) | ✅ |
| **V3** | `peekTextureHandle(slot)` is public, returns `mcTextureNodeIndex` (NOT gosHandle) and is bounds-checked against `nextAvailable` | [`mclib/terrtxm.h:290-296`](mclib/terrtxm.h:290) — function takes `DWORD texture` (slot), returns `textures[texture].mcTextureNodeIndex`. | ✅ |
| **V4** | `isCement(slot)` is public, bounds-checked against `nextAvailable`, returns true iff `flags & MC2_TERRAIN_CEMENT_FLAG`. **Argument is a `textures[]` slot, NOT a nodeIdx.** | [`mclib/terrtxm.h:337-342`](mclib/terrtxm.h:337). | ✅ |
| **V5** | `tex_resolve(nodeIdx)` accepts `mcTextureNodeIndex` and returns `gosHandle` (engine handle, not GL texture) | `docs/superpowers/specs/2026-04-27-modern-terrain-tex-resolve-table-design.md:171,216,275`. Used at [`gos_terrain_indirect.cpp:944`](GameOS/gameos/gos_terrain_indirect.cpp:944) on `q.terrainHandle`, confirming `q.terrainHandle` is a nodeIdx (V22). | ✅ |
| **V6** | `gos_terrain_bridge_glTextureForGosHandle(gosHandle)` returns the GLuint texture name; returns 0 for INVALID_TEXTURE_ID/0/non-resident | [`GameOS/gameos/gos_terrain_bridge.h:37-47`](GameOS/gameos/gos_terrain_bridge.h:37) declares; implementation at [`gameos_graphics.cpp:1775-1781`](GameOS/gameos/gameos_graphics.cpp:1775) (subagent V6 grep). Closes ❓2. | ✅ |
| **V7** | `MC_MAX_TERRAIN_TXMS = 3000` — upper bound on `nextAvailable` (textures[] slot space). NOT the upper bound on `mcTextureNodeIndex`. | [`mclib/terrtxm.h:34`](mclib/terrtxm.h:34). | ✅ |
| **V26** | `mcTextureNodeIndex` is bounded by `MC_MAXTEXTURES = 4096` (NOT MC_MAX_TERRAIN_TXMS = 3000, NOT MAX_MC2_GOS_TEXTURES = 3000). The masterTextureNodes[] array is sized MC_MAXTEXTURES at allocation. The cement layer-index map MUST be sized MC_MAXTEXTURES to avoid silently dropping cement textures with nodeIdx in [3000, 4096). | [`mclib/txmmgr.h:44`](mclib/txmmgr.h:44) `#define MC_MAXTEXTURES 4096`; [`mclib/txmmgr.cpp:206-211`](mclib/txmmgr.cpp:206) sizes `masterTextureNodes` at `MC_MAXTEXTURES`. Subagent v2.1 review C1-v21. | ✅ (fix: A.2 Step 1, A.3 Step 2, A.4 Step 2 all use MC_MAXTEXTURES) |
| **V8** | `BASE_CEMENT_TYPE = 10`, `START_CEMENT_TYPE = 13`, `END_CEMENT_TYPE = 20` | [`mclib/terrtxm.h:42-44`](mclib/terrtxm.h:42). | ✅ |
| **V9** | `MC2_TERRAIN_CEMENT_FLAG = 0x00000001` | [`mclib/terrtxm.h:53`](mclib/terrtxm.h:53). | ✅ |
| **V10** | `TerrainQuadRecipe` is 9 vec4 = 144 B (frozen) | [`gos_terrain_patch_stream.h:87-99`](GameOS/gameos/gos_terrain_patch_stream.h:87). | ✅ |
| **V11** | `TerrainQuadThinRecord._pad0` exists at [`gos_terrain_patch_stream.h:107`](GameOS/gameos/gos_terrain_patch_stream.h:107); maps to `control.w` in the SSBO struct (per [`gos_terrain_thin.vert:5`](shaders/gos_terrain_thin.vert:5)) | C++ struct layout: `recipeIdx`, `terrainHandle`, `flags`, `_pad0` = `control.x/y/z/w`. | ✅ |
| **V12** | The thin-record SSBO at binding 2 is bound by the bridge per draw via `glBindBufferRange` | [`gameos_graphics.cpp:2355`](GameOS/gameos/gameos_graphics.cpp:2355). Frag-side read uses the same binding-2 sub-range. | ✅ |
| **V13** | `gl_PrimitiveID` restarts at 0 per sub-draw under `glMultiDrawArraysIndirect`, ruling out a frag-side global recordIdx derivation from PrimitiveID alone — hence the flat varying transport | OpenGL 4.6 spec §10.2 ("primitive ID is reset … at the beginning of each draw call"). | ✅ (rationale in Delta from v1) |
| **V14** | Legacy non-thin VS chain shares `gos_terrain.frag`; adding a varying to one chain without the matching declaration in the other = silent linker fail = transparent terrain | Comment at [`gos_terrain_thin.vert:22-31`](shaders/gos_terrain_thin.vert:22). Mitigation: add `flat out uint RecordIdx; RecordIdx=0u;` to **TES**, NOT VS (V25). | ✅ (mitigation: B.1) |
| **V15** | `tex3` declared `sampler2D`, currently unused | [`shaders/gos_terrain.frag:35`](shaders/gos_terrain.frag:35). | ✅ |
| **V16** | `useAtlasColormap` reset block at `gameos_graphics.cpp:2364-2367` (M3 / v1 typo fix) | Confirmed inside `gos_terrain_bridge_drawIndirect`. | ✅ |
| **V17** | `terrainTypeToMaterial` ([`mclib/quad.cpp:142-145`](mclib/quad.cpp:142)) and `terrainTypeToMaterialLocal` ([`gos_terrain_indirect.cpp:244-245`](GameOS/gameos/gos_terrain_indirect.cpp:244)) BOTH miss `case 20` for Concrete; both must be patched together | Confirmed both files. | ✅ (fix: A.1.bis) |
| **V18** | `BuildColormapAtlas()` called from `BuildDenseRecipe()` at [`gos_terrain_indirect.cpp:476`](GameOS/gameos/gos_terrain_indirect.cpp:476) | Confirmed. Cement atlas hooks immediately after. | ✅ |
| **V19** | `ResetDenseRecipe()` `g_atlasGLTex` teardown at lines 497-501 | [`gos_terrain_indirect.cpp:498-501`](GameOS/gameos/gos_terrain_indirect.cpp:498). | ✅ |
| **V20** | `Terrain::worldUnitsPerVertex = 128.0f` — publicly accessible | [`mclib/terrain.cpp:92`](mclib/terrain.cpp:92) (subagent confirmed). Used as `atlasCementWorldUnitsPerTile` uniform value. | ✅ |
| ~~V21~~ | ~~`q.terrainHandle` at packer time is the un-resolved `mcTextureNodeIndex` (textures[] slot)~~ | **RETRACTED.** Conflated `mcTextureNodeIndex` and `textures[]` slot. The two are distinct DWORDs. See V22 for the corrected semantics. | ❌ retracted |
| **V22** | `q.terrainHandle` at packer time is `mcTextureNodeIndex` (the **return value** of `getTextureHandle`), NOT a `textures[]` slot. The slot is the **input** to `getTextureHandle`. The two are distinct DWORDs. | [`mclib/quad.cpp:546`](mclib/quad.cpp:546) writes `terrainHandle = Terrain::terrainTextures->getTextureHandle((vertices[0]->pVertex->textureData & 0x0000ffff))` — the slot is the arg, the nodeIdx is the return. [`mclib/terrtxm.h:281-288`](mclib/terrtxm.h:281): `getTextureHandle(DWORD texture)` returns `textures[texture].mcTextureNodeIndex`. The cement-layer map MUST be keyed by nodeIdx (Task A.3 Step 2) so the packer can index it directly with `q.terrainHandle` (Task A.4 Step 2). | ✅ |
| **V23** | `_pad0` carries cement-layer-valid bit at bit 31, layer index in **bits 15:0** (V27 widening; was 7:0 pre-V27); bits 30:16 reserved for future layers | New encoding in v2.1 per advisor C2. Disambiguates "layer 0" from "not cement", which matters for alpha-cement boundary fragments where TerrainType can interpolate near 3.0 while the quad isn't pure cement. V27 widens the layer field from 8 → 16 bits (0..65535 encoding cap) so the atlas budget — not the bit field — is the limiting factor. | ✅ |
| **V24** | `BuildCementCatalogAtlas` saves/restores `GL_ACTIVE_TEXTURE` (advisor M2) in addition to unit-0 binding and PACK/UNPACK alignment | New requirement in v2.1; previously the function force-set `glActiveTexture(GL_TEXTURE0)` without restore. | ✅ (Task A.3 Step 2) |
| **V25** | The legacy `gos_terrain` material program is VS+TCS+TES+FS; the frag's varying SOURCE is the TES (not the VS). TCS strips VS outputs not consumed. Adding `flat out` to the VS does NOT reach the frag | [`shaders/gos_terrain.tese:5-16`](shaders/gos_terrain.tese:5) reads `tcs_*` from TCS, writes the bare frag-input names (`Color`, `Texcoord`, `TerrainType`, `WorldNorm`, `WorldPos`, `UndisplacedDepth`). The thin program is VS+FS only — VS outputs feed frag directly. | ✅ (mitigation: B.1) |
| **V27** | Engine cement-flag bug at [`mclib/terrtxm.cpp:162`](mclib/terrtxm.cpp:162): bulk `memset(textures, -1, ...)` leaves `TerrainTXM::flags` as `0xFFFFFFFF`, making `isCement()` report true on every uninitialized slot (~715/724 base slots reading garbage flags). Fixed in this slice with explicit `flags = 0` per-slot init after the bulk memset, preserving `mcTextureNodeIndex == 0xFFFFFFFF` "unallocated" sentinel for other fields. ALSO in V27: `_pad0` layer-index field widened from bits 7:0 (255 max) to bits 15:0 (65535 encoding cap); atlas-budget cap raised from 255 → 1024 (gridSide=32, 2048×2048 = 16 MB) to provide headroom for high-cement-coverage maps (e.g., mc2_24 at ~20% concrete). | Engine fix: [`mclib/terrtxm.cpp:162-167`](mclib/terrtxm.cpp:162). Encoding fix: [`gos_terrain_indirect.cpp` `PackThinRecordsForFrame`] cementWord mask & 0xFFFFu. Atlas cap: [`BuildCementCatalogAtlas`] `< 1024` guard. | ✅ |

### Open ❓ items

| ID | Item | Action |
|---|---|---|
| ~~❓1~~ | ~~Does TES re-emit varyings?~~ | **CLOSED** by V25: yes, TES re-emits all frag-input names; B.1 patches TES only. |
| ~~❓2~~ | ~~Confirm `gos_terrain_bridge_glTextureForGosHandle` is implemented~~ | **CLOSED** by V6: implemented at `gameos_graphics.cpp:1775-1781`. |
| ~~❓3~~ | ~~The N≤255 cap in Task A.3 Step 2 is now surfaced via `event=cement_catalog_truncated` and Gate A treats it as FAIL. If a stock mission ever hits the cap, escalate to expanding the layer-index width (use bits 15:0 instead of 7:0; reduces "reserved for future layers" from 23 bits to 15 — still ample).~~ | **CLOSED** by V27: layer-index widened to bits 15:0; atlas cap raised to 1024. Diag run discovered 110-240 real cement per tier1 map (~9 base + 100-230 transitions), edging the old 255 cap. |
| ❓4 | `unmapped_pack_count` non-zero post-Stage A means a non-zero `q.terrainHandle` mapped to no cement layer. Could be a benign non-cement quad whose texture is in the colormap atlas (won't surface as a render bug because frag's `useCementAtlas` gate doesn't fire on that path due to the validity bit). Could also be a cement transition texture whose nodeIdx wasn't enumerated (would render as the underlying biome — visible as Gate A failure). Investigate if Gate A fails and `unmapped_pack_count` is large. | Operational. |

---

## Cross-references

| Symbol | File:line | Plan section |
|---|---|---|
| `getNextAvailableSlot()` (NEW) | `mclib/terrtxm.h` after :234 | A.1 |
| `case 20:` patch (Concrete) | `mclib/quad.cpp:142-145`, `gos_terrain_indirect.cpp:244-245` | A.1.bis |
| Cement-atlas statics (`g_cementLayerIndexByNodeIdx[]`) | `gos_terrain_indirect.cpp` after :377 | A.2 |
| `BuildCementCatalogAtlas()` | `gos_terrain_indirect.cpp` after :419 | A.3 |
| `BuildCementCatalogAtlas()` call site | `gos_terrain_indirect.cpp:476` (after `BuildColormapAtlas()`) | A.3 Step 4 |
| `tr._pad0` populate (with validity bit) | `gos_terrain_indirect.cpp:991` | A.4 |
| `flat out uint RecordIdx` (thin VS) | `shaders/gos_terrain_thin.vert` (after :35, in main after :169) | A.5 |
| `flat out uint RecordIdx` (legacy TES) | `shaders/gos_terrain.tese` (after :16, in main alongside other out-assignments) | B.1 |
| Cement uniforms + SSBO + branch (frag) | `shaders/gos_terrain.frag` (after :60 and replacing :230-237 block) | B.2 |
| Bridge cement bind + reset + uniform-loc warn | `gameos_graphics.cpp` (after :2323 and after :2367) | B.3 |
| `IsEnabled()` flip | `gos_terrain_indirect.cpp:40-46` | C.1 |
| `ResetDenseRecipe` cement teardown | `gos_terrain_indirect.cpp:479` (after :501) | A.2 Step 2 |
