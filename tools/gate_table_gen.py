#!/usr/bin/env python3
"""gate_table_gen -- the C++ half of tools/gate_gen.py (typed gate registry, slice G1a).

WHAT IT OWNS
------------
  Gates/gate_table.cpp    hand-edited constexpr table. THIS TOOL WRITES IT ONCE (--init-table)
                          from tools/gate_table.seed.json; after that people edit it.
  Gates/gate_index.gen.h  generated {name, kind, rule, tokens} index the hot header reads.
  Gates/gate_table.json   emitted by the compiler (tools/gate_dump), committed. The index,
                          and later the yaml/registry shim, are derived from THIS, so no
                          Python regex ever parses the C++ table.

MODES (called from gate_gen.py)
-------------------------------
  --init-table   seed -> Gates/gate_table.cpp + Gates/gate_index.gen.h   (refuses to overwrite
                 an existing table without --force)
  --write        Gates/gate_table.json (or the seed when the JSON does not exist yet)
                 -> Gates/gate_index.gen.h
  --check        stage A (hermetic): the JSON is well formed and sorted, the index matches it,
                 every seed gate is still in the table, registered gates agree with the
                 feature registry, the per-site rule-override count did not rise.
                 --rebuild adds stage B: rebuild tools/gate_dump and diff its JSON against
                 the committed one (catches default/doc/owner drift the compiler cannot).
"""

import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"))
# Owner rule: no window for any child process (cmake, the gate_dump exe).
from smoke_lib import nofocus  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

GATES_DIR = "Gates"
TABLE_CPP_REL = GATES_DIR + "/gate_table.cpp"
INDEX_REL = GATES_DIR + "/gate_index.gen.h"
TABLE_JSON_REL = GATES_DIR + "/gate_table.json"
SEED_REL = "tools/gate_table.seed.json"
LEDGER_REL = "tools/gate_g0_ledger.json"
YAML_REL = "docs/flags.auto.yaml"
HISTORY_REL = "docs/gate-history.md"
OVERRIDE_BASELINE_REL = "tools/gate_overrides.baseline.json"
JSON_VERSION = 1
MAX_DOC = 160

#: Gates read fresh on every call, so an in-process _putenv is visible (design 5.5).
REREAD = {"MC2_ACTIVE_MOD", "MC2_BRAIN_PATROL", "MC2_BRAIN_ENGAGE"}
#: Names the gate module itself defines rows for; they are read by Gates/gate.cpp and so
#: appear in the seed only once Gates/ is a scanned directory.
OWN_ROWS = {
    "MC2_CONFIG_DUMP": dict(kind="Enum", default="default", tokens=["default", "all", "none"],
                            doc="[CONFIG v1] dump scope: default (env-set gates), all (every row), none (summary only)"),
    "MC2_CONFIG_STRICT": dict(kind="Bool", default=False,
                              doc="make a bad value or an unregistered MC2_* variable fatal at startup"),
}

#: Gates whose seed kind (a guess from the reader idiom) is known to be wrong: they carry text.
KIND_OVERRIDES = {"MC2_ACTIVE_MOD": "String", "MC2_MOD_DEPS": "String", "MC2_LOG": "String",
                  "MC2_LOG_DECORATE": "String"}

_LEGACY_BOOL_RULES = {"Std", "Presence", "NonEmpty", "Exact1", "Lead1", "Lead0", "Lead0Kill", "Lead0P",
                      "DefOnLead0", "AtoiNZ", "Is01"}
_IMPLIED_UNSET = {"Lead0Kill": True, "DefOnLead0": True, "DefOnAtoiNZ": True}


# ---------------------------------------------------------------------------
# Row derivation from the seed
# ---------------------------------------------------------------------------

def _bool_rule(rec):
    rule = rec["rule"]
    if rule in _LEGACY_BOOL_RULES:
        return rule
    idiom = rec["readers"][0]["idiom"] if rec["readers"] else ""
    if rule == "Partial":
        if "Partial[D,D," in idiom:
            return "Lead0Def"
        return "Lead0Kill" if rec["default"] is True else "Lead0P"
    if rule == "Custom":
        if "[10001011]" in idiom:
            return "DefOnAtoiNZ"
        return "Lead0Kill"
    return "Std"


def _row_default(rec, kind, rule):
    d = rec["default"]
    if kind == "Bool":
        if rule in _IMPLIED_UNSET:
            return _IMPLIED_UNSET[rule]
        if rule not in ("Std", "Lead0Def"):
            return False
        return bool(d) if d is not None else False
    if kind == "Tri":
        return False
    if kind == "Int":
        return int(d) if isinstance(d, (int, float)) and not isinstance(d, bool) else 0
    if kind == "Float":
        return float(d) if isinstance(d, (int, float)) and not isinstance(d, bool) else 0.0
    return d if isinstance(d, str) else ""


