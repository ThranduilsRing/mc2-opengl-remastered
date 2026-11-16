"""Captured module output must not depend on the parent's Windows code page."""
import subprocess
import sys
import threading
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_tools_tests as runner


@pytest.mark.parametrize("fallback,rc", [(False, 0), (False, 1), (True, 0), (True, 7)])
def test_module_utf8_stderr(tmp_path, monkeypatch, fallback, rc):
    monkeypatch.setattr(subprocess, "_text_encoding", lambda: "cp1252")
    exceptions = []
    monkeypatch.setattr(threading, "excepthook", exceptions.append)
    module = tmp_path / "test_child.py"
    emit = "import os; os.write(2, b'utf8: \\xe2\\x80\\x9d invalid: \\xff\\n')\n"
    if fallback:
        source = f"if __name__ == '__main__':\n    {emit}    raise SystemExit({rc})\n"
    else:
        source = ("import atexit\n"
                  f"atexit.register(lambda: exec({emit!r}))\n"
                  f"def test_child():\n    assert {rc} == 0\n")
    module.write_text(source, encoding="utf-8")
    result = runner._run_module_once(str(module))
    assert not exceptions, [str(e.exc_value) for e in exceptions]
    assert result[1] == rc
    assert 'utf8: \u201d invalid: \ufffd' in result[3]
    assert bool(result[2]) == bool(rc)


@pytest.mark.parametrize("fallback", [False, True])
def test_module_missing_capture(monkeypatch, fallback):
    results = iter(([SimpleNamespace(stdout=None, stderr=None, returncode=5)] if fallback else [])
                   + [SimpleNamespace(stdout=None, stderr=None, returncode=0)])
    monkeypatch.setattr(subprocess, "run", lambda *a, **kw: next(results))
    assert runner._run_module_once("test_child.py") == ("test_child.py", 0, [], "")
