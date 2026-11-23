"""Source contract: HDR presentation must be ready before mission prewarm."""
from pathlib import Path
import re

SOURCE = Path(__file__).resolve().parents[2] / "GameOS/gameos/engine_present_vk.cpp"


def body(source, signature):
    start = source.index("{", source.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start + 1:end - 1]


def test_swapchain_shared_images_create_encode_before_present():
    source = SOURCE.read_text(encoding="utf-8")
    shared = body(source, "bool buildSharedImage(")
    assert "ensureHdrEncode(s)" in shared
    assert shared.index("s.imgW = w") < shared.index("ensureHdrEncode(s)")
    assert "if (!ensureHdrEncode(s))" in shared
    init = body(source, "bool EnginePresentVk::init(")
    assert "buildSharedImage(s," in init
    frame = body(source, "bool EnginePresentVk::presentFrameBody(")
    assert "(s.hdr && !prewarmPipelines())" not in frame


def test_frame_encode_reads_are_guarded_with_sdr_fallback():
    source = SOURCE.read_text(encoding="utf-8")
    frame = body(source, "bool EnginePresentVk::presentFrameBody(")
    assert "ensureHdrEncode(s)" not in frame  # never create a plain pipeline while recording
    guard = re.search(r"const bool encodeReady = (.*?);", frame, re.S)
    assert guard, "missing frame encode readiness guard"
    condition = guard.group(1)
    for resource in ("encodePass", "encodePipe", "encodeSet", "encodeLayout"):
        assert f"s.{resource}" in condition
    assert "imageIndex < s.encodeFrames.size()" in condition
    assert "s.encodeFrames[imageIndex]" in condition
    assert condition.index("imageIndex <") < condition.index("s.encodeFrames[imageIndex]")
    encoded = body(frame, "if (encodeReady)")
    for resource in ("encodeFrames", "encodePipe", "encodeSet"):
        reads = re.findall(rf"s\.{resource}\b", frame)
        guarded = re.findall(rf"s\.{resource}\b", condition + encoded)
        assert len(reads) == len(guarded), f"unguarded {resource} read"
    assert "[HDR_OUTPUT v1] encode_missing fallback=sdr" in frame
    assert "if (s.hdr && !encodeReady)" in frame
    assert "static bool logged" in body(frame, "if (s.hdr && !encodeReady)")
    assert re.search(r"if \(encodeReady\).*?\} else \{.*?vkCmdBlitImage", frame, re.S)
    assert "encodeReady ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_TRANSFER_BIT" in frame
