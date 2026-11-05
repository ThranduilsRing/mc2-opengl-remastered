"""run_with_tree_timeout: rc passthrough, and a hung child is killed on timeout."""
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build_contract_harnesses as bch  # noqa: E402


def test_rc_preserved():
    rc = bch.run_with_tree_timeout([sys.executable, "-c", "import sys; sys.exit(7)"], os.getcwd(), dict(os.environ), 60)
    assert rc == 7


def test_hang_is_killed():
    started = time.monotonic()
    rc = bch.run_with_tree_timeout([sys.executable, "-c", "import time; time.sleep(600)"], os.getcwd(),
                                   dict(os.environ), 2)
    assert rc is None
    assert time.monotonic() - started < 30
