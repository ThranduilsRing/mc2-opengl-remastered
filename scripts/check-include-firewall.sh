#!/usr/bin/env sh
# scripts/check-include-firewall.sh
#
# Phase 1 (M1): grep-based include + symbol firewall for the
# RenderWorld boundary. Run pre-commit when any RenderCore/,
# RenderWorld/, or GameAdapters/ file changes.
#
# Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
#       section 12.
#
# Exit 0  = clean
# Exit 1  = at least one violation

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# M2 fix (adversarial review pass 2 2026-05-22): enumerate the full
# Section 12 module list. The `[ -d ] || continue` guard below makes
# the script forward-compatible without per-slice script edits.
# GameAdapters is the carve-out module: it MAY include both sides.
# Do NOT add it to SCOPE_DIRS.
SCOPE_DIRS="RenderCore RenderWorld Visibility MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL EditorBridge Gates"

# EDITOR-BRIDGE-GPU-FIREWALL-1: editor/ is a header-ONLY scope. Unlike the
# RenderWorld-boundary modules above, the mission editor legitimately includes
# game-logic headers and references game-logic symbols (Appearance, ObjectManager,
# Mission, ...) -- those are NOT violations here. The ONLY thing forbidden in
# editor/ is direct inclusion of the GPU-internal batcher/registry/cull headers;
# all editor GPU access must route through EditorBridge::*. So editor/ is checked
# against EDITOR_FORBIDDEN_HEADERS only, never against FORBIDDEN_SYMBOLS.
EDITOR_HEADER_ONLY_DIRS="editor"
EDITOR_FORBIDDEN_HEADERS="gos_static_prop_batcher.h gos_static_prop_registry.h gos_mech_batcher.h gpu_cull_compute.h"

# Forbidden headers (any include of these from SCOPE_DIRS is a violation).
# C1 fix: RenderCore must stay pure -- gos_static_prop_batcher.h pulls
# <GL/glew.h> + Stuff/Stuff.hpp transitively; that violates Section 12
# even though the header is engine-side. Same for tgl.h / msl.h.
FORBIDDEN_HEADERS="appear.h bdactor.h mech3d.h objectappearance.h objmgr.h mission.h warrior.h gos_static_prop_batcher.h tgl.h msl.h GL/glew.h Stuff/Stuff.hpp"

# Forbidden symbol names (case-sensitive). Catches forward-decls,
# function-signature uses, typedef/using aliases that an include-only
# checker misses.
FORBIDDEN_SYMBOLS="Appearance BldgAppearance TreeAppearance GVAppearance Mech3DAppearance GenericAppearance ObjectAppearance ObjectManager Mission MechWarrior"

VIOLATIONS=0

allowlisted() {
    # $1 = path
    while IFS= read -r _aline; do
        # skip blanks and comments
        case "$_aline" in
            ""|"#"*) continue ;;
        esac
        if [ "$1" = "$_aline" ]; then
            return 0
        fi
    done < scripts/check-include-firewall.allowlist
    return 1
}

# Strip the path:lineno: prefix and check if the remaining content is
# a C/C++ comment-only line (// ... or part of a /* ... */ block).
# This is a conservative single-line check; multi-line block comments
# that span more than one source line are not stripped, but a hit on
# the opening or middle line of such a block is still rejected if it
# starts with whitespace + `*` or `/*`.
is_comment_line() {
    # $1 = full grep "path:lineno:content" line
    # Trim leading whitespace from content portion.
    body="$(printf '%s' "$1" | sed 's/^[^:]*:[0-9][0-9]*://' | sed 's/^[[:space:]]*//')"
    case "$body" in
        "//"*) return 0 ;;
        "/*"*) return 0 ;;
        "*"*)  return 0 ;;
    esac
    return 1
}

TMPHITS="$(mktemp 2>/dev/null || echo "/tmp/firewall.$$")"
trap 'rm -f "$TMPHITS"' EXIT

