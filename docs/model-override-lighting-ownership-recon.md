<!-- doc-status: stale | reviewed:2026-10-11 -->
# MODEL-OVERRIDE — Light-slot ownership recon (Task 0 M2 gate result: **STOP**)

**Status:** DIAGNOSTIC measurement result + verdict. The TREE-OVERRIDE-LOD-MVP-1
Task 0 light-slot cardinality PROOF GATE was run and **FAILED the bound**:
per-instance light-slot growth is real. Per the plan's STOP condition, the LOD
plan is **HALTED** — do NOT proceed to LOD Task 1 until a separate
lighting-ownership slice collapses per-instance light slots to a bounded set.

**Branch:** `claude/model-override-system-recon-1` (repo `A:/Games/mc2-model-override-recon`).
**Gate instrumentation:** env-gated `MC2_LIGHTSLOT_TRACE`, one summary line per
map at mission_ready (`[LIGHTSLOT v1]`). Pure counting, zero behavior change.
Build: mclib + mc2, RelWithDebInfo, 0 errors. Run: `--validate --frames 20
-mission mc2_01` on `A:/Games/mc2-opengl/mc2-win64-v0.3`, exit 0, **0 GL errors,
0 shader errors**.

---

## 1. Measured numbers (the evidence)

| Run | K (static_prop_instances, in-view at emit frame) | N (override_tree_types) | R (registered_recipes) | **U (unique_light_slots)** | **D (per_instance_distinct)** | H (dedup_hits) | B (baked_slots) | table_count |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **Override forest** (lush 6-type, tree_lush.glb) | 29 | 6 | 982 | **29** | **29** | 264 | 982 | 983 |
| **Stock baseline** (empty manifest) | 119 | 0 | 982 | **119** | **119** | 299 | 982 | 983 |

Raw lines:
```
[LIGHTSLOT v1] static_prop_instances=29  override_tree_types=6 registered_recipes=982 unique_light_slots=29  dedup_hits=264 baked_slots=982 per_instance_distinct=29  table_count=983
[LIGHTSLOT v1] static_prop_instances=119 override_tree_types=0 registered_recipes=982 unique_light_slots=119 dedup_hits=299 baked_slots=982 per_instance_distinct=119 table_count=983
```

## 2. Verdict: **STOP** — U and D scale with INSTANCES (K), not types

**U == D == K in BOTH runs** (override 29/29/29; stock 119/119/119). The ratio
U/K = D/K = **1.0** exactly. Every static-prop tree instance consumes its OWN
distinct light slot. There is no collapse to ~O(types). This is the plan's
explicit STOP condition (`unique_light_slots ≈ K`, dedup ineffective).

(The override run's K=29 vs stock K=119 is only because fewer instances are
in-view at the emit frame with the heavier override mesh + culling; it is NOT a
reduction in per-instance growth — U still equals K in both. The decisive fact
is the 1:1 ratio, identical in both manifests.)

## 3. Why dedup does not bound it — the failing allocator site

The per-instance `lightDataIndex` that feeds `GpuStaticPropRegistry::markVisible`
(`mclib/bdactor.cpp:4226`) and the GPU SSBO comes from
`treeShape->getCachedGpuLightIndex()` (captured at `mclib/bdactor_static.cpp:88` /
`:4700`). That index is set by:

- `TG_MultiShape::CacheGpuLightData()` (`mclib/msl_transform.cpp:545`) → first SHAPE_NODE
  leaf `TG_Shape::GatherGpuObjectLightDataOnly()` (`mclib/tgl_transform.cpp:1364`)
- → `MC_TextureManager::addLightDataStructureWithPerActorColor()`
  (`mclib/txmmgr_lightrow.cpp:409`) → `GatherLightsParameters()` then content dedup in
  `addLightDataStructure()` (`mclib/txmmgr_lights.cpp:586`, FNV+memcmp `:1285-1297`).

`GatherLightsParameters` produces a **position-dependent** `TG_HWLightsData`
(per-instance `getTerrainLight` at the tree's world position + per-actor color
decompose). Two trees at different terrain cells therefore produce
**byte-different** light structs, so the FNV hash differs and the memcmp dedup
**never matches across instances**. Result: `addLightDataStructure` appends a
fresh slot for every instance → U grows 1:1 with K. The 264/299 `dedup_hits`
are intra-instance / per-frame re-emits of the *same* instance, not cross-
instance collapse.

