# tools/terrain_gen/burnin_renderer.py
from __future__ import annotations

import os
import numpy as np
from PIL import Image, ImageFilter
from opensimplex import OpenSimplex

from terrain_gen.terrain_recipe import TerrainRecipe
from terrain_gen.material_classifier import TerrainMasks
from terrain_gen.biome_presets import BiomePreset


def _load_tiled(path: str, scale: float, res: int) -> "np.ndarray | None":
    """Load a texture and tile it to (res, res, 3) float32 in [0,1].

    scale = repeats across the full map width. Missing/empty path -> None
    (caller falls back to flat palette tint). Warns once on a missing file."""
    if not path:
        return None
    if not os.path.exists(path):
        print(f"WARNING: texture not found, palette fallback: {path}", flush=True)
        return None
    tile = max(8, int(round(res / max(1.0, scale))))
    img = Image.open(path).convert('RGB').resize((tile, tile), Image.BILINEAR)
    a = np.asarray(img, dtype=np.float32) / np.float32(255.0)
    reps = res // tile + 1
    return np.tile(a, (reps, reps, 1))[:res, :res, :]


class BurninRenderer:
    def render(self, masks: TerrainMasks, recipe: TerrainRecipe, preset: BiomePreset) -> Image.Image:
        engine_res = recipe.burnin_resolution()

        # Final output cap. We intentionally do NOT emit enormous engine-matched
        # colormaps anymore; detail comes from normal/detail systems.
        final_cap = int(getattr(recipe, "_burnin_final_cap", 4096))
        target_res = min(engine_res, final_cap)

        # Working resolution cap. Standard generation can shade at 2048 then upscale
        # to <=4096. Preview sets this to 256.
        working_cap = int(getattr(recipe, "_burnin_working_cap", min(2048, target_res)))
        res = min(target_res, working_cap)

        b   = recipe.burnin
        p   = preset.palette
        m   = recipe.materials

        def up(arr: np.ndarray) -> np.ndarray:
            img = Image.fromarray((np.clip(arr, 0, 1) * 255).astype(np.uint8), mode='L')
            return np.asarray(img.resize((res, res), Image.BICUBIC), dtype=np.float32) / np.float32(255.0)

        alt    = up(masks.altitude)
        slope  = up(masks.slope)
        curv   = up(masks.curvature)
        valley = up(masks.valley)
        noise  = up(masks.noise)

        # Continuous blend weights (each pixel is a weighted mix of palette entries)
        w_snow  = np.clip((alt - m.snow_line) / 0.12, 0, 1)
        w_rock  = np.clip((slope - (m.rock_slope - 0.15)) / 0.15, 0, 1) * (1.0 - w_snow)
        w_rock += np.clip((curv - (m.rock_curvature - 0.15)) / 0.15, 0, 1) * (1.0 - w_snow - w_rock)
        w_rock  = np.clip(w_rock, 0, 1)
        w_water = np.clip((m.water_level + 0.04 - alt) / 0.04, 0, 1)
        w_grass = np.clip(1.0 - w_snow - w_rock - w_water, 0, 1)
        w_dirt  = w_grass * np.clip(noise * 2.0 - 0.5, 0, 1) * (1.0 - np.clip(alt / (m.grass_lowland + 1e-8), 0, 1))
        w_grass = np.clip(w_grass - w_dirt, 0, 1)
        w_mud   = w_grass * valley * 0.3
        w_grass = np.clip(w_grass - w_mud, 0, 1)
        w_forest = w_grass * np.clip((m.grass_lowland - alt) / (m.grass_lowland + 0.01), 0, 1) * 0.4
        w_grass = np.clip(w_grass - w_forest, 0, 1)

        def to_f(rgb):
            return np.array([v / 255.0 for v in rgb], dtype=np.float32)

        # Optional tiled detail textures. Each configured texture is tiled
        # to (res,res,3) and multiplied by the palette color (tint). Unset -> flat
        # palette color (identical to the pre-texture render). Textures are loaded at
        # the shading `res` so they upscale with the burnin if res != target_res.
        tx = getattr(recipe, "textures", None)
        if tx is not None:
            t_grass  = _load_tiled(tx.grass,        tx.grass_scale,        res)
            t_rock   = _load_tiled(tx.rock,         tx.rock_scale,         res)
            t_dirt   = _load_tiled(tx.dirt,         tx.dirt_scale,         res)
            t_mud    = _load_tiled(tx.mud,          tx.mud_scale,          res)
            t_snow   = _load_tiled(tx.snow,         tx.snow_scale,         res)
            t_forest = _load_tiled(tx.forest_floor, tx.forest_floor_scale, res)
        else:
            t_grass = t_rock = t_dirt = t_mud = t_snow = t_forest = None

        def layer(tint, tex):
            # tex*tint (textured) or flat tint; both broadcast against w[...,None].
            return tex * tint if tex is not None else tint

        rock_tint = to_f(p.rock) * 0.6 + to_f(p.dark_rock) * 0.4

        color = (
            layer(to_f(p.grass),        t_grass)  * w_grass[..., None]  +
            layer(to_f(p.dirt),         t_dirt)   * w_dirt[..., None]   +
            layer(to_f(p.mud),          t_mud)    * w_mud[..., None]    +
            layer(to_f(p.forest_floor), t_forest) * w_forest[..., None] +
            layer(rock_tint,            t_rock)   * w_rock[..., None]   +
            layer(to_f(p.snow),         t_snow)   * w_snow[..., None]   +
            to_f(p.water)        * w_water[..., None]
        )
        color = np.clip(color, 0, 1)

        # Ambient occlusion
        alt_pil  = Image.fromarray((alt * 255).astype(np.uint8), mode='L')
        alt_blur = np.asarray(
            alt_pil.filter(ImageFilter.GaussianBlur(radius=max(1, res // 20))),
            dtype=np.float32,
        ) / np.float32(255.0)
        ao = np.clip(alt - alt_blur, 0, 1)
        color *= (1.0 - b.ao_strength * ao)[..., None]

        # Slope shading
        color *= (1.0 - b.slope_shading * slope)[..., None]

        # Erosion streaks
        streak = valley * slope * 0.3
        color = color * (1.0 - streak[..., None])

        # Large color variation (vectorised; the per-pixel python loop over res*res
        # = ~1.6M noise2 calls dominated generation time).
        gen = OpenSimplex(recipe.seed + 3)
        vc = np.arange(res, dtype=np.float64) / res * 4.0
        var_noise = gen.noise2array(vc, vc).astype(np.float32)
        var_noise = (var_noise - var_noise.min()) / (var_noise.max() - var_noise.min() + 1e-8)
        color = color * (1.0 - b.color_variation * 0.5 + var_noise[..., None] * b.color_variation)

        # Fine grain
        rng = np.random.default_rng(recipe.seed)
        grain = (rng.random((res, res), dtype=np.float32) - np.float32(0.5)) * np.float32(b.grain_scale * 0.05)
        color += grain[..., None]

        color = np.clip(color * 255, 0, 255).astype(np.uint8)
        img = Image.fromarray(color, mode='RGB')
        if res != target_res:
            img = img.resize((target_res, target_res), Image.BICUBIC)
        return img
