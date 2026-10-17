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
Round 2 (after the before-review) adds, all by text:
  app_write      RAW WRITERS of an `appearance` member (`appearance = x`, `->appearance = x`, `.appearance = x`; local declarations excluded). B7o.2 seals them: the count outside the BODIES of GameObject::adoptAppearance/releaseAppearance (those rows are kind app_write_api, not raw) must reach 0
  app_write_other  the same text in a class that is not a GameObject (LogisticsChassis): listed, never sealed
  app_copy_api   `GameObject::set` / `Mover::set` (by-value copy APIs that copy the appearance view)
  app_delete (extended) also `delete bomber|theSky|compass|pObject` (artillery bomber, camera sky/compass, SimpleCamera object)
  marker_new / marker_delete / marker_write / marker_copy   the MechWarrior waypoint marker: creations, deletes, assignments to `.marker`, whole-slot queue copies (`tacOrderQueue[i] = tacOrderQueue[j]`, `MemCpy(..tacOrderQueue..)`)
  marker_read   (round 4) a RUNTIME read of the wire field: any `.marker` / `->marker` token in code/warrior* that is not an assignment and not a `delete` (the draw loop, `if (...marker)` guards); target 0 after B7o.4 (readers move to `queueMarkers[i].get()`)
  marker_wire_null   (round 4) a write of nullptr/0/NULL to the wire field (staging nulling, the dead-storage clears): ALLOWED to remain after B7o.4, listed apart from `marker_write` (a write of a real pointer, target 0) so the zero-token target is not contradicted by the retained staging write
  objcache_actor / objcache_subarea / objcache_text / objcache_ptrarr   the objectCache->Malloc/Free rows split by what they allocate (actor allocation operators, terrain sub-area short arrays, weapon-bolt texture text, manager pointer arrays and the collidable list)
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
    "objtype_op": "B7o.9-operators",
    "objcache_op": "B7o.9-operators",
    "objcache_actor": "B7o.9-operators",
    "objcache_subarea": "B7o.8a-subarea-text",
    "objcache_text": "B7o.8a-subarea-text",
    "objcache_ptrarr": "B7o.8b-ptrarrays",
    "objtypemgr": "B7o.7-typemgr",
    "shape_node": "B7o.6-shapenode",
    "app_write": "B7o.2-seal",
    "app_write_other": "B7o.2-seal",
    "app_write_api": "B7o.2-seal",
    "app_copy_api": "B7o.2-seal",
    "marker_new": "B7o.4-queue-markers",
    "marker_delete": "B7o.4-queue-markers",
    "marker_write": "B7o.4-queue-markers",
    "marker_copy": "B7o.4-queue-markers",
    "marker_read": "B7o.4-queue-markers",
    "marker_wire_null": "B7o.4-queue-markers",
}
FILE_RULES = [
    ("B7o.5-nonactor-owners", re.compile(r"^code/(gamecam|simplecamera|missiongui\w*|artlry\w*)\.(cpp|h)$|^gui/")),
]
APPEARANCE_OWNER_BATCH = "B7o.3-actor-owner"
#: Round 3: the exemption is NOT a file. The only raw `appearance` writes allowed once B7o.2 lands are inside the BODIES of these two
#: functions of code/gameobj.cpp (the adopt/release API); `GameObject::init` and `GameObject::set` in the same file are writers like any other.
API_FILE = "code/gameobj.cpp"
API_FUNCS = re.compile(r"GameObject::(?:adoptAppearance|releaseAppearance)\s*\(")

def api_body_lines(path: str, lines: list[str]) -> set[int]:
    """1-based line numbers inside the braces of the API function definitions of `path` (empty for any other file)."""
    if path != API_FILE:
        return set()
    out: set[int] = set()
    i = 0
    while i < len(lines):
        if API_FUNCS.search(lines[i]) and not lines[i].rstrip().endswith(";"):
            depth, started, j = 0, False, i
            while j < len(lines):
                for ch in lines[j]:
                    if ch == "{":
                        depth += 1
                        started = True
                    elif ch == "}":
                        depth -= 1
                if started:
                    out.add(j + 1)
                if started and depth <= 0:
                    break
                j += 1
            i = j
        i += 1
    return out

