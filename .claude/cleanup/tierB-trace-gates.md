<!-- doc-status: stale | reviewed:2026-10-11 -->
# Tier B trace/diag gates: cleanup verdicts

Source: `py -3 tools/repo_intel/repo_query.py dead-gate-scan` (Tier B, 106 gates). Generated 2026-10-08. Read-only analysis; nothing in the repo was edited.

## Method

- **Default**: all 106 gates are opt-in env reads (`getenv`, `atoi`, `ParseEnvBool`, `envFlag`, `lightClampEnvFlagDefaultOff`) with no default-on branch; default is **OFF (unset)**. The tool's own `default` field is null for all of them.
- **Sites**: source lines in `code/`, `GameOS/`, `mclib/`, `RenderWorld/`, `editor/`, `tools/`, `scripts/` (`.cpp .h .hpp .cc .c .inl .py`) containing the gate name, excluding `//` comments. Counted with an in-memory grep, not the tool's reader list, so it can differ slightly from `dead-gate-scan`.
- **Guarded lines (estimate)**: for each site, brace-matched block length starting at the site line (single statement when no braces). Sum across sites. Rough; it over-counts sites that only read the flag into a variable.
- **Last commit**: `git log -1 -S"<GATE>" --format=%cs` (last commit that changed the occurrence count). Stale = before 2026-08-09 (>60 days).
- **Verdict rules** (applied mechanically, in this order):
  - **KEEP**: name appears in repo `CLAUDE.md`, `memory/HANDOFF-2026-10-08b-vk-parity.md`, or `tools/visual_verify/`. Result: 0 gates.
  - **DELETE**: stale (>60 days) and no hit in any memory file or any `.md` under the repo (any hit counts, including archived plans).
  - **MERGE**: everything else. Fresh gates go into a per-subsystem `MC2_TRACE=<subsystem>` knob. Stale gates that are still cited in a memory handoff or doc are kept as historical references and folded into the same knob, so the citations still resolve.

## Summary

| Verdict | Gates | Sites | Guarded lines (est.) |
|---|---:|---:|---:|
| KEEP | 0 | 0 | 0 |
| DELETE | 28 | 39 | 538 |
| MERGE | 78 | 150 | 2083 |
| **Total** | **106** | **189** | **2621** |

Stale (>60d): 58 of 106. Fresh: 48.

## Per subsystem

### Terrain/water (16 gates)

