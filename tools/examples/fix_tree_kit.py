"""Two existing tree meshes, corrected for the Stage 1 owner look proof."""
import json
import io
import shutil
import sys
from pathlib import Path
import numpy as np
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.content_pipeline import gltf_io, ir, package, texcook
from tools.content_pipeline.frontends import gltf_file
from tools.neural_mat.export_nmat import _dds_bc7
ROOT = Path(__file__).resolve().parents[2]
WORK = Path('E:/mc2-tmp/slot07/tree-kit-fix-1')  # drive-path: ok assigned PC prototype scratch
OUT = WORK / 'package-tga'
INSTALL = Path('E:/mc2-installs/tree-kit-fix-1')  # drive-path: ok assigned isolated PC prototype install
SOURCE = Path('C:/Users/Joe/Downloads/GameAsset')
PACK = SOURCE / 'Trees/treepack1'
_, w, h, levels = texcook.read_ktx2((INSTALL / 'data/tgl/128/a_treeclump1.ktx2').read_bytes())
target = np.array(Image.open(io.BytesIO(_dds_bc7(levels[0], w, h))).convert('RGBA'))
target_mean = target[target[..., 3] >= 128, :3].mean(axis=0) / 255
assets = []
for aid, stock, source, frame in [
    ('kit-broadleaf', 'tc1_1', Path('E:/mc2-ref/packages/mc2-tree-kit/v3/data/model_overrides/cooked/tc1_1.glb'), 'mc2_runtime'),  # drive-path: ok owner-specified PC source pack
    ('broadleaf-maple', 'tc1_3', SOURCE / 'uploads_files_4996646_Broadleaf_GLTF/maple1.glb', 'gltf')]:
    asset = gltf_file.to_ir(aid, 'tree:' + stock, source, kind='tree', work_dir=WORK / aid,
        meta={'frame': frame}, classes={m['name']: 'wood' for m in gltf_io.load_document(source)[0]['materials']})
    scene = asset.states['intact'][0].scene
    flat = scene.flattened()
    scene.meshes = [ir.Mesh(name, [prim]) for name, prim in flat]
    scene.nodes = [ir.Node(f'{aid}-{i}', mesh=i) for i in range(len(flat))]
    lo, hi = scene.bounds()
    height = 120.05009973049164  # tc1_1 and tc1_3 stock catalog height
    # Restore tree1's pre-fit proportions measured from its shipped blend, then scale uniformly.
    extent = np.array([40.35788, 34.37226, 32.713108]) if frame == 'mc2_runtime' else hi - lo
    scale = extent / (hi - lo) * (height / extent[1])
    transform = np.diag([*scale, 1.0])
    transform[:3, 3] = -np.array([(lo[0]+hi[0])/2, lo[1], (lo[2]+hi[2])/2]) * scale
    for mesh in scene.meshes:
        mesh.primitives = [ir.transform_primitive(p, transform) for p in mesh.primitives]
        # The prop importer ignores doubleSided; bake the authored reverse side,
        # including its normal, so color, shadow and reflection see the same cards.
        for p in list(mesh.primitives):
            if asset.materials[p.material].double_sided:
                back = ir.transform_primitive(p, np.eye(4))
                back.indices = back.indices[:, [0, 2, 1]]
                if back.normals is not None:
                    back.normals *= -1
                if back.tangents is not None:
                    back.tangents[:, 3] *= -1
                mesh.primitives.append(back)
    lo, hi = scene.bounds()
    assert abs(lo[1]) < 1e-4 and abs(hi[1] - height) < 1e-4, (aid, lo, hi)
    for m in asset.materials:
        leaf = m.alpha_mode != 'OPAQUE'
        m.alpha_mode = 'MASK' if leaf else 'OPAQUE'
        m.alpha_cutoff, m.reviewed = 0.5, True
        if frame == 'mc2_runtime':
            m.base_color = ir.TextureRef.of(PACK / ('branch-texture-png.png' if leaf else 'tilebark.jpg'), 'base_color')
            m.normal = ir.TextureRef.of(PACK / ('branch-texturenormal-png.png' if leaf else 'tilebark2normal.jpg'), 'normal')
        if leaf:
            rgba = texcook.load_rgba(m.base_color.path)
            mean = rgba[rgba[..., 3] >= 128, :3].mean(axis=0) / 255
            m.base_color_factor = (*((texcook.srgb_to_linear(target_mean) / texcook.srgb_to_linear(mean)).tolist()), 1.0)
        m.review_note = 'Prototype: shipped normals, stock palette, depth-writing cutout.'
    asset.frontend['texel_scale'] = 8
    assets.append(asset)
records = []
for asset in assets:
    names = package.cook_base_colors(asset, OUT, {'files': {}}, False)
    for slot, m in enumerate(asset.materials):
        m.base_color = ir.TextureRef.of(OUT / 'data/tgl/128' / names[slot], 'base_color', passthrough=True)
    records.append(package.cook_asset(asset, OUT, compress=True))
(OUT / 'data/model_overrides/models.json').write_text(json.dumps({'overrides': records}, indent=1))
assert not package.check_package(OUT)
# Cooked assets enter only the assigned install; parity pins every emitted byte.
declared = {}
rel = 'data/tgl/128/a_treeclump1.ktx2'
(INSTALL / rel).parent.mkdir(parents=True, exist_ok=True)
declared[rel] = ir.sha256_file(INSTALL / rel)
for p in sorted((OUT / 'data').rglob('*')):
    if not p.is_file() or p.name.endswith('.cook.json') or ('tgl' in p.parts and p.suffix == '.ktx2'):
        continue
    rel = p.relative_to(OUT)
    dest = INSTALL / rel
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(p, dest)
    declared[rel.as_posix()] = ir.sha256_file(dest)
(ROOT / 'intentional-asset-changes.json').write_text(json.dumps(declared, indent=1) + '\n')
print('OVERRIDE_VALIDATOR PASS', len(declared), 'intentional assets')
