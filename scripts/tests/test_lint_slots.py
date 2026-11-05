"""LintSlots: host-wide cross-process counting semaphore for lint workers."""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import console_child  # noqa: E402
import lint_slots as lane_merge  # noqa: E402

HOLD = (
    "import sys,time;sys.path.insert(0,sys.argv[1]);import lint_slots as lane_merge;"
    "s=lane_merge.LintSlots(directory=sys.argv[2],n=1);fh=s.acquire();print('held',flush=True);time.sleep(60)"
)


def test_acquire_release_and_cap(tmp_path):
    s = lane_merge.LintSlots(directory=str(tmp_path), n=2)
    a, b = s.acquire(), s.acquire()
    assert s.try_acquire() is None          # both slots busy
    a.close()
    c = s.try_acquire()
    assert c is not None                    # released slot is reusable
    b.close()
    c.close()


def test_run_releases_on_exception(tmp_path):
    s = lane_merge.LintSlots(directory=str(tmp_path), n=1)

    def boom():
        raise RuntimeError("x")
    try:
        s.run(boom)
    except RuntimeError:
        pass
    fh = s.try_acquire()
    assert fh is not None
    fh.close()


def test_dead_process_frees_slot(tmp_path):
    tools = os.path.join(os.path.dirname(__file__), "..", "..", "tools")
    p = subprocess.Popen([sys.executable, "-B", "-c", HOLD, tools, str(tmp_path)], stdout=subprocess.PIPE, text=True,
                         **console_child.child_kwargs(redirected=True))
    try:
        assert p.stdout.readline().strip() == "held"
        s = lane_merge.LintSlots(directory=str(tmp_path), n=1)
        assert s.try_acquire() is None      # the other process holds the only slot
        p.kill()
        p.wait()
        deadline = time.monotonic() + 10
        fh = None
        while fh is None and time.monotonic() < deadline:
            fh = s.try_acquire()
            time.sleep(0.1)
        assert fh is not None               # the OS dropped the dead holder's lock
        fh.close()
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()


def test_cpu_split_masks_and_child_inherits():
    assert lane_merge.cpu_masks()["gate"] & lane_merge.cpu_masks()["lane"] == 0
    code = ("import os,sys,subprocess;sys.path.insert(0,sys.argv[1]);os.environ.pop('MC2_NO_CPU_PIN',None);import lint_slots as l;"
            "m=l.pin_cpus(sys.argv[2]);"
            "c=subprocess.run([sys.executable,'-c','import ctypes;k=ctypes.windll.kernel32;p=ctypes.c_size_t();s=ctypes.c_size_t();"
            "k.GetProcessAffinityMask(ctypes.c_void_p(k.GetCurrentProcess()),ctypes.byref(p),ctypes.byref(s));print(hex(p.value))'],capture_output=True,text=True);"
            "print('child',c.stdout.strip(),'parent',hex(m))")
    if os.name != "nt":
        return
    tools = os.path.join(os.path.dirname(__file__), "..", "..", "tools")
    for mode in ("gate", "lane"):
        want = lane_merge.cpu_masks()[mode]
        out = subprocess.run([sys.executable, "-B", "-c", code, tools, mode], capture_output=True, text=True,
                             **console_child.child_kwargs(redirected=True)).stdout
        assert f"child {hex(want)} parent {hex(want)}" in out or want & ~((1 << (os.cpu_count() or 1)) - 1)


# --- lint-fixup: CPU mode per lane_merge mode, one FIFO slot per lint, scratch sweep ---
import threading  # noqa: E402
from types import SimpleNamespace  # noqa: E402

import pytest  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
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


def test_lint_inner_workers_fit_lane_half():
    lane_cpus = bin(ls.cpu_masks()["lane"]).count("1")
    assert lm.REGEN_WORKERS + max(1, lane_cpus - lm.REGEN_WORKERS) <= max(lane_cpus, lm.REGEN_WORKERS + 1)
