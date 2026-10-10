#!/usr/bin/env python3
"""gl_vk_feature_coverage.py -- does the Vulkan twin of a GL shader carry the GL feature a gate drives?

WHY: the VK ports were made against the GL shaders as they stood, and the default-OFF GL
features were left behind (sky-exclude in fog_oob/edge_fog, shore meander in water, the
sun-axis fix in the mech lighting). Gate amnesty flips default-OFF gates ON; for every gate
whose feature the VK twin lacks, that flip changes GL only and VK silently diverges.

This tool finds those gaps ahead of the flip:

  1. derives the GL<->VK shader twin table            (tools/gl_vk_twins.py)
  2. for each GL shader, lists the uniforms / compile-time toggles an MC2_* gate drives
                                                      (tools/gl_vk_cpp_gates.py)
  3. checks whether the VK twin (shader + includes, VK island C++, gpu_abi) carries the same
     name, and writes docs/gl-vk-shader-twins.md, docs/gl-vk-feature-coverage.md/.json
  4. enforces tools/vk_not_ported.allowlist: a NOT_PORTED row with no entry is a NEW gap, an
     entry whose row is gone is STALE (mirrors tools/glsl_port_diff.allowlist)
  5. --flip-check BASE: fails a commit that turns a listed gate's default ON

Usage:
  py -3 tools/gl_vk_feature_coverage.py                 # grade the working tree vs the allowlist
  py -3 tools/gl_vk_feature_coverage.py --write         # regenerate the three docs
  py -3 tools/gl_vk_feature_coverage.py --check         # also fail if the docs are stale
  py -3 tools/gl_vk_feature_coverage.py --ref HEAD      # grade a git revision, not the tree
  py -3 tools/gl_vk_feature_coverage.py --flip-check HEAD~1
Exit 0 ok; 1 new gap / stale entry / stale docs / flipped gap; 2 harness error.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gl_vk_cpp_gates as cg  # noqa: E402
import gl_vk_twins as tw  # noqa: E402
from gl_vk_source import ROOT, Source, blank_comments  # noqa: E402

ALLOWLIST = "tools/vk_not_ported.allowlist"
DOC_TWINS = "docs/gl-vk-shader-twins.md"
DOC_COV = "docs/gl-vk-feature-coverage.md"
DOC_JSON = "docs/gl-vk-feature-coverage.json"
VARIANTS = "scripts/gl_shader_variants.json"
GATE_SEED = "tools/gate_table.seed.json"
REGISTRY_H = "Gates/gate_table.json"   # the compiled gate table; its registry rows carry defaultOn
CPP_DIRS = ("GameOS/", "mclib/", "code/", "RenderCore/", "RenderWorld/", "GameAdapters/", "gui/", "HostServices/")
VK_CPP_GLOBS = (("GameOS/gameos/vulkan_", (".cpp", ".h")), ("GameOS/gameos/engine_present_vk", (".cpp", ".h")),
                ("RenderCore/gpu_abi/", (".h",)))
MIN_STEM = 6            # shorter stems ("tex", "mode") match too much to count as evidence
RANK = {"HIGH": 3, "MEDIUM": 2, "LOW": 1}
_IDENT = re.compile(r"[A-Za-z_]\w*")
_SLOT_RE = re.compile(r"^[ \t]*(?:[iu]?vec[234]|mat[234]|float|int|uint)[ \t]+([A-Za-z_]\w*)(?:\[[^\]]*\])?"
                      r"[ \t]*;[ \t]*//(.*)$", re.M)


# --- inputs -----------------------------------------------------------------

def gate_info(src):
    """{gate: {'default_on': bool, 'kind': str}} from the typed gate seed."""
    seed = json.loads(src.text(GATE_SEED) or '{"gates": []}')
    out = {}
    for g in seed["gates"]:
        d = g.get("default")
        on = d if isinstance(d, bool) else (d != 0 if isinstance(d, (int, float)) else False)
        out[g["name"]] = {"default_on": bool(on), "kind": g.get("registryKind") or "unregistered"}
    return out


def load_cpp(src):
    paths = [p for p in src.paths() if p.endswith((".cpp", ".h", ".hpp")) and p.startswith(CPP_DIRS)
             and "/tests/" not in p and not p.startswith("GameOS/gameos/vulkan_")]
    src.preload(paths)
    known = set(src.paths())
    files, ndefs = {}, {}
    family = {}          # translation unit -> [text of every piece]: a uniform is named in one piece and set in another
    for p in paths:
        t = src.text(p)
        if not t:
            continue
        fam = tu_family(p, known)
        cg.count_defs(fam, blank_comments(t, keep_strings=False), ndefs)   # a split unit counts as one file
        if fam != p:
            family.setdefault(fam, []).append(t)
        elif "MC2_" in t or '"u_' in t:
            files[p] = cg.CppFile(p, t)
    for fam, texts in family.items():
        if fam in known:
            texts.insert(0, src.text(fam) or "")
        merged = chr(10).join(texts)
        if "MC2_" in merged or '"u_' in merged:
            files[fam] = cg.CppFile(fam, merged)
    return files, ndefs


def port_renames(root):
    """{vk path: [(regex, repl)]} from glsl_port_diff's registry: the VK->GL spelling folds."""
    pd = tw._load_module(os.path.join(root, "tools", "glsl_port_diff.py"), "_glsl_port_diff_ren")
    out = {}
    for port in pd.REGISTRY:
        out.setdefault(port.vk, []).extend(port.rename)
    return out


