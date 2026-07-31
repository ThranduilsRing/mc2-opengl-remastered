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
#     shaders/gos_terrain.frag at the audited water/shoreline sites.
#     (SHORELINE-DISPOSITION-1: this line used to say "the three audited
#     sites". There are TWO -- gos_terrain.frag:1103 and :1156. The stale
#     count is corrected rather than left standing; the census itself only
#     ever enforced the FILE, never the number, so nothing behavioural
#     changes here.)
#   rc_gbuffer1_legacyDebugSentinelScreenShadowEligible appears only in
#     shaders/static_prop.frag.
#
# And the MRT_ENABLED injection-site census (Census 4, SHORELINE-DISPOSITION-1):
#
#   `#define MRT_ENABLED` is injected from exactly ONE C++ site.
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

# ----- Census 4: MRT_ENABLED is injected from exactly ONE C++ site ----------
#
# WHY THIS IS A CONTRACT (SHORELINE-DISPOSITION-1, 2026-07-31):
#
#   gos_terrain.frag's only GBuffer1 writes -- the two
#   rc_gbuffer1_legacyTerrainMaterialAlpha sites guarded by Census 2 -- sit
#   inside `#ifdef MRT_ENABLED`. That macro is NOT defined in any shader; it is
#   appended to the shader prologue by C++ string concatenation. So a reader of
#   the shader alone cannot tell whether those two lines are live code or dead
#   code, and the shader compiles, deploys and passes reflection goldens either
#   way. That invisibility already produced one wrong conclusion in this repo.
#
#   Today exactly ONE site injects it -- gameos_graphics.cpp
#   getMaterialVariation(), which feeds only gosRenderMaterial::load(). The one
#   gos_terrain program built that way is terrain_material_, and it has had ZERO
#   bind sites since TERRAIN-DEADCODE-DELETE-1 (75b8ebefe) removed
#   terrainDrawIndexedPatches: it is created, pushed to materialList_, and
#   destroyed, but never apply()'d. Therefore gos_terrain.frag's continuous
#   `materialAlpha` cannot reach a framebuffer in ANY configuration -- including
#   MC2_TERRAIN_LOD_CHUNK=0, which is NOT an escape hatch for it.
#
#   That is load-bearing well beyond this file: it is the reason the legacy
#   screen-space runShoreline() pass is classified DEAD rather than merely
#   default-suppressed. shaders/shoreline.frag:22 (and its Vulkan twin
#   shaders/vulkan/shoreline.frag:102) classify a pixel as "water" by
#   GBuffer1.a in (0.15, 0.35), and the continuous materialAlpha is the only
#   producer that was ever water-correlated in that band. A SECOND injection
#   site -- or the same macro reaching a program that actually draws -- would
#   silently revive it and invalidate that reasoning without editing one shader
#   file. Fail loudly instead of re-deriving this by hand a third time.
#
# NOTE ON SCOPE: this counts INJECTION sites, not uses and not mentions.
#   * Adding `#ifdef MRT_ENABLED` to another shader is fine -- not matched.
#   * PROSE about the macro is fine -- the pattern requires either a real
#     preprocessor directive at start-of-line, or the macro text opening a C++
#     string literal (the `"#define MRT_ENABLED 1\n"` append form that is how
#     this is actually injected). Discussing `#define MRT_ENABLED` in a comment
#     with backticks or single quotes does NOT trip it. That distinction is
#     load-bearing: the first draft of this census counted its own explanatory
#     comments and failed, which is exactly the false-positive class that gets
#     a check disabled rather than fixed.

mrt_inject_sites=$(
    grep -rn --include=*.cpp --include=*.h -E \
        '(^[[:space:]]*#define[[:space:]]+MRT_ENABLED|"#define[[:space:]]+MRT_ENABLED)' \
        "$ROOT/GameOS" "$ROOT/RenderCore" "$ROOT/mclib" "$ROOT/code" 2>/dev/null \
        || true
)
mrt_inject_count=$(printf '%s' "$mrt_inject_sites" | grep -c . )

if [ "$mrt_inject_count" -ne 1 ]; then
    echo "FAIL: expected exactly 1 C++ injection site for '#define MRT_ENABLED';" >&2
    echo "      found $mrt_inject_count." >&2
    echo "" >&2
    echo "      MRT_ENABLED gates gos_terrain.frag's continuous materialAlpha write" >&2
    echo "      to GBuffer1 -- the only producer ever correlated with water inside" >&2
    echo "      the (0.15, 0.35) band that shaders/shoreline.frag:22 and" >&2
    echo "      shaders/vulkan/shoreline.frag:102 classify as water." >&2
    echo "" >&2
    echo "      If you ADDED a site: confirm whether the program it builds is ever" >&2
    echo "      bound and drawn. If it is, the legacy terrain materialAlpha is live" >&2
    echo "      again and every 'the shoreline pass cannot reach water' claim in" >&2
    echo "      the tree must be re-derived before you update this count." >&2
    echo "      If you REMOVED the last site: those shader lines are now dead in" >&2
    echo "      the preprocessor too, and Census 2 above should be revisited." >&2
    echo "" >&2
    echo "$mrt_inject_sites" >&2
    echo "" >&2
    failures=$((failures + 1))
fi

if [ "$failures" -gt 0 ]; then
    echo "Render contract grep census FAILED ($failures violation class(es))." >&2
    exit 1
fi

echo "Render contract grep census PASSED."
exit 0
