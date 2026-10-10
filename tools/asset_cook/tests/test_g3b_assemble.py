#!/usr/bin/env python3
"""tools/asset_cook/tests/test_g3b_assemble.py — G3b assembly + projection + no-central-write.

End-to-end: stage bigbox -> cook a (synthesized) albedo -> assemble. Asserts the
full manifest is schema-valid, the projected models.generated.json would resolve
in the registry (Python mirror), capabilities are derived correctly, and the
central models.json is NEVER touched (Patch 4: sha256+mtime before/after).

  py -3 tools/asset_cook/tests/test_g3b_assemble.py
Exit 0 = pass.  Skips if ktx.exe / PIL unavailable.
"""
from __future__ import annotations

import hashlib
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
VALIDATOR = HERE.parent / "validate_asset_manifest.py"
FIXTURE = HERE / "fixtures" / "bigbox.glb"
KTX = Path(r"A:/Games/mc2-tools/ktx/ktx.exe")


def run(*a) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, *map(str, a)], capture_output=True, text=True, **console_child.child_kwargs(redirected=True))


def _sig(p: Path):
    st = p.stat()
    return (hashlib.sha256(p.read_bytes()).hexdigest(), st.st_mtime_ns, st.st_size)


def main() -> int:
    try:
        from PIL import Image
    except ImportError:
        print("SKIP test_g3b_assemble (PIL unavailable)"); return 0
    if not KTX.is_file():
        print(f"SKIP test_g3b_assemble (ktx.exe missing)"); return 0

    errs: list[str] = []
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        deploy = td / "deploy"
        bundle = deploy / "data" / "model_overrides" / "cooked" / "bigbox"
        texdir = td / "src"; texdir.mkdir(parents=True)

        # 1. stage
        r = run(COOK, "stage", FIXTURE, bundle, "--id", "bigbox", "--class", "staticprop",
                "--appearance", "hangar", "--source-rel", "data/model_overrides/source/props/bigbox.glb")
        if r.returncode != 0:
            print("FAIL stage:\n" + r.stdout + r.stderr); return 1
        staged = json.loads((bundle / "staged.json").read_text())
        texname = staged["materials_discovered"][0]["textureName"]  # 'bigbox_mat'

        # 2. synthesize the discovered albedo + cook
        img = Image.new("RGB", (256, 256))
        for y in range(256):
            for x in range(256):
                img.putpixel((x, y), (x, y, (x ^ y) & 255))
        img.save(texdir / f"{texname}.png", "PNG")
        r = run(COOK, "textures", "--staged", bundle / "staged.json", "--texture-dir", texdir,
                "--out-root", deploy, "--out-json", bundle / "materials.json", "--tiers", "128,256")
        if r.returncode != 0:
            print("FAIL textures:\n" + r.stdout + r.stderr); return 1

        # 3. plant a sentinel CENTRAL models.json and snapshot it
        central = deploy / "data" / "model_overrides" / "models.json"
        central.write_text(json.dumps({"overrides": [
            {"type": "model", "class": "tree", "replaces": "tree:tc1_1",
             "source": "source/trees/tree_lush.glb", "renderOnly": True, "scale": 1.0, "fallback": "stock"}
        ]}), encoding="utf-8")
        before = _sig(central)

        # 4. assemble
        r = run(COOK, "assemble", "--staged", bundle / "staged.json", "--materials", bundle / "materials.json",
                "--out-dir", bundle, "--override-source", "cooked/bigbox/bigbox.glb")
        if r.returncode != 0:
            print("FAIL assemble:\n" + r.stdout + r.stderr); return 1

        # 4a. central models.json untouched (Patch 4)
        if _sig(central) != before:
            errs.append("CENTRAL models.json was modified by assemble (no-central-write violated)")

        # 4b. full manifest schema-valid (independent re-check)
        man = bundle / "manifest.json"
        if run(VALIDATOR, man).returncode != 0:
            errs.append("assembled manifest.json failed schema validation")
        m = json.loads(man.read_text())
        caps = m["capabilities"]
        if caps["alphaTest"] is not False or caps["hasCookedGlb"] is not True or caps["hasLodChain"] is not False:
            errs.append(f"capabilities derived wrong: {caps}")
        if m["deps"]["stockFallback"] != "hangar":
            errs.append("deps.stockFallback != hangar")
        if not m["provenance"]["sourceSha256"]:
            errs.append("provenance.sourceSha256 empty")

        # 4c. projection resolves
        gen = json.loads((bundle / "models.generated.json").read_text())
        e = gen["overrides"][0]
        if e["replaces"] != "staticprop:hangar" or e["source"] != "cooked/bigbox/bigbox.glb" \
                or e["renderOnly"] is not True or e["scale"] != 1.0 or e["fallback"] != "stock":
            errs.append(f"projection entry wrong: {e}")

    if errs:
        print(f"FAIL test_g3b_assemble ({len(errs)}):")
        for x in errs:
            print(f"  - {x}")
        return 1
    print("PASS test_g3b_assemble (stage->textures->assemble: schema-valid manifest, "
          "resolvable projection, derived caps, central models.json untouched)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
