#!/usr/bin/env python3
"""sim_determinism - is the simulation the same run after run, and the same across a refactor?

WHAT: reads the `[SIMID v1] simframe=N t=T n=C graded=.. visual=.. strict=..` line the engine prints for every sim frame
(MC2_SIM_IDENTITY_TRACE) and compares two runs of one mission frame by frame over the frames both reached. It names the first divergent
sim frame and prints both lines. `run` launches the mission twice through scripts/run_smoke.py under MC2_SMOKE_FIXED_TIMESTEP=1,
MC2_DETERMINISTIC_RNG=1 and MC2_SIM_IDENTITY_TRACE=1 (the three gates that remove wall-clock and RNG-seed variation). `--baseline`
additionally compares the chain digest of the first N frames with a committed one (tests/smoke/sim_determinism/<mission>.json), which is
the "SIMID unchanged" proof for a refactor (ownership, enum, math steps) across commits. A baseline pins the install data fingerprint, the
mission seed and the extra env it was recorded under, so a check against other data or another seed fails instead of lying.
WHY: a step whose proof is "same behaviour" needs a behaviour oracle that is itself stable. Two runs of the same exe must agree before a
difference between two exes means anything. The runs end at different frames (the smoke duration is wall clock), so only the common
prefix is compared.

  py -3 tools/sim_determinism.py run mc2_03 --duration 20 [--baseline [path]]      # --baseline alone = tests/smoke/sim_determinism/mc2_03.json
  py -3 tools/sim_determinism.py run mc2_03 --duration 60 --ticks 900 --headless   # run a windowed, run b headless: the chains must be equal
  py -3 tools/sim_determinism.py check mc2_16 --env MC2_FIXTURE_ORDER_LIVENESS=1   # two runs + the committed baseline (env comes from the file too)
  py -3 tools/sim_determinism.py run mc2_10 --env-a MC2_CAMERA_PARK=-1500,2500,0,60,800@300 --env-b MC2_CAMERA_PARK=-2000,2900,90,50,600@300
  py -3 tools/sim_determinism.py pairs-until-fail --mission mc2_24 --max 8 --duration 30 --out <dir>   # stops at the first diverging pair
  --duration may be up to 60 (MAX_DURATION): the determinism window past sim tick ~3600 needs it (owner ruling 2026-10-13). Tier smokes and captures keep the 30 s cap of run_smoke.py;
  the runs here pass --sim-determinism to it, which is the only thing that lifts the cap.
  py -3 tools/sim_determinism.py compare run_a/mc2_03.log run_b/mc2_03.log
  py -3 tools/sim_determinism.py write-baseline run_a/mc2_03.log --frames 1200 --mission mc2_03 [--env NAME=VALUE]
  py -3 tools/sim_determinism.py selftest          # offline; the `sim_determinism` board row
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import console_child  # noqa: E402
from repo_intel import lane_model  # noqa: E402

PREFIX = "[SIMID v1] simframe="
_STRICT = re.compile(r" strict=\w+")
_RNG_COUNTS = re.compile(r" rng=(\d+)/\d+/\d+(?=\s|$)")
#: Only the appearance and weather draw counts are non-sim (D3 splits the appearance stream); the SIMULATION draw count stays in
#: every comparison and graded hash, so one extra discarded sim draw (which shifts all later gameplay randomness) fails.
_RNG_NONSIM = r" rng=\1"
_COUNTS = ("n", "nobj", "nabl")


def observation_error(lines) -> str:
    """Reject incomplete observations, including a dropped tick in both runs."""
    previous = None
    for i, line in enumerate(lines):
        fields = dict(re.findall(r"(\w+)=([^ ]+)", line))
        for key in ("simframe", *_COUNTS):
            if not fields.get(key, "").isdigit():
                return f"missing or invalid observation count {key} at index {i}"
        if not _RNG_COUNTS.search(line):
            return f"missing or invalid rng=sim/app/wx counts at index {i}"
        for key in ("graded", "visual", "strict", "objects", "abl"):
            if not re.fullmatch(r"[0-9a-fA-F]+", fields.get(key, "")):
                return f"missing or invalid {key} digest at index {i}"
        tick = int(fields["simframe"])
        if previous is not None and tick != previous + 1:
            return f"missing or repeated observation at index {i}: simframe {previous} -> {tick}"
        previous = tick
    return ""
GATES = {"MC2_SMOKE_FIXED_TIMESTEP": "1", "MC2_SIM_IDENTITY_TRACE": "1"}
#: A comparison over fewer frames proves nothing (the mission may not have left the loading screen).
MIN_FRAMES = 300
#: One baseline file per mission.
BASELINE_DIR = os.path.join(ROOT, "tests", "smoke", "sim_determinism")
#: The install the runs launch (`run_smoke --deploy 0.4`); its data is what a baseline pins.
INSTALL_DIR = "A:/Games/mc2-opengl/mc2-win64-v0.4"
#: What the data fingerprint covers: the archives the simulation reads whole (content) and the loose data trees (path and size; data/tgl
#: alone is 4 GB, and a changed model is a changed size or name in practice).
FINGERPRINT_ARCHIVES = ("mission.fst", "tgl.fst", "effect.fst", "misc.fst", "camera.fst")
FINGERPRINT_TREES = ("data/tgl", "data/objects", "data/effects", "data/missions", "data/warriors")
#: The loose files a baseline run is allowed on top of the stock data (the fixtures tests need), by content hash. A baseline pins the
#: stock archives and trees plus exactly this overlay, never a lane's working install; `write-overlay` regenerates it from an install.
OVERLAY_MANIFEST = os.path.join(BASELINE_DIR, "data_overlay.json")


def baseline_path(mission: str) -> str:
    directory = os.path.join(BASELINE_DIR, "linux") if sys.platform == "linux" else BASELINE_DIR
    return os.path.join(directory, mission + ".json")


def _sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def load_overlay_manifest(path: str = OVERLAY_MANIFEST) -> dict:
    """The declared overlay: {"overlay": {install-relative path: sha256}, "stock_digest": ...}. Empty when the file is missing."""
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {"overlay": {}, "stock_digest": ""}


def data_fingerprint(install: str = INSTALL_DIR, manifest: dict | None = None) -> dict:
    """-> {"digest", "inputs", "stock_digest", "overlay", "problems"}: a digest of the install data the simulation reads.

    The digest covers two parts. STOCK: the archives by content and the loose trees by relative path and size, leaving out the declared
    overlay, `*.beauty` presentation sidecars and texture cook receipts (render-only). OVERLAY: the
    files tests/smoke/sim_determinism/data_overlay.json declares, by content. `problems` names a declared overlay file that is missing or
    changed and a stock part that no longer equals the manifest's `stock_digest`, so a dirty install says why instead of only differing.
    """
    manifest = load_overlay_manifest() if manifest is None else manifest
    declared = manifest.get("overlay", {})
    stock = hashlib.sha256()
    inputs = []
    for name in FINGERPRINT_ARCHIVES:
        p = os.path.join(install, name)
        if os.path.exists(p):
            stock.update(_sha256(p).encode())
            inputs.append(name)
    for tree in FINGERPRINT_TREES:
        base = os.path.join(install, tree)
        if not os.path.isdir(base):
            continue
        entries = []
        for dp, dn, fns in os.walk(base):
            dn[:] = [d for d in dn if not d.endswith(".beauty")]
            for fn in fns:
                if tree == "data/tgl" and fn.endswith(".cookstamp"):
                    continue  # Texture provenance receipts do not affect simulation data.
                full = os.path.join(dp, fn)
                rel = os.path.relpath(full, install).replace(os.sep, "/")
                if rel not in declared:
                    entries.append(f"{rel} {os.path.getsize(full)}")
        for e in sorted(entries):
            stock.update(e.encode("utf-8", "replace"))
            stock.update(b"\n")
        inputs.append(tree + "/")
    overlay, problems = {}, []
    for rel in sorted(declared):
        full = os.path.join(install, rel)
        got = _sha256(full) if os.path.isfile(full) else "missing"
        overlay[rel] = got[:16]
        if got != declared[rel]:
            problems.append(f"overlay file {rel} is {'missing' if got == 'missing' else 'not the declared content'}")
    stock_digest = stock.hexdigest()[:16]
    if manifest.get("stock_digest") and manifest["stock_digest"] != stock_digest:
        problems.append(f"the stock part {stock_digest} is not the manifest's {manifest['stock_digest']} (loose data added or removed outside the overlay)")
    h = hashlib.sha256(stock_digest.encode())
    for rel in sorted(overlay):
        h.update(f"{rel} {overlay[rel]}\n".encode())
    return {"digest": h.hexdigest()[:16], "inputs": inputs, "stock_digest": stock_digest, "overlay": overlay, "problems": problems}


def load_simid(path: str) -> list:
    """The SIMID lines of a log, newline stripped, in order."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        return [m.group().rstrip() for ln in fh
                for m in re.finditer(re.escape(PREFIX) + r"[^\r\n\[]*", ln)]


