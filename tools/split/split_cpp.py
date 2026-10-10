#!/usr/bin/env python3
"""What: splits one oversized C++ file into <stem>_<responsibility>.cpp pieces plus a generated <stem>_internal.h, driven by a config module.
Why: the file-size program moves whole top-level units verbatim (CRLF/LF bytes kept); the header carries the verbatim include bundle,
the externs of the preamble globals and prototypes of shared free functions, so the split compiles with no logic change.
Usage: py -3 tools/split/split_cpp.py CONFIG.py   (writes <stem>_*.cpp, <stem>_internal.h next to the source and <src>.new; the caller moves .new over the source)
Config names: SRC, STEM, INCLUDE_RAW=(first,last) 1-based raw lines copied into the header, PREAMBLE_END (first post-preamble line),
KEEP_FROM (first preamble line the core keeps), GROUPS=[(name,[(a,b)...])] with a 'core' group, DOC={name:text},
PREAMBLE_MOVES={group:[(a,b)...]} (preamble line ranges moved to the top of a group file), HEADER_EXTRA=[lines], BANNER=[lines].
Optional: CORE_ONLY=[(a,b)...] (bundle lines kept out of the header and emitted into the core file: a header that DEFINES variables may be included by one TU only), BASE_REV / BASE_SRC (git rev and path holding the original, default HEAD and SRC) and ALLOW (see split_verify.py) drive the fail-closed checks that run
after the pieces are written; any finding exits 1 and the caller must not install the .new file. OUT (output dir, default the source's directory), HEADER_PRE_MOVES / HEADER_MOVES=[(a,b)...] (1-based line ranges whose units move verbatim into the
header, before / after the externs: macros and inline definitions every piece needs), HEADER_PRE=[lines]."""
import importlib.util, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cpp_units

spec = importlib.util.spec_from_file_location("cfg", sys.argv[1])
C = importlib.util.module_from_spec(spec)
spec.loader.exec_module(C)

lines, units = cpp_units.parse(C.SRC)
raw = open(C.SRC, "rb").read().split(b"\n")


def chunk(u):
    return b"\n".join(raw[u["start"]:u["end"] + 1])


def text(u):
    return chunk(u).decode("utf-8", "replace").replace("\r", "")


OUT = getattr(C, "OUT", os.path.dirname(C.SRC) or ".")
PRE_MOVES = getattr(C, "HEADER_PRE_MOVES", [])
POST_MOVES = getattr(C, "HEADER_MOVES", [])


def in_ranges(n, ranges):
    return any(a <= n <= b for a, b in ranges)


def in_header(n):
    return in_ranges(n, PRE_MOVES) or in_ranges(n, POST_MOVES)


def moved_text(ranges):
    out = []
    for u in units:
        if in_ranges(u["start"] + 1, ranges) and u["kind"] != "tail":
            out += text(u).split("\n")
    return out


def group_of(n):
    for name, ranges in C.GROUPS:
        for a, b in ranges:
            if a <= n <= b:
                return name
    return None


def moved_group(n):
    for g, ranges in getattr(C, "PREAMBLE_MOVES", {}).items():
        for a, b in ranges:
            if a <= n <= b:
                return g
    return None


# ---- header ---------------------------------------------------------------------------------------------
G = C.STEM.upper() + "_INTERNAL_H"
hdr = list(C.BANNER) + ["#ifndef " + G, "#define " + G, ""]
a, b = C.INCLUDE_RAW
# include groups are copied verbatim: some guards skip a header that no longer exists, so they must stay guards
CORE_ONLY = getattr(C, "CORE_ONLY", [])
hdr += [l.decode("utf-8", "replace").rstrip(chr(13)) for i, l in enumerate(raw[a - 1:b], start=a)
        if not in_ranges(i, CORE_ONLY)]
hdr += list(getattr(C, "HEADER_PRE", []))
hdr += moved_text(PRE_MOVES)
hdr += ["", "// Globals defined in %s.cpp (or elsewhere in the engine) that the split pieces share." % C.STEM]

externs = []
for u in units:
    n = u["start"] + 1
    if not (C.KEEP_FROM <= n < C.PREAMBLE_END) or u["kind"] != "decl" or moved_group(n) or in_header(n):
        continue
    t = text(u)
    body = [l for l in t.split("\n") if l.strip() and not l.strip().startswith("//")]
    if not body:
        continue
    joined = "\n".join(body)
    if re.search(r"^\s*#", joined, re.M):
        continue
    if joined.lstrip().startswith(("static", "class", "struct", "typedef", "enum", "union", "namespace", "/*")):
        continue
    if "::" in joined.split("=")[0].split("(")[0] and not joined.lstrip().startswith("extern"):
        continue
    if joined.lstrip().startswith("extern"):
        externs.append(re.sub(r"\s*//.*$", "", joined.strip(), flags=re.M))
        continue
    depth = 0
    cut = None
    for i, ch in enumerate(joined):
        if ch in "[(":
            depth += 1
        elif ch in "])":
            depth -= 1
        elif ch == "=" and depth == 0:
            cut = i
            break
    head = joined[:cut] if cut is not None else joined.rstrip().rstrip(";")
    head = re.sub(r"\s*//.*$", "", head, flags=re.M)
    head = re.sub(r"\s+", " ", head).strip()
    if head.startswith("const ") or not head:
        continue
    externs.append("extern " + head + ";")
