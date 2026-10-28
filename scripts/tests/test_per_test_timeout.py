"""The root conftest watchdog turns a hung test into a failure instead of stalling the run."""
import subprocess
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_hung_test_is_interrupted(tmp_path):
    t = tmp_path / "test_hang.py"
    t.write_text(textwrap.dedent("""
        import time
        def test_hang():
            import subprocess, sys
            subprocess.run([sys.executable, "-c", "import time; time.sleep(60)"])
            time.sleep(0.5)
        def test_after():
            assert True
    """))
    (tmp_path / "conftest.py").write_text((ROOT / "conftest.py").read_text())
    r = subprocess.run([sys.executable, "-B", "-m", "pytest", str(t), "-q", "-p", "no:cacheprovider", "--rootdir", str(tmp_path)],
                       env={**__import__("os").environ, "MC2_TEST_TIMEOUT": "2"}, capture_output=True, text=True, timeout=50, cwd=tmp_path)
    assert "1 failed, 1 passed" in r.stdout, r.stdout[-800:]
    assert "test-timeout" in r.stderr + r.stdout