def simulation_fields(line: str) -> str:
    """Keep authoritative fields and sim/anim RNG draws; ignore cosmetic RNG draws."""
    return _RNG_COUNTS.sub(_RNG_NONSIM, line.rstrip())


def chain(lines, frames: int | None = None) -> str:
    """Graded acceptance chain: all fields, including strict, except RNG draw counts."""
    return full_chain([simulation_fields(ln) for ln in lines], frames)


def full_chain(lines, frames: int | None = None) -> str:
    """Legacy full-line chain (also scripts/smoke_lib/logparse.py's simid_chain)."""
    h = hashlib.sha256()
    for ln in (lines if frames is None else lines[:frames]):
        h.update(ln.rstrip().encode("utf-8", "replace"))
    return h.hexdigest()[:16]


def visible_chain(lines, frames: int | None = None) -> str:
    """Legacy diagnostic chain excluding strict; never sufficient for acceptance."""
    h = hashlib.sha256()
    for ln in (lines if frames is None else lines[:frames]):
        h.update(_STRICT.sub("", ln.rstrip()).encode("utf-8", "replace"))
    return h.hexdigest()[:16]


_MOVER = re.compile(r"\[SIMID v1 mover\] simframe=(\d+) id=(\d+) .*?pos=(\S+) rot=(\S+) hp=\S+ gesture=(\S+) anim=(\S+) .*?pstep=(\S+)")


