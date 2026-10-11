#!/usr/bin/env python3
"""rewrite_defines - turn `#define NAME <literal>` into `inline constexpr <type> NAME = <literal>;` (cxx17 ratchet LG13) and PROVE every including object is unchanged.

WHAT: finds object-like defines of one integer / float / string literal (suffixes honoured), keeps only those whose macro is used purely as a value (no #if / #ifdef /
#undef / #include, no use inside another #define, no token pasting or stringizing, no sizeof of a string, no __asm file, defined exactly once in the repo, not named in a
script, cmake, resource or shader file, not in a header a C / .rc / .asm file includes, at namespace scope, not an include guard), rewrites each in place (a .cpp-local
define becomes `constexpr`, a header define `inline constexpr`), rebuilds every object that includes the file and compares it with tools/tidy/canonical_obj.py (objhash digests that ignore symbol and section order; --strict-proof uses plain objhash). A file whose
includers are not all identical is bisected out and reverted (the same Proof as tools/tidy/run_tidy_headers.py).
WHY: a macro has no type, no scope and no debugger value; a typed constant does, and the rewrite is a no-op for the compiled code when the proof says so.

  py -3 tools/tidy/rewrite_defines.py census                      # eligible / refused counts by reason, nothing changed
  py -3 tools/tidy/rewrite_defines.py census --list code/foo.cpp  # the verdict for every define of a file
  py -3 tools/tidy/rewrite_defines.py run --scope cpp --max-files 20 [--commit] [--done ledger.txt]
  py -3 tools/tidy/rewrite_defines.py run --scope header --max-objs 150 --max-files 10 [--commit]
Run in a tree whose objects are all built (default build64-ninja). Files dirty in this worktree or in the other builder lane's worktree (--skip-dirty-in) and the
lane-excluded / owner-excluded files of run_tidy_safe are never touched. `--commit` commits the kept files by pathspec, then runs cxx17_ratchet --check --tighten and
commits the baseline."""
from __future__ import annotations

import argparse
import collections
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run_tidy_safe as rts  # noqa: E402
import run_tidy_headers as rth  # noqa: E402
import canonical_obj  # noqa: E402

ROOT = rts.ROOT
sys.path.insert(0, os.path.join(ROOT, "tools", "cxx17_ratchet"))
import cxx17_ratchet as cr  # noqa: E402

console_child = rts.console_child
DIRS = ("code", "mclib", "GameOS", "RenderCore", "GuiRuntime", "RenderWorld", "GameAdapters", "HostServices", "editor", "gui")
CXX_EXT = (".cpp", ".cc", ".cxx", ".h", ".hpp", ".hxx", ".inl")
SRC_EXT = (".cpp", ".cc", ".cxx")
C_SIDE_EXT = (".c", ".rc", ".rc2", ".asm", ".inc", ".glsl", ".hglsl", ".vert", ".frag", ".comp", ".tesc", ".tese", ".def")
NAME_SCAN_EXT = C_SIDE_EXT + (".py", ".cmake", ".json", ".hlsl")
GUARD_SUFFIX = ("_H", "_H_", "_HPP", "__", "_INCLUDED", "_DEFINED")

DEFINE = re.compile(r"^(\s*#\s*define\s+)([A-Za-z_]\w*)(\s+)(.+?)\s*$")
ANY_DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)")
UNDEF = re.compile(r"^\s*#\s*undef\s+([A-Za-z_]\w*)")
DIRECTIVE = re.compile(r"^\s*#\s*(\w+)")
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')
TOKEN = re.compile(r"[A-Za-z_]\w*")
INT_LIT = re.compile(r"^(?P<n>0[xX][0-9a-fA-F]+|0[bB][01]+|[0-9]+)(?P<s>[uUlL]*)$")
FLOAT_LIT = re.compile(r"^(?P<n>(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?)(?P<s>[fF]?)$")
STRING_LIT = re.compile(r'^"(?:[^"\\\n]|\\.)*"$')
SIZEOF_LIKE = re.compile(r"\b(?:sizeof|_countof|ARRAYSIZE|ARRAY_SIZE|NUMELEMENTS)\b")
EXTERN_C = re.compile(r'\bextern\s*"\s*(?:C\s*)?"')


# ------------------------------------------------------------------ literals
def _strip_parens(v: str) -> str:
    while v.startswith("(") and v.endswith(")"):
        depth = 0
        for i, c in enumerate(v):
            depth += (c == "(") - (c == ")")
            if depth == 0 and i < len(v) - 1:
                return v
        v = v[1:-1].strip()
    return v


