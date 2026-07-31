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
KNOWN_COUNT=0

# Arrays to collect results for the summary table.
NAMES=()
RESULTS=()
SECS=()

# ---------------------------------------------------------------------------
# PER-CHECK WALL TIME (UNWIRED-CHECKS-CLOSE-1, 2026-07-30)
# ---------------------------------------------------------------------------
# This file's own header promises "run time under ~5 seconds" per check. Nobody
# was measuring, so nobody knew: a full run is 360s over 130 rows. That matters
# for exactly the reason this lane exists -- a suite too slow to run before a
# commit only gets run when someone remembers, and "when someone remembers" is
# how six checks sat red for weeks without being seen. Slowness is not a
# cosmetic property of a guard; it is the mechanism by which guards go dark.
#
# The distribution is not "the suite is slow". Median is 0.24s and 75% of rows
# finish under a second. EIGHT rows breach the 5s promise and account for 79%
# of the runtime, and just TWO of them are 60% of the whole suite:
#     no_raw_gl_from_game  121.7s
#     include_firewall      95.8s
# Both are shell scripts doing repeated repo-wide greps. Fixing those two takes
# the suite from ~6min to ~2.4min without touching the other 128 rows.
#
# DELIBERATELY NOT A GATE YET. A budget ratchet wired today would be red on
# arrival on 8 rows, and a permanently-red row teaches everyone to skip red --
# the same blindness this lane spent its time removing, in a different colour.
# So this measures and reports; it does not fail. TRIGGER for making it a gate:
# once the two offenders above are fixed, set BUDGET_SEC and fail on new
# breaches.
BUDGET_SEC=5

_now_ms() { date +%s%N 2>/dev/null | cut -c1-13; }

# ---------------------------------------------------------------------------
# DUPLICATE REGISTRATION GUARD (PROVENANCE-FORGERY-CLOSE-1)
# ---------------------------------------------------------------------------
# `bless_refusals_selftest` was a byte-identical duplicate of `bless_selftest`:
# same command, different label, so nothing noticed. It counted one selftest
# twice. A board whose headline number is "122 passed" is a measurement, and a
# measurement that can silently double-count is not one.
#
# Registration is now checked in BOTH directions -- a repeated label, and a
# repeated command under a fresh label (the shape that actually happened). A
# duplicate is a bug in this file, not a contract failure, so it is reported as
# its own FAIL row (the suite must exit non-zero) and the command is NOT run
# again: re-running it would put the inflated count right back.
SEEN_LABELS=""
SEEN_CMDS=""

_register_row() {   # 0 = ok to run, 1 = duplicate (already reported)
    local label="$1" cmd="$2" why=""
    case "$SEEN_LABELS" in *"<${label}>"*) why="label '${label}' is already registered" ;; esac
    if [ -z "$why" ]; then
        case "$SEEN_CMDS" in *"<${cmd}>"*) why="command is byte-identical to an earlier row's" ;; esac
    fi
    if [ -n "$why" ]; then
        echo "DUPLICATE CONTRACT ROW: $why" >&2
        echo "    label: $label" >&2
        echo "    cmd:   $cmd" >&2
        echo "    A duplicated row double-counts one check as two greens." >&2
        FAIL_COUNT=$((FAIL_COUNT + 1))
        NAMES+=("DUPLICATE-ROW:$label")
        RESULTS+=("FAIL")
        return 1
    fi
    SEEN_LABELS="${SEEN_LABELS}<${label}>"
    SEEN_CMDS="${SEEN_CMDS}<${cmd}>"
    return 0
}

run_check() {
    local label="$1"
    local cmd="$2"

    _register_row "$label" "$cmd" || return 0

    local t0 t1
    t0="$(_now_ms)"
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
    t1="$(_now_ms)"
    SECS+=("$((t1 - t0))")
}

