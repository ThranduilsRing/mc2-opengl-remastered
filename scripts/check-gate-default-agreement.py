#!/usr/bin/env python3
"""GATE-DEFAULT-TRUTH-1 -- the registry's STATED default must equal the reader's
IMPLEMENTED default.

WHY THIS EXISTS
---------------
scripts/check-env-registry.sh verifies that every MC2_* env var read in source is
REGISTERED. It never verifies that the registry's `defaultOn` field agrees with
what the reader actually does when the variable is unset.

Its sibling scripts/check-gate-defaults.py classifies reader defaults and prints
them -- but it never opens RendererFeatureRegistry.h, never compares anything to
`defaultOn`, is not registered in check-contracts.sh, and documents itself as
"Exit 0 (report) always". So between the two of them, one tool knew what the
registry claimed and the other knew what the code did, and nothing ever put the
two numbers side by side. That is the whole gap, and it shipped two live lies:

  MC2_SHADOW_BOUNDED_NEAR_FIT -- registry said false, docs/tier1_env_vars.md said
    OFF, and the reader's OWN comment two lines above itself said "default OFF",
    while gos_postprocess.cpp did `return !(v && v[0] == '0');  // default ON`.
    Three written sources, one binary, and the binary disagreed with all three.

  MC2_SHADOW_ENABLE -- registry said false / "Opt-in =1" while BOTH GL readers
    did `!(getenv(..) && getenv(..)[0]=='0')`, i.e. default ON.

The cost is not cosmetic. Anyone running the documented A/B ("=1 versus unset")
compared ON against ON and graded an arm that did not exist. A whole session of
shadow work was steered by it.

WHAT THIS CHECKS
----------------
For every Feature-kind entry in RenderCore/RendererFeatureRegistry.h that names
an env var, locate its reader(s) in C++ source, classify each reader's unset-case
default from its idiom, and require:

  1. every reader agrees with the registry's `defaultOn`, and
  2. every reader agrees with every other reader.

UNCLASSIFIABLE IS A FAILURE, NOT A PASS. A reader written in an idiom this script
cannot classify is exactly the situation that hides a lie, so it fails and must
either be rewritten into a known idiom or given an explicit, reasoned entry in
scripts/gate-defaults-allowlist.txt. Silence is never evidence of agreement.

COMMENTS ARE STRIPPED BEFORE ANY MATCHING. Two contract rules added earlier in
the same session passed their own self-tests while satisfied by nothing more than
an identifier sitting in a comment. Comment text is not code; the self-test
asserts this directly (see the "comment_only" case in _selftest).

USAGE
  py -3 scripts/check-gate-default-agreement.py            # report
  py -3 scripts/check-gate-default-agreement.py --quiet    # exit code only
  py -3 scripts/check-gate-default-agreement.py --selftest # prove it catches it
"""

import os
import re
import sys

# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
WORKTREE = os.path.dirname(SCRIPT_DIR)
REGISTRY = os.path.join(WORKTREE, "RenderCore", "RendererFeatureRegistry.h")
ALLOWLIST = os.path.join(SCRIPT_DIR, "gate-defaults-allowlist.txt")

# Where readers live. The registry header itself is excluded: it *names* every
# var in string literals, so scanning it would find a "reader" for everything.
SCAN_DIRS = ["GameOS", "mclib", "code", "RenderCore", "editor"]
SCAN_EXTS = (".cpp", ".h", ".hpp", ".cc", ".inl")

ON, OFF, UNKNOWN = "ON", "OFF", "UNCLASSIFIABLE"


# ---------------------------------------------------------------------------
# Comment stripping
# ---------------------------------------------------------------------------

