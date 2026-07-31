#!/usr/bin/env bash
# scripts/check-env-registry.sh
#
# Grep gate: every MC2_* env var used in source must be either
#   (a) registered in RenderCore/RendererFeatureRegistry.h, or
#   (b) in the ALLOWLIST below.
#
# Unknown vars -> exit 1 (CI-blocking).
#
# Usage:
#   sh scripts/check-env-registry.sh           # run from worktree root
#   sh scripts/check-env-registry.sh --verbose  # also print registered + allowlisted sets
#
# To register a new env var:
#   1. Add an entry to kFeatureTable or kAuxEnvVars in RendererFeatureRegistry.h.
#   2. Re-run this script to confirm it passes.
#   Do NOT add it to ALLOWLIST below -- that is for legacy/trace vars only.
#
# To add a known-legacy var without promoting it to the registry:
#   Add it to ALLOWLIST (alphabetically) with a comment.
#   Mark it "# promote-to-registry" if it's a feature gate that should be
#   registered eventually.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKTREE="$(cd "$SCRIPT_DIR/.." && pwd)"
HEADER="$WORKTREE/RenderCore/RendererFeatureRegistry.h"
VERBOSE=0
if [[ "${1:-}" == "--verbose" ]]; then VERBOSE=1; fi

# ---------------------------------------------------------------------------
# Step 1: extract registered var names from the header
# ---------------------------------------------------------------------------
# Greps all "MC2_[A-Z_]+" string literals in the header (both tables).
#
# GATE-PARITY-CLOSE-1: COMMENTS ARE STRIPPED FIRST, and that is the whole point.
# This used to grep the RAW header, so -- as the old comment here cheerfully
# admitted -- "any comments that happen to quote a var name" counted as a
# registration. That is the same defect class that let two contract rules ship
# green earlier today while satisfied by an identifier sitting in a comment: a
# CI gate whose evidence is prose is not a gate.
#
# INJECTION-PROVEN, not argued: deleting the real kAuxEnvVars entry for
# MC2_TREE_DIAG_TRACE while leaving `// prose mention only: "MC2_TREE_DIAG_TRACE"`
# in a comment left this script exiting 0. With the strip it exits 1 and names
# the var.
#
# ZERO BLAST RADIUS, measured before changing anything: raw grep, comment-stripped
# grep, and strict one-field-per-line extraction all yield exactly 623 names on
# this header today, so no currently-registered var depends on a prose mention.
# The awk pass removes /* ... */ (including multi-line) and // to end of line.

REGISTERED=()
while IFS= read -r name; do
    REGISTERED+=("$name")
