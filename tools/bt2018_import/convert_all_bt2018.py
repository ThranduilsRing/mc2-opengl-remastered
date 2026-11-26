#!/usr/bin/env python3
"""End-to-end BT2018 mech import: convert EVERY mech FBX into a deployable,
clip-retargeted, textured GLB under MC2Conversions/ using ORIGINAL BattleTech names.

Per mech:
  1. FBX2glTF -b                          -> MC2Conversions/<btname>.glb (embeds textures)
  2. retarget clips (atlas reference)     -> in place (legs animate correctly)
  3. extract base-albedo PNG -> TGA       -> MC2Conversions/<btname>/<derived>.tga
  4. deploy GLB + TGA                      -> <deploy data/tgl>/

The engine derives the runtime tga name from the GLB material image via deriveName
(lowercase stem + .tga); file.cpp strips the size subdir so a single data/tgl copy
serves the 128/ request too (no LOD copy needed).

Source FBX never modified. Atlas reference GLB (raw bind) never modified.

Usage:
  py -3 convert_all_bt2018.py \
    --fbx2gltf C:/Users/Joe/Downloads/FBX2glTF-windows-x64.exe \
    --dump     A:/Games/mc2-opengl/BattleTech_2018_Dump \
    --out      A:/Games/mc2-opengl/MC2Conversions \
    --deploy   A:/Games/mc2-opengl/releases/mc2-win64-v0.5.0/data/tgl \
    [--force] [--only marauder,warhammer]
"""
import argparse
import glob
import io
import json
import os
import re
import shutil
import subprocess
import sys

from PIL import Image
from pygltflib import GLTF2

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))
from bt2018_retarget_clips import pose_table, retarget_file  # noqa: E402
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

FBX_GLOB = "Animator/chrPrfMech_*Base-001/chrPrfMech_*Base-001.fbx"
BTNAME_RE = re.compile(r"chrPrfMech_(.+)Base-001", re.IGNORECASE)


def btname_of(fbx_path):
    stem = os.path.splitext(os.path.basename(fbx_path))[0]
    m = BTNAME_RE.match(stem)
    return m.group(1) if m else stem


def image_bytes(gltf, img):
    """Return raw bytes of a glTF image (bufferView-backed; GLB embed)."""
    bv = gltf.bufferViews[img.bufferView]
    blob = gltf.binary_blob()
    off = bv.byteOffset or 0
    return blob[off:off + bv.byteLength]


def pick_albedo(gltf):
    """Pick the base-albedo image: prefer name containing '-base-alb', else any
    '-alb' that isn't a blip, else the largest non-blip/non-nrm image."""
    imgs = gltf.images or []
    def ok(n): return "blip" not in n.lower()
    for want in ("-base-alb", "base-alb", "-alb"):
        for im in imgs:
            if im.name and want in im.name.lower() and ok(im.name):
                return im
    cands = [im for im in imgs if im.name and ok(im.name) and "nrm" not in im.name.lower()]
    return cands[0] if cands else None


def derive_tga_name(png_name):
    """Mirror the engine's deriveName for base albedo: lowercase stem + .tga."""
    stem = os.path.splitext(png_name)[0]
    return stem.lower() + ".tga"


def raw_glb_path(out_dir, btname):
    """FBX2glTF output cache. Retargeting reads this and writes <out>/<btname>.glb, so a
    repeat cook starts from the same bytes and produces the same hash."""
    return os.path.join(out_dir, "_raw", f"{btname}.glb")


POSE_ROWS = []   # ready-pose self-check rows, one per chassis (printed and written at the end)


