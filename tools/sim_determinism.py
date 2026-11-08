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


def observation_error(lines, gaps: bool = False) -> str:
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
        if previous is not None and (tick <= previous if gaps else tick != previous + 1):
            return f"missing or repeated observation at index {i}: simframe {previous} -> {tick}"
        previous = tick
    return ""
GATES = {"MC2_SMOKE_FIXED_TIMESTEP": "1", "MC2_SIM_IDENTITY_TRACE": "1"}
#: Missions whose determinism runs also arm the G7b overlap audit (aborts on the first violation; read-only otherwise, so the SIMID
#: chain is unchanged).
OVERLAP_AUDIT_MISSIONS = ("mc2_01", "mc2_03", "mc2_10", "mc2_16", "mc2_24")
OVERLAP_VIOLATION = "[SIM_OVERLAP v1] VIOLATION"


def overlap_violation(text: str) -> str:
    """The first `[SIM_OVERLAP v1] VIOLATION` line in a run's output, or ""."""
    for line in text.splitlines():
        if OVERLAP_VIOLATION in line:
            return line.strip()
    return ""
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


class SimidLines(list):
    """The SIMID lines of a log; .dropped counts the garbled ones left out."""
    dropped = 0


_ANIMRNG = re.compile(r"\banimrng=\d+(?=\s|$)")


def load_simid(path: str) -> list:
    """The SIMID lines of a log, newline stripped, in order. The engine writes each line in one call, but another thread's stderr
    record (a [VK_VALIDATION v1] message) can still land inside it. A line whose animrng token is not a number followed by
    end-of-line or a space is dropped as garbled and counted (.dropped), never compared; text after a good token is cut."""
    out = SimidLines()
    with open(path, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            for m in re.finditer(re.escape(PREFIX) + r"[^\r\n\[]*", ln):
                t = m.group().rstrip()
                a = _ANIMRNG.search(t)
                if a:
                    out.append(t[:a.end()])
                else:
                    out.dropped += 1
    return out


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


_TICK = re.compile(r"simframe=(\d+)")


def _tick(line: str) -> int:
    m = _TICK.search(line)
    return int(m.group(1)) if m else -1


def _alignment_problem(a, b, dropped) -> str:
    """Why two SIMID sequences with garbled (dropped) lines cannot be aligned on ticks, or "". Both ORIGINAL sequences must be
    well-formed (no duplicate or conflicting tick, valid fields, increasing ticks), and each hole inside a run must be explained by
    one of its garbled lines AND the tick must be present in the other run (otherwise it is a real gap, not a skipped garble)."""
    for name, lines, drops, other in (("a", a, dropped[0], b), ("b", b, dropped[1], a)):
        seen = {}
        for x in lines:
            t = _tick(x)
            if t in seen:
                return f"run {name} has {'a conflicting' if seen[t] != x else 'a repeated'} observation for simframe {t}"
            seen[t] = x
        error = observation_error(lines, gaps=True)
        if error:
            return f"run {name}: {error}"
        if lines:
            other_ticks = {_tick(x) for x in other}
            holes = [t for t in range(_tick(lines[0]), _tick(lines[-1]) + 1) if t not in seen]
            if len(holes) > drops:
                return f"run {name} is missing {len(holes)} ticks inside its range (first {holes[0]}) but only {drops} garbled line(s) explain a hole"
            lost = [t for t in holes if t not in other_ticks]
            if lost:
                return f"run {name} lacks simframe {lost[0]} and the other run does not have it either: a gap, not a skipped garbled line"
    return ""


def _inconclusive(msg: str) -> bool:
    return msg.startswith("INCONCLUSIVE")


def _verdict(ok: bool, msg: str) -> str:
    return "sim_determinism: PASS " if ok else ("sim_determinism: " if _inconclusive(msg) else "sim_determinism: FAIL ")


def compare(a, b, min_frames: int = MIN_FRAMES, required_length: int = 0):
    """-> (ok, message). Only the frames both runs reached are compared; required_length additionally rejects a truncated run."""
    if required_length and (len(a) < required_length or len(b) < required_length):
        return False, (f"INCONCLUSIVE: runs reached {len(a)} and {len(b)} sim frames, need {required_length}: a run stopped before the baseline")
    dropped = (getattr(a, "dropped", 0), getattr(b, "dropped", 0))
    if any(dropped):  # a garbled line leaves a hole: align on ticks, but only after both ORIGINAL sequences validate
        problem = _alignment_problem(a, b, dropped)
        if problem:
            return False, "INCONCLUSIVE: " + problem
        da, db = {_tick(x): x for x in a}, {_tick(x): x for x in b}
        keys = [k for k in da if k in db]
        if required_length and len(keys) < required_length:
            return False, (f"INCONCLUSIVE: only {len(keys)} common sim ticks after dropping garbled lines "
                           f"(a={dropped[0]} b={dropped[1]}), need {required_length}")
        a, b = [da[k] for k in keys], [db[k] for k in keys]
    common = min(len(a), len(b))
    if common < min_frames:
        return False, f"only {common} common sim frame(s) (need {min_frames}): the mission did not run long enough to prove anything"
    for lines in (a, b):
        error = observation_error(lines, gaps=any(dropped))  # dropped garbled frames leave known holes
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
                  f"runs had {len(a)} and {len(b)}; garbled frames dropped a={dropped[0]} b={dropped[1]})")


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


