#!/usr/bin/env python3
"""Run game rows for the CURRENT worktree HEAD on the rented Linux box (4x RTX 3060).

    py -3 tools/box_run.py --gpu auto -- smoke-lite
    py -3 tools/box_run.py --gpu 1 -- smoke-lite capture:mc2_01:mc2_01_saved_205644 determinism:mc2_10
    py -3 tools/box_run.py --seed          # once: build the golden install on the box (needs the Windows golden)

Steps (each timed, summary printed at the end): lock a GPU, git-bundle HEAD to the box mirror, clang build in
/workspace/lanes/<branch>/src (ccache, -j16, pinned to the GPU's 8 cores), install = hardlink copy of the golden install
(big immutable data linked, the rest copied) + deploy_payload, run the rows, copy reports back to
<worktree>/proofs/box/<sha8>/, print the DEPLOY_FINGERPRINT line, release the lock.

Rows: smoke-lite | smoke-tier1 | capture:<mission>:<bookmark-stem> | determinism:<mission> | saveload[:<mission>] | unit | sh:<command>
(sh runs in the lane src dir with $INSTALL, $OUT, $GPU_ENV set). GPU 3 is reserved for merge gates (--gate). GPUs 1-3 cannot
do screenshot captures yet (VK present init fails there): capture rows are forced onto GPU 0 by `--gpu auto`.
Windows-only behaviour (window focus, firewall, owner-test) is not covered: those rows stay on the Windows PC.
"""
import argparse, base64, io, json, os, subprocess, sys, tarfile, tempfile, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import console_child  # noqa: E402

KEY = os.environ.get("MC2_BOX_KEY", "C:/SPB_Data/.ssh/mc2farm_ed25519")
HOST = os.environ.get("MC2_BOX_HOST", "root@14.227.95.149")
PORT = os.environ.get("MC2_BOX_PORT", "30598")
MIRROR = "/workspace/toolchains/mirror.git"
GOLDEN_WIN = "A:/Games/mc2-opengl/mc2-win64-v0.4c"
_SSH_OPTS = ["-i", KEY, "-p", PORT, "-o", "LogLevel=ERROR", "-o", "ServerAliveInterval=30", HOST]
SSH = ["ssh", "-n", *_SSH_OPTS]        # -n: ssh must not swallow our stdin (every call that does not pipe a script in)
SSH_STDIN = ["ssh", *_SSH_OPTS]        # the two calls that pipe the remote script in
# verdicts beyond PASS/FAIL; every one of them is NOT green (exit code != 0), and none is ever retried by this tool
RC_LOCK_TIMEOUT = 76    # waited 30 min for a GPU lock (was 75, the same code as other failures)
RC_NO_BASELINE = 78     # determinism/saveload row refused: no Linux baseline file recorded
NEWEST_BOX_CLANG_FIX = "ee183497d"   # last nifty commit that kept the Linux clang build green (BRIDGE diag tag); older tips cannot build on the box
SCP = ["scp", "-q", "-i", KEY, "-P", PORT]
# `--gate` with no rows: the headless determinism rows the PC gate runs plus the two saveload pins (tests/*/linux baselines)
# plus what needs no Windows: the doctests, the Linux-capable contract rows (scripts/box_contracts.sh) and smoke-lite
GATE_ROWS = ([f"determinism:mc2_{n}" for n in ("01", "03", "10", "16", "24")] + ["saveload:mc2_01", "saveload:mc2_10"]
             + ["unit", "sh:bash scripts/box_contracts.sh", "smoke-lite"])

# a retry must never reuse a row's --out dir (sim_determinism refuses an existing one and the row would be VOID): one fresh dir per attempt
FRESH_FN = 'fresh() { local d=$1 n=0; while [ -e "$d.a$n" ]; do n=$((n+1)); done; printf %s "$d.a$n"; }'

