#!/usr/bin/env python3
"""Run the merge gate for a staged candidate on the farm laptop, pull the evidence and payload back.

PC side (orchestrator):   py -3 tools/laptop_gate.py W [--setup] [-- <extra merge_gate args>]
  1. push claude/stage-W (and nifty) to the persistent laptop clone  C:/mc2gate/drv/A/Games/mc2-main
  2. over ssh: build there (warm Ninja tree, cold the first time), run merge_gate.py --full against the laptop install
  3. pull gate-verdict.json, gate.log, summary.json and the payload delta (tar of every manifest file that differs from
     the PC's published v0.4) with sha256 checks into E:/mc2-archive-2026-10/proofs/laptop-gate/W/
Then, on PASS (orchestrator only):
  py -3 tools/lane_merge.py --promote W --verdict <pulled>/gate-verdict.json
  py -3 scripts/deploy_payload.py --target game --from-payload <pulled>/payload

The laptop has no A:/E: drive, so the remote half substs A: and E: onto C:/mc2gate/drv/{A,E}; merge_gate.py then runs
unchanged with its hard-coded paths. The laptop's v0.4 and _lean-seed are junctions to C:/mc2gate/install.
Remote half: this same file, uploaded to C:/mc2gate/bin, run as `--remote <step>`.

Determinism + saveload evidence for a window in ONE invocation, after the full run (or --keep-candidate) left the deployed candidate:
  py -3 tools/laptop_gate.py W --sim [--sim-rows determinism-mc2_01,saveload-mc2_10] [--sim-pair] [--dry-run]
  No push, build or capture: the laptop runs merge_gate.sim_plan() (merge_gate's OWN determinism-mc2_01/03/10/24 and saveload-mc2_03/10
  step definitions: same args, --mask, tick ranges) on a stock copy of the gate's deployed candidate (_gate-reuse/deployed), under the
  farm queue lock. Results land in <proofs>/laptop-gate/W/sim/ and a "sim" key in that window's summary.json; exit 0 only when every row is
  green. --dry-run prints the command list and runs nothing. The clone must still be at the stage tip the gate deployed (else a clear error).
Every run (and --sim) also mirrors the PC's E:/mc2-ref/campaign-mods (MechCommanderOmnitech, mco-compat; the sources of --keep-candidate's
  mods and of merge_gate's MC2_GATE_MODS_SOURCE default) into the laptop's drv/E/mc2-ref/campaign-mods, size+mtime-synced and idempotent.
  It only reads the PC folder; nothing is written under A:/Games/mc2-opengl.
"""
import argparse, csv, hashlib, json, os, shutil, subprocess, sys, tarfile, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
# The remote half runs from C:/mc2gate/bin alone: every module it imports (module level, plus r_run's queue_lock) is a HELPER,
# repo-relative, that provision() uploads next to it (scripts/tests/test_laptop_gate.py fails when an import is missing).
HELPERS = ("scripts/console_child.py", "tools/farm/queue_lock.py")
try:
    import console_child  # noqa: E402  (BELOW_NORMAL + CREATE_NO_WINDOW for every child)
except ImportError as exc:
    sys.exit(f"laptop_gate: helper missing next to {__file__} ({exc}); run laptop_gate.py from the PC, which provisions "
             f"{', '.join(HELPERS)} into C:/mc2gate/bin")

HOST = "Kaitlyn@192.168.68.107"
KEY = "C:/SPB_Data/.ssh/mc2farm_ed25519"
G = "C:/mc2gate"
REPO = G + "/drv/A/Games/mc2-main"
INSTALL = G + "/install"
PC_V04 = Path("A:/Games/mc2-opengl/mc2-win64-v0.4")
PC_SEED = Path("E:/mc2-installs/_lean-seed")
PC_STOCK = Path("G:/mc2-assets/out/scifi-kit/round2/stock")
PROOFS = Path("E:/mc2-archive-2026-10/proofs/laptop-gate")
PC_REF = Path("E:/mc2-ref")
PC_TMP = Path("G:/mc2-tmp/gatelaptop")
REF = G + "/ref"   # the laptop's MC2_REF_ROOT: the subset of E:/mc2-ref the gate stages read
REF_MANIFESTS = ("data/kit/industrial-kit.json", "data/kit/vehicle-kit.json")   # deploy's pinned kits
REF_EXTRA = ("scifi-kit/mapping/inventory.csv",)   # kit_art row
MODS_SRC = Path("E:/mc2-ref/campaign-mods")   # merge_gate.CANDIDATE_MODS_SOURCE on the PC (EROOT + /mc2-ref/campaign-mods)
MODS_DST = G + "/drv/E/mc2-ref/campaign-mods"   # the same path under the laptop's MC2_GATE_EROOT
MODS_NAMES = ("MechCommanderOmnitech", "mco-compat")   # merge_gate.CANDIDATE_MODS
E_EXTRA = ("mc2-archive-2026-10/asset-modernize/docs/assets/catalog.json",)   # scifi_kit counts.CATALOG (art suite)
# Generated, gitignored deploy inputs whose cook sources exist only on the PC: the laptop clone gets the published copies. Without
# them the laptop deploy omits and then prunes them, and compare-captures fails on the cliffs and concrete (W129 run 3: MAD 0.20/0.52).
LAYER_EXTRA = tuple(f"data/terrain_layers/{n}.ktx2" for n in ("concrete_detail_pad", "concrete_detail_road", "cliff_rock_ah", "cliff_rock_nra"))
# flip_evaluator: compare-captures' FLIP columns (no cp315 wheel: build the sdist under vcvars64 with CC=CXX=cl, not MinGW)
PACKAGES = ("numpy", "PIL", "scipy", "skimage", "pefile", "pytest", "flip_evaluator")
GIT_BASH = "C:/Program Files/Git/bin/bash.exe"
JOBS = "A:/Games/mc2-ops/JOBS"   # kit_payload.build_importer reads it at this path (A: is subst'd on the laptop)
SSH_OPTS = ["-i", KEY, "-o", "BatchMode=yes", "-o", "LogLevel=ERROR", "-o", "ServerAliveInterval=30"]


def env():
    return dict(os.environ, HOME="C:/SPB_Data")


