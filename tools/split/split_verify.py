#!/usr/bin/env python3
"""What: fail-closed checks that a file split moved code without changing it: every old line survives byte-for-byte, linkage and
init form are unchanged, hoisted includes and macros still precede their users, headers hold no per-TU state, inline functions are
not split from their callers, static registrars keep their order, and the scripts that name the old path are listed.
Why: a split is "move only". Past slices silently turned an eager file-scope gate cache into a lazy local static, lost a UTF-8 dash
in a log string, and hoisted a mid-file include above the macro it needs; reviewers could not see it in a 9k-line diff.
Usage (library): split_cpp.py calls verify() after writing the pieces; a finding exits non-zero, nothing is installed.
Usage (retro):   py -3 tools/split/split_verify.py --base REV --src code/mech.cpp --stem mech [--out-dir code] [--allow ALLOW.json]
                 [--run-selftests]   (REV = the commit before the split; the pieces are read from the worktree)
                 [--at SPLIT_COMMIT]  read the pieces from the split commit instead of the worktree: a retro run pinned to the split
                 cannot be failed by later edits to the pieces (the script-name scan is skipped, it asks about the current tree)
ALLOW (config dict / JSON): {"edits": [[old_line, new_line], ...]   intentional line rewrites (both sides allowed),
                             "drop": [old_line, ...]                 old lines intentionally removed,
                             "add": [new_line, ...]                  new lines intentionally added,
                             "header_vars": [name, ...]              mutable/non-inline variables allowed in the header,
                             "inline_calls": [name, ...]             inline functions allowed to be called across pieces,
                             "static_calls": [name, ...]             internal-linkage functions reviewed as safe to be called from another piece,
                             "scripts": [path, ...]                  scripts that name the old path and were reviewed,
                             "includes": [header, ...]               hoisted includes reviewed against earlier macros,
                             "exclude_files": [basename, ...]        stem_*.cpp files that are not pieces of this split (retro mode)}"""
import argparse, collections, json, os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts"))
import console_child  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
REGISTRARS = [r"ABLi_addFunction\s*\(", r"\bREGISTER_\w+", r"AutoRegister", r"\bstatic\s+\w*Registrar\b", r"\bFeatureRegistry\b"]


def _git(args):
    r = subprocess.run(["git"] + args, cwd=REPO, capture_output=True, **console_child.child_kwargs(redirected=True))
    return r.stdout if r.returncode == 0 else b""


def lines_of(data):
    return [l.rstrip(b"\r") for l in data.split(b"\n")]


def strip_code(line):
    """line without string/char literals and // comments, for brace and token scans."""
    out, i, n = [], 0, len(line)
    while i < n:
        c = line[i:i + 1]
        if c in (b'"', b"'"):
            j = i + 1
            while j < n and line[j:j + 1] != c:
                j += 2 if line[j:j + 1] == b"\\" else 1
            i = j + 1
        elif line[i:i + 2] == b"//":
            break
        else:
            out.append(c); i += 1
    return b"".join(out)


def top_level(lines):
    """(index, line) for every line that starts at brace depth 0 (block comments are not tracked; fine for these sources)."""
    depth, out = 0, []
    for i, l in enumerate(lines):
        if depth == 0:
            out.append((i, l))
        code = strip_code(l)
        depth += code.count(b"{") - code.count(b"}")
        if depth < 0:
            depth = 0
    return out


def norm(l):
    s = re.sub(rb"\s+", b" ", l.strip())
    if s.startswith(b"extern "):
        s = re.sub(rb"\s*//.*$", b"", s)   # the splitter drops trailing comments from the externs it generates
    return s


def _is_generated(l, stem, in_header):
    s = l.strip()
    if not s or s.startswith(b"//") or s.startswith(b"/*") or s.startswith(b"*"):
        return True
    if (s.startswith(b'#include "') and s.endswith(b'_internal.h"') and s[10:].startswith(stem.encode() + b"_")) \
            or (s.startswith((b"#ifndef ", b"#define ")) and s.endswith(b"_INTERNAL_H") and s.split(b" ")[1].startswith(stem.upper().encode() + b"_")) \
            or s.startswith(b"#endif // " + stem.upper().encode()):
        return True
    if in_header and (s.startswith(b"extern ") or s.endswith(b");") or s.startswith(b"struct ") or s == b"};" or s == b"{" or s == b"}"):
        return True
    return False