def one_line_doc(text):
    text = " ".join((text or "").split())
    text = text.encode("ascii", "replace").decode("ascii").replace("?", "?")
    if not text:
        return ""
    m = re.search(r"(?<=[a-z0-9\)\"'`])\.(?=\s)", text)
    if m:
        text = text[:m.start() + 1]
    if len(text) > MAX_DOC:
        cut = text[:MAX_DOC - 3]
        cut = cut[:cut.rfind(" ")] if " " in cut else cut
        text = cut.rstrip(" ,;:") + "..."
    return text


def _unicode_safe(text):
    """The seed docs carry typographic characters; fold them to ASCII instead of '?'."""
    table = {"—": "-", "–": "-", "‘": "'", "’": "'", "“": '"', "”": '"',
             "→": "->", "≤": "<=", "≥": ">=", "×": "x", "…": "...", " ": " "}
    return "".join(table.get(c, c) for c in text)


def rows_from_seed(seed, ledger, smoke, history):
    rows = []
    gates = {g["name"]: g for g in seed["gates"]}
    for name, spec in OWN_ROWS.items():
        if name not in gates:
            gates[name] = {"name": name, "kind": spec["kind"], "default": spec["default"], "rule": "Std",
                           "owner": "gameos", "retireBy": "", "doc": spec["doc"], "readers": [],
                           "numericRange": {"enum": spec["tokens"]} if "tokens" in spec else None,
                           "registered": False}
    for name in sorted(gates):
        rec = gates[name]
        kind = KIND_OVERRIDES.get(name, rec["kind"])
        if name in KIND_OVERRIDES:
            rec = dict(rec, kind=kind, rule="Std", default="")
        rule = _bool_rule(rec) if kind == "Bool" else ("Prefix" if (kind in ("Int", "Float") and rec["rule"] == "Prefix") else "Std")
        d = _row_default(rec, kind, rule)
        nr = rec.get("numericRange") or {}
        tokens = ""
        if kind == "Enum":
            toks = list(nr.get("enum") or [])
            if d not in toks:
                toks.insert(0, d)
            tokens = "|".join(toks)
        rng = None
        if kind in ("Int", "Float") and "lo" in nr and "hi" in nr and not nr.get("loExclusive") and not nr.get("hiExclusive"):
            if nr.get("style") == "compare-reject":
                rng = ("range", nr["lo"], nr["hi"])
            elif nr.get("style") == "clamp":
                rng = ("clampRange", nr["lo"], nr["hi"])
        doc = one_line_doc(_unicode_safe(rec.get("doc", "")))
        rows.append({
            "name": name, "kind": kind, "rule": rule, "default": d, "owner": rec.get("owner") or "unclassified",
            "retireBy": rec.get("retireBy", "") or "", "doc": doc, "tokens": tokens, "range": rng,
            "reread": name in REREAD,
            "divergent": ledger.get(name, {}).get("status") == "divergent",
            "smokePassthrough": name in smoke, "logTopic": "",
            "history": name in history,
            "defaultUnknown": rec.get("seedDefaultSource") == "unknown" and kind in ("Int", "Float", "Bool", "String"),
        })
    return rows


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def cpp_str(s):
    out = ['"']
    for ch in s:
        o = ord(ch)
        if ch in '"\\':
            out.append("\\" + ch)
        elif o < 0x20 or o == 0x7F:
            out.append("\\x%02x" % o + ('""' if False else ""))
        elif o > 0x7E:
            out.append("\\u%04x" % o if o < 0x10000 else "\\U%08x" % o)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def float_lit(x):
    r = repr(float(x))
    if "e" in r or "E" in r or "." in r or "n" in r:
        return r
    return r + ".0"


def num_lit(kind, x):
    return str(int(x)) if kind == "Int" else float_lit(x)