def check_baseline_both(a, b, baseline_path: str, seed_a: str = "", seed_b: str = "", fingerprint: dict | None = None):
    """-> (ok, message). BOTH original streams must satisfy the pin: matching run a to the baseline says nothing about run b."""
    for name, lines, seed in (("a", a, seed_a), ("b", b, seed_b)):
        ok, msg = check_baseline(lines, baseline_path, seed, fingerprint)
        if not ok:
            return False, f"run {name}: {msg}"
    return True, msg


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
                ref_exe: str | None = None, ref_commit: str | None = None, sim_speed: int = 1, headless: bool | str = False, ticks: int = 0,
                runs: int = 2) -> list:
    """Launch two (runs=1: one, for `check --single`) fresh determinism runs under host slots and retain their own logs.

    `extra_env` maps a/b to per-run variables and both to a shared fixture.
    """
    if int(duration) > MAX_DURATION:
        sys.exit(f"sim_determinism: --duration {duration} exceeds the {MAX_DURATION} s cap")
    battle_fixture = mission == "battle"
    if battle_fixture:
        mission = "mc2_24"  # battle is the CPU profiler's 128-clone/two-vehicle fixture, not a map.
    target = smoke_target(install, ref_exe, ref_commit)
    logs = []
    for tag in ("a", "b")[:runs]:
        dest = os.path.join(out, tag)
        if os.path.exists(dest):
            sys.exit(f"sim_determinism: output already exists: {dest}; use a fresh --out")
        env = dict(os.environ, **GATES, **({"MC2_SIM_OVERLAP_AUDIT": "1"} if mission in OVERLAP_AUDIT_MISSIONS else {}),
                   **(extra_env or {}).get("both", {}), **(extra_env or {}).get(tag, {}))
        if battle_fixture:
            # The UI channel consumes its input. Each run needs a private copy;
            # never let the engine truncate the committed baseline fixture.
            fixture = "tests/smoke/sim_determinism/battle-128.commands.txt"
            if env.get("MC2_UI_COMMAND_FILE") != fixture or env.get("MC2_TEST_SPAWN") != "1":
                sys.exit("battle baseline requires the recorded 128-mech fixture env")
            os.makedirs(out, exist_ok=True)
            channel = os.path.abspath(os.path.join(out, tag + ".commands.txt"))
            if os.path.exists(channel + ".ack"):
                sys.exit("battle fixture has stale acknowledgements; use a fresh --out")
            shutil.copyfile(os.path.join(ROOT, fixture), channel)
            env["MC2_UI_COMMAND_FILE"] = channel
        r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "run_smoke.py"), "--mission", mission, "--duration", duration,
                            *target, "--keep-logs", "--sim-determinism", *(["--sim-speed", str(sim_speed)] if sim_speed > 1 else []),
                            *(["--ticks", str(ticks)] if ticks > 0 else []), *(["--headless"] if headless == "both" or (headless and tag == "b") else [])], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           errors="replace", **console_child.child_kwargs(redirected=True))
        headless_run = headless == "both" or (headless and tag == "b")
        arts = [line.removeprefix("[runner] artifacts: ").strip() for line in r.stdout.splitlines()
                if line.startswith("[runner] artifacts: ")]
        if not arts:
            sys.exit(f"sim_determinism: smoke run {tag} produced no artifacts (rc={r.returncode}):\n" + "\n".join(r.stdout.splitlines()[-8:]))
        log_path = os.path.join(arts[-1], mission + ".log")
        log_text = ""
        if os.path.exists(log_path):
            with open(log_path, encoding="utf-8", errors="replace") as fh:
                log_text = fh.read()
        for warn_line in r.stdout.splitlines():
            if "[PERF_COUNTER_WARN]" in warn_line:
                print(warn_line.strip(), flush=True)   # perf_counter-only: shown in the gate row status, never a failure
        text = r.stdout + "\n" + log_text
        violation = overlap_violation(text)
        if violation:
            sys.exit(f"sim_determinism: smoke run {tag} hit an overlap audit violation (RED):\n{violation}")
        if headless_run:
            if not headless_exit_ok(r.returncode, text):
                sys.exit(f"sim_determinism: headless smoke run {tag} did not reach its tick budget cleanly (rc={r.returncode}):\n"
                         + "\n".join(r.stdout.splitlines()[-8:]))
        elif r.returncode and not windowed_exit_ok(r.returncode, text):
            # A windowed run must pass its smoke everywhere (gate or standalone): matching SIMIDs cannot launder a fingerprint,
            # shader, crash or content failure. The one narrow exception is the perf_counter-only verdict (see windowed_exit_ok).
            sys.exit(f"sim_determinism: smoke run {tag} failed (rc={r.returncode}):\n" + "\n".join(r.stdout.splitlines()[-8:]))
        if battle_fixture:
            ack_path = channel + ".ack"
            ack = ""
            if os.path.isfile(ack_path):
                with open(ack_path, encoding="utf-8", errors="replace") as ack_file:
                    ack = ack_file.read()
            for kind, count in (("clone", 128), ("vehicle", 2)):
                spawned = re.findall(r"spawn " + kind + r" .*? -> (\w+) \(([^\n]+)\)", ack)
                if len(spawned) != count or any(status != "ok" or "WARNING" in detail for status, detail in spawned):
                    sys.exit(f"battle fixture failed: expected {count} healthy {kind} acknowledgements")
            if "bench_attack -> ok" not in ack:
                sys.exit("battle fixture failed: attack command was not acknowledged")
        shutil.copytree(arts[-1], dest)
        logs.append(os.path.join(dest, mission + ".log"))
    return logs


