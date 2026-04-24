#!/bin/sh
# scripts/check-asset-scale-callers.sh
#
# Enforces: nobody computes pixel offsets as
# "<unitConst> / <texturePtr>->width|height"
# for CPU blit callsites outside the AssetScale subsystem.
# Such patterns are the bug class fixed by the AssetScale rework.
#
# Spec: docs/superpowers/specs/2026-04-23-asset-scale-aware-rendering-design.md
#
# NOTE: UV draws against the fixed s_textureMemory dest atlas are exempt —
# they index the destination, not the upscaled source. Only flag patterns
# that read from s_MechTextures / s_VehicleTextures / similar source atlases.

set -e
violations=0

# Pattern 1: s_MechTextures / s_VehicleTextures source-pixel arithmetic
# outside the refactored nominalToActualRect path.
hits=$(grep -rEn 's_(Mech|Vehicle|pilot)Textures?->width[[:space:]]*\*' \
    code/ mclib/ GameOS/ \
    --include='*.cpp' --include='*.h' \
    | grep -v 'asset_scale\.' \
    | grep -v 'nominalToActualRect' \
    | grep -Ev ':[[:space:]]*//' || true)
if [ -n "$hits" ]; then
    echo "[INVARIANT] raw source-texture width arithmetic (blit stride) outside AssetScale:"
    echo "$hits"
    violations=1
fi

# Pattern 2: pSrcRow advance by source texture width — the classic blit
# stride pattern that indicates CPU blit without scale awareness.
hits=$(grep -rEn 'pSrcRow[[:space:]]*\+=[[:space:]]*s_(Mech|Vehicle)Textures->width' \
    code/ mclib/ GameOS/ \
    --include='*.cpp' --include='*.h' \
    | grep -v 'asset_scale\.' \
    | grep -Ev ':[[:space:]]*//' || true)
if [ -n "$hits" ]; then
    echo "[INVARIANT] pSrcRow += source->width outside AssetScale blit:"
    echo "$hits"
    violations=1
fi

if [ "$violations" -ne 0 ]; then
    echo "FAIL: see above — route through AssetScale::nominalToActualRect for scale-aware blits"
    exit 1
fi
echo "OK"
exit 0
