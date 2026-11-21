"""ensure_actor_pbr: the cold cook runs with the deploy phase clock paused, so a long cook never trips the phase watcher."""
from __future__ import annotations

import json
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts import deploy_payload as dp  # noqa: E402


def test_long_cook_does_not_trip_phase_watcher(tmp_path, monkeypatch):
    pins = tmp_path / dp.ACTOR_PBR_PINS
    pins.parent.mkdir(parents=True)
    pins.write_text(json.dumps({"data/x.ktx2": {"ktx2_sha256": "0" * 64}}))
    monkeypatch.setenv("MC2_LEAN_SEED", str(tmp_path / "noseed"))
    import asset_parity
    monkeypatch.setattr(asset_parity, "default_ref", lambda: tmp_path / "noref")
    exits = []
    monkeypatch.setattr(dp.os, "_exit", lambda code: exits.append(code))
    seen = []
    gate = threading.Event()

    def slow_cooker(*_a, **_k):
        seen.append(dp._PAUSE["depth"])
        gate.wait(0.5)  # 10x the phase budget below
        return subprocess.CompletedProcess([], 0, "", "")

    with dp.deploy_phase("payload enumeration", 0.05):
        dp.ensure_actor_pbr(str(tmp_path), cooker=slow_cooker)
    assert seen == [1] and exits == []
