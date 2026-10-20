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
# Census 1 has TWO halves (VK-GBUFFER1-CENSUS-1, 2026-07-31):
#
#   1a  GL  -- shaders/*.frag. ZERO-TOLERANCE, no allowlist. Unchanged.
#   1b  VK  -- shaders/vulkan/*.frag. Allowlisted; see
#              scripts/check-render-contract-gbuffer1.allowlist.
#
#   WHY 1b EXISTS: Census 1 globbed only "$SHADERS_DIR"/*.frag, which does not
#   descend into shaders/vulkan/. Six Vulkan fragment shaders write attachment 1
#   with a raw vec4 and were entirely outside the census. That blind spot was not
#   theoretical -- shaders/vulkan/color_prop_tier1.frag emitted a FLAT 0.25 for
#   every static prop, while the GL producer static_prop.frag:657-661 emits
#   0.1875 for the alpha-test lattice class (trees, foliage, chain-link). The VK
#   prop island genuinely draws that class -- its own FS applies the GL 0.5
#   alpha-test discard -- so the flat value misclassified every VK-drawn tree.
#   Nothing could have caught it, because nothing was looking.
#
#   WHY 1b IS SEPARATE CODE RATHER THAN A WIDER GLOB: keeping 1a's grep
#   byte-identical is the cheapest possible proof that extending the census did
#   not narrow it. A single unified pattern would have to be re-argued against
#   every GL shader; two halves cost a few lines and cannot regress the GL side.
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

LIST_VK=0
if [ "${1:-}" = "--list-vk" ]; then
    LIST_VK=1
fi

# ----- Census 1a: no raw GBuffer1 = vec4(...) outside the helper file -------
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

# ----- Census 1b: shaders/vulkan/ attachment-1 writers (allowlisted) --------
#
# Strategy -- deliberately NOT a grep for the literal name "GBuffer1":
#   for each shaders/vulkan/*.frag
#     1. strip `//` comments (line-preserving) so prose about a write is not a write
#     2. DISCOVER the attachment-1 output's name from the file's own
#        `layout(location = 1) out <type> <name>;` declaration
#     3. census every assignment to that name
#     4. a write whose RHS is an rc_* helper call passes with no allowlist entry;
#        anything else must be keyed in the allowlist
#
# Step 2 is the load-bearing one. The VK shaders do NOT agree on a name --
# oGBuffer1 (prop, mech), GBuffer1 (terrain, skirt, water), o_normal (vfx) -- so a
# name grep would have silently skipped vfx_particle.frag, and any future shader
# picking a fresh name would be invisible for free. Reading the declaration means
# the census cannot be evaded by renaming, only by removing the output.
#
# KNOWN LIMITS (stated so nobody assumes more than is delivered):
#   * one write per source line (all current sites comply; the eight terrain CSM
#     debug early-outs are one write each on their own line);
#   * `/* ... */` block comments are not stripped -- no VK shader uses them for
#     this text today, and adding one would show up as a NEW violation, which is
#     the safe direction to fail;
#   * the allowlist pins the VALUE and the SITE, never that the value is correct.

VK_DIR="$SHADERS_DIR/vulkan"
VK_ALLOWLIST="$SCRIPT_DIR/check-render-contract-gbuffer1.allowlist"

vk_hits_file="$(mktemp 2>/dev/null || echo "/tmp/gb1-vk-hits.$$")"
vk_keys_file="$(mktemp 2>/dev/null || echo "/tmp/gb1-vk-keys.$$")"
vk_allow_file="$(mktemp 2>/dev/null || echo "/tmp/gb1-vk-allow.$$")"
trap 'rm -f "$vk_hits_file" "$vk_keys_file" "$vk_allow_file"' EXIT
: > "$vk_hits_file"

vk_files_scanned=0

if [ -d "$VK_DIR" ]; then
    for src in "$VK_DIR"/*.frag; do
        [ -f "$src" ] || continue
        vk_files_scanned=$((vk_files_scanned + 1))
        rel="shaders/vulkan/$(basename "$src")"

        # 1. comment-stripped view of the file (line numbers preserved).
        stripped="$(sed 's|//.*$||' "$src")"

        # 2. discover the attachment-1 output name from the shader's own decl.
        outname="$(printf '%s\n' "$stripped" | sed -n \
            's/.*location[[:space:]]*=[[:space:]]*1[[:space:]]*)[[:space:]]*out[[:space:]].*[[:space:]]\([A-Za-z_][A-Za-z0-9_]*\)[[:space:]]*;.*/\1/p' \
            | head -n 1)"
        [ -n "$outname" ] || continue   # shader has no attachment-1 output

        # 3. census assignments to it (skip `==` comparisons via the [^=] guard).
        printf '%s\n' "$stripped" \
            | sed -n "s/.*\(\b${outname}[[:space:]]*=[^=;][^;]*;\).*/\1/p" \
            | sed 's/[[:space:]][[:space:]]*/ /g; s/^ //; s/ $//' \
            | while IFS= read -r expr; do
                [ -n "$expr" ] || continue
                # 4. an rc_* helper call is contract-routed; no entry needed.
                if printf '%s' "$expr" | grep -qE '=[[:space:]]*rc_[A-Za-z0-9_]*[[:space:]]*\('; then
                    continue
                fi
                printf '%s::%s\n' "$rel" "$expr"
            done >> "$vk_hits_file"
    done
fi