def row_line(r):
    k = r["kind"]
    if k == "Bool" or k == "Tri":
        d = "true" if r["default"] else "false"
    elif k == "Int":
        d = str(int(r["default"]))
    elif k == "Float":
        d = float_lit(r["default"])
    else:
        d = cpp_str(r["default"])
    opts = []
    if r["reread"]:
        opts.append("reread")
    if r["divergent"]:
        opts.append("divergent")
    if r["smokePassthrough"]:
        opts.append("passthrough")
    if r["range"]:
        fn, lo, hi = r["range"]
        opts.append("%s(%s, %s)" % (fn, num_lit(k, lo) if k == "Int" else float_lit(lo),
                                    num_lit(k, hi) if k == "Int" else float_lit(hi)))
    if r["tokens"]:
        opts.append("tokens(%s)" % cpp_str(r["tokens"]))
    if r["logTopic"]:
        opts.append("topic(%s)" % cpp_str(r["logTopic"]))
    tail = (", " + " | ".join(opts)) if opts else ""
    common = "%s, %s, %s, %s, %s, %s%s" % (cpp_str(r["name"]), k, d, cpp_str(r["owner"]), cpp_str(r["retireBy"]),
                                           cpp_str(r["doc"]), tail)
    if r["rule"] == "Std":
        line = "G(%s)," % common
    elif r["rule"] == "Prefix":
        line = "P(%s)," % common
    else:
        line = "L(%s, %s)," % (r["rule"], common)
    if r.get("defaultUnknown"):
        line += "  // default not derivable from the tree"
    return line


INDEX_HEAD = """// Gates/gate_index.gen.h -- GENERATED by tools/gate_gen.py --write from Gates/gate_table.json.
// DO NOT EDIT. One sorted line per gate: everything gate.h needs to resolve a name at
// compile time. A row added to gate_table.cpp without regenerating this file fails the
// static_asserts at the bottom of gate_table.cpp.
#pragma once

#include <cstddef>

#include "gate_types.h"

namespace mc2::gate::detail {

inline constexpr std::size_t kGateCount = %d;

inline constexpr IndexEntry kIndex[kGateCount] = {
"""


def render_index(rows):
    L = [INDEX_HEAD % len(rows)]
    for r in rows:
        tok = cpp_str(r["tokens"]) if r["tokens"] else "{}"
        L.append("    {%s, Kind::%s, Rule::%s, %s},\n" % (cpp_str(r["name"]), r["kind"], r["rule"], tok))
    L.append("};\n\n}  // namespace mc2::gate::detail\n")
    return "".join(L)


TABLE_HEAD = '''// Gates/gate_table.cpp -- THE gate table: one row per MC2_* environment variable the engine
// reads. The compiler is its parser (tools/gate_dump emits Gates/gate_table.json from the
// compiled rows); nothing parses this file with a regex.
//
// Row = G(name, Kind, default, owner, retireBy, doc[, options])    Std rule
//       L(Rule, name, Kind, default, owner, retireBy, doc[, options])   legacy rule, kept so a
//                                                                  migrated site reads exactly as before
//       P(...)                                                      numeric row with atoi/atof parsing
// Rows stay one per line, sorted by name, names unique, doc at most 160 characters; long prose
// lives in docs/gate-history.md (see the `// history:` pointers). Adding or renaming a row
// needs `py -3 tools/gate_gen.py --write` so the index follows.
#include "gate_table_dsl.h"

#ifndef GATE_INDEX_BOOTSTRAP
#include "gate_index.gen.h"
#endif

// The table lives in rows::tbl so that the kind names (Bool, Tri, ...) found through
// `using enum Kind` in rows win over the type mc2::gate::Tri.
namespace mc2::gate::rows::tbl {

constexpr Row kTable[] = {
'''

TABLE_TAIL = '''};

constexpr std::size_t kCount = sizeof(kTable) / sizeof(kTable[0]);

static_assert(nameIsSortedUnique(kTable, kCount), "gate_table.cpp: rows must be sorted by name with no duplicates");
static_assert(firstInsaneRow(kTable, kCount) == kCount, "gate_table.cpp: a row breaks a table rule (see rowIsSane in gate_table_dsl.h)");

#ifndef GATE_INDEX_BOOTSTRAP
constexpr bool matchesIndex() {
    if (kCount != mc2::gate::detail::kGateCount) return false;
    for (std::size_t i = 0; i < kCount; ++i) {
        const Row& r = kTable[i];
        const IndexEntry& e = mc2::gate::detail::kIndex[i];
        if (r.name != e.name || r.kind != e.kind || r.rule != e.rule || r.tokens != e.tokens) return false;
    }
    return true;
}
static_assert(matchesIndex(), "gate_table.cpp and gate_index.gen.h disagree: run py -3 tools/gate_gen.py --write");
#endif

}  // namespace mc2::gate::rows::tbl

namespace mc2::gate {
const Row* tableData() noexcept { return rows::tbl::kTable; }
std::size_t tableSize() noexcept { return rows::tbl::kCount; }
}  // namespace mc2::gate
'''


