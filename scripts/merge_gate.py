#!/usr/bin/env python3
"""Orchestrator merge-window gate; --dry-run prints the plan without any launches.

Run after lane_merge verification. Deploy consumes that build64-ninja tree;
this gate never builds the engine a second time. Real runs require mc2-main
on claude/nifty-mendeleev, or an explicit lane-owned install for rehearsal.
Use --base for change selection, --full for the daily full sweep.

CALLERS MUST WAIT AT LEAST 10800 s (3 h) for a full gate (W113 ran 90 min); a shorter caller timeout kills it mid-stage. A first
SIGINT/SIGTERM/SIGBREAK is held until the running stage ends (finished stages stay recorded for reuse), then the gate stops.
"""
from __future__ import annotations

import argparse
import json
from contextlib import contextmanager, nullcontext, redirect_stdout, redirect_stderr
import os
from pathlib import Path
import subprocess
import sys
import time
import shutil
import tempfile
import hashlib
import threading
import re
import math
from concurrent.futures import Future, ThreadPoolExecutor, TimeoutError as FutureTimeout

import console_child

ROOT = Path(__file__).resolve().parents[1]
#: The one determinism row that keeps a window and a GPU (a guard for the windowed path); the others run headless.
WINDOWED_DETERMINISM = "mc2_10"
#: Rows the box gate (tools/box_run.py --gate: GPU 3, clang/libc++ build, Linux baselines) proves instead of the PC. The PC keeps
#: determinism-mc2_10 and saveload-mc2_10 as the MSVC canary; the daily --full gate keeps every row on the PC. A red box row is a red gate row.
BOX_GATE_TIMEOUT_S = 2400  # the box stage's wall cap as seen from the PC; past it the stage is BOX_INFRA and the PC rows run
BOX_OFFLOADED = ("determinism-mc2_03", "determinism-mc2_16", "determinism-mc2_24", "saveload-mc2_01")
BOX_GATE_COMMAND = ["tools/box_run.py", "--gate"]
sys.path.insert(0, str(ROOT / "tools"))
from lane_merge import git_bash, run_logged  # noqa: E402


# Only the visual captures need a quiet host; everything else shares the two game slots.
# The gate takes and drops the exclusive hold per stage, so a window never holds it for more than a stage (TTL 1200 s).
EXCLUSIVE_PREFIXES = ("capture-before", "capture-after")
EXCLUSIVE_TTL_S = 1200
EXCLUSIVE_SECONDS = []
FAILED_STAGES = []
ACTIVE_STATE = None

OFFLINE_STAGES = ("contracts", "harnesses", "pytest", "unit-msvc-build")


def gate_env(name):
    """A8: MC2_IN_GATE (lease priority, nested-gate refusal) belongs on game-launching stages only; the offline
    stages run test suites that must see the same environment a lane sees."""
    return {} if name in OFFLINE_STAGES else {"MC2_IN_GATE": "1"}


def window_reusable(window, tree, now=None):
    """A6: lane_merge verification may be reused when the window recorded the commit tree and it is still HEAD's tree
    (content-addressed, no clock); a record without a tree keeps the old 20-minute clock."""
    if window.get("tree"):
        return window["tree"] == tree
    return (now if now is not None else time.time()) - window.get("started", 0) <= 1200

# Last-measured stage seconds (W112) for worker balancing when no journal exists yet.
DEFAULT_STAGE_SECONDS = {"mods": 876, "tier1": 304, "capture-after": 70, "player-path": 180, "video-decode": 250, "kit-visibility": 45, "quiet": 45,
                         "determinism-mc2_03": 55, "determinism-mc2_10": 120, "determinism-mc2_16": 104, "determinism-mc2_24": 191}


def stage_seconds(name):
    """Seconds of `name` at the last gate that ran it (build64-ninja/gate-durations.json), else the W112 default."""
    if ACTIVE_STATE is not None:
        try:
            value = json.loads(ACTIVE_STATE.path.with_name("gate-durations.json").read_text(encoding="utf-8")).get(name)
            if isinstance(value, (int, float)) and value > 0:
                return float(value)
        except (OSError, ValueError):
            pass
    return float(DEFAULT_STAGE_SECONDS.get(name, 100 if name.startswith("saveload-") else 60))


def balance_movable(movable, load_a, load_b, seconds=None):
    """Longest-first split of rows that may run on either worker. -> (rows for A, rows for B), original order kept."""
    seconds = seconds or stage_seconds
    to_a, to_b = [], []
    for step in sorted(movable, key=lambda s: -seconds(s[0])):
        if load_a < load_b:
            to_a.append(step)
            load_a += seconds(step[0])
        else:
            to_b.append(step)
            load_b += seconds(step[0])
    return [s for s in movable if s in to_a], [s for s in movable if s in to_b]


# A red row whose child output names one of these failed on the host, not in the product: no suspect range.
INFRA_RE = re.compile(r"WinError (?:32|5|112)\b|No space left on device|not enough space on the disk"
                      r"|REFUSED: a merge gate is running|host[- ]lease[^\n]*(?:timed out|timeout)|BOX_INFRA", re.I)


def infra_status(returncode, output):
    """'INFRA (rc): <marker>' for a red child whose output carries a host/infra marker, else None."""
    match = INFRA_RE.search(output or "")
    return f"INFRA ({returncode}): {match.group(0)}" if returncode and match else None


def stage_passed(status):
    return status == "PASS" or status.startswith(("REUSED:", "PASS (perf_counter-only WARN"))


def box_reachable(runner=None):
    """True when the rented box answers ssh (box_run --probe, 15 s cap). Anything else selects the Windows-full fallback."""
    try:
        return (runner or run_child)([sys.executable, str(ROOT / BOX_GATE_COMMAND[0]), "--probe"], cwd=ROOT, env=os.environ.copy()).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def apply_box_route(steps, full=False):
    """Policy: with a live box-gate stage the offloaded PC rows are REUSED from it (execute() grades them red when the
    box stage is red; nothing is waived). A skipped box stage, or the daily --full gate, leaves every PC row as planned."""
    box = next((s for s in steps if s[0] == "box-gate"), None)
    if full or box is None or box[2].get("MC2_GATE_SKIP_REASON"):
        return steps
    why = "box gate (GPU 3, clang/libc++, Linux baselines) proves this row; the PC keeps determinism-mc2_10 and saveload-mc2_10 as the MSVC canary"
    out = []
    for n, c, e in steps:
        if e.get("MC2_GATE_SKIP_REASON"):
            out.append((n, c, e))
        elif n in BOX_OFFLOADED:
            out.append((n, c, dict(e, MC2_GATE_SKIP_REASON=why, MC2_GATE_REUSE_STAGE="box-gate")))
        elif n == "contracts" and "--rows" not in c and "--skip-rows" not in c:
            # The box runs every row but the PC-only ones (scripts/box_contracts_pc_only.txt); the PC runs exactly those. The
            # Linux half is a reused step: green box = reused, red box = red, dead box = box_fallback runs it here.
            pc = box_contracts_pc_only()
            reuse = dict(e, MC2_GATE_SKIP_REASON="box gate runs the Linux-capable contract rows (scripts/box_contracts.sh)", MC2_GATE_REUSE_STAGE="box-gate")
            if pc:
                rx = "^(" + "|".join(pc) + ")$"
                out += [(n, c + ["--rows", rx], e), ("contracts-linux", c + ["--skip-rows", rx], reuse)]
            else:
                out.append((n, c, reuse))
        elif n == "harnesses" and "--unit" in c:
            # doctests move to the box (clang/libc++); the PC keeps the Python contracts. Dead box: unit-linux runs the native tier here.
            # The PC still compiles mc2_tests with MSVC (no run): the box's clang cannot prove the MSVC build of the test tree.
            out += [(n, ["--python-only" if a == "--unit" else a for a in c], e),
                    ("unit-msvc-build", [a for a in c if a != "--run"] + ["--unit-only"], e),
                    ("unit-linux", c, dict(e, MC2_GATE_SKIP_REASON="box gate runs the doctest tier (box_run unit row)", MC2_GATE_REUSE_STAGE="box-gate"))]
        else:
            out.append((n, c, e))
    return out


def box_contracts_pc_only():
    """Contract row labels that need Windows (scripts/box_contracts_pc_only.txt, one per line, # comments)."""
    try:
        lines = (ROOT / "scripts" / "box_contracts_pc_only.txt").read_text(encoding="utf-8").splitlines()
    except OSError:
        return []
    return [ln.strip() for ln in lines if ln.strip() and not ln.lstrip().startswith("#")]


def box_fallback(reused, completed):
    """Windows-full fallback: when the box stage died of BOX_INFRA (box failed, no product verdict) the offloaded rows run on the PC."""
    if not str(completed.get("box-gate", "")).startswith("INFRA"):
        return reused, False
    strip = ("MC2_GATE_SKIP_REASON", "MC2_GATE_REUSE_STAGE")
    return [(n, c, {k: v for k, v in e.items() if k not in strip} if e.get("MC2_GATE_REUSE_STAGE") == "box-gate" else e)
            for n, c, e in reused], True


def farm_key(command):
    """Command identity for the farm runner table; execute() swaps the --out value for a fresh directory on resume, so the key leaves it out."""
    command = list(command)
    if "--out" in command:
        i = command.index("--out") + 1
        del command[i:i + 1]
    return tuple(command)


def perf_warn_status(output):
    """'PASS (perf_counter-only WARN: ...)' when a green child printed [PERF_COUNTER_WARN] lines (scripts/run_smoke.py), else 'PASS'.

    A perf_counter-only smoke verdict (no pixel/SIMID/save/validation/content bucket) never fails a row; the numbers ride in the status."""
    notes = [l.split("[PERF_COUNTER_WARN]", 1)[1].strip() for l in (output or "").splitlines() if "[PERF_COUNTER_WARN]" in l]
    return "PASS (perf_counter-only WARN: " + "; ".join(dict.fromkeys(notes)).replace("|", "/") + ")" if notes else "PASS"


def clean_failed_outputs(state, proof):
    """Explicit preflight cleanup; receipts and passing evidence are retained."""
    root = Path(proof).resolve()
    rows = state.data['stages']
    protected = [Path(row['artefact_dir']).resolve() for row in rows.values()
                 if stage_passed(row.get('status', '')) and row.get('artefact_dir')]
    for name, row in rows.items():
        if not name.startswith('determinism-') or not row.get('status', '').startswith('FAIL'):
            continue
        source = Path(row['artefact_dir'])
        target = source.resolve()
        if target.parent != root or not target.name.startswith(name) or source.is_symlink() or source.is_junction():
            raise ValueError(f'refusing output cleanup outside proof root: {target}')
        if any(p == target or p.is_relative_to(target) for p in protected):
            raise ValueError(f'refusing cleanup of passing evidence: {target}')
        if target.exists():
            shutil.rmtree(target)
            print(f'[merge_gate] CLEAN failed output {name}: {target}; receipt retained', flush=True)


