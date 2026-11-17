"""Literal format emitters share the printf census's tag and key contract."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import log_tag_census as lc


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
