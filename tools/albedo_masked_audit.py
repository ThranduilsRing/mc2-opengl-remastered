#!/usr/bin/env python3
"""ALBEDO-REOPEN-1 -- masked, UV-coverage, surface-area-weighted albedo census.

WHY THIS EXISTS
---------------
The "static props are dark because their textures are dark" theory rested on a
3.3x figure (PROP-LIGHT-CLASS-4-STATUS.md:80 -- truck 41.6 vs building 136.6)
that was a mean over *screen-space PNG rectangles* in debug-view 40. The
contaminant (a truck fills a fraction of its rect; the surrounding dark ground
is inside the mean) was identified in DARK-PROP-LIGHTING-6-STATUS.md:73-81 and
explicitly never tested. The counter-claim ("the .tga looks normal") was an
eyeball read. This script replaces both with a measurement.

WHAT IT MEASURES
----------------
For each asset (a .ase shape) it computes the mean albedo over the texels that
the engine actually samples, weighted by the world-space surface area that maps
to them. That is the quantity that composes with the lighting multiplier:

    final_pixel ~= albedo * band(ndl)

METHODOLOGY -- every rule below is transcribed from the C++/GLSL, not from docs
------------------------------------------------------------------------------
1. TEXTURE RESOLUTION.  Object textures for EVERY class (mech, vehicle, static
   prop) load from the same directory:
       tglPath + ObjectTextureSize + PATH_SEPARATOR   -> data/tgl/128/
   (mclib/bdactor.cpp:794, mclib/mech3d.cpp:1544, mclib/gvactor.cpp:1452,
   mclib/msl.cpp:922 -- identical sprintf in all of them).
   The name is the *BITMAP basename from the .ase, run through _splitpath
   (TG_TypeMultiShape::GetTextureName, mclib/msl.cpp:1198-1215).

   .ktx2 sidecars exist next to the .tga in shipped deploys, but
   MC2_TEXMGR_KTX_PRIMARY defaults to mode 1 = "decode the .ktx2 only when the
   .tga is ABSENT" (mclib/txmmgr.cpp:5261-5268; the bdactor existence probe
   textureOrKtxSidecarExists at bdactor.cpp:184-200 checks fileExists(tga)
   FIRST). Every shipped deploy has the .tga present, so the .tga is what is
   sampled on a default launch. Mode 2 (force .ktx2) is not the default.

2. TEXTURE LIST LAYOUT.  numTextures = 2 * numMaterials. The first half are the
   albedo .tga names from *BITMAP; the second half are the same stems + "X.tga"
   (silhouette/shadow textures, always textureAlpha=true) -- mclib/msl.cpp:904-912.
   Face material id indexes the FIRST half:
       localTextureHandle = min(MESH_MTLID, numTextures/2 - 1)
   (mclib/tgl.cpp:1140-1151).  This script therefore only ever reads first-half
   albedo textures.

3. UV RULE.  The engine reads *MESH_TVERT as (u, v, w) and stores
       u = tvert.x
       v = 1.0 - tvert.y          <-- V IS FLIPPED
   with an out-of-range guard: |value| > 100 -> the component is forced to 0.0
   (mclib/tgl.cpp:1204-1300, the u0/v0/u1/v1/u2/v2 blocks -- all six carry the
   same guard).  The fragment shader wraps with fract():
       vec2 uvSampled = fract(v_uv) * vec2(uvScaleX, uvScaleY)
   (shaders/static_prop.frag:262).  uvScale is the mixed-size texture-array
   sub-region crop and is 1.0 when a layer fills its array slot; it is a
   *layout* remap inside the array, not a change to which texel of the source
   image is sampled, so the mask is computed on fract(uv) against the source
   image dimensions.

4. OPAQUE-TEXEL RULE.  There are exactly two states and the engine picks by
   FILENAME PREFIX, not by inspecting the image:
     - name starts with "a_"  -> gos_Texture_Alpha, SetTextureAlpha(true)
     - otherwise              -> gos_Texture_Solid
   (mclib/bdactor.cpp:801-809, mech3d.cpp:1550-1561, gvactor.cpp:914+ -- the
   same S_strnicmp(txmName,"a_",2) test at every load site.)
   For gos_Texture_Solid loads with an alpha channel, makeKindaSolid() ORs
   0xff000000 over EVERY texel (GameOS/gameos/gameos_graphics_texture.cpp:39-49,
   called from convertIfNecessary:74-75) -- so a non-"a_" texture has no
   transparent texels by construction, and there is nothing to mask out.
   For alpha textures the discard threshold is:
       if (tex_color.a < 0.5) discard;
   (shaders/static_prop.frag:305, shaders/mech.frag:177 -- same 0.5 in both.)
   In byte terms: keep alpha >= 128 (0.5*255 = 127.5).
   The MC2_SP_GLASS_DITHER Bayer variant (static_prop.frag:189-199) perturbs the
   threshold per-pixel around the same 0.5 for partial-alpha texels; --alpha-rule
   dither reports that variant.

5. COLOUR-KEY RULE.  There is NO magenta colour key on the .tga path.
   keyMagentaToAlphaPNG() is gated on a ".png" extension sniff at both load
   sites (gameos_graphics_texture.cpp:102-107 and :144-147) and stock ships zero
   PNGs. gos_Texture_Keyed is reachable only when Environment.Renderer == 3 =
   the software renderer (mclib/txmmgr.cpp:5230-5233), not the default GL path.
   So exact-magenta texels in a stock .tga are DRAWN, opaque. The script reports
   the magenta area fraction as a diagnostic rather than silently excluding it.

6. COLOUR SPACE.  The engine has no GL_FRAMEBUFFER_SRGB (asserted by the
   pipeline probe at GameOS/gameos/gos_postprocess.cpp:1483-1495, whose own
   message for the fbSrgb==0 branch is
   "GAMMA_SPACE_MATH:no_FRAMEBUFFER_SRGB->shader_lighting_in_sRGB_space").
   Stock object textures upload as GL_RGBA8 and static-prop BC7 arrays are
   built LINEAR/UNORM specifically to match that GL_RGBA8 baseline
   (gos_static_prop_batcher.cpp:3622, :4254). Stock mech albedo is NOT
   linearized either -- mech.frag:182-188 gates pow(rgb, u_importedGamma) on
   v_importedMech, which is 0 for every stock mech, and the imported-mech
   default is gamma 1.0 anyway (gos_mech_batcher.cpp:441-443).
   => All three object classes are sampled and multiplied in the SAME
   (sRGB-encoded, non-linearized) space.  The headline number is therefore the
   mean of the RAW BYTES, which is also the perceptually correct space since
   the framebuffer is presented as sRGB.  The linear-space mean is reported
   alongside because getting this backwards is a ~2.2x error.
   (Exception, reported not folded in: the terrain PBR material array is
   GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM -- gos_terrain_lod_chunk.cpp:579-584 --
   so terrain *material* albedo IS sampler-decoded to linear. Terrain is not
   one of the three object classes under test.)

7. PAINT SCHEME.  Mech and vehicle albedo is REWRITTEN AT LOAD.
   Mech3DAppearance::setPaintScheme (mclib/mech3d.cpp:2104-2233) and the GV
   equivalent (mclib/gvactor.cpp:1438+) lock the texture and replace every texel
   whose R, G or B dominates the other two channels by kRatio=3.0 (with
   kMinDom=16) with a team tint. This script ports that classifier verbatim, so
   a mech's "authored .tga mean" is never quoted as its drawn albedo.
   Default player scheme: prefs.baseColor = prefs.highlightColor = 0xffff7e00
   (code/prefs.cpp:72-73), passed as resetPaintScheme(highlight, highlight, base)
   at code/mission.cpp:2313. BGR is not defined in this tree, so psRed/psGreen/
   psBlue take the values unswapped (mech3d.cpp:2247-2259).

WHAT THE NORMALISATION CAN HIDE  (state it, do not bury it)
-----------------------------------------------------------
* Area weighting answers "what is the mean reflectance of this object's
  surface", NOT "what does the player see". A viewer sees one hemisphere, at a
  distance, with the roof at a grazing angle. A building whose dark roof is 60%
  of its surface area reads brighter than its area-weighted mean from a ground
  camera. --report-per-node exposes the per-mesh-node breakdown so this is
  auditable rather than assumed away.
* Screen coverage is not modelled at all. A building occupies many more pixels
  than a truck; a per-object mean says nothing about how much of the frame each
  class paints.
* LOD0 only. LOD1 shapes have their own (usually smaller) textures.
* Damage shapes ("...dam") are excluded: they are not what a default launch
  shows for an intact object.

NEGATIVE CONTROL
----------------
--negative-control measures pairs that are known to be the same or near-same
art and asserts the tool reports them as similar. If a measurement has never
returned "no difference", its "difference" verdicts are not trustworthy.

USAGE
-----
  py -3 tools/albedo_masked_audit.py --data-root "A:/Games/mc2-opengl/releases/mc2-win64-v0.5.1"
  py -3 tools/albedo_masked_audit.py --data-root <deploy> --negative-control
  py -3 tools/albedo_masked_audit.py --data-root <deploy> --assets truckprop,cargotruckprop
  py -3 tools/albedo_masked_audit.py --data-root <deploy> --json out.json --report-per-node

--data-root must contain data/objects/, data/tgl/ and data/tgl/128/. Both a
deployed install and an unpacked srcdata tree satisfy that (srcdata has the
dirs at the top level; both layouts are probed).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from collections import defaultdict

import numpy as np

# --------------------------------------------------------------------------
# Engine constants -- every one transcribed from source, with its citation.
# --------------------------------------------------------------------------

# shaders/static_prop.frag:305 and shaders/mech.frag:177: `tex_color.a < 0.5`.
ALPHA_TEST_THRESHOLD_BYTE = 0.5 * 255.0          # keep alpha >= 127.5

# mclib/tgl.cpp:1204-1300 -- the six identical |value| > 100 guards.
ASE_UV_RANGE_GUARD = 100.0

# mclib/mech3d.cpp:2258-2260 (setPaintScheme classifier constants).
PAINT_MIN_DOM = 16
PAINT_RATIO = 3.0
PAINT_RATIO_MARGIN = 1.5

# code/prefs.cpp:72-73, applied via code/mission.cpp:2313.
DEFAULT_PS_ARGB = 0xffff7e00

# code/dobjtype.h:32-56 (enum ObjectTypeClass, zero-based from TREE_TYPE).
OBJTYPE = {
    0: "TREE", 1: "BUILDING", 2: "BATTLEMECH", 3: "VEHICLE", 4: "EXPLOSION",
    5: "FIRE", 6: "LASER", 7: "SMOKES", 8: "PROJECTILE", 9: "MECHARM",
    10: "MAPICON", 11: "TERRAINOBJECT", 12: "ARTILLERY", 13: "MINE",
    14: "ELEMENTAL", 15: "BRIDGE", 16: "JET", 17: "PROJLASER",
    18: "TREEBUILDING", 19: "CAMERADRONE", 20: "TRAINCAR", 21: "TURRET",
    22: "GATE", 23: "KLIEG_LIGHT", 24: "WEAPONBOLT",
}

# Which appearance class each ObjectTypeNum resolves to. BUILDING_TYPE ->
# BldgAppearance directly; TERRAINOBJECT_TYPE carrying a [TerrainObjectData]
# block also lands on BldgAppearance via the else branch at
# mclib/terrobj.cpp:1216 (chain verified in DARK-PROP-LIGHTING-6-STATUS.md:40-50
# and re-checked at source this session).
STATIC_PROP_TYPES = {1, 11, 18, 21, 22}
MECH_TYPES = {2}
VEHICLE_TYPES = {3}


# --------------------------------------------------------------------------
# TGA decode
# --------------------------------------------------------------------------

def read_tga(path):
    """Minimal TGA reader: uncompressed (2) and RLE (10) true-colour, 24/32bpp.

    Returns (h, w, 4) uint8 RGBA with row 0 = TOP of the image, matching the
    orientation the engine's Image loader hands to GL (the descriptor bit-5
    origin flag is honoured).
    """
    with open(path, "rb") as fh:
        buf = fh.read()
    if len(buf) < 18:
        raise ValueError(f"{path}: too short for a TGA header")
    idlen = buf[0]
    cmap_type = buf[1]
    imgtype = buf[2]
    cmap_len = int.from_bytes(buf[5:7], "little")
    cmap_bpp = buf[7]
    w = int.from_bytes(buf[12:14], "little")
    h = int.from_bytes(buf[14:16], "little")
    bpp = buf[16]
    descriptor = buf[17]
    if imgtype not in (2, 10):
        raise ValueError(f"{path}: unsupported TGA image type {imgtype}")
    if bpp not in (24, 32):
        raise ValueError(f"{path}: unsupported TGA bpp {bpp}")
    off = 18 + idlen
    if cmap_type:
        off += cmap_len * (cmap_bpp // 8)
    nchan = bpp // 8
    npix = w * h

    if imgtype == 2:
        need = npix * nchan
        raw = np.frombuffer(buf, dtype=np.uint8, count=need, offset=off)
        raw = raw.reshape(h, w, nchan)
    else:
        out = np.empty(npix * nchan, dtype=np.uint8)
        src = off
        dst = 0
        end = npix * nchan
        mv = buf
        while dst < end:
            pkt = mv[src]
            src += 1
            count = (pkt & 0x7F) + 1
            if pkt & 0x80:
                px = mv[src:src + nchan]
                src += nchan
                block = np.frombuffer(px, dtype=np.uint8)
                out[dst:dst + count * nchan] = np.tile(block, count)
                dst += count * nchan
            else:
                n = count * nchan
                out[dst:dst + n] = np.frombuffer(mv, dtype=np.uint8, count=n, offset=src)
                src += n
                dst += n
        raw = out.reshape(h, w, nchan)

    # TGA stores BGR(A).
    rgba = np.empty((h, w, 4), dtype=np.uint8)
    rgba[..., 0] = raw[..., 2]
    rgba[..., 1] = raw[..., 1]
    rgba[..., 2] = raw[..., 0]
    rgba[..., 3] = raw[..., 3] if nchan == 4 else 255

    # Descriptor bit 5: 0 = origin bottom-left (needs a flip to top-down).
    if not (descriptor & 0x20):
        rgba = rgba[::-1, :, :]
    return rgba


# --------------------------------------------------------------------------
# ASE parse
# --------------------------------------------------------------------------

_RE_MATCLASS = re.compile(r"\*MATERIAL_CLASS\s+\"([^\"]*)\"")
_RE_NUMSUB = re.compile(r"\*NUMSUBMTLS\s+(\d+)")
_RE_BITMAP = re.compile(r"\*BITMAP\s+\"([^\"]*)\"")
_RE_MATCOUNT = re.compile(r"\*MATERIAL_COUNT\s+(\d+)")


def parse_ase(path):
    """Parse the subset of ASE the engine parses.

    Returns dict(materials=[basename,...], nodes=[{name, verts, faces, tverts,
    tfaces}, ...]).  `materials` is the FIRST HALF of the engine's texture list
    (the albedo names); the engine's second half is the "...X.tga" silhouette
    set (mclib/msl.cpp:904-912) and is deliberately not modelled here.
    """
    with open(path, "r", errors="ignore") as fh:
        text = fh.read()

    # --- material list -----------------------------------------------------
    # mclib/tgl.cpp:1602-1642: walk MATERIAL_COUNT entries; "Standard" adds 1,
    # "Multi" adds NUMSUBMTLS. Bitmap names are then read in flat order
    # (mclib/msl.cpp:847-873, one *BITMAP per slot, in file order).
    m = _RE_MATCOUNT.search(text)
    nmat_decl = int(m.group(1)) if m else 0
    slots = 0
    pos = 0
    for _ in range(nmat_decl):
        mc = _RE_MATCLASS.search(text, pos)
        if not mc:
            break
        pos = mc.end()
        if "Multi" in mc.group(1):
            ns = _RE_NUMSUB.search(text, pos)
            if ns:
                slots += int(ns.group(1))
                pos = ns.end()
            else:
                slots += 1
        else:
            slots += 1
    bitmaps = [os.path.basename(b.replace("\\", "/")).lower()
               for b in _RE_BITMAP.findall(text)]
    if slots and len(bitmaps) > slots:
        bitmaps = bitmaps[:slots]
    if not bitmaps:
        bitmaps = ["NULLTXM"]

    # --- geometry ----------------------------------------------------------
    nodes = []
    for chunk_start in [m.start() for m in re.finditer(r"\*GEOMOBJECT\s*\{", text)]:
        nxt = text.find("*GEOMOBJECT", chunk_start + 5)
        chunk = text[chunk_start: nxt if nxt > 0 else len(text)]
        nm = re.search(r"\*NODE_NAME\s+\"([^\"]*)\"", chunk)
        name = nm.group(1) if nm else "?"

        verts = {}
        for mm in re.finditer(r"\*MESH_VERTEX\s+(\d+)\s+([-\d.eE+]+)\s+([-\d.eE+]+)\s+([-\d.eE+]+)", chunk):
            verts[int(mm.group(1))] = (float(mm.group(2)), float(mm.group(3)), float(mm.group(4)))

        faces = []
        for mm in re.finditer(
                r"\*MESH_FACE\s+(\d+):\s*A:\s*(\d+)\s*B:\s*(\d+)\s*C:\s*(\d+).*?\*MESH_MTLID\s+(\d+)",
                chunk):
            faces.append((int(mm.group(1)), int(mm.group(2)), int(mm.group(3)),
                          int(mm.group(4)), int(mm.group(5))))
        if not faces:
            for mm in re.finditer(
                    r"\*MESH_FACE\s+(\d+):\s*A:\s*(\d+)\s*B:\s*(\d+)\s*C:\s*(\d+)", chunk):
                faces.append((int(mm.group(1)), int(mm.group(2)), int(mm.group(3)),
                              int(mm.group(4)), 0))

        tverts = {}
        for mm in re.finditer(r"\*MESH_TVERT\s+(\d+)\s+([-\d.eE+]+)\s+([-\d.eE+]+)", chunk):
            tverts[int(mm.group(1))] = (float(mm.group(2)), float(mm.group(3)))

        tfaces = {}
        for mm in re.finditer(r"\*MESH_TFACE\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", chunk):
            tfaces[int(mm.group(1))] = (int(mm.group(2)), int(mm.group(3)), int(mm.group(4)))

        if faces:
            nodes.append(dict(name=name, verts=verts, faces=faces,
                              tverts=tverts, tfaces=tfaces))
    return dict(materials=bitmaps, nodes=nodes)


def _uv_guard(val):
    """mclib/tgl.cpp: |component| > 100 -> forced to 0.0f."""
    return 0.0 if (val > ASE_UV_RANGE_GUARD or val < -ASE_UV_RANGE_GUARD) else val


# --------------------------------------------------------------------------
# Paint-scheme port (mclib/mech3d.cpp:2145-2226) -- verbatim arithmetic
# --------------------------------------------------------------------------

def paint_classify(rgb):
    """Return (slot, shade, mix) arrays. slot -1 = not a paint-mask texel."""
    r = rgb[..., 0].astype(np.float32)
    g = rgb[..., 1].astype(np.float32)
    b = rgb[..., 2].astype(np.float32)
    slot = np.full(r.shape, -1, dtype=np.int8)
    dom = np.zeros(r.shape, dtype=np.float32)
    other = np.zeros(r.shape, dtype=np.float32)

    is_r = (r >= PAINT_MIN_DOM) & (r >= PAINT_RATIO * g) & (r >= PAINT_RATIO * b)
    is_g = (~is_r) & (g >= PAINT_MIN_DOM) & (g >= PAINT_RATIO * r) & (g >= PAINT_RATIO * b)
    is_b = (~is_r) & (~is_g) & (b >= PAINT_MIN_DOM) & (b >= PAINT_RATIO * r) & (b >= PAINT_RATIO * g)

    slot[is_r] = 0
    dom[is_r] = r[is_r]
    other[is_r] = np.maximum(g, b)[is_r]
    slot[is_g] = 1
    dom[is_g] = g[is_g]
    other[is_g] = np.maximum(r, b)[is_g]
    slot[is_b] = 2
    dom[is_b] = b[is_b]
    other[is_b] = np.maximum(r, g)[is_b]

    d = dom / 255.0
    shade = 1.0 - (1.0 - d) * (1.0 - d)
    ratio = dom / (other + 1.0)
    margin = (ratio - PAINT_RATIO) / (PAINT_RATIO * (PAINT_RATIO_MARGIN - 1.0))
    mix = np.clip(margin, 0.0, 1.0)
    mix[slot < 0] = 0.0
    return slot, shade, mix


def apply_paint(rgba, ps_red, ps_green, ps_blue):
    """Return a painted copy of rgba using the engine's classifier + blend."""
    out = rgba.copy()
    slot, shade, mix = paint_classify(rgba)
    slot_colors = np.zeros(rgba.shape[:2] + (3,), dtype=np.float32)
    for s, c in ((0, ps_red), (1, ps_green), (2, ps_blue)):
        sel = slot == s
        slot_colors[sel, 0] = (c >> 16) & 0xFF
        slot_colors[sel, 1] = (c >> 8) & 0xFF
        slot_colors[sel, 2] = c & 0xFF
    tint = slot_colors * shade[..., None]
    src = rgba[..., :3].astype(np.float32)
    blended = tint * mix[..., None] + src * (1.0 - mix[..., None])
    touched = mix > 0.0
    out[..., :3] = np.where(touched[..., None],
                            np.clip(blended, 0, 255).astype(np.uint8),
                            rgba[..., :3])
    return out, slot, mix


