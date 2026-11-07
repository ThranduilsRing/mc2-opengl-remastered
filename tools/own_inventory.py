#!/usr/bin/env python3
"""own_inventory - who allocates each type, who frees it, and which class owns it (the step 4 ownership ledger).

WHAT: a grep-grade scan of the committed C++ sources (comments and strings blanked) that lists, per allocated type, its `new T` / `new T[]`
sites, the `delete` / `delete[]` sites that free a member the type was assigned to, and the classes that both assign the member and free it
(the owner). The committed baseline `tests/unit/own_inventory_baseline.json` holds the per-type, per-file counts; a batch that migrates a
type to an owned form commits the row diff with its code.
WHY: the ownership step is judged per type ("every site of T is owned by X"), and a reviewer needs the site list, not the LG03 total. The
`--check` row is a ratchet: a type may not gain allocation or free sites and no new type may appear unseen; counts that fell are reported
so the baseline can be lowered (`--write-baseline`) with the reason in the commit.

  py -3 tools/own_inventory.py --report [--type EString] [--dirs code mclib]
  py -3 tools/own_inventory.py --write-baseline
  py -3 tools/own_inventory.py --check
  py -3 tools/own_inventory.py --selftest

Heuristics, not a parser: the type of a `delete x` is known only when `x` is a member that some `member = new T` assigned in the same class;
other deletes are counted per file under "untyped". The enclosing class comes from the nearest `Class::method(` definition above the site.
Stdlib only; reads HEAD through git so other agents' uncommitted edits never flap the row.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "cxx17_ratchet"))
import cxx17_ratchet as cr  # noqa: E402  (git_blobs and the comment/string blanker)

BASELINE = os.path.join(ROOT, "tests", "unit", "own_inventory_baseline.json")
DEFAULT_DIRS = ["code", "mclib", "gui", "GameOS", "RenderCore", "RenderWorld", "GameAdapters", "GuiRuntime", "editor", "Gates"]

_NEW = re.compile(r"\bnew\b\s*(\((?:[^()]|\([^()]*\))*\)\s*)?(?:\(\s*)?(?:const\s+)?([A-Za-z_][\w:]*(?:\s*<[^;{}()]*?>)?)\s*(\[)?")
_DELETE = re.compile(r"\bdelete\b\s*(\[\s*\])?\s*\(?\s*([A-Za-z_][\w\.\->\[\]]*)")
#: `operator new` / `operator delete` tokens are allocator definitions or calls to the global ones, never a typed site.
_OPERATOR = re.compile(r"operator\s*$")
_ASSIGN_NEW = re.compile(r"([A-Za-z_][\w\.\->\[\]]*)\s*=\s*new\b")
#: A function definition whose signature starts a line: `Class::method(` names the class; a free function resets the owner to file scope.
_FUNC = re.compile(r"^(?:[\w:<>,\*&~]+[ \t]+)*(?:[\w:<>,\*&~]*?\b)?([A-Za-z_][\w:]*?)(?:::(~?[A-Za-z_]\w*))?\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?\{", re.M)
_SKIP_TYPES = {"char", "unsigned", "BYTE", "unsigned char", "int", "long", "float", "double", "short", "bool", "void", "uint8_t", "int8_t",
               "uint16_t", "int16_t", "uint32_t", "int32_t", "uint64_t", "int64_t", "DWORD", "WORD", "size_t", "wchar_t", "MemoryPtr"}


def _owner_of(line_no: int, funcs: list) -> str:
    """The class of the nearest `Class::method(` definition above `line_no` ("" at file scope)."""
    owner = ""
    for ln, cls in funcs:
        if ln > line_no:
            break
        owner = cls
    return owner


def scan_text(text: str, rel: str) -> dict:
    """-> {"allocs": [(type, array, line, owner, member)], "frees": [(array, expr, line, owner)]} for one file (comments and strings blanked)."""
    clean = "\n".join(cr.strip(text)[1])
    funcs = []
    for m in _FUNC.finditer(clean):
        cls = m.group(1).split("::")[-1] if m.group(2) else ""
        funcs.append((clean.count("\n", 0, m.start()) + 1, cls))
    allocs, frees = [], []
    for m in _NEW.finditer(clean):
        if _OPERATOR.search(clean, max(0, m.start() - 12), m.start()):
            continue   # `operator new(size)`: an allocator definition or a call to the global one, not an allocation of a type
        if m.group(1) and not m.group(1).lstrip("(").lstrip().startswith(("std::nothrow", "nothrow")):
            continue   # placement new (`new (buf) T`, also `new (static_cast<void*>(p)) T`) constructs in storage it was handed: no allocation
        t = re.sub(r"\s+", "", m.group(2))
        if t in _SKIP_TYPES or t.startswith("(") or t == "" or t[0].isdigit():
            continue
        ln = clean.count("\n", 0, m.start()) + 1
        head = clean[max(0, clean.rfind("\n", 0, m.start())) : m.start()]
        am = _ASSIGN_NEW.search(head + "new")
        member = am.group(1) if am else ""
        allocs.append((t, bool(m.group(3)), ln, _owner_of(ln, funcs), member))
    for m in _DELETE.finditer(clean):
        if _OPERATOR.search(clean, max(0, m.start() - 12), m.start()):
            continue
        ln = clean.count("\n", 0, m.start()) + 1
        frees.append((bool(m.group(1)), m.group(2), ln, _owner_of(ln, funcs)))
    return {"allocs": allocs, "frees": frees}


def inventory(blobs: dict) -> dict:
    """-> {"types": {T: {"new": n, "new_array": n, "delete": n, "delete_array": n, "owners": [...], "files": {rel: {...counts}},
    "sites": {...}}}, "untyped_frees": {rel: n}}. A free is typed when its expression is a member some `member = new T` assigned in the
    same class."""
    per_file = {rel: scan_text(data.decode("utf-8", "replace"), rel) for rel, data in blobs.items()}
    member_type = {}   # (owner class, member expr) -> type
    for rel, res in per_file.items():
        for t, arr, ln, owner, member in res["allocs"]:
            if member:
                member_type[(owner, member.replace("this->", ""))] = t
    types = defaultdict(lambda: {"new": 0, "new_array": 0, "delete": 0, "delete_array": 0, "owners": set(), "files": defaultdict(lambda: defaultdict(int)), "sites": []})
    untyped = defaultdict(int)
    for rel, res in sorted(per_file.items()):
        for t, arr, ln, owner, member in res["allocs"]:
            rec = types[t]
            key = "new_array" if arr else "new"
            rec[key] += 1
            rec["files"][rel][key] += 1
            rec["sites"].append(f"{rel}:{ln} {key} in {owner or '(file scope)'}" + (f" -> {member}" if member else ""))
        for arr, expr, ln, owner in res["frees"]:
            t = member_type.get((owner, expr.replace("this->", "")))
            if not t:
                untyped[rel] += 1
                continue
            rec = types[t]
            key = "delete_array" if arr else "delete"
            rec[key] += 1
            rec["files"][rel][key] += 1
            rec["owners"].add(owner)
            rec["sites"].append(f"{rel}:{ln} {key} {expr} in {owner}")
    out = {}
    for t, rec in sorted(types.items()):
        out[t] = {"new": rec["new"], "new_array": rec["new_array"], "delete": rec["delete"], "delete_array": rec["delete_array"],
                  "owners": sorted(rec["owners"]), "files": {f: dict(c) for f, c in sorted(rec["files"].items())}, "sites": rec["sites"]}
    return {"types": out, "untyped_frees": dict(sorted(untyped.items()))}


def baseline_from(inv: dict, dirs: list) -> dict:
    """The committed shape: per-type counts and per-file counts, no line numbers (they churn on unrelated edits)."""
    return {"about": "tools/own_inventory.py --write-baseline; per type: allocation and typed free sites, owners, per-file counts",
            "dirs": dirs,
            "types": {t: {k: v for k, v in rec.items() if k != "sites"} for t, rec in inv["types"].items()},
            "untyped_frees": inv["untyped_frees"]}


def check(inv: dict, base: dict, out=sys.stdout) -> int:
    """Ratchet: a type may not gain `new`/`delete` sites, a type unknown to the baseline may not appear, untyped frees per file may not rise.
    Falls are reported (re-baseline). -> 0 ok, 1 a count rose."""
    rose, fell = [], []
    bt = base.get("types", {})
    for t, rec in inv["types"].items():
        brec = bt.get(t)
        if brec is None:
            rose.append(f"new type {t}: new={rec['new']} new_array={rec['new_array']} ({'; '.join(rec['sites'][:3])})")
            continue
        for k in ("new", "new_array", "delete", "delete_array"):
            if rec[k] > brec.get(k, 0):
                rose.append(f"{t}.{k}: {brec.get(k, 0)} -> {rec[k]}")
            elif rec[k] < brec.get(k, 0):
                fell.append(f"{t}.{k}: {brec.get(k, 0)} -> {rec[k]}")
    for t in bt:
        if t not in inv["types"]:
            fell.append(f"{t}: gone")
    bu = base.get("untyped_frees", {})
    for rel, n in inv["untyped_frees"].items():
        if n > bu.get(rel, 0):
            rose.append(f"untyped deletes in {rel}: {bu.get(rel, 0)} -> {n}")
    for line in fell:
        print("own_inventory: lowered (rewrite the baseline): " + line, file=out)
    for line in rose:
        print("own_inventory: FAIL " + line, file=out)
    print(f"own_inventory: {'FAIL' if rose else 'ok'} ({len(inv['types'])} types, {len(rose)} rose, {len(fell)} fell)", file=out)
    return 1 if rose else 0


def selftest() -> int:
    src = """
