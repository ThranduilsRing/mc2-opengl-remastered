#!/usr/bin/env python3
"""install_cliff_dressing.py -- installer for TERRAIN-CLIFF-MESH-DRESSING-P0
(and its dedicated scatter clone, TERRAIN-SCATTER-OBJTYPENUM-1189).

Installs the MarbleCliff building-class dressing prop into a deployed MC2
install: copies data/tgl/{marblecliff.ini,MarbleCliffGLB.glb}, appends one
line to data/art/Buildings.csv (+ gui copy), and appends one FIT packet to
data/objects/object2.pak via tools/pak_append.py.

Always backs up object2.pak and both Buildings.csv files first (suffix
".bak_cliffp0"; skipped if a backup already exists, so re-running is safe).

--with-scatter-clone additionally installs a second FIT packet/CSV line for
"MarbleCliffScatter" (same AppearanceName="marblecliff", so it renders
identically), reusing the SAME art assets -- no new .glb/.ini needed. This
gives the mask-driven scatter cook (tools/terrain_beautify/scatter_tool.py) a
dedicated objTypeNum (1189) that is NOT the hand-placeable MarbleCliff (1188),
so a scatter re-cook's "delete by objTypeNum" step can never nuke hand
placements. Uses a SEPARATE backup suffix (".bak_scatter1189") so re-running
either step independently is still safe/idempotent.

Usage:
  python install_cliff_dressing.py --deploy-root "A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0"
  python install_cliff_dressing.py --deploy-root "..." --with-scatter-clone
  python install_cliff_dressing.py --deploy-root "..." --scatter-clone-only

To revert P0: restore the three ".bak_cliffp0" files over their originals and
delete data/tgl/marblecliff.ini + MarbleCliffGLB.glb.
To revert the scatter clone: restore the three ".bak_scatter1189" files.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CSV_LINE = "MarbleCliff,11,30434,BUILDING,{fit_id},,0,0,0,0,,1\r\n"
CSV_LINE_SCATTER = "MarbleCliffScatter,11,30434,BUILDING,{fit_id},,0,0,0,0,,1\r\n"


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


def install_p0(deploy_root: Path, fit_text: Path) -> int | None:
    obj_pak = deploy_root / "data" / "objects" / "object2.pak"
    csv_main = deploy_root / "data" / "art" / "Buildings.csv"
    csv_gui = deploy_root / "data" / "art" / "gui" / "Buildings.csv"
    tgl_dir = deploy_root / "data" / "tgl"

    for p in (obj_pak, csv_main, csv_gui, tgl_dir):
        if not p.exists():
            print(f"error: expected path missing: {p}", file=sys.stderr)
            return None

    print("[P0] Backing up (skipped if already present)...")
    backup(obj_pak, ".bak_cliffp0")
    backup(csv_main, ".bak_cliffp0")
    backup(csv_gui, ".bak_cliffp0")

    print("[P0] Copying data/tgl assets...")
    shutil.copyfile(REPO_ROOT / "data" / "tgl" / "marblecliff.ini", tgl_dir / "marblecliff.ini")
    shutil.copyfile(REPO_ROOT / "data" / "tgl" / "MarbleCliffGLB.glb", tgl_dir / "MarbleCliffGLB.glb")

    print("[P0] Appending FIT packet to object2.pak...")
    fit_id = append_fit_packet(obj_pak, fit_text)

    print(f"[P0] Appending Buildings.csv line (fitID={fit_id})...")
    line = CSV_LINE.format(fit_id=fit_id)
    with open(csv_main, "ab") as f:
        f.write(line.encode("ascii"))
    with open(csv_gui, "ab") as f:
        f.write(line.encode("ascii"))

    print(f"\n[P0] Install complete. FitID={fit_id}. Backups: *.bak_cliffp0 next to each modified file.")
    print("[P0] Revert: copy each *.bak_cliffp0 back over the original, then delete "
          f"{tgl_dir / 'marblecliff.ini'} and {tgl_dir / 'MarbleCliffGLB.glb'}.")
    return fit_id


def install_scatter_clone(deploy_root: Path, fit_text: Path) -> int | None:
    """Install the dedicated scatter-prop clone (expected FitID/objTypeNum
    1189). Reuses marblecliff.ini/.glb already installed by P0 -- only the
    FIT packet + CSV lines are new, following the exact P0 registration
    pattern (byte-identity-preserving pak append, own backup suffix)."""
    obj_pak = deploy_root / "data" / "objects" / "object2.pak"
    csv_main = deploy_root / "data" / "art" / "Buildings.csv"
    csv_gui = deploy_root / "data" / "art" / "gui" / "Buildings.csv"
    tgl_dir = deploy_root / "data" / "tgl"

    for p in (obj_pak, csv_main, csv_gui, tgl_dir):
        if not p.exists():
            print(f"error: expected path missing: {p}", file=sys.stderr)
            return None
    if not (tgl_dir / "marblecliff.ini").exists() or not (tgl_dir / "MarbleCliffGLB.glb").exists():
        print("error: marblecliff.ini/MarbleCliffGLB.glb not present in deploy -- "
              "run the P0 install first (or --with-scatter-clone) so the shared "
              "AppearanceName=\"marblecliff\" assets exist.", file=sys.stderr)
        return None

    print("[scatter1189] Backing up (skipped if already present)...")
    backup(obj_pak, ".bak_scatter1189")
    backup(csv_main, ".bak_scatter1189")
    backup(csv_gui, ".bak_scatter1189")

    # prior-packet byte-identity check: verify appending doesn't disturb any
    # existing packet (matches the round-trip guarantee pak_append.py provides
    # and scatter_tool.py's test_cook_roundtrip_byte_identity_on_real_pak_copy).
    sys.path.insert(0, str(REPO_ROOT / "tools"))
    from pak_append import read_packets as _read_packets  # noqa: E402
    before = _read_packets(obj_pak)

    print("[scatter1189] Appending FIT packet to object2.pak...")
    fit_id = append_fit_packet(obj_pak, fit_text)

    after = _read_packets(obj_pak)
    assert len(after) == len(before) + 1, "unexpected packet count after append"
    for b, a in zip(before, after):
        if b.payload != a.payload:
            print(f"error: prior packet {b.index} changed unexpectedly during scatter-clone append!",
                  file=sys.stderr)
            return None
    print("[scatter1189] prior-packet byte-identity check passed.")

    print(f"[scatter1189] Appending Buildings.csv line (fitID={fit_id})...")
    line = CSV_LINE_SCATTER.format(fit_id=fit_id)
    with open(csv_main, "ab") as f:
        f.write(line.encode("ascii"))
    with open(csv_gui, "ab") as f:
        f.write(line.encode("ascii"))

    print(f"\n[scatter1189] Install complete. FitID={fit_id}. "
          "Backups: *.bak_scatter1189 next to each modified file.")
    print("[scatter1189] Revert: copy each *.bak_scatter1189 back over the original.")
    return fit_id


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--deploy-root", required=True,
                     help='e.g. "A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0"')
    ap.add_argument("--fit-text", default=str(REPO_ROOT / "tools" / "marblecliff_fit_template.txt"),
                     help="FIT packet text to append to object2.pak for the P0 hand-placeable prop")
    ap.add_argument("--scatter-fit-text",
                     default=str(REPO_ROOT / "tools" / "marblecliffscatter_fit_template.txt"),
                     help="FIT packet text for the dedicated scatter clone (objTypeNum 1189)")
    ap.add_argument("--with-scatter-clone", action="store_true",
                     help="also install the MarbleCliffScatter (1189) clone after P0")
    ap.add_argument("--scatter-clone-only", action="store_true",
                     help="skip P0 install, only install the scatter clone (P0 must already be installed)")
    args = ap.parse_args()

    deploy_root = Path(args.deploy_root)

    if not args.scatter_clone_only:
        fit_id = install_p0(deploy_root, Path(args.fit_text))
        if fit_id is None:
            return 1
        if fit_id != 1188:
            print(f"warning: P0 FitID was {fit_id}, expected 1188 (Buildings.csv/scatter_tool.py "
                  "assume 1188 for MarbleCliff)", file=sys.stderr)

    if args.with_scatter_clone or args.scatter_clone_only:
        scatter_id = install_scatter_clone(deploy_root, Path(args.scatter_fit_text))
        if scatter_id is None:
            return 1
        if scatter_id != 1189:
            print(f"warning: scatter clone FitID was {scatter_id}, expected 1189 "
                  "(scatter_tool.py RESERVED_SCATTER_OBJTYPENUMS assumes 1189)", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
