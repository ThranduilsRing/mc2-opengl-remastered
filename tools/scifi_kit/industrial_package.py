"""Cook the owner-approved industrial intent bakes as a small, pristine-only mod.

Uses the common IR cooker. Reads art exports without modifying the art worktree.
Generated geometry/textures stay outside git. Run from the repository root:
  py -3 tools/scifi_kit/industrial_package.py --out E:/mc2-archive-2026-10/proofs/kit-ingame-2/package
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.content_pipeline import package
from tools.content_pipeline.frontends import gltf_file

ROUND1 = "smallfueltank pipetank largefueltank liquidstorage watertower straightpipe1 straightpipe2 straightpipe3 cornerpipe threewaypipe fourwaypipe endpipe fuelpipe industrialplant heavyindustrial factorymd".split()
ROUND2 = "factorysm factorylg industrialcomplex processingplant purificationplant chemicalplant valvestation wasteprocessor weaponsfacility grainstorage civstoragea civstorageb civstoragec gantry crane".split()
BAKES = {**dict.fromkeys(ROUND1, "t1"), **dict.fromkeys(ROUND2, "r2"),
         "transformerstation": "e1", "powergenerator": "b7", "l_turretgenerator": "b7"}
REVISIONS = {"t1": "round 1", "r2": "round 2", "e1": "7809b383d", "b7": "1e3d3bca3"}


def assets(root, work):
    for name, tag in BAKES.items():
        folder = root / "_work" / tag / "pbr"
        stamp = json.loads((folder / f"{name}_pristine-bake.json").read_text())
        paths = [folder / f"{name}_pristine_lod{i}.glb" for i in range(3)]
        if len(stamp["lods"]) != 3 or not all(p.is_file() for p in paths):
            raise ValueError(f"{name}: approved three-LOD bake missing")
        a = gltf_file.to_ir(name, f"staticProp:{name}", paths[0],
                            lods=[(paths[i], stamp["lods"][i]["distance_wu"]) for i in (1, 2)],
                            work_dir=work / name)
        # Approved exports already carry TG-depth/1 node transforms. The IR
        # preserves those transforms and fixes mirrored winding when flattening.
        repaired = 0
        for lod in a.states["intact"]:
            for node, world in zip(lod.scene.nodes, lod.scene.world_matrices()):
                if node.extras.get("mc2_anim_node"):
                    node.extras["mc2_pivot_tg_wu"] = world[:3, 3].tolist()
            for mesh in lod.scene.meshes:
                for p in mesh.primitives:
                    if p.tangents is None:
                        continue
                    n = p.normals / np.maximum(np.linalg.norm(p.normals, axis=1, keepdims=True), 1e-20)
                    t = p.tangents[:, :3]
                    t -= n * np.sum(t * n, axis=1, keepdims=True)
                    bad = np.linalg.norm(t, axis=1) < 1e-7
                    t[bad] = np.cross(np.eye(3)[np.argmin(abs(n[bad]), axis=1)], n[bad])
                    t /= np.maximum(np.linalg.norm(t, axis=1, keepdims=True), 1e-20)
                    p.tangents[:, 3] = np.where(p.tangents[:, 3] < 0, -1, 1)
                    repaired += int(bad.sum())
        a.frontend.update(approved_bake=tag, art_revision=REVISIONS[tag], tangent_repairs=repaired)
        print(f"cook {name}: {tag}", flush=True)
        yield a


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", type=Path, default=Path("E:/mc2-ref/scifi-kit/art/intent-pass"))
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--overwrite", action="store_true")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix="industrial-", dir=args.out.parent) as tmp:
        package.build_package(assets(args.source, Path(tmp)), args.out, mod_id="mc2-industrial-kit",
                              name="Industrial remakes", description="34 owner-approved pristine industrial remakes; stock damage and collision.",
                              overwrite=args.overwrite)
    # Runtime uses compressed sidecars; omit duplicate uncompressed albedo.
    for p in (args.out / "data/tgl").rglob("*.tga"):
        if not p.with_suffix(".ktx2").is_file():
            raise ValueError(f"missing compressed texture: {p}")
        p.unlink()
    print(f"package: {args.out}", flush=True)


if __name__ == "__main__":
    main()
