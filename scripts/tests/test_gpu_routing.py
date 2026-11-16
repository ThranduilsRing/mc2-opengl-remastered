"""Two lane GPUs, no gate GPU: a run is routed by the lease slot it wins (slot 0 = 7900 XTX, slot 1 = 2070 SUPER) unless a device is forced; a run that landed elsewhere than forced is red."""
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pytest  # noqa: E402
from smoke_lib import host_lease, nofocus, vk_profile  # noqa: E402

K = "MC2_VK_DEVICE"


def _env(**kw):
    return nofocus.apply_env({"PATH": "x", **kw})


@pytest.fixture(autouse=True)
def _clean_gpu_env(monkeypatch):
    monkeypatch.setattr(host_lease, "runtime_lane_adapter", lambda: None)
    for k in ("MC2_VK_DEVICE", "MC2_HARNESS_GPU", "MC2_IN_GATE"):
        monkeypatch.delenv(k, raising=False)
    monkeypatch.setattr(host_lease, "GPU_SPLIT_PATH", Path("Z:/no/such/GPU_SPLIT"))


def test_an_unforced_run_asks_for_no_device_the_lease_slot_decides():
    assert K not in _env()
    assert K not in _env(MC2_VK_GPU_TIMESTAMPS="1")
    assert K not in _env(MC2_IN_GATE="1")                      # no gate GPU on the PC any more
    assert K not in nofocus.apply_env({"PATH": "x"}, perf=True)


def test_explicit_override():
    assert _env(MC2_HARNESS_GPU="xtx")[K] == "AMD"
    assert K not in _env(MC2_HARNESS_GPU="any")
    assert _env(MC2_HARNESS_GPU="2070", MC2_VK_GPU_TIMESTAMPS="1")[K] == "NVIDIA"
    assert _env(**{K: "Intel"})[K] == "Intel"


def test_the_split_file_orders_the_slots(tmp_path, monkeypatch):
    split = tmp_path / "GPU_SPLIT"
    split.write_text("# swapped\nslot0=NVIDIA\nslot1=AMD\n", encoding="utf-8")
    monkeypatch.setattr(host_lease, "GPU_SPLIT_PATH", split)
    monkeypatch.delenv(host_lease.PATH_ENV, raising=False)   # a redirected lease (tests) ignores the real split
    assert host_lease.lane_devices() == ("NVIDIA", "AMD")
    assert host_lease.slot_device(0) == "NVIDIA" and host_lease.slot_device(1) == "AMD"


# The engine's side of the contract, run through the real nofocus.spawn: the child sees the routed env and logs the device it "picked" the way
# vulkan_context.cpp does (device whose name contains MC2_VK_DEVICE, else the first discrete one, the XTX).
_FAKE_ENGINE = (
    "import os\n"
    "devices = ['AMD Radeon RX 7900 XTX', 'NVIDIA GeForce RTX 2070 SUPER']\n"
    "hint = os.environ.get('MC2_VK_DEVICE', '').lower()\n"
    "pick = next((d for d in devices if hint and hint in d.lower()), devices[0])\n"
    "print('[VK_PROFILE v1] verdict=ok device=\"%s\" api=1.4.0 instance_api=1.4.0 floor=1.2 required_missing=- wanted_missing=- items=- enabled=- extensions=-' % pick)\n"
)


def _logged_devices(env):
    proc = nofocus.spawn([sys.executable, "-c", _FAKE_ENGINE], stdout=subprocess.PIPE, text=True, env=env)
    out, _ = proc.communicate(timeout=60)
    return [p.device for p in vk_profile.parse(out)]


def test_the_spawned_child_lands_on_the_forced_gpu():
    env = {"PATH": "x", "SystemRoot": "C:\\Windows"}
    forced = {**env, K: "NVIDIA"}
    devices = _logged_devices(forced)
    assert devices == ["NVIDIA GeForce RTX 2070 SUPER"]
    assert nofocus.gpu_mismatch(forced, False, devices) is None
    xtx = {**env, "MC2_HARNESS_GPU": "xtx"}
    devices = _logged_devices(xtx)
    assert devices == ["AMD Radeon RX 7900 XTX"]
    assert nofocus.gpu_mismatch(xtx, False, devices) is None


def test_a_run_on_the_wrong_gpu_or_without_a_device_line_is_red():
    assert nofocus.gpu_mismatch({"PATH": "x"}, False, ["NVIDIA GeForce RTX 2070 SUPER"]) is None   # nothing forced: either lane GPU is right
    assert "no [VK_PROFILE]" in nofocus.gpu_mismatch({"PATH": "x", "MC2_HARNESS_GPU": "xtx"}, False, [])
    assert nofocus.gpu_mismatch({"PATH": "x", "MC2_HARNESS_GPU": "xtx"}, False, ["NVIDIA GeForce RTX 2070 SUPER"])
    assert nofocus.gpu_mismatch({"PATH": "x", "MC2_HARNESS_GPU": "2070"}, False, ["AMD Radeon RX 7900 XTX"])
    assert nofocus.gpu_mismatch({"PATH": "x", "MC2_HARNESS_GPU": "any"}, False, ["AMD Radeon RX 7900 XTX"]) is None   # nothing asked for


def test_an_explicit_request_that_matched_nothing_is_red():
    log = "MC2_VK_DEVICE=AMD matches no accepted device; using the default pick"
    assert nofocus.gpu_mismatch({"PATH": "x", "MC2_HARNESS_GPU": "2070"}, False, ["Intel(R) HD Graphics 630"], log)


def test_an_explicit_mistyped_device_hint_is_red_even_when_it_matched_nothing():
    log = "MC2_VK_DEVICE=NVDIA matches no accepted device; using the default pick"
    assert nofocus.gpu_mismatch({"PATH": "x", "MC2_VK_DEVICE": "NVDIA"}, False, ["Intel(R) HD Graphics 630"], log)


def test_host_lane_device_follows_the_gpus_the_host_has():
    from smoke_lib import host_lease
    pc = ["amd radeon rx 7900 xtx", "nvidia geforce rtx 2070 super"]
    laptop = ["nvidia geforce gtx 1050 ti", "intel(r) hd graphics 630"]
    split = ("AMD", "NVIDIA")
    assert host_lease.host_lane_device(split, pc) == "AMD"          # the PC gate still selects the XTX
    assert host_lease.host_lane_device(split, laptop) == "NVIDIA"   # the laptop never gets AMD forced
    assert host_lease.host_lane_device(split, ["intel(r) hd graphics 630"]) is None
    assert host_lease.host_lane_device(split, []) == "AMD"          # unknown host: slot 0, as before


def test_gate_env_never_forces_a_missing_gpu(monkeypatch):
    import merge_gate as gate
    from smoke_lib import host_lease
    monkeypatch.setattr(host_lease, "host_gpu_names", lambda: ["nvidia geforce gtx 1050 ti"])
    assert gate.gate_env("tier1")["MC2_VK_DEVICE"] == "NVIDIA"
    assert gate.gate_env("tier1", "AMD")["MC2_VK_DEVICE"] == "NVIDIA"   # an aux/explicit AMD the host lacks falls back
    monkeypatch.setattr(host_lease, "host_gpu_names", lambda: ["intel(r) hd graphics 630"])
    assert "MC2_VK_DEVICE" not in gate.gate_env("tier1")
    monkeypatch.setattr(host_lease, "host_gpu_names", lambda: ["amd radeon rx 7900 xtx"])
    assert gate.gate_env("tier1")["MC2_VK_DEVICE"] == "AMD"