def detail_diff(path_a: str, path_b: str) -> tuple[dict, dict]:
    """Per-mover SIMID lines (MC2_SIM_IDENTITY_TRACE_DETAIL=1): -> (motion, anim). motion = pos/rot/path-step diffs, anim = gesture/anim
    diffs, each {"count", "first": (simframe, mover), "movers": n, "max_pos": metres}. A re-pin is anim-only when motion["count"] == 0."""
    def load(path):
        out = {}
        with open(path, encoding="utf-8", errors="replace") as fh:
            for ln in fh:
                m = _MOVER.search(ln)
                if m:
                    out[(int(m.group(1)), int(m.group(2)))] = m.groups()[2:]
        return out
    a, b = load(path_a), load(path_b)
    res = {"motion": {"count": 0, "first": None, "movers": set(), "max_pos": 0.0}, "anim": {"count": 0, "first": None, "movers": set(), "max_pos": 0.0}}
    for key in sorted(a.keys() & b.keys()):
        (pa, ra, ga, na, sa), (pb, rb, gb, nb, sb) = a[key], b[key]
        for kind, differs in (("motion", (pa, ra, sa) != (pb, rb, sb)), ("anim", (ga, na) != (gb, nb))):
            if differs:
                r = res[kind]
                r["count"] += 1
                r["first"] = r["first"] or key
                r["movers"].add(key[1])
                if kind == "motion" and pa != pb:
                    xa, xb = [float(v) for v in pa.split(",")], [float(v) for v in pb.split(",")]
                    r["max_pos"] = max(r["max_pos"], sum((p - q) ** 2 for p, q in zip(xa, xb)) ** 0.5)
    for r in res.values():
        r["movers"] = len(r["movers"])
    return res["motion"], res["anim"]


def compare(a, b, min_frames: int = MIN_FRAMES):
    """-> (ok, message). Only the frames both runs reached are compared."""
    common = min(len(a), len(b))
    if common < min_frames:
        return False, f"only {common} common sim frame(s) (need {min_frames}): the mission did not run long enough to prove anything"
    for lines in (a, b):
        error = observation_error(lines)
        if error:
            return False, error
    for i in range(common):
        if simulation_fields(a[i]) != simulation_fields(b[i]):
            prev = a[i - 1] if i else "(first frame)"
            return False, (f"DIVERGED at sim frame index {i}\n  run a: {a[i]}\n  run b: {b[i]}\n  last agreed: {prev}\n"
                           "  rerun with MC2_SIM_IDENTITY_TRACE_DETAIL=1 for the per-mover lines of that frame")
    return True, (f"identical over {common} sim frames (chain {chain(a, common)}; "
                  f"full a={full_chain(a, common)} b={full_chain(b, common)}; "
                  f"rng a={a[common-1].split(' rng=')[-1]} b={b[common-1].split(' rng=')[-1]}; "
                  f"runs had {len(a)} and {len(b)})")


def check_baseline(lines, baseline_path: str, seed: str = "", fingerprint: dict | None = None):
    """-> (ok, message). `seed` is this run's [DET_RNG] seed and `fingerprint` the install data digest; when the baseline recorded them
    they must agree, or the comparison is between different missions in all but name."""
    with open(baseline_path, encoding="utf-8") as fh:
        base = json.load(fh)
    if base.get("seed") and seed and base["seed"] != seed:
        return False, f"this run's mission seed {seed} is not the baseline's {base['seed']} (MC2_RNG_SEED set, or another mission name?)"
    if base.get("data_fingerprint") and fingerprint and base["data_fingerprint"]["digest"] != fingerprint["digest"]:
        return False, (f"the install data fingerprint {fingerprint['digest']} is not the baseline's {base['data_fingerprint']['digest']}: "
                       "the baseline pins other data (re-record it after a data change, with the reason in the commit)"
                       + "".join("; " + pr for pr in (fingerprint.get("problems") or [])))
    n = int(base["frames"])
    if len(lines) < n:
        return False, f"the run has {len(lines)} sim frames, the baseline needs {n}"
    error = observation_error(lines[:n])
    if error:
        return False, error
    # Old committed baselines contain only irreversible full-line hashes. Keep
    # validating them exactly; never infer a count-free digest from a hash.
    got = chain(lines, n) if base.get("chain_schema") == 2 else full_chain(lines, n)
    if got != base["chain"]:
        return False, (f"strict acceptance chain over first {n} frames differs: {got}, baseline {base['chain']} "
                       f"({base.get('mission')} at {base.get('sha')})")
    return True, f"first {n} frames match the baseline chain {got} in every digest"