def strip_comments(src):
    """Remove // and /* */ comments, preserving string literals and byte offsets.

    Offsets are preserved by overwriting comment bytes with spaces (newlines
    kept) so reported line numbers stay correct. String literals are preserved
    because env var names live inside them.
    """
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n:
                if src[i] == "\\":
                    i += 2
                    continue
                if src[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out[i] = " "
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            end = src.find("*/", i + 2)
            end = n if end == -1 else end + 2
            for j in range(i, end):
                if out[j] != "\n":
                    out[j] = " "
            i = end
            continue
        i += 1
    return "".join(out)


# ---------------------------------------------------------------------------
# Registry parsing
# ---------------------------------------------------------------------------

ENTRY_RE = re.compile(
    r'"(MC2_[A-Z0-9_]+)"\s*,\s*'               # featureId
    r'(?:"(MC2_[A-Z0-9_]+)"|nullptr)\s*,\s*'   # envVar or nullptr
    r"EnvVarKind::(\w+)\s*,\s*"                # kind
    r"(true|false)\s*,",                       # defaultOn
    re.S,
)


def parse_registry(text):
    """-> list of dicts {feature, var, kind, default_on, line}."""
    body = strip_comments(text)
    entries = []
    for m in ENTRY_RE.finditer(body):
        var = m.group(2)
        if not var:
            continue  # nullptr env var = always-on, no runtime gate to check
        entries.append({
            "feature": m.group(1),
            "var": var,
            "kind": m.group(3),
            "default_on": m.group(4) == "true",
            "line": body.count("\n", 0, m.start()) + 1,
        })
    return entries


# ---------------------------------------------------------------------------
# Reader classification
# ---------------------------------------------------------------------------
#
# Ordered rules: first match wins, so the most specific idioms come first.
#   DEFAULT ON  -- absence means enabled; the var is a KILL-SWITCH ("=0 disables")
#   DEFAULT OFF -- absence means disabled; the var is OPT-IN ("=1 enables")

_R = re.compile

CLASSIFY_RULES = [
    # ---- default ON (kill-switch idioms) ----------------------------------
    # !(v && v[0] == '0')   /   !(getenv(X) != nullptr && getenv(X)[0] == '0')
    (ON, _R(r"!\s*\([^;]*\[\s*0\s*\]\s*==\s*'0'"), "!(...[0]=='0')"),
    # !v || v[0] != '0'   /   v == nullptr || v[0] != '0'
    (ON, _R(r"(?:!\s*\w+|\w+\s*==\s*(?:nullptr|NULL|0))\s*\|\|[^;]*"
            r"\[\s*0\s*\]\s*!=\s*'0'"), "!v || v[0]!='0'"),
    # (v && v[0] == '0') ? 0 : 1   /   ... ? false : true
    (ON, _R(r"\[\s*0\s*\]\s*==\s*'0'\s*\)?\s*\?\s*(?:0|false)\s*:\s*(?:1|true)"),
     "(...=='0') ? 0 : 1"),
    # if (!v || !v[0]) return 1;  -- unset falls through to a truthy return
    (ON, _R(r"if\s*\(\s*!\s*\w+\s*\|\|\s*!\s*\w+\s*\[\s*0\s*\]\s*\)\s*"
            r"return\s+(?:1|true)\b"), "if(unset) return 1"),
    # if (v == nullptr) return true;   /   if (!v) return true;
    (ON, _R(r"if\s*\(\s*(?:\w+\s*==\s*(?:nullptr|NULL)|!\s*\w+)\s*\)\s*"
            r"return\s+true\b"), "if(!v) return true"),
    # named helpers whose documented contract is default-on
    (ON, _R(r"\benvFlagDefaultOn\b|\bgvEnvFlagDefaultOn\b|\bEnvFlagDefaultOn\b"),
     "envFlagDefaultOn helper"),

    # ---- default OFF (opt-in idioms) --------------------------------------
    (OFF, _R(r"\benvFlagDefaultOff\b|\bEnvFlagDefaultOff\b"),
     "envFlagDefaultOff helper"),
    (OFF, _R(r"\[\s*0\s*\]\s*==\s*'1'"), "v[0]=='1'"),
    (OFF, _R(r"envTriState\s*\([^)]*\)\s*==\s*1"), "envTriState()==1"),
    # VK family helpers. subGate()/directChainGate() fall back to umbrellaOn(),
    # which is `envTriState("MC2_VK_SHARED_GBUFFER") == 1` -- default OFF -- so an
    # unset member of either family resolves OFF. (vk_shared_gbuffer_gate.h:87-101)
    (OFF, _R(r"\b(?:mc2vkgate::)?(?:subGate|directChainGate)\s*\("),
     "VK subGate/directChainGate (umbrella default OFF)"),
    # v != nullptr && (v[0] != '0')
    (OFF, _R(r"\w+\s*!=\s*(?:nullptr|NULL)\s*&&[^;]*\[\s*0\s*\]\s*!=\s*'0'"),
     "v!=nullptr && v[0]!='0'"),
    # if (v == nullptr) return false;   /   if (!v) return false;
    (OFF, _R(r"if\s*\(\s*(?:\w+\s*==\s*(?:nullptr|NULL)|!\s*\w+)\s*\)\s*"
             r"return\s+false\b"), "if(!v) return false"),
    # v && v[0] && v[0] != '0'      /      v && v[0] != '\0' && v[0] != '0'
    (OFF, _R(r"\w+\s*&&\s*\w+\s*\[\s*0\s*\](?:\s*!=\s*'\\0')?\s*&&\s*"
             r"\w+\s*\[\s*0\s*\]\s*!=\s*'0'"), "v && v[0] && v[0]!='0'"),
    # v && v[0] != '0'
    (OFF, _R(r"\w+\s*&&\s*\w+\s*\[\s*0\s*\]\s*!=\s*'0'"), "v && v[0]!='0'"),
    # if (!v || !v[0]) return 0;
    (OFF, _R(r"if\s*\(\s*!\s*\w+\s*\|\|\s*!\s*\w+\s*\[\s*0\s*\]\s*\)\s*"
             r"return\s+(?:0|false)\b"), "if(unset) return 0"),
    # bare presence tests
    (OFF, _R(r"getenv\s*\([^)]*\)\s*!=\s*(?:nullptr|NULL|0)\b"), "getenv()!=nullptr"),
    (OFF, _R(r"\breturn\s+(?:std::)?getenv\s*\([^)]*\)\s*;"), "return getenv()"),
    # if (getenv(X))  /  if (const char* e = getenv(X))  -- presence-gated
    (OFF, _R(r"if\s*\(\s*(?:const\s+char\s*\*\s*\w+\s*=\s*)?(?:std::)?getenv\s*\("),
     "if(getenv(X)) presence gate"),
]

# Applied BEFORE the ordered rules above, because these forms contain substrings
# that an earlier rule would otherwise claim.
PRE_RULES = [
    # HIGHEST PRIORITY: an explicit branch on the UNSET case. `if (!v) return X`
    # *is* the default, so it outranks any general expression elsewhere in the
    # same body. Ordering this below the generic rules was a real defect, caught
    # by a live injection test rather than by the unit self-test: a reader whose
    # unset branch was flipped to `return false` still classified ON, because the
    # trailing `return !(v[0] == '0' && v[1] == '\0');` matched the kill-switch
    # pattern first. The checker reported AGREE on a reader that had just been
    # inverted -- i.e. it would have missed the very class of bug it exists for.
    (OFF, _R(r"if\s*\(\s*(?:!\s*\w+|\w+\s*==\s*(?:nullptr|NULL))\s*\)\s*"
             r"return\s+(?:false|0)\b"), "if(unset) return false [explicit]"),
    (ON, _R(r"if\s*\(\s*(?:!\s*\w+|\w+\s*==\s*(?:nullptr|NULL))\s*\)\s*"
            r"return\s+(?:true|1)\b"), "if(unset) return true [explicit]"),
    (OFF, _R(r"if\s*\(\s*!\s*\w+\s*\|\|\s*!\s*\w+\s*\[\s*0\s*\]\s*\)\s*"
             r"return\s+(?:0|false)\b"), "if(unset) return 0 [explicit]"),
    (ON, _R(r"if\s*\(\s*!\s*\w+\s*\|\|\s*!\s*\w+\s*\[\s*0\s*\]\s*\)\s*"
            r"return\s+(?:1|true)\b"), "if(unset) return 1 [explicit]"),

    # v && !(v[0] == '0' && v[1] == '\0')   -- PRESENCE-gated, so default OFF.
    # Without this, the "!(...[0]=='0')" kill-switch rule below claims it and
    # reports ON. That is not hypothetical: it mis-classified the real helper
    # envFlagDefaultOff() (debug_renderer.cpp:32) as default-ON, i.e. the checker
    # would have manufactured a fake DISAGREEMENT against an honest entry. The
    # leading `v &&` is the whole difference between "unset means on" and "unset
    # means off", so it has to be tested before the shorter pattern.
    (OFF, _R(r"\w+\s*&&\s*!\s*\([^;]*\[\s*0\s*\]\s*==\s*'0'"),
     "v && !(v[0]=='0') presence-gated"),
    # if (v && v[0]=='0' && v[1]=='\0') state = 0; else state = 1;
    (ON, _R(r"\[\s*0\s*\]\s*==\s*'0'[^;]*\)\s*state\s*=\s*0\s*;\s*else\s*"
            r"state\s*=\s*1"), "if(=='0') state=0 else state=1"),
    # (!v || !v[0]) ? true : (v[0] != '0')
    (ON, _R(r"\(\s*!\s*\w+\s*\|\|\s*!\s*\w+\s*\[\s*0\s*\]\s*\)\s*\?\s*true"),
     "(unset) ? true : ..."),
]

# A getenv() hit inside one of these is a VALUE read (a path, a radius, a
# bitmask), not a boolean gate, so `defaultOn` carries no meaning for it.
VALUE_READ_RE = re.compile(
    r"\batof\s*\(|\batoi\s*\(|\bstrtol\s*\(|\bstrtod\s*\(|\bstrtoul\s*\(|"
    r"\bsscanf\s*\(|\bstd::sto[il]\b|\bstrcmp\s*\(|\b_stricmp\s*\(|"
    r"\bstrncmp\s*\(|\bstrchr\s*\(|\bstrstr\s*\("
)

# Not a reader at all: a diagnostic that PRINTS the raw env string, or a site
# that WRITES the variable. Both mention the var without deciding its default,
# and counting them would let a logging line outvote the real reader.
NON_READER_RE = re.compile(
    r"\bf?printf\s*\(|\bvlog\s*\(|\bstd::cerr\b|\bstd::cout\b|"
    r"\b_putenv(?:_s)?\s*\(|\bsetenv\s*\(|\bSetEnvironmentVariable"
)


MAX_BLOCK = 900   # chars; larger than this and we are looking at a whole function
FALLBACK_TAIL = 400


def enclosing_block(text, pos):
    """Span of the innermost { ... } containing `pos`, or None."""
    depth, open_at = 0, -1
    i = pos - 1
    while i >= 0:
        c = text[i]
        if c == "}":
            depth += 1
        elif c == "{":
            if depth == 0:
                open_at = i
                break
            depth -= 1
        i -= 1
    if open_at == -1:
        return None
    depth, j, n = 0, open_at, len(text)
    while j < n:
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return (open_at, j + 1)
        j += 1
    return None


def statement_around(text, pos):
    """Context for a getenv() at `pos`.

    The idiom that decides the default is almost never on the same statement as
    the getenv() -- the canonical form is

        const char* v = getenv("X");
        return !(v && v[0] == '0');

    so a statement-only window (everything up to the first ';') sees the getenv
    and none of the logic, and classifies every reader in the codebase as
    UNCLASSIFIABLE. The self-test caught exactly that. Prefer the innermost
    enclosing block, which for the ubiquitous `static const bool s = []{...}()`
    reader IS the reader body; fall back to statement-plus-tail when the block
    is a whole function.
    """
    blk = enclosing_block(text, pos)
    if blk:
        body = text[blk[0]:blk[1]]
        # Measure the block by its CODE length, not its raw span. strip_comments
        # blanks comments to spaces to preserve line numbers, so a reader with a
        # 40-line rationale above it spans thousands of characters while being a
        # four-line function. Measuring raw span pushed exactly those
        # heavily-documented readers -- the interesting ones -- past the cap.
        if len(re.sub(r"\s+", "", body)) <= MAX_BLOCK:
            return body
    start = max(text.rfind(";", 0, pos), text.rfind("{", 0, pos),
                text.rfind("}", 0, pos))
    start = 0 if start == -1 else start + 1
    return text[start:min(len(text), pos + FALLBACK_TAIL)]


def classify_statement(stmt, helper_body=False):
    """-> (verdict|None, rule_name). None means 'not a boolean gate'.

    `helper_body=True` skips the value-read / non-reader guards. A helper's own
    body legitimately contains strcmp() (literal caching) or a log line while
    still being a perfectly ordinary boolean gate resolver; applying the guards
    there made the resolver refuse to classify exactly the helpers most worth
    resolving.
    """
    flat = re.sub(r"\s+", " ", stmt)
    if not helper_body:
        if VALUE_READ_RE.search(flat):
            return None, "value-read (not a boolean gate)"
        if NON_READER_RE.search(flat):
            return None, "print/write site, not a reader"
    for verdict, rx, name in PRE_RULES:
        if rx.search(flat):
            return verdict, name
    for verdict, rx, name in CLASSIFY_RULES:
        if rx.search(flat):
            return verdict, name
    return UNKNOWN, "no rule matched"


# ---------------------------------------------------------------------------
# Named-helper resolution
# ---------------------------------------------------------------------------
#
# Many gates are not read with a raw getenv() but through a one-argument helper:
# tglc_envOn("X"), envFlagOn("X"), HudImGui::envGateOn("X"),
# lightClampEnvFlagDefaultOn("X"), mc2vkgate::subGate("X"). Hardcoding a list of
# helper names would rot the moment someone adds the next one -- and a checker
# that silently stops seeing readers is worse than no checker. So resolve them:
# find every `bool NAME(const char* ...)` definition, classify ITS body with the
# same rules, and treat a call NAME("MC2_X") as a reader with that verdict.

HELPER_DEF_RE = re.compile(
    r"\bbool\s+(\w+)\s*\(\s*const\s+char\s*\*\s*\w*\s*\)\s*\{"
)


def resolve_helpers(sources):
    """-> {helper_name: verdict} for single-const-char* boolean helpers."""
    helpers = {}
    for _path, body in sources:
        for m in HELPER_DEF_RE.finditer(body):
            name = m.group(1)
            blk = enclosing_block(body, m.end())
            if not blk:
                continue
            verdict, _rule = classify_statement(body[blk[0]:blk[1]],
                                                helper_body=True)
            if verdict in (ON, OFF) and helpers.get(name, verdict) == verdict:
                helpers[name] = verdict
            elif verdict in (ON, OFF) and helpers.get(name) != verdict:
                helpers[name] = UNKNOWN   # two definitions disagree: refuse to guess
    return helpers


CALLEE_RE = re.compile(r"([A-Za-z_]\w*)\s*(?:::\s*)?$")


def find_readers(var, sources, helpers=None):
    """-> list of dicts {path, line, verdict, rule, stmt}.

    `sources` must already be comment-stripped, which is what makes a var named
    only in a comment incapable of registering as a reader.
    """
    helpers = helpers or {}
    needle = re.compile(r'\(\s*"' + re.escape(var) + r'"\s*\)')
    hits = []
    for path, body in sources:
        for m in needle.finditer(body):
            # Identify the callee immediately left of the '(' -- getenv, or a
            # resolved helper, or something else entirely.
            callee_m = CALLEE_RE.search(body[max(0, m.start() - 64):m.start()])
            callee = callee_m.group(1) if callee_m else ""
            stmt = statement_around(body, m.start())

            if callee in helpers and helpers[callee] in (ON, OFF):
                verdict, rule = helpers[callee], f"helper {callee}() -> {helpers[callee]}"
            else:
                verdict, rule = classify_statement(stmt)

            if verdict is None:
                continue
            hits.append({
                "path": path,
                "line": body.count("\n", 0, m.start()) + 1,
                "verdict": verdict,
                "rule": rule,
                "stmt": re.sub(r"\s+", " ", stmt).strip()[:180],
            })
    return hits


# ---------------------------------------------------------------------------
# Allowlist
# ---------------------------------------------------------------------------

def load_allowlist(path=ALLOWLIST):
    """VAR<whitespace>reason. A bare VAR, or a token reason, is itself an error."""
    entries, malformed = {}, []
    if not os.path.exists(path):
        return entries, malformed
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 1)
            if len(parts) < 2 or len(parts[1].strip()) < 20:
                malformed.append(line)
                continue
            entries[parts[0]] = parts[1].strip()
    return entries, malformed