def check_lines(old, new_by_file, stem, allow):
    f = []
    edits = [(norm(a.encode()), norm(b.encode())) for a, b in allow.get("edits", [])]
    drop = set(norm(x.encode()) for x in allow.get("drop", []))
    add = set(norm(x.encode()) for x in allow.get("add", []))
    old_c = collections.Counter(norm(l) for l in old if l.strip())
    new_c = collections.Counter()      # lines that must be explained by the original
    new_all = collections.Counter()    # every line, generated ones included (they can satisfy an old line)
    where = {}
    for name, ls in new_by_file.items():
        for l in ls:
            if not l.strip():
                continue
            n = norm(l)
            new_all[n] += 1
            if not _is_generated(l, stem, name.endswith("_internal.h")):
                new_c[n] += 1; where.setdefault(n, name)
    # file-scope declarations repeated in the original are de-duplicated into the header: presence is enough
    for l in list(old_c):
        if l.startswith(b"extern ") and new_all[l]:
            old_c[l] = 0
        elif l.startswith(b"extern ") is False and new_all[l] > new_c[l] and new_c[l] < old_c[l]:
            old_c[l] = max(0, old_c[l] - (new_all[l] - new_c[l]))
    for a, b in edits:
        old_c[a] = max(0, old_c[a] - 1); new_c[b] = max(0, new_c[b] - 1)
    for l in drop:      # a listed line may be missing from the new files, any number of times
        old_c[l] = min(old_c[l], new_c[l])
    for l in add:       # a listed line may appear beyond its original count
        new_c[l] = min(new_c[l], old_c[l])
    for l, n in (old_c - new_c).items():
        if l.startswith(b"//") or l.startswith(b"/*") or l.startswith(b"*"):
            continue   # comment edits are policy-allowed (comment de-bloat rides along with every move)
        kind = "linkage/init-form" if (b"static" in l or b"getenv" in l or b"inline" in l or b"extern" in l) else "line changed"
        f.append(("lines", "%s: old line missing from the new files: %s" % (kind, l[:140].decode("utf-8", "replace"))))
    for l, n in (new_c - old_c).items():
        if l.startswith(b"//") or l.startswith(b"/*") or l.startswith(b"*") or l.startswith(b"extern "):
            continue
        kind = "linkage/init-form" if (b"static" in l or b"inline" in l or b"getenv" in l) else "line added"
        f.append(("lines", "%s: new line not in the original (%s): %s" % (kind, where.get(l, "?"), l[:140].decode("utf-8", "replace"))))
    return f


def check_includes(old, header_lines, allow, src_dir):
    f = []
    ok = set(allow.get("includes", []))
    hdr = set(norm(l) for l in header_lines)
    defined, first_def = [], None
    for i, l in enumerate(old):
        m = re.match(rb"\s*#\s*define\s+(\w+)", l)
        if m:
            defined.append((i, m.group(1).decode()))
        m = re.match(rb'\s*#\s*include\s*[<"]([^>"]+)[>"]', l)
        if m and norm(l) in hdr:
            before = [n for j, n in defined if j < i]
            if not before:
                continue
            inc = m.group(1).decode()
            if os.path.basename(inc) in ok or inc in ok:
                continue
            text = _find_header_text(inc, src_dir)
            if text is None:
                continue
            hit = [n for n in before if re.search(rb"\b%s\b" % n.encode(), text)]
            if hit:
                f.append(("includes", "include %s sits in the header but the original defined %s before it; the header may read them" % (inc, ", ".join(hit[:6]))))
    return f


