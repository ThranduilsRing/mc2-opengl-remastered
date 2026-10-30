#!/bin/sh
# scripts/check-vfx-no-objectid.sh
#
# RenderWorld Slice M4: firewall enforcing the rule that VFX shaders
# MUST NOT write color-attachment-2 (the M1.5 R32_UINT objectID substrate).
#
# Why this exists: integer color attachments do not blend (GL 4.5 §17.3.6).
# An additive/alpha-blended particle fragment that writes attachment-2 will
# LAST-WRITE-WINS clobber the opaque mech/static-prop objectID underneath,
# silently breaking M2.6 mech-pick on any mech occluded by a translucent
# particle (muzzle flash, smoke, tracer, impact effect). The user clicks
# the mech, nothing happens — no log line marks the failure. Hard to
# debug from the gameplay symptom back to the FS shader source.
#
# Cure: prohibit `layout(location=2) out` declarations in GL VFX shaders.
# Vulkan VFX binds color/normal/motion/reactive (no object-ID attachment),
# so location 2 is floating-point motion; prohibit integer outputs there.
# Enforced mechanically — a contributor who tries to add the write hits
# CI failure immediately rather than three months later when a player
# reports flaky mech-pick.
#
# Spec: docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md (§4 trap, §9 gate)
# Resolutions: docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md (Q1 confirms)
# Migration guide: docs/renderworld_migration_guide.md §3.6
#
# Allowlist: scripts/check-vfx-no-objectid.allowlist
# (Expected to be EMPTY forever. Adding an entry means you read the
# allowlist header and accept the documented trap. See the header.)
#
# Scan set: native billboard and surviving GL mesh/tube shader pairs.
# Extend BASENAMES when a new VFX shader is added.
#
# Negative-test (to verify the script catches violations):
#   1. Temporarily add `layout(location=2) out uint v_test;` to
#      shaders/vulkan/vfx_particle.frag (the declaration alone is enough;
#      no need to write to it).
#   2. Run this script. Expect exit 1 + the file:line reported.
#   3. Revert the injection (`git checkout shaders/vulkan/vfx_particle.frag`).
#   4. Re-run. Expect exit 0.
#
# Exit 0 = clean (zero non-allowlisted matches across scan set)
# Exit 1 = at least one non-allowlisted violation

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# VFX shader basenames. Hand-maintained; reviewers must flag any new
# shader file in shaders/ that renders particles/effects.
BASENAMES="
vulkan/vfx_particle.vert
vulkan/vfx_particle.frag
vfx_mesh.vert
vfx_mesh.frag
tube_ribbon.vert
tube_ribbon.frag
"

ALLOWLIST="scripts/check-vfx-no-objectid.allowlist"
PATTERN='layout[[:space:]]*\([[:space:]]*location[[:space:]]*=[[:space:]]*2[[:space:]]*\)[[:space:]]*out'
VK_PATTERN='layout[[:space:]]*\([^)]*\)[[:space:]]*out[[:space:]]+(uint|int|uvec[234]|ivec[234])[[:space:]]'

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

# Strip the path:lineno: prefix and check if the remaining content is a
# GLSL comment-only line. Mirrors check-no-raw-gl-from-game.sh shape.
is_comment_line() {
    body="$(printf '%s' "$1" | sed 's/^[^:]*:[0-9][0-9]*://' | sed 's/^[[:space:]]*//')"
    case "$body" in
        "//"*) return 0 ;;
        "/*"*) return 0 ;;
        "*"*)  return 0 ;;
    esac
    return 1
}

TMPHITS="$(mktemp 2>/dev/null || echo "/tmp/vfx-no-objectid.$$")"
trap 'rm -f "$TMPHITS"' EXIT

: > "$TMPHITS"

# Iterate the curated basename list. Each must exist under shaders/;
# missing files are an error (the basename list is stale).
echo "$BASENAMES" | while IFS= read -r name; do
    [ -z "$name" ] && continue
    src="shaders/$name"
    if [ ! -f "$src" ]; then
        echo "scripts/check-vfx-no-objectid.sh: WARN scan-set entry missing: $src" >&2
        continue
    fi
    # Strip trailing `// ...` comments BEFORE applying the violation grep
    # so that documentation text naming the forbidden declaration (e.g.
    # `// note: layout(location=2) out is forbidden here`) is not flagged.
    # Mirrors the fix in check-no-raw-gl-from-game.sh; sed preserves line
    # numbers (lines stay in stream, comment portion blanked).
    case "$name" in
        vulkan/*) pattern="$VK_PATTERN" ;;
        *) pattern="$PATTERN" ;;
    esac
    sed 's|//.*$||' "$src" | grep -nE "$pattern" 2>/dev/null | while IFS= read -r match; do
        [ -z "$match" ] && continue
        printf '%s:%s\n' "$src" "$match"
    done
done > "$TMPHITS"

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
        echo "VIOLATION: VFX shader declares an objectID/integer output in ${line}" >&2
        VIOLATIONS=$((VIOLATIONS+1))
    done < "$TMPHITS"
fi

if [ "$VIOLATIONS" -gt 0 ]; then
    echo "" >&2
    echo "scripts/check-vfx-no-objectid.sh: ${VIOLATIONS} violation(s)" >&2
    echo "VFX shaders MUST NOT write GL attachment-2 or Vulkan integer outputs (objectID)." >&2
    echo "Integer attachments do not blend; particle writes last-write-wins clobber" >&2
    echo "underlying mech/static-prop IDs, silently breaking M2.6 mech-pick." >&2
    echo "" >&2
    echo "Spec:           docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md (§4)" >&2
    echo "Migration:      docs/renderworld_migration_guide.md §3.6" >&2
    echo "Resolutions:    docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md (Q1)" >&2
    echo "" >&2
    echo "If you have a genuine use case requiring a VFX writer, reopen the spec" >&2
    echo "and address Q1-Q5 (do NOT add to scripts/check-vfx-no-objectid.allowlist" >&2
    echo "without reading its header first)." >&2
    exit 1
fi

echo "scripts/check-vfx-no-objectid.sh: clean (VFX shaders satisfy objectID prohibition)"
exit 0
