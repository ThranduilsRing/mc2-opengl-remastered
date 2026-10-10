#!/usr/bin/env python3
"""tools/asset_cook/tests/test_g2_textures.py — G2 KTX2 material cook gate.

Exercises `trackg_cook.py textures` with generated source textures (bigbox is
untextured, so we synthesize): one opaque + one alpha material. Asserts every
cooked tier is STORED BC7 (vkFormat 145/146, no supercompression — the loader
rejects anything else), the a_-prefix/alpha_test coherence holds, and the
emitted materials.json carries a full tier map.

  py -3 tools/asset_cook/tests/test_g2_textures.py
Exit 0 = pass.  Skips (exit 0) if ktx.exe or PIL is unavailable.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

HERE = Path(__file__).resolve().parent
COOK = HERE.parent / "trackg_cook.py"
KTX = Path(r"A:/Games/mc2-tools/ktx/ktx.exe")
TIERS = [128, 256]


def _gen(path: Path, mode: str):
    from PIL import Image
    img = Image.new(mode, (300, 300))
    px = img.load()
    for y in range(300):
        for x in range(300):
            r, g, b = (x * 255) // 299, (y * 255) // 299, ((x ^ y) * 255) // 299
            px[x, y] = (r, g, b, ((x + y) * 255) // 598) if mode == "RGBA" else (r, g, b)
    img.save(path, "PNG")


def main() -> int:
    try:
        import PIL  # noqa: F401
    except ImportError:
        print("SKIP test_g2_textures (PIL unavailable)")
        return 0
    if not KTX.is_file():
        print(f"SKIP test_g2_textures (ktx.exe missing: {KTX})")
        return 0

    errs: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        texdir = td / "src"; texdir.mkdir()
        _gen(texdir / "testprop.png", "RGB")    # opaque
        _gen(texdir / "leafprop.png", "RGBA")   # alpha (a_leafprop)
        staged = {"materials_discovered": [
            {"slot": 0, "textureName": "testprop", "alphaClass": 0},
            {"slot": 1, "textureName": "a_leafprop", "alphaClass": 1},
        ]}
        sp = td / "staged.json"; sp.write_text(json.dumps(staged))
        outj = td / "materials.json"
        deploy = td / "deploy"

        proc = subprocess.run(
            [sys.executable, str(COOK), "textures", "--staged", str(sp),
             "--texture-dir", str(texdir), "--out-root", str(deploy),
             "--out-json", str(outj), "--tiers", ",".join(map(str, TIERS))],
            capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
        if proc.returncode != 0:
            print("FAIL textures exited nonzero:\n" + proc.stdout + proc.stderr)
            return 1

        mats = json.loads(outj.read_text())["materials"]
        if len(mats) != 2:
            errs.append(f"want 2 materials, got {len(mats)}")

        # KTX2 BC7 header check (independent of the cook's own assert)
        def vkfmt(p: Path) -> int:
            import struct
            with open(p, "rb") as f:
                if f.read(12) != b"\xabKTX 20\xbb\r\n\x1a\n":
                    return -1
                vk, *_rest, sc = struct.unpack("<9I", f.read(36))
            return vk if sc == 0 else -1

        for mat in mats:
            tn = mat["textureName"]
            tm = mat.get("albedo_ktx2", {})
            if set(tm) != {str(t) for t in TIERS}:
                errs.append(f"{tn}: albedo_ktx2 tiers {set(tm)} != {TIERS}")
            for tier, rel in tm.items():
                f = deploy / rel
                if not f.exists():
                    errs.append(f"{tn} tier {tier}: missing {f}")
                    continue
                vk = vkfmt(f)
                if vk not in (145, 146):
                    errs.append(f"{tn} tier {tier}: not stored BC7 (vkFormat={vk})")
            # a_ / alpha_test coherence
            at = mat["flags"]["alpha_test"]
            if (mat["alphaClass"] == 1) != at or (tn.startswith("a_")) != (mat["alphaClass"] == 1):
                errs.append(f"{tn}: alphaClass/alpha_test/a_ disagree")

    if errs:
        print(f"FAIL test_g2_textures ({len(errs)}):")
        for e in errs:
            print(f"  - {e}")
        return 1
    print("PASS test_g2_textures (opaque + a_ alpha materials cooked to stored-BC7 KTX2 "
          "tiers 128/256; tier map + a_/alpha_test coherence)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
