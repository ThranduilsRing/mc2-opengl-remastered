"""Lane game runs route to the lane side of the GPU split (default 7900 XTX), gate runs to the gate side (2070 SUPER); a run that landed elsewhere is red."""
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
    for k in ("MC2_VK_DEVICE", "MC2_HARNESS_GPU", "MC2_IN_GATE"):
        monkeypatch.delenv(k, raising=False)
    monkeypatch.setattr(host_lease, "GPU_SPLIT_PATH", Path("Z:/no/such/GPU_SPLIT"))


def test_lane_runs_default_to_the_lane_gpu():
    assert _env()[K] == "AMD"
    assert _env(MC2_VK_GPU_TIMESTAMPS="1")[K] == "AMD"
    assert _env(MC2_FPS_CAP="0")[K] == "AMD"
    assert nofocus.apply_env({"PATH": "x"}, perf=True)[K] == "AMD"


def test_gate_runs_route_to_the_gate_gpu():
    assert _env(MC2_IN_GATE="1")[K] == "NVIDIA"


def test_explicit_override():
    assert _env(MC2_HARNESS_GPU="xtx")[K] == "AMD"
    assert K not in _env(MC2_HARNESS_GPU="any")
    assert _env(MC2_HARNESS_GPU="2070", MC2_VK_GPU_TIMESTAMPS="1")[K] == "NVIDIA"
    assert _env(**{K: "Intel"})[K] == "Intel"


def test_the_split_file_picks_the_sides(tmp_path, monkeypatch):
    split = tmp_path / "GPU_SPLIT"
    split.write_text("# swapped\ngate=AMD\nlane=NVIDIA\n", encoding="utf-8")
    monkeypatch.setattr(host_lease, "GPU_SPLIT_PATH", split)
    monkeypatch.delenv(host_lease.PATH_ENV, raising=False)   # a redirected lease (tests) ignores the real split
    assert _env()[K] == "NVIDIA"
    assert _env(MC2_IN_GATE="1")[K] == "AMD"


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


def test_the_spawned_child_lands_on_the_requested_gpu():
    env = {"PATH": "x", "SystemRoot": "C:\\Windows"}
    devices = _logged_devices(env)
    assert devices == ["AMD Radeon RX 7900 XTX"]
    assert nofocus.gpu_mismatch(env, False, devices) is None
    gate = {**env, "MC2_IN_GATE": "1"}
    devices = _logged_devices(gate)
    assert devices == ["NVIDIA GeForce RTX 2070 SUPER"]
    assert nofocus.gpu_mismatch(gate, False, devices) is None


def test_a_run_on_the_wrong_gpu_or_without_a_device_line_is_red():
    assert "2070" in nofocus.gpu_mismatch({"PATH": "x"}, False, ["NVIDIA GeForce RTX 2070 SUPER"])
    assert "no [VK_PROFILE]" in nofocus.gpu_mismatch({"PATH": "x"}, False, [])
    assert nofocus.gpu_mismatch({"PATH": "x", "MC2_HARNESS_GPU": "xtx"}, False, ["NVIDIA GeForce RTX 2070 SUPER"])
    assert nofocus.gpu_mismatch({"PATH": "x", "MC2_IN_GATE": "1"}, False, ["AMD Radeon RX 7900 XTX"])   # a gate run on the lane GPU
    assert nofocus.gpu_mismatch({"PATH": "x", "MC2_HARNESS_GPU": "any"}, False, ["AMD Radeon RX 7900 XTX"]) is None   # nothing asked for


def test_an_unmatched_implicit_hint_is_not_red_but_an_explicit_request_is():
    log = "MC2_VK_DEVICE=AMD matches no accepted device; using the default pick"
    assert nofocus.gpu_mismatch({"PATH": "x"}, False, ["Intel(R) HD Graphics 630"], log) is None
    assert nofocus.gpu_mismatch({"PATH": "x", "MC2_HARNESS_GPU": "2070"}, False, ["Intel(R) HD Graphics 630"], log)


def test_an_explicit_mistyped_device_hint_is_red_even_when_it_matched_nothing():
    log = "MC2_VK_DEVICE=NVDIA matches no accepted device; using the default pick"
    assert nofocus.gpu_mismatch({"PATH": "x", "MC2_VK_DEVICE": "NVDIA"}, False, ["Intel(R) HD Graphics 630"], log)
