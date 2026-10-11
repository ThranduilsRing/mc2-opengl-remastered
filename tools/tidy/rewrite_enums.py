#!/usr/bin/env python3
"""rewrite_enums - turn a named unscoped enum into `enum class NAME { ... }; using enum NAME;` (cxx17 ratchet LG14) and PROVE every including object is unchanged.

WHAT: `using enum NAME;` (C++20) puts the enumerators back into the enclosing scope, so unqualified uses keep compiling; what breaks is exactly the code that treated the
enumerators as ints (arithmetic, bit flags, array indices, comparison or assignment with an int, switch on an int). A token census refuses an enum when any enumerator
sits next to an arithmetic / bitwise / index token, then the compile decides the rest: the including objects are rebuilt and compared with tools/objhash.py (the same Proof
as run_tidy_headers.py and rewrite_defines.py); a file whose includers fail to compile or differ is bisected out and reverted.
WHY: an unscoped enum converts silently to int and leaks its enumerators; a scoped enum states what the value is. The skipped enums stay in the ratchet floor on purpose.

  py -3 tools/tidy/rewrite_enums.py census                       # candidates, refusals by reason, nothing changed
  py -3 tools/tidy/rewrite_enums.py run --scope cpp --max-files 20 [--commit] [--done ledger.txt] [--taint-file ledger.txt]
  py -3 tools/tidy/rewrite_enums.py run --scope header --tree build-alltargets --max-objs 120 --max-files 15 [--commit]
Refused up front: a typedef'd or anonymous enum, an enum without a body, an enum whose enumerator values need the unscoped underlying type (a value above INT_MAX without a
fixed type), an enum in a header a C / .rc / .asm file includes, a name defined twice, and files dirty here or in the other lane's worktree."""
from __future__ import annotations

import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rewrite_defines as rd  # noqa: E402
import run_tidy_safe as rts  # noqa: E402
import run_tidy_headers as rth  # noqa: E402
import canonical_obj  # noqa: E402

cr = rd.cr
ENUM_HEAD = re.compile(r"\benum\b(?!\s+(?:class|struct)\b)\s+([A-Za-z_]\w*)\s*(:\s*[\w ]+?)?\s*\{")
TYPEDEF_ENUM = re.compile(r"\btypedef\s+enum\b")
INT_MAX = 0x7FFFFFFF
ARITH_BEFORE = re.compile(r"[-+*/%|&^~<>\[\(]\s*$")
ARITH_AFTER = re.compile(r"^\s*(?:[-+*/%|&^<>\]]|\+\+|--|==\s*\d|!=\s*\d)")


class Enum:
    __slots__ = ("path", "line", "name", "fixed", "body", "close_line", "reasons", "scope", "enumerators")

    def __init__(self, path, line, name, fixed, body, close_line):
        self.path, self.line, self.name, self.fixed, self.body, self.close_line = path, line, name, fixed, body, close_line
        self.reasons, self.enumerators, self.scope = [], [], "ns"


def enumerators_of(body: str):
    out = []
    for part in re.split(r",", re.sub(r"\([^()]*\)", "()", body)):
        m = re.match(r"\s*([A-Za-z_]\w*)\s*(?:=\s*(.+?))?\s*$", part, re.S)
        if m:
            out.append((m.group(1), m.group(2)))
    return out


def literal_value(expr):
    if expr is None:
        return None
    e = expr.strip()
    if re.fullmatch(r"-?(?:0[xX][0-9a-fA-F]+|\d+)[uUlL]*", e):
        try:
            return int(e.rstrip("uUlL"), 0)
        except ValueError:
            return None
    return None


