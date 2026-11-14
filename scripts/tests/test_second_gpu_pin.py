"""Two identical XTX cards: lane games and Blender are pinned to the non-display one by Vulkan deviceUUID (MC2_LANE_UUID / GPU_SPLIT lane_uuid)."""
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools" / "blender_pipeline"))
from smoke_lib import host_lease, nofocus  # noqa: E402
import blender_gpu  # noqa: E402
import blender_runner  # noqa: E402

SECOND = "00000000-2700-0000-0000-000000000000"
DISPLAY = "00000000-2a00-0000-0000-000000000000"


def test_pin_device_appends_uuid_only_to_amd_names(monkeypatch):
    monkeypatch.setattr(host_lease, "runtime_lane_adapter", lambda: None)
    monkeypatch.setattr(host_lease, "redirected", lambda: False)
    monkeypatch.setenv("MC2_LANE_UUID", SECOND)
    assert host_lease.pin_device("AMD") == f"AMD@{SECOND}"
    assert host_lease.pin_device("xtx") == f"xtx@{SECOND}"
    assert host_lease.pin_device("NVIDIA") == "NVIDIA"
    assert host_lease.pin_device(f"AMD@{DISPLAY}") == f"AMD@{DISPLAY}"
    assert host_lease.pin_device(None) is None
    monkeypatch.delenv("MC2_LANE_UUID")
    monkeypatch.setattr(host_lease, "GPU_SPLIT_PATH", Path("Z:/none"))
    assert host_lease.pin_device("AMD") == "AMD"


def test_pinned_device_still_picks_a_lease_slot(monkeypatch):
    monkeypatch.setattr(host_lease, "lane_devices", lambda: ("AMD", "AMD"))
    assert host_lease.eligible_slots(f"AMD@{SECOND}") == [0, 1]


def test_gpu_mismatch_checks_the_logged_uuid(monkeypatch):
    out = {"MC2_VK_DEVICE": f"AMD@{SECOND}"}
    good = f"[VK] [VK_DEVICE] index=1 of 2 uuid={SECOND} name=\"AMD Radeon RX 7900 XTX\""
    bad = good.replace(SECOND, DISPLAY)
    names = ["AMD Radeon RX 7900 XTX"]
    assert nofocus.gpu_mismatch(out, False, names, good) is None
    assert "uuid" in nofocus.gpu_mismatch(out, False, names, bad)
    assert "uuid" in nofocus.gpu_mismatch(out, False, names, "")


def test_gpu_split_accepts_old_files_and_the_new_key():
    assert host_lease._parse_gpu_split("slot0=AMD\nslot1=AMD\n") == ("AMD", "AMD")
    assert host_lease._parse_gpu_split("slot0=AMD\nlane_uuid=" + SECOND + "\n") == ("AMD", "NVIDIA")


def test_blender_bus_from_uuid_and_device_selection():
    assert blender_gpu.bus_of_uuid(SECOND) == 0x27
    devs = [("CPU", "CPU"), ("HIP_AMD Radeon RX 7900 XTX_0000:2a:00", "HIP"), ("HIP_AMD Radeon RX 7900 XTX_0000:27:00", "HIP")]
    assert blender_gpu.select_devices(devs, 0x27) == ["HIP_AMD Radeon RX 7900 XTX_0000:27:00"]
    assert blender_gpu.select_devices(devs, 0x31) == []
    with pytest.raises(ValueError):
        blender_gpu.bus_of_uuid("nonsense")


def test_every_runner_command_pins_the_gpu():
    cmd = blender_runner.build_blender_cmd("blender.exe", __file__, {})
    assert cmd.index(blender_runner.gpu_startup_args()[1]) < cmd.index(str(Path(__file__).resolve()))


@pytest.fixture
def detected_pair(monkeypatch):
    from smoke_lib import gpu_adapters
    rows = (dict(name="AMD Radeon RX 7900 XTX", uuid=DISPLAY, luid=b"display!", display=True),
            dict(name="AMD Radeon RX 7900 XTX", uuid=SECOND, luid=b"compute!", display=False))
    monkeypatch.setattr(gpu_adapters, "adapters", lambda: rows)
    monkeypatch.setattr(gpu_adapters.sys, "platform", "win32")
    monkeypatch.setattr(host_lease, "redirected", lambda: False)
    gpu_adapters.lane_adapter.cache_clear()
    yield rows
    gpu_adapters.lane_adapter.cache_clear()


@pytest.mark.parametrize("mode", ["auto", "any", "xtx", "2070"])
def test_every_pc_launch_uses_detected_non_display_card(detected_pair, mode):
    want = f"AMD Radeon RX 7900 XTX@{SECOND}"
    for perf in (False, True):
        out = {"MC2_HARNESS_GPU": mode, "MC2_VK_DEVICE": "NVIDIA"}
        nofocus.route_gpu(out, perf)
        assert out["MC2_VK_DEVICE"] == want
    assert host_lease.device_hint({}) == want
    assert host_lease.device_on_host(want, ["amd radeon rx 7900 xtx"])


def test_identical_cards_have_distinct_uuid_lease_slots(detected_pair):
    assert len(host_lease.lane_devices()) == 2
    for index, row in enumerate(detected_pair):
        selector = f"{row['name']}@{row['uuid']}"
        with host_lease.keyed_by(selector):
            assert host_lease.eligible_slots(selector) == [index]
    assert host_lease.eligible_slots(host_lease.device_hint({})) == [1]


def test_merge_gate_stock_and_aux_rows_use_non_display_uuid(detected_pair):
    import merge_gate
    want = f"AMD Radeon RX 7900 XTX@{SECOND}"
    for row in ("tier1", "capture", "battle-bench", "fault-injection"):
        for requested in (None, "NVIDIA", f"AMD@{DISPLAY}", merge_gate.aux_device()):
            assert merge_gate.gate_env(row, requested) == {"MC2_IN_GATE": "1", "MC2_VK_DEVICE": want}
    assert merge_gate.gate_env("pytest") == {}


@pytest.mark.parametrize("failure", ["single", "unknown", "exception"])
def test_detection_failure_warns_and_keeps_default(monkeypatch, capsys, failure):
    from smoke_lib import gpu_adapters
    monkeypatch.setattr(gpu_adapters.sys, "platform", "win32")
    def enumerate_rows():
        if failure == "exception":
            raise OSError("DXGI unavailable")
        return [dict(name="AMD Radeon RX 7900 XTX", display=None if failure == "unknown" else True)]
    monkeypatch.setattr(gpu_adapters, "adapters", enumerate_rows)
    gpu_adapters.lane_adapter.cache_clear()
    try:
        assert gpu_adapters.lane_adapter() is None
        assert "WARNING" in capsys.readouterr().err
    finally:
        gpu_adapters.lane_adapter.cache_clear()


def test_blender_uses_detected_uuid_without_config(detected_pair, monkeypatch):
    monkeypatch.delenv("BLENDER_GPU_UUID", raising=False)
    monkeypatch.delenv("MC2_LANE_UUID", raising=False)
    assert blender_gpu.lane_uuid() == SECOND
