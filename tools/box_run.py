#!/usr/bin/env python3
"""Run game rows for the CURRENT worktree HEAD on the rented Linux box (4x RTX 3060).

    py -3 tools/box_run.py --gpu auto -- smoke-lite
    py -3 tools/box_run.py --gpu 1 -- smoke-lite capture:mc2_01:mc2_01_saved_205644 determinism:mc2_10
    py -3 tools/box_run.py --seed          # once: build the golden install on the box (needs the Windows golden)

Steps (each timed, summary printed at the end): lock a GPU, git-bundle HEAD to the box mirror, clang build in
/workspace/lanes/<branch>/src (ccache, -j16, pinned to the GPU's 8 cores), install = hardlink copy of the golden install
(big immutable data linked, the rest copied) + deploy_payload, run the rows, copy reports back to
<worktree>/proofs/box/<sha8>/, print the DEPLOY_FINGERPRINT line, release the lock.

Rows: smoke-lite | smoke-tier1 | syncval[:aa_off|aa_fxaa|aa_cmaa|cold_bake|imgui|resize|nowater|gpuav|canary] | capture:<mission>:<bookmark-stem> | determinism:<mission> | saveload[:<mission>] | sh:<command>
(sh runs in the lane src dir with $INSTALL, $OUT, $GPU_ENV set). GPU 3 is reserved for merge gates (--gate). GPUs 1-3 cannot
do screenshot captures yet (VK present init fails there): capture rows are forced onto GPU 0 by `--gpu auto`.
Windows-only behaviour (window focus, firewall, owner-test) is not covered: those rows stay on the Windows PC.
"""
import argparse, os, subprocess, sys, tempfile, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import console_child  # noqa: E402

KEY = os.environ.get("MC2_BOX_KEY", "C:/SPB_Data/.ssh/mc2farm_ed25519")
HOST = os.environ.get("MC2_BOX_HOST", "root@14.227.95.149")
PORT = os.environ.get("MC2_BOX_PORT", "30598")
MIRROR = "/workspace/toolchains/mirror.git"
GOLDEN_WIN = "A:/Games/mc2-opengl/mc2-win64-v0.4c"
SSH = ["ssh", "-i", KEY, "-p", PORT, "-o", "LogLevel=ERROR", "-o", "ServerAliveInterval=30", HOST]
SCP = ["scp", "-q", "-i", KEY, "-P", PORT]
# `--gate` with no rows: the headless determinism rows the PC gate runs plus the two saveload pins (tests/*/linux baselines)
GATE_ROWS = [f"determinism:mc2_{n}" for n in ("01", "03", "10", "16", "24")] + ["saveload:mc2_01", "saveload:mc2_10"]