#: Verdict buckets a headless run fails by design (no frames, GPU or presence lines). Anything else stays red.
HEADLESS_RENDER_BUCKETS = ("heartbeat_freeze_play", "heartbeat_freeze_load", "perf_counters_missing", "perf_counter",
                           "video_floor_missing", "vegetation_floor_missing", "vegetation_visible_missing",
                           "cull_parity_missing", "light_contract_unchecked")
_SMOKE_FAIL_ROW = re.compile(r"^\|\s*(\S+)\s*\|\s*FAIL\s*\|\s*([^|]*)\|", re.M)


def headless_exit_ok(returncode: int, text: str) -> bool:
    """A headless run is clean only when it reached its tick budget without a crash (mirrors saveload_roundtrip).

    rc=1 is accepted only as the runner's render-gate verdict: the engine summary passed, no hard failure was printed
    (fingerprint, resolution, floors), the run is valid, and every FAIL row lists only HEADLESS_RENDER_BUCKETS."""
    if "[SMOKE v1] event=tick_budget" not in text or "Crash Report" in text:
        return False
    if returncode == 0:
        return True
    if returncode != 1 or "[SMOKE v1] event=summary result=pass" not in text:
        return False
    # The order-liveness fixture needs a play phase with an autowin window; a headless run has none, so its "summary line absent"
    # verdict is by design here (any other order-liveness failure keeps its HARD FAIL line and stays red).
    live_fails = re.findall(r"ORDER-LIVENESS FAIL .*", text)
    if live_fails and all("ORDER_LIVENESS_SUMMARY line absent" in line for line in live_fails):
        text = re.sub(r"^.*\[ORDER-LIVENESS\] HARD FAIL.*$", "", text, flags=re.M)
    if "HARD FAIL" in text or "[DEPLOY_FINGERPRINT] MISMATCH" in text or "DEPLOY_FINGERPRINT:** FAIL" in text:
        return False
    if "RUN INVALID" in text:
        # A headless run creates no Vulkan device, so the runner's "asked for a GPU but the log has no [VK_PROFILE] line" check
        # always fires on it; that one reason is by design here. Any other invalid reason stays red.
        reasons = re.findall(r"^> - (.+)$", text, re.M)
        if not reasons or any("has no [VK_PROFILE] device line" not in r for r in reasons):
            return False
    rows = _SMOKE_FAIL_ROW.findall(text)
    if not rows:
        return False  # rc=1 without a failed mission row: some other runner gate failed
    for _stem, buckets in rows:
        names = [b.strip() for b in buckets.split(",") if b.strip()]
        if not names or any(not b.startswith(HEADLESS_RENDER_BUCKETS) for b in names):
            return False
    return True


