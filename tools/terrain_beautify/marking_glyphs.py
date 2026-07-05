#!/usr/bin/env python3
"""ROAD-MARKINGS-VECTOR-1: parametric glyph library for road/runway markings.

Recon: `.claude/ROAD-MARKINGS-VECTOR-RECON-1.md` section 4 ("VECTOR REAUTHOR
design"). Ruling (locked): SVG import is deferred; authoring uses a
PROCEDURAL-PILLOW parametric glyph DSL instead (zero new deps -- this machine
has no SVG rasterizer, but Pillow 12 + numpy are already the imaging stack
used by every other `tools/terrain_beautify/*.py` tool). Each glyph is a
plain Python function that renders directly to an alpha-native RGBA
`numpy.ndarray` at an arbitrary requested resolution (px-per-world-unit) --
this is what makes the source "lossless" (no fixed-size raster asset, no
tile-grid coupling): the SAME glyph function can be asked for 4 px/wu or
64 px/wu and always renders crisp.

Contract for every glyph function:
    glyph_fn(px_per_wu: float, color: tuple[int,int,int], **params) -> np.ndarray
        Returns HxWx4 uint8 RGBA, alpha-native (0 = transparent, NOT a
        magenta colorkey -- that legacy mechanism is explicitly retired by
        this slice, recon section 4 "leave the colorkey era behind").
        The glyph's local origin (world (0,0) for this placement) is at the
        array's geometric centre (W/2, H/2) unless documented otherwise, so
        callers can rotate/translate around a stable pivot.

Colors (recon "Author markings white/yellow" -- avoid the colorkey chroma
test `r>150 and b>150 and r>g+40` in terrtxm.cpp:459, which would silently
erase saturated magenta/pink; white/yellow are the two conventional airfield/
road paint colors and both are safely outside that test).
"""
from __future__ import annotations

import math
from typing import Tuple

import numpy as np
from PIL import Image, ImageDraw, ImageFont

WHITE = (255, 255, 255)
YELLOW = (255, 214, 0)

# --- glyph-placement JSON schema (markings.json) -----------------------------
# A markings.json file (per mission, `data/missions/<stem>.beauty/markings.json`
# offline source -- consumed by cook_markings.py, never read by the engine
# directly) is:
#   {"glyphs": [
#       {"glyph": "runway_centerline_dash", "x": 120.0, "y": -340.0,
#        "rotation_deg": 90.0, "scale": 1.0, "color": "white",
#        "params": {"length_wu": 12.0}},
#       ...
#   ]}
# `glyph` names the function key in GLYPHS below. `x`/`y` are world units
# (same convention as overlay_v2 bounds: +Y = north). `rotation_deg` rotates
# the rendered glyph raster counter-clockwise about its centre before
# compositing (screen-space CCW == world CCW under the row0=north, no-flip
# convention this toolchain uses throughout). `color` is "white" or "yellow"
# (anything else is rejected by validation -- the colorkey-erasure landmine).
COLOR_NAMES = {"white": WHITE, "yellow": YELLOW}


def _new_canvas(w: int, h: int) -> Image.Image:
    return Image.new("RGBA", (max(1, w), max(1, h)), (0, 0, 0, 0))


def _to_array(img: Image.Image) -> np.ndarray:
    return np.array(img, dtype=np.uint8)


# --- 1. road centerline dash (single) ----------------------------------------
def road_centerline_dash(px_per_wu: float, color=WHITE,
                          length_wu: float = 3.0, width_wu: float = 0.25) -> np.ndarray:
    """A single dash segment, long axis along local +X. Placement composes a
    run of these (with gaps) along a road spline; this glyph is just one dash."""
    w = max(1, round(length_wu * px_per_wu))
    h = max(1, round(width_wu * px_per_wu))
    img = _new_canvas(w, h)
    ImageDraw.Draw(img).rectangle([0, 0, w - 1, h - 1], fill=(*color, 255))
    return _to_array(img)