# --------------------------------------------------------------------------
# Measurement
# --------------------------------------------------------------------------

def srgb_to_linear(x):
    x = x / 255.0
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def luma601(rgb):
    return 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]


def accumulate_coverage(node, materials, tex_shapes, samples_per_tri):
    """Rasterise UV triangles into per-texture weight accumulators.

    weight = the world-space surface area that maps onto that texel.
    Sampling is a fixed stratified barycentric grid so the result is
    deterministic and independent of triangle screen size.
    """
    acc = {}
    verts, faces = node["verts"], node["faces"]
    tverts, tfaces = node["tverts"], node["tfaces"]
    nmat = len(materials)

    # Stratified barycentric samples (centroids of a subdivided triangle).
    n = samples_per_tri
    bary = []
    for i in range(n):
        for j in range(n - i):
            bary.append(((i + 1 / 3) / n, (j + 1 / 3) / n))
            if i + j < n - 1:
                bary.append(((i + 2 / 3) / n, (j + 2 / 3) / n))
    bary = np.array(bary, dtype=np.float64)
    bu, bv = bary[:, 0], bary[:, 1]
    bw = 1.0 - bu - bv
    keep = bw >= -1e-9
    bu, bv, bw = bu[keep], bv[keep], np.clip(bw[keep], 0.0, None)
    nsamp = len(bu)

    total_area = 0.0
    for (fid, a, b, c, mtlid) in faces:
        if a not in verts or b not in verts or c not in verts:
            continue
        # mclib/tgl.cpp:1147-1149: matLid clamped into the first half.
        slot = min(mtlid, nmat - 1) if nmat else 0
        texname = materials[slot] if nmat else "NULLTXM"
        shape = tex_shapes.get(texname)
        if shape is None:
            continue
        th, tw = shape

        p0 = np.array(verts[a]); p1 = np.array(verts[b]); p2 = np.array(verts[c])
        area = 0.5 * float(np.linalg.norm(np.cross(p1 - p0, p2 - p0)))
        if area <= 0.0:
            continue
        total_area += area

        tf = tfaces.get(fid)
        if tf is None:
            continue
        try:
            t0, t1, t2 = (tverts[tf[0]], tverts[tf[1]], tverts[tf[2]])
        except KeyError:
            continue
        # Engine UV rule: u = x, v = 1 - y, each with the |.|>100 guard.
        uv = np.array([
            [_uv_guard(t0[0]), _uv_guard(1.0 - t0[1])],
            [_uv_guard(t1[0]), _uv_guard(1.0 - t1[1])],
            [_uv_guard(t2[0]), _uv_guard(1.0 - t2[1])],
        ])

        su = bw * uv[0, 0] + bu * uv[1, 0] + bv * uv[2, 0]
        sv = bw * uv[0, 1] + bu * uv[1, 1] + bv * uv[2, 1]
        # shaders/static_prop.frag:262 -- fract() wrap.
        su = su - np.floor(su)
        sv = sv - np.floor(sv)
        xi = np.clip((su * tw).astype(np.int64), 0, tw - 1)
        yi = np.clip((sv * th).astype(np.int64), 0, th - 1)

        if texname not in acc:
            acc[texname] = np.zeros((th, tw), dtype=np.float64)
        np.add.at(acc[texname], (yi, xi), area / nsamp)
    return acc, total_area


