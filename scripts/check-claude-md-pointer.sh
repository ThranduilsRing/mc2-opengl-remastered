#!/usr/bin/env bash
# check-claude-md-pointer.sh
#
# Enforces that A:/Games/mc2-opengl-src/CLAUDE.md (the ROOT repo's
# CLAUDE.md) remains a thin pointer to the worktree CLAUDE.md, and
# never accumulates project rules, session narratives, or other
# substantive content.
#
# Background: agents that open a session from the root repo read its
# CLAUDE.md for context. If root CLAUDE.md drifts (e.g. someone copies
# the worktree CLAUDE.md content into it, or appends session notes),
# stale guidance leaks into those sessions and causes wrong behavior
# (wrong shader version, wrong smoke gate command, missing release
# rules, etc.). The fix is to keep root CLAUDE.md as a literal
# "go read the worktree" pointer; this script is the enforcement.
#
# Run before commits that touch the root CLAUDE.md, OR as part of a
# pre-commit hook in the root repo.
#
# Exit codes:
#   0 = OK, root CLAUDE.md is still the pointer template
#   1 = file missing
#   2 = required sentinel string absent (someone replaced the template)
#   3 = file exceeds the hard line cap (someone added content)

set -e

ROOT_REPO="A:/Games/mc2-opengl-src"
TARGET="$ROOT_REPO/CLAUDE.md"
LINE_CAP=60

if [ ! -f "$TARGET" ]; then
    echo "[check-claude-md-pointer] FAIL: $TARGET does not exist" >&2
    exit 1
fi

# Sentinel substrings that must appear in the pointer template. If any
# is missing, someone has replaced the template with substantive
# content -- revert. To intentionally update the template, edit both
# the file AND this script's SENTINELS array in the same commit.
SENTINELS=(
    "DO NOT EDIT"
    "root CLAUDE.md placeholder"
    ".claude/worktrees/nifty-mendeleev/CLAUDE.md"
    "must remain a thin pointer"
    "check-claude-md-pointer.sh"
)

missing_any=0
for sentinel in "${SENTINELS[@]}"; do
    if ! grep -qF "$sentinel" "$TARGET"; then
        echo "[check-claude-md-pointer] FAIL: missing sentinel: '$sentinel'" >&2
        missing_any=1
    fi
done

if [ $missing_any -ne 0 ]; then
    echo "[check-claude-md-pointer] FAIL: $TARGET is no longer the pointer template." >&2
    echo "[check-claude-md-pointer] Substantive content belongs in the worktree CLAUDE.md, not here." >&2
    exit 2
fi

LINES=$(wc -l < "$TARGET")
# wc may emit leading whitespace on some msys/cygwin builds; strip it.
LINES=$(echo "$LINES" | tr -d '[:space:]')

if [ "$LINES" -gt "$LINE_CAP" ]; then
    echo "[check-claude-md-pointer] FAIL: $TARGET has $LINES lines, max allowed is $LINE_CAP." >&2
    echo "[check-claude-md-pointer] The root file is a pointer; content belongs in the worktree CLAUDE.md." >&2
    exit 3
fi

echo "[check-claude-md-pointer] OK: $TARGET has $LINES lines, all $((${#SENTINELS[@]})) sentinels present"