# ---------------------------------------------------------------------------
# TRI-STATE RUNNER -- so that RED still means "action required by you, now"
# ---------------------------------------------------------------------------
# GOLDEN-PROVENANCE-TRUTH-1 (2026-07-30). Some findings are correct, permanent
# and irreparable: capture_provenance reports two zero-byte capture manifests
# (ENOSPC truncation) that record no binary at all, so no tool can ever
# attribute them. A row that is red every single day teaches everyone to skip
# red -- at which point the next REAL failure is invisible.
#
# TRISTATE-WIRE-1 (2026-07-31) corrects what this paragraph used to claim. It
# said "ONE CHECK ON THIS BOARD FAILS PERMANENTLY AND CORRECTLY", naming
# capture_provenance, and that sentence was read for a day as "that row is the
# amber one". It is not, and never has been -- af4f79c8a introduced this runner
# and the goldens scan in ONE commit, so the row was red from birth. It exits 1,
# because only 2 of the 5 findings it reports are ledgerable and the other 3 are
# GOLDEN-UNATTRIBUTABLE, a code the ledger refuses on purpose. Opting a check
# into this runner does not make its row amber. The CHECK decides, per run, by
# reconciling every finding it has against the ledger -- and if even one finding
# is unpinned it returns 1 and the row is red. Measured, this session:
# amber(3) with only ledgered findings in scope; red(1) the moment one more
# unledgered finding appears, including one of the very same irreparable class.
#
# The usual cure is a suppression list, which can hide a real failure. This is
# not that. A check opted into this runner may return exit 3 to mean "there are
# failures and every one of them is pinned in an exact, expiring,
# irreparable-only ledger". The check itself proves that, and it fails RED if:
#   * a failure is NOT in the ledger              (a regression)
#   * a ledger entry matches NO failure           (stale entry -- or the check
#                                                  went blind and stopped
#                                                  seeing it)
#   * a ledger entry pins a REPAIRABLE finding    (fix it instead)
#   * a ledger entry is past its expiry
#
# KNOWN IS NOT PASS. It gets its own row, its own count, and its reason is
# printed even in --quiet. Any exit code other than 0 or 3 -- including a crash,
# a syntax error or a missing interpreter -- is FAIL, so this fails closed.
run_check_tri() {
    local label="$1"
    local cmd="$2"
    local out rc

    _register_row "$label" "$cmd" || return 0

    local t0 t1
    t0="$(_now_ms)"
    out="$( (cd "$WORKTREE_ROOT" && eval "$cmd") 2>&1 )" && rc=0 || rc=$?
    t1="$(_now_ms)"
    SECS+=("$((t1 - t0))")

    NAMES+=("$label")
    if [ "$rc" -eq 0 ]; then
        PASS_COUNT=$((PASS_COUNT + 1))
        RESULTS+=("PASS")
    elif [ "$rc" -eq 3 ]; then
        KNOWN_COUNT=$((KNOWN_COUNT + 1))
        RESULTS+=("KNOWN")
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        RESULTS+=("FAIL")
    fi

    if [ "$QUIET" -eq 1 ]; then
        # Even in --quiet, a KNOWN must state WHY it is known. A silent amber
        # row is indistinguishable from a green one at a glance.
        if [ "$rc" -eq 3 ]; then
            printf '%s\n' "$out" | grep -E 'KNOWN' || true
        elif [ "$rc" -ne 0 ]; then
            # TRISTATE-WIRE-1 (2026-07-31): ...and a FAIL must state why far
            # more urgently than a KNOWN does, because FAIL is the state that
            # means "act now". This runner ALREADY HAS the explanation sitting
            # in $out -- run_check throws its output at /dev/null and genuinely
            # cannot show you anything, but this one captured it and then
            # discarded it on every code except 3. So the one row on this board
            # equipped to tell "understood, pinned, bounded" apart from "new
            # regression" rendered, in the mode everybody actually runs, as a
            # bare `FAIL <---` with no reason attached at all.
            #
            # That is exactly how a red row becomes wallpaper: capture_provenance
            # has been red for three GOLDEN-UNATTRIBUTABLE findings the ledger
            # REFUSES to pin (see the row's own comment below), and lane after
            # lane walked past it as "known-red, not mine" because the amber
            # mechanism next to it was the only thing that ever explained itself.
            # Printing is strictly additive: it cannot pass a check that failed,
            # and it cannot change any count.
            echo "--- $label (FAIL, rc=$rc) ---"
            printf '%s\n' "$out"
            echo ""
        fi
    else
        echo "--- $label ---"
        printf '%s\n' "$out"
        echo ""
    fi
}

# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
run_check "env_registry"           "sh scripts/check-env-registry.sh"
# TREE-CONSOLIDATE-1 (2026-07-30): env_registry's answer is only as good as its
# comment stripper, and the stripper is the part that silently broke. A registry
# description that quoted a shell glob put a block-comment opener inside a string
# literal; the regex stripper read it as a real comment open, found no close, and
# discarded the header from that line to EOF -- so every gate registered below it
# reported UNREGISTERED and env_registry went red for two gates that were in fact
# registered correctly. Injection-proven both ways before and after the fix.
# The selftest pins the stripper against a fixture, so a future "simplification"
# of it fails here instead of silently unregistering half the header.
run_check "env_registry_selftest"  "sh scripts/check-env-registry.sh --selftest"

