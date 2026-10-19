#!/usr/bin/env bash
# scripts/check-contracts.sh
# CONTRACTS-AGGREGATOR-1: run all cheap static-contract checks and report a
# concise PASS/FAIL table.  Exits 0 only if every check passes.
#
# Usage:
#   bash scripts/check-contracts.sh           # run from worktree root
#   bash scripts/check-contracts.sh --quiet   # PASSING rows print nothing; a
#                                             # FAILING row still says why
#   bash scripts/check-contracts.sh --selftest  # prove the FAIL-REASON
#                                             # rendering above still works,
#                                             # in seconds, without the board
#
# Env:
#   CONTRACTS_FAIL_TAIL=N   lines of a failing check's output to show (default 40)
#
# What counts as "cheap":
#   * Pure grep/awk over source files (no build required)
#   * Run time under ~5 seconds on a cold file cache
#   * Idempotent and read-only
#
# Excluded (too heavy for this aggregator):
#   (none since 2026-10-12; the live-engine JSON checks moved to the mc2-render-state MCP)
#
# RED-ROWS-CLOSE-2 (2026-07-31): check-shader-schema.sh USED to be excluded here,
# by this comment, on the stated grounds that it "requires glslangValidator +
# compiled goldens". BOTH CLAUSES WERE FALSE and it is now wired below as
# `shader_schema`. Measured: tools/shader_schema/validate.py contains no
# subprocess/glslang/spirv-cross call of any kind (it is pure stdlib), and the
# 82 shader_reflect goldens it reads are TRACKED, COMMITTED files, not build
# output. It runs in ~0.3s -- inside this file's own ~5s promise.
#
# The cost of that wrong sentence: the check was red and unwatched, on a REAL
# C++/GLSL desync (GpuMechInstance bytes 56/60 renamed in gos_mech_batcher.h with
# offsetof static_asserts, while shaders/mech.vert still called them _pad2/_pad3).
# Note the shape -- a prose comment was the ONLY thing excluding it, which is the
# mention-is-not-a-caller pattern check-contract-coverage.py exists to refuse.
# Coverage did not catch it, and is not at fault: CMakeLists.txt:830 defines a
# real add_custom_target(shader_schema) whose COMMAND genuinely invokes the
# script, so arm (b) credited it as "driven by another runner" -- correctly, by
# its own rules. But that target carries no ALL, so no build, script or gate ever
# asks for it. Coverage's arm (b) is ONE LEVEL: it asks "does a runner invoke
# this?", never "is that runner itself ever driven?". A check can therefore be
# accounted-for and still be transitively dark. See the coverage TODO in
# scripts/check-contract-coverage.py.
#
# check-vulkan-bindings.py IS wired below (vulkan_bindings + vulkan_bindings_selftest).
#   Per-pipeline classifier (BINDING-CHECKER-PER-PIPELINE-CLASSIFIER-1): it ERRORs
#   (exit nonzero) ONLY on a same-pipeline distinct-resource share; disjoint-pipeline
#   shares are OK (allowlisted) or WARN (exit 0). Pre-existing acknowledged cases
#   (SSBO 14/20/23 disjoint, mask-solid slot-2) are in the allowlist, so the default
#   run is exit 0. The self-test proves a NEW same-pipeline dup still fails.
#   `--strict` additionally fails on any un-allowlisted WARN (future tightening).

set -euo pipefail
# Rows run test suites that build the harness cursor guard; none may touch a person's pointer.
export MC2_CURSOR_GUARD_OFF=1

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKTREE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
QUIET=0
# PARALLEL-CONTRACTS-1 (2026-10-09): rows run in a pool by default. --serial or
# CONTRACTS_JOBS=1 restores the old one-at-a-time behaviour; -jN / --jobs=N sets
# the pool size. Row names, outputs, table order, counts and the exit code are
# identical to a serial run; only the wall clock changes.
JOBS="${CONTRACTS_JOBS:-}"
SELFTEST=0
SELFTEST_RENDER=0
# Row-level result cache (scripts/contracts_cache.py + scripts/contracts-cache-inputs.txt): a row that PASSED against
# byte-identical declared inputs is reported "PASS (cached)" and not run. --no-cache (or CONTRACTS_NO_CACHE=1) runs
# everything. Only PASS is ever cached; undeclared rows always run. Parallel mode only (-j2 and up, the default).
NO_CACHE="${CONTRACTS_NO_CACHE:-0}"
# --rows REGEX runs only the rows whose label matches (ERE, unanchored): the inner-loop filter for iterating on one check. It is a
# PARTIAL board and says so; the full board stays mandatory per batch.
ROWS_RE=""
ROWS_SKIPPED=0
_ARGS=()
while [ $# -gt 0 ]; do
    if [ "$1" = "--rows" ]; then
        [ $# -ge 2 ] || { echo "--rows needs a regex" >&2; exit 1; }
        ROWS_RE="$2"; shift 2; continue
    fi
    _ARGS+=("$1"); shift
done
set -- ${_ARGS[@]+"${_ARGS[@]}"}
# Named presets for --rows (the regex form still works for anything else).
case "$ROWS_RE" in
    split) ROWS_RE="cxx17_ratchet|dup_lines|gate_gen|file_size|split_path|split_map|background_launch|scripts_tests" ;;
    tidy)  ROWS_RE="cxx17_ratchet|file_size|background_launch|tools_tests" ;;
esac
for arg in "$@"; do
    case "$arg" in
        --quiet|-q) QUIET=1 ;;
        --selftest) SELFTEST=1 ;;
        --serial) JOBS=1 ;;
        --no-cache) NO_CACHE=1 ;;
        -j[0-9]*) JOBS="${arg#-j}" ;;
        --jobs=*) JOBS="${arg#--jobs=}" ;;
        --rows=*) ROWS_RE="${arg#--rows=}"
                  case "$ROWS_RE" in
                      split) ROWS_RE="cxx17_ratchet|dup_lines|gate_gen|file_size|split_path|split_map|background_launch|scripts_tests" ;;
                      tidy)  ROWS_RE="cxx17_ratchet|file_size|background_launch|tools_tests" ;;
                  esac ;;
        # Internal. Registers synthetic rows instead of the real board so the
        # selftest can grade what the REAL runners render. Not for humans.
        --selftest-render) SELFTEST_RENDER=1 ;;
        --help|-h)
            echo "Usage: bash $0 [--quiet] [--selftest] [--serial | -jN | --jobs=N] [--rows REGEX] [--no-cache]"
            echo "  --no-cache    run every row (default: rows that PASSED on identical declared inputs are 'PASS (cached)')"
            echo "  --rows REGEX  run only rows whose label matches (ERE, unanchored), e.g. --rows 'ratchet|dup_lines'."
            echo "                A PARTIAL board: it is for iterating mid-batch, never a substitute for the full board."
            echo "  --rows split  the file-split rows (cxx17_ratchet dup_lines gate_gen file_size split_path split_map background_launch scripts_tests)"
            echo "  --rows tidy   the tidy-batch rows (cxx17_ratchet file_size background_launch tools_tests)"
            echo "  Rows run in parallel by default (CONTRACTS_JOBS / -jN size the pool,"
            echo "  --serial = old behaviour). Rows that build, launch or write shared"
            echo "  files are detected (_is_serial_row) and run alone after the pool."
            echo "  Runs cheap source-level contract checks and prints a PASS/FAIL table."
            echo "  Exits 0 if all pass, nonzero if any fail."
            echo "  --quiet silences PASSING rows only; a FAILING row always states why."
            echo "  CONTRACTS_FAIL_TAIL=N caps a failing row's output (default 40 lines)."
            echo "  --selftest proves that FAIL-REASON rendering still holds (seconds,"
            echo "             no board run); it is also wired as its own row."
            exit 0
            ;;
        *) echo "Unknown argument: $arg" >&2; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Check runner
# ---------------------------------------------------------------------------
_WALL_T0="$(date +%s%N 2>/dev/null | cut -c1-13)"
PASS_COUNT=0
FAIL_COUNT=0
KNOWN_COUNT=0

# Arrays to collect results for the summary table.
# STRICTLY PARALLEL, all four of them. Anything that appends to NAMES must
# append to the other three in the same breath or every row after it renders
# another row's number (see _register_row, which used to append to NAMES and
# RESULTS only and shifted SECS for the whole rest of the board).
NAMES=()
RESULTS=()
SECS=()
REASONS=()

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

_row_selected() {   # 0 = run this row (no --rows filter, or the label matches)
    [ -z "$ROWS_RE" ] && return 0
    if [[ "$1" =~ $ROWS_RE ]]; then return 0; fi
    ROWS_SKIPPED=$((ROWS_SKIPPED + 1))
    return 1
}

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
        SECS+=(0)
        REASONS+=("$why -- this is a bug in check-contracts.sh, not in the check")
        return 1
    fi
    SEEN_LABELS="${SEEN_LABELS}<${label}>"
    SEEN_CMDS="${SEEN_CMDS}<${cmd}>"
    return 0
}