def measure_shape(ase_path, tex_dir, samples_per_tri, paint=None,
                  per_node=False):
    ase = parse_ase(ase_path)
    materials = ase["materials"]

    textures, tex_shapes, missing = {}, {}, []
    for name in materials:
        if name in textures or name == "nulltxm":
            continue
        p = os.path.join(tex_dir, name)
        if not os.path.exists(p):
            missing.append(name)
            continue
        try:
            img = read_tga(p)
        except Exception as exc:                      # noqa: BLE001
            missing.append(f"{name}({exc})")
            continue
        textures[name] = img
        tex_shapes[name] = img.shape[:2]

    if not textures:
        return dict(ok=False, reason="no textures resolved", materials=materials,
                    missing=missing)

    acc_all = defaultdict(lambda: None)
    per_node_out = []
    world_area = 0.0
    for node in ase["nodes"]:
        acc, area = accumulate_coverage(node, materials, tex_shapes, samples_per_tri)
        world_area += area
        for k, v in acc.items():
            acc_all[k] = v if acc_all[k] is None else acc_all[k] + v
        if per_node and acc:
            per_node_out.append(dict(node=node["name"], area=area,
                                     textures=sorted(acc.keys())))

    stats = _reduce(textures, acc_all, materials, paint)
    stats.update(ok=True, ase=os.path.basename(ase_path), materials=materials,
                 missing=missing, world_area=world_area,
                 node_count=len(ase["nodes"]))
    if per_node:
        stats["per_node"] = per_node_out
    return stats


