#!/usr/bin/env bash
# deploy-editor.sh -- copy a freshly-built Mission Editor + its overlay assets
# into the v0.4 install so the editor lives alongside the game and modders
# can run both from one install.
#
# Why this script exists: editor used to deploy into A:/Games/mc2-opengl/mc2-editor/
# (a separate clone of v0.4 with overlays). 2026-05-25 we collapsed that into a
# single install. Editor's CMake POST_BUILD targets stay relative to
# build64/out/editor/... (it should not know about deploy paths); this script
# bridges the gap.
#
# Inputs:
#   $BUILD_CFG -- which configuration to deploy from. Default RelWithDebInfo
#                 (matches what mc2-editor was previously seeded from on
#                 2026-05-25 14:03). Use Release for stripped editor.
#   $BUILD     -- build root. Default <repo>/build64
#   $DEPLOY    -- target install. Default A:/Games/mc2-opengl/mc2-win64-v0.4
#
# What ships:
#   - Mission Editor.exe (+ .pdb if it exists) at $DEPLOY/
#   - data/art/Buildings.csv -- v0.1.1 stock; editor needs fitID coupling with
#                               Object2.pak; game (mc2.exe) never reads it
#   - esplash.bmp + tacsplash.bmp -- CMake POST_BUILD already drops these next
#                                    to the exe; we mirror to $DEPLOY/
#
# Re-running is safe: copy_if_different semantics via cmp.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$REPO/build64}"
BUILD_CFG="${BUILD_CFG:-RelWithDebInfo}"
DEPLOY="${DEPLOY:-A:/Games/mc2-opengl/mc2-win64-v0.4}"

EDITOR_OUT="$BUILD/out/editor/$BUILD_CFG"
EDITOR_EXE="$EDITOR_OUT/Mission Editor.exe"
EDITOR_PDB="$EDITOR_OUT/Mission Editor.pdb"

[ -f "$EDITOR_EXE" ] || { echo "deploy-editor: missing $EDITOR_EXE -- build EditRel first"; exit 1; }
[ -d "$DEPLOY" ]     || { echo "deploy-editor: DEPLOY not found: $DEPLOY"; exit 1; }

copy_if_diff() {
    local src="$1" dst="$2"
    if [ ! -f "$dst" ] || ! cmp -s "$src" "$dst"; then
        cp -f "$src" "$dst"
        echo "  -> $(basename "$dst")"
    fi
}

echo "[deploy-editor] from $EDITOR_OUT -> $DEPLOY"

copy_if_diff "$EDITOR_EXE" "$DEPLOY/Mission Editor.exe"
[ -f "$EDITOR_PDB" ] && copy_if_diff "$EDITOR_PDB" "$DEPLOY/Mission Editor.pdb" || true

# Splashes -- CMake POST_BUILD already drops these next to the built exe.
for splash in esplash.bmp tacsplash.bmp; do
    if [ -f "$EDITOR_OUT/$splash" ]; then
        copy_if_diff "$EDITOR_OUT/$splash" "$DEPLOY/$splash"
    fi
done

# Buildings.csv -- v0.1.1 stock; required by the editor (EditorInterface*.cpp).
# Source: $REPO/data/art/editor/Buildings.csv if present, else mc2srcdata stock.
BLDGS_SRC=""
for candidate in \
    "$REPO/data/art/editor/Buildings.csv" \
    "$REPO/data/art/Buildings.csv" \
    "A:/Games/mc2-opengl/mc2-editor/data/art/Buildings.csv"; do
    if [ -f "$candidate" ]; then
        BLDGS_SRC="$candidate"
        break
    fi
done
if [ -n "$BLDGS_SRC" ]; then
    copy_if_diff "$BLDGS_SRC" "$DEPLOY/data/art/Buildings.csv"
else
    echo "  (warn) Buildings.csv source not found; editor will fall back to art.fst"
fi

# Terrain generator (Path B) -- the "Generate Map" editor button shells out to
# tools/terrain_gen/terrain_gen.py relative to the deploy CWD. Ship the python
# package so it's runnable from the install. (Requires Python 3 + numpy/Pillow/
# opensimplex on the user's PATH: pip install -r tools/terrain_gen/requirements.txt)
if [ -d "$REPO/tools/terrain_gen" ]; then
    mkdir -p "$DEPLOY/tools/terrain_gen/recipes"
    for f in "$REPO"/tools/terrain_gen/*.py "$REPO"/tools/terrain_gen/requirements.txt; do
        [ -f "$f" ] && copy_if_diff "$f" "$DEPLOY/tools/terrain_gen/$(basename "$f")"
    done
    for f in "$REPO"/tools/terrain_gen/recipes/*.json; do
        [ -f "$f" ] && copy_if_diff "$f" "$DEPLOY/tools/terrain_gen/recipes/$(basename "$f")"
    done
fi

# Deploy-coherence manifest (advisory; see scripts/check-deploy-coherence.py).
# --merge so the game's manifest entries in the shared install survive.
# Best-effort: never fail the deploy over manifest bookkeeping.
if ! py -3 "$REPO/scripts/write-deploy-manifest.py" "$DEPLOY" \
        "Mission Editor.exe" --merge --worktree "$REPO"; then
    echo "  (warn) deploy manifest write failed (advisory only)"
fi

# CSV manifest refresh (.deployed_manifest.csv) -- the manifest that
# deploy_payload.py --verify-only checks. This editor copy mutated the
# install WITHOUT going through deploy_payload, so without this step the
# CSV would go stale (the exact provenance-drift bug this slice fixes).
# --write-manifest-only re-hashes the files already in $DEPLOY and rewrites
# the CSV (merging, so the game's mc2.exe rows survive). No copy, no lock
# check. Best-effort: never fail the deploy over manifest bookkeeping.
if ! py -3 "$REPO/scripts/deploy_payload.py" "$DEPLOY" \
        --exe-name "Mission Editor.exe" \
        --build-dir "$EDITOR_OUT" \
        --source-root "$REPO" \
        --write-manifest-only; then
    echo "  (warn) CSV deploy manifest refresh failed (advisory only)"
fi

echo "[deploy-editor] done"