# ---------------------------------------------------------------------------
# FAIL REASONS (FAIL-REASON-1, 2026-07-31)
# ---------------------------------------------------------------------------
# TRISTATE-WIRE-1 gave the ONE tri-state row the ability to say why it is red.
# The other 142 rows run through run_check, which sent its command's output to
# /dev/null under --quiet and so could not say anything at all: a failing row
# could only ever render as a bare `FAIL <---`.
#
# That discard was NOT careless. `--quiet` is documented at the top of this file
# as "suppress individual script output", and it was written when this board had
# EIGHT rows (241f22dff, 2026-05-28). At eight rows the honest answer to "why?"
# was "re-run it without --quiet", which cost seconds. At 143 rows and ~340s it
# costs six minutes, so nobody does it -- and a red row with no reason becomes
# wallpaper. capture_provenance was walked past by lane after lane as
# "known-red, not mine" precisely because the board never said why; when
# somebody finally read it, three of its five findings needed a USER decision
# and two were already pinned. That was one redirect away the entire time.
#
# So the volume argument is honoured exactly where it is true and nowhere else:
#   * a PASSING row still prints NOTHING under --quiet. A green board is
#     byte-for-byte as quiet as before. Added output is proportional to the
#     number of RED rows and to nothing else.
#   * a FAILING row gets a one-line reason ON its row in the table, plus a
#     bounded block printed AFTER the table -- beside the summary the reader is
#     already looking at, not 130 rows up in the scrollback.
#   * a chatty failure is windowed (head + tail) and the elision names the full
#     log plus the exact one-line command to re-run just that check, so nobody
#     has to spend 340s to see the rest.
#
# The failure mode of a fix like this is burying the one red row under pages of
# noise from a chatty one -- the same wallpaper problem wearing a new hat. It is
# prevented structurally, not by taste: green rows emit nothing, every block is
# capped at CONTRACTS_FAIL_TAIL lines, and all of it sits BELOW the table.
FAIL_TAIL_LINES="${CONTRACTS_FAIL_TAIL:-40}"
FAIL_HEAD_LINES=8
FAIL_LABELS=()
FAIL_BLOCKS=()
FAILLOG_DIR=""

# One line, chosen to be the most explanatory thing the check said. This is a
# heuristic and says so: the bounded block below the table always carries the
# rest, so a bad guess costs a glance, never the diagnosis.
_reason_digest() {          # $1 = captured output, $2 = rc  -> one line on stdout
    local out="$1" rc="$2" line=""
    if printf '%s\n' "$out" | grep -q '^Traceback'; then
        # A CRASHED checker. "Traceback (most recent call last):" is not the
        # reason; the exception on the last line is.
        line="$(printf '%s\n' "$out" | grep -vE '^[[:space:]]*$' | tail -n 1)" || true
    else
        line="$(printf '%s\n' "$out" \
            | grep -m1 -E '(^|[^[:alnum:]_])(FAIL|ERROR|VIOLATION|MISMATCH|Traceback)')" || true
        if [ -z "$line" ]; then
            line="$(printf '%s\n' "$out" | grep -vE '^[[:space:]]*$' | tail -n 1)" || true
        fi
    fi
    line="$(printf '%s' "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
    if [ -z "$line" ]; then
        # Not a cosmetic case. A check that fails and says NOTHING is a defect in
        # that check, and this line is the only place anyone would ever see it.
        line="(exit $rc and printed nothing -- this check fails without saying why)"
    fi
    if [ "${#line}" -gt 100 ]; then line="${line:0:97}..."; fi
    printf '%s' "$line"
}

_record_failure() {         # $1 label, $2 cmd, $3 captured output, $4 rc
    local label="$1" cmd="$2" out="$3" rc="$4"
    local n block rerun logpath=""
    rerun="re-run just this row:  (cd \"$WORKTREE_ROOT\" && $cmd)"
    n="$(printf '%s\n' "$out" | grep -c '')" || true
    [ -n "$n" ] || n=0
    if [ -z "$out" ]; then
        block="(the check exited $rc and printed nothing at all -- there is no
reason to read here because it never gave one. That is a defect in the
check, not a mystery about the tree.)
$rerun"
    elif [ "$n" -le "$FAIL_TAIL_LINES" ]; then
        block="$out
$rerun"
    else
        # Truncation gets a pointer, or it is just a different way of hiding the
        # answer. The log is written outside the worktree so it cannot pollute
        # the tree the other checks scan.
        if [ -z "$FAILLOG_DIR" ]; then
            FAILLOG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/contracts-fail-XXXXXX")"
        fi
        logpath="$FAILLOG_DIR/${label}.log"
        printf '%s\n' "$out" > "$logpath"
        block="$(printf '%s\n' "$out" | head -n "$FAIL_HEAD_LINES")
... $((n - FAIL_TAIL_LINES)) of $n lines elided (raise with CONTRACTS_FAIL_TAIL=N) ...
... full output: $logpath
$(printf '%s\n' "$out" | tail -n "$((FAIL_TAIL_LINES - FAIL_HEAD_LINES))")
$rerun"
    fi
    FAIL_LABELS+=("$label")
    FAIL_BLOCKS+=("$block")
}

# ---------------------------------------------------------------------------
# PARALLEL EXECUTION (PARALLEL-CONTRACTS-1, 2026-10-09)
# ---------------------------------------------------------------------------
# In parallel mode run_check / run_check_tri REGISTER a row (duplicate guard
# unchanged) and only queue it. _drain_queue executes the queue in a pool,
# capturing each row's output/rc/ms to files, then replays the results through
# the same finishing code a serial run uses (_finish_check / _finish_check_tri)
# IN REGISTRATION ORDER, so the table, per-row blocks, counts and exit code match.
#
# SERIAL ROWS: a row whose command builds, launches the game, mutates git, or
# rewrites tracked/shared files must not overlap anything else. They are detected
# by command text (_is_serial_row) and run one at a time AFTER the pool drains.
# On the current board none match; the detector guards the day somebody adds one.
PARALLEL=0
Q_LABEL=(); Q_CMD=(); Q_KIND=()
_is_serial_row() {   # $1 = command
    case "$1" in
        *cmake*|*msbuild*|*MSBuild*|*ninja*|*mc2.exe*|*run_smoke*|*run_visual_capture*|*run-smoke*) return 0 ;;
        *"git add"*|*"git commit"*|*"git checkout"*|*"git stash"*|*"git reset"*|*"git worktree"*) return 0 ;;
        *" --write"*|*" --regen"*|*" --update"*|*" --bless"*|*" --fix"*|*" --apply"*) return 0 ;;
    esac
    return 1
}

_finish_check() {   # $1 label $2 cmd $3 out $4 rc $5 ms $6 header-already-printed(0/1)
    local label="$1" cmd="$2" out="$3" rc="$4"
    SECS+=("$5")
    NAMES+=("$label")
    if [ "$rc" -eq 0 ]; then
        PASS_COUNT=$((PASS_COUNT + 1))
        RESULTS+=("PASS")
        REASONS+=("")
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        RESULTS+=("FAIL")
        REASONS+=("$(_reason_digest "$out" "$rc")")
        _record_failure "$label" "$cmd" "$out" "$rc"
    fi

    if [ "$QUIET" -ne 1 ]; then
        if [ "${6:-0}" -ne 1 ]; then echo "--- $label ---"; fi
        if [ -n "$out" ]; then printf '%s\n' "$out"; fi
        echo ""
    fi
}

run_check() {
    local label="$1"
    local cmd="$2"

    _row_selected "$label" || return 0
    _register_row "$label" "$cmd" || return 0

    if [ "$PARALLEL" -eq 1 ]; then
        Q_LABEL+=("$label"); Q_CMD+=("$cmd"); Q_KIND+=("check")
        return 0
    fi

    local t0 t1 out rc
    # Serial mode: header BEFORE the run so a reader sees which check is in
    # flight while a slow row grinds.
    if [ "$QUIET" -ne 1 ]; then echo "--- $label ---"; fi
    t0="$(_now_ms)"
    out="$( (cd "$WORKTREE_ROOT" && eval "$cmd") 2>&1 )" && rc=0 || rc=$?
    t1="$(_now_ms)"
    _finish_check "$label" "$cmd" "$out" "$rc" "$((t1 - t0))" 1
    return 0
}

_PAR_DIR=""
_par_worker() {   # $1 idx
    local i="$1" t0 t1 out rc
    t0="$(_now_ms)"
    out="$( (cd "$WORKTREE_ROOT" && eval "${Q_CMD[$i]}") 2>&1 )" && rc=0 || rc=$?
    t1="$(_now_ms)"
    printf '%s' "$out" > "$_PAR_DIR/$i.out"
    printf '%s' "$((t1 - t0))" > "$_PAR_DIR/$i.ms"
    printf '%s' "$rc" > "$_PAR_DIR/$i.rc"     # written last: presence == done
}

# Pool size: CONTRACTS_JOBS / -jN, else min(cores, 16).
_default_jobs() {
    local n="${NUMBER_OF_PROCESSORS:-}"
    [ -n "$n" ] || n="$(nproc 2>/dev/null || echo 4)"
    [ "$n" -gt 16 ] && n=16
    [ "$n" -lt 2 ] && n=2
    printf '%s' "$n"
}