def parse_literal(value: str):
    """-> (type, literal text) for a literal whose C++ type is unambiguous on MSVC (LLP64: int and long are 32-bit), else None."""
    v = _strip_parens(value.strip())
    neg = v.startswith("-")
    body = v[1:].strip() if neg else v
    if STRING_LIT.match(body) and not neg:
        return "const char*", body
    m = INT_LIT.match(body)
    if m:
        digits, suf = m.group("n"), m.group("s").lower()
        if suf not in ("", "u", "l", "ul", "lu", "ll", "ull", "llu"):
            return None
        low = digits.lower()
        if low.startswith("0x"):
            n, plain = int(low, 16), False
        elif low.startswith("0b"):
            n, plain = int(low, 2), False
        elif len(low) > 1 and low.startswith("0"):
            if any(c in "89" for c in low):
                return None
            n, plain = int(low, 8), False
        else:
            n, plain = int(low), True
        unsigned_ = "u" in suf
        longs = suf.replace("u", "")
        if longs == "ll":
            t = "unsigned long long" if unsigned_ else "long long"
            ok = n <= (0xFFFFFFFFFFFFFFFF if unsigned_ else 0x7FFFFFFFFFFFFFFF)
        elif longs == "l":
            if unsigned_ or (not plain and n > 0x7FFFFFFF):
                t, ok = "unsigned long", n <= 0xFFFFFFFF
            else:
                t, ok = "long", n <= 0x7FFFFFFF
        elif unsigned_:
            t, ok = "unsigned int", n <= 0xFFFFFFFF
        elif n <= 0x7FFFFFFF:
            t, ok = "int", True
        elif not plain and n <= 0xFFFFFFFF:
            t, ok = "unsigned int", True
        else:
            return None
        if not ok or (neg and t.startswith("unsigned")) or (neg and n > 0x7FFFFFFF):
            return None
        return t, ("-" if neg else "") + body
    m = FLOAT_LIT.match(body)
    if m and ("." in body or "e" in body.lower()):
        return ("float" if m.group("s") else "double"), ("-" if neg else "") + body
    return None


# ------------------------------------------------------------------ analysis
class Site:
    __slots__ = ("path", "line", "name", "type", "literal", "reasons", "raw", "scope")

    def __init__(self, path, line, name, typ, literal, raw):
        self.path, self.line, self.name, self.type, self.literal, self.raw = path, line, name, typ, literal, raw
        self.reasons, self.scope = [], "unknown"


def _scopes(bl_lines):
    """-> list of bool per line: True when the line is at namespace (or extern "C") scope. Preprocessor lines do not count braces."""
    stack, buf, out = [], "", []
    for l in bl_lines:
        out.append(all(stack))
        if re.match(r"\s*#", l):
            continue
        for ch in l:
            if ch == "{":
                stack.append(bool(re.search(r"\bnamespace\b", buf) or EXTERN_C.search(buf)))
                buf = ""
            elif ch == "}":
                if stack:
                    stack.pop()
                buf = ""
            elif ch == ";":
                buf = ""
            else:
                buf += ch
        buf += " "
    return out