def _find_header_text(inc, src_dir):
    cand = os.path.join(REPO, src_dir, inc)
    if os.path.isfile(cand):
        return open(cand, "rb").read()
    base = os.path.basename(inc)
    out = _git(["ls-files", "--", "*/" + base, base]).decode("utf-8", "replace").split()
    for p in out:
        if p.replace("\\", "/").endswith(inc.replace("\\", "/")) or os.path.basename(p) == base:
            return open(os.path.join(REPO, p), "rb").read()
    return None


def check_macros(old, new_by_file, header_name):
    f = []
    defs = {}
    for i, l in enumerate(old):
        m = re.match(rb"\s*#\s*define\s+(\w+)", l)
        if m:
            defs.setdefault(m.group(1).decode(), i)
    if not defs:
        return f
    uses_old = {}
    for i, l in enumerate(old):
        m = re.match(rb"\s*#\s*(?:if|ifdef|ifndef|elif)\b(.*)", l)
        if m:
            for n in re.findall(rb"\w+", m.group(1)):
                n = n.decode()
                if n in defs:
                    uses_old.setdefault(n, i)
    hdr_text = b"\n".join(new_by_file.get(header_name, []))
    for name, ls in new_by_file.items():
        base = b"" if name == header_name else hdr_text
        seen = base
        for l in ls:
            m = re.match(rb"\s*#\s*(?:if|ifdef|ifndef|elif)\b(.*)", l)
            if m:
                for n in re.findall(rb"\w+", m.group(1)):
                    n = n.decode()
                    if n in defs and n in uses_old and defs[n] < uses_old[n] \
                            and not re.search(rb"#\s*define\s+%s\b" % n.encode(), seen):
                        f.append(("macros", "%s tests %s but its #define (before the first use in the original) is not earlier in this piece or the header" % (name, n)))
            seen += b"\n" + l
    return f


def check_header(header_lines, allow):
    f = []
    okv = set(allow.get("header_vars", []))
    for i, l in top_level(header_lines):
        code = strip_code(l).strip()
        if not code or code.startswith(b"#"):
            continue
        # a variable has no '(' before any '='
        head = code.split(b"=")[0]
        if b"(" in head:
            continue
        name = re.findall(rb"(\w+)\s*(?:\[[^\]]*\])*\s*(?:=|;|$)", head.rstrip(b" ;"))
        nm = name[-1].decode() if name else "?"
        if nm in okv:
            continue
        if re.match(rb"static\b(?!\s+(?:inline|constexpr)\b)", code):
            f.append(("header", "non-inline static variable in the internal header (one copy per piece): %s" % code[:100].decode("utf-8", "replace")))
        elif re.match(rb"inline\b", code) and b"const" not in code and not code.startswith(b"inline namespace"):
            f.append(("header", "mutable inline variable in the internal header (use inline constexpr or list it in header_vars): %s" % code[:100].decode("utf-8", "replace")))
    return f


def check_inline_calls(new_by_file, header_name, allow):
    f = []
    okc = set(allow.get("inline_calls", []))
    rx = re.compile(rb"\s*(?:static\s+)?inline\s+[^;{=]*?\b(?:\w+::)?(\w+)\s*\(")
    rx_any = re.compile(rb"\s*(?:[\w:<>\*&]+\s+)+(?:\w+::)?(\w+)\s*\(")
    defs = collections.defaultdict(set)    # unqualified name -> files that define something of that name
    inline_in = {}
    for name, ls in new_by_file.items():
        for i, l in top_level(ls):
            m = rx_any.match(l)
            if m and not l.lstrip().startswith((b"return", b"#", b"//")):
                defs[m.group(1).decode()].add(name)
            m = rx.match(l)
            if m and name != header_name:
                inline_in.setdefault(m.group(1).decode(), name)
    for nm, owner in inline_in.items():
        if nm in okc or len(defs[nm]) > 1:
            continue    # a same-named function elsewhere (another class) makes the call sites ambiguous; reviewed by name only
        pat = re.compile(rb"\b%s\s*\(" % nm.encode())
        for name, ls in new_by_file.items():
            if name == owner:
                continue
            if any(pat.search(strip_code(l)) for l in ls):
                f.append(("inline", "inline function %s is defined in %s and called from %s (an inline body must be visible in every TU that calls it)" % (nm, owner, name)))
                break
    return f