def _reduce(textures, acc_all, materials, paint):
    """Collapse per-texture weight maps into the reported statistics."""
    tot_w = 0.0
    sum_rgb = np.zeros(3, dtype=np.float64)
    sum_rgb_lin = np.zeros(3, dtype=np.float64)
    sum_rgb_painted = np.zeros(3, dtype=np.float64)
    sum_rgb_lin_painted = np.zeros(3, dtype=np.float64)
    sum_rgb_nopaintmask = np.zeros(3, dtype=np.float64)
    w_nopaintmask = 0.0
    w_paintmask = 0.0
    w_discarded = 0.0
    w_magenta = 0.0
    covered_texels = 0
    total_texels = 0
    whole_img_sum = np.zeros(3, dtype=np.float64)
    whole_img_n = 0
    samples = []                                        # for weighted quantiles
    per_tex = []

    for name, img in textures.items():
        h, w = img.shape[:2]
        total_texels += h * w
        whole_img_sum += img[..., :3].reshape(-1, 3).astype(np.float64).sum(axis=0)
        whole_img_n += h * w

        wmap = acc_all.get(name)
        if wmap is None:
            per_tex.append(dict(texture=name, size=[w, h], drawn_area=0.0,
                                coverage_frac=0.0))
            continue

        # Engine alpha rule: "a_" prefix -> alpha test at 0.5; otherwise
        # makeKindaSolid() forced every texel opaque at load.
        alpha_tested = name.startswith("a_")
        alpha = img[..., 3].astype(np.float64)
        if alpha_tested:
            discard = alpha < ALPHA_TEST_THRESHOLD_BYTE
            w_discarded += float(wmap[discard].sum())
            wmap = np.where(discard, 0.0, wmap)

        rgb = img[..., :3].astype(np.float64)
        magenta = (img[..., 0] == 255) & (img[..., 1] == 0) & (img[..., 2] == 255)
        w_magenta += float(wmap[magenta].sum())

        wsum = float(wmap.sum())
        tot_w += wsum
        covered_texels += int((wmap > 0).sum())
        sum_rgb += (rgb * wmap[..., None]).reshape(-1, 3).sum(axis=0)
        lin = srgb_to_linear(rgb)
        sum_rgb_lin += (lin * wmap[..., None]).reshape(-1, 3).sum(axis=0)

        slot, mix = None, None
        if paint is not None:
            painted, slot, mix = apply_paint(img, *paint)
            prgb = painted[..., :3].astype(np.float64)
            sum_rgb_painted += (prgb * wmap[..., None]).reshape(-1, 3).sum(axis=0)
            plin = srgb_to_linear(prgb)
            sum_rgb_lin_painted += (plin * wmap[..., None]).reshape(-1, 3).sum(axis=0)
            unmasked = mix <= 0.0
            wm = np.where(unmasked, wmap, 0.0)
            w_nopaintmask += float(wm.sum())
            w_paintmask += float(wmap[~unmasked].sum())
            sum_rgb_nopaintmask += (rgb * wm[..., None]).reshape(-1, 3).sum(axis=0)

        sel = wmap > 0
        if sel.any():
            samples.append((luma601(rgb[sel].T), wmap[sel]))

        per_tex.append(dict(
            texture=name, size=[w, h], drawn_area=round(wsum, 2),
            coverage_frac=round(float((wmap > 0).sum()) / (h * w), 4),
            alpha_tested=alpha_tested,
            mean_rgb=[round(float(x), 2) for x in
                      ((rgb * wmap[..., None]).reshape(-1, 3).sum(axis=0) / wsum)]
            if wsum > 0 else None,
        ))

    if tot_w <= 0:
        return dict(ok=False, reason="zero UV coverage")

    mean_rgb = sum_rgb / tot_w
    mean_lin = sum_rgb_lin / tot_w
    out = dict(
        mean_rgb=[round(float(x), 2) for x in mean_rgb],
        L_srgb=round(float(luma601(mean_rgb)), 2),
        L_linear_equiv=round(float(luma601(mean_lin) * 255.0), 2),
        drawn_area=round(tot_w, 2),
        uv_coverage_texels=covered_texels,
        total_texels=total_texels,
        uv_coverage_frac=round(covered_texels / total_texels, 4) if total_texels else 0.0,
        alpha_discarded_area_frac=round(w_discarded / (tot_w + w_discarded), 4)
        if (tot_w + w_discarded) > 0 else 0.0,
        magenta_area_frac=round(w_magenta / tot_w, 4),
        whole_image_mean_rgb=[round(float(x), 2) for x in (whole_img_sum / whole_img_n)]
        if whole_img_n else None,
        whole_image_L=round(float(luma601(whole_img_sum / whole_img_n)), 2)
        if whole_img_n else None,
        per_texture=per_tex,
    )

    if samples:
        lum = np.concatenate([s[0] for s in samples])
        wts = np.concatenate([s[1] for s in samples])
        order = np.argsort(lum)
        lum, wts = lum[order], wts[order]
        cdf = np.cumsum(wts) / wts.sum()
        for q, key in ((0.10, "L_p10"), (0.25, "L_p25"), (0.50, "L_p50"),
                       (0.75, "L_p75"), (0.90, "L_p90")):
            out[key] = round(float(lum[np.searchsorted(cdf, q)]), 2)
        mu = float((lum * wts).sum() / wts.sum())
        var = float((wts * (lum - mu) ** 2).sum() / wts.sum())
        out["L_sd"] = round(math.sqrt(var), 2)

    if paint is not None:
        out.update(
            paint_mask_area_frac=round(w_paintmask / tot_w, 4),
            L_srgb_painted=round(float(luma601(sum_rgb_painted / tot_w)), 2),
            L_linear_equiv_painted=round(
                float(luma601(sum_rgb_lin_painted / tot_w) * 255.0), 2),
            L_srgb_unpainted_only=round(float(luma601(sum_rgb_nopaintmask / w_nopaintmask)), 2)
            if w_nopaintmask > 0 else None,
        )
    return out