# --- 2. double/solid centerline ----------------------------------------------
def road_centerline_solid(px_per_wu: float, color=WHITE,
                           length_wu: float = 3.0, width_wu: float = 0.2,
                           double: bool = True, gap_wu: float = 0.15) -> np.ndarray:
    """A continuous (non-dashed) centerline segment; `double=True` draws two
    parallel solid lines (the no-passing convention) separated by `gap_wu`."""
    w = max(1, round(length_wu * px_per_wu))
    line_h = max(1, round(width_wu * px_per_wu))
    gap_h = max(0, round(gap_wu * px_per_wu))
    h = line_h if not double else (2 * line_h + gap_h)
    img = _new_canvas(w, h)
    draw = ImageDraw.Draw(img)
    draw.rectangle([0, 0, w - 1, line_h - 1], fill=(*color, 255))
    if double:
        draw.rectangle([0, line_h + gap_h, w - 1, h - 1], fill=(*color, 255))
    return _to_array(img)


# --- 3. lane edge line --------------------------------------------------------
def lane_edge_line(px_per_wu: float, color=WHITE,
                    length_wu: float = 3.0, width_wu: float = 0.15) -> np.ndarray:
    """Continuous solid edge-of-pavement stripe (thinner than the centerline)."""
    w = max(1, round(length_wu * px_per_wu))
    h = max(1, round(width_wu * px_per_wu))
    img = _new_canvas(w, h)
    ImageDraw.Draw(img).rectangle([0, 0, w - 1, h - 1], fill=(*color, 255))
    return _to_array(img)


# --- 4. dirt-road tread (subtle) ----------------------------------------------
def dirt_road_tread(px_per_wu: float, color=WHITE,
                     length_wu: float = 3.0, width_wu: float = 1.6,
                     track_width_wu: float = 0.3, alpha: int = 60) -> np.ndarray:
    """Two faint parallel tire-tread lines down an unpaved road -- low alpha,
    subtler than painted road/runway markings (dirt roads aren't painted)."""
    w = max(1, round(length_wu * px_per_wu))
    h = max(1, round(width_wu * px_per_wu))
    track_h = max(1, round(track_width_wu * px_per_wu))
    img = _new_canvas(w, h)
    draw = ImageDraw.Draw(img)
    draw.rectangle([0, 0, w - 1, track_h - 1], fill=(*color, alpha))
    draw.rectangle([0, h - track_h, w - 1, h - 1], fill=(*color, alpha))
    return _to_array(img)


# --- 5. runway centerline dashes ----------------------------------------------
def runway_centerline_dashes(px_per_wu: float, color=WHITE,
                              length_wu: float = 30.0, dash_wu: float = 6.0,
                              gap_wu: float = 4.0, width_wu: float = 0.9) -> np.ndarray:
    """A run of dashes along local +X spanning `length_wu`, dash/gap sized to
    real-world runway centerline proportions (dash notably longer than a road
    dash). One call renders the WHOLE dash run as a single raster -- placement
    composes one of these per straight runway segment, not one per dash."""
    w = max(1, round(length_wu * px_per_wu))
    h = max(1, round(width_wu * px_per_wu))
    img = _new_canvas(w, h)
    draw = ImageDraw.Draw(img)
    dash_px = max(1, round(dash_wu * px_per_wu))
    gap_px = max(1, round(gap_wu * px_per_wu))
    x = 0
    while x < w:
        x_end = min(w, x + dash_px)
        draw.rectangle([x, 0, x_end - 1, h - 1], fill=(*color, 255))
        x += dash_px + gap_px
    return _to_array(img)


