#!/usr/bin/env bash
# check-worktree-context.sh
#
# Advisory guard against the "subagent edited stale ROOT file instead
# of the worktree copy" incident class.
#
# Background: lanes live in .claude/worktrees/<lane>/ (own branch).
# The repo ROOT checkout often sits on an old/unrelated branch. An
# agent that opens a session at the ROOT and edits e.g.
# mclib/mech3d.cpp is editing a STALE copy -- the change never lands
# in the lane and silently diverges (documented incident: root
# mclib/mech3d.cpp edited while the lane lived in
# ../mc2-main).
#
# This script reports, from ANY cwd inside the repo or a worktree:
#   1. where you are (root vs which worktree) + current branch
#   2. a DANGER report when you are at the ROOT, the root tree is
#      dirty, and active lane worktrees exist (the stale-root-edit
#      signature)
#   3. a one-glance table of all worktrees: path, branch, dirty count
#
# Advisory: always exits 0. Pass --strict to exit 1 when the danger
# signature is detected (for pre-commit hooks / CI lanes).
#
# Usage:
#   sh scripts/check-worktree-context.sh [--strict]

set -e

STRICT=0
[ "${1:-}" = "--strict" ] && STRICT=1

TAG="[check-worktree-context]"

TOPLEVEL=$(git rev-parse --show-toplevel 2>/dev/null) || {
    echo "$TAG FAIL: not inside a git repository (cwd: $(pwd))" >&2
    exit 0
}
BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "DETACHED")

# The main worktree (repo root) is the first entry of `git worktree list`.
MAIN_ROOT=$(git worktree list --porcelain | awk '/^worktree /{print $2; exit}')

if [ "$TOPLEVEL" = "$MAIN_ROOT" ]; then
    WHERE="repo ROOT"
else
    WHERE="worktree $(basename "$TOPLEVEL")"
fi

echo "$TAG 1. You are at: $WHERE"
echo "$TAG    path:   $TOPLEVEL"
echo "$TAG    branch: $BRANCH"
echo ""

# ---- 3. worktree table (computed first; reused by danger report) ----
echo "$TAG 3. All worktrees:"
printf '%s    %-55s %-35s %s\n' "$TAG" "PATH" "BRANCH" "STATE"

LANE_WORKTREES=""
git worktree list --porcelain | awk '/^worktree /{print $2}' | while read -r wt; do
    wbranch=$(git -C "$wt" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")
    dirty=$(git -C "$wt" status --porcelain 2>/dev/null | grep -c . || true)
    if [ "$dirty" -eq 0 ]; then state="clean"; else state="DIRTY ($dirty files)"; fi
    marker=""
    [ "$wt" = "$TOPLEVEL" ] && marker="  <-- you are here"
    printf '%s    %-55s %-35s %s%s\n' "$TAG" "$wt" "$wbranch" "$state" "$marker"
done
echo ""

# ---- 2. DANGER report (stale-root-edit signature) ----
DANGER=0
if [ "$TOPLEVEL" = "$MAIN_ROOT" ]; then
    ROOT_DIRTY=$(git -C "$MAIN_ROOT" status --porcelain 2>/dev/null | grep -c . || true)
    OTHER_WTS=$(git worktree list --porcelain | awk -v r="$MAIN_ROOT" '/^worktree /{if ($2 != r) print $2}')
    case "$BRANCH" in
        claude/nifty-mendeleev*) ON_LANE_BRANCH=1 ;;
        *) ON_LANE_BRANCH=0 ;;
    esac
    if [ "$ROOT_DIRTY" -gt 0 ] && [ -n "$OTHER_WTS" ] && [ "$ON_LANE_BRANCH" -eq 0 ]; then
        DANGER=1
        echo "$TAG 2. DANGER: you are at repo ROOT on branch '$BRANCH' and the root tree is DIRTY ($ROOT_DIRTY files)." >&2
        N_WTS=$(echo "$OTHER_WTS" | grep -c .)
        echo "$TAG    $N_WTS lane worktrees exist (first 10 below; see table above for all):" >&2
        echo "$OTHER_WTS" | head -10 | while read -r wt; do
            echo "$TAG      - $wt ($(git -C "$wt" rev-parse --abbrev-ref HEAD 2>/dev/null))" >&2
        done
        echo "$TAG    Root edits are LIKELY WRONG -- lane work belongs in its worktree" >&2
        echo "$TAG    (incident class: root mclib/mech3d.cpp edited while lane lived in" >&2
        echo "$TAG     ../mc2-main). cd into the lane worktree first." >&2
        # Cheap stale-edit cross-check: root-dirty tracked files that also
        # differ in a lane worktree (same path modified both places).
        git -C "$MAIN_ROOT" status --porcelain | awk '{print $2}' | while read -r f; do
            echo "$OTHER_WTS" | while read -r wt; do
                if [ -f "$wt/$f" ] && ! git -C "$wt" diff --quiet -- "$f" 2>/dev/null; then
                    echo "$TAG    SUSPECT: '$f' is dirty at root AND modified in $wt" >&2
                fi
            done
        done
    else
        echo "$TAG 2. No danger signature (root clean, on a lane branch, or no lane worktrees)."
    fi
else
    echo "$TAG 2. No danger signature (you are inside a worktree, not the root)."
fi

if [ "$DANGER" -eq 1 ] && [ "$STRICT" -eq 1 ]; then
    echo "$TAG --strict: exiting 1 due to danger signature" >&2
    exit 1
fi
exit 0