def _tokens(text):
    return set(_IDENT.findall(text))


class VkEvidence:
    def __init__(self, src, twins, renames):
        self.src, self.twins, self.renames = src, twins, renames
        self._file_tok = {}
        cpp = []
        for prefix, exts in VK_CPP_GLOBS:
            cpp += [p for p in src.paths() if p.startswith(prefix) and p.endswith(exts)]
        src.preload(cpp)
        self.cpp_tok = set()
        for p in cpp:
            self.cpp_tok |= _tokens(blank_comments(src.text(p) or ""))

    def _load(self, vk_path):
        if vk_path not in self._file_tok:
            text, _ = tw.expand_includes(self.src, vk_path)
            for rx, repl in self.renames.get(vk_path, []):
                text = re.sub(rx, repl, text)
            raw, _ = tw.expand_includes(self.src, vk_path, keep_comments=True)
            slots = {}
            for member, comment in _SLOT_RE.findall(raw):
                if re.search(r"[A-Za-z_]\w*\s*\.\s*" + re.escape(member) + "(?![A-Za-z0-9_])", text):
                    for tok in _tokens(comment):
                        slots.setdefault(tok, member)
            self._file_tok[vk_path] = (_tokens(text), slots)
        return self._file_tok[vk_path]

    def shader_tokens(self, vk_path):
        return self._load(vk_path)[0]

    def packed_slots(self, vk_path):
        """{documented name: UBO member} for members declared with a `// (a, b, c, d)` comment
        and read in code, e.g. the water FS packs u_waterEdgeFeather into F.f8."""
        return self._load(vk_path)[1]

    def family_files(self, gl_path):
        out = []
        for vs in self.twins.vk_family_for_gl(gl_path):
            out += [v for v in self.twins.vk_files if tw.split_stem(v)[0] == vs]
        return sorted(out)

    def check(self, gl_path, name, kind):
        """(found, how, vk file, spelling) for a feature name against the VK twin family of gl_path."""
        forms = [(name, "exact")]
        if kind == "uniform":
            forms += [(s, "stem") for s in stems_of(name)]
        files = self.family_files(gl_path)
        for f in files:
            toks = self.shader_tokens(f)
            for form, how in forms:
                if form in toks:
                    return True, how, f, form
        for f in files:
            if name in self.packed_slots(f):
                return True, "packed", f, self.packed_slots(f)[name]
        in_cpp = name in self.cpp_tok       # exact only: a stem like `debugMode` collides across islands
        return False, ("cpp-only" if in_cpp else "absent"), "", ""


