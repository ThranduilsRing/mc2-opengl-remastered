#!/usr/bin/env python3
"""Offline cook: mech PBR maps -> KTX2 BC7 (albedo, MR/AO) / BC5 (normals) with a full mip chain, encoded by bc7enc_rdo.

    py -3 tools/texcook/cook_bc7enc.py --bc7enc build/bc7enc [--jobs 8] [--role base_color|normal|orm] IN.tga|png ...

Each input X.tga|png gets X.ktx2 written next to it (--out-dir to redirect). The game loads a sidecar .ktx2 in preference to
the source when one is present (mclib/texture_assets.cpp), and falls back to the TGA/PNG path exactly as before when it is
absent, so mod loose TGA/PNG keep working and the cook is purely additive.

Mip chains, colour-space handling and the KTX2 container come from tools/content_pipeline/texcook.py (one contract); only the
block encoder differs (bc7enc_rdo instead of texconv, so the cook runs on the Linux box: taskset -c 32-63 ... --jobs 30).

Never cooked (EXCLUDE): UI, HUD, atlas, font, icon and terrain control/cement/colormap textures. They are sampled with
point/edge semantics or composited on the CPU and block compression corrupts them. A path that matches is skipped.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from content_pipeline import texcook  # noqa: E402

EXCLUDE = re.compile(r"(^|[\\/_.-])(ui|hud|gui|atlas|font|fonts|icon|icons|mechicon|cursor|cement|colormap|ctrl|control|"
                     r"terrain|tersec|water|loading|logo|splash|bitmap|bmp)([\\/_.-]|$)", re.I)
ROLE_SUFFIX = ((re.compile(r"(_n|_normal|_nrm|_normalgl)$", re.I), "normal"),
               (re.compile(r"(_orm|_mr|_ao|_rma|_metalrough)$", re.I), "orm"),
               (re.compile(r"(_e|_emissive)$", re.I), "emissive"))


def role_of(path: Path, forced: str | None) -> str:
    if forced:
        return forced
    for rx, role in ROLE_SUFFIX:
        if rx.search(path.stem):
            return role
    return "base_color"


def encode_level(bc7enc: Path, level: np.ndarray, role: str, workdir: Path) -> bytes:
    h, w = level.shape[:2]
    pw, ph = max(4, w), max(4, h)
    if (pw, ph) != (w, h):     # a 1x1 / 2x2 level is one block: pad by edge replication
        level = np.pad(level, ((0, ph - h), (0, pw - w), (0, 0)), mode="edge")
    src = workdir / "lvl.png"
    out = workdir / "lvl.dds"
    out.unlink(missing_ok=True)
    Image.fromarray(level, "RGBA").save(src)
    mode = ["-5"] if texcook.ROLES[role].vk_format == texcook.VK_BC5_UNORM else ["-u4"]
    r = subprocess.run([str(bc7enc), "-f", "-g", "-q", *mode, str(src)], cwd=workdir, capture_output=True, text=True)
    if r.returncode or not out.is_file():
        raise RuntimeError(f"bc7enc failed for {w}x{h}: {r.stdout[-300:]} {r.stderr[-300:]}")
    data = out.read_bytes()
    if data[:4] != b"DDS " or data[84:88] != b"DX10":
        raise RuntimeError("bc7enc did not write a DX10 DDS")
    blocks = data[148:]
    bw, bh = (w + 3) // 4, (h + 3) // 4
    if (pw, ph) != (w, h):     # keep only the blocks that cover the real level
        row = (pw // 4) * 16
        blocks = b"".join(blocks[r * row:r * row + bw * 16] for r in range(bh))
    if len(blocks) != bw * bh * 16:
        raise RuntimeError(f"unexpected block payload {len(blocks)} for {w}x{h}")
    return blocks


def cook_one(bc7enc: Path, src: Path, role: str, out: Path) -> str:
    rgba = texcook.load_rgba(src.read_bytes())
    h, w = rgba.shape[:2]
    levels = texcook.mip_chain(rgba, role)
    with tempfile.TemporaryDirectory(prefix="bc7cook-") as d:
        blocks = [encode_level(bc7enc, lv, role, Path(d)) for lv in levels]
    out.write_bytes(texcook.write_ktx2(texcook.ROLES[role].vk_format, w, h, blocks))
    return f"{src.name} -> {out.name} {role} {w}x{h} mips={len(levels)} bytes={out.stat().st_size}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("inputs", nargs="+", type=Path)
    ap.add_argument("--bc7enc", type=Path, required=True)
    ap.add_argument("--role", choices=sorted(texcook.ROLES))
    ap.add_argument("--out-dir", type=Path)
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--force", action="store_true", help="re-cook even if the .ktx2 is newer than the source")
    a = ap.parse_args()
    todo = []
    for p in a.inputs:
        if EXCLUDE.search(str(p.as_posix())):
            print(f"skip (excluded class) {p}")
            continue
        out = (a.out_dir or p.parent) / (p.stem + ".ktx2")
        if out.is_file() and not a.force and out.stat().st_mtime >= p.stat().st_mtime:
            print(f"skip (up to date) {p}")
            continue
        todo.append((p, role_of(p, a.role), out))
    failed = 0
    with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        futs = [(p, ex.submit(cook_one, a.bc7enc, p, role, out)) for p, role, out in todo]
        for p, f in futs:
            try:
                print(f.result())
            except Exception as e:  # report every input, then fail the run
                failed += 1
                print(f"FAIL {p}: {e}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
