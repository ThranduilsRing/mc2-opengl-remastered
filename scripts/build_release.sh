#!/usr/bin/env bash
# build_release.sh — package release zips from a deployed install + mc2srcdata.
#
# Why this script exists: v0.2 shipped without the burnin_4x overlay, which made
# terrain colormaps blurry (every per-mission .burnin.tga was at stock resolution
# instead of 4× upscaled). The packaging step had been done manually and the
# overlay was forgotten. This script codifies the recipe so it cannot regress.
#
# Output: 7 zips in release_assets/ — engine, gamedata (no loose burnins,
# no .BIK), burnins-4x-pt1, burnins-4x-pt2 (overlay split into two halves
# to stay under GitHub's 2 GB per-asset cap; combined raw-deflate of the
# 4x-upscaled overlay was 2.3 GB in v0.3), art, tgl, movies. Burnins are
# an optional overlay — FST archives carry stock-resolution fallbacks,
# and either part can be applied independently. Movies (data/movies/*.BIK,
# ~100 MB) ship in their own zip so end users can skip them — gameplay
# works without intro cinematics, and bundling them into gamedata.zip
# would needlessly inflate the must-download path.
#
# Inputs:
#   $DEPLOY  — full deployed install (mc2.exe + data/ + *.fst). Default:
#              A:/Games/mc2-opengl/mc2-win64-v0.2
#   $SRCDATA — mc2srcdata tree. Default: A:/Games/mc2-opengl-src/mc2srcdata
#   $OUTDIR  — where zips land. Default: <repo>/release_assets
#
# Re-running is safe: zips are rewritten in place.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DEPLOY="${DEPLOY:-A:/Games/mc2-opengl/mc2-win64-v0.2}"
SRCDATA="${SRCDATA:-A:/Games/mc2-opengl-src/mc2srcdata}"
OUTDIR="${OUTDIR:-$REPO/release_assets}"
SEVENZIP="${SEVENZIP:-/c/Program Files/7-Zip/7z.exe}"

[ -d "$DEPLOY" ]   || { echo "DEPLOY not found: $DEPLOY"; exit 1; }
[ -d "$SRCDATA" ]  || { echo "SRCDATA not found: $SRCDATA"; exit 1; }
[ -x "$SEVENZIP" ] || { echo "7z not at: $SEVENZIP"; exit 1; }
mkdir -p "$OUTDIR"

z() { "$SEVENZIP" a -tzip -mx=7 -bso0 -bsp0 "$@"; }