# ---------------------------------------------------------------------------
# GATE-DEFAULT-TRUTH-1 (2026-07-30)
# ---------------------------------------------------------------------------
# check-env-registry.sh above proves a gate is REGISTERED. It has never proved
# the registry's stated `defaultOn` AGREES with what the reader does when the
# var is unset -- and that gap shipped live lies: MC2_SHADOW_BOUNDED_NEAR_FIT
# (registry, tier1 doc, and the reader's own adjacent comment all said OFF while
# gos_postprocess.cpp implemented ON) and MC2_SHADOW_ENABLE (registry said
# opt-in while both GL readers were kill-switches). Anyone A/B-ing "=1 vs unset"
# off those entries graded ON against ON.
#
# UNCLASSIFIABLE counts as FAIL here, not as a silent pass: an unreadable idiom
# is precisely where a lie hides. Exemptions live in
# scripts/gate-defaults-allowlist.txt and each one MUST carry a real reason.
run_check "gate_default_agreement"          "py -3 scripts/check-gate-default-agreement.py --quiet"
run_check "gate_default_agreement_selftest" "py -3 scripts/check-gate-default-agreement.py --selftest"
# CPU-GPU-COGATING-1: the check above keeps a gate honest about its OWN default.
# This one keeps a MIRROR PAIR honest about EACH OTHER. When one quantity is
# computed twice -- CPU for gameplay, GPU for the picture -- the two halves must
# be controlled by one predicate. Written from the terrain-Phong defect: the GPU
# TES half was deleted by 75b8ebef and the CPU half kept feeding gameplay
# elevation for a month, up to ~19 wu off the drawn surface, while its own
# comment still claimed it was "matching GPU TES". Both endpoints were
# internally consistent and correctly registered, so no existing check could
# see it -- what broke was the RELATIONSHIP. Fails RED (never silently green) if
# an anchor stops matching, so it cannot go blind on the site it watches.
run_check "cpu_gpu_cogating"          "py -3 scripts/check-cpu-gpu-cogating.py --quiet"
run_check "cpu_gpu_cogating_selftest" "py -3 scripts/check-cpu-gpu-cogating.py --selftest"
# GATE-HYGIENE-CLOSE-1: the smoke harness must keep INHERITING the parent env
# (os.environ.copy()). If that ever changes, run_smoke.py's vestigial
# passthrough tuple becomes load-bearing and every registered gate missing
# from it starts silently dropping -- gate-ON smoke runs would execute the
# gate-OFF path and report "no difference" for the wrong reason. Also asserts
# the genuinely-undriveable guard still fails CLOSED (non-zero sys.exit).
run_check "smoke_env_inheritance"  "py -3 scripts/check-smoke-env-inheritance.py"
# GATE-PARITY-CLOSE-1: the OTHER half of the same problem. The check above keeps
# the harness INHERITING the parent env; this one keeps the declared passthrough
# allowlist honest, so the contract holds whichever mechanism is load-bearing.
# Step 1 of the "Adding a new MC2_* gate" checklist (register) is CI-blocking via
# check-env-registry.sh; step 4 (allowlist) was enforced by nothing --
# check-env-allowlist.sh is advisory by design AND is not wired into this file,
# and the only section it fails on under --strict is harness refs, not engine
# reads. MC2_TREE_DIAG_TRACE lived in that gap: registered nowhere, allowlisted
# nowhere, documented nowhere, so a smoke run driving it ON emitted no [TREE_DIAG]
# lines at all. Ratchets against a frozen 82-entry debt list; only NEW drops fail.
run_check "registered_gate_passthrough" "py -3 scripts/check-registered-gate-passthrough.py --quiet"
run_check "registered_gate_passthrough_selftest" "py -3 scripts/check-registered-gate-passthrough.py --selftest"
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
# LIGHTROW-FRESHNESS-1. cachedGpuLightIndex_ names a light slot that is rebased
# every frame; reading it without its cachedFrame_ stamp draws the actor against
# another actor's row (pure black, since the shader indexes light[] unchecked).
# Two of three consumers already guarded; the dynamic static-prop submit path
# did not. This freezes "every consumer references the stamp".
run_check "light_row_freshness"          "py -3 scripts/check-light-row-freshness.py --quiet"
run_check "light_row_freshness_selftest" "py -3 scripts/check-light-row-freshness.py --selftest"
# LIGHTING-DIVERGENCE-PIN-1: MC2 has no single shading model -- FIVE scalar
# ambient floors, FOUR hemisphere-fill colour pairs, two calc_light definitions
# and two no-lights semantics. Every one of those is deliberate today and
# documented in docs/lighting-divergence-map.md; none is visible from any one
# file. This pins them, so a FIFTH floor or a SIXTH hand-rolled light loop has
# to be argued for in review instead of landing silently. Exit 2 (check blind)
# is distinct from exit 1 (real drift); the comment-stripper witness is a live
# shipping shader, not a fixture.
run_check "lighting_divergence"    "py -3 scripts/check-lighting-divergence.py"
run_check "sampler_bindings"       "py -3 scripts/check-sampler-bindings.py --quiet"
# MULTIPLEX-ANCHOR-1 (VERIFY-DEBT-1): proves the per-pass anchor that replaced the
# inert DEMOTED_ANCHORS tuple actually FAILS on drift -- inverted unit comments,
# a rebound unit, a de-multiplexed name, an unrecorded third pass.
run_check "sampler_bindings_selftest" "py -3 scripts/check-sampler-bindings-selftest.py"
# GOLDEN-COVERAGE-LEDGER-1 (VERIFY-DEBT-1): committed golden sets must account for
# every region they could cover, and must contain no un-failable (flat) crops.
run_check "golden_coverage"          "py -3 scripts/check-golden-coverage.py --quiet"
run_check "golden_coverage_selftest" "py -3 scripts/check-golden-coverage.py --selftest"
# THREE-RUN-ALL-PAIRS-1 + DEGENERATE-CROP-REFUSAL-1 (VERIFY-DEBT-1): proves bless.py
# refuses a two-run set, a duplicated run, a degenerate crop and an unstable region.
run_check "bless_selftest"           "py -3 tools/visual_verify/bless_selftest.py"
run_check "pipeline_desc"          "py -3 scripts/check-pipeline-desc.py --quiet"
# DOCTEST-COUNT-LOCK-ARM-1: the doctest suite in tests/unit/ is not executed by
# anything in this file (or by any CI -- there is none), so its hand-pinned count
# assertions are unarmed: test_rendercore.cpp sat at `== 46` while the real
# RendererFeature::COUNT was 61, for fifteen appends. These two rows compare the
# pinned numbers against the headers statically, which needs no compiler and so
# can live in this ~5s-per-check aggregator. They do NOT restore test execution.
run_check "doctest_count_locks"    "py -3 scripts/check-doctest-count-locks.py --quiet"
run_check "doctest_count_selftest" "py -3 scripts/check-doctest-count-locks.py --self-test"
# DARK-TESTS-RUN-1 (2026-07-31): the sibling rows above arm the COUNT pins the
# unrun binary cannot. These arm the tier itself. check-contract-coverage.py
# made an unwired script in scripts/ structurally impossible and its docstring
# names the hole it leaves -- "It has no equivalent for tests/." This is that
# equivalent: a test source in no target, a target no runner names, an
# add_test() nothing drives, or a TEST_CASE newly hidden behind `#if 0` all
# fail here. Measured on the tree that produced them: test_render_contract_3
# was named by no runner AND did not link (LNK2019 x2), which is the state
# self-test arm 2 re-injects verbatim.
#
# Static only -- no compiler, so it stays inside this file's ~5s promise
# (measured 0.2s). Test EXECUTION deliberately stays in the manual tier with
# scripts/run-unit-tests.sh; see manual-checks.allowlist for why, and for the
# measured numbers that would justify promoting it.
run_check "test_tier_coverage"     "py -3 scripts/check-test-tier-coverage.py --quiet"
run_check "test_tier_selftest"     "py -3 scripts/check-test-tier-coverage.py --self-test"
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
# BACKGROUND-SMOKE-1: an agent run must never take focus, go fullscreen, or
# grab the user's cursor. Three bypasses are checked: a bare Popen of the
# engine, an env policy that STRIPS the windowing vars before they reach the
# child (how the capture path silently regressed), and raw focus/cursor Win32.
run_check "background_launch"      "py -3 scripts/check-background-launch.py --quiet"
# CURSOR-GRAB-FIX-1: the ENGINE-side half of the same policy. background_launch
# above guards the harness (Python) side, and it was fully green while the exe it
# launched clipped the user's physical pointer to a rect at (-32000,-32000):
# MC2_WINDOW_OFFSCREEN=1 shows the window offscreen, and apply_mouse_grab() gated
# only on gos_CursorControlSuppressed(), which that variable does not set. So a
# clean harness delivered the right env to an engine that ignored it. This check
# asserts, in C++, that no input-capturing call is reachable when the window is
# not user-visible -- and its --selftest re-injects that exact regression.
run_check "input_capture_visibility"          "py -3 scripts/check-input-capture-visibility.py --quiet"
run_check "input_capture_visibility_selftest" "py -3 scripts/check-input-capture-visibility.py --selftest"
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
# DELIVERY-GATE-1: every check above asks "is the fix CORRECT?". None of them
# asks the only question the user ever experiences -- "is the fix in the binary
# I run, ON by default, with its required data present?" -- because its four
# parts live in four tools (exe_fingerprint, gate-default-agreement,
# beauty-sidecars, deploy-data-parity) and nothing JOINed them. The cost was
# measured: 19 of 27 gates shipped default-OFF, the v0.5.2 release ships 0 of
# 28,459 files as .beauty sidecars, and VEHICLE-AMBIENT-FLOOR-1 landed
# default-ON while HALF DATA (its uniform exists in no deployed shaders/ tree,
# so glGetUniformLocation returns -1 and the gate runs OFF while reporting ON).
#
# --contract is the machine-independent half: it never touches an install, and
# fails when scripts/delivery-fixes.json describes code that is not there --
# a commit that does not exist, a needle absent from the path at the commit
# claiming it, a data path missing from the tree, or an expect_default that
# disagrees with the C++ READER in the working tree. Install grading needs
# binaries on disk and is run by hand: py -3 scripts/check-delivery.py
#
# Live-fired during bring-up, not just self-tested: flipping the real
# MC2_WATER_SHORE_MASK reader in GameOS/gameos/gameos_graphics.cpp from
# kill-switch to opt-in took this check to exit 2 while the whole rest of the
# suite stayed green -- that gate is allowlisted out of the registry
# (check-env-registry.sh:803), so GATE-DEFAULT-TRUTH-1 iterates registry
# entries and never sees it. An unregistered default-ON gate had nothing
# checking its default at all.
run_check "delivery_manifest"      "py -3 scripts/check-delivery.py --contract --quiet"
run_check "delivery_selftest"      "py -3 scripts/check-delivery.py --selftest"
# DELIVERY-CLOSE-1: THE GRADING HALF, WIRED. Until now the two rows above were
# the only wirings, and neither one ever looks at an install -- --contract is
# repo integrity and --selftest is fixtures. The half of the script that answers
# the actual question ("would the user see this if he launched?") was reachable
# only by a human typing it, and could not have failed anything if they had:
# its exit code was ONE `any` spanning every fix AND every install, so a run in
# which two fixes printed "<-- DELIVERED NOWHERE" still exited 0 because two
# other fixes landed somewhere. Measured on this disk: 4 of 9 fixes at 0/18
# installs, exit code 0. A gate that cannot return non-zero is a report.
#
# The gap is now per fix, and this row consumes it. THIS ROW IS EXPECTED RED
# UNTIL A BUILD AT THE TIP IS DEPLOYED: the newest engine binary on this disk
# was built at 09:53, and the afternoon's user-facing fixes (terrain ambient
# floor, the runway art guard, the vehicle ambient floor) are in no install at
# all. That is the finding, not a flaw in the row -- red here means "correct
# code that the user cannot reach", which is the one thing nothing else on this
# board measures. It goes green by deploying, not by editing this line.
#
# Deliberately NOT run_check_tri: that runner is for findings that are
# IRREPARABLE, and it fails closed on a ledger entry that pins a repairable one.
# A missing deploy is the most repairable finding there is.
run_check "delivery_installs"      "py -3 scripts/check-delivery.py --quiet"
run_check "pass_coverage"          "py -3 scripts/check-pass-coverage.py --quiet"
run_check "render_ctx_parity"      "py -3 scripts/check-editor-game-render-context-parity.py --quiet"
run_check "editor_pass_parity"     "py -3 scripts/check-editor-pass-contract-parity.py --quiet"
run_check "gated_layout_macros"    "py -3 scripts/check-gated-layout-macros.py --quiet"
# TERRAIN-AMBIENT-FLOOR-2: terrain_daylight.h duplicates camera.cpp's night-band
# constants so the harness can build standalone. Pin them, and prove the pin
# fires (the header's comment claimed they were pinned before anything was).
run_check "terrain_daylight_consts"  "py -3 scripts/check-terrain-daylight-constants.py --quiet"
run_check "terrain_daylight_selftest" "py -3 scripts/check-terrain-daylight-constants.py --self-test"
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
# COUNTER-HONESTY-1: instruments must not be able to state a confident number
# they cannot know. R1 = every syncStaticProp() recipe mint pairs with a
# setRecipePopulation() (an untagged recipe silently corrupts the population
# census). R2 = every first-N print limiter emits a self-declaring token, so a
# truncated stream cannot be read as a census; pre-existing sites are inventoried
# in scripts/counter-honesty-allowlist.txt and the rule ratchets on new ones.
# TREE-POP-TAG-1 adds R3 = every GpuStaticPropPopulation enumerator has a real
# setRecipePopulation() assignment site, or an inventoried reason in
# scripts/counter-honesty-population-inventory.txt. R1 asks "does this mint tag
# something?"; R3 asks the dual question R1 cannot — "is this VALUE ever
# assigned at all?" — which is what makes a reg_recipes_* field structurally
# incapable of being non-zero. Ratchets both ways (a stale entry for a value
# that has since gained an assignment site also fails). Regen: --write.
run_check "counter_honesty"          "py -3 scripts/check-counter-honesty.py --quiet"
run_check "counter_honesty_selftest" "py -3 scripts/check-counter-honesty.py --self-test 2>/dev/null"

