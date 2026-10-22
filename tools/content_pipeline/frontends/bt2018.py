"""BattleTech 2018 mech GLB (+ bt2018_mech_package.json) -> IR.

The landing point for the BT2018 first-class chassis plan (E:/mc2-archive-2026-10/plans/
mech-import-plan-2026-10-22.md, packets P1/P11/P13/P14): the IR keeps what the runtime importer
still merges away -- every part's own material slot, the skeleton and clip names, sockets
(weapon mounts / muzzles, cockpit, feet), part/damage regions and LOD chains -- and records the
source bytes. Geometry stays on the skinned importer path ([Import] Source=), so this frontend
produces IR + validation for the package compiler; it does not cook a runtime model override.

Material policy: the package's albedo/normal/AO are authored BT data, but a part gets an
authored class only from a reviewed per-chassis profile ({material name: class}); otherwise the
slot stays legacy_prelit and its maps are listed as unreviewed in the frontend report. FBX2glTF's
synthetic metallic/roughness factors are never treated as authored PBR.
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np

from .. import ir
from . import gltf_file

SOCKET_KINDS = {"weapon_": "muzzle", "foot": "foot", "jumpjet": "jumpjet", "cockpit": "attach", "joint_": "attach",
                "hit_": "attach"}


def _socket_kind(key):
    return next((kind for prefix, kind in SOCKET_KINDS.items() if key.startswith(prefix)), "attach")


def to_ir(glb, package_json=None, *, classes=None, parts=(), work_dir=None):
    glb = Path(glb)
    pkg = json.loads(Path(package_json).read_text(encoding="utf-8")) if package_json else {}
    chassis = pkg.get("chassisId") or glb.stem.lower()
    # Units: BT GLBs are authored at Unity scale; the importer fits height at runtime
    # (MC2_MECH_SKEL_HEIGHT). The IR keeps source units; the chassis profile owns the scale.
    asset = gltf_file.to_ir(chassis, f"mech:{chassis}", glb, kind="mech", classes=classes or {},
                            meta={"frame": "gltf", "unclassified": "legacy",
                                  "license": "owner's BattleTech 2018 install (personal use)"},
                            work_dir=work_dir, allow_skin=True)
    joints = set(asset.skeleton.joints if asset.skeleton else [])
    for key, joint in pkg.get("nodes", {}).items():
        if joint in joints:
            asset.sockets.append(ir.Socket(key, _socket_kind(key), joint, np.eye(4)))
    asset.parts.extend(parts)
    if package_json:
        asset.provenance.append(ir.Source.of("mech_package", package_json))
    unreviewed = {k: v.get("tga") for k, v in pkg.get("materials", {}).items()}
    asset.frontend.update({"name": "bt2018", "chassis": chassis, "featureBits": pkg.get("featureBits", []),
                           "unreviewed_maps": unreviewed,
                           "slots": [m.name for m in asset.materials]})
    return asset.check()