class Foo {
public:
	Foo();
	~Foo();
	void reset();
	Bar* bar;
	char* name;
};
Foo::Foo() {
	bar = new Bar(1, 2);   // owned
	name = new char[32];
	this->bar2 = new Bar;
}
Foo::~Foo() {
	delete bar;
	delete [] name;
	delete bar2;
}
void Foo::reset() { Baz* tmp = new Baz; delete tmp; }
void helper() { Qux* q = new Qux[4]; delete[] q; }
void* Foo::operator new (size_t ourSize) { return ::operator new(ourSize); }
void Foo::seek(long newPosition) { logicalPosition = newPosition; }
void Foo::prune() { deleteNode(bar); }
void Foo::operator delete (void* us) { ::operator delete(us); }
"""
    fails = []
    inv = inventory({"code/foo.cpp": src.encode()})
    t = inv["types"]
    if not (t["Bar"]["new"] == 2 and t["Bar"]["delete"] == 2 and t["Bar"]["owners"] == ["Foo"]):
        fails.append(f"Bar should be allocated twice and freed twice by Foo: {t['Bar']}")
    if "char" in t:
        fails.append("builtin element types are not tracked")
    if inv["untyped_frees"].get("code/foo.cpp", 0) != 1:
        fails.append(f"deleteNode(bar) is a call, not a free: {inv['untyped_frees']}")
    if "Position" in t:
        fails.append("an identifier that starts with new (newPosition) is not an allocation")
    if "ourSize" in t or "size_t" in t:
        fails.append("operator new definitions and calls are not allocation sites")
    if not (t["Baz"]["new"] == 1 and t["Baz"]["delete"] == 1 and t["Baz"]["owners"] == ["Foo"]):
        fails.append(f"a local new/delete pair in a method is typed through its variable: {t['Baz']}")
    if not (t["Qux"]["new_array"] == 1 and t["Qux"]["delete_array"] == 1 and t["Qux"]["owners"] == [""]):
        fails.append(f"a file-scope array pair: {t['Qux']}")
    if inv["untyped_frees"] != {"code/foo.cpp": 1}:
        fails.append(f"delete [] name is untyped (char): {inv['untyped_frees']}")
    base = baseline_from(inv, ["code"])
    import io
    if check(inv, base, io.StringIO()) != 0:
        fails.append("an inventory equal to its baseline passes")
    inv2 = inventory({"code/foo.cpp": (src + "\nvoid more() { Bar* b = new Bar; }\n").encode()})
    if check(inv2, base, io.StringIO()) != 1:
        fails.append("a new allocation site of a known type fails")
    inv3 = inventory({"code/foo.cpp": (src + "\nvoid more() { Zed* z = new Zed; }\n").encode()})
    if check(inv3, base, io.StringIO()) != 1:
        fails.append("a type the baseline does not know fails")
    inv4 = inventory({"code/foo.cpp": src.replace("void helper() { Qux* q = new Qux[4]; delete[] q; }", "").encode()})
    if check(inv4, base, io.StringIO()) != 0:
        fails.append("fewer sites pass (and are reported as lowered)")
    for f in fails:
        print("own_inventory selftest FAIL: " + f)
    print("own_inventory selftest: " + ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--report", action="store_true")
    g.add_argument("--write-baseline", action="store_true")
    g.add_argument("--check", action="store_true")
    g.add_argument("--selftest", action="store_true")
    ap.add_argument("--type", action="append", default=[], help="report only these types (repeatable)")
    ap.add_argument("--dirs", nargs="+", default=None, help="directories to scan (default: the baseline's, else %s)" % " ".join(DEFAULT_DIRS))
    ap.add_argument("--worktree", action="store_true", help="scan the working tree instead of HEAD")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    base = None
    if os.path.exists(BASELINE):
        with open(BASELINE, encoding="utf-8") as fh:
            base = json.load(fh)
    dirs = args.dirs or (base or {}).get("dirs") or DEFAULT_DIRS
    if args.worktree:
        blobs = {}
        for rel in cr.list_files(dirs):
            with open(os.path.join(ROOT, rel), "rb") as fh:
                blobs[rel] = fh.read()
    else:
        blobs = cr.git_blobs(dirs, "head")
    for rel in cr.INVENTORY_EXEMPT:
        blobs.pop(rel, None)
    inv = inventory(blobs)
    if args.write_baseline:
        with open(BASELINE, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(baseline_from(inv, dirs), fh, indent=1, sort_keys=True)
            fh.write("\n")
        print(f"own_inventory: wrote {os.path.relpath(BASELINE, ROOT)} ({len(inv['types'])} types from {len(blobs)} files)")
        return 0
    if args.check:
        if base is None:
            print("own_inventory: no baseline; run --write-baseline")
            return 2
        return check(inv, base)
    wanted = set(args.type)
    for t, rec in inv["types"].items():
        if wanted and t not in wanted:
            continue
        print(f"{t}: new={rec['new']} new[]={rec['new_array']} delete={rec['delete']} delete[]={rec['delete_array']} owners={','.join(rec['owners']) or '-'}")
        if wanted:
            for s in rec["sites"]:
                print("  " + s)
    print(f"untyped deletes: {sum(inv['untyped_frees'].values())} in {len(inv['untyped_frees'])} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
