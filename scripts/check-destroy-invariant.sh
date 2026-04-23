#!/bin/sh
# scripts/check-destroy-invariant.sh
# Enforces: all setExists(false) call sites outside GameObject::destroy
# are violations (stability spec §3.8).
#
# Portability notes:
#  - Uses `grep -E` (POSIX extended regex) with `[[:space:]]*` instead of
#    `\s`. Plain `grep` / BRE does NOT interpret `\s` as whitespace.
#  - Tested against GNU grep (MSYS2/Git-Bash on Windows) and POSIX grep.

set -e
violations=0

# Literal: setExists(false) must only appear inside GameObject::destroy
# (code/gameobj.cpp). Comment lines (//) are skipped — wrapper docs in
# code/gameobj.h reference the symbol literally without calling it.
lits=$(grep -rEn 'setExists[[:space:]]*\([[:space:]]*false' code/ mclib/ GameOS/ \
    --include='*.cpp' --include='*.h' \
    | grep -v 'code/gameobj.cpp' \
    | grep -Ev ':[[:space:]]*//' || true)
if [ -n "$lits" ]; then
    echo "[INVARIANT] literal setExists(false) outside GameObject::destroy:"
    echo "$lits"
    violations=1
fi

# Non-literal: setExists(<expr>) where expr is not true/false literal —
# flag for manual review. Does not fail the check by itself.
nonlit=$(grep -rEn 'setExists[[:space:]]*\(' code/ mclib/ GameOS/ \
    --include='*.cpp' --include='*.h' \
    | grep -Ev 'setExists[[:space:]]*\([[:space:]]*(true|false)' \
    | grep -v 'code/gameobj.cpp' || true)
if [ -n "$nonlit" ]; then
    echo "[INVARIANT] non-literal setExists(<expr>) — manual review required:"
    echo "$nonlit"
fi

if [ $violations -eq 0 ]; then
    echo "OK"
fi
exit $violations
