#!/usr/bin/env python3
"""Orchestrator merge-window gate; --dry-run prints the plan without any launches.

Run after lane_merge verification. Deploy consumes that build64-ninja tree;
this gate never builds the engine a second time. Real runs require mc2-main
on claude/nifty-mendeleev, or an explicit lane-owned install for rehearsal.
Use --base for change selection, --full for the daily full sweep.
"""
from __future__ import annotations

import argparse
import json
from contextlib import contextmanager, redirect_stdout, redirect_stderr
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
from concurrent.futures import ThreadPoolExecutor

import console_child

ROOT = Path(__file__).resolve().parents[1]
#: The one determinism row that keeps a window and a GPU (a guard for the windowed path); the others run headless.
WINDOWED_DETERMINISM = "mc2_10"
sys.path.insert(0, str(ROOT / "tools"))
from lane_merge import git_bash, run_logged  # noqa: E402


# Only the visual captures need a quiet host; everything else shares the two game slots.
# The gate takes and drops the exclusive hold per stage, so a window never holds it for more than a stage (TTL 1200 s).
EXCLUSIVE_PREFIXES = ("capture-before", "capture-after")
EXCLUSIVE_TTL_S = 1200
EXCLUSIVE_SECONDS = []
FAILED_STAGES = []
ACTIVE_STATE = None


def stage_passed(status):
    return status == "PASS" or status.startswith("REUSED:")


def preflight(head, install, window, steps, seed=Path("E:/mc2-installs/_lean-seed")):
    """Offline, fail before any lease, copy, build or deployment."""
    from exe_fingerprint import extract, sha_agrees
    from mod_smoke import load_known_failures
    import saveload_roundtrip as saves

    started = time.monotonic()
    errors = []
    rebuild = "py -3 scripts/ninja_build.py mc2 mc2_asset_viewer -j " + Path("A:/Games/mc2-ops/JOBS").read_text().strip()
    for name in ("mc2.exe", "mc2_asset_viewer.exe"):
        fp = extract(ROOT / "build64-ninja" / name)
        if not fp.get('present') or fp.get('dirty') or not sha_agrees(fp.get('sha'), head):
            errors.append(f"{name} fingerprint != clean HEAD {head}; fix: {rebuild}")
    bookmarks = set()
    for name, command, env in steps:
        if name.startswith('capture-') and '--bookmarks' in command and not env.get('MC2_GATE_SKIP_REASON'):
            bookmarks.update(command[command.index('--bookmarks') + 1].split(','))
    for target in dict.fromkeys((Path(install), Path(seed))):
        for bookmark in sorted(bookmarks):
            path = target / 'saved_bookmarks' / (bookmark + '.json')
            try:
                doc = json.loads(path.read_text(encoding='utf-8'))
                if not doc.get('mission') or len(doc.get('bookmarks', [])) < 2:
                    raise ValueError('requires mission and at least two camera poses')
            except (OSError, ValueError, TypeError) as exc:
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
    def __init__(self, path, head, exe, proof, steps, resume=False):
        self.path, self.head, self.exe = Path(path), head, Path(exe)
        self.mutex = threading.Lock()
        self.lock = None
        self.data = dict(pid=os.getpid(), head=head, artefact_dir=str(proof),
                         order=[n for n, _, _ in steps], stages={})
        self.resume = resume
        self.reused = []
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
            if self.resume and self.path.exists():
                previous = json.loads(self.path.read_text(encoding="utf-8"))
                self.data["stages"] = previous.get("stages", {})
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
        return hashlib.sha256(json.dumps([command, overrides], sort_keys=True).encode()).hexdigest()

    def reusable(self, name, command, overrides):
        row = self.data["stages"].get(name, {})
        sha = self.exe_sha()
        return (self.resume and sha is not None and row.get("head") == self.head
                and row.get("exe_sha") == sha and stage_passed(row.get("status", ""))
                and row.get("signature") == self.signature(*self.specs.get(name, (command, overrides))))

    def record(self, name, command, overrides, status, seconds):
        with self.mutex:
            artefact = overrides.get("MC2_SMOKE_ARTIFACT_ROOT", self.data["artefact_dir"])
            for option in ("--out", "--out-root"):
                if option in command:
                    artefact = command[command.index(option) + 1]
            self.data["stages"][name] = dict(
                head=self.head, exe_sha=self.exe_sha(), status=status, seconds=seconds,
                artefact_dir=artefact,
                signature=self.signature(*self.specs.get(name, (command, overrides))))
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
    # spawns infantry after tick 2404: its audit must cover that lazy shape load.
    for mission in ("mc2_03", "mc2_10", "mc2_24"):
        steps.append(("determinism-" + mission,
                      [py, "tools/sim_determinism.py", "check", mission, "--install", install, "--out", str(proof / ("determinism-" + mission)),
                       # mc2_10 reaches only ~650 of the 800 sim frames in 20 s under load; 2x gets there in half the host time (both runs of a pair share the speed)
                       *(["--sim-speed", "2"] if mission == WINDOWED_DETERMINISM else
                         (["--sim-speed", "4", "--ticks", "2600"] if mission == "mc2_24" else ["--headless-both"]))], {}))
    for mission in ("mc2_01", "mc2_03", "mc2_10", "mc2_16", "mc2_24"):
        steps.append(("saveload-" + mission,
                      [py, "tools/saveload_roundtrip.py", "check", "--mission", mission,
                       "--install", install, "--mask", "tools/save_noise_mask.json", "--first-save-only", "--out", str(proof / ("saveload-" + mission))], {}))
    # Staging has one cache writer; two isolated smoke children lease their own slots.
    steps.append(("mods", [py, "tools/mod_smoke.py", "--battery", "--workers", "2", "--exe", install + "/mc2.exe", "--build-dir", "build64-ninja"], {}))
    steps.append(("validation",
                  [py, "scripts/run_smoke.py", "--lite", "--keep-logs", "--deploy", "0.4"],
                  {"MC2_VULKAN_VALIDATION": "1"}))
    return steps