def _build_sha(log: str) -> str:
    """The sha of the exe that wrote the log (its [INSTR v1] enabled: line carries build=<sha>)."""
    with open(log, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            if ln.startswith("[INSTR v1] enabled:") and " build=" in ln:
                return ln.split(" build=", 1)[1].split()[0]
    return ""


_SEED = re.compile(r"^\[DET_RNG\] mission=\S+ reseed=(0x[0-9A-Fa-f]+)")


def _seed(log: str) -> str:
    """The mission seed the run printed ([DET_RNG] reseed=0x...), "" when the deterministic RNG gate was off."""
    with open(log, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            m = _SEED.match(ln)
            if m:
                return m.group(1).upper().replace("0X", "0x")
    return ""


def _load_env(base_path: str) -> dict:
    """The extra env a baseline was recorded under (MC2_FIXTURE_ORDER_LIVENESS for mc2_16), to apply to a check run."""
    if not os.path.exists(base_path):
        return {}
    with open(base_path, encoding="utf-8") as fh:
        return dict(json.load(fh).get("env") or {})


def _git_sha() -> str:
    r = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short=12", "HEAD"], stdout=subprocess.PIPE, text=True,
                       **console_child.child_kwargs(redirected=True))
    return r.stdout.strip()


def resolve_install(name_or_path: str) -> str:
    """Registered install path, or an absolute caller-supplied install path."""
    hit = lane_model.lane_for(name_or_path)
    return hit[1]["install"] if hit else os.path.abspath(os.path.expanduser(name_or_path))


def build_and_deploy(install: str = INSTALL_DIR) -> None:
    """Build committed HEAD with Ninja and deploy to the selected install for the smoke fingerprint gate."""
    for cmd in ([sys.executable, os.path.join(ROOT, "scripts", "ninja_build.py")],
                [sys.executable, os.path.join(ROOT, "scripts", "deploy_payload.py"), resolve_install(install), "--target", "game", "--ninja"]):
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", **console_child.child_kwargs(redirected=True))
        if r.returncode:
            sys.exit("sim_determinism: " + os.path.basename(cmd[1]) + " failed:\n" + "\n".join(r.stdout.splitlines()[-6:]))


#: The longest smoke window a determinism run may ask for (owner ruling 2026-10-13); scripts/run_smoke.py refuses more than 30 s without `--sim-determinism`.
MAX_DURATION = 60


def smoke_target(install: str, ref_exe: str | None = None, ref_commit: str | None = None) -> list[str]:
    """run_smoke.py arguments naming the exe and its fingerprint reference. The reference passes through so a run of an exe built from another
    commit (an A/B across a refactor) is judged against that commit or those bytes instead of HEAD; without it run_smoke compares with HEAD."""
    install = resolve_install(install)
    lane = lane_model.lane_for(install)
    target = ["--deploy", lane[0]] if lane else ["--exe", os.path.join(install, "mc2.exe" if os.name == "nt" else "mc2")]
    if ref_exe and ref_commit:
        raise ValueError("--ref-exe and --ref-commit are exclusive (run_smoke.py accepts one fingerprint reference)")
    if ref_exe:
        target += ["--ref-exe", ref_exe]
    elif ref_commit:
        target += ["--ref-commit", ref_commit]
    return target


def add_ref_args(p) -> None:
    """--ref-exe / --ref-commit, forwarded to every run_smoke.py child (its fingerprint reference)."""
    g = p.add_mutually_exclusive_group()
    g.add_argument("--ref-exe", metavar="PATH", help="run_smoke fingerprint reference: require byte identity with this exe")
    g.add_argument("--ref-commit", metavar="SHA", help="run_smoke fingerprint reference: a clean commit (default: HEAD)")


def run_mission(mission: str, duration: str, out: str, extra_env: dict | None = None, install: str = INSTALL_DIR,
                ref_exe: str | None = None, ref_commit: str | None = None, sim_speed: int = 1, headless: bool | str = False, ticks: int = 0) -> list:
    """Launch two fresh determinism runs under host slots and retain their own logs.

    `extra_env` maps a/b to per-run variables and both to a shared fixture.
    """
    if int(duration) > MAX_DURATION:
        sys.exit(f"sim_determinism: --duration {duration} exceeds the {MAX_DURATION} s cap")
    target = smoke_target(install, ref_exe, ref_commit)
    logs = []
    for tag in ("a", "b"):
        dest = os.path.join(out, tag)
        if os.path.exists(dest):
            sys.exit(f"sim_determinism: output already exists: {dest}; use a fresh --out")
        env = dict(os.environ, **GATES, **(extra_env or {}).get("both", {}), **(extra_env or {}).get(tag, {}))
        r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "run_smoke.py"), "--mission", mission, "--duration", duration,
                            *target, "--keep-logs", "--sim-determinism", *(["--sim-speed", str(sim_speed)] if sim_speed > 1 else []),
                            *(["--ticks", str(ticks)] if ticks > 0 else []), *(["--headless"] if headless == "both" or (headless and tag == "b") else [])], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           errors="replace", **console_child.child_kwargs(redirected=True))
        arts = [line.removeprefix("[runner] artifacts: ").strip() for line in r.stdout.splitlines()
                if line.startswith("[runner] artifacts: ")]
        if r.returncode and os.environ.get("MC2_IN_GATE") == "1":
            sys.exit(f"sim_determinism: gate smoke {tag} failed (rc={r.returncode}):\n" + r.stdout)
        if r.returncode and arts and os.path.exists(os.path.join(arts[-1], mission + ".log")):
            print(f"sim_determinism: note: smoke run {tag} returned {r.returncode} (a smoke gate verdict); its SIMID stream is still compared")
        elif r.returncode or not arts:
            sys.exit(f"sim_determinism: smoke run {tag} failed (rc={r.returncode}):\n" + "\n".join(r.stdout.splitlines()[-8:]))
        shutil.copytree(arts[-1], dest)
        logs.append(os.path.join(dest, mission + ".log"))
    return logs


