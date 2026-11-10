"""Every vegetation card the engine loads is tracked in git and named in deploy_payload's payload list."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts import deploy_payload as dp  # noqa: E402


def _engine_cards():
    text = (ROOT / "GameOS/gameos/gos_vegetation.cpp").read_text(encoding="utf-8")
    return sorted(set(re.findall(r'"(data/vegetation/cards/[a-z_0-9]+\.png)"', text)))


def test_engine_cards_are_in_the_payload_list():
    cards = _engine_cards()
    assert len(cards) == 8
    for png in cards:
        stem = Path(png).stem
        cooked = f"data/vegetation/cooked/{stem}.ktx2"
        for rel in (png, cooked):
            assert rel in dp.GAME_DATA_PAYLOAD, f"{rel} is loaded by the engine but not deployed"
            assert (ROOT / rel).is_file(), f"{rel} missing from the source tree"
