#!/bin/sh
# F1 unified-projection retirement gate (spec §6.2.1 + §6.2.2).
# Fails CI if legacy projection scaffolding survives Stage A.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ALLOWLIST="$SCRIPT_DIR/check-unified-projection-retirement.allowlist"

# Build a clean allowlist with comments + blank lines stripped.
# Critical: `grep -vFf` with a file containing blank lines treats the empty
# pattern as matching every line (empty string is substring of all), which
# would silently allowlist EVERYTHING. The Task 19 batch hit this bug.
ALLOWLIST_CLEAN="$(mktemp)"
trap 'rm -f "$ALLOWLIST_CLEAN"' EXIT
grep -vE '^\s*(#|$)' "$ALLOWLIST" > "$ALLOWLIST_CLEAN" || true

fail=0

# Uniform name retirement (terrainMVP renamed to u_worldToClipGL).
# Two-part check:
#  (1) Shader code referencing terrainMVP/u_terrainMVP at all (would refer to
#      a uniform that no longer exists).
#  (2) CPU code containing the string literals "terrainMVP", "u_terrainMVP",
#      or "gos_SetTerrainMVP" -- real lookup/call sites of retired APIs.
# CPU identifiers (struct fields, variable names) like `terrainLocs_.terrainMVP`
# are NOT flagged: they're internal aliases for the new u_worldToClipGL location
# and renaming them is cosmetic.
echo "Checking terrainMVP retirement in shader code..."
hits=$(grep -RE "\bterrainMVP\b|\bu_terrainMVP\b" shaders 2>/dev/null || true)
hits_filtered=$(echo "$hits" | grep -vFf "$ALLOWLIST_CLEAN" 2>/dev/null || true)
# Filter line-leading comments (CPP/GLSL `//`, shell-style `#`) -- comments are doc-debt, not live code.
hits_filtered=$(echo "$hits_filtered" | grep -vE '^[^:]+:\s*(//|#)' 2>/dev/null || true)
# Filter shader comments (line-leading `//`) -- comments are doc-debt, not consumption.
hits_filtered=$(echo "$hits_filtered" | grep -vE '^[^:]+:\s*//' 2>/dev/null || true)
if [ -n "$hits_filtered" ]; then
    echo "FAIL: legacy terrainMVP scaffolding in shaders:"
    echo "$hits_filtered"
    fail=1
fi

# ---------------------------------------------------------------------------
# UNWIRED-CHECK-CLOSE-1 (2026-07-30): this subcheck FUSED TWO CLAIMS. Split.
# ---------------------------------------------------------------------------
# It used to also match `"gos_SetTerrainMVP"|gos_SetTerrainMVP\s*\(`, asserting
# the API itself no longer EXISTS. Adjudicated 2026-07-30; that half is ROT:
#
#   * The F1 spec (docs/superpowers/specs/2026-05-22-unified-projection-v2-f1-
#     atomic-design.md §5.2) did intend to delete the API, and 59fae278 did
#     delete it. It stayed deleted for TWO DAYS.
#   * 2f92e322 (05-24, editor bring-up) deliberately re-added the decl+def for
#     an EditorCamera.h shim; b09da4be (05-29, WATER-TERRAIN-REFLECTION-1)
#     added the two call sites. Both were reviewed decisions taken by people
#     who had no signal this gate existed -- because it was never wired to
#     anything. The gate has been red continuously for ~2 months.
#   * At HEAD the API is a raw float[16] setter used ONLY inside
#     RenderWaterReflectionPass(), which early-returns unless
#     MC2_WATER_REFLECTION_RT=1 (default OFF). Zero calls on the default path.
#   * Migrating those two calls to gos_SetWorldToClipGL would be an ACTIVE
#     REGRESSION: that setter bumps g_viewContentEpoch, and ComputeDispatch
#     stamps g_dispatchMvpViewEpoch from it, so a scoped mirror install/restore
#     would desynchronise the snapshot stamp and silently disable the FixB
#     z-fight fix via gos_GetObjectDrawMVP's staleness fallback. The raw
#     setter's BYPASS of publish bookkeeping is precisely what makes it correct
#     for save/install/restore.
#
# The other half -- that no CPU code looks up the RETIRED UNIFORM NAMES -- has
# held cleanly since 05-22 and is still worth guarding, so it is kept alone.
# Deliberately NOT allowlisted instead: `grep -vFf` makes an allowlist entry a
# blanket substring exemption that would also silence FUTURE call sites.
echo "Checking terrainMVP string-literal retirement in CPU code..."
hits=$(grep -RE '"terrainMVP"|"u_terrainMVP"' code GameOS mclib 2>/dev/null || true)
hits_filtered=$(echo "$hits" | grep -vFf "$ALLOWLIST_CLEAN" 2>/dev/null || true)
# Filter line-leading comments (CPP/GLSL `//`, shell-style `#`) -- comments are doc-debt, not live code.
hits_filtered=$(echo "$hits_filtered" | grep -vE '^[^:]+:\s*(//|#)' 2>/dev/null || true)
if [ -n "$hits_filtered" ]; then
    echo "FAIL: legacy terrainMVP/u_terrainMVP string-literal uniform lookups in CPU code:"
    echo "$hits_filtered"
    fail=1
