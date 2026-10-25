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

import console_child

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from lane_merge import git_bash, run_logged  # noqa: E402


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
        ("pytest", [py, "-B", "-m", "pytest", "scripts/tests", "-q", "-p", "no:cacheprovider"], {}),
        ("tier1", [py, "scripts/run_smoke.py", "--tier", "tier1", "--keep-logs", "--deploy", "0.4"],
         {"MC2_DEBUG_STATE_DUMP": "1", "MC2_DIAG_TAGS": "CONFIG,BUILD,DEVICE"}),
        ("mods", [py, "tools/mod_smoke.py", "--battery", "--exe", install + "/mc2.exe", "--build-dir", "build64-ninja"], {}),
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
    for mission in ("mc2_03", "mc2_10", "mc2_24"):
        steps.append(("determinism-" + mission,
                      [py, "tools/sim_determinism.py", "check", mission, "--install", install, "--out", str(proof / ("determinism-" + mission))], {}))
    for mission in ("mc2_01", "mc2_03", "mc2_10", "mc2_16", "mc2_24"):
        steps.append(("saveload-" + mission,
                      [py, "tools/saveload_roundtrip.py", "check", "--mission", mission,
                       "--install", install, "--mask", "tools/save_noise_mask.json", "--first-save-only", "--out", str(proof / ("saveload-" + mission))], {}))
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
    game = bool(domains & {"engine", "shaders", "art"})
    sim = "engine" in domains
    selected = []
    names = {step[0] for step in steps}
    shared_smokes = {"determinism-" + m for m in ("mc2_03", "mc2_10", "mc2_24")}
    batch_tier = (sim or full) and shared_smokes <= names
    for name, cmd, env in steps:
        env = dict(env)
        cmd = list(cmd)
        reason = None
        if not full:
            if (name.startswith("determinism-") and not sim) or (name.startswith("saveload-") and not (sim or "art" in domains)):
                reason = "diff has no engine/simulation inputs (" + ", ".join(sorted(domains)) + ")"
            elif name not in ("contracts", "pytest", "harnesses") and not game:
                reason = "docs/tooling-only diff has no runtime inputs"
            elif name == "harnesses" and not sim:
                reason = "diff has no C++ harness inputs"
        if name == "pytest" and "contracts" in names:
            env["MC2_GATE_REUSE_STAGE"] = "contracts"
            reason = "scripts_tests contract row owns these modules; no duplicate execution"
        if name == "validation" and (game or full) and "tier1" in names:
            env["MC2_GATE_REUSE_STAGE"] = "tier1,determinism-mc2_10" if batch_tier else "tier1"
            reason = "tier1 and shared determinism smokes grade Vulkan validation errors/layer activation"
        if name == "tier1":
            env["MC2_VULKAN_VALIDATION"] = "1"
            if batch_tier:
                cmd += ["--mission", "mc2_01", "--mission", "mc2_17"]
                env["MC2_GATE_COVERAGE"] = "tier1 mc2_03/10/24 assigned to strict determinism smokes; this stage runs mc2_01/17"
        if name in shared_smokes and batch_tier:
            env.update(MC2_VULKAN_VALIDATION="1", MC2_MOVER_COHERENCE_AUDIT="1",
                       MC2_DEBUG_STATE_DUMP="1", MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE")
        if reason:
            env["MC2_GATE_SKIP_REASON"] = reason
        selected.append((name, cmd, env))
        if name.startswith("saveload-") and (sim or "art" in domains or full):
            env["MC2_OPTIONS_OVERRIDE"] = "RemasteredBuildings=TRUE"
            classic = list(cmd)
            if "--out" in classic:
                classic[classic.index("--out") + 1] += "-classic"
            selected.append((name + "-classic", classic,
                             dict(env, MC2_OPTIONS_OVERRIDE="RemasteredBuildings=FALSE")))
    if game or full:
        # Material mode can affect either renderer path. Preserve day/night A/B in both.
        clones = []
        for name, cmd, env in selected:
            if name not in ("capture-before", "capture-after", "compare-captures"):
                continue
            classic = [arg + "-classic" if Path(arg).name in ("before", "after", "ab") else arg for arg in cmd]
            if name.startswith("capture-"):
                classic += ["--env-declare", "MC2_OPTIONS_OVERRIDE=RemasteredBuildings=FALSE"]
            clones.append((name + "-classic", classic, dict(env)))
        if "capture-before" not in names and "deploy" in names and "capture-after" in names:
            after = next(c for n, c, _ in selected if n == "capture-after")
            before_cmd = list(after)
            before_path = str(Path(after[after.index("--out") + 1]).parent / "before-classic")
            before_cmd[before_cmd.index("--out") + 1] = before_path
            before_cmd += ["--env-declare", "MC2_OPTIONS_OVERRIDE=RemasteredBuildings=FALSE"]
            selected.insert(next(i for i, step in enumerate(selected) if step[0] == "deploy"),
                            ("capture-before-classic", before_cmd, {}))
            for i, (n, c, e) in enumerate(clones):
                if n == "compare-captures-classic":
                    c[c.index("--compare-only") + 1] = before_path
        for clone in clones:
            original = clone[0].removesuffix("-classic")
            index = next(i for i, step in enumerate(selected) if step[0] == original)
            selected.insert(index + 1, clone)
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
            command += ["--base-install", str(install), "--stage-root", str(install / "mod_smoke-stage"), "--duration", "10"]
        result.append((name, command, env))
    return result


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True,
                                   **console_child.child_kwargs(redirected=True)).strip()