def ssh(cmd, check=True, stream=False):
    argv = ["ssh", *SSH_OPTS, HOST, cmd]
    if stream:
        r = subprocess.run(argv, env=env(), **console_child.child_kwargs())
        out = ""
    else:
        r = subprocess.run(argv, env=env(), capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
        out = r.stdout
    if check and r.returncode:
        sys.exit(f"laptop_gate: ssh failed rc={r.returncode}: {cmd[:120]}\n{getattr(r, 'stderr', '') or ''}")
    return r.returncode, out


def scp(src, dst, recursive=False):
    argv = ["scp", "-q", *(["-r"] if recursive else []), *SSH_OPTS, str(src), str(dst)]
    r = subprocess.run(argv, env=env(), **console_child.child_kwargs())
    if r.returncode:
        sys.exit(f"laptop_gate: scp failed: {src} -> {dst}")


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def remote_py(step, *args):
    return f'py -3 {G}/bin/laptop_gate.py --remote {step} ' + " ".join(args)


def remote_run_cmd(window, gate_args):
    """The remote run command; merge_gate args go after `--` (W130 run 1: without it the remote parse put `deploy` into the
    positionals and handed merge_gate `deploy --stages --keep-candidate`)."""
    return remote_py("run", window, *(["--", *gate_args] if gate_args else []))


# ---------------------------------------------------------------- laptop half

def r_subst():
    """A: and G: for this logon session (the laptop's D:/E: are a card reader, so E-paths use MC2_GATE_EROOT instead).
    G: holds the PC's G:/mc2-assets stock mirror that the kit_art contract row reads at hard-coded paths. Never I:."""
    for letter, target in (("A:", G + "/drv/A"), ("G:", G + "/drv/G")):
        os.makedirs(target, exist_ok=True)
        cur = subprocess.run(["subst"], capture_output=True, text=True, **console_child.child_kwargs(redirected=True)).stdout
        if any(line.upper().startswith(letter) for line in cur.splitlines()):
            continue
        subprocess.run(["subst", letter, target.replace("/", "\\")], check=True, **console_child.child_kwargs())


def junction(link, target):
    link = Path(link)
    if link.exists():
        return
    link.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cmd", "/c", "mklink", "/J", str(link).replace("/", "\\"), str(target).replace("/", "\\")], check=True, capture_output=True,
                   **console_child.child_kwargs(redirected=True))


def r_init():
    Path(G, "tmp").mkdir(parents=True, exist_ok=True)
    Path(INSTALL).mkdir(parents=True, exist_ok=True)
    r_subst()
    # cmd.exe keeps git's single quotes around the repo path; this wrapper ignores its argument
    Path(G, "bin").mkdir(parents=True, exist_ok=True)
    Path(G, "bin/rp.cmd").write_text(f"@git receive-pack {REPO}\r\n")
    repo = Path(REPO)
    if not (repo / ".git").exists():
        repo.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "-q", "C:/mc2lint/repo", str(repo)], check=True, **console_child.child_kwargs())
        subprocess.run(["git", "-C", str(repo), "config", "receive.denyCurrentBranch", "ignore"], check=True,
                       **console_child.child_kwargs())
    import shutil
    for sub in ("cmake", "lib"):   # untracked build deps, same recipe as C:/mc2lint/msvc.cmd
        dst = repo / "3rdparty" / sub
        if not dst.exists():
            shutil.copytree("C:/mc2lint/deps/" + sub, dst)
    ops = Path(G, "drv/A/Games/mc2-ops")
    ops.mkdir(parents=True, exist_ok=True)
    (ops / "JOBS").write_text("6\n")
    junction(G + "/drv/A/Games/mc2-opengl/mc2-win64-v0.4", INSTALL)
    junction(G + "/drv/E/mc2-installs/_lean-seed", INSTALL)
    Path(G, "drv/E/mc2-archive-2026-10/proofs").mkdir(parents=True, exist_ok=True)
    print("init ok; install has mc2.exe:", Path(INSTALL, "mc2.exe").exists())


def run(cmd, log, **kw):
    t = time.time()
    with open(log, "a", encoding="utf-8", errors="replace") as lf:
        r = subprocess.run(cmd, cwd=REPO, stdout=lf, stderr=subprocess.STDOUT, **console_child.child_kwargs(redirected=True, **kw))
    return r.returncode, round(time.time() - t, 1)


def gate_env():
    return dict(os.environ, TEMP=G + "/tmp", TMP=G + "/tmp", TMPDIR=G + "/tmp", PYTHONUTF8="1",
                MC2_BUILD_JOBS="7", MC2_BUILD_TOKENS="0", MC2_COMPILE_CACHE="0", MC2_GATE_EROOT=G + "/drv/E",
                MC2_NO_CPU_PIN="1",   # CPU_SPLIT is the PC's 16-CPU split; on the 8-thread laptop it would pin 7 jobs to 4 CPUs
                # the laptop's only fixed drive is C: (its E: is a card reader): every E:-rooted gate path lives under drv/E
                MC2_GATE_REUSE_ROOT=G + "/drv/E/mc2-installs/_gate-reuse", MC2_STAGE_BASE=G + "/drv/E/mc2-installs",
                MC2_ISO_ALLOW_SYSTEM_DRIVE="1", MC2_REF_ROOT=REF,
                # Git's usr/bin is not on the laptop PATH; bin/bash.exe is the wrapper that puts coreutils on it
                MC2_BASH=GIT_BASH)


def fast_red(log, e):
    """The laptop-environment reds that used to surface only after a full gate, in minutes, before merge_gate.py: the art/kit
    suite (W129 run 1: no catalog under drv/E, after 2.5 min of contracts) and the deploy row's shader inventory against the
    published install (W129 run 2: 'published shader inventory != HEAD build', after contracts + harnesses + capture-before)."""
    bad = []
    rc, s = run([sys.executable, "scripts/run_tools_tests.py", "--suite", "art", "--jobs", "3"], log, env=e)
    if rc:
        bad.append(f"art/kit suite rc={rc} ({s} s, see preflight.log)")
    sys.path.insert(0, REPO + "/scripts")
    import deploy_payload
    problems = deploy_payload.shader_inventory_problems(REPO, INSTALL)
    with open(log, "a", encoding="utf-8") as lf:
        lf.write("\n".join(problems or ["shader inventory OK"]) + "\n")
    bad += problems
    print("fast red: " + ("; ".join(bad) if bad else "OK"), flush=True)
    return bad


FARM_QUEUE_LOCK = "C:/mc2farm/queue.lock"   # the runner's lock: perf timing never overlaps this build + gate (p31)


def r_run(window, extra):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import queue_lock   # uploaded next to this file by pc_main (tools/farm/queue_lock.py)
    with queue_lock.hold(FARM_QUEUE_LOCK, log=lambda m: print("laptop_gate: " + m, flush=True)):
        return _r_run(window, extra)