# Rows measured slow go first so the pool does not end on a long tail.
_SLOW_FIRST=" no_raw_gl_from_game include_firewall delivery_installs input_capture_visibility_selftest sampler_bindings scripts_tests tools_tests "

_drain_queue() {
    local n="${#Q_LABEL[@]}" i
    [ "$n" -gt 0 ] || return 0
    _PAR_DIR="$(mktemp -d "${TMPDIR:-/tmp}/contracts-par-XXXXXX")"
    local -a order_ser=() first=() rest=()
    for ((i = 0; i < n; i++)); do
        if _is_serial_row "${Q_CMD[$i]}"; then
            order_ser+=("$i")
        elif [[ "$_SLOW_FIRST" == *" ${Q_LABEL[$i]} "* ]]; then
            first+=("$i")
        else
            rest+=("$i")
        fi
    done
    # Result cache: one helper call keys every declared queued row; a HIT is filled in as an instant PASS.
    local -A ckey=() chit=()
    if [ "$NO_CACHE" != "1" ]; then
        local tsv="" lbl key st
        for ((i = 0; i < n; i++)); do
            case "${Q_CMD[$i]}" in *$'\t'*|*$'\n'*) continue ;; esac
            tsv+="${Q_LABEL[$i]}"$'\t'"${Q_CMD[$i]}"$'\n'
        done
        local res
        res="$(printf '%s' "$tsv" | py -3 -B "$SCRIPT_DIR/contracts_cache.py" keys "$WORKTREE_ROOT" 2>/dev/null)" || res=""
        while IFS=$'\t' read -r lbl key st; do
            [ -n "$lbl" ] || continue
            st="${st%$'\r'}"    # python on Windows ends lines with CRLF
            ckey["$lbl"]="$key"
            [ "$st" = "HIT" ] && chit["$lbl"]=1
        done <<< "$res"
    fi
    local -a first2=() rest2=() ser2=()
    for i in "${first[@]}"; do if [ "${chit[${Q_LABEL[$i]}]:-0}" = 1 ]; then _cache_fill "$i"; else first2+=("$i"); fi; done
    for i in "${rest[@]}";  do if [ "${chit[${Q_LABEL[$i]}]:-0}" = 1 ]; then _cache_fill "$i"; else rest2+=("$i");  fi; done
    for i in "${order_ser[@]}"; do if [ "${chit[${Q_LABEL[$i]}]:-0}" = 1 ]; then _cache_fill "$i"; else ser2+=("$i"); fi; done
    for i in "${first2[@]}" "${rest2[@]}"; do
        while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n 2>/dev/null || true; done
        _par_worker "$i" &
    done
    wait
    for i in "${ser2[@]}"; do _par_worker "$i"; done
    # Replay in registration order through the serial finishing code.
    local out rc ms
    for ((i = 0; i < n; i++)); do
        if [ ! -f "$_PAR_DIR/$i.rc" ]; then
            out="(row produced no result file -- worker died)"; rc=99; ms=0
        else
            out="$(cat "$_PAR_DIR/$i.out")"; rc="$(cat "$_PAR_DIR/$i.rc")"; ms="$(cat "$_PAR_DIR/$i.ms")"
        fi
        if [ "${Q_KIND[$i]}" = "tri" ]; then
            _finish_check_tri "${Q_LABEL[$i]}" "${Q_CMD[$i]}" "$out" "$rc" "$ms"
        else
            _finish_check "${Q_LABEL[$i]}" "${Q_CMD[$i]}" "$out" "$rc" "$ms"
        fi
    done
    # Record every uncached row that PASSED (rc 0) against the key computed before it ran. FAIL / KNOWN never stored.
    local store=""
    for ((i = 0; i < n; i++)); do
        [ -n "${ckey[${Q_LABEL[$i]}]:-}" ] || continue
        [ "${chit[${Q_LABEL[$i]}]:-0}" = "1" ] && continue
        [ "$(cat "$_PAR_DIR/$i.rc" 2>/dev/null)" = "0" ] || continue
        store+="${Q_LABEL[$i]}"$'\t'"${ckey[${Q_LABEL[$i]}]}"$'\n'
    done
    if [ -n "$store" ]; then printf '%s' "$store" | py -3 -B "$SCRIPT_DIR/contracts_cache.py" store "$WORKTREE_ROOT" 2>/dev/null || true; fi
    rm -rf "$_PAR_DIR"
}

CACHED_SET=" "
_cache_fill() {   # $1 idx: a cache HIT is a PASS with no run
    printf '%s' "(cached: PASS on identical inputs; --no-cache to force)" > "$_PAR_DIR/$1.out"
    printf '%s' 0 > "$_PAR_DIR/$1.ms"
    printf '%s' 0 > "$_PAR_DIR/$1.rc"
    CACHED_SET+="${Q_LABEL[$1]} "
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

    _row_selected "$label" || return 0
    _register_row "$label" "$cmd" || return 0

    if [ "$PARALLEL" -eq 1 ]; then
        Q_LABEL+=("$label"); Q_CMD+=("$cmd"); Q_KIND+=("tri")
        return 0
    fi

    local t0 t1
    t0="$(_now_ms)"
    out="$( (cd "$WORKTREE_ROOT" && eval "$cmd") 2>&1 )" && rc=0 || rc=$?
    t1="$(_now_ms)"
    _finish_check_tri "$label" "$cmd" "$out" "$rc" "$((t1 - t0))"
}

# Shared finishing code for the tri-state runner (serial and parallel replay).
_finish_check_tri() {   # $1 label $2 cmd $3 out $4 rc $5 ms
    local label="$1" cmd="$2" out="$3" rc="$4"
    SECS+=("$5")

    NAMES+=("$label")
    if [ "$rc" -eq 0 ]; then
        PASS_COUNT=$((PASS_COUNT + 1))
        RESULTS+=("PASS")
        REASONS+=("")
    elif [ "$rc" -eq 3 ]; then
        KNOWN_COUNT=$((KNOWN_COUNT + 1))
        RESULTS+=("KNOWN")
        REASONS+=("")
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        RESULTS+=("FAIL")
        REASONS+=("$(_reason_digest "$out" "$rc")")
        _record_failure "$label" "$cmd" "$out" "$rc"
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
            #
            # FAIL-REASON-1 (2026-07-31) kept that guarantee and MOVED where it
            # lands. run_check can now explain itself too, so there are 143 rows
            # that might need to; if this one printed HERE and those printed
            # after the table, the board would grow two different idioms for the
            # same thing, which is how this file gets bitten. Both runners now
            # hand the reason to _record_failure and it is rendered below the
            # summary -- the digest is on this row IN the table, so it is more
            # visible than it was here, not less. KNOWN keeps printing inline
            # above, untouched: it is the amber contract and it is two lines.
            :
        fi
    else
        echo "--- $label ---"
        printf '%s\n' "$out"
        echo ""
    fi
}

# ---------------------------------------------------------------------------
# THE FAIL-REASON RENDERING NOW HAS A GUARD (CONTRACTS-DOC-SELFTEST-1, 2026-07-31)
# ---------------------------------------------------------------------------
# Nearly every behaviour this file guards has a `*_selftest` row proving the
# guard still fires. The rendering directly above -- 25008e321, the `why>` digest
# on a failing row plus the bounded head+tail block below the table -- had none.
# So a later simplification of run_check or run_check_tri could delete either
# half and the board would stay green while going mute: exactly the regression
# class the surrounding comments exist to prevent, left open in the file that
# hosts them. A guard whose own guard is a paragraph is not guarded.
#
# The lane that authored FAIL-REASON-1 could not add this row: its proof standard
# demanded the board's tallies be IDENTICAL before and after, and a new row moves
# the headline number by construction. That was a constraint of that lane, not an
# argument against the row. The number moving is the POINT, and it is stated in
# the commit that adds it so it is attributable rather than looking like drift.
#
# WHAT IT PINS:
#   (a) a check that exits non-zero renders a NON-EMPTY `why>` on its table row,
#       and the digest is the explanatory line rather than the first line
#   (b) a failure longer than CONTRACTS_FAIL_TAIL is truncated, AND the elision
#       names both the elided line COUNT and a READABLE full-output log path.
#       Truncation without a pointer is a slower way of hiding the answer
#   (c) a check that fails and prints NOTHING still renders an explanation
#       instead of a blank line -- that defect belongs to the check, and this is
#       the only place anybody would ever see it
#   (d) a PASSING row still emits ZERO BYTES under --quiet. This is the property
#       that stops the fix becoming noise, and it is the one a well-meaning edit
#       breaks first
#   (d2) ...and, so (d) cannot be satisfied by muting run_check outright, the
#       same passing row's output MUST still appear when --quiet is absent. A
#       green board is byte-for-byte as quiet as before, and no quieter.
#
# IT IS NOT A REIMPLEMENTATION, and that is the whole design. It re-executes
# THIS script with --selftest-render, which puts synthetic rows through the REAL
# run_check and run_check_tri and prints the REAL summary table, then greps the
# rendered stdout. Only the CHECK COMMANDS are synthetic (`printf`, `exit 7`) --
# those are the inputs, not the logic. A copy of the rendering that agrees with
# itself proves nothing; that is how a selftest reports SUCCESS while the
# behaviour it is named for is already gone.
#
# The fixture rows below are deliberately written with a SINGLE-quoted command:
# check-contract-coverage.py's row census matches a double-quoted command, so
# this spelling keeps fixtures out of a count that means "checks on the board".
_selftest_rows() {
    case "${SELFTEST_SCENARIO:-fail}" in
    pass)
        run_check     "st_pass_talks" 'printf "%s\n" ST-STDOUT-SENTINEL'
        run_check_tri "st_tri_pass"   'printf "%s\n" ST-STDOUT-SENTINEL-TRI'
        ;;
    fail)
        run_check     "st_fail_talks"  'printf "%s\n" "note: a preamble line" "ERROR: ST-DIGEST-SENTINEL blew up" "trailing noise"; exit 1'
        run_check     "st_fail_silent" 'exit 7'
        run_check     "st_fail_chatty" 'i=1; while [ $i -le 40 ]; do printf "chatty-line-%02d\n" "$i"; i=$((i + 1)); done; exit 1'
        run_check_tri "st_tri_fail"    'printf "%s\n" "VIOLATION: ST-TRI-SENTINEL"; exit 1'
        run_check_tri "st_tri_known"   'printf "%s\n" "KNOWN: ST-KNOWN-SENTINEL pinned in the ledger"; exit 3'
        ;;
    esac
}

