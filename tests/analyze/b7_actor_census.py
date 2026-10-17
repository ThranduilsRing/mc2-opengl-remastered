#!/usr/bin/env python3
"""b7_actor_census - the sealed census of the C++23 step 4 batch B7 (actor instance ownership).

WHAT: scans the tracked game files (code/, mclib/, gui/, and tests/unit for the objTypeManager assignments) for seven site kinds, in comment- and string-free text:
  app_new        `new <X>Appearance` (an actor, camera, marker or VTOL takes an appearance instance)
  app_delete     `delete appearance` / `delete vTol[..]` / `delete <expr>->appearance` (an owner frees one)
  objtype_op     ObjectType::operator new / delete (routes to objectTypeCache)
  objcache_op    GameObject::operator new / delete (routes to objectCache; zero-fills: save bytes)
  objcache_heap  objectCache->Malloc / Free (the manager's pool arrays; the heap goes when the last one does)
  objtypemgr     objTypeManager `new`, `delete` or assignment (a raw pointer that tests assign borrowed objects to)
  shape_node     raw `new` / `delete` of a TG node: TG_ShapeRec::node, typeNodeStore entries (`new TG_Shape`, `new TG_TypeShape/Node`, `delete ...node`, `delete listOfTypeShapes[..]`)
One line per site goes to the committed list `.claude/cleanup/cxx23-step4-b7-callsites.txt`; `--check` compares the tree against it (line numbers ignored).
WHY: same shape as b5_asset_census: a sub-batch commit shrinks the list by intent (`--write` in the same commit); a site that appears or vanishes outside a batch fails `--check`.
Not a compiler census: names are matched by text. Editor (editor/), tools and GameOS are not scanned (rebuilt by tools/build_all_targets.py; forced edits are listed in the batch commit).

  py -3 tests/analyze/b7_actor_census.py            # summary
  py -3 tests/analyze/b7_actor_census.py --check    # exit 1 when the tree differs from the committed list
  py -3 tests/analyze/b7_actor_census.py --write    # rewrite the committed list (a batch commit does this by intent)
"""
from __future__ import annotations

import argparse
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console_child  # noqa: E402
from b5_asset_census import strip_comments  # noqa: E402  (the comment/string blanker is shared)

LIST = os.path.join(ROOT, ".claude", "cleanup", "cxx23-step4-b7-callsites.txt")
SCAN_DIRS = ["code", "mclib", "gui", "tests/unit"]
SOURCE_EXT = (".cpp", ".h", ".hpp")

#: Sub-batch of a site: the kind decides for the non-appearance kinds, then the file for the appearance kinds.
KIND_BATCH = {
    "objtype_op": "B7.5-objectcache",
    "objcache_op": "B7.5-objectcache",
    "objcache_heap": "B7.5-objectcache",
    "objtypemgr": "B7.4-typemgr",
    "shape_node": "B7.3-shapenode",
}
FILE_RULES = [
    ("B7.2-camera-ui", re.compile(r"^code/(gamecam|simplecamera|missiongui\w*|warrior\w*|artlry\w*)\.(cpp|h)$|^gui/")),
]
APPEARANCE_OWNER_BATCH = "B7.1-actor-appearance"

RX = {
    "app_new": re.compile(r"\bnew\s+(?!\w*AppearanceType)\w*Appearance\b"),
    "app_delete": re.compile(r"\bdelete\s+(?:\w+(?:->|\.))?(?:appearance\b|vTol\s*\[)"),
    "objtype_op": re.compile(r"\bObjectType::operator\s+(?:new|delete)\b"),
    "objcache_op": re.compile(r"\bGameObject::operator\s+(?:new|delete)\b"),
    "objcache_heap": re.compile(r"\bobjectCache\s*->\s*(?:Malloc|Free)\s*\("),
    "objtypemgr": re.compile(r"\bobjTypeManager\s*=(?!=)|\bdelete\s+(?:\w+(?:->|\.))?objTypeManager\b|\bobjTypeManager\b.*\bnew\b"),
    "shape_node": re.compile(r"\bnew\s+(?:TG_Shape|TG_TypeShape|TG_TypeNode)\b|\bdelete\s+\w*\.?node\b|\bdelete\s+listOf(?:Type)?Shapes\b|\bdelete\s+listOfShapes\s*\[[^\]]*\]\s*\.node\b|\bdelete\s+listOfTypeShapes\s*\["),
}
#: The shape_node rows belong to the TG files only; the same words elsewhere are other classes.
SHAPE_NODE_FILES = re.compile(r"^mclib/(msl|tgl|assimp_importer)[\w]*\.(cpp|h)$")