# --- row building -----------------------------------------------------------

_WORD_RE = re.compile(r"[A-Z]?[a-z0-9]+|[A-Z]+(?![a-z])")


def stems_of(name):
    """Spellings a VK port plausibly uses for a GL uniform: the name without `u_`, and that with
    leading domain words dropped (u_terrainAmbientFloor -> ambientFloor, as the terrain UBO does).
    Never fewer than two words, so a bare `floor` or `mode` cannot count as evidence."""
    base = name[2:] if name.startswith("u_") else name
    words = _WORD_RE.findall(base)
    out = []
    if name.startswith("u_") and len(base) >= MIN_STEM:
        out.append(base)
    for k in range(1, len(words) - 1):
        rest = words[k:]
        s = rest[0].lower() + "".join(rest[1:])
        if len(s) >= MIN_STEM + 2 and not s.startswith("debug"):
            out.append(s)
    return out


def _best(a, b):
    return a if RANK[a] <= RANK[b] else b      # the weaker of two confidences


def _conf(hops):
    return None if hops > 4 else cg.hops_to_conf(hops)


_PIECE_RE = re.compile(r"^(.*/)?([^/]+?)_[a-z0-9]+\.cpp$")


def tu_family(path, known):
    """The translation unit a split piece belongs to: GameOS/gameos/gos_postprocess_fbo.cpp -> gos_postprocess.cpp when
    gos_postprocess_internal.h exists. A shader's program is built in one piece and its uniforms are set in another,
    so "builds this shader's program" is a question about the whole family."""
    m = _PIECE_RE.match(path)
    if m:
        # <stem>_<part>.cpp, or a piece of a piece (<stem>_<part>_<sub>.cpp): the longest stem whose internal header exists
        stem = m.group(2)
        while True:
            if "%s%s_internal.h" % (m.group(1) or "", stem) in known:
                return "%s%s.cpp" % (m.group(1) or "", stem)
            if "_" not in stem:
                break
            stem = stem.rsplit("_", 1)[0]
    if path.endswith("_internal.h"):
        base = path[:-len("_internal.h")]
        for cand in (base, base.rsplit("_", 1)[0]):          # <stem>_internal.h or <stem>_<part>_internal.h
            if cand != path and cand + ".cpp" in known:
                return cand + ".cpp"                          # the shared header of a split unit holds class bodies of that unit
    return path


