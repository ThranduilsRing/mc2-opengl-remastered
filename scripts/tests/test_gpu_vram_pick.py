"""GPU pick by least dedicated VRAM: PDH instance parsing and free-slot ordering (no GPUs needed)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from scripts.smoke_lib import gpu_adapters, host_lease  # noqa: E402

DEV = {0: "XTX@display", 1: "XTX@second"}
SECOND = "XTX@second"


def test_luid_from_instance_zero_high():
    assert gpu_adapters.luid_from_instance("luid_0x00000000_0x000144C7_phys_0") == b"\xc7\x44\x01\x00\x00\x00\x00\x00"


def test_luid_from_instance_nonzero_high_and_garbage():
    assert gpu_adapters.luid_from_instance("luid_0x00000001_0x0000ABCD_phys_1") == b"\xcd\xab\x00\x00\x01\x00\x00\x00"
    assert gpu_adapters.luid_from_instance("pid_123_luid_0x0_0x1_phys_0") is None
    assert gpu_adapters.luid_from_instance("luid_0xZZ_0x1_phys_0") is None


def test_prefers_lower_usage():
    # the non-display card is loaded (LLM), the display card is idle: display wins
    usage = {"XTX@display": 1 << 30, "XTX@second": 20 << 30}
    assert host_lease.order_free_slots([1, 0], DEV, usage, SECOND) == [0, 1]
    assert host_lease.order_free_slots([0, 1], DEV, {"XTX@display": 20 << 30, "XTX@second": 1 << 30}, SECOND) == [1, 0]


def test_tie_prefers_non_display():
    usage = {"XTX@display": 5, "XTX@second": 5}
    assert host_lease.order_free_slots([0, 1], DEV, usage, SECOND) == [1, 0]


def test_empty_or_partial_usage_is_old_order():
    for usage in ({}, {"XTX@display": 1}):
        assert host_lease.order_free_slots([0, 1], DEV, usage, SECOND) == [1, 0]
        assert host_lease.order_free_slots([0, 1], lambda i: DEV[i], usage, SECOND) == [1, 0]
