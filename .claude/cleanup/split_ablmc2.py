#!/usr/bin/env python3
"""split_ablmc2.py -- one-shot splitter for code/ablmc2.cpp (see giant-file-split-plan.md s9). Units are moved verbatim
(per-line EOL bytes kept); only the generated header, per-file banners and the repro hook qualifier are new."""
import re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cpp_units

SRC = "code/ablmc2.cpp"
lines, units = cpp_units.parse(SRC)
raw = open(SRC, "rb").read().split(b"\n")


def chunk(u):
    return b"\n".join(raw[u["start"]:u["end"] + 1])


GROUPS = [
    ("queries", [(159, 315), (392, 1189)]),
    ("orders", [(1190, 1594), (3930, 4076)]),
    ("objects", [(1595, 2309)]),
    ("state", [(2310, 3436)]),
    ("misc", [(3438, 3929), (4077, 4161)]),
    ("camera", [(4162, 4858)]),
    ("roster", [(4859, 5928)]),
    ("runtime", [(5929, 6174), (6603, 6726)]),
    ("core", [(6175, 6602)]),
]
DOC = {
    "queries": "warrior/contact/weapon queries and setters, plus the shared target-plan helpers",
    "orders": "movement/attack orders and the core* tactical order bindings",
    "objects": "object damage/distance/status, timers, objectives, music/speech/video and mission-status bindings",
    "state": "object state get/set, salvage, capture, strikes, prisoners and gates",
    "misc": "unit status, repair, messaging, path queries, pilot state and debug bindings",
    "camera": "movie mode, camera get/set pairs, GUI/logistics callouts and help/target/shelter requests",
    "roster": "text messages, teleport, roster/pilot operations, campaign globals, strike toggles, radius counts, omni stubs, trigger areas and off-map queries",
    "runtime": "ABL host callbacks (heaps, files, fatal/endless-state) and the arg-guard repro hooks",
}


def group_of(n):
    for name, ranges in GROUPS:
        for a, b in ranges:
            if a <= n <= b:
                return name
    return None


incs = []
for u in units:
    if u["start"] + 1 > 111:
        break
    for l in lines[u["start"]:u["end"] + 1]:
        m = re.match(r'\s*#\s*include\s*"([^"]+)"', l)
        if m and m.group(1) not in incs:
            incs.append(m.group(1))


def proto(u):
    body = [l.rstrip("\r") for l in lines[u["start"]:u["end"] + 1]]
    i = 0
    while i < len(body) and (not body[i].strip() or body[i].lstrip().startswith("//") or body[i].lstrip().startswith("/*")):
        i += 1
    sig = []
    while i < len(body):
        t = body[i]
        sig.append(t.split("{")[0].split("//")[0])
        if "{" in t:
            break
        i += 1
    s = re.sub(r"\s+", " ", " ".join(sig)).strip()
    if s.startswith(("static", "inline")):
        return None
    return s.rstrip() + ";"


execs, others, inline_units = [], [], []
for u in units:
    n = u["start"] + 1
    if u["kind"] != "func" or n < 159:
        continue
    first = None
    for l in lines[u["start"]:u["end"] + 1]:
        t = l.strip()
        if t and not t.startswith("//"):
            first = t
            break
    nm = re.search(r"\b(exec\w+)\s*\(\s*void\s*\)", first or "")
    if nm and nm.group(1) == u["name"]:
        p = proto(u)
        assert p, u
        execs.append(p)
    elif 316 <= n <= 391:
        inline_units.append(u)
    else:
        p = proto(u)
        if p:
            others.append((n, u["name"], p))

BS = chr(92)
hdr = [
    "// Shared surface of the ABL native-binding translation units (ablmc2*.cpp).",
    "//",
    "// ablmc2.cpp owns the globals, initABL and the ONE ordered ABLi_addFunction list: registration order is",
    "// state (enterStandardRoutine assigns each routine its key positionally into a fixed table), so the list",
    "// never moves next to the functions. The exec* bindings live in ablmc2_<domain>.cpp.",
    "#ifndef ABLMC2_INTERNAL_H",
    "#define ABLMC2_INTERNAL_H",
    "",
]
for h in incs:
    hdr.append('#include "%s"' % h)