# --------------------------------------------------------------------------
# Asset discovery
# --------------------------------------------------------------------------

def resolve_roots(data_root):
    """Accept either a deployed install (<root>/data/...) or an srcdata tree."""
    for prefix in ("data", ""):
        base = os.path.join(data_root, prefix) if prefix else data_root
        objs = os.path.join(base, "objects")
        tgl = os.path.join(base, "tgl")
        tex = os.path.join(tgl, "128")
        if os.path.isdir(objs) and os.path.isdir(tex):
            return objs, tgl, tex
    raise SystemExit(f"could not find objects/ + tgl/128/ under {data_root}")


_RE_FIT_TYPENUM = re.compile(r"ObjectTypeNum\s*=\s*(-?\d+)")
_RE_FIT_APPEAR = re.compile(r"AppearanceName\s*=\s*\"([^\"]*)\"")


def build_catalog(objs_dir, tgl_dir):
    """objects/*.fit -> (class, appearance) -> tgl/<app>.ini -> LOD0 .ase."""
    cat = []
    for fn in sorted(os.listdir(objs_dir)):
        if not fn.lower().endswith(".fit"):
            continue
        try:
            with open(os.path.join(objs_dir, fn), "r", errors="ignore") as fh:
                txt = fh.read()
        except OSError:
            continue
        tm = _RE_FIT_TYPENUM.search(txt)
        am = _RE_FIT_APPEAR.search(txt)
        if not tm or not am:
            continue
        tnum = int(tm.group(1))
        app = am.group(1)
        ini = os.path.join(tgl_dir, app + ".ini")
        ase_stem = app
        if os.path.exists(ini):
            with open(ini, "r", errors="ignore") as fh:
                itxt = fh.read()
            fm = re.search(r"FileName0\s*=\s*\"([^\"]*)\"", itxt)
            if fm:
                ase_stem = fm.group(1)
        # Filenames on disk are lowercase; the engine's lookup is case-blind.
        ase = None
        for cand in (ase_stem, ase_stem.lower(), app, app.lower()):
            p = os.path.join(tgl_dir, cand + ".ase")
            if os.path.exists(p):
                ase = p
                break
        if not ase:
            continue
        if tnum in STATIC_PROP_TYPES:
            cls = "static_prop"
        elif tnum in MECH_TYPES:
            cls = "mech"
        elif tnum in VEHICLE_TYPES:
            cls = "vehicle"
        elif tnum == 0:
            cls = "tree"
        else:
            continue
        cat.append(dict(fit=fn, stem=os.path.splitext(fn)[0], cls=cls,
                        objtype=OBJTYPE.get(tnum, str(tnum)), ase=ase))
    return cat


