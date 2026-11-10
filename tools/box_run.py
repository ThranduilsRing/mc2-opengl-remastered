#!/usr/bin/env python3
"""Run game rows for the CURRENT worktree HEAD on the rented Linux box (RTX 4060; GPUs discovered at submission).

    py -3 tools/box_run.py --gpu auto -- smoke-lite
    py -3 tools/box_run.py --gpu 0 -- smoke-lite capture:mc2_01:mc2_01_saved_205644 determinism:mc2_10
    py -3 tools/box_run.py --seed          # once: build the golden install on the box (needs the Windows golden)

Steps (each timed, summary printed at the end): lock a GPU, git-bundle HEAD to the box mirror, clang build in
/workspace/lanes/box-lane-NN/src (worktree slot, reused across branches), install = hardlink copy of the golden install
(big immutable data linked, the rest copied) + deploy_payload, run the rows, copy reports back to
E:/mc2-archive-2026-10/proofs/box/<worktree>/<sha8>/ (MC2_BOX_PROOFS), print the DEPLOY_FINGERPRINT line, release the lock.

Rows: ab:<before-ref> (day + night A/B against HEAD, plus the gpu-passes ratio table) | gpu-passes (per-pass GPU table, mc2_01/mc2_10/mc2_24; refuses a dirty worktree) | smoke-lite | smoke-tier1 | syncval[:aa_off|aa_fxaa|aa_cmaa|cold_bake|imgui|resize|nowater|gpuav|canary] | capture:<mission>:<bookmark-stem> | screen:<id>[@N] (logistics, campaign_missions, mechbay: front-end screenshot) | determinism:<mission>[:audit] | saveload[:<mission>] | unit | unit-san (doctests under ASan+UBSan, CPU-only) | build | lint | cpu:<command> | sh:<command> (no GPU lock) | gpu:<command> (takes the lock)
(sh runs in the lane src dir with $INSTALL, $OUT, $GPU_ENV set). Merge gates share the discovered GPUs.
Legacy capture rows are forced onto GPU 0 by `--gpu auto`.
Windows-only behaviour (window focus, firewall, owner-test) is not covered: those rows stay on the Windows PC.
"""
import argparse, base64, io, json, os, re, subprocess, sys, tarfile, tempfile, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import console_child  # noqa: E402

BOX_FILE = os.environ.get("MC2_BOX_FILE", "A:/Games/mc2-ops/BOX")   # the one address file: host=, port=, key= lines


def load_box_config(path=None):
    """{host, port, key} from the BOX file (missing or unreadable file: {}); the callers fall back to the constants below"""
    cfg = {}
    try:
        for ln in Path(path or BOX_FILE).read_text().splitlines():
            k, sep, v = ln.partition("=")
            if sep and not ln.lstrip().startswith("#") and v.strip():
                cfg[k.strip()] = v.strip()
    except OSError:
        pass
    return cfg