# ---------------------------------------------------------------------------
# DISK-GUARD-1 (2026-07-29 ENOSPC incident)
# ---------------------------------------------------------------------------
# The A: drive hit 0 bytes free. Because no script checked free space, a write
# hit ENOSPC mid-stream and TRUNCATED THREE FILES TO ZERO BYTES; they survived
# only because sibling worktrees held copies. scripts/check-disk-space.py is
# the preflight that makes that failure loud and early instead of silent.
#
# ONLY the selftest is registered. A live free-space probe is
# environment-dependent and would make this aggregator flaky -- and the
# selftest is what actually proves the guard works, since a guard only ever
# seen passing on good input is not evidence of anything. The live checks live
# at the call sites (build_release.sh, deploy_payload.py, run_visual_capture.py,
# renderdoc_capture.py). Read-only, no build, sub-second.
run_check "disk_guard_selftest" "py -3 scripts/check-disk-space.py --selftest"

# ---------------------------------------------------------------------------
# LINK-GUARD-1 (2026-07-30 shared-install incident)
# ---------------------------------------------------------------------------
# Four deploy dirs under A:/Games/mc2-opengl shared ~1530 hardlinked files each
# -- mc2.exe, the whole shader tree, every DLL, and .mc2_deploy_lane.json (the
# lane lock itself). deploy_payload.py copies with shutil.copyfile, which opens
# the destination "wb" and so TRUNCATES THE SHARED INODE instead of replacing
# the directory entry: deploying to one dir rewrote the other three in place.
# Every existing guard was path-keyed and reported each dir free.
#
# Same registration shape as DISK-GUARD-1 above, for the same reason: ONLY the
# selftests run here. A live scan of A:/Games/mc2-opengl is machine-specific
# and would make this aggregator flaky on any other checkout, while the
# selftests are what actually prove the guard fires -- they build real
# hardlinks in a temp dir, assert the shared inode is detected and the peer
# named, assert read-only data sharing is NOT flagged (it is the cheap-install
# mechanism, not the bug), and assert a bad path fails closed. The live checks
# live at the call sites (deploy_payload.py, deploy_status(), register_lane()).
# Read-only outside a temp dir, no build, sub-second.
run_check "link_guard_selftest"  "py -3 scripts/check-hardlink-isolation.py --selftest"
run_check "iso_deploy_selftest"  "py -3 scripts/make-isolated-deploy.py --selftest"