#: The only render-gate verdict a windowed determinism run may fail by (owner rule: perf_counter-only verdicts never fail a row).
WINDOWED_TOLERATED_BUCKETS = ("perf_counter",)


def windowed_exit_ok(returncode: int, text: str) -> bool:
    """rc=0 is clean; rc=1 is tolerated only when the engine summary passed, no hard failure was printed (fingerprint, shader,
    invalid run) and every FAIL row lists only WINDOWED_TOLERATED_BUCKETS. Any other nonzero exit is red."""
    if returncode == 0:
        return True
    if returncode != 1 or "[SMOKE v1] event=summary result=pass" not in text or "Crash Report" in text:
        return False
    if "HARD FAIL" in text or "RUN INVALID" in text or "[DEPLOY_FINGERPRINT] MISMATCH" in text or "DEPLOY_FINGERPRINT:** FAIL" in text:
        return False
    rows = _SMOKE_FAIL_ROW.findall(text)
    if not rows:
        return False
    return all((names := [b.strip() for b in buckets.split(",") if b.strip()]) and all(b.startswith(WINDOWED_TOLERATED_BUCKETS) for b in names)
               for _stem, buckets in rows)


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
    ok, msg = compare(mk(400), mk(300), 300, required_length=400)
    expect(not ok and "need 400" in msg, "a run b truncated before the --headless-both baseline must fail")
    ok, _ = compare(mk(200), mk(200))
    expect(not ok, "fewer than MIN_FRAMES common frames must fail, not pass")
    ok, _ = compare([], [])
    expect(not ok, "no SIMID lines at all must fail (the trace gate was not applied)")
    expect(chain(mk(10)) == chain(mk(12), 10), "the chain over N frames ignores later frames")
    expect(chain(mk(10)) != chain(mk(10, bad=3)), "the chain sees a changed digest")
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "x.log")
        with open(p, "w") as fh:
            fh.write("noise\n" + "\n".join(ln + " animrng=0" for ln in mk(5)) + "\n[HEARTBEAT] frames=1\n")
        expect(len(load_simid(p)) == 5, "load_simid keeps only the SIMID lines")
        with open(p, "w") as fh:
            fh.write(mk(1)[0] + " animrng=27 to VK_FORMAT_R8\n" + mk(1)[0] + " animrng=2700061d\n" + mk(1)[0] + "\n")
        got = load_simid(p)
        expect(len(got) == 1 and got[0].endswith("animrng=27") and got.dropped == 2, "a garbled animrng token is dropped and counted, a trailing splice is cut")
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


