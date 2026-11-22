"""GPU_ONE_CARD kill-switch: one lease slot on the named XTX (owner 11-21 after driver crashes)."""
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from smoke_lib import host_lease as hl

ROWS = [{"name": "AMD Radeon RX 7900 XTX", "uuid": "d", "display": True},
        {"name": "AMD Radeon RX 7900 XTX", "uuid": "l", "display": False}]


def _setup(monkeypatch, tmp_path, text):
    fake = SimpleNamespace(adapters=lambda: ROWS, selector=lambda r: f"{r['name']}@{r['uuid']}")
    monkeypatch.setattr(hl, "redirected", lambda: False)
    monkeypatch.setattr(hl, "runtime_lane_adapter", lambda: ROWS[1])
    monkeypatch.setattr(hl, "_gpu_adapters", lambda: fake)
    path = tmp_path / "GPU_ONE_CARD"
    if text is not None:
        path.write_text(text, encoding="utf-8")
    monkeypatch.setattr(hl, "ONE_CARD_PATH", path)


def test_no_file_keeps_both_cards(monkeypatch, tmp_path):
    _setup(monkeypatch, tmp_path, None)
    assert [d[-1] for d in hl.lane_devices()] == ["d", "l"]


def test_display_pins_display_card(monkeypatch, tmp_path):
    _setup(monkeypatch, tmp_path, "display\n")
    assert [d[-1] for d in hl.lane_devices()] == ["d"]
    assert hl.slot_count() == 1


def test_lane_pins_non_display_card(monkeypatch, tmp_path):
    _setup(monkeypatch, tmp_path, "LANE")
    assert [d[-1] for d in hl.lane_devices()] == ["l"]


def test_unknown_value_keeps_both(monkeypatch, tmp_path):
    _setup(monkeypatch, tmp_path, "nonsense")
    assert len(hl.lane_devices()) == 2
