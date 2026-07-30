#!/usr/bin/env bash
# deploy-mc2fx-tools.sh -- copy the standalone gosFX effect tools into an
# install's tools/ folder so modders can run them alongside the game/editor.
#
# Why this script exists: mc2fx / mc2fx_preview are opt-in dev/mod tools
# (-DENABLE_MC2FX=ON, -DENABLE_MC2FX_PREVIEW=ON). Their CMake targets emit to
# build64/out/tools/... and know nothing about deploy paths. This script bridges
# that gap and keeps the launchers + guide a single source of truth (repo
# tools/mc2fx/), so a clean redeploy recreates the whole tools/ payload.
#
# What ships into $DEPLOY/tools/:
#   - mc2fx.exe, mc2fx_preview.exe            (from the build)
#   - mc2fx-console.bat, mc2fx-preview.bat    (repo tools/mc2fx/dist/)
#   - README.md                              (repo tools/mc2fx/)
# The previewer's runtime DLLs (SDL2.dll, glew32.dll) already live at the
# install root next to mc2.exe; the launchers cd there so they resolve.
#
# Inputs (env):
#   $BUILD_CFG -- build configuration. Default RelWithDebInfo.
#   $BUILD     -- build root.          Default <repo>/build64
#   $DEPLOY    -- target install(s).   Default = BOTH v0.4 (game) and 0.4c (editor),
#                space-separated. Override to deploy to one, e.g.
#                  DEPLOY=A:/Games/mc2-opengl/mc2-win64-v0.4 scripts/deploy-mc2fx-tools.sh
#
# Re-running is safe: copy-if-different via cmp. Missing tool exes are a soft
# skip (warn) -- they only exist when the tool targets were built.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$REPO/build64}"
BUILD_CFG="${BUILD_CFG:-RelWithDebInfo}"
DEPLOY="${DEPLOY:-A:/Games/mc2-opengl/mc2-win64-v0.4 A:/Games/mc2-opengl/mc2-win64-0.4c}"

CLI_EXE="$BUILD/out/tools/mc2fx/$BUILD_CFG/mc2fx.exe"
PRV_EXE="$BUILD/out/tools/mc2fx_preview/$BUILD_CFG/mc2fx_preview.exe"
DIST="$REPO/tools/mc2fx/dist"
README="$REPO/tools/mc2fx/README.md"

copy_if_diff() {
    # $1 src  $2 dst -- copy only when contents differ; never cp -r (no-overwrite trap).
    local src="$1" dst="$2"
    [ -f "$src" ] || { echo "  SKIP (missing): $src"; return 1; }
    if [ -f "$dst" ] && cmp -s "$src" "$dst"; then
        echo "  ok (unchanged): $dst"
    else
        cp -f "$src" "$dst"
        cmp -s "$src" "$dst" && echo "  copied: $dst" || { echo "  ERROR: copy mismatch $dst" >&2; return 2; }
    fi
}

if [ ! -f "$CLI_EXE" ] && [ ! -f "$PRV_EXE" ]; then
    echo "deploy-mc2fx-tools: no tool exes under $BUILD/out/tools/ -- build with"
    echo "  -DENABLE_MC2FX=ON -DENABLE_MC2FX_PREVIEW=ON first. Nothing to deploy."
    exit 0
fi

status=0
for target in $DEPLOY; do
    if [ ! -d "$target" ]; then
        echo "deploy-mc2fx-tools: DEPLOY not found, skipping: $target" >&2
        status=1
        continue
    fi
    # LINK-GUARD-1 (2026-07-30): `cp -f` below opens the destination with
    # O_TRUNC -- it only unlinks if that open FAILS -- so copying into a
    # hardlinked deploy dir rewrites the shared inode in place, exactly as
    # deploy_payload.py's shutil.copyfile did. tools/ is in the guard's
    # mutated set for that reason. Checked against the EXACT relpaths written
    # below, so an unrelated shared file elsewhere in the install does not
    # block a tools-only deploy. Fatal on SHARED/ERROR and on a guard that
    # will not run (c5d934d5: a guard that fails open is just a log line).
    guard="$REPO/scripts/check-hardlink-isolation.py"
    if [ "${MC2_DEPLOY_ALLOW_SHARED_LINKS:-0}" = "1" ]; then
        echo "[deploy-mc2fx-tools] WARNING: MC2_DEPLOY_ALLOW_SHARED_LINKS=1 --" \
             "hardlink preflight BYPASSED for $target" >&2
    elif [ ! -f "$guard" ]; then
        echo "[deploy-mc2fx-tools] FAIL: $guard missing -- refusing to deploy" \
             "UNGUARDED into $target. Restore it, or set" \
             "MC2_DEPLOY_ALLOW_SHARED_LINKS=1 to bypass deliberately." >&2
        exit 3
    else
        linklist="$(mktemp)"
        printf '%s\n' \
            "tools/mc2fx.exe" "tools/mc2fx_preview.exe" \
            "tools/mc2fx-console.bat" "tools/mc2fx-preview.bat" \
            "tools/README.md" > "$linklist"
        rc=0
        py -3 "$guard" --target "$target" --payload-list "$linklist" \
            --quiet --label "mc2fx tools deploy" || rc=$?
        rm -f "$linklist"
        if [ "$rc" -ne 0 ]; then
            echo "[deploy-mc2fx-tools] FAIL: $target is not hardlink-isolated" \
                 "for these files (guard exit $rc) -- NOTHING written there." >&2
            exit 3
        fi
    fi

    tools="$target/tools"
    mkdir -p "$tools"
    echo "[deploy-mc2fx-tools] -> $tools"
    copy_if_diff "$CLI_EXE" "$tools/mc2fx.exe"          || true
    copy_if_diff "$PRV_EXE" "$tools/mc2fx_preview.exe"  || true
    copy_if_diff "$DIST/mc2fx-console.bat" "$tools/mc2fx-console.bat" || true
    copy_if_diff "$DIST/mc2fx-preview.bat" "$tools/mc2fx-preview.bat" || true
    copy_if_diff "$README" "$tools/README.md"           || true
done

echo "[deploy-mc2fx-tools] done."
exit $status
