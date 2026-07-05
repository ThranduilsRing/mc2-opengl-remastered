#!/usr/bin/env python3
"""install_tree_scatter.py -- installer for TERRAIN-TREE-SCATTER-V0.

Installs three dedicated TREE-class scatter clones -- "PineScatter1/2/3" --
that share stock AppearanceName="Pine1"/"Pine2"/"Pine3" (all fully resolved:
shapes packed in tgl.fst as data/tgl/pine{1,2,3}.tgl + pine{1,2,3}l1.tgl +
pine{1,2,3}x.tgl, icon art packed in art.fst as data/art/tree_pine{1,2,3}.tga
-- verified via tools/mod_install/fst_listing.py, zero art copying needed).

This mirrors tools/install_cliff_dressing.py's registration pattern exactly:
one FIT packet appended to data/objects/object2.pak per clone (TREE_TYPE
dispatch code 0, [TreeData] block, AppearanceName pointing at the existing
stock pine appearance) + one Buildings.csv line (+gui copy) per clone.

Why a DEDICATED clone instead of reusing stock Pine1/2/3 (FitID 210/352/353)
directly: those FitIDs are also the HAND-PLACEABLE stock catalog entries.
Reusing them as the scatter cook's objTypeNum would mean a re-cook's
"delete prior scatter records by objTypeNum" step could nuke hand-placed
stock pines sharing that same type (identical hazard to the MarbleCliff/
MarbleCliffScatter split, fa1cf3b0). PineScatter1/2/3 get their OWN
objTypeNums (1190/1191/1192, next free after MarbleCliffScatter's 1189) so
a scatter re-cook only ever touches its own records.

Always backs up object2.pak and both Buildings.csv files first (suffix
".bak_treescatter"; skipped if a backup already exists, so re-running is
safe). No new .glb/.ini/.tga is copied -- all three AppearanceNames resolve
against packed stock content already present in any deploy.

Usage:
  python install_tree_scatter.py --deploy-root "A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0"

To revert: restore the three ".bak_treescatter" files over their originals.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# (clone name, appearance name reused, fit template file, expected fitID)
CLONES = [
    ("PineScatter1", "Pine1", "pinescatter1_fit_template.txt", 1190),
    ("PineScatter2", "Pine2", "pinescatter2_fit_template.txt", 1191),
    ("PineScatter3", "Pine3", "pinescatter3_fit_template.txt", 1192),
]

# Buildings.csv row format (art/Buildings.csv header):
# File Name,Group ID,NameID,Type,FitID,SPECIAL TYPE,ALIGNMENT?,CAPTURABLE,
# Draw Building On TacMap,TGA Filename (if any),Mechlopedia Scale,
# Mission Briefing scale
# Mirrors stock Pine1 row: Pine1,1,30351,TREE,210,TREE8,0,0,0,tree_pine1,,1
CSV_LINE_TEMPLATE = "{name},1,30351,TREE,{fit_id},TREE8,0,0,0,tree_pine{n},,1\r\n"


def backup(path: Path, suffix: str) -> None:
    bak = path.with_name(path.name + suffix)
    if bak.exists():
        print(f"  backup already exists, skipping: {bak}")
        return
    shutil.copyfile(path, bak)
    print(f"  backed up {path} -> {bak}")


def append_fit_packet(obj_pak: Path, fit_text: Path) -> int:
    pak_append = REPO_ROOT / "tools" / "pak_append.py"
    result = subprocess.run(
        [sys.executable, str(pak_append), "append", "--pak", str(obj_pak),
         "--text-file", str(fit_text)],
        capture_output=True, text=True)
    print(result.stdout)
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        raise RuntimeError("pak_append failed")
    return int(result.stdout.strip().splitlines()[-1])


def install(deploy_root: Path) -> list[int] | None:
    obj_pak = deploy_root / "data" / "objects" / "object2.pak"
    csv_main = deploy_root / "data" / "art" / "Buildings.csv"
    csv_gui = deploy_root / "data" / "art" / "gui" / "Buildings.csv"

    for p in (obj_pak, csv_main, csv_gui):
        if not p.exists():
            print(f"error: expected path missing: {p}", file=sys.stderr)
            return None

    print("[tree-scatter] Backing up (skipped if already present)...")
    backup(obj_pak, ".bak_treescatter")
    backup(csv_main, ".bak_treescatter")
    backup(csv_gui, ".bak_treescatter")

    sys.path.insert(0, str(REPO_ROOT / "tools"))
    from pak_append import read_packets as _read_packets  # noqa: E402

    fit_ids: list[int] = []
    for name, appearance, template, expected_fit_id in CLONES:
        before = _read_packets(obj_pak)

        print(f"[tree-scatter] Appending FIT packet for {name} (AppearanceName={appearance})...")
        fit_text = REPO_ROOT / "tools" / template
        fit_id = append_fit_packet(obj_pak, fit_text)

        # prior-packet byte-identity check, same discipline as
        # install_cliff_dressing.py's scatter-clone install.
        after = _read_packets(obj_pak)
        assert len(after) == len(before) + 1, "unexpected packet count after append"
        for b, a in zip(before, after):
            if b.payload != a.payload:
                print(f"error: prior packet {b.index} changed unexpectedly during {name} append!",
                      file=sys.stderr)
                return None
        print(f"[tree-scatter] {name}: prior-packet byte-identity check passed.")

        if fit_id != expected_fit_id:
            print(f"warning: {name} FitID was {fit_id}, expected {expected_fit_id} "
                  "(scatter_tool.py RESERVED_SCATTER_OBJTYPENUMS assumes this value)",
                  file=sys.stderr)

        n = appearance[-1]  # "1"/"2"/"3" from "Pine1"/"Pine2"/"Pine3"
        line = CSV_LINE_TEMPLATE.format(name=name, fit_id=fit_id, n=n)
        with open(csv_main, "ab") as f:
            f.write(line.encode("ascii"))
        with open(csv_gui, "ab") as f:
            f.write(line.encode("ascii"))
        print(f"[tree-scatter] {name}: Buildings.csv line appended (fitID={fit_id}).")

        fit_ids.append(fit_id)

    print(f"\n[tree-scatter] Install complete. FitIDs={fit_ids}. "
          "Backups: *.bak_treescatter next to each modified file.")
    print("[tree-scatter] Revert: copy each *.bak_treescatter back over the original.")
    return fit_ids


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--deploy-root", required=True,
                     help='e.g. "A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0"')
    args = ap.parse_args()

    deploy_root = Path(args.deploy_root)
    fit_ids = install(deploy_root)
    if fit_ids is None:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
