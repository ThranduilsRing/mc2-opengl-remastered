"""Local CC0 base detail (Poly Haven 2k, ambientCG 1k). MC2 palette colours stay authoritative.

Each source contributes luminance grain to base colour (MC2 hue kept) and, where a
height/displacement map ships, a bump term. Tangent-space normals (NormalGL only;
never NormalDX) are not applied in the box-projected authoring shaders because the
source pieces have no tangent frame; the baked game normal map carries that detail.
Lookup order: 'class.slot' first, then 'class'.
"""
from pathlib import Path

ROOT = Path('C:/Users/Joe/Downloads/GameAsset/Materials')


def ph(folder, name):
    tex = ROOT / folder / 'textures'
    return dict(colour=tex / f'{name}_diff_2k.jpg', normal_gl=tex / f'{name}_nor_gl_2k.jpg')


def acg(folder, name):
    base = ROOT / 'Metal' / folder / f'{folder}_'
    return dict(colour=Path(f'{base}Color.png'), normal_gl=Path(f'{base}NormalGL.png'),
                height=Path(f'{base}Displacement.png'))


SOURCES = {
    'ferro': ph('Concrete/concrete_panels_2k', 'concrete_panels'),
    'corr': acg('CorrugatedSteel009_1K-PNG', 'CorrugatedSteel009'),
    'corr.corr': ph('Metal/worn_corrugated_iron_2k', 'worn_corrugated_iron'),
    'paint': acg('PaintedMetal003_1K-PNG', 'PaintedMetal003'),
    'hazard': acg('PaintedMetal002_1K-PNG', 'PaintedMetal002'),
    'metal': acg('Metal052C_1K-PNG', 'Metal052C'),
    'grate': acg('Metal061B_1K-PNG', 'Metal061B'),
    'wood': ph('Wood/weathered_planks_2k', 'weathered_planks'),
    'wood.second': ph('Wood/rough_wood_2k', 'rough_wood'),
    'canvas': ph('Fabric/hessian_230_2k', 'hessian_230'),
    'gravel': ph('Ground/gravel_road_2k', 'gravel_road'),
    'rubber': ph('Rubber/rubberized_track_2k', 'rubberized_track'),
}


def lookup(key):
    cls = key.partition('.')[0]
    src = SOURCES.get(key) or SOURCES.get(cls)
    if src and src['colour'].is_file():
        return src
    return None
