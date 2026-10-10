<!-- doc-status: current | reviewed:2026-10-11 -->
# TREE-OVERRIDE-LOD-SPEC-1 — Design spec

**Status:** SPEC (design only — no implementation, no runtime behavior change, no collision/asset mutation). Feeds the plan→review→execute phase.
**Branch:** `claude/model-override-system-recon-1`.
**Inputs:** `docs/model-override-lighting-lod-recon.md` (the perf-architecture recon — authoritative), `docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md` (the black-prop root-cause + Fix 3), `docs/model-override-mvp-notes.md` (dual-shape + texture binding), `docs/model-override-gpu-instancing-proposal.md`.

## Problem (one line)
A model-override forest renders the full-detail 706k-tri override mesh **at every distance** (trees are pinned to a single LOD) → `Render.GpuStaticProps` GPU raster = 2.29s/frame. The fix is a **safe, per-LOD-pre-registered LOD path** for override static props/trees that (a) draws distance-appropriate geometry and (b) does NOT resurrect the black-tree regression class.

## Non-goals
- Not a lighting fix — lighting is already O(1) for registered overrides (recon §2–§5); the stale "32-slot UBO" claim was corrected in Phase 0 (`MODEL-OVERRIDE-LIGHTING-COMMENT-CORRECTION-1`, commit `fc4c363c`).
- Not collision/gameplay — collision stays stock (Task 0 = CASE A; dual-shape invariant). LOD lives ONLY on render shapes.
- Not a building-LOD rework — buildings already have a (churning) LOD-swap path; this spec targets **trees first** and may later generalize. Touching the building path is out of MVP scope.
- Not stock-asset mutation, not `tgl.fst` surgery.

---

## 1. Current tree-LOD reality (recon §4.1, grep-verified)
- `TreeAppearance` builds its per-instance render shape from **LOD index 0 only** and never swaps at runtime: `currentLOD=0` (`bdactor.cpp:3834`); `getTreeRenderShape(0)->CreateFrom()` at init (`:3856`) and setObjStatus NORMAL (`:3999`); `registerStatic` transforms LOD-0 geometry (`:4833`).
- Tree `lodDistance[]` IS loaded (`mclib/bdactor_update.cpp:223-228`) but **never used** to swap the render shape. Buildings, by contrast, have distance LOD-swap blocks (`:3025,:3092,:3556`) — and those are the churn/black-prop source.
- Type-level render shape is a **single** `treeRenderShape` (`bdactor.h:478`), NOT `treeRenderShape[MAX_LODS]`; `getTreeRenderShape(long lod)` discards `lod` when an override is present (`mclib/bdactor.h:603`). **This is the structural seam to change.**

## 2. The black-tree regression class (recon §4.2 — the load-bearing constraint)
Root cause (`lod-swap-churn spec:78-87`): after a shape/LOD swap the new `TG_MultiShape`'s `lightData_` is default-zero (only `TransformMultiShape_BuildRecipe` positions ran). Static replay `touch()` → `ResubmitCachedGpuLightData()` re-ships all-zero lighting → zero-light slot → **black prop/tree**. Cure already in code: every registration/swap site re-arms `needsFullBakeNextFrame=true` (`bdactor.cpp:4908,:4281,:4256,:2893,:1508,:1476`) forcing a full `update()` that populates `lightData_` before any `touch()`. **Live regression** (`docs/known_issues.md:125` — a prior repoint "resurrected the 2026-05-05 black-tree class").

**LIGHTING SAFETY RULE (mandatory invariant for this slice):**
> Any geometry that can be selected for draw (any LOD) MUST have been fully baked (its `lightData_` populated and a permanent baked light slot assigned) BEFORE it is first replayed. A LOD switch must never select a shape whose `lightData_` is still zero. Achieved by **pre-registering + pre-baking every LOD at mission load** (§4), so a switch is a lookup among already-baked recipes — never an invalidate+re-register. Belt-and-suspenders: any code path that does switch the active shape also sets `needsFullBakeNextFrame=true`.

---

## 3. Answers to the 9 spec questions

**Q1 — How do stock tree LOD fields load today?**
`BldgAppearanceType`/`TreeAppearanceType` read `FileName0..N` + `Distance0..N` into `treeShape[MAX_LODS]` + `lodDistance[]` at type load (`bdactor.cpp:3634-3639` tree). Stock trees populate LOD0 only in practice; buildings populate multiple. The override path currently fills only the single `treeRenderShape` (LOD0).

**Q2 — Can the override glTF provide multiple LODs?** Three options; spec picks (c) for MVP with (a) allowed:
- (a) **Separate manifest files per LOD** — explicit, modder-authored. Manifest gains an optional `lods` array (§5).
- (b) **Nodes named `LOD0/LOD1/LOD2`** inside one glTF — convenient but couples to authoring convention; defer.
- (c) **Generated decimation at import** — reuse the vendored gltfpack/meshopt + the leaf-card-preserving thinner (`.claude/tree_export_lush.py` approach) to derive LOD1/LOD2 from LOD0 at load. **MVP default.** Far-LOD may be a 2-tri impostor billboard (Phase 2/3, not MVP).

