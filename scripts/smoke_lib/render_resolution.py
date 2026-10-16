# scripts/smoke_lib/render_resolution.py
"""The one place that says what resolution a capture or a perf trace is taken at, and why.

WHAT: PINNED_WIDTH x PINNED_HEIGHT and the env pair that delivers them to the engine.
WHY: the engine's drawable size comes from options.cfg ResolutionX/Y unless MC2_WIDTH/MC2_HEIGHT are set. Every install on this host ships a
3840x2160 options.cfg, so captures and Tracy traces silently came out at 4K, and the same harness on a differently configured install would
have graded different pixels (golden regions are fractions of the frame and the diff resizes a mismatched crop). Pinning makes a capture or a
trace mean the same thing on every install and keeps its cost bounded: 1080p is a quarter of the pixels of 4K, which is the size a player on
the reference desktop actually runs.
WHO READS IT: scripts/run_visual_capture.py (so tools/visual_verify/capture.py, capture_ab.py and the golden/baseline capture runs),
tools/tracy_capture_compare.py and the smoke runner (scripts/run_smoke.py) for the tiers in PINNED_TIERS (tier1: its fps numbers are a baseline,
so they are taken at the same size as the captures and the traces; before 2026-10-13 they were whatever the install's options.cfg said, which is 4K).
The `resolution` tier sets its own sizes from RESOLUTION_TIER_SIZES; the other tiers keep the options.cfg size.
"""
from __future__ import annotations

from typing import Mapping, Optional

PINNED_WIDTH = 1920
PINNED_HEIGHT = 1080

#: The VK golden set (tests/visual/goldens/<name>) blessed at the pinned size; `verify.py --golden current` and `check-capture-resolution.py --current`
#: resolve to it. The GL sets (`golden-e9e3f19d6-1080p`, `reset-50331d5`) are retired (tests/visual/goldens/RETIRED.md): a GL capture cannot grade against it.
CURRENT_GOLDEN = "golden-d9e1f3801716-vk-1080p"

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


#: The smoke tiers whose runs are taken at the pinned size (the per-entry `resolution=` of the `resolution` tier still wins).
PINNED_TIERS = ("tier1",)


def asked_size(tier: str, entry_resolution: str = "") -> "Optional[tuple[int, int]]":
    """The (width, height) a smoke entry asks the engine for: its own `resolution=` when it has one, the pinned size for a pinned tier, else None
    (the install's options.cfg size applies)."""
    if entry_resolution:
        return parse(entry_resolution)
    return (PINNED_WIDTH, PINNED_HEIGHT) if tier in PINNED_TIERS else None


def tier_size_env(tier: str, entry_resolution: str = "") -> "dict[str, str]":
    """The engine env for `asked_size`, empty when the entry asks for no size."""
    size = asked_size(tier, entry_resolution)
    return size_env(*size) if size else {}


SIZE_KEYS = ("MC2_WIDTH", "MC2_HEIGHT")


class HalfDeclaredSize(ValueError):
    """A caller declared MC2_WIDTH or MC2_HEIGHT but not both."""


def pin_unless_declared(env: "dict[str, str]", declared: "Optional[Mapping[str, str]]") -> "dict[str, str]":
    """Put the pinned size into `env` unless the caller declared one (--env / --env-declare). A size is a pair: declaring one side alone would
    give a mixed size (2560 wide beside the pinned 1080 high) that nobody asked for, so that is refused instead of guessed."""
    given = [k for k in SIZE_KEYS if k in (declared or {})]
    if len(given) == 1:
        raise HalfDeclaredSize(f"{given[0]} is declared without {SIZE_KEYS[1 - SIZE_KEYS.index(given[0])]}: declare both (a size is a pair) or neither "
                               f"(the pinned {PINNED_WIDTH}x{PINNED_HEIGHT} applies)")
    if not given:
        env.update(pinned_env())
    return env
