"""--quiet output filter (scripts/quiet_output.py) and its wiring in run_smoke / deploy_payload / merge_gate / laptop_gate."""
from __future__ import annotations

import io
import re
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import quiet_output as q  # noqa: E402
import console_child  # noqa: E402  (BELOW_NORMAL + CREATE_NO_WINDOW for every child)

SAMPLE = [
    "[deploy_payload] copying a.dll\n",
    "[deploy_payload] deployed a.dll (OK)\n",
    "[runner] launching mc2_10\n",
    "[DEPLOY_FINGERPRINT] OK: exe sha=abc matches worktree HEAD\n",
    "# Smoke run 2026  result=PASS (2/2 passed)\n",
    "| mc2_10  | PASS   | none | 100 |\n",
    "[runner] artifacts: A:/x/tests/smoke/20260101/\n",
    "[runner] WARN: slow disk\n",
    "random chatter\n",
    "## Failures\n",
    "### mc2_01 - crash\n",
    "plain detail after the failures heading\n",
]


def test_filter_keeps_only_signal():
    flt = q.LineFilter()
    shown = [l for l in SAMPLE if flt(l)]
    assert "[DEPLOY_FINGERPRINT] OK: exe sha=abc matches worktree HEAD\n" in shown
    assert any("result=PASS" in l for l in shown)
    assert any(l.startswith("| mc2_10") for l in shown)
    assert any("artifacts:" in l for l in shown)
    assert any("WARN" in l for l in shown)
    # noise dropped
    for noisy in ("[deploy_payload] copying a.dll\n", "[deploy_payload] deployed a.dll (OK)\n",
                  "[runner] launching mc2_10\n", "random chatter\n"):
        assert noisy not in shown
    # everything after the Failures heading stays
    assert "plain detail after the failures heading\n" in shown
    assert flt.total == len(SAMPLE) and flt.shown == len(shown)


def test_filter_stream_logs_everything():
    out, log = io.StringIO(), io.StringIO()
    q.filter_stream(iter(SAMPLE), out, log)
    assert log.getvalue() == "".join(SAMPLE)
    assert len(out.getvalue()) < len(log.getvalue())


def test_install_quiet_end_to_end(tmp_path):
    """fd-level redirect: own prints and a child process's prints are filtered, the log has all, the rc survives."""
    script = tmp_path / "chatty.py"
    script.write_text(textwrap.dedent(f"""
        import subprocess, sys
        sys.path.insert(0, {str(ROOT / 'scripts')!r})
        import quiet_output
        quiet_output.install_quiet({str(tmp_path / 'full.log')!r})
        print("noise line one")
        print("[DEPLOY_FINGERPRINT] OK: exe sha=1 matches worktree HEAD")
        subprocess.run([sys.executable, "-c", "print('child noise'); print('child FAIL: boom')"], check=True)
        print("tail result=PASS", file=sys.stderr)
        sys.exit(3)
    """), encoding="utf-8")
    r = subprocess.run([sys.executable, "-I", str(script)], capture_output=True, text=True, timeout=60,
                       **console_child.child_kwargs(redirected=True))
    assert r.returncode == 3
    shown = r.stdout + r.stderr
    assert "DEPLOY_FINGERPRINT" in shown and "child FAIL: boom" in shown and "result=PASS" in shown
    assert "noise line one" not in shown and "child noise" not in shown
    assert "[quiet] 3 of 5 lines shown; full output:" in shown
    full = (tmp_path / "full.log").read_text(encoding="utf-8")
    assert "noise line one" in full and "child noise" in full


@pytest.mark.parametrize("rel", ["scripts/run_smoke.py", "scripts/deploy_payload.py", "scripts/merge_gate.py",
                                 "tools/laptop_gate.py"])
def test_script_accepts_quiet(rel):
    r = subprocess.run([sys.executable, str(ROOT / rel), "--help"], capture_output=True, text=True, timeout=120,
                       **console_child.child_kwargs(redirected=True))
    assert r.returncode == 0, r.stderr[-400:]
    assert re.search(r"--quiet(?!-)", r.stdout), rel
    src = (ROOT / rel).read_text(encoding="utf-8")
    assert "quiet_output.install_quiet" in src, rel