hdr += [
    "",
    "// ABL-OPCODE-BOUNDS-HARDEN gates and the ABL exec context (defined in mclib/ablxstd.cpp / ablexec.cpp).",
    "extern bool s_ablArgGuard;",
    "extern bool s_ablRuntimeSoftfail;",
    "extern int         execLineNumber;",
    "extern ABLModulePtr CurModule;",
    "extern int32_t     FileNumber;",
    "",
    "// Globals defined in ablmc2.cpp.",
    "extern MoverGroupPtr	CurGroup;",
    "extern GameObjectPtr	CurObject;",
    "extern long	CurObjectClass;",
    "extern MechWarriorPtr	CurWarrior;",
    "extern GameObjectPtr	CurContact;",
    "extern long	CurAlarm;",
    "extern MoverPtr	moverList[256];",
    "extern bool	TacOrderOrigin;",
    "extern long	CurMultiplayCode;",
    "extern long	CurMultiplayParam;",
    "extern UserHeapPtr	AblStackHeap;",
    "extern UserHeapPtr	AblCodeHeap;",
    "extern UserHeapPtr	AblSymbolHeap;",
    "",
    "// Defined elsewhere in the engine.",
    "extern float	MaxVisualRadius;",
    "extern float	WeaponRange[NUM_FIRERANGES];",
    "extern GameLog*	BugLog;",
    "extern bool	GeneralAlarm;",
    "extern bool	friendlyDestroyed;",
    "extern bool	enemyDestroyed;",
    "extern bool	invulnerableON;		// tutorials: mechs take damage but look like they are not",
    "void DEBUGWINS_setGameObject (long debugObj, GameObjectPtr obj);",
    "void DEBUGWINS_print (const char* s, long window);",
    "",
    "// Emits an [ABL_ARG_GUARD] trace line (flushed, for smoke-log capture) when an exec* binding skips a null pointer arg.",
    "static inline void abl_arg_guard_log(const char* funcName, const char* paramName) {",
    '	const char* srcFile = (CurModule && FileNumber > -1) ? CurModule->getSourceFile(FileNumber) : "unavailable";',
    '	const char* modName = CurModule ? CurModule->getName() : "(unknown)";',
    '	printf("[ABL_ARG_GUARD] func=%s param=%s module=%s file=%s line=%d - null ptr, skipping' + BS + 'n",',
    "		funcName, paramName, modName, srcFile, execLineNumber);",
    "	fflush(stdout);",
    "}",
    "",
    "// Shared inline helpers (defined below).",
    "GameObjectPtr getObject (long partId, bool existsOnly = false);",
    "long getMovers (long partId, MoverPtr* list, bool existsOnly = false);",
    "",
]
out_hdr = "\r\n".join(hdr).encode("utf-8")
out_hdr += b"\r\n" + b"\n".join(chunk(u) for u in inline_units) + b"\r\n"
rest = ["", "// Non-exec functions shared across the translation units."]
for n, nm, p in others:
    rest.append(p)
rest.append("void runAblArgGuardRepro(void);")
rest.append("")
rest.append("// ABL native bindings, one per registered routine (registered in initABL, ablmc2.cpp).")
rest += execs
rest.append("")
rest.append("#endif // ABLMC2_INTERNAL_H")
rest.append("")
out_hdr += "\r\n".join(rest).encode("utf-8")
open("code/ablmc2_internal.h", "wb").write(out_hdr)

bodies = {g: [] for g, _ in GROUPS}
for u in units:
    n = u["start"] + 1
    if n <= 158:
        txt = chunk(u).decode("utf-8", "replace")
        last = txt.strip().split("\n")[-1].strip()
        is_def = u["kind"] == "decl" and not last.startswith("extern") and re.match(
            r"(MoverGroupPtr|GameObjectPtr|long|MechWarriorPtr|MoverPtr|bool|UserHeapPtr)\s", last)
        if is_def:
            bodies["core"].append(chunk(u))
        continue
    if 316 <= n <= 391 or u["kind"] == "tail":
        continue
    g = group_of(n)
    if g is None:
        raise SystemExit("unassigned unit at %d (%s)" % (n, u["name"]))
    c = chunk(u)
    if u["name"] in ("if", "guards") and n >= 6603:
        c = c.replace(b"static void runAblArgGuardRepro(void) {", b"void runAblArgGuardRepro(void) {")
    bodies[g].append(c)

CORE_HEAD = b"\r\n".join([
    b"// ABL native bindings for MechCommander 2: globals, initABL and closeABL.",
    b"//",
    b"// initABL holds the single ordered ABLi_addFunction list. Registration order is state (each routine's key is",
    b"// its position in a fixed table), so the list stays in this one file; the exec* bodies live in ablmc2_<domain>.cpp.",
    b'#include "ablmc2_internal.h"',
    b"", b""])
open("code/ablmc2.cpp.new", "wb").write(CORE_HEAD + b"\n".join(bodies["core"]) + b"\n")
for g, _ in GROUPS:
    if g == "core":
        continue
    head = ("// ABL native bindings: %s.\r\n// Declared in ablmc2_internal.h; registered, in order, by initABL in ablmc2.cpp.\r\n"
            '#include "ablmc2_internal.h"\r\n\r\n' % DOC[g]).encode("utf-8")
    open("code/ablmc2_%s.cpp" % g, "wb").write(head + b"\n".join(bodies[g]) + b"\n")
print("includes:", len(incs), " exec protos:", len(execs), " other protos:", len(others), " inline units:", len(inline_units))
for g, _ in GROUPS:
    print(g, sum(c.count(b"\n") + 1 for c in bodies[g]))