# Unknown inputs deliberately retain every stage. Only proven independent domains
# can waive game proofs; generators/build configuration retain full coverage.
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
    offline = {"contracts", "harnesses", "pytest"}
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
            command += ["--base-install", str(install), "--stage-root", str(install.parent / (install.name + "-mods-stage")), "--duration", "15"]
        result.append((name, command, env))
    return result


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True,
                                   **console_child.child_kwargs(redirected=True)).strip()


def run_child(command, cwd, env):
    return run_logged(cwd, command, env=env)


@contextmanager
def stock_copy(install, parent=None):
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


def farm_stage(command, overrides, runner=None):
    """Submit one unchanged determinism check, wait, and fetch its evidence."""
    runner = run_child if runner is None else runner
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
    submitted = runner(host + ["submit", str(job), "--exe", str(ROOT / "build64-ninja/mc2.exe")], cwd=ROOT, env=os.environ.copy())
    if submitted.returncode:
        return submitted
    ids = [line.strip() for line in submitted.stdout.splitlines() if re.fullmatch(r"[0-9a-f]{32}", line.strip())]
    if len(ids) != 1:
        raise RuntimeError("farm submit returned no unique job ID")
    job_id = ids[0]
    waited = runner(host + ["wait", job_id, "--timeout", "2160", "--queue-timeout", "1200"], cwd=ROOT, env=os.environ.copy())
    if 'queue_timeout:' in waited.stdout:
        print('[merge_gate] farm queue capped at 20 min; repeating locally', flush=True)
        return runner(command, cwd=ROOT, env=dict(os.environ, **overrides))
    if "job remains owned" in waited.stdout:
        return waited  # never delete the payload of a still-running farm job
    fetched = runner(host + ["fetch", job_id, str(out)], cwd=ROOT, env=os.environ.copy())
    return subprocess.CompletedProcess(command, waited.returncode or fetched.returncode, waited.stdout + fetched.stdout)