REMOTE = r'''#!/bin/bash
# box_run.py remote half: args GPU BRANCH SHA OUTDIR CAP0 ROWS_B64 (rows newline-joined, base64)
set -uo pipefail
GPU=$1; BR=$2; SHA=$3; OUT=$4; mapfile -t ROWS < <(printf %s "$6" | base64 -d); ROWS=("${ROWS[@]%$'\r'}")   # $5 = cap0, $6 = base64 of the rows
L=/workspace/lanes/$BR; S=$L/src; I=$L/install; GOLD=/workspace/golden-install; MIRROR=/workspace/toolchains/mirror.git
#@@FRESH_FN@@
T0=$(date +%s); tick() { echo "[box_run] stage=$1 secs=$(( $(date +%s) - T0 )) total"; T0=$(date +%s); }
mkdir -p /workspace/locks $L $OUT
rm -f "$OUT"/rows.tsv "$OUT"/fingerprints.tsv   # both are append-only: a stale PASS from an earlier run at this sha must never come back
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
    [ $SECONDS -gt 1800 ] && { echo "[box_run] LOCK_TIMEOUT: gave up waiting for a GPU"; exit 76; }
    timeout 3 tail -f /dev/null
  done
fi
[ -n "$WANTF" ] && rm -f "$WANTF"; WANTF=""
GPU=$GOT; echo "$$ $BR $SHA $(date -Is)" > /workspace/locks/gpu$GPU.owner
echo "[box_run] gpu=$GPU locked lock_wait_secs=$SECONDS"
[ -d $GOLD ] || { echo "[box_run] no golden install: run 'py -3 tools/box_run.py --seed' once"; exit 3; }
N=$GPU; bus=$(nvidia-smi --query-gpu=pci.bus_id --format=csv,noheader -i "$N"); bus=${bus: -12}
export DRI_PRIME="pci-$(echo "$bus" | tr ':.' '__')" MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1
export MC2_VK_DEVICE=NVIDIA SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy OMP_NUM_THREADS=1 MC2_LOG=1 TMPDIR=$L/tmp
mkdir -p $TMPDIR; CPUS="$((4*N))-$((4*N+3))"   # box quota is ~30.7 CPUs and a game uses ~1 core: rows get 4 cores of their GPU; builds use cores 16-63 at nice 19 (-j24)
BCPUS=16-63
gr() { taskset -c "$CPUS" xvfb-run -a -s "-screen 0 1920x1080x24" "$@" 9>&-; }   # -a: free display picked per run, fd 9 (GPU lock) not inherited
export GIT_LFS_SKIP_SMUDGE=1
# source: bundle was fetched into the mirror as refs/lanes/$BR by the Windows half
if [ -e $S/.git ]; then git -C $S checkout -q -f --detach $SHA; else git -C $MIRROR worktree add -q -f --detach $S $SHA; fi
tick source
# build: shared ccache keyed without the directory so every lane hits the same objects
export CCACHE_BASEDIR=/workspace/lanes CCACHE_NOHASHDIR=1 CCACHE_SLOPPINESS=time_macros
cd $S
[ -f build64-ninja/build.ninja ] || nice -n 19 taskset -c "$BCPUS" cmake -S . -B build64-ninja -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_CXX_FLAGS=-stdlib=libc++ -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
 -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
 -DVulkan_INCLUDE_DIR=/workspace/toolchains/vulkan/include -DMC2_DXC=/workspace/toolchains/dxc/bin/dxc -DMC2_SLANGC=/workspace/toolchains/slang/bin/slangc > $OUT/configure.log 2>&1 || { tail -20 $OUT/configure.log; exit 4; }
nice -n 19 taskset -c "$BCPUS" ninja -C build64-ninja -j24 mc2 > $OUT/build.log 2>&1; rc=$?; tail -2 $OUT/build.log
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
# optional payload: only what THIS tree built (a lane without crashpad has no crashpad_handler; deploy must never require it)
for opt in crashpad_handler; do [ -f build64-ninja/$opt ] && { cp -f build64-ninja/$opt $I/$opt && chmod +x $I/$opt || { echo "[box_run] DEPLOY FAILED: $opt"; exit 6; }; }; done
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
  name=$(printf %s "$row" | tr -c 'A-Za-z0-9._-' _); case $row in sh:*) name=${name:0:50}-$(printf %s "$row" | cksum | cut -d' ' -f1);; esac; echo "[box_run] row=$row"
  case $row in
    smoke-lite)  cmd="python3 scripts/run_smoke.py --lite --keep-logs --exe $I/mc2" ;;
    smoke-tier1) cmd="python3 scripts/run_smoke.py --tier tier1 --keep-logs --exe $I/mc2" ;;
    capture:*)   IFS=: read -r _ m b <<< "$row"; cmd="python3 scripts/run_visual_capture.py --mission $m --bookmarks tests/visual/bookmarks/$b.json --exe $I/mc2 --classic-assets --out-dir $(fresh $OUT/$name)" ;;
    determinism:*) m=${row#*:}; extra=""; [ "$m" = mc2_24 ] && extra="--ticks 2600"   # mc2_24: the finale spawns infantry after tick 2404 (as the PC gate)
                   cmd="python3 tools/sim_determinism.py check $m --install $I --headless-both $extra --out $(fresh $OUT/$name)" ;;
    saveload*)   m=${row#saveload}; m=${m#:}; m=${m:-mc2_01}   # headless: a gate GPU has no present-capable surface; the save bytes equal the windowed ones
                 cmd="python3 tools/saveload_roundtrip.py check --mission $m --install $I --mask tools/save_noise_mask.json --first-save-only --headless --out $(fresh $OUT/$name) --save-dir $(fresh $OUT/$name.savegame)" ;;
    unit)        # doctest tier (tests/unit): its own configure with the box clang toolchain (a bare cmake finds no default compiler: "CMAKE_CXX_COMPILER not set")
                 cmd="cmake -S tests/unit -B build64-tests -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_CXX_FLAGS=-stdlib=libc++ -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache && nice -n 19 ninja -C build64-tests -j4 -k 0 mc2_tests && ./build64-tests/mc2_tests --no-breaks" ;;
    sh:*)        cmd="${row#sh:}" ;;
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
  gr bash -c 'cd "$1" && eval "$2"' _ "$S" "$cmd" > $OUT/$name.log 2>&1; rc=$?   # eval from argv: a row may carry spaces, quotes, slashes
  case $row in determinism:*|saveload*) [ $rc != 0 ] && grep -aq "baseline FAIL no baseline at\|check: no baseline for" $OUT/$name.log && rc=78;; esac   # NO_BASELINE: own verdict, never a plain FAIL
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

REMOTE = REMOTE.replace("#@@FRESH_FN@@", FRESH_FN)


def run(cmd, **kw):
    return subprocess.run(cmd, **console_child.child_kwargs(redirected=bool(_STDIO_KW & kw.keys()), **kw))


def git(*a, repo):
    return subprocess.run(["git", "-C", str(repo), *a], capture_output=True, text=True, check=True, **console_child.child_kwargs(redirected=True)).stdout.strip()


def probe():
    try:
        r = run([*SSH[:-1], "-o", "ConnectTimeout=10", "-o", "BatchMode=yes", HOST, "true"], capture_output=True, timeout=15)
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
BACK = ("cd {out} || exit 0; mkdir -p .tails && for f in *.log; do [ -f \"$f\" ] && tail -n 200 \"$f\" > .tails/$f; done; "
        "find . -type f \\( -name '*.tsv' -o -name '*.json' -o -path './.tails/*' \\) | tar cf - -T -")


def rows_b64(rows):
    return base64.b64encode("\n".join(rows).encode()).decode()


def ssh_out(cmd):
    return run([*SSH, cmd], capture_output=True, text=True).stdout


def pick_gpu():
    """least-loaded of the lane GPUs 0-2 by queued+running pueue tasks (ties: lowest)"""
    out = ssh_out(f"for g in 0 1 2; do echo $g $({PUEUE} status -g gpu$g --json | jq '[.tasks[]|select(.status|tostring|test(\"Queued|Running\"))]|length'); done")
    load = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 2 and p[1].isdigit():
            load[p[0]] = int(p[1])
    return min(("0", "1", "2"), key=lambda g: (load.get(g, 99), g))


def fetch_back(out_remote, local_out):
    r = subprocess.run([*SSH, BACK.format(out=out_remote)], capture_output=True, **console_child.child_kwargs(redirected=True))
    if r.stdout:
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
    res = body.get("result") if isinstance(body, dict) else None
    rc = None if state != "Done" else (0 if res == "Success" else res.get("Failed", 1) if isinstance(res, dict) else 1)
    return state, rc, t.get("label", "")


def row_codes(tsv):
    return [ln.rpartition("\t")[2].strip() for ln in tsv.read_text().splitlines()] if tsv.exists() else []


def verdict(rc, codes=()):
    """(name, exit code). PASS only for rc 0 with every row 0; LOCK_TIMEOUT and NO_BASELINE are distinct and NEVER green."""
    if rc == RC_LOCK_TIMEOUT:
        return "LOCK_TIMEOUT", RC_LOCK_TIMEOUT
    if rc == 0 and all(c == "0" for c in codes):
        return "PASS", 0
    if str(RC_NO_BASELINE) in codes and all(c in ("0", str(RC_NO_BASELINE)) for c in codes):
        return "NO_BASELINE", 4
    return "FAIL", 1


def task_verdict(tid, repo, wait):
    """ONE line: box_run task=<id> state=<..> rc=<..> rows=<pass>/<total>. --wait blocks, then copies the reports back."""
    if wait:
        run([*SSH, f"{PUEUE} wait {tid} >/dev/null 2>&1"])
    state, rc, label = task_state(tid)
    parts = label.split(":")   # box_run:<branch>:<sha8>:<group>
    rows = ""
    if state == "Done" and len(parts) == 4:
        local_out = Path(repo) / "proofs" / "box" / parts[2]
        local_out.mkdir(parents=True, exist_ok=True)
        fetch_back(f"/workspace/lanes/{parts[1]}/out/{parts[2]}", local_out)
        tsv = local_out / "rows.tsv"
        v = row_codes(tsv)
        rows = f" rows={sum(x == '0' for x in v)}/{len(v)} reports={local_out}"
    name, code = verdict(rc, row_codes(Path(repo) / "proofs" / "box" / parts[2] / "rows.tsv") if state == "Done" and len(parts) == 4 else ()) if rc is not None else ("", 2)
    print(f"[box_run] task={tid} state={state} rc={'-' if rc is None else rc}{rows}" + (f" verdict={name}" if name else ""))
    return code


def lane_key(abbrev, sha):
    """Lane dir / ref key: the branch name, or detached-<sha8> when HEAD is detached (git prints 'HEAD'), so detached runs never share a dir."""
    return f"detached-{sha[:8]}" if abbrev in ("", "HEAD") else abbrev.replace("/", "_")


def check_behind_box_fix(repo):
    """Cheap pre-submit warning: a tip that lacks the newest box-clang fix on nifty cannot build on Linux; fail fast on the PC, not after a push + build."""
    r = subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor", NEWEST_BOX_CLANG_FIX, "HEAD"], capture_output=True, **console_child.child_kwargs(redirected=True))
    if r.returncode == 1:
        print(f"[box_run] WARNING: HEAD does not contain {NEWEST_BOX_CLANG_FIX} (box clang fixes on nifty); the Linux build will probably fail. Merge claude/nifty-mendeleev first.", file=sys.stderr)
    return r.returncode


def box_has(sha):
    return run([*SSH, f"git -C {MIRROR} cat-file -e {sha}^{{commit}}"], capture_output=True).returncode == 0


def main():
    ap =argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gpu", default="auto", help="0-2, or auto (first free of 0-2; captures need 0); 3 needs --gate")
    ap.add_argument("--capture-any-gpu", action="store_true", help="allow capture and smoke rows on GPU 1-3 (they present; mc2_01 dies after the 60 s present-retry budget) (needs a build with claude/vk-headless-present eac614635)")
    ap.add_argument("--gate", action="store_true", help="merge gate: allow GPU 3")
    ap.add_argument("--seed", action="store_true", help="one-time golden install seeding")
    ap.add_argument("--probe", action="store_true", help="exit 0 if the box answers ssh within 15 s (merge_gate picks the box route or the Windows-full fallback with it)")
    ap.add_argument("--queue", action="store_true", help="submit to the box's pueue group (gpuN, or gate with --gate) and print the task id at once instead of blocking")
    ap.add_argument("--wait", metavar="ID", help="block until queued task ID ends, copy the reports back, print ONE verdict line (exit code = the task's)")
    ap.add_argument("--status", metavar="ID", help="print ONE state line for queued task ID (exit 0 done+ok, 1 failed, 2 queued/running)")
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parents[1]))
    ap.add_argument("rows", nargs="*")
    a = ap.parse_args([x for x in sys.argv[1:] if x != "--"])
    repo = Path(a.repo)
    if a.probe:
        return probe()
    if a.wait or a.status:
        return task_verdict(a.wait or a.status, repo, wait=bool(a.wait))
    if a.seed:
        return seed(repo)
    if a.gate and not a.rows:
        a.rows = GATE_ROWS
    if not a.rows:
        ap.error("give at least one row, e.g. smoke-lite")
    if git("status", "--porcelain", "--untracked-files=no", repo=repo):
        print("[box_run] warning: uncommitted changes are NOT sent; the box runs HEAD", file=sys.stderr)
    sha = git("rev-parse", "HEAD", repo=repo)
    branch = lane_key(git("rev-parse", "--abbrev-ref", "HEAD", repo=repo), sha)
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
    # gpu choice
    # TODO: drop cap0 (smoke/capture rows forced onto GPU 0) once claude/vk-headless-present (eac614635) is in the base
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
    if a.gate:   # a receipt must come from THIS run, never a leftover of an earlier run at the same sha
        for stale in ("rows.tsv", "fingerprints.tsv", "gate_receipt.json"):
            (local_out / stale).unlink(missing_ok=True)
    # one ssh: the remote script arrives on stdin (cat > file) and runs at once; rows travel as one base64 argument
    script = f"/workspace/lanes/.box_lane_{branch}.sh"
    if a.queue:   # submit to the pueue group and return the task id; --wait/--status give the verdict later
        if gpu == "auto":
            gpu = "0" if cap0 else pick_gpu()
        grp = "gate" if a.gate else f"gpu{gpu}"
        label = f"box_run:{branch}:{sha[:8]}:{grp}"
        r = run([*SSH_STDIN, f"cat > {script} && {PUEUE} add -g {grp} --print-task-id -l {label} -- bash {script} {gpu} {branch} {sha} {out_remote} {cap0} {rows_b64(a.rows)}"],
                input=REMOTE.encode(), capture_output=True)   # bytes: text mode would turn LF into CRLF on Windows and bash chokes on the CR
        out = r.stdout.decode(errors="replace") if isinstance(r.stdout, bytes) else r.stdout
        err = r.stderr.decode(errors="replace") if isinstance(r.stderr, bytes) else r.stderr
        tid = out.strip().splitlines()[-1] if r.returncode == 0 and out.strip() else ""
        print(f"[box_run] queued task={tid} group={grp} sha={sha[:12]} reports={local_out}" if tid else f"[box_run] queue submit FAILED rc={r.returncode} {err.strip()[-200:]}")
        return 0 if tid else 1
    t1 = time.time()
    r = run([*SSH_STDIN, f"cat > {script} && bash {script} {gpu} {branch} {sha} {out_remote} {cap0} {rows_b64(a.rows)}"], input=REMOTE.encode())
    t["remote (build+install+rows)"] = time.time() - t1
    t2 = time.time()
    fetch_back(out_remote, local_out)
    t["copy-back"] = time.time() - t2
    print(f"[box_run] gpu={gpu} (see run log for the one taken) sha={sha[:12]} rc={r.returncode} reports={local_out}")
    print("[box_run] timings: " + ", ".join(f"{k}={v:.0f}s" for k, v in t.items()) + f", total={time.time() - t0:.0f}s")
    name, code = verdict(r.returncode, row_codes(local_out / "rows.tsv"))
    print(f"[box_run] verdict={name}")
    if a.gate:
        return gate_verdict(write_receipt(local_out, sha, branch, a.rows, r.returncode))
    return code


def gate_verdict(rec):
    """merge_gate's box stage exit code: 0 every row PASS; 1 a row FAILed (a red gate row, never waived); 3 BOX_INFRA (no
    row verdicts, or rows that never ran: the box failed, not the product, and merge_gate falls back to the Windows rows)."""
    rows = rec["rows"]
    for r in rows:
        print(f"[box_run] gate row {r['row']}: {r['verdict']} exe_sha256_16={r.get('exe_sha256_16', '-')} data_fp={r.get('data_fingerprint', '-')}")
    if not rows or any(r["verdict"] in ("NOT RUN", "LOCK_TIMEOUT") for r in rows):   # LOCK_TIMEOUT: never ran, not green; the box was busy, so the PC rows stand in
        print("BOX_INFRA: " + ", ".join(r["row"] for r in rows if r["verdict"] in ("NOT RUN", "LOCK_TIMEOUT")) + " produced no verdict (remote run died or timed out on the GPU lock before them)")
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
    for lg in sorted(local_out.glob("*.log")):
        for ln in lg.read_text(errors="replace").splitlines():
            if "DEPLOY_FINGERPRINT" in ln or "BUILD_FINGERPRINT" in ln:
                fp = ln.strip()[:300]
                break
        if fp:
            break
    rec = {"kind": "box-clang-gate-advisory", "sha": sha, "branch": branch, "gpu": 3, "toolchain": "clang-20 + libc++ + lld, RelWithDebInfo, ccache",
           "rows": [{"row": r, "verdict": verdicts.get(r, "LOCK_TIMEOUT" if rc == RC_LOCK_TIMEOUT else "NOT RUN"), **row_fp.get(r, {})} for r in rows], "fingerprint": fp, "script_rc": rc,
           "note": "advisory; box ms are ratio-only; the PC gate keeps the MSVC tier1"}
    out = local_out / "gate_receipt.json"
    out.write_text(json.dumps(rec, indent=2))
    print(f"[box_run] gate receipt: {out}")
    return rec


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (subprocess.CalledProcessError, OSError) as exc:
        if "--gate" not in sys.argv:
            raise
        print(f"BOX_INFRA: box_run could not reach or drive the box: {exc}")
        sys.exit(3)
