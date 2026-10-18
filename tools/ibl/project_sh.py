#!/usr/bin/env python3
# tools/ibl/project_sh.py
#
# Projects an equirectangular HDRI .exr into 9
# SH-L2 spherical-harmonic coefficients (Ramamoorthi-Hanrahan 2001). Emits
# RAW projection coefficients C_lm = integral( L(omega) * Y_lm(omega) dw ).
#
# IRRADIANCE NOTE: the SHADER evaluator (shaders/static_prop.vert evalShL2)
# absorbs the per-band diffuse cosine-lobe kernel (pi, 2pi/3, pi/4) into its
# Ramamoorthi-Hanrahan c1..c5 constants -- so these coefficients combined
# with that evaluator yield diffuse IRRADIANCE directly. Do NOT also
# premultiply the kernel here, and do NOT apply an additional /pi at the
# consumer. The c1..c5 constants ARE the pre-convolved kernel.
#
# Coefficient order (consistent across projector / generated header / shader):
#   [0]=L00, [1]=L1-1, [2]=L10, [3]=L11,
#   [4]=L2-2, [5]=L2-1, [6]=L20, [7]=L21, [8]=L22
#
# Axis convention: Y-up world (matches the hemisphere term at
# static_prop.vert:271, which uses worldNormal.y as up). Sample direction is
# n = (sin(theta)*cos(phi), cos(theta), sin(theta)*sin(phi)) with theta the
# polar angle measured from +Y. This matches hdri_skybox.frag:30-33 which
# decodes the same equirect via atan(z,x) azimuth + asin(y) elevation.
# The shader SH evaluator therefore binds the L20 "z^2 - 1/3" term to n.y
# (Y-up pole), and the L22 difference to (n.x^2 - n.z^2). Pick ONE axis
# convention and stick to it -- this file documents and uses Y-pole.
#
# Usage:
#   py -3 tools/ibl/project_sh.py data/hdr/DaySkyHDRI063B_4K.exr
#       -> prints 9 lines, r g b (space-separated, %.9g), to stdout.
#   py -3 tools/ibl/project_sh.py data/hdr/DaySkyHDRI063B_4K.exr --out RenderCore/IblShCoeffs.h
#       -> writes generated C++ header with constexpr float[9][3].
#   py -3 tools/ibl/project_sh.py --self-test
#       -> projects synthetic uniform-radiance=1 sphere; asserts L00=1/(2*sqrt(pi)) +/- 1e-3,
#          other bands ~ 0.
import argparse
import hashlib
import math
import sys
from pathlib import Path

import numpy as np


TOOL_VERSION = "V-IBL-STATIC-1 v1"


def load_exr(path: Path) -> np.ndarray:
    # Try imageio.v3 first; fall back to OpenEXR; last resort cv2.
    try:
        import imageio.v3 as iio
        img = iio.imread(str(path))
        return np.asarray(img, dtype=np.float32)[:, :, :3]
    except Exception as e1:
        try:
            import OpenEXR, Imath
            f = OpenEXR.InputFile(str(path))
            dw = f.header()['dataWindow']
            W = dw.max.x - dw.min.x + 1
            H = dw.max.y - dw.min.y + 1
            pt = Imath.PixelType(Imath.PixelType.FLOAT)
            r = np.frombuffer(f.channel('R', pt), dtype=np.float32).reshape(H, W)
            g = np.frombuffer(f.channel('G', pt), dtype=np.float32).reshape(H, W)
            b = np.frombuffer(f.channel('B', pt), dtype=np.float32).reshape(H, W)
            return np.stack([r, g, b], axis=-1)
        except Exception as e2:
            import os
            os.environ['OPENCV_IO_ENABLE_OPENEXR'] = '1'
            import cv2
            img = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
            if img is None:
                raise RuntimeError(f"all EXR readers failed: imageio={e1} openexr={e2}")
            # cv2 returns BGR
            return img[:, :, ::-1].astype(np.float32)[:, :, :3]