sort -u "$vk_hits_file" > "$vk_keys_file"

# Allowlist keys, comments + blank lines stripped.
if [ -f "$VK_ALLOWLIST" ]; then
    grep -vE '^[[:space:]]*(#|$)' "$VK_ALLOWLIST" | sed 's/[[:space:]]*$//' | sort -u > "$vk_allow_file"
else
    : > "$vk_allow_file"
fi

if [ "$LIST_VK" -eq 1 ]; then
    echo "VK attachment-1 write census (${vk_files_scanned} shaders/vulkan/*.frag scanned):"
    if [ -s "$vk_keys_file" ]; then cat "$vk_keys_file"; else echo "  (none)"; fi
    exit 0
fi

# --- 1b self-check: a blind scanner reports a clean tree. Prove it can see. ---
#
# WITNESS/DECOY. shaders/vulkan/color_prop_tier1.frag carries, in its header
# comment and clearly labelled, a complete semicolon-terminated attachment-1 write
# (`oGBuffer1 = vec4(0.0, 0.0, 0.0, 0.9);`) that is NOT real code and NOT
# allowlisted. It has exactly ONE real write. So the file must census to EXACTLY 1.
# Two means the comment stripper stopped working and prose is being counted as code
# -- the exact false-positive class that gets a check disabled instead of fixed,
# and one a sibling lane's first draft shipped. Zero means the write pattern no
# longer matches live code and the census has gone blind.
#
# The decoy is DELIBERATELY PLANTED rather than borrowed from existing prose. The
# first draft of this witness pointed at water_geom.frag:13-14, which does quote a
# GBuffer1 write in a comment -- but without a trailing `;`, so it could never have
# matched the write pattern whether stripped or not. That witness passed while
# proving nothing, and a probe run with the stripper deliberately disabled still
# reported the tree clean. A witness must be tested by breaking the thing it
# watches; this one was, and it fails as designed.
if [ "$vk_files_scanned" -eq 0 ]; then
    echo "FAIL: Census 1b scanned ZERO shaders under $VK_DIR." >&2
    echo "      The Vulkan census is blind, not the tree clean." >&2
    failures=$((failures + 1))
else
    witness_n=$(grep -c '^shaders/vulkan/color_prop_tier1\.frag::' "$vk_hits_file" || true)
    if [ "$witness_n" -ne 1 ]; then
        echo "FAIL: Census 1b comment-stripper witness: color_prop_tier1.frag yielded" >&2
        echo "      $witness_n attachment-1 write(s), expected exactly 1." >&2
        if [ "$witness_n" -gt 1 ]; then
            echo "      >1 means COMMENTS are being censused as code (that file plants a" >&2
            echo "      labelled decoy write in its header comment, on purpose)." >&2
        else
            echo "      0 means the write pattern no longer matches live code, or the" >&2
            echo "      attachment-1 output declaration was not discovered." >&2
        fi
        failures=$((failures + 1))
    fi
fi

# --- 1b forward ratchet: no un-allowlisted raw writes. ---
vk_new=$(comm -23 "$vk_keys_file" "$vk_allow_file")
if [ -n "$vk_new" ]; then
    echo "FAIL: raw attachment-1 writes in shaders/vulkan/ that are not in the" >&2
    echo "      Census 1b allowlist:" >&2
    echo "" >&2
    echo "$vk_new" >&2
    echo "" >&2
    echo "      VK shaders cannot include shaders/include/render_contract.hglsl, so" >&2
    echo "      the house convention is to INLINE the helper's literal value. That is" >&2
    echo "      allowed -- but the inlined constant must be registered, with the GL" >&2
    echo "      helper and file:line it was compared against, in:" >&2
    echo "        scripts/check-render-contract-gbuffer1.allowlist" >&2
    echo "" >&2
    echo "      Read that file's header first. If you are here because you EDITED an" >&2
    echo "      already-listed write, the alpha constant is part of the key on purpose:" >&2
    echo "      changing a shadow class is exactly the change that must not pass" >&2
    echo "      unreviewed. Re-adjudicate against GL, then update the key AND its" >&2
    echo "      reason. Do not just paste the new line in to get green." >&2
    echo "" >&2
    failures=$((failures + 1))
fi

# --- 1b reverse ratchet: no stale allowlist entries. ---
vk_stale=$(comm -13 "$vk_keys_file" "$vk_allow_file")
if [ -n "$vk_stale" ]; then
    echo "FAIL: stale Census 1b allowlist entries -- these expressions no longer" >&2
    echo "      appear anywhere under shaders/vulkan/:" >&2
    echo "" >&2
    echo "$vk_stale" >&2
    echo "" >&2
    echo "      An exemption for a write that no longer exists is a false record of" >&2
    echo "      what the tree does, and it silently pre-approves the write's return." >&2
    echo "      Delete the entry AND its reason comment." >&2
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

# The GL compiler and its last legacy terrain program are retired. Any revived
# MRT_ENABLED injection would reintroduce a dead backend path.
if [ "$mrt_inject_count" -ne 0 ]; then
    echo "FAIL: retired GL MRT_ENABLED injection returned ($mrt_inject_count site(s))." >&2
    echo "$mrt_inject_sites" >&2
    failures=$((failures + 1))
fi

if [ "$failures" -gt 0 ]; then
    echo "Render contract grep census FAILED ($failures violation class(es))." >&2
    exit 1
fi

echo "Render contract grep census PASSED."
exit 0