#: Processes whose presence beside a pair explains a timing-dependent result (builds and other game runs).
_LOAD_WATCH = ("cl.exe", "link.exe", "ninja.exe", "clang-cl.exe", "mc2.exe", "msbuild.exe")


def host_load() -> dict:
    """A one-line picture of how busy the host is right now: cpu percent and the count of build or game processes. Used to ask whether a
    timing-dependent failure follows host load."""
    import psutil
    counts = {}
    for p in psutil.process_iter(["name"]):
        n = (p.info.get("name") or "").lower()
        if n in _LOAD_WATCH:
            counts[n] = counts.get(n, 0) + 1
    return {"cpu": psutil.cpu_percent(interval=0.5), "procs": counts}


def format_pair_line(index: int, ok: bool, msg: str, before: dict, after: dict, seconds: float, starved: bool = False) -> str:
    """One ledger line per pair: verdict (PASS, FAIL, or STARVED when the runs did not reach enough frames to say), load at start and
    end, wall time. A failure's message is reduced to its first line."""
    def load(d):
        procs = ",".join(f"{k.removesuffix('.exe')}={v}" for k, v in sorted(d["procs"].items())) or "none"
        return f"cpu={d['cpu']:.0f}% procs={procs}"
    verdict = "STARVED" if starved else ("PASS" if ok else "FAIL")
    return f"pair {index}: {verdict} {seconds:.0f}s | start {load(before)} | end {load(after)} | {msg.splitlines()[0]}"


#: Consecutive starved pairs (a build or another game on the host kept both runs under MIN_FRAMES) after which the loop gives up.
MAX_STARVED_IN_A_ROW = 3


def pairs_until_fail(mission: str, duration: str, out: str, max_pairs: int, min_frames: int, extra_env: dict | None = None,
                     install: str = INSTALL_DIR, ref_exe: str | None = None, ref_commit: str | None = None, sim_speed: int = 1) -> int:
    """Run pairs until one diverges or `max_pairs` have passed. A pair whose runs did not both reach `min_frames` is STARVED: it proves
    nothing either way, is not counted, and is retried (the host was busy); MAX_STARVED_IN_A_ROW of them end the loop with exit 2. A
    failing pair's artifacts stay in `<out>/pair_<n>`; every pair appends a line to `<out>/pairs.log`. -> 0 when all passed, 1 at the
    first divergence."""
    import time
    os.makedirs(out, exist_ok=True)
    ledger = os.path.join(out, "pairs.log")
    passed, starved_run, i = 0, 0, 0
    while passed < max_pairs:
        i += 1
        before, t0 = host_load(), time.time()
        pair_dir = os.path.join(out, f"pair_{i}")
        la, lb = run_mission(mission, duration, pair_dir, extra_env, install=install, ref_exe=ref_exe, ref_commit=ref_commit, sim_speed=sim_speed)
        a, b = load_simid(la), load_simid(lb)
        starved = min(len(a), len(b)) < min_frames
        ok, msg = compare(a, b, min_frames)
        line = format_pair_line(i, ok, msg, before, host_load(), time.time() - t0, starved)
        print("sim_determinism: " + line, flush=True)
        with open(ledger, "a", encoding="utf-8") as fh:
            fh.write(line + "\n")
            if not ok and not starved:
                fh.write(msg + "\n")
        if starved:
            starved_run += 1
            shutil.rmtree(pair_dir, ignore_errors=True)
            if starved_run >= MAX_STARVED_IN_A_ROW:
                print(f"sim_determinism: {starved_run} starved pairs in a row: the host is too busy to prove anything (ledger {ledger})", flush=True)
                return 2
            continue
        starved_run = 0
        if not ok:
            print("sim_determinism: " + msg + f"\nartifacts: {pair_dir}", flush=True)
            return 1
        passed += 1
        shutil.rmtree(pair_dir, ignore_errors=True)
    print(f"sim_determinism: {max_pairs} pair(s) of {mission} identical (ledger {ledger})")
    return 0