ST_FAILS=0
_st_ok()     { printf '[fail-reason-selftest]   ok  %s\n' "$1"; }
_st_bad()    { printf '[fail-reason-selftest] FAIL  %s\n' "$1" >&2; ST_FAILS=$((ST_FAILS + 1)); }
_st_want()   { case "$2" in *"$3"*) _st_ok "$1" ;; *) _st_bad "$1 -- expected to find: $3" ;; esac; }
_st_absent() { case "$2" in *"$3"*) _st_bad "$1 -- must NOT appear, but did: $3" ;; *) _st_ok "$1" ;; esac; }

# The `why>` text rendered on ONE row of the REAL summary table, or "" if that
# row rendered none. Reads the table, not any internal variable: a digest
# computed and then dropped on the floor must grade as absent.
_st_row_reason() {          # $1 label, $2 rendered board output
    printf '%s\n' "$2" | awk -v lbl="$1" '
        $1 == lbl && index($0, "FAIL  <---") {
            if ((getline) > 0 && $1 == "why>") {
                sub(/^[[:space:]]*why>[[:space:]]*/, ""); print $0
            }
            exit
        }'
}

_fail_reason_selftest() {
    local self="$SCRIPT_DIR/$(basename -- "$0")"
    local td out rc reason logpath n

    td="$(mktemp -d "${TMPDIR:-/tmp}/contracts-selftest-XXXXXX")"

    # --- the failing shapes, as the real runners render them -----------------
    # TMPDIR is redirected so the child's full-output log lands somewhere this
    # function can read AND delete; CONTRACTS_FAIL_TAIL is squeezed to 12 so a
    # 40-line failure exercises truncation without printing 340 lines to prove
    # it.
    rc=0
    out="$(CONTRACTS_SELFTEST=1 SELFTEST_SCENARIO=fail CONTRACTS_FAIL_TAIL=12 \
           TMPDIR="$td" bash "$self" --selftest-render --quiet 2>&1)" || rc=$?

    if [ "$rc" -eq 1 ]; then
        _st_ok "a red row still takes the suite's exit code to 1"
    else
        _st_bad "expected exit 1 from a render containing failures, got $rc"
    fi

    # (a) --------------------------------------------------------------------
    reason="$(_st_row_reason st_fail_talks "$out")"
    if [ -n "$reason" ]; then
        _st_ok "(a) a failing row carries a non-empty why> digest on its table row"
    else
        _st_bad "(a) st_fail_talks rendered NO why> on its row -- a red row went mute"
    fi
    _st_want "(a) the digest is the explanatory line, not merely the first one" \
             "$reason" "ST-DIGEST-SENTINEL"
    reason="$(_st_row_reason st_tri_fail "$out")"
    if [ -n "$reason" ]; then
        _st_ok "(a) run_check_tri's FAIL row carries a digest too"
    else
        _st_bad "(a) st_tri_fail rendered NO why> -- the tri-state runner went mute"
    fi

    # (c) --------------------------------------------------------------------
    reason="$(_st_row_reason st_fail_silent "$out")"
    if [ -n "$reason" ]; then
        _st_ok "(c) a failure that printed nothing still renders an explanation"
    else
        _st_bad "(c) st_fail_silent rendered a BLANK row -- exit 7, and the board said nothing"
    fi
    _st_want "(c) ...naming the exit code"              "$reason" "exit 7"
    _st_want "(c) ...and saying the check gave no reason" "$reason" "printed nothing"

    # (b) --------------------------------------------------------------------
    _st_want   "(b) a chatty failure is truncated, and the elision counts the lines" \
               "$out" "28 of 40 lines elided"
    _st_want   "(b) the head of the output survives"  "$out" "chatty-line-01"
    _st_want   "(b) the tail of the output survives"  "$out" "chatty-line-40"
    _st_absent "(b) the middle is genuinely elided"   "$out" "chatty-line-20"
    _st_want   "(b) the elision says how to see more" "$out" "CONTRACTS_FAIL_TAIL=N"
    _st_want   "(b) ...and how to re-run just that row" "$out" "re-run just this row"
    logpath="$(printf '%s\n' "$out" | sed -n 's/^.*full output: //p' | head -n 1)"
    if [ -n "$logpath" ] && [ -r "$logpath" ]; then
        n="$(grep -c '' "$logpath" || true)"
        if [ "$n" = "40" ]; then
            _st_ok "(b) the named log is readable and holds the whole 40 lines"
        else
            _st_bad "(b) the named log holds $n lines, not the full 40"
        fi
        _st_want "(b) ...including the lines the table elided" \
                 "$(cat "$logpath")" "chatty-line-20"
    else
        _st_bad "(b) the elision named no readable full-output log path: '$logpath'"
    fi

    # KNOWN is not part of this fix, but it shares these code paths ----------
    _st_want "amber rows still render as KNOWN, not FAIL" "$out" "KNOWN (correct failure"
    _st_want "amber rows still state why under --quiet"   "$out" "ST-KNOWN-SENTINEL"

    # (d) --------------------------------------------------------------------
    rc=0
    out="$(CONTRACTS_SELFTEST=1 SELFTEST_SCENARIO=pass TMPDIR="$td" \
           bash "$self" --selftest-render --quiet 2>&1)" || rc=$?
    if [ "$rc" -eq 0 ]; then
        _st_ok "(d) an all-green render exits 0"
    else
        _st_bad "(d) an all-green render exited $rc"
    fi
    _st_absent "(d) a PASSING row prints none of its own output under --quiet" \
               "$out" "ST-STDOUT-SENTINEL"
    _st_absent "(d) ...no per-check header either" "$out" "--- st_pass_talks ---"
    _st_absent "(d) ...no why> line anywhere"      "$out" "why>"
    _st_absent "(d) ...and no failure block below the table" "$out" "WHY THE FAILING ROW"

    # (d2) the positive control, so (d) cannot be won by muting run_check -----
    rc=0
    out="$(CONTRACTS_SELFTEST=1 SELFTEST_SCENARIO=pass TMPDIR="$td" \
           bash "$self" --selftest-render 2>&1)" || rc=$?
    _st_want "(d2) WITHOUT --quiet the same row still prints its output" \
             "$out" "ST-STDOUT-SENTINEL"
    _st_want "(d2) ...its header"                "$out" "--- st_pass_talks ---"
    _st_want "(d2) ...and the tri-state runner prints too" \
             "$out" "ST-STDOUT-SENTINEL-TRI"

    rm -rf "$td"

    if [ "$ST_FAILS" -eq 0 ]; then
        echo "[fail-reason-selftest] OK: a red row says why, a chatty one is bounded and"
        echo "                          points at its full log, a silent one is explained,"
        echo "                          and a green row is still silent under --quiet."
        return 0
    fi
    echo "[fail-reason-selftest] FAIL: $ST_FAILS assertion(s) did not hold. The board can" >&2
    echo "    still go red -- it just cannot say why any more, which is the state this" >&2
    echo "    row exists to make impossible. See run_check / run_check_tri /" >&2
    echo "    _reason_digest / _record_failure in this file." >&2
    return 1
}

if [ "$SELFTEST" -eq 1 ]; then
    _st_rc=0
    _fail_reason_selftest || _st_rc=$?
    exit "$_st_rc"
fi

# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
# The real board is skipped entirely under --selftest-render so the selftest
# costs seconds instead of ~340s. The rows below are NOT re-indented on purpose:
# indenting 640 lines would bury a four-line change in a 640-line diff on the
# most contended file in this repo.
if [ "$SELFTEST_RENDER" -eq 1 ]; then
    _selftest_rows