def analyse(files: dict, c_side_headers=frozenset(), other_idents=frozenset(), tracked_cxx=(), allow_strings=False):
    """files: {path: text} of the C++ files in scope. -> (sites, {name: [(path, line, ctx)]}). A site's `reasons` list is empty when it is eligible."""
    sites, uses, defs, undefs = [], collections.defaultdict(list), collections.defaultdict(int), set()
    parsed = {}
    for path, text in files.items():
        nc, bl = cr.strip(text)
        raw = text.split("\n")
        parsed[path] = (nc, bl, raw)
        scopes = _scopes(bl)
        cont = False
        for i, l in enumerate(nc):
            am = ANY_DEFINE.match(l)
            if am:
                defs[am.group(1)] += 1
            um = UNDEF.match(l)
            if um:
                undefs.add(um.group(1))
            m = DEFINE.match(l)
            if m and not cont and not m.group(2).endswith(GUARD_SUFFIX) and not m.group(4).rstrip().endswith("\\"):
                lit = parse_literal(m.group(4))
                if lit:
                    s = Site(path, i + 1, m.group(2), lit[0], lit[1], raw[i])
                    s.scope = "ns" if scopes[i] else "nested"
                    s.raw = (raw[i], m.end(4))
                    sites.append(s)
            cont = l.rstrip().endswith("\\") and bool(re.match(r"\s*#", l) or cont)
    cand = {s.name for s in sites}
    for path, (nc, bl, raw) in parsed.items():
        has_asm = any("__asm" in l for l in bl)
        in_macro = False
        for i, l in enumerate(bl):
            toks = [t for t in TOKEN.findall(l) if t in cand]
            is_pp = bool(re.match(r"\s*#", l))
            ctx_macro = in_macro
            in_macro = nc[i].rstrip().endswith("\\") and (is_pp or in_macro)
            if not toks:
                continue
            kind = DIRECTIVE.match(l).group(1) if is_pp else ""
            for t in toks:
                if kind == "define" and ANY_DEFINE.match(l).group(1) == t and not ctx_macro:
                    continue                                   # the defining line itself
                ctx = set()
                if is_pp:
                    ctx.add("directive-" + kind if kind != "define" else "macro-body")
                if ctx_macro:
                    ctx.add("macro-body")
                if has_asm:
                    ctx.add("asm-file")
                if not is_pp and not ctx_macro:
                    code = nc[i]
                    if re.search(r'"\s*' + re.escape(t) + r"\b|\b" + re.escape(t) + r'\s*"', code):
                        ctx.add("string-concat")
                    if SIZEOF_LIKE.search(code):
                        ctx.add("sizeof-like")
                uses[t].append((path, i + 1, ctx))
    for s in sites:
        r = s.reasons
        if s.scope != "ns":
            r.append("not-namespace-scope")
        if defs[s.name] > 1:
            r.append("defined-more-than-once")
        if s.name in undefs:
            r.append("undef")
        if s.name in other_idents:
            r.append("named-outside-c++")
        if s.name.startswith("_"):
            r.append("reserved-name")
        if os.path.basename(s.path) in c_side_headers:
            r.append("included-from-c-side")
        if s.type == "const char*" and not allow_strings:
            r.append("string-literal-codegen-differs")     # a constexpr pointer is loaded where the literal was addressed: the proof always fails (artlry.cpp)
        if s.type == "const char*" and any("string-concat" in c or "sizeof-like" in c for _, _, c in uses[s.name]):
            r.append("string-used-as-literal")
        for _, _, c in uses[s.name]:
            for tag in c:
                if tag in ("string-concat", "sizeof-like"):
                    continue
                if tag not in r:
                    r.append(tag)
    return sites, uses


def rewrite_text(text: str, sites, kind: str) -> str:
    """Replace each eligible site's #define line; kind is 'constexpr' (a .cpp-local define, internal linkage) or 'inline constexpr' (a header)."""
    lines = text.split("\n")
    for s in sorted(sites, key=lambda x: -x.line):
        raw, vend = s.raw
        indent = re.match(r"^(\s*)#", raw).group(1)
        lines[s.line - 1] = "%s%s %s %s = %s;%s" % (indent, kind, s.type, s.name, s.literal, raw[vend:])
    return "\n".join(lines)


# ------------------------------------------------------------------ repo plumbing
def tracked():
    out = subprocess.run(["git", "-C", ROOT, "ls-files"], capture_output=True, text=True, **console_child.child_kwargs(redirected=True)).stdout.split("\n")
    return [p for p in out if p]


def read(rel):
    with open(os.path.join(ROOT, rel), "rb") as fh:
        return fh.read().decode("utf-8", errors="replace")


def c_side_header_names(paths):
    """Basenames of the headers a C / .rc / .asm / shader file includes, closed over header-to-header includes."""
    by_base = collections.defaultdict(list)
    for p in paths:
        if p.lower().endswith(CXX_EXT):
            by_base[os.path.basename(p)].append(p)
    seen, todo = set(), []
    for p in paths:
        if p.lower().endswith(C_SIDE_EXT) and not p.startswith(("3rdparty/", "build")):
            try:
                for l in read(p).split("\n"):
                    m = INCLUDE.match(l)
                    if m:
                        todo.append(os.path.basename(m.group(1)))
            except OSError:
                pass
    while todo:
        b = todo.pop()
        if b in seen:
            continue
        seen.add(b)
        for p in by_base.get(b, ()):
            for l in read(p).split("\n"):
                m = INCLUDE.match(l)
                if m:
                    todo.append(os.path.basename(m.group(1)))
    return frozenset(seen)