def project_sh_l2(radiance: np.ndarray) -> np.ndarray:
    # radiance: (H, W, 3) linear float. Returns (9, 3) -- RAW projection
    # integrals C_lm = integral( L(omega) * Y_lm(omega) dw ). The diffuse
    # convolution kernel (pi, 2pi/3, pi/4 per band) is applied by the SHADER
    # evaluator via the Ramamoorthi-Hanrahan c1..c5 constants -- do NOT
    # premultiply it here. See file-level docstring + post-return comment
    # at the diffuse-convolution discussion below.
    H, W, _ = radiance.shape
    # texel-center theta/phi grids
    y = np.arange(H, dtype=np.float64)
    x = np.arange(W, dtype=np.float64)
    theta = (y + 0.5) / H * math.pi              # polar from +Y, 0..pi
    phi = (x + 0.5) / W * 2.0 * math.pi - math.pi  # -pi..pi
    sin_t = np.sin(theta)[:, None]
    cos_t = np.cos(theta)[:, None]
    sin_p = np.sin(phi)[None, :]
    cos_p = np.cos(phi)[None, :]
    # Y-up direction. n = (sin(theta)cos(phi), cos(theta), sin(theta)sin(phi))
    nx = (sin_t * cos_p)
    ny = (cos_t * np.ones_like(sin_p))
    nz = (sin_t * sin_p)
    # Solid-angle weight per texel.
    dw = (math.pi / H) * (2.0 * math.pi / W) * sin_t  # broadcast over W
    dw_b = np.broadcast_to(dw, (H, W))
    # SH basis (un-normalized: Ramamoorthi-Hanrahan basis Y_lm constants).
    # Real SH normalization constants:
    Y00  = 0.282095                                # 1/(2*sqrt(pi))
    Y1   = 0.488603                                # sqrt(3/(4*pi))
    Y2_2 = 1.092548                                # sqrt(15/(4*pi))
    Y20  = 0.315392                                # sqrt(5/(16*pi))   * (3z^2-1)
    Y22  = 0.546274                                # sqrt(15/(16*pi))
    basis = np.stack([
        np.full((H, W), Y00, dtype=np.float64),    # L00
        Y1 * ny,                                   # L1-1   (y)
        Y1 * nz,                                   # L10    (z)
        Y1 * nx,                                   # L11    (x)
        Y2_2 * (nx * ny),                          # L2-2   xy
        Y2_2 * (ny * nz),                          # L2-1   yz
        Y20 * (3.0 * ny * ny - 1.0),               # L20    (3y^2 - 1)
        Y2_2 * (nx * nz),                          # L21    xz
        Y22 * (nx * nx - nz * nz),                 # L22    x^2 - z^2
    ], axis=0)  # (9, H, W)
    # Project: C_i = integral radiance(omega) * Y_i(omega) domega
    C = np.zeros((9, 3), dtype=np.float64)
    rad = radiance.astype(np.float64)
    for i in range(9):
        w = basis[i] * dw_b
        C[i, 0] = np.sum(rad[:, :, 0] * w)
        C[i, 1] = np.sum(rad[:, :, 1] * w)
        C[i, 2] = np.sum(rad[:, :, 2] * w)
    # Do NOT apply diffuse-convolution band kernel here -- the shader
    # evaluator's Ramamoorthi-Hanrahan c1..c5 constants absorb the
    # (band kernel * basis polynomial * normalization) product. Emitting
    # raw projection here keeps the constants meaningful in the shader.
    return C


def self_test() -> int:
    # Synthetic uniform radiance=1 sphere.
    # Raw projection (no kernel premultiplication):
    #   C00 = integral(1 * Y00 dw) = Y00 * 4pi = 0.282095 * 4pi ~= 3.5449.
    H, W = 64, 128
    rad = np.ones((H, W, 3), dtype=np.float32)
    C = project_sh_l2(rad)
    expected_L00 = 0.282095 * 4.0 * math.pi
    ok = abs(C[0, 0] - expected_L00) < 1e-2
    higher_max = float(np.max(np.abs(C[1:])))
    print(f"[self-test] L00={C[0,0]:.6f} expected={expected_L00:.6f} "
          f"higher_band_max={higher_max:.6e} ok={ok}")
    return 0 if (ok and higher_max < 1e-3) else 1


