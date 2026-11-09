"""Static checks for the frame GPU-hole fixes: the submit kick, the HZB per-slot ticket and the retired fence-defer kill-switches."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
G = ROOT / "GameOS" / "gameos"


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


def test_kick_flushes_pending_under_the_submit_lock():
    assert re.search(r"void kick\(\) const;", read(G / "vulkan_context.h"))
    body = re.search(r"void VulkanContext::kick\(\) const \{(.*?)\n\}", read(G / "vulkan_context.cpp"), re.S)
    assert body, "VulkanContext::kick is not defined"
    b = body.group(1)
    assert "assertOwner(mc2::overlap::Owner::Render" in b
    assert b.index("lockSubmit(submitMutex_)") < b.index("flushPendingLocked()")


def test_kick_follows_draw_screen_and_the_shadow_submit():
    main = read(G / "gameosmain.cpp")
    i = main.index("draw_screen();")
    assert ".kick();" in main[i:i + 1500], "no kick right after draw_screen"
    shadow = read(G / "vulkan_shadow_island_live.cpp")
    j = shadow.index("s->bake.markSubmitted(s->recordEpoch);")
    assert "sctx.kick();" in shadow[j:j + 300], "no kick right after the shadow island submit"


def test_hzb_waits_for_its_own_slot():
    src = read(G / "vulkan_post_hzb_native.cpp")
    assert re.search(r"uint64_t ticket\[mc2vk::kMaxFramesInFlight\]", src)
    assert re.search(r"OwnedBuffer instances\[mc2vk::kMaxFramesInFlight\]", src)
    assert re.search(r"VkDescriptorSet cullSet\[mc2vk::kMaxFramesInFlight\]", src)
    assert "GpuTicket{s->ticket[slot], 0}" in src
    assert "submit(si, VK_NULL_HANDLE, &s->ticket[slot])" in src
    assert not re.search(r"s->ticket[^\[\w]", src), "a scalar s->ticket use remains"
    # host reads of in-flight results are polled, never assumed complete
    assert "isComplete(mc2vk::GpuTicket{s->lastTicket, 0})" in src
    assert "isComplete(mc2vk::GpuTicket{pc.ticket, 0})" in src


def test_fence_defer_killswitches_are_gone():
    names = ["MC2_VK_%s_FENCE_DEFER" % n for n in ("MECH", "PROP", "TERRAIN", "WATER")]
    for p in list(G.glob("vulkan_*island*.cpp")) + list(G.glob("vulkan_*island*.h")) + [ROOT / "Gates" / "gate_rows_4.inc"]:
        text = read(p)
        assert "FenceDeferEnabled" not in text, p.name
        for n in names:
            assert n not in text, (p.name, n)
    retired = set(json.loads(read(ROOT / "Gates" / "retired_names.json")))
    assert set(names) <= retired


def test_dead_water_submit_once_is_gone():
    for p in G.glob("vulkan_water_island*"):
        assert "submit_once" not in read(p), p.name