# ---------------------------------------------------------------------------
# BEAUTY-SIDECAR-INSTALL-GATE-1 (2026-07-30 uninstalled-cook audit)
# ---------------------------------------------------------------------------
# Terrain-beautify sidecars (data/missions/<stem>.beauty/*) are cooked offline
# into tests/terrain/beautify/ and then installed into a deploy tree by a MANUAL
# copy. deploy_payload.py deliberately never walks .beauty/, so a cooked-but-
# never-installed sidecar was invisible to every existing gate -- and the engine
# loaders fail SILENTLY (one diagnostic line, then carry on). MC2_WATER_SHORE_MASK
# is default-ON and degrades to a complete no-op without shoreline_mask.png.
# check-deploy-hygiene.py is the mirror image (cook byproducts that LEAKED INTO a
# tree); neither subsumes the other.
#
# Same registration shape as DISK-GUARD-1 / LINK-GUARD-1 above and for the same
# reason: the LANE audit reads A:/Games/mc2-opengl/*, which is machine-specific
# and would make this aggregator flaky on any other checkout. Registered here are
# the two machine-independent halves -- --no-lanes (proves the engine-source
# extractor still finds the loader literals and the committed inventory is
# current; fails closed on zero extractions) and --selftest (proves the gate
# actually FIRES: it builds a synthetic repo + deploy tree, asserts an
# uninstalled cook is reported MISSING, asserts content drift is distinguished
# from absence, and asserts a sidecar name appearing ONLY inside a comment is
# NOT counted as a loader). The live lane audit is run deliberately:
#   py -3 scripts/check-beauty-sidecars.py [--all-lanes] [--install]
# Read-only, no build, sub-second.
run_check "beauty_sidecars"          "py -3 scripts/check-beauty-sidecars.py --no-lanes --quiet"
run_check "beauty_sidecars_selftest" "py -3 scripts/check-beauty-sidecars.py --selftest"

