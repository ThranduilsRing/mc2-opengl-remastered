"""ninja_build.TreeLock: a second build in the same tree waits or exits naming the holder; the lock frees on close."""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import ninja_build  # noqa: E402
import pytest  # noqa: E402

pytestmark = pytest.mark.real_treelock   # tests the real lock (temp-dir roots only)


def test_warm_base_takes_the_tree_lock_before_switching_the_checkout(tmp_path, monkeypatch):
    import pytest
    calls = []
    monkeypatch.setattr(ninja_build.subprocess, "run", lambda *a, **k: calls.append(a[0]) or pytest.fail("git ran while the tree was locked"))
    monkeypatch.setenv("MC2_BUILD_LOCK_WAIT", "0")
    holder = ninja_build.TreeLock(str(tmp_path), "t")
    assert holder.acquire(0)
    try:
        with pytest.raises(SystemExit) as exc:
            ninja_build.warm(str(tmp_path), ["--base", "somebranch"], "t")
        assert "not switching the base" in str(exc.value) and not calls
    finally:
        holder.close()


def test_warm_base_holds_the_lock_while_switching(tmp_path, monkeypatch):
    import subprocess
    seen = []

    def fake_run(cmd, **k):
        if "switch" in cmd:
            second = ninja_build.TreeLock(str(tmp_path), "t")
            seen.append(second.acquire(0))   # a second taker must fail while warm holds the lock
            second.close()
        return subprocess.CompletedProcess(cmd, 0, "", "")
    monkeypatch.setattr(ninja_build.subprocess, "run", fake_run)
    monkeypatch.setattr(ninja_build.subprocess, "Popen", lambda *a, **k: type("P", (), {"pid": 1})())
    (tmp_path / "t").mkdir()
    (tmp_path / "t" / "build.ninja").write_text("")
    ninja_build.warm(str(tmp_path), ["--base", "somebranch"], "t")
    assert seen == [False]
    after = ninja_build.TreeLock(str(tmp_path), "t")
    assert after.acquire(0)   # released again once the detached build is launched
    after.close()


def test_second_lock_fails_fast_names_holder_then_succeeds_after_close(tmp_path):
    first, second = (ninja_build.TreeLock(str(tmp_path), "t") for _ in range(2))
    assert first.acquire(0)
    assert not second.acquire(0)
    assert "pid " + str(os.getpid()) in second.holder()
    first.close()
    assert second.acquire(0)
    second.close()