**The `baked_slots=982` table is a DIFFERENT, already-bounded table** and is the
source of the recon's earlier "lighting is O(1)" claim — it is correct *about
that table*. `s_bakedStaticLight` (`txmmgr.cpp:1191`) is keyed by monotonic
`recipeIndex` (one per type×leaf, NOT per instance), so it stays at 982 (the
whole mission's leaf-recipe count) regardless of instance count — unchanged
between the 29-instance and 119-instance runs. But the **per-instance**
`lightDataIndex` that actually drives the draw is the position-keyed gather
slot, NOT the recipe-keyed baked slot, and *that* is what scales with K. The
prior recon (`docs/model-override-lighting-lod-recon.md` §3.3/§3.5) flagged the
gather as "position-dependent ... MAY produce distinct slots"; this measurement
confirms it DOES, 1:1.

## 4. Consequence for LOD

LOD multiplies the active shape count per instance. If per-LOD recipes each run
the same per-instance position-keyed gather, the light-slot table would grow
toward K×(active LODs) for the visible forest — strictly worse, on an already
1:1-with-instances axis. Building LOD on top of this would amplify the unbounded
light-slot growth, exactly the "do NOT build LOD on per-instance light growth"
failure mode the gate exists to prevent.

## 5. Recommended separate lighting-ownership slice (do this BEFORE LOD)

Pick one (sequenced, cheapest first):

1. **Shared per-type (or per-cell-quantized) light** — quantize the terrain
   light sample to a coarse grid (or share one gathered light per override
   *type*), so many instances dedup to a handful of slots. Lowest effort;
   matches the "trees are mission-static, lighting is position-derived constant"
   assumption already documented. Risk: visible lighting banding if the grid is
   too coarse — tune cell size.
2. **Instanced GPU light gather** — move the per-instance terrain-light sample
   to the GPU (sample a lightmap/terrain-light texture in `static_prop.vert`
   by instance world position) and drop the per-instance CPU slot entirely, so
   the SSBO holds only a bounded shared set. Highest ROI, most work.
3. **Recipe-keyed light reuse** — make the per-instance `lightDataIndex`
   resolve through the already-bounded recipe baked table instead of the
   position-keyed gather, accepting per-recipe (not per-instance) lighting
   fidelity. Smallest code change; verify it does not resurrect the black-tree
   class (must re-arm `needsFullBakeNextFrame` per the LOD-swap spec).

After any of these lands and a re-run shows `U`/`D` ~O(types/recipes) and NOT
~K, re-open TREE-OVERRIDE-LOD-MVP-1 Task 1.

## 6. Instrumentation left in place (env-gated, demote-not-delete)

- `MC2_LIGHTSLOT_TRACE=1` → `[LIGHTSLOT v1]` summary line at mission_ready.
- Files: `mclib/txmmgr.cpp` (dedup/actor-key hit counters + `mc2LightSlot*`
  accessor free fns), `mclib/bdactor.cpp` (`mc2_lightslot_trace` namespace:
  per-frame distinct-slot/instance/type accumulation at the tree static
  capture + markVisible sites; one-shot emit ~8 frames after mission_ready).
- No draw/state change; default-off; safe to leave in for re-measurement after
  the lighting-ownership slice.

## 7. Perf-measurement convention (owner ruling 2026-06-03)

For frame-rate / call-count / hot-zone decomposition on this work (the
lighting-ownership re-gate, the LOD perf-capture, the 2.29s `Render.GpuStaticProps`
split), use **`cost-split`** CostSplit buckets (per-frame ns accumulators + call
counters + summary line) — methodology `.claude/skills/cost-split-recon-bucket-design.md`,
shipped pattern at `gos_terrain_indirect.cpp` cost-split, env `MC2_*_COST_SPLIT`.
**Cut the summary cadence to every 10 frames** (not the default 600) so a short
`--validate --frames 20` run actually emits the line and re-gate runs stay quick.
Respect the 100ns Tracy-floor / <10% instrumentation-overhead rule (don't time
per-element work inside the per-instance loop — wrap the loop + count N). Relay the
summary line back after each run (operator-visibility pattern 1).
