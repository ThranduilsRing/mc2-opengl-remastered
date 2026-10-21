"""The shared cell-level JSON format and small grid operations (no game writes)."""
from __future__ import annotations

import json
import math
from collections import deque
from pathlib import Path

import numpy as np

SCHEMA = "mc2-mapgen-1"
CELL_WU = 128 / 3
STEEP_DEG = 35.0
FLAT_DEG = 10.0
INSTALL_ROOT = Path("A:/Games/mc2-opengl")
LAYERS = ("roads", "cement", "water", "cliffs", "runway")


def output_path(path: str | Path, protected=()) -> Path:
    """Refuse output inside source installs, including through symlinks."""
    result = Path(path).resolve()
    for root in (INSTALL_ROOT, *protected):
        if result.is_relative_to(Path(root).resolve()):
            raise ValueError(f"output would modify a read-only install: {result}")
    return result


def write_json(path, value, *, pretty=False):
    path = output_path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # A replacement prevents an interrupted extraction leaving a half-written mission.
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, sort_keys=True, allow_nan=False,
                              indent=2 if pretty else None,
                              separators=None if pretty else (",", ":")) + "\n",
                   encoding="utf-8")
    tmp.replace(path)


def load_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def world_cell(x, y, vertices):
    half = vertices * 64.0
    return [int((half - y) * 3 / 128), int((x + half) * 3 / 128)]


def packed_cell(cell):
    return (int(cell[0]) << 16) | int(cell[1])


def slope_grid(height, spacing=CELL_WU):
    height = np.asarray(height, dtype=float)
    if height.ndim != 2 or min(height.shape) < 2 or not np.isfinite(height).all():
        raise ValueError("heightfield must be a finite 2D grid, at least 2x2")
    dr, dc = np.gradient(height, spacing)
    return np.degrees(np.arctan(np.hypot(dr, dc)))


def spans(mask, scale=1, shape=None):
    """Boolean raster -> [row, first_col, end_col_exclusive] in terrain cells."""
    mask = np.asarray(mask, dtype=bool)
    rows, cols = shape or (mask.shape[0] * scale, mask.shape[1] * scale)
    out = []
    for r, line in enumerate(mask):
        edges = np.flatnonzero(np.diff(np.r_[False, line, False]))
        for first, end in zip(edges[::2], edges[1::2]):
            for row in range(r * scale, min((r + 1) * scale, rows)):
                out.append([row, int(first) * scale, min(int(end) * scale, cols)])
    return sorted(out)


def mask_of(layout, name):
    result = np.zeros((layout["size"]["rows"], layout["size"]["cols"]), dtype=bool)
    for r, first, end in layout["layers"].get(name, []):
        result[r, first:end] = True
    return result


def terrain_at(layout, field, cell):
    terrain = layout["terrain"]
    grid = terrain[field]
    step = terrain["cell_step"]
    r = min(max(int(cell[0]) // step, 0), len(grid) - 1)
    c = min(max(int(cell[1]) // step, 0), len(grid[0]) - 1)
    return grid[r][c]


def distance(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def building_role(obj):
    if "role" in obj:
        return obj["role"]
    name = (obj.get("type_name", "") + " " + obj.get("appearance", "")).lower()
    groups = (("airfield", ("hangar", "airfield", "controltower", "runway")),
              ("hq", ("_hq", "headquarter", "command", "sensor", "lookout")),
              ("industrial", ("factory", "tank", "fuel", "pipe", "refinery", "power", "generator", "warehouse", "storage", "mine", "industrial")),
              ("town", ("house", "apartment", "office", "shop", "church", "hotel", "shack", "skyscraper", "residen", "commercial")))
    for role, words in groups:
        if any(word in name for word in words):
            return role
    return "military"


def components(mask, diagonal=False):
    """Return deterministic components as cell lists, largest first."""
    remaining = np.array(mask, dtype=bool, copy=True)
    rows, cols = remaining.shape
    directions = ((-1, 0), (0, -1), (0, 1), (1, 0))
    if diagonal:
        directions += ((-1, -1), (-1, 1), (1, -1), (1, 1))
    result = []
    for r, c in zip(*np.nonzero(remaining)):
        if not remaining[r, c]:
            continue
        remaining[r, c] = False
        queue = deque([(int(r), int(c))])
        cells = []
        while queue:
            rr, cc = queue.popleft()
            cells.append((rr, cc))
            for dr, dc in directions:
                nr, nc = rr + dr, cc + dc
                if 0 <= nr < rows and 0 <= nc < cols and remaining[nr, nc]:
                    remaining[nr, nc] = False
                    queue.append((nr, nc))
        result.append(cells)
    return sorted(result, key=len, reverse=True)


def distance_field(mask):
    """Eight-neighbour distance in cells; a documented Chebyshev proxy."""
    rows, cols = mask.shape
    result = np.full(mask.shape, rows + cols, dtype=np.int32)
    result[mask] = 0
    # Two raster passes give the exact chessboard distance transform.
    index = np.arange(cols, dtype=np.int32)
    for order, dr in ((range(rows), -1), (range(rows-1, -1, -1), 1)):
        for r in order:
            previous = r + dr
            if 0 <= previous < rows:
                row = result[previous]
                neighbours = np.minimum(row, np.r_[row[0], row[:-1]])
                neighbours = np.minimum(neighbours, np.r_[row[1:], row[-1]])
                result[r] = np.minimum(result[r], neighbours + 1)
            row = np.minimum.accumulate(result[r] - index) + index
            result[r] = (np.minimum.accumulate((row + index)[::-1])[::-1] - index)
    return result
