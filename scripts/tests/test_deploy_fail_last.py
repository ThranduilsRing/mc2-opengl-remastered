"""A deploy refusal is the last stderr line, even when cleanup logs print after fail()."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts import console_child

SCRIPTS = Path(__file__).resolve().parents[1]

CHILD = """
import sys
sys.path.insert(0, {scripts!r})
import deploy_payload as d
try:
    d.fail('REFUSING stale exe\\n  detail', code=3)
finally:
    d.log('folder lease released: x')
"""


def test_failure_is_last_line_and_exit_nonzero():
    p = subprocess.run([sys.executable, "-c", CHILD.format(scripts=str(SCRIPTS))],
                       capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    assert p.returncode == 3
    err = p.stderr.replace("\r", "").strip().splitlines()
    assert err[-1] == "[deploy_payload] FAIL (exit 3): REFUSING stale exe"