def build_rows(src, twins, files, ginfo, ndefs=None):
    known = set(src.paths())
    index = cg.GateIndex(files, ndefs)
    ev = VkEvidence(src, twins, port_renames(src.root))
    rows = {}

    def add(gate, gl, feat, kind, conf, how_gate):
        info = ginfo.get(gate, {"default_on": False, "kind": "unregistered"})
        if info["kind"] in ("Trace", "Retired"):
            return
        fam = twins.vk_family_for_gl(gl)
        if not fam:
            status, vk, spelling = "NO_TWIN", "", ""
            c = conf
        else:
            found, how, vkf, spelling = ev.check(gl, feat, kind)
            spelling = "%s:%s" % (how, spelling) if found else how
            status = "PORTED" if found else "NOT_PORTED"
            vk = vkf or ", ".join("shaders/vulkan/%s.*" % s for s in fam)
            c = conf
            if found and how in ("stem", "packed"):
                c = _best(c, "MEDIUM")
            if not found and how == "cpp-only":
                c = _best(c, "MEDIUM")
        key = (gate, gl, feat, kind)
        old = rows.get(key)
        if old is None or RANK[c] > RANK[old["confidence"]]:
            rows[key] = {"gate": gate, "gl_shader": gl, "feature": feat, "kind": kind,
                         "vk_twin": vk if status != "NO_TWIN" else "", "status": status,
                         "confidence": c, "gate_default_on": info["default_on"],
                         "gate_evidence": how_gate, "vk_evidence": spelling}

    # uniforms: every non-sampler default-block uniform of every GL shader
    gl_uniforms = {}
    gl_toggles = {}
    for g in twins.gl_files:
        text, _ = tw.expand_includes(src, g)
        gl_uniforms[g] = tw.uniforms_of(text)
        gl_toggles[g] = tw.toggles_of(text)
    manifest = json.loads(src.text(VARIANTS) or '{"variants": []}')
    prog_src = {}      # GL shader file -> C++ files that build a program containing it
    for v in manifest["variants"]:
        for sf in v["stages"].values():
            prog_src.setdefault(sf, set()).add(tu_family(v["source"].split(":")[0], known))
    names = sorted(set().union(*gl_uniforms.values()))
    for name in names:
        sites = []    # (gate, conf, path, hops)
        for path, off in cg.feature_sites(files, name):
            for gate, hops in index.attribute(path, off).items():
                c = _conf(hops)
                if c:
                    sites.append((gate, c, path, hops))
            for gate, hops in index.attribute_cond(path, off).items():
                c = _conf(hops)
                if c:
                    sites.append((gate, _best(c, "MEDIUM"), path, hops))   # enclosing condition only
        for g in twins.gl_files:
            if name not in gl_uniforms[g]:
                continue
            best = {}
            for gate, c, path, hops in sites:
                # a setter in a file that builds none of this shader's programs is probably
                # feeding another program that declares the same uniform name
                if prog_src.get(g) and tu_family(path, known) not in prog_src[g]:
                    c = _best(c, "MEDIUM")
                if gate not in best or RANK[c] > RANK[best[gate][0]]:
                    best[gate] = (c, "%s (%d hop%s)" % (path, hops, "" if hops == 1 else "s"))
            for gate, (c, how) in sorted(best.items()):
                add(gate, g, name, "uniform", c, how)

    # compile-time toggles: optional #define groups the C++ injects, from the variant manifest
    by_id = {}
    for v in manifest["variants"]:
        by_id.setdefault(v["id"], []).append(v)
    for pid, vs in sorted(by_id.items()):
        base = next((v for v in vs if not v["optional_groups_on"]), None)
        if base is None:
            continue
        base_def = {d.split("=")[0] for d in base["defines"]}
        stage_files = sorted(set(base["stages"].values()))
        src_path = tu_family(base["source"].split(":")[0], known)     # a program built in a split piece is read as its whole unit
        for v in vs:
            if len(v["optional_groups_on"]) != 1:
                continue
            cond = v["optional_groups_on"][0]
            new_defs = {d.split("=")[0] for d in v["defines"]} - base_def
            if src_path not in files:
                continue
            for gate, hops in index.attribute_expr(src_path, cond).items():
                c = _conf(hops)
                if not c:
                    continue
                for d in sorted(new_defs):
                    for sf in stage_files:
                        if sf in gl_toggles and d in gl_toggles[sf]:
                            add(gate, sf, d, "define", c, "%s group '%s' (%d hop%s)" % (
                                src_path, cond, hops, "" if hops == 1 else "s"))
    return sorted(rows.values(), key=lambda r: (r["gate"], r["gl_shader"], r["feature"], r["kind"]))


# --- allowlist --------------------------------------------------------------

def load_allowlist(path):
    """{(gate, gl_shader): why}"""
    out = {}
    if not os.path.isfile(path):
        return out
    with open(path, "r", encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split("|", 2)]
            if len(parts) != 3 or not all(parts):
                raise ValueError("%s:%d: want 'gate | GL shader | WHY'" % (path, n))
            out[(parts[0], parts[1])] = parts[2]
    return out