# PERF (2026-10-09): this used to run ~190 recursive greps and, per HIT, a chain
# of cut/sed/grep subprocesses plus a re-read of the allowlist (~20 s on Windows,
# where every spawn costs tens of ms). Now: ONE recursive grep per dir per class
# builds a candidate cache, and ONE awk per class does the per-header/per-symbol
# match, allowlist, comment-line and code-body filtering in-process. Semantics
# are those of the old loops, applied in the same order:
#   for each dir: for each header (hits in grep -r order) ; for each symbol ;
#   then for each editor dir: for each editor-forbidden header.
# A pattern is matched against the CONTENT only (after "path:lineno:"), never
# the path. Hit filters: allowlisted file -> skip; comment-looking line (body
# starts with //, /* or * after leading whitespace) -> skip; symbols additionally
# need the word to survive stripping of "string literals", trailing // comments,
# RenderResourceLifetime::Mission and an `Mission = <digit>...` enumerator line.
CACHE="$(mktemp 2>/dev/null || echo "/tmp/firewall-c.$$")"
OUTV="$(mktemp 2>/dev/null || echo "/tmp/firewall-o.$$")"
trap 'rm -f "$TMPHITS" "$CACHE" "$OUTV"' EXIT

_alt() { _r=""; for _w in $1; do _r="${_r:+$_r|}$_w"; done; printf '%s' "$_r"; }

# _scan <kind: hdr|sym> <dir> <words> <message-prefix>
_scan() {
    if [ "$1" = "hdr" ]; then
        grep -rnE "include.*($(_alt "$3"))" "$2" > "$CACHE" 2>/dev/null || true
    else
        grep -rnwE "$(_alt "$3")" "$2" > "$CACHE" 2>/dev/null || true
    fi
    [ -s "$CACHE" ] || return 0
    awk -v kind="$1" -v words="$3" -v pfx="$4" -v allow="scripts/check-include-firewall.allowlist" '
        BEGIN {
            while ((getline a < allow) > 0) { if (a != "" && substr(a,1,1) != "#") ok[a] = 1 }
            nw = split(words, W, " ")
        }
        { if ($0 != "") L[++m] = $0 }
        END {
            for (w = 1; w <= nw; w++) {
                word = W[w]
                for (k = 1; k <= m; k++) {
                    line = L[k]
                    if (!match(line, /^[^:]*:[0-9]+:/)) continue
                    content = substr(line, RLENGTH + 1)
                    file = substr(line, 1, index(line, ":") - 1)
                    if (kind == "hdr") {
                        if (content !~ ("include.*" word)) continue
                    } else {
                        if (content !~ ("(^|[^[:alnum:]_])" word "($|[^[:alnum:]_])")) continue
                    }
                    if (file in ok) continue
                    body = content; sub(/^[[:space:]]+/, "", body)
                    if (substr(body,1,2) == "//" || substr(body,1,2) == "/*" || substr(body,1,1) == "*") continue
                    if (kind == "sym") {
                        cb = content
                        gsub(/"[^"]*"/, "", cb)
                        sub(/\/\/.*$/, "", cb)
                        gsub(/RenderResourceLifetime::Mission/, "", cb)
                        sub(/^[[:space:]]*Mission[[:space:]]*=[[:space:]]*[0-9].*$/, "", cb)
                        if (cb !~ ("(^|[^[:alnum:]_])" word "($|[^[:alnum:]_])")) continue
                        print "VIOLATION: forbidden symbol \047" word "\047 in " line
                    } else {
                        print "VIOLATION: " pfx word " in " line
                    }
                }
            }
        }' "$CACHE" >> "$OUTV"
}

: > "$OUTV"
for dir in $SCOPE_DIRS; do
    if [ ! -d "$dir" ]; then continue; fi
    _scan hdr "$dir" "$FORBIDDEN_HEADERS" "forbidden include of "
    _scan sym "$dir" "$FORBIDDEN_SYMBOLS" ""
done

for dir in $EDITOR_HEADER_ONLY_DIRS; do
    if [ ! -d "$dir" ]; then continue; fi
    _scan hdr "$dir" "$EDITOR_FORBIDDEN_HEADERS" "editor/ must route GPU access through EditorBridge -- forbidden include of "
done

if [ -s "$OUTV" ]; then
    cat "$OUTV" >&2
    VIOLATIONS=$(grep -c '' "$OUTV")
fi

if [ "$VIOLATIONS" -gt 0 ]; then
    echo "" >&2
    echo "scripts/check-include-firewall.sh: ${VIOLATIONS} violation(s)" >&2
    echo "Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md section 12" >&2
    exit 1
fi
echo "scripts/check-include-firewall.sh: clean (scope: ${SCOPE_DIRS}; editor header-only: ${EDITOR_HEADER_ONLY_DIRS})"
exit 0