def selftest() -> int:
    def mk(n, bad=None):
        out = []
        for i in range(n):
            g = "%016x" % (i * 7919 + (1 if i == bad else 0))
            out.append(f"{PREFIX}{i} t={i / 30:.3f} n=38 graded={g} visual={g} strict={g} nobj=2 objects={g} nabl=1 abl={g} rng=1/2/3")
        return out

    fails = []

    def expect(cond, what):
        if not cond:
            fails.append(what)

    ok, _ = compare(mk(400), mk(400))
    expect(ok, "identical logs must pass")
    ok, msg = compare(mk(400), mk(400, bad=123))
    expect(not ok and "index 123" in msg and "last agreed" in msg, "a divergence must name its first frame")
    ok, _ = compare(mk(400), mk(350))
    expect(ok, "runs of different length compare over the common prefix")
    ok, _ = compare(mk(200), mk(200))
    expect(not ok, "fewer than MIN_FRAMES common frames must fail, not pass")
    ok, _ = compare([], [])
    expect(not ok, "no SIMID lines at all must fail (the trace gate was not applied)")
    expect(chain(mk(10)) == chain(mk(12), 10), "the chain over N frames ignores later frames")
    expect(chain(mk(10)) != chain(mk(10, bad=3)), "the chain sees a changed digest")
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "x.log")
        with open(p, "w") as fh:
            fh.write("noise\n" + "\n".join(mk(5)) + "\n[HEARTBEAT] frames=1\n")
        expect(len(load_simid(p)) == 5, "load_simid keeps only the SIMID lines")
        b = os.path.join(td, "b.json")
        with open(b, "w") as fh:
            json.dump({"frames": 5, "chain": full_chain(mk(5)), "visible_chain": visible_chain(mk(5)), "mission": "m", "sha": "s"}, fh)
        expect(check_baseline(mk(9), b)[0], "a run that starts with the baseline frames matches it")
        expect(not check_baseline(mk(9, bad=2), b)[0], "a run that differs inside the baseline frames does not")
        expect(not check_baseline(mk(4), b)[0], "a run shorter than the baseline does not")
        strict_moved = [_STRICT.sub(" strict=ffff", ln) if i == 2 else ln for i, ln in enumerate(mk(9))]
        ok, msg = check_baseline(strict_moved, b)
        expect(not ok and "strict" in msg, "a change in the strict digest alone fails")
        expect(not compare([ln.replace(" n=38", "") for ln in mk(400)], mk(400))[0], "missing observation count fails")
        with open(b, "w") as fh:
            json.dump({"frames": 5, "chain": full_chain(mk(5)), "visible_chain": visible_chain(mk(5)), "mission": "m", "sha": "s", "seed": "0x1",
                       "data_fingerprint": {"digest": "d1", "inputs": []}}, fh)
        expect(check_baseline(mk(9), b, "0x1", {"digest": "d1"})[0], "matching seed and data fingerprint pass")
        expect(not check_baseline(mk(9), b, "0x2", {"digest": "d1"})[0], "another seed fails before any frame is compared")
        expect(not check_baseline(mk(9), b, "0x1", {"digest": "d2"})[0], "other install data fails before any frame is compared")
        expect(check_baseline(mk(9), b, "", None)[0], "a run without seed or fingerprint still compares the frames")
        with open(p, "a") as fh:
            fh.write("[DET_RNG] mission=m reseed=0x00AB12CD (MC2_DETERMINISTIC_RNG=1)\n")
        expect(_seed(p) == "0x00AB12CD", "the seed is read from the [DET_RNG] line")
        inst = os.path.join(td, "inst")
        os.makedirs(os.path.join(inst, "data", "tgl"))
        with open(os.path.join(inst, "mission.fst"), "wb") as fh:
            fh.write(b"abc")
        with open(os.path.join(inst, "data", "tgl", "x.ini"), "wb") as fh:
            fh.write(b"1234")
        f1 = data_fingerprint(inst, {})
        with open(os.path.join(inst, "data", "tgl", "x.tga.cookstamp"), "wb") as fh:
            fh.write(b"123456789abcdef0\n")
        expect(data_fingerprint(inst, {}) == f1, "texture provenance receipts do not move the data fingerprint")
        with open(os.path.join(inst, "data", "tgl", "x.ini"), "wb") as fh:
            fh.write(b"12345")
        expect(f1["digest"] != data_fingerprint(inst, {})["digest"] and f1["inputs"] == ["mission.fst", "data/tgl/"],
               "the data fingerprint sees a changed loose file size and names its inputs")
        os.makedirs(os.path.join(inst, "data", "missions", "m.beauty"))
        with open(os.path.join(inst, "data", "missions", "m.beauty", "h.r32"), "wb") as fh:
            fh.write(b"x")
        with open(os.path.join(inst, "data", "missions", "fx.fit"), "wb") as fh:
            fh.write(b"fixture")
        man = {"overlay": {"data/missions/fx.fit": _sha256(os.path.join(inst, "data", "missions", "fx.fit"))}, "stock_digest": ""}
        f2 = data_fingerprint(inst, man)
        man["stock_digest"] = f2["stock_digest"]
        with open(os.path.join(inst, "data", "missions", "m.beauty", "h.r32"), "wb") as fh:
            fh.write(b"yy")
        expect(data_fingerprint(inst, man) == f2 and not f2["problems"], "a presentation sidecar does not move the fingerprint")
        with open(os.path.join(inst, "data", "missions", "fx.fit"), "wb") as fh:
            fh.write(b"changed")
        f3 = data_fingerprint(inst, man)
        expect(f3["digest"] != f2["digest"] and f3["problems"] and "fx.fit" in f3["problems"][0], "a changed overlay file moves the digest and is named")
        os.remove(os.path.join(inst, "data", "missions", "fx.fit"))
        with open(os.path.join(inst, "data", "missions", "other.fit"), "wb") as fh:
            fh.write(b"stray")
        f4 = data_fingerprint(inst, man)
        expect(any("missing" in x for x in f4["problems"]) and any("stock part" in x for x in f4["problems"]),
               "a missing overlay file and a stray loose file are both reported")
    from unittest.mock import patch

    class _Stop(Exception):
        pass

    def argv_for(speed):
        seen = []

        def fake(cmd, **_kw):
            seen.append(cmd)
            raise _Stop

        with tempfile.TemporaryDirectory() as out, patch(__name__ + ".subprocess.run", side_effect=fake):
            try:
                run_mission("m", "10", out, sim_speed=speed)
            except _Stop:
                pass
        return seen[0] if seen else []
    a4 = argv_for(4)
    expect(a4[-2:] == ["--sim-speed", "4"] and "--sim-speed" not in argv_for(1), "--sim-speed reaches run_smoke, and 1x adds no argument")
    quiet = {"cpu": 3.0, "procs": {}}
    busy = {"cpu": 71.4, "procs": {"cl.exe": 4, "mc2.exe": 1}}
    line = format_pair_line(2, False, "DIVERGED at sim frame index 9\n  run a: x", quiet, busy, 61.2)
    expect(line.startswith("pair 2: FAIL 61s") and "procs=none" in line and "procs=cl=4,mc2=1" in line and "cpu=71%" in line and "\n" not in line,
           "a pair line carries verdict, load at start and end, and only the first message line")
    expect(format_pair_line(1, True, "identical over 5 sim frames", quiet, quiet, 1).startswith("pair 1: PASS"), "a passing pair line says PASS")
    expect(format_pair_line(3, False, "only 20 common sim frame(s)", quiet, busy, 9, starved=True).startswith("pair 3: STARVED"),
           "a pair that reached too few frames says STARVED, not FAIL")
    for f in fails:
        print("sim_determinism selftest FAIL: " + f)
    print("sim_determinism selftest: " + ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compare")
    c.add_argument("log_a")
    c.add_argument("log_b")
    c.add_argument("--min-frames", type=int, default=MIN_FRAMES)
    dd = sub.add_parser("detail-diff", help="two DETAIL logs: pos/rot/path-step diff next to the gesture/anim diff")
    dd.add_argument("log_a")
    dd.add_argument("log_b")
    def env_args(p):
        p.add_argument("--env", action="append", default=[], metavar="NAME=VALUE", help="set for both runs (a mission fixture; repeatable)")
        p.add_argument("--env-a", action="append", default=[], metavar="NAME=VALUE", help="set only for run a (repeatable)")
        p.add_argument("--env-b", action="append", default=[], metavar="NAME=VALUE", help="set only for run b (repeatable)")
        p.add_argument("--sim-speed", type=int, choices=(1, 2, 3, 4), default=1, metavar="N",
                       help="N fixed sim steps per drawn frame (run_smoke --sim-speed); the SIMID chain is that of 1x, reached in ~1/N host time")

    def install_arg(p):
        p.add_argument("--install", type=resolve_install, default=INSTALL_DIR, metavar="NAME_OR_PATH",
                       help="install for runs and data fingerprint (default: 0.4)")

    r = sub.add_parser("run")
    r.add_argument("mission")
    r.add_argument("--duration", default="20")
    r.add_argument("--min-frames", type=int, default=MIN_FRAMES)
    r.add_argument("--baseline", nargs="?", const="", help="also compare with the committed baseline (default: tests/smoke/sim_determinism/<mission>.json)")
    r.add_argument("--deploy", action="store_true", help="build and deploy HEAD to the selected install first")
    r.add_argument("--out", help="keep both runs' artifacts here (default: a temporary directory)")
    env_args(r)
    install_arg(r)
    add_ref_args(r)
    k = sub.add_parser("check", help="run twice and compare with the committed baseline (its recorded env is applied)")
    k.add_argument("mission")
    k.add_argument("--duration", default="20")
    k.add_argument("--min-frames", type=int, default=MIN_FRAMES)
    k.add_argument("--deploy", action="store_true")
    k.add_argument("--out")
    env_args(k)
    install_arg(k)
    add_ref_args(k)
    u = sub.add_parser("pairs-until-fail", help="run pairs until one diverges (or --max pass); records host load per pair")
    u.add_argument("--mission", required=True)
    u.add_argument("--max", type=int, required=True, help="stop after this many passing pairs")
    u.add_argument("--duration", default="30")
    u.add_argument("--min-frames", type=int, default=MIN_FRAMES)
    u.add_argument("--out", help="ledger and failing-pair artifacts go here (default: a temporary directory)")
    env_args(u)
    install_arg(u)
    add_ref_args(u)
    for hp in (r, k):
        hp.add_argument("--headless", action="store_true", help="run b headless (no window, GPU or audio): a windowed-vs-headless equality check; needs --ticks")
        hp.add_argument("--headless-both", action="store_true", help="run a AND b headless (the merge gate's determinism rows: no GPU, no host slot); ticks default to the baseline's frames")
        hp.add_argument("--ticks", type=int, default=0, metavar="N", help="both runs quit after N sim ticks (run_smoke --ticks) instead of at the wall-clock end")
    w = sub.add_parser("write-baseline")
    w.add_argument("log")
    w.add_argument("--frames", type=int, required=True)
    w.add_argument("--mission", required=True)
    w.add_argument("--out", help="default: tests/smoke/sim_determinism/<mission>.json")
    w.add_argument("--env", action="append", default=[], metavar="NAME=VALUE", help="the fixture env the run had, recorded for `check`")
    install_arg(w)
    o = sub.add_parser("write-overlay", help="declare the overlay: the loose .fit/.pak files directly in data/missions of --install, with the stock digest")
    install_arg(o)
    sub.add_parser("selftest")
    args = ap.parse_args(argv)
    if getattr(args, "headless_both", False):
        args.headless = "both"
        if args.ticks <= 0 and getattr(args, "cmd", "") == "check":
            args.ticks = int(json.load(open(baseline_path(args.mission), encoding="utf-8"))["frames"])
    if getattr(args, "headless", False) and getattr(args, "ticks", 0) <= 0:
        sys.exit("sim_determinism: --headless needs --ticks N (a headless run has no wall-clock end)")
    if args.cmd == "selftest":
        return selftest()
    if args.cmd == "detail-diff":
        motion, anim = detail_diff(args.log_a, args.log_b)
        print(f"pos/rot/path-step: {motion}")
        print(f"gesture/anim:      {anim}")
        return 0 if motion["count"] == 0 else 1
    if args.cmd == "write-overlay":
        base = os.path.join(args.install, "data", "missions")
        declared = {f"data/missions/{fn}": _sha256(os.path.join(base, fn)) for fn in sorted(os.listdir(base))
                    if fn.endswith((".fit", ".pak")) and os.path.isfile(os.path.join(base, fn))}
        fp = data_fingerprint(args.install, {"overlay": declared, "stock_digest": ""})
        with open(OVERLAY_MANIFEST, "w", encoding="utf-8", newline="\n") as fh:
            json.dump({"about": "loose fixture files a baseline run may carry on top of the stock data, by sha256; *.beauty sidecars are not fingerprinted; "
                                "tools/sim_determinism.py write-overlay regenerates this", "stock_digest": fp["stock_digest"], "overlay": declared}, fh, indent=1)
            fh.write("\n")
        print(f"sim_determinism: wrote {os.path.relpath(OVERLAY_MANIFEST, ROOT)}: {len(declared)} overlay files, stock {fp['stock_digest']}")
        return 0
    if args.cmd == "write-baseline":
        lines = load_simid(args.log)
        if len(lines) < args.frames:
            sys.exit(f"the log has {len(lines)} sim frames, fewer than --frames {args.frames}")
        fp = data_fingerprint(args.install)
        if fp["problems"]:
            sys.exit("the install is not the declared stock-plus-overlay data set; no baseline written:\n  " + "\n  ".join(fp["problems"]))
        out = args.out or baseline_path(args.mission)
        error = observation_error(lines[:args.frames])
        if error:
            sys.exit(error)   # before anything is opened: a rejected log must leave an existing baseline untouched
        os.makedirs(os.path.dirname(out), exist_ok=True)
        tmp = out + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
            json.dump({"mission": args.mission, "frames": args.frames, "chain_schema": 2,
                       "chain": chain(lines, args.frames), "full": full_chain(lines, args.frames), "visible_chain": visible_chain(lines, args.frames),
                       "sha": _build_sha(args.log) or _git_sha(), "seed": _seed(args.log), "data_fingerprint": fp,
                       "gates": GATES, "env": dict(kv.split("=", 1) for kv in args.env),
                       "about": "chain digest of the first N [SIMID v1] lines; tools/sim_determinism.py check <mission>"}, fh, indent=1)
            fh.write("\n")
        os.replace(tmp, out)
        print(f"sim_determinism: wrote {os.path.relpath(out, ROOT)}")
        return 0
    if args.cmd == "compare":
        ok, msg = compare(load_simid(args.log_a), load_simid(args.log_b), args.min_frames)
        print(("sim_determinism: PASS " if ok else "sim_determinism: FAIL ") + msg)
        return 0 if ok else 1
    out = args.out or tempfile.mkdtemp(prefix="simdet_")
    base = None
    if args.cmd == "check":
        base = baseline_path(args.mission)
    elif getattr(args, "baseline", None) is not None:
        base = args.baseline or baseline_path(args.mission)
    both = dict(_load_env(base) if base else {}, **dict(kv.split("=", 1) for kv in args.env))
    extra = {"both": both, "a": dict(kv.split("=", 1) for kv in args.env_a), "b": dict(kv.split("=", 1) for kv in args.env_b)}
    if args.cmd == "pairs-until-fail":
        return pairs_until_fail(args.mission, args.duration, out, args.max, args.min_frames, extra, install=args.install,
                                ref_exe=args.ref_exe, ref_commit=args.ref_commit, sim_speed=args.sim_speed)
    if args.deploy:
        build_and_deploy(args.install)
    la, lb = run_mission(args.mission, args.duration, out, extra, install=args.install, ref_exe=args.ref_exe, ref_commit=args.ref_commit,
                         sim_speed=args.sim_speed, headless=args.headless, ticks=args.ticks)
    a, b = load_simid(la), load_simid(lb)
    ok, msg = compare(a, b, args.min_frames)
    print(("sim_determinism: PASS " if ok else "sim_determinism: FAIL ") + msg)
    if ok and base:
        if not os.path.exists(base):
            print(f"sim_determinism: baseline FAIL no baseline at {os.path.relpath(base, ROOT)} (record one with write-baseline)")
            return 1
        ok2, msg2 = check_baseline(a, base, _seed(la), data_fingerprint(args.install))
        print(("sim_determinism: baseline PASS " if ok2 else "sim_determinism: baseline FAIL ") + msg2)
        ok = ok and ok2
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