# ---------------------------------------------------------------------------
# Core
# ---------------------------------------------------------------------------

def load_sources(root=WORKTREE, scan_dirs=None, exclude=(REGISTRY,)):
    scan_dirs = SCAN_DIRS if scan_dirs is None else scan_dirs
    excl = {os.path.normcase(os.path.abspath(p)) for p in exclude}
    out = []
    for d in scan_dirs:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [x for x in dirnames
                           if x not in (".git", "build64", "third_party", "3rdparty")]
            for fn in filenames:
                if not fn.endswith(SCAN_EXTS):
                    continue
                full = os.path.join(dirpath, fn)
                if os.path.normcase(os.path.abspath(full)) in excl:
                    continue
                try:
                    with open(full, "r", encoding="utf-8", errors="replace") as fh:
                        raw = fh.read()
                except OSError:
                    continue
                out.append((os.path.relpath(full, root).replace("\\", "/"),
                            strip_comments(raw)))
    return out


def analyse(registry_text, sources, allow):
    """-> (findings, stats)."""
    entries = parse_registry(registry_text)
    helpers = resolve_helpers(sources)
    findings = []
    stats = {"feature_entries": 0, "checked": 0, "no_reader": 0,
             "agree": 0, "disagree": 0, "unclassifiable": 0,
             "reader_conflict": 0, "allowlisted": 0,
             "helpers_resolved": len([v for v in helpers.values()
                                      if v in (ON, OFF)])}

    for e in entries:
        if e["kind"] != "Feature":
            continue  # Trace/Retired: defaultOn carries no shipping semantics
        stats["feature_entries"] += 1
        readers = find_readers(e["var"], sources, helpers)

        if not readers:
            # No reader found. That is check-env-registry.sh / dead-gate
            # territory, not a default-agreement question. Counted, not failed.
            stats["no_reader"] += 1
            continue

        stats["checked"] += 1
        if e["var"] in allow:
            stats["allowlisted"] += 1
            continue

        verdicts = {r["verdict"] for r in readers}
        stated = ON if e["default_on"] else OFF

        if UNKNOWN in verdicts:
            stats["unclassifiable"] += 1
            bad = [r for r in readers if r["verdict"] == UNKNOWN]
            findings.append({
                "kind": "UNCLASSIFIABLE", "var": e["var"],
                "msg": ("reader idiom not recognised, so its default cannot be "
                        f"compared with the registry's stated {stated}"),
                "sites": [f"{r['path']}:{r['line']}  {r['stmt']}" for r in bad],
            })
            continue

        if len(verdicts) > 1:
            stats["reader_conflict"] += 1
            findings.append({
                "kind": "READER_CONFLICT", "var": e["var"],
                "msg": "readers disagree with EACH OTHER about the unset default",
                "sites": [f"{r['path']}:{r['line']}  [{r['verdict']}] {r['rule']}"
                          for r in readers],
            })
            continue

        implemented = verdicts.pop()
        if implemented != stated:
            stats["disagree"] += 1
            findings.append({
                "kind": "DISAGREEMENT", "var": e["var"],
                "msg": (f"registry states default {stated} "
                        f"(RendererFeatureRegistry.h:{e['line']}) but the reader "
                        f"implements default {implemented}"),
                "sites": [f"{r['path']}:{r['line']}  [{r['verdict']}] {r['rule']}"
                          for r in readers],
            })
        else:
            stats["agree"] += 1

    return findings, stats