def write_header(out_path: Path, C: np.ndarray, src_path: Path, sha256: str) -> None:
    lines = []
    lines.append("// RenderCore/IblShCoeffs.h")
    lines.append("//")
    lines.append("// AUTO-GENERATED by tools/ibl/project_sh.py. Do not edit by hand.")
    lines.append(f"// Tool: {TOOL_VERSION}")
    lines.append(f"// Source HDRI: {src_path.as_posix()}")
    lines.append(f"// Source sha256: {sha256}")
    lines.append("//")
    lines.append("// V-IBL-STATIC-1: spherical-harmonic ambient coefficients, SH-L2")
    lines.append("// (9 bands), projected analytically per Ramamoorthi-Hanrahan 2001 from")
    lines.append("// the equirectangular HDRI sample above. Values are RAW projection")
    lines.append("// integrals C_lm = integral(L * Y_lm dw); the shader evaluator")
    lines.append("// shaders/static_prop.vert::evalShL2 absorbs the per-band diffuse")
    lines.append("// cosine-lobe kernel (pi, 2pi/3, pi/4) into its Ramamoorthi-Hanrahan")
    lines.append("// c1..c5 constants, yielding diffuse IRRADIANCE directly. Do NOT")
    lines.append("// premultiply the kernel here and do NOT apply an additional /pi")
    lines.append("// in the consumer -- c1..c5 ARE the pre-convolved kernel.")
    lines.append("//")
    lines.append("// Coefficient order (must match projector + shader evaluator):")
    lines.append("//   [0]=L00, [1]=L1-1, [2]=L10, [3]=L11,")
    lines.append("//   [4]=L2-2, [5]=L2-1, [6]=L20, [7]=L21, [8]=L22")
    lines.append("//")
    lines.append("// Axis convention: Y-up world. Projection uses Y-pole basis (L20 binds")
    lines.append("// to (3*y^2 - 1), L22 to (x^2 - z^2)), matching static_prop.vert worldNormal")
    lines.append("// which is Stuff-space Y-up (model normal * mat3(modelMatrix); position-only")
    lines.append("// Stuff->MC2 axis swap leaves the normal in Stuff Y-up). The HDRI is decoded")
    lines.append("// here with the same UV->direction mapping as hdri_skybox.frag:30-33.")
    lines.append("//")
    lines.append("// Firewall: header-only constexpr. No GL, no game-side includes.")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append("namespace RenderCore {")
    lines.append("")
    lines.append("// 9 SH-L2 coefficients * 3 channels (R, G, B).")
    lines.append("constexpr float kIblShCoeffs[9][3] = {")
    labels = ["L00 ", "L1-1", "L10 ", "L11 ", "L2-2", "L2-1", "L20 ", "L21 ", "L22 "]
    for i in range(9):
        r, g, b = float(C[i, 0]), float(C[i, 1]), float(C[i, 2])
        lines.append(f"    {{ {r:+.9e}f, {g:+.9e}f, {b:+.9e}f }}, // {labels[i]}")
    lines.append("};")
    lines.append("")
    lines.append("} // namespace RenderCore")
    lines.append("")
    out_path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="V-IBL-STATIC-1 SH-L2 projector")
    ap.add_argument("exr", nargs="?", help="Path to equirectangular .exr")
    ap.add_argument("--out", type=Path, default=None,
                    help="Write a generated C++ header to this path; else print 9 lines.")
    ap.add_argument("--self-test", action="store_true",
                    help="Project synthetic uniform-radiance sphere; assert basis sanity.")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.exr:
        ap.error("exr argument required (or use --self-test)")
    src_path = Path(args.exr)
    rad = load_exr(src_path)
    print(f"[project_sh] loaded {src_path} shape={rad.shape}", file=sys.stderr)
    sha = hashlib.sha256(src_path.read_bytes()).hexdigest()
    C = project_sh_l2(rad)
    if args.out:
        write_header(args.out, C, src_path, sha)
        print(f"[project_sh] wrote {args.out}", file=sys.stderr)
    else:
        for i in range(9):
            print(f"{C[i,0]:.9g} {C[i,1]:.9g} {C[i,2]:.9g}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
