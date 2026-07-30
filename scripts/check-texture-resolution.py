#!/usr/bin/env python3
"""check-texture-resolution.py -- cooked-texture resolution/format validator
+ cook.json provenance-by-inspection (S15+ cook polish).

Two jobs, one read-only pass over a cooked-asset tree:

  (A) VALIDATE that cooked textures obey the engine/cook resolution+format
      rules (the loader-side rules that today fail soft -- black tiles, skipped
      textures -- this catches them at cook time instead):
        * burnin colormap JPG : width%256==0 AND height%256==0
          (numTexturesAcross = width/256; UV-decoupled sampling -> mis-sized
           colormap samples garbage. COLOR_MAP_TEXTURE_SIZE=256.)
        * tile TGA in data/textures/<N>/ : square, power-of-two, dims==<N>
          (terrtxm2.cpp textureIsOKFormat: square POT or loader skips it).
        * KTX2 : valid magic+header; vkFormat in the cook-allowed set
          {37,43,145,146}; level chain coherent; and under data/tgl/<N>/ the
          longest edge must be <= <N> (tier cook never upscales).

  (B) EMIT a cook.json inventory: per cooked texture, its sha256 + dims +
      format + mips + validation verdict. This is provenance-BY-INSPECTION --
      it records what is actually cooked and its integrity hash (drift / bad-cook
      detector). True source+param provenance needs cook-TIME emission; the
      asset_cook GLB path already has that (manifest.json provenance block). The
      texture cooks (mc2texcook/cook_tgl_tiers/burnin_jpeg) emit none -- this
      gives the deployed tree an inspectable manifest until they do.

House style: standalone scripts/check-*.py, exit 0 = pass / 1 = any FAIL (WARN
does not fail). SKIP jpg/tga checks gracefully if Pillow is absent (KTX2 checks
need no Pillow). No emoji. Python 3 stdlib + optional Pillow.

Usage:
  py -3 scripts/check-texture-resolution.py [--dir DIR ...] [--manifest cook.json]
  py -3 scripts/check-texture-resolution.py --self-test
  py -3 scripts/check-texture-resolution.py --verbose
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

PREFIX = "[check-texture-resolution]"
KTX2_MAGIC = b"\xabKTX 20\xbb\r\n\x1a\n"  # 12 bytes
ALLOWED_VKFORMAT = {37, 43, 145, 146}  # RGBA8 UNORM/SRGB, BC7 UNORM/SRGB
POT = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192}
DEFAULT_DEPLOY = Path(r"A:/Games/mc2-opengl/mc2-win64-v0.4")

try:
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = None  # 5120^2 burnin atlases are not "bombs"
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _read_ktx2_header(path: Path) -> dict:
    """magic(12) then 9x uint32 LE: vkFormat,typeSize,w,h,depth,layers,faces,
    levelCount,supercompression. (Matches mc2texcook writer / ktx2_probe.)"""
    data = path.read_bytes()[:48]
    if data[:12] != KTX2_MAGIC:
        raise ValueError("bad KTX2 magic")
    (vk, _ts, w, h, _d, _l, _f, levels, _sc) = struct.unpack_from("<9I", data, 12)
    return {"vkFormat": vk, "width": w, "height": h, "levelCount": levels}


def _expected_mips(w: int, h: int) -> int:
    return max(w, h, 1).bit_length()  # log2(pot)+1; for non-pot, ceil-ish upper bound


import re

# cook_tgl_tiers.py outputs BC7 KTX2 only to these tiers (longest-edge cap,
# never upscale). data/tgl/128/ is NOT a cook tier (legacy native-size art).
_TGL_TIERS = {"256", "512", "1024"}


def _detail_tile_size(path: Path):
    """Detail/water/mask tile under data/textures/<N>[mask|Overlays]/ -> N.

    These are the loader-checked tiles (terrtxm2.cpp textureIsOKFormat: square,
    power-of-two, dims == folder size). Returns the expected edge or None.
    """
    parts = [p.lower() for p in path.parts]
    if "textures" not in parts:
        return None
    parent = path.parent.name  # e.g. "128", "128mask", "256Overlays"
    m = re.match(r"^(\d+)", parent)
    return int(m.group(1)) if m else None


def _tgl_tier_cap(path: Path):
    """KTX2 under a cook_tgl_tiers output tier (data/tgl/{256,512,1024}/) -> cap.

    SCOPE: the upscale-cap rule is tgl-tree-specific. A KTX2 misrouted into the
    data/textures/ tree gets no cap check (returns None) -- cook policy is
    responsible for routing tier KTX2 to data/tgl/, not data/textures/.
    """
    parts = [p.lower() for p in path.parts]
    if "tgl" not in parts:
        return None
    parent = path.parent.name
    return int(parent) if parent in _TGL_TIERS else None


# ---------------------------------------------------------------------------
# Per-file classifiers -> record dict {verdict, reasons, ...}
# ---------------------------------------------------------------------------

def _rec(path: Path, root: Path, kind: str, **extra) -> dict:
    rel = os.path.relpath(str(path), str(root)).replace(os.sep, "/")
    base = {"path": rel, "kind": kind, "sha256": _sha256(path),
            "width": None, "height": None, "vkFormat": None, "levelCount": None,
            "verdict": "PASS", "reasons": []}
    base.update(extra)
    return base


def check_ktx2(path: Path, root: Path) -> dict:
    rec = _rec(path, root, "ktx2")
    try:
        h = _read_ktx2_header(path)
    except (ValueError, struct.error, OSError) as exc:
        rec["verdict"] = "FAIL"
        rec["reasons"].append(f"unreadable KTX2 header: {exc}")
        return rec
    w, ht, vk, lv = h["width"], h["height"], h["vkFormat"], h["levelCount"]
    rec.update(width=w, height=ht, vkFormat=vk, levelCount=lv)
    if vk not in ALLOWED_VKFORMAT:
        rec["verdict"] = "FAIL"
        rec["reasons"].append(f"vkFormat {vk} not in cook-allowed {sorted(ALLOWED_VKFORMAT)}")
    if w < 1 or ht < 1:
        rec["verdict"] = "FAIL"
        rec["reasons"].append(f"degenerate dims {w}x{ht}")
    cap = _tgl_tier_cap(path)
    if cap is not None and max(w, ht) > cap:
        rec["verdict"] = "FAIL"
        rec["reasons"].append(f"tier upscale: max({w},{ht})>{cap} (cook must never upscale)")
    # NOTE: single-level (no in-file mip chain) is the NORM here -- LOD comes
    # from the tier-folder system (data/tgl/<N>) and small TXM-derived props are
    # cooked single-level by design. So lv==1 is NOT flagged. Only a PARTIAL
    # chain (lv>1 but not the full chain) is odd enough to surface.
    if lv > 1 and lv != _expected_mips(w, ht):
        if rec["verdict"] == "PASS":
            rec["verdict"] = "WARN"
        rec["reasons"].append(f"partial mip chain: levelCount {lv} != full {_expected_mips(w, ht)}")
    return rec


def check_burnin_jpg(path: Path, root: Path) -> dict:
    rec = _rec(path, root, "burnin_jpg")
    if not HAVE_PIL:
        rec["verdict"] = "SKIP"
        rec["reasons"].append("Pillow not installed")
        return rec
    try:
        with Image.open(path) as im:
            w, ht = im.size
    except Exception as exc:  # noqa: BLE001
        rec["verdict"] = "FAIL"
        rec["reasons"].append(f"decode failed: {exc}")
        return rec
    rec.update(width=w, height=ht)
    if w % 256 != 0 or ht % 256 != 0:
        rec["verdict"] = "FAIL"
        rec["reasons"].append(f"colormap {w}x{ht} not %256 (UV-decoupled sampling -> garbage)")
    return rec


def check_tile_tga(path: Path, root: Path) -> dict:
    rec = _rec(path, root, "tile_tga")
    if not HAVE_PIL:
        rec["verdict"] = "SKIP"
        rec["reasons"].append("Pillow not installed")
        return rec
    try:
        with Image.open(path) as im:
            w, ht = im.size
    except Exception as exc:  # noqa: BLE001
        rec["verdict"] = "FAIL"
        rec["reasons"].append(f"decode failed: {exc}")
        return rec
    rec.update(width=w, height=ht)
    if w != ht:
        rec["verdict"] = "FAIL"
        rec["reasons"].append(f"tile not square: {w}x{ht}")
    if w not in POT:
        rec["verdict"] = "FAIL"
        rec["reasons"].append(f"tile dim {w} not power-of-two")
    # square + power-of-two are the loader HARD rules (textureIsOKFormat ->
    # skip+MessageBox). dims==folder is a CONVENTION (the folder is the nominal
    # sampling tier); a square-POT tile of a different size still loads, so that
    # is a WARN (convention drift) not a loader failure.
    size = _detail_tile_size(path)
    if size is not None and (w != size or ht != size):
        if rec["verdict"] == "PASS":
            rec["verdict"] = "WARN"
        rec["reasons"].append(f"tile {w}x{ht} != folder nominal {size} (convention drift)")
    return rec


def classify(path: Path, root: Path):
    name = path.name.lower()
    if name.endswith(".ktx2"):
        return check_ktx2(path, root)
    if name.endswith(".burnin.jpg"):
        return check_burnin_jpg(path, root)
    # Only data/textures/<N>/ TGAs are loader-checked detail tiles. TGAs under
    # data/tgl/ are native-size stock prop art (no detail-tile rule).
    if name.endswith(".tga") and _detail_tile_size(path) is not None:
        return check_tile_tga(path, root)
    return None  # not a cooked texture we have a rule for


# ---------------------------------------------------------------------------
# Walk + report
# ---------------------------------------------------------------------------

def scan(roots, verbose=False):
    records = []
    for root in roots:
        root = Path(root)
        if not root.is_dir():
            print(f"{PREFIX} WARN: dir not found, skipping: {root}", file=sys.stderr)
            continue
        for dirpath, dirs, files in os.walk(str(root)):
            # UNWIRED-CHECK-CLOSE-1 (2026-07-30): do not grade cook SCRATCH.
            # This validator was reporting 100 FAILs -- every single one a
            # "vkFormat 0 not in cook-allowed" under _aoktxtmp/, which is the
            # temp dir tools/bt2018_import/cook_ao_from_package.py creates for
            # intermediate KTX2 output (see its `tmp = os.path.join(args.deploy,
            # "_aoktxtmp")`). Those files are transcoder scratch that never
            # ships, so their headers are not expected to satisfy the shipping
            # format rules. Grading them made the check permanently and
            # WRONGLY red, which is its own kind of blindness: a gate that is
            # always red teaches everyone to ignore it, and would have hidden a
            # real cooked-texture regression among the noise. Nobody saw it
            # because this check was wired to nothing.
            dirs[:] = [d for d in dirs if d not in COOK_SCRATCH_DIRS]
            dirs.sort()  # deterministic descent order -> byte-stable cook.json
            for fn in sorted(files):
                p = Path(dirpath) / fn
                rec = classify(p, root)
                if rec is not None:
                    records.append(rec)
    return records


# Cook SCRATCH directories -- transcoder intermediates that never ship, so the
# shipping format rules do not apply to them. Grading them is what made this
# validator permanently red on 100 phantom failures.
COOK_SCRATCH_DIRS = {"_aoktxtmp"}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Cooked-texture resolution validator + cook.json inventory.")
    ap.add_argument("--dir", action="append", default=[],
                    help="root(s) to scan (repeatable; default: deploy data/textures + data/tgl)")
    ap.add_argument("--manifest", help="write cook.json inventory here")
    ap.add_argument("--utc", default="", help="generated_utc stamp for the manifest (default empty = byte-stable)")
    ap.add_argument("--verbose", action="store_true", help="print PASS rows too")
    ap.add_argument("--self-test", action="store_true", help="run synthetic self-test")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if args.dir:
        roots = [Path(d) for d in args.dir]
    else:
        roots = [DEFAULT_DEPLOY / "data" / "textures", DEFAULT_DEPLOY / "data" / "tgl"]

    if not HAVE_PIL:
        print(f"{PREFIX} NOTE: Pillow absent -- JPG/TGA checks SKIPPED, KTX2 checks still run",
              file=sys.stderr)

    records = scan(roots, args.verbose)
    counts = {"PASS": 0, "WARN": 0, "FAIL": 0, "SKIP": 0}
    for r in records:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
        if r["verdict"] != "PASS" or args.verbose:
            detail = ("; ".join(r["reasons"]) or "ok")
            dims = f'{r["width"]}x{r["height"]}' if r["width"] else "?"
            print(f'{PREFIX} {r["verdict"]:4s} {r["path"]} [{dims}] {detail}')

    print(f"{PREFIX} scanned {len(records)} cooked textures: "
          f'PASS={counts["PASS"]} WARN={counts["WARN"]} '
          f'FAIL={counts["FAIL"]} SKIP={counts["SKIP"]}')

    if args.manifest:
        manifest = {
            "schema": "mc2-cook-inventory/1",
            "generated_utc": args.utc,
            "roots": [str(Path(r)).replace("\\", "/") for r in roots],
            "counts": counts,
            "entries": records,
        }
        outp = Path(args.manifest)
        outp.parent.mkdir(parents=True, exist_ok=True)
        outp.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print(f"{PREFIX} cook.json inventory written: {outp} ({len(records)} entries)")

    return 1 if counts["FAIL"] else 0


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def _write_ktx2(path: Path, vk: int, w: int, h: int, levels: int) -> None:
    hdr = KTX2_MAGIC + struct.pack("<9I", vk, 1, w, h, 0, 0, 1, levels, 0)
    path.write_bytes(hdr + b"\x00" * 16)


def self_test() -> int:
    import tempfile
    import shutil
    tmp = Path(tempfile.mkdtemp(prefix="texres_selftest_"))
    fails = []

    def expect(label, rec, want):
        got = rec["verdict"] if rec else "<none>"
        ok = got == want
        print(f"  [{'ok' if ok else 'FAIL'}] {label:34s} verdict={got} "
              f'reasons={"; ".join(rec["reasons"]) if rec else ""}')
        if not ok:
            fails.append(label)

    try:
        # --- KTX2 (no Pillow needed) ---
        tgl = tmp / "data" / "tgl" / "256"
        tgl.mkdir(parents=True)
        _write_ktx2(tgl / "good.ktx2", 146, 256, 256, _expected_mips(256, 256))
        expect("ktx2 good BC7 tier", classify(tgl / "good.ktx2", tmp), "PASS")
        _write_ktx2(tgl / "upscaled.ktx2", 146, 512, 512, _expected_mips(512, 512))
        expect("ktx2 tier upscale", classify(tgl / "upscaled.ktx2", tmp), "FAIL")
        _write_ktx2(tgl / "badfmt.ktx2", 999, 256, 256, _expected_mips(256, 256))
        expect("ktx2 bad vkFormat", classify(tgl / "badfmt.ktx2", tmp), "FAIL")
        # single-level is the norm (LOD via tier folders) -> accepted (PASS)
        _write_ktx2(tgl / "single.ktx2", 146, 256, 256, 1)
        expect("ktx2 single-level accepted", classify(tgl / "single.ktx2", tmp), "PASS")
        # partial mip chain (3 of 9 for 256) -> WARN
        _write_ktx2(tgl / "partial.ktx2", 146, 256, 256, 3)
        expect("ktx2 partial mip chain", classify(tgl / "partial.ktx2", tmp), "WARN")
        (tgl / "corrupt.ktx2").write_bytes(b"not a ktx2 file at all")
        expect("ktx2 corrupt header", classify(tgl / "corrupt.ktx2", tmp), "FAIL")

        if HAVE_PIL:
            tex = tmp / "data" / "textures"
            (tex / "128").mkdir(parents=True)
            Image.new("RGB", (128, 128)).save(tex / "128" / "good.tga")
            expect("tga good 128", classify(tex / "128" / "good.tga", tmp), "PASS")
            Image.new("RGB", (64, 128)).save(tex / "128" / "notsquare.tga")
            expect("tga not square", classify(tex / "128" / "notsquare.tga", tmp), "FAIL")
            Image.new("RGB", (256, 256)).save(tex / "128" / "wrongsize.tga")
            expect("tga wrong folder (square POT)", classify(tex / "128" / "wrongsize.tga", tmp), "WARN")
            Image.new("RGB", (96, 96)).save(tex / "128" / "notpot.tga")
            expect("tga not power-of-two", classify(tex / "128" / "notpot.tga", tmp), "FAIL")
            Image.new("RGB", (1024, 512)).save(tex / "good.burnin.jpg")
            expect("burnin %256 ok", classify(tex / "good.burnin.jpg", tmp), "PASS")
            Image.new("RGB", (1000, 512)).save(tex / "bad.burnin.jpg")
            expect("burnin not %256", classify(tex / "bad.burnin.jpg", tmp), "FAIL")
        else:
            print("  [skip] Pillow absent -- JPG/TGA checks not exercised")

        # --- manifest emission ---
        recs = scan([tmp])
        man = tmp / "cook.json"
        rc = main(["--dir", str(tmp), "--manifest", str(man)])
        ok = man.is_file() and man.stat().st_size > 0 and rc == 1  # has FAILs -> rc 1
        m = json.loads(man.read_text(encoding="utf-8"))
        ok = ok and m.get("schema") == "mc2-cook-inventory/1" and len(m["entries"]) == len(recs)
        print(f"  [{'ok' if ok else 'FAIL'}] manifest emission           "
              f"rc={rc} entries={len(m.get('entries', []))}")
        if not ok:
            fails.append("manifest")
    finally:
        shutil.rmtree(str(tmp), ignore_errors=True)

    if fails:
        print(f"SELF-TEST FAIL: {', '.join(fails)}")
        return 2
    print("SELF-TEST PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
