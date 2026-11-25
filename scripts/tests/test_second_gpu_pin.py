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
    assert host_lease._parse_gpu_split("slot0=AMD\nlane_uuid=" + SECOND + "\n") == ("AMD", "AMD")


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
def detected_pair(monkeypatch, tmp_path):
    from smoke_lib import gpu_adapters
    rows = (dict(name="AMD Radeon RX 7900 XTX", uuid=DISPLAY, luid=b"display!", display=True),
            dict(name="AMD Radeon RX 7900 XTX", uuid=SECOND, luid=b"compute!", display=False))
    monkeypatch.setattr(gpu_adapters, "adapters", lambda: rows)
    monkeypatch.setattr(host_lease, "_gpu_adapters", lambda: gpu_adapters)
    monkeypatch.setattr(gpu_adapters.sys, "platform", "win32")
    monkeypatch.setattr(host_lease, "redirected", lambda: False)
    for key in (host_lease.BATCH_ENV, "MC2_VK_DEVICE", "MC2_LANE_UUID", "MC2_HARNESS_GPU", "MC2_SLOT"):
        monkeypatch.delenv(key, raising=False)
    monkeypatch.setenv("MC2_HOST_LEASE_PATH", str(tmp_path / "lease.json"))
    monkeypatch.setattr(host_lease, "GPU_SPLIT_PATH", tmp_path / "GPU_SPLIT")
    monkeypatch.setattr(host_lease, "ONE_CARD_PATH", tmp_path / "GPU_ONE_CARD")   # the live host kill-switch must not leak in
    monkeypatch.setattr(host_lease, "QUIET_PATH", tmp_path / "GATE_QUIET.json")     # a running gate's capture-stage marker must not leak in
    monkeypatch.setattr(host_lease, "runtime_lane_adapter", lambda: rows[1])
    gpu_adapters.lane_adapter.cache_clear()
    yield rows
    gpu_adapters.lane_adapter.cache_clear()


@pytest.mark.parametrize("mode", ["auto", "any", "xtx", "2070"])
def test_every_pc_launch_defers_to_the_acquired_slot(detected_pair, mode, monkeypatch):
    monkeypatch.setattr(host_lease, "held_device", lambda: None)
    for perf in (False, True):
        out = {"MC2_HARNESS_GPU": mode, "MC2_VK_DEVICE": "NVIDIA"}
        nofocus.route_gpu(out, perf)
        assert "MC2_VK_DEVICE" not in out
    assert host_lease.device_hint({}) is None
    for uuid in (SECOND, DISPLAY):
        want = f"AMD Radeon RX 7900 XTX@{uuid}"
        monkeypatch.setattr(host_lease, "held_device", lambda: want)
        out = {"MC2_VK_DEVICE": "NVIDIA"}
        nofocus.route_gpu(out)
        assert out["MC2_VK_DEVICE"] == want


def test_identical_cards_have_distinct_uuid_lease_slots(detected_pair):
    assert len(host_lease.lane_devices()) == 2
    for index, row in enumerate(detected_pair):
        selector = f"{row['name']}@{row['uuid']}"
        with host_lease.keyed_by(selector):
            assert host_lease.eligible_slots(selector) == [index]
    assert host_lease.eligible_slots(host_lease.device_hint({})) == [0, 1]


def test_merge_gate_stock_and_aux_rows_defer_to_the_lease(detected_pair):
    import merge_gate
    for row in ("tier1", "capture", "battle-bench", "fault-injection"):
        for requested in (None, "NVIDIA", f"AMD@{DISPLAY}", merge_gate.aux_device()):
            assert {k: v for k, v in merge_gate.gate_env(row, requested).items() if k != "MC2_LANE_SCRATCH"} == {"MC2_IN_GATE": "1"}
    assert merge_gate.gate_env("pytest") == {}


@pytest.mark.skipif(sys.platform != "win32", reason="acquire uses msvcrt locking")
@pytest.mark.parametrize("image", [None, "mc2.exe", "Mission Editor.exe"])
def test_owner_game_reserves_only_display_slot(detected_pair, monkeypatch, image):
    monkeypatch.setattr(host_lease, "_refresh_game_affinity", lambda: None)
    monkeypatch.setattr(host_lease.child_registry, "kill_if_orphan", lambda *a: pytest.fail("touched owner"))
    try:
        host_lease.acquire("lane", wait=False, scan=lambda: [(4242, image)] if image else [], kill_orphans=True)
        assert host_lease.held_device() == f"AMD Radeon RX 7900 XTX@{SECOND}"
        assert not host_lease._read(host_lease.lease_path().with_suffix(".queue.json"))["tickets"]
    finally:
        host_lease.release()


@pytest.mark.skipif(sys.platform != "win32", reason="acquire uses msvcrt locking")
@pytest.mark.parametrize("owner,exclusive", [(False, False), (True, False), (True, True)])
def test_second_lane_uses_display_only_without_owner(detected_pair, monkeypatch, owner, exclusive):
    import os
    monkeypatch.setattr(host_lease, "_refresh_game_affinity", lambda: None)
    monkeypatch.setattr(host_lease, "_managed_engine", lambda pid, holders: pid == 4343)
    host_lease._write_record(host_lease.slot_path(1), dict(
        pid=os.getppid(), slot=1, device=f"AMD Radeon RX 7900 XTX@{SECOND}",
        affinity_mask=host_lease.affinity_mask(1), since=0))
    try:
        scan = lambda: [(4343, "mc2.exe")] + ([(4242, "mc2.exe")] if owner else [])
        if owner or exclusive:
            with pytest.raises(host_lease.HostLeaseBusy):
                host_lease.acquire("second lane", wait=False, scan=scan, exclusive=exclusive)
        else:
            host_lease.acquire("second lane", wait=False, scan=scan)
            assert host_lease.held_device() == f"AMD Radeon RX 7900 XTX@{DISPLAY}"
    finally:
        host_lease.release()
        host_lease._remove_records([(p, r) for p, r in host_lease._records() if r])


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


def test_holder_script_mode_imports_gpu_adapters():
    """The batch holder runs `python host_lease.py _holder` (no package): its gpu_adapters import must still resolve."""
    import subprocess
    lease = Path(__file__).resolve().parents[1] / "smoke_lib" / "host_lease.py"
    code = ("import runpy, sys; sys.argv=['host_lease.py']; "
            f"g = runpy.run_path({str(lease)!r}, run_name='not_main'); "
            "print(g['_gpu_adapters']().__name__)")
    import console_child
    out = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, cwd=str(lease.parent),
                         **console_child.child_kwargs(redirected=True))
    assert out.returncode == 0, out.stderr
    assert out.stdout.strip().endswith("gpu_adapters")