def static_functions(lines):
    """Names of the functions a piece defines with internal linkage: `static` at namespace scope, or any function inside an anonymous namespace."""
    rx = re.compile(rb"\s*(?:[\w:<>\*&,]+\s+)+\**&?(\w+)\s*\([^;]*$")
    names, depth, anon_depths = set(), 0, []
    for l in lines:
        code = strip_code(l)
        st = code.strip()
        top = depth == 0 or (anon_depths and depth == anon_depths[-1] + 1)
        if top and not st.startswith((b"#", b"return", b"//", b"namespace")):
            m = rx.match(code)
            if m and (re.match(rb"\s*static\b", code) and depth == 0 or (anon_depths and depth == anon_depths[-1] + 1)):
                if b";" not in code.split(b"(")[0] and not code.rstrip().endswith(b";"):
                    names.add(m.group(1).decode())
        if re.match(rb"\s*namespace\s*\{", code):
            anon_depths.append(depth)
        depth += code.count(b"{") - code.count(b"}")
        while anon_depths and depth <= anon_depths[-1]:
            anon_depths.pop()
        if depth < 0:
            depth = 0
    return names


def check_static_calls(new_by_file, header_name, allow):
    """A function with internal linkage (file-static or in an anonymous namespace) exists once per piece: a call from another piece does not link to it."""
    f = []
    okc = set(allow.get("static_calls", []))
    pieces = {n: ls for n, ls in new_by_file.items() if n != header_name and not n.endswith((".h", ".hpp"))}
    rx_def = re.compile(rb"\s*(?:[\w:<>\*&,]+\s+)+\**&?(?:\w+::)?(\w+)\s*\(")
    for owner, ls in pieces.items():
        for nm in sorted(static_functions(ls)):
            if nm in okc:
                continue
            pat = re.compile(rb"(?<![\w.>:])%s\s*\(" % nm.encode())
            for other, ols in pieces.items():
                if other == owner:
                    continue
                if any(rx_def.match(l) and rx_def.match(l).group(1).decode() == nm for _, l in top_level(ols)):
                    continue     # the other piece has its own function of that name
                if any(pat.search(strip_code(l)) and not l.lstrip().startswith((b"//", b"*", b"/*")) for l in ols):
                    f.append(("static", "function %s has internal linkage in %s but is called from %s (move it with its caller or make it external)" % (nm, owner, other)))
                    break
    return f


def check_registrars(old, new_by_file, patterns=REGISTRARS):
    f = []
    rx = re.compile("|".join(patterns).encode())
    old_seq = [norm(l) for i, l in top_level(old) if rx.search(strip_code(l))]
    if not old_seq:
        return f
    per_file = {}
    for name, ls in new_by_file.items():
        seq = [norm(l) for i, l in top_level(ls) if rx.search(strip_code(l))]
        if seq:
            per_file[name] = seq
    if len(per_file) != 1 or list(per_file.values())[0] != old_seq:
        f.append(("registrars", "file-scope registrar sequence changed or was spread over pieces (static-init order between TUs is unspecified): old %d, new %s" %
                  (len(old_seq), {k: len(v) for k, v in per_file.items()})))
    return f