def run(quiet=False):
    with open(REGISTRY, "r", encoding="utf-8", errors="replace") as fh:
        registry_text = fh.read()
    sources = load_sources()
    allow, malformed = load_allowlist()
    findings, stats = analyse(registry_text, sources, allow)

    for line in malformed:
        findings.append({
            "kind": "BAD_ALLOWLIST",
            "var": line.split()[0] if line.split() else "?",
            "msg": ("allowlist entry has no reason, or a reason under 20 chars. "
                    "An unexplained exemption is how the original lie survived."),
            "sites": [line],
        })

    if not quiet:
        print("=" * 70)
        print("  GATE-DEFAULT-TRUTH-1 -- registry default vs reader default")
        print("=" * 70)
        print(f"  Feature entries with an env var : {stats['feature_entries']}")
        print(f"  ... with a reader found         : {stats['checked']}")
        print(f"  ... no reader in scanned source : {stats['no_reader']}")
        print(f"  allowlisted (explicit reason)   : {stats['allowlisted']}")
        print(f"  AGREE                           : {stats['agree']}")
        print(f"  DISAGREE                        : {stats['disagree']}")
        print(f"  READER CONFLICT                 : {stats['reader_conflict']}")
        print(f"  UNCLASSIFIABLE                  : {stats['unclassifiable']}")
        print()
        for f in findings:
            print(f"  [{f['kind']}] {f['var']}")
            print(f"      {f['msg']}")
            for s in f["sites"]:
                print(f"      - {s}")
            print()
        if not findings:
            print("  OK: every classifiable Feature gate's registry default "
                  "matches its reader.")
        print("=" * 70)

    return 0 if not findings else 1


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