def preflight(head, install, window, steps, seed=Path("E:/mc2-installs/_lean-seed")):
    """Offline, fail before any lease, copy, build or deployment."""
    from exe_fingerprint import extract, build_current
    from mod_smoke import load_known_failures
    from deploy_payload import verify_published_identity, staleness_report
    import saveload_roundtrip as saves

    started = time.monotonic()
    errors = []
    rebuild = "py -3 scripts/ninja_build.py mc2 mc2_asset_viewer -j" + Path("A:/Games/mc2-ops/JOBS").read_text().strip()
    for name in ("mc2.exe", "out/tools/asset_viewer/mc2_asset_viewer.exe"):
        fp = extract(ROOT / "build64-ninja" / name)
        # A clean ancestor build stays current when no build input changed since (data/test-only commits).
        if not fp.get('present') or not build_current(fp.get('sha'), head, ROOT, bool(fp.get('dirty'))):
            errors.append(f"{name} fingerprint != clean HEAD {head} (or a build-input-identical ancestor); fix: {rebuild}")
    bookmarks = set()
    for name, command, env in steps:
        if name.startswith('capture-') and '--bookmarks' in command and not env.get('MC2_GATE_SKIP_REASON'):
            bookmarks.update(command[command.index('--bookmarks') + 1].split(','))
    for target in dict.fromkeys((Path(install), Path(seed))):
        try:
            if any(n == 'deploy' and not e.get('MC2_GATE_SKIP_REASON') for n, _, e in steps):
                # Before captures use the published build; the candidate is
                # checked against HEAD after deployment, before runtime proofs.
                report = staleness_report(str(target))
                if not report['has_manifest'] or not report['version_ok'] or report['stale'] or report['missing']:
                    raise ValueError('published seed bytes differ from its manifest')
            else:
                verify_published_identity(target, ROOT, head)
        except (OSError, ValueError, KeyError) as exc:
            errors.append(f"published identity {target}: {exc}; fix: deploy clean HEAD with {rebuild}, then refresh this seed/install")
        for bookmark in sorted(bookmarks):
            path = target / 'saved_bookmarks' / (bookmark + '.json')
            try:
                doc = json.loads(path.read_text(encoding='utf-8'))
                poses = set()
                for pose in doc.get('bookmarks', []):
                    values = tuple(float(v) for v in (*pose['pos'], float(pose['rot']) % 360, pose['proj'], pose['alt']))
                    if len(values) != 6 or not all(math.isfinite(v) for v in values):
                        raise ValueError('invalid camera pose')
                    poses.add(values)
                if not doc.get('mission') or len(poses) < 2:
                    raise ValueError('requires mission and at least two distinct camera poses')
            except (OSError, ValueError, TypeError, KeyError) as exc:
                source = ROOT / 'tests/visual/bookmarks' / path.name
                errors.append(f"camera fixture {path}: {exc}; fix: New-Item -ItemType Directory -Force '{path.parent}'; Copy-Item -LiteralPath '{source}' -Destination '{path}'")
    for name, command, env in steps:
        if not name.startswith('saveload-') or env.get('MC2_GATE_SKIP_REASON'):
            continue
        mission = command[command.index('--mission') + 1]
        mask = ROOT / command[command.index('--mask') + 1]
        try:
            record = json.loads((ROOT / 'tests/saveload/baselines' / (mission + '.json')).read_text())
            if (json.dumps(record.get('comparison'), sort_keys=True) != json.dumps(saves.mask_policy(str(mask)), sort_keys=True)
                    or record.get('judged') not in ('first-save-only', 'first-save-and-resave')
                    or not record.get('overall') or not record.get('runs')
                    or not record.get('packets') or record.get('save_at') is None or record.get('load_at') is None):
                raise ValueError('reference is missing evidence or has stale comparison policy')
        except (OSError, ValueError, TypeError) as exc:
            errors.append(f"{mission} first-save reference: {exc}; fix: py -3 tools/saveload_roundtrip.py --baseline --mission {mission} --install {install} --mask tools/save_noise_mask.json --first-save-only")
    try:
        entries = load_known_failures()
        if window is None or window < 1:
            raise ValueError('current window required; pass --window N to merge_gate.py')
        expired = [e for e in entries if not e['first_window'] <= window < e['expires_window']]
        if expired:
            raise ValueError('expired/not-yet-active entries; renew tools/mod_known_failures.json with an owner note')
    except (OSError, ValueError, TypeError, KeyError) as exc:
        errors.append(f"known-failure list: {exc}; fix: edit tools/mod_known_failures.json with owner-approved ceilings and three-window expiry")
    for error in errors:
        print('[merge_gate] preflight FAIL: ' + error, flush=True)
    print(f"[merge_gate] preflight {'FAIL' if errors else 'PASS'} seconds={time.monotonic() - started:.2f}", flush=True)
    return int(bool(errors))