fi

# abs(.w) round-trip kludge retirement.
echo "Checking abs(clip.w) kludge retirement..."
hits=$(grep -RE "abs\(\s*[a-zA-Z_]+\.w\s*\)" shaders 2>/dev/null || true)
hits_filtered=$(echo "$hits" | grep -vFf "$ALLOWLIST_CLEAN" 2>/dev/null || true)
# Filter line-leading comments (CPP/GLSL `//`, shell-style `#`) -- comments are doc-debt, not live code.
hits_filtered=$(echo "$hits_filtered" | grep -vE '^[^:]+:\s*(//|#)' 2>/dev/null || true)
if [ -n "$hits_filtered" ]; then
    echo "FAIL: abs(.w) kludge survives:"
    echo "$hits_filtered"
    fail=1
fi

# terrainViewport retirement.
echo "Checking terrainViewport retirement..."
hits=$(grep -RE "terrainViewport|u_terrainViewport" code GameOS mclib shaders 2>/dev/null || true)
hits_filtered=$(echo "$hits" | grep -vFf "$ALLOWLIST_CLEAN" 2>/dev/null || true)
# Filter line-leading comments (CPP/GLSL `//`, shell-style `#`) -- comments are doc-debt, not live code.
hits_filtered=$(echo "$hits_filtered" | grep -vE '^[^:]+:\s*(//|#)' 2>/dev/null || true)
if [ -n "$hits_filtered" ]; then
    echo "FAIL: terrainViewport survives:"
    echo "$hits_filtered"
    fail=1
fi

# Screen-rhw round-trip retirement.
echo "Checking screen-rhw round-trip retirement..."
hits=$(grep -RE "\*\s*rhw\s*[\+\-\*]" shaders 2>/dev/null || true)
hits_filtered=$(echo "$hits" | grep -vFf "$ALLOWLIST_CLEAN" 2>/dev/null || true)
# Filter line-leading comments (CPP/GLSL `//`, shell-style `#`) -- comments are doc-debt, not live code.
hits_filtered=$(echo "$hits_filtered" | grep -vE '^[^:]+:\s*(//|#)' 2>/dev/null || true)
if [ -n "$hits_filtered" ]; then
    echo "FAIL: screen-rhw round-trip survives:"
    echo "$hits_filtered"
    fail=1
fi

# Bare mvp retirement in terrain shader set.
echo "Checking bare-mvp uniform retirement (terrain set only)..."
hits=$(grep -RE "\buniform[[:space:]]+mat4[[:space:]]+u?_?mvp\b" shaders 2>/dev/null || true)
hits_filtered=$(echo "$hits" | grep -vFf "$ALLOWLIST_CLEAN" 2>/dev/null || true)
# Filter line-leading comments (CPP/GLSL `//`, shell-style `#`) -- comments are doc-debt, not live code.
hits_filtered=$(echo "$hits_filtered" | grep -vE '^[^:]+:\s*(//|#)' 2>/dev/null || true)
if [ -n "$hits_filtered" ]; then
    echo "FAIL: bare mvp uniform in non-allowlisted shader:"
    echo "$hits_filtered"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo ""
    echo "Either: (a) finish migrating the flagged sites, or (b) add to"
    echo "$ALLOWLIST with a documented reason."
    exit 1
fi

echo "PASS: unified-projection retirement complete."