else
[ -n "$JOBS" ] || JOBS="$(_default_jobs)"
if [ "$JOBS" -gt 1 ]; then PARALLEL=1; fi
# A --selftest child must never re-enter the FULL board. One dropped flag on the
# fail_reason_selftest row below turns this file into a fork bomb, so it is
# refused structurally rather than by remembering.
if [ "${CONTRACTS_SELFTEST:-0}" = "1" ]; then
    echo "check-contracts.sh: refusing to run the full board from inside --selftest;" >&2
    echo "    a selftest child must pass --selftest-render (see _fail_reason_selftest)." >&2
    exit 2
fi
# env_registry / env_registry_selftest (scripts/check-env-registry.sh, with its ALLOWLIST) are retired: "every MC2_* read
# is registered" is now a compile error (gate::get<"MC2_X">() static_asserts the name is a row of Gates/gate_table.cpp)
# and a runtime verdict ([CONFIG v1] warn=unregistered, fatal under MC2_CONFIG_STRICT), and the raw-read ratchet (LG19)
# keeps a getenv from bypassing both. Last revision of the script: a0cf5620d.

# ---------------------------------------------------------------------------
# GATE-TABLE-G2 (2026-10-10): the registry is generated, not parsed
# ---------------------------------------------------------------------------
# The kFeatureTable / kAuxEnvVars rows of RendererFeatureRegistry.h and docs/flags.auto.yaml (v3) are generated
# from the compiled gate table (Gates/gate_table.json). The C++ grammar parser (registry_parse.py), the getenv
# sweep generator (flag-registry-auto-gen.py) and the registry truth check that graded one against the other are
# deleted with it; the tools that need the registry read the JSON through scripts/gate_table_reader.py, which
# refuses a torn or inconsistent table (build_review_pack --selftest keeps that proven below).
# gate_gen_check (below) covers the derived header rows, yaml and index, `verified`, and the compiler check; the
# fast hermetic half that used to be its own row (flag_registry_drift) was a strict subset and is retired.