def render_table_cpp(rows):
    L = [TABLE_HEAD]
    for r in rows:
        if r["history"]:
            L.append("// history: %s#%s\n" % (HISTORY_REL, r["name"]))
        L.append(row_line(r) + "\n")
    L.append(TABLE_TAIL)
    return "".join(L)


# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------

def _load_json(root, rel):
    p = os.path.join(root, rel.replace("/", os.sep))
    if not os.path.isfile(p):
        return None
    with open(p, "r", encoding="utf-8") as fh:
        return json.load(fh)


def smoke_names(root):
    p = os.path.join(root, YAML_REL.replace("/", os.sep))
    names = set()
    if not os.path.isfile(p):
        return names
    cur = None
    with open(p, "r", encoding="utf-8") as fh:
        for line in fh:
            m = re.match(r"\s+- name: (MC2_\w+)", line)
            if m:
                cur = m.group(1)
                continue
            if cur and re.match(r"\s+in_smoke_passthrough: yes", line):
                names.add(cur)
    return names


def history_names(root):
    p = os.path.join(root, HISTORY_REL.replace("/", os.sep))
    if not os.path.isfile(p):
        return set()
    with open(p, "r", encoding="utf-8") as fh:
        return set(re.findall(r"(?m)^## (MC2_\w+)\s*$", fh.read()))


def seed_rows(root):
    seed = _load_json(root, SEED_REL)
    if seed is None:
        raise SystemExit("gate_table_gen: %s is missing; run gate_gen.py --import-legacy" % SEED_REL)
    ledger = (_load_json(root, LEDGER_REL) or {}).get("gates", {})
    return rows_from_seed(seed, ledger, smoke_names(root), history_names(root))


def rows_from_json(doc):
    rows = []
    for g in doc["gates"]:
        rng = g.get("range")
        rows.append({
            "name": g["name"], "kind": g["kind"], "rule": g["rule"], "default": g["default"],
            "owner": g["owner"], "retireBy": g["retireBy"], "doc": g["doc"], "tokens": g.get("tokens", ""),
            "range": (("clampRange" if rng.get("clamp") else "range"), rng["lo"], rng["hi"]) if rng else None,
            "reread": g.get("reread", False), "divergent": g.get("divergent", False),
            "smokePassthrough": g.get("smokePassthrough", False), "logTopic": g.get("logTopic", ""),
            "history": False, "defaultUnknown": False,
        })
    return rows


def _write(root, rel, text):
    p = os.path.join(root, rel.replace("/", os.sep))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------

def init_table(root, force=False):
    p = os.path.join(root, TABLE_CPP_REL.replace("/", os.sep))
    if os.path.isfile(p) and not force:
        print("gate_table_gen: %s exists (hand-edited since the first write); pass --force to overwrite" % TABLE_CPP_REL)
        return 1
    rows = seed_rows(root)
    _write(root, TABLE_CPP_REL, render_table_cpp(rows))
    _write(root, INDEX_REL, render_index(rows))
    print("gate_table_gen: wrote %s and %s (%d rows from the seed)" % (TABLE_CPP_REL, INDEX_REL, len(rows)))
    return 0


def write_index(root):
    doc = _load_json(root, TABLE_JSON_REL)
    if doc is not None:
        rows = rows_from_json(doc)
        src = TABLE_JSON_REL
    else:
        rows = seed_rows(root)
        src = SEED_REL + " (no compiled table JSON yet)"
    _write(root, INDEX_REL, render_index(rows))
    print("gate_table_gen: wrote %s from %s (%d gates)" % (INDEX_REL, src, len(rows)))
    return 0


def count_overrides(root):
    """Per-site rule overrides: gate::get<"MC2_X", gate::Rule::...>() outside Gates/ and tests."""
    pat = re.compile(r'get\s*<\s*"MC2_\w+"\s*,\s*(?:gate::|mc2::gate::)?Rule::')
    n = 0
    for top in ("code", "mclib", "GameOS", "RenderCore", "GuiRuntime", "RenderWorld", "GameAdapters",
                "HostServices", "EditorBridge", "editor", "gui"):
        base = os.path.join(root, top)
        for dp, _dn, fns in os.walk(base):
            for fn in fns:
                if fn.endswith((".cpp", ".h", ".hpp", ".inl", ".cc")):
                    try:
                        with open(os.path.join(dp, fn), "r", encoding="utf-8", errors="replace") as fh:
                            n += len(pat.findall(fh.read()))
                    except OSError:
                        pass
    return n