# ---------------------------------------------------------------------------
# STOCK-DATA-TRUTH-1 (2026-07-30 wrong-mission-data audit)
# ---------------------------------------------------------------------------
# Twelve offline tools hard-coded A:/Games/Carver5-feasibility/data/missions as
# "stock mission data". Measured against the shipped mission.fst it differs on
# ALL 24 campaign maps -- .pak AND .fit -- so anything run with default flags
# graded terrain the engine never loads, silently: the files exist, they parse,
# the numbers look plausible. One lane came within a meta-review of publishing
# a shoreline go/no-go computed on it.
#
# Same registration shape as beauty_sidecars above: the DATA arm reads
# A:/Games/mc2-opengl/* and A:/Games/mc2-opengl/mc2srcdata-fresh, which are
# machine-specific, so only the two machine-independent halves are registered.
# --no-data is a pure `ast` scan of tools/**.py + scripts/**.py plus manifest
# shape; --selftest proves the scanner FIRES on an argparse default, a module
# constant, and a DIFFERENT wrong tree, and proves it is NOT satisfied by the
# path appearing only in a comment or only in a docstring. The live data arm
# (candidate trees + every deploy lane's mission.fst vs the committed digest)
# is run deliberately:
#   py -3 scripts/check-stock-mission-data.py
# Read-only, no build, sub-second.
run_check "stock_mission_data"          "py -3 scripts/check-stock-mission-data.py --no-data --quiet"
run_check "stock_mission_data_selftest" "py -3 scripts/check-stock-mission-data.py --selftest"

# CAPTURE-PROVENANCE-TRUTH-1: a capture manifest must name the BINARY it
# measured, not the worktree the tool happened to run from. `identity.git.commit`
# was `git rev-parse HEAD` in the emitting worktree and sat beside "dirty": false,
# so it read as provenance while describing an unrelated lane. Measured over 404
# manifests: of the 31 whose exe still exists byte-identical, 28 name a commit
# their binary was NOT built from -- scratch/mc203reg + scratch/mc203air record
# seven different commits across 23 arms that all ran ONE binary (79360fdb,
# branch claude/spotlight-direction-1). The live check runs over the evidence
# trees (tests/visual, run) and reconciles each manifest against the exe's own
# embedded fingerprint, read from the binary's bytes -- never from the generated
# header, which a failed compile leaves freshly stamped beside a stale exe
# (c5d934d5). --selftest proves it FIRES: it builds synthetic binaries carrying
# real fingerprint stamps and asserts misattribution, a contradicted fingerprint,
# a missing exe sha256 and a zero-byte manifest are all caught, that an honest
# manifest passes, and that a manifest carrying every keyword ONLY inside a
# comment still fails. Offline, read-only, no game launch.
#
# GOLDEN-PROVENANCE-TRUTH-1 extends the same ratchet to `golden.json` --
# tests/visual/goldens was in NEITHER the filename list NOR the scan roots, so
# the artifact class that GRADES RELEASES was 100% unscanned while the checker
# reported confidently on 141 lesser manifests (same shape as a shader checker
# that silently skips every GLSL file). Scan is now 144; the three extra are the
# three committed goldens, and all three FAIL: none can name the binary that
# minted it. `--write` deliberately REFUSES to back-stamp a golden -- that is a
# tracked-file diff and a USER decision, and back-stamping would record whatever
# binary sits at that path today, which for baseline-5ef261f4 is a different
# lane's build.
#
# run_check_tri (not run_check): exit 3 == "failures, all pinned in
# scripts/provenance-known-failures.json, all irreparable, none expired" and
# prints as KNOWN, not PASS. See the runner's comment above for why that is not
# a suppression mechanism.
#
# THIS ROW IS RED, NOT AMBER, AND THAT IS THE CORRECT ANSWER (TRISTATE-WIRE-1,
# 2026-07-31 -- measured, not inherited). Being wired to the tri-state runner is
# an ELIGIBILITY, not a colour. Today the check finds 5 failures and exits 1:
#   * 2 x EMPTY-MANIFEST  -- zero-byte, ledgered, irreparable  -> would be KNOWN
#   * 3 x GOLDEN-UNATTRIBUTABLE on the three committed goldens -> UNLEDGERED
# The three goldens CANNOT be acknowledged away, and this was proven by trying:
# appending one is rejected with "acknowledges code GOLDEN-UNATTRIBUTABLE, which
# HAS a remedy ... Fix it instead" (check-capture-provenance.py:664-669,
# IRREPARABLE_CODES at :154). The remedy is real but is a USER decision and
# needs a capture run: retire v0.5.1-baseline and v0.5.2-baseline-PROPOSED (the
# case is prepared in .claude/GOLDEN-v051-v052-RETIREMENT-CASE.md) and re-bless
# baseline-5ef261f4, whose identity block is hand-added -- it carries no
# `embedded_fingerprint` and no `exe_sha256_at_scan`, so it is a claim rather
# than a reading (provenance_gate.py:392, :404), and the binary it names was
# overwritten by another lane's build. Red here means "act now", and the act is
# the user's. Do NOT step around this row as "known-red": it is a live item.
run_check_tri "capture_provenance"      "py -3 scripts/check-capture-provenance.py --quiet"
run_check "capture_provenance_selftest" "py -3 scripts/check-capture-provenance.py --selftest"
# The provenance GATE the minting tools call (bless.py / verify.py /
# run_golden_parity.py). Proves each verdict on binaries carrying real
# fingerprint stamps, that a hand-added claim and a keywords-in-a-comment
# manifest both stay UNESTABLISHED, and -- the other half -- that an honest
# artifact still passes. Offline, read-only, no game launch.
run_check "provenance_gate_selftest"    "py -3 scripts/provenance_gate.py --selftest"
# PROVENANCE-FORGERY-CLOSE-1: `bless_refusals_selftest` USED TO SIT HERE running
# a command byte-identical to the `bless_selftest` row above -- one selftest,
# counted twice, inflating the board's green by a row that measured nothing new.
# Removed here; `run_check` now refuses a duplicate label OR command outright,
# and check-contract-coverage.py arm (d) fails the suite if one is committed.

