#!/usr/bin/env python3
"""gate_conflicts -- every gate whose readers disagree about what a value MEANS.

WHY THIS EXISTS
---------------
The typed gate registry (docs: .claude/cleanup/typed-gate-registry-design.md)
gives each gate ONE truthiness rule. A gate whose readers already disagree
cannot be migrated byte-identically: whichever rule is chosen changes some
reader's behaviour. The design's section 4 lists nine such gates, found by
review; this tool finds ALL of them, so the unification slice (G0) works from
a proven-complete list and not from a sample.

A "reader" is one `"MC2_*"` literal passed to getenv (or to a helper that
getenv's it). The C++ around it is executed by the interpreter in
scripts/gate_idioms.py against eight probe values -- unset, "", "0", "false",
"1", "yes", "10", "2.5" -- so a reader is described by what it DOES. Two readers
of one gate conflict when, for any probe, they give different answers.

OUTPUTS
-------
  docs/gate-reader-conflicts.md    one row per conflicting gate, for people
  docs/gate-reader-conflicts.json  the same data, for tools

RATCHET
-------
`--check` compares the live tree to tools/gate_conflicts.baseline.json and
fails if the number of conflicting gates, or of readers this tool cannot
classify, went up -- or if a gate that was not in conflict is now. The count can
only fall. `--write-baseline` refuses to record a higher count.

USAGE
  py -3 tools/gate_conflicts.py            # print a summary
  py -3 tools/gate_conflicts.py --write    # regenerate docs/gate-reader-conflicts.{md,json}
  py -3 tools/gate_conflicts.py --check    # ratchet against the baseline (check-contracts row)
  py -3 tools/gate_conflicts.py --write-baseline
"""

import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import gate_idioms as gi  # noqa: E402
import console_child  # noqa: E402  (windowless console children without a conhost each)

SCAN_DIRS = ["code", "mclib", "GameOS", "RenderCore", "GuiRuntime", "RenderWorld",
             "GameAdapters", "HostServices", "editor", "gui"]
REGISTRY = os.path.join("RenderCore", "RendererFeatureRegistry.h")
OUT_MD = os.path.join("docs", "gate-reader-conflicts.md")
OUT_JSON = os.path.join("docs", "gate-reader-conflicts.json")
BASELINE = os.path.join("tools", "gate_conflicts.baseline.json")
LEDGER = os.path.join("tools", "gate_g0_ledger.json")

#: Ledger statuses that are an owner-recorded outcome, not an open conflict: `enum`
#: (the readers compare level characters on purpose) and `divergent` (the defaults
#: differ and the owner has to choose). A conflicting gate with any other status, or
#: none, is UNRESOLVED and counts against the ratchet.
LEDGER_ACCEPTED = ("enum", "divergent", "intentional-optin")

#: Where a gate being SET (rather than read) makes a reader disagreement visible
#: in routine runs: harness/launcher configs, not source.
CONFIG_DIRS = ["scripts", "tests", "tools", "data", "run"]
CONFIG_ROOT_EXTS = (".json", ".bat", ".ps1", ".cmd")
CONFIG_EXTS = (".py", ".sh", ".bat", ".ps1", ".cmd", ".json", ".yaml", ".yml",
               ".txt", ".cfg", ".ini", ".toml", ".cmake", ".md")
TIER1_DOC = os.path.join("docs", "tier1_env_vars.md")
#: Files that name every gate without setting any: their mentions are not config.
CONFIG_EXCLUDE = {
    OUT_MD.replace("\\", "/"), OUT_JSON.replace("\\", "/"),
    BASELINE.replace("\\", "/"), "tools/gate_conflicts.py",
}

#: A gate named like this is a "kill" flag; its readers may legitimately use the
#: complement of one another's test, so a reviewer should look before unifying.
_INVERTED_NAME_RE = re.compile(r"DISABLE|(?:^|_)NO_|_OFF(?:_|$)|KILL|SKIP|BYPASS|FORCE_OFF")

#: The nine conflicts the design lists in its section 4, with the reader sites it
#: names. `claims` are (file suffix, line); the tool confirms each against the
#: sites it found and reports the nearest actual reader when they differ.
DESIGN_S4 = [
    ("MC2_SHADOW_ENABLE", [("GameOS/gameos/gos_mech_batcher.cpp", 1167),
                           ("GameOS/gameos/gos_static_prop_batcher.cpp", 9290),
                           ("GameOS/gameos/vulkan_post_screenfx_native.cpp", 759)]),
    ("MC2_DISABLE_GOSFX", [("mclib/mlr/mlr_gate.cpp", 37),
                           ("GameOS/gameos/gameosmain.cpp", 1340),
                           ("GuiRuntime/GraphicsOptionsWindow.cpp", 1477)]),
    ("MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR", [("GameOS/gameos/gos_terrain_height_tex.cpp", 51)]),
    ("MC2_LIGHT_GRID_DIM", [("GameOS/gameos/gos_terrain_lighting.cpp", 276)]),
    ("MC2_LIGHT_POOL_MAX", [("GameOS/gameos/gos_terrain_lighting.cpp", 859)]),
    ("MC2_SNAPSHOT_STATIC_PROP_BUILD", [("GameOS/gameos/gos_static_prop_batcher.cpp", 246),
                                        ("GameOS/gameos/gos_static_prop_batcher.cpp", 247)]),
    ("MC2_ACTIVE_MOD", [("mclib/file.cpp", 727),
                        ("mclib/anim_override_registry.cpp", 161),
                        ("mclib/fx_def_registry.cpp", 355),
                        ("mclib/model_override_registry.cpp", 203)]),
]

