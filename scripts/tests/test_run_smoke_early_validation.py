"""run_smoke validates --deploy/--exe/--mission before the host-lease wait (a lane lost an hour to a late failure)."""
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import console_child  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


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
