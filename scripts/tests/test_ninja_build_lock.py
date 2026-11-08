"""ninja_build.TreeLock: a second build in the same tree waits or exits naming the holder; the lock frees on close."""
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import ninja_build  # noqa: E402


def test_second_lock_fails_fast_names_holder_then_succeeds_after_close(tmp_path):
    first, second = (ninja_build.TreeLock(str(tmp_path), "t") for _ in range(2))
    assert first.acquire(0)
    assert not second.acquire(0)
    assert "pid " + str(os.getpid()) in second.holder()
    first.close()
    assert second.acquire(0)
    second.close()
