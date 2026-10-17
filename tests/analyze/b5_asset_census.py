#!/usr/bin/env python3
"""b5_asset_census - the sealed census of the C++23 step 4 batch B5 (shared assets: textures, TG shapes, appearance types, object types and the class-routed heaps).

WHAT: scans the tracked game files (code/, mclib/, gui/) for the seven site kinds the batch changes, in comment- and string-free text:
  tex_release    a call of MC_TextureManager::removeTexture / removeTextureNode (the two release APIs TextureRef replaces)
  tex_acquire    a call of loadTexture / textureFromMemory / textureFromMemoryRaw (a node reference is taken)
  heap_malloc    appearanceHeap / tglHeap ->Malloc(   (class-routed asset heaps; the raw-buffer heaps were B4)
  heap_free      appearanceHeap / tglHeap ->Free(
  class_op       X::operator new / operator delete / operator new[] / operator delete[] of an asset class
  asset_new      `new <asset type>` for a type, shape or appearance-type class (a factory owned by a registry elsewhere)
  asset_delete   `delete <expr>` of a type-, shape-, appearance-, texture- or heap-named expression
One line per site goes to the committed list `.claude/cleanup/cxx23-step4-b5-callsites.txt`; `--check` compares the tree against it.
WHY: B3 ran on a census that could not be re-run; B4 fixed that with a sealed list. B5 starts from the same shape: a sub-batch commit shrinks the list by intent
(`--write` in the same commit), and a site that appears or vanishes outside a batch fails `--check`.
Not a compiler census: the release APIs are two member functions found by name, the heaps are always spelled `<heap>->Malloc(` / `->Free(`.
Editor (editor/), tools and GameOS call the same APIs; they are NOT scanned (E-batches / tools are rebuilt by `tools/build_all_targets.py`, the notes' section 1 names them).

  py -3 tests/analyze/b5_asset_census.py            # summary: per sub-batch and per kind
  py -3 tests/analyze/b5_asset_census.py --check    # exit 1 when the tree differs from the committed list
  py -3 tests/analyze/b5_asset_census.py --write    # rewrite the committed list (a batch commit does this by intent)
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
import console_child  # noqa: E402
LIST = os.path.join(ROOT, ".claude", "cleanup", "cxx23-step4-b5-callsites.txt")

SCAN_DIRS = ["code", "mclib", "gui"]
SOURCE_EXT = (".cpp", ".h", ".hpp")

#: Sub-batch of a site: the texture kinds are B5.1 wherever they occur; the rest go by file (first matching rule wins).
FILE_RULES = [
    ("B5.2-tg", re.compile(r"^mclib/(tgl|msl)[\w]*\.(cpp|h)$|^mclib/assimp_importer\.(cpp|h)$")),
    ("B5.3-appearance", re.compile(r"^mclib/(appear|apprtype|dappear|objectappearance|bdactor|gvactor|genactor|mech3d)[\w]*\.(cpp|h)$|^code/actor\.h$")),
    ("B5.4-objecttype", re.compile(r"^code/(objtype|dobjtype|terrobj|dterrobj)\.(cpp|h)$")),
]
OWNER_BATCH = "B5.5-heap-owner"

TEX_FILES_EXCLUDE = re.compile(r"^mclib/txmmgr[\w]*\.(cpp|h)$")  # the manager defines the APIs; its own rows are the node store (B4 class D)

RX = {
    "tex_release": re.compile(r"(?<![\w:])(?<!void )(removeTexture|removeTextureNode)\s*\("),
    "tex_acquire": re.compile(r"(?<![\w:])(?<!DWORD )(loadTexture|textureFromMemory|textureFromMemoryRaw)\s*\("),
    "heap_malloc": re.compile(r"\b(appearanceHeap|tglHeap)\s*->\s*Malloc\s*\("),
    "heap_free": re.compile(r"\b(appearanceHeap|tglHeap)\s*->\s*Free\s*\("),
    "class_op": re.compile(r"\b\w+::operator\s+(?:new|delete)\s*(?:\[\s*\])?\s*\("),
    "asset_new": re.compile(r"\bnew\s+(?:\w*AppearanceType|TG_TypeMultiShape|TG_MultiShape|TG_AnimateShape|TG_TypeShape|TG_TypeNode|TG_Shape|\w*ObjectType)\b"),
    "asset_delete": re.compile(r"\bdelete\b(?!\s*;)"),
}
ASSET_NAME = re.compile(r"(?i)shape|type|appear|tg_|texture|heap")
#: Deletes that match ASSET_NAME but belong to another batch: an actor's own Appearance instance (B7), other subsystems' heaps (B4 / B6 / B8), UI icon texture memory (B6).
OTHER_BATCH_DELETE = re.compile(r"delete\s+(?:\[\s*\]\s*)?(?:appearance|Abl\w*Heap|collisionHeap|Radio::radioHeap|systemHeap|guiHeap|globalHeapList|missionHeap|crater\w*Heap|"
                                r"progressTextureMemory|mergedTexture|MechIcon::|ForceGroupIcon::|VehicleIcon::|mcTextureManager)")
#: class_op only counts in asset classes, not every class of the files the rules name.
CLASS_OP_CLASSES = re.compile(r"\b(AppearanceType|\w*AppearanceType|\w*Appearance|TG_\w+|ObjectType|\w*ObjectType|\w*Shape)::operator")


class CensusError(Exception):
    """An input the census cannot read: reported as one line, exit code 2."""


def strip_comments(text: str) -> list[str]:
    """Blank out // and /* */ comments and string literals, keeping line numbers."""
    i, n = 0, len(text)
    buf: list[str] = []
    in_block = False
    while i < n:
        c = text[i]
        if in_block:
            if text.startswith("*/", i):
                in_block, i = False, i + 2
                continue
            buf.append("\n" if c == "\n" else " ")
            i += 1
        elif text.startswith("/*", i):
            in_block, i = True, i + 2
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            buf.append(c + " " * max(0, min(j, n) - i - 1) + (c if j < n and text[j] == c else ""))
            i = j + 1 if j < n and text[j] == c else j
        else:
            buf.append(c)
            i += 1
    return "".join(buf).split("\n")