# --------------------------------------------------------------------------
# Negative control
# --------------------------------------------------------------------------

# A measurement that has never returned "no difference" is not trustworthy when
# it returns "difference". These controls feed the tool inputs that are KNOWN to
# be the same art and require it to say so.
#
# CONTROL A -- identical art reached through DIFFERENT files. These .ase pairs
# have byte-identical *MESH_VERTEX / *MESH_TVERT / *MESH_FACE / *BITMAP content
# (verified by content hash over the whole tgl/ tree) but are separate files
# loaded independently, and -- critically for THIS question -- the members of
# each pair are classified into DIFFERENT object classes by their .fit
# (`...prop...` = static prop via BUILDING_TYPE, the other = VEHICLE_TYPE).
# If the tool reports a class difference here, its class split is an artifact.
NEGCTL_IDENTITY_PAIRS = [
    ("cargotruckdam", "cargotruckpropdam"),
    ("fueltruckdam", "fueltruckpropdam"),
    ("ordnancetruckdam", "ordnancetruckpropdam"),
]

# CONTROL B -- the same shape measured against two INDEPENDENT copies of the
# texture tree (the srcdata .tga and the shipped deploy's .tga are different
# files with different byte encodings of the same image). Supplied via
# --control-alt-root.
NEGCTL_CROSS_TREE_ASSETS = ["truckprop", "2civliving", "madcat"]

# CONTROL C -- numerical invariance: the same shape at two sampling densities.
NEGCTL_SAMPLING_ASSETS = ["truckprop", "2civliving", "madcat"]

# NOT a negative control -- a DISCRIMINATION check. These four props all sample
# the same trucksprop.tga atlas; a whole-image mean cannot tell them apart, so a
# tool that reports them as identical is not masking at all.
DISCRIMINATION_SET = ["truckprop", "cargotruckprop", "fueltruckprop",
                      "ordnancetruckprop"]


