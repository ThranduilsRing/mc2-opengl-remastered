#!/bin/bash
# Box idle filler: sync.sh [bundle | --mirror]  (runs on the box as /workspace/filler/sync.sh; PC side: py -3 tools/box_filler_push.py;
# --mirror = queued by box-supervisor when the mirror's nifty tip is ahead of current_sha)
# Fetches the nifty tip from the incremental git bundle, builds it (clang-20 + ccache, -j16 on cores 32-47, nice 19) and swaps
# exe + shaders into /workspace/filler/install (a cp -al clone of /workspace/gamerun/install). Writes /workspace/filler/current_sha last.
set -euo pipefail
export GIT_LFS_SKIP_SMUDGE=1   # LFS objects (3rdparty.zip) are not on the box and not needed for the Linux build
F=/workspace/filler
B=${1:-$F/incoming/nifty.bundle}
mkdir -p $F/incoming $F/results $F/tmp $F/logs
exec 7>$F/sync.lock
flock -n 7 || { echo "sync already running"; exit 3; }

[ -e $F/src/.git ] || git clone -q /workspace/gamerun/src $F/src
cd $F/src
if [ "${1:-}" = --mirror ]; then   # box-supervisor autosync: the nifty tip that box_run already pushed into the mirror (no bundle needed)
  git fetch -q /workspace/toolchains/mirror.git "+refs/heads/claude/nifty-mendeleev:refs/filler/nifty"
  SHA=$(git rev-parse refs/filler/nifty)
elif [ -f "$B" ]; then
  git fetch -q "$B" "+claude/nifty-mendeleev:refs/filler/nifty" "+claude/box-clang-green:refs/filler/green" 2>/dev/null || git fetch -q "$B" "+claude/nifty-mendeleev:refs/filler/nifty"
  SHA=$(git rev-parse refs/filler/nifty)
else SHA=$(cat $F/current_sha); fi
GREEN=$(git rev-parse -q --verify refs/filler/green || true)
git reset -q --hard
git checkout -q --detach "$SHA"
echo "[filler-sync] building $SHA"

export CCACHE_BASEDIR=/workspace CCACHE_NOHASHDIR=1
if [ ! -f build64-ninja/build.ninja ]; then
  cmake -S . -B build64-ninja -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_CXX_FLAGS=-stdlib=libc++ -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DVulkan_INCLUDE_DIR=/workspace/toolchains/vulkan/include -DMC2_DXC=/workspace/toolchains/dxc/bin/dxc \
    -DMC2_SLANGC=/workspace/toolchains/slang/bin/slangc > $F/logs/configure.log 2>&1 || { tail -20 $F/logs/configure.log; exit 1; }
fi
export CCACHE_BASEDIR=/workspace CCACHE_NOHASHDIR=1 CCACHE_SLOPPINESS=time_macros
[ -f /workspace/locks/cores.env ] && . /workspace/locks/cores.env   # the one core map
build() { nice -n 19 taskset -c "${BUILD_CPUS:-32-63}" ninja -C build64-ninja -j16 mc2 > $F/logs/build.log 2>&1; }
PORT=none
if ! build; then
  echo "[filler-sync] RED: nifty $SHA does not build on Linux (no patch fallback; fix nifty)"; grep -m5 -A3 "error:" $F/logs/build.log; exit 1
fi
echo "[filler-sync] built with port=$PORT"

# swap: first-time install is a hardlink clone; small writable top-level files and cache/ get private copies so the game never writes through to gamerun
exec 6>$F/install.lock
flock -x 6        # waits for running jobs (they hold it shared), blocks new ones for the few seconds of the swap
I=$F/install
if [ ! -d $I ]; then
  cp -al /workspace/gamerun/install $I
  for f in $I/*; do
    [ -f "$f" ] && [ "$(basename "$f")" != mc2 ] && { cp -p "$f" "$f.tmp"; mv -f "$f.tmp" "$f"; }
  done
  if [ -d $I/cache ]; then cp -a $I/cache $I/cache.new; rm -rf $I/cache; mv $I/cache.new $I/cache; fi
  rm -rf $I/debug_state; mkdir -p $I/debug_state
fi
cp build64-ninja/mc2 $I/mc2.new && mv -f $I/mc2.new $I/mc2
rm -rf $I/shaders.new; mkdir -p $I/shaders.new
(cd shaders && tar cf - --exclude=./fixtures .) | tar xf - -C $I/shaders.new
mkdir -p $I/shaders.new/vulkan; cp build64-ninja/shaders/vulkan/*.spv $I/shaders.new/vulkan/
rm -rf $I/shaders.old; [ -d $I/shaders ] && mv $I/shaders $I/shaders.old; mv $I/shaders.new $I/shaders; rm -rf $I/shaders.old
[ -f scripts/console_child.py ] && { cp scripts/console_child.py $I/scripts/console_child.py.new; mv -f $I/scripts/console_child.py.new $I/scripts/console_child.py; }
echo "$PORT" > $F/current_port
echo "$SHA" > $F/current_sha.tmp; mv -f $F/current_sha.tmp $F/current_sha
echo "[filler-sync] done $SHA"
