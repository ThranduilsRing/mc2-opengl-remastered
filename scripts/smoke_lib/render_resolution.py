# scripts/smoke_lib/render_resolution.py
"""The one place that says what resolution a capture or a perf trace is taken at, and why.

WHAT: PINNED_WIDTH x PINNED_HEIGHT and the env pair that delivers them to the engine.
WHY: the engine's drawable size comes from options.cfg ResolutionX/Y unless MC2_WIDTH/MC2_HEIGHT are set. Every install on this host ships a
3840x2160 options.cfg, so captures and Tracy traces silently came out at 4K, and the same harness on a differently configured install would
have graded different pixels (golden regions are fractions of the frame and the diff resizes a mismatched crop). Pinning makes a capture or a
trace mean the same thing on every install and keeps its cost bounded: 1080p is a quarter of the pixels of 4K, which is the size a player on
the reference desktop actually runs.
WHO READS IT: scripts/run_visual_capture.py (so tools/visual_verify/capture.py, capture_ab.py and the golden/baseline capture runs) and
tools/tracy_capture_compare.py. The smoke runner (scripts/run_smoke.py tier1) is NOT pinned: it keeps options.cfg size on purpose, and the
`resolution` tier sets its own sizes from RESOLUTION_TIER_SIZES.
"""
from __future__ import annotations

PINNED_WIDTH = 1920
PINNED_HEIGHT = 1080

#: The sizes the weekly `resolution` smoke tier runs one mission at: the pinned size and the 4K size a stock install is configured for.
RESOLUTION_TIER_SIZES = ((PINNED_WIDTH, PINNED_HEIGHT), (3840, 2160))


def parse(text: str) -> "tuple[int, int]":
    """'WxH' -> (W, H); ValueError on anything else or a non-positive side."""
    w, sep, h = str(text).lower().replace(" ", "").partition("x")
    if not sep or not w.isdigit() or not h.isdigit() or int(w) <= 0 or int(h) <= 0:
        raise ValueError(f"malformed resolution {text!r} (want WxH)")
    return int(w), int(h)


def size_env(width: int = PINNED_WIDTH, height: int = PINNED_HEIGHT) -> "dict[str, str]":
    """The engine env that makes the drawable width x height."""
    return {"MC2_WIDTH": str(int(width)), "MC2_HEIGHT": str(int(height))}


def pinned_env() -> "dict[str, str]":
    return size_env()
