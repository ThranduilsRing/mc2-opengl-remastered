"""Literal format emitters share the printf census's tag and key contract."""
import sys
import json
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import log_tag_census as lc


def test_hdr_output_windows_state_key_matches_baseline():
    root = Path(__file__).resolve().parents[2]
    source = (root / "GameOS/gameos/engine_present_vk.cpp").read_text(encoding="utf-8")
    rows = lc.census_of_text(source)
    hdr = [key for key in rows if key[0] == "HDR_OUTPUT" and "policy" in key[3]]
    assert len(hdr) == 1
    assert hdr[0][3][-1] == "windows_hdr"
    baseline = json.loads((root / "tests/unit/log_tag_baseline.json").read_text(encoding="utf-8"))
    assert not lc.diff(lc.collections.Counter({hdr[0]: rows[hdr[0]]}),
                       {"emitters": [r for r in baseline["emitters"] if r["tag"] == "HDR_OUTPUT"]})


@pytest.mark.parametrize("callee,args", [
    ("std::format", ""), ("fmt::format", ""),
    ("std::format_to", "out, "), ("fmt::format_to", "out, "),
    ("std::format_to_n", "out, size, "), ("fmt::format_to_n", "out, size, "),
])
def test_format_literal_keys(callee, args):
    src = f'{callee}({args}"[VK_GPU_TRIS v1] pose={{}} frame={{}} n={{}} |{{}}\\n", pose, frame, n, tris);'
    assert lc.census_of_text(src) == {("VK_GPU_TRIS", 1, "buffer", ("pose", "frame", "n")): 1}


def test_multiline_format_with_adjacent_literals():
    src = '''auto line = std::format(
        "[PERF v2] frame={} "
        "ms={:.2f} {}={} {{literal=ok}}\\n",
        frame, ms, name, value);'''
    assert lc.census_of_text(src) == {("PERF", 2, "buffer", ("frame", "ms", "literal")): 1}


@pytest.mark.parametrize("src", [
    'std::format("hello frame={}", frame);',
    'std::format("prefix [PERF v1] frame={}", frame);',
    'std::format(pattern, frame);',
    'obj.format("[PERF v1] frame={}", frame);',
    '// std::format("[PERF v1] frame={}", frame);',
])
def test_non_tag_or_non_emitter_format_is_ignored(src):
    assert lc.census_of_text(src) == {}
