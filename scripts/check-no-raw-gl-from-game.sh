#!/bin/sh
# scripts/check-no-raw-gl-from-game.sh
#
# RenderWorld Slice M6: function-level firewall enforcing the rule that
# game-side code (code/, mclib/) MUST NOT call raw OpenGL functions
# (gl*()). Rendering routes through engine abstractions
# (gos_*, MeshRenderer, MaterialSystem, RenderWorld, GameAdapters).
#
# Spec: docs/superpowers/specs/2026-05-24-renderworld-slice-m6-firewall-audit-spec.md
# Recon: docs/superpowers/explorations/2026-05-23-renderworld-slice-m6-firewall-audit-recon.md
#
# Orthogonal to scripts/check-include-firewall.sh:
#   - that script answers "is RenderWorld pure?" (include-level)
#   - this script answers  "is game-side abstracted?" (function-call-level)
#
# Pattern: \bgl[A-Z][a-zA-Z]+\s*\(  -- the function-call anchor (\s*\()
# eliminates known false positives like glX/glY field names,
# glTextureId (mc2video struct field), and glTexId (local variable).
#
# Allowlist: scripts/check-no-raw-gl-from-game.allowlist
#
# Negative-test (to verify the script catches violations): manually inject
# `glClear(GL_COLOR_BUFFER_BIT);` into any code/ file, run script, expect
# non-zero exit + the file/line reported; then revert.
#
# Exit 0  = clean
# Exit 1  = at least one non-allowlisted violation

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SCOPE_DIRS="code mclib"
ALLOWLIST="scripts/check-no-raw-gl-from-game.allowlist"
PATTERN='\bgl[A-Z][a-zA-Z]+[[:space:]]*\('

VIOLATIONS=0

allowlisted() {
    # $1 = path
    [ -f "$ALLOWLIST" ] || return 1
    while IFS= read -r _aline; do
        case "$_aline" in
            ""|"#"*) continue ;;
        esac
        if [ "$1" = "$_aline" ]; then
            return 0
        fi
    done < "$ALLOWLIST"
    return 1
}

# Strip the path:lineno: prefix and check if the remaining content is
# a C/C++ comment-only line. Mirrors check-include-firewall.sh.
is_comment_line() {
    # $1 = full grep "path:lineno:content" line
    body="$(printf '%s' "$1" | sed 's/^[^:]*:[0-9][0-9]*://' | sed 's/^[[:space:]]*//')"
    case "$body" in
        "//"*) return 0 ;;
        "/*"*) return 0 ;;
        "*"*)  return 0 ;;
    esac
    return 1
}

TMPHITS="$(mktemp 2>/dev/null || echo "/tmp/no-raw-gl.$$")"
TMPFILES="$(mktemp 2>/dev/null || echo "/tmp/no-raw-gl-files.$$")"
trap 'rm -f "$TMPHITS" "$TMPFILES"' EXIT

# Enumerate game-side files via git ls-files to avoid build artifacts.
: > "$TMPFILES"
for dir in $SCOPE_DIRS; do
    if [ ! -d "$dir" ]; then continue; fi
    git ls-files "$dir" >> "$TMPFILES" 2>/dev/null || true
done

if [ ! -s "$TMPFILES" ]; then
    echo "scripts/check-no-raw-gl-from-game.sh: no files enumerated (git ls-files empty?)" >&2
    exit 1
fi

# Filter to C/C++ sources + headers.
SOURCES="$(grep -E '\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl)$' "$TMPFILES" || true)"

if [ -z "$SOURCES" ]; then
    echo "scripts/check-no-raw-gl-from-game.sh: clean (scope: ${SCOPE_DIRS}; 0 source files)"
    exit 0
fi

: > "$TMPHITS"
# PERF (2026-10-09): was a per-file `sed | grep | while read` shell loop (one
# process chain per source file, plus an allowlist re-read per hit). Now ONE awk
# pass over all files. Same semantics, applied in the same order:
#   1. blank everything from `//` to EOL (line numbers preserved)
#   2. keep lines matching PATTERN (\b -> start-or-nonword; awk has no \b)
#   3. drop hits in allowlisted files (exact path match, `#`/blank ignored)
#   4. drop comment-looking lines (body starts with `//`, `/*` or `*` after
#      leading whitespace) -- the former is_comment_line()
# Survivors are emitted as `path:lineno:content` for the reporting loop below.
# Unreadable / missing files are skipped, as the old `[ -f ]` guard did.
echo "$SOURCES" | tr '\n' '\0' | xargs -0 awk -v allow="$ALLOWLIST" '
    BEGIN {
        if (allow != "") while ((getline a < allow) > 0) { sub(/\r$/, "", a); if (a != "" && substr(a,1,1) != "#") ok[a] = 1 }
    }
    FNR == 1 { skip = (FILENAME in ok) }
    skip { nextfile }
    {
        line = $0
        sub(/\/\/.*$/, "", line)
        if (match(line, /(^|[^[:alnum:]_])gl[A-Z][a-zA-Z]+[[:space:]]*\(/)) {
            body = line; sub(/^[[:space:]]+/, "", body)
            c = substr(body, 1, 1); c2 = substr(body, 1, 2)
            if (c2 == "//" || c2 == "/*" || c == "*") next
            printf "%s:%d:%s\n", FILENAME, FNR, line
        }
    }' > "$TMPHITS" 2>/dev/null || true

if [ -s "$TMPHITS" ]; then
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        # Extract the matched function token for a tidy message.
        matched="$(printf '%s' "$line" | grep -oE "$PATTERN" | head -1)"
        echo "VIOLATION: raw GL call ${matched} in ${line}" >&2
        VIOLATIONS=$((VIOLATIONS+1))
    done < "$TMPHITS"
fi

if [ "$VIOLATIONS" -gt 0 ]; then
    echo "" >&2
    echo "scripts/check-no-raw-gl-from-game.sh: ${VIOLATIONS} violation(s)" >&2
    echo "Spec: docs/superpowers/specs/2026-05-24-renderworld-slice-m6-firewall-audit-spec.md" >&2
    echo "Route rendering through engine API (gos_*, MeshRenderer, RenderWorld)." >&2
    echo "If the call is a diagnostic-only state query, add the TU to" >&2
    echo "scripts/check-no-raw-gl-from-game.allowlist with a justification." >&2
    exit 1
fi
echo "scripts/check-no-raw-gl-from-game.sh: clean (scope: ${SCOPE_DIRS})"
exit 0
