"""parent_watchdog: polling loops stop once the process that started them is gone (a TaskStop kills the shell, not always its child)."""
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child  # noqa: E402
import parent_watchdog  # noqa: E402

KW = console_child.child_kwargs(redirected=True)


def _wait(pred, s=20.0):
    end = time.time() + s
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.2)
    return False


def test_live_parent_is_not_gone():
    assert not parent_watchdog.ParentWatch().gone()


def test_dead_watched_process_is_gone_and_logged_once():
    p = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **KW)
    lines = []
    w = parent_watchdog.ParentWatch(pid=p.pid, log=lines.append)
    assert not w.gone()
    p.kill()
    p.wait(10)
    assert _wait(w.gone)
    assert w.gone() and len(lines) == 1


def test_reused_pid_counts_as_gone():
    w = parent_watchdog.ParentWatch(pid=os.getpid(), log=lambda m: None)
    w.created -= 100.0                                   # the remembered parent started long before this pid's process
    assert w.gone()


def test_dev_shell_watcher_exits_when_its_parent_dies(tmp_path):
    (tmp_path / "shaders").mkdir()
    pidfile, out = tmp_path / "watch.pid", tmp_path / "watch.out"
    # The middle process starts the watcher, waits until it is polling, records its pid, and exits: the watcher's parent is then gone.
    middle = ("import subprocess, sys, time\n"
              f"o = open(r'{out}', 'w')\n"
              f"p = subprocess.Popen([sys.executable, '-u', r'{ROOT / 'tools' / 'dev_shell' / 'watch.py'}', r'{tmp_path}', '--interval', '0.2'],"
              " stdout=o, stderr=subprocess.STDOUT, creationflags=0x08000000 if sys.platform == 'win32' else 0)\n"
              f"open(r'{pidfile}', 'w').write(str(p.pid))\n"
              "end = time.time() + 15\n"
              f"while time.time() < end and '[watch]' not in open(r'{out}').read(): time.sleep(0.1)\n"
              "time.sleep(0.6)\n"
              "sys.exit(0 if p.poll() is None else 3)\n")
    rc = subprocess.run([sys.executable, "-c", middle], timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **KW).returncode
    pid = int(pidfile.read_text())
    assert rc == 0, "watch.py was not polling while its parent lived:\n" + out.read_text()
    import psutil
    assert _wait(lambda: not psutil.pid_exists(pid)), f"watch.py pid {pid} kept polling after its parent exited"
    assert "parent pid" in out.read_text()
