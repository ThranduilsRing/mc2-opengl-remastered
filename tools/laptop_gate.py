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
"""
import argparse, csv, hashlib, json, os, subprocess, sys, tarfile, time
from pathlib import Path

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
REF = G + "/ref"   # the laptop's MC2_REF_ROOT: the subset of E:/mc2-ref the gate stages read
REF_MANIFESTS = ("data/kit/industrial-kit.json", "data/kit/vehicle-kit.json")   # deploy's pinned kits
REF_EXTRA = ("scifi-kit/mapping/inventory.csv",)   # kit_art row
PACKAGES = ("numpy", "PIL", "scipy", "skimage", "pefile", "pytest")
GIT_BASH = "C:/Program Files/Git/bin/bash.exe"
SSH_OPTS = ["-i", KEY, "-o", "BatchMode=yes", "-o", "LogLevel=ERROR", "-o", "ServerAliveInterval=30"]


def env():
    return dict(os.environ, HOME="C:/SPB_Data")


def ssh(cmd, check=True, stream=False):
    argv = ["ssh", *SSH_OPTS, HOST, cmd]
    if stream:
        r = subprocess.run(argv, env=env())
        out = ""
    else:
        r = subprocess.run(argv, env=env(), capture_output=True, text=True)
        out = r.stdout
    if check and r.returncode:
        sys.exit(f"laptop_gate: ssh failed rc={r.returncode}: {cmd[:120]}\n{getattr(r, 'stderr', '') or ''}")
    return r.returncode, out


def scp(src, dst, recursive=False):
    argv = ["scp", "-q", *(["-r"] if recursive else []), *SSH_OPTS, str(src), str(dst)]
    r = subprocess.run(argv, env=env())
    if r.returncode:
        sys.exit(f"laptop_gate: scp failed: {src} -> {dst}")


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def remote_py(step, *args):
    return f'py -3 {G}/bin/laptop_gate.py --remote {step} ' + " ".join(args)


# ---------------------------------------------------------------- laptop half

def r_subst():
    """A: and G: for this logon session (the laptop's D:/E: are a card reader, so E-paths use MC2_GATE_EROOT instead).
    G: holds the PC's G:/mc2-assets stock mirror that the kit_art contract row reads at hard-coded paths. Never I:."""
    for letter, target in (("A:", G + "/drv/A"), ("G:", G + "/drv/G")):
        os.makedirs(target, exist_ok=True)
        cur = subprocess.run(["subst"], capture_output=True, text=True).stdout
        if any(line.upper().startswith(letter) for line in cur.splitlines()):
            continue
        subprocess.run(["subst", letter, target.replace("/", "\\")], check=True)


def junction(link, target):
    link = Path(link)
    if link.exists():
        return
    link.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cmd", "/c", "mklink", "/J", str(link).replace("/", "\\"), str(target).replace("/", "\\")], check=True, capture_output=True)


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
        subprocess.run(["git", "clone", "-q", "C:/mc2lint/repo", str(repo)], check=True)
        subprocess.run(["git", "-C", str(repo), "config", "receive.denyCurrentBranch", "ignore"], check=True)
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
        r = subprocess.run(cmd, cwd=REPO, stdout=lf, stderr=subprocess.STDOUT, **kw)
    return r.returncode, round(time.time() - t, 1)


def r_run(window, extra):
    r_subst()
    out = Path(G, "out", window)
    out.mkdir(parents=True, exist_ok=True)
    for old in out.iterdir():
        old.unlink() if old.is_file() else None
    branch = "claude/stage-" + window
    e = dict(os.environ, TEMP=G + "/tmp", TMP=G + "/tmp", TMPDIR=G + "/tmp", PYTHONUTF8="1",
             MC2_BUILD_JOBS="7", MC2_BUILD_TOKENS="0", MC2_COMPILE_CACHE="0", MC2_GATE_EROOT=G + "/drv/E",
             # the laptop's only fixed drive is C: (its E: is a card reader): every E:-rooted gate path lives under drv/E
             MC2_GATE_REUSE_ROOT=G + "/drv/E/mc2-installs/_gate-reuse", MC2_STAGE_BASE=G + "/drv/E/mc2-installs",
             MC2_ISO_ALLOW_SYSTEM_DRIVE="1", MC2_REF_ROOT=REF,
             # Git's usr/bin is not on the laptop PATH; bin/bash.exe is the wrapper that puts coreutils on it
             MC2_BASH=GIT_BASH)
    sm = {"window": window, "host": "laptop"}
    git = lambda *a: subprocess.run(["git", "-C", REPO, *a], capture_output=True, text=True, env=e)
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
        proof = G + "/drv/E/mc2-archive-2026-10/proofs/merge-gate/" + window
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
        if (out / "gate-verdict.json").is_file():
            v = json.loads((out / "gate-verdict.json").read_text())
            sm["verdict"], sm["timings"] = v.get("verdict"), v.get("timings")
    (out / "summary.json").write_text(json.dumps(sm, indent=1))
    print(json.dumps({k: v for k, v in sm.items() if k != "timings"}))


def r_pack(window):
    """tar of every file in the laptop install's manifest that differs from the PC's v0.4 manifest (in/<W>/pc.manifest.csv)."""
    out = Path(G, "out", window)

    def rows(p):
        lines = Path(p).read_text(encoding="utf-8-sig").splitlines()[1:]
        return {r["relpath"]: r for r in csv.DictReader(lines)}
    mine, theirs = rows(Path(INSTALL, ".deployed_manifest.csv")), rows(Path(G, "in", window, "pc.manifest.csv"))
    delta = [rel for rel, r in mine.items() if theirs.get(rel, {}).get("sha256") != r["sha256"]]
    tarp = out / "payload.tar"
    with tarfile.open(tarp, "w") as tar:
        for rel in delta:
            tar.add(Path(INSTALL, rel), arcname="files/" + rel, recursive=False)
        tar.add(Path(INSTALL, ".deployed_manifest.csv"), arcname=".deployed_manifest.csv")
    (out / "payload.sha256").write_text(f"{sha256(tarp)}  payload.tar\n{len(delta)} files\n")
    print(f"payload: {len(delta)} files, {tarp.stat().st_size} bytes")


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


def r_preflight():
    """Everything the stages need before a full run: Python packages, data roots, tools. Prints one line per problem."""
    import importlib.util
    import shutil
    bad = [f"python package missing: {m}" for m in PACKAGES if not importlib.util.find_spec(m)]
    for path, why in ((REPO + "/.git", "laptop clone"), (INSTALL + "/.seeded", "seeded install"),
                      (G + "/drv/G/mc2-assets/out/scifi-kit/round2/stock/.seeded", "G: stock mirror (kit_art)"),
                      (GIT_BASH, "MC2_BASH")):
        if not Path(path).exists():
            bad.append(f"missing {why}: {path}")
    bad += [f"tool not on PATH: {t}" for t in ("git", "tar") if not shutil.which(t)]
    bad += r_refcheck(REF + "/SHA256SUMS")
    print("\n".join(bad) if bad else "preflight OK")
    return 1 if bad else 0


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
        if subprocess.run(["tar", "--force-local", "-cf", str(local), "-C", str(PC_REF), "-T", str(lst)]).returncode:
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
        if subprocess.run(["tar", "--force-local", "-cf", str(local), "-C", str(PC_STOCK.parent), PC_STOCK.name]).returncode:
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
        if subprocess.run(["tar", "-cf", str(local), "-C", str(PC_SEED), "."]).returncode:
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


def pc_main(args):
    w = args.window
    branch = "claude/stage-" + w
    head = subprocess.check_output(["git", "rev-parse", branch], text=True).strip()
    t0 = time.time()
    ssh("cmd /c if not exist C:\\mc2gate\\bin mkdir C:\\mc2gate\\bin")
    scp(__file__, f"{HOST}:{G}/bin/laptop_gate.py")
    ssh(remote_py("init"), stream=True)
    seed()
    seed_ref()
    if ssh(remote_py("preflight"), check=False, stream=True)[0]:
        sys.exit("laptop preflight failed (above); fix it before a full run")
    print(f"pushing {branch} @ {head[:9]}", flush=True)
    genv = dict(env(), GIT_SSH_COMMAND="ssh " + " ".join(SSH_OPTS))
    url = f"ssh://{HOST}/{REPO}"
    # The LFS pre-push hook runs; the laptop clone has no LFS locking API (a stage adding new LFS objects fails here, loudly).
    nolock = f"lfs.https://{HOST.split('@')[1]}/{REPO}.git/info/lfs.locksverify=false"
    r = subprocess.run(["git", "-c", nolock, "push", "-q", "-f", f"--receive-pack={G}/bin/rp.cmd", url,
                        f"{branch}:refs/heads/{branch}", "claude/nifty-mendeleev:refs/heads/claude/nifty-mendeleev"], env=genv)
    if r.returncode:
        sys.exit("push to laptop clone failed")
    ssh(f"cmd /c mkdir C:\\mc2gate\\in\\{w} 2>nul & exit /b 0", check=False)
    scp(PC_V04 / ".deployed_manifest.csv", f"{HOST}:{G}/in/{w}/pc.manifest.csv")
    extra = " ".join(args.gate_args)
    print("running build + gate on the laptop (long)", flush=True)
    rc, _ = ssh(remote_py("run", w, extra), check=False, stream=True)
    ssh(remote_py("pack", w), check=False, stream=True)
    dest = PROOFS / w
    dest.mkdir(parents=True, exist_ok=True)
    for name in ("summary.json", "gate-verdict.json", "gate.log", "gate.console.log", "build.log", "payload.tar", "payload.sha256"):
        r = subprocess.run(["scp", "-q", *SSH_OPTS, f"{HOST}:{G}/out/{w}/{name}", str(dest / name)], env=env())
        if r.returncode:
            print(f"laptop_gate: {name} not available", flush=True)
    tarp = dest / "payload.tar"
    if tarp.is_file():
        want = (dest / "payload.sha256").read_text().split()[0]
        got = sha256(tarp)
        print(f"payload.tar sha256 {got} {'OK' if got == want else 'MISMATCH expected ' + want}")
        if got == want:
            with tarfile.open(tarp) as tar:
                tar.extractall(dest / "payload", filter="data")
            print(f"payload extracted: {dest / 'payload'} (deploy with --from-payload)")
    if (dest / "gate-verdict.json").is_file():
        print("verdict file sha256", sha256(dest / "gate-verdict.json"), dest / "gate-verdict.json")
    sm = json.loads((dest / "summary.json").read_text()) if (dest / "summary.json").is_file() else {}
    print("SUMMARY", json.dumps(sm), f"total_wall_s={time.time() - t0:.0f}")
    return exit_code(sm)


def exit_code(sm):
    """0 only for an explicit PASS verdict; FAIL, no verdict or a setup crash are non-zero."""
    return 0 if sm.get("verdict") == "PASS" else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("window", nargs="?")
    ap.add_argument("--remote", help=argparse.SUPPRESS)
    ap.add_argument("--preflight", action="store_true", help="check the laptop's packages, data roots and tools; no build or gate")
    ap.add_argument("gate_args", nargs="*", help="extra merge_gate.py args (after --)")
    a, rest = ap.parse_known_args()
    if a.remote:
        step = a.remote
        return {"init": r_init, "run": lambda: r_run(a.window, a.gate_args + rest), "pack": lambda: r_pack(a.window),
                "preflight": r_preflight, "ref-commit": r_ref_commit}[step]() or 0
    if a.preflight:
        scp(__file__, f"{HOST}:{G}/bin/laptop_gate.py")
        return ssh(remote_py("preflight"), check=False, stream=True)[0]
    if not a.window:
        ap.error("window required")
    return pc_main(a)


if __name__ == "__main__":
    sys.exit(main())
