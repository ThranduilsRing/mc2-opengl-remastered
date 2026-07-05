#!/usr/bin/env python3
"""gen_cliff_wall_glb.py -- generate a modular CLIFF_WALL mesh GLB for the
terrain mesh-decal system (Slice 0A visual proof).

WHY: data/tgl/MarbleCliffGLB.glb is a dense photogrammetry ROCK SAMPLE (a
rounded ~4.4-unit blob, 53,901 verts, all three axes ~equal extent). Stretched
x160 to cover a cliff face it reads as a smeared pebble, NOT a wall. This tool
instead BUILDS a real near-vertical WALL segment: a subdivided plane displaced
along its outward normal by the real marble_cliff displacement map plus fractal
noise, giving genuine rocky relief and an irregular BROKEN top edge (silhouette).

The mesh is authored MESH-LOCAL and Y-up. MC2's assimp importer
(mclib/assimp_importer.cpp) reads MESH-LOCAL vertices and DROPS node
rotation/scale (only pivot translation is decomposed), then applies its default
axis map case 0 = (-x, -y, z) (assimp_importer.cpp:90). So we PRE-COMPENSATE:
author the wall in a frame that, after (-x,-y,z), lands upright with:
  * wall height along +Y (world up),
  * wall width along X (contour / tangent),
  * outward face normal along +Z (the CLIFF_WALL "facing" placed by the engine
    frame; local geometry just needs its relief bulging along one horizontal
    axis so the engine's facing basis points it outward).
Concretely we author height along -Y local (so it becomes +Y after -y), width
along -X local (becomes +X), and relief/thickness along +Z local (kept by z).

Origin: X/Z centered on the wall; the BASE sits a bit BELOW y=0 (self-sinking
skirt) so the engine's small outward offset + terrain seat hides the bottom
seam instead of floating.

Texturing: reuses the marble_cliff_01 base/normal/rough JPEGs extracted from the
stock MarbleCliffGLB (same material the terrain-cliff work already ships). The
baseColor image is named "marble_cliff_01" so MC2's DeriveMC2TextureName
(assimp_importer.cpp:281) derives the SAME stem the existing cliff uses ->
identical texture binding, no NULLTXM.

Output: a clean SINGLE SHAPE-node GLB (one mesh, one primitive) -- avoids the
static-prop non-SHAPE_NODE-child abort landmine.

Usage:
  python gen_cliff_wall_glb.py OUT.glb \
      --disp C:/Users/Joe/Downloads/GameAsset/Terrain/marble_cliff_01_disp_4k.png \
      --tex-dir <dir with marble_cliff_01{,_nor_gl,_rough}.jpg> \
      [--subdiv 128] [--width 260] [--height 300] [--relief 26] \
      [--noise 10] [--sink 24] [--seed 1337]
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import numpy as np


def fractal_noise(h, w, octaves, seed):
    """Simple value-noise fBm in [-1,1], smooth via bilinear upsampling."""
    rng = np.random.default_rng(seed)
    acc = np.zeros((h, w), dtype=np.float64)
    amp = 1.0
    norm = 0.0
    for o in range(octaves):
        gh = max(2, (h >> (octaves - o)) + 2)
        gw = max(2, (w >> (octaves - o)) + 2)
        grid = rng.random((gh, gw)) * 2.0 - 1.0
        # bilinear upsample grid -> (h,w)
        ys = np.linspace(0, gh - 1, h)
        xs = np.linspace(0, gw - 1, w)
        y0 = np.floor(ys).astype(int); y1 = np.minimum(y0 + 1, gh - 1)
        x0 = np.floor(xs).astype(int); x1 = np.minimum(x0 + 1, gw - 1)
        fy = (ys - y0)[:, None]; fx = (xs - x0)[None, :]
        g00 = grid[np.ix_(y0, x0)]; g01 = grid[np.ix_(y0, x1)]
        g10 = grid[np.ix_(y1, x0)]; g11 = grid[np.ix_(y1, x1)]
        top = g00 * (1 - fx) + g01 * fx
        bot = g10 * (1 - fx) + g11 * fx
        acc += amp * (top * (1 - fy) + bot * fy)
        norm += amp
        amp *= 0.5
    return acc / norm


def build_wall(disp_path, subdiv, width, height, relief, noise_amp, sink, seed):
    from PIL import Image
    N = subdiv + 1  # verts per side
    # Parametric grid u,v in [0,1]
    u = np.linspace(0.0, 1.0, N)              # across width
    v = np.linspace(0.0, 1.0, N)              # up the wall (0=base,1=top)
    U, V = np.meshgrid(u, v)                  # (N,N)

    # Sample 16-bit displacement, normalized to [0,1]
    im = Image.open(disp_path)
    disp = np.asarray(im).astype(np.float64)
    disp = (disp - disp.min()) / max(1.0, (disp.max() - disp.min()))
    dh, dw = disp.shape
    # sample at grid uv (nearest is fine at 129x129 from 4k)
    yi = np.clip((V * (dh - 1)).astype(int), 0, dh - 1)
    xi = np.clip((U * (dw - 1)).astype(int), 0, dw - 1)
    d = disp[yi, xi]                          # (N,N) in [0,1]

    # fractal noise for broken relief + top silhouette
    fbm = fractal_noise(N, N, octaves=5, seed=seed)  # [-1,1]

    # Combined outward relief (0..~1), centered so the wall isn't all pushed out
    reliefField = (d - 0.5) * 2.0 * relief + fbm * noise_amp    # world units

    # Broken TOP edge: pull the top row's HEIGHT down by a noisy amount so the
    # silhouette is irregular, not a flat ruled edge. Weight ramps in near top.
    topRng = np.random.default_rng(seed ^ 0x9E37)
    topEdge = fbm[-1, :] * 0.5 + 0.5          # 0..1 per column
    edgeCut = (0.12 + 0.22 * topRng.random(N)) * height  # up to ~34% down
    # weight: 0 below 70% height, ramps to 1 at top
    hw = np.clip((V - 0.70) / 0.30, 0.0, 1.0)
    heightField = V * height - hw * edgeCut[None, :] * topEdge[None, :]

    # LOCAL authoring frame (pre-compensating the runtime -x,-y,z axis map):
    #   local X = -width  (becomes +width contour after -x)
    #   local Y = -height (becomes +up after -y); base below 0 via +sink
    #   local Z = +relief (kept; becomes outward facing)
    cx = width * 0.5
    X = -(U * width - cx)                     # centered, sign pre-compensated
    Y = -(heightField - sink)                 # base at y=+sink local -> below 0 world
    Z = reliefField                           # outward relief

    pos = np.stack([X, Y, Z], axis=-1).reshape(-1, 3).astype(np.float32)

    # UVs: U across width, V up. MC2 importer V-flips (toMC2V=1-v); author raw uv.
    uv = np.stack([U, 1.0 - V], axis=-1).reshape(-1, 2).astype(np.float32)

    # indices (two tris per quad).
    idx = []
    for r in range(subdiv):
        for c in range(subdiv):
            a = r * N + c
            b = a + 1
            e = a + N
            f = e + 1
            idx += [a, e, b,  b, e, f]
    indices = np.asarray(idx, dtype=np.uint32)

    # WINDING / NORMAL ORIENTATION FIX (cliff-wall dark/culled bug).
    # The wall's OUTWARD face is +Z local (the relief/facing axis; buildCliff
    # WallMatrix maps local Z -> world outward-facing). Two engine invariants
    # depend on the outward face being CCW-front under +Z:
    #   * GL_CULL_FACE GL_BACK is ON for static props (gos_static_prop_batcher):
    #     a backward-wound outward face is culled -> we see the dark interior.
    #   * calc_light NdotL uses max(dot(N,sun),0): an inward-pointing normal
    #     kills the sun term -> near-black (ambient only).
    # The (U,V) parametrization above has its width axis SIGN-FLIPPED
    # (X = -(U*width-cx)) so the naive [a,e,b,...] order winds the +Z face
    # CW (geometric normals land on -Z). Detect and correct deterministically:
    # if the mean geometric face-normal points along -Z, reverse every triangle
    # so the outward (+Z) face becomes front-facing / CCW. Then derive smooth
    # normals from the corrected winding -> they point OUT of the wall (+Z).
    tris = indices.reshape(-1, 3)
    v0 = pos[tris[:, 0]]; v1 = pos[tris[:, 1]]; v2 = pos[tris[:, 2]]
    fnz = np.cross(v1 - v0, v2 - v0)[:, 2]
    if float(np.mean(fnz)) < 0.0:
        # swap the 2nd and 3rd vertex of each tri to flip winding
        tris = tris[:, [0, 2, 1]]
        indices = tris.reshape(-1).astype(np.uint32)

    # smooth normals from the (now outward-wound) displaced surface
    normals = compute_normals(pos, indices)
    return pos, uv, indices, normals


def compute_normals(pos, indices):
    n = np.zeros_like(pos)
    tris = indices.reshape(-1, 3)
    v0 = pos[tris[:, 0]]; v1 = pos[tris[:, 1]]; v2 = pos[tris[:, 2]]
    fn = np.cross(v1 - v0, v2 - v0)
    for k in range(3):
        np.add.at(n, tris[:, k], fn)
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    ln[ln == 0] = 1.0
    return (n / ln).astype(np.float32)


def pad4(b: bytes) -> bytes:
    while len(b) % 4:
        b += b"\x00"
    return b


def write_glb(out_path, pos, uv, indices, normals, tex_dir):
    import json
    tex_dir = Path(tex_dir)
    imgs = {
        "base":  ("marble_cliff_01",         tex_dir / "marble_cliff_01.jpg"),
        "norm":  ("marble_cliff_01_nor_gl",  tex_dir / "marble_cliff_01_nor_gl.jpg"),
        "rough": ("marble_cliff_01_rough",   tex_dir / "marble_cliff_01_rough.jpg"),
    }
    for _, p in imgs.values():
        if not p.exists():
            raise SystemExit(f"missing texture: {p}")

    bin_parts = []
    views = []
    accessors = []

    def add_view(data, target=None):
        data = pad4(data)
        off = sum(len(x) for x in bin_parts)
        bin_parts.append(data)
        bv = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target is not None:
            bv["target"] = target
        views.append(bv)
        return len(views) - 1

    # POSITION
    pv = add_view(pos.tobytes(), 34962)
    accessors.append({"bufferView": pv, "componentType": 5126, "count": len(pos),
                      "type": "VEC3",
                      "min": [float(x) for x in pos.min(0)],
                      "max": [float(x) for x in pos.max(0)]})
    posA = len(accessors) - 1
    # NORMAL
    nv = add_view(normals.tobytes(), 34962)
    accessors.append({"bufferView": nv, "componentType": 5126, "count": len(normals),
                      "type": "VEC3"})
    nrmA = len(accessors) - 1
    # TEXCOORD_0
    tv = add_view(uv.tobytes(), 34962)
    accessors.append({"bufferView": tv, "componentType": 5126, "count": len(uv),
                      "type": "VEC2"})
    uvA = len(accessors) - 1
    # indices
    iv = add_view(indices.tobytes(), 34963)
    accessors.append({"bufferView": iv, "componentType": 5125, "count": len(indices),
                      "type": "SCALAR"})
    idxA = len(accessors) - 1

    # images (embedded via bufferView)
    images = []
    textures = []
    for key in ("base", "norm", "rough"):
        name, p = imgs[key]
        data = p.read_bytes()
        bvi = add_view(data)
        images.append({"name": name, "mimeType": "image/jpeg", "bufferView": bvi})
        textures.append({"source": len(images) - 1})

    material = {
        "name": "marble_cliff_01",
        "pbrMetallicRoughness": {
            "baseColorTexture": {"index": 0},
            "metallicRoughnessTexture": {"index": 2},
            "metallicFactor": 0.0,
            "roughnessFactor": 1.0,
        },
        "normalTexture": {"index": 1},
    }

    # Node name MUST start with "_PAB" so the building footprint walk
    # (BldgAppearance::calcCellsCovered / markMoveMap, bdactor.cpp) SKIPS this
    # shape — otherwise it records one move-map cell PER VERTEX and a 16k-vertex
    # wall overflows MAX_CELL_COORDS=5000 -> Fatal "too many coords for cellList".
    # "_PAB" is checked ONLY in passability/footprint code, never in the render
    # path, so the mesh still draws. Same data-only fix place_marblecliff uses.
    mesh = {"name": "_PAB_CliffWall", "primitives": [{
        "attributes": {"POSITION": posA, "NORMAL": nrmA, "TEXCOORD_0": uvA},
        "indices": idxA, "material": 0,
    }]}

    gltf = {
        "asset": {"version": "2.0", "generator": "gen_cliff_wall_glb.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        # single SHAPE node (identity TRS; runtime drops it anyway).
        # "_PAB" prefix skips the building footprint move-map walk (see mesh name).
        "nodes": [{"name": "_PAB_CliffWall", "mesh": 0}],
        "meshes": [mesh],
        "materials": [material],
        "images": images,
        "textures": textures,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": sum(len(x) for x in bin_parts)}],
    }

    bin_blob = b"".join(bin_parts)
    # GLB spec: JSON chunk padded with SPACES (0x20), BIN chunk with NULs.
    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(json_bytes) % 4:
        json_bytes += b" "
    bin_blob = pad4(bin_blob)   # NUL padding for BIN is correct

    out = bytearray()
    out += b"glTF" + struct.pack("<II", 2, 12 + 8 + len(json_bytes) + 8 + len(bin_blob))
    out += struct.pack("<II", len(json_bytes), 0x4E4F534A) + json_bytes
    out += struct.pack("<II", len(bin_blob), 0x004E4942) + bin_blob
    Path(out_path).write_bytes(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--disp", required=True)
    ap.add_argument("--tex-dir", required=True)
    ap.add_argument("--subdiv", type=int, default=128)
    ap.add_argument("--width", type=float, default=260.0)
    ap.add_argument("--height", type=float, default=300.0)
    ap.add_argument("--relief", type=float, default=26.0)
    ap.add_argument("--noise", type=float, default=10.0)
    ap.add_argument("--sink", type=float, default=24.0)
    ap.add_argument("--seed", type=int, default=1337)
    args = ap.parse_args(argv)

    pos, uv, indices, normals = build_wall(
        args.disp, args.subdiv, args.width, args.height,
        args.relief, args.noise, args.sink, args.seed)
    write_glb(args.out, pos, uv, indices, normals, args.tex_dir)

    mn = pos.min(0); mx = pos.max(0)
    print(f"WROTE {args.out}")
    print(f"  verts={len(pos)} tris={len(indices)//3} subdiv={args.subdiv}")
    print(f"  local bounds min={mn} max={mx}")
    print(f"  extent XYZ = {(mx-mn)}")
    print(f"  after runtime (-x,-y,z): height(+Y)~{mx[1]-mn[1]:.1f} "
          f"width(X)~{mx[0]-mn[0]:.1f} relief(Z)~{mx[2]-mn[2]:.1f}")
    print(f"  base sinks to world y~{-mx[1]:.1f} (below origin by sink={args.sink})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