# --- 6. runway threshold bars ("piano keys") ----------------------------------
def runway_threshold_bars(px_per_wu: float, color=WHITE,
                           runway_width_wu: float = 16.0, num_bars: int = 8,
                           bar_length_wu: float = 6.0, bar_width_wu: float = 0.9,
                           gap_wu: float = 0.6) -> np.ndarray:
    """The threshold "piano key" stripe pattern, oriented across the runway
    (local +Y = across, local +X = along runway). Bars are laid out
    side-by-side spanning `runway_width_wu`, each `bar_length_wu` long
    (parallel to the runway centerline)."""
    h = max(1, round(runway_width_wu * px_per_wu))
    w = max(1, round(bar_length_wu * px_per_wu))
    img = _new_canvas(w, h)
    draw = ImageDraw.Draw(img)
    bar_h = max(1, round(bar_width_wu * px_per_wu))
    gap_h = max(1, round(gap_wu * px_per_wu))
    period = bar_h + gap_h
    y = 0
    while y < h:
        y_end = min(h, y + bar_h)
        draw.rectangle([0, y, w - 1, y_end - 1], fill=(*color, 255))
        y += period
    return _to_array(img)


# --- 7. runway edge stripes ---------------------------------------------------
def runway_edge_stripes(px_per_wu: float, color=WHITE,
                         length_wu: float = 30.0, width_wu: float = 0.9) -> np.ndarray:
    """Continuous solid edge line along one side of the runway."""
    w = max(1, round(length_wu * px_per_wu))
    h = max(1, round(width_wu * px_per_wu))
    img = _new_canvas(w, h)
    ImageDraw.Draw(img).rectangle([0, 0, w - 1, h - 1], fill=(*color, 255))
    return _to_array(img)


# --- 8. runway numerals (0-9 + L/C/R) -----------------------------------------
_NUMERAL_GLYPHS = set("0123456789LCR")


def runway_numeral(px_per_wu: float, color=WHITE,
                    text: str = "27L", height_wu: float = 6.0) -> np.ndarray:
    """Renders a runway designator string (e.g. "09", "27L", "18C") as a single
    glyph raster. Characters restricted to 0-9 + L/C/R (recon glyph #8:
    "numerals 0-9 + L/C/R as a glyph set"). Uses PIL's built-in scalable
    default font (no external TTF dependency -- keeps this tool zero-new-deps
    per recon ruling #1) sized to `height_wu` world units tall."""
    bad = set(text.upper()) - _NUMERAL_GLYPHS
    if bad:
        raise ValueError(f"runway_numeral: unsupported characters {sorted(bad)} "
                          f"(only 0-9,L,C,R allowed)")
    px_h = max(8, round(height_wu * px_per_wu))
    font = ImageFont.load_default(size=px_h)
    # Measure then render on a tightly-fit transparent canvas.
    tmp = Image.new("RGBA", (1, 1), (0, 0, 0, 0))
    bbox = ImageDraw.Draw(tmp).textbbox((0, 0), text.upper(), font=font)
    w = max(1, bbox[2] - bbox[0])
    h = max(1, bbox[3] - bbox[1])
    img = _new_canvas(w, h)
    ImageDraw.Draw(img).text((-bbox[0], -bbox[1]), text.upper(), font=font, fill=(*color, 255))
    return _to_array(img)


# --- 9. runway aiming-point / touchdown blocks --------------------------------
def runway_aiming_point(px_per_wu: float, color=WHITE,
                         block_length_wu: float = 12.0, block_width_wu: float = 3.0,
                         gap_wu: float = 8.0) -> np.ndarray:
    """Two solid rectangular touchdown/aiming-point blocks flanking the
    centerline, `gap_wu` apart (local +X = along runway, local +Y = across)."""
    w = max(1, round(block_length_wu * px_per_wu))
    block_h = max(1, round(block_width_wu * px_per_wu))
    gap_px = max(1, round(gap_wu * px_per_wu))
    h = 2 * block_h + gap_px
    img = _new_canvas(w, h)
    draw = ImageDraw.Draw(img)
    draw.rectangle([0, 0, w - 1, block_h - 1], fill=(*color, 255))
    draw.rectangle([0, block_h + gap_px, w - 1, h - 1], fill=(*color, 255))
    return _to_array(img)