REMOTE = r'''#!/bin/bash
# box_run.py remote half: args GPU BRANCH SHA OUTDIR ; rows on stdin (one per line)
set -uo pipefail
GPU=$1; BR=$2; SHA=$3; OUT=$4; mapfile -t ROWS; ROWS=("${ROWS[@]%$'\r'}")
L=/workspace/lanes/$BR; S=$L/src; I=$L/install; GOLD=/workspace/golden-install; MIRROR=/workspace/toolchains/mirror.git
T0=$(date +%s); tick() { echo "[box_run] stage=$1 secs=$(( $(date +%s) - T0 )) total"; T0=$(date +%s); }
mkdir -p /workspace/locks $L $OUT
# GPU lock = flock on /workspace/locks/gpuN (the idle filler's convention; released when this script exits, no stale locks).
# While waiting, gpuN.want is touched so the filler starts no new job there.
CAP0=${5:-0}; WANTF=""
trap '[ -n "$WANTF" ] && rm -f "$WANTF"' EXIT
if [ "$GPU" = auto ]; then cand="0 1 2"; [ "$CAP0" = 1 ] && cand=0; else cand=$GPU; fi
GOT=""
for g in $cand; do exec 9>>/workspace/locks/gpu$g; flock -n 9 && { GOT=$g; break; }; done
if [ -z "$GOT" ]; then
  g=${cand%% *}; WANTF=/workspace/locks/gpu$g.want; echo "[box_run] all of ($cand) busy; waiting on GPU $g (up to 30 min)"
  while [ -z "$GOT" ]; do   # retry every candidate; flock is not FIFO and other lanes also wait
    touch $WANTF
    for c in $cand; do exec 9>>/workspace/locks/gpu$c; flock -n 9 && { GOT=$c; break; }; done
    [ -n "$GOT" ] && break
    [ $SECONDS -gt 1800 ] && { echo "[box_run] gave up waiting for a GPU"; exit 75; }
    timeout 3 tail -f /dev/null
  done
fi
[ -n "$WANTF" ] && rm -f "$WANTF"; WANTF=""
GPU=$GOT; echo "$$ $BR $SHA $(date -Is)" > /workspace/locks/gpu$GPU.owner
echo "[box_run] gpu=$GPU locked"
[ -d $GOLD ] || { echo "[box_run] no golden install: run 'py -3 tools/box_run.py --seed' once"; exit 3; }
N=$GPU; bus=$(nvidia-smi --query-gpu=pci.bus_id --format=csv,noheader -i "$N"); bus=${bus: -12}
export DRI_PRIME="pci-$(echo "$bus" | tr ':.' '__')" MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1
export MC2_VK_DEVICE=NVIDIA SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy OMP_NUM_THREADS=1 MC2_LOG=1 TMPDIR=$L/tmp
mkdir -p $TMPDIR; CPUS="$((8*N))-$((8*N+7))"
gr() { taskset -c "$CPUS" xvfb-run -a -s "-screen 0 1920x1080x24" "$@" 9>&-; }   # -a: free display picked per run, fd 9 (GPU lock) not inherited
export GIT_LFS_SKIP_SMUDGE=1
# source: bundle was fetched into the mirror as refs/lanes/$BR by the Windows half
if [ -e $S/.git ]; then git -C $S checkout -q -f --detach $SHA; else git -C $MIRROR worktree add -q -f --detach $S $SHA; fi
tick source
# build: shared ccache keyed without the directory so every lane hits the same objects
export CCACHE_BASEDIR=/workspace/lanes CCACHE_NOHASHDIR=1 CCACHE_SLOPPINESS=time_macros
cd $S
[ -f build64-ninja/build.ninja ] || taskset -c "$CPUS" cmake -S . -B build64-ninja -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_CXX_FLAGS=-stdlib=libc++ -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
 -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
 -DVulkan_INCLUDE_DIR=/workspace/toolchains/vulkan/include -DMC2_DXC=/workspace/toolchains/dxc/bin/dxc -DMC2_SLANGC=/workspace/toolchains/slang/bin/slangc -DMC2_SPIRV_OPT=/workspace/toolchains/gfxr-1.4.350.0/bin/spirv-opt > $OUT/configure.log 2>&1 || { tail -20 $OUT/configure.log; exit 4; }
taskset -c "$CPUS" ninja -C build64-ninja -j16 mc2 > $OUT/build.log 2>&1; rc=$?; tail -2 $OUT/build.log
[ $rc = 0 ] || { echo "[box_run] BUILD FAILED rc=$rc (log: $OUT/build.log)"; exit 5; }
tick build
# install: link the big immutable data from the golden install, copy everything else, then deploy the lane build over it
if [ ! -d $I ]; then
  mkdir -p $I; cp -al $GOLD/data $I/ 2>/dev/null; rm -rf $I/data/missions $I/data/terrain_layers
  cp -a $GOLD/data/missions $I/data/ 2>/dev/null; [ -d $GOLD/data/terrain_layers ] && cp -a $GOLD/data/terrain_layers $I/data/
  for f in $GOLD/*; do b=$(basename $f); [ "$b" = data ] && continue; case $b in *.fst) cp -al $f $I/;; *) cp -a $f $I/;; esac; done
fi
# deploy_payload.py is Windows-shaped (SDL2.dll, mc2.exe); on Linux the lane build is the binary plus its shaders
cp -f build64-ninja/mc2 $I/mc2 && chmod +x $I/mc2 || { echo "[box_run] DEPLOY FAILED"; exit 6; }
mkdir -p $I/shaders && cp -a $S/shaders/. $I/shaders/ && cp -a build64-ninja/shaders/. $I/shaders/
tick install
export INSTALL=$I OUT
RES=0
FPPY='import sys, json, os
sys.path.insert(0, "tools")
import sim_determinism as sd
fp = sd.data_fingerprint(sys.argv[1])
p = sd.baseline_path(sys.argv[2])
want = json.load(open(p))["data_fingerprint"]["digest"] if os.path.exists(p) else None
print("digest=%s stock=%s baseline=%s problems=%d" % (fp["digest"], fp["stock_digest"], want, len(fp["problems"])))
sys.exit(7 if want and want != fp["digest"] else 0)'
for row in "${ROWS[@]}"; do
  name=${row//[:\/ ]/_}; echo "[box_run] row=$row"
  case $row in
    smoke-lite)  cmd="python3 scripts/run_smoke.py --lite --keep-logs --exe $I/mc2" ;;
    smoke-tier1) cmd="python3 scripts/run_smoke.py --tier tier1 --keep-logs --exe $I/mc2" ;;
    capture:*)   IFS=: read -r _ m b <<< "$row"; cmd="python3 scripts/run_visual_capture.py --mission $m --bookmarks tests/visual/bookmarks/$b.json --exe $I/mc2 --classic-assets --out-dir $OUT/$name" ;;
    determinism:*) m=${row#*:}; extra=""; [ "$m" = mc2_24 ] && extra="--ticks 2600"   # mc2_24: the finale spawns infantry after tick 2404 (as the PC gate)
                   cmd="python3 tools/sim_determinism.py check $m --install $I --headless-both $extra --out $OUT/$name" ;;
    saveload*)   m=${row#saveload}; m=${m#:}; m=${m:-mc2_01}   # headless: a gate GPU has no present-capable surface; the save bytes equal the windowed ones
                 cmd="python3 tools/saveload_roundtrip.py check --mission $m --install $I --mask tools/save_noise_mask.json --first-save-only --headless --out $OUT/$name --save-dir $OUT/$name.savegame" ;;
    sh:*)        cmd="${row#sh:}" ;;
    syncval*)    # SYNCVAL row type: Khronos sync validation as a standing gate. syncval (= tier1) | syncval:aa_off | :aa_fxaa | :aa_cmaa | :cold_bake | :nowater | :canary
                 # Every arm fails on any SYNC-HAZARD / severity=error line (run_smoke's vk_validation grade, MC2_VULKAN_VALIDATION=sync, layer_enabled required).
                 # :canary is the deliberate-bug row: it applies tests/syncval/canary_producer_handoff_barrier.patch, rebuilds, and PASSES only if the hazard is reported.
                 arm=${row#syncval}; arm=${arm#:}; arm=${arm:-tier1}; sv="MC2_VULKAN_VALIDATION=sync"
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
                   gpuav)     # direct launch: run_smoke caps a run at duration+120 s and GPU-assisted load alone exceeds that; graded here (layer live, canary fired, zero severity=error)
                              cmd="cd $I && MC2_VULKAN_VALIDATION=gpu-assisted timeout 480 ./mc2 --profile stock --mission mc2_01 --duration 20 > $OUT/syncval_gpuav.game.log 2>&1; g=\$?; L=$OUT/syncval_gpuav.game.log; grep -a 'VK_VALIDATION v1' \$L | grep -a 'severity=error' | grep -ao 'id=[A-Za-z0-9_.-]*' | sort | uniq -c | sed 's/^/[gpuav] error /'; grep -aq 'id=layer_enabled' \$L || { echo '[gpuav] layer not live'; exit 1; }; grep -aq 'id=canary_fired' \$L || { echo '[gpuav] canary missed'; exit 1; }; ! grep -a 'VK_VALIDATION v1' \$L | grep -aq 'severity=error' && [ \$g = 0 ]" ;;   # GPU-assisted validation run mode (slow: one mission)
                   canary)    cmd="git apply tests/syncval/canary_producer_handoff_barrier.patch && { ninja -C build64-ninja -j16 mc2 > $OUT/canary_build.log 2>&1; b=\$?; git checkout -q -- GameOS/gameos/vk_native_producers.cpp; [ \$b = 0 ] || exit 9; cp -f build64-ninja/mc2 $I/mc2; $sv python3 scripts/run_smoke.py --lite --keep-logs --exe $I/mc2 > $OUT/canary_smoke.log 2>&1; grep -qa 'SYNC-HAZARD' $OUT/canary_smoke.log && { echo '[syncval] canary: hazard reported, as required'; exit 0; }; echo '[syncval] canary: NO hazard reported (validation is not live or the canary is stale)'; exit 1; }" ;;
                   *) echo "[box_run] unknown syncval arm $arm"; RES=2; continue ;;
                 esac ;;
    *) echo "[box_run] unknown row $row"; RES=2; continue ;;
  esac
  case $row in determinism:*|saveload*)
    # data-fingerprint precheck: refuse up front when the install's stock data is not the data the baseline pins (a full run would only reach that verdict at the end)
    fpm=${row#*:}; [ "$row" = saveload ] && fpm=mc2_01
    fpl=$(cd $S && python3 -c "$FPPY" "$I" "$fpm" 2>&1); fprc=$?
    echo "[box_run] data_fp row=$row $fpl"
    printf "%s\t%s\t%s\n" "$row" "$(sha256sum $I/mc2 | cut -c1-16)" "$fpl" >> $OUT/fingerprints.tsv
    if [ $fprc != 0 ]; then
      echo "[box_run] row=$row REFUSED: the install data fingerprint does not match the baseline's; fix the golden install, not the baseline" | tee $OUT/$name.log
      printf "%s\t%s\n" "$row" 7 >> $OUT/rows.tsv; RES=1; continue
    fi ;;
  esac
  gr bash -c "cd $S && $cmd" > $OUT/$name.log 2>&1; rc=$?
  echo "[box_run] row=$row rc=$rc"; grep -a "^| mc2_\|result=\|DEPLOY_FINGERPRINT\|FAIL\|PASS" $OUT/$name.log | head -8 | cut -c1-220
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


def run(cmd, **kw):
    return subprocess.run(cmd, **console_child.child_kwargs(redirected=bool(_STDIO_KW & kw.keys()), **kw))


def git(*a, repo):
    return subprocess.run(["git", "-C", str(repo), *a], capture_output=True, text=True, check=True, **console_child.child_kwargs(redirected=True)).stdout.strip()


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


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gpu", default="auto", help="0-2, or auto (first free of 0-2; captures need 0); 3 needs --gate")
    ap.add_argument("--capture-any-gpu", action="store_true", help="allow capture and smoke rows on GPU 1-3 (they present; mc2_01 dies after the 60 s present-retry budget) (needs a build with claude/vk-headless-present eac614635)")
    ap.add_argument("--gate", action="store_true", help="merge gate: allow GPU 3")
    ap.add_argument("--seed", action="store_true", help="one-time golden install seeding")
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parents[1]))
    ap.add_argument("rows", nargs="*")
    a = ap.parse_args([x for x in sys.argv[1:] if x != "--"])
    repo = Path(a.repo)
    if a.seed:
        return seed(repo)
    if a.gate and not a.rows:
        a.rows = GATE_ROWS
    if not a.rows:
        ap.error("give at least one row, e.g. smoke-lite")
    if git("status", "--porcelain", "--untracked-files=no", repo=repo):
        print("[box_run] warning: uncommitted changes are NOT sent; the box runs HEAD", file=sys.stderr)
    sha = git("rev-parse", "HEAD", repo=repo)
    branch = (git("rev-parse", "--abbrev-ref", "HEAD", repo=repo) or "detached").replace("/", "_")
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
    if sha in have:   # mirror already has this commit: just point the lane ref at it
        git("update-ref", "-d", ref, repo=repo)
        run([*SSH, f"git -C {MIRROR} update-ref refs/lanes/{branch} {sha}"], check=True)
    else:
        try:
            run(["git", "-C", str(repo), "bundle", "create", "-q", str(bundle), ref, *excl], check=True)
            run([*SCP, str(bundle), f"{HOST}:/workspace/lanes_{branch}.bundle"], check=True)
        finally:
            git("update-ref", "-d", ref, repo=repo)
            bundle.unlink(missing_ok=True)
        run([*SSH, f"mkdir -p /workspace/lanes && GIT_LFS_SKIP_SMUDGE=1 git -C {MIRROR} fetch -q /workspace/lanes_{branch}.bundle "
                   f"{ref}:refs/lanes/{branch} && rm /workspace/lanes_{branch}.bundle"], check=True)
    t["bundle+push"] = time.time() - t0
    # gpu choice
    cap0 = 1 if (any(r.startswith(("capture", "smoke")) for r in a.rows) and not a.capture_any_gpu) else 0
    gpu = a.gpu
    if a.gate:
        gpu = "3"   # advisory clang gate evidence runs on the reserved gate GPU (lock /workspace/locks/gpu3)
    if gpu == "3" and not a.gate:
        print("[box_run] GPU 3 is reserved for merge gates (--gate)")
        return 2
    out_remote = f"/workspace/lanes/{branch}/out/{sha[:8]}"
    local_out = repo / "proofs" / "box" / sha[:8]
    local_out.mkdir(parents=True, exist_ok=True)
    scr = Path(tempfile.gettempdir()) / "box_lane.sh"
    scr.write_text(REMOTE, newline="\n")
    run([*SCP, str(scr), f"{HOST}:/workspace/lanes/.box_lane_{branch}.sh"], check=True)
    t1 = time.time()
    r = run([*SSH, f"bash /workspace/lanes/.box_lane_{branch}.sh {gpu} {branch} {sha} {out_remote} {cap0}"], input="\n".join(a.rows) + "\n", text=True)
    t["remote (build+install+rows)"] = time.time() - t1
    t2 = time.time()
    run([*SCP, "-r", f"{HOST}:{out_remote}/.", str(local_out)])
    t["copy-back"] = time.time() - t2
    scr.unlink(missing_ok=True)
    print(f"[box_run] gpu={gpu} (see run log for the one taken) sha={sha[:12]} rc={r.returncode} reports={local_out}")
    print("[box_run] timings: " + ", ".join(f"{k}={v:.0f}s" for k, v in t.items()) + f", total={time.time() - t0:.0f}s")
    if a.gate:
        write_receipt(local_out, sha, branch, a.rows, r.returncode)
    return r.returncode


def write_receipt(local_out, sha, branch, rows, rc):
    """Advisory clang-gate receipt: sha, rows with verdicts, the box build fingerprint. The PC gate keeps the MSVC tier1."""
    import json
    verdicts = {}
    tsv = local_out / "rows.tsv"
    if tsv.exists():
        for line in tsv.read_text().splitlines():
            row, _, code = line.rpartition("\t")
            verdicts[row] = "PASS" if code.strip() == "0" else f"FAIL(rc={code.strip()})"
    # determinism/saveload rows print no build fingerprint: the remote half records the exe sha and the install data fingerprint per row
    row_fp = {}
    fps = local_out / "fingerprints.tsv"
    if fps.exists():
        for line in fps.read_text().splitlines():
            row, _, rest = line.partition("\t")
            exe, _, data = rest.partition("\t")
            row_fp[row] = {"exe_sha256_16": exe, "data_fingerprint": data}
    fp = ""
    for lg in sorted(local_out.glob("*.log")):
        for ln in lg.read_text(errors="replace").splitlines():
            if "DEPLOY_FINGERPRINT" in ln or "BUILD_FINGERPRINT" in ln:
                fp = ln.strip()[:300]
                break
        if fp:
            break
    rec = {"kind": "box-clang-gate-advisory", "sha": sha, "branch": branch, "gpu": 3, "toolchain": "clang-20 + libc++ + lld, RelWithDebInfo, ccache",
           "rows": [{"row": r, "verdict": verdicts.get(r, "NOT RUN"), **row_fp.get(r, {})} for r in rows], "fingerprint": fp, "script_rc": rc,
           "note": "advisory; box ms are ratio-only; the PC gate keeps the MSVC tier1"}
    out = local_out / "gate_receipt.json"
    out.write_text(json.dumps(rec, indent=2))
    print(f"[box_run] gate receipt: {out}")


if __name__ == "__main__":
    sys.exit(main())
