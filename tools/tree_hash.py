#!/usr/bin/env python3
"""tree_hash.py - exact-match guard for the TreeIsland state sync (S1).

Hashes the per-mission TREE instances found in mission PAK packet 1 (the 40-byte records of
mclib/mission_codec.h: type, position, rotation, damage, team, parentId). A record is a tree when its
type's Object2.pak packet contains a [TreeData] block (code/terrobj.cpp:401). Records keep file order,
which is the per-block handle order the saves depend on.

  py -3 tools/tree_hash.py write  tools/tree_hash_baseline.json
  py -3 tools/tree_hash.py check  tools/tree_hash_baseline.json     # exit 1 on any difference

A root is a directory holding objects/object2.pak and missions/*.pak (stock srcdata or a mod install).
Missing roots are skipped (reported), so the check works on machines without the mod installs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pak_dump  # noqa: E402

REC = 40
ROOTS = {
    "stock": "A:/Games/mc2-opengl/mc2srcdata-fresh",
    "omnitech": "A:/Games/mc2-opengl/MechCommanderOmnitech/data",
    "exodus": "A:/Games/mc2-opengl/MC2-Exodus/data",
}


def _pak(path: Path):
    return pak_dump.read_packets(path)


def tree_type_set(object2: Path) -> set[int]:
    out = set()
    for rec in _pak(object2):
        body = pak_dump.decode_packet(rec)
        if body and b"[TreeData]" in body:
            out.add(rec.index)
    return out


def hash_records(packet: bytes, trees: set[int]) -> dict:
    if len(packet) < 4:
        return {"error": "short"}
    (count,) = struct.unpack_from("<I", packet, 0)
    if len(packet) != 4 + count * REC:
        return {"error": "bad_size", "bytes": len(packet)}
    h = hashlib.sha256()
    n = 0
    types: dict[int, int] = {}
    for i in range(count):
        r = packet[4 + i * REC: 4 + (i + 1) * REC]
        t = struct.unpack_from("<i", r, 0)[0]
        if t in trees:
            h.update(r)
            n += 1
            types[t] = types.get(t, 0) + 1
    return {"objects": count, "trees": n, "sha256": h.hexdigest(), "types": len(types)}


def scan_root(root: Path) -> dict | None:
    obj = root / "objects" / "object2.pak"
    mdir = root / "missions"
    if not obj.exists() or not mdir.is_dir():
        return None
    trees = tree_type_set(obj)
    res = {}
    for p in sorted(mdir.glob("*.pak")):
        try:
            packets = _pak(p)
            if len(packets) < 2:
                continue
            body = pak_dump.decode_packet(packets[1])
        except Exception as e:  # noqa: BLE001
            res[p.stem] = {"error": str(e)[:80]}
            continue
        if body is None:
            continue
        info = hash_records(body, trees)
        if info.get("trees"):
            res[p.stem] = info
    return {"treeTypes": len(trees), "missions": res}


def scan_all(roots: dict[str, str]) -> dict:
    out = {}
    for name, path in roots.items():
        r = scan_root(Path(path))
        if r is not None:
            out[name] = r
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["write", "check"])
    ap.add_argument("json")
    a = ap.parse_args(argv)
    cur = scan_all(ROOTS)
    if a.mode == "write":
        Path(a.json).write_text(json.dumps(cur, indent=1, sort_keys=True) + "\n")
        print({k: len(v["missions"]) for k, v in cur.items()})
        return 0
    base = json.loads(Path(a.json).read_text())
    bad = 0
    for name, b in base.items():
        if name not in cur:
            print(f"SKIP root {name} (not present)")
            continue
        if b != cur[name]:
            bad += 1
            for m in sorted(set(b["missions"]) | set(cur[name]["missions"])):
                if b["missions"].get(m) != cur[name]["missions"].get(m):
                    print(f"DIFF {name}/{m}")
    print("tree_hash: " + ("FAIL" if bad else "OK"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
