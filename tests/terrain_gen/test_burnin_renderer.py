# tests/terrain_gen/test_burnin_renderer.py
import sys, os
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))

from terrain_gen.terrain_recipe import TerrainRecipe
from terrain_gen.height_generator import HeightGenerator
from terrain_gen.material_classifier import MaterialClassifier
from terrain_gen.burnin_renderer import BurninRenderer
from terrain_gen.biome_presets import BIOMES


def _run(biome="temperate_hills", size=60, seed=1):
    r = TerrainRecipe(size=size, seed=seed, biome=biome)
    r.apply_biome()
    h = HeightGenerator().generate(r)
    masks = MaterialClassifier().classify(h, r)
    return BurninRenderer().render(masks, r, BIOMES[biome])


def test_output_size():
    img = _run()
    res = TerrainRecipe(size=60).burnin_resolution()
    assert img.size == (res, res)


def test_output_mode_rgb():
    img = _run()
    assert img.mode == "RGB"


def test_no_pure_black():
    img = _run()
    arr = np.array(img)
    dark_pixels = np.all(arr < 10, axis=2).sum()
    total = arr.shape[0] * arr.shape[1]
    assert dark_pixels / total < 0.01, f"Too many pure-black pixels: {dark_pixels}/{total}"


def _bright_fraction(img):
    arr = np.array(img)
    return np.all(arr > 180, axis=2).mean()


def test_snow_biome_is_desolate_not_snowfield():
    # 600a48689 (2026-06-05) retuned snow_mountain to a bleak frozen-rock biome
    # (snow_line 0.98: snow only on the highest peaks, <=1% of cells), so it
    # must no longer render as a white snowfield. The old test asserted bright
    # snow pixels, which predates that tune.
    assert _bright_fraction(_run(biome="snow_mountain", seed=42)) < 0.01


def test_snow_layer_still_renders_when_snow_line_is_low():
    # Guards the renderer's snow blend itself, now that the preset keeps it off:
    # an explicit low snow_line (the pre-600a48689 value was 0.52) must paint
    # bright snow pixels.
    r = TerrainRecipe(size=60, seed=42, biome="snow_mountain")
    r.apply_biome()
    r.materials.snow_line = 0.5
    h = HeightGenerator().generate(r)
    masks = MaterialClassifier().classify(h, r)
    img = BurninRenderer().render(masks, r, BIOMES["snow_mountain"])
    assert _bright_fraction(img) > 0.05


def test_all_biomes_render():
    for biome in ("temperate_hills", "rocky_badlands", "snow_mountain", "swamp_forest", "desert"):
        img = _run(biome=biome, seed=99)
        assert img.mode == "RGB"
        assert img.size[0] > 0