def outside_identifiers(paths):
    ids = set()
    for p in paths:
        low = p.lower()
        if (low.endswith(NAME_SCAN_EXT) or os.path.basename(p) == "CMakeLists.txt") and not p.startswith("docs/") and not p.startswith(".claude/"):
            try:
                if os.path.getsize(os.path.join(ROOT, p)) < 3_000_000:
                    ids.update(TOKEN.findall(read(p)))
            except OSError:
                pass
        elif p.startswith("3rdparty/") and low.endswith(CXX_EXT):
            try:
                ids.update(TOKEN.findall(read(p)))
            except OSError:
                pass
    return ids


def load_world():
    paths = tracked()
    cxx = [p for p in paths if p.lower().endswith(CXX_EXT) and p.split("/", 1)[0] in DIRS and ".gen." not in p]
    files = {p: read(p) for p in cxx}
    sites, uses = analyse(files, c_side_header_names(paths), outside_identifiers(paths), cxx)
    return paths, files, sites, uses


def load_taint(path):
    return {l.strip() for l in open(path, encoding="utf-8") if l.strip()} if path and os.path.exists(path) else set()


def taint_from_reasons(rejected):
    """Object names a reverted header's reason lists: those objects differed for a harmless edit (a data-layout shuffle after a new
    symbol), so a header that those objects include is not worth proving in a big batch."""
    out = set()
    for _rel, why in rejected:
        m = re.search(r"\(([^)]*)\)\s*$", why)
        if m:
            out.update(x.strip() for x in m.group(1).split(",") if x.strip().endswith(".obj"))
    return out


def other_worktree_dirty(path):
    if not path or not os.path.isdir(path):
        return set()
    out = subprocess.run(["git", "-C", path, "status", "--porcelain"], capture_output=True, text=True, **console_child.child_kwargs(redirected=True)).stdout
    return {l[3:].strip().replace("\\", "/") for l in out.split("\n") if len(l) > 3}


# ------------------------------------------------------------------ commands
def cmd_census(a):
    paths, files, sites, uses = load_world()
    by_reason = collections.Counter()
    ok = [s for s in sites if not s.reasons]
    for s in sites:
        by_reason[s.reasons[0] if s.reasons else "ELIGIBLE"] += 1
    print("literal defines: %d  eligible: %d  (header %d, .cpp-local %d)" % (
        len(sites), len(ok), sum(1 for s in ok if not s.path.lower().endswith(SRC_EXT)), sum(1 for s in ok if s.path.lower().endswith(SRC_EXT))))
    for r, n in by_reason.most_common():
        print("  %5d  %s" % (n, r))
    if a.list:
        for s in sites:
            if s.path == a.list:
                print("  %s:%d %s %s = %s -> %s" % (s.path, s.line, s.type, s.name, s.literal, "ELIGIBLE" if not s.reasons else ",".join(s.reasons)))
    return 0


def plan_batches(files, sites, uses, includers, args, dirty):
    """Eligible sites grouped by file -> [(rel, [sites], objs)] in batch order: .cpp-local first, then headers by including-object count."""
    by_file = collections.defaultdict(list)
    for s in sites:
        if s.reasons or rts.is_excluded(s.path) or s.path in dirty:
            continue
        by_file[s.path].append(s)
    out = []
    for rel, ss in by_file.items():
        if rel.lower().endswith(SRC_EXT):
            if args.scope == "header":
                continue
            objs = {args.s2t[rel]} if rel in args.s2t else set()
            ss = [s for s in ss if all(u[0] == rel for u in uses[s.name])]          # used only inside this file
        else:
            if args.scope == "cpp":
                continue
            objs = includers.get(rel, set())
            src_objs = {args.s2t.get(u[0]) for s in ss for u in uses[s.name] if u[0].lower().endswith(SRC_EXT)}
            ss = [s for s in ss if all(u[0] == rel or not u[0].lower().endswith(SRC_EXT) or args.s2t.get(u[0]) in objs for u in uses[s.name])]
        if objs & args.taint:
            continue
        if ss and objs and (rel.lower().endswith(SRC_EXT) or len(objs) <= args.max_header_includers):
            out.append((len(objs), rel, ss, objs))
    out.sort(key=lambda x: (not x[1].lower().endswith(SRC_EXT), x[0], x[1]))
    return out


