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
