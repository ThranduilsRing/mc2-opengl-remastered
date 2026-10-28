import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import build_tokens  # noqa: E402
import ninja_build  # noqa: E402


def test_pool_size_reads_build_pool_file(tmp_path, monkeypatch):
    f = tmp_path / "BUILD_POOL"
    f.write_text("12\n")
    assert build_tokens.pool_size(str(f)) == 12
    # the env override names the pool file; JOBS (per-agent -j) never sizes the pool
    monkeypatch.setenv("MC2_BUILD_POOL_FILE", str(f))
    assert build_tokens.pool_size() == 12
    monkeypatch.delenv("MC2_BUILD_POOL_FILE")
    # missing or garbage file: cpu_count - 4, at least 1
    g = tmp_path / "bad"
    g.write_text("zero\n")
    want = max(1, (os.cpu_count() or 8) - 4)
    assert build_tokens.pool_size(str(tmp_path / "missing")) == want
    assert build_tokens.pool_size(str(g)) == want


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


def test_ninja_pool_wait_is_bounded_and_timeout_never_requests_unlimited_jobs(monkeypatch):
    seen = {}

    def exhausted(want, **kwargs):
        seen.update(kwargs)
        return build_tokens.Tokens([], 120, 6)

    monkeypatch.setattr(build_tokens, "acquire", exhausted)
    argv, tokens = ninja_build.pool_jobs(["mc2", "-j6"])
    assert seen["timeout_s"] == 120
    assert argv == ["mc2", "-j1"]
    tokens.close()