_SELF_REGISTRY = '''
static constexpr EnvVarDesc kFeatureTable[] = {
    { "MC2_FEATURE_ST_HONEST_ON",  "MC2_ST_HONEST_ON",  EnvVarKind::Feature, true,
      "kill-switch, default ON, reader agrees" },
    { "MC2_FEATURE_ST_HONEST_OFF", "MC2_ST_HONEST_OFF", EnvVarKind::Feature, false,
      "opt-in, default OFF, reader agrees" },
    { "MC2_FEATURE_ST_LIAR",       "MC2_ST_LIAR",       EnvVarKind::Feature, false,
      "registry claims OFF; reader implements ON -- the real bug" },
    { "MC2_FEATURE_ST_WEIRD",      "MC2_ST_WEIRD",      EnvVarKind::Feature, true,
      "reader uses an idiom no rule covers" },
    { "MC2_FEATURE_ST_SPLIT",      "MC2_ST_SPLIT",      EnvVarKind::Feature, true,
      "two readers that disagree with each other" },
    { "MC2_FEATURE_ST_COMMENT",    "MC2_ST_COMMENT",    EnvVarKind::Feature, true,
      "named ONLY inside a comment -- must not count as a reader" },
    { "MC2_FEATURE_ST_INVERTED",   "MC2_ST_INVERTED",   EnvVarKind::Feature, true,
      "explicit unset branch says OFF while a later expression looks like ON" },
    { "MC2_TRACE_ST_VALUE",        "MC2_ST_VALUE",      EnvVarKind::Trace,   false,
      "Trace kind numeric tunable -- must be skipped" },
};
'''