class CensusError(Exception):
    """An input the census cannot read: reported as one line, exit code 2."""


def tracked() -> list[str]:
    try:
        out = subprocess.run(["git", "-C", ROOT, "ls-files"] + SCAN_DIRS, capture_output=True, text=True, check=True, **console_child.child_kwargs(redirected=True)).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        raise CensusError(f"git ls-files failed: {e}")
    return [p for p in out.splitlines() if p.endswith(SOURCE_EXT)]


def batch_of(path: str, kind: str) -> str:
    if kind in KIND_BATCH:
        return KIND_BATCH[kind]
    for name, rx in FILE_RULES:
        if rx.search(path):
            return name
    return APPEARANCE_OWNER_BATCH


def census() -> list[str]:
    rows = []
    for path in sorted(tracked()):
        try:
            with open(os.path.join(ROOT, path), encoding="latin-1") as f:
                raw = f.read().replace("\r\n", "\n")
        except OSError as e:
            raise CensusError(f"tracked file {path} is unreadable: {e}")
        src_lines = raw.split("\n")
        for lineno, line in enumerate(strip_comments(raw), 1):
            for kind, rx in RX.items():
                if not rx.search(line):
                    continue
                if kind == "shape_node" and not SHAPE_NODE_FILES.search(path):
                    continue
                if kind != "objtypemgr" and path.startswith("tests/unit/"):
                    continue
                rows.append(f"{batch_of(path, kind)}\t{path}:{lineno}\t{kind}\t{src_lines[lineno - 1].strip()[:120]}")
    return rows


def summary(rows: list[str]) -> None:
    files = {r.split("\t")[1].rsplit(":", 1)[0] for r in rows}
    print(f"b7_actor_census: {len(rows)} sites in {len(files)} files")
    per = collections.defaultdict(collections.Counter)
    perfiles = collections.defaultdict(set)
    for r in rows:
        b, loc, k, _ = r.split("\t", 3)
        per[b][k] += 1
        perfiles[b].add(loc.rsplit(":", 1)[0])
    for b in sorted(per):
        print(f"  {b:24s} {sum(per[b].values()):4d} sites {len(perfiles[b]):3d} files  " + " ".join(f"{k}={n}" for k, n in sorted(per[b].items())))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--check", action="store_true")
    g.add_argument("--write", action="store_true")
    args = ap.parse_args(argv)
    try:
        rows = census()
    except CensusError as e:
        print(f"b7_actor_census: {e}")
        return 2
    if args.write:
        with open(LIST, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(rows) + "\n")
    summary(rows)
    if args.check:
        def key(r: str) -> str:
            batch, loc, rest = r.split("\t", 2)
            return f"{batch}\t{loc.rsplit(':', 1)[0]}\t{rest}"
        try:
            with open(LIST, encoding="utf-8") as f:
                sealed = collections.Counter(key(r) for r in f.read().splitlines() if r)
        except OSError as e:
            print(f"b7_actor_census: the sealed list {LIST} is unreadable: {e}")
            return 2
        now = collections.Counter(key(r) for r in rows)
        gone, new = sealed - now, now - sealed
        for r in sorted(gone.elements()):
            print(f"  GONE {r}")
        for r in sorted(new.elements()):
            print(f"  NEW  {r}")
        if gone or new:
            print(f"b7_actor_census: FAIL ({sum(gone.values())} gone, {sum(new.values())} new): rewrite the list with --write in the batch commit")
            return 1
        print("b7_actor_census: PASS (tree matches the sealed list)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
