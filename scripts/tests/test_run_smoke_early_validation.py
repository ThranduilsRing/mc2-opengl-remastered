"""run_smoke validates --deploy/--exe/--mission before the host-lease wait (a lane lost an hour to a late failure)."""
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import console_child  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize("mode,duration,cap,accepted", [
    ("--via-menu", None, 300, True), ("--via-menu", 120, 300, True),
    ("--via-menu", 300, 300, True), ("--via-menu", 301, 300, False),
    (None, 30, 30, True), (None, 31, 30, False),
    ("--sim-determinism", 60, 60, True), ("--sim-determinism", 61, 60, False),
])
def test_duration_caps_before_any_lease_or_launch(monkeypatch, capsys, mode, duration, cap, accepted):
    import argparse
    import run_smoke
    parsed = []
    real_parse = argparse.ArgumentParser.parse_args
    def parse(parser, *args, **kwargs):
        result = real_parse(parser, *args, **kwargs)
        parsed.append(result)
        return result
    monkeypatch.setattr(argparse.ArgumentParser, "parse_args", parse)
    monkeypatch.delenv("MC2_OWNER_NO_LEASE", raising=False)
    monkeypatch.delenv("MC2_PLAY_MISSION", raising=False)
    monkeypatch.setattr(sys, "argv", ["run_smoke", "--no-lease", *([mode] if mode else []),
                                    *(["--duration", str(duration)] if duration is not None else [])])
    with pytest.raises(SystemExit) as error:
        run_smoke.main()
    assert error.value.code == (6 if accepted else 2)
    stderr = capsys.readouterr().err
    if accepted:
        # Deliberately stop at the existing owner-only guard, before lease/game work.
        assert "--no-lease is owner-only" in stderr and "hard cap" not in stderr
        if mode == "--via-menu":
            assert parsed[0].duration == (duration if duration is not None else run_smoke.VIA_MENU_TIMEOUT_S)
    else:
        assert f"cap of {cap}s" in stderr


def _run(*args):
    return subprocess.run([sys.executable, str(ROOT / "scripts/run_smoke.py"), *args],
                          capture_output=True, text=True, timeout=60, **console_child.child_kwargs(redirected=True))


def test_deploy_path_rejected_before_lease():
    r = _run("--mission", "mc2_01", "--deploy", "E:/mc2-installs/some-lane", "--no-wait")
    assert r.returncode == 2
    assert "--exe <path>/mc2.exe" in r.stderr and "[LEASE]" not in r.stderr


def test_missing_exe_rejected_before_lease(tmp_path):
    r = _run("--mission", "mc2_01", "--exe", str(tmp_path / "mc2.exe"), "--no-wait")
    assert r.returncode == 2 and "is not a file" in r.stderr


def test_unknown_mission_rejected_before_lease(tmp_path):
    exe = tmp_path / "mc2.exe"
    exe.write_bytes(b"")
    r = _run("--mission", "no_such_mission", "--exe", str(exe), "--no-wait")
    # the exe guard may refuse an empty stub first (exit 7); the mission error must not need a lease either way
    assert r.returncode in (2, 7) and "host lease" not in r.stderr.lower()


def test_cold_cache_grace_applies_once_per_deployed_exe(tmp_path):
    import run_smoke
    exe = tmp_path / "mc2.exe"
    exe.write_bytes(b"one")
    assert run_smoke.cold_cache_extra_grace(exe) == run_smoke.COLD_CACHE_EXTRA_GRACE_S
    assert run_smoke.cold_cache_extra_grace(exe) == 0
    exe.write_bytes(b"redeployed")
    assert run_smoke.cold_cache_extra_grace(exe) == run_smoke.COLD_CACHE_EXTRA_GRACE_S


def test_exe_directory_names_the_exe_path(tmp_path):
    r = _run("--mission", "mc2_01", "--exe", str(tmp_path), "--no-wait")
    assert r.returncode == 2 and "mc2.exe" in r.stderr and "directory" in r.stderr


def test_gpu_routing_failure_clears_passed():
    """report.md says FAIL for a GPU routing mismatch; the exit code must too (the laptop gate recorded PASS for it, W131/W132)."""
    import ast
    tree = ast.parse((ROOT / "scripts/run_smoke.py").read_text(encoding="utf-8"))
    main = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "main")
    clears = [n for n in ast.walk(main) if isinstance(n, ast.If) and isinstance(n.test, ast.Name) and n.test.id == "_gpu_failures"
              and any(isinstance(b, ast.Assign) and getattr(b.targets[0], "id", "") == "passed"
                      and isinstance(b.value, ast.Constant) and b.value.value is False for b in n.body)]
    assert clears, "run_smoke.main must set passed = False when _gpu_failures is non-empty"


def test_gpu_routing_check_skips_headless_runs():
    """A --headless run opens no Vulkan device: the routing check must not fire on it (W133 --sim: every headless determinism row went red)."""
    import ast
    import run_smoke
    env = {"MC2_HARNESS_GPU": "2070", "MC2_VK_DEVICE": "2070"}   # an explicit request the (empty) device list cannot satisfy
    msg = run_smoke.gpu_routing_failure(False, env, False, [], "")
    assert msg and "GPU routing" in msg, "a windowed run with an unroutable GPU request must report the mismatch"
    assert run_smoke.gpu_routing_failure(True, env, False, [], "") is None, "a headless run must never report a routing failure"
    assert run_smoke.gpu_routing_failure(True, env, False, ["Some Other GPU"], "") is None
    # main() must feed the headless flag through that helper (the behaviour above is only worth anything if the call site uses it)
    main = next(n for n in ast.parse((ROOT / "scripts/run_smoke.py").read_text(encoding="utf-8")).body if isinstance(n, ast.FunctionDef) and n.name == "main")
    assert any(isinstance(n, ast.Call) and getattr(n.func, "id", "") == "gpu_routing_failure" and "args.headless" in ast.unparse(n.args[0])
               for n in ast.walk(main))
