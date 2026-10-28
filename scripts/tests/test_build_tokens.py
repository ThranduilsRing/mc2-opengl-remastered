import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import build_tokens  # noqa: E402


def test_pool_size_reads_jobs_file(tmp_path):
    f = tmp_path / "JOBS"
    f.write_text("6\n")
    assert build_tokens.pool_size(str(f)) == 6
    assert build_tokens.pool_size(str(tmp_path / "missing")) >= 1


def test_tokens_are_shared_and_released(tmp_path):
    d = str(tmp_path)
    a = build_tokens.acquire(3, total=4, directory=d)
    assert len(a) == 3
    b = build_tokens.acquire(3, total=4, directory=d)
    assert len(b) == 1  # only one slot left; takes it rather than waiting for three
    waits = []
    c = build_tokens.acquire(2, total=4, directory=d, timeout_s=0.05, poll_s=0.01, on_wait=waits.append)
    assert len(c) == 0 and waits == [4]  # pool exhausted: one wait line, then falls through at timeout
    a.close()
    d2 = build_tokens.acquire(4, total=4, directory=d)
    assert len(d2) == 3  # freed slots come back
    b.close()
    d2.close()
