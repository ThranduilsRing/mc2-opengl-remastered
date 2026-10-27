"""Pass/fail harness runs route to the 2070 SUPER; perf runs keep the default GPU; a run that landed elsewhere is red."""
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from smoke_lib import nofocus, vk_profile  # noqa: E402

K = "MC2_VK_DEVICE"


def _env(**kw):
    return nofocus.apply_env({"PATH": "x", **kw})


def test_passfail_defaults_to_2070():
    assert _env()[K] == "NVIDIA"


def test_perf_keeps_default_gpu():
    assert K not in _env(MC2_VK_GPU_TIMESTAMPS="1")
    assert K not in _env(MC2_FPS_CAP="0")
    assert K not in nofocus.apply_env({"PATH": "x"}, perf=True)


def test_explicit_override():
    assert K not in _env(MC2_HARNESS_GPU="xtx")
    assert K not in _env(MC2_HARNESS_GPU="any")
    assert _env(MC2_HARNESS_GPU="2070", MC2_VK_GPU_TIMESTAMPS="1")[K] == "NVIDIA"
    assert _env(**{K: "Intel"})[K] == "Intel"


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
    assert devices == ["NVIDIA GeForce RTX 2070 SUPER"]
    assert nofocus.gpu_mismatch(env, False, devices) is None
    xtx = {**env, "MC2_HARNESS_GPU": "xtx"}
    devices = _logged_devices(xtx)
    assert devices == ["AMD Radeon RX 7900 XTX"]
    assert nofocus.gpu_mismatch(xtx, False, devices) is None


def test_a_run_on_the_wrong_gpu_or_without_a_device_line_is_red():
    assert "7900 XTX" in nofocus.gpu_mismatch({"PATH": "x"}, False, ["AMD Radeon RX 7900 XTX"])
    assert "no [VK_PROFILE]" in nofocus.gpu_mismatch({"PATH": "x"}, False, [])
    assert nofocus.gpu_mismatch({"PATH": "x", "MC2_HARNESS_GPU": "xtx"}, False, ["NVIDIA GeForce RTX 2070 SUPER"])
    assert nofocus.gpu_mismatch({"PATH": "x"}, True, ["AMD Radeon RX 7900 XTX"]) is None     # perf run: nothing asked for


def test_an_unmatched_implicit_hint_is_not_red_but_an_explicit_request_is():
    log = "MC2_VK_DEVICE=NVIDIA matches no accepted device; using the default pick"
    assert nofocus.gpu_mismatch({"PATH": "x"}, False, ["Intel(R) HD Graphics 630"], log) is None
    assert nofocus.gpu_mismatch({"PATH": "x", "MC2_HARNESS_GPU": "2070"}, False, ["Intel(R) HD Graphics 630"], log)