def check_scripts(src, allow, run_selftests):
    f = []
    ok = set(allow.get("scripts", []))
    base = os.path.basename(src)
    pats = [src.replace("\\", "/"), base]
    hits = []
    out = _git(["grep", "-l", "-I", "-e", pats[0], "-e", re.escape(base).replace("\\.", ".")] + ["--", "scripts", "tools", "tests", "cmake", "CMakeLists.txt", ".github"])
    for p in out.decode("utf-8", "replace").split():
        if re.search(r"baseline|seed\.json|gate-history|\.md$|\.yaml$|flags\.auto|axis-conversion|(^|/)CMakeLists\.txt$|\.cmake$", p):
            continue
        hits.append(p)
    hits = [p for p in hits if not p.startswith("tools/split/")]
    for p in hits:
        if p in ok:
            continue
        f.append(("scripts", "%s names %s; widen any 'forbid/require in file X' rule to the stem*.cpp pieces, then list it in ALLOW['scripts']" % (p, src)))
    if run_selftests:
        for p in hits:
            full = os.path.join(REPO, p)
            txt = open(full, "rb").read()
            if b"--self-test" in txt or b"--selftest" in txt:
                flag = "--self-test" if b"--self-test" in txt else "--selftest"
                cmd = ([sys.executable, "-B", full] if p.endswith(".py") else ["sh", full]) + [flag]
                r = subprocess.run(cmd, cwd=REPO, capture_output=True, **console_child.child_kwargs(redirected=True))
                print("[split_verify] self-test %s -> rc=%d" % (p, r.returncode))
                if r.returncode != 0:
                    f.append(("scripts", "%s %s failed (rc=%d)" % (p, flag, r.returncode)))
    return f


def verify(old_bytes, new_files, stem, src, allow=None, run_selftests=False, check_script_names=True):
    """new_files: {display_name: bytes}; the header is the entry ending in _internal.h."""
    allow = allow or {}
    old = lines_of(old_bytes)
    new_by_file = {n: lines_of(b) for n, b in new_files.items()}
    header_name = next((n for n in new_by_file if n.endswith("_internal.h")), "")
    f = []
    f += check_lines(old, new_by_file, stem, allow)
    f += check_includes(old, new_by_file.get(header_name, []), allow, os.path.dirname(src))
    f += check_macros(old, new_by_file, header_name)
    f += check_header(new_by_file.get(header_name, []), allow)
    f += check_inline_calls(new_by_file, header_name, allow)
    f += check_static_calls(new_by_file, header_name, allow)
    f += check_registrars(old, new_by_file)
    if check_script_names:
        f += check_scripts(src, allow, run_selftests)
    return f


def report(findings):
    for c, m in findings:
        print(("[split_verify] %s: %s" % (c, m)).encode("ascii", "replace").decode())
    print("[split_verify] %s (%d finding(s))" % ("FAIL" if findings else "OK", len(findings)))
    return 1 if findings else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--src", required=True)
    ap.add_argument("--stem", required=True)
    ap.add_argument("--out-dir")
    ap.add_argument("--allow")
    ap.add_argument("--run-selftests", action="store_true")
    ap.add_argument("--at", help="read the pieces from this commit instead of the worktree (pin a retro run to the split commit; "
                                 "later edits to the pieces then cannot fail it; the script-name scan is skipped)")
    a = ap.parse_args()
    out = a.out_dir or os.path.dirname(a.src)
    old = _git(["show", "%s:%s" % (a.base, a.src.replace("\\", "/"))])
    if not old:
        raise SystemExit("cannot read %s:%s" % (a.base, a.src))
    if a.at:
        listing = _git(["ls-tree", "--name-only", a.at, out.replace("\\", "/") + "/"]).decode("utf-8", "replace").split()
        names = [x for x in listing if os.path.basename(x).startswith(a.stem + "_") and (x.endswith(".cpp") or x.endswith("_internal.h"))]
        files = {a.src: _git(["show", "%s:%s" % (a.at, a.src.replace("\\", "/"))])}
        for x in sorted(names):
            files[os.path.join(out, os.path.basename(x))] = _git(["show", "%s:%s" % (a.at, x)])
    else:
        files = {a.src: open(os.path.join(REPO, a.src), "rb").read()}
        for n in sorted(os.listdir(os.path.join(REPO, out))):
            if n.startswith(a.stem + "_") and (n.endswith(".cpp") or n.endswith("_internal.h")):
                files[os.path.join(out, n)] = open(os.path.join(REPO, out, n), "rb").read()
    allow = json.load(open(a.allow)) if a.allow else {}
    for n in list(files):
        if os.path.basename(n) in allow.get("exclude_files", []):
            del files[n]
    sys.exit(report(verify(old, files, a.stem, a.src.replace("\\", "/"), allow, a.run_selftests, check_script_names=not a.at)))


if __name__ == "__main__":
    main()
