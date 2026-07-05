#!/usr/bin/env bash
# scripts/check-contracts.sh
# CONTRACTS-AGGREGATOR-1: run all cheap static-contract checks and report a
# concise PASS/FAIL table.  Exits 0 only if every check passes.
#
# Usage:
#   bash scripts/check-contracts.sh           # run from worktree root
#   bash scripts/check-contracts.sh --quiet   # suppress individual script output
#
# What counts as "cheap":
#   * Pure grep/awk over source files (no build required)
#   * Run time under ~5 seconds on a cold file cache
#   * Idempotent and read-only
#
# Excluded (too heavy for this aggregator):
#   check-shader-schema.sh   -- requires glslangValidator + compiled goldens
#   check-debug-state-json.py -- requires a live engine JSON snapshot
#
# check-vulkan-bindings.py IS wired below (vulkan_bindings + vulkan_bindings_selftest).
#   Per-pipeline classifier (BINDING-CHECKER-PER-PIPELINE-CLASSIFIER-1): it ERRORs
#   (exit nonzero) ONLY on a same-pipeline distinct-resource share; disjoint-pipeline
#   shares are OK (allowlisted) or WARN (exit 0). Pre-existing acknowledged cases
#   (SSBO 14/20/23 disjoint, mask-solid slot-2) are in the allowlist, so the default
#   run is exit 0. The self-test proves a NEW same-pipeline dup still fails.
#   `--strict` additionally fails on any un-allowlisted WARN (future tightening).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKTREE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
QUIET=0
for arg in "$@"; do
    case "$arg" in
        --quiet|-q) QUIET=1 ;;
        --help|-h)
            echo "Usage: bash $0 [--quiet]"
            echo "  Runs cheap source-level contract checks and prints a PASS/FAIL table."
            echo "  Exits 0 if all pass, nonzero if any fail."
            exit 0
            ;;
        *) echo "Unknown argument: $arg" >&2; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Check runner
# ---------------------------------------------------------------------------
PASS_COUNT=0
FAIL_COUNT=0

# Arrays to collect results for the summary table.
NAMES=()
RESULTS=()

run_check() {
    local label="$1"
    local cmd="$2"

    if [ "$QUIET" -eq 1 ]; then
        if (cd "$WORKTREE_ROOT" && eval "$cmd" > /dev/null 2>&1); then
            PASS_COUNT=$((PASS_COUNT + 1))
            NAMES+=("$label")
            RESULTS+=("PASS")
        else
            FAIL_COUNT=$((FAIL_COUNT + 1))
            NAMES+=("$label")
            RESULTS+=("FAIL")
        fi
    else
        echo "--- $label ---"
        if (cd "$WORKTREE_ROOT" && eval "$cmd"); then
            PASS_COUNT=$((PASS_COUNT + 1))
            NAMES+=("$label")
            RESULTS+=("PASS")
        else
            FAIL_COUNT=$((FAIL_COUNT + 1))
            NAMES+=("$label")
            RESULTS+=("FAIL")
        fi
        echo ""
    fi
}

# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
run_check "env_registry"           "sh scripts/check-env-registry.sh"
run_check "material_gpu_mirror"    "sh scripts/check-material-gpu-mirror.sh"
run_check "visibility_log_schema"  "sh scripts/check-visibility-log-schema.sh"
run_check "include_firewall"       "sh scripts/check-include-firewall.sh"
run_check "no_raw_gl_from_game"    "sh scripts/check-no-raw-gl-from-game.sh"
run_check "vfx_no_objectid"        "sh scripts/check-vfx-no-objectid.sh"
run_check "destroy_invariant"      "sh scripts/check-destroy-invariant.sh"
run_check "render_contract_gbuf1"  "sh scripts/check-render-contract-gbuffer1.sh"
run_check "binding_slots"          "py -3 scripts/check-binding-slots.py --quiet"
run_check "light_abi_lockstep"     "py -3 scripts/check-light-abi-lockstep.py --quiet"
run_check "light_loop_numlights"   "py -3 scripts/check-light-loop-numlights-bound.py --quiet"
run_check "sampler_bindings"       "py -3 scripts/check-sampler-bindings.py --quiet"
run_check "pipeline_desc"          "py -3 scripts/check-pipeline-desc.py --quiet"
run_check "shader_injectors"       "py -3 scripts/check-shader-injectors.py --quiet"
run_check "pipeline_key"           "py -3 scripts/check-pipeline-key.py --quiet"
run_check "vfx_blend_distinction"  "py -3 scripts/check-vfx-blend-distinction.py --quiet"
run_check "multiply_blend"         "py -3 scripts/check-multiply-blend.py --quiet"
run_check "colormask_ownership"    "py -3 scripts/check-colormask-ownership.py --quiet"
run_check "pass_attachment"        "py -3 scripts/check-pass-attachment-contract.py --quiet"
run_check "frame_feedback"         "py -3 scripts/check-frame-resource-feedback.py --quiet"
run_check "drawbuffer_ownership"   "py -3 scripts/check-drawbuffer-ownership.py --quiet"
run_check "raw_gl_depthfunc"       "py -3 scripts/check-raw-gl-depthfunc.py --quiet"
run_check "raw_gl_depthmask"       "py -3 scripts/check-raw-gl-depthmask.py --quiet"
run_check "raw_gl_colormask"       "py -3 scripts/check-raw-gl-colormask.py --quiet"
run_check "raw_gl_blendfunc"       "py -3 scripts/check-raw-gl-blendfunc.py --quiet"
run_check "raw_gl_fbobind"         "py -3 scripts/check-raw-gl-fbobind.py --quiet"
run_check "raw_gl_capstone"        "py -3 scripts/check-raw-gl-capstone.py --quiet"
run_check "apply_pass_bumped"      "py -3 scripts/check-apply-pass-bumped.py --quiet"
run_check "static_prop_family"     "py -3 scripts/check-static-prop-family.py --quiet"
run_check "mdi_submission"         "py -3 scripts/check-mdi-submission-ownership.py --quiet"
run_check "buffer_lifetime"        "py -3 scripts/check-buffer-lifetime-ownership.py --quiet"
run_check "gpu_buffer_owners"      "py -3 scripts/check-gpu-buffer-owners.py --quiet"
run_check "texture_owners"         "py -3 scripts/check-texture-owners.py --quiet"
run_check "render_resource_ids"    "py -3 scripts/check-render-resource-ids.py"
run_check "vulkan_bindings"        "py -3 scripts/check-vulkan-bindings.py --quiet"
run_check "vulkan_bindings_selftest" "py -3 scripts/check-vulkan-bindings.py --self-test"
run_check "shader_variant"         "py -3 scripts/check-shader-variant.py --quiet"
run_check "spirv_artifacts"        "py -3 scripts/check-spirv-artifacts.py --quiet"
run_check "spirv_reflection"       "py -3 scripts/check-spirv-reflection-contract.py --quiet"
run_check "shader_package"         "py -3 scripts/check-shader-package.py --quiet"
run_check "visual_compare"         "py -3 scripts/visual_compare.py --self-test"
run_check "brain_fit_schema"       "py -3 scripts/check-brain-fit-schema.py --quiet"
run_check "brain_fixture_manifest" "py -3 scripts/check_brain_fixture_manifest.py --quiet"
run_check "brain_fixture_deploy"   "py -3 scripts/check_brain_fixture_deploy_sync.py --quiet"
run_check "brain_relaxed_guard"    "py -3 scripts/check_brain_relaxed_guard_doc.py --quiet"
run_check "deploy_data_parity"     "py -3 scripts/check-deploy-data-parity.py --quiet"
run_check "pass_coverage"          "py -3 scripts/check-pass-coverage.py --quiet"
run_check "render_ctx_parity"      "py -3 scripts/check-editor-game-render-context-parity.py --quiet"
run_check "editor_pass_parity"     "py -3 scripts/check-editor-pass-contract-parity.py --quiet"
run_check "gated_layout_macros"    "py -3 scripts/check-gated-layout-macros.py --quiet"

# ---------------------------------------------------------------------------
# Summary table
# ---------------------------------------------------------------------------
echo "====================================================================="
echo "  CONTRACT CHECK RESULTS"
echo "====================================================================="
for i in "${!NAMES[@]}"; do
    result="${RESULTS[$i]}"
    label="${NAMES[$i]}"
    if [ "$result" = "PASS" ]; then
        printf "  %-40s  PASS\n" "$label"
    else
        printf "  %-40s  FAIL  <---\n" "$label"
    fi
done
echo "---------------------------------------------------------------------"
printf "  %d passed, %d failed\n" "$PASS_COUNT" "$FAIL_COUNT"
echo "====================================================================="

if [ "$FAIL_COUNT" -gt 0 ]; then
    exit 1
fi
exit 0
