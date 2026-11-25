"""check_fingerprint works when smoke_lib is imported with only scripts/ on sys.path (run_player_path, run_smoke)."""
import subprocess
import sys
from pathlib import Path
from scripts import console_child

SCRIPTS = Path(__file__).resolve().parents[1]

CODE = (
    "import sys; sys.path.insert(0, sys.argv[1])\n"
    "from smoke_lib import fingerprint\n"
    "lines, bad = fingerprint.check_fingerprint(None, None)\n"
    "print(lines, bad)\n"
)


def test_check_fingerprint_with_scripts_dir_only_on_path():
    r = subprocess.run([sys.executable, "-I", "-c", CODE, str(SCRIPTS)], capture_output=True, text=True,
                       **console_child.child_kwargs(redirected=True))
    assert r.returncode == 0, r.stderr
    assert "absent" in r.stdout