def analyse(files: dict, c_side_headers=frozenset(), other_idents=frozenset()):
    """-> (enums, uses {enumerator: [(path, line, left, right)]}); an enum's `reasons` list is empty when it is a candidate."""
    enums, scopes_of = [], {}
    for path, text in files.items():
        nc, bl = cr.strip(text)
        joined = "\n".join(nc)
        starts = [0]
        for l in nc:
            starts.append(starts[-1] + len(l) + 1)
        scopes = rd._scopes(bl)
        for m in ENUM_HEAD.finditer(joined):
            if TYPEDEF_ENUM.search(joined[max(0, m.start() - 12):m.start() + 5]):
                continue
            ln = joined.count("\n", 0, m.start())
            depth, i = 1, m.end()
            while i < len(joined) and depth:
                depth += (joined[i] == "{") - (joined[i] == "}")
                i += 1
            tail = joined[i:i + 40].lstrip()
            body = joined[m.end():i - 1]
            e = Enum(path, ln + 1, m.group(1), (m.group(2) or "").strip(":").strip(), body, joined.count("\n", 0, i) + 1)
            e.scope = "ns" if scopes[ln] else "class"
            e.enumerators = enumerators_of(body)
            if not tail.startswith(";"):
                e.reasons.append("declarator-after-body")
            enums.append(e)
    names = collections.Counter(e.name for e in enums)
    enumerators = {n for e in enums for n, _ in e.enumerators}
    uses = collections.defaultdict(list)
    for path, text in files.items():
        nc, bl = cr.strip(text)
        for i, l in enumerate(bl):
            if re.match(r"\s*#", l):
                continue
            for m in rd.TOKEN.finditer(l):
                if m.group(0) in enumerators:
                    uses[m.group(0)].append((path, i + 1, l[:m.start()], l[m.end():]))
    by_enumerator = collections.defaultdict(set)
    for e in enums:
        for n, _ in e.enumerators:
            by_enumerator[n].add(e.name)
    for e in enums:
        r = e.reasons
        if names[e.name] > 1:
            r.append("name-defined-twice")
        if not e.enumerators:
            r.append("empty")
        if e.name in other_idents:
            r.append("named-outside-c++")
        if os.path.basename(e.path) in c_side_headers:
            r.append("included-from-c-side")
        if not e.fixed and any((literal_value(v) or 0) > INT_MAX for _, v in e.enumerators):
            r.append("needs-unscoped-underlying-type")
        for n, v in e.enumerators:
            if len(by_enumerator[n]) > 1:
                r.append("enumerator-name-shared")
                break
        for n, _ in e.enumerators:
            for path, ln, left, right in uses[n]:
                if path == e.path and e.line <= ln <= e.close_line:
                    continue                                            # inside its own body
                if ARITH_BEFORE.search(left) and not re.search(r"[A-Za-z_]\s*\(\s*$|,\s*$|\{\s*$|\(\s*$|=\s*$|return\s*$|case\s*$", left):
                    r.append("arithmetic-or-index-use")
                elif ARITH_AFTER.match(right):
                    r.append("arithmetic-or-index-use")
                if "arithmetic-or-index-use" in r:
                    break
            if "arithmetic-or-index-use" in r:
                break
    return enums, uses


def rewrite_text(text: str, enums) -> str:
    lines = text.split("\n")
    for e in sorted(enums, key=lambda x: -x.line):
        i = e.line - 1
        lines[i] = re.sub(r"\benum\s+" + re.escape(e.name) + r"\b", "enum class " + e.name, lines[i], count=1)
        j = e.close_line - 1
        # the declaration ends at the first ';' at or after the closing brace line
        while j < len(lines) and ";" not in lines[j].split("}")[-1] and "};" not in lines[j]:
            j += 1
        indent = re.match(r"\s*", lines[i]).group(0)
        eol = "\r" if lines[j].endswith("\r") else ""
        lines.insert(j + 1, indent + "using enum " + e.name + ";" + eol)
    return "\n".join(lines)


def load_world():
    paths = rd.tracked()
    cxx = [p for p in paths if p.lower().endswith(rd.CXX_EXT) and p.split("/", 1)[0] in rd.DIRS and ".gen." not in p]
    files = {p: rd.read(p) for p in cxx}
    enums, uses = analyse(files, rd.c_side_header_names(paths), rd.outside_identifiers(paths))
    return paths, files, enums, uses


def cmd_census(a):
    paths, files, enums, uses = load_world()
    c = collections.Counter(e.reasons[0] if e.reasons else "CANDIDATE" for e in enums)
    print("named unscoped enums with a body: %d  candidates: %d" % (len(enums), c["CANDIDATE"]))
    for r, n in c.most_common():
        print("  %4d  %s" % (n, r))
    return 0


