#!/usr/bin/env python3
# HISTORICAL: patches mclib/sortlist.cpp, which was deleted at 29f95649a; only runs on a tree from before that commit.
"""apply_probes - patch a SCRATCH worktree with the step 4 B2 probes P1 (sort ties) and P2 (A* open list). Never run it on the main worktree.

WHAT: edits mclib/sortlist.cpp (qsort -> b2P1Sort), the five live sort call sites (a site tag), mclib/pqueue.{h,cpp} (P2 counters and the per-solve ophash),
the five solver entry points (a solver tag), GlobalMap::propogateCost (the door/openIndex hit), and appends b2_probes.inc to code/sim_identity.cpp.
WHY: the probes answer questions only a run can settle: how often (score, fill slot) differs from the CRT qsort order inside the sim-visible prefix (P1), and
what the A* open list does on stock missions (P2: bug hits, find()==0, the largest index handed to change, max open size per solver, the operation hash that
a new heap must reproduce). They change no result: the old comparators and the old heap run unchanged.
Each anchor must match exactly the expected number of times or the script stops, so a drifted tree cannot be patched silently.

  py -3 tests/analyze/b2_probes/apply_probes.py A:/Games/mc2-g2
Files are read and written byte for byte (CR LF files keep CR LF).
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


class Patcher:
    def __init__(self, root: str):
        self.root = root

    def edit(self, rel: str, steps) -> None:
        path = os.path.join(self.root, rel)
        with open(path, "rb") as f:
            raw = f.read().decode("latin-1")
        crlf = "\r\n" in raw
        text = raw.replace("\r\n", "\n")
        for old, new, count in steps:
            found = text.count(old)
            if found != count:
                sys.exit(f"apply_probes: {rel}: anchor expected {count}x, found {found}x:\n{old}")
            text = text.replace(old, new)
        if crlf:
            text = text.replace("\n", "\r\n")
        with open(path, "wb") as f:
            f.write(text.encode("latin-1"))
        print(f"apply_probes: {rel}")


def nth(text_old: str, text_new: str, index: int):
    """Marker for the index-th occurrence only (0-based): handled by edit_nth."""
    return (text_old, text_new, index)


def edit_nth(p: Patcher, rel: str, old: str, new: str, index: int, total: int) -> None:
    path = os.path.join(p.root, rel)
    with open(path, "rb") as f:
        raw = f.read().decode("latin-1")
    crlf = "\r\n" in raw
    text = raw.replace("\r\n", "\n")
    if text.count(old) != total:
        sys.exit(f"apply_probes: {rel}: anchor expected {total}x, found {text.count(old)}x:\n{old}")
    pos = -1
    for _ in range(index + 1):
        pos = text.index(old, pos + 1)
    text = text[:pos] + new + text[pos + len(old):]
    if crlf:
        text = text.replace("\n", "\r\n")
    with open(path, "wb") as f:
        f.write(text.encode("latin-1"))
    print(f"apply_probes: {rel} (occurrence {index})")


def main() -> int:
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        sys.exit(__doc__)
    root = sys.argv[1]
    if os.path.abspath(root).replace("\\", "/").lower().rstrip("/").endswith("/mc2-main"):
        sys.exit("apply_probes: refusing to patch the main worktree")
    p = Patcher(root)
    decl = 'extern "C" { extern const char* g_b2Site; extern const char* g_b2Solver; }\n'
    # P1: the comparator-sorted call and the site tags.
    p.edit("mclib/sortlist.cpp", [
        ("#include<gameos.hpp>\n", '#include<gameos.hpp>\nextern "C" void b2P1Sort(void*, size_t, size_t, int (*)(const void*, const void*), int);\n', 1),
        ("qsort((void*)list, (size_t)numItems, sizeof(SortListNode), descendingCompare);",
         "b2P1Sort((void*)list, (size_t)numItems, sizeof(SortListNode), descendingCompare, 1);", 1),
        ("qsort((void*)list, (size_t)numItems, sizeof(SortListNode), ascendingCompare);",
         "b2P1Sort((void*)list, (size_t)numItems, sizeof(SortListNode), ascendingCompare, 0);", 1),
    ])
    p.edit("code/ablmc2_camera.cpp", [
        ("\t\tTeam::sortList->sort(sortDescending);", '\t\tg_b2Site = "getMoversWithinRadius"; Team::sortList->sort(sortDescending);', 1)])
    with open(os.path.join(root, "code/ablmc2_camera.cpp"), "rb") as f:
        head = f.read()
    with open(os.path.join(root, "code/ablmc2_camera.cpp"), "wb") as f:
        f.write(decl.encode("latin-1") + (b"" if b"\r\n" not in head else b"") + head)
    p.edit("code/contact.cpp", [
        ("\t\tSensorSystem::sortList->sort(descendSort);", '\t\tg_b2Site = "getContacts"; SensorSystem::sortList->sort(descendSort);', 1)])
    with open(os.path.join(root, "code/contact.cpp"), "rb") as f:
        head = f.read()
    with open(os.path.join(root, "code/contact.cpp"), "wb") as f:
        f.write(decl.encode("latin-1") + head)
    for index, tag in enumerate(("calcOptimalRange", "sortWeapons_attack", "sortWeapons_value")):
        edit_nth(p, "code/mover_combat.cpp", "\t\tsortList->sort();", f'\t\tg_b2Site = "{tag}"; sortList->sort();', 0, 3 - index)
    with open(os.path.join(root, "code/mover_combat.cpp"), "rb") as f:
        head = f.read()
    with open(os.path.join(root, "code/mover_combat.cpp"), "wb") as f:
        f.write(decl.encode("latin-1") + head)

    # P2: the heap counters.
    p.edit("mclib/pqueue.h", [
        ("#ifndef PQUEUE_H\n#define PQUEUE_H\n",
         '#ifndef PQUEUE_H\n#define PQUEUE_H\nextern "C" void b2P2Clear(void);\n', 1),
        ("\t\tvoid clear (void) {\n\t\t\tnumItems = 0;\n\t\t}", "\t\tvoid clear (void) {\n\t\t\tb2P2Clear();\n\t\t\tnumItems = 0;\n\t\t}", 1),
    ])
    p.edit("mclib/pqueue.cpp", [
        ("#include<gameos.hpp>\n",
         '#include<gameos.hpp>\nextern "C" { void b2P2Insert(uint32_t, int32_t, int, int); void b2P2Remove(uint32_t, int32_t); void b2P2Change(int, int, int, int); void b2P2Find(int); }\n', 1),
        ("\tpqList[++numItems] = item;\n\tupHeap(numItems);", "\tpqList[++numItems] = item;\n\tb2P2Insert(item.id, item.key, numItems, maxItems);\n\tupHeap(numItems);", 1),
        ("\titem = pqList[1];\n", "\titem = pqList[1];\n\tb2P2Remove(item.id, item.key);\n", 1),
        ("void PriorityQueue::change (int itemIndex, int newValue) {\n", "void PriorityQueue::change (int itemIndex, int newValue) {\n\tb2P2Change(itemIndex, newValue, numItems, maxItems);\n", 1),
        ("\t\t\treturn(index);\n        }\n\treturn(0);\n", "\t\t\tb2P2Find(1);\n\t\t\treturn(index);\n        }\n\tb2P2Find(0);\n\treturn(0);\n", 1),
    ])
    # Solver tags.
    tagdecl = 'extern "C" { extern const char* g_b2Solver; }\n'
    edit_nth(p, "mclib/move_movemap_path.cpp", "\tsolveOpen->clear();", '\tg_b2Solver = "movemap"; solveOpen->clear();', 0, 2)
    edit_nth(p, "mclib/move_movemap_path.cpp", "\tsolveOpen->clear();", '\tg_b2Solver = "movemap_jump"; solveOpen->clear();', 0, 1)
    p.edit("mclib/move_movemap_escape.cpp", [("\tsolveOpen->clear();", '\tg_b2Solver = "movemap_escape"; solveOpen->clear();', 1)])
    p.edit("mclib/move_globalmap_path.cpp", [
        ("\topenList->clear();\n", '\tg_b2Solver = "globalmap"; openList->clear();\n', 1),
        ("\t\t\tlong openIndex = openList->find(door);\n", "\t\t\tlong openIndex = openList->find(door);\n\t\t\tb2P2Hit(door, openIndex, openList->getNumItems());\n", 1),
    ])
    p.edit("code/group_goals.cpp", [("\topenList->clear();\n", '\tg_b2Solver = "goalmap"; openList->clear();\n', 1)])
    for rel, extra in (("mclib/move_movemap_path.cpp", ""), ("mclib/move_movemap_escape.cpp", ""), ("code/group_goals.cpp", ""),
                       ("mclib/move_globalmap_path.cpp", 'extern "C" void b2P2Hit(long, long, int);\n')):
        with open(os.path.join(root, rel), "rb") as f:
            head = f.read()
        with open(os.path.join(root, rel), "wb") as f:
            f.write((tagdecl + extra).encode("latin-1") + head)
    # The inc and the frame hook.
    with open(os.path.join(HERE, "b2_probes.inc"), "rb") as f:
        inc = f.read().replace(b"\r\n", b"\n")
    with open(os.path.join(root, "code/sim_identity.cpp"), "rb") as f:
        raw = f.read()
    crlf = b"\r\n" in raw
    text = raw.replace(b"\r\n", b"\n").decode("latin-1")
    hook_old = "\tif (!simIdentityEnabled() || !simIdentityTrace() || !ObjectManager)\n\t\treturn;\n\n\tstd::vector<MoverRecord>& recs = scratch();"
    if text.count(hook_old) != 1:
        sys.exit("apply_probes: code/sim_identity.cpp: frame hook anchor not found exactly once")
    text = text.replace(hook_old, "\tif (!simIdentityEnabled() || !simIdentityTrace() || !ObjectManager)\n\t\treturn;\n\tb2P1Frame(simFrame);\n\n\tstd::vector<MoverRecord>& recs = scratch();")
    text = 'extern "C" void b2P1Frame(long);\n' + text + "\n" + inc.decode("latin-1")
    if crlf:
        text = text.replace("\n", "\r\n")
    with open(os.path.join(root, "code/sim_identity.cpp"), "wb") as f:
        f.write(text.encode("latin-1"))
    print("apply_probes: code/sim_identity.cpp (frame hook + probes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
