"""Pass/fail harness runs route to the 2070 SUPER; perf runs keep the default GPU."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from smoke_lib import nofocus  # noqa: E402

K = "VK_LOADER_DRIVERS_SELECT"


def _env(**kw):
    return nofocus.apply_env({"PATH": "x", **kw})


def test_passfail_defaults_to_2070():
    assert _env()[K] == "*nvoglv64*"


def test_perf_keeps_default_gpu():
    assert K not in _env(MC2_VK_GPU_TIMESTAMPS="1")
    assert K not in _env(MC2_FPS_CAP="0")
    assert K not in nofocus.apply_env({"PATH": "x"}, perf=True)


def test_explicit_override():
    assert K not in _env(MC2_HARNESS_GPU="xtx")
    assert _env(MC2_HARNESS_GPU="2070", MC2_VK_GPU_TIMESTAMPS="1")[K] == "*nvoglv64*"
    assert _env(**{K: "custom"})[K] == "custom"
