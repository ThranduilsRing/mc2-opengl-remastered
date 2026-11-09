import os
import sys
import multiprocessing
import subprocess

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import build_tokens  # noqa: E402
import console_child  # noqa: E402


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
    want = min(max(1, (os.cpu_count() or 8) - 4), build_tokens.compile_cpu_count())
    assert build_tokens.pool_size(str(tmp_path / "missing")) == want
    assert build_tokens.pool_size(str(g)) == want


def test_tokens_are_shared_and_released(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_BUILD_POOL_FILE", str(tmp_path / "missing"))
    d = str(tmp_path)
    a = build_tokens.acquire(3, total=4, directory=d)
    assert len(a) == 3
    waits = []
    b = build_tokens.acquire(3, total=4, directory=d, timeout_s=0.05, poll_s=0.01, on_wait=waits.append)
    assert len(b) == 0 and waits == [4]  # one free slot cannot meet the per-build floor
    assert len(build_tokens.status(d, total=4)["waiters"]) == 0  # timed-out ticket is reclaimed
    a.close()
    d2 = build_tokens.acquire(4, total=4, directory=d)
    assert len(d2) == 4
    d2.close()
    # A wide request can still run at the floor instead of waiting for its full width.
    with build_tokens.acquire(3, total=6, directory=d):
        with build_tokens.acquire(6, total=6, directory=d) as partial:
            assert len(partial) == 3


def test_ninja_pool_keeps_one_ticket_and_requested_width(monkeypatch):
    import ninja_build

    seen, calls = {}, []

    def acquire(want, **kwargs):
        seen.update(kwargs)
        calls.append(want)
        return build_tokens.Tokens([object()] * 3, 125, 6)

    monkeypatch.setattr(build_tokens, "acquire", acquire)
    argv, tokens = ninja_build.pool_jobs(["mc2", "-j1"])
    assert seen.get("timeout_s") is None and calls == [1]
    assert argv == ["mc2", "-j1"] and len(tokens) == 3
    tokens.handles = []


@pytest.mark.parametrize("config,floor", [("12\n", 3), ("12 4\n", 4), ("12 bad\n", 3)])
def test_floor_reservation_caps_small_build_concurrency(tmp_path, monkeypatch, config, floor):
    config_file = tmp_path / "BUILD_POOL"
    config_file.write_text(config)
    monkeypatch.setenv("MC2_BUILD_POOL_FILE", str(config_file))
    assert build_tokens.pool_size() == 12
    a = build_tokens.acquire(1, total=6, directory=str(tmp_path))
    try:
        assert len(a) == floor
        b = build_tokens.acquire(1, total=6, directory=str(tmp_path), timeout_s=0, poll_s=0.01)
        try:
            assert len(b) == (floor if floor == 3 else 0)
        finally:
            b.close()
    finally:
        a.close()
    # A tiny configured pool must remain usable, even below the default floor.
    with build_tokens.acquire(1, total=2, directory=str(tmp_path)) as small:
        assert len(small) == 2


def _waiter(directory, index, waiting, admitted, release, results):
    with build_tokens.acquire(3, total=3, directory=directory, poll_s=0.01,
                              on_wait=lambda _: waiting.set()):
        results.put(index)
        admitted.set()
        if not release.wait(120):
            raise RuntimeError("test did not release the admitted build")


@pytest.fixture
def queued_builds(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_BUILD_POOL_FILE", str(tmp_path / "missing"))
    context = multiprocessing.get_context("spawn")
    blocker = build_tokens.acquire(3, total=3, directory=str(tmp_path))
    results = context.Queue()
    children = []
    try:
        for index in range(3):
            waiting, admitted, release = (context.Event() for _ in range(3))
            process = context.Process(target=_waiter, args=(str(tmp_path), index, waiting, admitted, release, results))
            process.start()
            children.append((process, admitted, release))
            assert waiting.wait(60), f"waiter failed to register its ticket (exit={process.exitcode})"
        yield blocker, children, results
    finally:
        blocker.close()
        for process, _, release in children:
            # terminate() can strand Event's condition semaphore in a dead waiter.
            # Its token locks are already released; never signal its test event.
            if process.is_alive():
                release.set()
            process.join(5)
            if process.is_alive():
                process.terminate()
                process.join(5)
        results.close()
        results.join_thread()


def test_three_waiters_admitted_in_ticket_order(queued_builds):
    blocker, children, results = queued_builds
    blocker.close()
    for index, (process, admitted, release) in enumerate(children):
        assert admitted.wait(60)
        assert results.get(timeout=5) == index
        release.set()
        process.join(5)
        assert process.exitcode == 0


def test_killed_waiter_is_skipped(queued_builds):
    blocker, children, results = queued_builds
    dead = children[0][0]
    dead.terminate()
    dead.join(5)
    assert not dead.is_alive()
    blocker.close()
    for index in (1, 2):
        process, admitted, release = children[index]
        assert admitted.wait(60)
        assert results.get(timeout=5) == index
        release.set()
        process.join(5)
        assert process.exitcode == 0


def test_killed_holder_releases_tokens_and_ticket(queued_builds):
    blocker, children, results = queued_builds
    blocker.close()
    process, admitted, _ = children[0]
    assert admitted.wait(60)
    assert results.get(timeout=5) == 0
    process.terminate()
    process.join(5)
    assert not process.is_alive()
    for index in (1, 2):
        process, admitted, release = children[index]
        assert admitted.wait(60)
        assert results.get(timeout=5) == index
        release.set()
        process.join(5)
        assert process.exitcode == 0


def test_status_lists_holder_and_ordered_waiters(queued_builds, tmp_path, monkeypatch):
    _, children, _ = queued_builds
    snapshot = build_tokens.status(str(tmp_path), total=3)
    assert snapshot["pool_size"] == 3
    assert [(h["pid"], h["worktree"], h["tokens"]) for h in snapshot["holders"]] == [(os.getpid(), os.getcwd(), 3)]
    assert snapshot["holders"][0]["held_s"] >= 0
    assert [w["pid"] for w in snapshot["waiters"]] == [p.pid for p, _, _ in children]
    assert all(w["waiting_s"] >= 0 and w["worktree"] == os.getcwd() for w in snapshot["waiters"])
    pool = tmp_path / "BUILD_POOL"
    pool.write_text("3\n")
    monkeypatch.setenv("MC2_BUILD_TOKEN_DIR", str(tmp_path))
    monkeypatch.setenv("MC2_BUILD_POOL_FILE", str(pool))
    result = subprocess.run([sys.executable, build_tokens.__file__, "status"], capture_output=True, text=True,
                            timeout=60, **console_child.child_kwargs(redirected=True))
    assert result.returncode == 0, result.stderr
    assert "Pool: 3 tokens" in result.stdout
    assert f"Holder pid={os.getpid()} worktree={os.getcwd()} tokens=3 held for " in result.stdout
    for index, (process, _, _) in enumerate(children, 1):
        assert f"Waiter {index} pid={process.pid} worktree={os.getcwd()} requested=3 waiting for " in result.stdout