run_check "log_tag_census"                 "py -3 tools/log_tag_census.py --check"
run_check "abl_writer_census"              "py -3 tools/abl_writer_census.py --check"
run_check "analyze_dangling"              "py -3 tests/analyze/analyze_dangling.py"   # lifetime gate: clang-cl catches the seeded dangling-pointer shapes, the clean twin is silent (notes section 5)
run_check "estring_callsites"          "py -3 tests/analyze/estring_callsites.py --check"   # step 4 B1: every remaining call-site row still names its kind on its line; a converted file leaves the list through --drop
run_check "container_callsites"       "py -3 tests/analyze/container_callsites.py --check"   # step 4 B2: the compiler-derived EList/SortList/PriorityQueue call-site list; an edited file needs --remap BASE, a converted one --drop
# (the vk_umbrella_readers rule was retired with the umbrella: the Vulkan scene path is unconditional)
run_check "gate_echo_reads"                 "py -3 scripts/check-gate-echo-reads.py --quiet"
# GATE-TABLE-G1A (typed-gate-registry G1a, 2026-10-09): the compiled gate table is the truth.
# gate_gen --check proves Gates/gate_index.gen.h is what Gates/gate_table.json generates, every
# seed gate is still a row, registered gates agree with the feature registry, and the per-site
# rule-override count did not rise; --rebuild compiles tools/gate_dump and diffs its JSON against
# the committed one (the compiler, not a regex, is the parser of the table). The pytest file covers
# the generator's own mapping rules.
run_check "gate_gen_check"                  "py -3 tools/gate_gen.py --check --rebuild && py -3 -B -m pytest scripts/tests/test_gate_table_gen.py -q -p no:cacheprovider"
# Class-type template parameters and consteval (Gates/gate.h) need C++20: a standalone CMake
# project that compiles a source reading MC2_* gates must reach it, or it stops building the day
# that source migrates. Self-maintaining: the list is derived from the sources, not enumerated.
run_check "sim_fp_contract"                 "py -3 scripts/check-sim-fp-contract.py && py -3 -B -m pytest scripts/tests/test_check_sim_fp_contract.py -q -p no:cacheprovider"   # every sim TU /fp:precise, no contraction (MSVC + clang-cl); rejection probes
run_check "cxx_standard_floor"              "py -3 scripts/check-cxx-standard-floor.py && py -3 -B -m pytest scripts/tests/test_check_cxx_standard_floor.py -q -p no:cacheprovider"
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
# GATE-HYGIENE-CLOSE-1: the smoke harness must keep INHERITING the parent env
# (os.environ.copy()). If that ever changes, run_smoke.py's vestigial
# passthrough tuple becomes load-bearing and every registered gate missing
# from it starts silently dropping -- gate-ON smoke runs would execute the
# gate-OFF path and report "no difference" for the wrong reason. Also asserts
# the genuinely-undriveable guard still fails CLOSED (non-zero sys.exit).
run_check "smoke_env_inheritance"  "py -3 scripts/check-smoke-env-inheritance.py"
# The smoke passthrough list is generated from the smokePassthrough rows of the gate
# table (scripts/run_smoke.py reads Gates/gate_table.json), so a registered gate
# cannot be missing from it; the old allowlist ratchet and its debt file are gone.
run_check "material_gpu_mirror"    "sh scripts/check-material-gpu-mirror.sh"
run_check "visibility_log_schema"  "sh scripts/check-visibility-log-schema.sh"
run_check "include_firewall"       "sh scripts/check-include-firewall.sh"
run_check "no_raw_gl_from_game"    "sh scripts/check-no-raw-gl-from-game.sh"
run_check "vk_submit_funnel"       "sh scripts/check-vk-submit-funnel.sh"
run_check "sim_no_audio_reads"     "sh scripts/check-sim-no-audio-reads.sh"
run_check "vfx_no_objectid"        "sh scripts/check-vfx-no-objectid.sh"
run_check "destroy_invariant"      "sh scripts/check-destroy-invariant.sh"
run_check "render_contract_gbuf1"  "sh scripts/check-render-contract-gbuffer1.sh"
run_check "binding_slots"          "py -3 scripts/check-binding-slots.py --quiet"
# light_abi_lockstep retired: the per-object light cap and record stride are derived from
# RenderCore/gpu_abi/GpuObjectLights.h and verified by the gpu_abi row below.
run_check "light_loop_numlights"   "py -3 scripts/check-light-loop-numlights-bound.py --quiet"
# LIGHTROW-FRESHNESS-1. cachedGpuLightIndex_ names a light slot that is rebased
# every frame; reading it without its cachedFrame_ stamp draws the actor against
# another actor's row (pure black, since the shader indexes light[] unchecked).
# Two of three consumers already guarded; the dynamic static-prop submit path
# did not. This freezes "every consumer references the stamp".
run_check "light_row_freshness"          "py -3 scripts/check-light-row-freshness.py --quiet"
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
# GOLDEN-COVERAGE-LEDGER-1 (VERIFY-DEBT-1): committed golden sets must account for
# every region they could cover, and must contain no un-failable (flat) crops.
run_check "golden_coverage"          "py -3 scripts/check-golden-coverage.py --quiet"
# THREE-RUN-ALL-PAIRS-1 + DEGENERATE-CROP-REFUSAL-1 (VERIFY-DEBT-1): proves bless.py
# refuses a two-run set, a duplicated run, a degenerate crop and an unstable region.
run_check "cxx17_ratchet"            "py -3 tools/cxx17_ratchet/cxx17_ratchet.py --check"
# D8 (opus-consistency-review): copy-paste is the recurring GL<->VK drift source -- a fix
# lands in one copy and the twin keeps the bug. Duplicated-line count (runs of >= 8 normalized
# lines occurring twice) per owned dir may only fall. Scans the COMMITTED tree, so another
# agent's dirty working tree cannot flap it. Selftest lives in scripts/tests (scripts_tests row).
run_check "dup_lines_ratchet"        "py -3 tools/dup_lines_ratchet.py --check"
# D11 (opus-consistency-review): the MC2->render-space axis swap (x,y,z)->(-x,z,y) is re-typed at
# ~180 sites (shaders + C++); dup_lines_ratchet cannot see one-line copies. Token-level count may
# only fall until the typed helper replaces them. HEAD-scanned like the row above. Work queue:
# docs/axis-conversion-sites.md. Selftest lives in scripts/tests (scripts_tests row).
run_check "axis_swizzle_ratchet"     "py -3 tools/axis_swizzle_ratchet.py --check"
# Comments say WHAT and WHY: batch / slice / arc tags, plan-doc and review-id references and agent names in C++ comments
# go stale when the work closes. The committed-tree count may only fall; `--diff <base>` lists the new ones on a branch.
# Pytest twin: tools/test_journal_comment_ratchet.py (tools_tests row).
run_check "journal_comment_ratchet"  "py -3 tools/journal_comment_ratchet.py --check"
# File-size program (ORCHESTRATOR-HANDOFF item 8): oversized files cost agents read context and
# force whole-TU rebuilds. Per-dir count of files > 1500 / > 3000 lines and the max file length
# may only fall, and no new file may enter the > 1500 set. Scans the COMMITTED tree. Selftest
# lives in scripts/tests (scripts_tests row).
run_check "file_size_ratchet"        "py -3 tools/file_size_ratchet.py --check"
# docs/split-map.md (old file -> pieces -> what each holds) is generated from tools/split/allow/*.json and the pieces' header comments.
run_check "split_map"                 "py -3 tools/split/split_map.py --check"
# Comment de-bloat guard (giant-file-split-plan.md, "Comment de-bloat pass"): proves the lexer, protection rules and
# exit codes still hold. Fixtures are in-memory; pytest twin is tools/test_comment_guard.py (tools_tests row).
# Doc cite repair guard (docs/cite-guard.md): proves the cite grammar, SPLIT/DELETED/STALE-LINE classes and the diff-guard still
# hold on a temp git repo. No repo-wide --check row: the docs are red by design until the triage pass lands. Pytest twin: tools/test_cite_guard.py.
run_check "pipeline_desc"          "py -3 scripts/check-pipeline-desc.py --quiet"
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
run_check "shader_injectors"       "py -3 scripts/check-shader-injectors.py --quiet"
run_check "pipeline_key"           "py -3 scripts/check-pipeline-key.py --quiet"
run_check "vfx_blend_distinction"  "py -3 scripts/check-vfx-blend-distinction.py --quiet && py -3 -B -m pytest scripts/tests/test_check_vfx_blend_distinction.py -q -p no:cacheprovider"
run_check "multiply_blend"         "py -3 scripts/check-multiply-blend.py --quiet && py -3 -B -m pytest scripts/tests/test_check_native_post_contracts.py -k multiply -q -p no:cacheprovider"
run_check "colormask_ownership"    "py -3 scripts/check-colormask-ownership.py --quiet && py -3 -B -m pytest scripts/tests/test_check_native_post_contracts.py -k color_mask -q -p no:cacheprovider"
run_check "pass_attachment"        "py -3 scripts/check-pass-attachment-contract.py --quiet"
run_check "frame_feedback"         "py -3 scripts/check-frame-resource-feedback.py --quiet"
run_check "drawbuffer_ownership"   "py -3 scripts/check-drawbuffer-ownership.py --quiet"
run_check "apply_pass_bumped"      "py -3 scripts/check-apply-pass-bumped.py --quiet"
run_check "static_prop_family"     "py -3 scripts/check-static-prop-family.py --quiet"
run_check "mdi_submission"         "py -3 scripts/check-mdi-submission-ownership.py --quiet"
run_check "buffer_lifetime"        "py -3 scripts/check-buffer-lifetime-ownership.py --quiet"
run_check "gpu_buffer_owners"      "py -3 scripts/check-gpu-buffer-owners.py --quiet"
run_check "texture_owners"         "py -3 scripts/check-texture-owners.py --quiet"
run_check "render_resource_ids"    "py -3 scripts/check-render-resource-ids.py"
run_check "vulkan_bindings"        "py -3 scripts/check-vulkan-bindings.py --quiet"
run_check "shader_variant"         "py -3 scripts/check-shader-variant.py --quiet"
# GL-VARIANT-MATRIX-1: every GL program variant the C++ can build (manifest re-extracted
# from GameOS/gameos and diffed) is assembled like shader_builder and glslang compile+LINKED;
# also proves it catches the static_prop.frag include-splice regression. ~6s, offline.
run_check "gl_shader_variants"     "py -3 scripts/check-gl-shader-variants.py --quiet && py -3 -B -m pytest scripts/tests/test_extract_gl_shader_variants.py -q -p no:cacheprovider"
run_check "spirv_artifacts"        "py -3 scripts/check-spirv-artifacts.py --quiet"
# SLANG-TOOLCHAIN-1: .slang -> SPIR-V through build_variants.py (fixtures only; no shipped Slang yet).
run_check "slang_build"            "py -3 -B -m pytest scripts/tests/test_slang_build.py -q -p no:cacheprovider"
run_check "gpu_abi"                "py -3 tools/gpu_abi_gen.py --check && py -3 -B -m pytest scripts/tests/test_gpu_abi_gen.py -q -p no:cacheprovider"
run_check "spirv_reflection"       "py -3 scripts/check-spirv-reflection-contract.py --quiet"
run_check "vk_pipeline_layout"    "py -3 scripts/check-vk-pipeline-layout.py --quiet && py -3 -B -m pytest scripts/tests/test_check_vk_pipeline_layout.py -q -p no:cacheprovider"
run_check "shader_package"         "py -3 scripts/check-shader-package.py --quiet"
# D1/D2 (opus-consistency-review A2): the three tools/ offline provers that used to be
# orphaned. check_ubo_layout cross-checks 13 C++ UBO mirror structs against their GLSL
# std140 blocks; glsl_port_diff proves the marked GL->VK shader ports verbatim (post
# rename map) with the two known VK gaps pinned in tools/glsl_port_diff.allowlist;
# check_vk_shaders compiles every shaders/vulkan/* file with glslang.
run_check "vk_ubo_layout"          "py -3 tools/check_ubo_layout.py"
run_check "glsl_port_diff"         "py -3 tools/glsl_port_diff.py --all"
run_check "vk_shaders_compile"     "py -3 tools/check_vk_shaders.py"
# CONTRACT-TRUTH-2: the census above enforces that shaders ROUTE THROUGH the
# rc_* helpers. It says nothing about whether the helpers' own documentation is
# true, and it wasn't: shaders/include/render_contract.hglsl:77 stated the
# alpha-test class as a value no shader has ever emitted -- contradicting the
# helper ten lines above it AND the rationale block twenty lines above it -- and
# the same stale constant had been copied into shaders/shadow_screen.frag. A
# constant duplicated between a comment and code, where only the code is
# checked, drifts silently. This row derives the class table from the EMITTED
# literals (GL helper bodies + the VK shader's inlined copy), cross-checks it
# against the reader bands, and fails when prose in the contract file set names
# a prop-band value that nothing emits.
run_check "render_contract_alpha_classes"          "py -3 scripts/check-render-contract-alpha-classes.py"
# RED-ROWS-CLOSE-2 (2026-07-31): the C++/GLSL struct ABI gate, wired at last --
# see the header for why the comment that excluded it was wrong on both clauses.
# It asserts that every field with an offsetof static_assert on the C++ side
# exists in the GLSL struct AT THE SAME std430 OFFSET. That is the one desync no
# other shader row can see: binding_slots/spirv_* grade bindings and artifacts,
# material_gpu_mirror grades a different struct, and a layout skew inside a
# matching-size struct is invisible to all of them while the shader silently
# reads the wrong words.
run_check "shader_schema"          "bash scripts/check-shader-schema.sh --quiet"
# VERIFY-INTEGRITY-3: bookmark poses must be able to SHOW what they grade.
# A pose framing empty terrain does not fail an A/B -- it PASSES both arms and
# manufactures evidence (mc2_10: byte-identical frames, 4801 logged rejects).
# BACKGROUND-SMOKE-1: an agent run must never take focus, go fullscreen, or
# grab the user's cursor. Three bypasses are checked: a bare Popen of the
# engine, an env policy that STRIPS the windowing vars before they reach the
# child (how the capture path silently regressed), and raw focus/cursor Win32.
run_check "background_launch"      "py -3 scripts/check-background-launch.py --quiet"
# Committed branch changes keep this row meaningful after the index is clean.
run_check "staged_hygiene"         "py -3 scripts/check-staged-hygiene.py --range --quiet"
# CURSOR-GRAB-FIX-1: the ENGINE-side half of the same policy. background_launch
# above guards the harness (Python) side, and it was fully green while the exe it
# launched clipped the user's physical pointer to a rect at (-32000,-32000):
# MC2_WINDOW_OFFSCREEN=1 shows the window offscreen, and apply_mouse_grab() gated
# only on gos_CursorControlSuppressed(), which that variable does not set. So a
# clean harness delivered the right env to an engine that ignored it. This check
# asserts, in C++, that no input-capturing call is reachable when the window is
# not user-visible -- and its --selftest re-injects that exact regression.
run_check "input_capture_visibility"          "py -3 scripts/check-input-capture-visibility.py --quiet"
run_check "bookmark_honesty"       "py -3 scripts/check-visual-bookmark-honesty.py"
# QUIET-DEFAULT-BUILD-1: a DEFAULT build must not grow new unconditional
# diagnostic output -- that log is what a real user sees on a normal launch.
# Runtime check, not a grep: "is this printf gated?" is not answerable
# lexically (getenv-seeded statics, MC2_*_DIAG bitmasks, default-ON IsEnabled(),
# #ifdefs, gated enclosing functions). It grades the newest smoke run that was
# taken with NO output-affecting MC2_* env, and SKIPs (exit 0) when no such run
# exists, so it is safe on a machine that has not smoked.
run_check "default_build_quiet"    "py -3 scripts/check-default-build-quiet.py"
# VK-OWNERSHIP-1: the ledger of hand-written Vulkan create/release code is the tool's rendering of the source; a converted island that grows manual code again, or a
# ledger nobody regenerated, is stale.
run_check "vk_ownership_ledger"    "py -3 tools/vk_ownership_ledger.py --check"
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
# CAPTURE-RESOLUTION-ASSERT-1 (HARNESS-HYGIENE-3). Golden region rects are
# normalized fractions and verify.py silently nearest-neighbour-upscales a
# smaller candidate crop to the golden's dims, so a capture taken at the WRONG
# RESOLUTION crops, grades and blesses successfully. A deploy install shipped at
# 800x600 was graded against a 4K-referenced golden -- ~17x fewer pixels -- and
# only a human reading a sidecar caught it. No tool asserted resolution; now one
# does. Self-test only here (a live check needs a capture + golden pair).
# ENV-DRIVE-GUARD-1 (HARNESS-HYGIENE-3). run_smoke's ENV-DROP guard used to
# refuse to start whenever ANY MC2_* var was set but unallowlisted, on the false
# premise that Popen's env replacement drops unlisted vars. runner.py builds the
# child env from os.environ.copy(), so nothing is dropped -- the guard was
# hard-stopping legitimate A/B arms. It now refuses only the genuinely
# UNDRIVEABLE set (FORCED_ENV / POPPED_ENV / conflicting env_extra) and RECORDS
# deliberate ambient gates in the run manifest.
run_check "env_drive_guard"        "py -3 scripts/check-env-drive-guard.py --quiet"
# ...and prove it fires, by mutating the runner's env tables.
# DEPLOY-LANE-PARITY-1 (HARNESS-HYGIENE-3). Two deploy-lane registries exist
# (deploy_lease.DEPLOY_FOLDERS and check-deploy-target.GOOD_DIRS) and the MCP
# deploy_status tool answers from their UNION -- so it reports lanes that
# `run_smoke.py --deploy` cannot resolve, and tolerates one name mapping to two
# different trees. --allow-unresolved keeps the known 0.4d-rc1 path conflict a
# LOUD WARN (it needs a user ruling) while any NEW divergence hard-fails.
run_check "deploy_lane_parity"     "py -3 scripts/check-deploy-lane-parity.py --allow-unresolved"
# ...and prove it fires, on synthetic diverged registries.
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
# The gap is now PER FIX, and this row consumes it: it exits 1 when a DECLARED
# fix is delivered on ZERO installs, so a fix that reaches nobody can no longer
# hide inside an aggregate with fixes that reached somebody.
#
# WHAT THIS BLOCK SAID HERE WAS WRONG, AND SAYING SO IS THE POINT. It declared
# the row "EXPECTED RED UNTIL A BUILD AT THE TIP IS DEPLOYED" and blamed a
# binary built at 09:53. The row is GREEN, and no deploy happened. CONTRACT-
# GREEN-1 (b919ee243) measured the actual cause: check-delivery.py's
# DEFAULT_INSTALL_ROOTS did not COVER the four newest engine binaries on this
# disk --
#     A:/Games/MC2-OpenGL-CURRENT-2026-07-30     e1bdb7af697d
#     A:/Games/MC2-OpenGL-AB-SUNAXIS-2026-07-30  dab061529977
#     A:/Games/MC2-OpenGL-TIP-2026-07-31         e4ce37d91162
#     A:/Games/MC2-OpenGL-CURSORFIX-2026-07-31   3d7f9d7871f7
# -- each of which delivers all nine manifest fixes, 9 of 9. A hand-maintained
# root list cannot keep up with installs named for the day they were cut, so the
# entry added is the glob A:/Games/MC2-OpenGL-*; graded installs went 19 -> 23
# and the row went green. A diagnosis that says "deploy and I go green" is worse
# than no diagnosis when the real fault is that the tool was not looking: it
# sends the reader to do work that would not have changed the verdict.
#
# WHAT IS STILL TRUE, AND IS NOT A CLEAN WIN. The pass bar is "at least ONE of
# the 23 installs shows the fix", so adding installs can only make this row
# EASIER to pass. It can therefore be green off one freshly-cut directory while
# the install carrying the PRODUCT NAME -- A:/Games/MC2 OpenGL Remastered --
# still shows 0 of 9, which is exactly the state on this disk today. Read this
# row as "somebody could reach these fixes", never as "the user can". Nominating
# ONE graded install is the real fix; it is a user decision and is filed
# separately rather than taken here.
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
run_check "doc_links"             "py -3 scripts/check-doc-links.py --quiet"
# META-FIX-BATCH-1 (design C): split rendering-path contracts — color
# convention table (#24 class), depth-fudge single-source/form, tess predicate
# chokepoint (#34 class), GL/VK pair stamps. Self-test proves each rule still
# fails on a synthetic violation (vulkan_bindings_selftest precedent).
run_check "split_path_contracts"   "py -3 scripts/check-split-path-contracts.py --quiet"
# META-FIX-BATCH-1 (design E): canonical-key chokepoint confinement
# (FastFileFind/elfHash callers) + raw read-open count ratchet.
run_check "path_normalization"     "sh scripts/check-path-normalization.sh"
# META-FIX-BATCH-2 (design D): resolution/aspect two-space contract — modern
# dirs never read Environment.screenWidth/Height (RULE A), legacy-layer reads
# only shrink (RULE B generated ratchet), and the four #45/#49/HUD-RES-CLAMP
# fix sites survive (RULE C anchors). Runtime sibling: MC2_RES_ASSERT.
run_check "res_aspect_contract"    "py -3 scripts/check-res-aspect-contract.py --quiet"
# LAUNCHER-ENCODING-1: launcher-facing manifest strings (mods/ + examples/mods/
# mod.json) are ASCII-only — the launcher UI is Win32 ANSI, so UTF-8 typography
# (em-dash, curly quotes) renders as mojibake. Self-test proves a synthetic
# em-dash manifest still fails.
run_check "launcher_ascii"         "py -3 scripts/check-launcher-ascii.py --quiet"
# META-FIX-BATCH-2 (design F): silent-order chokepoint — every fireWeapon
# pre-delivery early-return is traced (fireGateTrace) or declared
# (KNOWN_UNCOVERED); comment-stripped scan (four commented return(4)s in
# mech_weapons.cpp are the trap); nofire emitter presence; both-ways-live ledger.
run_check "order_gate_coverage"    "py -3 scripts/check-order-gate-coverage.py --quiet"
# META-FIX-BATCH-3 (design A): mover-registry coherence — setOnGUI/isOnGui
# write allowlists (S1), write-pairing windows (S2), trimmed read-gate pins
# (S3, hard=3), #42b hover-guard shape (S4 — SOLE defense, never demote),
# link-up gate shape (S5), off-map-staging carve-out: runtime audit region
# stays position-free (S6, INVIS-VEHICLES-FIX-1). Runtime sibling:
# MC2_MOVER_COHERENCE_AUDIT (force-set by run_smoke for --tier runs).
run_check "mover_registry_coherence" "sh scripts/check-mover-registry-coherence.sh"
# META-FIX-BATCH-3 (design B): fail-loud asset-load policy — raw loader-call
# confinement (P1, TGL-LOAD-EXEMPT/VERIFIED tags), PART 0-4 presence pins
# (P2), and the APPEAR-AUDIT off-map-staging carve-out (P3: scan region
# stays position/visibility-free — INVIS-VEHICLES-FIX-1).
run_check "tgl_load_callers"   "sh scripts/check-tgl-load-callers.sh"
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
# STEP-3.5 (2026-10-11): the comparator behind "SIMID unchanged" -- two runs of one mission under the determinism gates must agree frame by frame.
# The selftest is offline; the live proof is `py -3 tools/sim_determinism.py run mc2_03 --baseline` (launches the game twice; run it at the program boundaries).
run_check "sim_determinism"             "py -3 tools/sim_determinism.py selftest"
# The save/load round trip (tools/saveload_roundtrip.py run) launches the game; this row proves its analysis. Live result at 2026-10-11: FAILS (see lane1-state).
run_check "saveload_roundtrip"          "py -3 tools/saveload_roundtrip.py selftest"
run_check "save_inspect"                "py -3 tools/save_inspect.py selftest"
# STEP-4 B0 (2026-10-11): the ownership ledger (type -> allocation sites, typed free sites, owner) and the pointer-order hazard list, both
# ratchets over HEAD; a batch that migrates a type rewrites the baseline with its code (tests/unit/*_baseline.json).
run_check "own_inventory"               "py -3 tools/own_inventory.py --check"
run_check "pointer_order_scan"          "py -3 tools/pointer_order_scan.py --check"
run_check "verified_ledger"             "py -3 tools/verified.py check"
run_check "script_callers"              "py -3 tools/callers.py --check"
run_check "savegame_backup"             "py -3 tools/savegame_backup.py selftest"

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
# The provenance GATE the minting tools call (bless.py / verify.py).
# Proves each verdict on binaries carrying real
# fingerprint stamps, that a hand-added claim and a keywords-in-a-comment
# manifest both stay UNESTABLISHED, and -- the other half -- that an honest
# artifact still passes. Offline, read-only, no game launch.
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