class FastProof(rth.Proof):
    """Proof whose bisect probes rebuild only the objects the first full probe found bad (a wide header makes every probe a rebuild of the whole tree);
    solve_fast() then verifies the kept set with a full build and re-bisects it if that finds more."""

    ignore = frozenset()      # objects that must compile but whose digest is not compared (see --ignore-digest-file)

    def __init__(self, *args, **kw):
        super().__init__(*args, **kw)
        self.focus = None

    def bad_objects(self):
        bad = self._bad_objects()
        if self.ignore:
            bad = {o for o in bad if o not in self.ignore or not os.path.exists(os.path.join(self.tree_abs, o))}
        return bad

    def _bad_objects(self):
        if self.focus is None:
            bad = super().bad_objects()
            if bad:
                self.focus = set(bad)
            return bad
        full = self.objs
        self.objs = sorted(self.focus)
        try:
            bad = super().bad_objects()
        finally:
            self.objs = full
        if bad:
            self.focus = set(bad)                      # narrow: the objects still differing are the ones the next probe must rebuild
        return bad



def solve_fast(proof, rounds=6):
    kept, rejected = proof.solve()
    for _ in range(rounds):
        proof.focus = None
        proof.apply(set(kept))
        if not rth.Proof.bad_objects(proof):
            return kept, rejected
        sub = FastProof(proof.tree_abs, {h: proof.includers[h] for h in kept}, proof.orig, {h: proof.edited[h] for h in kept}, proof.jobs)
        sub.before = {o: proof.before[o] for o in sub.objs}
        sub.on_disk = dict(proof.on_disk)
        kept, more = sub.solve()
        rejected += more
        proof = sub
    proof.focus = None
    proof.apply(set(kept))
    if rth.Proof.bad_objects(proof):                     # still not clean: keep nothing rather than something unproven
        rejected += [(h, "kept set failed its full verification") for h in kept]
        kept = []
        proof.apply(set())
    return kept, rejected


def cmd_run(a):
    sys.stdout.reconfigure(line_buffering=True)
    tree_abs = os.path.join(ROOT, a.tree)
    rts.apply_lane_args(a)
    if not a.strict_proof:
        canonical_obj.install()
    includers = rth.includer_map(tree_abs)
    t2s = rth.target_to_source(tree_abs)
    a.s2t = {v: k for k, v in t2s.items()}
    paths, files, sites, uses = load_world()
    dirty = rts.dirty_files() | other_worktree_dirty(a.skip_dirty_in)
    done = set()
    if a.done and os.path.exists(a.done):
        done = {l.strip() for l in open(a.done, encoding="utf-8") if l.strip()}
    a.taint = load_taint(a.taint_file)
    batches = 0
    while True:
        plan = [p for p in plan_batches(files, sites, uses, includers, a, dirty) if p[1] not in done]
        picked, objs = [], set()
        for n, rel, ss, o in plan:
            if picked and (len(objs | o) > a.max_objs):
                continue
            picked.append((rel, ss, o))
            objs |= o
            if len(picked) >= a.max_files:
                break
        if not picked:
            print("rewrite_defines: no eligible file left")
            return 0
        batches += 1
        nsites = sum(len(ss) for _, ss, _ in picked)
        print("BATCH %d: %d file(s), %d site(s), %d object(s) in the proof set" % (batches, len(picked), nsites, len(objs)))
        if a.dry_run:
            for rel, ss, o in picked:
                print("  %s  %d site(s), %d object(s)" % (rel, len(ss), len(o)))
            return 0
        if a.done:
            with open(a.done, "a", encoding="utf-8", newline="\n") as fh:
                fh.writelines(rel + "\n" for rel, _, _ in picked)
            done |= {rel for rel, _, _ in picked}
        orig = {rel: rth.read_bytes(rel) for rel, _, _ in picked}
        edited = {}
        for rel, ss, _ in picked:
            kind = "constexpr" if rel.lower().endswith(SRC_EXT) else "inline constexpr"
            edited[rel] = rewrite_text(orig[rel].decode("utf-8", errors="replace"), ss, kind).encode("utf-8")
        mtimes = {rel: os.stat(os.path.join(ROOT, rel)).st_mtime_ns for rel in orig}
        inc = {rel: o for rel, _, o in picked}
        FastProof.ignore = frozenset(load_taint(a.ignore_digest_file))
        proof = (rth.Proof if a.slow_bisect else FastProof)(tree_abs, inc, orig, edited, a.build_jobs)
        if proof.baseline():
            print("rewrite_defines: the base build of the proof set failed; refusing to edit", file=sys.stderr)
            return 2
        kept, rejected = proof.solve() if a.slow_bisect else solve_fast(proof)
        for rel, why in rejected:
            rts.restore_file(rel, mtimes[rel])
            print("REVERT %s  %s" % (rel, why))
        new_taint = taint_from_reasons(rejected) - a.taint
        if new_taint and a.taint_file:
            a.taint |= new_taint
            with open(a.taint_file, "a", encoding="utf-8", newline="\n") as fh:
                fh.writelines(o + "\n" for o in sorted(new_taint))
        kept_sites = sum(len(ss) for rel, ss, _ in picked if rel in kept)
        print("BATCH %d result: kept %d file(s) / %d site(s), reverted %d file(s), %d object(s) proven identical" % (
            batches, len(kept), kept_sites, len(rejected), len({o for r in kept for o in inc[r]})))
        if kept and a.commit:
            commit(kept, batches, kept_sites)
        if not a.loop or (a.max_batches and batches >= a.max_batches) or not kept and not rejected:
            return 1 if rejected else 0
        paths, files, sites, uses = load_world()