# Stage a clean tree we can shape, so we can exclude stock-resolution burnins
# from gamedata without touching the deployed install.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# ---------- mc2-burnins-4x.zip ----------
# Verify all source burnins are 4× upscaled (any stock-resolution file would
# defeat the point of the overlay). 4× of stock 1280/1536/768 is 5120/6144/3072.
echo "[burnins-4x] verifying source dimensions"
bad=0
for f in "$SRCDATA"/textures/burnin_4x/*.burnin.tga; do
    [ -f "$f" ] || continue
    dim=$(xxd -s 12 -l 4 -p "$f")
    case "$dim" in
        00140014|00180018|000c000c) ;;
        *) echo "  WRONG: $f -> $dim"; bad=1 ;;
    esac
done
[ "$bad" -eq 0 ] || { echo "[burnins-4x] aborting — non-4× file in burnin_4x/"; exit 1; }

# Split into two parts so each zip stays under GitHub's 2 GB per-asset cap.
# Part 1: campaign-A missions (mc2_NN.burnin.tga).
# Part 2: campaign-M missions (mc2_mNN), tutorials (tut_NN), and e3demo.
mkdir -p "$STAGE/burnins-pt1/data/textures" "$STAGE/burnins-pt2/data/textures"
for f in "$SRCDATA"/textures/burnin_4x/*.burnin.tga; do
    [ -f "$f" ] || continue
    name=$(basename "$f")
    case "$name" in
        mc2_m*|tut_*|e3demo*) cp -f "$f" "$STAGE/burnins-pt2/data/textures/" ;;
        mc2_*)                cp -f "$f" "$STAGE/burnins-pt1/data/textures/" ;;
        *)                    cp -f "$f" "$STAGE/burnins-pt2/data/textures/" ;;
    esac
done
rm -f "$OUTDIR/mc2-burnins-4x-pt1.zip" "$OUTDIR/mc2-burnins-4x-pt2.zip"
( cd "$STAGE/burnins-pt1" && z "$OUTDIR/mc2-burnins-4x-pt1.zip" data ) >/dev/null
( cd "$STAGE/burnins-pt2" && z "$OUTDIR/mc2-burnins-4x-pt2.zip" data ) >/dev/null
echo "[burnins-4x-pt1] $(ls -lh "$OUTDIR/mc2-burnins-4x-pt1.zip" | awk '{print $5}')"
echo "[burnins-4x-pt2] $(ls -lh "$OUTDIR/mc2-burnins-4x-pt2.zip" | awk '{print $5}')"

# ---------- mc2-gamedata.zip ----------
# Everything in DEPLOY/data/ except burnin .tga (now in mc2-burnins-4x.zip)
# and movies/*.BIK (now in mc2-movies.zip), plus all .fst archives at the
# install root. FST contains stock-resolution burnins as a fallback; the
# overlay zip is optional.
echo "[gamedata] staging"
mkdir -p "$STAGE/gamedata"
cp -r "$DEPLOY/data" "$STAGE/gamedata/"
find "$STAGE/gamedata/data/textures" -maxdepth 1 -name "*.burnin.tga" -delete
find "$STAGE/gamedata/data/movies" -maxdepth 1 -iname "*.bik" -delete 2>/dev/null || true
cp "$DEPLOY"/*.fst "$STAGE/gamedata/"
rm -f "$OUTDIR/mc2-gamedata.zip"
( cd "$STAGE/gamedata" && z "$OUTDIR/mc2-gamedata.zip" data *.fst ) >/dev/null
# Sanity: gamedata zip must NOT contain loose burnin tgas or .BIK movies.
if "$SEVENZIP" l "$OUTDIR/mc2-gamedata.zip" | grep -q "\.burnin\.tga"; then
    echo "[gamedata] FAIL — burnin.tga leaked into gamedata.zip"; exit 1
fi
if "$SEVENZIP" l "$OUTDIR/mc2-gamedata.zip" | grep -qi "\.bik"; then
    echo "[gamedata] FAIL — .BIK leaked into gamedata.zip"; exit 1
fi
echo "[gamedata] $(ls -lh "$OUTDIR/mc2-gamedata.zip" | awk '{print $5}')"

# ---------- mc2-movies.zip ----------
# Operation intro and unit briefing cinematics. Source: $DEPLOY/data/movies/*.BIK.
# Sidecar WAVs (cinema1..5.wav) ride in mc2-gamedata.zip alongside data/sound/
# — that ladder is what makes the Operation intros audible in our FFmpeg path.
echo "[movies] staging"
mkdir -p "$STAGE/movies/data/movies"
shopt -s nullglob nocaseglob
biks=( "$DEPLOY"/data/movies/*.bik )
shopt -u nocaseglob
if [ "${#biks[@]}" -eq 0 ]; then
    echo "[movies] FAIL — no .BIK in $DEPLOY/data/movies/"; exit 1
fi
for f in "${biks[@]}"; do cp -f "$f" "$STAGE/movies/data/movies/"; done
rm -f "$OUTDIR/mc2-movies.zip"
( cd "$STAGE/movies" && z "$OUTDIR/mc2-movies.zip" data ) >/dev/null
echo "[movies] $(ls -lh "$OUTDIR/mc2-movies.zip" | awk '{print $5}') (${#biks[@]} BIK files)"

# ---------- mc2-art.zip / mc2-tgl.zip ----------
# These are the 4× upscaled overlays for static art and 3D model textures.
# They live in art_4x_gpu/ and tgl_4x_gpu/ respectively.
for kind in art tgl; do
    src="$SRCDATA/${kind}_4x_gpu"
    [ -d "$src" ] || { echo "[$kind] skipping — $src not found"; continue; }
    mkdir -p "$STAGE/$kind/data"
    cp -r "$src" "$STAGE/$kind/data/$kind"
    rm -f "$OUTDIR/mc2-$kind.zip"
    ( cd "$STAGE/$kind" && z "$OUTDIR/mc2-$kind.zip" data ) >/dev/null
    echo "[$kind] $(ls -lh "$OUTDIR/mc2-$kind.zip" | awk '{print $5}')"
done

# ---------- mc2-remastered-engine.zip ----------
# mc2.exe + shaders/ + runtime DLLs at the install root. Excludes data/ and FSTs.
echo "[engine] staging"
mkdir -p "$STAGE/engine"
cp "$DEPLOY/mc2.exe" "$STAGE/engine/"
cp -r "$DEPLOY/shaders" "$STAGE/engine/"
# Runtime DLLs at the deploy root (SDL2, GLEW, FFmpeg, MSVC redist, etc).
for f in "$DEPLOY"/*.dll "$DEPLOY"/run-with-log.bat; do
    [ -e "$f" ] && cp "$f" "$STAGE/engine/"
done
rm -f "$OUTDIR/mc2-remastered-engine.zip"
( cd "$STAGE/engine" && z "$OUTDIR/mc2-remastered-engine.zip" . ) >/dev/null
echo "[engine] $(ls -lh "$OUTDIR/mc2-remastered-engine.zip" | awk '{print $5}')"

echo
echo "Done. Zips in $OUTDIR :"
ls -lh "$OUTDIR"/*.zip