def build_gate_dump(root, outdir):
    """Configure and build tools/gate_dump into `outdir`; return the exe path or None."""
    src = os.path.join(root, "tools", "gate_dump")
    if not os.path.isfile(os.path.join(src, "CMakeLists.txt")):
        print("gate_table_gen: tools/gate_dump/CMakeLists.txt is missing")
        return None
    vs = ("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/"
          "Microsoft/CMake/CMake/bin/cmake.exe")
    cm = vs if os.path.isfile(vs) else "cmake"
    r = subprocess.run([cm, "-S", src, "-B", outdir, "-A", "x64"], capture_output=True, text=True,
                       **nofocus.hidden_kwargs())
    if r.returncode != 0:
        print((r.stdout + r.stderr)[-2000:])
        return None
    r = subprocess.run([cm, "--build", outdir, "--config", "RelWithDebInfo"], capture_output=True, text=True,
                       **nofocus.hidden_kwargs())
    if r.returncode != 0:
        print((r.stdout + r.stderr)[-4000:])
        return None
    for cand in (os.path.join("RelWithDebInfo", "gate_dump.exe"), "gate_dump.exe", "gate_dump"):
        p = os.path.join(outdir, cand)
        if os.path.isfile(p):
            return p
    return None


def check(root, rebuild=False):
    fails = []
    doc = _load_json(root, TABLE_JSON_REL)
    if doc is None:
        return ["%s is missing: build tools/gate_dump and commit its output" % TABLE_JSON_REL]
    rows = rows_from_json(doc)
    names = [r["name"] for r in rows]
    if names != sorted(names) or len(set(names)) != len(names):
        fails.append("%s is not sorted/unique" % TABLE_JSON_REL)
    if doc.get("count") != len(rows):
        fails.append("%s count field %s != %d rows" % (TABLE_JSON_REL, doc.get("count"), len(rows)))
    ip = os.path.join(root, INDEX_REL.replace("/", os.sep))
    have = open(ip, "r", encoding="utf-8", newline="").read() if os.path.isfile(ip) else None
    if have != render_index(rows):
        fails.append("%s is stale against %s: run py -3 tools/gate_gen.py --write" % (INDEX_REL, TABLE_JSON_REL))
    seed = _load_json(root, SEED_REL)
    if seed is not None:
        missing = sorted(set(g["name"] for g in seed["gates"]) - set(names))
        if missing:
            fails.append("%d seed gate(s) are not in the table: %s" % (len(missing), ", ".join(missing[:6])))
    # registered gates agree with the feature registry's default
    try:
        sys.path.insert(0, os.path.join(root, "scripts"))
        import registry_parse as rp
        reg_text = open(os.path.join(root, "RenderCore", "RendererFeatureRegistry.h"), "r", encoding="utf-8",
                        errors="replace").read()
        byname = {r["name"]: r for r in rows}
        noted = {g["name"] for g in (seed or {}).get("gates", []) if g.get("defaultDisagreement") or g.get("conflict")}
        for _k, e in rp.parse_registry_text(reg_text, "RenderCore/RendererFeatureRegistry.h").items():
            ev = e.get("envVar")
            if not ev or ev not in byname:
                if ev:
                    fails.append("registry gate %s is not in the table" % ev)
                continue
            row = byname[ev]
            if row["kind"] == "Bool" and e.get("defaultOn") is not None and ev not in noted \
                    and bool(row["default"]) != bool(e["defaultOn"]):
                fails.append("%s: table default %s but the feature registry says %s (no defaultDisagreement note)"
                             % (ev, row["default"], e["defaultOn"]))
    except (ImportError, OSError, KeyError, AttributeError) as ex:
        fails.append("registry cross-check could not run: %s" % ex)
    base = (_load_json(root, OVERRIDE_BASELINE_REL) or {}).get("count", 0)
    ov = count_overrides(root)
    if ov > base:
        fails.append("per-site rule overrides rose: %d (baseline %d)" % (ov, base))
    if rebuild and not fails:
        import tempfile
        with tempfile.TemporaryDirectory(prefix="gate_dump_") as td:
            exe = build_gate_dump(root, td)
            if exe is None:
                fails.append("tools/gate_dump did not build")
            else:
                r = subprocess.run([exe], capture_output=True, text=True, **nofocus.hidden_kwargs())
                want = open(os.path.join(root, TABLE_JSON_REL.replace("/", os.sep)), "r", encoding="utf-8",
                            newline="").read()
                if r.returncode != 0:
                    fails.append("gate_dump exited %d" % r.returncode)
                elif r.stdout.replace("\r\n", "\n") != want.replace("\r\n", "\n"):
                    fails.append("%s differs from what the compiled table emits: rebuild tools/gate_dump and commit "
                                 "its output (default/doc/owner drift)" % TABLE_JSON_REL)
    return fails