class GateState:
    """Crash-safe stage journal with an OS lock (released even after a crash)."""
    def __init__(self, path, head, exe, proof, steps, resume=False, read_only=False):
        self.path, self.head, self.exe = Path(path), head, Path(exe)
        self.mutex = threading.Lock()
        self.lock = None
        self.data = dict(pid=os.getpid(), head=head, artefact_dir=str(proof),
                         order=[n for n, _, _ in steps], stages={})
        self.resume = resume
        self.read_only = read_only
        self.reused = []
        self.started_signatures = {}
        self.started_reports = {}
        self.specs = {n: (c, e) for n, c, e in steps}

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.lock = self.path.with_suffix(".lock").open("a+b")
        self.lock.seek(0)
        if os.fstat(self.lock.fileno()).st_size == 0:
            self.lock.write(b"0")
            self.lock.flush()
        self.lock.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(self.lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            self.lock.close()
            raise RuntimeError(f"gate-state held by a live PID: {self.path}") from exc
        try:
            if self.path.exists():
                previous = json.loads(self.path.read_text(encoding="utf-8"))
                if self.read_only:
                    self.data = previous
                    return self
                self.data = {**previous, **self.data, "stages": previous.get("stages", {})}
            self.data["exe_sha"] = self.exe_sha()
            self.write()
        except BaseException:
            self.__exit__(None, None, None)
            raise
        return self

    def __exit__(self, *exc):
        self.lock.seek(0)
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(self.lock.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(self.lock.fileno(), fcntl.LOCK_UN)
        self.lock.close()

    def write(self):
        pending = self.path.with_suffix(".tmp")
        pending.write_text(json.dumps(self.data, indent=2) + "\n", encoding="utf-8")
        pending.replace(self.path)

    def exe_sha(self):
        if not self.exe.is_file():
            return None  # missing exe evidence can never be resumed
        with self.exe.open("rb") as source:
            return hashlib.file_digest(source, "sha256").hexdigest()

    def signature(self, command, overrides):
        # HEAD pins clean tracked sources. Also hash file inputs: commands and
        # policy/data receipts can change without a new source commit.
        inputs = {ROOT / 'tools/save_noise_mask.json', ROOT / 'tools/mod_known_failures.json',
                  ROOT / 'Gates/gate_table.json', ROOT / 'scripts/merge_gate.py',
                  ROOT / 'build64-ninja/CMakeCache.txt'}
        inputs.update((ROOT / 'tests/saveload/baselines').glob('*.json'))
        inputs.update((ROOT / 'tests/smoke/sim_determinism').glob('*.json'))
        mod_battery = any(Path(arg).name == 'mod_smoke.py' for arg in command)
        base = None
        if mod_battery:
            import mod_smoke as mods
            def option(flag, default):
                return Path(command[command.index(flag) + 1] if flag in command else default).resolve()
            base = option('--base-install', mods.DEFAULT_BASE_INSTALL)
            mod_root = option('--install-root', mods.DEFAULT_INSTALL_ROOT)
            build = option('--build-dir', ROOT / 'build64-ninja')
            inputs.update(p for p, _ in mods.engine_payload_items(build, build))
            inputs.add(mods.WARNING_BUDGET)
            inputs.add(ROOT / 'system.cfg')
            for directory in ('scripts', 'tools'):
                inputs.update((ROOT / directory).rglob('*.py'))
            # Hash bytes, including cached recipes; size/mtime is never proof.
            # Candidate directories have random names; key their contents relatively.
            inputs.update(p for p in base.rglob('*') if p.is_file()
                          and p.relative_to(base).parts[0] not in ('debug_state', 'crashes', 'logs')
                          and p.suffix.lower() not in ('.log', '.dmp', '.tmp', '.pdb'))
            for _, folder, parent in mods.BATTERY:
                for layer in mods.mod_layers(mod_root, folder, parent):
                    if not layer.is_dir():
                        raise FileNotFoundError(f'mod receipt input missing: {layer}')
                    inputs.update(p for p in layer.rglob('*') if p.is_file())
        for arg in command:
            path = Path(arg)
            if not path.is_absolute():
                path = ROOT / path
            if path.is_file():
                inputs.add(path)
            elif path.is_dir() and '--install' in command and arg == command[command.index('--install') + 1]:
                inputs.add(path / '.deployed_manifest.csv')
        hashes = {}
        for path in sorted(inputs, key=str):
            key = str(path)
            if base is not None and path.is_relative_to(base):
                key = '{INSTALL}/' + path.relative_to(base).as_posix()
            if path.is_file():
                if base is not None and path == base / '.deployed_manifest.csv':
                    import csv
                    with path.open(encoding='utf-8-sig', newline='') as source:
                        next(source)
                        rows = [{k: row[k] for k in ('relpath', 'sha256', 'bytes', 'src_commit')}
                                for row in csv.DictReader(source)]
                    hashes[key] = hashlib.sha256(json.dumps(sorted(rows, key=lambda r: r['relpath']), sort_keys=True).encode()).hexdigest()
                    continue
                with path.open('rb') as source:
                    hashes[key] = hashlib.file_digest(source, 'sha256').hexdigest()
            else:
                hashes[key] = None
        if base is not None:
            command = [arg.replace(str(base), '{INSTALL}').replace(base.as_posix(), '{INSTALL}') for arg in command]
        ambient = {k: v for k, v in os.environ.items() if k.startswith('MC2_') or k == 'PYTHONPATH'}
        if mod_battery:
            overrides = dict(overrides)
            for env in (overrides, ambient):
                if 'MC2_SMOKE_ARTIFACT_ROOT' in env:
                    env['MC2_SMOKE_ARTIFACT_ROOT'] = '{PROOF}/smokes'
        return hashlib.sha256(json.dumps([command, overrides, hashes, ambient], sort_keys=True).encode()).hexdigest()

    def reusable(self, name, command, overrides):
        row = self.data["stages"].get(name, {})
        sha = self.exe_sha()
        offline = name in OFFLINE_STAGES
        if not ((row.get('head') == self.head or (not offline and head_delta_nonruntime(row.get('head'), self.head, name)))
                and (offline or sha is not None and row.get('exe_sha') == sha)
                and stage_passed(row.get('status', ''))):
            return False
        specs = (command, overrides) if name == 'mods' else self.specs.get(name, (command, overrides))
        evidence = row.get('evidence', {})
        if not offline and name != 'deploy':
            # Every runtime row is reused only on the evidence it recorded: a missing or edited file refuses it.
            if (name == 'mods' and not row.get('signature')) or not evidence:
                print(f'[merge_gate] {name}: no recorded evidence hash; rerunning', flush=True)
                return False
            try:
                signature = self.signature(*specs)
                if not all(Path(p).is_file() and hashlib.sha256(Path(p).read_bytes()).hexdigest() == sha
                           for p, sha in evidence.items()):
                    print(f'[merge_gate] {name}: recorded evidence missing or changed; rerunning', flush=True)
                    return False
            except (OSError, ValueError, KeyError):
                return False
        else:
            signature = self.signature(*specs)
        if row.get("signature") != signature:
            return False
        if name == 'deploy' and 'scripts/deploy_payload.py' in command:
            from deploy_payload import verify_published_identity
            target = (Path(command[command.index('--allow-new-target') - 1])
                      if '--allow-new-target' in command else Path('A:/Games/mc2-opengl/mc2-win64-v0.4'))
            try:
                verify_published_identity(target, ROOT, self.head)
            except (OSError, ValueError, RuntimeError):
                print(f'[merge_gate] deploy: cached proof requires current payload in {target}', flush=True)
                return False
        return True

    def record(self, name, command, overrides, status, seconds, scheduler_wait=0.0):
        with self.mutex:
            specs = (command, overrides) if name == 'mods' else self.specs.get(name, (command, overrides))
            try:
                signature = None if name == 'mods' and status != 'PASS' else self.signature(*specs)
            except (OSError, ValueError, KeyError):
                if name != 'mods':
                    raise
                signature = None
            evidence = {}
            if name == 'mods' and status == 'PASS' and '--stage-root' in command:
                stage_root = Path(command[command.index('--stage-root') + 1])
                reports = list(stage_root.glob('*/battery.json'))
                report = max(reports, key=lambda p: p.stat().st_mtime_ns) if reports else None
                if (report and signature and signature == self.started_signatures.get(name)
                        and report not in self.started_reports.get(name, set())):
                    roots = [report.parent]
                    if overrides.get('MC2_SMOKE_ARTIFACT_ROOT'):
                        roots.append(Path(overrides['MC2_SMOKE_ARTIFACT_ROOT']))
                    evidence = {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                                for root in roots for p in root.rglob('*')
                                if p.is_file() and p.suffix in ('.json', '.log')}
            elif name not in ('contracts', 'harnesses', 'pytest', 'deploy', 'mods') and stage_passed(status) \
                    and not status.startswith('REUSED'):
                try:
                    evidence = row_evidence(command, overrides)
                except OSError:
                    evidence = {}
            artefact = overrides.get("MC2_SMOKE_ARTIFACT_ROOT", self.data["artefact_dir"])
            for option in ("--out", "--out-root"):
                if option in command:
                    artefact = command[command.index(option) + 1]
            if status == "PASS" and seconds > 0 and not self.read_only:
                durations = self.path.with_name("gate-durations.json")
                try:
                    known = json.loads(durations.read_text(encoding="utf-8"))
                except (OSError, ValueError):
                    known = {}
                known[name] = round(seconds, 1)
                try:
                    durations.write_text(json.dumps(known, sort_keys=True), encoding="utf-8")
                except OSError:
                    pass
            self.data["stages"][name] = dict(
                head=self.head, exe_sha=self.exe_sha(), status=status, seconds=seconds,
                artefact_dir=artefact, scheduler_wait_seconds=scheduler_wait,
                signature=signature, evidence=evidence)
            self.write()


@contextmanager
def gate_journal(path, head, exe, proof, steps, resume, dry_run):
    global ACTIVE_STATE
    if dry_run:
        yield None
        return
    with GateState(path, head, exe, proof, steps, resume) as state:
        ACTIVE_STATE = state
        try:
            yield state
        finally:
            ACTIVE_STATE = None


def plan(jobs, proof_dir=None, capture_before_runs=1, before_dir=None):
    if capture_before_runs < 1:
        raise ValueError("--capture-before-runs must be positive")
    py = sys.executable
    install = "A:/Games/mc2-opengl/mc2-win64-v0.4"
    proof = Path(proof_dir or "E:/mc2-archive-2026-10/proofs/merge-gate")
    capture = [py, "tools/visual_verify/capture.py", "--install", install,
               "--bookmarks", "mc2_01_saved_205644,mc2_10_saved_204523", "--clean"]
    steps = [
        ("capture-before", capture + ["--out", str(proof / "before")], {}),
        ("deploy", [py, "scripts/deploy_payload.py", "--target", "game", "--ninja", "--require-clean"], {}),
        ("capture-after", capture + ["--out", str(proof / "after")], {}),
        ("compare-captures", [py, "tools/visual_verify/ab.py", "--compare-only", str(proof / "before"),
                              str(proof / "after"), "--out-root", str(proof / "ab")], {}),
        ("contracts", [git_bash(), "scripts/check-contracts.sh", "-j" + str(jobs)], {}),
        ("harnesses", [py, "tools/build_contract_harnesses.py", "--run", "--unit", "-j", str(jobs)], {}),
        ("tier1", [py, "scripts/run_smoke.py", "--tier", "tier1", "--keep-logs", "--deploy", "0.4"],
         {"MC2_DEBUG_STATE_DUMP": "1", "MC2_DIAG_TAGS": "CONFIG,BUILD,DEVICE"}),
        # What a player does: scripted menu input (MC2_INPUT_SCRIPT) -> New Campaign mission 1 past the first comm video, and Load Game.
        # Real video decode and the real front end; every other smoke boots --mission under SmokeMode and skips both.
        ("player-path", [py, "scripts/run_player_path.py", "--install", install, "--out", str(proof / "player-path")], {}),
        # Real comm-video decode with the sim overlap worker ON (smoke mode skips the movie): red on a SIM_OVERLAP violation or no decode.
        ("video-decode", [py, "scripts/video_decode_smoke.py", "--exe", install + "/mc2.exe", "--out", str(proof / "video-decode")], {}),
        ("kit-visibility", [py, "scripts/check-kit-visibility.py", "--install", install,
                            "--out", str(proof / "kit-visibility")], {}),
        # The ordinary contracts row is offline; fresh default-build quiet grading launches the game, so it lives here with the gate's exe.
        ("quiet", [py, "scripts/check-default-build-quiet.py", "--fresh-smoke", "--exe", install + "/mc2.exe"], {}),
    ]
    # Preserve the original stage names and paths for existing --stages callers.
    noise = []
    for run in range(2, capture_before_runs + 1):
        before = proof / f"before-{run}"
        noise.extend([
            (f"capture-before-{run}", capture + ["--out", str(before)], {}),
            (f"compare-noise-{run}", [py, "tools/visual_verify/ab.py", "--compare-only",
                                     str(proof / "before"), str(before), "--out-root",
                                     str(proof / f"noise-{run}")], {}),
        ])
    steps[1:1] = noise
    if before_dir is not None:
        before = str(proof / "before")
        steps = [(name, [str(before_dir) if arg == before else arg for arg in cmd], env)
                 for name, cmd, env in steps if name != "capture-before"]
    # mc2_03 runs headless; mc2_10 and mc2_24 retain the GPU path. The finale
    # spawns infantry after tick 2404: --ticks is a required SIMID length (zero
    # tolerance), so the baseline prefix alone cannot satisfy this audit.
    for mission in ("mc2_03", "mc2_10", "mc2_16", "mc2_24"):
        steps.append(("determinism-" + mission,
                      [py, "tools/sim_determinism.py", "check", mission, "--install", install, "--out", str(proof / ("determinism-" + mission)),
                       # mc2_10 reaches only ~650 of the 800 sim frames in 20 s under load; 2x gets there in half the host time (both runs of a pair share the speed)
                       *(["--sim-speed", "2"] if mission == WINDOWED_DETERMINISM else
                         (["--sim-speed", "4", "--ticks", "2600"] if mission == "mc2_24" else ["--headless-both"]))], {}))
    for mission in ("mc2_01", "mc2_03", "mc2_10", "mc2_16", "mc2_24"):
        steps.append(("saveload-" + mission,
                      [py, "tools/saveload_roundtrip.py", "check", "--mission", mission,
                       "--install", install, "--mask", "tools/save_noise_mask.json", "--first-save-only", "--out", str(proof / ("saveload-" + mission)),
                       # private save dir (engine MC2_SAVE_DIR): saveload overlaps the mods battery with no shared quicksave names
                       "--save-dir", str(proof / ("saveload-" + mission) / "savegame")], {}))
    # Staging has one cache writer; two isolated smoke children lease their own slots.
    steps.append(("mods", [py, "tools/mod_smoke.py", "--battery", "--workers", "2", "--exe", install + "/mc2.exe", "--build-dir", "build64-ninja"], {}))
    steps.append(("validation",
                  [py, "scripts/run_smoke.py", "--lite", "--keep-logs", "--deploy", "0.4"],
                  {"MC2_VULKAN_VALIDATION": "1"}))
    # First, so the serial path has its verdict before the reused rows are graded; execute_parallel starts it in the background at gate start.
    steps.insert(0, ("box-gate", [py, *BOX_GATE_COMMAND], {}))
    return steps


# Unknown inputs deliberately retain every stage. Only proven independent domains
# can waive game proofs; generators/build configuration retain full coverage.
EVIDENCE_SUFFIXES = ('.json', '.log', '.png', '.md', '.csv', '.txt')


def evidence_roots(command, overrides):
    """Where a runtime row writes or reads its proof: --out/--out-root, both --compare-only inputs, MC2_SMOKE_ARTIFACT_ROOT."""
    roots = []
    for option, count in (('--out', 1), ('--out-root', 1), ('--compare-only', 2)):
        if option in command:
            i = command.index(option)
            roots += [Path(arg) for arg in command[i + 1:i + 1 + count]]
    if overrides.get('MC2_SMOKE_ARTIFACT_ROOT'):
        roots.append(Path(overrides['MC2_SMOKE_ARTIFACT_ROOT']))
    return [root if root.is_absolute() else ROOT / root for root in roots]


def row_evidence(command, overrides):
    """sha256 of every evidence file under the row's roots (empty = the row left nothing to reuse)."""
    files = set()
    for root in evidence_roots(command, overrides):
        if root.is_file():
            files.add(root)
        elif root.is_dir():
            files.update(p for p in root.rglob('*') if p.is_file() and p.suffix.lower() in EVIDENCE_SUFFIXES)
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(files)}


# Proof inputs read by one row family only (stage-name prefixes). A commit that touches just these (e.g. a re-frozen
# quiet baseline) reruns those rows and keeps every other row's evidence for the same exe.
ROW_PROOF_INPUTS = {
    "tests/contract/default_build_quiet_baseline.json": ("quiet",),
    "scripts/check-default-build-quiet.py": ("quiet",),
    "tools/save_noise_mask.json": ("saveload-",),
    "tools/saveload_roundtrip.py": ("saveload-",),
    "tests/saveload/": ("saveload-",),
    "tests/smoke/sim_determinism/": ("determinism-",),
    "tools/mod_smoke.py": ("mods",),
}


def row_scoped_input(path):
    """The row prefixes a row-scoped proof input belongs to, or None for an ordinary path."""
    p = path.replace("\\", "/").lower()
    return next((rows for prefix, rows in ROW_PROOF_INPUTS.items() if p.startswith(prefix)), None)


def head_delta_nonruntime(old, new, row=None):
    """True when old..new touches only docs/tooling paths (change_domains; unknown paths count as engine). A runtime stage
    that passed at `old` with the same exe sha and the same signature inputs is then still valid at `new`. With row (a
    stage name), a row-scoped proof input (ROW_PROOF_INPUTS) of another row is ignored and one of this row's is a change."""
    if not old or not new:
        return False
    try:
        out = subprocess.run(['git', 'diff', '--name-only', f'{old}..{new}'], cwd=ROOT, capture_output=True, text=True,
                             check=True, **console_child.child_kwargs(redirected=True)).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        return False
    if row is not None:
        scoped = {p: row_scoped_input(p) for p in out}
        if any(rows and row.startswith(rows) for rows in scoped.values()):
            return False
        rest = [p for p, rows in scoped.items() if not rows]
        return bool(out) and change_domains(rest) <= {'docs', 'tooling'}
    return bool(out) and change_domains(out) <= {'docs', 'tooling'}


def change_domains(paths):
    domains = set()
    for raw in paths:
        p = raw.replace("\\", "/").lower()
        suffix = Path(p).suffix
        if p.startswith(("docs/", ".claude/")) or suffix in (".md", ".rst"):
            domains.add("docs")
        elif p.startswith(("scripts/merge_gate.py", "scripts/deploy_payload.py", "scripts/kit", "scripts/run_smoke.py",
                           "scripts/smoke_lib/", "scripts/run_visual_capture.py", "scripts/check-default-build-quiet.py",
                           "scripts/cc_launcher.py", "scripts/ninja_build.py", "scripts/run-unit-tests.sh",
                           "tools/sim_determinism.py", "tools/saveload_roundtrip.py", "tools/mod_smoke.py",
                           "tools/visual_verify/", "tools/build_all_targets.py", "tools/build_contract_harnesses.py",
                           "tests/saveload/", "tests/smoke/sim_determinism/", "tests/smoke/baselines.json",
                           "tests/contract/default_build_quiet_baseline.json", "tools/save_noise_mask.json")):
            domains.add("engine")  # Packaging and game-proof tools can affect every runtime check.
        elif p.startswith(("tools/scifi_kit/", "tools/blender_pipeline/", "tools/content_pipeline/", "tools/mc2data/", "tools/ase_to_glb.py", "release_assets_src/", "mods/", "examples/mods/")) or suffix in (".blend", ".glb", ".ktx2", ".png", ".tga", ".dds", ".jpg"):
            domains.add("art")
        elif p.startswith("shaders/") or suffix in (".slang", ".frag", ".vert", ".comp", ".glsl"):
            domains.add("shaders")
        elif p.startswith(("rendercore/", "gameos/")) and suffix in (".cpp", ".h", ".hpp", ".inc"):
            # Renderer and OS headers may affect simulation timing/serialization too.
            domains.add("engine")
        elif ((suffix == ".py" and (p.startswith(("scripts/tests/", "tests/")) or "/tests/" in p or Path(p).name.startswith("test_")))
              or p.startswith(("tools/lane_merge.py", "tools/file_size_ratchet", "tools/cxx17_ratchet/",
                               "tools/dup_lines_ratchet", "tools/axis_swizzle_ratchet", "tests/callers",
                               "scripts/run_tools_tests.py", "scripts/run_scripts_tests.py", "scripts/module_test_cache.py",
                               "scripts/check-contracts.sh", "scripts/test_audit_site/"))):
            domains.add("tooling")
        else:
            domains.add("engine")
    return domains


def affected_plan(steps, paths, full=False):
    domains = change_domains(paths)
    # Save paths retain serialization coverage; asset names alone do not imply it.
    classic_saves = full or any(any(token in p.replace("\\", "/").lower() for token in (
        "save", "serializ", "mission_load", "msl_load")) for p in paths
        if change_domains([p]) & {"engine", "art", "shaders"})
    art = full or bool(domains & {"art"}) or any(
        any(token in p.replace("\\", "/").lower() for token in
            ("kit", "building", "bdactor", "model_override", "loose_asset", "mc2texcook", "content_pipeline", "content_ir",
             "appearance", "texture", "material", "gltf", "deploy_payload", "render_options"))
        for p in paths)
    # Unknown runtime inputs may change asset routing; only recognized C++
    # domains can use the reduced non-art serialization pair.
    art |= any("engine" in change_domains([p]) and not (
        p.replace("\\", "/").lower().startswith(("code/", "mclib/", "rendercore/", "gameos/"))
        and Path(p).suffix.lower() in (".cpp", ".h", ".hpp", ".inc")) for p in paths)
    game = bool(domains & {"engine", "shaders", "art"})
    sim = "engine" in domains
    selected = []
    names = {step[0] for step in steps}
    shared_smokes = {"determinism-" + WINDOWED_DETERMINISM}
    batch_tier = (sim or full) and shared_smokes <= names
    for name, cmd, env in steps:
        env = dict(env)
        cmd = list(cmd)
        reason = None
        if not full:
            if (name.startswith("determinism-") and not sim) or (name.startswith("saveload-") and not (sim or "art" in domains or classic_saves)):
                reason = "diff has no engine/simulation inputs (" + ", ".join(sorted(domains)) + ")"
            elif name not in ("contracts", "pytest", "harnesses") and not game:
                reason = "docs/tooling-only diff has no runtime inputs"
            elif name == "harnesses" and not sim:
                reason = "diff has no C++ harness inputs"
        if name == "validation" and (game or full) and "tier1" in names:
            env["MC2_GATE_REUSE_STAGE"] = "tier1,determinism-mc2_10" if batch_tier else "tier1"
            reason = "tier1 and shared determinism smokes grade Vulkan validation errors/layer activation"
        if name == 'kit-visibility' and not full:
            reason = 'nightly --full coverage; window reviewer opens day/night montage'
        if name == "tier1":
            env["MC2_VULKAN_VALIDATION"] = "1"
            if batch_tier:
                cmd += ["--mission", "mc2_01", "--mission", "mc2_03", "--mission", "mc2_17", "--mission", "mc2_24"]
                env["MC2_GATE_COVERAGE"] = "tier1 mc2_10 assigned to the windowed determinism smoke; the headless determinism rows run no GPU, so this stage runs mc2_01/03/17/24"
        if name.startswith("saveload-") and not art and not full:
            if name not in ("saveload-mc2_01", "saveload-mc2_10"):
                reason = "no kit/art inputs: day/night save/load pair covers shared serialization"
        if name.startswith("saveload-") and not full and name not in ("saveload-mc2_10", "saveload-mc2_24"):
            # Keep 1x save ticks; night's slower remastered arms retain 28s.
            cmd += ["--duration", "20"]
        if name in shared_smokes and batch_tier:
            env.update(MC2_VULKAN_VALIDATION="1", MC2_MOVER_COHERENCE_AUDIT="1",
                       MC2_DEBUG_STATE_DUMP="1", MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE")
            if not full:
                cmd += ["--sim-speed", "2"]
                env["MC2_GATE_COVERAGE"] = "2 fixed sim ticks per drawn frame; exact pinned chains still required; daily --full retains 1x"
        if reason:
            env["MC2_GATE_SKIP_REASON"] = reason
        selected.append((name, cmd, env))
        if name.startswith("saveload-") and (sim or "art" in domains or classic_saves or full):
            env["MC2_OPTIONS_OVERRIDE"] = "RemasteredBuildings=TRUE"
    for name, cmd, env in selected:
        if name.startswith("capture-") and not name.endswith("-classic"):
            cmd += ["--env-declare", "MC2_OPTIONS_OVERRIDE=RemasteredBuildings=TRUE"]
    # Offline failures must appear before taking a game lease or copying payloads.
    offline = set(OFFLINE_STAGES)
    return [s for s in selected if s[0] in offline] + [s for s in selected if s[0] not in offline]


def install_plan(steps, install):
    old = "A:/Games/mc2-opengl/mc2-win64-v0.4"
    result = []
    for name, command, env in steps:
        command = [arg.replace(old, str(install).replace("\\", "/")) for arg in command]
        if name == "deploy":
            command += [str(install), "--allow-new-target"]
        if name in ("tier1", "validation"):
            idx = command.index("--deploy")
            command[idx:idx + 2] = ["--exe", str(install / "mc2.exe")]
        if name == "mods":
            # Candidate names are random and their siblings are deleted at exit.
            # Keep the existing incremental payload cache across windows, scoped
            # to this worktree; battery() still removes every disposable install.
            stage_root = Path('E:/mc2-archive-2026-10/proofs') / (ROOT.name + '-mods-stage')
            command += ["--base-install", str(install), "--stage-root", str(stage_root), "--duration", "15"]
        result.append((name, command, env))
    return result


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True,
                                   **console_child.child_kwargs(redirected=True)).strip()


def run_child(command, cwd, env):
    return run_logged(cwd, command, env=env)


@contextmanager
def stock_copy(install, parent=None, keep=False):
    """Share read-only data and copy mutable outputs with the isolation guard."""
    install = Path(install).resolve()
    # Lane rehearsals live on E:; hardlinks require the install's volume.
    parent = Path(parent or install.parent).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    prefix = install.name + "-stock-"
    target = Path(tempfile.mkdtemp(prefix=prefix, dir=parent)).resolve()
    started = time.monotonic()
    try:
        subprocess.run([sys.executable, str(ROOT / "scripts/make-isolated-deploy.py"),
                        "--from", str(install), "--to", str(target), '--snapshot'], check=True,
                       **console_child.child_kwargs())
        for child in target.iterdir():
            if child.is_dir() and (child.name.startswith(".gate-stock-") or child.name.endswith("-mods-stage") or child.name in
                                  ("mod_smoke-stage", "debug_state", "dev_shell_out")):
                shutil.rmtree(child)
        print(f"| stock-copy | PASS | {time.monotonic() - started:.1f} |", flush=True)
        yield target
    finally:
        if not keep:  # keep: the caller moves it to the candidate path (keep_stock_copy)
            if target.parent != parent or not target.name.startswith(prefix):
                raise RuntimeError(f"refusing cleanup outside lane install: {target}")
            mod_stage = target.with_name(target.name + "-mods-stage")
            if mod_stage.exists():
                if mod_stage.resolve().parent != parent:
                    raise RuntimeError(f"refusing cleanup outside lane install: {mod_stage}")
                shutil.rmtree(mod_stage)
            size = sum(p.stat().st_size for p in target.rglob("*") if p.is_file())
            shutil.rmtree(target)
            print(f"[merge_gate] removed {target}: {size / 2**30:.2f} GiB logical (physical space not measured)", flush=True)


# Queue depth does not prove a live worker (W98/W99 lost 20 min per row to an unclaimed job): the farm is opt-in
# (--farm-determinism) and a job nobody claims within this window is cancelled and run locally.
FARM_CLAIM_SECONDS = 30       # W113: 120 s on every determinism row; a live worker claims within its 1 s poll
FARM_HEARTBEAT_MAX_AGE_S = 60  # runner.py rewrites C:/mc2farm/heartbeat.json every poll; `host.py alive` reports its age

STOP_REQUESTED = threading.Event()


def install_stage_watchdog():
    """Hold the first SIGINT/SIGTERM/SIGBREAK until the running stage finishes (execute() then stops before the next stage);
    a second one restores the default so an operator can still abort. Main thread only."""
    import signal

    def handler(signum, _frame):
        if STOP_REQUESTED.is_set():
            signal.signal(signum, signal.SIG_DFL)
            signal.raise_signal(signum)
            return
        STOP_REQUESTED.set()
        print(f"[merge_gate] signal {signum}: finishing the running stage, then stopping (send again to abort now)", flush=True)
    for name in ("SIGINT", "SIGTERM", "SIGBREAK"):
        if hasattr(signal, name):
            try:
                signal.signal(getattr(signal, name), handler)
            except (ValueError, OSError):
                pass


INSTALL_LOCKS = {}
INSTALL_LOCKS_MUTEX = threading.Lock()


def install_lock(command):
    """One lock per game install a row launches from (--install, --base-install or --exe's folder). run_smoke refuses
    a second mc2.exe on the same exe path, so local game rows on one install take turns; a clone has its own lock."""
    for option in ("--install", "--base-install", "--exe"):
        if option in command and command.index(option) + 1 < len(command):
            path = Path(command[command.index(option) + 1])
            key = os.path.normcase(str((path.parent if option == "--exe" else path).resolve()))
            with INSTALL_LOCKS_MUTEX:
                return INSTALL_LOCKS.setdefault(key, threading.RLock())
    return nullcontext()


def farm_stage(command, overrides, runner=None, local_env=None):
    """Submit one unchanged determinism check, wait, and fetch its evidence. local_env is the stage env execute()
    built (MC2_IN_GATE / lease batch included); a local fallback runs with it so GATE QUIET treats it as the gate's own."""
    runner = run_child if runner is None else runner
    local_env = dict(os.environ, **overrides) if local_env is None else local_env
    out = Path(command[command.index("--out") + 1])
    out.parent.mkdir(parents=True, exist_ok=True)
    remote = list(command)
    remote[0] = "python"
    remote[1] = "C:/mc2farm/harness/tools/sim_determinism.py"
    remote[remote.index("--install") + 1] = "{ROOT}"
    remote[remote.index("--out") + 1] = "{RESULT}/determinism"
    remote += ["--ref-exe", "{EXE}"]
    env = {k: v for k, v in overrides.items() if not k.startswith("MC2_GATE_")}
    env.update(MC2_SMOKE_ARTIFACT_ROOT="{RESULT}/smokes",
               MC2_HOST_LEASE_PATH="C:/mc2farm/host_game_lease.json",
               MC2_DEPLOY_LEASE_PATH="C:/mc2farm/deploy_leases.json",
               MC2_CHILD_REGISTRY_DIR="C:/mc2farm/children", MC2_HOST_GAME_SLOTS="1",
               MC2_HARNESS_GPU="any")
    job = out.parent / (out.name + "-farm-job.json")
    job.write_text(json.dumps(dict(argv=remote, cwd="C:/mc2farm/harness", timeout=900, env=env)), encoding="utf-8")
    host = [sys.executable, "tools/farm/host.py"]
    def local(reason):
        print(f'[merge_gate] farm {reason}; repeating locally', flush=True)
        print(f'[merge_gate] determinism row host={os.environ.get("COMPUTERNAME", "local-pc")} (local PC, farm fallback)', flush=True)
        with install_lock(command):  # never beside the mods battery or a stock row on the same exe
            return runner(command, cwd=ROOT, env=local_env)

    try:
        alive = runner(host + ["alive", "--max-age", str(FARM_HEARTBEAT_MAX_AGE_S)], cwd=ROOT, env=os.environ.copy())
    except (OSError, subprocess.SubprocessError) as exc:
        return local(f'heartbeat check unavailable: {exc}')
    if alive.returncode:
        return local('no live worker heartbeat (skipping the claim wait)')
    try:
        submitted = runner(host + ["submit", str(job), "--exe", str(ROOT / "build64-ninja/mc2.exe")], cwd=ROOT, env=os.environ.copy())
    except (OSError, subprocess.SubprocessError) as exc:
        return local(f'submit unavailable: {exc}')
    if submitted.returncode:
        return local('submit failed')
    ids = [line.strip() for line in submitted.stdout.splitlines() if re.fullmatch(r"[0-9a-f]{32}", line.strip())]
    if len(ids) != 1:
        return local('submit returned no unique job ID')
    job_id = ids[0]
    waited = runner(host + ["wait", job_id, "--timeout", "2160", "--queue-timeout", str(FARM_CLAIM_SECONDS)], cwd=ROOT, env=os.environ.copy())
    if 'queue_timeout:' in waited.stdout:
        reason = [l for l in waited.stdout.splitlines() if l.startswith('queue_timeout:')][-1][:160]
        print(f'[merge_gate] WARNING farm determinism did not run ({reason}); no worker claimed the job in '
              f'{FARM_CLAIM_SECONDS}s or the farm was unreachable; RUNNING LOCALLY', flush=True)
        print(f'[merge_gate] determinism row host={os.environ.get("COMPUTERNAME", "local-pc")} (local PC, farm fallback)', flush=True)
        with install_lock(command):  # never beside the mods battery or a stock row on the same exe
            return runner(command, cwd=ROOT, env=local_env)
    if "job remains owned" in waited.stdout:
        return waited  # never delete the payload of a still-running farm job
    try:
        fetched = runner(host + ["fetch", job_id, str(out)], cwd=ROOT, env=os.environ.copy())
    except (OSError, subprocess.SubprocessError) as exc:
        return local(f'fetch unavailable: {exc}')
    if fetched.returncode:
        return local('fetch failed')
    print(f'[merge_gate] determinism row host=farm job={job_id}', flush=True)
    return subprocess.CompletedProcess(command, waited.returncode or fetched.returncode, waited.stdout + fetched.stdout)


PUBLISHED = Path("A:/Games/mc2-opengl/mc2-win64-v0.4")


def stage_published(install, published=PUBLISHED):
    """Make the private candidate the pixel row's single install.

    The lean seed has no data/textures/*.burnin.* (W99: before ran on v0.4 with the 6144 burn-in colormap, after on the seed with 1536,
    a constant false MAD of 1.4/0.9). Give the candidate the burn-in files so every row exercises the path the owner plays, and put the
    published exe and shaders on it so capture-before runs the OLD build on the SAME data; the deploy row then replaces them."""
    install, published = Path(install), Path(published)
    shutil.copy2(published / "mc2.exe", install / "mc2.exe")
    for name in ("shaders", "shaders_vk"):
        if (published / name).is_dir():
            shutil.rmtree(install / name, ignore_errors=True)
            shutil.copytree(published / name, install / name)
    textures = install / "data" / "textures"
    textures.mkdir(parents=True, exist_ok=True)
    for src in (published / "data" / "textures").glob("*.burnin.*"):
        if not (textures / src.name).exists():
            shutil.copy2(src, textures / src.name)
    # The deploy row cooks the BC7 detail-normal sidecars into the target; without them capture-before imports the TGAs and
    # capture-after reads the sidecars, a real pixel difference (W100: MAD 0.40 / 0.14 with an identical rendering path).
    from deploy_payload import ensure_engine_normal_cooks
    ensure_engine_normal_cooks(str(install))
    absent = [n for n in [f"mat{i}_normal.ktx2" for i in range(6)] + ["engine_normals.cookver"] if not (textures / n).is_file()]
    if absent:  # ensure_engine_normal_cooks only logs a WARN on failure; a candidate without them gives a false pixel diff
        raise RuntimeError(f"stage_published: detail-normal cook left {', '.join(absent)} missing in {textures}")


CANDIDATE = Path("E:/mc2-installs/merge-gate-candidate")
CANDIDATE_MARK = "build64-ninja/kept-candidate.json"
# Campaign mods the kept candidate carries so sweep_campaign resolves MCO (hardlinked, same volume; never from an owner install).
CANDIDATE_MODS = ("MechCommanderOmnitech", "mco-compat")
# Persistent source that lane cleanup never touches (E:/mc2-ref is reference storage, not a lane folder). Must hold one folder per name
# in CANDIDATE_MODS, each with data/. Missing source fails the keep step loudly (CANDIDATE_ERROR); the gate verdict is unaffected.
CANDIDATE_MODS_SOURCE = Path(os.environ.get("MC2_GATE_MODS_SOURCE", "E:/mc2-ref/campaign-mods"))
CANDIDATE_SEED = Path("E:/mc2-installs/_lean-seed")
CANDIDATE_ERRORS = []


def _link_or_copy(src, dst):
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def stage_candidate_mods(install, source=None, names=CANDIDATE_MODS):
    """Hardlink the campaign mods into <install>/mods (copy only if the volumes differ). A missing source is an error."""
    source, mods = Path(source or CANDIDATE_MODS_SOURCE), Path(install) / "mods"
    for name in names:
        if not (source / name).is_dir():
            raise RuntimeError(f"keep-candidate: mod {name} missing under {source}")
        shutil.rmtree(mods / name, ignore_errors=True)
        shutil.copytree(source / name, mods / name, copy_function=_link_or_copy)
    return [str(mods / n) for n in names]


def drop_candidate(candidate=None, root=None):
    """Remove the kept candidate install and its record."""
    candidate = Path(candidate or CANDIDATE)
    shutil.rmtree(candidate, ignore_errors=True)
    (Path(root or ROOT) / CANDIDATE_MARK).unlink(missing_ok=True)
    print(f"[merge_gate] candidate dropped: {candidate}", flush=True)


def candidate_identity(install, head, root=None):
    """What the candidate actually runs: its mc2.exe must be the gate's build (same bytes as build64-ninja/mc2.exe) and that
    build must be clean and current for head. Raises RuntimeError otherwise (e.g. the deploy failed and the copy still holds
    the published exe). Returns the exe sha256 and the commit the exe was built from, which may be an ancestor of head."""
    import exe_fingerprint
    root = Path(root or ROOT)
    exe, build = Path(install) / "mc2.exe", root / "build64-ninja" / "mc2.exe"
    try:
        digest, built = sha256_path(exe), sha256_path(build)
    except OSError as exc:
        raise RuntimeError(f"keep-candidate: cannot hash the candidate exe: {exc}") from exc
    if digest != built:
        raise RuntimeError(f"keep-candidate: {exe} is not the gate build {build} (deploy did not land); previous candidate kept")
    fp = exe_fingerprint.extract(exe)
    if not fp.get("present") or not exe_fingerprint.build_current(fp.get("sha"), head, root, bool(fp.get("dirty"))):
        raise RuntimeError(f"keep-candidate: {exe} fingerprint sha={fp.get('sha', 'absent')} dirty={fp.get('dirty')} "
                           f"is not a clean build of {head[:12]}; previous candidate kept")
    return dict(exe_sha256=digest, exe_commit=fp["sha"])


def sha256_path(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def keep_stock_copy(install, head, candidate=None, root=None):
    """Verify the deployed exe, stage campaign mods on the kept stock copy, move it to the fixed candidate path and record
    it. The previous candidate and its record survive any verification or staging failure."""
    candidate = Path(candidate or CANDIDATE)
    identity = candidate_identity(install, head, root)
    stage_candidate_mods(install)
    if candidate.exists():
        shutil.rmtree(candidate)
    os.replace(install, candidate)
    # head = the commit the gate proved; exe_commit = what the exe embeds (an ancestor after data-only commits).
    (Path(root or ROOT) / CANDIDATE_MARK).write_text(json.dumps(dict(path=str(candidate), head=head, created=time.time(),
                                                                     **identity)))
    return candidate


def execute_window(steps, head, dry_run=False, keep_going=False, farm_determinism=False, keep_candidate=False):
    """Prove a private candidate. Publishing to v0.4 is a separate step (--publish) that runs after the montage has been viewed."""
    deploy = next((s for s in steps if s[0] == "deploy"), None)
    scheduling = dict(farm_determinism=True) if farm_determinism else {}
    if not deploy or deploy[2].get("MC2_GATE_SKIP_REASON"):
        return execute_parallel(steps, head, dry_run, keep_going, **scheduling)
    candidate = CANDIDATE
    def prove(install):
        # capture-before and capture-after both run on the candidate: before with the published exe, after with the new one.
        if not dry_run:
            stage_published(install)
        return execute_parallel(install_plan(steps, install), head, dry_run, keep_going, **scheduling)
    if dry_run:
        return prove(candidate)
    with stock_copy(CANDIDATE_SEED, **(dict(keep=True) if keep_candidate else {})) as install:
        try:
            return prove(install)
        finally:
            if keep_candidate:
                try:
                    kept = keep_stock_copy(install, head)
                except Exception as exc:  # never mask the gate verdict; main() reports this after it
                    shutil.rmtree(install, ignore_errors=True)  # keep=True skipped stock_copy's cleanup
                    CANDIDATE_ERRORS.append(f"{type(exc).__name__}: {exc}")
                else:
                    print(f"[merge_gate] CANDIDATE_KEPT path={kept} head={head[:12]} (walks: sweep_campaign stock|MechCommanderOmnitech <secs> {kept}; "
                          "remove with --drop-candidate)", flush=True)


GREEN_MARK = "build64-ninja/last-green-gate.json"
MAD_LIMIT = 0.05
DECLARED_VIEWS = {}
DECLARED_MAD_BOUND = 0.5


COMPARE_BOOKMARKS = ("mc2_01_saved_205644", "mc2_10_saved_204523")


def raw_mads(output):
    """{bookmark: unrounded MAD} from the summary.json that ab.py names on its '[ab] wrote <dir>' line (the printed table is rounded)."""
    raw = {}
    for line in (output or "").splitlines():
        if line.startswith("[ab] wrote "):
            try:
                rows = json.loads((Path(line[len("[ab] wrote "):].strip()) / "summary.json").read_text()).get("rows", [])
                raw.update({r["bookmark"]: float(r["mad"]) for r in rows})
            except (OSError, ValueError, KeyError, TypeError):
                pass
    return raw


def parse_declared_views(items):
    """['name', 'name=maxMAD'] -> {name: bound}; a bare name gets DECLARED_MAD_BOUND."""
    out = {}
    for item in items or ():
        name, sep, bound = item.partition("=")
        try:
            out[name.strip()] = float(bound) if sep else DECLARED_MAD_BOUND
        except ValueError:
            raise argparse.ArgumentTypeError("--declared-view %r: bound %r is not a number (expected NAME or NAME=maxMAD)" % (item, bound))
    return out


def mad_flags(output, declared=(), expected=()):
    """Bookmarks in ab.py --compare-only output ('name | MAD | ...') whose MAD exceeds MAD_LIMIT, or, for a declared view, its declared bound.
    declared is {name: maxMAD} (a plain set/list means DECLARED_MAD_BOUND each). A declared view still needs a row (when expected) with a numeric MAD under its bound.
    Fail closed: an expected bookmark with no row (crashed capture), a 'missing png' line, or a size mismatch is a flag too."""
    if not isinstance(declared, dict):
        declared = {name: DECLARED_MAD_BOUND for name in declared}
    bad, seen = [], set()
    raw = raw_mads(output)
    for line in (output or "").splitlines():
        if "missing png" in line or "size mismatch" in line:
            bad.append(line.strip().replace("[ab] ", "")[:80])
            continue
        cells = [c.strip() for c in line.split("|")]
        if len(cells) < 2:
            continue
        if cells[0] == "bookmark":
            continue  # table header
        try:
            mad = float(cells[1])
        except ValueError:
            mad = float("nan")
        if math.isnan(mad):
            bad.append(f"{cells[0]}=non-numeric MAD '{cells[1][:20]}'")  # a nan/n-a row is never a pass, declared or not
            continue
        seen.add(cells[0])
        mad = max(mad, raw.get(cells[0], mad))   # the unrounded value can only raise the printed one: a printed 0.05 can be a real 0.0539
        limit = declared.get(cells[0], MAD_LIMIT)
        if mad > limit:
            bad.append(f"{cells[0]}={cells[1]}" + (f" (declared bound {limit})" if cells[0] in declared else ""))
    bad.extend(f"{name}=no compare row" for name in expected if name not in seen)
    return bad


def publish(head, dry_run=False):
    """Deploy the verified build to v0.4, only for a HEAD whose gate was green and complete (run after viewing the montage)."""
    try:
        mark = json.loads((ROOT / GREEN_MARK).read_text())
    except (OSError, ValueError):
        mark = {}
    if mark.get("head") != head and not dry_run:
        print(f"[merge_gate] publish-v0.4: refused; no complete green gate recorded for HEAD {head[:12]} ({GREEN_MARK})")
        return 1
    deploy = next(s for s in plan(1) if s[0] == "deploy")
    rc = execute([("publish-v0.4", deploy[1], {})], head, dry_run)
    if rc == 0 and not dry_run:
        drop_candidate()
    return rc


def execute_parallel(steps, head, dry_run=False, keep_going=False, farm_determinism=False):
    """Overlap isolated mods/determinism with the stock install chain.

    Each harness takes its own FIFO host slot. Never share a batch token between
    workers: that would assign both engines the same affinity and reservation.
    Before/deploy/after remain ordered, and every child is joined before return.
    """
    if dry_run:
        print("Coverage plan: two host slots; isolated mods then determinism/saveload overlap stock captures; contracts/harnesses overlap"
              + ("; non-windowed determinism on farm; mc2_10 remains local" if farm_determinism else ""))
        return execute(steps, head, True)
    if os.environ.get("MC2_HOST_LEASE_BATCH"):
        if farm_determinism:
            print("[merge_gate] farm determinism off: an inherited host batch reservation forces local serial execution", flush=True)
        print("Coverage plan: inherited batch reservation requires serial execution")
        return execute(steps, head)
    pending = []
    completed = {}
    reused = [s for s in steps if s[2].get("MC2_GATE_REUSE_STAGE")]
    steps = [s for s in steps if s not in reused]
    farm = [s for s in steps if farm_determinism and s[0].startswith("determinism-")
            and s[0] != 'determinism-' + WINDOWED_DETERMINISM
            and not s[2].get("MC2_GATE_SKIP_REASON")]
    steps = [s for s in steps if s not in farm]
    box = [s for s in steps if s[0] == "box-gate" and not s[2].get("MC2_GATE_SKIP_REASON")]
    steps = [s for s in steps if s not in box]
    with ThreadPoolExecutor(max_workers=2 + bool(farm) + bool(box)) as pool:
        # The box gate needs no PC game slot: start it now and let it run beside every PC row.
        # A daemon thread, not a pool worker: a hung box must not block the pool's shutdown after the timeout below.
        box_future = Future() if box else None
        if box:
            def box_main():
                try:
                    box_future.set_result(execute(box, head, runner=run_child, completed=completed))
                except BaseException as exc:  # noqa: BLE001 - surfaced through result()
                    box_future.set_exception(exc)
            threading.Thread(target=box_main, name="box-gate", daemon=True).start()
        harness = next((s for s in steps if s[0] == "harnesses" and not s[2].get("MC2_GATE_SKIP_REASON")), None)
        contracts = next((s for s in steps if s[0] == "contracts" and not s[2].get("MC2_GATE_SKIP_REASON")), None)
        harness_future = None
        def timed_child(command, cwd, env):
            started = time.monotonic()
            result = run_child(command, cwd=cwd, env=env)
            result.gate_elapsed = time.monotonic() - started
            return result
        def runner(command, cwd, env):
            nonlocal harness_future
            if contracts and harness and command == contracts[1]:
                harness_future = pool.submit(timed_child, harness[1], cwd,
                    dict(os.environ, **harness[2]))
            if harness_future and command == harness[1]:
                return harness_future.result()
            with install_lock(command):
                return run_child(command, cwd=cwd, env=env)
        # The wrapper deliberately leaves leasing to each harness, rather than
        # execute's legacy exclusive reservation for a serial chain.
        stock = []
        mods, determinism = [], []
        for step in steps:
            (mods if step[0] == "mods" else stock).append(step)
        # Save fixtures share Windows' user save directory, so stay on one worker.
        # Determinism has no writes there and consumes its own named artifacts.
        if any(not s[2].get("MC2_GATE_SKIP_REASON") for s in mods):
            determinism = [s for s in stock if s[0].startswith("determinism-") and not s[2].get("MC2_GATE_SKIP_REASON")]
        if determinism:
            # These checks have no user-save writes and can use the same clone.
            determinism += [s for s in stock if s[0] in ("kit-visibility", "quiet")
                            and not s[2].get("MC2_GATE_SKIP_REASON")]
            stock = [s for s in stock if s not in determinism]
            # Balance the two workers by last-run durations (A2): A = post-deploy stock rows, B = mods; the
            # headless/clone-safe rows go to whichever is lighter. Saves stay on A (install).
            deploy_at = next((i for i, s in enumerate(stock) if s[0] == "deploy"), -1)
            load_a = sum(stage_seconds(s[0]) for s in stock[deploy_at + 1:] if not s[2].get("MC2_GATE_SKIP_REASON")
                         and s[0] not in OFFLINE_STAGES)
            load_b = sum(stage_seconds(s[0]) for s in mods if not s[2].get("MC2_GATE_SKIP_REASON"))
            to_a, determinism = balance_movable(determinism, load_a, load_b)
            stock += to_a
            print(f"[merge_gate] worker balance: A+={[s[0] for s in to_a]} B+={[s[0] for s in determinism]} (A={load_a:.0f}s B={load_b:.0f}s before)", flush=True)
        # Save rows have private MC2_SAVE_DIR dirs, so they no longer wait for the mods battery.
        offline = [s for s in stock if s[0] in OFFLINE_STAGES]
        offline_result = execute(offline, head, runner=runner, completed=completed) if offline else 0
        if offline_result and not keep_going:
            return 1
        stock = [s for s in stock if s not in offline]
        boundary = next((i + 1 for i, s in enumerate(stock) if s[0] == "deploy"), 0)
        prefix = stock[:boundary]
        result = execute(prefix, head, runner=runner, completed=completed) if prefix else offline_result
        if result:
            if not keep_going:
                return result  # do not launch runtime proofs after a failed deploy/offline check
            print("[merge_gate] --keep-going: deploy/pre-stage failure recorded; later stages still run", flush=True)
        if farm:
            remote_envs = {farm_key(c): e for _, c, e in farm}
            def farm_runner(command, cwd, env):
                return farm_stage(command, remote_envs[farm_key(command)], local_env=env)
            pending.append(pool.submit(execute, farm, head, runner=farm_runner, completed=completed))
        if mods:
            copy_context, copy_future = None, None
            command = next((c for _, c, _ in determinism + stock if "--install" in c), None)
            if command:
                install = Path(command[command.index("--install") + 1]).resolve()
                copy_context = stock_copy(install)
                copy_future = pool.submit(copy_context.__enter__)
                # Snapshot before any runtime harness mutates the live install.
                try:
                    copy_future.result()
                except (OSError, RuntimeError) as exc:
                    print(f"[merge_gate] stock-copy FAIL (not waived): {exc}", flush=True)
                    return 1
            # Without a private stock snapshot, keep mod staging away from
            # stock harness writes to the live install's configs.
            if not copy_future:
                result |= execute(mods, head, runner=runner, completed=completed)
            def auxiliary():
                try:
                    result = 0
                    if copy_future:
                        clone = copy_future.result()
                        remapped = []
                        for n, c, e in mods + determinism:
                            command = list(c)
                            for option in ("--install", "--base-install", "--exe"):
                                if option in command:
                                    i = command.index(option) + 1
                                    command[i] = command[i].replace(str(install), str(clone)).replace(install.as_posix(), clone.as_posix())
                            remapped.append((n, command, e))
                        mod_rows = [s for s in remapped if s[0] == 'mods']
                        result |= execute(mod_rows, head, runner=runner, completed=completed)
                        rest = [s for s in remapped if s[0] != 'mods']
                        if rest:
                            result |= execute(rest, head, runner=runner, completed=completed)
                    return result
                except (OSError, RuntimeError) as exc:
                    print(f"[merge_gate] stock-copy/determinism FAIL (not waived): {exc}", flush=True)
                    return 1
                finally:
                    if copy_future:
                        try:
                            copy_future.result()
                        except (OSError, RuntimeError):
                            pass  # stock_copy already cleaned an incomplete copy
                        else:
                            copy_context.__exit__(None, None, None)
            pending.append(pool.submit(auxiliary))
        result |= execute(stock[boundary:], head, runner=runner, completed=completed) if stock[boundary:] else 0
        result |= offline_result
        for future in pending:
            result |= future.result()
        # Reuse can depend on both workers. Join them before reading their verdicts.
        if box_future:
            try:
                box_result = box_future.result(timeout=BOX_GATE_TIMEOUT_S)
            except FutureTimeout:
                # A hung box stage is infrastructure, not a verdict: fall back to the PC rows (the box side's own wall caps should have ended it).
                print(f"[merge_gate] box-gate exceeded {BOX_GATE_TIMEOUT_S}s: BOX_INFRA (timeout)", flush=True)
                completed["box-gate"] = "INFRA (timeout): BOX_INFRA"
                box_result = 3
            reused, fell_back = box_fallback(reused, completed)
            if fell_back:
                print("[merge_gate] BOX UNAVAILABLE MID-RUN (BOX_INFRA): the box produced no verdicts; running the offloaded rows on the PC "
                      "(Windows-full fallback): " + ", ".join(n for n, _, e in reused if n in BOX_OFFLOADED or n in ("contracts", "contracts-linux", "unit-linux")), flush=True)
                FAILED_STAGES[:] = [f for f in FAILED_STAGES if f[0] != "box-gate"]
                box_result = 0
            result |= box_result
        if reused:
            result |= execute(reused, head, runner=runner, completed=completed)
    return result


def validate_before(before_dir, expected_sha):
    """Require saved day/night evidence from the explicitly named pre-deploy exe."""
    if not expected_sha or len(expected_sha) != 64 or any(c not in "0123456789abcdefABCDEF" for c in expected_sha):
        raise ValueError("--before-dir requires --expect-pre-exe-sha (64 hex characters)")
    try:
        manifest = json.loads((Path(before_dir) / "capture_run.json").read_text(encoding="utf-8"))
        actual = manifest["identity"]["exe"]["sha256"]
        if actual.lower() != expected_sha.lower():
            raise ValueError("before evidence exe SHA does not match --expect-pre-exe-sha")
        poses = {pose["bookmark"]: pose for pose in manifest["poses"]}
        for bookmark in ("mc2_01_saved_205644", "mc2_10_saved_204523"):
            pose = poses[bookmark]
            png = Path(pose["primary_png"])
            if not png.is_absolute():
                png = Path(before_dir) / png
            if not pose["all_present"] or not png.is_file():
                raise ValueError(f"before evidence missing capture: {bookmark}")
    except (OSError, KeyError, TypeError, AttributeError) as exc:
        raise ValueError(f"invalid before evidence: {exc}") from exc


def log_lease_holders():
    from smoke_lib import host_lease
    state = host_lease.status()
    seen = set()
    for rec in state["slots"]:
        if rec.get("pid") in seen:
            continue
        seen.add(rec.get("pid"))
        elapsed = max(0, time.time() - float(rec.get("since", time.time())))
        print(f"[merge_gate] lease holder pid={rec.get('pid')} label={rec.get('label')} "
              f"cwd={rec.get('cwd')} held={elapsed:.0f}s", flush=True)
    if not state["slots"]:
        print("[merge_gate] lease slots free", flush=True)


@contextmanager
def game_batch():
    """Reserve the existing host lease once for the serial game proof chain."""
    from smoke_lib import host_lease
    if os.environ.get(host_lease.BATCH_ENV):
        yield {}
        return
    tag = f"merge-gate-{os.getpid()}"
    pid = host_lease.hold(tag, ttl_s=EXCLUSIVE_TTL_S, timeout_s=1200, exclusive=True)
    try:
        yield {host_lease.BATCH_ENV: tag}
    finally:
        host_lease.release_batch(tag)
        import psutil
        try:
            holder = psutil.Process(pid)
            try:
                holder.wait(timeout=5)
            except psutil.TimeoutExpired:
                holder.terminate()   # exactly the holder process this invocation started
                holder.wait(timeout=3)
        except psutil.NoSuchProcess:
            pass


def execute(steps, head, dry_run=False, runner=run_child, completed=None):
    """Run every stage and return nonzero if any stage failed."""
    if not steps:
        raise ValueError("no stages selected; gate requires at least one stage")
    results = []
    completed = {} if completed is None else completed
    failed = False
    for name, command, overrides in steps:
        if not dry_run and STOP_REQUESTED.is_set():
            status = "FAIL: not run, gate interrupted after the previous stage (rerun reuses finished stages)"
            print(f"[merge_gate] {name}: {status}", flush=True)
            results.append((name, status, 0.0))
            completed[name] = status
            failed = True
            continue
        if not dry_run and ACTIVE_STATE and ACTIVE_STATE.reusable(name, command, overrides):
            status = "REUSED: gate-state HEAD/exe/signature match"
            if name == 'mods':
                row = ACTIVE_STATE.data['stages'][name]
                status += f" receipt={ACTIVE_STATE.path} input={row['signature']} previous={row['seconds']:.1f}s"
            row = ACTIVE_STATE.data['stages'][name]
            print(f"REUSED {name} sig={row['signature']} from {row['artefact_dir']}", flush=True)
            ACTIVE_STATE.reused.append(name)
            completed[name] = status
            results.append((name, status, 0.0))
            # Keep original elapsed time and evidence paths in the receipt.
            continue
        reason = overrides.get("MC2_GATE_SKIP_REASON")
        if reason:
            reused = overrides.get("MC2_GATE_REUSE_STAGE")
            status = "SKIP: " + reason
            if reused:
                passed = all(stage_passed(completed.get(source, "")) for source in reused.split(","))
                status = ("REUSED: " if passed or dry_run else "FAIL reuse: source checks failed; ") + reason
                failed |= not (passed or dry_run)
            print(f"[merge_gate] {name}: {status}", flush=True)
            results.append((name, status, 0.0))
            completed[name] = status
            if not dry_run and ACTIVE_STATE:
                if stage_passed(status):
                    ACTIVE_STATE.reused.append(name)
                ACTIVE_STATE.record(name, command, overrides, status, 0.0)
            continue
        status, elapsed, scheduler_wait = "SKIPPED", 0.0, 0.0
        if dry_run:
            print("PLAN " + " ".join(f"{k}={v}" for k, v in overrides.items()) + " " + subprocess.list2cmdline(command))
            status = "DRY-RUN"
        else:
            started = time.monotonic()
            measured = None
            try:
                if git("rev-parse", "HEAD") != head:
                    raise RuntimeError("HEAD changed during gate; rerun verification and gate")
                if name.startswith('determinism-') and '--out' in command:
                    output_index = command.index('--out') + 1
                    previous = Path(command[output_index])
                    if previous.exists():
                        command = list(command)
                        command[output_index] = tempfile.mkdtemp(prefix=previous.name + '-', dir=previous.parent)
                        print(f'[merge_gate] preserved {previous}; fresh output {command[output_index]}', flush=True)
                print(f"[merge_gate] {name}", flush=True)
                if ACTIVE_STATE and name == 'mods':
                    try:
                        ACTIVE_STATE.started_signatures[name] = ACTIVE_STATE.signature(command, overrides)
                    except (OSError, ValueError, KeyError):
                        ACTIVE_STATE.started_signatures[name] = None
                    if '--stage-root' in command:
                        stage_root = Path(command[command.index('--stage-root') + 1])
                        ACTIVE_STATE.started_reports[name] = set(stage_root.glob('*/battery.json'))
                if overrides.get("MC2_GATE_COVERAGE"):
                    print("[merge_gate] " + overrides["MC2_GATE_COVERAGE"], flush=True)
                log_lease_holders()
                if name.startswith(EXCLUSIVE_PREFIXES):
                    # Only stages that need a quiet host hold every slot, and only for that stage.
                    lease_started = time.monotonic()
                    with game_batch() as batch_env:
                        waited = time.monotonic() - lease_started
                        held_started = time.monotonic()
                        result = runner(command, cwd=ROOT, env=dict(os.environ, **overrides, **batch_env, **gate_env(name)))
                    held = time.monotonic() - held_started
                    EXCLUSIVE_SECONDS.append(held)
                    print(f"[merge_gate] exclusive hold {name}: waited {waited:.0f}s, held {held:.0f}s", flush=True)
                else:
                    result = runner(command, cwd=ROOT, env=dict(os.environ, **overrides, **gate_env(name)))
                if result.returncode == 0 and name in ('deploy', 'publish-v0.4') and 'scripts/deploy_payload.py' in command:
                    from deploy_payload import verify_published_identity
                    target = (Path(command[command.index('--allow-new-target') - 1])
                              if '--allow-new-target' in command else Path('A:/Games/mc2-opengl/mc2-win64-v0.4'))
                    verify_published_identity(target, ROOT, head)
                measured = getattr(result, "gate_elapsed", None)
                scheduler_wait = getattr(result, 'gate_scheduler_wait', 0.0)
                status = perf_warn_status(getattr(result, "stdout", "")) if result.returncode == 0 else (
                    infra_status(result.returncode, getattr(result, "stdout", "")) or f"FAIL ({result.returncode})")
                if result.returncode == 0 and name.startswith("compare-captures"):
                    moved = mad_flags(getattr(result, "stdout", ""), DECLARED_VIEWS, COMPARE_BOOKMARKS if name == "compare-captures" else ())
                    if moved:
                        status = "FAIL: MAD > %s or missing/mismatched capture on undeclared view(s) %s" % (MAD_LIMIT, ", ".join(moved))
            except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
                print(f"[merge_gate] {name}: {exc}", file=sys.stderr)
                # The child could not be run or reaped (OS/subprocess error): the host failed, not the product.
                kind = "INFRA: " if isinstance(exc, (OSError, subprocess.SubprocessError)) else "FAIL: "
                status = kind + str(exc).replace("|", "/").replace("\n", " ")
            elapsed = time.monotonic() - started if measured is None else measured
            failed |= not stage_passed(status)
        results.append((name, status, elapsed))
        completed[name] = status
        if not dry_run and ACTIVE_STATE:
            ACTIVE_STATE.record(name, command, overrides, status, elapsed, scheduler_wait)
    print(f"Merge gate HEAD {head} (deploy tree: build64-ninja)")
    print("| Step | Result | Seconds |\n|---|---|---:|")
    for name, status, elapsed in results:
        print(f"| {name} | {status} | {elapsed:.1f} |")
    for name, status, _ in results:
        if not stage_passed(status) and not status.startswith(("DRY-RUN", "SKIP")):
            FAILED_STAGES.append((name, status))
    for name, _, overrides in steps:
        if overrides.get("MC2_GATE_COVERAGE"):
            print(f"Coverage plan {name}: {overrides['MC2_GATE_COVERAGE']}")
    return int(failed)


def select_stages(steps, stages=None, skip=None):
    available = {step[0]: step for step in steps}
    requested = [name for group in stages or [] for name in group.split(',')]
    omitted = {name for group in skip or [] for name in group.split(',')}
    unknown = (set(requested) | omitted) - available.keys()
    if unknown:
        raise ValueError("unknown stages: " + ", ".join(sorted(unknown)) + "; available: " + ", ".join(available))
    if len(requested) != len(set(requested)):
        raise ValueError("duplicate stages")
    overlap = set(requested) & omitted
    if overlap:
        raise ValueError("stages both selected and skipped: " + ", ".join(sorted(overlap)))
    selected = [available[name] for name in requested or available if name not in omitted]
    if not selected or stages == []:
        raise ValueError("no stages selected; gate requires at least one stage")
    return selected


class Tee:
    def __init__(self, console, log):
        self.console, self.log = console, log

    def write(self, text):
        self.console.write(text)
        self.log.write(text)
        self.flush()
        return len(text)

    def flush(self):
        self.console.flush()
        self.log.flush()


def main(argv=None):
    FAILED_STAGES.clear()
    CANDIDATE_ERRORS.clear()
    ap = argparse.ArgumentParser(description=__doc__)
    if "MC2_IN_GATE" in os.environ:
        ap.error("MC2_IN_GATE is set: nested merge gates are forbidden")
    ap.add_argument("--base", help="pre-window commit; absent/stale window record means full coverage")
    ap.add_argument("--preflight", action="store_true", help="offline input checks only; no leases, copies or launches")
    ap.add_argument("--window", type=int, help="merge-window number for known-failure expiry")
    ap.add_argument("--full", action="store_true", help="all domains; orchestrator runs at least daily")
    ap.add_argument("--resume", action="store_true", help="reuse successful stages with matching HEAD and executable SHA")
    ap.add_argument("--clean-output", action="store_true", help="with --preflight --resume, remove only failed determinism output; preserve receipts and passing evidence")
    ap.add_argument("--farm-determinism", action=argparse.BooleanOptionalAction, default=True, help="offer determinism to the farm (default ON; --no-farm-determinism keeps it local); an unreachable farm or a job no live worker claims within the probe window runs on this PC, and each row logs its host")
    ap.add_argument("--box-gate", action=argparse.BooleanOptionalAction, default=True, help="start tools/box_run.py --gate on box GPU 3 at gate start (default ON); it proves determinism mc2_03/16/24 and saveload mc2_01, "
                    "and the PC keeps determinism mc2_10 + saveload mc2_10 as the MSVC canary. --no-box-gate, an unreachable box, or --full keeps every row on the PC")
    ap.add_argument("--no-box", dest="box_gate", action="store_false", help="escape hatch, same as --no-box-gate: every row stays on the PC")
    ap.add_argument("--publish", action="store_true", help="after viewing the montage: deploy the verified build to v0.4 (needs a complete green gate for HEAD)")
    ap.add_argument("--keep-candidate", action="store_true", help="leave the deployed candidate (with MechCommanderOmnitech + mco-compat mods) at E:/mc2-installs/merge-gate-candidate for campaign walks; --publish or --drop-candidate removes it")
    ap.add_argument("--drop-candidate", action="store_true", help="remove the kept candidate install and exit")
    ap.add_argument("--declared-view", action="append", default=[], help="NAME[=maxMAD]: bookmark whose pixels are expected to change; flagged only above maxMAD (default 0.5) and still needs a numeric row")
    ap.add_argument("--install", type=Path, help="explicit lane-owned E:/mc2-installs target for a real rehearsal")
    ap.add_argument("--dry-run", action="store_true", help="print commands only; no builds, deploys, or game launches")
    ap.add_argument("--capture-before-runs", type=int, default=1,
                    help="pre-deploy capture sweeps; extra runs are compared with the first to measure noise")
    ap.add_argument("--expect-head", help="ref that HEAD must resolve to, e.g. the reviewed merge SHA")
    ap.add_argument("--stages", nargs="+", help="base-stage groups in execution order; both art modes run together (space or comma separated)")
    ap.add_argument("--skip", nargs="+", help="stage names to omit (space or comma separated)")
    ap.add_argument("--keep-going", action="store_true",
                    help="run every later stage after a failure; the final verdict still fails and lists all failed stages")
    ap.add_argument("--log", type=Path, help="gate log path (default: timestamped merge-gate proof directory)")
    ap.add_argument("--proofs-dir", type=Path, help="exact proof directory for this invocation")
    ap.add_argument("--before-dir", type=Path, help="reuse a previous capture-before directory")
    ap.add_argument("--expect-pre-exe-sha", help="required executable SHA256 for --before-dir evidence")
    try:
        jobs = Path("A:/Games/mc2-ops/JOBS").read_text().strip()
    except OSError:
        jobs = "2"
    ap.add_argument("-j", type=int, default=int(jobs))
    args = ap.parse_args(argv)
    STOP_REQUESTED.clear()
    if not (args.dry_run or args.preflight) and threading.current_thread() is threading.main_thread():
        install_stage_watchdog()
        from smoke_lib import host_lease
        import atexit
        host_lease.open_gate_window(git('rev-parse', 'HEAD'), str(args.window or ""))   # lane game launchers refuse (GATE QUIET) while this marker is live
        atexit.register(host_lease.close_gate_window)   # backstop; the main() finally below is the normal removal
    if args.j < 1:
        ap.error("-j must be positive")
    global DECLARED_VIEWS
    try:
        DECLARED_VIEWS = parse_declared_views(args.declared_view)
    except argparse.ArgumentTypeError as exc:
        ap.error(str(exc))
    if args.drop_candidate:
        drop_candidate()
        return 0
    if args.publish:
        head_now = git('rev-parse', 'HEAD')
        if args.expect_head and git('rev-parse', args.expect_head + '^{commit}') != head_now:
            print(f"[merge_gate] publish-v0.4: refused; HEAD {head_now[:12]} is not the expected {args.expect_head} (the deploy reads the live build)")
            return 1
        return publish(head_now, args.dry_run)
    if args.clean_output:
        if not (args.preflight and args.resume and args.proofs_dir):
            ap.error('--clean-output requires --preflight --resume --proofs-dir')
        state_path = ROOT / 'build64-ninja/gate-state.json'
        if not state_path.is_file():
            ap.error('--clean-output requires an existing gate journal')
        with GateState(state_path, git('rev-parse', 'HEAD'), ROOT / 'build64-ninja/mc2.exe', args.proofs_dir, [], True, read_only=True) as state:
            clean_failed_outputs(state, args.proofs_dir)
    if args.preflight:
        return preflight(git('rev-parse', 'HEAD'), args.install or Path('E:/mc2-installs/_lean-seed'),
                         args.window, plan(args.j))
    state_path = ROOT / "build64-ninja/gate-state.json"
    resumed_proof = None
    if args.resume and state_path.exists():
        resumed_proof = Path(json.loads(state_path.read_text(encoding="utf-8"))["artefact_dir"])
    proof = args.proofs_dir or resumed_proof or Path("E:/mc2-archive-2026-10/proofs/merge-gate") / time.strftime("%Y%m%d-%H%M%S")
    if not args.dry_run and not args.install:
        (ROOT / GREEN_MARK).unlink(missing_ok=True)  # cleared at start too, so a crashed run cannot publish an older green
    try:
        if args.before_dir:
            validate_before(args.before_dir, args.expect_pre_exe_sha)
        elif args.expect_pre_exe_sha:
            raise ValueError("--expect-pre-exe-sha requires --before-dir")
        planned = (plan(args.j, proof, args.capture_before_runs, args.before_dir) if args.before_dir
                   else plan(args.j, proof, args.capture_before_runs))
        asked = [n for g in args.stages or [] for n in g.split(',')]
        if "box-gate" not in asked:
            if not args.box_gate:
                planned = [s for s in planned if s[0] != "box-gate"]
            elif not (args.dry_run or args.stages) and not box_reachable():
                print("[merge_gate] BOX UNREACHABLE: the box did not answer ssh; WINDOWS-FULL FALLBACK, determinism and saveload run on the PC "
                      "(box-gate stage dropped, no box evidence for this gate)", flush=True)
                planned = [s for s in planned if s[0] != "box-gate"]
        steps = select_stages(planned, args.stages, args.skip)
        omitted = [step for step in planned if step[0] not in {s[0] for s in steps}]
    except ValueError as exc:
        ap.error(str(exc))
    head = git("rev-parse", "HEAD")
    match = re.search(r'merge(\d+)(?:\D|$)', str(proof))
    window_number = args.window or (int(match[1]) if match else None)
    window = {}
    try:
        window = json.loads((ROOT / "build64-ninja/merge-window.json").read_text())
        if window.get("head") != head or window.get("verify") != "OK":
            window = {}
    except (OSError, ValueError):
        pass
    base = args.base or window.get("base")
    if base:
        git("merge-base", "--is-ancestor", base, head)
        paths = [p for p in git("diff", "--name-only", "-z", base, head).split("\0") if p]
        steps = affected_plan(steps, paths, args.full)
        steps = [(n, c, dict(e, MC2_GATE_BASE="" if args.full else base)) for n, c, e in steps]
    else:
        steps = [(n, c, dict(e, MC2_GATE_BASE="")) for n, c, e in affected_plan(steps, [], True)]
    steps = apply_box_route(steps, full=args.full or not base)
    if args.before_dir and any(n == "capture-before-classic" for n, _, _ in steps):
        # Reusing pre-fix evidence must not launch that unsafe executable for the classic arm.
        classic_before = args.before_dir.with_name(args.before_dir.name + "-classic")
        try:
            validate_before(classic_before, args.expect_pre_exe_sha)
        except ValueError as exc:
            ap.error(str(exc))
        steps = [(n, c, e) for n, c, e in steps if n != "capture-before-classic"]
        for n, c, _ in steps:
            if n == "compare-captures-classic":
                c[c.index("--compare-only") + 1] = str(classic_before)
    if window and window_reusable(window, git("rev-parse", "HEAD^{tree}")):
        reused = []
        for name, command, overrides in steps:
            if name == "harnesses" and "--unit" in command:
                command = ["--python-only" if arg == "--unit" else arg for arg in command]
                overrides = dict(overrides, MC2_GATE_COVERAGE="native unit tier reused from successful lane_merge verification at this HEAD; Python contracts run here")
            reused.append((name, command, overrides))
        steps = reused
    if args.install:
        install = args.install.resolve()
        allowed = Path("E:/mc2-installs").resolve()
        if install.parent != allowed or install.name.lower() in ("v0.4", "mc2-win64-v0.4", "lane2", "lane3"):
            ap.error("--install must be a dedicated lane directory directly under E:/mc2-installs")
        steps = install_plan(steps, install)
    for name, command, env in steps:
        # Rows with no output folder of their own write smoke artefacts here, so --resume has evidence to hash.
        if (name == "mods" or name.startswith(("determinism-", "saveload-"))
                or (name not in ("contracts", "harnesses", "pytest", "deploy", "publish-v0.4")
                    and not any(o in command for o in ("--out", "--out-root", "--compare-only")))):
            env["MC2_SMOKE_ARTIFACT_ROOT"] = str(proof / "smokes" / name)
    for name, command, _ in steps:
        if name == 'mods' and window_number:
            command += ['--window', str(window_number)]
    if args.expect_head and git("rev-parse", args.expect_head + "^{commit}") != head:
        ap.error("HEAD does not match --expect-head")
    if not args.dry_run:
        if not args.install and (ROOT.resolve() != Path("A:/Games/mc2-main").resolve()
                or git("branch", "--show-current") != "claude/nifty-mendeleev"):
            ap.error("real merge gates are orchestrator-only in mc2-main on claude/nifty-mendeleev; use --dry-run in lanes")
        if git("status", "--porcelain", "--untracked-files=no"):
            ap.error("commit tracked changes before running the merge gate")
        if preflight(head, args.install or Path('E:/mc2-installs/_lean-seed'), window_number, steps):
            return 1
    else:
        print('PLAN preflight (offline inputs, clean game/viewer HEAD, fresh save policy, known failures)')
    for n, c, e in omitted:
        steps.append((n, c, dict(e, MC2_GATE_SKIP_REASON="operator stage filter; partial gate")))
    log_path = args.log or proof / "gate.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    # Acquire before opening the log: a refused duplicate must not truncate it.
    try:
        journal = gate_journal(state_path, head, ROOT / "build64-ninja/mc2.exe", proof, steps, args.resume, args.dry_run)
        journal.__enter__()
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"[merge_gate] REFUSED: {exc}")
        return 1
    try:
        with log_path.open("a" if args.resume else "w", encoding="utf-8") as log:
            with redirect_stdout(Tee(sys.stdout, log)), redirect_stderr(Tee(sys.stderr, log)):
                print(f"[merge_gate] log: {log_path}", flush=True)
                started = window.get("started", time.time())
                scheduling = dict(farm_determinism=True) if args.farm_determinism else {}
                execute_gate = execute_parallel if args.install else execute_window
                if args.keep_candidate and not args.install:
                    scheduling["keep_candidate"] = True
                result = execute_gate(steps, head, args.dry_run, keep_going=args.keep_going, **scheduling)
                elapsed = time.time() - started
                # Verdict rc: 0 every row green, 1 a red row, 2 partial gate (skipped rows), 3 only infra reds. The clock is information only.
                skipped = [n for n, _, e in steps if e.get("MC2_GATE_SKIP_REASON") and not e.get("MC2_GATE_REUSE_STAGE")]
                # INFRA: every red row failed on the host (INFRA status), so no product regression is claimed.
                infra_only = bool(result and FAILED_STAGES and all(s.startswith("INFRA") for _, s in FAILED_STAGES))
                verdict = "INFRA" if infra_only else "FAIL" if result else ("PARTIAL" if skipped else "PASS")
                print(f"[merge_gate] exclusive host hold total={sum(EXCLUSIVE_SECONDS) / 60:.1f} min over {len(EXCLUSIVE_SECONDS)} stages (limit 20 min)")
                reused = ",".join(ACTIVE_STATE.reused) if ACTIVE_STATE else ""
                print(f"[merge_gate] clock: {elapsed / 60:.1f} min {'since lane_merge start' if window else 'gate only'} (window box 120 min, cap 180; info only) verdict={verdict} REUSED={reused or 'none'}"
                      + (f" SKIPPED={','.join(skipped)}" if skipped else ""))
                for err in CANDIDATE_ERRORS:
                    print(f"[merge_gate] CANDIDATE_ERROR (after verdict={verdict}; the gate result above is unaffected, no candidate kept): {err}")
                if FAILED_STAGES:
                    print("[merge_gate] failed stages: " + ", ".join(f"{name} ({status})" for name, status in FAILED_STAGES))
                if args.full and result == 0 and not args.dry_run:
                    (ROOT / "build64-ninja/last-full-gate.json").write_text(json.dumps(dict(head=head, finished=time.time())))
                if verdict != "PASS" and not args.dry_run and not args.install:
                    (ROOT / GREEN_MARK).unlink(missing_ok=True)  # a red or partial rerun at the same HEAD must not leave an older green
                if verdict == "PASS" and not args.dry_run and not args.install:
                    (ROOT / GREEN_MARK).write_text(json.dumps(dict(head=head, finished=time.time(),
                                                                        candidate=str(CANDIDATE) if args.keep_candidate and not CANDIDATE_ERRORS else None)))
                    print("[merge_gate] gate green and complete: view the day and night montages, then `py -3 scripts/merge_gate.py --publish` (v0.4 is NOT published yet)")
                return result if args.dry_run else {"PASS": 0, "FAIL": 1, "PARTIAL": 2, "INFRA": 3}[verdict]
    finally:
        journal.__exit__(None, None, None)
        if not (args.dry_run or args.preflight):
            from smoke_lib import host_lease
            host_lease.close_gate_window()


if __name__ == "__main__":
    sys.exit(main())
