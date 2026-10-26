"""
mech_bone_parity.py — MECH-BONE-PARITY-GATE-1

Proves the ENGINE import and the standalone harness ORACLE compute identical
skeleton bone matrices for a mech GLB. Both run game-free in seconds:

  oracle = mech_import_harness gpu-bones --rest   (mc2skel directly)
  engine = tg_import_dump <glb> with MC2_MECH_SKEL_BONE_DUMP=<path>
           (runs the REAL ImportGeometryFromFile bake, dumps the bones it consumes)

Exit 0 if the bone sets match within epsilon; nonzero on any mismatch. This is
the regression guard for the shared mc2skel path and the scaffold for 1B: when the
engine animates per-frame (EvaluateClipGpuBones), point both sides at the same
--clip/--frame instead of --rest.

  py -3 scripts/mech_bone_parity.py [--glb <path>] [--eps 1e-4]
        [--harness <exe>] [--tgdump <exe>]
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

ROOT = Path(__file__).resolve().parent.parent
DEF_GLB = Path(r"A:/Games/mc2-opengl/BattleTech_2018_Dump/MadCat/marauder_fbx2gltf.glb")
DEF_HARNESS = ROOT / "build64-harness/RelWithDebInfo/mech_import_harness.exe"
DEF_TGDUMP = ROOT / "build64-tgdump/RelWithDebInfo/tg_import_dump.exe"


def load_bones(doc):
    """name -> [16] from a {bones:[{name,m}]} doc."""
    return {b["name"]: b["m"] for b in doc.get("bones", [])}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--glb", default=str(DEF_GLB))
    ap.add_argument("--eps", type=float, default=1e-4)
    ap.add_argument("--clip", default="", help="forced clip name (default: rest pose)")
    ap.add_argument("--frame", type=int, default=0, help="clip frame (with --clip)")
    ap.add_argument("--harness", default=str(DEF_HARNESS))
    ap.add_argument("--tgdump", default=str(DEF_TGDUMP))
    args = ap.parse_args()

    for p in (args.harness, args.tgdump, args.glb):
        if not Path(p).is_file():
            sys.exit(f"missing: {p}")

    tmp = Path(tempfile.mkdtemp(prefix="bone_parity_"))
    oracle_json = tmp / "oracle.json"
    engine_json = tmp / "engine.json"

    posed = bool(args.clip)
    label = f"{args.clip}@{args.frame}" if posed else "rest"
    print(f"[bone_parity] pose = {label}")

    # Oracle: harness gpu-bones (--rest | --clip/--frame).
    oracle_cmd = [args.harness, "gpu-bones", args.glb, "--out", str(oracle_json)]
    oracle_cmd += (["--clip", args.clip, "--frame", str(args.frame)] if posed else ["--rest"])
    subprocess.run(oracle_cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **console_child.child_kwargs(redirected=True))

    # Engine: tg_import_dump runs the real import bake; MC2_MECH_SKEL_BONE_DUMP writes
    # bones. MC2_MECH_IMPORT_FORCE_CLIP/_FRAME pose the bake to the same clip frame.
    env = os.environ.copy()
    env["OMP_NUM_THREADS"] = "1"
    env["MC2_MECH_SKEL_BONE_DUMP"] = str(engine_json)
    if posed:
        env["MC2_MECH_IMPORT_FORCE_CLIP"] = args.clip
        env["MC2_MECH_IMPORT_FORCE_FRAME"] = str(args.frame)
    else:
        env.pop("MC2_MECH_IMPORT_FORCE_CLIP", None)
        env.pop("MC2_MECH_IMPORT_FORCE_FRAME", None)
    subprocess.run([args.tgdump, args.glb], check=True, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **console_child.child_kwargs(redirected=True))

    if not oracle_json.exists() or not engine_json.exists():
        sys.exit("FAIL: one side produced no bone dump")

    oracle = json.loads(oracle_json.read_text())
    engine = json.loads(engine_json.read_text())
    ob, eb = load_bones(oracle), load_bones(engine)

    print(f"[bone_parity] oracle bones={len(ob)} checksum={oracle.get('checksum')}")
    print(f"[bone_parity] engine bones={len(eb)} checksum={engine.get('checksum')}")

    fails = []
    if set(ob) != set(eb):
        only_o = sorted(set(ob) - set(eb))
        only_e = sorted(set(eb) - set(ob))
        if only_o: fails.append(f"bones only in oracle: {only_o[:5]}")
        if only_e: fails.append(f"bones only in engine: {only_e[:5]}")

    worst = 0.0; worst_bone = ""
    for name in set(ob) & set(eb):
        for a, b in zip(ob[name], eb[name]):
            d = abs(a - b)
            if d > worst: worst, worst_bone = d, name
            if d > args.eps:
                fails.append(f"bone '{name}' element diff {d:.6f} > eps {args.eps}")
                break

    print(f"[bone_parity] max per-element diff = {worst:.8f} (at {worst_bone or 'n/a'})")
    if fails:
        print("PARITY FAILED:")
        for f in fails[:10]:
            print("  - " + f)
        return 1
    print(f"PARITY OK ({len(ob)} bones, all within eps {args.eps})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