def enforced(rows):
    """{(gate, gl_shader)} NOT_PORTED pairs the allowlist must account for. HIGH confidence only:
    a MEDIUM row rests on a long alias chain or a partial name match, and gating a commit on a
    guess teaches people to ignore the gate. MEDIUM rows stay visible in the generated docs."""
    return {(r["gate"], r["gl_shader"]) for r in rows
            if r["status"] == "NOT_PORTED" and r["confidence"] == "HIGH"}


def suggest_line(rows, gate, shader):
    """A ready-to-edit allowlist line for a NOT_PORTED pair, stating only what the tool established."""
    sel = [r for r in rows if r["gate"] == gate and r["gl_shader"] == shader and r["status"] == "NOT_PORTED"]
    feats = ", ".join(sorted({"%s %s" % (r["kind"], r["feature"]) for r in sel}))
    twin = sorted({r["vk_twin"] for r in sel})[0] if sel else "?"
    return "%s | %s | %s: no such name in the VK twin (%s) shader, island C++ or gpu_abi" % (
        gate, shader, feats, twin)


def grade(rows, allow):
    cur = enforced(rows)
    new = sorted(cur - set(allow))
    stale = sorted(set(allow) - cur)
    return new, stale


# --- documents --------------------------------------------------------------

def twins_doc(twins):
    L = ["# GL <-> VK shader twin table", "",
         "Generated by `py -3 tools/gl_vk_feature_coverage.py --write`; do not hand-edit. Inputs:",
         "same-name files, the `_tier1`/`_native` suffix rule, `tools/gl_vk_twin_map.json`, the",
         "`tools/glsl_port_diff.py` registry and the `scripts/check-split-path-contracts.py` PAIRS.", ""]
    fams = {}
    for g, v, via in twins.file_pairs:
        fams.setdefault(g, []).append((v, via))
    exact = sum(1 for g, v, _ in twins.file_pairs if not v.startswith("("))
    L += ["- GL shaders: %d, VK shaders: %d" % (len(twins.gl_files), len(twins.vk_files)),
          "- twin pairs (GL file, VK file, same stage): **%d**" % exact,
          "- GL shaders with a VK twin family: %d, **unpaired GL shaders (the not-ported surface): %d**"
          % (len(fams), len(twins.gl_unpaired)),
          "- VK shaders with no GL twin: %d" % len(twins.no_twin_vk), "",
          "## Twin pairs", "", "| GL shader | VK twin(s) | via |", "|---|---|---|"]
    for g in sorted(fams):
        vs = sorted(fams[g])
        L.append("| `%s` | %s | %s |" % (g, "<br>".join(
            ("`%s`" % v) if not v.startswith("(") else v for v, _ in vs),
            ", ".join(sorted({x for _, x in vs}))))
    L += ["", "## GL shaders with no VK twin", "", "| GL shader |", "|---|"]
    L += ["| `%s` |" % g for g in twins.gl_unpaired]
    L += ["", "## VK shaders with no GL twin", "", "| VK shader | why |", "|---|---|"]
    L += ["| `%s` | %s |" % (v, w) for v, w in sorted(twins.no_twin_vk.items())]
    return "\n".join(L) + "\n"