seen = set()
for e in externs:
    k = re.sub(r"\s+", " ", e)
    if k not in seen:
        seen.add(k)
        hdr.append(e)
hdr += list(C.HEADER_EXTRA)
hdr += moved_text(POST_MOVES)
hdr.append("")


def proto(u):
    body = [l.rstrip("\r") for l in lines[u["start"]:u["end"] + 1]]
    i = 0
    while i < len(body) and (not body[i].strip() or body[i].lstrip().startswith("//")):
        i += 1
    sig = []
    while i < len(body):
        t = body[i]
        sig.append(t.split("{")[0].split("//")[0])
        if "{" in t:
            break
        i += 1
    return re.sub(r"\s+", " ", " ".join(sig)).strip()


protos = []
for u in units:
    n = u["start"] + 1
    if u["kind"] != "func" or n < C.PREAMBLE_END or group_of(n) in (None, "core"):
        continue
    s = proto(u)
    if "::" in s.split("(")[0] or s.startswith(("static", "inline", "#")):
        continue
    if re.match(r"^(if|for|while|switch|return|[A-Z][A-Z_]+_MAP\w*)", s):
        continue
    if re.match(r"^[\w\s\*&:<>]+\(", s):
        protos.append(s.rstrip() + ";")
if protos:
    hdr.append("// Free functions defined in the split pieces.")
    hdr += protos
    hdr.append("")
hdr.append("#endif // " + G)
hdr.append("")
open(os.path.join(OUT, "%s_internal.h" % C.STEM), "wb").write("\r\n".join(hdr).encode("utf-8"))

# ---- groups ---------------------------------------------------------------------------------------------
bodies = {g: [] for g, _ in C.GROUPS}
pre_moves = {g: [] for g in getattr(C, "PREAMBLE_MOVES", {})}
core_pre = []
for u in units:
    n = u["start"] + 1
    if u["kind"] == "tail" or in_header(n):
        continue
    if n < C.PREAMBLE_END:
        if n < C.KEEP_FROM:
            continue
        mg = moved_group(n)
        if mg:
            pre_moves[mg].append(chunk(u))
        else:
            core_pre.append((u, chunk(u)))
        continue
    g = group_of(n)
    if g is None:
        raise SystemExit("unassigned unit at %d (%s)" % (n, u["name"]))
    bodies[g].append(chunk(u))

norm = lambda s: re.sub(r"\s+", " ", re.sub(r"//.*$", "", s, flags=re.M)).strip()
hn = set(norm(l) for l in hdr if l.strip())
core_keep = []
for u, c in core_pre:
    body = c.decode("utf-8", "replace")
    nb = norm(body)
    if u["kind"] == "decl" and nb.startswith("extern") and nb in hn and "#" not in body:
        continue
    core_keep.append(c)

core_head = b"\n".join(raw[0:C.INCLUDE_RAW[0] - 1]).rstrip() + b"\r\n\r\n#include \"%s_internal.h\"\r\n\r\n" % C.STEM.encode()
if CORE_ONLY:
    core_head += b"\n".join(b"\n".join(raw[x - 1:y]) for x, y in CORE_ONLY) + b"\n\n"
open(C.SRC + ".new", "wb").write(core_head + b"\n".join(core_keep) + b"\n" + b"\n".join(bodies["core"]) + b"\n")
for g, _ in C.GROUPS:
    if g == "core":
        continue
    head = ("// %s: %s.\r\n// Declared in the class headers; shared names in %s_internal.h.\r\n#include \"%s_internal.h\"\r\n\r\n"
            % (C.TITLE, C.DOC[g], C.STEM, C.STEM)).encode("utf-8")
    pm = (b"\n".join(pre_moves.get(g, [])) + b"\n") if pre_moves.get(g) else b""
    open(os.path.join(OUT, "%s_%s.cpp" % (C.STEM, g)), "wb").write(head + pm + b"\n".join(bodies[g]) + b"\n")
print("externs", len(seen), "protos", len(protos))
for g, _ in C.GROUPS:
    print(g, sum(c.count(b"\n") + 1 for c in bodies[g]))

# ---- fail-closed verification against the original (tools/split/split_verify.py) ------------------------------
import split_verify
_old = split_verify._git(["show", "%s:%s" % (getattr(C, "BASE_REV", "HEAD"), getattr(C, "BASE_SRC", C.SRC).replace(chr(92), "/"))])
if not _old:
    raise SystemExit("split_verify: cannot read %s from %s" % (C.SRC, getattr(C, "BASE_REV", "HEAD")))
_files = {C.SRC: open(C.SRC + ".new", "rb").read(),
          os.path.join(OUT, "%s_internal.h" % C.STEM): open(os.path.join(OUT, "%s_internal.h" % C.STEM), "rb").read()}
for g, _ in C.GROUPS:
    if g != "core":
        _p = os.path.join(OUT, "%s_%s.cpp" % (C.STEM, g))
        _files[_p] = open(_p, "rb").read()
sys.exit(split_verify.report(split_verify.verify(_old, _files, C.STEM, getattr(C, "BASE_SRC", C.SRC).replace(chr(92), "/"), getattr(C, "ALLOW", {}))))