# ---------------------------------------------------------------------------
# The review pack's annotation anchors must never re-attach a stored comment to
# a DIFFERENT artifact after the page is regenerated -- that would put the
# user's words against something he never saw. Mutates the real manifest the
# way a future session will (insert/delete sections, figure blocks, images,
# gate/lane/meta rows, plus a pure reorder) and asserts every id shared across
# the mutation still points at the same artifact. --self-test proves it FIRES:
# it re-runs the whole battery under a deliberately positional (DOM-order) id
# scheme and requires that mis-attachments are detected. Offline, read-only.
run_check "review_anchor_stability"          "py -3 scripts/check-review-anchor-stability.py --quiet"
run_check "review_anchor_stability_selftest" "py -3 scripts/check-review-anchor-stability.py --self-test"

# SIM-IDENTITY-HASH-1: the refuse-to-grade gate splits mover fields VISUAL vs
# NON-VISUAL in two independent places (C++ digestMoverVisual, Python
# VISUAL_FIELDS). If they drift, the gate misgrades in the DANGEROUS direction:
# a real visual divergence gets classified LATENT and GRADED -- a false pass
# produced by the instrument built to stop false passes. Both halves' own
# self-tests stay green through that drift, because each is internally
# consistent; only this cross-check catches it. Live-fired by moving `status`
# from VISUAL to NON-VISUAL in the real module: the check went FAIL/exit 1, and
# an arm-A-alive / arm-B-DEAD mover pair graded LATENT + exit 0 until reverted.
run_check "sim_identity_fieldsets" "py -3 scripts/check-sim-identity-fieldsets.py"
# The gate's own verdict logic: GRADED / LATENT / UNVERIFIED / UNGRADED, plus
# localisation (names the first diverging mover and field) and the sub-quantum
# boundary-straddle label. Offline, no GPU, no capture.
run_check "sim_identity_gate_selftest" "py -3 scripts/sim_identity.py self-test"
# PIPE-LIVENESS: the hash crosses engine -> sidecar -> capture manifest ->
# combined manifest -> gate. The first version of this slice dropped the
# combined-manifest link: every other check stayed GREEN while the gate was
# dead and graded everything as UNVERIFIED. Re-injecting that bug leaves
# sim_identity_fieldsets and sim_identity_gate_selftest at exit 0; only this
# check fires. A gate is wired when the payload traverses the pipe, not when
# both ends compile.
run_check "sim_identity_propagation" "py -3 scripts/check-sim-identity-propagation.py"
# VERIFY-THE-UNVERIFIED-1: the propagation check above shipped WITHOUT a
# selftest, and it was blind. Its link-5 needles (`_sim_identity_gate(` and
# `_SIM_EXIT_UNGRADED`) were satisfied by the DEFINITION and the module-level
# constant, so replacing golden_compare.py's gate block with `if False:` --
# deleting the only call, making exit 7 unreachable, grading every comparison --
# left this row, sim_identity_fieldsets and sim_identity_gate_selftest all at
# exit 0. Measured by injection into the live tree, not theorised. Link 5 is now
# AST-verified and comments are stripped before any substring match; this row
# proves that machinery still discriminates.
run_check "sim_identity_propagation_selftest" "py -3 scripts/check-sim-identity-propagation.py --selftest"

# ---------------------------------------------------------------------------
# UNWIRED-CHECK-CLOSE-1 (2026-07-30) -- the checks nobody was running
# ---------------------------------------------------------------------------
# A census found 21 of 117 check scripts with ZERO executable callers. Running
# them for the first time turned up SIX live failures, some weeks or months
# old: a memory ratchet red on 3 files, a projection gate red for ~2 months
# while two reviewed arcs quietly reversed its invariant, a pointer gate red
# for 14 days, and a frame-graph gate that exited 127 on literally every
# invocation because bare "bash" resolved to WSL. None of it was noticed,
# because none of it ran.
#
# Everything cheap, deterministic and source-only is wired here. Everything
# that needs a build, a cook, a deploy tree or a live engine is in
# scripts/manual-checks.allowlist WITH A REASON, and check-contract-coverage.py
# fails if any check script is in neither list -- so this class cannot recur.
run_check "contract_coverage"          "py -3 scripts/check-contract-coverage.py"
run_check "contract_coverage_selftest" "py -3 scripts/check-contract-coverage.py --selftest"
run_check "agents_md"                  "sh scripts/check-agents-md.sh"
run_check "asset_manifest_fixtures"    "py -3 scripts/check-asset-manifest-fixtures.py"
run_check "asset_pipeline_doc"         "sh scripts/check-asset-pipeline-doc.sh"
run_check "claude_md_pointer"          "sh scripts/check-claude-md-pointer.sh"
run_check "json_isolation"             "sh scripts/check-json-isolation.sh"
run_check "memory_size"                "py -3 scripts/check-memory-size.py"
run_check "mlr_leaves_gated"           "sh scripts/check-mlr-leaves-gated.sh"
run_check "object_mvp_currency"        "py -3 scripts/check-object-mvp-currency.py"
run_check "object_mvp_currency_selftest" "py -3 scripts/check-object-mvp-currency.py --selftest"
run_check "particles_no_cpu_projection" "sh scripts/check-particles-no-cpu-projection.sh"
run_check "toolchain_bom"              "py -3 scripts/check-toolchain-bom.py"
run_check "unified_projection_retirement" "sh scripts/check-unified-projection-retirement.sh"
# --allow-missing-artifacts is DELIBERATE and must stay explicit: this check
# grades the tag registry against BOTH a gate doc and a golden artifact log
# dir. The log dir is a smoke output that is usually absent in a source
# checkout, and until today the script silently skipped that half and still
# printed "OK: all 8 required tags present" -- a full-credit PASS from a
# half-run check. It now FAILS on a missing dir unless the reduced scope is
# declared right here, at the call site, where a reader can see it.
run_check "oracle_registry"            "py -3 scripts/check-oracle-registry.py --allow-missing-artifacts"