_CFG = load_box_config()
# precedence: environment, then the BOX file, then these constants (the current box)
KEY = os.environ.get("MC2_BOX_KEY") or _CFG.get("key", "C:/SPB_Data/.ssh/mc2farm_ed25519")
HOST = os.environ.get("MC2_BOX_HOST") or _CFG.get("host", "root@14.227.95.149")
PORT = os.environ.get("MC2_BOX_PORT") or _CFG.get("port", "30598")
MIRROR = "/workspace/toolchains/mirror.git"
GOLDEN_WIN = "A:/Games/mc2-opengl/mc2-win64-v0.4c"
_SSH_OPTS = ["-i", KEY, "-p", PORT, "-o", "LogLevel=ERROR", "-o", "ServerAliveInterval=30", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", "-o", "StrictHostKeyChecking=accept-new", HOST]
SSH = ["ssh", "-n", *_SSH_OPTS]        # -n: ssh must not swallow our stdin (every call that does not pipe a script in)
SSH_STDIN = ["ssh", *_SSH_OPTS]        # the two calls that pipe the remote script in
# verdicts beyond PASS/FAIL; every one of them is NOT green (exit code != 0), and none is ever retried by this tool
RC_LOCK_TIMEOUT = 76    # waited 30 min for a GPU lock (was 75, the same code as other failures)
RC_BOX_INFRA = 77       # the box refused to start: /workspace has less than the free-disk floor (MC2_BOX_MIN_FREE_GB, default 15)
RC_NO_BASELINE = 78    # determinism/saveload row refused: no Linux baseline file recorded
NEWEST_BOX_CLANG_FIX = "ee183497d"   # last nifty commit that kept the Linux clang build green (BRIDGE diag tag); older tips cannot build on the box
SCP = ["scp", "-q", "-i", KEY, "-P", PORT]
# `--gate` with no rows: the headless determinism rows the PC gate runs plus the two saveload pins (tests/*/linux baselines)
NO_GPU_ROWS = ("build", "unit", "unit-san", "lint")   # rows that never touch a GPU: no GPU lock, queue group "build"
# plus what needs no Windows: the doctests, the Linux-capable contract rows (scripts/box_contracts.sh) and smoke-lite
GATE_ROWS = ([f"determinism:mc2_{n}" for n in ("01", "03", "10", "16", "24")] + ["saveload:mc2_01", "saveload:mc2_10"]
             + ["unit", "sh:bash scripts/box_contracts.sh", "smoke-lite"])

# a retry must never reuse a row's --out dir (sim_determinism refuses an existing one and the row would be VOID): one fresh dir per attempt
FRESH_FN = 'fresh() { local d=$1 n=0; while [ -e "$d.a$n" ]; do n=$((n+1)); done; printf %s "$d.a$n"; }'


def write_payload_manifest(install, commit):
    """Hash the installed Linux payload, using the production deploy CSV writer."""
    import re
    import deploy_payload as deploy
    from exe_fingerprint import extract

    install = Path(install)
    fingerprint = extract(install / "mc2")
    sha = fingerprint.get("sha") if fingerprint.get("present") else None
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or not sha or not commit.startswith(sha):
        raise ValueError("Linux payload executable commit mismatches requested commit")
    paths = {install / "mc2"}
    paths.update(p for tree in ("data", "shaders") for p in (install / tree).rglob("*") if p.is_file())
    paths.update(install.glob("*.fst"))
    if (install / "crashpad_handler").is_file():
        paths.add(install / "crashpad_handler")
    hashes = {}
    for path in sorted(paths):
        stat = path.stat()
        hashes[path.relative_to(install).as_posix()] = (deploy.sha256_file(path), stat.st_size, stat.st_mtime_ns)
    if not any(p.startswith("shaders/vulkan/") and p.endswith(".spv") for p in hashes):
        raise ValueError("Linux payload has no compiled shaders")
    deploy.check_hardlink_isolation([(None, deploy.MANIFEST_NAME, "manifest")], str(install))
    deploy.write_manifest(str(install), hashes, commit)

# Box-wide build semaphore (owner P0 2026-11-10: 10 lanes ran `ninja -j16` at once on a ~17-CPU quota). A FIFO of ticket files in
# $BQ named <ns-timestamp>.<lane>.<pid>; the first N live tickets build, the rest wait and print their position every 60 s.
# One build at a time (MC2_BOX_BUILDS overrides) on the dedicated BUILD_CPUS of locks/cores.env, -j = how many cores that list holds, capped at the cgroup quota. Dead tickets (pid gone) are pruned by whoever looks.
GAME_CHECK = r'''
# A free-text row (gpu:/sh:/cpu:) that names the game binary but produced no engine log did not run the game: rc 0 must not become PASS.
# game_not_run <cmd> <rc> <outdir> <marker>: 0 (true) when cmd mentions ./mc2 or $I/mc2, rc is 0 and no file under outdir newer than marker holds VK_ENGINE_CONTEXT.
game_not_run() {
  case "$1" in *'./mc2'*|*'$I/mc2'*|*'${I}/mc2'*) ;; *) return 1 ;; esac
  [ "$2" = 0 ] || return 1
  [ -z "$(find "$3" -type f -newer "$4" -exec grep -la 'VK_ENGINE_CONTEXT' {} + 2>/dev/null)" ]
}
'''

BUILD_SEM = r'''
BQ=${MC2_BOX_BUILDQ:-/workspace/locks/build.queue}; TK=""
bq_ncpu() { local n=0 r a b; for r in ${1//,/ }; do a=${r%-*}; b=${r#*-}; n=$((n + b - a + 1)); done; echo "$n"; }   # "8-35" -> 28, "0-3,8" -> 5
bq_quota() { if [ -n "${MC2_BOX_QUOTA_CPUS:-}" ]; then echo "$MC2_BOX_QUOTA_CPUS"; return; fi   # cgroup CPU quota (cpu.max), else unlimited
  awk '$1 ~ /^[0-9]+$/ && $2 > 0 { printf "%d", $1 / $2; found = 1 } END { exit !found }' /sys/fs/cgroup/cpu.max 2>/dev/null || echo 9999; }
bq_j() { local n q; n=$(bq_ncpu "$1"); q=$(bq_quota); [ "$q" -ge 1 ] 2>/dev/null || q=1; [ "$n" -le "$q" ] || n=$q; [ "$n" -ge 1 ] || n=1; echo "$n"; }   # -j = BUILD_CPUS count, capped at the quota
bq_alive() { local p; p=$(cat "$1" 2>/dev/null); [ -n "$p" ] && kill -0 "$p" 2>/dev/null; }
bq_acquire() {
  local n t me last=0 pos m now hold k
  n=${MC2_BOX_BUILDS:-1}; [ "$n" -ge 1 ] || n=1
  mkdir -p "$BQ"; TK="$BQ/$(date +%s%N).$BR.$$"; echo $$ > "$TK"; me=$(basename "$TK")
  while :; do
    pos=0; m=0
    for t in $(ls "$BQ" 2>/dev/null | sort); do
      if ! bq_alive "$BQ/$t"; then rm -f "$BQ/$t"; continue; fi
      m=$((m + 1)); [ "$t" = "$me" ] && pos=$m
    done
    if [ "$pos" -le "$n" ] && [ "$pos" -ge 1 ]; then touch "$TK"; echo "[box_run] build slot granted (n=$n)"; return 0; fi
    now=$(date +%s)
    if [ $((now - last)) -ge 60 ]; then
      hold=$(ls "$BQ" | sort | head -1); k=$((pos - n))
      echo "[box_run] build queued: position $k of $((m - n)), holder $(echo "$hold" | cut -d. -f2) $((now - $(stat -c %Y "$BQ/$hold" 2>/dev/null || echo "$now")))s"
      last=$now
    fi
    sleep "${MC2_BOX_BUILDQ_POLL:-5}"
  done
}
bq_release() { [ -n "$TK" ] && rm -f "$TK"; TK=""; }
'''

# Client tie (owner P0): a foreground run is a child of its ssh session. When the local box_run dies the session's sshd exits and this
# script would run on as an orphan; the watchdog ends the whole session group. A queued (pueue) task is detached by design: no tie.
CLIENT_TIE = r'''
_pp=$(ps -o ppid= -p $PPID 2>/dev/null | tr -d ' ')
if [ -n "$_pp" ] && [ "$_pp" != 1 ] && ps -o comm= -p "$_pp" 2>/dev/null | grep -q '^sshd'; then
  ( while kill -0 "$_pp" 2>/dev/null; do sleep 5; done; echo "[box_run] client gone: ending this lane run" >&2; pkill -TERM -g "$(ps -o pgid= -p $$ | tr -d ' ')" ) >/dev/null 2>&1 &
fi
'''

REMOTE = r'''#!/bin/bash
# box_run.py remote half: args GPU BRANCH SHA OUTDIR CAP0 ROWS_B64 (rows newline-joined, base64)
set -uo pipefail
#@@CLIENT_TIE@@
GPU=$1; BR=$2; SHA=$3; OUT=$4; mapfile -t ROWS < <(printf %s "$6" | base64 -d); ROWS=("${ROWS[@]%$'\r'}")   # $5 = cap0, $6 = base64 of the rows
echo "[box_run] lane=$BR branch=${7:-$BR} sha=$SHA"
L=/workspace/lanes/$BR; S=$L/src; I=$L/install; GOLD=/workspace/golden-install; MIRROR=/workspace/toolchains/mirror.git
#@@FRESH_FN@@
T0=$(date +%s); tick() { echo "[box_run] stage=$1 secs=$(( $(date +%s) - T0 )) total"; T0=$(date +%s); }
#@@LANE_FLOOR@@
[ -f /workspace/lanes/EVICTIONS.log ] && grep -F " removed $BR " /workspace/lanes/EVICTIONS.log | tail -1 | sed 's/^/[box_run] this lane dir was removed earlier: /'   # a lane that was evicted sees why
{   # new OR existing lane dir: below the floor, evict (branches merged into nifty first, then oldest; never this lane, running, queued in pueue, wanted or recent)
  LFLOOR=${MC2_BOX_LANE_FLOOR_GB:-30}; FREE_GB=$(df -BG --output=avail /workspace | tail -1 | tr -dc 0-9)
  if [ "${FREE_GB:-0}" -lt "$LFLOOR" ]; then
    bash /workspace/mc2-box/lane_cleanup.sh evict "$LFLOOR" "$BR"
    FREE_GB=$(df -BG --output=avail /workspace | tail -1 | tr -dc 0-9)
    if [ "${FREE_GB:-0}" -lt "$LFLOOR" ]; then echo "[box_run] BOX_INFRA: ${FREE_GB:-0} GB free on /workspace, floor $LFLOOR GB, nothing evictable (holders listed above): not running lane $BR"; exit 77; fi
  fi
}
#@@END_LANE_FLOOR@@
mkdir -p /workspace/locks $L $OUT
export I OUT S   # free-text rows (sh:/cpu:/gpu:) run in a child bash and use $I (the install) and $OUT (this run's report dir): unexported they were empty and `cd $I` went home
GOT=""; SB=""; WANTF=""; LWANT=/workspace/locks/$BR.want; trap '[ -n "${TK:-}" ] && rm -f "$TK"; rm -f "$LWANT"; [ -n "$WANTF" ] && rm -f "$WANTF"; [ -n "$SB" ] && rm -f /workspace/locks/$SB.owner /workspace/locks/$SB.row' EXIT   # a dead run must not leave a stale owner file
exec 8>$L/.lane.lock; flock 8   # one run per lane at a time: src, install and out are shared (fd 8 is not passed on to rows)
#@@DISK_GUARD@@
FREE_GB=$(df -BG --output=avail /workspace | tail -1 | tr -dc 0-9)
if [ "${FREE_GB:-0}" -lt "${MC2_BOX_MIN_FREE_GB:-15}" ]; then echo "[box_run] BOX_INFRA: ${FREE_GB:-0} GB free on /workspace, floor ${MC2_BOX_MIN_FREE_GB:-15} GB: not starting a build"; exit 77; fi
#@@END_DISK_GUARD@@
[ -f /workspace/locks/cores.env ] && . /workspace/locks/cores.env   # the one core map (BUILD_CPUS, GPU<n>_CPUS)
rm -f "$OUT"/rows.tsv "$OUT"/fingerprints.tsv   # both are append-only: a stale PASS from an earlier run at this sha must never come back
printf '%s\n' "${ROWS[@]}" > "$OUT/requested.tsv"
[ -d $GOLD ] || { echo "[box_run] no golden install: run 'py -3 tools/box_run.py --seed' once"; exit 3; }
BCPUS=${BUILD_CPUS:-8-$(( $(nproc) - 1 ))}   # builds: the dedicated BUILD_CPUS of cores.env at nice 19, one build at a time (bq_acquire), no GPU lock held (the lock only wraps game rows)
BUILD_J=$(bq_j "$BCPUS")
export GIT_LFS_SKIP_SMUDGE=1
# source: bundle was fetched into the mirror as refs/lanes/$BR by the Windows half
git -C $MIRROR worktree prune 2>/dev/null   # an evicted lane dir leaves a dangling worktree record that blocks `worktree add`
if [ -e $S/.git ]; then git -C $S checkout -q -f --detach $SHA; else git -C $MIRROR worktree add -q -f --detach $S $SHA; fi
[ -e $S/.git ] && [ "$(git -C $S rev-parse HEAD 2>/dev/null)" = "$SHA" ] || { echo "[box_run] BOX_INFRA: could not provision the lane source at $SHA ($S; was the lane dir evicted?)"; exit 77; }   # never a silent exit 0 with nothing run
tick source
ONLY_LINT=1; for r in "${ROWS[@]}"; do [ "$r" = lint ] || ONLY_LINT=""; done   # a lint-only run is pure Python + git: no configure, no build, no install
cd $S
build_install() {
#@@BUILD_SEM_USE@@
# build: shared ccache keyed without the directory so every lane hits the same objects
export CCACHE_BASEDIR=/workspace CCACHE_NOHASHDIR=1 CCACHE_SLOPPINESS=time_macros
cd $S
# MC2_IMGUI=ON: without it GuiRuntime is compiled out and every front-end screen renders black on the box (imgui_vk_ready=0); a tree configured before this reconfigures once.
grep -q "^MC2_IMGUI:BOOL=ON" build64-ninja/CMakeCache.txt 2>/dev/null || rm -f build64-ninja/build.ninja
[ -f build64-ninja/build.ninja ] || nice -n 19 taskset -c "$BCPUS" cmake -S . -B build64-ninja -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_CXX_FLAGS=-stdlib=libc++ -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
 -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DMC2_IMGUI=ON \
 -DVulkan_INCLUDE_DIR=/workspace/toolchains/vulkan/include -DMC2_DXC=/workspace/toolchains/dxc/bin/dxc -DMC2_SLANGC=/workspace/toolchains/slang/bin/slangc -DMC2_SPIRV_OPT=/workspace/toolchains/gfxr-1.4.350.0/bin/spirv-opt > $OUT/configure.log 2>&1 || { tail -20 $OUT/configure.log; exit 4; }
nice -n 19 taskset -c "$BCPUS" ninja -C build64-ninja -j$BUILD_J mc2 > $OUT/build.log 2>&1; rc=$?; bq_release; tail -2 $OUT/build.log
[ $rc = 0 ] || { echo "[box_run] BUILD FAILED rc=$rc (log: $OUT/build.log)"; exit 5; }
tick build
# install: link the big immutable data from the golden install, copy everything else, then deploy the lane build over it
if [ ! -d $I ]; then
  mkdir -p $I; cp -al $GOLD/data $I/ 2>/dev/null; rm -rf $I/data/missions $I/data/terrain_layers
  cp -a $GOLD/data/missions $I/data/ 2>/dev/null; [ -d $GOLD/data/terrain_layers ] && cp -a $GOLD/data/terrain_layers $I/data/
  for f in $GOLD/*; do b=$(basename $f); [ "$b" = data ] && continue; case $b in *.fst) cp -al $f $I/;; *) cp -a $f $I/;; esac; done
fi
# deploy_payload.py is Windows-shaped (SDL2.dll, the Windows game exe); on Linux the lane build is the binary plus its shaders
cp -f build64-ninja/mc2 $I/mc2 && chmod +x $I/mc2 || { echo "[box_run] DEPLOY FAILED"; exit 6; }
# optional payload: only what THIS tree built (a lane without crashpad has no crashpad_handler; deploy must never require it)
for opt in crashpad_handler; do [ -f build64-ninja/$opt ] && { cp -f build64-ninja/$opt $I/$opt && chmod +x $I/$opt || { echo "[box_run] DEPLOY FAILED: $opt"; exit 6; }; }; done
mkdir -p $I/shaders && cp -a $S/shaders/. $I/shaders/ && cp -a build64-ninja/shaders/. $I/shaders/ || { echo "[box_run] DEPLOY FAILED: shaders"; exit 6; }
# Use the tip's tooling for both arms, including when the before revision predates it.
git -C $S checkout -q -f --detach $SHA || exit 6
python3 -c 'from tools.box_run import write_payload_manifest; import sys; write_payload_manifest(sys.argv[1], sys.argv[2])' "$I" "$BUILD_SHA" || { echo "[box_run] DEPLOY FAILED: manifest"; exit 6; }
mkdir -p "$I/saved_bookmarks"
for bookmark in mc2_01_saved_205644 mc2_10_saved_204523; do
  cp "$S/tests/visual/bookmarks/$bookmark.json" "$I/saved_bookmarks/$bookmark.json" || exit 6
done
}
AB_BEFORE=""
for row in "${ROWS[@]}"; do case $row in ab:*) AB_BEFORE=$(git rev-parse --verify "${row#ab:}^{commit}") || exit 6;; esac; done
[ -z "$AB_BEFORE" ] || git merge-base --is-ancestor "$AB_BEFORE" "$SHA" || { echo "[box_run] AB REFUSED: before is not an ancestor of tip"; exit 6; }
BUILD_SHA=${AB_BEFORE:-$SHA}
if [ -z "$ONLY_LINT" ]; then
  git checkout -q -f --detach "$BUILD_SHA" || exit 6
  build_install
fi
tick install
export INSTALL=$I OUT
export TMPDIR=$L/tmp; mkdir -p $TMPDIR
grc() { nice -n 19 taskset -c "$BCPUS" "$@" 8>&-; }   # CPU rows (build, unit, cpu:, sh:): BUILD_CPUS, no GPU lock
take_lock() {   # taken lazily, just before the first row that runs the game
CAP0=${5:-0}; LW0=$SECONDS   # the 30 min wait is measured from here (the build already ran)
AVAILABLE=$(nvidia-smi -L | sed -n 's/^GPU \([0-9][0-9]*\):.*/\1/p' | tr '\n' ' ')
[ -n "$AVAILABLE" ] || { echo "[box_run] no GPUs discovered"; exit 77; }
if [ "$GPU" != auto ]; then
  case " $AVAILABLE " in *" $GPU "*) ;; *) echo "[box_run] GPU $GPU does not exist ($AVAILABLE)"; exit 77;; esac
fi
if [ "$GPU" = auto ]; then cand="$AVAILABLE"
  if [ "$CAP0" = 1 ]; then
    case " $AVAILABLE " in *" 0 "*) cand=0;; *) echo "[box_run] capture GPU 0 does not exist"; exit 77;; esac
  fi
elif [ "${MC2_GPU_STRICT:-0}" = 1 ]; then cand=$GPU
else cand=$GPU; for o in $AVAILABLE; do [ "$o" = "$GPU" ] && continue; [ "$CAP0" = 1 ] && [ "$o" != 0 ] && continue; cand="$cand $o"; done; fi
GOT=""
KS=${MC2_BOX_GPU_SLOTS:-${GPU_SLOTS:-3}}   # game slots per GPU (a counting semaphore of K flock files gpu<g>.s<k>; cores.env GPU_SLOTS, env MC2_BOX_GPU_SLOTS overrides); the gate GPU (GATE_GPU in cores.env) stays one exclusive slot (legacy file gpu<N>)
case $KS in ''|*[!0-9]*|0) KS=1;; esac
try_slot() {   # first free (gpu, slot) among the candidates: non-blocking, fd 9 keeps the winner's flock
  local c k
  for c in $cand; do
    if [ "$c" = "${GATE_GPU:-0}" ]; then exec 9>>/workspace/locks/gpu${GATE_GPU:-0}; flock -n 9 && { GOT=${GATE_GPU:-0}; SLOT=0; SB=gpu${GATE_GPU:-0}; return 0; }; continue; fi
    for k in $(seq 0 $((KS-1))); do exec 9>>/workspace/locks/gpu$c.s$k; flock -n 9 && { GOT=$c; SLOT=$k; SB=gpu$c.s$k; return 0; }; done
  done; return 1
}
try_slot
if [ -z "$GOT" ]; then
  g=${cand%% *}; WANTF=/workspace/locks/gpu$g.want; echo "[box_run] all of ($cand) busy; waiting on GPU $g (up to 30 min)"
  while [ -z "$GOT" ]; do   # retry every candidate; flock is not FIFO and other lanes also wait
    touch $WANTF $LWANT   # $LWANT: lane_cleanup never evicts this lane while it waits
    try_slot && break
    [ $((SECONDS - LW0)) -gt 1800 ] && { echo "[box_run] LOCK_TIMEOUT: gave up waiting for a GPU"; exit 76; }
    timeout 1 tail -f /dev/null
  done
fi
[ -n "$WANTF" ] && rm -f "$WANTF" "$LWANT"; WANTF=""
[ "$GPU" != auto ] && [ "$GPU" != "$GOT" ] && [ "${MC2_GPU_STRICT:-0}" = 1 ] && { echo "[box_run] GPU_MISMATCH: asked for GPU $GPU, locked $GOT"; exit 77; }
GPU=$GOT; echo "$$ $BR $SHA $(date -Is)" > /workspace/locks/$SB.owner
echo "[box_run] gpu=$GPU slot=$SLOT locked lock_wait_secs=$((SECONDS - LW0))"
N=$GPU; bus=$(nvidia-smi --query-gpu=pci.bus_id --format=csv,noheader -i "$N"); bus=${bus: -12}
export DRI_PRIME="pci-$(echo "$bus" | tr ':.' '__')" MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1
export MC2_VK_DEVICE=NVIDIA SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy OMP_NUM_THREADS=1 MC2_LOG=1 TMPDIR=$L/tmp
mkdir -p $TMPDIR; cv=GPU${N}_CPUS; CPUS=${!cv:-"$((4*N))-$((4*N+3))"}   # cores.env GPU<n>_CPUS, else 4 per GPU
CORES=(); for r in ${CPUS//,/ }; do for c in $(seq "${r%-*}" "${r#*-}"); do CORES+=("$c"); done; done
CORE=${CORES[$(( ${SLOT:-0} % ${#CORES[@]} ))]}   # one core per actual game run: the slot index picks it from this GPU's cores (owner 2026-11-10)
gr() { taskset -c "$CORE" xvfb-run -a -s "-screen 0 1920x1080x24" "$@" 9>&- 8>&-; }   # -a: free display picked per run, fd 9 (GPU lock) not inherited
}
# CPU rows first, then game rows: the GPU lock is only ever held around the game rows
# determinism:* and saveload* run the engine with --headless (no window, no device: gameos_graphics.cpp), so they are CPU rows too (r7 finding 3)
CR=(); GR=(); for r in "${ROWS[@]}"; do case $r in build|unit|unit-san|lint|cpu:*|sh:*|determinism:*|saveload*) CR+=("$r");; *) GR+=("$r");; esac; done; ROWS=("${CR[@]}" "${GR[@]}"); LOCKED=""
grh() { nice -n 19 taskset -c "$BCPUS" xvfb-run -a -s "-screen 0 640x480x24" "$@" 9>&- 8>&-; }   # headless engine rows: build cores, no GPU lock, private xvfb (SDL init only)
RES=0
FPPY='import sys, json, os
sys.path.insert(0, "tools")
import sim_determinism as sd
fp = sd.data_fingerprint(sys.argv[1])
p = sd.baseline_path(sys.argv[2])
want = json.load(open(p))["data_fingerprint"]["digest"] if os.path.exists(p) else None
print("digest=%s stock=%s baseline=%s problems=%d" % (fp["digest"], fp["stock_digest"], want, len(fp["problems"])))
sys.exit(7 if want and want != fp["digest"] else 0)'
RIDX=0
for row in "${ROWS[@]}"; do
  RIDX=$((RIDX+1))
  case $row in ab:*)
    ABOUT=$(fresh "$OUT/ab")
    marks=mc2_01_saved_205644,mc2_10_saved_204523
    take_lock
    gr timeout -k 30 600 python3 tools/visual_verify/capture.py --install "$I" --bookmarks "$marks" --out "$ABOUT/a" > "$OUT/ab_before.log" 2>&1; rc=$?
    gr timeout -k 30 1500 python3 tools/box_gpu_passes.py run --install "$I" --out "$ABOUT" --name gpu_passes_base --ref-commit "$AB_BEFORE" > "$OUT/ab_gpu_base.log" 2>&1; gprc=$?   # same lock, same arm: its own fresh log dir
    rm -f /workspace/locks/gpu$GOT.owner /workspace/locks/gpu$GOT.row; exec 9>&-; GOT=""; LOCKED=""
    BUILD_SHA=$SHA; build_install
    if [ "$rc" = 0 ]; then
      take_lock; LOCKED=1
      gr timeout -k 30 600 python3 tools/visual_verify/capture.py --install "$I" --bookmarks "$marks" --out "$ABOUT/b" > "$OUT/ab_after.log" 2>&1; rc=$?
      gr timeout -k 30 1500 python3 tools/box_gpu_passes.py run --install "$I" --out "$ABOUT" --name gpu_passes_tip --ref-commit "$SHA" > "$OUT/ab_gpu_tip.log" 2>&1; gprc2=$?; [ "$gprc" = 0 ] && gprc=$gprc2
      if [ "$rc" = 0 ]; then
        grc python3 tools/visual_verify/ab.py --compare-only "$ABOUT/a" "$ABOUT/b" --out-root "$ABOUT/compare" > "$OUT/ab_compare.log" 2>&1; rc=$?
      fi
    fi
    printf "%s\t%s\n" "$row" "$rc" >> "$OUT/rows.tsv"
    echo "[box_run] row=$row rc=$rc out=$ABOUT"
    if [ -f "$ABOUT/gpu_passes_base.json" ] && [ -f "$ABOUT/gpu_passes_tip.json" ]; then   # jsons next to rows.tsv, ratios in the run output
      cp "$ABOUT/gpu_passes_base.json" "$ABOUT/gpu_passes_tip.json" "$OUT/"
      grc python3 tools/box_gpu_passes.py compare "$OUT/gpu_passes_base.json" "$OUT/gpu_passes_tip.json" --out "$OUT/gpu_passes_ab.txt"
    else echo "[box_run] gpu-passes A/B: NOT MEASURED (a pass run did not finish; see ab_gpu_base.log / ab_gpu_tip.log)"; fi
    printf "%s\t%s\n" "ab-gpu-passes" "${gprc:-1}" >> "$OUT/rows.tsv"; [ "${gprc:-1}" = 0 ] || RES=1
    [ "$rc" = 0 ] || RES=1
    continue ;;
  esac
  name=$(printf %s "$row" | tr -c 'A-Za-z0-9._-' _); case $row in sh:*|cpu:*|gpu:*) name=row$RIDX-$(printf %s "$row" | cksum | cut -d' ' -f1);; esac; echo "[box_run] row=$row"   # free-text rows: index + hash, never the row text (File name too long)
  case $row in build|unit|unit-san|lint|cpu:*|sh:*) RUN=grc;; determinism:*|saveload*) RUN=grh;; *) RUN=gr; [ -z "$LOCKED" ] && { take_lock; LOCKED=1; };; esac
  case $row in unit-san) TMO=3000;; unit) TMO=1200;; lint) TMO=3600;; smoke*|capture:*|screen:*) TMO=600;; gpu-passes) TMO=1500;; determinism:*|saveload*) TMO=900;; *) TMO=1800;; esac   # a hung row must not hold a GPU or the lane
  case $row in
    smoke-lite)  QS=""; [ "$GOT" = "${GATE_GPU:-0}" ] && [ -x /workspace/bin/quiet-slot ] && QS="/workspace/bin/quiet-slot --gpu ${GATE_GPU:-0} --"   # gate GPU: pause and drain the idle filler first (its mc2 jobs are foreign engines that hold the game lease)
                 cmd="$QS python3 scripts/run_smoke.py --lite --keep-logs --exe $I/mc2" ;;
    smoke-tier1) cmd="python3 scripts/run_smoke.py --tier tier1 --keep-logs --exe $I/mc2" ;;
    screen:*)    sc=${row#screen:}; cmd="python3 scripts/screen_shot.py --screen $sc --exe $I/mc2 --out-dir $(fresh $OUT/$name)" ;;
    capture:*)   IFS=: read -r _ m b <<< "$row"; cmd="python3 scripts/run_visual_capture.py --mission $m --bookmarks tests/visual/bookmarks/$b.json --exe $I/mc2 --classic-assets --out-dir $(fresh $OUT/$name)" ;;
    determinism:*) m=${row#*:}; extra=""; ae=""; case $m in *:audit) m=${m%:audit}; ae="MC2_SIM_OVERLAP_AUDIT=12";; esac   # :audit = R0 overlap-audit arm: level 2 (render-owns fence) + 10 ms jitter; any [SIM_OVERLAP v1] VIOLATION line fails the row
                   [ "$m" = mc2_24 ] && extra="--ticks 2600"   # mc2_24: the finale spawns infantry after tick 2404 (as the PC gate)
                   cmd="$ae python3 tools/sim_determinism.py check $m --install $I --headless-both $extra --out $(fresh $OUT/$name)" ;;
    saveload*)   m=${row#saveload}; m=${m#:}; m=${m:-mc2_01}   # headless: a gate GPU has no present-capable surface; the save bytes equal the windowed ones
                 cmd="python3 tools/saveload_roundtrip.py check --mission $m --install $I --mask tools/save_noise_mask.json --first-save-only --headless --out $(fresh $OUT/$name) --save-dir $(fresh $OUT/$name.savegame)" ;;
    unit)        # doctest tier (tests/unit): its own configure with the box clang toolchain (a bare cmake finds no default compiler: "CMAKE_CXX_COMPILER not set")
                 cmd="grep -q '^CMAKE_HOME_DIRECTORY:INTERNAL=/' build64-tests/CMakeCache.txt 2>/dev/null || rm -rf build64-tests; cmake -S \$PWD/tests/unit -B \$PWD/build64-tests -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_CXX_FLAGS=-stdlib=libc++ -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache && nice -n 19 ninja -C build64-tests -j$BUILD_J -k 0 mc2_tests && cd build64-tests && { ./mc2_tests --no-breaks --test-case-exclude=\"\$(grep -v '^#' ../tests/unit/box_exclude.txt | sed 's/ *#.*//' | paste -sd,)\" -r=xml -o=unit.xml > unit_out.txt 2>&1; python3 ../tools/box/unit_report.py unit.xml unit_out.txt; }" ;;   # verdict = doctest XML (should_fail controls are counted as controls, never as red)
                 # cwd = the build dir: cmake emits sources relative to it, so a __FILE__-relative test finds its files only from there.
                 # tests/unit/box_exclude.txt: the cases that are data- or ABI-dependent on this box, each with its reason (tracked, not a ratchet row)
    lint)        cmd="python3 tools/lane_merge.py --lint HEAD" ;;   # CPU-only: lane_merge's own lint (exit code = verdict) against the mirror's claude/nifty-mendeleev
    unit-san)    # same doctest tier under clang ASan+UBSan in its OWN tree (build64-tests-san); UBSan recovers (all findings listed) and is counted from log_path files
                 SANF="-fsanitize=address,undefined -fno-sanitize=vptr -fsanitize-recover=address -fno-omit-frame-pointer"   # vptr off: the harness links stub subsets of the engine, so typeinfo for Mover/Appearance is never emitted and a vptr check only fails the link
                 cmd="cmake -S tests/unit -B build64-tests-san -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_CXX_FLAGS='-stdlib=libc++ $SANF' -DCMAKE_C_FLAGS='$SANF' -DCMAKE_EXE_LINKER_FLAGS='-fuse-ld=lld $SANF' -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache && nice -n 19 ninja -C build64-tests-san -j$BUILD_J -k 0 mc2_tests && cd build64-tests-san && rm -f \$OUT/ubsan.* \$OUT/asan.* && ASAN_OPTIONS=detect_leaks=0:halt_on_error=0:log_path=\$OUT/asan UBSAN_OPTIONS=print_stacktrace=1:log_path=\$OUT/ubsan ./mc2_tests --no-breaks; rc=\$?; n=\$(cat \$OUT/ubsan.* 2>/dev/null | grep -c 'runtime error'); a=\$(cat \$OUT/asan.* 2>/dev/null | grep -c 'ERROR: AddressSanitizer'); echo \"[san] rc=\$rc ubsan_findings=\$n asan_findings=\$a\"; [ \$rc = 0 ] && [ \$n = 0 ] && [ \$a = 0 ]" ;;
    gpu-passes)  cmd="python3 tools/box_gpu_passes.py run --install $I --out $OUT --name gpu_passes; rc=\$?; cat $OUT/gpu_passes.txt 2>/dev/null; exit \$rc" ;;   # fixed poses (mc2_01 day, mc2_10 night, mc2_24 battle) off-screen; its own fresh log dir; table in this row's log + $OUT/gpu_passes.json
    build)       cmd="true" ;;   # build-only: the build/install above already ran with no GPU lock; this row just records it
    sh:*|cpu:*)  cmd="${row#*:}" ;;   # CPU-only command: no GPU lock
    gpu:*)       cmd="${row#gpu:}" ;;   # arbitrary command that runs the game: takes the lock
    syncval*)    # SYNCVAL row type: Khronos sync validation as a standing gate. syncval (= tier1) | syncval:aa_off | :aa_fxaa | :aa_cmaa | :cold_bake | :nowater | :canary
                 # Every arm fails on any SYNC-HAZARD / severity=error line (run_smoke's vk_validation grade, MC2_VULKAN_VALIDATION=sync, layer_enabled required).
                 # :canary is the deliberate-bug row: it applies tests/syncval/canary_producer_handoff_barrier.patch, rebuilds, and PASSES only if the hazard is reported.
                 arm=${row#syncval}; arm=${arm#:}; arm=${arm:-tier1}; sv="MC2_VULKAN_VALIDATION=sync"
                 # Newer Khronos layer, side by side (the system layer is 1.3.275: no VK_EXT_layer_settings, no syncval extra properties, a semaphore-timeout false positive). Only these rows see it.
                 VVL=/workspace/toolchains/vvl-1.4.350.0/explicit_layer.d; vlp=""; [ -d $VVL ] && vlp="VK_LAYER_PATH=$VVL"; sv="$sv $vlp"
                 case $arm in
                   tier1)     cmd="$sv python3 scripts/run_smoke.py --tier tier1 --keep-logs --exe $I/mc2" ;;
                   aa_off)    cmd="$sv MC2_OPTIONS_OVERRIDE=SpatialAA=0 python3 scripts/run_smoke.py --lite --keep-logs --exe $I/mc2" ;;
                   aa_fxaa)   cmd="$sv MC2_OPTIONS_OVERRIDE=SpatialAA=2 python3 scripts/run_smoke.py --lite --keep-logs --exe $I/mc2" ;;
                   aa_cmaa)   cmd="$sv MC2_OPTIONS_OVERRIDE=SpatialAA=3 python3 scripts/run_smoke.py --lite --keep-logs --exe $I/mc2" ;;
                   cold_bake) rm -rf $OUT/coldhome; mkdir -p $OUT/coldhome   # a fresh $HOME is a fresh user dir, so no <userdir>/cache/shadow: the bake runs cold
                              cmd="$sv HOME=$OUT/coldhome XDG_CONFIG_HOME=$OUT/coldhome python3 scripts/run_smoke.py --lite --keep-logs --exe $I/mc2" ;;
                   imgui|resize) # direct launch (run_smoke does not pass MC2_IMGUI_* through and cannot resize); graded here: layer_enabled present, zero severity=error
                              envx=""; wm=""
                              [ $arm = imgui ] && envx="MC2_IMGUI_DEMO=1"
                              [ $arm = resize ] && { command -v xdotool >/dev/null || apt-get install -y -qq xdotool >/dev/null 2>&1; wm='( sleep 7; w=\$(xdotool search --onlyvisible --name . 2>/dev/null | tail -1); xdotool windowsize \$w 1100 700; sleep 2; xdotool windowsize \$w 1500 900 ) &'; }
                              cmd="cd $I && $wm env $sv $envx ./mc2 --profile stock --mission mc2_01 --duration 18 > $OUT/syncval_$arm.game.log 2>&1; g=\$?; grep -a 'VK_VALIDATION v1' $OUT/syncval_$arm.game.log | grep -c 'severity=error' | sed 's/^/[syncval] errors=/'; grep -aq 'id=layer_enabled' $OUT/syncval_$arm.game.log || { echo '[syncval] layer not live'; exit 1; }; ! grep -a 'VK_VALIDATION v1' $OUT/syncval_$arm.game.log | grep -aq 'severity=error' && [ \$g = 0 ]" ;;
                   nowater)   cmd="$sv python3 scripts/run_smoke.py --mission gaea_peaks_01 --allow-any-mission --keep-logs --exe $I/mc2" ;;
                   gpuav)     cmd="MC2_VULKAN_VALIDATION=gpu-assisted $vlp python3 scripts/run_smoke.py --mission mc2_01 --duration 30 --grace-s 900 --keep-logs --exe $I/mc2" ;;   # GPU-assisted validation run mode (slow: one mission; only this row raises the walltime cap, duration+grace)
                   canary)    cmd="git apply tests/syncval/canary_producer_handoff_barrier.patch && { ninja -C build64-ninja -j${BUILD_J:-16} mc2 > $OUT/canary_build.log 2>&1; b=\$?; git checkout -q -- GameOS/gameos/vk_native_producers.cpp; [ \$b = 0 ] || exit 9; cp -f build64-ninja/mc2 $I/mc2; $sv python3 scripts/run_smoke.py --lite --keep-logs --exe $I/mc2 > $OUT/canary_smoke.log 2>&1; grep -qa 'SYNC-HAZARD' $OUT/canary_smoke.log && { echo '[syncval] canary: hazard reported, as required'; exit 0; }; echo '[syncval] canary: NO hazard reported (validation is not live or the canary is stale)'; exit 1; }" ;;
                   *) echo "[box_run] unknown syncval arm $arm"; RES=2; continue ;;
                 esac ;;
    *) echo "[box_run] unknown row $row"; RES=2; continue ;;
  esac
  case $row in determinism:*|saveload*)
    # data-fingerprint precheck: refuse up front when the install's stock data is not the data the baseline pins (a full run would only reach that verdict at the end)
    fpm=${row#*:}; fpm=${fpm%:audit}; [ "$row" = saveload ] && fpm=mc2_01
    fpl=$(cd $S && python3 -c "$FPPY" "$I" "$fpm" 2>&1); fprc=$?
    echo "[box_run] data_fp row=$row $fpl"
    printf "%s\t%s\t%s\n" "$row" "$(sha256sum $I/mc2 | cut -c1-16)" "$fpl" >> $OUT/fingerprints.tsv
    if [ $fprc != 0 ]; then
      echo "[box_run] row=$row REFUSED: the install data fingerprint does not match the baseline's; fix the golden install, not the baseline" | tee $OUT/$name.log
      printf "%s\t%s\n" "$row" 7 >> $OUT/rows.tsv; RES=1; continue
    fi ;;
  esac
  [ -n "$GOT" ] && [ "$RUN" = gr ] && echo "$row" > /workspace/locks/$SB.row   # STATUS.json shows the row the GPU is on
  RMARK=$OUT/.rowmark; touch $RMARK
  case $row in unit|unit-san) bq_acquire;; esac   # unit builds queue behind every other build on the box
  $RUN timeout -k 30 $TMO bash -c 'cd "$1" && eval "$2"' _ "$S" "$cmd" > $OUT/$name.log 2>&1; rc=$?
  case $row in unit|unit-san) bq_release;; esac
  case $row in gpu:*|sh:*|cpu:*) game_not_run "$cmd" $rc "$OUT" "$RMARK" && { echo "[box_run] row=$row GAME_NOT_RUN: the command names mc2 but no engine log (VK_ENGINE_CONTEXT) was written; is \$I set and ./mc2 launched?"; rc=81; };; esac   # eval from argv: a row may carry spaces, quotes, slashes
  case $row in determinism:*|saveload*) [ $rc != 0 ] && grep -aq "baseline FAIL no baseline at\|check: no baseline for" $OUT/$name.log && rc=78;; esac   # NO_BASELINE: own verdict, never a plain FAIL
  echo "[box_run] row=$row rc=$rc"; grep -a "^| mc2_\|result=\|DEPLOY_FINGERPRINT\|FAIL\|PASS" $OUT/$name.log | head -8 | cut -c1-220
  [ "$row" = lint ] && grep -a "LINT FAIL" $OUT/$name.log | head -30 | cut -c1-300   # every red lint line comes back in the row summary
  printf "%s\t%s\n" "$row" "$rc" >> $OUT/rows.tsv
  [ $rc = 0 ] || RES=1
  tick "row:$name"
done
# keep run_smoke reports, drop the heavy per-run logs unless asked
[ -d $S/smoke_logs ] && cp -a $S/smoke_logs $OUT/ 2>/dev/null
grep -a -h "DEPLOY_FINGERPRINT\|BUILD_FINGERPRINT" $OUT/*.log 2>/dev/null | sort -u | head -3 | cut -c1-200
exit $RES
'''


_STDIO_KW = {"input", "stdin", "stdout", "stderr", "capture_output"}

REMOTE = REMOTE.replace("#@@FRESH_FN@@", FRESH_FN + BUILD_SEM + GAME_CHECK).replace("#@@CLIENT_TIE@@", CLIENT_TIE).replace("#@@BUILD_SEM_USE@@", "bq_acquire")


def run(cmd, **kw):
    return subprocess.run(cmd, **console_child.child_kwargs(redirected=bool(_STDIO_KW & kw.keys()), **kw))


def git(*a, repo):
    return subprocess.run(["git", "-C", str(repo), *a], capture_output=True, text=True, check=True, **console_child.child_kwargs(redirected=True)).stdout.strip()


def has_vk_headless(repo):
    """True when HEAD contains eac614635 (headless VK present): smoke and capture rows then run on any GPU, not only GPU 0"""
    return subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor", "eac614635", "HEAD"], capture_output=True, **console_child.child_kwargs(redirected=True)).returncode == 0


def probe():
    try:
        r = run(["ssh", "-o", "ConnectTimeout=10", *SSH[1:], "true"], capture_output=True, timeout=15)
    except (OSError, subprocess.SubprocessError):
        return 1
    return r.returncode


def seed(repo):
    """One-time: /workspace/golden-install (+terrain_layers, textures from the Windows golden) and /workspace/golden-material."""
    gw = Path(GOLDEN_WIN)
    tmp = Path(tempfile.mkdtemp(prefix="box_seed_"))
    tar = tmp / "tl.tar"
    import tarfile
    with tarfile.open(tar, "w") as tf:
        tf.add(gw / "data" / "terrain_layers", arcname="data/terrain_layers")
    run([*SCP, str(tar), f"{HOST}:/workspace/tl.tar"], check=True)
    sh = ("set -e; [ -d /workspace/golden-install ] || cp -a /workspace/gamerun/install /workspace/golden-install; "
          "cd /workspace/golden-install && tar -xf /workspace/tl.tar && rm /workspace/tl.tar; rm -rf cache mods; "
          "[ -d /workspace/golden-material ] || cp -a /workspace/gamerun/golden /workspace/golden-material; "
          "du -sh /workspace/golden-install /workspace/golden-material")
    run([*SSH, sh], check=True)
    tar.unlink(missing_ok=True); tmp.rmdir()


PUEUE = "/workspace/toolchains/pueue/pueue"   # queue: see box-game-runs.md "Queue"
# copy-back: only *.tsv, *.json and the last 200 lines of each log (the full logs stay on the box)
BACK = ("set -euo pipefail; cd {out}; mkdir -p .tails; for f in *.log; do if [ -f \"$f\" ]; then tail -n 200 \"$f\" > .tails/$f; fi; done; "
        "find . -type f \\( -name '*.tsv' -o -name '*.json' -o -name '*.png' -o -name '*.jpg' -o -path './.tails/*' \\) | tar cf - -T -")


def rows_b64(rows):
    return base64.b64encode("\n".join(rows).encode()).decode()


def ssh_out(cmd):
    return run([*SSH, cmd], capture_output=True, text=True).stdout


def gpu_ids():
    ids = re.findall(r"^GPU (\d+):", ssh_out("nvidia-smi -L"), re.MULTILINE)
    if not ids:
        raise RuntimeError("no GPUs discovered by nvidia-smi -L on the box")
    return ids


def pick_gpu():
    """Least queued+running tasks among GPUs that actually exist (ties: lowest index)."""
    ids = gpu_ids()
    out = ssh_out(f"for g in {' '.join(ids)}; do echo $g $({PUEUE} status -g gpu$g --json | jq '[.tasks[]|select(.status|tostring|test(\"Queued|Running\"))]|length'); done")
    load = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 2 and p[1].isdigit():
            load[p[0]] = int(p[1])
    return min(ids, key=lambda g: (load.get(g, 99), int(g)))


def local_reports(repo, sha8):
    """Where the fetched reports land: the proofs archive, never the worktree (a copy under <repo>/proofs gets committed)."""
    return Path(os.environ.get("MC2_BOX_PROOFS", "E:/mc2-archive-2026-10/proofs/box")) / Path(repo).resolve().name / sha8


def fetch_back(out_remote, local_out):
    # background-launch: allow ssh tar pull, the engine runs on the Linux box
    for stale in ("rows.tsv", "requested.tsv", "fingerprints.tsv"):
        (local_out / stale).unlink(missing_ok=True)
    r = subprocess.run([*SSH, BACK.format(out=out_remote)], capture_output=True, **console_child.child_kwargs(redirected=True))
    if r.returncode or not r.stdout:
        raise OSError(f"copy-back failed (ssh rc={r.returncode}, empty={not bool(r.stdout)})")
    with tarfile.open(fileobj=io.BytesIO(r.stdout)) as tf:
        tf.extractall(local_out, filter="data")


def task_state(tid):
    """(state, rc or None, label) of a pueue task; state is Queued/Running/Done/... or missing"""
    d = json.loads(ssh_out(f"{PUEUE} status --json") or "{}").get("tasks", {})
    t = d.get(str(tid))
    if not t:
        return "missing", None, ""
    st = t["status"]
    state, body = next(iter(st.items())) if isinstance(st, dict) else (st, None)
    # Pueue serializes Done as the result itself ("Success" or {"Failed": n}).
    # Accept the older nested fixture shape too, but never guess FAIL for an unknown result.
    res = body.get("result", body) if isinstance(body, dict) else body
    rc = None if state != "Done" else (0 if res == "Success" else res.get("Failed", RC_BOX_INFRA) if isinstance(res, dict) else RC_BOX_INFRA)
    return state, rc, t.get("label", "")


def row_codes(tsv):
    return [ln.rpartition("\t")[2].strip() for ln in tsv.read_text().splitlines()] if tsv.exists() else []


def verdict(rc, codes=()):
    """(name, exit code). PASS only for rc 0 with every row 0; LOCK_TIMEOUT and NO_BASELINE are distinct and NEVER green."""
    if rc == RC_LOCK_TIMEOUT:
        return "LOCK_TIMEOUT", RC_LOCK_TIMEOUT
    if rc == RC_BOX_INFRA:
        return "BOX_INFRA", RC_BOX_INFRA
    if not codes or (rc != 0 and all(c == "0" for c in codes)):
        return "BOX_INFRA", RC_BOX_INFRA
    if rc == 0 and all(c == "0" for c in codes):
        return "PASS", 0
    if str(RC_NO_BASELINE) in codes and all(c in ("0", str(RC_NO_BASELINE)) for c in codes):
        return "NO_BASELINE", 4
    return "FAIL", 1


def task_verdict(tid, repo, wait):
    """ONE line: box_run task=<id> state=<..> rc=<..> rows=<pass>/<total>. --wait blocks, then copies the reports back."""
    state, rc, rows, table, detail = "unknown", None, "", [], ""
    try:
        if wait:
            w = run([*SSH, f"{PUEUE} wait {tid} >/dev/null 2>&1"])
            if w.returncode:
                raise OSError(f"pueue wait failed rc={w.returncode}")
        state, rc, label = task_state(tid)
        parts = label.split(":")   # box_run:<lane>:<sha8>:<group>[:<branch>]
        name, code = ("", 2)
        if state == "Done":
            if len(parts) not in (4, 5) or parts[0] != "box_run":
                raise OSError("task has no box_run report label")
            local_out = local_reports(repo, parts[2])
            local_out.mkdir(parents=True, exist_ok=True)
            fetch_back(f"/workspace/lanes/{parts[1]}/out/{parts[2]}", local_out)
            tsv = local_out / "rows.tsv"
            table = [ln.rsplit("\t", 1) for ln in tsv.read_text().splitlines()] if tsv.exists() else []
            if any(len(r) != 2 or not r[1].isdigit() for r in table):
                raise OSError("invalid rows.tsv")
            v = [r[1] for r in table]
            rows = f" rows={sum(x == '0' for x in v)}/{len(v)} reports={local_out}"
            name, code = verdict(rc, v)
            requested = local_out / "requested.tsv"
            if requested.exists() and code not in (RC_LOCK_TIMEOUT, RC_BOX_INFRA):
                from collections import Counter
                missing = Counter(requested.read_text().splitlines()) - Counter(r[0] for r in table)
                if missing:
                    raise OSError(f"missing requested rows: {list(missing)}")
            if name == "BOX_INFRA":
                detail = "wrapper failed outside rows or row evidence missing"
        elif state == "missing" or wait:
            raise OSError(f"task did not complete: {state}")
    except (OSError, ValueError, tarfile.TarError, subprocess.SubprocessError) as exc:
        name, code, detail = "BOX_INFRA", RC_BOX_INFRA, str(exc)
    print(f"[box_run] task={tid} state={state} rc={'-' if rc is None else rc}{rows}" + (f" verdict={name} exit={code}" if name else "")
          + (f" detail={json.dumps(detail)}" if detail else "") + (f" table={json.dumps(table)}" if name else ""))
    return code


def refuse_dirty(rows):
    """Rows whose numbers would silently describe the committed state while the worktree differs."""
    return any(r == "gpu-passes" or r.startswith("ab:") for r in rows)


def lane_key(abbrev, sha, repo=None):
    """Reuse a fixed box lane for slot worktrees; retain legacy keys elsewhere."""
    if repo is not None:
        slot = re.fullmatch(r"mc2-slot(\d{2})", Path(repo).resolve().name, re.I)
        if slot:
            return f"box-lane-{slot.group(1)}"
    return f"detached-{sha[:8]}" if abbrev in ("", "HEAD") else abbrev.replace("/", "_")


def check_behind_box_fix(repo):
    """Cheap pre-submit warning: a tip that lacks the newest box-clang fix on nifty cannot build on Linux; fail fast on the PC, not after a push + build."""
    r = subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor", NEWEST_BOX_CLANG_FIX, "HEAD"], capture_output=True, **console_child.child_kwargs(redirected=True))
    if r.returncode == 1:
        print(f"[box_run] WARNING: HEAD does not contain {NEWEST_BOX_CLANG_FIX} (box clang fixes on nifty); the Linux build will probably fail. Merge claude/nifty-mendeleev first.", file=sys.stderr)
    return r.returncode


def box_has(sha):
    return run([*SSH, f"git -C {MIRROR} cat-file -e {sha}^{{commit}}"], capture_output=True).returncode == 0


def sync_nifty(repo):
    """The lint row diffs against the mirror's claude/nifty-mendeleev (ungated_tags, ratchets): point it at the local nifty tip, bundling only when the mirror lacks the commit."""
    try:
        nifty = git("rev-parse", "--verify", "-q", "claude/nifty-mendeleev^{commit}", repo=repo)
    except subprocess.CalledProcessError:
        nifty = ""
    if not nifty:
        print("[box_run] lint: no local claude/nifty-mendeleev; the mirror's copy is used as is", file=sys.stderr)
        return
    if not box_has(nifty):
        ref, bundle = "refs/box-run/nifty", Path(tempfile.gettempdir()) / "box_nifty.bundle"
        git("update-ref", ref, nifty, repo=repo)
        try:
            have = subprocess.run([*SSH, f"git -C {MIRROR} for-each-ref --format='%(objectname)'"], capture_output=True, text=True, **console_child.child_kwargs(redirected=True)).stdout.split()
            excl = ["^" + h for h in have if subprocess.run(["git", "-C", str(repo), "cat-file", "-e", h + "^{commit}"], capture_output=True, **console_child.child_kwargs(redirected=True)).returncode == 0]
            run(["git", "-C", str(repo), "bundle", "create", "-q", str(bundle), ref, *excl], check=True, capture_output=True)
            run([*SCP, str(bundle), f"{HOST}:/workspace/lanes_nifty.bundle"], check=True)
            run([*SSH, f"GIT_LFS_SKIP_SMUDGE=1 git -C {MIRROR} fetch -q -f /workspace/lanes_nifty.bundle +{ref}:refs/box-run/nifty && rm /workspace/lanes_nifty.bundle"], check=True)
        finally:
            bundle.unlink(missing_ok=True)
            git("update-ref", "-d", ref, repo=repo)
    run([*SSH, f"git -C {MIRROR} update-ref refs/heads/claude/nifty-mendeleev {nifty}"], check=True)


def box_status():
    """print /workspace/STATUS.json (written every 60 s by the filler loop); exit 0 when it is readable"""
    r = run([*SSH, "cat /workspace/STATUS.json"], capture_output=True, text=True)
    if r.returncode or not r.stdout.strip():
        print("[box_run] no STATUS.json on the box (filler loop not running?)", file=sys.stderr)
        return 1
    print(r.stdout.rstrip())
    ev = run([*SSH, "tail -n 15 /workspace/lanes/EVICTIONS.log 2>/dev/null"], capture_output=True, text=True)
    print("lane evictions (last 15, /workspace/lanes/EVICTIONS.log):\n" + ((ev.stdout or "").rstrip() or "  none"))
    return 0


def box_cleanup(branch, dry_run):
    """remove a lane on the box (dir, .box_lane script, mirror ref, worktree entry); refuses a lane a live process uses"""
    key = lane_key(branch, "0" * 40)
    r = run([*SSH, f"bash /workspace/mc2-box/lane_cleanup.sh one {key}" + (" --dry-run" if dry_run else "")], capture_output=True, text=True)
    print((r.stdout or "").rstrip() or (r.stderr or "").rstrip())
    return r.returncode


def deploy_box(revert=None):
    """upload tools/box/ and run its deploy.sh (apply, or revert [TS]); every replaced box file is backed up first"""
    src = Path(__file__).resolve().parent / "box"
    if revert is None:
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as tf:
            tf.add(src, arcname=".", filter=lambda ti: (setattr(ti, "uid", 0), setattr(ti, "gid", 0), ti)[2])
        r = run([*SSH_STDIN, "rm -rf /workspace/mc2-box.incoming && mkdir -p /workspace/mc2-box.incoming && tar xf - -C /workspace/mc2-box.incoming && bash /workspace/mc2-box.incoming/deploy.sh apply"], input=buf.getvalue())
        return r.returncode
    return run([*SSH, f"bash /workspace/mc2-box.incoming/deploy.sh revert {revert}".rstrip()]).returncode


def main():
    ap =argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gpu", default="auto", help="existing GPU index, or auto (discover GPUs with nvidia-smi -L; queue when busy)")
    ap.add_argument("--capture-any-gpu", action="store_true", help="allow capture and smoke rows on any discovered GPU (requires headless-present support)")
    ap.add_argument("--gate", action="store_true", help="merge gate rows and queue priority, using available GPUs")
    ap.add_argument("--seed", action="store_true", help="one-time golden install seeding")
    ap.add_argument("--probe", action="store_true", help="exit 0 if the box answers ssh within 15 s (merge_gate picks the box route or the Windows-full fallback with it)")
    ap.add_argument("--queue", action="store_true", help="submit to the box's pueue group (gpuN, or gate with --gate) and print the task id at once instead of blocking")
    ap.add_argument("--wait", metavar="ID", help="wait, fetch reports and print row table: 0 PASS, 1 row FAIL, 4 NO_BASELINE, 76 LOCK_TIMEOUT, 77 BOX_INFRA")
    ap.add_argument("--status", metavar="ID", nargs="?", const="", help="with ID: ONE state line for queued task ID (exit 0 done+ok, 1 failed, 2 queued/running); with no ID: print the box's STATUS.json (GPU lock holders, load, free disk)")
    ap.add_argument("--gpu-strict", action="store_true", help="wait for --gpu N only; otherwise fall back to another discovered GPU")
    ap.add_argument("--cleanup", metavar="BRANCH", help="remove BRANCH's lane on the box (dir, runner script, mirror ref, worktree entry); refuses a lane a live process uses")
    ap.add_argument("--dry-run", action="store_true", help="print run paths without contacting the box; with --cleanup: print removals")
    ap.add_argument("--deploy-box", action="store_true", help="install tools/box/ on the box (backs up what it replaces); --revert [TS] undoes it")
    ap.add_argument("--revert", metavar="TS", nargs="?", const="", help="with --deploy-box: restore the backup TS (default newest)")
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parents[1]))
    ap.add_argument("rows", nargs="*")
    a = ap.parse_args([x for x in sys.argv[1:] if x != "--"])
    repo = Path(a.repo)
    if a.probe:
        return probe()
    if a.cleanup:
        return box_cleanup(a.cleanup, a.dry_run)
    if a.deploy_box:
        return deploy_box(a.revert)
    if a.status == "":
        return box_status()
    if a.wait or a.status:
        return task_verdict(a.wait or a.status, repo, wait=bool(a.wait))
    if a.seed:
        return seed(repo)
    if a.gate and not a.rows:
        a.rows = GATE_ROWS
    if not a.rows:
        ap.error("give at least one row, e.g. smoke-lite")
    if any(r.startswith("ab:") for r in a.rows) and len(a.rows) != 1:
        ap.error("ab:<before-ref> runs alone (builds and captures both arms)")
    dirty = git("status", "--porcelain", "--untracked-files=no", repo=repo)
    if dirty and refuse_dirty(a.rows):
        print("[box_run] REFUSED: the gpu-passes / ab: rows measure the committed tip only and the worktree has uncommitted changes (the box would time the committed state, not yours). Commit first:\n" + dirty[:600], file=sys.stderr)
        return 2
    if dirty:
        print("[box_run] warning: uncommitted changes are NOT sent; the box runs HEAD", file=sys.stderr)
    sha = git("rev-parse", "HEAD", repo=repo)
    abbrev = git("rev-parse", "--abbrev-ref", "HEAD", repo=repo)
    identity = lane_key(abbrev, sha)
    branch = lane_key(abbrev, sha, repo)
    if a.dry_run:
        print(f"[box_run] lane=/workspace/lanes/{branch} branch={abbrev} sha={sha} rows={a.rows} reports={local_reports(repo, sha[:8])}")
        return 0
    check_behind_box_fix(repo)
    t = {}
    t0 = time.time()
    # bundle: exclude every commit the mirror already has
    have = subprocess.run([*SSH, f"git -C {MIRROR} for-each-ref --format='%(objectname)'"], capture_output=True, text=True, **console_child.child_kwargs(redirected=True)).stdout.split()
    excl = []
    for h in have:
        if subprocess.run(["git", "-C", str(repo), "cat-file", "-e", h + "^{commit}"], capture_output=True, **console_child.child_kwargs(redirected=True)).returncode == 0:
            excl.append("^" + h)
    ref = f"refs/box-run/{branch}"
    git("update-ref", ref, sha, repo=repo)
    bundle = Path(tempfile.gettempdir()) / f"box_{branch}.bundle"
    present = sha in have or box_has(sha)   # also true when sha is an ancestor of the mirror refs: an excluded-everything bundle would be empty
    if not present:
        try:
            p = run(["git", "-C", str(repo), "bundle", "create", "-q", str(bundle), ref, *excl], capture_output=True, text=True)
            if p.returncode:
                print(f"[box_run] git bundle create failed rc={p.returncode}: {(p.stderr or p.stdout).strip()[-400:]}", file=sys.stderr)
                present = box_has(sha)   # nothing to send is fine only when the box already has the commit
                if not present:
                    git("update-ref", "-d", ref, repo=repo)
                    return 1
            else:
                run([*SCP, str(bundle), f"{HOST}:/workspace/lanes_{branch}.bundle"], check=True)
        finally:
            bundle.unlink(missing_ok=True)
    git("update-ref", "-d", ref, repo=repo)
    if present:   # mirror already has this commit: just point the lane ref at it
        run([*SSH, f"git -C {MIRROR} update-ref refs/lanes/{branch} {sha}"], check=True)
    else:   # delete + '+' force: a stale refs/lanes/<lane> (rewound or unrelated history) must never break the fetch
        run([*SSH, f"mkdir -p /workspace/lanes && git -C {MIRROR} update-ref -d refs/lanes/{branch}; GIT_LFS_SKIP_SMUDGE=1 git -C {MIRROR} fetch -q -f /workspace/lanes_{branch}.bundle "
                   f"+{ref}:refs/lanes/{branch} && rm /workspace/lanes_{branch}.bundle"], check=True)
    t["bundle+push"] = time.time() - t0
    if "lint" in a.rows or any(r.startswith("ab:") for r in a.rows):
        sync_nifty(repo)
    # gpu choice
    # TODO: drop cap0 (smoke/capture rows forced onto GPU 0) once claude/vk-headless-present (eac614635) is in the base
    cap0 = 1 if (any(r.startswith(("capture", "smoke", "screen", "gpu-passes")) for r in a.rows) and not a.capture_any_gpu and not has_vk_headless(repo)) else 0
    gpu = a.gpu
    if gpu != "auto" and not gpu.isdigit():
        print(f"[box_run] --gpu must be an index or auto, got {gpu!r}")
        return 2
    out_remote = f"/workspace/lanes/{branch}/out/{sha[:8]}"
    local_out = local_reports(repo, sha[:8])
    local_out.mkdir(parents=True, exist_ok=True)
    if a.gate:   # a receipt must come from THIS run, never a leftover of an earlier run at the same sha
        for stale in ("rows.tsv", "fingerprints.tsv", "gate_receipt.json"):
            (local_out / stale).unlink(missing_ok=True)
    # one ssh: the remote script arrives on stdin (cat > file) and runs at once; rows travel as one base64 argument
    script = f"/workspace/lanes/.box_lane_{branch}.sh"
    runner = "env MC2_GPU_STRICT=1 bash" if a.gpu_strict else "bash"
    if os.environ.get("MC2_BOX_GPU_SLOTS", "").isdigit():   # K override for this run only (the box default is GPU_SLOTS in cores.env)
        runner = f"env MC2_BOX_GPU_SLOTS={os.environ['MC2_BOX_GPU_SLOTS']} {runner}"   # a busy --gpu N falls back to auto unless --gpu-strict
    if a.queue:   # submit to the pueue group and return the task id; --wait/--status give the verdict later
        if gpu == "auto":
            if cap0:
                if "0" not in gpu_ids():
                    print("[box_run] capture GPU 0 does not exist on the box")
                    return 2
                gpu = "0"
            else:
                gpu = pick_gpu()
        elif any(x not in NO_GPU_ROWS for x in a.rows) and gpu not in gpu_ids():
            print(f"[box_run] GPU {gpu} does not exist on the box")
            return 2
        grp = "gate" if a.gate else f"gpu{gpu}"
        if all(x in NO_GPU_ROWS for x in a.rows):
            grp = "build"   # build-only / unit-only: never touches a GPU, so it queues in the build group and holds no gpuN slot
        label = f"box_run:{branch}:{sha[:8]}:{grp}" + (f":{identity}" if identity != branch else "")
        after = ""
        if grp != "build":   # GPU rows: build first in the build group (BUILD_CPUS), then the GPU task, which finds the tree built and only takes the lock for the rows
            rb = run([*SSH_STDIN, f"cat > {script}.new.$$ && mv -f {script}.new.$$ {script} && {PUEUE} add -g build --print-task-id -l box_run:{branch}:{sha[:8]}:build:{identity} -- bash {script} {gpu} {branch} {sha} {out_remote} {cap0} {rows_b64(['build'])} {identity}"],
                     input=REMOTE.encode(), capture_output=True)
            bt = (rb.stdout.decode(errors="replace") if isinstance(rb.stdout, bytes) else rb.stdout).strip().splitlines()
            if rb.returncode == 0 and bt and bt[-1].isdigit():
                after = f"-a {bt[-1]} "
        r = run([*SSH_STDIN, f"cat > {script}.new.$$ && mv -f {script}.new.$$ {script} && {PUEUE} add -g {grp} {after}--print-task-id -l {label} -- {runner} {script} {gpu} {branch} {sha} {out_remote} {cap0} {rows_b64(a.rows)} {identity}"],
                input=REMOTE.encode(), capture_output=True)   # bytes: text mode would turn LF into CRLF on Windows and bash chokes on the CR
        out = r.stdout.decode(errors="replace") if isinstance(r.stdout, bytes) else r.stdout
        err = r.stderr.decode(errors="replace") if isinstance(r.stderr, bytes) else r.stderr
        tid = out.strip().splitlines()[-1] if r.returncode == 0 and out.strip() else ""
        print(f"[box_run] queued task={tid} group={grp} sha={sha[:12]} reports={local_out}" if tid else f"[box_run] queue submit FAILED rc={r.returncode} {err.strip()[-200:]}")
        return 0 if tid else 1
    t1 = time.time()
    r = run([*SSH_STDIN, f"cat > {script}.new.$$ && mv -f {script}.new.$$ {script} && {runner} {script} {gpu} {branch} {sha} {out_remote} {cap0} {rows_b64(a.rows)} {identity}"], input=REMOTE.encode(), stderr=subprocess.PIPE)
    t["remote (build+install+rows)"] = time.time() - t1
    err_bytes = getattr(r, "stderr", None) or b""
    err_text = err_bytes.decode(errors="replace") if isinstance(err_bytes, bytes) else str(err_bytes)
    if err_text:
        sys.stderr.write(err_text)   # stderr was captured only to name the cause on a BOX_INFRA line; show it as before
    first_err = next((ln.strip() for ln in err_text.splitlines() if ln.strip()), "")
    t2 = time.time()
    fetch_back(out_remote, local_out)
    t["copy-back"] = time.time() - t2
    print(f"[box_run] gpu={gpu} (see run log for the one taken) sha={sha[:12]} rc={r.returncode} reports={local_out}")
    print("[box_run] timings: " + ", ".join(f"{k}={v:.0f}s" for k, v in t.items()) + f", total={time.time() - t0:.0f}s")
    name, code = verdict(r.returncode, row_codes(local_out / "rows.tsv"))
    print(f"[box_run] verdict={name}")
    if a.gate:
        return gate_verdict(write_receipt(local_out, sha, identity, a.rows, r.returncode), first_err)
    return code