# The same page's GATE INVENTORY is rebuilt from RenderCore/RendererFeatureRegistry.h
# at render time. That parse used to warn on stderr and publish anyway: one
# regeneration on 2026-07-30 parsed 10 rows of 359 from a torn read, exited 0,
# and wrote a well-formed page in which 13 of the 39 gates the pack names lost
# their compiled default -- one rendering the red claim "GHOST: this gate does
# nothing" about a gate that is registered and works. parse_registry() now
# refuses (exit 2, page not written) on a missing table, an unclosed table, an
# undecodable byte, a duplicate key, or any row the source declares that did not
# parse. This arm proves the guard FIRES: it corrupts a COPY OF THE REAL header
# four ways and requires a refusal from each, so the guard cannot go blind on a
# fixture that stopped resembling the file it stands for. Offline, read-only.

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
# CONTRACTS-DOC-SELFTEST-1: and the aggregator's OWN rendering, which until now
# was the one guarded behaviour on this board with nothing guarding it. See the
# block above _selftest_rows for what it pins and why it re-executes this script
# rather than reimplementing run_check. Seconds, three short child runs.
run_check "agents_md"                  "sh scripts/check-agents-md.sh"
run_check "asset_manifest_fixtures"    "py -3 scripts/check-asset-manifest-fixtures.py"
run_check "asset_pipeline_doc"         "sh scripts/check-asset-pipeline-doc.sh"
run_check "claude_md_pointer"          "sh scripts/check-claude-md-pointer.sh"
run_check "json_isolation"             "sh scripts/check-json-isolation.sh"
run_check "memory_size"                "py -3 scripts/check-memory-size.py"
run_check "mlr_leaves_gated"           "sh scripts/check-mlr-leaves-gated.sh"
run_check "object_mvp_currency"        "py -3 scripts/check-object-mvp-currency.py"
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
# new_gates (scripts/check-new-gates.py --base HEAD~1) is retired: a new MC2_* gate in a diff must be a row of the gate
# table or it does not compile, and a raw getenv is a ratchet failure. Last revision of the script: a0cf5620d.