def coverage_doc(rows, allow, new, stale):
    cnt = {}
    for r in rows:
        cnt[(r["status"], r["confidence"])] = cnt.get((r["status"], r["confidence"]), 0) + 1
    L = ["# GL <-> VK feature coverage", "",
         "Generated by `py -3 tools/gl_vk_feature_coverage.py --write`; do not hand-edit. A row is a",
         "feature (uniform or compile-time toggle) of a GL shader that an `MC2_*` gate drives, and whether",
         "the VK twin carries the same name. Method and limits: `tools/gl_vk_cpp_gates.py`. Confidence is the",
         "weaker of how directly the gate reaches the setter and how firm the VK evidence is.", "",
         "| status | HIGH | MEDIUM | LOW |", "|---|---|---|---|"]
    for s in ("PORTED", "NOT_PORTED", "NO_TWIN"):
        L.append("| %s | %d | %d | %d |" % (s, *(cnt.get((s, c), 0) for c in ("HIGH", "MEDIUM", "LOW"))))
    np_rows = [r for r in rows if r["status"] == "NOT_PORTED"]
    live = [r for r in np_rows if r["gate_default_on"] and r["confidence"] != "LOW"]
    L += ["", "## NOT_PORTED: the VK twin lacks the feature", "",
          "Each (gate, GL shader) pair is accounted for in `tools/vk_not_ported.allowlist`. "
          "Flipping one of these gates ON changes GL only. **Live drift** = the gate is already default ON.", "",
          "| gate | default | GL shader | feature | VK twin | confidence | allowlisted |", "|---|---|---|---|---|---|---|"]
    for r in np_rows:
        L.append("| `%s` | %s | `%s` | `%s` (%s) | %s | %s | %s |" % (
            r["gate"], "**ON (live drift)**" if r["gate_default_on"] else "OFF", r["gl_shader"],
            r["feature"], r["kind"], r["vk_twin"], r["confidence"],
            "yes" if (r["gate"], r["gl_shader"]) in allow else "no"))
    L += ["", "- NOT_PORTED pairs: %d, of which already default ON: %d" % (
        len({(r["gate"], r["gl_shader"]) for r in np_rows}), len({(r["gate"], r["gl_shader"]) for r in live}))]
    L += ["- NEW gaps (not allowlisted): %d, STALE allowlist entries: %d" % (len(new), len(stale)), "",
          "## PORTED", "", "| gate | GL shader | feature | VK twin | confidence |", "|---|---|---|---|---|"]
    for r in rows:
        if r["status"] == "PORTED":
            L.append("| `%s` | `%s` | `%s` | `%s` | %s |" % (r["gate"], r["gl_shader"], r["feature"],
                                                         r["vk_twin"], r["confidence"]))
    nt = {}
    for r in rows:
        if r["status"] == "NO_TWIN":
            nt.setdefault(r["gl_shader"], set()).add(r["gate"])
    L += ["", "## NO_TWIN: gate-driven GL features whose shader has no VK twin at all", "",
          "| GL shader | gates |", "|---|---|"]
    for g in sorted(nt):
        L.append("| `%s` | %s |" % (g, ", ".join("`%s`" % x for x in sorted(nt[g]))))
    return "\n".join(L) + "\n"


# --- amnesty hook -----------------------------------------------------------

def _truthy(d):
    return d if isinstance(d, bool) else (d != 0 if isinstance(d, (int, float)) else False)


def gate_defaults(text_registry, text_seed):
    """{gate: default_on} from the gate table registry rows and the typed gate seed (either may be None)."""
    out = {}
    if text_registry:
        sys.path.insert(0, os.path.join(ROOT, "scripts"))
        import gate_table_reader as gtr
        for key, ent in gtr.registry_entries_from_text(text_registry, without_only=True).items():
            if ent.get("envVar"):
                out[ent["envVar"]] = bool(ent["defaultOn"])
    if text_seed:
        for g in json.loads(text_seed)["gates"]:
            out[g["name"]] = out.get(g["name"], False) or _truthy(g.get("default"))
    return out


def flipped_gates(base_defaults, head_defaults):
    return sorted(g for g, on in head_defaults.items() if on and not base_defaults.get(g, False))


def flip_check(allow, base_defaults, head_defaults):
    """[message] for each allowlisted VK gap whose gate default went OFF -> ON."""
    flipped = set(flipped_gates(base_defaults, head_defaults))
    msgs = []
    for (gate, shader), why in sorted(allow.items()):
        if gate in flipped:
            msgs.append("VK-GAP-FLIP %s now defaults ON but its VK twin of %s lacks the feature: %s"
                        % (gate, shader, why))
    return msgs


# --- main -------------------------------------------------------------------