_SELF_SOURCE = r'''
bool honestOn() {
    static const bool s = [] {
        const char* v = getenv("MC2_ST_HONEST_ON");
        return !(v && v[0] == '0');
    }();
    return s;
}
bool honestOff() {
    static const bool s = [] {
        const char* v = getenv("MC2_ST_HONEST_OFF");
        return v && v[0] && v[0] != '0';
    }();
    return s;
}
bool liar() {
    static const bool s = [] {
        const char* v = getenv("MC2_ST_LIAR");
        return !(v && v[0] == '0');
    }();
    return s;
}
bool weird() {
    const char* v = getenv("MC2_ST_WEIRD");
    return v ? someUnknownHelper(v) : fallbackFromConfigFile();
}
bool splitA() {
    const char* v = getenv("MC2_ST_SPLIT");
    return !(v && v[0] == '0');
}
bool splitB() {
    const char* v = getenv("MC2_ST_SPLIT");
    return v && v[0] == '1';
}
bool inverted() {
    const char* v = getenv("MC2_ST_INVERTED");
    // The honest idiom, left ONLY in a comment:
    // if (!v) return true; // default ON
    if (!v) return false;   // the code actually defaults OFF
    return !(v[0] == '0' && v[1] == '\0');
}
// MC2_ST_COMMENT is discussed at length here: getenv("MC2_ST_COMMENT")
// would be the reader if this were code, and it is !(v && v[0] == '0').
/* Block comment naming getenv("MC2_ST_COMMENT") for good measure. */
int radius() {
    const char* v = getenv("MC2_ST_VALUE");
    return v ? atoi(v) : 2500;
}
'''