# --- 10. taxiway centerline (yellow) ------------------------------------------
def taxiway_centerline(px_per_wu: float, color=YELLOW,
                        length_wu: float = 20.0, width_wu: float = 0.5) -> np.ndarray:
    """Continuous solid yellow taxiway centerline (yellow is the standard
    taxiway-vs-runway-white paint discriminator)."""
    w = max(1, round(length_wu * px_per_wu))
    h = max(1, round(width_wu * px_per_wu))
    img = _new_canvas(w, h)
    ImageDraw.Draw(img).rectangle([0, 0, w - 1, h - 1], fill=(*color, 255))
    return _to_array(img)


# --- 11. crossing / intersection paint ----------------------------------------
def crossing_paint(px_per_wu: float, color=WHITE,
                    size_wu: float = 8.0, bar_width_wu: float = 0.5,
                    style: str = "x") -> np.ndarray:
    """Intersection paint for road/runway crossings (recon: "crossings (x_*)
    in scope"). `style="x"`: a diagonal cross spanning the cell -- this is
    exactly the composition case that legacy per-tile oriented markings could
    NOT handle correctly (recon failure (3): "per-tile oriented markings
    can't survive intersections"). Because this glyph is placed once in WORLD
    SPACE at the crossing's true center (not selected from a 15-tile
    orientation set), it composes correctly regardless of which two road
    families meet here."""
    n = max(1, round(size_wu * px_per_wu))
    bar_px = max(1, round(bar_width_wu * px_per_wu))
    img = _new_canvas(n, n)
    draw = ImageDraw.Draw(img)
    if style == "x":
        draw.line([(0, 0), (n - 1, n - 1)], fill=(*color, 255), width=bar_px)
        draw.line([(0, n - 1), (n - 1, 0)], fill=(*color, 255), width=bar_px)
    elif style == "plus":
        cx, cy = n // 2, n // 2
        draw.line([(cx, 0), (cx, n - 1)], fill=(*color, 255), width=bar_px)
        draw.line([(0, cy), (n - 1, cy)], fill=(*color, 255), width=bar_px)
    else:
        raise ValueError(f"crossing_paint: unknown style {style!r} (want 'x' or 'plus')")
    return _to_array(img)


GLYPHS = {
    "road_centerline_dash": road_centerline_dash,
    "road_centerline_solid": road_centerline_solid,
    "lane_edge_line": lane_edge_line,
    "dirt_road_tread": dirt_road_tread,
    "runway_centerline_dashes": runway_centerline_dashes,
    "runway_threshold_bars": runway_threshold_bars,
    "runway_edge_stripes": runway_edge_stripes,
    "runway_numeral": runway_numeral,
    "runway_aiming_point": runway_aiming_point,
    "taxiway_centerline": taxiway_centerline,
    "crossing_paint": crossing_paint,
}


def render_glyph(name: str, px_per_wu: float, color_name: str = "white", **params) -> np.ndarray:
    """Dispatch helper used by cook_markings.py: name -> RGBA ndarray."""
    if name not in GLYPHS:
        raise ValueError(f"unknown glyph {name!r}; available: {sorted(GLYPHS)}")
    if color_name not in COLOR_NAMES:
        raise ValueError(f"unknown color {color_name!r}; want 'white' or 'yellow'")
    return GLYPHS[name](px_per_wu, COLOR_NAMES[color_name], **params)


def rotate_glyph(rgba: np.ndarray, rotation_deg: float) -> np.ndarray:
    """Rotate a rendered glyph raster about its own centre, expanding the
    canvas so nothing is clipped. CCW positive, matching world +rotation
    convention (row0=north/no-flip, recon/control_map_tool.py row-order)."""
    if rotation_deg % 360.0 == 0.0:
        return rgba
    img = Image.fromarray(rgba, mode="RGBA")
    rotated = img.rotate(rotation_deg, expand=True, resample=Image.BICUBIC)
    return np.array(rotated, dtype=np.uint8)


def scale_glyph(rgba: np.ndarray, scale: float) -> np.ndarray:
    if scale == 1.0:
        return rgba
    img = Image.fromarray(rgba, mode="RGBA")
    w, h = img.size
    new_w = max(1, round(w * scale))
    new_h = max(1, round(h * scale))
    return np.array(img.resize((new_w, new_h), Image.BICUBIC), dtype=np.uint8)