def run_negative_controls(tgl_dir, tex_dir, alt_tex_dir, samples):
    print("\n=== NEGATIVE CONTROLS ===")
    ok_all = True

    print("A. identical art via different files (and different .fit classes)"
          " -- expect ratio 1.000")
    for a, b in NEGCTL_IDENTITY_PAIRS:
        pa = os.path.join(tgl_dir, a + ".ase")
        pb = os.path.join(tgl_dir, b + ".ase")
        if not (os.path.exists(pa) and os.path.exists(pb)):
            print(f"   {a} / {b}: MISSING")
            continue
        ra = measure_shape(pa, tex_dir, samples, paint=None)
        rb = measure_shape(pb, tex_dir, samples, paint=None)
        if not (ra.get("ok") and rb.get("ok")):
            print(f"   {a} / {b}: measure failed")
            continue
        la, lb = ra["L_srgb"], rb["L_srgb"]
        ratio = max(la, lb) / max(min(la, lb), 1e-9)
        good = ratio < 1.001
        ok_all &= good
        print(f"   {a:<22} L={la:<7} {b:<24} L={lb:<7} ratio={ratio:.4f}"
              f"  {'PASS (no difference)' if good else 'FAIL'}")

    if alt_tex_dir:
        print("B. same shape, two independent texture trees -- expect <1%")
        for stem in NEGCTL_CROSS_TREE_ASSETS:
            p = os.path.join(tgl_dir, stem + ".ase")
            if not os.path.exists(p):
                continue
            r1 = measure_shape(p, tex_dir, samples, paint=None)
            r2 = measure_shape(p, alt_tex_dir, samples, paint=None)
            if not (r1.get("ok") and r2.get("ok")):
                print(f"   {stem}: measure failed on one tree")
                continue
            l1, l2 = r1["L_srgb"], r2["L_srgb"]
            ratio = max(l1, l2) / max(min(l1, l2), 1e-9)
            good = ratio < 1.01
            ok_all &= good
            print(f"   {stem:<22} src L={l1:<7} alt L={l2:<7} ratio={ratio:.4f}"
                  f"  {'PASS (no difference)' if good else 'FAIL'}")
    else:
        print("B. skipped (pass --control-alt-root <deploy> to enable)")

    print("C. sampling-density invariance (n=4 vs n=20) -- expect <3%")
    for stem in NEGCTL_SAMPLING_ASSETS:
        p = os.path.join(tgl_dir, stem + ".ase")
        if not os.path.exists(p):
            continue
        r1 = measure_shape(p, tex_dir, 4, paint=None)
        r2 = measure_shape(p, tex_dir, 20, paint=None)
        if not (r1.get("ok") and r2.get("ok")):
            continue
        l1, l2 = r1["L_srgb"], r2["L_srgb"]
        ratio = max(l1, l2) / max(min(l1, l2), 1e-9)
        good = ratio < 1.03
        ok_all &= good
        print(f"   {stem:<22} n=4 L={l1:<7} n=20 L={l2:<7} ratio={ratio:.4f}"
              f"  {'PASS (no difference)' if good else 'FAIL'}")

    print("D. DISCRIMINATION check (not a negative control): four props sharing"
          " one atlas -- expect the tool to SEPARATE them")
    vals = []
    for stem in DISCRIMINATION_SET:
        p = os.path.join(tgl_dir, stem + ".ase")
        if not os.path.exists(p):
            continue
        r = measure_shape(p, tex_dir, samples, paint=None)
        if r.get("ok"):
            vals.append((stem, r["L_srgb"], r["whole_image_L"]))
    for stem, l, wl in vals:
        print(f"   {stem:<22} masked L={l:<8} whole-atlas L={wl}")
    if len(vals) > 1:
        ls = [v[1] for v in vals]
        print(f"   masked spread {min(ls):.1f}..{max(ls):.1f} "
              f"({max(ls)/max(min(ls),1e-9):.2f}x) vs whole-atlas spread 0.00x"
              f"  -> {'PASS (masking discriminates)' if max(ls)/min(ls) > 1.1 else 'FAIL'}")

    print(f"\nNEGATIVE CONTROL VERDICT: {'PASS' if ok_all else 'FAIL'}")
    return ok_all




# --------------------------------------------------------------------------
# Composition: final ~= albedo * band.  BANDS ARE NOT DERIVED HERE.
# --------------------------------------------------------------------------
#
# The multiplier bands below are QUOTED VERBATIM from the sibling lane
# CLASS-LIGHT-BAND-HARNESS-1's Stage A matrix
# (tools/class_light_band_harness, `--test print_stage_a_matrix`). This lane
# does not re-derive them; if that harness changes, re-run it and update this
# table rather than trusting the copy.
#
# Two of the bands in the inherited META-RIGHT-QUESTION-2 §1 table are WRONG and
# their harness corrects them -- both corrections are adopted here:
#   * terrain floor is 0.367, not 0.35 (diffuse is clamped to 0.02 first)
#   * vehicle band EQUALS the mech band at HEAD; VEHICLE-AMBIENT-FLOOR-1
#     (e3310409) is in HEAD and default-ON, so "A + S*ndl, no floor" is stale.
#
# Format: mission -> {class: (floor, ceiling)} over ndl in [0,1].
BANDS = {
    "mc2_03": {  # A=0.1216 S=0.6745 -- an authored-dark day mission
        "terrain": (0.367, 1.200),
        "cement": (1.000, 1.000),
        "mech": (0.472, 1.000),
        "vehicle": (0.472, 1.000),
        "static_prop": (0.929, 1.000),   # prop column, IBL-up
    },
    "mc2_10": {  # A=S=0.7529 -- the mission the user reported trucks on
        "terrain": (0.367, 1.200),
        "cement": (1.000, 1.000),
        "mech": (1.000, 1.000),
        "vehicle": (1.000, 1.000),
        "static_prop": (1.000, 1.000),   # prop column, IBL-up
    },
}


