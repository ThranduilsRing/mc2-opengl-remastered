#!/bin/sh
# check-render-contract-gbuffer1.sh
#
# Render Contract Registry — phase-1 grep census (commit 8 of the impl plan).
#
# Enforces the invariant from the spec exit criteria:
#
#   Fail if any production fragment shader assigns GBuffer1 directly from
#   vec4(...) instead of an rc_* helper. The script ignores comments and
#   excludes shaders/include/render_contract.hglsl helper definitions.
#
# Also enforces the legacy escape-hatch census (spec exit criterion 10):
#
#   rc_gbuffer1_legacyTerrainMaterialAlpha appears only in
#     shaders/gos_terrain.frag at the three audited water/shoreline sites.
#   rc_gbuffer1_legacyDebugSentinelScreenShadowEligible appears only in
#     shaders/static_prop.frag.
#
# Spec: docs/superpowers/specs/2026-04-26-render-contract-registry-design.md

set -u

# Locate worktree root from the script's location.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SHADERS_DIR="$ROOT/shaders"

if [ ! -d "$SHADERS_DIR" ]; then
    echo "FAIL: cannot locate shaders directory at $SHADERS_DIR" >&2
    exit 2
fi

failures=0

# ----- Census 1: no raw GBuffer1 = vec4(...) outside the helper file --------
#
# Strategy:
#   - find lines matching `GBuffer1<whitespace>=<whitespace>vec4(`
#   - strip everything after `//` so commented-out matches are ignored
#   - exclude the helper definition file itself
#   - report any survivor as a violation

raw_violations=$(
    grep -nE '^[^/]*GBuffer1[[:space:]]*=[[:space:]]*vec4[[:space:]]*\(' \
        "$SHADERS_DIR"/*.frag 2>/dev/null \
        | grep -v 'render_contract.hglsl' \
        || true
)

if [ -n "$raw_violations" ]; then
    echo "FAIL: raw GBuffer1 = vec4(...) writes detected; route through rc_* helpers" >&2
    echo "      defined in shaders/include/render_contract.hglsl." >&2
    echo "" >&2
    echo "$raw_violations" >&2
    echo "" >&2
    failures=$((failures + 1))
fi

# ----- Census 2: legacy material-alpha helper is gos_terrain.frag-only ------

legacy_water_uses=$(
    grep -nE 'rc_gbuffer1_legacyTerrainMaterialAlpha' \
        "$SHADERS_DIR"/*.frag "$SHADERS_DIR"/include/*.hglsl 2>/dev/null \
        | grep -vE '/(gos_terrain\.frag|render_contract\.hglsl):' \
        || true
)

if [ -n "$legacy_water_uses" ]; then
    echo "FAIL: rc_gbuffer1_legacyTerrainMaterialAlpha used outside gos_terrain.frag" >&2
    echo "      The legacy water/shoreline escape hatch is single-shader-only." >&2
    echo "      See spec §3.1 (CONTRACT VIOLATION) and follow-up F1." >&2
    echo "" >&2
    echo "$legacy_water_uses" >&2
    echo "" >&2
    failures=$((failures + 1))
fi

# ----- Census 3: legacy debug sentinel helper is static_prop.frag-only ------

legacy_debug_uses=$(
    grep -nE 'rc_gbuffer1_legacyDebugSentinelScreenShadowEligible' \
        "$SHADERS_DIR"/*.frag "$SHADERS_DIR"/include/*.hglsl 2>/dev/null \
        | grep -vE '/(static_prop\.frag|render_contract\.hglsl):' \
        || true
)

if [ -n "$legacy_debug_uses" ]; then
    echo "FAIL: rc_gbuffer1_legacyDebugSentinelScreenShadowEligible used outside static_prop.frag" >&2
    echo "      The debug-sentinel helper is single-shader-only." >&2
    echo "" >&2
    echo "$legacy_debug_uses" >&2
    echo "" >&2
    failures=$((failures + 1))
fi

if [ "$failures" -gt 0 ]; then
    echo "Render contract grep census FAILED ($failures violation class(es))." >&2
    exit 1
fi

echo "Render contract grep census PASSED."
exit 0