def tracked() -> list[str]:
    try:
        out = subprocess.run(["git", "-C", ROOT, "ls-files"] + SCAN_DIRS, capture_output=True, text=True, check=True, **console_child.child_kwargs(redirected=True)).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        raise CensusError(f"git ls-files failed: {e}")
    return [p for p in out.splitlines() if p.endswith(SOURCE_EXT)]


def file_batch(path: str) -> str | None:
    for name, rx in FILE_RULES:
        if rx.search(path):
            return name
    return None


def census() -> list[str]:
    rows = []
    for path in sorted(tracked()):
        fb = file_batch(path)
        try:
            with open(os.path.join(ROOT, path), encoding="latin-1") as f:
                raw = f.read().replace("\r\n", "\n")
        except OSError as e:
            raise CensusError(f"tracked file {path} is unreadable: {e}")
        src_lines = raw.split("\n")
        for lineno, line in enumerate(strip_comments(raw), 1):
            for kind, rx in RX.items():
                if kind.startswith("tex_") and TEX_FILES_EXCLUDE.search(path):
                    continue
                m = rx.search(line)
                if not m:
                    continue
                if kind == "class_op" and not CLASS_OP_CLASSES.search(line):
                    continue
                if kind == "asset_delete" and (not ASSET_NAME.search(line) or OTHER_BATCH_DELETE.search(line)):
                    continue
                if kind == "asset_delete" and re.search(r"=\s*delete\b", line):
                    continue
                if kind.startswith("tex_"):
                    batch = "B5.1-texture"
                else:
                    batch = fb or OWNER_BATCH
                rows.append(f"{batch}\t{path}:{lineno}\t{kind}\t{src_lines[lineno - 1].strip()[:120]}")
    return rows


def summary(rows: list[str]) -> None:
    files = {r.split("\t")[1].rsplit(":", 1)[0] for r in rows}
    print(f"b5_asset_census: {len(rows)} sites in {len(files)} files")
    per = collections.defaultdict(collections.Counter)
    for r in rows:
        b, _, k, _ = r.split("\t", 3)
        per[b][k] += 1
    for b in sorted(per):
        print(f"  {b:18s} {sum(per[b].values()):4d}  " + " ".join(f"{k}={n}" for k, n in sorted(per[b].items())))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--check", action="store_true")
    g.add_argument("--write", action="store_true")
    args = ap.parse_args(argv)
    try:
        rows = census()
    except CensusError as e:
        print(f"b5_asset_census: {e}")
        return 2
    if args.write:
        with open(LIST, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(rows) + "\n")
    summary(rows)
    if args.check:
        # Line numbers move with any edit above a site, so the check compares the sites without their line numbers.
        def key(r: str) -> str:
            batch, loc, rest = r.split("\t", 2)
            return f"{batch}\t{loc.rsplit(':', 1)[0]}\t{rest}"
        try:
            with open(LIST, encoding="utf-8") as f:
                sealed = collections.Counter(key(r) for r in f.read().splitlines() if r)
        except OSError as e:
            print(f"b5_asset_census: the sealed list {LIST} is unreadable: {e}")
            return 2
        now = collections.Counter(key(r) for r in rows)
        gone, new = sealed - now, now - sealed
        for r in sorted(gone.elements()):
            print(f"  GONE {r}")
        for r in sorted(new.elements()):
            print(f"  NEW  {r}")
        if gone or new:
            print(f"b5_asset_census: FAIL ({sum(gone.values())} gone, {sum(new.values())} new): rewrite the list with --write in the batch commit")
            return 1
        print("b5_asset_census: PASS (tree matches the sealed list)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
