# scripts/smoke_lib/vram.py
"""The `[VRAM v1]` lines the engine prints at the end of mission load and at the end of a smoke run (GameOS/gameos/gpu_memory_report.cpp).

WHAT: `parse(text)` returns one `Sample` per line; `describe(sample)` is the one-line text a report shows; `used_mb(sample)` is the best "memory in
use" number the sample carries and where it came from.
WHY: a frame-time baseline that does not say how much GPU memory it ran at cannot tell a regression from a bigger working set, and a change that
doubles texture memory looks like nothing in fps. The line carries every counter the driver offers (-1 = that counter does not exist here):
the process's dedicated video memory from DXGI (any API) and the Vulkan Memory Allocator's budget and usage (the VK backend).
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Optional

LINE_RE = re.compile(r"\[VRAM v1\] phase=(?P<phase>\w+) backend=(?P<backend>\w+)(?P<rest>(?: \w+=-?\d+)*)\s*$")
FIELD_RE = re.compile(r"(\w+)=(-?\d+)")

#: Every numeric field of a v1 line, in the engine's order. Parsing is by name, so a later field added at the end does not break an older reader.
FIELDS = ("vk_budget_mb", "vk_usage_mb", "vk_allocated_mb", "adapter_dedicated_mb", "proc_dedicated_mb", "proc_budget_mb")


@dataclass
class Sample:
    phase: str
    backend: str
    values: dict = field(default_factory=dict)   # field name -> MiB, -1 when the engine reported it unavailable

    def get(self, name: str) -> Optional[int]:
        """The value in MiB, or None when the counter is unavailable or absent from the line."""
        v = self.values.get(name)
        return None if v is None or v < 0 else v


def parse(text: str) -> "list[Sample]":
    """Every well-formed `[VRAM v1]` line of `text`, in order. A line that does not parse (truncated mid-write) is skipped."""
    out = []
    for line in text.splitlines():
        at = line.find("[VRAM v1]")
        if at < 0:
            continue
        m = LINE_RE.search(line[at:])
        if not m:
            continue
        out.append(Sample(m.group("phase"), m.group("backend"), {k: int(v) for k, v in FIELD_RE.findall(m.group("rest"))}))
    return out


def used_mb(sample: Sample) -> "tuple[Optional[int], str]":
    """(MiB in use, source). Source order is the one a reader should trust: the process's own DXGI counter, then the Vulkan allocator's usage on a
    VK run. (None, 'none') when the line carries no usable counter."""
    proc = sample.get("proc_dedicated_mb")
    if proc is not None:
        return proc, "dxgi-process"
    if sample.backend == "vk" and sample.get("vk_usage_mb") is not None:
        return sample.get("vk_usage_mb"), "vma-usage"
    return None, "none"


def describe(sample: Sample) -> str:
    """`load_end gl: 5123 MiB in use (dxgi-process)`, plus the budget when the line has one."""
    used, source = used_mb(sample)
    text = f"{sample.phase} {sample.backend}: " + (f"{used} MiB in use ({source})" if used is not None else "no GPU memory counter available")
    budget = sample.get("proc_budget_mb")
    return text + (f", budget {budget} MiB" if budget is not None else "")
