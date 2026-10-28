"""run_smoke --legacy-env: the env is filtered to the gates an older exe's gate table knows."""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from smoke_lib import runner  # noqa: E402


def test_filter_legacy_env():
    names = runner.legacy_gate_names(json.dumps({"gates": [{"name": "MC2_OLD"}]}))
    try:
        env = {"MC2_OLD": "1", "MC2_NEW": "1", "PATH": "x"}
        assert runner.filter_legacy_env(env) == [] and len(env) == 3  # off by default
        runner.set_legacy_gates(names)
        assert runner.filter_legacy_env(env) == ["MC2_NEW"]
        assert env == {"MC2_OLD": "1", "PATH": "x"}
    finally:
        runner.set_legacy_gates(None)


def test_cli_flag_exists():
    src = (ROOT / "scripts" / "run_smoke.py").read_text(encoding="utf-8")
    assert '"--legacy-env"' in src and "set_legacy_gates" in src