# ---------------------------------------------------------------------------
# UNWIRED-CHECKS-CLOSE-1 (2026-07-30) -- the checks the CENSUS was blind to
# ---------------------------------------------------------------------------
# The block above closed 21 dark checks and installed check-contract-coverage.py
# so the class could not recur. It recurred immediately. Coverage's arm (b),
# "driven by another runner", was a bare substring test -- `if name in text` --
# so a check counted as watched when its name appeared in a COMMENT, a docstring
# or an English sentence. Injection-proven: one Python comment naming a
# brand-new dark check moved it straight into "driven by another runner".
#
# Nine more checks were sitting behind that credit. check-new-gates.py was held
# up by the words "Modeled on scripts/check-new-gates.py Mode A" in an unrelated
# checker's docstring, and was exiting 1. Arm (b) is now invocation-shaped, and
# these are the ones that turned out to be cheap, deterministic and source-only.
run_check "asset_manifests"            "py -3 scripts/check-asset-manifests.py --quiet"
run_check "asset_scale_callers"        "sh scripts/check-asset-scale-callers.sh"
run_check "smoke_matrices"             "py -3 scripts/check-smoke-matrices.py --quiet"
# --strict is REQUIRED, not decoration: bare mode returns 0 unconditionally, so
# wiring it without --strict would register a row that cannot ever go red. Its
# two --strict failures were a classify() bug, not stale fixtures -- it keyed on
# the basename alone, so an asset manifest and a brain-fixture corpus manifest
# were both graded as run/release manifests and told to grow an identity block.
run_check "manifest_schema"            "py -3 scripts/check-manifest-schema.py --strict"
# --base HEAD~1 is the whole point. The default base is `merge-base HEAD
# origin/main`, which on this long-lived branch is 25 days back, so a bare run
# grades the ENTIRE branch and reports 99 gates -- true, useless, and unfixable
# in one commit. Mode A diffs the WORKING TREE against the base, so HEAD~1 asks
# the question this aggregator can act on: does the change in front of me add an
# MC2_* gate that is in neither RendererFeatureRegistry.h nor tier1_env_vars.md?
# The 99-gate backlog is real and stays visible: `py -3 scripts/check-new-gates.py --all`.
run_check "new_gates"                  "py -3 scripts/check-new-gates.py --base HEAD~1"

# ---------------------------------------------------------------------------
# Summary table
# ---------------------------------------------------------------------------
echo "====================================================================="
echo "  CONTRACT CHECK RESULTS"
echo "====================================================================="
TOTAL_MS=0
OVER_BUDGET=()
for i in "${!NAMES[@]}"; do
    result="${RESULTS[$i]}"
    label="${NAMES[$i]}"
    ms="${SECS[$i]:-0}"
    TOTAL_MS=$((TOTAL_MS + ms))
    secs="$(printf '%d.%02d' "$((ms / 1000))" "$(((ms % 1000) / 10))")"
    if [ "$ms" -gt $((BUDGET_SEC * 1000)) ]; then
        OVER_BUDGET+=("$label|$secs")
        mark="  <-- over ${BUDGET_SEC}s budget"
    else
        mark=""
    fi
    if [ "$result" = "PASS" ]; then
        printf "  %-40s %8ss  PASS%s\n" "$label" "$secs" "$mark"
    elif [ "$result" = "KNOWN" ]; then
        printf "  %-40s %8ss  KNOWN (correct failure, ledger-pinned)%s\n" "$label" "$secs" "$mark"
    else
        printf "  %-40s %8ss  FAIL  <---%s\n" "$label" "$secs" "$mark"
    fi
done
echo "---------------------------------------------------------------------"
printf "  %d passed, %d failed, %d known-correct-failure\n" \
    "$PASS_COUNT" "$FAIL_COUNT" "$KNOWN_COUNT"
printf "  wall time %d.%02ds across %d rows\n" \
    "$((TOTAL_MS / 1000))" "$(((TOTAL_MS % 1000) / 10))" "${#NAMES[@]}"
if [ "${#OVER_BUDGET[@]}" -gt 0 ]; then
    printf "  %d row(s) over the %ss-per-check budget this file promises:\n" \
        "${#OVER_BUDGET[@]}" "$BUDGET_SEC"
    for e in "${OVER_BUDGET[@]}"; do
        printf "      %-38s %ss\n" "${e%%|*}" "${e##*|}"
    done
    echo "  (measured, not enforced -- see the PER-CHECK WALL TIME note above)"
fi
if [ "$KNOWN_COUNT" -gt 0 ]; then
    echo ""
    echo "  KNOWN is NOT a pass. It means every failure that check found is"
    echo "  pinned by exact identity in a ledger, is irreparable by any tool,"
    echo "  and has an unexpired human acknowledgement. The underlying defect is"
    echo "  still there and the remedy is a USER decision. It is separated from"
    echo "  FAIL so that a FAIL row still means: action required by you, now."
    echo "  A ledger entry that matches no finding is itself a FAIL, so this"
    echo "  cannot quietly outlive its condition -- nor hide a blind checker."
fi
echo "====================================================================="

if [ "$FAIL_COUNT" -gt 0 ]; then
    exit 1
fi
exit 0