def _r_run(window, extra):
    r_subst()
    out = Path(G, "out", window)
    # nothing from an earlier run may survive: a gate that dies before writing its verdict must leave no verdict to copy
    shutil.rmtree(out, ignore_errors=True)
    shutil.rmtree(Path(G, "drv/E/mc2-archive-2026-10/proofs/merge-gate", window), ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    branch = "claude/stage-" + window
    e = gate_env()
    sm = {"window": window, "host": "laptop"}
    git = lambda *a: subprocess.run(["git", "-C", REPO, *a], capture_output=True, text=True, env=e,
                                    **console_child.child_kwargs(redirected=True))
    for step in (["checkout", "-q", "-f", branch], ["clean", "-fdq"]):   # no -x: build tree and deps survive
        r = git(*step)
        if r.returncode:
            sys.exit("remote git failed: " + r.stderr)
    head = git("rev-parse", "HEAD").stdout.strip()
    sm["head"] = head
    cold = not Path(REPO, "build64-ninja/build.ninja").exists()
    sm["cold_build"] = cold
    blog = out / "build.log"
    if cold:
        rc, s = run([sys.executable, "scripts/configure_ninja.py", "--no-compile-cache"], blog, env=e)
        sm["configure_seconds"] = s
        if rc:
            sm["error"] = "configure failed"
    if "error" not in sm:
        rc, s = run([sys.executable, "scripts/ninja_build.py"], blog, env=e)
        sm["build_seconds"], sm["build_rc"] = s, rc
        if rc:
            sm["error"] = "build failed"
    if "error" not in sm:
        sm["fast_red"] = fast_red(out / "preflight.log", e)
        if sm["fast_red"]:
            sm["error"] = "fast red: " + "; ".join(sm["fast_red"])
    if "error" not in sm:
        proof =G + "/drv/E/mc2-archive-2026-10/proofs/merge-gate/" + window
        cmd = [sys.executable, "scripts/merge_gate.py", "--full",
               "--proofs-dir", proof, "--expect-head", head]
        if window.isdigit():
            cmd += ["--window", window]
        rc, s = run(cmd + extra, out / "gate.console.log", env=e)
        sm["gate_seconds"], sm["gate_rc"] = s, rc
        for name, dst in (("gate-verdict.json", "gate-verdict.json"), ("gate.log", "gate.log")):
            src = Path(G, "drv/E/mc2-archive-2026-10/proofs/merge-gate", window, name)
            if src.is_file():
                (out / dst).write_bytes(src.read_bytes())
        proof_dir = Path(G, "drv/E/mc2-archive-2026-10/proofs/merge-gate", window)
        for sub in EVIDENCE_DIRS:   # the capture images and A/B montages the orchestrator views before --publish
            if (proof_dir / sub).is_dir():
                shutil.copytree(proof_dir / sub, out / "evidence" / sub, dirs_exist_ok=True,
                                ignore=shutil.ignore_patterns("*.log", "*.exe", "*.dll", "*.pdb"))
        if (out / "gate-verdict.json").is_file():
            v = json.loads((out / "gate-verdict.json").read_text())
            sm["verdict"], sm["timings"] = v.get("verdict"), v.get("timings")
    (out / "summary.json").write_text(json.dumps(sm, indent=1))
    print(json.dumps({k: v for k, v in sm.items() if k != "timings"}))


def r_sim(window, tokens):
    """Remote `sim` step: merge_gate.sim_plan() rows on a stock copy of the gate's deployed candidate, under the farm queue lock."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import queue_lock   # uploaded next to this file by provision()
    with queue_lock.hold(FARM_QUEUE_LOCK, log=lambda m: print("laptop_gate: " + m, flush=True)):
        return _r_sim(window, tokens)


def sim_args(tokens):
    """(rows, pair) from the remote `sim` tokens: row names plus --pair."""
    return [t for t in tokens if not t.startswith("-")], "--pair" in tokens


def _r_sim(window, tokens):
    r_subst()
    rows, pair = sim_args(tokens)
    e = gate_env()
    os.environ.update(e)   # merge_gate / gate_reuse read MC2_GATE_EROOT and MC2_GATE_REUSE_ROOT at import
    git = lambda *a: subprocess.run(["git", "-C", REPO, *a], capture_output=True, text=True, env=e,
                                    **console_child.child_kwargs(redirected=True))
    branch = "claude/stage-" + window
    if git("rev-parse", "--verify", "-q", branch).returncode == 0 and git("checkout", "-q", "-f", branch).returncode:
        sys.exit("remote git checkout failed for " + branch)
    head = git("rev-parse", "HEAD").stdout.strip()
    out = Path(G, "out", window, "sim")
    shutil.rmtree(out, ignore_errors=True)   # nothing from an earlier sim run may survive; the gate's own out/<W> files stay
    out.mkdir(parents=True, exist_ok=True)
    sm = {"window": window, "host": "laptop", "head": head, "rows": {}, "pair": pair}
    sys.path.insert(0, REPO + "/scripts")
    import gate_reuse
    import merge_gate
    if not hasattr(merge_gate, "sim_plan"):
        sm["error"] = f"{REPO}/scripts/merge_gate.py at {head[:9]} has no sim_plan (stage the window from a tip that has it)"
    seed = None if "error" in sm else gate_reuse.deployed_seed(Path(REPO), head)
    if "error" not in sm and seed is None:
        sm["error"] = (f"no deployed candidate seed for {head[:9]} under {gate_reuse.CACHE_ROOT}: run the laptop gate (or --keep-candidate) "
                       f"for {window} first, and keep the clone at claude/stage-{window}")
    rc = 1
    if "error" not in sm:
        sm["seed"] = str(seed)
        try:
            with merge_gate.stock_copy(seed, parent=Path(merge_gate.CANDIDATE_SEED).parent) as install:
                steps = merge_gate.sim_plan(install, out, rows, pair)
                sm["commands"] = {n: subprocess.list2cmdline(c) for n, c, _ in steps}
                merge_gate.ROW_RESULTS.clear()
                rc = merge_gate.execute_parallel(steps, head)
        except Exception as exc:  # noqa: BLE001 - recorded in the summary the PC pulls
            sm["error"] = f"{type(exc).__name__}: {exc}"
        sm["rows"] = {n: {k: r.get(k) for k in ("status", "seconds", "wait")} for n, r in merge_gate.ROW_RESULTS.items()}
    sm["rc"] = rc
    sm["verdict"] = sim_verdict(sm, [n for n, _, _ in steps] if "commands" in sm else [])
    (out / "summary.json").write_text(json.dumps(sm, indent=1))
    print(json.dumps({k: v for k, v in sm.items() if k != "commands"}))


def sim_verdict(sm, planned):
    """PASS only when the run had no error, rc 0, and every planned row reported a green status; else FAIL."""
    green = lambda st: st == "PASS" or st.startswith(("REUSED:", "PASS (perf_counter-only WARN"))   # merge_gate.stage_passed
    rows = sm.get("rows", {})
    ok = "error" not in sm and sm.get("rc") == 0 and bool(planned) and all(green(str(rows.get(n, {}).get("status", ""))) for n in planned)
    return "PASS" if ok else "FAIL"


def manifest_rows(p):
    lines = Path(p).read_text(encoding="utf-8-sig").splitlines()[1:]
    return {r["relpath"]: r for r in csv.DictReader(lines)}


def manifest_delta(mine, theirs):
    """relpaths in manifest `mine` whose sha256 manifest `theirs` lacks."""
    return sorted(rel for rel, r in mine.items() if theirs.get(rel, {}).get("sha256") != r["sha256"])


def candidate_install():
    """The gate's deployed candidate: a kept stock copy (<seed>-stock-*), --keep-candidate's merge-gate-candidate, else the
    pristine deployed seed gate_reuse snapshots right after the candidate deploy (the stock copy is removed at cleanup).
    Never INSTALL: that is the published v0.4 the candidate is diffed against."""
    base = Path(G, "drv/E/mc2-installs")
    stock = sorted(base.glob("*-stock-*"), key=lambda p: p.stat().st_mtime, reverse=True) if base.is_dir() else []
    for p in (*stock, base / "merge-gate-candidate", base / "_gate-reuse/deployed"):
        if (p / ".deployed_manifest.csv").is_file():
            return p
    sys.exit(f"laptop_gate pack: no candidate install with a .deployed_manifest.csv under {base}")


def r_pack(window):
    """tar of every file in the candidate's manifest that differs from the PC's v0.4 manifest (in/<W>/pc.manifest.csv)."""
    out = Path(G, "out", window)
    cand = candidate_install()
    delta = manifest_delta(manifest_rows(cand / ".deployed_manifest.csv"), manifest_rows(Path(G, "in", window, "pc.manifest.csv")))
    tarp = out / "payload.tar"
    with tarfile.open(tarp, "w") as tar:
        for rel in delta:
            tar.add(cand / rel, arcname="files/" + rel, recursive=False)
        tar.add(cand / ".deployed_manifest.csv", arcname=".deployed_manifest.csv")
    (out / "payload.sha256").write_text(f"{sha256(tarp)}  payload.tar\n{len(delta)} files\n")
    print(f"payload from {cand}: {len(delta)} files, {tarp.stat().st_size} bytes")


EVIDENCE_DIRS = ("before", "after", "ab")   # merge_gate proof subdirs holding the day/night captures and montages
SYNC_SKIP = {".seeded", ".tree-synced"}   # laptop-only markers (seed() and the first sha-mode sync)
# Runtime outputs any game run on either side rewrites (pipeline cache, vegetation diag), not inputs to a capture: W129 run 4's
# preflight failed because a PC run touched them between sync_tree's listing and its tar.
SYNC_SKIP_RUNTIME = ("cache/", "veg_diag.txt")


def tree_rows(root, sha=False):
    """Sorted "rel|size|mtime" rows for every file under root (links not followed); sha=True appends "|sha256"."""
    root, rows = Path(root), []
    for d, _, files in os.walk(root):
        for f in files:
            p = Path(d, f)
            rel = p.relative_to(root).as_posix()
            if rel in SYNC_SKIP or rel.startswith(SYNC_SKIP_RUNTIME):
                continue
            st = p.stat()
            rows.append(f"{rel}|{st.st_size}|{int(st.st_mtime)}" + (f"|{sha256(p)}" if sha else ""))
    return sorted(rows)


def tree_diff(src, dst):
    """(copy, touch, delete) relpaths making dst equal src. copy: missing in dst or different size/mtime (sha rows: different
    content); touch: sha rows with equal content and another mtime; delete: only in dst."""
    s = {r.split("|", 1)[0]: r.split("|")[1:] for r in src}
    d = {r.split("|", 1)[0]: r.split("|")[1:] for r in dst}
    copy, touch = [], []
    for rel, (size, mt, *sh) in sorted(s.items()):
        o = d.get(rel)
        if o is None or o[0] != size or (sh and o[2:] != sh) or (not sh and o[1] != mt):
            copy.append(rel)
        elif o[1] != mt:
            touch.append(rel)
    return copy, touch, sorted(set(d) - set(s))


def tree_digest(rows):
    """"<count>:<sha256>" of the sorted size+mtime rows (sha columns dropped)."""
    rows = sorted("|".join(r.split("|")[:3]) for r in rows)
    return f"{len(rows)}:{hashlib.sha256(chr(10).join(rows).encode()).hexdigest()}"


def tree_problems(pc_rows, mine):
    """[] when the size+mtime listings match, else the digest mismatch plus the first 10 differing rows."""
    a, b = (sorted("|".join(r.split("|")[:3]) for r in x) for x in (pc_rows, mine))
    if tree_digest(a) == tree_digest(b):
        return []
    diffs = sorted([f"pc-only {r}" for r in set(a) - set(b)] + [f"laptop-only {r}" for r in set(b) - set(a)],
                   key=lambda s: s.split(" ", 1)[1])
    return [f"install tree {tree_digest(b)} != PC v0.4 {tree_digest(a)}", *diffs[:10]]


def r_tree(sha="0", root=INSTALL, name="tree.txt"):
    """Write root's (default INSTALL) tree rows to tmp/<name> for the PC to fetch."""
    Path(G, "tmp", name).write_text("\n".join(tree_rows(root, sha == "1")) + "\n", encoding="utf-8")


def r_apply(listfile, root=INSTALL):
    """Apply "D|rel" (delete) and "T|rel|mtime" (set mtime) lines to root (default INSTALL)."""
    for line in Path(listfile).read_text(encoding="utf-8").splitlines():
        op, rel, *mt = line.split("|")
        if not rel.strip("./"):
            continue   # never the root itself
        p = Path(root, rel)
        if op == "D":
            p.unlink(missing_ok=True)
        elif op == "T":
            os.utime(p, (int(mt[0]), int(mt[0])))


def r_refcheck(sums):
    """Problems with the laptop reference copy against a SHA256SUMS file ("<sha>  <rel>" lines); [] when every file matches."""
    if not Path(sums).is_file():
        return [f"missing {sums}"]
    bad = []
    for line in Path(sums).read_text(encoding="utf-8").splitlines():
        want, rel = line.split("  ", 1)
        p = Path(REF, rel)
        if not p.is_file() or sha256(p) != want:
            bad.append(f"ref mismatch: {rel}")
    return bad


def toolchain_problems(cn, e):
    """The executables and files configure_ninja/ninja_build/kit_payload.build_importer launch or read, resolved as they resolve them.
    Other deploy_payload children: sys.executable, git (PATH check), wmic (psutil fallback, failure tolerated), ldd (Linux only)."""
    need = {"VCVARS": cn.VCVARS, "NINJA": cn.NINJA, "JOBS file": JOBS}
    if e.get("MC2_COMPILE_CACHE") != "0":
        need["SCCACHE_EXE"] = cn.SCCACHE_EXE
    bad = [f"missing {k}: {v}" for k, v in need.items() if not Path(v).exists()]
    try:
        cn.find_cmake()
    except SystemExit:
        bad.append("missing cmake (find_cmake: VS2022 BuildTools or PATH)")
    return bad


def published_problems(src, published, tip, is_ancestor):
    """merge_gate's capture-before and stage_published read the published install (the laptop's v0.4 junction = INSTALL):
    its mc2.exe must be the PC's published v0.4 build and an ancestor of the stage tip, else the before-captures are stale."""
    if not src:
        return ["published install mc2.exe has no MC2FP1 fingerprint"]
    bad = []
    if published and not (src.startswith(published) or published.startswith(src)):
        bad.append(f"published install mc2.exe src {src} != the PC's published v0.4 {published} (laptop_gate refreshes it)")
    if tip and not is_ancestor(src, tip):
        bad.append(f"published install mc2.exe src {src} is not an ancestor of {tip}")
    return bad


def r_preflight(window="-", published=""):
    """Everything the stages need before a full run: Python packages, data roots, tools. Prints one line per problem."""
    import importlib
    import shutil
    bad = provision_problems()   # first: a missing helper or seed is a preflight red, never a mid-gate one
    for m in PACKAGES:   # import, not find_spec: a source-built extension can be present and still fail to load its DLLs
        try:
            importlib.import_module(m)
        except Exception as exc:  # noqa: BLE001
            bad.append(f"python package missing or broken: {m} ({exc})")
    for path, why in ((REPO + "/.git", "laptop clone"), (INSTALL + "/.seeded", "seeded install"),
                      (G + "/drv/G/mc2-assets/out/scifi-kit/round2/stock/.seeded", "G: stock mirror (kit_art)"),
                      (GIT_BASH, "MC2_BASH")):
        if not Path(path).exists():
            bad.append(f"missing {why}: {path}")
    bad += [f"tool not on PATH: {t}" for t in ("git", "tar") if not shutil.which(t)]
    r_subst()   # JOBS is read at A:/Games/mc2-ops/JOBS
    sys.path.insert(0, REPO + "/scripts")
    import configure_ninja
    bad += toolchain_problems(configure_ninja, gate_env())
    import exe_fingerprint
    fp = exe_fingerprint.parse_literal(Path(INSTALL, "mc2.exe").read_bytes()) if Path(INSTALL, "mc2.exe").is_file() else None
    tip = None if window in (None, "-") else "claude/stage-" + window
    bad += published_problems((fp or {}).get("sha"), published, tip,
                              lambda a, b: subprocess.run(["git", "-C", REPO, "merge-base", "--is-ancestor", a, b],
                                                                       **console_child.child_kwargs()).returncode == 0)
    bad += r_refcheck(REF + "/SHA256SUMS")
    bad += mods_problems()
    pc_tree = Path(G, "in/pc.tree.txt")   # the PC v0.4's rows, uploaded by sync_tree
    bad += tree_problems(pc_tree.read_text(encoding="utf-8").splitlines(), tree_rows(INSTALL)) if pc_tree.is_file() \
        else [f"missing {pc_tree} (the PC v0.4 tree listing sync_tree uploads)"]
    print("\n".join(bad) if bad else "preflight OK")
    return 1 if bad else 0


def mods_problems(dst=MODS_DST):
    """One line per campaign mod --keep-candidate stages that is not mirrored (with its data/) under the laptop's EROOT."""
    return [f"missing campaign mod {n} under {dst}: run tools/laptop_gate.py from the PC, which mirrors E:/mc2-ref/campaign-mods"
            for n in MODS_NAMES if not Path(dst, n, "data").is_dir()]


def r_ref_commit():
    """Verify a freshly extracted reference copy against SHA256SUMS.new, then make it the accepted SHA256SUMS."""
    bad = r_refcheck(REF + "/SHA256SUMS.new")
    if bad:
        print("\n".join(bad[:20]))
        return 1
    os.replace(REF + "/SHA256SUMS.new", REF + "/SHA256SUMS")
    return 0


# ------------------------------------------------------------------- PC half

def ref_sums(src_root):
    """SHA256SUMS text for the E:/mc2-ref subset the gate reads: the pinned kit files (shas from the manifests) and REF_EXTRA."""
    lines = []
    for m in REF_MANIFESTS:
        manifest = json.loads(Path(src_root, m).read_text(encoding="utf-8"))
        store = Path(manifest["store"]).relative_to(PC_REF).as_posix()
        lines += [f"{r['sha256']}  {store}/{r['path']}" for r in manifest["files"]]
    lines += [f"{sha256(PC_REF / rel)}  {rel}" for rel in REF_EXTRA]
    return "\n".join(lines) + "\n"


def seed_ref():
    """Copy the E:/mc2-ref subset to the laptop's REF when its accepted SHA256SUMS differs, sha-verified on the laptop."""
    text = ref_sums(Path(__file__).resolve().parents[1])
    want = hashlib.sha256(text.encode()).hexdigest()
    rc, out = ssh(f'certutil -hashfile "{REF}/SHA256SUMS" SHA256', check=False)
    if rc == 0 and want in out.replace(" ", "").lower():
        return
    print("copying the E:/mc2-ref subset to the laptop", flush=True)
    tmp = Path(os.environ.get("TEMP", "."))
    local, lst, sums = tmp / "mc2gate-ref.tar", tmp / "mc2gate-ref.lst", tmp / "SHA256SUMS.new"
    try:
        sums.write_text(text, encoding="utf-8", newline="\n")
        lst.write_text("\n".join(line.split("  ", 1)[1] for line in text.splitlines()) + "\n", encoding="utf-8", newline="\n")
        # the PC's tar is Windows bsdtar: it reads C:\ paths as files and rejects --force-local
        if subprocess.run(["tar", "-cf", str(local), "-C", str(PC_REF), "-T", str(lst)], **console_child.child_kwargs()).returncode:
            sys.exit("ref tar failed")
        scp(local, f"{HOST}:{G}/tmp/ref.tar")
        ssh(f'powershell -c "New-Item -ItemType Directory -Force {REF} | Out-Null" && tar -xf {G}/tmp/ref.tar -C {REF} '
            f"&& del {G.replace('/', chr(92))}\\tmp\\ref.tar")
        scp(sums, f"{HOST}:{REF}/SHA256SUMS.new")
    finally:
        for p in (local, lst, sums):
            p.unlink(missing_ok=True)
    if ssh(remote_py("ref-commit"), check=False, stream=True)[0]:
        sys.exit("laptop reference copy failed sha verification")
    print("reference copy verified")


def seed_stock():
    """One-time: mirror G:/mc2-assets/out/scifi-kit/round2/stock (kit_art row input, ~135 MB) to the laptop's drv/G."""
    dst = G + "/drv/G/mc2-assets/out/scifi-kit/round2"
    if ssh(f"if exist {dst.replace('/', chr(92))}\\stock\\.seeded echo yes")[1].strip() == "yes":
        return
    local = Path(os.environ.get("TEMP", ".")) / "mc2gate-stock.tar"
    try:
        if subprocess.run(["tar", "-cf", str(local), "-C", str(PC_STOCK.parent), PC_STOCK.name], **console_child.child_kwargs()).returncode:
            sys.exit("stock tar failed")
        scp(local, f"{HOST}:{G}/tmp/stock.tar")
        ssh(f'powershell -c "New-Item -ItemType Directory -Force {dst} | Out-Null" && tar -xf {G}/tmp/stock.tar -C {dst} '
            f"&& del {G.replace('/', chr(92))}\\tmp\\stock.tar && cmd /c echo ok> {dst.replace('/', chr(92))}\\stock\\.seeded")
    finally:
        local.unlink(missing_ok=True)
    print("mirrored", PC_STOCK)


def seed():
    """One-time: stream the PC's lean seed (published v0.4 payload + game data) into C:/mc2gate/install."""
    seed_stock()
    if ssh(f"if exist {INSTALL.replace('/', chr(92))}\\.seeded echo yes")[1].strip() == "yes":
        print("laptop install already seeded"); return
    print("seeding laptop install from", PC_SEED, flush=True)
    t = time.time()
    # A piped ssh stdin on Windows OpenSSH crawls (~0.5 MB/s); one tar file through scp, extracted on the laptop, is ~20x faster.
    local = Path(os.environ.get("TEMP", ".")) / "mc2gate-seed.tar"
    try:
        if subprocess.run(["tar", "-cf", str(local), "-C", str(PC_SEED), "."], **console_child.child_kwargs()).returncode:
            sys.exit("seed tar failed")
        scp(local, f"{HOST}:{G}/tmp/seed.tar")
        ssh(f"tar -xf {G}/tmp/seed.tar -C {INSTALL} && del {G.replace('/', chr(92))}\\tmp\\seed.tar")
    finally:
        local.unlink(missing_ok=True)
    want = sha256(PC_SEED / "mc2.exe")
    got = ssh(f'certutil -hashfile "{INSTALL}/mc2.exe" SHA256')[1].splitlines()[1].strip()
    if got.lower() != want.lower():
        sys.exit(f"seed mc2.exe sha mismatch {got} != {want}")
    ssh(f"cmd /c echo ok> {INSTALL.replace('/', chr(92))}\\.seeded")
    print(f"seeded in {time.time() - t:.0f}s, mc2.exe sha256 {want}")


def published_sha():
    """The MC2FP1 source sha of the PC's published v0.4 mc2.exe (what merge_gate on the PC captures 'before' with)."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
    import exe_fingerprint
    return (exe_fingerprint.parse_literal((PC_V04 / "mc2.exe").read_bytes()) or {}).get("sha", "")


CHUNK = 2 << 30   # bytes per tar chunk


def remote_rows(sha, step="tree", name="tree.txt"):
    ssh(remote_py(step, "-", "1" if sha else "0"))
    local = PC_TMP / ("laptop." + name)
    scp(f"{HOST}:{G}/tmp/{name}", local)
    try:
        # an empty tree is written as "\n": without the filter its "" row became a delete of the root itself (W133 sync_mods)
        return [r for r in local.read_text(encoding="utf-8").splitlines() if r]
    finally:
        local.unlink(missing_ok=True)


def chunks(rels, sizes, limit=CHUNK):
    """rels split into consecutive groups of at most `limit` bytes (a larger file gets a group of its own)."""
    out, cur, n = [], [], 0
    for rel in rels:
        if cur and n + sizes[rel] > limit:
            out.append(cur); cur, n = [], 0
        cur.append(rel); n += sizes[rel]
    return out + ([cur] if cur else [])


def sync_tree():
    """Make the laptop's published install (v0.4 junction = INSTALL) equal the PC's v0.4 file for file, the tree merge_gate's
    capture-before and stage_published read. Both ways: copy missing/different files in ~2 GB tar chunks (tar keeps mtimes),
    then delete laptop-only files. The first sync compares sha256 (only real differences cross the wire, mtimes are fixed in
    place); later syncs size+mtime. Re-running resumes: every call re-diffs. Uploads the PC rows for the preflight."""
    PC_TMP.mkdir(parents=True, exist_ok=True)
    first = ssh(f"if exist {INSTALL.replace('/', chr(92))}\\.tree-synced (echo yes) else (echo no)")[1].strip() != "yes"
    t = time.time()
    pc = tree_rows(PC_V04, sha=first)
    copy, touch, delete = tree_diff(pc, remote_rows(first))
    sizes = {r.split("|", 1)[0]: int(r.split("|")[1]) for r in pc}
    print(f"sync_tree ({'sha256' if first else 'size+mtime'}, {time.time() - t:.0f}s): copy {len(copy)} files "
          f"({sum(sizes[r] for r in copy) >> 20} MB), touch {len(touch)}, delete {len(delete)}", flush=True)
    lst, local, ops = PC_TMP / "sync.lst", PC_TMP / "sync.tar", PC_TMP / "sync.ops"
    try:
        for i, group in enumerate(chunks(copy, sizes), 1):
            lst.write_text("\n".join(group) + "\n", encoding="utf-8", newline="\n")
            if subprocess.run(["tar", "-cf", str(local), "-C", str(PC_V04), "-T", str(lst)], **console_child.child_kwargs()).returncode:
                sys.exit("sync tar failed")
            scp(local, f"{HOST}:{G}/tmp/sync.tar")
            ssh(f"tar -xf {G}/tmp/sync.tar -C {INSTALL} && del {G.replace('/', chr(92))}\\tmp\\sync.tar")
            print(f"  chunk {i}: {len(group)} files, {local.stat().st_size >> 20} MB, {time.time() - t:.0f}s", flush=True)
        if touch or delete:
            mt = {r.split("|", 1)[0]: r.split("|")[2] for r in pc}
            ops.write_text("".join([*(f"T|{r}|{mt[r]}\n" for r in touch), *(f"D|{r}\n" for r in delete)]), encoding="utf-8")
            scp(ops, f"{HOST}:{G}/tmp/sync.ops")
            ssh(remote_py("apply", "-", f"{G}/tmp/sync.ops"))
        lst.write_text("\n".join("|".join(r.split("|")[:3]) for r in pc) + "\n", encoding="utf-8", newline="\n")
        ssh("cmd /c if not exist C:\\mc2gate\\in mkdir C:\\mc2gate\\in")
        scp(lst, f"{HOST}:{G}/in/pc.tree.txt")
        if first:
            ssh(f"cmd /c echo ok> {INSTALL.replace('/', chr(92))}\\.tree-synced")
    finally:
        for p in (lst, local, ops):
            p.unlink(missing_ok=True)
    print(f"sync_tree done in {time.time() - t:.0f}s", flush=True)


def sync_mods(src=MODS_SRC):
    """Idempotent size+mtime mirror of the PC's campaign-mods reference (read only on the PC) into MODS_DST on the laptop, where
    merge_gate's stage_candidate_mods reads it (W132 keep-candidate: 'MechCommanderOmnitech missing under ...campaign-mods').
    Copies missing/different files in tar chunks (tar keeps mtimes), fixes mtimes, deletes laptop-only files; a second run is a no-op."""
    missing = [n for n in MODS_NAMES if not Path(src, n, "data").is_dir()]
    if missing:
        sys.exit(f"laptop_gate: campaign mod(s) {', '.join(missing)} missing under {src} on the PC")
    PC_TMP.mkdir(parents=True, exist_ok=True)
    t = time.time()
    pc = tree_rows(src)
    copy, touch, delete = tree_diff(pc, remote_rows(False, "mods-tree", "mods.tree.txt"))
    sizes = {r.split("|", 1)[0]: int(r.split("|")[1]) for r in pc}
    print(f"sync_mods (size+mtime): copy {len(copy)} files ({sum(sizes[r] for r in copy) >> 20} MB), touch {len(touch)}, delete {len(delete)}", flush=True)
    lst, local, ops = PC_TMP / "mods.lst", PC_TMP / "mods.tar", PC_TMP / "mods.ops"
    try:
        for group in chunks(copy, sizes):
            lst.write_text("\n".join(group) + "\n", encoding="utf-8", newline="\n")
            if subprocess.run(["tar", "-cf", str(local), "-C", str(src), "-T", str(lst)], **console_child.child_kwargs()).returncode:
                sys.exit("campaign-mods tar failed")
            scp(local, f"{HOST}:{G}/tmp/mods.tar")
            ssh(f'powershell -c "New-Item -ItemType Directory -Force {MODS_DST} | Out-Null" && tar -xf {G}/tmp/mods.tar -C {MODS_DST} '
                f"&& del {G.replace('/', chr(92))}\\tmp\\mods.tar")
        if touch or delete:
            mt = {r.split("|", 1)[0]: r.split("|")[2] for r in pc}
            ops.write_text("".join([*(f"T|{r}|{mt[r]}\n" for r in touch), *(f"D|{r}\n" for r in delete)]), encoding="utf-8")
            scp(ops, f"{HOST}:{G}/tmp/mods.ops")
            ssh(remote_py("mods-apply", "-", f"{G}/tmp/mods.ops"))
    finally:
        for p in (lst, local, ops):
            p.unlink(missing_ok=True)
    print(f"sync_mods done in {time.time() - t:.0f}s", flush=True)


def provision_files(eroot=Path("E:/")):
    """(laptop path relative to C:/mc2gate, PC source) for every file the remote half needs before it runs: itself, its HELPERS
    and the E-rooted seeds the contracts read under MC2_GATE_EROOT (E_EXTRA, e.g. the scifi_kit catalog), plus the generated
    deploy inputs (LAYER_EXTRA) copied from the published install into the laptop clone."""
    repo = Path(__file__).resolve().parents[1]
    clone = REPO.removeprefix(G + "/")
    return ([("bin/laptop_gate.py", Path(__file__).resolve())] + [("bin/" + Path(h).name, repo / h) for h in HELPERS]
            + [("drv/E/" + rel, Path(eroot, rel)) for rel in E_EXTRA]
            + [(f"{clone}/{rel}", PC_V04 / rel) for rel in LAYER_EXTRA])


def provision(bin_only=False):
    """Self-provisioning, run at the start of every PC-side run (and --preflight): sync every provision_files() entry whose laptop
    sha differs, then upload bin/PROVISION.sha256, which the remote preflight verifies (missing or stale = a clear preflight red).
    bin_only: just the remote half and its helpers (before init)."""
    ssh("cmd /c if not exist C:\\mc2gate\\bin mkdir C:\\mc2gate\\bin")
    lines = []
    for rel, src in provision_files():
        if bin_only and not rel.startswith("bin/"):
            continue
        if not src.is_file():
            sys.exit(f"laptop_gate: provision source missing on the PC: {src}")
        want, dst = sha256(src), f"{G}/{rel}"
        rc, out = ssh(f'certutil -hashfile "{dst}" SHA256', check=False)
        if not (rc == 0 and want in out.replace(" ", "").lower()):
            print(f"provision: copying {src} -> {dst}", flush=True)
            ssh(f'cmd /c mkdir "{str(Path(dst).parent).replace("/", chr(92))}" 2>nul & exit /b 0', check=False)
            scp(src, f"{HOST}:{dst}")
        lines.append(f"{want}  {rel}")
    if bin_only:
        return
    sums = PC_TMP / "PROVISION.sha256"
    sums.parent.mkdir(parents=True, exist_ok=True)
    try:
        sums.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
        scp(sums, f"{HOST}:{G}/bin/PROVISION.sha256")
    finally:
        sums.unlink(missing_ok=True)


def provision_problems(root=G):
    """Remote: every file in bin/PROVISION.sha256 exists under `root` with its sha. Missing manifest, file or sha = one line each."""
    sums = Path(root, "bin/PROVISION.sha256")
    if not sums.is_file():
        return [f"missing {sums}: run tools/laptop_gate.py from the PC, which provisions the laptop"]
    bad = []
    for line in sums.read_text(encoding="utf-8").splitlines():
        want, rel = line.split("  ", 1)
        path = Path(root, rel)
        if not path.is_file():
            bad.append(f"missing provisioned file {path} (helper or E-root seed; laptop_gate.py on the PC uploads it)")
        elif sha256(path) != want:
            bad.append(f"stale provisioned file {path}: sha256 != the PC's {want[:12]}")
    return bad


def pc_main(args):
    w = args.window
    branch = "claude/stage-" + w
    head = subprocess.check_output(["git", "rev-parse", branch], text=True, **console_child.child_kwargs(redirected=True)).strip()
    t0 = time.time()
    provision(bin_only=True)   # the remote half itself, so init can run (init clones the repo the layer seeds go into)
    ssh(remote_py("init"), stream=True)
    provision()   # helpers + E-root and layer seeds, sha-checked; the remote preflight verifies PROVISION.sha256
    seed()
    seed_ref()
    sync_mods()
    sync_tree()
    print(f"pushing {branch} @ {head[:9]}", flush=True)
    genv = dict(env(), GIT_SSH_COMMAND="ssh " + " ".join(SSH_OPTS))
    url = f"ssh://{HOST}/{REPO}"
    # The LFS pre-push hook runs; the laptop clone has no LFS locking API (a stage adding new LFS objects fails here, loudly).
    nolock = f"lfs.https://{HOST.split('@')[1]}/{REPO}.git/info/lfs.locksverify=false"
    r = subprocess.run(["git", "-c", nolock, "push", "-q", "-f", f"--receive-pack={G}/bin/rp.cmd", url,
                        f"{branch}:refs/heads/{branch}", "claude/nifty-mendeleev:refs/heads/claude/nifty-mendeleev"], env=genv,
                       **console_child.child_kwargs())
    if r.returncode:
        sys.exit("push to laptop clone failed")
    ssh(f"cmd /c mkdir C:\\mc2gate\\in\\{w} 2>nul & exit /b 0", check=False)
    scp(PC_V04 / ".deployed_manifest.csv", f"{HOST}:{G}/in/{w}/pc.manifest.csv")
    if ssh(remote_py("preflight", w, published_sha()), check=False, stream=True)[0]:   # after the push: it checks the stage tip
        sys.exit("laptop preflight failed (above); fix it before a full run")
    print("running build + gate on the laptop (long)", flush=True)
    dest = PROOFS / w
    incoming = PROOFS / (w + ".incoming")
    for stale in (dest, incoming, PROOFS / (w + ".rejected")):   # a failed rerun must never leave an old PASS to read
        shutil.rmtree(stale, ignore_errors=True)
    run_rc, _ = ssh(remote_run_cmd(w, args.gate_args), check=False, stream=True)
    pack_rc, _ = ssh(remote_py("pack", w), check=False, stream=True)
    ok, why, sm = pull_and_accept(w, head, run_rc, pack_rc, dest, incoming)
    print("SUMMARY", json.dumps(sm), f"total_wall_s={time.time() - t0:.0f}")
    if not ok:
        print(f"laptop_gate: result REJECTED: {why} (diagnostics in {PROOFS / (w + '.rejected')})", flush=True)
        return 1
    if (dest / "gate-verdict.json").is_file():
        print("verdict file sha256", sha256(dest / "gate-verdict.json"), dest / "gate-verdict.json")
    return exit_code(sm)


def sim_command_list(window, rows=None, pair=False):
    """Lines (`env... command`) of the sim rows laptop_gate --sim would run, from the checkout's own merge_gate.sim_plan()."""
    import merge_gate
    steps = merge_gate.sim_plan("<deployed candidate stock copy>", f"{G}/out/{window}/sim", rows, pair)
    return [" ".join(f"{k}={v}" for k, v in env.items()) + " " + subprocess.list2cmdline(cmd) for _, cmd, env in steps]


def merge_sim_summary(dest, sim):
    """Put the pulled sim summary under the "sim" key of dest/summary.json (created when the window has no laptop summary yet).
    -> a problem string when the sim head is not the window summary's head, else ''."""
    path = dest / "summary.json"
    try:
        sm = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        sm = {}
    why = ""
    if sm.get("head") and sim.get("head") and not _same_sha(sm["head"], sim["head"]):
        why = f"sim head {sim['head']} is not the window summary head {sm['head']}"
    sm["sim"] = sim
    dest.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(sm, indent=1), encoding="utf-8")
    return why