done < <(awk '
    { line = $0
      gsub(/\/\*[^*]*\*\//, "", line)
      if (inblk) { if (match(line, /\*\//)) { line = substr(line, RSTART + 2); inblk = 0 } else next }
      if (match(line, /\/\*/))  { line = substr(line, 1, RSTART - 1); inblk = 1 }
      sub(/\/\/.*/, "", line)
      print line }' "$HEADER" \
    | grep -oE '"MC2_[A-Z0-9_]+"' | tr -d '"' | sort -u)

if [[ $VERBOSE -eq 1 ]]; then
    echo "=== Registered (${#REGISTERED[@]}) ==="
    printf '  %s\n' "${REGISTERED[@]}"
fi

# ---------------------------------------------------------------------------
# Step 2: ALLOWLIST of known-but-not-registered vars
# ---------------------------------------------------------------------------
# Sorted alphabetically. Categories:
#   trace       -- diagnostic output only; no correctness effect
#   feature     -- promote-to-registry (is a real gate; just not registered yet)
#   infra       -- smoke / tooling / capture infrastructure
#   override    -- forced-value override (not a feature toggle)
#   legacy      -- pre-modern path still in code
#   parity      -- correctness parity check / A-B comparison

ALLOWLIST=(
    # --- MERGE-WAVE-2 registry reconcile (2026-07-22): vars that landed via the
    # merge waves (VK diagnostics, unit-profile seam sources, upload stats, boot
    # fixtures). Alphabetical order below preserved per-batch.
    MC2_BOOT_QUICKLOAD_AT       # fixture -- quickload boot fixture (issue-fixes wave)
    MC2_BOOT_QUICKSAVE_AT       # fixture -- quicksave boot fixture (issue-fixes wave)
    MC2_EQUIPMENT_ADD           # feature -- EQUIP-ADD S2 (unit-profile seam, default OFF)
    MC2_EQUIPMENT_DEFINITION_SOURCE  # feature -- equipment definition source override (unit-profile seam)
    MC2_LIGHT_UPLOAD_STATS      # trace -- light SSBO upload stats (vk-interop wave)
    MC2_TERRAIN_MASK_UPLOAD_STATS  # trace
    MC2_UNIT_DEFINITION_SOURCE  # feature -- unit definition source override (unit-profile seam)
    MC2_VFX_TUBE_UPLOAD_STATS   # trace
    MC2_VISUAL_CAPTURE_EXIT_AFTER  # fixture -- visual capture exit-after frame
    MC2_VK_COMPOSITE_PRESENT    # vk-diag/override (vk-interop wave)
    MC2_VK_CSM_BISECT           # vk-diag
    MC2_VK_CSM_COPYMODE         # vk-diag/override
    MC2_VK_FEED_DUMP            # vk-diag -- feed dump
    MC2_VK_GPU_TIMESTAMPS       # vk-diag -- GPU timestamp collection
    MC2_VK_HUD_DRAW_DUMP        # vk-diag
    MC2_VK_OBJECTID_DIRECT_CHAIN  # vk-diag/override
    MC2_VK_PRESENT_SKIP_GL_HANDOFF  # vk-diag/override
    MC2_VK_PRESENT_VERIFY       # vk-diag
    MC2_VK_PROP_LIGHT_PROBE     # vk-diag
    MC2_VK_PROP_LIGHT_VERTEX_PROBE  # vk-diag
    MC2_VK_PROP_MIP_PROBE       # vk-diag
    MC2_VK_PROP_PACKET_DUMP     # vk-diag
    MC2_VK_SELECT_TEST          # vk-diag
    MC2_VK_TERRAIN_COPY_STATS   # trace
    MC2_VK_TERRAIN_CSM_DEBUG    # vk-diag
    MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL  # vk-diag/experimental
    MC2_VK_WATER_PERF           # trace
    MC2_ABL_REG_TRACE           # trace
    MC2_ABL_TRACE               # trace
    MC2_ALPHA_TEST_TRACE        # trace
    MC2_ASSET_SCALE_SELFTEST    # infra -- startup self-test
    MC2_ASSET_SCALE_TRACE       # trace
    MC2_BATCHER_FLUSH_TIMING    # trace
    MC2_BLOCK_FRUSTUM_FALLBACK  # feature -- promote-to-registry
    MC2_BRAIN_ALIAS             # feature -- TECHSCRIPT ALIAS-1 (default OFF)
    MC2_BRAIN_FLOW              # feature -- TECHSCRIPT FLOW-WAIT-1 (default OFF)
    MC2_BRAIN_SCOPE_GLOBAL      # feature -- TECHSCRIPT SCOPE-GLOBAL-1 (default OFF)
    MC2_BRAIN_VARIANTOF         # feature -- TECHSCRIPT VARIANTOF-1 (default OFF)
    MC2_BLDG_DIAG_TRACE         # trace
    MC2_BLDG_REG_TRACE          # trace
    MC2_BLKIDX_TRACE            # trace
    MC2_BUCKET_CENSUS           # trace
    MC2_BUCKET_HEADER_TRACE     # trace
    MC2_COALESCE_FORCE_DISARM   # override
    MC2_COALESCE_GPU_VS_CPU_COUNT_TRACE  # trace
    MC2_CONE_LEAF_DUMP          # trace -- batch-8C static-prop leaf-name census (default OFF)
    MC2_CPU_PROJ_COST_SPLIT     # feature -- promote-to-registry
    MC2_DRAW_PACKET_COMPARE         # trace
    MC2_DRAW_PACKET_COMPARE_VERBOSE # trace
    MC2_DECAL_GLPROBE               # trace
    MC2_DECOR_SHADOW_TRACE      # trace
    MC2_DEBUG_OVERLAY_PREDICATE_MODE  # feature -- promote-to-registry
    MC2_DEBUG_SHADOW_FRUSTUM    # trace
    MC2_DEBUG_SHADOW_STATIC     # trace
    MC2_DEBUG_SHADOW_ZRANGE     # trace
    MC2_DEBUG_VIDEO             # trace
    MC2_DEFS_ROOT               # infra -- data root override
    MC2_DEPTHBIAS_CALIB         # trace
    MC2_DEPTH_TRANSITION_PROBE  # trace
    MC2_DESTROY_TRACE           # trace
    MC2_DEV_SHELL               # infra -- dev command socket (DEV-SHELL-1)
    MC2_DEV_SHELL_PORT          # infra -- dev command socket port override
    MC2_DISABLE_GOSFX           # feature -- promote-to-registry
    MC2_EDITOR_BYPASS_BLDG_CULL # feature -- editor-only override
    MC2_EDITOR_MODE             # infra -- editor mode flag
    MC2_EDITOR_TRACE            # trace
    MC2_EFFECT_ADMISSION_PREDICATE  # feature -- promote-to-registry
    MC2_FF_TRACE                # trace
    MC2_GAMEPLAY_PICK_SELFTEST  # infra -- self-test (uses selftestEnvFlag; covered by grep-pattern extension)
    MC2_FORCE_DYNAMIC_BUILDINGS # override -- dev testing
    MC2_FORCE_DYNAMIC_TREES     # override -- dev testing
    MC2_FPS_CAP                 # infra
    MC2_HEIGHT                  # infra -- window height override (launcher/windowed)
    MC2_WIDTH                   # infra -- window width override (launcher/windowed)
    MC2_WINDOWED                # infra -- windowed-mode override (launcher, issue #49)
    MC2_FRAMECAP_TRACE          # trace
    MC2_FX_TRACE                # trace
    MC2_GOSFX_GROUP_LOG         # trace
    MC2_GPU_PARTICLES_LOG       # trace
    MC2_GPU_TRAIL_DISABLE       # feature -- promote-to-registry
    MC2_GPUBUF_COUNTER          # trace -- GPU-UPDATE-BUFFER-COUNTER-1 per-frame orphan-on-write tally (default OFF)
    MC2_GL_DEBUG                # infra -- GL debug context
    MC2_GL_DEBUG_FATAL          # infra -- abort on GL_DEBUG_SEVERITY_HIGH
    MC2_GL_ERROR_DRAIN_SILENT   # infra
    MC2_GPUPROPS_TRACE          # trace
    MC2_GLTF_AXIS               # override -- glTF import axis mapping select (orientation dial)
    MC2_GLTF_GROUND             # override -- glTF import auto-ground end select (1=base,2=opp,0=off)
    MC2_GLTF_YOFF               # override -- glTF import up-offset (un-bury)
    MC2_GPU_GROUND_PICK_PARITY  # parity -- GPU-GROUND-PICK-PARITY-1 oracle (default OFF)
    MC2_GPU_GROUND_PICK_PARITY_CENTER  # override -- parity center-probe mode
    MC2_GPU_GROUND_PICK_PARITY_SAMPLE  # override -- parity sample stride (default 30)
    MC2_GPU_GROUND_PICK_PARITY_TAG     # trace -- parity log tag suffix
    MC2_GPU_CULL                # feature -- promote-to-registry
    MC2_GPU_CULL_AABB_PARITY    # parity
    MC2_GPU_CULL_COMPUTE_TRACE  # trace
    MC2_GPU_CULL_CONSERVATIVE_OR  # override
    MC2_GPU_CULL_DUMP_SHADER    # trace
    MC2_GPU_CULL_FORCE_FENCE_NOT_READY  # override -- test fence path
    MC2_GPU_CULL_FRUSTUM_DILATION  # feature -- promote-to-registry
    MC2_GPU_CULL_LIFECYCLE      # feature -- promote-to-registry
    MC2_GPU_CULL_LIFECYCLE_TRACE  # trace
    MC2_GPU_CULL_READBACK       # feature -- promote-to-registry
    MC2_GPU_CULL_READBACK_TRACE # trace
    MC2_GPU_CULL_SUBSTRATE      # feature -- promote-to-registry
    MC2_GPU_CULL_SUBSTRATE_TRACE  # trace
    MC2_GPU_DRIVEN              # feature -- promote-to-registry
    MC2_GPU_DRIVEN_OVERLAY      # feature -- promote-to-registry
    MC2_GPU_DRIVEN_PARITY       # parity
    MC2_GPU_DRIVEN_TERRAIN_SOLID  # feature -- promote-to-registry
    MC2_GPU_DRIVEN_TRACE        # trace
    MC2_GPU_DRIVEN_WATER        # feature -- promote-to-registry
    MC2_GPU_PARTICLES           # feature -- promote-to-registry
    MC2_GPU_PROPS_DEBUG_MODE    # trace
    MC2_HDRI_SKY                # feature -- promote-to-registry
    MC2_HAZE_PARITY             # parity
    MC2_HEARTBEAT               # infra
    MC2_HOTKEY_TRACE            # trace
    MC2_INVIEW_CONFLATION_TRACE # trace
    MC2_IMPOSTOR_DIST           # override -- tree impostor far-LOD distance (world units)
    MC2_LEGACY_INSTANCE_POOLS   # override -- dev testing (revert pool-skip)
    MC2_MISSION_CYCLE_TEST      # fixture -- TEAM-COMMANDER-OWNERSHIP-1 in-process reset probe (default OFF)
    MC2_LIGHT_COST_SPLIT        # feature -- promote-to-registry
    MC2_LIGHT_DEDUP_TRACE       # trace
    MC2_LIGHTBAKE               # feature -- promote-to-registry
    MC2_LIGHTBRIDGE             # feature -- promote-to-registry
    MC2_LIGHTSSBO_TRACE         # trace
    MC2_LIGHTSLOT_TRACE         # trace -- TREE-OVERRIDE-LOD light-slot cardinality gate
    MC2_LIGHTING_SHADOW_PREDICATE_MODE  # feature -- promote-to-registry
    MC2_LODBUG_TRACE            # trace
    MC2_MECH_AMBIENT_V1         # feature -- promote-to-registry
    MC2_MECH_AMBIENT_V1_STRENGTH # feature -- promote-to-registry
    MC2_MECH_BATCHER_STATS      # trace
    MC2_MECH_FRAG_DEBUG         # trace
    MC2_MECH_GLASS_LUMA_THRESH    # feature -- promote-to-registry
    MC2_MECH_GLASS_MAXCHAN_THRESH # feature -- promote-to-registry
    MC2_MECH_GLASS_ROUGHNESS      # feature -- promote-to-registry
    MC2_MECH_LIGHT_TRACE        # trace
    MC2_MECH_METAL_ROUGHNESS      # feature -- promote-to-registry
    MC2_MECH_SPECULAR_STRENGTH    # feature -- promote-to-registry
    MC2_MECH_SPECULAR_V1          # feature -- promote-to-registry
    MC2_MECH_LOD_TRACE          # trace
    MC2_MECH_NODE_TRACE         # trace
    MC2_MECH_NORMALS_MODE       # feature -- promote-to-registry
    MC2_MECH_NORMALS_SMOOTH_DEG # feature -- promote-to-registry
    MC2_MECH_OBJECT_ID_SELFTEST # infra -- self-test
    MC2_MECH_PICK               # feature -- promote-to-registry
    MC2_MECH_PICK_DEBUG         # trace
    MC2_MECH_PICK_PIERCE_FOG    # feature -- promote-to-registry
    MC2_MECH_PICK_SELFTEST      # infra -- self-test
    MC2_MECH_RESTORE_TRACE      # trace
    MC2_MECH_VIEWUNIFORMS       # feature -- promote-to-registry
    MC2_MECH_VIEWUNIFORMS_DIAG  # trace
    MC2_MECH_TEX_READBACK       # trace
    MC2_MENU_CANARY_SKIP_INTRO  # infra -- smoke test
    MC2_MODERN_TERRAIN_PATCHES  # feature -- promote-to-registry
    MC2_MODERN_TERRAIN_SURFACE  # feature -- promote-to-registry
    MC2_MODERN_TEX_RESOLVE      # feature -- promote-to-registry
    MC2_MODERN_TEX_RESOLVE_TRACE  # trace
    MC2_MODERN_TEX_RESOLVE_VALIDATE  # parity
    MC2_MODOVERRIDE_TRACE       # trace -- model-override seam diagnostics
    MC2_OBJECT_ADMISSION_PREDICATE  # feature -- promote-to-registry
    MC2_OBJECT_ADMISSION_SELFTEST  # infra -- self-test
    MC2_OBJECT_ID_BUFFER_SELFTEST  # infra -- self-test
    MC2_OBJECT_PARITY_CHECK     # parity
    MC2_OBJECT_PARITY_TRACE     # trace
    MC2_OBJECT_RECON_TRACY      # infra -- Tracy profiler zone
    MC2_OBJBATCHER_TRACE        # trace
    MC2_PATCH_STREAM_FORCE_INIT_FAIL  # override -- fault injection
    MC2_PATCH_STREAM_TRACE      # trace
    MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND  # feature -- promote-to-registry
    MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND_CHECK  # parity
    MC2_PATCHSTREAM_QUAD_RECORDS  # trace
    MC2_PATCHSTREAM_QUAD_RECORDS_DRAW  # trace
    MC2_PATCHSTREAM_THIN_RECORD_FASTPATH  # feature -- promote-to-registry
    MC2_PATCHSTREAM_THIN_RECORDS  # trace
    MC2_PATCHSTREAM_THIN_RECORDS_DRAW  # trace
    MC2_PRESWAP_FINISH          # trace -- env-gated glFinish before SwapWindow (present-stall attribution)
    MC2_PROJECTZ_BYPASS_MODE    # feature -- promote-to-registry
    MC2_PROJECTZ_GUARD_PX       # override
    MC2_PROJECTZ_HEATMAP        # trace
    MC2_PROJECTZ_SUMMARY        # trace
    MC2_PROJECTZ_TRACE          # trace
    MC2_RDC_CAPTURE_FRAME       # infra -- RenderDoc capture
    MC2_RDC_CAPTURE_PATH        # infra -- RenderDoc capture
    MC2_RDC_EXIT_AFTER          # infra -- RenderDoc capture
    MC2_RDC_MISSION             # infra -- RenderDoc capture: supplies {mission} for
                                #          MC2_RDC_CAPTURE_PATH expansion when the
                                #          run is not under smoke mode (--mission)
    MC2_RENDER_SNAPSHOT_LOG     # trace
    MC2_REGFLUSH_DIAG_TRACE     # trace
    MC2_REGFLUSH_MULTI          # trace
    MC2_REGFLUSH_TYPEHIST       # trace
    MC2_RENDER_CONTRACT_ASSERT  # infra -- contract assertion
    MC2_RENDER_FRAME_PLAN_TRACE  # trace -- RENDER-FRAME-PLAN-SCAFFOLD-1 (per-frame pass tattle)
    MC2_PIPELINE_COLORMASK       # feature -- COLORMASK-OWNERSHIP-1 (opt-in colorMask via applyPipeline)
    MC2_RENDER_WATER_FASTPATH   # feature -- promote-to-registry
    MC2_RENDER_WATER_FASTPATH_DEBUG  # trace
    MC2_RENDER_WATER_PARITY_CHECK  # parity
    MC2_RENDER_WORLD_SELFTEST   # infra -- self-test
    MC2_RENDERSTATES_LEGACY     # legacy
    MC2_RENDERSTATES_TRACE      # trace
    MC2_REVERSE_Z_TRACE         # trace
    MC2_RING_FORCE_FINISH       # override
    MC2_RING_TRACE              # trace
    MC2_SCREENXY_PREDICATE_MODE # feature -- promote-to-registry
    MC2_SELECTION_PICKING_PREDICATE_MODE  # feature -- promote-to-registry
    MC2_SHADER_HOT_RELOAD       # infra
    MC2_SHAPE_C_PARITY_CHECK    # parity
    MC2_SHADOW_CASTER_CULL_MARGIN  # feature -- NDC margin for shadow caster light-box cull (default 0.25)
    MC2_SHADOW_CASTER_LIGHTBOX_CULL  # feature -- promote-to-registry; cull dynamic prop shadow casters to the shadow frustum (default OFF)
    MC2_SHADOW_CULL_DEBUG       # trace -- dump caster world pos + NDC for the light-box cull
    MC2_SHADOW_DIAG             # trace
    MC2_SHADOW_FOCUS_CENTER     # feature -- promote-to-registry; SHADOW-FOCUS-CENTER-1 center shadow box on camera near-ground focus point (default OFF)
    MC2_SHADOW_FOCUS_DIST       # override -- focus-point distance in front of camera (WU, default 1500, clamp [256,8000])
    MC2_SMOKE_MODE              # infra -- smoke harness
    MC2_SMOKE_PERF_SAMPLES      # infra -- smoke harness
    MC2_SMOKE_SEED              # infra -- smoke harness
    MC2_SPOTLIGHT_REAL_TRACE    # trace
    MC2_SPOT_DIAG               # trace
    MC2_STATIC_FORCE_ADMIT      # override
    MC2_STATIC_PROP_BAKE_SELFTEST  # infra -- self-test
    MC2_STATIC_PROP_DEPTH_PREPASS  # feature -- promote-to-registry; FOLIAGE-STATICPROP-DEPTH-PREPASS-1 camera depth-prepass (GL_EQUAL early-Z foliage overdraw cut, default OFF)
    MC2_STATIC_PROP_GLOBAL_CAP  # override
    MC2_STATIC_PROP_GLOBAL_POOL_LEGACY  # legacy
    MC2_STATIC_PROP_PICK        # feature -- promote-to-registry
    MC2_STATIC_PROP_PICK_DEBUG  # trace
    MC2_STATIC_PROP_TRACE       # trace
    MC2_STATICPROP_MATERIAL_PBR_SLOTS  # feature -- promote-to-registry
    MC2_STATICPROP_ORM_TRACE    # trace -- static-prop ORM material diagnostics
    MC2_STRINGS_TRACE           # trace
    MC2_SUBMIT_TYPEHIST         # trace
    MC2_SUBSTRATE_COALESCE_LEGACY  # legacy
    MC2_SUBSTRATE_COALESCE_TRACE  # trace
    MC2_TERRAIN_CULL_WIDE         # feature -- promote-to-registry
    MC2_TERRAIN_CULL_PROBE        # trace -- TERRAIN-CULL-STATE-PROBE-1 (ambient cull readback)
    MC2_TERRAIN_ADMISSION_LEGACY  # legacy
    MC2_TERRAIN_COST_SPLIT      # feature -- promote-to-registry
    MC2_TERRAIN_DEBUG_MODE      # trace
    MC2_TERRAIN_INDIRECT        # feature -- promote-to-registry
    MC2_TERRAIN_INDIRECT_CPU_FALLBACK  # legacy
    MC2_TERRAIN_INDIRECT_MINE   # feature -- promote-to-registry
    MC2_TERRAIN_INDIRECT_OVERLAY  # feature -- promote-to-registry
    MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK  # parity
    MC2_TERRAIN_INDIRECT_PARITY_CHECK  # parity
    MC2_TERRAIN_INDIRECT_THINEMIT_TRACE  # trace
    MC2_TERRAIN_INDIRECT_TRACE  # trace
    MC2_TERRAIN_LIGHTING_GPU    # feature -- promote-to-registry
    MC2_TERRAIN_LIGHTING_GPU_TRACE  # trace
    MC2_TERRAIN_LIGHTING_PARITY # parity
    MC2_GROUND_CONTACT_BLOB     # feature -- GROUND-CONTACT-BLOB-1 contact-darkening disc under movers (default OFF)
# MC2_SKYBOX_FOG_EXCLUDE -- REMOVED by BACKEND-DEFAULT-PARITY-1: now a real
# RendererFeatureRegistry.h entry (kAuxEnvVars, Feature, defaultOn=true). The
# line that used to sit here said "(default OFF)" long after
# SKYBOX-FOG-EXCLUDE-DEFAULT-ON-1 flipped it, and because this allowlist is the
# ONLY thing that accounted for the var, nothing compared that claim to a reader.
    MC2_TERRAIN_CONTROLMAP      # feature -- TERRAIN-CONTROLMAP-SAMPLE-1 authored material-weight override (default OFF)
    MC2_TERRAIN_CONTROLMAP_ALBEDO_STRENGTH  # override -- TERRAIN-CONTROLMAP-ALBEDO-1 strength env override
    MC2_TERRAIN_CONTROLMAP_FILE # override -- explicit control-map path override
    MC2_TERRAIN_MATERIAL_LIB    # feature -- TERRAIN-MATERIAL-LIB-1 data-defined terrain material layers (default OFF)
    MC2_TERRAIN_MATERIAL_LIB_FILE  # override -- explicit terrain_materials.json path override
    MC2_TERRAIN_SHORELINE       # feature -- TERRAIN-SHORELINE-V3 elevation-placed wetness/foam band (default ON; said OFF here until SHORELINE-GLOW-1, same stale-claim shape as the MC2_SKYBOX_FOG_EXCLUDE note above -- and this file is not swept by check-gate-default-agreement.py, so nothing compared it to a reader)
    MC2_TERRAIN_SHORELINE_FILE  # override -- explicit shoreline mask (optional modulator) path override
    MC2_TERRAIN_SHORELINE_STRENGTH   # override -- wet/damp darken intensity multiplier [0,2]
    MC2_TERRAIN_SHORELINE_FOAM       # override -- foam rim intensity multiplier [0,2]
    MC2_TERRAIN_SHORELINE_WET_RUN     # override -- V3 wet-band horizontal run from waterline (world units)
    MC2_TERRAIN_SHORELINE_FOAM_RUN    # override -- V3 foam-band horizontal run from waterline (world units)
    MC2_TERRAIN_SHORELINE_WET_HEIGHT  # override -- legacy alias for _WET_RUN (now horizontal-run units)
    MC2_TERRAIN_SHORELINE_FOAM_HEIGHT # override -- legacy alias for _FOAM_RUN (now horizontal-run units)
    MC2_TERRAIN_SHORELINE_EDGE_JITTER # override -- V4-STYLE static world-XY band-distance jitter amplitude (wu, [0,32], default 4)
    MC2_TERRAIN_VISUAL_DISPLACE # feature -- TERRAIN-VISUAL-HEIGHT displacement from 4x bake (default OFF)
    MC2_TERRAIN_VISUAL_DISPLACE_FAR  # override -- far-band displacement scale knob
    MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE  # feature -- TERRAIN-REAUTH-UNPIN-1 near-object displacement fade (default ON when displacing; 0 disables)
    MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE_RADIUS  # override -- objfade mover fade radius in wu (default 256)
    MC2_TERRAIN_MASK_DISPATCH   # feature -- promote-to-registry
    MC2_TERRAIN_MASK_DISPATCH_SOLID  # feature -- promote-to-registry
    MC2_TERRAIN_MASK_DISPATCH_TRACE  # trace
    MC2_TERRAIN_MASK_DISPATCH_WATER  # feature -- promote-to-registry
    MC2_TERRAIN_SOLID_NARROW    # feature -- promote-to-registry
    MC2_TERRAIN_SOLID_WINDOW_PARITY  # parity
    MC2_TERRAIN_SURFACE         # feature -- promote-to-registry
    MC2_TERRAIN_SURFACE_TRACE   # trace
    MC2_TEX_HANDOFF_TRACE       # trace
    MC2_TEX_LIFECYCLE_TRACE     # trace
    MC2_TEX_LIFECYCLE_TRACE_VERBOSE  # trace
    MC2_THIN_DEBUG              # trace
    MC2_TYPE_TABLE_CAND_LOG     # trace
    MC2_TYPE_TABLE_CAND_VERBOSE # trace
    MC2_TGL_POOL_TRACE          # trace
    MC2_TOBJ_COST_SPLIT         # feature -- promote-to-registry
    MC2_TOBJ_PARITY             # parity
    MC2_TRACE_EXIT              # infra
# MC2_TREE_DIAG_TRACE -- REMOVED by GATE-PARITY-CLOSE-1: now a real
# RendererFeatureRegistry.h entry (kAuxEnvVars, Trace, defaultOn=false).
# Being accounted for ONLY by this allowlist is what kept it out of the
# run_smoke.py passthrough allowlist and out of docs/tier1_env_vars.md for
# the whole life of the gate -- an allowlist line silences the registry
# check without ever putting the var in front of the doc/allowlist checks.
    MC2_TREE_REG_TRACE          # trace
    MC2_VERTEX_PROJECT_FAST     # feature -- promote-to-registry
    MC2_VERTEX_PROJECT_PARITY   # parity
    MC2_VISUAL_DIFF_CAPTURE     # infra -- visual diff tool
    MC2_VISUAL_DIFF_MISSION     # infra -- visual diff tool
    MC2_VISUAL_DIFF_OUT         # infra -- visual diff tool
    MC2_VISUAL_TUNING_FILE      # infra -- visual tuning profile path override (visual_tuning_profile.cpp)
    MC2_VK_PROP_FENCE_DEFER     # feature -- promote-to-registry; VK-PROP-FENCE-DEFER-1 killswitch (=0 restores inline wait)
    MC2_VK_PROP_PERF            # trace -- [VK_PROPS_PERF] per-frame us breakdown (water-style)
    MC2_VK_MECH_FENCE_DEFER     # feature -- promote-to-registry; VK-MECH-FENCE-DEFER-1 killswitch (=0 restores inline wait)
    MC2_VK_LIGHTFEED_DEDUP      # feature -- promote-to-registry; VK-LIGHTFEED-DEDUP-1 killswitch (=0 restores twin per-island LightsData copies)
    MC2_VK_MECH_PERF            # trace -- [VK_MECH_PERF] per-frame us breakdown (water-style)
    MC2_VK_TERRAIN_FENCE_DEFER  # feature -- promote-to-registry; VK-TERRAIN-FENCE-DEFER-1 killswitch (=0 restores inline wait)
    MC2_VK_TERRAIN_PERF         # trace -- [VK_TERRAIN_PERF] per-frame us breakdown (water-style)
    MC2_VPL_CULL                # feature -- promote-to-registry
    MC2_VPL_PICK                # feature -- promote-to-registry
    MC2_VPL_REDUCE              # feature -- promote-to-registry
    MC2_VSYNC                   # infra
    MC2_WATER_GATE_DIAG         # trace
    MC2_VERIFY_MODE             # infra -- MC2-VERIFY-LIVE-1 guard mode (log/fatal/off, default log)
    MC2_WATER_DEBUG             # trace
    MC2_WATER_DEBUG_MODE        # trace -- WATER-DEBUG-VIEWS-1 MDI FS material-space debug
    MC2_WATER_DEPTHPROBE        # trace
    MC2_WATER_MATERIAL_PROBE    # trace
    MC2_WATER_RENDERPROBE       # trace
    MC2_WATER_HDRI_REFL_FULL    # override -- WATER-HDRI-REFL-PERF-1 restore full-rate LOD-1.0 HDRI reflection sampling
    MC2_WATER_REFL_RT_PIXELPROOF # trace -- WATER-REFLECTION-CLIP-1 opt-in full pixel-coverage readback proof (sync stall)
    MC2_WATER_REFL_TRACE        # trace
    MC2_WATER_REFLECTION        # feature -- promote-to-registry; WATER-SKY-REFLECTION-1 gated SH-L2 sky reflection (default OFF)
    MC2_WATER_REFLECTION_RT     # feature -- promote-to-registry; WATER-TERRAIN-REFLECTION-1 mirrored terrain into reflection RT (default OFF)
    MC2_WATER_SKYTINT           # feature -- promote-to-registry; WATER-VISUAL-FIRST-SLICE gated sky tint (default OFF)
    MC2_WATER_STREAM_DEBUG      # trace
    MC2_WATER_UPLOAD_NARROW     # feature -- promote-to-registry

    # --- batch 1 (legacy registry sweep, A-C): trace/feature/override/infra ---
    MC2_ABL_ARG_GUARD                 # feature -- ABL VM arg-count guard (behavior-affecting safety check)
    MC2_ABL_ARG_GUARD_REPRO           # infra -- repro/test harness for arg guard
    MC2_ABL_CORE_TRACE                # trace
    MC2_ABL_RUNTIME_SOFTFAIL          # feature -- ABL runtime softfail behavior toggle
    MC2_ABL_SKIP_ERRORED_MODULES      # feature -- skip-errored-module behavior toggle
    MC2_ABL_VM_FACTS                  # trace
    MC2_ACTIVE_MOD                    # override -- active mod selection override
    MC2_AMBIENT_ASSERT_FATAL          # infra -- legacy alias for contract assert
    MC2_ANIMATED_PROP_PROBE           # trace
    MC2_ANIM_ADVANCE_TRACE            # trace
    MC2_ANIM_CADENCE_GUARD            # feature -- companion guard to cadence fix
    MC2_ASSIMP_IMPORT                 # feature -- Assimp mesh import path toggle
    MC2_ASSIMP_PAUSE_GATE             # override -- import debug pause gate
    MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE  # feature -- building anim-static eligibility toggle
    MC2_BOOT_TO_BAY                   # infra -- smoke/dev boot shortcut
    MC2_BOOT_TO_MECHLAB               # infra -- smoke/dev boot shortcut
    MC2_BOOT_TO_MISSION               # infra -- smoke/dev boot shortcut
    MC2_BOOT_TO_SCREEN                # infra -- smoke/dev boot shortcut
    MC2_BOX_DECAL                     # feature -- box decal render toggle
    MC2_BOX_DECAL_NORMAL_REJECT       # override -- box decal normal-reject tuning
    MC2_BOX_DECAL_STRENGTH            # override -- box decal strength tuning
    MC2_BOX_DECAL_YSPAN               # override -- box decal Y-span tuning
    MC2_BRAIN_COMMIT_PHASE            # feature -- brain dispatch commit-phase gate
    MC2_BRAIN_DISPATCH_APPLY          # feature -- brain-dispatch apply gate
    MC2_BRAIN_DISPATCH_CALL           # feature -- brain-dispatch call gate
    MC2_BRAIN_DISPATCH_FSM_TODO       # feature -- brain-dispatch FSM-todo gate
    MC2_BRAIN_DISPATCH_VAR            # feature -- brain-dispatch var gate
    MC2_BRAIN_ENGAGE                  # feature -- brain engage gate
    MC2_BRAIN_ENGAGE_TRACE            # trace
    MC2_BRAIN_FIXED_TICK              # feature -- brain fixed-tick gate
    MC2_BRAIN_FIXED_TICK_HZ           # override -- brain fixed-tick Hz override
    MC2_BRAIN_FIXED_TICK_TRACE        # trace
    MC2_BRAIN_FSM                     # feature -- brain FSM gate
    MC2_BRAIN_INLINE_EMPTY_SKIP       # feature -- brain inline-empty-skip gate
    MC2_BRAIN_INTENT_QUEUE            # feature -- brain intent-queue gate
    MC2_BRAIN_MISSIONFIT_OPORD        # feature -- brain mission-fit OPORD gate
    MC2_BRAIN_MISSION_SEED            # override -- brain mission seed override
    MC2_BRAIN_PATROL                  # feature -- brain patrol gate
    MC2_BRAIN_RUNTIME_FORCE_MODE      # override -- brain runtime force-mode override
    MC2_BRAIN_RUNTIME_TRACE           # trace
    MC2_BRAIN_SNAPSHOT                # infra -- brain snapshot dump tool
    MC2_BRAIN_SPECIAL_FIT             # feature -- brain special-fit gate
    MC2_BRAIN_TASKQ                   # feature -- brain task-queue gate
    MC2_BRAIN_TASKQ_TRACE             # trace
    MC2_BRAIN_VAR_MISSION             # feature -- brain var-mission gate
    MC2_BRIDGE_MOVER_PERIOD_SEC       # override -- bridge mover period override
    MC2_BRIDGE_MOVER_STATE            # feature -- bridge mover state gate
    MC2_BUILDING_FOOTPRINT_SHADOW     # feature -- building footprint shadow gate
    MC2_BUILDING_PBR                  # feature -- building PBR material gate
    MC2_BUILDING_PBR_TRACE            # trace
    MC2_CHEAT_INFINITE_MONEY          # override -- dev cheat

    # --- batch 2 (C-F) ---
    MC2_CHEAT_SALVAGE_ALL             # override -- dev cheat
    MC2_CLOUD_DIAG                    # trace
    MC2_CLOUD_SHADOW                  # feature -- cloud shadow gate
    MC2_COLLISION_FACTS               # trace
    MC2_CURSOR_TARGET_TRACE           # trace
    MC2_DEPLOY_KIND_TRACE             # trace
    MC2_DETERMINISTIC_RNG             # feature -- deterministic RNG mode (test/soak repeatability)
    MC2_DIAGNOSTIC_TRACE_FILE         # infra -- diagnostic JSONL trace file path (documented tier1)
    MC2_DIAG_TAGS                     # infra -- diagnostic trace tag filter (documented tier1)
    MC2_DRAW_PACKET_WARN              # trace
    MC2_EDGE_FOG                      # feature -- edge fog post-process gate
    MC2_EDGE_FOG_HEIGHT               # override -- edge fog height tuning
    MC2_EDGE_FOG_MAX                  # override -- edge fog max tuning
    MC2_EDGE_FOG_START                # override -- edge fog start tuning
    MC2_EDITOR_AUTODOCK               # infra -- editor-only UI dock setting
    MC2_EDITOR_BEAUTY_AUTOAPPLY       # infra -- editor-only tool auto-apply
    MC2_EDITOR_DOCK                   # infra -- editor-only UI dock setting
    MC2_EDITOR_DRAG_TRACE             # trace
    MC2_EDITOR_FAR_CLIP               # override -- editor-only far-clip override
    MC2_EDITOR_FPS_CAP                # infra -- editor-only FPS cap
    MC2_EDITOR_GPU_PICK               # feature -- editor GPU-pick gate
    MC2_EDITOR_GPU_TIMERS             # infra -- editor GPU timer diagnostics
    MC2_EDITOR_MOVER_REFRESH          # override -- editor mover refresh override
    MC2_EDITOR_MOVER_TRACE            # trace
    MC2_EDITOR_PANEL_W                # infra -- editor-only panel width setting
    MC2_EDITOR_PICK_TRACE             # trace
    MC2_EDITOR_PROJECT_TRACE          # trace
    MC2_EDITOR_RTT                    # infra -- editor-only RTT setting
    MC2_EDITOR_STATIC_PREWARM_OFF     # override -- editor static prewarm disable
    MC2_EDITOR_STATIC_PRIME_DIAG      # trace
    MC2_EDITOR_WATCHDOG               # infra -- editor watchdog toggle
    MC2_ENGINE_XL                     # override -- dev testing (engine XL cheat)
    MC2_FASTPATH_DROP_LOG             # trace
    MC2_FL_TRACE                      # trace
    MC2_FORCE_TXM_REGEN               # override -- force texture regen
    MC2_FRAMECTX_MISMATCH_FATAL       # infra -- fatal-assert opt-in for frame-context mismatch
    MC2_FRAMEGRAPH_AMBIENT_FATAL      # infra -- fatal-assert opt-in (legacy alias)
    MC2_FRAMEGRAPH_AMBIENT_GUARD      # feature -- framegraph ambient guard gate
    MC2_FRAMEGRAPH_DRYRUN             # feature -- framegraph dry-run gate
    MC2_FRAMEGRAPH_REORDER_SPMECH     # feature -- framegraph StaticProp/Mech reorder gate (REJECTED candidate, default-OFF permanently)
    MC2_FRAME_JOBS                    # feature -- frame-jobs master gate
    MC2_FRAME_JOBS_BATCH              # override -- frame-jobs batch size override
    MC2_FRAME_JOBS_PATHB_DIAG         # trace
    MC2_FRAME_JOBS_TOUCH              # feature -- frame-jobs touch gate
    MC2_FRAME_JOBS_TRACE              # trace
    MC2_FRAME_JOBS_WORKERS            # override -- frame-jobs worker count override
    MC2_FRAME_PASS_STATS              # trace
    MC2_FRAME_PASS_STATS_EVERY        # trace
    MC2_FXAA                          # feature -- FXAA post-process gate
    MC2_FXAA_EDGE_THRESHOLD           # override -- FXAA tuning
    MC2_FXAA_EDGE_THRESHOLD_MIN       # override -- FXAA tuning

    # --- batch 3 (F-M) ---
    MC2_FXAA_SUBPIX                   # override -- FXAA tuning
    MC2_FX_COUNT_LOG                  # trace
    MC2_GEOM_PHASE_SPLIT              # feature -- geometry phase-split gate
    MC2_GLTF_YAW_DEG                  # override -- glTF import yaw override
    MC2_GOM_RECON                     # feature -- game-object-manager recon gate
    MC2_GPU_CULL_OWNERSHIP_PARITY     # parity
    MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE  # feature -- static-prop eager light-bake gate
    MC2_GPU_CULL_STATIC_FROZEN_ORDER_ORACLE  # parity
    MC2_GPU_CULL_STATIC_FROZEN_RECORDS  # feature -- static-prop frozen-records gate
    MC2_GPU_DEBUG_NAMES               # infra -- KHR_debug object labels (documented tier1)
    MC2_GPU_PICK_HOVER                # feature -- GPU pick-hover gate
    MC2_GPU_PICK_HOVER_TRACE          # trace
    MC2_GPU_SCENE_SCALE_PROBE         # trace
    MC2_GPU_SYNC_TRACE                # trace
    MC2_HDRI_SKY_AZ_OFFSET            # override -- HDRI sky azimuth offset tuning
    MC2_HDRI_SKY_FRAME_FIX            # feature -- HDRI sky frame-fix gate
    MC2_HDRI_SKY_STATE_PROBE          # trace
    MC2_HDRI_SKY_UV_DEBUG             # trace
    MC2_HITCH_MS                      # override -- hitch trace threshold (documented tier1 companion)
    MC2_HITCH_TRACE                   # trace
    MC2_HZB_FORCE_HORIZON             # override -- HZB debug force-horizon
    MC2_HZB_FORCE_HORIZON_POS         # override -- HZB debug force-horizon position
    MC2_HZB_VIEW_FILE                 # infra -- HZB debug view file path
    MC2_IMGUI_DEMO                    # infra -- ImGui demo window toggle
    MC2_JUMPJETS_ALL                  # override -- dev cheat (all jumpjets)
    MC2_LAUNCHED                      # infra -- launcher-provenance flag (documented tier1 companion)
    MC2_LAUNCHER_ENV_JSON             # infra -- =0 kill-switch for the MC2_NO_LAUNCHER launcher_env.json read (gameosmain LAUNCHER_ENV v1)
    MC2_LIGHTBRIDGE_COMMIT_TRACE      # trace
    MC2_LIGHTING_DEBUG_VIEW           # trace
    MC2_LIGHTING_LINEAR_AUDIT         # trace
    MC2_LOAD_RACE_TRACE               # trace -- mission-load residency-race one-shot diagnostics
    MC2_LOAD_RACE_VERDICT_FRAME       # trace -- frame to emit LOADRACE verdict (defaults to MC2_SCREENSHOT_AT_FRAME)
    MC2_LOG_CURSOR                    # trace
    MC2_LOG_LOGISTICS                 # trace
    MC2_LOG_LOGISTICS_FLOW            # trace
    MC2_LOG_MECHICON                  # trace
    MC2_LOG_MECH_ICON                 # trace
    MC2_LOG_PILOTS                    # trace
    MC2_LOG_PREVIEW                   # trace
    MC2_LOG_STRINGS                   # trace
    MC2_LOWCAM_OBJ_NEARPAD            # feature -- low-camera object near-pad gate
    MC2_LOWCAM_OBJ_NEARPAD_SCALE      # override -- low-camera near-pad scale tuning
    MC2_LOWCAM_PICK                   # feature -- low-camera pick gate
    MC2_LOWCAM_SOLID_NEAR             # override -- low-camera solid-near tuning
    MC2_LOWCAM_TERRAIN_NEAR           # feature -- low-camera terrain-near gate
    MC2_LOWCAM_ZOOM_ANCHOR            # override -- low-camera zoom anchor override
    MC2_CAMERA_SMOOTH                 # override -- gameplay camera smoothing time-constant ms (0=legacy)
    MC2_CAMERA_SMOOTH_TRACE           # trace
    MC2_CAM_SMOOTH_TEST               # trace -- synthetic scroll driver for smoothing evidence
    MC2_MATERIALLIB_TRACE             # trace
    MC2_MDI_SUBMIT_TRACE              # trace
    MC2_MECH_ANIM_ROTATION_ONLY       # feature -- mech anim rotation-only gate
    MC2_MECH_BACK_FILL                # feature -- mech back-fill gate
    MC2_MECH_IBL_SH                   # feature -- mech IBL spherical-harmonics gate
    MC2_MECH_IMPORT_ANIMATE           # feature -- mech import animate gate

    # --- batch 4 (M-P) ---
    MC2_MECH_IMPORT_CLIP_DIAG         # trace
    MC2_MECH_IMPORT_DYNAMIC_LIFT      # feature -- mech import dynamic-lift gate
    MC2_MECH_IMPORT_FORCE_BONE        # override -- mech import force-bone debug
    MC2_MECH_IMPORT_FORCE_CLIP        # override -- mech import force-clip debug
    MC2_MECH_IMPORT_FORCE_FRAME       # override -- mech import force-frame debug
    MC2_MECH_IMPORT_FORCE_POSE        # override -- mech import force-pose debug
    MC2_MECH_IMPORT_GPU               # feature -- mech import GPU gate
    MC2_MECH_IMPORT_GPU_AXIS          # override -- mech import GPU axis override
    MC2_MECH_IMPORT_GPU_LIFT          # override -- mech import GPU lift override
    MC2_MECH_IMPORT_GPU_LIFT_AXIS     # override -- mech import GPU lift-axis override
    MC2_MECH_NODE_DIAG                # trace
    MC2_MECH_PREVIEW_TRACE            # trace
    MC2_MECH_SKEL_BONE_DUMP           # infra -- skeletal bone dump tool
    MC2_MECH_SKEL_HEIGHT              # override -- mech skeleton height override
    MC2_MECH_SKEL_TRACE               # trace
    MC2_MECH_SURFACE_MATERIAL         # override -- mech surface material override
    MC2_MIF_SPLIT                     # feature -- mission-init-file split gate
    MC2_MISSION_SPLIT                 # feature -- mission split gate
    MC2_MOD_DEPS                      # feature -- mod dependency-check gate
    MC2_MOUSE_RECON                   # trace
    MC2_MOVE_CHUNK_SHADOW             # feature -- move-recon chunk-shadow alt-enable gate
    MC2_MOVE_PATH_CACHE_SHADOW        # feature -- move-recon path-cache-shadow alt-enable gate
    MC2_MVP_EARLY_TRACE               # trace
    MC2_MVP_PUBLISH_EARLY             # feature -- MVP-publish-early gate
    MC2_NO_LAUNCHER                   # infra -- launcher-bypass flag (documented tier1 companion)
    MC2_OBJECT_ID_BRIDGE_TRACE        # trace
    MC2_OBJECT_POLY_OFFSET            # override -- object poly-offset override
    MC2_OBJECT_POLY_OFFSET_FACTOR     # override -- object poly-offset factor override
    MC2_OBJECT_POLY_OFFSET_UNITS      # override -- object poly-offset units override
    MC2_OBJECT_WALK_FACTS             # trace
    MC2_OBJ_MVP_STALE_FATAL           # infra -- fatal-assert opt-in for stale object MVP
    MC2_OOB_FOG                       # feature -- out-of-bounds fog gate
    MC2_OOB_FOG_COLOR                 # override -- OOB fog color override
    MC2_OOB_LETTERBOX                 # feature -- OOB letterbox gate
    MC2_OS_CURSOR                     # feature -- OS-cursor gate
    MC2_OVERLAY_MAGENTA_TRACE         # trace
    MC2_OVERLAY_TEXTURE_TRACE         # trace
    MC2_PARTICLE_FLUSH_STALL_TRACE    # trace
    MC2_PATH_JUMP_FAIL_BACKOFF        # feature -- path jump-fail backoff gate
    MC2_PATH_SOLVE_ISOLATED           # feature -- path-solve isolated-context gate
    MC2_PATROL_TRACE                  # trace
    MC2_PBR_AMBIENT_SPECULAR          # override -- mech PBR ambient-specular tuning
    MC2_PBR_METALLIC_INFLUENCE        # override -- mech PBR metallic tuning
    MC2_PBR_ROUGHNESS_MAX             # override -- mech PBR roughness tuning
    MC2_PBR_ROUGHNESS_MIN             # override -- mech PBR roughness tuning
    MC2_PBR_TILE_SCALE                # override -- mech PBR tile-scale tuning
    MC2_PBR_TRIPLANAR                 # feature -- mech PBR triplanar gate
    MC2_PBR_TRIPLANAR_SCALE           # override -- mech PBR triplanar scale tuning
    MC2_PBR_WEAR_STRENGTH             # override -- mech PBR wear-strength tuning
    MC2_PICK_BEHIND_LEGACY            # legacy -- legacy pick-behind path
    MC2_PICK_CAP_TRACE                # trace

    # --- batch 5 (P-S) ---
    MC2_PICK_RECON                    # feature -- pick recon gate
    MC2_PIPELINE_BIND_TRACE           # trace
    MC2_POSTPROCESS_BACKEND           # feature -- postprocess backend-select gate
    MC2_PREVIEW_SCENE_DRAIN           # feature -- preview scene-drain gate
    MC2_PROJECTED_DECALS              # feature -- projected decal gate
    MC2_PROP_FIXB_MVP                 # feature -- prop FixB MVP gate
    MC2_PURCHASE_ALL                  # override -- dev cheat (purchase all)
    MC2_QUADSETUP_ARMED_SKIP          # feature -- quad-setup armed-skip gate (documented in debug_state_dump)
    MC2_RAIN_BATCH                    # feature -- rain batching gate
    MC2_REBUILD_MOD_CACHE             # override -- mod cache rebuild override
    MC2_RENDER_BACKEND_REGION_IFACE   # feature -- render-backend region-iface seam gate
    MC2_RENDER_FRAME_DRIVER           # feature -- render-frame-driver gate
    MC2_RENDER_PASS_CONTRACT_ASSERT   # infra -- pass-scope contract assert (documented tier1)
    MC2_RENDER_PASS_CONTRACT_TRACE    # trace
    MC2_RENDER_PASS_TELEMETRY         # trace
    MC2_RENDER_PASS_TIME              # trace
    MC2_RENDER_PASS_TIME_EVERY        # trace
    MC2_RENDER_PATH                   # trace
    MC2_RES_DIAG                      # trace
    MC2_RNG_SEED                      # override -- RNG seed override (soak/test)
    MC2_SCENE_LIGHTING_ASSERT         # infra -- fatal-assert opt-in for scene lighting
    MC2_SCENE_LIGHTING_TRACE          # trace
    MC2_SCREENSHOT_AT_FRAME           # infra -- screenshot capture tool
    MC2_CAMERA_PARK                   # infra -- CAMERA-PARK-1 fixture: live camera pose pin "x,y,rot,proj,alt[@frame]"
    MC2_SCREENSHOT_PATH               # infra -- screenshot capture tool
    MC2_SENSOR_SCAN_FACTS             # trace
    MC2_SHADER_PATH_TINT              # trace
    MC2_SHADOW_CASTER_DIAG            # trace
    MC2_SHADOW_CSM_COUNT              # override -- CSM cascade-count override
    MC2_SHADOW_CSM_LAMBDA             # override -- CSM lambda tuning
    MC2_SHADOW_CSM_SOFTNESS           # override -- CSM softness tuning
    MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY  # feature -- shadow dynamic-prop dirty-only gate
    MC2_SHADOW_FOCUS_DIAG             # trace
    MC2_SHADOW_FULLMAP_SEPARATE       # feature -- shadow fullmap-separate gate
    MC2_SHADOW_FULLMAP_SIZE           # override -- shadow fullmap size override
    MC2_SHADOW_MAP_SIZE               # override -- shadow map size override
    MC2_SHADOW_MECH_SOFT              # feature -- mech soft-shadow gate
    MC2_SHADOW_OBJ_NORMAL_BIAS        # override -- shadow object normal-bias tuning
    MC2_SHADOW_PROP_ALPHA             # override -- shadow prop alpha tuning
    MC2_SHADOW_PROP_ALPHA_ATLAS       # override -- SHADOW-PROP-ALPHA-ATLAS-1 coalesce/BC7-atlas shadow alpha kill switch (default ON)
    MC2_SHADOW_ROBUST_BASIS           # feature -- shadow robust-basis gate
    MC2_SHADOW_STATE_TRACE            # trace
    MC2_SHADOW_TIER_STATS             # trace
    MC2_SKIP_STATIC_BUILDINGS         # override -- dev testing (skip static buildings)
    MC2_SKIP_STATIC_TREES             # override -- dev testing (skip static trees)
    MC2_SKIP_STATIC_TREES_DIAG        # trace
    MC2_SMART_LOAD_TRACE              # trace
    MC2_SMOKE_FIXED_TIMESTEP          # infra -- smoke harness fixed timestep
    MC2_SOAK_AUTOWIN                  # infra -- soak harness autowin (documented tier1)
    MC2_SOAK_AUTO_PURCHASE            # infra -- soak harness auto-purchase
    MC2_SOAK_CHECK_SCREENS            # infra -- soak harness screen-check
    MC2_SOAK_KILL_ENEMY               # infra -- soak harness kill-enemy

    # --- batch 6 (S-T) ---
    MC2_SOAK_LANCE_RANDOM             # infra -- soak harness lance-random
    MC2_SOAK_PILOT_PROMOTE            # infra -- soak harness pilot-promote
    MC2_SOAK_WIN_AFTER_SEC            # infra -- soak harness win-after-sec (documented tier1)
    MC2_STABLE_LIGHT_SKIP             # feature -- stable-light-skip gate
    MC2_STATICPROP_TEX_TIER           # override -- static-prop texture-tier override
    MC2_STATIC_INST_DIAG              # trace
    MC2_STATIC_LEAF_DIAG              # trace
    MC2_STATIC_POP_SPLIT              # feature -- static-prop population-split gate
    MC2_STATIC_POP_SPLIT_CMD_DIAG     # trace
    MC2_STATIC_PROP_SNAPSHOT_FILL_DIRTYONLY  # feature -- static-prop snapshot dirty-only gate
    MC2_STATIC_RECIPE_RECYCLE         # feature -- static-prop recipe-recycle gate
    MC2_STATIC_REG_COVERAGE           # trace
    MC2_STATIC_REG_PREWARM            # feature -- static-registry prewarm gate
    MC2_STATIC_REG_PREWARM_TRACE      # trace
    MC2_STATIC_STALE_DROP_FATAL       # infra -- fatal-assert opt-in for stale drop (documented tier1)
    MC2_TACMAP_FORMATION_LINE         # feature -- tactical-map formation-line gate
    MC2_TACTICAL_OVERVIEW             # feature -- tactical overview gate
    MC2_TACTICAL_OVERVIEW_DEBUG       # trace
    MC2_TACTICAL_OVERVIEW_TINT        # override -- tactical overview tint override
    MC2_TACTIC_WEIGHTS                # feature -- brain tactic-weights gate
    MC2_TACTIC_WEIGHTS_B              # feature -- brain tactic-weights-B gate
    MC2_TARGETING_GUARD               # feature -- targeting guard gate
    MC2_TERRAIN_CAM_DIAG              # trace
    MC2_TERRAIN_CEMENT_DIAG_CONNECT   # trace
    MC2_TERRAIN_CLIFF_POM             # feature -- terrain cliff triplanar POM gate
    MC2_TERRAIN_CLIFF_POM_DEPTH       # override -- terrain cliff POM depth tuning
    MC2_TERRAIN_CLIFF_POM_STEPS       # override -- terrain cliff POM march steps tuning
    MC2_TERRAIN_CLIFF_TRIPLANAR       # feature -- terrain cliff-triplanar gate
    MC2_TERRAIN_CLIFF_TRIPLANAR_STRENGTH  # override -- terrain cliff-triplanar strength tuning
    MC2_TERRAIN_DETAIL_ANTITILE       # feature -- terrain detail-antitile gate
    MC2_TERRAIN_EDGE_FEATHER          # feature -- terrain edge-feather gate
    MC2_TERRAIN_EDGE_FEATHER_STRENGTH # override -- terrain edge-feather strength tuning
    MC2_TERRAIN_LOD_CHECKER_DIAG      # trace
    MC2_TERRAIN_LOD_CHUNK_CEMENT_MAXLOD  # override -- LOD-chunk cement max-LOD override
    MC2_TERRAIN_LOD_CHUNK_DIAG        # trace
    MC2_TERRAIN_LOD_CHUNK_DIST_SCALE  # override -- LOD-chunk distance-scale override
    MC2_TERRAIN_LOD_CHUNK_FORCE_COLOR # trace
    MC2_TERRAIN_LOD_CHUNK_FORCE_LOD   # override -- LOD-chunk force-LOD debug override
    MC2_TERRAIN_LOD_CHUNK_NO_APRON    # override -- LOD-chunk disable-apron debug
    MC2_TERRAIN_LOD_CHUNK_NO_CULL     # override -- LOD-chunk disable-cull debug
    MC2_TERRAIN_LOD_CHUNK_NO_SKIRTS   # override -- LOD-chunk disable-skirts debug
    MC2_TERRAIN_LOD_CHUNK_NO_STITCH   # override -- LOD-chunk disable-stitch debug
    MC2_TERRAIN_LOD_CHUNK_SKIRT_MAX   # override -- LOD-chunk skirt-max override
    MC2_TERRAIN_LOD_DEPTH_ALWAYS      # feature -- LOD-chunk depth-always gate
    MC2_TERRAIN_LOD_GEOMORPH          # override -- TERRAIN-LOD-GEOMORPH-1 killswitch (=0 disables mips+morph; rides MC2_TERRAIN_VISUAL_DISPLACE)
    MC2_TERRAIN_LOD_MORPH_START       # override -- TERRAIN-LOD-GEOMORPH-1 morph ramp start fraction of band (default 0.6)
    MC2_TERRAIN_MACRO_VARIATION       # feature -- terrain macro-variation gate
    MC2_TERRAIN_MACRO_VARIATION_STRENGTH  # override -- terrain macro-variation strength tuning
    MC2_TERRAIN_MINE_AB               # parity
    MC2_TERRAIN_NORMAL_ARRAY          # feature -- terrain normal-array gate
    MC2_TERRAIN_PICK_PARITY           # parity
    MC2_TERRAIN_PROBE                 # trace
    MC2_TERRAIN_SHORELINE_PROBE       # trace -- TERRAIN-SHORELINE-V3 shore-delta one-shot probe
    MC2_TERRAIN_RAYCAST_PICK          # feature -- terrain raycast-pick gate
    MC2_TERRAIN_RUNTIME_DECALS        # feature -- terrain-runtime decals gate
    MC2_TERRAIN_RUNTIME_GROUNDING     # feature -- terrain-runtime grounding gate

    # --- batch 7 (T-W, final) ---
    MC2_TERRAIN_RUNTIME_PARITY        # parity
    MC2_TERRAIN_SLOPE_BIAS            # feature -- terrain slope-bias gate
    MC2_TERRAIN_SLOPE_BIAS_STRENGTH   # override -- terrain slope-bias strength tuning
    MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN  # override -- terrain snow-brightness-dampen tuning
    MC2_TERRAIN_VISUAL_HEIGHT_FILE    # override -- terrain visual-height file override
    MC2_TEXMGR_KTX_PRIMARY            # feature -- texture-manager KTX-primary gate
    MC2_TEXMGR_LOAD_TRACE             # trace
    MC2_TUBE_PROFILE_LOG              # trace
    MC2_TXMMGR_BOUNDS_TRACE           # trace
    MC2_TXM_LEAK_TRACE                # trace
    MC2_UI_PASS_MEASURE               # trace
    MC2_UNIT_PROFILE_TEST_GRANT_VEHICLE  # infra -- unit-profile test fixture
    MC2_VANISH_PROBE                  # trace
    MC2_VEGETATION_ATLAS              # override -- vegetation atlas path override
    MC2_VEGETATION_CARDS              # feature -- vegetation cards gate (documented in debug_state_dump, default OFF)
    MC2_VEG_DEBUG_FORCE_VISIBLE       # trace
    MC2_VEG_MAX_DIST                  # override -- vegetation max-distance override
    MC2_VFX_BLACKBODY                 # feature -- VFX blackbody gate
    MC2_VFX_DISTORTION                # feature -- VFX distortion gate
    MC2_VFX_DISTORT_AMP               # override -- VFX distortion amplitude tuning
    MC2_VFX_DISTORT_FIXTURE           # infra -- VFX distortion test fixture
    MC2_VFX_ORACLE_SHAPE              # parity
    MC2_VFX_ORACLE_SHAPE_LOG          # trace
    MC2_VFX_SCENECOLOR_GRAB           # feature -- VFX scene-color-grab gate
    MC2_VISUAL_BOOKMARK_CAPTURE       # infra -- visual capture tool
    MC2_VISUAL_CAPTURE_DIR            # infra -- visual capture tool
    MC2_VISUAL_CAPTURE_FRAME          # infra -- visual capture tool
    MC2_VISUAL_SETTLE                 # infra -- visual capture tool
    MC2_VULKAN_CACHE_DIR              # infra -- Vulkan-prep pipeline cache dir (documented arc)
    MC2_VULKAN_ISLAND_FORCE_FALLBACK  # override -- Vulkan island force-fallback test knob
    MC2_VULKAN_OOB_FOG_ISLAND         # feature -- Vulkan OOB-fog island gate (documented arc)
    MC2_VULKAN_OOB_FOG_ISLAND_FORCE_FALLBACK  # override -- Vulkan OOB-fog island force-fallback test knob
    MC2_VULKAN_POSTPROCESS_SUBGRAPH_FORCE_FALLBACK  # override -- Vulkan postprocess-subgraph force-fallback test knob
    MC2_VULKAN_PROBE                  # infra -- Vulkan-prep headless probe (documented arc)
    MC2_VULKAN_SPV_DIR                # infra -- Vulkan-prep SPIR-V dir override (documented arc)
    MC2_VULKAN_SWAPCHAIN_PRESENT_HIDDEN  # infra -- Vulkan swapchain-present hidden-window test knob
    MC2_VULKAN_VALIDATION             # infra -- Vulkan-prep validation preset (documented arc)
    MC2_WATCHID_GENERATION            # feature -- watch-ID generation gate
    MC2_WATER_ASPECT_DIAG             # trace
    MC2_WATER_EDGE_FEATHER            # feature -- water edge-feather gate
    MC2_WATER_EDGE_FEATHER_STRENGTH   # override -- water edge-feather strength tuning
    MC2_WATER_SHORE_FEATHER           # feature -- SHORELINE-STAIRCASE-1: shore-band feather floor (wu) dissolving the tile-quantized water-edge staircase; default OFF
    MC2_LIGHT_BILLBOARD_TWOSIDED      # killswitch -- EDGE-LIGHT-BILLBOARD-CULL-1 (MASTER-FAIL #8): =0 restores legacy backface cull of beacon/runway edge-light glow cards; default ON
    MC2_LIGHT_BILLBOARD_FACING        # killswitch -- LIGHT-BILLBOARD-GPU-1 (MASTER-FAIL #8): camera-facing billboard + capped min screen size for beacon/runway light fixtures on the GPU static-prop batcher path; =1 opts in, =2 exaggerates for verification; default OFF (RUNWAY-MARKER-LIGHTS-1 flipped ON->OFF after the user rejected the uncapped-clamp look)
    MC2_LETTERBOX_OPAQUE              # killswitch -- LETTERBOX-TOP-RESTORE-1: cinematic letterbox bars are opaque black while the bar EXTENT still wipes in; =0 restores the stock 5s alpha ramp (translucent bars during the swoop)
    MC2_WATER_BIAS_LEGACY             # killswitch -- WATER-BIAS-LINEARIZE-1: =1 restores constant NDC water bias (37557c8b)
    MC2_CEMENT_MISSION_LIGHT          # killswitch -- CEMENT-NIGHT-LIGHT-1: =0 restores unlit cement (67d5ce40)
    MC2_MISSING_RUNWAY_FIX            # killswitch -- MISSING-RUNWAY-1: =0 restores blanket decal skip (ef0f96d5)
    MC2_TERRAIN_TILE_CENSUS           # diagnostic -- per-tile overlay/cement census probe (ef0f96d5)
    MC2_DPI_UNAWARE                   # killswitch -- DPI-AWARE-1: =1 restores DPI-virtualized window (4da86076 cherry-pick)
    MC2_PAD_EDGE_ROAD_FIX             # killswitch -- PAD-EDGE-ROAD-1: =0 restores analytic pad edge (road notch)
    MC2_OVERLAY_BIAS_LEGACY           # killswitch -- OVERLAY-BIAS-LINEARIZE-1: =1 restores constant NDC overlay bias
    MC2_CEMENT_ATLAS_SEAM_FIX         # killswitch -- CEMENT-ATLAS-SEAM-1: =0 restores gutter-less atlas sampling (tile seam grid)
    MC2_LIGHT_POOL_SOFT               # killswitch -- NIGHT-LIGHT-POOLS-2: =0 restores exact linear GetFalloff pool edge
    MC2_LIGHT_POOL_MOVER_DIM          # override -- NIGHT-LIGHT-POOLS-2/4: mover-light pool dim factor (default 0.25 since POOLS-4)
    MC2_LIGHT_POOL_MOVER_RADIUS       # override -- NIGHT-LIGHT-POOLS-4: mover pool radius scale (default 0.6, 1 = legacy radii)
    MC2_LIGHT_POOL_MOVER_MAXBLEND     # killswitch -- NIGHT-LIGHT-POOLS-4: =0 restores additive mover-pool stacking
    MC2_MOVER_CONTACT_SHADOW          # override -- MOVER-CONTACT-SHADOW-1 (MASTER-FAIL #7): under-mech night contact shadow strength 0..1 (default 0=off, byte-identical)
    MC2_LIGHT_POOL_VIS_GATE           # killswitch -- LIGHT-POOL-VIS-GATE-1: =0 restores packing inactive lights (FoW leak)
    MC2_LIGHT_CULL_RADIUS             # killswitch -- LIGHT-CULL-RADIUS-1: =0 restores position-only light screen test (pool popping)
    MC2_LIGHT_POOL_MOVER_FWD          # override -- MOVER-LIGHT-FORWARD-1: mover pool forward bias wu (default 24, clamp [0,120]; 0 = centered)
    MC2_LIGHT_POOL_OBJECTS            # killswitch -- POOL-CONSUMERS-1: =0 drops pool light from prop/mover shaders (terrain-only pools)
    MC2_LIGHT_CONE_NODES              # killswitch -- LIGHT-CONE-MODELS-1: =0 restores heuristic pool centers (no authored-cone aim/spill)
    MC2_LIGHT_CONE_GLOW               # killswitch -- LIGHT-CONE-MODELS-1: =0 hides the additive authored light-cone beams at night
    MC2_LIGHT_CONE_GLOW_ALPHA         # override -- LIGHT-CONE-MODELS-1: peak cone-glow vertex alpha 0..255 (default 40; 255 = exaggeration probe)
    MC2_LIGHT_CONE_SPILL              # override -- LIGHT-CONE-MODELS-1: spill anchor scale on authored cone length (default 1.0, clamp [0,3])
    MC2_LIGHT_MOVER_VIS_GATE          # PARKED default-OFF (GHOST-CONE-VERIFY-4) -- =1 arms the mover fairness gate: suppresses mover lights/cones for sensor-blip-only enemies. Parked because default-ON was reported to blind mech lights in a tier1 smoke.
    MC2_LIGHT_CONE_VIS_GATE           # PARKED default-OFF (GHOST-CONE-VERIFY-4) -- =1 arms the cone staleness guard + the alpha-based cone-GEOMETRY suppression. Parked alongside MC2_LIGHT_MOVER_VIS_GATE.
    MC2_VIS_SEMANTICS                 # killswitch -- VISIBILITY-SEMANTICS-1: default ON. Separates the four jobs of "visible" (mesh drawn / known to player / emits light / is lit). =0 restores the parked legacy predicates. SUPERSEDES MC2_LIGHT_MOVER_VIS_GATE + MC2_LIGHT_CONE_VIS_GATE.
    MC2_FORCE_ALL_VISIBLE             # diagnostic -- VISIBILITY-SEMANTICS-1: =1 forces every mover to read as drawn/known/emitting/lit. Never ship enabled.
    MC2_VIS_SEMANTICS_TRACE           # trace -- VISIBILITY-SEMANTICS-1: =1 dumps [VIS-SEM v1] per-mover predicate state (sensorLevel/meshAlpha/status/relation + the four verdicts)
    MC2_VIS_SEMANTICS_TRACE_EVERY     # override -- VISIBILITY-SEMANTICS-1: trace sample period in frames (default 120)
    MC2_LIGHT_CONE_DIAG               # trace -- CONE-AIM-DIAG-1: =1 dumps per-node apex/aim + cone-queue events (throttled)
    MC2_LIGHT_CONE_AIM_FLIP           # probe -- CONE-AIM-FLIP-1: =1 negates the computed cone aim (wrong-side A/B probe, default OFF)
    MC2_LIGHT_POOL_DUMP               # trace -- GHOST-LIGHT-DUMP-1: =1 dumps packed light-pool entries w/ slot+owner every 240 frames
    MC2_LIGHT_REALISM                 # feature -- LIGHT-REALISM-1: =1 enables feathered translucent cones + emitter-continuous pool falloff (default OFF)
    MC2_LIGHT_REALISM_CONE_OPACITY    # override -- LIGHT-REALISM-1: cone peak-alpha scale (default 0.55, clamp [0,4]); lower = more transparent
    MC2_LIGHT_REALISM_CONE_FEATHER    # override -- LIGHT-REALISM-1: screen-space silhouette feather exponent (default 1.5, clamp [0,8]; 0 = no feather)
    MC2_LIGHT_REALISM_POOL_SPILL      # override -- LIGHT-REALISM-1: pool farDistance scale so the pool reaches back to its emitter (default 1.6, clamp [0.25,4])
    MC2_LIGHT_REALISM_POOL_CORE       # override -- LIGHT-REALISM-1: pool closeDistance scale; shrinks the flat-topped bright disc (default 0.15, clamp [0,1])
    MC2_LIGHT_REALISM_BEACON          # override -- LIGHT-REALISM-1: beacon ground-glow peak alpha 0..255 (default 45; subtle by design)
    MC2_LIGHT_POOL_MAX                # override -- LIGHT-POOL-CAP-1: max simultaneous terrain pool lights (default 256, was 64; clamp [1,4096])
    MC2_LIGHT_REALISM_BEACON_RADIUS   # override -- LIGHT-REALISM-1: beacon ground-glow disc radius in world units (default 28, clamp [1,200])
    MC2_STATICPROP_RECIPE_SPOTLIGHT_SKIP  # killswitch -- GHOST-CONE-RECIPE-SKIP-1: =0 restores baking SpotLight_ cone meshes into static recipes (opaque tan wedges)
    MC2_LIGHT_POOL_OBJ_SCALE          # override -- POOL-CONSUMERS-1: object-side pool intensity scale (default 0.7, clamp [0,2])
    MC2_WATER_SHORE_MASK              # killswitch -- WATER-SHORE-MASK-1: =0 restores tile-staircase water shore band (no mask feather)
    MC2_SHADOW_MIN_SUN_ALT            # override -- SHADOW-SUN-ALT-CLAMP-1: min shadow sun altitude degrees (default 15, 0 disables)
    MC2_SHADOW_DIST_FADE              # override -- SHADOW-DIST-FADE-1: dynamic-shadow dissolve ray distance wu (default 80, 0 = killswitch)
    MC2_SHADOW_DIST_FADE_MAX          # override -- SHADOW-DONUT-FIX-1: max fade opacity [0,1] (default 0.65; 1 = old full erase)
    MC2_SHADOW_FADE_CONT              # killswitch -- SHADOW-FADE-CONTINUOUS-1: =0 restores 4-step occluder-distance ladder (stepped fade)
    MC2_SHADOW_FADE_RAMP0             # killswitch -- SHADOW-FADE-RAMP0-1: =0 restores 0.45-onset fade window (crisp/soft seam)
    MC2_SHADOW_ALT_SCALE              # killswitch -- SHADOW-ALT-SCALE-1: =0 restores fixed low-sun fade gains at all sun altitudes
    MC2_LIGHT_POOL_MOVER_COLOR        # override -- MOVER-LIGHT-HALOGEN-1: mover light RRGGBB hex (default FFEED1 halogen; FFFF00 = legacy yellow)
    MC2_PAD_AREA_GLOW                 # override -- PAD-AREA-GLOW-1: night pad-area lift strength (default 0.12, 0 = killswitch)
    MC2_PAD_AREA_GLOW_SPILL           # override -- PAD-AREA-GLOW-1: spill feather distance wu past pad edge (default 48, clamp [4,127])
    MC2_WATER_FULL_RECIPE_SPIKE       # trace
    MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE  # feature -- water GPU full-recipe authoritative gate (documented in debug_state_dump, default true)
    MC2_WATER_GPU_FULL_RECIPE_CULL    # feature -- water GPU full-recipe cull gate
    MC2_WATER_HDRI_LOD                # override -- water HDRI LOD override
    MC2_WATER_NO_DEPTH_WRITE          # feature -- water no-depth-write gate
    MC2_WATER_SHINE                   # feature -- water shine gate
    MC2_WATER_THINRING_TRACE          # trace

    # --- batch 8 (ENV-DROP sweep 2026-07-07: recent slices that skipped registration) ---
    MC2_3DVIEW_DIAG                   # trace -- one-shot 3D-view composite diagnostics (gameos_graphics.cpp)
    MC2_BOOKMARK_SAVE_DIR             # override -- output dir for in-game Save Camera Bookmark button (default ./saved_bookmarks)
    MC2_BOOKMARK_SAVE_TEST            # infra -- one-shot auto bookmark save ~120 frames after mission load (button round-trip proof)
    MC2_CAMERA_ASPECT_NATIVE          # feature -- native window-aspect camera projection (default ON; =0 restores legacy 4:3 stretch)
    MC2_DRAW_PACKET_COALESCE_V5       # feature -- static-prop DrawPacket v5 per-draw substitutive dispatch (opt-in =1, needs ARB_base_instance)
    MC2_DRAW_PACKET_COALESCE_V5_TRACE # trace
    MC2_DRAW_PACKET_STATIC_PROP_V6_TRACE  # trace -- v7 canonical packet dispatch trace (v6 gate itself retired)
    MC2_DRAW_PACKET_V3                # feature -- DrawPacket v3 candidate->ABI promotion, no dispatch (opt-in =1)
    MC2_EDITOR_UI_DEFS                # feature -- editor shell data/defs UI path (default ON; =0 forces legacy pure-MFC shell)
    MC2_FORCE_43                      # feature -- forced 4:3 letterbox composite box (launcher toggle; default OFF)
    MC2_GAMEOS_UI_DEFS                # feature -- GameOS data/defs UI replacement path (default ON; =0 forces legacy FIT path)
    MC2_HDRI_BC6H                     # feature -- HDRI sky BC6H .ktx2 sidecar upload (default ON; =0 forces RGBA16F fallback, documented tier1)
    MC2_HUD_SCALE                     # override -- HUD scale env seed [0.2,1.0] (ImGui slider wins after)
    MC2_LEGACY_LOAD_BANNER            # legacy -- restore retired world-floating load banner overlay (default OFF)
    MC2_M2D_PZ_PARITY                 # parity -- quad pz clip-vs-legacy probe, demoted-not-deleted (default OFF)
    MC2_PNG_MAGENTA_KEY               # feature -- PNG magenta->alpha keying at decode (default ON; =0 killswitch)
    MC2_PREVIEW_FBO_SCALE             # override -- camera-preview FBO supersample factor 1-8 (default 4)
    MC2_PREVIEW_NOCULL                # override -- panel-preview force-disable GL_CULL_FACE (FBO winding debug)
    MC2_R2B_STATIC_NATURAL_TRACE      # trace -- documented tier1
    MC2_R2B_TOUCH_PRESERVE            # feature -- R2b tree cachedFrame_ liveness stamp killswitch (default ON; =0 reproduces tree drop, documented tier1)
    MC2_REGFLUSH_GUARD2               # trace -- static-registry persistent-vanish streak discriminator (default OFF, per-range map cost)
    MC2_SCREENSHOT_SOURCE             # infra -- screenshot capture source select (=backbuffer for logistics/menu screens)
    MC2_SHADOW_CSM_R0                 # override -- CSM near cascade radius WU (default 512, min 64)
    MC2_SHADOW_CSM_R1                 # override -- CSM mid cascade radius WU (default 4096, min 128)
    MC2_SHADOW_STATIC_BANDFIX         # feature -- static-shadow Poisson-tap banding fix v1 (default ON 2026-07-18; =0 kills)
    MC2_SHADOW_STATIC_BANDFIX_V2      # feature -- slope-normal shadow-bias banding fix v2 (default ON 2026-07-18; =0 kills)
    MC2_SHOT_RESULTS                  # infra -- RESULTS-CAPTURE-1 deterministic AAR screenshots (pairs with MC2_DEV_SHELL)
    MC2_SOAK_STOP_AT_BAY              # infra -- soak harness park at mech bay (requires MC2_SOAK_AUTOWIN)
    MC2_STANDARD_LIT_V1               # feature -- mech StandardLit GGX gate (opt-in =1; temporary default-OFF)
    MC2_STATIC_C1B_DRAW               # override -- re-enable (buggy) C1b GPU indirect instance count for static-prop draw (default OFF)
    MC2_TERRAIN_CLIFF_SHADOW_FLOOR    # feature -- cliff shadow-side floor lift, env is authoritative arm (unset/=0 byte-identical no-op)
    MC2_TERRAIN_DECAL_TRACE           # trace -- CLIFF_WALL mesh-decal placement trace
    MC2_UI_ASPECT_ANCHOR              # feature -- centered 16:9 UI canvas anchor (default ON; =0 legacy full-surface, documented tier1)
    MC2_UI_DEFS_IGNORE_PACKAGE_VERSION  # override -- bypass UI defs package version check (modder iteration escape hatch)
    MC2_UI_DEFS_LOADSCREEN            # feature -- opt loadscreen pages back into data/defs UI (default OFF = pinned legacy)
    MC2_UI_DEFS_RESULTS               # feature -- opt mission-results page into data/defs UI (default OFF = pinned legacy)
    MC2_UI_DEFS_TRACE                 # trace
    MC2_WATER_S6_COST                 # trace -- water S6 setupTextures QPC cost summary (600-frame idiom)
    MC2_WATER_S6_TRACE                # trace -- water S6 armed-skip ownership transitions
    MC2_WINDOW_RESIZABLE              # feature -- SDL resizable window when windowed (default ON with MC2_WINDOWED; =0 fixed)

    # --- batch 9 (SIM-CLOCK-SPEED-1 port + SIM-SPEED-BUTTON-1, 2026-07-09) ---
    MC2_HUD_SIM_SPEED                 # feature -- HUD sim-speed tab 1x/2x/4x (requires MC2_HUD_IMGUI_TABS; default OFF)
    MC2_SIM_CLOCK_TRACE               # trace -- once-per-second sim/wall clock ratio stderr line
    MC2_SIM_SPEED                     # override -- sim-speed multiplier env seed [0.25,4.0], runtime-settable via HUD speed tab
    # --- batch 10 (HARNESS-HYGIENE-3, 2026-07-29): readers made visible by
    # closing the accessor-idiom blind spot in step 4. The scan previously
    # matched only getenv/envFlag/selftestEnvFlag/envFlagDefaultOn, so these
    # 66 vars were read by shipping code while CI reported PASS. Nothing here
    # is new code -- only newly VISIBLE. Categories assigned by reader idiom;
    # the VK native-post family and the lightgrid/cluster/blur trios are real
    # feature gates that should be promoted to the registry.
    MC2_CLUSTER_DEPTH_PYRAMID                 # feature -- read via envFlagDefaultOff in gos_cluster_depth_pyramid.cpp
    MC2_CLUSTER_DEPTH_PYRAMID_PLANT           # feature -- read via envFlagDefaultOff in gos_cluster_depth_pyramid.cpp
    MC2_CLUSTER_DEPTH_PYRAMID_VERIFY          # trace -- read via envFlagDefaultOff in gos_cluster_depth_pyramid.cpp
    MC2_CRASH_FAST_EXIT                       # feature -- read via read_env_flag in gos_crashbundle.cpp
    MC2_DEBUG_RENDERER_TEST                   # feature -- read via envFlagDefaultOff in debug_renderer.cpp
    MC2_FOO_AUDIT                             # infra -- NOT A REAL GATE -- doc/test fixture literal, newly visible to the broadened scan
    MC2_FRAME_JOBS_TOUCH_DIAG                 # trace -- read via ParseEnvBool in bdactor.cpp
    MC2_GPU_GV_SHADOW_SKIP                    # feature -- read via gvEnvFlagDefaultOn in gvactor.cpp
    MC2_HOSTSVC_DEFINITELY_UNSET_KEY_XYZ      # infra -- NOT A REAL GATE -- doc/test fixture literal, newly visible to the broadened scan
    MC2_HUD_IMGUI_TACMAP                      # feature -- read via HudImGui::envGateOn in gametacmap.cpp
    MC2_HZB_FORCE_HORIZON_ALT                 # override -- read via envf in gamecam.cpp
    MC2_HZB_FORCE_HORIZON_ANGLE               # override -- read via envf in gamecam.cpp
    MC2_HZB_FORCE_HORIZON_ROT                 # override -- read via envf in gamecam.cpp
    MC2_HZB_FORCE_HORIZON_X                   # override -- read via envf in gamecam.cpp
    MC2_HZB_FORCE_HORIZON_Y                   # override -- read via envf in gamecam.cpp
    MC2_LIGHTBRIDGE_STABLE_SKIP               # feature -- read via ParseEnvBool in bdactor.cpp
    MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG          # trace -- read via ParseEnvBool in bdactor.cpp
    MC2_LIGHTGRID_BUILD                       # feature -- read via envFlagDefaultOff in gos_lightgrid_build.cpp
    MC2_LIGHTGRID_PLANT                       # feature -- read via envFlagDefaultOff in gos_lightgrid_build.cpp
    MC2_LIGHTGRID_VERIFY                      # trace -- read via envFlagDefaultOff in gos_lightgrid_build.cpp
    MC2_LIGHT_CLAMP_FIXTURE                   # feature -- read via lightClampEnvFlagDefaultOff in txmmgr.cpp
    MC2_LIGHT_CLAMP_PROBE                     # trace -- read via lightClampEnvFlagDefaultOff in txmmgr.cpp
    MC2_LIGHT_ROW_CENSUS                      # trace -- read via lightClampEnvFlagDefaultOff in txmmgr.cpp
    MC2_PLAYTEST_EXE                          # override -- read via GetEnvironmentVariableA in EditorPlaytest.cpp
    MC2_POSTPROCESS_COMPUTE_BLUR              # feature -- read via envFlagDefaultOff in gos_postprocess_blur.cpp
    MC2_POSTPROCESS_COMPUTE_BLUR_PLANT        # feature -- read via envFlagDefaultOff in gos_postprocess_blur.cpp
    MC2_POSTPROCESS_COMPUTE_BLUR_VERIFY       # trace -- read via envFlagDefaultOff in gos_postprocess_blur.cpp
    MC2_RDC_MAX_WAIT                          # override -- read via envInt in gos_rdoc_capture.cpp
    MC2_SMOKE_ACTIVE                          # feature -- read via GetEnvironmentVariableA in EditorPlaytest.cpp
    MC2_STABLE_LIGHT_SKIP_DIAG                # trace -- read via ParseEnvBool in bdactor.cpp
    MC2_STABLE_LIGHT_SKIP_TOUCH               # feature -- read via ParseEnvBool in bdactor.cpp
    MC2_STATIC_PER_INSTANCE_LIGHT             # feature -- read via parseEnvBoolWithDefault in gos_static_prop_registry.cpp
    MC2_STATIC_PROP_LATE_SPAWN_REG            # feature -- read via parseEnvBoolWithDefault in gos_static_prop_registry.cpp
    MC2_STATIC_PROP_MISSION_LOAD_REG          # feature -- read via parseEnvBoolWithDefault in gos_static_prop_registry.cpp
    MC2_STATIC_UPDATE_TRACE                   # trace -- read via ParseEnvBool,suParseBool in gameosmain.cpp,terrobj.cpp
    MC2_TERRAIN_CONTROLMAP_ALBEDO             # feature -- read via envIsSet in terrain_material_lib.cpp
    MC2_TERRAIN_DETAIL_FADE_END               # override -- read via envF in gameos_graphics.cpp
    MC2_TERRAIN_DETAIL_FADE_START             # override -- read via envF in gameos_graphics.cpp
    MC2_TERRAIN_DETAIL_LOD1_STRENGTH          # override -- read via envF in gos_terrain_lod_chunk.cpp
    MC2_TERRAIN_DETAIL_MACRO_STRENGTH         # override -- read via envF in gameos_graphics.cpp,gos_terrain_lod_chunk.cpp
    MC2_VEGETATION_DENSITY                    # override -- read via envInt in VegetationAdapter.cpp
    MC2_VEGETATION_MAX                        # override -- read via envInt in VegetationAdapter.cpp
    MC2_VISUAL_DIFF_FRAME_N                   # feature -- read via envInt in gos_visual_diff.cpp
    MC2_VISUAL_DIFF_MAX_FRAMES                # feature -- read via envInt in gos_visual_diff.cpp
    MC2_VK_HUD                                # feature -- read via mc2vkgate::subGate in gameos_graphics.cpp
    MC2_VK_HUD_COLOR_PROOF                    # feature -- read via mc2vkgate::directChainGate in engine_present_vk.cpp
    MC2_VK_HUD_SKIP_GL                        # feature -- read via mc2vkgate::subGate in gameos_graphics.cpp
    MC2_VK_MECH_FEED_AUDIT                    # trace -- read via mc2vkgate::envTriState in vulkan_mech_island.cpp
    MC2_VK_MECH_FEED_NATIVE                   # feature -- read via mc2vkgate::envTriState in gos_mech_batcher.cpp
    MC2_VK_POST_COMPOSITE                     # feature -- read via mc2vkgate::directChainGate in engine_present_vk.cpp
    MC2_VK_POST_FOG_DIRECT_CHAIN              # feature -- read via mc2vkgate::directChainGate,mc2vkgate::envTriState in gos_postprocess.cpp,vulkan_post_fog_native.cpp
    MC2_VK_POST_FOG_NATIVE                    # feature -- read via mc2vkgate::subGate in vulkan_post_fog_native.cpp
    MC2_VK_POST_HZB                           # feature -- read via mc2vkgate::envTriState in vulkan_post_hzb_native.cpp
    MC2_VK_POST_HZB_DIRECT_CHAIN              # feature -- read via mc2vkgate::directChainGate,mc2vkgate::envTriState in gos_postprocess.cpp,vulkan_post_hzb_native.cpp
    MC2_VK_POST_SCREENFX                      # feature -- read via mc2vkgate::envTriState in vulkan_post_screenfx_native.cpp
    MC2_VK_POST_SCREENFX_CLOUD                # feature -- read via mc2vkgate::envTriState in vulkan_post_screenfx_native.cpp
    MC2_VK_POST_SCREENFX_DIRECT_CHAIN         # feature -- read via mc2vkgate::directChainGate,mc2vkgate::envTriState in gos_postprocess.cpp,vulkan_post_screenfx_native.cpp
    MC2_VK_POST_SCREENFX_SHADOW               # feature -- read via mc2vkgate::envTriState in vulkan_post_screenfx_native.cpp
    MC2_VK_POST_SCREENFX_SHORELINE            # feature -- read via mc2vkgate::envTriState in vulkan_post_screenfx_native.cpp
    MC2_VK_POST_SSAO                          # feature -- read via mc2vkgate::envTriState in vulkan_post_ssao_native.cpp
    MC2_VK_POST_SSAO_DIRECT_CHAIN             # feature -- read via mc2vkgate::directChainGate,mc2vkgate::envTriState in gos_postprocess.cpp,vulkan_post_ssao_native.cpp
    MC2_VK_PRESENT_DIRECT_FBO                 # feature -- read via mc2vkgate::directChainGate in engine_present_vk.cpp
    MC2_VK_TERRAIN_ALL_LODS                   # feature -- read via mc2vkgate::subGate in vulkan_terrain_island.cpp
    MC2_VK_TERRAIN_SKIRTS                     # feature -- read via mc2vkgate::subGate in vulkan_terrain_island.cpp
    MC2_VK_TERRAIN_VISUALDISPLACE_FAR         # feature -- read via mc2vkgate::envTriState in vulkan_terrain_island.cpp
    MC2_VK_WATER_FENCE_DEFER                  # feature -- read via mc2vkgate::envTriState in vulkan_water_island.cpp
    MC2_VK_WATER_TIER1                        # feature -- read via mc2vkgate::subGate,subGate in engine_present_vk.cpp,vk_shared_gbuffer_gate.h,vulkan_water_island.cpp
)

if [[ $VERBOSE -eq 1 ]]; then
    echo "=== Allowlist (${#ALLOWLIST[@]}) ==="
    printf '  %s\n' "${ALLOWLIST[@]}"
fi

# ---------------------------------------------------------------------------
# Step 3: build combined known set
# ---------------------------------------------------------------------------
declare -A KNOWN
for v in "${REGISTERED[@]}";  do KNOWN["$v"]=1; done
for v in "${ALLOWLIST[@]}";   do KNOWN["$v"]=1; done

# ---------------------------------------------------------------------------
# Step 4: extract all MC2_* vars used in source
# ---------------------------------------------------------------------------
# HARNESS-HYGIENE-3 -- BLIND SPOT CLOSED.
#
# This step used to match only FOUR hardcoded accessor names:
#     (getenv|envFlag|selftestEnvFlag|envFlagDefaultOn)\s*\(\s*"MC2_..."
# and it therefore reported PASS while not seeing 134 readers. The codebase
# reads env vars through 25+ other idioms -- mc2vkgate::envTriState,
# mc2vkgate::subGate, envFlagDefaultOff (!), parseEnvBoolWithDefault,
# ParseEnvBool, envFloat, envInt, envF, envf, tglc_envOn, tglc_envStrength,
# mc2AuditLatch, HudImGui::envGateOn, read_env_flag, lightClampEnvFlagDefaultOff,
# gvEnvFlagDefaultOn, bdForceDynamicDefault, directChainGate, ... The
# envFlagDefaultOn/envFlagDefaultOff pair is the clearest illustration: the
# "On" spelling was listed, the "Off" spelling was invisible.
#
# A name whitelist cannot keep up with that, and every gap in it is silent and
# fail-OPEN. So match ANY identifier immediately followed by an "MC2_*" string
# literal, and subtract the known env WRITERS (which are not reads). This
# over-reports rather than under-reports: a false positive costs one ALLOWLIST
# row with a comment, whereas a false negative is an unregistered gate that CI
# swears is accounted for.
#
# Excludes the registry header itself (it's the registry, not a consumer).
# Excludes scripts/ and docs/ to avoid false positives from comments.

SOURCE_DIRS=(
    "$WORKTREE/GameOS"
    "$WORKTREE/GameAdapters"
    "$WORKTREE/RenderWorld"
    "$WORKTREE/RenderCore"
    "$WORKTREE/mclib"
    "$WORKTREE/code"
    "$WORKTREE/editor"
    "$WORKTREE/GuiRuntime"
    "$WORKTREE/gui"
    "$WORKTREE/EditorBridge"
    "$WORKTREE/tests"
)

# Identifiers that WRITE the environment -- matching them would register vars
# that no code ever reads. Anchored as whole trailing identifiers.
ENV_WRITERS='(_putenv_s|_putenv|putenv|setenv|SetEnvironmentVariableA|SetEnvironmentVariableW)'

FOUND=()
for dir in "${SOURCE_DIRS[@]}"; do
    [[ -d "$dir" ]] || continue
    while IFS= read -r name; do
        FOUND+=("$name")
    done < <(
        grep -rh --include='*.cpp' --include='*.h' --include='*.hpp' \
                 --include='*.cc' --include='*.cxx' --include='*.c' --include='*.inl' \
             -oE '[A-Za-z_][A-Za-z0-9_:]*[[:space:]]*\([[:space:]]*"MC2_[A-Z0-9_]+"' "$dir" 2>/dev/null \
        | grep -vE "(^|[^A-Za-z0-9_])${ENV_WRITERS}[[:space:]]*\(" \
        | grep -oE '"MC2_[A-Z0-9_]+"' \
        | tr -d '"' \
        | sort -u
    )
done

# Deduplicate FOUND across all dirs
mapfile -t FOUND < <(printf '%s\n' "${FOUND[@]}" | sort -u)

if [[ $VERBOSE -eq 1 ]]; then
    echo "=== Used in source (${#FOUND[@]}) ==="
    printf '  %s\n' "${FOUND[@]}"
fi

# ---------------------------------------------------------------------------
# Step 5: report unknowns
# ---------------------------------------------------------------------------
FAIL=0
UNKNOWN=()
for name in "${FOUND[@]}"; do
    if [[ -z "${KNOWN[$name]+x}" ]]; then
        UNKNOWN+=("$name")
        FAIL=1
    fi
done

if [[ $FAIL -eq 1 ]]; then
    echo "FAIL: check-env-registry.sh -- unregistered MC2_* env vars found:" >&2
    for u in "${UNKNOWN[@]}"; do
        echo "  $u" >&2
    done
    echo "" >&2
    echo "Fix: add to kFeatureTable or kAuxEnvVars in RenderCore/RendererFeatureRegistry.h" >&2
    echo "     OR add to ALLOWLIST in scripts/check-env-registry.sh with a category comment." >&2
    exit 1
fi

echo "PASS: check-env-registry.sh -- all ${#FOUND[@]} MC2_* env vars accounted for (${#REGISTERED[@]} registered, ${#ALLOWLIST[@]} allowlisted)"
exit 0
