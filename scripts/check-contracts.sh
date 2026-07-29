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
run_check "harness_gate_drive"     "py -3 scripts/check-harness-gate-drive.py"
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
# VERIFY-INTEGRITY-3: bookmark poses must be able to SHOW what they grade.
# A pose framing empty terrain does not fail an A/B -- it PASSES both arms and
# manufactures evidence (mc2_10: byte-identical frames, 4801 logged rejects).
run_check "bookmark_honesty"       "py -3 scripts/check-visual-bookmark-honesty.py"
# QUIET-DEFAULT-BUILD-1: a DEFAULT build must not grow new unconditional
# diagnostic output -- that log is what a real user sees on a normal launch.
# Runtime check, not a grep: "is this printf gated?" is not answerable
# lexically (getenv-seeded statics, MC2_*_DIAG bitmasks, default-ON IsEnabled(),
# #ifdefs, gated enclosing functions). It grades the newest smoke run that was
# taken with NO output-affecting MC2_* env, and SKIPs (exit 0) when no such run
# exists, so it is safe on a machine that has not smoked.
run_check "default_build_quiet"    "py -3 scripts/check-default-build-quiet.py"
# CAPTURE-DETERMINISM-1: two captures of the SAME arm must be identical, or
# every pixel A/B taken across two launches is measuring noise. Measured on
# mc2_10 (2026-07-26): up to 27.3% of pixels differed between byte-identical
# runs, while the sidecars agreed on frame, pose, seed AND deterministic:true.
# Grades the newest multi-run capture set on disk; SKIPs (exit 0) when none
# exists, so it never launches the game from the contract suite.
run_check "capture_determinism"    "py -3 scripts/check-capture-determinism.py --quiet"
# DETERMINISM-RATCHET-2: the ratchet above shipped BROKEN -- it globbed one flat
# level, so it was blind to tools/visual_verify's nested crop layout (the
# pipeline that produces the goldens which grade releases), reported EMPTY as a
# pass, and compared only 2 runs. It returned 0 with a 44%-noise 3-run set in
# its own search root. This check proves it still fires, on synthetic bad input,
# offline. A ratchet nobody watches fail is indistinguishable from one that
# cannot fail.
run_check "capture_determinism_ratchet" "py -3 scripts/check-capture-determinism.py --selftest"
# SMOKE-DEPLOY-TRAP-1: run_smoke.py printed "PASS (5/5)" alongside a
# DEPLOY_FINGERPRINT MISMATCH for eight separate lanes -- it had smoked another
# lane's stale exe, and several lanes reported verification results that were
# meaningless and had to be redone. DEPLOY-FINGERPRINT-DEFAULT-ON-1 (e01e8ac1)
# made that fatal by default; this check exists so the fix cannot rot back, and
# covers the three remaining ways to reopen the trap (flip the knob to opt-in,
# print the failure without clearing the verdict, or break the import/check so
# it is skipped silently). Offline, no game launch.
run_check "smoke_fingerprint_honesty" "py -3 scripts/check-smoke-fingerprint-honesty.py --quiet"
# ...and prove those guards actually fire, by injecting each regression into a
# copy of the source. A guard only ever seen passing on good input is not
# evidence that it works.
run_check "smoke_fingerprint_ratchet" "py -3 scripts/check-smoke-fingerprint-honesty.py --selftest"
# CAPTURE-ENV-PIN-1: run_visual_capture.py launched the engine with
# os.environ.copy(), so every stray MC2_* in the operator's shell rode into the
# captured frame unrecorded. Three vars removed from the registry persisted in
# every already-open shell and contaminated dozens of captures; one of them
# (MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE=1, default-OFF in the shipped game) hid
# 35 never-baked prop light slots and moved a capture hash. This check enforces
# both halves of the fix: capture harnesses pin their env through
# scripts/mc2_env_pin.py, and every capture manifest records the MC2_* set it
# actually ran under. Offline, no game launch.
run_check "capture_env_declared"   "py -3 scripts/check-capture-env-declared.py"
# ...and prove it fires, by injecting a contaminated manifest.
run_check "capture_env_ratchet"    "py -3 scripts/check-capture-env-declared.py --self-test"
# CAPTURE-RESOLUTION-ASSERT-1 (HARNESS-HYGIENE-3). Golden region rects are
# normalized fractions and verify.py silently nearest-neighbour-upscales a
# smaller candidate crop to the golden's dims, so a capture taken at the WRONG
# RESOLUTION crops, grades and blesses successfully. A deploy install shipped at
# 800x600 was graded against a 4K-referenced golden -- ~17x fewer pixels -- and
# only a human reading a sidecar caught it. No tool asserted resolution; now one
# does. Self-test only here (a live check needs a capture + golden pair).
run_check "capture_resolution_ratchet" "py -3 scripts/check-capture-resolution.py --selftest"
# ENV-DRIVE-GUARD-1 (HARNESS-HYGIENE-3). run_smoke's ENV-DROP guard used to
# refuse to start whenever ANY MC2_* var was set but unallowlisted, on the false
# premise that Popen's env replacement drops unlisted vars. runner.py builds the
# child env from os.environ.copy(), so nothing is dropped -- the guard was
# hard-stopping legitimate A/B arms. It now refuses only the genuinely
# UNDRIVEABLE set (FORCED_ENV / POPPED_ENV / conflicting env_extra) and RECORDS
# deliberate ambient gates in the run manifest.
run_check "env_drive_guard"        "py -3 scripts/check-env-drive-guard.py --quiet"
# ...and prove it fires, by mutating the runner's env tables.
run_check "env_drive_ratchet"      "py -3 scripts/check-env-drive-guard.py --selftest"
# DEPLOY-LANE-PARITY-1 (HARNESS-HYGIENE-3). Two deploy-lane registries exist
# (deploy_lease.DEPLOY_FOLDERS and check-deploy-target.GOOD_DIRS) and the MCP
# deploy_status tool answers from their UNION -- so it reports lanes that
# `run_smoke.py --deploy` cannot resolve, and tolerates one name mapping to two
# different trees. --allow-unresolved keeps the known 0.4d-rc1 path conflict a
# LOUD WARN (it needs a user ruling) while any NEW divergence hard-fails.
run_check "deploy_lane_parity"     "py -3 scripts/check-deploy-lane-parity.py --allow-unresolved"
# ...and prove it fires, on synthetic diverged registries.
run_check "deploy_lane_ratchet"    "py -3 scripts/check-deploy-lane-parity.py --selftest"
run_check "brain_fit_schema"       "py -3 scripts/check-brain-fit-schema.py --quiet"
run_check "brain_fixture_manifest" "py -3 scripts/check_brain_fixture_manifest.py --quiet"
run_check "brain_fixture_deploy"   "py -3 scripts/check_brain_fixture_deploy_sync.py --quiet"
run_check "brain_relaxed_guard"    "py -3 scripts/check_brain_relaxed_guard_doc.py --quiet"
run_check "deploy_data_parity"     "py -3 scripts/check-deploy-data-parity.py --quiet"
run_check "pass_coverage"          "py -3 scripts/check-pass-coverage.py --quiet"
run_check "render_ctx_parity"      "py -3 scripts/check-editor-game-render-context-parity.py --quiet"
run_check "editor_pass_parity"     "py -3 scripts/check-editor-pass-contract-parity.py --quiet"
run_check "gated_layout_macros"    "py -3 scripts/check-gated-layout-macros.py --quiet"
run_check "doc_links"             "py -3 scripts/check-doc-links.py --quiet"
# META-FIX-BATCH-1 (design C): split rendering-path contracts — color
# convention table (#24 class), depth-fudge single-source/form, tess predicate
# chokepoint (#34 class), GL/VK pair stamps. Self-test proves each rule still
# fails on a synthetic violation (vulkan_bindings_selftest precedent).
run_check "split_path_contracts"   "py -3 scripts/check-split-path-contracts.py --quiet"
run_check "split_path_selftest"    "py -3 scripts/check-split-path-contracts.py --self-test 2>/dev/null"
# META-FIX-BATCH-1 (design E): canonical-key chokepoint confinement
# (FastFileFind/elfHash callers) + raw read-open count ratchet.
run_check "path_normalization"     "sh scripts/check-path-normalization.sh"
run_check "path_norm_selftest"     "sh scripts/check-path-normalization.sh --self-test"
# META-FIX-BATCH-2 (design D): resolution/aspect two-space contract — modern
# dirs never read Environment.screenWidth/Height (RULE A), legacy-layer reads
# only shrink (RULE B generated ratchet), and the four #45/#49/HUD-RES-CLAMP
# fix sites survive (RULE C anchors). Runtime sibling: MC2_RES_ASSERT.
run_check "res_aspect_contract"    "py -3 scripts/check-res-aspect-contract.py --quiet"
run_check "res_aspect_selftest"    "py -3 scripts/check-res-aspect-contract.py --self-test 2>/dev/null"
# LAUNCHER-ENCODING-1: launcher-facing manifest strings (mods/ + examples/mods/
# mod.json) are ASCII-only — the launcher UI is Win32 ANSI, so UTF-8 typography
# (em-dash, curly quotes) renders as mojibake. Self-test proves a synthetic
# em-dash manifest still fails.
run_check "launcher_ascii"         "py -3 scripts/check-launcher-ascii.py --quiet"
run_check "launcher_ascii_selftest" "py -3 scripts/check-launcher-ascii.py --self-test"
# META-FIX-BATCH-2 (design F): silent-order chokepoint — every fireWeapon
# pre-delivery early-return is traced (fireGateTrace) or declared
# (KNOWN_UNCOVERED); comment-stripped scan (four commented return(4)s in
# mech.cpp are the trap); nofire emitter presence; both-ways-live ledger.
run_check "order_gate_coverage"    "py -3 scripts/check-order-gate-coverage.py --quiet"
run_check "order_gate_selftest"    "py -3 scripts/check-order-gate-coverage.py --self-test 2>/dev/null"
# META-FIX-BATCH-3 (design A): mover-registry coherence — setOnGUI/isOnGui
# write allowlists (S1), write-pairing windows (S2), trimmed read-gate pins
# (S3, hard=3), #42b hover-guard shape (S4 — SOLE defense, never demote),
# link-up gate shape (S5), off-map-staging carve-out: runtime audit region
# stays position-free (S6, INVIS-VEHICLES-FIX-1). Runtime sibling:
# MC2_MOVER_COHERENCE_AUDIT (force-set by run_smoke for --tier runs).
run_check "mover_registry_coherence" "sh scripts/check-mover-registry-coherence.sh"
run_check "mover_registry_selftest"  "sh scripts/check-mover-registry-coherence.sh --self-test"
# META-FIX-BATCH-3 (design B): fail-loud asset-load policy — raw loader-call
# confinement (P1, TGL-LOAD-EXEMPT/VERIFIED tags), PART 0-4 presence pins
# (P2), and the APPEAR-AUDIT off-map-staging carve-out (P3: scan region
# stays position/visibility-free — INVIS-VEHICLES-FIX-1).
run_check "tgl_load_callers"   "sh scripts/check-tgl-load-callers.sh"
run_check "tgl_load_selftest"  "sh scripts/check-tgl-load-callers.sh --self-test"

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
