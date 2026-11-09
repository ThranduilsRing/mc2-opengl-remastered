#!/bin/sh
# scripts/check-path-normalization.sh
#
# META-FIX design E (file-path fragility, issue #8 family): static guard for
# the canonical-key chokepoint (mclib/fst_hash.cpp).
#
# Rule 1 — CONFINEMENT: FastFileFind( / elfHash( call sites may exist ONLY in
#   the chokepoint surface {mclib/file.cpp, mclib/fastfile.cpp/.h,
#   mclib/ffile.cpp, mclib/fst_hash.cpp/.h, mclib/vfs.cpp} (tests/unit/ and tools/ are not
#   scanned). A new game-code caller bypassing the normalized funnel is red.
#
# Rule 2 — PER-FILE COUNT RATCHET: raw read-mode opens (fopen "r*",
#   _open _O_RDONLY, CreateFileA GENERIC_READ, ifstream) in code/ gui/ mclib/
#   are counted per file and compared against scripts/path-open-baseline.txt.
#   Any file EXCEEDING its baseline fails (a new bypass lookup layer);
#   decreases print a notice to regenerate via --update-baseline. Intentional
#   new loose-only readers take an inline exemption comment:
#       // path-audit: loose-only
#   (pattern precedent: check-asset-scale-callers.sh). The existing ~32
#   loose-only sidecar/manifest readers are deliberately OUTSIDE the
#   canonical-key contract (Win32 fopen is separator/case-agnostic) — the
#   ratchet only guards against growth.
#
# Usage:
#   sh scripts/check-path-normalization.sh                  # check (CI)
#   sh scripts/check-path-normalization.sh --update-baseline
#   sh scripts/check-path-normalization.sh --self-test
#
# Exit: 0 pass / 1 violation / 2 harness error.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BASELINE="$ROOT/scripts/path-open-baseline.txt"
TAB="$(printf '\t')"

# ---------------------------------------------------------------------------
# Rule 1: chokepoint confinement.
#   $1 = root to scan. Prints violating grep lines; empty when clean.
# ---------------------------------------------------------------------------
rule1_confinement() {
    scan_root="$1"
    dirs=""
    for d in code gui mclib GameOS editor netlib RenderCore RenderWorld GameAdapters data_tools; do
        [ -d "$scan_root/$d" ] && dirs="$dirs $scan_root/$d"
    done
    [ -n "$dirs" ] || return 0
    # shellcheck disable=SC2086
    grep -rEn '(FastFileFind|elfHash)[[:space:]]*\(' $dirs \
        --include='*.cpp' --include='*.h' --include='*.hpp' 2>/dev/null \
        | grep -Ev '^[^:]*:[0-9]+:[[:space:]]*(//|\*)' \
        | grep -Ev 'mclib/(file|fastfile|ffile|fst_hash)\.(cpp|h):|mclib/vfs\.cpp:' \
        || true
}

# ---------------------------------------------------------------------------
# Rule 2: raw read-open census.
#   $1 = root to scan. Emits "count<TAB>relpath" per file.
# ---------------------------------------------------------------------------
rule2_census() {
    scan_root="$1"
    dirs=""
    for d in code gui mclib; do
        [ -d "$scan_root/$d" ] && dirs="$dirs $scan_root/$d"
    done
    [ -n "$dirs" ] || return 0
    # shellcheck disable=SC2086
    grep -rEn 'fopen[[:space:]]*\([^;]*"r|_open[[:space:]]*\([^;]*_O_RDONLY|CreateFileA[[:space:]]*\([^;]*GENERIC_READ|std::ifstream|[^A-Za-z_]ifstream[[:space:]]' \
        $dirs --include='*.cpp' --include='*.h' 2>/dev/null \
        | grep -Ev '^[^:]*:[0-9]+:[[:space:]]*(//|\*)' \
        | grep -v 'path-audit: loose-only' \
        | sed -e 's/:[0-9][0-9]*:.*$//' \
        | sed -e "s|^$scan_root/||" \
        | sort | uniq -c \
        | awk -v OFS="$TAB" '{print $1, $2}' \
        || true
}