def convert_one(fbx, btname, args):
    out_dir = args.out
    glb = os.path.join(out_dir, f"{btname}.glb")
    asset_dir = os.path.join(out_dir, btname)
    os.makedirs(asset_dir, exist_ok=True)
    raw = raw_glb_path(out_dir, btname)

    # 1. FBX2glTF into the raw cache (skip if present unless --force). Nothing writes the
    #    raw GLB after FBX2glTF.
    if args.force or not os.path.isfile(raw):
        os.makedirs(os.path.dirname(raw), exist_ok=True)
        r = subprocess.run([args.fbx2gltf, "-i", fbx, "-o", os.path.splitext(raw)[0], "-b"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **console_child.child_kwargs(redirected=True))
        if not os.path.isfile(raw):
            print(f"[FAIL] {btname}: FBX2glTF produced no glb\n{r.stdout[-400:]}")
            return None

    # 2. retarget clips raw -> final (atlas reference). Always from the raw bind, never from a
    #    previous output: retargeting an already-retargeted GLB in place stacked the bind
    #    correction on every repeat run. Atlas itself = identity no-op.
    try:
        retarget_file(raw, args.atlas_ref, glb)
        r = retarget_file.last_report
        if r:
            POSE_ROWS.append((btname, r['error_before'], r['error_after'], r['iterations'], r['converged'], r['blend']))
    except Exception as e:
        print(f"[FAIL] {btname}: retarget error: {e}")
        return None

    # 3. extract base-albedo -> TGA
    g = GLTF2().load(glb)
    alb = pick_albedo(g)
    tga_name = None
    if alb is None:
        print(f"[WARN] {btname}: no albedo image found in GLB (skipping texture)")
    else:
        png = image_bytes(g, alb)
        tga_name = derive_tga_name(alb.name)
        tga_path = os.path.join(asset_dir, tga_name)
        Image.open(io.BytesIO(png)).convert("RGBA").save(tga_path)
        shutil.copy2(tga_path, os.path.join(args.deploy, tga_name))

    # 4. deploy GLB
    shutil.copy2(glb, os.path.join(args.deploy, f"{btname}.glb"))
    print(f"[OK] {btname}: glb + tga={tga_name} (anims={len(g.animations or [])})")
    return {"btname": btname, "glb": f"{btname}.glb", "tga": tga_name}


def convert_bundle(bundle, btname, out, dump):
    """Read the intact base prefab directly; game bundles are never modified."""
    import numpy as np
    import UnityPy
    import pygltflib as G
    from pathlib import Path
    from UnityPy.helpers.MeshHelper import MeshHandler
    from tools.bt2018_import import bake_clips

    assert UnityPy.__version__ == bake_clips.UNITYPY_VERSION
    bundle = Path(bundle)
    if not bundle.is_file():
        for dlc in ('flashpoint', 'heavymetal', 'urbanwarfare'):
            candidate = bundle.parent / dlc
            if candidate.is_file():
                probe = UnityPy.load(str(candidate))
                if any(o.type.name == 'GameObject' and (o.peek_name() or '').lower() ==
                       f'chrprfmech_{btname}base-001' for o in probe.objects):
                    bundle = candidate
                    break
        else:
            raise FileNotFoundError(f'{btname}: no base prefab bundle or DLC prefab in {bundle.parent}')
    env = UnityPy.load(str(bundle))
    transforms = {o.path_id: o.read() for o in env.objects if o.type.name == 'Transform'}
    root = next(i for i, t in transforms.items()
                if t.m_GameObject.read().m_Name.lower() == f'chrprfmech_{btname}base-001')
    ids = []
    def visit(i):
        ids.append(i)
        for child in transforms[i].m_Children:
            visit(child.path_id)
    visit(root)
    indices = {i: n for n, i in enumerate(ids)}
    gameobjects = {transforms[i].m_GameObject.path_id: i for i in ids}
    g, blob = G.GLTF2(asset=G.Asset(version='2.0')), bytearray()
    def put(values, dtype, kind, bounds=False):
        a = np.asarray(values, dtype=dtype)
        blob.extend(b'\0' * (-len(blob) % 4))
        offset = len(blob)
        blob.extend(a.tobytes())
        view = len(g.bufferViews)
        g.bufferViews.append(G.BufferView(buffer=0, byteOffset=offset, byteLength=a.nbytes))
        acc = G.Accessor(bufferView=view, componentType={'<f4': 5126, '<u4': 5125, '<u2': 5123}[dtype],
                         count=len(a), type=kind)
        if bounds:
            acc.min, acc.max = a.min(axis=0).tolist(), a.max(axis=0).tolist()
        g.accessors.append(acc)
        return len(g.accessors) - 1
    for i in ids:
        t = transforms[i]
        p, q, s = t.m_LocalPosition, t.m_LocalRotation, t.m_LocalScale
        g.nodes.append(G.Node(name=t.m_GameObject.read().m_Name,
                              translation=[-p.x, p.y, p.z], rotation=[q.x, -q.y, -q.z, q.w],
                              scale=[s.x, s.y, s.z], children=[indices[c.path_id] for c in t.m_Children]))
    materials = {}
    def material(ptr):
        if ptr.path_id in materials:
            return materials[ptr.path_id]
        mat = ptr.read()
        textures = dict(mat.m_SavedProperties.m_TexEnvs)
        texture = textures['_MainTex'].m_Texture.read()
        img = texture.image
        png = io.BytesIO()
        img.save(png, format='PNG')
        data = png.getvalue()
        blob.extend(b'\0' * (-len(blob) % 4))
        g.bufferViews.append(G.BufferView(buffer=0, byteOffset=len(blob), byteLength=len(data)))
        blob.extend(data)
        g.images.append(G.Image(name=texture.m_Name, bufferView=len(g.bufferViews)-1, mimeType='image/png'))
        g.textures.append(G.Texture(source=len(g.images)-1))
        g.materials.append(G.Material(name=mat.m_Name, pbrMetallicRoughness=G.PbrMetallicRoughness(
            baseColorTexture=G.TextureInfo(index=len(g.textures)-1), metallicFactor=0, roughnessFactor=1)))
        materials[ptr.path_id] = len(g.materials)-1
        texdir = Path(dump) / 'Texture2D'
        texdir.mkdir(parents=True, exist_ok=True)
        for key in ('_OcclusionMap', '_BumpMap'):
            ref = textures.get(key)
            if ref and ref.m_Texture.path_id:
                tex = ref.m_Texture.read()
                tex.image.save(texdir / (tex.m_Name + '.png'))
        asset = Path(out) / btname
        asset.mkdir(parents=True, exist_ok=True)
        img.convert('RGBA').save(asset / derive_tga_name(texture.m_Name))
        return materials[ptr.path_id]
    for obj in env.objects:
        if obj.type.name != 'SkinnedMeshRenderer':
            continue
        r = obj.read()
        tid = gameobjects.get(r.m_GameObject.path_id)
        if tid is None:
            continue
        chain, cur = [], tid
        while cur in indices:
            chain.append(transforms[cur].m_GameObject.read().m_Name.lower())
            cur = transforms[cur].m_Father.path_id
        if any('_dmg' in n or 'blip' in n for n in chain):
            continue
        mesh = r.m_Mesh.read()
        h = MeshHandler(mesh)
        h.process()
        vertices = np.asarray(h.m_Vertices) * [-1, 1, 1]
        normals = np.asarray(h.m_Normals) * [-1, 1, 1]
        uv = np.asarray(h.m_UV0)[:, :2].copy()
        uv[:, 1] = 1 - uv[:, 1]
        attrs = G.Attributes(POSITION=put(vertices, '<f4', 'VEC3', True),
                             NORMAL=put(normals, '<f4', 'VEC3'), TEXCOORD_0=put(uv, '<f4', 'VEC2'),
                             JOINTS_0=put(h.m_BoneIndices, '<u2', 'VEC4'),
                             WEIGHTS_0=put(h.m_BoneWeights, '<f4', 'VEC4'))
        primitives = []
        for slot, triangles in enumerate(h.get_triangles()):
            tri = np.asarray(triangles)[:, [0, 2, 1]].reshape(-1)
            primitives.append(G.Primitive(attributes=attrs, indices=put(tri, '<u4', 'SCALAR'),
                                          material=material(r.m_Materials[slot])))
        mirror = np.diag([-1, 1, 1, 1])
        binds = [mirror @ np.array([[getattr(b, f'e{row}{col}') for col in range(4)]
                                   for row in range(4)]) @ mirror for b in mesh.m_BindPose]
        g.skins.append(G.Skin(joints=[indices[b.path_id] for b in r.m_Bones],
                              inverseBindMatrices=put([b.T.reshape(-1) for b in binds], '<f4', 'MAT4')))
        g.meshes.append(G.Mesh(name=mesh.m_Name, primitives=primitives))
        node = g.nodes[indices[tid]]
        node.mesh, node.skin = len(g.meshes)-1, len(g.skins)-1
    if not g.meshes:
        raise ValueError(f'{btname}: no intact skinned meshes in base prefab')
    g.scenes, g.scene = [G.Scene(nodes=[indices[root]])], 0
    g.buffers = [G.Buffer(byteLength=len(blob))]
    g.set_binary_blob(bytes(blob))
    dest = Path(out) / (btname + '.glb')
    g.save(str(dest))
    bake_clips.add_to_glb(str(dest), str(dest), bake_clips.native_clips(str(bundle), btname))
    return {'btname': btname, 'glb': dest.name, 'tga': next((Path(out) / btname).glob('*.tga')).name}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fbx2gltf", required=True)
    ap.add_argument("--dump", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--deploy", required=True)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    args.atlas_ref = os.path.join(args.dump, "Atlas", "atlas_fbx2gltf.glb")
    if not os.path.isfile(args.atlas_ref):
        sys.exit(f"FATAL: atlas reference missing: {args.atlas_ref}")
    os.makedirs(args.out, exist_ok=True)

    only = {s.strip().lower() for s in args.only.split(",") if s.strip()}
    fbxs = sorted(glob.glob(os.path.join(args.dump, FBX_GLOB)))
    manifest = []
    for fbx in fbxs:
        bt = btname_of(fbx)
        if only and bt.lower() not in only:
            continue
        rec = convert_one(fbx, bt, args)
        if rec:
            manifest.append(rec)

    mpath = os.path.join(args.out, "conversions_manifest.json")
    with open(mpath, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    print(f"\n[done] converted {len(manifest)} mechs -> {args.out}")
    print(f"[done] manifest: {mpath}")


if __name__ == "__main__":
    main()
