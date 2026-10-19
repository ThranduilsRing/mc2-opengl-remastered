"""The lifetime checker rejects removed/misordered native synchronization."""
from pathlib import Path
import importlib.util
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('buffer_lifetime',ROOT/'scripts/check-buffer-lifetime-ownership.py')
checker=importlib.util.module_from_spec(spec);spec.loader.exec_module(checker)

def source(): return checker.NATIVE_CULL.read_text(encoding='utf-8')
def test_live_native_producer_has_slot_and_visibility_guards():
    assert checker.native_cull_hazards(source())==[]
def test_a_comment_cannot_replace_the_upload_ticket_guard():
    text=source();guard='if (slot.submitted && !ctx.isComplete(slot.ticket)) return NativeCullSubmit::Busy;'
    assert any('CPU overwrite' in e for e in checker.native_cull_hazards(text.replace(guard,'// '+guard)))
def test_a_guard_after_the_upload_is_too_late():
    text=source();guard='if (slot.submitted && !ctx.isComplete(slot.ticket)) return NativeCullSubmit::Busy;'
    text=text.replace(guard,'').replace('if (vkBeginCommandBuffer',guard+'\n    if (vkBeginCommandBuffer')
    assert any('CPU overwrite' in e for e in checker.native_cull_hazards(text))
def test_mapping_before_completion_is_rejected():
    text=source().replace('!ctx.isComplete(slot.ticket) || visibility.size()', 'false || visibility.size()')
    assert any('readback mapping' in e for e in checker.native_cull_hazards(text))
def test_removing_the_compute_copy_barrier_is_rejected():
    text=source().replace('VK_ACCESS_TRANSFER_READ_BIT);','VkAccessFlags(0));')
    assert any('compute-write' in e for e in checker.native_cull_hazards(text))
def test_removing_host_visibility_is_rejected():
    text=source().replace('VK_ACCESS_HOST_READ_BIT);','VkAccessFlags(0));')
    assert any('host-read' in e for e in checker.native_cull_hazards(text))
def test_device_wide_barriers_for_private_buffers_are_rejected():
    text=source().replace('vkCmdFillBuffer(slot.cb', 'mc2vk::cmdBarrier(slot.cb,0,0,0,0); vkCmdFillBuffer(slot.cb')
    assert any('device-wide' in e for e in checker.native_cull_hazards(text))