def cmd_run(a):
    sys.stdout.reconfigure(line_buffering=True)
    tree_abs = os.path.join(rts.ROOT, a.tree)
    rts.apply_lane_args(a)
    if not a.strict_proof:
        canonical_obj.install()
    includers = rth.includer_map(tree_abs)
    a.s2t = {v: k for k, v in rth.target_to_source(tree_abs).items()}
    a.taint = rd.load_taint(a.taint_file)
    done = {l.strip() for l in open(a.done, encoding="utf-8")} if a.done and os.path.exists(a.done) else set()
    batches = 0
    while True:
        paths, files, enums, uses = load_world()
        dirty = rts.dirty_files() | rd.other_worktree_dirty(a.skip_dirty_in)
        by_file = collections.defaultdict(list)
        for e in enums:
            if not e.reasons and not rts.is_excluded(e.path) and e.path not in dirty and e.path not in done:
                by_file[e.path].append(e)
        plan = []
        for rel, es in by_file.items():
            is_src = rel.lower().endswith(rd.SRC_EXT)
            if (a.scope == "cpp") != is_src and a.scope != "all":
                continue
            objs = {a.s2t[rel]} if is_src and rel in a.s2t else (includers.get(rel, set()) if not is_src else set())
            if is_src:
                es = [e for e in es if all(u[0] == rel for n, _ in e.enumerators for u in uses[n])]
            else:
                es = [e for e in es if all(u[0] == rel or not u[0].lower().endswith(rd.SRC_EXT) or a.s2t.get(u[0]) in objs for n, _ in e.enumerators for u in uses[n])]
            if es and objs and not (objs & a.taint) and (is_src or len(objs) <= a.max_header_includers):
                plan.append((len(objs), rel, es, objs))
        plan.sort(key=lambda x: (x[0], x[1]))
        picked, objs = [], set()
        for n, rel, es, o in plan:
            if picked and len(objs | o) > a.max_objs:
                continue
            picked.append((rel, es, o))
            objs |= o
            if len(picked) >= a.max_files:
                break
        if not picked:
            print("rewrite_enums: no candidate file left")
            return 0
        batches += 1
        nenum = sum(len(es) for _, es, _ in picked)
        print("BATCH %d: %d file(s), %d enum(s), %d object(s) in the proof set" % (batches, len(picked), nenum, len(objs)))
        if a.dry_run:
            for rel, es, o in picked:
                print("  %s  %s  (%d object(s))" % (rel, ",".join(e.name for e in es), len(o)))
            return 0
        if a.done:
            with open(a.done, "a", encoding="utf-8", newline="\n") as fh:
                fh.writelines(rel + "\n" for rel, _, _ in picked)
            done |= {rel for rel, _, _ in picked}
        orig = {rel: rth.read_bytes(rel) for rel, _, _ in picked}
        edited = {rel: rewrite_text(orig[rel].decode("utf-8", errors="replace"), es).encode("utf-8") for rel, es, _ in picked}
        mtimes = {rel: os.stat(os.path.join(rts.ROOT, rel)).st_mtime_ns for rel in orig}
        inc = {rel: o for rel, _, o in picked}
        proof = rth.Proof(tree_abs, inc, orig, edited, a.build_jobs)
        if proof.baseline():
            print("rewrite_enums: the base build of the proof set failed; refusing to edit", file=sys.stderr)
            return 2
        kept, rejected = proof.solve()
        for rel, why in rejected:
            rts.restore_file(rel, mtimes[rel])
            print("REVERT %s  %s" % (rel, why))
        new_taint = rd.taint_from_reasons(rejected) - a.taint
        if new_taint and a.taint_file:
            a.taint |= new_taint
            with open(a.taint_file, "a", encoding="utf-8", newline="\n") as fh:
                fh.writelines(o + "\n" for o in sorted(new_taint))
        kept_enums = sum(len(es) for rel, es, _ in picked if rel in kept)
        print("BATCH %d result: kept %d file(s) / %d enum(s), reverted %d file(s), %d object(s) proven identical" % (
            batches, len(kept), kept_enums, len(rejected), len({o for r in kept for o in inc[r]})))
        if kept and a.commit:
            rd.commit_paths(kept, "tidy(LG14): %d unscoped enum(s) -> enum class + using enum in %d file(s) (objhash identical over every including object)" % (kept_enums, len(kept)),
                            "cxx17 ratchet baseline tightened after LG14 batch")
        if not a.loop or (a.max_batches and batches >= a.max_batches) or (not kept and not rejected):
            return 1 if rejected else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("census")
    r = sub.add_parser("run")
    r.add_argument("--scope", choices=("cpp", "header", "all"), default="cpp")
    r.add_argument("--tree", default="build64-ninja")
    r.add_argument("--max-files", type=int, default=20)
    r.add_argument("--max-objs", type=int, default=120)
    r.add_argument("--max-header-includers", type=int, default=150)
    r.add_argument("--build-jobs", type=int, default=6)
    r.add_argument("--done")
    r.add_argument("--strict-proof", action="store_true", help="plain objhash digests instead of tools/tidy/canonical_obj.py")
    r.add_argument("--taint-file")
    r.add_argument("--skip-dirty-in", default="A:/Games/mc2-main")
    r.add_argument("--commit", action="store_true")
    r.add_argument("--loop", action="store_true")
    r.add_argument("--max-batches", type=int, default=0)
    r.add_argument("--dry-run", action="store_true")
    rts.add_lane_args(r)
    a = ap.parse_args(argv)
    return cmd_census(a) if a.cmd == "census" else cmd_run(a)


if __name__ == "__main__":
    sys.exit(main())