#: Why a design-section-4 gate is, or is not, a reader-vs-reader disagreement.
#: Four of the nine are hazards of a different kind -- a single reader whose range
#: or enum a Bool/Int row would widen, or a re-read after the environment changed --
#: so they are correctly absent from the conflict table and are explained here.
DESIGN_S4_NOTES = {
    "MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR":
        "one reader; the hazard is reader vs a proposed clamp row (accepts only 2 or 4), not reader vs reader",
    "MC2_LIGHT_GRID_DIM":
        "one reader; the hazard is reader vs a proposed clamp row (out of range falls back to the default)",
    "MC2_LIGHT_POOL_MAX":
        "one reader; the hazard is reader vs a proposed clamp row (out of range falls back to the default)",
    "MC2_ACTIVE_MOD":
        "all readers agree (NonEmpty); the hazard is a re-read after _putenv_s, i.e. caching, not truthiness",
    "MC2_SNAPSHOT_STATIC_PROP_BUILD":
        "the editor's `!getenv(..)` write-guard counts as a Presence reader; the design's ordering hazard stands",
}


# ---------------------------------------------------------------------------
# Scan
# ---------------------------------------------------------------------------

class Tree:
    """The files a scan reads: a git revision (the COMMITTED tree) or the working tree.

    The ratchet reads the committed tree by default, because the builder lane edits
    these C++ files continuously and a gate that flaps on someone else's half-made
    change teaches everyone to ignore it. `rev=None` reads the working tree (tests,
    and a developer checking an edit before committing it).
    """

    def __init__(self, root, rev=None):
        self.root, self.rev = root, rev

    def _git(self, args, inp=None):
        return subprocess.run(["git"] + args, cwd=self.root, input=inp, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, check=True,
                              **console_child.child_kwargs(redirected=True)).stdout

    def list(self, dirs, exts, root_level=False):
        """Relative paths under `dirs` (or the top level only) with one of `exts`."""
        rels = []
        if self.rev is None:
            if root_level:
                rels = [f for f in os.listdir(self.root)
                        if os.path.isfile(os.path.join(self.root, f))]
            for d in ([] if root_level else dirs):
                base = os.path.join(self.root, d)
                for dirpath, dirnames, filenames in os.walk(base):
                    dirnames[:] = [x for x in dirnames if x not in gi.SKIP_DIRS]
                    for fn in filenames:
                        rels.append(os.path.relpath(os.path.join(dirpath, fn), self.root).replace("\\", "/"))
        else:
            args = ["ls-tree", "-z", "--name-only", self.rev]
            if not root_level:
                args = ["ls-tree", "-r", "-z", "--name-only", self.rev, "--"] + list(dirs)
            rels = [x.decode("utf-8", "replace") for x in self._git(args).split(b"\0") if x]
        out = [r for r in rels if r.endswith(exts)
               and not any(part in gi.SKIP_DIRS for part in r.split("/")[:-1])]
        return sorted(set(out))

    def read_all(self, rels):
        """-> {rel: text}; missing files are simply absent."""
        out = {}
        if self.rev is None:
            for r in rels:
                try:
                    with open(os.path.join(self.root, r), "r", encoding="utf-8", errors="replace") as fh:
                        out[r] = fh.read()
                except OSError:
                    pass
            return out
        raw = self._git(["cat-file", "--batch"],
                        "".join("%s:%s\n" % (self.rev, r) for r in rels).encode("utf-8"))
        pos = 0
        for r in rels:
            eol = raw.index(b"\n", pos)
            hdr = raw[pos:eol].split()
            pos = eol + 1
            if len(hdr) == 3 and hdr[1] == b"blob":
                size = int(hdr[2])
                out[r] = raw[pos:pos + size].decode("utf-8", "replace")
                pos += size + 1
        return out

    def read(self, rel):
        return self.read_all([rel]).get(rel, "")

    def describe(self):
        if self.rev is None:
            return "working tree"
        try:
            sha = self._git(["rev-parse", "--short=9", self.rev]).decode().strip()
        except (subprocess.CalledProcessError, OSError):
            sha = "?"
        return "commit %s" % sha


def open_tree(root, worktree):
    """The committed tree when asked and when git can serve it, else the working tree."""
    if worktree:
        return Tree(root, None)
    t = Tree(root, "HEAD")
    try:
        t._git(["rev-parse", "--verify", "HEAD"])
    except (subprocess.CalledProcessError, OSError):
        return Tree(root, None)
    return t


