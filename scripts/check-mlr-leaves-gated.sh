#!/usr/bin/env bash
# check-mlr-leaves-gated.sh — invariant CI script per plan v6 §2.3.
#
# Verifies that the remaining MLR work-leaf in mclib/mlr/mlrclipper.cpp has
# MC2_GOSFX_GATE_EARLY_RETURN() near the top of their bodies. A1 wired
# these as the first env-gate that converts gosFX rendering into a no-op
# under MC2_DISABLE_GOSFX=1. Removing any of these gates silently brings
# gosFX particles back without changing default — this script trips loud.
#
# A4 deletes mclib/mlr/ entirely, so this script self-retires (its scan
# range disappears). Until then it lives as a tier-1 invariant.

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ROOT="$( cd "$SCRIPT_DIR/.." && pwd )"
TARGET="$ROOT/mclib/mlr/mlrclipper.cpp"

if [ ! -f "$TARGET" ]; then
    echo "[mlr-leaves-gated] $TARGET not found (A4 deleted? script self-retires.)"
    exit 0
fi

LEAVES=("DrawEffect")
FAIL=0

for retired in DrawShape DrawScalableShape; do
    if grep -q "MLRClipper::${retired} " "$TARGET"; then
        echo "[mlr-leaves-gated] FAIL: retired ${retired} CPU route restored"
        FAIL=1
    fi
done

for leaf in "${LEAVES[@]}"; do
    # Find the function definition line; then check the next ~20 lines for
    # the gate macro.
    line=$(grep -n "MLRClipper::${leaf} " "$TARGET" | head -1 | cut -d: -f1)
    if [ -z "$line" ]; then
        echo "[mlr-leaves-gated] FAIL: could not find MLRClipper::${leaf} definition"
        FAIL=1
        continue
    fi
    end=$((line + 25))
    if ! sed -n "${line},${end}p" "$TARGET" | grep -q "MC2_GOSFX_GATE_EARLY_RETURN"; then
        echo "[mlr-leaves-gated] FAIL: MLRClipper::${leaf} (line ${line}) missing MC2_GOSFX_GATE_EARLY_RETURN within 25 lines"
        FAIL=1
    fi
done

if [ $FAIL -ne 0 ]; then
    echo "[mlr-leaves-gated] FAIL — one or more gates missing. See plan v6 §2.3."
    exit 1
fi

echo "[mlr-leaves-gated] OK — DrawEffect gated; mesh CPU leaves retired"
exit 0