# ---------------------------------------------------------------------------
# SCRIPTS-TESTS-WIRE-1 (2026-07-31) -- a whole TEST TIER nothing executed
# ---------------------------------------------------------------------------
# check-contract-coverage.py grades scripts/check-*.{py,sh}. It does not look
# into scripts/tests/, so the four pytest modules there were dark in the one way
# this board cannot see: not "unwired and reported", just absent from the census
# entirely. Nothing in scripts/, no CI (there is no .github/workflows), and no
# row here ran them. Their only mentions in the whole repo are two prose docs
# and one docstring -- the mention-is-not-a-caller shape that arm (b) of
# check-contract-coverage.py exists to refuse, one directory out of its reach.
#
# A test tier nothing executes is indistinguishable from no tests at all, and
# that is not a slogan here: this session found a doctest binary reporting
# "630 passed, 0 failed, SUCCESS, exit 0" while a real case had been silenced,
# and a `COUNT == 46` assertion that sat unevaluated while the true value walked
# to 61 over fifteen appends.
#
# MEASURED, not taken on report: 46 tests, 2.2-2.4s wall over three runs, which
# is inside the 5s-per-check budget this file promises and ~0.7% of a ~340s
# board. `-B` and `-p no:cacheprovider` keep it honestly READ-ONLY: no .pyc, no
# .pytest_cache write. Exit 5 on zero-collected (verified), so the tier being
# deleted or renamed out from under this row fails LOUD instead of passing on an
# empty set.
#
# WHAT THIS ROW DOES NOT DO, stated so nobody reads more into a green: it does
# not pin the test COUNT, so deleting ONE test still passes. A naked count
# assertion here would go red the day someone adds a test, and a permanently-red
# row teaches everyone to skip red -- the failure mode the PER-CHECK WALL TIME
# note above refuses for the same reason. scripts/check-doctest-count-locks.py
# is the mechanism that manages a count lock properly if this tier ever needs
# one.
# pytest-xdist, when installed, runs the tier on the lane's 2 cores (-n 2); without it the tier stays serial.
_XDIST_N=""; if py -3 -c "import xdist" >/dev/null 2>&1; then _XDIST_N="-n 2"; fi
run_check "scripts_tests"              "py -3 -B -m pytest scripts/tests/ -q -p no:cacheprovider $_XDIST_N"

# ---------------------------------------------------------------------------
# TOOLS-TESTS-WIRE-1 (2026-10-09) -- the pytest modules that live NEXT TO their tools
# ---------------------------------------------------------------------------
# scripts_tests above covers scripts/tests/ only. Another ~75 test modules sit
# beside the tools they test (tools/terrain_beautify, tools/unit_converter,
# tools/blender_pipeline, tests/smoke, tests/terrain_gen, tools/asset_cook, ...)
# and no gate ran them: dark in exactly the way SCRIPTS-TESTS-WIRE-1 describes.
# Owner ruling 2026-10-09: they are live tests of live tools, so they run.
#
# scripts/run_tools_tests.py discovers them via git ls-files, runs each module in
# its own process in parallel (read-only: -B, no cacheprovider; script-style
# modules that pytest cannot collect are run as plain scripts), and reconciles
# failures against scripts/tools_tests.known-failures. It is run_check_tri on
# purpose: exit 3 == "every failing test is pinned in the ledger" renders as
# KNOWN, not PASS. A failure outside the ledger is red; a ledgered test that
# starts passing is ALSO red (stale ledger -- delete the line), so the ledger can
# only shrink. Nobody fixed the pinned failures when this row was added.
# Excluded: test_cxx17_ratchet (own row above) and test_release_zip_install
# (assembles release zips + launches a smoke; not an offline contract).
run_check_tri "tools_tests"             "py -3 scripts/run_tools_tests.py"
run_check "sim_include_firewall"        "py -3 -B scripts/tests/test_sim_include_firewall.py && py -3 scripts/check_sim_include_firewall.py"
fi   # end of the real board (skipped under --selftest-render, above)
if [ "$PARALLEL" -eq 1 ]; then _drain_queue; fi

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
        case "$CACHED_SET" in *" $label "*) mark=" (cached)$mark" ;; esac
        printf "  %-40s %8ss  PASS%s\n" "$label" "$secs" "$mark"
    elif [ "$result" = "KNOWN" ]; then
        printf "  %-40s %8ss  KNOWN (correct failure, ledger-pinned)%s\n" "$label" "$secs" "$mark"
    else
        printf "  %-40s %8ss  FAIL  <---%s\n" "$label" "$secs" "$mark"
        # FAIL-REASON-1: the row itself says what went wrong, so triage --
        # "act on this" vs "understood, awaiting a decision" -- costs a glance
        # and not a 340s re-run.
        if [ -n "${REASONS[$i]:-}" ]; then
            printf "        why> %s\n" "${REASONS[$i]}"
        fi
    fi
done
echo "---------------------------------------------------------------------"
printf "  %d passed, %d failed, %d known-correct-failure\n" \
    "$PASS_COUNT" "$FAIL_COUNT" "$KNOWN_COUNT"
printf "  summed row time %d.%02ds across %d rows\n" \
    "$((TOTAL_MS / 1000))" "$(((TOTAL_MS % 1000) / 10))" "${#NAMES[@]}"
_WALL_MS=$(( $(date +%s%N | cut -c1-13) - _WALL_T0 ))
printf "  wall time %d.%02ds (jobs=%s)\n" "$((_WALL_MS / 1000))" "$(((_WALL_MS % 1000) / 10))" "${JOBS:-1}"
if [ "${#OVER_BUDGET[@]}" -gt 0 ]; then
    printf "  %d row(s) over the %ss-per-check budget this file promises:\n" \
        "${#OVER_BUDGET[@]}" "$BUDGET_SEC"
    for e in "${OVER_BUDGET[@]}"; do
        printf "      %-38s %ss\n" "${e%%|*}" "${e##*|}"
    done
    echo "  (measured, not enforced -- see the PER-CHECK WALL TIME note above)"
fi
# FAIL-REASON-1: detail for the red rows, BELOW the table on purpose. A reason
# printed 130 rows up in the scrollback is a reason nobody reads, and that is
# the whole defect this closes. Volume here is proportional to the number of RED
# rows -- a green board adds not one byte.
if [ "${#FAIL_LABELS[@]}" -gt 0 ]; then
    echo ""
    echo "  WHY THE FAILING ROW(S) FAILED"
    echo "  ---------------------------------------------------------------------"
    for i in "${!FAIL_LABELS[@]}"; do
        echo "  --- ${FAIL_LABELS[$i]} ---"
        printf '%s\n' "${FAIL_BLOCKS[$i]}" | sed 's/^/    /'
        echo ""
    done
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
if [ -n "$ROWS_RE" ]; then
    echo ""
    echo "  PARTIAL BOARD: --rows '$ROWS_RE' ran $((PASS_COUNT + FAIL_COUNT + KNOWN_COUNT)) row(s) and skipped $ROWS_SKIPPED."
    echo "  This is NOT a green board; run the full board (no --rows) before committing a batch."
    if [ "$((PASS_COUNT + FAIL_COUNT + KNOWN_COUNT))" -eq 0 ]; then
        echo "  No row label matched '$ROWS_RE' -- nothing was checked."
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
fi
echo "====================================================================="

if [ "$FAIL_COUNT" -gt 0 ]; then
    exit 1
fi
exit 0