def check_single(args, out: str, extra: dict, base: str, needed: int) -> int:
    """`check --single`: one headless run must cover every pinned tick and match the committed chain (seed and data
    fingerprint included). INCONCLUSIVE (3) when it falls short; a missing pin is a FAIL, as in the pair."""
    (la,) = run_mission(args.mission, args.duration, out, extra, install=args.install, ref_exe=args.ref_exe,
                        ref_commit=args.ref_commit, sim_speed=args.sim_speed, headless=args.headless, ticks=args.ticks, runs=1)
    a = load_simid(la)
    needed = max(needed, args.ticks, args.min_frames)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "result.json"), "w", encoding="utf-8") as fh:
        json.dump({"mode": "single", "requested_ticks": args.ticks, "tick_tolerance": 0, "required_frames": needed,
                   "frames_a": len(a), "complete": len(a) >= needed}, fh, indent=2)
    if len(a) < needed:
        print(f"sim_determinism: INCONCLUSIVE (too few frames): {len(a)} of {needed} sim frames in the single run")
        return 3
    if not os.path.exists(base):
        print(f"sim_determinism: baseline FAIL no baseline at {os.path.relpath(base, ROOT)} (record one with write-baseline)")
        return 1
    ok, msg = check_baseline(a, base, _seed(la), data_fingerprint(args.install))
    print(("sim_determinism: baseline PASS (single run) " if ok else "sim_determinism: baseline FAIL (single run) ") + msg)
    return 0 if ok else 1


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
    k.add_argument("--single", action="store_true", help="one headless run checked against the pin only (nightly 03/16, p31); needs "
                   "--headless-both. No a/b pair: run-to-run drift is the weekly and sim-change pair's job")
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
        if getattr(args, "cmd", "") == "check":
            frames = int(json.load(open(baseline_path(args.mission), encoding="utf-8"))["frames"])
            if args.ticks <= 0:
                args.ticks = frames
            args.min_frames = max(args.min_frames, frames)
    if getattr(args, "single", False) and not args.headless_both:
        sys.exit("sim_determinism: --single needs --headless-both (the pin's full tick budget, no windowed wall-clock end)")
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
        brain_files = [rel for rel in declared if rel.endswith("_ai.fit") or "_specials" in rel]
        if brain_files:
            print("sim_determinism: brain fixtures belong in temporary mod overlays, not the base install; "
                  "refusing to pin: " + ", ".join(brain_files))
            return 1
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
        print(_verdict(ok, msg) + msg)
        return 0 if ok else (3 if _inconclusive(msg) else 1)
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
    try:
        needed = int(json.load(open(base, encoding="utf-8"))["frames"]) if base and os.path.exists(base) else 0
    except (OSError, ValueError, KeyError, TypeError):
        needed = 0  # unreadable baseline: check_baseline reports it
    if getattr(args, "single", False):
        return check_single(args, out, extra, base, needed)
    la, lb = run_mission(args.mission, args.duration, out, extra, install=args.install,
                        ref_exe=args.ref_exe, ref_commit=args.ref_commit, sim_speed=args.sim_speed,
                        headless=args.headless, ticks=args.ticks)
    a, b = load_simid(la), load_simid(lb)
    reached = min(len(a), len(b))
    # --ticks emits one SIMID per tick: tolerance is zero, including windowed runs.
    required = max(args.ticks, args.min_frames if getattr(args, "headless_both", False) else 0)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "result.json"), "w", encoding="utf-8") as fh:
        json.dump({"requested_ticks": args.ticks, "tick_tolerance": 0,
                   "required_frames": max(needed, required), "frames_a": len(a), "frames_b": len(b),
                   "complete": reached >= max(needed, required)}, fh, indent=2)
    needed = max(needed, required)
    if reached < needed:
        print(f"sim_determinism: INCONCLUSIVE (too few frames): {reached} of {needed} sim frames within the requested {args.duration} s budget")
        return 3
    # The baseline pins `needed` ticks: both runs must cover ALL of them after alignment (a garbled interior tick is a missing
    # pinned observation, INCONCLUSIVE), not merely the frames they happen to share.
    ok, msg = compare(a, b, args.min_frames, required_length=needed)
    print(_verdict(ok, msg) + msg)
    if not ok and _inconclusive(msg):
        return 3  # incomplete evidence is neither PASS nor a divergence; the gate row still fails on a nonzero rc
    if ok and base:
        if not os.path.exists(base):
            print(f"sim_determinism: baseline FAIL no baseline at {os.path.relpath(base, ROOT)} (record one with write-baseline)")
            return 1
        ok2, msg2 = check_baseline_both(a, b, base, _seed(la), _seed(lb), data_fingerprint(args.install))
        print(("sim_determinism: baseline PASS " if ok2 else "sim_determinism: baseline FAIL ") + msg2)
        ok = ok and ok2
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
