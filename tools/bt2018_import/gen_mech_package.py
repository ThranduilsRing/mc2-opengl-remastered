#!/usr/bin/env python3
"""BT2018-MECH-NODE-MANIFEST-1A — generate per-mech bt2018_mech_package.json.

First tenant of the asset-package layer: declares the MC2 semantic node -> source
joint map the importer consumes to resolve weapon firepoints + hit nodes (read-only
bone-world lookup; no FK/clip/shader/gameplay change). Default map is constant (all
BT mechs share the Atlas rig) but every file is per-mech, so it is override-capable —
edit the json (or DEFAULT_NODES here) for a mech whose rig differs, without engine code.

Writes MC2Conversions/<bt>/bt2018_mech_package.json and deploys the engine sidecar
<deploy>/<bt>.package.json (the importer reads <glbstem>.package.json next to the GLB).
Validates each source joint exists in that mech's GLB; WARNs (does not fail) on miss.

Usage:
  py -3 gen_mech_package.py --manifest MC2Conversions/conversions_manifest.json \
      --conversions MC2Conversions --deploy <release>/data/tgl
"""
import argparse
import json
import os
from pygltflib import GLTF2

# MC2 semantic node name -> source joint (Atlas rig). Override per-mech in the json.
DEFAULT_NODES = {
    "joint_torso":        "j_Spine2",
    # j_LHand / j_RHand are nodes in the GLB but the runtime bone list only
    # contains joints actually used by skinned meshes; hands are unskinned in
    # these rigs. j_LForearm / j_RForearm are in-skin and give a plausible
    # arm-tip fire point.
    "weapon_rightarm":    "j_RForearm",
    "weapon_leftarm":     "j_LForearm",
    "weapon_righttorso":  "j_RClavicle",
    "weapon_lefttorso":   "j_LClavicle",
    "weapon_centertorso": "j_Spine2",
    "cockpit":            "j_COCKPIT",
    "hit_left":           "j_LClavicle",
    "hit_right":          "j_RClavicle",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--conversions", required=True)
    ap.add_argument("--deploy", required=True)
    ap.add_argument("--dump", default="A:/Games/mc2-opengl/BattleTech_2018_Dump",
                    help="BT2018 dump root (AO -amb maps live in Texture2D/, NOT the GLB)")
    args = ap.parse_args()

    with open(args.manifest, encoding="utf-8") as f:
        mechs = [r["btname"] for r in json.load(f)]

    # AO (-amb) is NOT embedded in the GLB (FBX2glTF embedded only FBX-referenced
    # albedo + normal). It lives in the dump Texture2D/ as the chassis BASE atlas AO,
    # skin-independent (one -Base-amb per chassis, shared by BlackWidow/SLDF/BHA since
    # AO is geometry-based). Index the canonical (no Unity '#hash' dupe) -Base-amb pngs.
    import re as _re
    tex2d = os.path.join(args.dump, "Texture2D")
    # chrTxrMech_<chassis>-Base-amb[.png] or with a -NNN export-index suffix
    # (e.g. shadowhawk-base-amb-001.png). Skip Unity '#hash' dupes.
    _ao_re = _re.compile(r"^chrtxrmech_(.+?)-base-amb(?:-\d+)?\.png$")
    # NORMAL (-nrm) mirrors AO EXACTLY: dump-sourced, base-chassis-keyed,
    # skin-independent. Pick the CLEAN base-chassis PNG (no Unity '#hash').
    _nrm_re = _re.compile(r"^chrtxrmech_(.+?)-base-nrm(?:-\d+)?\.png$")
    ao_index = {}   # lowercase chassis -> AO source png path
    nrm_index = {}  # lowercase chassis -> normal source png path
    if os.path.isdir(tex2d):
        for fn in os.listdir(tex2d):
            if "#" in fn:
                continue
            m = _ao_re.match(fn.lower())
            if m:
                ao_index.setdefault(m.group(1), os.path.join(tex2d, fn))
            mn = _nrm_re.match(fn.lower())
            if mn:
                nrm_index.setdefault(mn.group(1), os.path.join(tex2d, fn))

    total_missing = 0
    ao_missing = []
    nrm_missing = []
    for bt in mechs:
        glb = os.path.join(args.conversions, f"{bt}.glb")
        if not os.path.isfile(glb):
            print(f"[pkg] SKIP {bt}: glb missing")
            continue
        g = GLTF2().load(glb)
        # Validate against the SKIN bone set, NOT every glTF node — the engine's
        # mc2skel::BuildSkeleton only records joints actually used by skinned
        # meshes (assimp_importer.cpp `e.names`). Nodes outside any skin
        # (e.g. j_LHand on these rigs) will pass a node-list check but fail at
        # runtime resolution.
        joints = set()
        for sk in (g.skins or []):
            for ji in sk.joints:
                n = g.nodes[ji]
                if n.name:
                    joints.add(n.name)
        nodes = dict(DEFAULT_NODES)
        missing = sorted({j for j in nodes.values() if j not in joints})
        if missing:
            total_missing += 1
            print(f"[pkg] WARN {bt}: source joints absent in rig: {missing}")

        # MATERIAL-PACKAGE-CONTRACT: declare the material maps embedded in the GLB.
        # The runtime tga-stem (deriveName = lowercase image stem + .tga) is what the
        # engine resolves; the cooked artifact is <stem>.ktx2 (BC7). featureBits drive
        # binding + shader. albedo is always present; ao/normal declared if embedded.
        def _img_stem(substr, exclude="blip"):
            for im in (g.images or []):
                if im.name and substr in im.name.lower() and exclude not in im.name.lower():
                    return os.path.splitext(im.name)[0].lower()  # chrtxrmech_<...>-<map>
            return None
        materials = {}
        feature_bits = []
        alb = _img_stem("-base-alb") or _img_stem("alb")   # embedded in GLB
        if alb:
            materials["albedo"] = {"tga": alb + ".tga", "ktx2": alb + ".ktx2",
                                   "logicalFormat": "srgb-rgb", "container": "bc7"}
        # AO + NORMAL from the DUMP (not GLB), keyed by base chassis (variant suffix
        # stripped; both are geometry-based / skin-independent — one -Base-amb and one
        # -Base-nrm per chassis). Normal mirrors AO EXACTLY: BC7 (the KTX_PRIMARY decoder
        # only handles BC7 145/146; BC5 is decoder-blocked), full XYZ in RGB.
        # AO is skin-independent). logicalFormat = linear single-channel; the CURRENT
        # container is BC7 (compatibility — the KTX_PRIMARY decoder only handles BC7
        # 145/146; a BC4/R8 path does not exist yet). DO NOT treat AO as color/sRGB.
        chassis = bt.split("_")[0].lower()                 # warhammer_BlackWidow -> warhammer
        ao_src = ao_index.get(chassis)
        if ao_src:
            ao_stem = f"chrtxrmech_{chassis}-base-amb"
            materials["ao"] = {"tga": ao_stem + ".tga", "ktx2": ao_stem + ".ktx2",
                               "logicalFormat": "linear-grayscale",
                               "container": "bc7-compat",  # ideal BC4/R8; BC7 until decoder supports it
                               "source": "dump:Texture2D/" + os.path.basename(ao_src),
                               "skinIndependent": True}
            feature_bits.append("HAS_AO")
        else:
            ao_missing.append(bt)
        nrm_src = nrm_index.get(chassis)
        if nrm_src:
            nrm_stem = f"chrtxrmech_{chassis}-base-nrm"
            materials["normal"] = {"tga": nrm_stem + ".tga", "ktx2": nrm_stem + ".ktx2",
                                   "logicalFormat": "linear-normal-xyz",
                                   "container": "bc7",
                                   "source": "dump:Texture2D/" + os.path.basename(nrm_src),
                                   "skinIndependent": True}
            feature_bits.append("HAS_NORMAL")
        else:
            nrm_missing.append(bt)
        pkg = {
            "schema": "bt2018-mech-package/2",
            "chassisId": bt,
            "sourceGlb": f"{bt}.glb",
            "clips": {"showcase": "atlas_moveCoreIdle"},
            "nodes": nodes,
            "materials": materials,
            "featureBits": feature_bits,
        }
        # source-of-truth copy next to the conversion
        asset_dir = os.path.join(args.conversions, bt)
        os.makedirs(asset_dir, exist_ok=True)
        text = json.dumps(pkg, indent=2)
        with open(os.path.join(asset_dir, "bt2018_mech_package.json"), "w", encoding="utf-8") as f:
            f.write(text)
        # engine sidecar: <deploy>/<bt>.package.json (read as <glbstem>.package.json)
        with open(os.path.join(args.deploy, f"{bt}.package.json"), "w", encoding="utf-8") as f:
            f.write(text)
        print(f"[pkg] {bt}: {len(nodes)} nodes, materials={sorted(materials)} feat={feature_bits}"
              + (" (joint warnings)" if missing else ""))

    print(f"\n[pkg] done: {len(mechs)} mechs, {total_missing} with missing-joint warnings")
    if ao_missing:
        print(f"[pkg] AO MISSING (no dump -Base-amb for base chassis) for {len(ao_missing)}: {sorted(ao_missing)}")
    else:
        print(f"[pkg] AO: all mechs resolved a dump -Base-amb source")
    if nrm_missing:
        print(f"[pkg] NORMAL MISSING (no dump -Base-nrm for base chassis) for {len(nrm_missing)}: {sorted(nrm_missing)}")
    else:
        print(f"[pkg] NORMAL: all mechs resolved a dump -Base-nrm source")


if __name__ == "__main__":
    main()