def compose_table(census_json, missions=("mc2_10", "mc2_03")):
    with open(census_json) as fh:
        data = json.load(fh)
    R = [r for r in data["results"] if r.get("ok")]

    def stat(cls, key):
        v = [r[key] for r in R if r["cls"] == cls and r.get(key) is not None]
        return np.median(v) if v else None

    albedo = {
        "static_prop": stat("static_prop", "L_srgb"),
        # mech/vehicle albedo is the PAINTED texture -- the raw .tga mean is not
        # what is drawn (the paint mask covers most of the surface).
        "mech": stat("mech", "L_srgb_painted") or stat("mech", "L_srgb"),
        "vehicle": stat("vehicle", "L_srgb_painted") or stat("vehicle", "L_srgb"),
    }
    for m in missions:
        b = BANDS.get(m)
        if not b:
            continue
        print(f"\n=== COMPOSED  final ~= albedo x band   [{m}] ===")
        print(f"{'class':<14}{'albedo L':>10}{'band lo':>9}{'band hi':>9}"
              f"{'final lo':>10}{'final hi':>10}")
        for cls in ("static_prop", "mech", "vehicle"):
            a = albedo.get(cls)
            if a is None:
                continue
            lo, hi = b[cls]
            print(f"{cls:<14}{a:>10.1f}{lo:>9.3f}{hi:>9.3f}"
                  f"{a*lo:>10.1f}{a*hi:>10.1f}")
        lo, hi = b["terrain"]
        print(f"{'terrain':<14}{'(n/a)':>10}{lo:>9.3f}{hi:>9.3f}"
              f"{'--':>10}{'--':>10}   <- albedo is the per-mission baked colormap")
        lo, hi = b["cement"]
        print(f"{'cement':<14}{'(n/a)':>10}{lo:>9.3f}{hi:>9.3f}"
              f"{'--':>10}{'--':>10}   <- lighting-EXEMPT")
        span = [(cls, albedo[cls]*b[cls][0], albedo[cls]*b[cls][1])
                for cls in ("static_prop", "mech", "vehicle") if albedo.get(cls)]
        lows = [s[1] for s in span]
        his = [s[2] for s in span]
        print(f"  worst cross-class final ratio at the band CEILING: "
              f"{max(his)/max(min(his),1e-9):.2f}x")
        print(f"  headroom a lighting-only fix could ever recover for the "
              f"darkest class: {max(his)/max(min(lows),1e-9):.2f}x "
              f"(= 1.00x when the band is already saturated)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data-root", required=True)
    ap.add_argument("--assets", default="",
                    help="comma-separated .fit stems; default = full census")
    ap.add_argument("--classes", default="static_prop,mech,vehicle,tree")
    ap.add_argument("--samples", type=int, default=12,
                    help="stratified barycentric subdivision per triangle")
    ap.add_argument("--paint", default=hex(DEFAULT_PS_ARGB),
                    help="ARGB paint colour for mech/vehicle (default = the "
                         "stock player scheme 0xffff7e00 from prefs.cpp:72-73)")
    ap.add_argument("--paint-slots", default="",
                    help="explicit per-slot colours 'R,G,B' as ARGB hex -- the "
                         "engine passes resetPaintScheme(highlight, highlight, base)")
    ap.add_argument("--no-paint", action="store_true")
    ap.add_argument("--report-per-node", action="store_true")
    ap.add_argument("--negative-control", action="store_true")
    ap.add_argument("--control-alt-root", default="",
                    help="second asset tree for negative control B")
    ap.add_argument("--compose", default="",
                    help="census .json -> print the albedo x band table")
    ap.add_argument("--json", default="")
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    if args.compose:
        compose_table(args.compose)
        return 0

    objs_dir, tgl_dir, tex_dir = resolve_roots(args.data_root)

    if args.negative_control:
        alt_tex = ""
        if args.control_alt_root:
            _, _, alt_tex = resolve_roots(args.control_alt_root)
        run_negative_controls(tgl_dir, tex_dir, alt_tex, args.samples)
        if not args.assets:
            return 0

    cat = build_catalog(objs_dir, tgl_dir)
    want_cls = set(args.classes.split(","))
    want_assets = set(a.strip().lower() for a in args.assets.split(",") if a.strip())

    sel = [c for c in cat if c["cls"] in want_cls and
           (not want_assets or c["stem"].lower() in want_assets)]
    if args.limit:
        sel = sel[:args.limit]

    if args.no_paint:
        ps_slots = None
    elif args.paint_slots:
        ps_slots = tuple(int(x, 16) for x in args.paint_slots.split(","))
    else:
        v = int(args.paint, 16)
        ps_slots = (v, v, v)
    results = []
    for i, c in enumerate(sel):
        paint = ps_slots if (ps_slots is not None and c["cls"] in ("mech", "vehicle")) else None
        try:
            st = measure_shape(c["ase"], tex_dir, args.samples, paint=paint,
                               per_node=args.report_per_node)
        except Exception as exc:                        # noqa: BLE001
            st = dict(ok=False, reason=f"{type(exc).__name__}: {exc}")
        st.update(stem=c["stem"], cls=c["cls"], objtype=c["objtype"],
                  ase=os.path.basename(c["ase"]))
        results.append(st)
        if not args.json:
            print(f"[{i+1}/{len(sel)}] {c['stem']:<26} {c['cls']:<12} "
                  + (f"L={st.get('L_srgb')}  rgb={st.get('mean_rgb')}  "
                     f"cov={st.get('uv_coverage_frac')}  tex={st.get('materials')}"
                     if st.get("ok") else f"SKIP: {st.get('reason')}"))

    ok = [r for r in results if r.get("ok")]

    print("\n=== CLASS SUMMARY (area-weighted masked mean, sRGB byte space) ===")
    print(f"{'class':<14}{'n':>4}{'L_mean':>9}{'L_median':>10}{'L_p25':>8}"
          f"{'L_p75':>8}{'L_min':>8}{'L_max':>8}")
    by_cls = defaultdict(list)
    for r in ok:
        by_cls[r["cls"]].append(r["L_srgb"])
    for cls in ("static_prop", "mech", "vehicle", "tree"):
        v = sorted(by_cls.get(cls, []))
        if not v:
            continue
        a = np.array(v)
        print(f"{cls:<14}{len(v):>4}{a.mean():>9.1f}{np.median(a):>10.1f}"
              f"{np.percentile(a,25):>8.1f}{np.percentile(a,75):>8.1f}"
              f"{a.min():>8.1f}{a.max():>8.1f}")

    if args.json:
        with open(args.json, "w") as fh:
            json.dump(dict(data_root=args.data_root, results=results), fh, indent=1)
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    sys.exit(main())
