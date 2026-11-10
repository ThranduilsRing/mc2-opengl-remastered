#!/usr/bin/env bash
# scripts/box_contracts.sh -- the Linux-capable contract rows, run on the box via `box_run.py "sh:bash scripts/box_contracts.sh"`.
# Rows call `py -3`; the box has only python3, so a shim named `py` goes first on PATH. Rows listed in
# scripts/box_contracts_pc_only.txt (one label per line) need Windows and are skipped here; merge_gate.py runs exactly
# those on the PC (--rows) and, when the box is down, the rest as well. Extra args go to check-contracts.sh.
# Builds and tools use BUILD_CPUS of locks/cores.env at nice 19 (lane rules section 5).
set -euo pipefail
cd "$(dirname "$0")/.."
shim="$(mktemp -d)"
trap 'rm -rf "$shim"' EXIT
printf '#!/bin/sh\n[ "$1" = -3 ] && shift\nexec python3 "$@"\n' > "$shim/py"
chmod +x "$shim/py"
export PATH="$shim:$PATH"
skip="$(grep -v '^[[:space:]]*\(#\|$\)' scripts/box_contracts_pc_only.txt | paste -sd'|' || true)"
[ -f /workspace/locks/cores.env ] && . /workspace/locks/cores.env
BC=${BUILD_CPUS:-8-$(( $(nproc) - 1 ))}; BJ=0; for r in ${BC//,/ }; do BJ=$((BJ + ${r#*-} - ${r%-*} + 1)); done
args=(-j$BJ --no-cache)
[ -n "$skip" ] && args+=(--skip-rows "^($skip)\$")
nice -n 19 taskset -c "$BC" bash scripts/check-contracts.sh "${args[@]}" "$@"
