#!/usr/bin/env python3
"""split_mover.py -- one-shot splitter for code/mover.cpp (giant-file-split-plan.md s11). Units move verbatim (per-line EOL
bytes kept); the generated mover_internal.h carries the include bundle, GOALMAP_CELL_DIM and externs/prototypes for what the
split pieces share."""
import re, sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "split"))
import cpp_units

SRC = "code/mover.cpp"
lines, units = cpp_units.parse(SRC)
raw = open(SRC, "rb").read().split(b"\n")


def chunk(u):
    return b"\n".join(raw[u["start"]:u["end"] + 1])


def text(u):
    return chunk(u).decode("utf-8", "replace").replace("\r", "")


GROUPS = [
    ("chunks", [(457, 479), (576, 950), (1158, 1684)]),
    ("diag", [(480, 575), (951, 1157), (2305, 2549), (3521, 3666)]),
    ("init", [(1685, 2304)]),
    ("combat", [(3146, 3520), (3667, 3949), (5289, 5908)]),
    ("path", [(3950, 5288)]),
    ("attack", [(5909, 6591)]),
    ("misc", [(6592, 7216)]),
    ("core", [(2550, 3145)]),
]
DOC = {
    "chunks": "multiplayer MoveChunk/StatusChunk build, pack, unpack and compare, plus MoverDynamics and MoverControl",
    "diag": "diagnostic dumps: chunk dump helpers, updateDebugWindow and the combat-log weapon fire/hit printers",
    "init": "Mover::loadGameSystem, set, init and release",
    "combat": "tactical orders, weapon helpers, weapon-fire/critical-hit/radio chunk queues, contacts and weapon queries",
    "path": "move goals, line of sight, global/local path calculation and path locking",
    "attack": "attack chance, shutdown/startup, refit and recover",
    "misc": "sensor text, waypoint drawing, optimal cells, mover placement, ejection and Save/CopyTo/Load",
}


def group_of(n):
    for name, ranges in GROUPS:
        for a, b in ranges:
            if a <= n <= b:
                return name
    return None


# ---- header ---------------------------------------------------------------------------------------------
incs = []
for u in units:
    if u["start"] + 1 >= 129:
        break
    for l in lines[u["start"]:u["end"] + 1]:
        m = re.match(r'\s*#\s*include\s*([<"][^>"]+[>"])', l)
        if m and m.group(1) not in incs:
            incs.append(m.group(1))

# guarded includes may carry trailing comments; keep the include list in order, plain
hdr = [
    "// Shared surface of the Mover translation units (mover*.cpp).",
    "//",
    "// mover.cpp keeps the tuning globals and tables below plus the small Mover accessors; the rest of the",
    "// Mover implementation is split by responsibility (chunks, diag, init, combat, path, attack, misc).",
    "#ifndef MOVER_INTERNAL_H",
    "#define MOVER_INTERNAL_H",
    "",
]
# the include groups are guarded by macros that other headers already define (some guard a header that no longer
# exists), so they are copied verbatim, guards included
hdr += [l.decode("utf-8", "replace").rstrip(chr(13)) for l in raw[5:128]]
hdr += ["", "#define	GOALMAP_CELL_DIM	61", "", "// Globals defined in mover.cpp (or elsewhere in the engine) that the split pieces share."]

externs = []
for u in units:
    n = u["start"] + 1
    if not (132 <= n <= 456) or u["kind"] != "decl":
        continue
    t = text(u)
    body = [l for l in t.split("\n") if l.strip() and not l.strip().startswith("//")]
    if not body:
        continue
    joined = "\n".join(body)
    if re.search(r"^\s*#", joined, re.M):
        continue  # preprocessor-wrapped (handled by hand below)
    if joined.lstrip().startswith(("static", "class", "struct", "typedef", "enum")):
        continue
    if "::" in joined.split("=")[0].split("(")[0]:
        continue  # Mover:: static member definitions are declared in mover.h
    if joined.lstrip().startswith("extern"):
        externs.append(re.sub(r"\s*//.*$", "", joined.strip(), flags=re.M))
        continue
    # definition -> extern declaration (strip the initializer)
    depth = 0; cut = None
    for i, ch in enumerate(joined):
        if ch in "[(": depth += 1
        elif ch in "])": depth -= 1
        elif ch == "=" and depth == 0:
            cut = i
            break
    head = joined[:cut] if cut is not None else joined.rstrip().rstrip(";")
    head = re.sub(r"\s*//.*$", "", head, flags=re.M)
    head = re.sub(r"\s+", " ", head).strip()
    if head.startswith("const "):
        continue
    externs.append("extern " + head + ";")
seen = set(); uniq = []
for e in externs:
    k = re.sub(r"\s+", " ", e)
    if k not in seen:
        seen.add(k); uniq.append(e)
hdr += uniq
hdr.append("")

# free functions defined outside core and shared: prototypes
protos = []
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
    s = re.sub(r"\s+", " ", " ".join(sig)).strip()
    return s

for u in units:
    n = u["start"] + 1
    if u["kind"] != "func" or n < 457:
        continue
    g = group_of(n)
    s = proto(u)
    if "::" in s.split("(")[0] or s.startswith(("static", "inline", "void Mover", "#")) or g in (None,):
        continue
    if re.match(r"^[\w\s\*&:<>]+\(", s) and g != "core":
        protos.append(s.rstrip() + ";")
hdr.append("// Free functions defined in the split pieces.")
hdr += protos
hdr.append("")
hdr.append("#endif // MOVER_INTERNAL_H")
hdr.append("")
open("code/mover_internal.h", "wb").write("\r\n".join(hdr).encode("utf-8"))

# ---- groups ---------------------------------------------------------------------------------------------
bodies = {g: [] for g, _ in GROUPS}
for u in units:
    n = u["start"] + 1
    if n < 457 or u["kind"] == "tail":
        continue
    g = group_of(n)
    if g is None:
        raise SystemExit("unassigned unit at %d (%s)" % (n, u["name"]))
    bodies[g].append(chunk(u))

pre = [chunk(u) for u in units if u["start"] + 1 < 457]
open("code/mover.cpp.new", "wb").write(b"\n".join(pre) + b"\n" + b"\n".join(bodies["core"]) + b"\n")
for g, _ in GROUPS:
    if g == "core":
        continue
    head = ("// Mover: %s.\r\n// Declared in mover.h; shared names in mover_internal.h.\r\n#include \"mover_internal.h\"\r\n\r\n" % DOC[g]).encode("utf-8")
    open("code/mover_%s.cpp" % g, "wb").write(head + b"\n".join(bodies[g]) + b"\n")
print("includes", len(incs), "externs", len(uniq), "protos", len(protos))
for g, _ in GROUPS:
    print(g, sum(c.count(b"\n") + 1 for c in bodies[g]))