RX = {
    "app_new": re.compile(r"\bnew\s+(?!\w*AppearanceType)\w*Appearance\b"),
    "app_delete": re.compile(r"\bdelete\s+(?:\w+(?:->|\.))?(?:appearance\b|vTol\s*\[)"),
    "app_delete_named": re.compile(r"\bdelete\s+(?:\w+(?:->|\.))?(?:bomber|theSky|compass|pObject)\b"),
    "app_write": re.compile(r"(?:^|[^\w])appearance\s*=(?!=)"),
    "app_copy_api": re.compile(r"\b(?:GameObject|Mover)::set\s*\("),
    "marker_new": re.compile(r"\bgetWayPointMarker\s*\("),
    "marker_delete": re.compile(r"\bdelete\s+[\w\[\]\.>-]*marker\b"),
    "marker_write": re.compile(r"\bmarker\s*=(?!=)"),
    "marker_read": re.compile(r"(?:\.|->)marker\b(?!\s*=(?!=))"),
    "marker_copy": re.compile(r"\btacOrderQueue\s*\[[^\]]*\]\s*=\s*tacOrderQueue|\bMemCpy\s*\(\s*(?:data\.)?tacOrderQueue\b"),
    "objtype_op": re.compile(r"\bObjectType::operator\s+(?:new|delete)\b"),
    "objcache_op": re.compile(r"\bGameObject::operator\s+(?:new|delete)\b"),
    "objcache_heap": re.compile(r"\bobjectCache\s*->\s*(?:Malloc|Free)\s*\("),
    "objtypemgr": re.compile(r"\bobjTypeManager\s*=(?!=)|\bdelete\s+(?:\w+(?:->|\.))?objTypeManager\b|\bobjTypeManager\b.*\bnew\b"),
    "shape_node": re.compile(r"\bnew\s+(?:TG_Shape|TG_TypeShape|TG_TypeNode)\b|\bdelete\s+\w*\.?node\b|\bdelete\s+listOf(?:Type)?Shapes\b|\bdelete\s+listOfShapes\s*\[[^\]]*\]\s*\.node\b|\bdelete\s+listOfTypeShapes\s*\["),
}
#: The shape_node rows belong to the TG files only; the same words elsewhere are other classes.
MARKER_NULL_WRITE = re.compile(r"\bmarker\s*=\s*(?:nullptr|NULL|0)\s*;")
MARKER_DELETE_LINE = re.compile(r"\bdelete\s+[\w\[\]\.>-]*marker\b")
MARKER_WRITE_LINE = re.compile(r"\bmarker\s*=(?!=)")
MARKER_FILES = re.compile(r"^code/warrior\w*\.(cpp|h)$")
NAMED_DELETE_FILES = re.compile(r"^code/(artlry|gamecam|simplecamera)\.cpp$")
LOCAL_DECL = re.compile(r"\b(?:AppearancePtr|Appearance|\w+Appearance)\s*\*?\s*appearance\s*=")
SHAPE_NODE_FILES = re.compile(r"^mclib/(msl|tgl|assimp_importer)[\w]*\.(cpp|h)$")


class CensusError(Exception):
    """An input the census cannot read: reported as one line, exit code 2."""


def tracked() -> list[str]:
    try:
        out = subprocess.run(["git", "-C", ROOT, "ls-files"] + SCAN_DIRS, capture_output=True, text=True, check=True, **console_child.child_kwargs(redirected=True)).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        raise CensusError(f"git ls-files failed: {e}")
    return [p for p in out.splitlines() if p.endswith(SOURCE_EXT)]


def classify(path: str, kind: str) -> str:
    """Final kind of a matched line: the objectCache rows split by what they allocate, the named deletes fold into app_delete."""
    if kind == "objcache_heap":
        if path == "code/gameobj.cpp":
            return "objcache_actor"
        if path == "code/terrobj.cpp":
            return "objcache_subarea"
        if path == "code/weaponbolt.cpp":
            return "objcache_text"
        return "objcache_ptrarr"
    if kind == "app_delete_named":
        return "app_delete"
    if kind == "app_write" and path.startswith("code/logistics"):
        return "app_write_other"
    return kind


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
        stripped = list(strip_comments(raw))
        api_lines = api_body_lines(path, stripped)
        for lineno, line in enumerate(stripped, 1):
            for kind, rx in RX.items():
                if not rx.search(line):
                    continue
                if kind == "shape_node" and not SHAPE_NODE_FILES.search(path):
                    continue
                if kind != "objtypemgr" and path.startswith("tests/unit/"):
                    continue
                if kind.startswith("marker_") and not MARKER_FILES.search(path):
                    continue
                if kind == "app_delete_named" and not NAMED_DELETE_FILES.search(path):
                    continue
                if kind == "app_write" and LOCAL_DECL.search(line):
                    continue
                if kind == "marker_read" and (MARKER_DELETE_LINE.search(line) or MARKER_WRITE_LINE.search(line)):
                    continue
                final = classify(path, kind)
                if final == "marker_write" and MARKER_NULL_WRITE.search(line):
                    final = "marker_wire_null"
                if final == "app_write" and lineno in api_lines:
                    final = "app_write_api"
                rows.append(f"{batch_of(path, final)}\t{path}:{lineno}\t{final}\t{src_lines[lineno - 1].strip()[:120]}")
    return rows


def raw_writers(rows: list[str]) -> list[str]:
    """Raw `appearance` writers outside the adopt/release API (only the bodies of GameObject::adoptAppearance/releaseAppearance are exempt): B7o.2 is proved when this is empty."""
    out = []
    for r in rows:
        _, loc, k, _t = r.split("\t", 3)
        if k == "app_write":
            out.append(r)
    return out


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
        print(f"  {b:26s} {sum(per[b].values()):4d} sites {len(perfiles[b]):3d} files  " + " ".join(f"{k}={n}" for k, n in sorted(per[b].items())))
    print(f"b7_actor_census: raw appearance writers outside the API: {len(raw_writers(rows))} (B7o.2 seal target 0)")
    reads = sum(1 for r in rows if r.split("\t", 3)[2] == "marker_read")
    mwrites = sum(1 for r in rows if r.split("\t", 3)[2] == "marker_write")
    print(f"b7_actor_census: runtime marker reads: {reads}, marker pointer writes: {mwrites} (B7o.4 target 0 and 0; marker_wire_null rows are allowed to remain)")


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