def gate_verdict(rec, first_err=""):
    """merge_gate's box stage exit code: 0 every row PASS; 1 a row FAILed (a red gate row, never waived); 3 BOX_INFRA (no
    row verdicts, or rows that never ran: the box failed, not the product, and merge_gate falls back to the Windows rows)."""
    rows = rec["rows"]
    for r in rows:
        print(f"[box_run] gate row {r['row']}: {r['verdict']} exe_sha256_16={r.get('exe_sha256_16', '-')} data_fp={r.get('data_fingerprint', '-')}")
    if not rows or any(r["verdict"] in ("NOT RUN", "LOCK_TIMEOUT") for r in rows):   # LOCK_TIMEOUT: never ran, not green; the box was busy, so the PC rows stand in
        print("BOX_INFRA: " + ", ".join(r["row"] for r in rows if r["verdict"] in ("NOT RUN", "LOCK_TIMEOUT")) + " produced no verdict (remote run died or timed out on the GPU lock before them)" + (f"; first remote stderr line: {first_err[:300]}" if first_err else ""))
        return 3
    return 0 if all(r["verdict"] == "PASS" for r in rows) else 1


def write_receipt(local_out, sha, branch, rows, rc):
    """Advisory clang-gate receipt: sha, rows with verdicts, the box build fingerprint. The PC gate keeps the MSVC tier1."""
    import json
    verdicts = {}
    tsv = local_out / "rows.tsv"
    if tsv.exists():
        for line in tsv.read_text().splitlines():
            row, _, code = line.rpartition("\t")
            c = code.strip()
            verdicts[row] = "PASS" if c == "0" else "NO_BASELINE" if c == str(RC_NO_BASELINE) else f"FAIL(rc={c})"
    # determinism/saveload rows print no build fingerprint: the remote half records the exe sha and the install data fingerprint per row
    row_fp = {}
    fps = local_out / "fingerprints.tsv"
    if fps.exists():
        for line in fps.read_text().splitlines():
            row, _, rest = line.partition("\t")
            exe, _, data = rest.partition("\t")
            row_fp[row] = {"exe_sha256_16": exe, "data_fingerprint": data}
    fp = ""
    locked_gpu = None
    for lg in sorted(local_out.glob("*.log")):
        content = lg.read_text(errors="replace")
        locked = re.search(r"\[box_run\] gpu=(\d+) locked", content)
        if locked:
            locked_gpu = int(locked.group(1))
        for ln in content.splitlines():
            if "DEPLOY_FINGERPRINT" in ln or "BUILD_FINGERPRINT" in ln:
                fp = ln.strip()[:300]
                break
        if fp:
            break
    rec = {"kind": "box-clang-gate-advisory", "sha": sha, "branch": branch, "gpu": locked_gpu, "toolchain": "clang-20 + libc++ + lld, RelWithDebInfo, ccache",
           "rows": [{"row": r, "verdict": verdicts.get(r, "LOCK_TIMEOUT" if rc == RC_LOCK_TIMEOUT else "NOT RUN"), **row_fp.get(r, {})} for r in rows], "fingerprint": fp, "script_rc": rc,
           "note": "advisory; box ms are ratio-only; the PC gate keeps the MSVC tier1"}
    out = local_out / "gate_receipt.json"
    out.write_text(json.dumps(rec, indent=2))
    print(f"[box_run] gate receipt: {out}")
    return rec


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (subprocess.CalledProcessError, OSError, tarfile.TarError) as exc:
        print(f"BOX_INFRA: box_run could not reach or drive the box: {exc}")
        sys.exit(3 if "--gate" in sys.argv else RC_BOX_INFRA)
