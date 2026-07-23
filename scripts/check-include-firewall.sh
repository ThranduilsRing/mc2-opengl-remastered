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
SCOPE_DIRS="RenderCore RenderWorld Visibility MeshRenderer MaterialSystem DebugRenderer RenderDeviceGL EditorBridge HostServices"

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

for dir in $SCOPE_DIRS; do
    if [ ! -d "$dir" ]; then continue; fi
    # Headers
    for hdr in $FORBIDDEN_HEADERS; do
        : > "$TMPHITS"
        grep -rn "include.*${hdr}" "$dir" > "$TMPHITS" 2>/dev/null || true
        if [ -s "$TMPHITS" ]; then
            while IFS= read -r line; do
                [ -z "$line" ] && continue
                file="$(printf '%s' "$line" | cut -d: -f1)"
                if allowlisted "$file"; then
                    continue
                fi
                if is_comment_line "$line"; then
                    continue
                fi
                echo "VIOLATION: forbidden include of ${hdr} in ${line}" >&2
                VIOLATIONS=$((VIOLATIONS+1))
            done < "$TMPHITS"
        fi
    done
    # Symbols (word-boundary case-sensitive grep)
    for sym in $FORBIDDEN_SYMBOLS; do
        : > "$TMPHITS"
        grep -rwn "${sym}" "$dir" > "$TMPHITS" 2>/dev/null || true
        if [ -s "$TMPHITS" ]; then
            while IFS= read -r line; do
                [ -z "$line" ] && continue
                file="$(printf '%s' "$line" | cut -d: -f1)"
                if allowlisted "$file"; then
                    continue
                fi
                if is_comment_line "$line"; then
                    continue
                fi
                # MERGE-WAVE-2 (2026-07-22): symbol coupling only matters in CODE.
                # Strip double-quoted string literals (registry description
                # strings) and trailing // comments from the body, then re-check
                # the word match. Doc-string/trailing-comment mentions of game
                # symbols are documentation, not includes/decls/signatures.
                code_body="$(printf '%s' "$line" | sed 's/^[^:]*:[0-9][0-9]*://' | sed 's/"[^"]*"//g' | sed 's|//.*$||' | sed 's/RenderResourceLifetime::Mission//g' | sed 's/^[[:space:]]*Mission[[:space:]]*=[[:space:]]*[0-9].*$//')"
                # (RenderResourceLifetime::Mission is RenderCore's OWN lifetime
                # enumerator — an unfortunate name collision with the game's
                # Mission class, not a boundary breach; the enumerator
                # definition line in RenderResourceRegistry.h likewise.)
                if ! printf '%s' "$code_body" | grep -qw "${sym}"; then
                    continue
                fi
                echo "VIOLATION: forbidden symbol '${sym}' in ${line}" >&2
                VIOLATIONS=$((VIOLATIONS+1))
            done < "$TMPHITS"
        fi
    done
done

# EDITOR-BRIDGE-GPU-FIREWALL-1: header-only firewall for editor/.
# Forbids direct GPU-internal-header includes; game-logic symbols are allowed.
for dir in $EDITOR_HEADER_ONLY_DIRS; do
    if [ ! -d "$dir" ]; then continue; fi
    for hdr in $EDITOR_FORBIDDEN_HEADERS; do
        : > "$TMPHITS"
        grep -rn "include.*${hdr}" "$dir" > "$TMPHITS" 2>/dev/null || true
        if [ -s "$TMPHITS" ]; then
            while IFS= read -r line; do
                [ -z "$line" ] && continue
                file="$(printf '%s' "$line" | cut -d: -f1)"
                if allowlisted "$file"; then
                    continue
                fi
                if is_comment_line "$line"; then
                    continue
                fi
                echo "VIOLATION: editor/ must route GPU access through EditorBridge -- forbidden include of ${hdr} in ${line}" >&2
                VIOLATIONS=$((VIOLATIONS+1))
            done < "$TMPHITS"
        fi
    done
done

if [ "$VIOLATIONS" -gt 0 ]; then
    echo "" >&2
    echo "scripts/check-include-firewall.sh: ${VIOLATIONS} violation(s)" >&2
    echo "Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md section 12" >&2
    exit 1
fi
echo "scripts/check-include-firewall.sh: clean (scope: ${SCOPE_DIRS}; editor header-only: ${EDITOR_HEADER_ONLY_DIRS})"
exit 0