def _selftest():
    failures = []

    def check(label, cond, detail=""):
        if cond:
            print(f"  PASS  {label}")
        else:
            print(f"  FAIL  {label}  {detail}")
            failures.append(label)

    print("GATE-DEFAULT-TRUTH-1 self-test")
    print("-" * 70)

    sources = [("selftest_fake.cpp", strip_comments(_SELF_SOURCE))]
    findings, stats = analyse(_SELF_REGISTRY, sources, allow={})
    by_var = {f["var"]: f for f in findings}

    # 1. THE REAL THING: registry says OFF, code says ON -- the exact shape of
    #    MC2_SHADOW_ENABLE and MC2_SHADOW_BOUNDED_NEAR_FIT.
    check("catches a REAL default disagreement (registry OFF, reader ON)",
          by_var.get("MC2_ST_LIAR", {}).get("kind") == "DISAGREEMENT",
          str(by_var.get("MC2_ST_LIAR")))

    # 2. Honest entries must NOT be flagged, or the rule is noise and the first
    #    person it annoys deletes it.
    check("does not flag an honest default-ON gate",
          "MC2_ST_HONEST_ON" not in by_var)
    check("does not flag an honest default-OFF gate",
          "MC2_ST_HONEST_OFF" not in by_var)

    # 3. Unclassifiable must FAIL, never pass silently.
    check("UNCLASSIFIABLE reader is a FAILURE, not a silent pass",
          by_var.get("MC2_ST_WEIRD", {}).get("kind") == "UNCLASSIFIABLE",
          str(by_var.get("MC2_ST_WEIRD")))

    # 4. Readers disagreeing with each other must fail even when the registry
    #    matches one of them -- the MC2_SHADOW_ENABLE GL-vs-VK shape.
    check("readers disagreeing with EACH OTHER is a FAILURE",
          by_var.get("MC2_ST_SPLIT", {}).get("kind") == "READER_CONFLICT",
          str(by_var.get("MC2_ST_SPLIT")))

    # 5. THE ANTI-THEATRE CASE. Two contract rules added earlier the same day
    #    passed their own self-tests while satisfied by an identifier sitting in
    #    a COMMENT. A commented-out getenv is not a reader, so this var must land
    #    in the no_reader bucket rather than be classified from comment text.
    check("an identifier in a COMMENT is not accepted as a reader",
          "MC2_ST_COMMENT" not in by_var and stats["no_reader"] >= 1,
          f"findings={list(by_var)} no_reader={stats['no_reader']}")

    # 6. THE LIVE-INJECTION REGRESSION. A body whose explicit unset branch says
    #    OFF, followed by a trailing `!(v[0]=='0' && v[1]=='\0')` that pattern-
    #    matches as ON. The first version of this checker reported AGREE here --
    #    the generic rule outranked the explicit unset branch, and the honest
    #    idiom sitting in a comment did nothing to save it. Found by flipping a
    #    real reader in the worktree, not by this unit test, which is why the
    #    case now lives here permanently.
    check("explicit `if (unset) return false` outranks a later ON-looking expr",
          by_var.get("MC2_ST_INVERTED", {}).get("kind") == "DISAGREEMENT",
          str(by_var.get("MC2_ST_INVERTED")))

    # 7. Trace-kind entries are skipped: defaultOn is meaningless for a numeric
    #    tunable and flagging them would be a false-positive mill.
    check("Trace-kind entries are not compared",
          "MC2_ST_VALUE" not in by_var)

    # 7. Comment stripping must not eat real code or string literals.
    stripped = strip_comments('a(); // getenv("X")\nb("/* not a comment */");')
    check("strip_comments preserves code and string literals",
          "a();" in stripped and 'b("/* not a comment */");' in stripped
          and 'getenv("X")' not in stripped, repr(stripped))

    # 8. An allowlist entry without a real reason must itself fail.
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as tf:
        tf.write("MC2_SOMETHING\nMC2_OTHER  short\n")
        tmp = tf.name
    try:
        _, malformed = load_allowlist(tmp)
        check("allowlist entries without a real reason are rejected",
              len(malformed) == 2, str(malformed))
    finally:
        os.unlink(tmp)

    print("-" * 70)
    if failures:
        print(f"SELF-TEST FAILED: {len(failures)} case(s): {failures}")
        return 1
    print("SELF-TEST PASSED (9 cases)")
    return 0


if __name__ == "__main__":
    argv = sys.argv[1:]
    if "--selftest" in argv or "--self-test" in argv:
        sys.exit(_selftest())
    sys.exit(run(quiet="--quiet" in argv))
