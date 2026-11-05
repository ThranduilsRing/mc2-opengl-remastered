#!/usr/bin/env bash
# scripts/box_contracts.sh -- the Linux-capable contract rows, run on the box via `box_run.py "sh:bash scripts/box_contracts.sh"`.
# Rows call `py -3`; the box has only python3, so a shim named `py` goes first on PATH. Rows listed in
# scripts/box_contracts_pc_only.txt (one label per line) need Windows and are skipped here; merge_gate.py runs exactly
# those on the PC (--rows) and, when the box is down, the rest as well. Extra args go to check-contracts.sh.
# Builds and tools use cores 32-63 at nice 19 (lane rules section 5).
set -euo pipefail
cd "$(dirname "$0")/.."
shim="$(mktemp -d)"
trap 'rm -rf "$shim"' EXIT
printf '#!/bin/sh\n[ "$1" = -3 ] && shift\nexec python3 "$@"\n' > "$shim/py"
chmod +x "$shim/py"
export PATH="$shim:$PATH"
skip="$(grep -v '^[[:space:]]*\(#\|$\)' scripts/box_contracts_pc_only.txt | paste -sd'|' || true)"
args=(-j16 --no-cache)
[ -n "$skip" ] && args+=(--skip-rows "^($skip)\$")
nice -n 19 taskset -c 32-63 bash scripts/check-contracts.sh "${args[@]}" "$@"