def sim_main(a):
    """--sim: provision, run the remote sim step, pull out/<W>/sim into <proofs>/<W>/sim, merge its summary. 0 only on an all-green PASS."""
    w = a.window
    rows = [r for r in (a.sim_rows or "").split(",") if r]
    t0 = time.time()
    provision(bin_only=True)
    provision()
    sync_mods()
    tokens = [*rows, *(["--pair"] if a.sim_pair else [])]
    run_rc, _ = ssh(remote_py("sim", w, *(["--", *tokens] if tokens else [])), check=False, stream=True)
    dest = PROOFS / w
    shutil.rmtree(dest / "sim", ignore_errors=True)
    dest.mkdir(parents=True, exist_ok=True)
    if not pull_one(f"{G}/out/{w}/sim", dest, recursive=True):
        print("laptop_gate --sim: could not pull out/%s/sim (ssh status %s)" % (w, run_rc))
        return 1
    try:
        sim = json.loads((dest / "sim/summary.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        print("laptop_gate --sim: sim/summary.json missing or unreadable")
        return 1
    why = merge_sim_summary(dest, sim)
    print("SIM", json.dumps({k: v for k, v in sim.items() if k != "commands"}), f"total_wall_s={time.time() - t0:.0f}")
    if why or run_rc:
        print(f"laptop_gate --sim: REJECTED: {why or 'ssh exit status ' + str(run_rc)}")
        return 1
    return 0 if sim.get("verdict") == "PASS" else 1


PULL_REQUIRED = ("summary.json", "gate-verdict.json", "gate.log", "gate.console.log", "build.log", "payload.tar", "payload.sha256")


def pull_one(remote, local, recursive=False):
    """scp one remote path to local; True only on rc 0."""
    r = subprocess.run(["scp", "-q", *(["-r"] if recursive else []), *SSH_OPTS, f"{HOST}:{remote}", str(local)], env=env(),
                       **console_child.child_kwargs())
    return r.returncode == 0


def _same_sha(a, b):
    return bool(a) and bool(b) and (a.startswith(b) or b.startswith(a))


def pull_and_accept(w, expected_head, run_rc, pack_rc, dest, incoming):
    """Pull the laptop's results into `incoming`, accept them only if every check passes, then rename incoming -> dest
    atomically. On any failure `dest` does not exist and the pulled diagnostics sit in <w>.rejected. -> (ok, reason, summary)."""
    incoming.mkdir(parents=True, exist_ok=True)
    missing = [n for n in PULL_REQUIRED if not pull_one(f"{G}/out/{w}/{n}", incoming / n)]
    pull_one(f"{G}/out/{w}/preflight.log", incoming / "preflight.log")   # fast_red's log, optional (absent when it never ran)
    ev_ok = pull_one(f"{G}/out/{w}/evidence", incoming, recursive=True)
    sm = {}
    try:
        sm = json.loads((incoming / "summary.json").read_text())
    except (OSError, ValueError):
        pass
    why = ""
    if run_rc or pack_rc:
        why = f"ssh exit status run={run_rc} pack={pack_rc}"
    elif missing:
        why = "partial pull, missing " + ", ".join(missing)
    elif not ev_ok:
        why = "capture evidence (images/montages) could not be pulled"
    elif sm.get("gate_rc") != 0 or sm.get("build_rc") != 0 or "error" in sm:
        why = f"gate_rc={sm.get('gate_rc')} build_rc={sm.get('build_rc')} error={sm.get('error')}"
    else:
        try:
            verdict = json.loads((incoming / "gate-verdict.json").read_text())
        except (OSError, ValueError):
            verdict = {}
        if not _same_sha(verdict.get("head"), expected_head):
            why = f"verdict head {verdict.get('head')} is not the pushed stage sha {expected_head}"
        elif not _same_sha(sm.get("head"), expected_head):
            why = f"summary head {sm.get('head')} is not the pushed stage sha {expected_head}"
        elif not extract_payload(incoming):
            why = "payload.tar sha256 mismatch"
    if why:
        rej = incoming.with_name(w + ".rejected")
        shutil.rmtree(rej, ignore_errors=True)
        os.replace(incoming, rej)
        return False, why, sm
    shutil.rmtree(dest, ignore_errors=True)
    os.replace(incoming, dest)
    return True, "", sm


def extract_payload(dest):
    """sha-check dest/payload.tar and extract it into a cleared dest/payload (no files left from an earlier run)."""
    tarp = dest / "payload.tar"
    if not tarp.is_file():
        return False
    want = (dest / "payload.sha256").read_text().split()[0]
    got = sha256(tarp)
    print(f"payload.tar sha256 {got} {'OK' if got == want else 'MISMATCH expected ' + want}")
    if got == want:
        shutil.rmtree(dest / "payload", ignore_errors=True)
        with tarfile.open(tarp) as tar:
            tar.extractall(dest / "payload", filter="data")
        print(f"payload extracted: {dest / 'payload'} (deploy with --from-payload)")
    return got == want


def exit_code(sm):
    """0 only for an explicit PASS verdict; FAIL, no verdict or a setup crash are non-zero."""
    return 0 if sm.get("verdict") == "PASS" else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("window", nargs="?")
    ap.add_argument("--remote", help=argparse.SUPPRESS)
    ap.add_argument("--preflight", action="store_true", help="check the laptop's packages, data roots and tools; no build or gate")
    ap.add_argument("--sim", action="store_true", help="run merge_gate's determinism+saveload rows on the gate's deployed candidate (no push/build)")
    ap.add_argument("--sim-rows", help="comma list of sim rows (default: determinism-mc2_01/03/10/24, saveload-mc2_03/10)")
    ap.add_argument("--sim-pair", action="store_true", help="a/b pair for mc2_03 determinism, as on --weekly or a sim-sensitive diff")
    ap.add_argument("--dry-run", action="store_true", help="with --sim: print the command list and run nothing")
    ap.add_argument("gate_args", nargs="*", help="extra merge_gate.py args (after --)")
    a, rest = ap.parse_known_args()
    if a.remote:
        step = a.remote
        return {"init": r_init, "run": lambda: r_run(a.window, [x for x in a.gate_args + rest if x != "--"]), "pack": lambda: r_pack(a.window),
                "tree": lambda: r_tree(*(a.gate_args + rest)[:1]), "apply": lambda: r_apply((a.gate_args + rest)[0]),
                "mods-tree": lambda: r_tree((a.gate_args + rest)[0], MODS_DST, "mods.tree.txt"),
                "mods-apply": lambda: r_apply((a.gate_args + rest)[0], MODS_DST),
                "sim": lambda: r_sim(a.window, [x for x in a.gate_args + rest if x != "--"]),"preflight": lambda: r_preflight(a.window, *(a.gate_args + rest)[:1]), "ref-commit": r_ref_commit}[step]() or 0
    if a.preflight:
        provision()
        return ssh(remote_py("preflight", a.window or "-", published_sha()), check=False, stream=True)[0]
    if not a.window:
        ap.error("window required")
    if a.dry_run and not a.sim:
        ap.error("--dry-run is for --sim")
    if a.sim and a.dry_run:
        print("\n".join(sim_command_list(a.window, [r for r in (a.sim_rows or "").split(",") if r], a.sim_pair)))
        return 0
    if a.sim:
        return sim_main(a)
    return pc_main(a)


if __name__ == "__main__":
    sys.exit(main())
