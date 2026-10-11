#!/usr/bin/env python3
"""vs_trees - the includer map and an object-identity gate for the Visual Studio generator trees (contract harnesses, the doctest tier, standalone tools).

WHAT: the Ninja trees (build64-ninja, build-alltargets) give `ninja -t deps`; the harness and test trees are msbuild trees whose only record of "this object read that header" is
the CL.read tlog (paths in upper case). `includer_map` turns those logs into {repo-relative header: {tree::object}} and `snapshot` / `verify` hash the affected objects before
and after an edit with tools/objhash.py (optionally --layout-only-ok, which accepts LAYOUT-ONLY) so a rewriter batch can be proven over EVERY target, not only the root trees.
WHY: tests/unit (mc2_tests), tools/*_harness and tools/mc2_modlint include game headers the root trees never see; an `enum class` or `constexpr` edit can break their build or
change their objects, and the Ninja-only proof said nothing about them.

  py -3 tools/tidy/vs_trees.py map HEADER...             # which VS-tree objects include each header
  py -3 tools/tidy/vs_trees.py snapshot OUT.json HEADER... # build the including targets, hash their objects
  py -3 tools/tidy/vs_trees.py verify OUT.json [--layout-only-ok]   # rebuild, compare; exit 1 on any object that is not IDENTICAL (or LAYOUT-ONLY when allowed)
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import objhash  # noqa: E402
import console_child  # noqa: E402

TREES = ("build64-tests", "build64-harness", "build64-tgdump", "build64-gate-dump") + tuple(
    sorted(os.path.basename(p) for p in glob.glob(os.path.join(ROOT, "build64-h-*"))))
CMAKE = ("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")
CONFIG = "RelWithDebInfo"


def _read_tlog(path):
    raw = open(path, "rb").read()
    return raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else raw.decode("utf-8", "replace")


def _tracked_lower():
    out = subprocess.run(["git", "-C", ROOT, "ls-files"], capture_output=True, text=True, **console_child.child_kwargs(redirected=True)).stdout.split("\n")
    return {p.lower(): p for p in out if p}


def real_case(base, rel):
    """The path `rel` under `base` with the on-disk spelling of every component (the tlogs are upper case; cmake target names are not)."""
    cur, parts = base, []
    for comp in rel.replace("\\", "/").split("/"):
        try:
            names = os.listdir(cur)
        except OSError:
            names = []
        hit = next((n for n in names if n.lower() == comp.lower()), comp)
        parts.append(hit)
        cur = os.path.join(cur, hit)
    return "/".join(parts)


def includer_map(trees=TREES):
    """-> {repo-relative path: set of 'tree::object'}; every tracked file an object read (headers and sources)."""
    tracked = _tracked_lower()
    root_up = ROOT.upper()
    by = {}
    for tree in trees:
        tree_abs = os.path.join(ROOT, tree)
        if not os.path.isdir(tree_abs):
            continue
        for f in glob.glob(os.path.join(tree_abs, "**", "CL.read.*.tlog"), recursive=True):
            if "CompilerId" in f or "VCTargetsPath" in f:
                continue
            objdir = os.path.dirname(os.path.dirname(f))          # <tree>/<target>.dir/<config>/<x>.tlog/CL.read -> the object directory
            objkeys = []
            for line in _read_tlog(f).splitlines():
                if line.startswith("^"):
                    # a compiler run may batch several sources; its tlog entry is the UNION of their reads, so every source of the group gets every read (an over-approximation)
                    objkeys = []
                    for src in line[1:].split("|"):
                        o = os.path.join(objdir, os.path.splitext(os.path.basename(src))[0] + ".obj")
                        if os.path.exists(o):
                            objkeys.append(real_case(tree_abs, os.path.relpath(o, tree_abs)))
                    continue
                if not objkeys or not line.upper().startswith(root_up):
                    continue
                rel = os.path.relpath(line, ROOT).replace("\\", "/").lower()
                if rel in tracked:
                    for orel in objkeys:
                        by.setdefault(tracked[rel], set()).add("%s::%s" % (tree, orel))
    return by


def _error_files(text):
    """Repo-relative, lower-case paths named by the `file(line): error` lines of a build log."""
    out = set()
    for m in re.finditer(r"^\s*(?:\d+>)?([A-Za-z]:[^()\n]*?)\(\d+(?:,\d+)?\)\s*:\s*(?:fatal )?error", text, re.M):
        try:
            out.add(os.path.relpath(m.group(1).strip(), ROOT).replace("\\", "/").lower())
        except ValueError:
            pass
    return out


def build_trees(keys, by_header=None):
    """Build every target holding one of the 'tree::object' keys. -> {key: error text} for the objects that failed.
    A failed target blames only the objects its errors name: the source file itself, or a header the object includes (`by_header`: header -> keys). When no error can be
    attributed the whole target is blamed."""
    targets = {}
    for k in keys:
        tree, obj = k.split("::", 1)
        targets.setdefault((tree, obj.split("/")[0]), []).append(k)
    failed = {}
    for (tree, tdir), ks in sorted(targets.items()):
        target = tdir[:-4] if tdir.endswith(".dir") else tdir
        r = subprocess.run([CMAKE if os.path.isfile(CMAKE) else "cmake", "--build", os.path.join(ROOT, tree), "--config", CONFIG, "--target", target],
                           capture_output=True, text=True, errors="replace", cwd=ROOT, **console_child.child_kwargs(redirected=True))
        if not r.returncode:
            continue
        log = r.stdout + r.stderr
        msg = "\n".join(l for l in log.splitlines() if "error" in l)[:600]
        efiles = _error_files(log)
        stems = {os.path.splitext(os.path.basename(f))[0] for f in efiles if f.endswith((".cpp", ".cc", ".cxx", ".c"))}
        blamed = {k for k in ks if os.path.splitext(os.path.basename(k.split("::", 1)[1]))[0].lower() in stems}
        for h, hk in (by_header or {}).items():
            if h.lower() in efiles:
                blamed |= set(hk) & set(ks)
        for k in (blamed or ks):
            failed[k] = msg
    return failed


def snapshot(headers, out_path, trees=TREES):
    m = includer_map(trees)
    keys = sorted({k for h in headers for k in m.get(h, ())})
    by_header = {h: sorted(m.get(h, ())) for h in headers}
    failed = build_trees(keys, by_header)
    doc = {"headers": list(headers), "by_header": by_header, "objects": {}, "failed_before": failed}
    for k in keys:
        tree, obj = k.split("::", 1)
        p = os.path.join(ROOT, tree, obj)
        d = objhash.obj_digest(p, per_symbol=True) if os.path.exists(p) else None
        doc["objects"][k] = d
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(doc, fh)
    return doc


def verify(path, layout_only_ok=False):
    """-> (lines, bad count, layout-only object names)."""
    doc = json.load(open(path, encoding="utf-8"))
    failed = build_trees(doc["objects"], doc.get("by_header"))
    lines, bad, layout = [], 0, []
    for k, before in sorted(doc["objects"].items()):
        if k in failed and k not in doc["failed_before"]:
            lines.append("FAIL  %s  does not build: %s" % (k, failed[k].replace("\n", " | ")[:200]))
            bad += 1
            continue
        tree, obj = k.split("::", 1)
        p = os.path.join(ROOT, tree, obj)
        now = objhash.obj_digest(p, per_symbol=True) if os.path.exists(p) else None
        if before is None or now is None:
            lines.append("FAIL  %s  object missing" % k)
            bad += 1
            continue
        verdict, why = objhash.classify(before, now, before.get("persym"), now.get("persym"))
        if verdict == "IDENTICAL":
            lines.append("OK    %s" % k)
        elif verdict == "LAYOUT-ONLY" and layout_only_ok:
            lines.append("LAYOUT-ONLY  %s  (%s)" % (k, ", ".join(why)))
            layout.append(k)
        else:
            lines.append("FAIL  %s  %s %s" % (k, verdict, "; ".join(why[:4])))
            bad += 1
    return lines, bad, layout


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("map")
    m.add_argument("headers", nargs="+")
    s = sub.add_parser("snapshot")
    s.add_argument("out")
    s.add_argument("headers", nargs="+")
    v = sub.add_parser("verify")
    v.add_argument("snapshot")
    v.add_argument("--layout-only-ok", action="store_true")
    a = ap.parse_args(argv)
    if a.cmd == "map":
        mp = includer_map()
        for h in a.headers:
            print("%s: %d object(s)" % (h, len(mp.get(h, ()))))
            for k in sorted(mp.get(h, ()))[:8]:
                print("   " + k)
        return 0
    if a.cmd == "snapshot":
        doc = snapshot(a.headers, a.out)
        print("vs_trees: %d object(s) hashed, %d before-build failure(s)" % (len(doc["objects"]), len(doc["failed_before"])))
        return 0
    lines, bad, layout = verify(a.snapshot, a.layout_only_ok)
    print("\n".join(lines))
    print("vs_trees: %d problem(s), %d layout-only" % (bad, len(layout)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
