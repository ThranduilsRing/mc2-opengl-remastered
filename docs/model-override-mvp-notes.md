<!-- doc-status: stale | reviewed:2026-10-11 -->
# MODEL-OVERRIDE-MVP-1 — Implementation Notes

Running validation/decision log. See plan: `docs/model-override-mvp-plan.md`.

---

## TREE-OVERRIDE-LOD-MVP-1 Task 0 (M2 light-slot cardinality) — **BASELINE FINDING** (reframed from STOP per owner ruling 2026-06-03)

- `MC2_LIGHTSLOT_TRACE` `[LIGHTSLOT v1]` measured (v0.3, `--validate --frames 20
  -mission mc2_01`, exit 0, 0 GL errors):
  - override lush 6-type: `instances=29 types=6 recipes=982 unique_slots=29 per_instance_distinct=29`
  - stock baseline:        `instances=119 types=0 recipes=982 unique_slots=119 per_instance_distinct=119`
- **Reframe (owner ruling): NOT a hard stop.** Single-LOD `U == K` is **pre-existing
  engine behavior** (stock ALSO shows `U == K`), **not** override-specific, **not** a
  per-frame bake (allocated once + O(1) baked replay), and **not** the GPU raster cost
  (that's LOD/overdraw). Position-dependent gather (`txmmgr.cpp:1278`/`:1333`,
  `mclib/msl_transform.cpp:585`) → per-instance slot. Recorded as **deferred lighting-ownership
  cleanup** (`docs/model-override-lighting-ownership-recon.md`), NOT a blocker.
- **NEW GATE — at Task 3** (after LOD1 imported + registered alongside LOD0, rerun LIGHTSLOT):
  - **PASS:** U stays ≈ K / bounded per instance — no multiplication by LOD count
    (LODs of one instance share position → same gather → dedup to one slot).
  - **STOP:** U grows toward K×M → split explicit lighting-ownership before continuing.
- Proceeding: Task 1 `treeRenderShape[MAX_LODS]`, Task 2 `staticReg[MAX_LODS]`, Task 3 LOD1 import + remeasure (the real K×M gate).

---

## TREE-OVERRIDE-LOD-MVP-1 Task 3 (K×M GATE) — **VERDICT: STOP** (2026-06-03)

LOD1 generated + imported + registered + baked alongside LOD0 for ONE 2-LOD type
(`tree:tc1_1`); `[LIGHTSLOT v1]` remeasured. **The K×M gate FAILS: LOD1's
per-instance light bake takes a FRESH slot per instance — the table grows by ~K,
not deduped to LOD0's slot.** Per the gate's STOP condition, HALT before Task 4.

### Asset
- `tree_lush_lod1.glb` = **235,546 tris** (leaf-card-PRESERVING thin of the lush
  LOD0: `.claude/tree_export_lush_lod1.py --keep 0.06`, kept 1,815/30,250 leaf
  cards — a SUBSET of LOD0's 9,075 via the same seed=1234 — + full trunk/branch
  122k). LOD0 lush = 706,628 tris. LOD1 ≈ 33% of LOD0, visibly lighter, canopy
  preserved. Deployed to v0.3.

### Manifest used (the 2-LOD type)
```json
{ "type":"model","class":"tree","replaces":"tree:tc1_1",
  "source":"source/trees/tree_lush.glb","renderOnly":true,"scale":1.0,"fallback":"stock",
  "lods":[ { "lod":1, "source":"source/trees/tree_lush_lod1.glb", "distance":300 } ] }
```
(single type, focused gate; `lods[]` parse added to registry — minimal-but-safe,
full validation + tests still Task 4. distance:300 unused until Task 5.)

