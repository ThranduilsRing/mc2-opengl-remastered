import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import gate_wait  # noqa: E402


def test_one_verdict_line_and_log(tmp_path, capsys):
    log = str(tmp_path / "g.log")
    assert gate_wait.main(["--log", log, "--", sys.executable, "-c", "print('hello')"]) == 0
    out = capsys.readouterr().out.strip().splitlines()
    assert len(out) == 1 and out[0].startswith("PASS rc=0 ") and out[0].endswith("log=" + log)
    assert "hello" in open(log).read()
    assert gate_wait.main(["--log", log, "--", sys.executable, "-c", "raise SystemExit(3)"]) == 1
    assert capsys.readouterr().out.startswith("FAIL rc=3 ")


def test_timeout_is_a_fail(tmp_path, capsys):
    log = str(tmp_path / "t.log")
    assert gate_wait.main(["--log", log, "--timeout", "1", "--", sys.executable, "-c", "import time; time.sleep(30)"]) == 1
    assert capsys.readouterr().out.startswith("FAIL rc=124 ")
