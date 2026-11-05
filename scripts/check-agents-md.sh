#!/bin/sh
# check-agents-md.sh — AGENTS.md (non-Claude agent adapter) freshness gate.
#
# AGENTS.md is a thin adapter over CLAUDE.md, not a clone. This gate catches the
# drift modes that actually bite:
#   1. AGENTS.md missing where CLAUDE.md exists (Codex gets nothing).
#   2. AGENTS.md stops pointing at CLAUDE.md.
#   3. Tools named in the AGENTS.md toolbelt no longer exist on disk.
#   4. File bloat (>120 lines = someone started cloning instead of adapting).
set -e
cd "$(dirname "$0")/.."

fail() { echo "[check-agents-md] FAIL: $1" >&2; exit 1; }

[ -f AGENTS.md ] || fail "AGENTS.md missing beside CLAUDE.md"
grep -q "CLAUDE.md" AGENTS.md || fail "AGENTS.md no longer points at CLAUDE.md"

for t in scripts/ninja_build.py tools/witness_smoke.py tools/menu_oracle.py \
         tools/list_free_ordinals.py tools/mc2x_import/fst.py \
         tools/repo_intel/repo_query.py scripts/deploy_payload.py scripts/run_smoke.py; do
    [ -f "$t" ] || fail "AGENTS.md references missing tool: $t"
done

lines=$(grep -c "" AGENTS.md)
[ "$lines" -le 120 ] || fail "AGENTS.md is $lines lines (>120): adapt, don't clone CLAUDE.md"

echo "[check-agents-md] OK ($lines lines, all referenced tools present)"
