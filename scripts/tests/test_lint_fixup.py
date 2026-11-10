"""lint-fixup: CPU mode per lane_merge mode, one FIFO slot per lint, stale scratch sweep, known-failure expiry warning."""
import os
import sys
import threading
import time
from types import SimpleNamespace

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from tools import lane_merge as lm  # noqa: E402
import lint_slots as ls  # noqa: E402


def _args(**kw):
    base = dict(lint=None, precheck=None, pre_gate=False, regenerate_all=False, batch=None, ref=None)
    base.update(kw)
    return SimpleNamespace(**base)


@pytest.mark.parametrize("kw,want", [
    (dict(batch=["a", "b"]), "gate"), (dict(ref="tip"), "gate"),
    (dict(lint="x"), "lane"), (dict(precheck="x"), "lane"),
    (dict(pre_gate=True), "lane"), (dict(regenerate_all=True), "lane"),
])
def test_cpu_mode(kw, want):
    assert lm.cpu_mode(_args(**kw)) == want


def test_one_slot_per_lint_run(tmp_path, monkeypatch):
    monkeypatch.setenv("MC2_LINT_SLOTS_DIR", str(tmp_path))
    acquired = []
    real = ls.LintSlots.acquire
    monkeypatch.setattr(ls.LintSlots, "acquire", lambda self, *a, **k: acquired.append(1) or real(self, *a, **k))
    monkeypatch.setattr(lm, "sweep_stale_scratch", lambda parents: [])
    monkeypatch.setattr(lm, "_lint_locked", lambda *a: 0)
    assert lm.lint(ROOT, "HEAD") == 0
    assert acquired == [1]
    assert ls.LintSlots(directory=str(tmp_path), n=1).try_acquire() is not None   # released afterwards


def test_slots_are_granted_in_start_order(tmp_path):
    first = ls.LintSlots(directory=str(tmp_path), n=1)
    holder = first.acquire()
    order, lock = [], threading.Lock()

    def waiter(tag):
        s = ls.LintSlots(directory=str(tmp_path), n=1, sleep=lambda _s: time.sleep(0.02))
        fh = s.acquire(wait_s=30)
        with lock:
            order.append(tag)
        time.sleep(0.05)
        fh.close()
    threads = []
    for tag in range(5):
        t = threading.Thread(target=waiter, args=(tag,))
        t.start()
        threads.append(t)
        time.sleep(0.15)   # distinct, ordered start times
    holder.close()
    for t in threads:
        t.join(30)
    assert order == [0, 1, 2, 3, 4]
    assert os.listdir(os.path.join(str(tmp_path), "queue")) == []   # tickets removed


def test_dead_waiter_ticket_does_not_block(tmp_path):
    s = ls.LintSlots(directory=str(tmp_path), n=1)
    q = tmp_path / "queue"
    q.mkdir()
    dead = q / "00000000000000000001-1-0000.q"
    dead.write_text("")
    os.utime(dead, (time.time() - 60, time.time() - 60))
    fh = s.acquire(wait_s=5)
    assert fh is not None and not dead.exists()
    fh.close()


def test_sweep_stale_scratch_skips_junctions(tmp_path):
    import subprocess
    outside = tmp_path / "outside"
    (outside / "tree").mkdir(parents=True)
    (outside / "tree" / "f.txt").write_text("x")
    link = tmp_path / "lane-lint-junc0001"
    r = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(outside)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        pytest.skip(f"cannot create junction: {r.stderr.strip() or r.stdout.strip()}")
    old = time.time() - 3 * 3600
    for p in (outside / "tree" / "f.txt", outside / "tree", outside):
        os.utime(p, (old, old))
    removed = ls.sweep_stale_scratch([str(tmp_path)])
    assert removed == []
    assert link.exists()   # not renamed, not deleted
    assert (outside / "tree" / "f.txt").is_file()


def test_sweep_stale_scratch_exact_names_age_and_liveness(tmp_path):
    old = time.time() - 3 * 3600
    stale = tmp_path / "lane-lint-ab12_xyz"
    stale_inv = tmp_path / "stale-inv-Zz09abcd"
    fresh = tmp_path / "lane-lint-fresh123"
    other = tmp_path / "lane-lint-notatempname"   # wrong suffix length: not ours
    unrelated = tmp_path / "keep-me"
    held = tmp_path / "lane-lint-held0001"
    for d in (stale, stale_inv, fresh, other, unrelated, held):
        (d / "tree").mkdir(parents=True)
        (d / "tree" / "f.txt").write_text("x")
    for d in (stale, stale_inv, other, unrelated, held):
        for p in (d / "tree" / "f.txt", d / "tree", d):
            os.utime(p, (old, old))
    cwd = os.getcwd()
    os.chdir(held)   # a live process using the dir (cwd) blocks the rename on Windows
    try:
        removed = ls.sweep_stale_scratch([str(tmp_path), None, str(tmp_path / "missing")])
    finally:
        os.chdir(cwd)
    assert sorted(os.path.basename(p) for p in removed if "held" not in p) == ["lane-lint-ab12_xyz", "stale-inv-Zz09abcd"]
    assert fresh.exists() and other.exists() and unrelated.exists()
    if os.name == "nt":
        assert held.exists() and str(held) not in removed




@pytest.mark.skipif(os.name == "nt", reason="the Linux scaling branch")
def test_linux_jobs_scale_with_usable_cores(monkeypatch, tmp_path):
    import lint_slots
    monkeypatch.setattr(lint_slots.Path, "read_text", lambda self: (_ for _ in ()).throw(OSError()))
    monkeypatch.setattr(os, "sched_getaffinity", lambda pid: set(range(32)), raising=False)
    assert lint_slots.LintSlots(directory=str(tmp_path)).n == 16
    monkeypatch.setattr(os, "sched_getaffinity", lambda pid: {0, 1}, raising=False)
    assert lint_slots.LintSlots(directory=str(tmp_path)).n == 2
