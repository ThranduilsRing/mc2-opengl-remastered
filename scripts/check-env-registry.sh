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
# Greps all "MC2_[A-Z_]+" string literals in the header (both tables +
# any comments that happen to quote a var name).

REGISTERED=()
while IFS= read -r name; do
    REGISTERED+=("$name")
done < <(grep -oE '"MC2_[A-Z_]+"' "$HEADER" | tr -d '"' | sort -u)

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
    MC2_ABL_REG_TRACE           # trace
    MC2_ABL_TRACE               # trace
    MC2_ALPHA_TEST_TRACE        # trace
    MC2_ASSET_SCALE_SELFTEST    # infra -- startup self-test
    MC2_ASSET_SCALE_TRACE       # trace
    MC2_BATCHER_FLUSH_TIMING    # trace
    MC2_BLOCK_FRUSTUM_FALLBACK  # feature -- promote-to-registry
    MC2_BLDG_DIAG_TRACE         # trace
    MC2_BLDG_REG_TRACE          # trace
    MC2_BLKIDX_TRACE            # trace
    MC2_BUCKET_CENSUS           # trace
    MC2_BUCKET_HEADER_TRACE     # trace
    MC2_COALESCE_FORCE_DISARM   # override
    MC2_COALESCE_GPU_VS_CPU_COUNT_TRACE  # trace
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
    MC2_SLIM_COST_SPLIT         # feature -- promote-to-registry
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
    MC2_SKYBOX_FOG_EXCLUDE      # feature -- SKYBOX-FOG-EXCLUDE-1 fog passes exclude stencil-tagged sky (default OFF)
    MC2_TERRAIN_CONTROLMAP      # feature -- TERRAIN-CONTROLMAP-SAMPLE-1 authored material-weight override (default OFF)
    MC2_TERRAIN_CONTROLMAP_ALBEDO_STRENGTH  # override -- TERRAIN-CONTROLMAP-ALBEDO-1 strength env override
    MC2_TERRAIN_CONTROLMAP_FILE # override -- explicit control-map path override
    MC2_TERRAIN_MATERIAL_LIB    # feature -- TERRAIN-MATERIAL-LIB-1 data-defined terrain material layers (default OFF)
    MC2_TERRAIN_MATERIAL_LIB_FILE  # override -- explicit terrain_materials.json path override
    MC2_TERRAIN_SHORELINE       # feature -- TERRAIN-SHORELINE-MASK-1 wetness/foam shoreline mask (default OFF)
    MC2_TERRAIN_SHORELINE_FILE  # override -- explicit shoreline mask path override
    MC2_TERRAIN_VISUAL_DISPLACE # feature -- TERRAIN-VISUAL-HEIGHT displacement from 4x bake (default OFF)
    MC2_TERRAIN_VISUAL_DISPLACE_FAR  # override -- far-band displacement scale knob
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
    MC2_TREE_DIAG_TRACE         # trace
    MC2_TREE_REG_TRACE          # trace
    MC2_VERTEX_PROJECT_FAST     # feature -- promote-to-registry
    MC2_VERTEX_PROJECT_PARITY   # parity
    MC2_VISUAL_DIFF_CAPTURE     # infra -- visual diff tool
    MC2_VISUAL_DIFF_MISSION     # infra -- visual diff tool
    MC2_VISUAL_DIFF_OUT         # infra -- visual diff tool
    MC2_VISUAL_TUNING_FILE      # infra -- visual tuning profile path override (visual_tuning_profile.cpp)
    MC2_VPL_CULL                # feature -- promote-to-registry
    MC2_VPL_PICK                # feature -- promote-to-registry
    MC2_VPL_REDUCE              # feature -- promote-to-registry
    MC2_VSYNC                   # infra
    MC2_WATER_GATE_DIAG         # trace
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
# Matches getenv("MC2_...") and envFlag("MC2_...") in .cpp / .h files.
# Excludes the header itself (it's the registry, not a consumer).
# Excludes scripts/ and docs/ to avoid false positives from comments.

SOURCE_DIRS=(
    "$WORKTREE/GameOS"
    "$WORKTREE/GameAdapters"
    "$WORKTREE/RenderWorld"
    "$WORKTREE/mclib"
    "$WORKTREE/code"
    "$WORKTREE/editor"
    "$WORKTREE/GuiRuntime"
    "$WORKTREE/EditorBridge"
)

FOUND=()
for dir in "${SOURCE_DIRS[@]}"; do
    [[ -d "$dir" ]] || continue
    while IFS= read -r name; do
        FOUND+=("$name")
    done < <(
        grep -rh --include='*.cpp' --include='*.h' \
             -oE '(getenv|envFlag|selftestEnvFlag)\s*\(\s*"MC2_[A-Z_]+"' "$dir" 2>/dev/null \
        | grep -oE '"MC2_[A-Z_]+"' \
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
