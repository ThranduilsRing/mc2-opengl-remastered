# tools/terrain_gen/material_classifier.py
from __future__ import annotations
from dataclasses import dataclass
from enum import IntEnum

import numpy as np
from PIL import Image, ImageFilter
from opensimplex import OpenSimplex

from terrain_gen.terrain_recipe import TerrainRecipe


class TerrainType(IntEnum):
    BLUE_WATER   = 0
    GREEN_WATER  = 1
    MUD          = 2
    MOSS         = 3
    DIRT         = 4
    ASH          = 5
    MOUNTAIN     = 6
    TUNDRA       = 7
    FOREST_FLOOR = 8
    GRASS        = 9
    CONCRETE     = 10
    CLIFF        = 11
    SLIMY        = 12
    NONE         = 20


@dataclass
class TerrainMasks:
    altitude:     np.ndarray   # float32 [0,1]
    slope:        np.ndarray   # float32 [0,1]
    curvature:    np.ndarray   # float32 [0,1]
    valley:       np.ndarray   # float32 [0,1]  high = low/flat areas
    noise:        np.ndarray   # float32 [0,1]  mid-freq variation
    terrain_type: np.ndarray   # uint8, TerrainType values
    # Semantic feature maps shared with the Arc-B stock
    # analyzer's mask language (cliffs/rivers/banks/flats/wet zones).
    flow:           np.ndarray  # float32 [0,1]  drainage accumulation (river paths)
    ridge:          np.ndarray  # float32 [0,1]  convex-up crests
    wetness:        np.ndarray  # float32 [0,1]  low + flow-fed moisture
    shoreline:      np.ndarray  # float32 [0,1]  land adjacent to water
    traversability: np.ndarray  # float32 [0,1]  gentle, dry, mech-passable


def _dilate(mask: np.ndarray) -> np.ndarray:
    """4-connected one-ring dilation (no scipy)."""
    out = mask.copy()
    out[1:, :] |= mask[:-1, :]; out[:-1, :] |= mask[1:, :]
    out[:, 1:] |= mask[:, :-1]; out[:, :-1] |= mask[:, 1:]
    return out


def _flow_accumulation(height: np.ndarray) -> np.ndarray:
    """Steepest-descent (D8) flow accumulation. Each cell starts with unit
    rainfall; processed high->low, each pushes its accumulated area to its
    single lowest 8-neighbour. O(N^2) — fine for generator-scale heightfields.
    Deterministic (height-ordered)."""
    N = height.shape[0]
    h = height.ravel()
    acc = np.ones(h.size, dtype=np.float64)
    order = np.argsort(h)[::-1]          # high -> low
    rows, cols = np.divmod(np.arange(h.size), N)
    for flat in order:
        r, c = rows[flat], cols[flat]
        best = -1
        besth = h[flat]
        for dr in (-1, 0, 1):
            for dc in (-1, 0, 1):
                if dr == 0 and dc == 0:
                    continue
                nr, nc = r + dr, c + dc
                if 0 <= nr < N and 0 <= nc < N:
                    nf = nr * N + nc
                    if h[nf] < besth:
                        besth = h[nf]
                        best = nf
        if best >= 0:
            acc[best] += acc[flat]
    return acc.reshape(N, N)


class MaterialClassifier:
    def classify(self, height: np.ndarray, recipe: TerrainRecipe) -> TerrainMasks:
        m = recipe.materials
        N = height.shape[0]

        altitude = height.astype(np.float32)

        # Slope from finite difference gradient
        gy, gx = np.gradient(height.astype(np.float64))
        slope_raw = np.sqrt(gx**2 + gy**2)
        slope = np.clip(slope_raw * 8.0, 0.0, 1.0).astype(np.float32)

        # Curvature (laplacian)
        lap = np.gradient(gx, axis=1) + np.gradient(gy, axis=0)
        curvature = np.clip(np.abs(lap) * 4.0, 0.0, 1.0).astype(np.float32)

        # Valley: inverted blurred altitude (high = low flat areas)
        h_pil  = Image.fromarray((np.clip(height, 0, 1) * 255).astype(np.uint8), mode='L')
        h_blur = np.array(h_pil.filter(ImageFilter.GaussianBlur(radius=max(1, min(N // 8, 48))))) / 255.0
        valley = np.clip(1.0 - h_blur.astype(np.float32), 0.0, 1.0)

        # Noise variation for dirt blending (vectorised; per-pixel python was O(N^2)).
        gen = OpenSimplex(recipe.seed + 2)
        coords = np.arange(N, dtype=np.float64) / N * m.dirt_noise_scale
        noise = gen.noise2array(coords, coords).astype(np.float32)
        noise = (noise - noise.min()) / (noise.max() - noise.min() + 1e-8)

        # TerrainType classification (priority: water < dirt < rock < snow)
        tt = np.full((N, N), TerrainType.GRASS, dtype=np.uint8)

        # Water
        tt[altitude < m.water_level] = TerrainType.GREEN_WATER

        # Low wet zones above water
        moss_mask = (altitude >= m.water_level) & (altitude < m.water_level + 0.05) & (valley > 0.6)
        tt[moss_mask] = TerrainType.MUD

        # Dirt in noisy intermediate zones
        dirt_mask = (altitude >= m.water_level) & (altitude < m.grass_lowland) & (noise > 0.5)
        tt[dirt_mask] = TerrainType.DIRT

        # Rock on steep slope
        rock_mask = (slope > m.rock_slope) & (altitude > m.water_level)
        tt[rock_mask] = TerrainType.MOUNTAIN

        # Cliff on very steep slope
        cliff_mask = slope > (m.rock_slope + 0.2)
        tt[cliff_mask] = TerrainType.CLIFF

        # Curvature-based rock
        curve_mask = (curvature > m.rock_curvature) & (altitude > m.grass_lowland)
        tt[curve_mask] = TerrainType.MOUNTAIN

        # Snow above snow line
        tt[altitude > m.snow_line] = TerrainType.TUNDRA

        # Steep + above snow line = cliff
        tt[(altitude > m.snow_line) & (slope > 0.3)] = TerrainType.CLIFF

        # --- Derived semantic feature maps ---
        # Ridge: convex-up crests (negative laplacian = local high). Reuses lap.
        ridge = np.clip(-lap * 4.0, 0.0, 1.0).astype(np.float32)

        # Flow / drainage accumulation: route unit rainfall downhill (steepest
        # descent), accumulating contributing area -> high along river channels.
        flow_acc = _flow_accumulation(height.astype(np.float64))
        flow_log = np.log1p(flow_acc)
        flow = (flow_log / (flow_log.max() + 1e-8)).astype(np.float32)

        # Shoreline: land cells touching water (one-ring), from the water level.
        water = altitude < m.water_level
        land = ~water
        shoreline = (land & _dilate(water) & (~water)).astype(np.float32)

        # Wetness: low/flat valleys + flow-fed channels, suppressed on dry highs.
        wetness = np.clip(valley * 0.5 + flow * 0.5, 0.0, 1.0).astype(np.float32)
        wetness[water] = 1.0

        # Traversability: gentle, dry, above water -> mech-passable. 1 = open.
        trav = (1.0 - np.clip(slope / max(m.rock_slope, 1e-3), 0.0, 1.0))
        trav = (trav * land.astype(np.float32)).astype(np.float32)

        return TerrainMasks(
            altitude=altitude,
            slope=slope,
            curvature=curvature,
            valley=valley,
            noise=noise,
            terrain_type=tt,
            flow=flow,
            ridge=ridge,
            wetness=wetness,
            shoreline=shoreline,
            traversability=trav,
        )