### `[LIGHTSLOT v1]` — single-LOD baseline vs 2-LOD (v0.3, `--validate --frames 20
-mission mc2_01`, MC2_LIGHTSLOT_TRACE=1, exit 0, gl_errors=[], shader_errors=[])

| Run (tc1_1 isolated) | K (instances, in-view@emit) | U (unique_light_slots, activeLOD=0 set) | baked_slots B (recipe high-water) | table_count | lod_bake_count | **lod_distinct_slots** |
|---|---:|---:|---:|---:|---:|---:|
| **1-LOD baseline** | 67 | 67 | 982 | 983 | 0 | 0 |
| **2-LOD (LOD0+LOD1)** | 69 | 69 | **1130** | **1131** | **148** | **148** |

(empty manifest no-mod identity: `override_tree_types=0 baked_slots=982 table_count=983
lod_bake_count=0` — stock byte-identical, exit 0, gl=[]/shader=[].)

### GATE VERDICT: **STOP** — U grows toward K×M (LOD1 does NOT dedup to LOD0)
- `registered_recipes`/`baked_slots`: 982 → **1130** = **+148**, exactly the LOD1
  recipe count (tc1_1 = 148 instances). ✓ LOD1 registered as expected.
- **`lod_distinct_slots=148` with `lod_bake_count=148` → ratio 1:1.** Every one of
  the 148 LOD1 instance bakes resolved to a DISTINCT `lightData_` slot.
- **`table_count`: 983 → 1131 = +148** — the global light table grew by exactly the
  LOD1 bake count. The LOD1 slots are **genuinely NEW appends**, NOT dedup hits
  against LOD0's existing per-instance slots. (If LOD1 had deduped to each
  instance's LOD0 slot, table_count would have stayed ~983 and lod_distinct_slots
  would have collapsed to ≈0 new.)
- Net: with M LODs registered+baked, the visible-forest light table → **K×M**
  (here K + K for 2 LODs). This is the exact STOP condition the gate exists to
  prevent — building Tasks 4-6 on top would amplify the already-1:1-with-instances
  light-slot growth (lighting-ownership recon §4).
- activeLOD=0 ⇒ the DRAWN image is still LOD0 (U/per_instance_distinct seen by the
  per-frame set tracks LOD0 only at 69; forest renders, exit 0). The K×M growth is
  in the registration-time bake table, not the draw.

### Why LOD1 does not dedup (the failing site)
The per-instance `lightDataIndex` comes from `TG_Shape::GatherGpuObjectLightDataOnly`
(`mclib/tgl_transform.cpp:1367`) → `addLightDataStructureWithPerActorColor` → FNV+memcmp dedup on
the leaf's `lightData_` (`txmmgr.cpp:1290`). The dedup is content-keyed on the
gathered `TG_HWLightsData`. LOD0 and LOD1 of one instance share world POSITION,
but they are DIFFERENT meshes (different first SHAPE_NODE leaf, different
per-vertex light accumulation feeding `lightData_`), so the gathered struct is
byte-DIFFERENT and the memcmp dedup misses → a fresh slot per LOD per instance.
Position alone is NOT the dedup key; the per-leaf gathered light content is, and
that differs per LOD geometry. So the architectural "same position → same gather →
dedup" assumption is FALSE at this site for distinct LOD meshes.

### Recommendation — HALT for the lighting-ownership slice BEFORE Tasks 4-6
Do NOT proceed to distance selection (Task 5) on this axis. The fix is the
separate lighting-ownership slice already scoped in
`docs/model-override-lighting-ownership-recon.md` §5: make the per-instance
`lightDataIndex` resolve through a position-quantized or recipe-keyed shared
light (independent of LOD mesh) so all LODs of an instance — and ideally all
instances of a type in a cell — collapse to a bounded slot set. Re-run this gate;
require `lod_distinct_slots` ≈ 0-new and `table_count` flat across LOD count
before re-opening Task 4. (Per-LOD register+bake plumbing from Tasks 1-3 is
correct and retained; only the light-slot ownership is the blocker.)

### Files / commit
- Asset: `.claude/tree_export_lush_lod1.py`, `data/model_overrides/source/trees/tree_lush_lod1.glb`.
- `mclib/model_override_registry.{h,cpp}`: `ModelOverrideLod` + `lods[]` minimal-safe parse.
- `mclib/bdactor.cpp`: per-LOD `lods[]` import into `treeRenderShape[lod]`; per-LOD
  registration-time light bake (lod>0) + `lod_bake_count`/`lod_distinct_slots`
  gate counters in `[LIGHTSLOT v1]`.
- exit 0, gl_errors=[], shader_errors=[] on all three runs (baseline / 2-LOD / empty).

---

## Slice 1 — `MODEL-OVERRIDE-REGISTRY-0` — SHIPPED

- Commits: `19acd6a1` (vendor nlohmann/json + isolation guard), `4d7c5285` (registry + tests + empty manifest), `8ebbeb6e` (review fixes).
- `mclib/model_override_registry.{h,cpp}` — engine-independent parse → `ModelOverrideRecord`, case-insensitive `resolve(class,name)`, lazy `std::call_once` singleton.
- Validation (logged + dropped, never crash): non-object entries, `type==model`, `renderOnly==true`, `fallback=="stock"`, `scale==1.0`, typed `<class>:<name>` split on first `:` + normalized (trim+lowercase), class ∈ {staticProp,tree}, class-field agreement, safe relative `.glb/.gltf` source (no absolute/drive/`..`), duplicate first-wins.
- Per-entry try/catch added (review C1): nlohmann `type_error` on mismatched field types is caught/dropped, not propagated.
- Tests: 10 blocks / 9 fixtures, MSVC + g++ C++14-clean, `ALL TESTS PASSED`. json isolation OK.
- Shipped default `data/model_overrides/models.json` = `{ "overrides": [] }` (no-mod identity).

---

## Task 0 — `MODEL-OVERRIDE-COLLISION-AUTHORITY-PROOF-0` — VERDICT: **CASE A**

**Collision/footprint is derived from the SAME `TG_TypeMultiShape` pointer the override would replace. A single-shape hook WOULD change gameplay. Render-only requires a DUAL-SHAPE architecture.**

### Evidence (read-only proof)
1. **Buildings:** `BldgAppearance::calcCellsCovered` reads `appearType->bldgShape[currentLOD]` (`mclib/bdactor_terrain.cpp:7`, vertex loop `:2786`) — the **same array** the base loader fills (`bdactor.cpp:211,235` `bldgShape[i]->LoadTGMultiShapeFromASE`).
2. **Trees:** `TreeAppearance::markTerrain` reads `appearType->treeShape[currentLOD]` (`bdactor.cpp:4266`, vertex loop `:4271`) — the **same array** the tree base loader fills (`bdactor.cpp:3385,3408`).
3. **Timing/cache:** `calcCellsCovered` runs once at scenario load (`code/objmgr_io.cpp:330` → `terrobj.cpp:1373`), result cached in `cellsCovered` (`terrobj.cpp:1442`), never re-derived at runtime.
4. **No separate authority:** `BldgAppearanceType` holds only `bldgShape[MAX_LODS]` + `bldgDmgShape` (visual damage); no `.FITS` footprint, no `collisionOffsetX/Y` consumer, no collision-only mesh. Trees identical. Passability is purely vertex-derived from the visual shape.

### Consequence — Slice 2 REDESIGNED to dual-shape (supersedes plan §Slice 2 single-shape redirect)
Do **NOT** redirect the base loader (`bdactor.cpp:217,235,3391,3414`) to `LoadFromFile`. That would overwrite the collision-authority shape.

Instead:
- **Keep** the existing stock load (`bldgShape[i]/treeShape[i] = LoadTGMultiShapeFromASE(stock)`) UNCHANGED → `calcCellsCovered`/`markTerrain` stay bound to stock geometry → collision/footprint identical to stock.
- **Add** a separate render-authority shape — e.g. `bldgRenderShape[MAX_LODS]` / `treeRenderShape[MAX_LODS]` (override-or-null), loaded via `LoadFromFile(aseFileName, "staticProp"/"tree")` AFTER the stock load, only when the registry resolves an override.
- **Route render/cull only** to the render shape when present, else stock: `render()`, `getRadius()`, `OBBRadius`/`highZ` recompute (`bdactor.cpp:693-744`), and static-prop registry submission read the render shape. `calcCellsCovered`/`markTerrain` MUST continue to read the **stock** `bldgShape/treeShape` — never the render shape.
- Bounds recompute then derives from the render (override) mesh → cull/HZB/shadow track the visible geometry; collision stays stock. This satisfies render-only.

### Open design points for Slice 2 planning (next session)
- Where to store the render shape + null-guard every render/cull consumer to fall back to stock (audit all `bldgShape`/`treeShape` readers; split render-vs-collision readers).
- `OBBRadius`/`highZ` currently computed from `bldgShape` in `init` — must compute from the render shape when an override exists.
- Damage states stay stock (out of MVP).
- Memory/lifetime of the extra shape (destroy alongside stock).

**Task 0 gate: CLEARED — CASE A, dual-shape adjustment recorded. No loader code touched.**

---

## Slice 2 — DUAL-SHAPE (APPROVED) — see `docs/model-override-dual-shape-plan.md`

Reader audit table written (gate cleared). Key refinement vs raw audit: two `bldgShape` members exist — type-level `BldgAppearanceType::bldgShape[]` (collision authority, what LoadFromFile fills) vs per-instance `BldgAppearance::bldgShape` (render instance via CreateFrom). Per-instance render readers inherit authority from their CreateFrom source, so the real edit set is ~6 route sites (CreateFrom source + type bounds) + a nullable type-level render shape + accessor. Collision/damage rows all "leave". Full table + Slice 2 Tasks A/B/C in the dual-shape plan.

## Slice 2 — DUAL-SHAPE — SHIPPED

- Commits: `7114fcca` (dual-shape impl), `d4941557` (review fixes).
- Type-level `bldgRenderShape`/`treeRenderShape` (nullable) on the *Type classes + `getBldgRenderShape`/`getTreeRenderShape` (render-or-stock) + `getBldg/TreeCollisionShape` (always stock). Freed null-safe in type `destroy()`.
- Override loaded (Option 1: direct `ImportGeometryFromFile(manifestDir + sourceRelPath, renderShape)` under `#ifdef ENABLE_ASSIMP_IMPORTER`) AFTER the UNCHANGED stock `LoadTGMultiShapeFromASE`. Base-name captured into `bldgBaseName`/`treeBaseName` before the damage block clobbers `aseFileName`. Failure/throw/zero-shapes → delete+NULL → stock fallback (try/catch around import).
- Routed ONLY the 4 per-instance CreateFrom sources (bldg init + setObjStatus NORMAL; tree init + setObjStatus NORMAL) to the render accessor. OBBRadius/highZ reads the per-instance shape → auto-inherits. ALL collision (calcCellsCovered/markMoveMap/markLOS/markTerrain/calcAdjCell) + damage readers UNCHANGED on stock type array (verified by diff: zero collision/damage tokens).
- `assimp_importer.cpp` AABB made vertex-tight AND mesh-local (matches zero-node-pivot render) so routed cull/HZB/shadow bounds are correct.
- Reviews: spec ✅ (render-only invariant held), code-quality ✅ after fixes (#1 mesh-local AABB, #2 registry `manifestDir()` getter replaces hardcoded literal, #3 snprintf, #4 assimp try/catch, #5 cross-ref comment).
- **Build: `mclib` compiles clean (exit 0, mclib.lib, 0 errors)** via VS2022 cmake; worktree provisioned by copying nifty's `3rdparty/cmake`+`lib` (SDL2) — gitignored build deps.
- No-mod identity: empty manifest → resolve NULL → render shapes NULL → accessors return stock → byte-identical.

### Open for Slice 3 (flagged by quality review)
- GPU static-prop batcher registers the stock TYPE shape at setObjStatus, not `bldgRenderShape`. The per-instance render shape (CreateFrom result) is what draws, so likely fine — but confirm the override actually renders in the Slice 3 applied-override smoke (render-completeness, not collision).

## Slice 3 — STATICPROP-MODEL-OVERRIDE-PROOF — PARTIAL (plumbing proven, RENDER INCOMPLETE)

**Verdict:** the override RESOLVE + IMPORT + dual-shape collision-safety path all work and the engine is stable (exit 0, 0 GL errors), BUT the override geometry is NOT yet rasterized — the GPU static-prop batcher draws the stock type shape (see Visual CORRECTION below). Remaining work: register the render shape into the batcher.

Runtime validated on the canonical runtime **`A:/Games/mc2-opengl/mc2-win64-v0.4`**, mission **`mc2_01`** (the validate-skill default `mis0101` does not exist in any deploy dir; the project smoke harness `scripts/run_smoke.py` uses `mc2_01` on v0.4 — that is the real target). Built full `mc2` (0 errors) and deployed exe+pdb+dlls+63 shaders + `data/model_overrides/`.

Asset: reused `tests/fixtures/assets/cube.gltf` (self-contained base64 GLB-equivalent) as `data/model_overrides/source/props/box.gltf` (unit cube).

Discovery: added env-gated `MC2_MODOVERRIDE_TRACE` log of every staticProp/tree appearance name at the resolve site (bdactor.cpp, matches `MC2_ASSIMP_TRACE` convention). One trace run on `mc2_01` listed 56 static props + 11 trees actually loaded.

Proof run (manifest overriding 14 real props → box, `MC2_MODOVERRIDE_TRACE` off):
- **All 14 `[MODOVERRIDE] staticProp 'X': render override applied`** (crateswithtarp, dumpster, geodesicdome, hangar, hqtent, junkpile, lookouttower, Quonset, quonset2, sandbagbunker, sandbagwall, tent, wirefence, woodencrates).
- `--validate --frames 30 -mission mc2_01` → **exit 0, gl_errors=[], shader_errors=[]**, avg 11.4ms. No destroys, no crash.
- **Visual — CORRECTION (initial conclusion was WRONG):** the hangar VANISHED in the override shot, and I first read that as "box rasterized." It is NOT. The GPU static-prop batcher (`g_useGpuObjects`/`static_prop_registry` ON) draws the **registered STOCK type shape** — `registerMultiShape(appearType->bldgShape[i])` at `bdactor.cpp:855` (trees: `treeShape[i]` at `:3781`). The dual-shape override fills `bldgRenderShape`/`treeRenderShape` + the per-instance shape, but the **batcher never registers the render shape**, so the override geometry is never rasterized. The hangar disappeared because the override shrank the per-instance render bounds → the actor was culled / drew degenerate — NOT because the box was drawn. **The Slice-2 quality-review render-completeness flag is CONFIRMED OPEN, not resolved.** Same root cause = override trees don't render. (The bare trees in the shots are unmodified `*dead` variants, which were never overridden.)
- **Fallback:** manifest pointing `hangar` at a missing `.glb` → `[MODOVERRIDE] staticProp 'hangar': import failed … using stock`, exit 0, gl_errors=[]. Stock renders, no crash.
- **No-mod identity:** discovery run with empty manifest = stock baseline (`registry loaded: 0 override(s)`, byte-equivalent to v0.4 stock validate `vcheck.json`).
- **Collision:** code-guaranteed (dual-shape; collision reads stock type array, diff-proven Slice 2) + stable run (+0 destroys). Visual gameplay-collision probe is out of `--validate` scope.

Deploy manifest restored to `{"overrides":[]}` (install not left modded). Trace code is permanent (env-gated, zero-cost when unset).

Limitation: unit-cube box is tiny at MC2 world scale, so the override reads as "prop disappeared" rather than "prop is a visible box". The render-replacement is nonetheless proven by the hangar's disappearance + the applied logs. A larger box asset would make it visually obvious (deferred; not needed for proof).

## Slice 4 — MODEL OVERRIDE RENDERS ✅ (verified 2026-06-02, A/B confirmed)

**RESOLVED via a focused GPU-batcher seam probe (`MODEL-OVERRIDE-GPU-BATCHER-SEAM-PROBE-1`).** Override geometry now rasterizes in-game, verified by same-camera A/B (`.claude/seamfix_before_zoom.png` = empty apron where the hangar was; `.claude/seamfix_after_zoom.png` = a solid override box drawn there). Collision stays stock (dual-shape). Draws untextured (borrowed albedo) — texture binding is the remaining polish.

### The render path had THREE seams (all now fixed), past registration:
The advisor's read was correct — geometry is NOT texture-style; it goes through the immutable mission-load GPU recipe/batcher. Registration + recipe + type-identity were already correct pre-finalize (`CreateFrom` preserves `myType` = the registered render-shape `TG_TypeShape*`). The override was lost downstream:
1. **Admission gate** (commit `566097f0`): `BldgAppearance::isStaticEligible()` rejected the override prop via the `bdAnimationState != -1` gate (stock buildings get a default idle gesture; the override has no anims) → `IsStaticNow()` false → `render()` never `markVisible()`d the recipe → no substrate record → not drawn (and stock displaced → prop vanished). Fix: skip that gate for override-backed types (`bldgRenderShape != null`) with no real anims; stock path byte-identical. (Trees have no such gate — already admitted.)
2. **Draw-consumption / multidraw cull** (commit `4171be63`): override packets import untextured (`NULLTXM`, `gosTextureHandle=0xFFFFFFFF`) → coalesce per-packet texture-array build assigns `layerForPacket=-1` → packet dropped from `s_sortedPacketOrder` (same skip as the damage-shape orange-ghost guard, bdactor.cpp:262) → override contributes ZERO draw commands. Fix: added `isOverride` flag to `GpuStaticPropType`/`registerMultiShape`; an override packet that would skip with layer −1 is routed to a valid layer (0) instead of dropped. Stock/damage keep the −1 skip (no flag) → no regression. Emitted packets 130→134; override types now contribute draws.
3. (necessary precondition) **register render shape before finalize** via `registerStatic` (committed earlier) so the override type is in the immutable VBO.

### Verified facts
- `[SEAMPROBE] OVERRIDE_ROUTE type=33/41/42/43 -> layer=0`; coalesce armed `types=217`; before/after A/B shows the box appears (I viewed both images).
- Prop (hangar→bigbox) + trees (tc1_*→tree_small) both route + draw.
- Caps 16M/8M, gltfpack built — real, retained.

### Remaining polish (NOT blockers — override now renders)
- ~~**Texture binding**~~ — DONE, see Slice 5 below.
- Leaf alpha-MASK for tree leaf cards; per-instance lighting/perf.
- Add a model-override row to `docs/asset-pipeline.md` (§7).

## Slice 5 — MODEL-OVERRIDE TEXTURE BINDING ✅ (2026-06-02)

Override trees now render with their OWN glTF textures (bark + leaf + trunk),
not the borrowed wrong layer-0 albedo. Verified on **v0.3 / mc2_01**, exit 0,
0 GL errors, collision untouched (only `assimp_importer.cpp` + `bdactor.cpp`
changed; stock byte-identical).

### Target
Dead tree types (bare → leafy, fewer instances, unmistakable). Discovery trace
on mc2_01: dead types present = maple1dead, maple2dead, maple3dead, oak1dead,
oak3dead (all have instances; pool peak jumps to 1.68M with all 5 overridden →
geometry really instances). `oak1dead` chosen as the single-type focus; all 5
used for the visual A/B (more instances = clearer change).

### Mechanism (3 parts)
1. **Importer assigns real texture NAMES** — `mclib/assimp_importer.cpp`
   `DeriveMC2TextureName()` (new) + rewritten `BuildTextureList()`. For a
   material WITH a base-color image (BASE_COLOR or DIFFUSE channel), derive the
   MC2 texture name from the image stem: resolve embedded `*N` →
   `scene->mTextures[N]->mFilename`, strip dir + source ext, sanitize
   (lowercase, `[a-z0-9_-]`, else `_`), clamp to fit `TG_Texture::textureName[256]`,
   append `.tga` (MC2 stores diffuse names WITH `.tga` — see msl.cpp shadow-X
   strlen-4). Truly-untextured material → keeps `NULLTXM`. Trace
   (`MC2_ASSIMP_TRACE=1`): material 0→`tree_small_02_branch_diff_1k.tga`,
   1→`tree_small_02_leaves_diff_1k.tga`, 2→`tree_small_02_diff_1k.tga`, 3→NULLTXM.
2. **Deployed loose TGAs** — extracted the 3 embedded diffuse images from
   `tree_small.glb` (branch jpg, leaves PNG, trunk jpg), converted via PIL →
   `data/tgl/128/<name>.tga` (loose path; `fileExists` uses literal `_stat`, so
   placed under `128/` directly — the file.cpp size-subdir strip is only a
   fallback). NOTE: the leaves diffuse has NO alpha channel (RGB) → leaf cards
   render OPAQUE (no cutout); acceptable per MVP priority.
3. **Type-render-shape texture resolution** — `mclib/bdactor.cpp`
   `LoadOverrideRenderShapeTextures()` (new static helper) called right after a
   successful override import for BOTH bldg + tree. Root cause it fixes: the
   stock per-instance loaders (bdactor.cpp ~3786/935) load textures onto the
   CreateFrom'd PER-INSTANCE shape, NOT the TYPE render shape that the GPU
   batcher registers + draws → the type shape's `listOfTextures[].gosTextureHandle`
   stayed 0xFFFFFFFF → batcher saw W<=0 → borrowed layer-0. The helper mirrors
   the stock loader (loadTexture by name → `SetTextureHandle` on the type
   multishape, which propagates to all type sub-shapes). The batcher's
   `4171be63` borrowed-layer-0 route is now a genuine FALLBACK only — it fires
   solely when W<=0 (texture unavailable); with handles resolved the packet
   takes the normal real-layer path (gos_static_prop_batcher.cpp ~2911).

### Verified facts (trace + viewed images)
- `[MODOVERRIDE_TEX] slot=0/1/2 -> gosHandle=933/934/935 alpha=0`, 0 NOT FOUND.
- `[SEAMPROBE] tree buildRecipe HIT … typeID=199/200/201` for the override tree.
- **`OVERRIDE_ROUTE` count = 0** and no `layer=-1 SKIP` for the override types →
  override packets resolve their OWN real texture layers, not the borrowed one.
  `[COALESCE] armed … unique_tex_on=37` (the 3 new tree textures added).
- exit 0, gl_errors=[], peak_textures 235→238 (=+3 deployed textures).
- A/B (`.claude/before.png` empty manifest vs `.claude/all5.png` 5 dead types,
  crop `.claude/ab_before_all5.png`): the left tree cluster gains denser leafy
  GREEN-textured / brown-bark canopies where bare dead trees were. Camera_motion=1
  offsets the two runs slightly so pixel-diff is noisy; the trace is the decisive
  proof, the images corroborate.

### Files / commits
- `mclib/assimp_importer.cpp`: `DeriveMC2TextureName` + `BuildTextureList` rewrite.
- `mclib/bdactor.cpp`: `LoadOverrideRenderShapeTextures` + 2 call sites (tree ~3642, bldg ~352).

### Remaining follow-ons
- **Leaf alpha-MASK**: source leaves diffuse is RGB (no alpha) → opaque leaf
  cards. Need a leaf texture WITH an alpha channel (or a separate opacity map
  bound to ALPHA_TEST_BIT, 0.5 cutoff) for true cutout foliage.
- **Branch KHR_texture_transform**: the branch material has a UV offset/scale
  (`offset[0,0.4] scale[3,0.6]` + texCoord 1) the importer does not apply →
  branch UVs may tile/shift wrong. Trunk + leaves (no transform) are correct.
- BC7 `.ktx2` cook of the 3 TGAs for the compressed path.
- Per-instance lighting/perf for heavy override forests.

### (history) earlier RETRACTION — kept for the record
An interim revision claimed trees render when they did not (misread dark stock trees), and a later revision correctly retracted that. The magenta-box test then proved no-render, which led to this seam probe that actually fixed it. Net: the retraction was right at the time; the box now genuinely draws. A definitive test (override the on-screen **hangar** with a bright **magenta 12-unit box**) proves the override geometry is **NOT drawn** on EITHER v0.4 or v0.3: the hangar vanishes (stock shape displaced by the override) and **no magenta box appears**. Screenshots `.claude/magenta_hangar_test.png` (v0.4), `.claude/v3_magenta_hangar.png` (v0.3).

### What actually happens
- `[MODOVERRIDE] render override applied` only means the glTF IMPORTED into the render shape — NOT that it draws.
- Batcher summary: `submit_buildings=56 … gpu_drawn_instances=0`; the override type appears in `[GPUPROPS] late registerType … CPU-fallback`.
- The `BldgAppearance::registerStatic` trace (gated on `bldgRenderShape != null`) did NOT fire, while the resolve-site trace DID → **`registerStatic` runs while `bldgRenderShape` is still null**, i.e. the override is imported into the render shape AFTER the mission-load registration pre-pass + the one-shot immutable `finalizeGeometry`. So the override geometry never enters the drawable VBO; meanwhile the per-instance render shape (CreateFrom'd from the override once it loads) displaces the stock shape in the legacy path → the prop disappears.

### Real root cause / what's still needed
The override must be RESOLVED + IMPORTED into the render shape **before** the GPU batcher's mission-load registration (so `registerStatic` sees it and `registerMultiShape` stages it pre-`finalizeGeometry`). Current Task-B import timing (appearance-type load) is evidently too late relative to the pre-pass/finalize for these appearances. Options: (a) resolve+import overrides during appearance-type construction guaranteed before `registerStaticPropsForMissionLoad`; (b) a batcher geometry rebuild (`resetForRestore` path) after overrides load; (c) verify appearance-type load order vs the pre-pass. NOT yet solved.

### Honest status of the whole effort
- PROVEN: registry (Slice 1), dual-shape collision authority (Task 0/Slice 2 — collision reads stock, diff-verified), resolve + glTF import, stock fallback, no-mod identity, builds clean.
- NOT working: actual override RASTERIZATION (render). The render integration is incomplete.
- Caps 16M/8M + gltfpack build are real/useful but did not fix render.

### (earlier "what made it work" notes below are INVALID — kept struck-through for history only)

### What made it work
1. **Register the override RENDER shape into the batcher BEFORE `finalizeGeometry`** — added `registerMultiShape(getBldg/TreeRenderShape(i))` at the TOP of `BldgAppearance::registerStatic` / `TreeAppearance::registerStatic` (`bdactor.cpp`). The mission-load pre-pass (`registerStaticPropsForMissionLoad`, default-on) runs before `StaticProp::finalizeGeometry` (mission.cpp:3198), so the override type-shapes enter the immutable VBO (`s_typeIndex`) and `buildRecipeFromShape` succeeds → GPU-instanced draw. (The 2 `[GPUPROPS] late registerType` lines are unrelated pre-existing stock types — verified: their pointers don't match any override leaf.)
2. **Pool caps sized for override forests** — `code/mission.cpp` pools 16M (vertex/color/shadow) / 8M (face/triangle), `mission2.cpp` startVertices 16M, `MC_MAXFACES` 8M. Per-instance lighting/transform buffers scale with override mesh complexity × instances; the detailed tree (~21k verts × ~147 instances of the 6 overridden types) peaks ~12.6M.
3. **gltfpack for mesh weight** — built the vendored `gltfpack` (`3rdparty/meshoptimizer/gltf/gltfpack.cpp`; standalone build in `.claude/gltfpack_build`). `gltfpack -si 0.1 -sa -kn` crushes leaf-card foliage (which Blender collapse-decimate can't) → `tree_light.glb` (~4.3k tris) for cases needing many instances. The detailed `tree_small.glb` (42k) also fits the 16M pools.

### CRITICAL debugging lesson (cost hours)
A hung `mc2.exe` (validate process that didn't exit) held a **lock on the deployed `mc2.exe`**, so `cp`/`Copy-Item` of new builds **silently failed** → runs used a STALE exe → intermittent "no override / no trees" that looked like code bugs. Fix: `taskkill /F /IM mc2.exe /T` before every deploy-copy, and verify the copy. Also: `strings` is broken in this env (0 output) and bash `grep` mis-reads the UTF-16 stderr logs — use PowerShell `Select-String` for engine logs.

### Remaining polish (not blockers)
- **Untextured** → trees render dark (geometry only; importer sets texture NAMES, MC_TextureManager resolves by name — the glTF's embedded leaf/bark textures aren't wired into MC2's texture set). Material/texture binding is a follow-on.
- **Leaf alpha**: tree leaf material is glTF BLEND (engine supports MASK/alpha-test only) — set CLIP in the Blender export; full leaf cutout needs the leaf texture's alpha bound.
- **Perf**: detailed tree × instances → avg ~66ms (heavy per-instance lighting). Lighter tree (gltfpack) or a GPU per-instance-lighting path (avoids per-vertex pool storage) is the perf follow-on.
- Add a **model-override** row to `docs/asset-pipeline.md` (§7 mandate) once finalized.

---

## (superseded) earlier RENDER-BLOCKED analysis — kept for history

Tree asset: real PBR tree `tree_small_02_1k.gltf` (user-supplied) → Blender headless decimate (`.claude/tree_export.py`) → `data/model_overrides/source/trees/tree_small.glb`. Decimate floored at ~42,885 tris (leaf cards are disconnected quads — collapse-decimate can't reduce; **meshopt_simplifySloppy / gltfpack would** — vendored at `3rdparty/meshoptimizer`, CLI `gltf/gltfpack.cpp`). Leaf material set CLIP/MASK.

Runtime (v0.4 / mc2_01): override resolves + imports + 6× `render override applied`, exit 0, 0 GL errors — **but the override geometry does NOT render** (confirmed visually, repeatedly).

### ROOT CAUSE — one-shot immutable GPU VBO
`GpuStaticPropBatcher::finalizeGeometry()` (`GameOS/gameos/gos_static_prop_batcher.cpp`): `if (s_geometryFinalized) return;` then `glBufferStorage(..., flags=0)` → a **single, fully-immutable** shared VBO/IBO built once. The batcher draws static props/trees from this VBO via per-type recipes (`s_typeIndex: unordered_map<const TG_TypeShape*,uint32_t>`). A type registered **after** finalize → `[GPUPROPS] late registerType … CPU-fallback for this type`. The override render-shape is new geometry; it is not in the immutable VBO, so it falls to the per-instance CPU path, which:
  (a) does not visibly rasterize under `g_useGpuObjects`, and
  (b) retains a per-instance `CreateFrom` copy → TGL pools saturate deterministically (4,939,908/5,000,000 = ~235 of ~1000 trees before overflow). The pools are now 10x'd (5M/2M) but the per-instance-retention model still can't hold ~1000 heavy instances — and the GPU path is the only viable one for that count (geometry stored once).

### What this means
The dual-shape design is correct (collision stays stock — proven), and resolve/import/registration are wired, but **a render-only model override of a GPU-batched static prop/tree requires its geometry to be in the immutable batcher VBO**, i.e. registered **before the first `finalizeGeometry`**. Fixes attempted (register render shape at `init` 855/3787 + at `registerStatic` top, before the pre-pass `finalizeGeometry` at mission.cpp:3198) did NOT land it — 2 `late registerType` persist, implying finalize is already done by the time the override type is first seen (likely an earlier menu/shell finalize, or staging-VBO sizing reserved pre-override). Needs deeper batcher work, NOT another blind patch.

### Candidate real fixes (for the expert/next session)
1. **Pre-register override geometry before the first `finalizeGeometry`** — resolve the mod manifest at mission/menu boot and register all override render-shapes into the staging VBO before it's frozen. (Cleanest; matches the immutable-VBO design.)
2. **Batcher VBO rebuild hook** — allow `finalizeGeometry` to re-stage when a mission with overrides loads (there's a `resetForRestore()` path in `GameAdapters/StaticPropRenderAdapter` / saveload.cpp:1628 worth reusing).
3. **meshopt runtime clamp** (task b) — clamp override meshes to a budget so even CPU-fallback is survivable; helps but does not fix the no-render.
Align any solution with `docs/asset-pipeline.md` (canonical; render-owner `GpuStaticPropBatcher`, gamedata-owner `BldgAppearance`/`TreeAppearance`) and add a **model-override** row there per its §7.

### Committed this session (correct + endorsed, even though render incomplete)
- Caps 10x: `code/mission_init.cpp:437-450` (5M/2M), `code/mission2.cpp:111`, `MC_MAXFACES` `mclib/txmmgr.h:49`. (User: caps were artificial CPU-era limits.)
- Batcher registers the RENDER shape: `bdactor.cpp` init sites (855/3787) + `registerStatic()` tops (both bldg + tree).
- Env-gated `MC2_MODOVERRIDE_TRACE` discovery log.
NOTE: the Slice-3 "static-prop render replaced" claim was a misread (bounds-cull, not rasterization) — same root cause. Static props have the same limitation; only the dual-shape collision-safety + resolve/import are proven.

---

## TREE OVERRIDE — VERIFIED RENDERING (2026-06-02, on-screen live-tree test)

**Definitively confirmed (two ways, after repeated false positives on dead trees):**
- The earlier "dead tree" overrides showed NO visible change because mc2_01's dead-tree instances (maple/oak dead) are OFF-CAMERA in the validate view — not because trees can't render. User was right to distrust those shots.
- Overriding the **on-screen LIVE tree types** (tc1_1..4, palm1, palms) makes the change unmistakable:
  - magenta/box override → the left tree cluster becomes a field of boxes (`.claude/vlt_magenta_livetrees.png`) — proves the tree override geometry RASTERIZES.
  - textured `tree_small.glb` override → the stock leafy cluster is replaced by the imported tree geometry with its OWN resolved textures (`[MODOVERRIDE_TEX] gosHandle=860/861/862`, no OVERRIDE_ROUTE → real layers, pool 81% = instanced) (`.claude/vlx_textured_livetrees.png`). exit 0, 0 GL errors.

**Net:** model overrides render in-game for BOTH static props (hangar magenta box, verified earlier) AND trees (live-tree box + textured, verified here), via the GPU static-prop batcher, with collision stock (dual-shape). 

**Lesson:** ALWAYS test overrides on instances confirmed ON-CAMERA; a no-visible-change result on off-screen instances is not evidence of non-render. Only an unmistakable on-screen change (box) counts.

**Quality polish remaining (renders, but rough):** decimated foliage is sparse; leaf cards opaque (no alpha-MASK; source leaf diffuse is RGB, no alpha); modest scale; branch KHR_texture_transform UV not applied. None block "renders"; they affect how good it looks.

---

## Slice 6 — LEAF ALPHA-CUTOUT + SCALE (2026-06-02, viewed) — leaves now cut out

Override trees now render with **alpha-tested leaf cards** (see-through gaps
between leaves, not opaque blocks) and a larger, tree-reading scale. Verified on
**v0.3 / mc2_01**, exit 0, gl_errors=[], shader_errors=[]. Collision untouched
(only `assimp_importer.cpp` + asset/scale changed; stock byte-identical, empty
manifest = stock).

### 1. Leaf alpha-cutout (highest impact) — DONE
- **Authored an RGBA leaf TGA** (`.claude/make_leaf_alpha_tga.py`): the source
  `tree_small_02_leaves_diff_1k.jpg` is an RGB leaf atlas on a PURE BLACK
  background; keyed the background out by luminance (soft 18->42 ramp) into an
  8-bit alpha channel. Written as uncompressed 32-bit **BGRA, bottom-left
  origin (desc=8)** — byte-format identical to the engine's other prop TGAs
  (verified by matching `tree_small_02_leaves_diff_1k.tga`'s header + pixel
  order). Deployed to `data/tgl/128/a_tree_small_02_leaves_diff_1k.tga`.
- **Wired alpha-test via the engine's "a_" naming convention.**
  `mclib/assimp_importer.cpp` `DeriveMC2TextureName()` now detects an
  alpha-cutout material (glTF `$mat.gltf.alphaMode` == MASK/BLEND, else material
  name contains leaf/leaves/foliage) and **prefixes the derived texture name
  with `a_`**. `bdactor.cpp LoadOverrideRenderShapeTextures` already loads
  `a_`-prefixed names as `gos_Texture_Alpha` + `SetTextureAlpha(true)`, which
  propagates `textureAlpha` to the leaf type-shapes; the static-prop batcher's
  per-packet alpha re-resolve (`GameOS/gameos/gos_static_prop_batcher_finalize.cpp:1660`,
  `src->listOfTextures[slot].textureAlpha`) then sets
  `STATIC_PROP_FLAG_ALPHA_TEST` (0.5 cutoff in `static_prop.frag`).
- **Trace proof:** `[MODOVERRIDE_TEX] slot=1 name='a_tree_small_02_leaves_diff_1k.tga'
  -> gosHandle=861 alpha=1` (was `alpha=0` pre-slice). Branch/trunk slots stay
  `alpha=0` (OPAQUE). All 6 live types apply.
- **Viewed:** `.claude/tree_closeup.png` (8x crop of an override tree on the
  sandbag compound, frames=8 framing) — brown trunk/branches + GREEN leaf cards
  with terrain visible THROUGH the gaps between leaves = genuinely alpha-cut,
  not a solid green block. `.claude/left_apron_zoom.png` shows the cluster of
  override trees on the apron compounds.

### 2. Fullness + scale
- Re-exported via `.claude/tree_export.py` (TARGET_TRIS 12k->40k, SCALE 20->32).
  Mesh tris floor at **42885** regardless of target — the leaf cards are
  disconnected quads collapse-decimate cannot reduce, so the canopy is already
  at full source density; the bump just stops decimate from thinning it. Pools
  16M/8M comfortably hold the 6 live types' instances (no overflow; exit 0).
  Scale 32 reads as a small real tree vs vehicles/hangar.

### 3. Branch KHR_texture_transform — DEFERRED (minor)
The branch material carries `offset[0,0.4] scale[3,0.6]` on texCoord 1; the
importer reads only `mTextureCoords[0]` (assimp_importer.cpp:412) and does not
apply the transform. Applying it needs per-material `$tex.uvtrafo` query + the
right UV set — non-trivial, and only slightly mis-tiles branch BARK (trunk +
leaves have no transform and are correct). Deferred per priority; documented.

### Honest assessment
Leaves are unmistakably alpha-cut and the tree reads as a real leafy tree (trunk
+ branches + cut-out green leaves). The canopy is **modest/sparse** because the
SOURCE asset (`tree_small_02`) is a small young sapling, not a dense mature tree
— that is the asset's nature, not a decimation artifact (tris are at the source
floor). A denser-canopy result would require a different source tree asset.

### Files / commands
- `mclib/assimp_importer.cpp`: `DeriveMC2TextureName` alpha-prefix detection.
- `.claude/make_leaf_alpha_tga.py`: leaf RGBA TGA author (repeatable).
- `.claude/tree_export.py`: TARGET_TRIS=40000, SCALE=32.
- Deploy manifest = 6 live tree types -> `source/trees/tree_small.glb`.

---

## Slice 7 — LUSH CANOPY via leaf-card-PRESERVING reduction (2026-06-03, viewed)

**The Slice-6 "tris floor at 42885 / source is a sapling" conclusion was WRONG.**
Inspected the source gltf accessors directly: `tree_small_02_1k.gltf` has 3
primitives — branches 62k verts/94k tris, **leaves 1.7M verts / 1.94M tris**
(30,250 disconnected leaf-card islands), trunk 15k/28k. The old
`.claude/tree_export.py` runs a Blender **COLLAPSE** decimate over ALL meshes
incl. leaves → crushes the 1.94M-tri canopy to ~42k total = the gutted sparse
sticks (`.claude/tree_closeup.png`). Collapse-decimate DOES reduce the leaf cards
(they share verts within each card) — it just destroys them.

### Fix — `.claude/tree_export_lush.py` (NEW, leaf-card-preserving)
- Separate the mesh by material; keep trunk + branches FULLY intact.
- Leaves: union-find the faces into loose-part ISLANDS (= leaf cards), keep a
  random fraction of WHOLE cards (`--keep`), delete the rest. NO collapse on
  leaves → every surviving card stays a full leaf quad. Leaf mat → CLIP @0.5.
- `--keep 0.30 --scale 32` → kept 9,075/30,250 cards → **leaves 583k + trunk/branch
  122k = 706,628 tris**. Output `data/model_overrides/source/trees/tree_lush.glb`.

### Pools raised to 32M/16M (the per-instance retention wall)
- `code/mission_init.cpp:444-456` vertex/color/shadow 16M→**32M**, face/triangle 8M→**16M**.
- `code/mission2.cpp:111` startVertices 16M→**32M**.
- `mclib/txmmgr.h:49` `MC_MAXFACES` 8M→**16M**.

### THE CONSTRAINT, quantified (this is the GPU-instancing signal)
- `tc1_1` ALONE = **148 on-screen instances** (`registerStatic` count). The
  706k-tri lush mesh × 148 → face pool peak **15,798,888 / 16,000,000 = 98%**,
  triangle 97%. ONE live type at this fullness already pegs the 32M/16M pools.
  A SECOND type would overflow and silently drop geometry.
- So: a lush 706k canopy fits **exactly ONE on-screen live type (~148 instances)**
  at 32M/16M pools. Fuller (higher --keep) or more types is impossible under the
  per-instance-retention model — the static-prop batcher retains per-instance
  geometry proportional to mesh×instances. **The proper fix is GPU instancing**
  (geometry stored ONCE in the VBO, transforms per instance, no per-instance pool
  retention) — recommend as the next slice rather than growing pools further.

### Verified (trace + VIEWED images)
- `[MODOVERRIDE] tree 'tc1_1': render override applied (...tree_lush.glb)`.
- `[MODOVERRIDE_TEX] slot=0/1/2 -> gosHandle=864/865/866`, leaf slot
  `a_tree_small_02_leaves_diff_1k.tga alpha=1` (alpha-cut leaves).
- `[SEAMPROBE] tree buildRecipe HIT name=tc1_1 ... typeID=46/47/48` for all 3
  child shapes → override geometry entered the immutable VBO and draws. NO
  OVERRIDE_ROUTE / layer=-1 SKIP for override → real texture layers.
- `--validate --frames 20 -mission mc2_01` exit 0, gl_errors=[], shader_errors=[],
  avg 101ms (heavy — 148× 706k mesh; perf is the instancing follow-on).
- Images I viewed: `.claude/lush_apron_tree_zoom.png` — brown trunks + a DENSE
  full green leafy canopy with terrain/sky showing through alpha-cut leaf gaps;
  dramatically fuller than the gutted `.claude/tree_closeup.png` (sparse sticks).
  `.claude/lush_tc1_1.png` full frame; `.claude/ov_apronleft.png` apron cluster.
  NOTE: same-camera A/B vs stock is unreliable — the validate flythrough lands
  at a different camera pose between runs; the buildRecipe-HIT trace + the viewed
  override-tree zoom are the decisive proof, not a pixel diff.

### Deploy state
- v0.3 manifest = `tc1_1 -> source/trees/tree_lush.glb` (the lush demo).
  `tree_lush.glb` deployed alongside the existing 3 tree TGAs (incl the `a_`
  alpha leaf) — no new textures needed (same materials as tree_small.glb).
