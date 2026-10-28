"""The root conftest watchdog turns a hung test into a failure instead of stalling the run."""
import subprocess
import sys
import textwrap
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize("phase", ["setup", "call", "teardown"])
def test_hung_test_is_interrupted(tmp_path, phase):
    t = tmp_path / "test_hang.py"
    t.write_text(textwrap.dedent("""
        import pytest
        def hang():
            import subprocess, sys
            subprocess.run([sys.executable, "-c", "import time; time.sleep(60)"])
        @pytest.fixture
        def fixture():
            if PHASE == "setup": hang()
            yield
            if PHASE == "teardown": hang()
        def test_hang(fixture):
            if PHASE == "call": hang()
        def test_after():
            assert True
    """).replace("PHASE", repr(phase)))
    # Exercise the Windows fallback too: /T must target a child PID, never a PPID filter.
    config = (ROOT / "conftest.py").read_text()
    config += f"\nsys.path.insert(0, {str(ROOT)!r})\nif os.name == 'nt': sys.modules['psutil'] = None\n"
    (tmp_path / "conftest.py").write_text(config)
    r = subprocess.run([sys.executable, "-B", "-m", "pytest", str(t), "-v", "-p", "no:cacheprovider", "--rootdir", str(tmp_path)],
                       env={**__import__("os").environ, "MC2_TEST_TIMEOUT": "2"}, capture_output=True, text=True, timeout=50, cwd=tmp_path)  # background-launch: allow pytest child, no engine
    assert r.returncode == 1 and "test_after PASSED" in r.stdout, (r.stdout + r.stderr)[-1600:]
    assert "test-timeout" in r.stderr + r.stdout
