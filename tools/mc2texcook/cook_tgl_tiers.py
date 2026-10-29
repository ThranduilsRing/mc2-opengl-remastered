#!/usr/bin/env python3
"""cook_tgl_tiers.py -- cook upscaled tgl .tga (from a release zip OR a dir) into
per-resolution BC7 KTX2 tiers (data/tgl/{256,512,1024}/<name>.ktx2).

Each tier caps the LONGEST edge at the tier value; sources are never upscaled
(a 512 source in the 1024 tier stays 512). Albedo/sRGB pipeline via the
KTX-Software ktx CLI (uastc encode -> bc7 transcode), matching batch_cook.py.

Test first:  --test            (cooks ONE source per distinct native size)
Full run:    (no --test/--limit)  cooks every .tga

Usage:
    py -3 cook_tgl_tiers.py --zip <mc2-tgl.zip> --deploy <.../data/tgl>
        [--tiers 256,512,1024] [--test] [--limit N] [--preset albedo]
"""
import argparse, io, os, subprocess, sys, tempfile, time, zipfile
from pathlib import Path
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.content_pipeline import provenance
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import console_child

KTX = r"A:/Games/mc2-tools/ktx/ktx.exe"


def _ktx_cook(img: Image.Image, dst: Path, srgb: bool):
    dst.parent.mkdir(parents=True, exist_ok=True)
    fmt = "R8G8B8A8_SRGB" if srgb else "R8G8B8A8_UNORM"
    tf  = "srgb" if srgb else "linear"
    with tempfile.TemporaryDirectory() as td:
        png = Path(td) / "s.png"
        uastc = Path(td) / "u.ktx2"
        img.convert("RGBA").save(png, "PNG")
        for step in (
            [KTX, "create", "--encode", "uastc", "--format", fmt,
             "--threads", "1", "--assign-tf", tf, "--generate-mipmap", str(png), str(uastc)],
            [KTX, "transcode", "--target", "bc7", str(uastc), str(dst)],
        ):
            r = subprocess.run(step, capture_output=True, text=True,
                               env=dict(os.environ, OMP_NUM_THREADS="1"),
                               **console_child.child_kwargs(redirected=True))
            if r.returncode != 0:
                raise RuntimeError(f"ktx {step[1]} rc={r.returncode}: "
                                   f"{(r.stderr or r.stdout).strip()[:200]}")




def _resized(img: Image.Image, cap: int) -> Image.Image:
    w, h = img.size
    m = max(w, h)
    if m <= cap:
        return img                      # never upscale
    if w >= h:
        nw, nh = cap, max(1, round(h * cap / w))
    else:
        nw, nh = max(1, round(w * cap / h)), cap
    return img.resize((nw, nh), Image.LANCZOS)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--zip", type=Path, required=True)
    ap.add_argument("--deploy", type=Path, required=True, help="deploy data/tgl dir (tiers written under it)")
    ap.add_argument("--tiers", default="256,512,1024")
    ap.add_argument("--preset", default="albedo", choices=["albedo", "orm", "normal"])
    ap.add_argument("--test", action="store_true", help="cook ONE source per distinct native size")
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    if not Path(KTX).is_file():
        print(f"ERROR: ktx CLI missing: {KTX}", file=sys.stderr); return 1
    tiers = [int(t) for t in args.tiers.split(",") if t.strip()]
    srgb = args.preset == "albedo"

    zf = zipfile.ZipFile(args.zip)
    entries = sorted(e for e in zf.namelist() if e.lower().endswith(".tga"))

    # read native size from TGA header (w@12, h@14, LE uint16)
    def native(e):
        b = zf.read(e)[:18]
        return int.from_bytes(b[12:14], "little"), int.from_bytes(b[14:16], "little"), b

    if args.test:
        seen, pick = set(), []
        for e in entries:
            w, h, _ = native(e)
            key = max(w, h)
            if key not in seen:
                seen.add(key); pick.append(e)
        entries = pick
        print(f"[test] one source per native size -> {len(entries)} files: {[Path(e).name for e in entries]}")
    elif args.limit:
        entries = entries[:args.limit]

    print(f"cook_tgl_tiers: {len(entries)} sources x {len(tiers)} tiers -> {args.deploy}")
    t0 = time.time(); cooked = failed = 0
    for i, e in enumerate(entries, 1):
        stem = Path(e).name
        try:
            raw = zf.read(e)
            img = Image.open(io.BytesIO(raw)).convert("RGBA")
            nw, nh = img.size
            outs = []
            for cap in tiers:
                ri = _resized(img, cap)
                dst = args.deploy / str(cap) / (Path(stem).stem + ".ktx2")
                _ktx_cook(ri, dst, srgb)
                provenance.record(dst, source_hash=provenance.sha256(raw), tool="mc2-tgl-tiers",
                    version=provenance.tool_version(__file__, KTX),
                    params={"preset": args.preset, "tier": cap, "resize": "lanczos-never-upscale",
                            "width": ri.width, "height": ri.height, "archiveEntry": e})
                outs.append(f"{cap}:{ri.size[0]}x{ri.size[1]}")
                cooked += 1
            print(f"  [{i}/{len(entries)}] {stem} (src {nw}x{nh}) -> {' '.join(outs)}")
        except Exception as ex:
            failed += 1
            print(f"  [{i}/{len(entries)}] {stem} FAILED: {type(ex).__name__}: {ex}", file=sys.stderr)
    print(f"DONE {time.time()-t0:.1f}s  cooked={cooked} failed={failed}")
    return 1 if failed and cooked == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