def run_child(command, cwd, env):
    return run_logged(cwd, command, env=env)


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
    pid = host_lease.hold(tag, ttl_s=1800, timeout_s=1200, exclusive=True)
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


def execute(steps, head, dry_run=False, runner=run_child):
    """Run every stage and return nonzero if any stage failed."""
    if not steps:
        raise ValueError("no stages selected; gate requires at least one stage")
    results = []
    failed = False
    batch_context, batch_env, lease_error = None, {}, None
    try:
        for name, command, overrides in steps:
            reason = overrides.get("MC2_GATE_SKIP_REASON")
            if reason:
                reused = overrides.get("MC2_GATE_REUSE_STAGE")
                status = "SKIP: " + reason
                if reused:
                    passed = all(next((r[1] == "PASS" for r in results if r[0] == source), False) for source in reused.split(","))
                    status = ("REUSED: " if passed or dry_run else "FAIL reuse: source checks failed; ") + reason
                    failed |= not (passed or dry_run)
                print(f"[merge_gate] {name}: {status}", flush=True)
                results.append((name, status, 0.0))
                continue
            status, elapsed = "SKIPPED", 0.0
            if dry_run:
                print("PLAN " + " ".join(f"{k}={v}" for k, v in overrides.items()) + " " + subprocess.list2cmdline(command))
                status = "DRY-RUN"
            else:
                started = time.monotonic()
                try:
                    if git("rev-parse", "HEAD") != head:
                        raise RuntimeError("HEAD changed during gate; rerun verification and gate")
                    print(f"[merge_gate] {name}", flush=True)
                    if overrides.get("MC2_GATE_COVERAGE"):
                        print("[merge_gate] " + overrides["MC2_GATE_COVERAGE"], flush=True)
                    log_lease_holders()
                    runtime = name.startswith(("capture-", "determinism-", "saveload-")) or name in ("deploy", "tier1", "mods", "quiet", "validation")
                    if runtime and runner is run_child and batch_context is None:
                        if lease_error:
                            raise RuntimeError("game batch unavailable: " + lease_error)
                        candidate = game_batch()
                        lease_started = time.monotonic()
                        try:
                            batch_env = candidate.__enter__()
                        except (OSError, RuntimeError) as exc:
                            lease_error = str(exc)
                            raise
                        batch_context = candidate
                        results.append(("game-lease", "PASS", time.monotonic() - lease_started))
                        started = time.monotonic()
                    result = runner(command, cwd=ROOT, env=dict(os.environ, **overrides, **batch_env, MC2_IN_GATE="1"))
                    status = "PASS" if result.returncode == 0 else f"FAIL ({result.returncode})"
                except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                    print(f"[merge_gate] {name}: {exc}", file=sys.stderr)
                    status = "FAIL: host game lease unavailable (not run)" if lease_error else "FAIL: " + str(exc).replace("|", "/").replace("\n", " ")
                elapsed = time.monotonic() - started
                failed |= status != "PASS"
            results.append((name, status, elapsed))
    finally:
        if batch_context is not None:
            batch_context.__exit__(*sys.exc_info())
    print(f"Merge gate HEAD {head} (deploy tree: build64-ninja)")
    print("| Step | Result | Seconds |\n|---|---|---:|")
    for name, status, elapsed in results:
        print(f"| {name} | {status} | {elapsed:.1f} |")
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
    ap = argparse.ArgumentParser(description=__doc__)
    if "MC2_IN_GATE" in os.environ:
        ap.error("MC2_IN_GATE is set: nested merge gates are forbidden")
    ap.add_argument("--base", help="pre-window commit; absent/stale window record means full coverage")
    ap.add_argument("--full", action="store_true", help="all domains; orchestrator runs at least daily")
    ap.add_argument("--install", type=Path, help="explicit lane-owned E:/mc2-installs target for a real rehearsal")
    ap.add_argument("--dry-run", action="store_true", help="print commands only; no builds, deploys, or game launches")
    ap.add_argument("--capture-before-runs", type=int, default=1,
                    help="pre-deploy capture sweeps; extra runs are compared with the first to measure noise")
    ap.add_argument("--expect-head", help="ref that HEAD must resolve to, e.g. the reviewed merge SHA")
    ap.add_argument("--stages", nargs="+", help="base-stage groups in execution order; both art modes run together (space or comma separated)")
    ap.add_argument("--skip", nargs="+", help="stage names to omit (space or comma separated)")
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
    proof = args.proofs_dir or Path("E:/mc2-archive-2026-10/proofs/merge-gate") / time.strftime("%Y%m%d-%H%M%S")
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
    if args.expect_head and git("rev-parse", args.expect_head + "^{commit}") != head:
        ap.error("HEAD does not match --expect-head")
    if not args.dry_run:
        if not args.install and (ROOT.resolve() != Path("A:/Games/mc2-main").resolve()
                or git("branch", "--show-current") != "claude/nifty-mendeleev"):
            ap.error("real merge gates are orchestrator-only in mc2-main on claude/nifty-mendeleev; use --dry-run in lanes")
        if git("status", "--porcelain", "--untracked-files=no"):
            ap.error("commit tracked changes before running the merge gate")
    for n, c, e in omitted:
        steps.append((n, c, dict(e, MC2_GATE_SKIP_REASON="operator stage filter; partial gate")))
    log_path = args.log or proof / "gate.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as log:
        with redirect_stdout(Tee(sys.stdout, log)), redirect_stderr(Tee(sys.stderr, log)):
            print(f"[merge_gate] log: {log_path}", flush=True)
            started = window.get("started", time.time())
            result = execute(steps, head, args.dry_run)
            elapsed = time.time() - started
            measured_window = bool(window)
            verdict = "PASS" if result == 0 and elapsed <= 1200 and measured_window and not omitted else "NEEDS_REVIEW"
            print(f"[merge_gate] total={elapsed:.1f}s budget=1200s scope={'lane_merge through gate' if measured_window else 'gate only; no verified lane_merge clock'} verdict={verdict}")
            if args.full and result == 0 and not args.dry_run:
                (ROOT / "build64-ninja/last-full-gate.json").write_text(json.dumps(dict(head=head, finished=time.time())))
            return result if args.dry_run else (0 if verdict == "PASS" else 1)


if __name__ == "__main__":
    sys.exit(main())