Proposed knob: `MC2_TRACE=terrain=<subsystem>` (replaces 12 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_BUCKET_HEADER_TRACE` | OFF | 1 | 11 | 2026-10-07 | **MERGE** | mem:1, docs:1 |
| `MC2_PATCH_STREAM_TRACE` | OFF | 2 | 30 | 2026-10-05 | **MERGE** | none |
| `MC2_TERRAIN_CAM_DIAG` | OFF | 1 | 19 | 2026-07-01 | **DELETE** | none |
| `MC2_TERRAIN_CULL_PROBE` | OFF | 1 | 2 | 2026-10-07 | **MERGE** | none |
| `MC2_TERRAIN_INDIRECT_THINEMIT_TRACE` | OFF | 1 | 6 | 2026-06-11 | **MERGE** | docs:1 |
| `MC2_TERRAIN_INDIRECT_TRACE` | OFF | 5 | 21 | 2026-10-07 | **MERGE** | mem:1, docs:4 |
| `MC2_TERRAIN_LIGHTING_GPU_TRACE` | OFF | 2 | 35 | 2026-06-11 | **DELETE** | none |
| `MC2_TERRAIN_LOD_CHECKER_DIAG` | OFF | 1 | 2 | 2026-07-08 | **MERGE** | mem:1, docs:1 |
| `MC2_TERRAIN_MASK_DISPATCH_TRACE` | OFF | 2 | 4 | 2026-06-11 | **DELETE** | none |
| `MC2_TERRAIN_PROBE` | OFF | 1 | 2 | 2026-07-01 | **MERGE** | mem:2, docs:1 |
| `MC2_TERRAIN_SHORELINE_PROBE` | OFF | 2 | 25 | 2026-07-02 | **DELETE** | none |
| `MC2_TERRAIN_SURFACE_TRACE` | OFF | 3 | 16 | 2026-06-11 | **MERGE** | docs:2 |
| `MC2_WATER_ASPECT_DIAG` | OFF | 1 | 15 | 2026-10-05 | **MERGE** | mem:3 |
| `MC2_WATER_DEBUG` | OFF | 9 | 94 | 2026-10-07 | **MERGE** | mem:2, docs:6 |
| `MC2_WATER_GATE_DIAG` | OFF | 2 | 26 | 2026-06-11 | **MERGE** | docs:2 |
| `MC2_WATER_MATERIAL_PROBE` | OFF | 1 | 20 | 2026-10-05 | **MERGE** | none |

### Mech/model (12 gates)

Proposed knob: `MC2_TRACE=mech=<subsystem>` (replaces 9 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_ANIMATED_PROP_PROBE` | OFF | 1 | 20 | 2026-07-23 | **DELETE** | none |
| `MC2_ANIM_ADVANCE_TRACE` | OFF | 1 | 9 | 2026-07-01 | **DELETE** | none |
| `MC2_LODBUG_TRACE` | OFF | 1 | 18 | 2026-06-11 | **DELETE** | none |
| `MC2_MECH_FRAG_DEBUG` | OFF | 6 | 64 | 2026-10-07 | **MERGE** | mem:3, docs:1 |
| `MC2_MECH_IMPORT_CLIP_DIAG` | OFF | 1 | 2 | 2026-07-01 | **MERGE** | mem:1 |
| `MC2_MECH_LIGHT_TRACE` | OFF | 1 | 2 | 2026-10-05 | **MERGE** | mem:1 |
| `MC2_MECH_LOD_TRACE` | OFF | 1 | 5 | 2026-10-05 | **MERGE** | mem:1, docs:1 |
| `MC2_MECH_NODE_TRACE` | OFF | 1 | 5 | 2026-10-05 | **MERGE** | mem:1, docs:1 |
| `MC2_MECH_PICK_DEBUG` | OFF | 1 | 2 | 2026-10-05 | **MERGE** | docs:1 |
| `MC2_MECH_PREVIEW_TRACE` | OFF | 1 | 3 | 2026-07-01 | **MERGE** | docs:2 |
| `MC2_MECH_SKEL_TRACE` | OFF | 1 | 5 | 2026-07-01 | **MERGE** | mem:1, docs:1 |
| `MC2_MECH_VIEWUNIFORMS_DIAG` | OFF | 1 | 2 | 2026-10-05 | **MERGE** | mem:1 |

### Static props (11 gates)

Proposed knob: `MC2_TRACE=static-props=<subsystem>` (replaces 11 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_COALESCE_GPU_VS_CPU_COUNT_TRACE` | OFF | 1 | 116 | 2026-06-11 | **MERGE** | docs:3 |
| `MC2_DRAW_PACKET_COALESCE_V5_TRACE` | OFF | 2 | 5 | 2026-10-05 | **MERGE** | docs:1 |
| `MC2_MDI_SUBMIT_TRACE` | OFF | 3 | 30 | 2026-10-05 | **MERGE** | docs:1 |
| `MC2_OBJBATCHER_TRACE` | OFF | 1 | 2 | 2026-10-05 | **MERGE** | none |
| `MC2_SKIP_STATIC_TREES_DIAG` | OFF | 1 | 2 | 2026-07-01 | **MERGE** | docs:1 |
| `MC2_STATIC_INST_DIAG` | OFF | 2 | 4 | 2026-07-01 | **MERGE** | mem:1 |
| `MC2_STATIC_LEAF_DIAG` | OFF | 2 | 4 | 2026-07-01 | **MERGE** | mem:4 |
| `MC2_STATIC_POP_SPLIT_CMD_DIAG` | OFF | 1 | 18 | 2026-10-05 | **MERGE** | mem:2 |
| `MC2_STATIC_PROP_PICK_DEBUG` | OFF | 1 | 2 | 2026-10-05 | **MERGE** | none |
| `MC2_STATIC_REG_PREWARM_TRACE` | OFF | 3 | 100 | 2026-07-01 | **MERGE** | mem:1 |
| `MC2_SUBSTRATE_COALESCE_TRACE` | OFF | 1 | 6 | 2026-10-05 | **MERGE** | docs:1 |

### Textures (7 gates)

Proposed knob: `MC2_TRACE=textures=<subsystem>` (replaces 5 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_ALPHA_TEST_TRACE` | OFF | 1 | 3 | 2026-10-05 | **MERGE** | none |
| `MC2_MATERIALLIB_TRACE` | OFF | 1 | 14 | 2026-07-08 | **DELETE** | none |
| `MC2_MODERN_TEX_RESOLVE_TRACE` | OFF | 2 | 5 | 2026-10-05 | **MERGE** | none |
| `MC2_TEXMGR_LOAD_TRACE` | OFF | 4 | 30 | 2026-10-07 | **MERGE** | mem:1 |
| `MC2_TEX_HANDOFF_TRACE` | OFF | 1 | 3 | 2026-06-11 | **DELETE** | none |
| `MC2_TXMMGR_BOUNDS_TRACE` | OFF | 1 | 1 | 2026-07-01 | **MERGE** | mem:1 |
| `MC2_TXM_LEAK_TRACE` | OFF | 1 | 1 | 2026-07-01 | **MERGE** | mem:1, docs:1 |

### Lighting (7 gates)

Proposed knob: `MC2_TRACE=lighting=<subsystem>` (replaces 4 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_LIGHTBRIDGE_COMMIT_TRACE` | OFF | 2 | 77 | 2026-07-01 | **DELETE** | none |
| `MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG` | OFF | 1 | 2 | 2026-07-29 | **MERGE** | docs:2 |
| `MC2_LIGHTSLOT_TRACE` | OFF | 1 | 9 | 2026-06-11 | **MERGE** | docs:4 |
| `MC2_LIGHT_CLAMP_PROBE` | OFF | 1 | 2 | 2026-10-05 | **MERGE** | mem:1, docs:2 |
| `MC2_LIGHT_DEDUP_TRACE` | OFF | 1 | 8 | 2026-06-11 | **DELETE** | none |
| `MC2_SCENE_LIGHTING_TRACE` | OFF | 1 | 2 | 2026-07-01 | **MERGE** | mem:1 |
| `MC2_STABLE_LIGHT_SKIP_DIAG` | OFF | 2 | 60 | 2026-07-29 | **DELETE** | none |

### Shadows (6 gates)

Proposed knob: `MC2_TRACE=shadows=<subsystem>` (replaces 4 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_DECOR_SHADOW_TRACE` | OFF | 1 | 4 | 2026-10-05 | **MERGE** | none |
| `MC2_SHADOW_CASTER_DIAG` | OFF | 1 | 7 | 2026-07-01 | **DELETE** | none |
| `MC2_SHADOW_CULL_DEBUG` | OFF | 2 | 42 | 2026-10-05 | **MERGE** | mem:1 |
| `MC2_SHADOW_DIAG` | OFF | 1 | 14 | 2026-10-05 | **MERGE** | docs:1 |
| `MC2_SHADOW_FOCUS_DIAG` | OFF | 1 | 5 | 2026-07-01 | **DELETE** | none |
| `MC2_SHADOW_STATE_TRACE` | OFF | 2 | 62 | 2026-10-05 | **MERGE** | none |

### Editor (6 gates)

Proposed knob: `MC2_TRACE=editor=<subsystem>` (replaces 2 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_EDITOR_DRAG_TRACE` | OFF | 1 | 23 | 2026-07-01 | **DELETE** | none |
| `MC2_EDITOR_MOVER_TRACE` | OFF | 1 | 2 | 2026-07-01 | **DELETE** | none |
| `MC2_EDITOR_PICK_TRACE` | OFF | 1 | 28 | 2026-10-07 | **MERGE** | none |
| `MC2_EDITOR_PROJECT_TRACE` | OFF | 1 | 2 | 2026-07-01 | **DELETE** | none |
| `MC2_EDITOR_STATIC_PRIME_DIAG` | OFF | 1 | 37 | 2026-07-08 | **MERGE** | mem:1 |
| `MC2_PATROL_TRACE` | OFF | 2 | 13 | 2026-07-08 | **DELETE** | none |

### GPU cull/props (6 gates)

Proposed knob: `MC2_TRACE=gpu-cull=<subsystem>` (replaces 5 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_GPUPROPS_TRACE` | OFF | 2 | 52 | 2026-10-05 | **MERGE** | none |
| `MC2_GPU_CULL_COMPUTE_TRACE` | OFF | 4 | 33 | 2026-06-11 | **MERGE** | docs:3 |
| `MC2_GPU_CULL_SUBSTRATE_TRACE` | OFF | 4 | 29 | 2026-06-11 | **MERGE** | mem:1, docs:2 |
| `MC2_GPU_DRIVEN_TRACE` | OFF | 3 | 15 | 2026-10-05 | **MERGE** | none |
| `MC2_GPU_SCENE_SCALE_PROBE` | OFF | 1 | 18 | 2026-07-01 | **DELETE** | none |
| `MC2_GPU_SYNC_TRACE` | OFF | 1 | 2 | 2026-07-01 | **MERGE** | mem:1 |

### Render pipeline (6 gates)

Proposed knob: `MC2_TRACE=render-pipeline=<subsystem>` (replaces 5 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_MVP_EARLY_TRACE` | OFF | 1 | 14 | 2026-07-08 | **DELETE** | none |
| `MC2_PARTICLE_FLUSH_STALL_TRACE` | OFF | 1 | 2 | 2026-10-07 | **MERGE** | none |
| `MC2_PIPELINE_BIND_TRACE` | OFF | 6 | 139 | 2026-10-07 | **MERGE** | mem:1, docs:2 |
| `MC2_RENDERSTATES_TRACE` | OFF | 1 | 2 | 2026-10-05 | **MERGE** | none |
| `MC2_RENDER_FRAME_PLAN_TRACE` | OFF | 3 | 32 | 2026-10-07 | **MERGE** | docs:1 |
| `MC2_RENDER_WATER_FASTPATH_DEBUG` | OFF | 4 | 19 | 2026-10-07 | **MERGE** | docs:1 |

### Gameplay/brain (5 gates)

Proposed knob: `MC2_TRACE=gameplay=<subsystem>` (replaces 4 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_ABL_CORE_TRACE` | OFF | 1 | 17 | 2026-07-08 | **MERGE** | mem:3 |
| `MC2_BRAIN_ENGAGE_TRACE` | OFF | 1 | 5 | 2026-07-08 | **DELETE** | none |
| `MC2_BRAIN_FIXED_TICK_TRACE` | OFF | 1 | 12 | 2026-10-07 | **MERGE** | none |
| `MC2_BRAIN_RUNTIME_TRACE` | OFF | 2 | 43 | 2026-10-07 | **MERGE** | none |
| `MC2_BRAIN_TASKQ_TRACE` | OFF | 2 | 47 | 2026-10-07 | **MERGE** | none |

### Misc (5 gates)

Proposed knob: `MC2_TRACE=misc=<subsystem>` (replaces 4 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_BLDG_DIAG_TRACE` | OFF | 1 | 15 | 2026-06-11 | **MERGE** | docs:3 |
| `MC2_BLDG_REG_TRACE` | OFF | 1 | 2 | 2026-10-05 | **MERGE** | docs:1 |
| `MC2_DEPLOY_KIND_TRACE` | OFF | 1 | 7 | 2026-07-01 | **DELETE** | none |
| `MC2_TREE_REG_TRACE` | OFF | 1 | 2 | 2026-06-11 | **MERGE** | docs:1 |
| `MC2_VANISH_PROBE` | OFF | 1 | 37 | 2026-07-01 | **MERGE** | mem:1 |

### Sky/post (3 gates)

Proposed knob: `MC2_TRACE=sky=<subsystem>` (replaces 1 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_CLOUD_DIAG` | OFF | 1 | 2 | 2026-07-01 | **DELETE** | none |
| `MC2_HDRI_SKY_STATE_PROBE` | OFF | 3 | 20 | 2026-07-01 | **DELETE** | none |
| `MC2_HDRI_SKY_UV_DEBUG` | OFF | 3 | 6 | 2026-10-05 | **MERGE** | none |

### Editor/UI (3 gates)

Proposed knob: `MC2_TRACE=editor=<subsystem>` (replaces 2 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_FL_TRACE` | OFF | 1 | 20 | 2026-07-01 | **DELETE** | none |
| `MC2_HOTKEY_TRACE` | OFF | 1 | 8 | 2026-10-07 | **MERGE** | none |
| `MC2_STRINGS_TRACE` | OFF | 1 | 2 | 2026-07-22 | **MERGE** | mem:1, docs:1 |

### Frame/object-manager (3 gates)

Proposed knob: `MC2_TRACE=frame=<subsystem>` (replaces 2 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_FRAME_JOBS_PATHB_DIAG` | OFF | 3 | 74 | 2026-07-01 | **DELETE** | none |
| `MC2_FRAME_JOBS_TOUCH_DIAG` | OFF | 3 | 146 | 2026-07-29 | **MERGE** | docs:1 |
| `MC2_INVIEW_CONFLATION_TRACE` | OFF | 2 | 52 | 2026-10-05 | **MERGE** | none |

### Vulkan props (3 gates)

Proposed knob: `MC2_TRACE=vulkan-props=<subsystem>` (replaces 2 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_VK_PROP_LIGHT_PROBE` | OFF | 1 | 14 | 2026-07-22 | **DELETE** | none |
| `MC2_VK_PROP_LIGHT_VERTEX_PROBE` | OFF | 1 | 2 | 2026-07-22 | **MERGE** | mem:1 |
| `MC2_VK_PROP_MIP_PROBE` | OFF | 1 | 10 | 2026-07-22 | **MERGE** | mem:1 |

### Picking (2 gates)

Proposed knob: `MC2_TRACE=picking=<subsystem>` (replaces 1 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_OBJECT_ID_BRIDGE_TRACE` | OFF | 2 | 20 | 2026-07-01 | **DELETE** | none |
| `MC2_PICK_CAP_TRACE` | OFF | 2 | 178 | 2026-10-05 | **MERGE** | docs:1 |

### Assets (1 gates)

Proposed knob: `MC2_TRACE=assets=<subsystem>` (replaces 1 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_ASSET_SCALE_TRACE` | OFF | 3 | 31 | 2026-10-05 | **MERGE** | mem:1 |

### FX (1 gates)

Proposed knob: `MC2_TRACE=fx=<subsystem>` (replaces 1 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_FX_TRACE` | OFF | 3 | 8 | 2026-10-05 | **MERGE** | mem:1, docs:4 |

### Perf (1 gates)

Proposed knob: `MC2_TRACE=perf=<subsystem>` (replaces 1 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_HITCH_TRACE` | OFF | 2 | 66 | 2026-10-07 | **MERGE** | mem:3, docs:4 |

### Projection (1 gates)

Proposed knob: `MC2_TRACE=projection=<subsystem>` (replaces 1 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_PROJECTZ_TRACE` | OFF | 4 | 35 | 2026-10-05 | **MERGE** | none |

### Visibility (1 gates)

Proposed knob: `MC2_TRACE=visibility=<subsystem>` (replaces 1 gates).

| Gate | Default | Sites | Guard lines (est.) | Last commit | Verdict | Memory/doc refs |
|---|---|---:|---:|---|---|---|
| `MC2_VIS_SEMANTICS_TRACE` | OFF | 4 | 173 | 2026-10-07 | **MERGE** | mem:1 |

## Notes

- `tools/visual_verify/scripts/` does not exist; the visual_verify check used `tools/visual_verify/` as a whole (no matches).
- No gate is named in CLAUDE.md or in `HANDOFF-2026-10-08b-vk-parity.md`.
- The scanner itself says deletion is human-gated and Tier B is advisory. Any DELETE here still needs a build and a grep of the call sites before removal.
- Recommended first step: delete the 28 DELETE gates in one commit per subsystem, then add a single `MC2_TRACE` dispatcher for the MERGE set.