def load_registry(tree):
    """-> {env var: (default_on, kind)} from the feature registry, via the
    existing checker's parser (one reading of that header, not two)."""
    text = tree.read(REGISTRY.replace("\\", "/"))
    if not text:
        return {}
    path = os.path.join(ROOT, "scripts", "gate_default_classifier.py")
    spec = importlib.util.spec_from_file_location("gate_default_agreement", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return {e["var"]: (e["default_on"], e["kind"]) for e in mod.parse_registry(text)}


def scan_tree(tree, scan_dirs=None):
    scan_dirs = SCAN_DIRS if scan_dirs is None else scan_dirs
    reg = REGISTRY.replace("\\", "/")
    rels = [r for r in tree.list(scan_dirs, gi.SCAN_EXTS) if r != reg]
    texts = tree.read_all(rels)
    sources = [(r, gi.strip_comments(texts[r])) for r in rels if r in texts]
    return gi.scan_sources(sources)


# ---------------------------------------------------------------------------
# Conflict analysis
# ---------------------------------------------------------------------------

def _distinct(vals):
    return sorted({v for v in vals if v not in (gi.WILD, "D")})


def analyse_gate(sites):
    """sites: the classified reader sites of ONE gate -> analysis dict or None.

    Sites are compared within a family only: a numeric knob and a presence trace
    of the same name are different questions, not a disagreement about one.
    """
    per_family = defaultdict(list)
    for s in sites:
        if s["status"] == "reader" and s["vector"]:
            per_family[s["family"]].append(s)
    # A mode read through atoi that only ever yields 0 or 1 IS a boolean reader, and
    # `v[0] != '0'` elsewhere disagrees with it on "false" (atoi gives 0, the
    # character test says on). Such a site joins the bool comparison.
    compare = {f: list(g) for f, g in per_family.items()}
    for s in per_family.get("num", []):
        proj = int_mode_projection(s["vector"], s["parsers"])
        if proj:
            compare.setdefault("bool", []).append(dict(s, vector=proj, cls=s["cls"] + " (int mode)"))
    disagreements = []
    for fam in sorted(compare):
        group = compare[fam]
        if len(group) < 2:
            continue
        for i, probe in enumerate(gi.PROBE_LABELS):
            by_val = defaultdict(list)
            for s in group:
                v = s["vector"][i]
                if v not in (gi.WILD, "D"):
                    by_val[v].append(s)
            if len(by_val) > 1:
                disagreements.append({
                    "family": fam, "probe": probe,
                    "outcomes": {v: sorted({"%s:%d" % (x["file"], x["line"]) for x in xs})
                                 for v, xs in sorted(by_val.items())},
                    "classes": {v: sorted({x["cls"] for x in xs}) for v, xs in sorted(by_val.items())},
                })
    return {"families": {f: len(g) for f, g in per_family.items()},
            "disagreements": disagreements}


_INT_PARSERS = {"atoi", "atol", "atoll", "_atoi64", "stoi", "stol", "stoll", "strtol", "strtoll"}


def int_mode_projection(vec, parsers):
    """Bool view of an integer-parsed reader with a known default of 0 or 1 whose
    first six answers are all 0 or 1 (a mode flag spelled as an int). Float knobs,
    seeds and frame numbers are left alone: reading `=0` as the value 0 is correct
    for them and says nothing about a flag."""
    head = vec[:6]
    if not parsers or any(p not in _INT_PARSERS for p in parsers):
        return None
    if vec[0] not in ("0", "1") or "1" not in head \
            or any(v not in ("0", "1", "D", gi.WILD) for v in head):
        return None
    out = []
    for v in vec:
        if v in ("D", gi.WILD):
            out.append(v)
        else:
            try:
                out.append("on" if float(v) != 0 else "off")
            except ValueError:
                return None
    return out


def _label_inputs(disagreements):
    """Probe labels, with the two X sub-probes folded into the 'X' class."""
    out = []
    for d in disagreements:
        p = {"X10": "X(\"10\")", "X25": "X(\"2.5\")"}.get(d["probe"], d["probe"])
        if p not in out:
            out.append(p)
    return out


def _std_vector(default_on):
    d = "on" if default_on else "off"
    return [d, d, "off", "off", "on", "on", "on", "on"]


def propose(gate, readers, reg):
    """-> (rule text, proposed vector or None, note) for one conflicting gate."""
    fams = {s["family"] for s in readers}
    if fams == {"num"} or ("num" in fams and "bool" not in fams):
        defaults = sorted({s["vector"][0] for s in readers
                           if s["family"] == "num" and s["vector"][0] not in ("D", gi.WILD)})
        parsers = sorted({p for s in readers if s["family"] == "num" for p in s["parsers"]})
        note = "readers use %s" % "/".join(parsers) if parsers else ""
        if len(defaults) > 1:
            note += "; defaults disagree: %s" % ", ".join(defaults)
        return ("Int/Float row: whole-string parse, out-of-range falls back to the one default"
                + (" (%s)" % defaults[0] if len(defaults) == 1 else ""), None, note)
    if fams == {"str"}:
        return "String row: NonEmpty (empty counts as unset)", None, ""
    bool_readers = [s for s in readers if s["family"] == "bool"]
    if any(gi.INH in s["vector"] for s in bool_readers):
        return ("Tri row: exact \"1\"/\"0\" wins, else inherit the umbrella gate "
                "(owner decision: which readers inherit)", None, "umbrella inheritance")
    if any(s["cls"].startswith("Custom") for s in bool_readers):
        levelish = [s["cls"] for s in bool_readers if s["cls"].startswith("Custom")]
        return ("Enum/Int row: a reader compares a leading character (levels), so a Bool row "
                "would collapse them (owner decision)", None,
                "custom outcome vector(s): " + ", ".join(sorted(set(levelish))))
    if gate in reg:
        default_on = reg[gate][0]
        basis = "registry defaultOn=%s" % str(default_on).lower()
    else:
        u = [s["vector"][0] for s in bool_readers if s["vector"][0] in ("on", "off")]
        default_on = u.count("on") > u.count("off")
        basis = "unregistered; unset outcome taken from the majority of readers (%d on / %d off)" % (
            u.count("on"), u.count("off"))
    rule = "Std(unset=%s)" % ("on" if default_on else "off")
    return rule, _std_vector(default_on), basis


def _registry_mismatch(reg_entry, readers):
    """Readers whose unset outcome contradicts the registry's stated default."""
    if not reg_entry:
        return []
    want = "on" if reg_entry[0] else "off"
    return ["%s:%d %s" % (s["file"], s["line"], s["cls"]) for s in
            sorted(readers, key=lambda x: (x["file"], x["line"]))
            if s["family"] == "bool" and not s["inverted"] and not s["cls"].startswith("Custom")
            and s["vector"][0] in ("on", "off")
            and s["vector"][0] != want]


def behaviour_delta(readers, proposed):
    """Which probes change per reader class if every bool reader moved to `proposed`."""
    if proposed is None:
        return []
    groups = defaultdict(lambda: {"sites": [], "changes": {}})
    for s in readers:
        if s["family"] != "bool":
            continue
        ch = {}
        for i, p in enumerate(gi.PROBE_LABELS):
            old = s["vector"][i]
            if old in (gi.WILD, "D") or old == proposed[i]:
                continue
            ch[p] = "%s->%s" % (old, proposed[i])
        key = (s["cls"] + (" (inverted use)" if s["inverted"] else ""))
        g = groups[key]
        g["sites"].append("%s:%d" % (s["file"], s["line"]))
        for p, c in ch.items():
            g["changes"][p] = c
    out = []
    for key in sorted(groups):
        g = groups[key]
        out.append({"class": key, "sites": sorted(g["sites"]),
                    "changes": dict(sorted(g["changes"].items(), key=lambda kv: gi.PROBE_LABELS.index(kv[0])))})
    return out


# ---------------------------------------------------------------------------
# Is the gate set anywhere a routine run would see it?
# ---------------------------------------------------------------------------

_NAME_RE = re.compile(r"MC2_[A-Z0-9_]+")
_ASSIGN_TAIL_RE = re.compile(
    r"""^(?:["']?\]?\s*(?<![=!<>])=(?!=)\s*\S|["']?\s*:\s*["']?[\w.\-/\\$"{]|["']\s*,\s*["'\w$])""")


def _config_rank(rel):
    """Harness and launcher configs first, recorded run artifacts last."""
    artifact = ("/artifacts/" in rel or "/captures/" in rel or rel.startswith("run/")
                or "/results/" in rel or "_manifest" in rel)
    return (1 if artifact else 0, rel)


def config_assignments(tree):
    """-> ({gate: config files that SET it, harness configs first}, {gates the tier1 doc sets}).

    Only assignment-shaped mentions count (`MC2_X=1`, `"MC2_X": "1"`,
    `$env:MC2_X = ...`, `env["MC2_X"] = ...`): a name in a list or a comment does
    not make a routine run see the disagreement.
    """
    sets = defaultdict(set)
    doc = set()
    rels = set(tree.list(CONFIG_DIRS, CONFIG_EXTS))
    rels |= set(tree.list(CONFIG_DIRS, CONFIG_ROOT_EXTS, root_level=True))
    rels = sorted(r for r in rels if r not in CONFIG_EXCLUDE
                  and not r.startswith("scripts/tests/fixtures/")
                  and "/goldens/" not in r and "node_modules" not in r)
    texts = tree.read_all(rels + [TIER1_DOC.replace("\\", "/")])
    for rel in rels:
        text = texts.get(rel)
        if not text or len(text) > 2_000_000:
            continue
        for m in _NAME_RE.finditer(text):
            if _ASSIGN_TAIL_RE.match(text[m.end():m.end() + 24]):
                sets[m.group(0)].add(rel)
    text = texts.get(TIER1_DOC.replace("\\", "/"), "")
    for m in _NAME_RE.finditer(text):
        if _ASSIGN_TAIL_RE.match(text[m.end():m.end() + 24]):
            doc.add(m.group(0))
    return {k: sorted(v, key=_config_rank) for k, v in sets.items()}, doc


# ---------------------------------------------------------------------------
# Build the full report
# ---------------------------------------------------------------------------

def load_ledger(tree_root):
    p = os.path.join(tree_root, LEDGER)
    if not os.path.isfile(p):
        return {}
    with open(p, "r", encoding="utf-8") as fh:
        return json.load(fh).get("gates", {})


# ---------------------------------------------------------------------------
# VK umbrella members: the one allowed reader class is Typed(effective)
# ---------------------------------------------------------------------------

#: How a Tri row that inherits the umbrella may be named in C++: through the adapter (armed<>,
#: regionTier1Armed<>), the raw state, or the adapter's own gate::effective<>. A getenv, a legacy helper
#: or a plain get<> would read the member without the umbrella (or without the fail-soft veto).
_UMBRELLA_TYPED_RE = re.compile(r'\b(?:armed|regionTier1Armed|effective|state|hasParent)\s*<\s*$')


def umbrella_members(tree):
    """Names of the gate-table rows that declare `inherits` (read from the committed JSON)."""
    try:
        doc = json.loads(tree.read("Gates/gate_table.json") or "{}")
    except ValueError:
        return []
    return sorted(g["name"] for g in doc.get("gates", []) if g.get("inherits"))


def umbrella_report(tree, scan_dirs=None):
    """-> {members, typed_sites, violations}: every literal of an umbrella member in the scanned C++, split into
    Typed(effective) sites and violations (any other way of naming it)."""
    members = umbrella_members(tree)
    scan_dirs = SCAN_DIRS if scan_dirs is None else scan_dirs
    reg = REGISTRY.replace("\\", "/")   # the generated feature table names every gate
    rels = [r for r in tree.list(scan_dirs, gi.SCAN_EXTS) if r != reg]
    texts = tree.read_all(rels)
    typed, bad = 0, []
    for rel in rels:
        if rel not in texts:
            continue
        code = gi.strip_comments(texts[rel])
        for m in re.finditer(r'"(MC2_[A-Z0-9_]+)"', code):
            if m.group(1) not in members:
                continue
            before = code[max(0, m.start() - 40):m.start()]
            if _UMBRELLA_TYPED_RE.search(before):
                typed += 1
            else:
                bad.append({"gate": m.group(1), "file": rel, "line": code.count(chr(10), 0, m.start()) + 1})
    return {"members": members, "typed_sites": typed, "violations": bad}



def build_report(tree, scan_dirs=None, with_config=True, ledger=None):
    sites, stats = scan_tree(tree, scan_dirs)
    reg = load_registry(tree)
    cfg_sets, cfg_doc = config_assignments(tree) if with_config else ({}, set())

    by_gate = defaultdict(list)
    for s in sites:
        by_gate[s["gate"]].append(s)

    gates = {}
    status_counts = Counter(s["status"].split(":")[0] if not s["status"].startswith("unclassified")
                            else s["status"] for s in sites)
    for gate in sorted(by_gate):
        ss = by_gate[gate]
        a = analyse_gate(ss)
        readers = [s for s in ss if s["status"] == "reader"]
        entry = {
            "reader_sites": len(readers),
            "families": a["families"],
            "conflict": bool(a["disagreements"]),
            "registry": ({"default_on": reg[gate][0], "kind": reg[gate][1]} if gate in reg else None),
        }
        if entry["conflict"]:
            rule, proposed, basis = propose(gate, readers, reg)
            fam_readers = [s for s in readers if s["family"] in {d["family"] for d in a["disagreements"]}]
            sets_in = cfg_sets.get(gate, [])
            entry.update({
                "sites": [{"file": s["file"], "line": s["line"], "callee": s["callee"],
                           "family": s["family"], "class": s["cls"], "inverted": s["inverted"],
                           "vector": s["vector"]} for s in sorted(readers, key=lambda x: (x["file"], x["line"]))],
                "disagreements": a["disagreements"],
                "disagree_inputs": _label_inputs(a["disagreements"]),
                "default_visible": ("yes" if sets_in else ("doc-only" if gate in cfg_doc else "no")),
                "set_in": sets_in,
                "kill_style_name": bool(_INVERTED_NAME_RE.search(gate)),
                "contradicts_registry": _registry_mismatch(reg.get(gate), readers),
                "proposed_rule": rule,
                "proposal_basis": basis,
                "behaviour_delta": behaviour_delta(fam_readers, proposed),
            })
        gates[gate] = entry

    conflicts = sorted(g for g, e in gates.items() if e["conflict"])
    ledger = ledger or {}
    unresolved = [g for g in conflicts if ledger.get(g, {}).get("status") not in LEDGER_ACCEPTED]
    regressed = [g for g in conflicts if ledger.get(g, {}).get("status") == "resolved"]
    ledger_rows = []
    for g in sorted(ledger):
        readers = [s for s in by_gate.get(g, []) if s["status"] in ("reader", "token")]
        e = ledger[g]
        ledger_rows.append({
            "gate": g, "status": e.get("status", ""), "rule": e.get("rule", ""),
            "delta": e.get("delta", ""), "evidence": e.get("evidence", ""),
            "in_conflict": g in conflicts,
            "readers": ["%s:%d %s" % (s["file"], s["line"], s["cls"])
                        for s in sorted(readers, key=lambda x: (x["file"], x["line"]))]})
    unclassified = [s for s in sites if s["status"].startswith("unclassified")]
    umbrella = umbrella_report(tree, scan_dirs)
    report = {
        "version": 1,
        "source": tree.describe(),
        "scan_dirs": scan_dirs or SCAN_DIRS,
        "probes": list(gi.PROBE_LABELS),
        "totals": {
            "gates_with_literal": len(stats["gates_with_literal"]),
            "gates_with_reader": len({s["gate"] for s in sites if s["status"] in ("reader", "token")}),
            "literal_occurrences": stats["literals"],
            "non_call_literals": stats["noncall"],
            "read_sites": len(sites),
            "reader_sites": status_counts.get("reader", 0),
            "token_sites": status_counts.get("token", 0),
            "nonreader_sites": status_counts.get("nonreader", 0),
            "unclassified_sites": len(unclassified),
            "conflict_gates": len(conflicts),
            "unresolved_conflicts": len(unresolved),
            "umbrella_members": len(umbrella["members"]),
            "umbrella_typed_sites": umbrella["typed_sites"],
            "umbrella_violations": len(umbrella["violations"]),
        },
        "umbrella_violations": umbrella["violations"],
        "conflict_gates": conflicts,
        "unresolved_gates": unresolved,
        "regressed_gates": regressed,
        "g0_ledger": ledger_rows,
        "gates": {g: gates[g] for g in conflicts},
        "unclassified": [{"gate": s["gate"], "file": s["file"], "line": s["line"],
                          "callee": s["callee"], "why": s["status"]} for s in unclassified],
        "class_histogram": dict(Counter(s["cls"] for s in sites if s["status"] == "reader").most_common()),
        "design_s4": check_design_s4(by_gate, gates),
    }
    return report


def check_design_s4(by_gate, gates):
    """Confirm or correct the nine conflicts (and their line numbers) in design section 4."""
    out = []
    for gate, claims in DESIGN_S4:
        ss = [s for s in by_gate.get(gate, []) if s["status"] in ("reader", "token")]
        rows = []
        for suffix, line in claims:
            near = [s for s in ss if s["file"].endswith(suffix)]
            exact = [s for s in near if abs(s["line"] - line) <= 1]
            if exact:
                rows.append({"claim": "%s:%d" % (suffix, line), "verdict": "confirmed",
                             "actual": "%s:%d" % (exact[0]["file"], exact[0]["line"]),
                             "class": exact[0]["cls"]})
            elif near:
                n = min(near, key=lambda s: abs(s["line"] - line))
                rows.append({"claim": "%s:%d" % (suffix, line), "verdict": "corrected",
                             "actual": "%s:%d" % (n["file"], n["line"]), "class": n["cls"]})
            else:
                rows.append({"claim": "%s:%d" % (suffix, line), "verdict": "not-found",
                             "actual": "", "class": ""})
        out.append({"gate": gate, "in_conflict": bool(gates.get(gate, {}).get("conflict")),
                    "note": DESIGN_S4_NOTES.get(gate, ""),
                    "sites": rows,
                    "all_readers": ["%s:%d (%s)" % (s["file"], s["line"], s["cls"]) for s in
                                    sorted(ss, key=lambda x: (x["file"], x["line"]))]})
    return out


# ---------------------------------------------------------------------------
# Markdown
# ---------------------------------------------------------------------------

def _esc(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def render_md(rep):
    t = rep["totals"]
    L = []
    w = L.append
    w("# Gate reader conflicts\n")
    w("Every gate whose readers answer differently for at least one of the eight probe inputs. "
      "A typed gate table gives each gate one truthiness rule, so each row below is a place where "
      "that rule would change some reader's behaviour; G0 of the typed-gate-registry design unifies "
      "each one, or records it as an owner-decided divergence. Generated by `py -3 tools/gate_conflicts.py --write`; "
      "the ratchet is `tools/gate_conflicts.py --check` against `tools/gate_conflicts.baseline.json`.\n")
    w("Probes: **U** unset, **E** `\"\"`, **Z** `\"0\"`, **F** `\"false\"`, **O** `\"1\"`, **X** other "
      "(`\"yes\"`; the sub-probes `\"10\"` and `\"2.5\"` separate first-character and prefix-parse readers). "
      "Outcomes are oriented so that `\"1\"` asserts the gate: a reader that uses the variable as a "
      "disable flag is shown as the same test, marked *(inverted use)*. `D` in a vector means the "
      "reader keeps whatever default it already had; `?` means the answer depends on program state "
      "the tool cannot see; `inh` means the reader inherits an umbrella gate. Neither counts as a disagreement.\n")
    w("## Totals\n")
    w("Scanned: %s.\n" % rep["source"])
    w("| Measure | Count |\n|---|---|")
    for k, label in [("gates_with_literal", "gates named by a literal in scanned code"),
                     ("gates_with_reader", "gates with at least one classified reader"),
                     ("literal_occurrences", "`\"MC2_*\"` literal occurrences"),
                     ("non_call_literals", "literals not passed to an env read (tables, writers, indirect names)"),
                     ("read_sites", "read sites analysed"),
                     ("reader_sites", "reader sites with an outcome vector"),
                     ("token_sites", "reader sites comparing a token no probe hits (not comparable)"),
                     ("nonreader_sites", "sites where the probe decides nothing (prints, paths)"),
                     ("unclassified_sites", "sites the tool could not classify"),
                     ("conflict_gates", "**gates with conflicting readers**"),
                     ("unresolved_conflicts", "**of which unresolved** (not ledgered as `enum`, `divergent` or `intentional-optin`)"),
                     ("umbrella_members", "VK umbrella members (`inherits` rows)"),
                     ("umbrella_typed_sites", "Typed(effective) sites: a member named through `armed<>` / `regionTier1Armed<>` / `state<>`"),
                     ("umbrella_violations", "**umbrella members named any other way** (each is a failure)")]:
        w("| %s | %d |" % (label, t[k]))
    w("")
    w("## Design section 4: confirmed or corrected\n")
    w("Line numbers are checked against the scanned tree (see Totals for which); "
      "`corrected` means the reader is in the named file but has moved.\n")
    w("| Gate | Reader-vs-reader conflict | Design claim | Verdict | Actual reader | Class | Note |\n|---|---|---|---|---|---|---|")
    for d in rep["design_s4"]:
        for i, r in enumerate(d["sites"]):
            w("| %s | %s | `%s` | %s | `%s` | %s | %s |" % (
                d["gate"] if i == 0 else "", ("yes" if d["in_conflict"] else "**no**") if i == 0 else "",
                _esc(r["claim"]), r["verdict"], _esc(r["actual"]), _esc(r["class"]),
                _esc(d["note"]) if i == 0 else ""))
    w("")
    w("## Conflicting gates\n")
    w("| Gate | Reader sites (file:line, class) | Inputs that disagree | Default-visible? | Proposed unified rule | Behaviour delta if unified |")
    w("|---|---|---|---|---|---|")
    order = sorted(rep["conflict_gates"], key=lambda g: (-rep["gates"][g]["reader_sites"], g))
    for g in order:
        e = rep["gates"][g]
        sites = ["`%s:%d` %s%s" % (s["file"], s["line"], s["class"], " (inv)" if s["inverted"] else "")
                 for s in e["sites"]]
        shown = sites[:6] + (["... +%d more" % (len(sites) - 6)] if len(sites) > 6 else [])
        dis = []
        for d in e["disagreements"]:
            dis.append("%s: %s" % (d["probe"], " vs ".join("%s" % v for v in d["outcomes"])))
        dv = e["default_visible"]
        if e["set_in"]:
            dv += " (" + ", ".join(e["set_in"][:2]) + (", +%d" % (len(e["set_in"]) - 2) if len(e["set_in"]) > 2 else "") + ")"
        rule = e["proposed_rule"]
        if e["proposal_basis"]:
            rule += " -- " + e["proposal_basis"]
        if e["kill_style_name"]:
            rule += " [name implies a kill flag: check complements are intentional]"
        if e["contradicts_registry"]:
            rule += " [CONTRADICTS REGISTRY: %s]" % ", ".join(e["contradicts_registry"])
        delta = []
        for c in e["behaviour_delta"]:
            if c["changes"]:
                delta.append("%s x%d: %s" % (c["class"], len(c["sites"]),
                                             ", ".join("%s %s" % (p, v) for p, v in c["changes"].items())))
        w("| `%s` | %s | %s | %s | %s | %s |" % (
            g, "<br>".join(_esc(x) for x in shown), _esc("; ".join(dis)), _esc(dv), _esc(rule),
            "<br>".join(_esc(x) for x in delta) if delta else ("none" if e["behaviour_delta"] else "n/a (owner decision)")))
    w("")
    w("## G0 resolution ledger\n")
    w("Source: `tools/gate_g0_ledger.json`. `resolved` = the readers now agree; `enum` = owner-intended "
      "level values, code unchanged; `divergent` = the readers' defaults differ and the owner decides; "
      "`note` = a single-reader hazard recorded without a code change.\n")
    w("| Gate | Status | Still conflicting | Readers | Chosen rule | Behaviour delta | Evidence |")
    w("|---|---|---|---|---|---|---|")
    for r in rep["g0_ledger"]:
        shown = r["readers"][:6] + (["... +%d more" % (len(r["readers"]) - 6)] if len(r["readers"]) > 6 else [])
        w("| `%s` | %s | %s | %s | %s | %s | %s |" % (
            r["gate"], r["status"], "yes" if r["in_conflict"] else "no",
            "<br>".join("`%s`" % _esc(x) for x in shown), _esc(r["rule"]), _esc(r["delta"]), _esc(r["evidence"])))
    w("")
    w("## Reader idiom classes\n")
    w("| Class | Reader sites |\n|---|---|")
    for k, v in rep["class_histogram"].items():
        w("| %s | %d |" % (_esc(k), v))
    w("")
    w("## Sites the tool could not classify (%d)\n" % t["unclassified_sites"])
    w("Mostly string- and path-valued reads whose only use is a library call (`fopen`, `snprintf`). "
      "They are listed in the JSON; the ratchet forbids the number rising so a new idiom cannot hide.\n")
    w("## Method and limits\n")
    w("- The reader's C++ is executed, not pattern-matched; the interpreter is `scripts/gate_idioms.py` "
      "and shares its source plumbing with `scripts/gate_default_classifier.py`.")
    w("- A condition on some other piece of program state is treated as the identity of `&&`/`||`: "
      "`getenv(X) && frame % 64 == 0` is a presence test as far as X goes.")
    w("- Table-driven reads (`kEnvGates[i].name` passed to getenv later) and names built at run time "
      "are invisible to a literal scan; they are counted under non-call literals.")
    w("- Reads of the same gate in different processes (editor, tools) are compared too; whether two "
      "readers can ever coexist in one run is a G0 judgement, not this tool's.")
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------
# Ratchet
# ---------------------------------------------------------------------------

def load_baseline(root):
    p = os.path.join(root, BASELINE)
    if not os.path.isfile(p):
        return None
    with open(p, "r", encoding="utf-8") as fh:
        return json.load(fh)


def baseline_from(rep):
    return {"version": 1,
            "note": "Ratchet for tools/gate_conflicts.py --check: both numbers may only fall.",
            "conflict_gates": rep["conflict_gates"],
            "unresolved_conflicts": rep["totals"]["unresolved_conflicts"],
            "unclassified_sites": rep["totals"]["unclassified_sites"]}


def check(rep, base):
    """-> list of failure strings (empty = ok)."""
    if base is None:
        return ["no baseline at %s: run `py -3 tools/gate_conflicts.py --write-baseline`" % BASELINE]
    fails = []
    cur, old = rep["conflict_gates"], base["conflict_gates"]
    if len(cur) > len(old):
        fails.append("conflicting gates rose: %d (baseline %d)" % (len(cur), len(old)))
    new = sorted(set(cur) - set(old))
    if new:
        fails.append("gates that were not in conflict are now: " + ", ".join(new))
    cur_unres = rep["totals"].get("unresolved_conflicts", len(cur))
    old_unres = base.get("unresolved_conflicts", len(old))
    if cur_unres > old_unres:
        fails.append("unresolved conflicts rose: %d (baseline %d)" % (cur_unres, old_unres))
    for v in rep.get("umbrella_violations", []):
        fails.append("%s is a VK umbrella member (inherits) but %s:%d names it outside armed<>/regionTier1Armed<>/"
                     "state<>/effective<> -- that reader ignores the umbrella or the fail-soft veto"
                     % (v["gate"], v["file"], v["line"]))
    for g in rep.get("regressed_gates", []):
        fails.append("%s was resolved by G0 and its readers disagree again" % g)
    if rep["totals"]["unclassified_sites"] > base["unclassified_sites"]:
        fails.append("unclassified reader sites rose: %d (baseline %d) -- a new idiom the tool "
                     "cannot read" % (rep["totals"]["unclassified_sites"], base["unclassified_sites"]))
    return fails


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--write", action="store_true", help="write docs/gate-reader-conflicts.{md,json}")
    ap.add_argument("--check", action="store_true", help="ratchet against the baseline file")
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--allow-raise", action="store_true",
                    help="with --write-baseline: permit recording a higher count")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--worktree", action="store_true",
                    help="scan the working tree instead of the committed tree (HEAD)")
    args = ap.parse_args(argv)

    rep = build_report(open_tree(args.root, args.worktree), with_config=not args.check,
                       ledger=load_ledger(args.root))
    t = rep["totals"]

    if args.write:
        with open(os.path.join(args.root, OUT_MD), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(render_md(rep))
        with open(os.path.join(args.root, OUT_JSON), "w", encoding="utf-8", newline="\n") as fh:
            json.dump(rep, fh, indent=1, sort_keys=True)
            fh.write("\n")

    if args.write_baseline:
        old = load_baseline(args.root)
        new = baseline_from(rep)
        if old and not args.allow_raise and (len(new["conflict_gates"]) > len(old["conflict_gates"])
                                             or new["unclassified_sites"] > old["unclassified_sites"]):
            print("refusing to RAISE the baseline (conflicts %d -> %d, unclassified %d -> %d); "
                  "fix the readers, or pass --allow-raise with a reason in the commit"
                  % (len(old["conflict_gates"]), len(new["conflict_gates"]),
                     old["unclassified_sites"], new["unclassified_sites"]))
            return 1
        with open(os.path.join(args.root, BASELINE), "w", encoding="utf-8", newline="\n") as fh:
            json.dump(new, fh, indent=1)
            fh.write("\n")

    if args.check:
        fails = check(rep, load_baseline(args.root))
        if fails:
            print("GATE-READER-CONFLICTS ratchet FAILED")
            for f in fails:
                print("  - " + f)
            print("  Two readers of one gate now disagree about what a value means. Unify them "
                  "(see docs/gate-reader-conflicts.md), or -- for a removal -- lower the baseline "
                  "with --write-baseline.")
            return 1
        if not args.quiet:
            print("gate_conflicts: %d conflicting gates (baseline %d), %d unclassified (baseline %d)"
                  % (t["conflict_gates"], len(load_baseline(args.root)["conflict_gates"]),
                     t["unclassified_sites"], load_baseline(args.root)["unclassified_sites"]))
        return 0

    if not args.quiet:
        print("gates named by a literal : %d" % t["gates_with_literal"])
        print("gates with a reader      : %d" % t["gates_with_reader"])
        print("read sites analysed      : %d (reader %d, token %d, nonreader %d, unclassified %d)" % (
            t["read_sites"], t["reader_sites"], t["token_sites"], t["nonreader_sites"], t["unclassified_sites"]))
        print("CONFLICTING GATES        : %d" % t["conflict_gates"])
        print("  unresolved (not enum/divergent): %d" % t["unresolved_conflicts"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