def compute(src):
    src.preload([p for p in src.paths() if p.startswith("shaders/") and not p.startswith("shaders/spv/")])
    twins = tw.derive(src)
    if twins.unclassified_vk:
        raise SystemExit("[gl_vk_feature_coverage] VK shader(s) with no GL twin and no no_gl_twin entry in "
                         "%s: %s" % (tw.TWIN_MAP, ", ".join(twins.unclassified_vk)))
    files, ndefs = load_cpp(src)
    rows = build_rows(src, twins, files, gate_info(src), ndefs)
    return twins, rows


def main(argv=None, root=ROOT):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ref", help="grade this git revision instead of the working tree")
    ap.add_argument("--write", action="store_true", help="regenerate the docs")
    ap.add_argument("--check", action="store_true", help="also fail if the committed docs are stale")
    ap.add_argument("--emit-missing", action="store_true",
                    help="print allowlist lines for every NOT_PORTED pair that has no entry")
    ap.add_argument("--flip-check", metavar="BASE", help="fail if a listed VK gap's gate went default ON since BASE")
    args = ap.parse_args(argv)

    allow = load_allowlist(os.path.join(root, ALLOWLIST))
    if args.flip_check:
        base, head = Source(root, ref=args.flip_check), Source(root, ref=args.ref)
        try:
            msgs = flip_check(allow, gate_defaults(base.text(REGISTRY_H), base.text(GATE_SEED)),
                              gate_defaults(head.text(REGISTRY_H), head.text(GATE_SEED)))
        except Exception as e:      # an unreadable registry must not read as "no flips"
            print("[gl_vk_feature_coverage] flip-check cannot read the gate defaults: %s" % e)
            return 2
        for m in msgs:
            print(m)
        print("[gl_vk_feature_coverage] flip-check vs %s: %s" % (args.flip_check, "FAIL" if msgs else "OK"))
        return 1 if msgs else 0

    src = Source(root, ref=args.ref)
    twins, rows = compute(src)
    new, stale = grade(rows, allow)
    docs = {DOC_TWINS: twins_doc(twins), DOC_COV: coverage_doc(rows, allow, new, stale),
            DOC_JSON: json.dumps({"rows": rows}, indent=1, sort_keys=True) + "\n"}
    rc = 0
    if args.emit_missing:
        for gate, shader in new:
            print(suggest_line(rows, gate, shader))
        return 0
    if args.write:
        for rel, text in docs.items():
            with open(os.path.join(root, rel), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
    elif args.check:
        for rel, text in docs.items():
            p = os.path.join(root, rel)
            have = open(p, encoding="utf-8", newline="").read().replace("\r\n", "\n") if os.path.isfile(p) else None
            if have != text:
                print("STALE-DOC %s (run: py -3 tools/gl_vk_feature_coverage.py --write)" % rel)
                rc = 1
    for gate, shader in new:
        feats = sorted({r["feature"] for r in rows if r["gate"] == gate and r["gl_shader"] == shader
                        and r["status"] == "NOT_PORTED"})
        print("NEW-VK-GAP %s | %s | %s -- port the feature, or add to %s:"
              % (gate, shader, ", ".join(feats), ALLOWLIST))
        print("    " + suggest_line(rows, gate, shader))
    for gate, shader in stale:
        print("STALE-ALLOWLIST %s | %s no longer NOT_PORTED (delete the entry)" % (gate, shader))
    npairs = len(enforced(rows))
    print("[gl_vk_feature_coverage] %d twin pairs, %d GL shaders unpaired, %d feature rows, %d NOT_PORTED pairs, "
          "%d new, %d stale" % (sum(1 for _, v, _ in twins.file_pairs if not v.startswith("(")),
                                len(twins.gl_unpaired), len(rows), npairs, len(new), len(stale)))
    return 1 if (new or stale or rc) else 0


if __name__ == "__main__":
    sys.exit(main())