**Q3 — Where should LOD selection happen for trees?** Per-instance, distance-driven, in `TreeAppearance` update — set `currentLOD` from `lodDistance[]` (mirroring the building distance logic but WITHOUT its invalidate+re-register churn). Selection chooses among **already-registered per-LOD recipes** (§4); it does not rebuild geometry. The GPU cull/substrate already buckets per recipe; per-LOD recipes are the natural extension.

**Q4 — How to pre-register one recipe per LOD?** Promote the render shape to an LOD-indexed set: `treeRenderShape[MAX_LODS]` (and `bldgRenderShape[MAX_LODS]` later). At mission load, for each populated LOD: load/generate the LOD mesh, resolve its textures, register a static recipe (`registerStatic` per LOD → its own `recipeIndex`), all **before** the one-shot `finalizeGeometry` freeze (recon: the immutable-VBO seam that bit Slices 3–5 — all LODs must be in `s_typeIndex` pre-finalize). `staticReg` becomes per-LOD (array keyed by LOD), per the lod-swap-churn spec **Fix 3** (`:105-115`).

**Q5 — How does each LOD get a valid baked light slot?** Each per-LOD recipe is a distinct registry `recipeIndex` → `mc2CacheOrBakeStaticGpuLight` assigns it a permanent baked slot on its first full bake (recon §1, `bdactor.cpp:1903-1939`; `EmitBakedGpuLightData` O(1) thereafter). Because slots are per-recipe (not per-instance), N LODs × M types = N·M recipe slots (tiny), not per-instance. Force the one-time full bake of every LOD at mission load (re-arm at each per-LOD registration) so no selected LOD is ever zero-lit.

**Q6 — How do textures/materials resolve per LOD?** Per-LOD via `LoadOverrideRenderShapeTextures` (mvp-notes Slice 5) on each LOD render shape. LODs derived by decimation share the LOD0 texture names (same materials) → same resolved layers; the alpha-`a_` leaf convention carries through. Explicit per-LOD files (option a) may name their own textures.

**Q7 — How to prevent black-tree regressions?** The LIGHTING SAFETY RULE (§2): pre-register + pre-bake all LODs; switching selects an already-baked recipe; never invalidate+re-register on a per-distance switch; re-arm `needsFullBakeNextFrame` at any actual active-shape change. A **regression gate** (a mission-load + switch test that asserts no zero-light actor is emitted) is baked into the slice.

**Q8 — How to validate culling/bounds across LODs?** Each LOD recipe computes its own vertex-tight, mesh-local AABB / `extentRadius` (same path as the dual-shape bounds recompute, mvp-notes Slice 2 — `assimp_importer.cpp` ComputeBoundingBox). The GPU cull uses the active recipe's bounds. Validate: each LOD's bounds are non-zero and enclose its mesh; the active-LOD bounds drive frustum/distance cull; no pop/disappear at LOD boundaries (a known stock LOD risk).

**Q9 — MVP?**
- **Two LODs** (LOD0 = full override mesh; LOD1 = decimated, e.g. leaf-card-thinned to a target tri budget) for **one** override tree type.
- **Static pre-registration** of both LODs at mission load, each with its own recipe + baked light slot.
- **Distance switch** selects the active LOD recipe (no invalidate/re-register).
- **Stock fallback** if a LOD fails to load/register (fall back to the lower LOD already registered, else stock).
- **No collision changes** (collision stays stock LOD-independent).
- Verified: forest GPU zone drops materially vs full-LOD-everywhere; no black trees across switches; no GL errors; collision/footprint unchanged.

---

## 4. Pre-registration architecture (the core design)
```
mission load (per override tree TYPE, before finalizeGeometry):
  for lod in 0..N where LOD source exists (file or generated):
    renderShapeLOD[lod] = import/generate + resolve textures + vertex-tight bounds
    register static recipe(renderShapeLOD[lod]) -> recipeIndex[lod]
    arm full bake for recipeIndex[lod]   (populate lightData_ once -> permanent slot)
  finalizeGeometry()  (immutable VBO now holds ALL LODs' geometry, once each)

runtime (per instance, per frame):
  currentLOD = pickLODByDistance(instance, lodDistance[])
  activeRecipe = recipeIndex[currentLOD]          # lookup, NOT rebuild
  markVisible(activeRecipe, bakedLightSlot[currentLOD], ...)   # O(1) replay
  # NO invalidateStaticRegistration, NO per-frame re-register, NO geometry rebuild
```
Why this is safe + fast: geometry per LOD is uploaded once (immutable VBO); light slot per LOD is baked once (O(1) replay); a distance switch is a recipe-index lookup → no zero-light window → no black frame → no CPU churn. This is exactly Fix 3 of the lod-swap-churn spec, applied to override render shapes.

