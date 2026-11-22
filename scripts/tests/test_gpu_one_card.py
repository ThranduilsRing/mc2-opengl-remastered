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
    monkeypatch.delenv("MC2_TEST_NO_ONE_CARD", raising=False)   # these tests exercise the switch itself
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


def test_one_visible_card_falls_back_to_one_slot(monkeypatch, tmp_path):
    """11-22: only one XTX visible -> runtime detection fails -> GPU_SPLIT; the switch must still leave one slot (review P1-1)."""
    _setup(monkeypatch, tmp_path, "display")
    monkeypatch.setattr(hl, "runtime_lane_adapter", lambda: None)
    split = tmp_path / "GPU_SPLIT"
    split.write_text("slot0=AMD" + chr(10) + "slot1=AMD" + chr(10), encoding="utf-8")
    monkeypatch.setattr(hl, "GPU_SPLIT_PATH", split)
    assert hl.lane_devices() == ("AMD",)
    assert hl.slot_count() == 1
    (tmp_path / "GPU_ONE_CARD").unlink()
    assert hl.lane_devices() == ("AMD", "AMD")


def test_named_card_missing_keeps_one_slot(monkeypatch, tmp_path):
    _setup(monkeypatch, tmp_path, "display")
    only_lane = [ROWS[1]]
    monkeypatch.setattr(hl, "_gpu_adapters", lambda: SimpleNamespace(adapters=lambda: only_lane,
                                                                    selector=lambda r: f"{r['name']}@{r['uuid']}"))
    assert [d[-1] for d in hl.lane_devices()] == ["l"]


def test_exclusive_hold_uses_switch_card(monkeypatch, tmp_path):
    """Review P1-2: an exclusive (gate) hold launched on the non-display card even with GPU_ONE_CARD=display."""
    _setup(monkeypatch, tmp_path, "display")
    monkeypatch.setattr(hl, "_records", lambda: [("x", {"exclusive": True})])
    monkeypatch.setattr(hl, "_ours", lambda rec: True)
    assert hl.held_device()[-1] == "d"
    (tmp_path / "GPU_ONE_CARD").unlink()
    assert hl.held_device()[-1] == "l"


def test_pytest_isolation_hides_live_file(monkeypatch, tmp_path):
    _setup(monkeypatch, tmp_path, "display")
    monkeypatch.setenv("MC2_TEST_NO_ONE_CARD", "1")
    assert hl.one_card_mode() == ""
    assert [d[-1] for d in hl.lane_devices()] == ["d", "l"]