def choose_farm(runner=run_child):
    """Compare one queue snapshot with a conservative host-slot wait estimate."""
    from smoke_lib import host_lease
    try:
        state = host_lease.status()
        slots = host_lease.slot_count()
        occupied = len(state['slots'])
        tickets = json.loads(host_lease.lease_path().with_suffix('.queue.json').read_text()).get('tickets', []) if host_lease.lease_path().with_suffix('.queue.json').exists() else []
        host_minutes = 15 * ((len(tickets) // slots) + 1) if occupied >= slots or tickets else 0
        result = runner([sys.executable, 'tools/farm/host.py', 'queue'], cwd=ROOT, env=os.environ.copy())
        depth = int(json.loads(result.stdout)['depth']) if not result.returncode else -1
        selected = depth >= 0 and depth * 15 < min(host_minutes, 20)
        print(f'[merge_gate] farm depth={depth} estimated={depth * 15}min host-slot estimate={host_minutes}min selected={selected}', flush=True)
        return selected
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        print(f'[merge_gate] farm queue unavailable; local determinism: {exc}', flush=True)
        return False


def execute_window(steps, head, dry_run=False, keep_going=False, farm_determinism=False):
    """Prove a private candidate before publishing the verified build to v0.4."""
    deploy = next((s for s in steps if s[0] == "deploy"), None)
    tier1 = next((s for s in steps if s[0] == "tier1" and not s[2].get("MC2_GATE_SKIP_REASON")), None)
    scheduling = dict(farm_determinism=True) if farm_determinism else {}
    if not deploy or deploy[2].get("MC2_GATE_SKIP_REASON"):
        return execute_parallel(steps, head, dry_run, keep_going, **scheduling)
    candidate = Path("E:/mc2-installs/merge-gate-candidate")
    def prove(install):
        staged = install_plan(steps, install)
        # Before evidence belongs to the currently published executable.
        staged = [original if original[0].startswith("capture-before") else private
                  for original, private in zip(steps, staged)]
        result = execute_parallel(staged, head, dry_run, keep_going, **scheduling)
        if result or not tier1:
            print("[merge_gate] publish-v0.4: withheld (candidate failed or tier1 not run)", flush=True)
            return result
        return execute([("publish-v0.4", deploy[1], {})], head, dry_run)
    if dry_run:
        return prove(candidate)
    with stock_copy(Path("E:/mc2-installs/_lean-seed")) as install:
        return prove(install)


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
            raise ValueError("farm scheduling requires no inherited host batch reservation")
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
    with ThreadPoolExecutor(max_workers=3 if farm else 2) as pool:
        harness = next((s for s in steps if s[0] == "harnesses" and not s[2].get("MC2_GATE_SKIP_REASON")), None)
        contracts = next((s for s in steps if s[0] == "contracts" and not s[2].get("MC2_GATE_SKIP_REASON")), None)
        harness_future = None
        mods_done = threading.Event()
        save_lock = threading.Lock()
        def timed_child(command, cwd, env):
            started = time.monotonic()
            result = run_child(command, cwd=cwd, env=env)
            result.gate_elapsed = time.monotonic() - started
            return result
        def runner(command, cwd, env):
            nonlocal harness_future
            if contracts and harness and command == contracts[1]:
                harness_future = pool.submit(timed_child, harness[1], cwd,
                    dict(os.environ, **harness[2], MC2_IN_GATE="1"))
            if harness_future and command == harness[1]:
                return harness_future.result()
            if any(Path(arg).name == 'saveload_roundtrip.py' for arg in command):
                mods_done.wait()
                # Both workers still share the user's quicksave filenames.
                with save_lock:
                    return run_child(command, cwd=cwd, env=env)
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
        # A mod cold-load starves the save fixture of frames (W83 false reds), so
        # saveload rows never share a window with the mods battery: they join the
        # auxiliary worker, which runs mods first. (Without a private clone mods
        # runs serially ahead of the stock chain, so the order still holds.)
        if any(not s[2].get("MC2_GATE_SKIP_REASON") for s in mods):
            saves = [s for s in stock if s[0].startswith("saveload-")]
            # The stock half follows tier1, and waits for mods in runner.
            shared = saves[1::2]
            stock = [s for s in stock if s not in saves or s in shared]
            determinism += [s for s in saves if s not in shared]
        else:
            mods_done.set()
        offline = [s for s in stock if s[0] in ("contracts", "harnesses", "pytest")]
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
            remote_envs = {tuple(c): e for _, c, e in farm}
            def farm_runner(command, cwd, env):
                return farm_stage(command, remote_envs[tuple(command)])
            pending.append(pool.submit(execute, farm, head, runner=farm_runner, completed=completed))
        if mods:
            copy_context, copy_future = None, None
            if determinism:
                command = next(c for _, c, _ in determinism if "--install" in c)
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
                mods_done.set()
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
                        mods_done.set()
                        rest = [s for s in remapped if s[0] != 'mods']
                        if rest:
                            result |= execute(rest, head, runner=runner, completed=completed)
                    return result
                except (OSError, RuntimeError) as exc:
                    print(f"[merge_gate] stock-copy/determinism FAIL (not waived): {exc}", flush=True)
                    return 1
                finally:
                    mods_done.set()
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
        if not dry_run and ACTIVE_STATE and ACTIVE_STATE.reusable(name, command, overrides):
            status = "REUSED: gate-state HEAD/exe/signature match"
            ACTIVE_STATE.reused.append(name)
            completed[name] = status
            results.append((name, status, 0.0))
            ACTIVE_STATE.record(name, command, overrides, status, 0.0)
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
        status, elapsed = "SKIPPED", 0.0
        if dry_run:
            print("PLAN " + " ".join(f"{k}={v}" for k, v in overrides.items()) + " " + subprocess.list2cmdline(command))
            status = "DRY-RUN"
        else:
            started = time.monotonic()
            measured = None
            try:
                if git("rev-parse", "HEAD") != head:
                    raise RuntimeError("HEAD changed during gate; rerun verification and gate")
                print(f"[merge_gate] {name}", flush=True)
                if overrides.get("MC2_GATE_COVERAGE"):
                    print("[merge_gate] " + overrides["MC2_GATE_COVERAGE"], flush=True)
                log_lease_holders()
                if name.startswith(EXCLUSIVE_PREFIXES):
                    # Only stages that need a quiet host hold every slot, and only for that stage.
                    lease_started = time.monotonic()
                    with game_batch() as batch_env:
                        waited = time.monotonic() - lease_started
                        held_started = time.monotonic()
                        result = runner(command, cwd=ROOT, env=dict(os.environ, **overrides, **batch_env, MC2_IN_GATE="1"))
                    held = time.monotonic() - held_started
                    EXCLUSIVE_SECONDS.append(held)
                    print(f"[merge_gate] exclusive hold {name}: waited {waited:.0f}s, held {held:.0f}s", flush=True)
                else:
                    result = runner(command, cwd=ROOT, env=dict(os.environ, **overrides, MC2_IN_GATE="1"))
                measured = getattr(result, "gate_elapsed", None)
                status = "PASS" if result.returncode == 0 else f"FAIL ({result.returncode})"
            except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                print(f"[merge_gate] {name}: {exc}", file=sys.stderr)
                status = "FAIL: " + str(exc).replace("|", "/").replace("\n", " ")
            elapsed = time.monotonic() - started if measured is None else measured
            failed |= status != "PASS"
        results.append((name, status, elapsed))
        completed[name] = status
        if not dry_run and ACTIVE_STATE:
            ACTIVE_STATE.record(name, command, overrides, status, elapsed)
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
    ap = argparse.ArgumentParser(description=__doc__)
    if "MC2_IN_GATE" in os.environ:
        ap.error("MC2_IN_GATE is set: nested merge gates are forbidden")
    ap.add_argument("--base", help="pre-window commit; absent/stale window record means full coverage")
    ap.add_argument("--preflight", action="store_true", help="offline input checks only; no leases, copies or launches")
    ap.add_argument("--window", type=int, help="merge-window number for known-failure expiry")
    ap.add_argument("--full", action="store_true", help="all domains; orchestrator runs at least daily")
    ap.add_argument("--resume", action="store_true", help="reuse successful stages with matching HEAD and executable SHA")
    ap.add_argument("--farm-determinism", action="store_true", help="force non-windowed determinism to farm (default: choose by queue versus host wait)")
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
    if args.j < 1:
        ap.error("-j must be positive")
    if args.preflight:
        return preflight(git('rev-parse', 'HEAD'), args.install or Path('E:/mc2-installs/_lean-seed'),
                         args.window, plan(args.j))
    state_path = ROOT / "build64-ninja/gate-state.json"
    resumed_proof = None
    if args.resume and state_path.exists():
        resumed_proof = Path(json.loads(state_path.read_text(encoding="utf-8"))["artefact_dir"])
    proof = args.proofs_dir or resumed_proof or Path("E:/mc2-archive-2026-10/proofs/merge-gate") / time.strftime("%Y%m%d-%H%M%S")
    try:
        if args.before_dir:
            validate_before(args.before_dir, args.expect_pre_exe_sha)
        elif args.expect_pre_exe_sha:
            raise ValueError("--expect-pre-exe-sha requires --before-dir")
        planned = (plan(args.j, proof, args.capture_before_runs, args.before_dir) if args.before_dir
                   else plan(args.j, proof, args.capture_before_runs))
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
    if window and time.time() - window.get("started", 0) <= 1200:
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
    for name, _, env in steps:
        if name == "mods" or name.startswith(("determinism-", "saveload-")):
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
                use_farm = args.farm_determinism or (not args.dry_run and not os.environ.get('MC2_HOST_LEASE_BATCH') and choose_farm())
                scheduling = dict(farm_determinism=True) if use_farm else {}
                execute_gate = execute_parallel if args.install else execute_window
                result = execute_gate(steps, head, args.dry_run, keep_going=args.keep_going, **scheduling)
                elapsed = time.time() - started
                measured_window = bool(window)
                verdict = ("FAIL" if result else
                           "PASS" if elapsed <= 1200 and measured_window and not omitted else "NEEDS_REVIEW")
                print(f"[merge_gate] exclusive host hold total={sum(EXCLUSIVE_SECONDS) / 60:.1f} min over {len(EXCLUSIVE_SECONDS)} stages (limit 20 min)")
                reused = ",".join(ACTIVE_STATE.reused) if ACTIVE_STATE else ""
                print(f"[merge_gate] total={elapsed:.1f}s budget=1200s scope={'lane_merge through gate' if measured_window else 'gate only; no verified lane_merge clock'} verdict={verdict} REUSED={reused or 'none'}")
                if FAILED_STAGES:
                    print("[merge_gate] failed stages: " + ", ".join(f"{name} ({status})" for name, status in FAILED_STAGES))
                if args.full and result == 0 and not args.dry_run:
                    (ROOT / "build64-ninja/last-full-gate.json").write_text(json.dumps(dict(head=head, finished=time.time())))
                return result if args.dry_run else (0 if verdict == "PASS" else 1)
    finally:
        journal.__exit__(None, None, None)


if __name__ == "__main__":
    sys.exit(main())