**Dual-shape invariant preserved:** `treeShape[]`/`bldgShape[]` (stock) remain the collision authority — `calcCellsCovered`/`markTerrain` read them, LOD-independent. LOD touches ONLY `*RenderShape[]`. The `_HierarchyOnly` pool-skip (GPU-INSTANCE-SKIP-POOLS-1) must keep populating `rec.shapeToWorld` for whichever LOD is selected.

---

## 5. Proposed manifest shape (additive, back-compatible)
Current per-override entry keeps working (single source = LOD0, LODs auto-generated). Optional explicit LODs:
```json
{
  "type": "model", "class": "tree", "replaces": "tree:tc1_1",
  "source": "source/trees/oak_lod0.glb",
  "renderOnly": true, "scale": 1.0, "fallback": "stock",
  "lods": [
    { "lod": 1, "source": "source/trees/oak_lod1.glb", "distance": 250 },
    { "lod": 2, "source": "source/trees/oak_lod2.glb", "distance": 800 }
  ],
  "autoLod": true   // if no explicit lods[], generate via decimation (MVP default)
}
```
Rules: `lod` ascending, `distance` ascending; `source` for LOD0 is the existing field; `lods[].distance` overrides/augments stock `lodDistance[]`; `autoLod` true (default) generates missing LODs by decimation. Validation (registry): reject non-ascending LODs/distances, missing LOD0, unsafe per-LOD source paths (same `isSafeSource` rule). Scale==1.0 still enforced.

---

## 6. Validation plan
- **Perf (the win):** override forest (multiple on-screen tree types) — `Render.GpuStaticProps` GPU zone drops materially vs the no-LOD baseline (target: from ~2.29s toward stock-tree-class ms). Capture Tracy GPU zone before/after. Pool peaks stay low (GPU-INSTANCE-SKIP-POOLS-1 already holds).
- **Black-tree regression gate (mandatory):** assert NO actor is emitted with an all-zero light slot across mission load AND across forced LOD switches (env-driven LOD force, e.g. `MC2_FORCE_LOD=N`). One full mission cycle + a camera dolly that crosses every LOD distance band shows no black trees (view screenshots + a zero-light-slot counter == 0).
- **Correctness:** no-mod identity byte-identical; stock trees unchanged; collision/`cellsCovered` identical to stock across all LODs (diff the footprint); 0 GL errors; +0 destroys.
- **Bounds/cull:** each LOD draws within its bounds; no disappear/pop at LOD switch (or document acceptable pop); off-frustum/distant instances drop from the draw (cull works per active LOD).
- **Fallback:** a failed LOD import → falls back to a registered lower LOD or stock; no crash, logged.

## 7. Implementation slices (for the plan phase)
1. **LOD-set render shape + per-LOD registration** — `*RenderShape[MAX_LODS]`, per-LOD `registerStatic` + bake at mission load (pre-finalize), per-LOD `staticReg`/recipe array. (No distance switch yet — register LOD0 only behaves as today: safety baseline.)
2. **Auto-LOD generation at import** — decimate LOD0 → LOD1(/2) (leaf-card-preserving), resolve textures + bounds per LOD.
3. **Distance-driven `currentLOD` selection for trees** — use `lodDistance[]`; select active recipe; re-arm `needsFullBakeNextFrame` on any active-shape change (guard rail).
4. **Manifest `lods[]`/`autoLod`** parse + validation in the registry.
5. **Regression gate + perf capture** wired (zero-light-slot counter; Tracy before/after; black-tree dolly test).
Sequencing: 1 (safe no-op baseline) → 2 → 3 (the perf win) → 4 → 5 throughout. Each slice keeps no-mod identity.

## 8. Stop conditions
- If a per-LOD recipe cannot be assigned a permanent baked light slot before first replay → STOP; do not ship distance switching (would resurrect black trees). Fall back to single-LOD.
- If auto-decimation cannot produce a usable lower LOD for leaf-card foliage within budget → require explicit `lods[]` files; document.
- If distance switching shows black trees in the regression gate despite pre-bake → BLOCK; the pre-bake/replay ordering is wrong — return to recon.
- If per-LOD pre-registration can't complete before the one-shot `finalizeGeometry` for some load order → STOP; resolve registration timing first (the Slice-3/5 seam).
- If any collision reader would observe a LOD shape → BLOCK (dual-shape violation).
- Building LOD generalization is explicitly deferred; if trees-only proves insufficient, re-spec.

## 9. Hard constraints (carried)
No implementation in this doc · no runtime behavior change from the spec · no collision changes · no stock-asset/`tgl.fst` mutation · no `git add -A` · LOD only on render shapes · all LODs pre-registered + pre-baked before finalize · preserve the `needsFullBakeNextFrame` black-tree guard.