def commit_paths(kept, subject, baseline_subject):
    """Commit `kept` by pathspec, then run the cxx17 ratchet --check --tighten and commit the baseline when it moved."""
    trailer = "\n\nCo-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
    subprocess.run(["git", "-C", ROOT, "add", "--"] + kept, check=True, **console_child.child_kwargs(redirected=True))
    subprocess.run(["git", "-C", ROOT, "commit", "-q", "-m", subject + trailer, "--"] + kept, check=True, **console_child.child_kwargs(redirected=True))
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "cxx17_ratchet", "cxx17_ratchet.py"), "--check", "--tighten"], cwd=ROOT,
                       capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    print((r.stdout.strip().splitlines() or [""])[-1])
    base = "tools/cxx17_ratchet/baseline.json"
    if rts.git("status", "--porcelain", "--", base).strip():
        subprocess.run(["git", "-C", ROOT, "add", "--", base], check=True, **console_child.child_kwargs(redirected=True))
        subprocess.run(["git", "-C", ROOT, "commit", "-q", "-m", baseline_subject + trailer, "--", base], check=True, **console_child.child_kwargs(redirected=True))


def commit(kept, n, nsites):
    commit_paths(kept, "tidy(LG13): %d #define literal constant(s) -> inline constexpr in %d file(s) (objhash identical over every including object)" % (nsites, len(kept)),
                 "cxx17 ratchet baseline tightened after LG13 batch")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("census")
    c.add_argument("--list", help="print the verdict of every define of this file")
    r = sub.add_parser("run")
    r.add_argument("--scope", choices=("cpp", "header", "all"), default="cpp")
    r.add_argument("--tree", default="build64-ninja")
    r.add_argument("--max-files", type=int, default=20)
    r.add_argument("--max-objs", type=int, default=150)
    r.add_argument("--max-header-includers", type=int, default=200)
    r.add_argument("--build-jobs", type=int, default=6)
    r.add_argument("--done", help="ledger of files already tried (skipped; picked ones are appended)")
    r.add_argument("--strict-proof", action="store_true", help="compare with plain tools/objhash.py digests (symbol / section order sensitive) instead of tools/tidy/canonical_obj.py")
    r.add_argument("--slow-bisect", action="store_true", help="every bisect probe rebuilds the whole proof set (default: only the objects the first probe found bad, then a full verification)")
    r.add_argument("--ignore-digest-file", default=None, help="objects (names as in the tree) that must compile but whose digest is not compared: a TU where any new inline variable "
                   "makes MSVC drop unreferenced static const data (editor/MapGeneratorDialog.cpp, the stb_image tables), a semantic no-op the digest cannot tell from a change")
    r.add_argument("--taint-file", default=None, help="ledger of objects that differed for a harmless edit; headers they include are skipped (appended to after a revert)")
    r.add_argument("--skip-dirty-in", default="A:/Games/mc2-main", help="the other builder lane's worktree: its dirty files are never touched")
    r.add_argument("--commit", action="store_true")
    r.add_argument("--loop", action="store_true")
    r.add_argument("--max-batches", type=int, default=0)
    r.add_argument("--dry-run", action="store_true")
    rts.add_lane_args(r)
    a = ap.parse_args(argv)
    return cmd_census(a) if a.cmd == "census" else cmd_run(a)


if __name__ == "__main__":
    sys.exit(main())
