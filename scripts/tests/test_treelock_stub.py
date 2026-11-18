"""conftest stubs the harness TreeLock: a held real lock must not block build_contract_harnesses in ordinary tests."""
import sys
import time
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))
import build_contract_harnesses as bch
from ninja_build import TreeLock


def test_a_held_real_lock_does_not_block_the_stubbed_builder(monkeypatch, tmp_path):
    monkeypatch.setattr(bch, "cmake_exe", lambda: "cmake")
    monkeypatch.setattr(bch.bd, "configure_args", lambda *a, **k: ["-S", "src"])
    monkeypatch.setattr(bch.bd, "build_args", lambda *a, **k: ["cmake", "--build"])
    monkeypatch.setattr(bch.subprocess, "run", lambda cmd, **k: SimpleNamespace(returncode=0, stdout="", stderr=""))
    monkeypatch.setattr(bch, "TREE_LOCK_WAIT_S", 60)
    holder = TreeLock(str(tmp_path), "build64-tests")
    assert holder.acquire(0)
    try:
        start = time.monotonic()
        ok, detail = bch.build_and_run_unit_tests("RelWithDebInfo", run=False, build_root=tmp_path)
        assert ok and time.monotonic() - start < 5, detail
    finally:
        holder.close()


def _child_acquires(root, env):
    code = "import sys; sys.path.insert(0, sys.argv[2]); import ninja_build; sys.exit(0 if ninja_build.TreeLock(sys.argv[1], 't').acquire(0) else 3)"
    import subprocess
    import console_child
    return subprocess.run([sys.executable, "-B", "-c", code, str(root), str(ROOT / "scripts")], env=env, timeout=60, **console_child.child_kwargs(redirected=True)).returncode


def test_env_switch_frees_a_subprocess_child_only_under_pytest(monkeypatch, tmp_path):
    import os
    monkeypatch.delenv("MC2_TEST_NO_TREELOCK", raising=False)   # the parent holds the REAL lock
    holder = TreeLock(str(tmp_path), "t")
    assert holder.acquire(0)
    try:
        env = dict(os.environ, MC2_TEST_NO_TREELOCK="1", PYTEST_CURRENT_TEST="x")
        assert _child_acquires(tmp_path, env) == 0                      # pytest child: no-op lock
        gate = {k: v for k, v in env.items() if k != "PYTEST_CURRENT_TEST"}
        assert _child_acquires(tmp_path, gate) == 3                     # gate path (no pytest): lock honoured
        assert _child_acquires(tmp_path, {k: v for k, v in gate.items() if k != "MC2_TEST_NO_TREELOCK"}) == 3
    finally:
        holder.close()