# Compare census against the baseline; mode=regress|shrink.
compare_census() {
    census_text="$1"; mode="$2"
    printf '%s\n' "$census_text" | awk -F"$TAB" -v baseline="$BASELINE" -v mode="$mode" '
        BEGIN {
            while ((getline line < baseline) > 0) {
                if (line ~ /^#/ || line == "") continue
                n = split(line, a, "\t")
                if (n == 2) base[a[2]] = a[1] + 0
            }
        }
        NF == 2 {
            cur = $1 + 0; p = $2
            b = (p in base) ? base[p] + 0 : 0
            if (mode == "regress" && cur > b)
                printf "  %s: raw read-opens %d > baseline %d\n", p, cur, b
            if (mode == "shrink" && (p in base) && cur < b)
                printf "  %s: %d -> %d\n", p, b, cur
        }'
}

run_check() {
    fail=0

    viol="$(rule1_confinement "$ROOT")"
    if [ -n "$viol" ]; then
        echo "[path-normalization] Rule 1 FAIL: FastFileFind/elfHash caller outside the chokepoint surface:"
        echo "$viol"
        echo "  -> route lookups through File::open / fileExists (mclib/file.cpp), or extend the surface with review"
        fail=1
    fi

    if [ ! -f "$BASELINE" ]; then
        echo "[path-normalization] Rule 2 HARNESS ERROR: baseline missing: $BASELINE (run --update-baseline)"
        return 2
    fi
    census="$(rule2_census "$ROOT")"
    regress="$(compare_census "$census" regress)"
    if [ -n "$regress" ]; then
        echo "[path-normalization] Rule 2 FAIL: raw read-open count grew past the ratchet baseline:"
        echo "$regress"
        echo "  -> intentional loose-only reader? tag the line '// path-audit: loose-only'."
        echo "     resolves game data? route through the File::open ladder instead."
        echo "     deliberate growth only: sh scripts/check-path-normalization.sh --update-baseline"
        fail=1
    fi
    shrink="$(compare_census "$census" shrink)"
    if [ -n "$shrink" ]; then
        echo "[path-normalization] notice: counts shrank (good) — refresh with --update-baseline:"
        echo "$shrink"
    fi

    if [ "$fail" -ne 0 ]; then
        echo "FAIL: path-normalization contract violated (see above)"
        return 1
    fi
    echo "OK: path-normalization (Rule 1 confinement clean; Rule 2 ratchet held)"
    return 0
}

update_baseline() {
    {
        echo "# scripts/path-open-baseline.txt — META-FIX design E Rule-2 count ratchet."
        echo "# Per-file raw read-mode open counts (fopen \"r*\" / _open O_RDONLY /"
        echo "# CreateFileA GENERIC_READ / ifstream) in code/ gui/ mclib/, excluding"
        echo "# '// path-audit: loose-only' tagged lines. Regenerate ONLY deliberately:"
        echo "#   sh scripts/check-path-normalization.sh --update-baseline"
        echo "# Generated at $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)."
        rule2_census "$ROOT"
    } > "$BASELINE"
    rows=$(grep -c "$TAB" "$BASELINE" || true)
    echo "baseline written: $BASELINE ($rows file rows)"
    return 0
}

self_test() {
    if ! run_check > /dev/null 2>&1; then
        echo "path-normalization --self-test: FAIL (not green on current tree)"
        return 1
    fi
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/code"

    # Rule 1 synthetic violation: a game TU calls elfHash directly.
    cat > "$tmp/code/selftest_fake.cpp" <<'EOF'
unsigned long elfHash(const char*);
unsigned long f(const char* n) { return elfHash(n); }
EOF
    v="$(rule1_confinement "$tmp")"
    if [ -z "$v" ]; then
        echo "path-normalization --self-test: FAIL (Rule 1 synthetic caller NOT detected)"
        return 1
    fi

    # VFS owns the ladder's FST calls, but its raw opens still need inline exemptions.
    mkdir -p "$tmp/mclib"
    cat > "$tmp/mclib/vfs.cpp" <<'EOF'
void f() { FastFileFind("data/x.bin", handle); }
void g() { FILE* f = fopen("data/x.bin", "rb"); (void)f; }
EOF
    v="$(rule1_confinement "$tmp" | grep 'mclib/vfs.cpp:' || true)"
    c="$(rule2_census "$tmp" | awk -F"$TAB" '$2 == "mclib/vfs.cpp" {print $1}')"
    if [ -n "$v" ] || [ "${c:-0}" -ne 1 ]; then
        echo "path-normalization --self-test: FAIL (VFS confinement/census boundary)"
        return 1
    fi

    # Rule 2 census must count synthetic raw opens...
    cat > "$tmp/code/selftest_open.cpp" <<'EOF'
#include <cstdio>
void g() { FILE* f = fopen("data/x.bin", "rb"); (void)f; }
void h() { FILE* f = fopen("data/y.bin", "r"); (void)f; }
EOF
    c="$(rule2_census "$tmp" | awk -F"$TAB" '/selftest_open.cpp/{print $1}')"
    if [ "${c:-0}" -lt 2 ]; then
        echo "path-normalization --self-test: FAIL (Rule 2 census missed synthetic opens, got '${c:-0}')"
        return 1
    fi
    # ...and honor the loose-only exemption tag.
    cat > "$tmp/code/selftest_exempt.cpp" <<'EOF'
#include <cstdio>
void k() { FILE* f = fopen("x.json", "rb"); (void)f; } // path-audit: loose-only
EOF
    e="$(rule2_census "$tmp" | awk -F"$TAB" '/selftest_exempt.cpp/{print $1}')"
    if [ -n "${e:-}" ]; then
        echo "path-normalization --self-test: FAIL (loose-only exemption not honored)"
        return 1
    fi

    echo "path-normalization --self-test: PASS (green on tree; Rule 1 + Rule 2 red on synthetic violations; exemption honored)"
    return 0
}

case "${1:-}" in
    --self-test)       self_test ;;
    --update-baseline) update_baseline ;;
    "")                run_check ;;
    *) echo "usage: $0 [--self-test|--update-baseline]" >&2; exit 2 ;;
esac
