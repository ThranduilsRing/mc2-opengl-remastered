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
For EVERY entry in RenderCore/RendererFeatureRegistry.h that names an env var,
locate its reader(s) in C++ source, classify each reader's unset-case default
from its idiom, and require:

  1. every reader agrees with the registry's `defaultOn`,
  2. every reader agrees with every other reader, and
  3. every in-source comment (C++ AND GLSL) and every docs/tier1_env_vars.md
     line that asserts a default for that var agrees with the reader too.

SCOPE WIDENED TWICE BY ENV-DROP-CLOSE-1 (2026-07-30), because the registry is
neither the only nor the first place a default is written down:

  * ALL KINDS, not just Feature. 144 Trace entries and 3 Retired ones were exempt
    on the theory that "defaultOn carries no shipping semantics" for them. Many
    are numeric knobs where that is true -- and those are skipped anyway, because
    a value read is not a boolean gate. But a BOOLEAN Trace gate misleads exactly
    like a Feature one. Checked entries went 146 -> 225.
  * COMMENTS AND THE TIER1 DOC. code/objmgr.cpp:1610 said
    "Default-off: MC2_STATIC_PROP_MISSION_LOAD_REG=0" for ~3 months while
    gos_static_prop_registry.cpp:141 read `parseEnvBoolWithDefault(..., true)`.
    Mission-load registration was LIVE and the only note about it said it wasn't.
    Sweeping this class found 7 more in-source comment inversions and 3 in
    docs/tier1_env_vars.md, one of which (MC2_STANDARD_LIT_V1, "Default ON.
    Kill=`=0`" against a reader whose own comment says "TEMPORARY DEFAULT-OFF")
    would have had anyone A/B-ing `=0` versus unset grading OFF against OFF.

SCOPE WIDENED A THIRD TIME BY SHADER-COMMENT-TRUTH-1 (2026-07-30): "every
in-source comment" meant every C++ in-source comment, because the comment sweep
reused the READER directory and extension lists. shaders/ was in neither, so
100% of GLSL was skipped in silence -- and GLSL is where a renderer gate is most
often explained. Three more inversions were sitting there, each a gate promoted
to default-ON in C++ and in the tier1 doc while its shader comment still said
OFF. See CLAIM_DIRS. The lesson is the general one: when a checker's stated
scope is a CLASS ("in-source comments") but its implementation is a LIST, the
list silently becomes the scope, and no failure is ever reported for the gap.

SCOPE WIDENED A FOURTH TIME BY BACKEND-DEFAULT-PARITY-1 (2026-07-30), and this
one is about rule 2, which had the same disease in a nastier form: the rule that
detects TWO READERS CONTRADICTING EACH OTHER was implemented as a branch inside
`for e in <registry entries>`. So it could only ever fire on a variable somebody
had already bothered to register -- and an unregistered variable is exactly the
one nobody has audited. Rule 3 then hid the same vars a second time, because it
attributed a comment claim only when a var had ONE classifiable verdict, and a
var whose readers CONFLICT has none. Two readers disagreeing therefore bought
immunity from the comment rule as well.

  MC2_SKYBOX_FOG_EXCLUDE -- gos_postprocess.cpp:578 `!(v && v[0] == '0')`
    (default ON, the GL fog passes) against vulkan_post_fog_native.cpp:589
    `v && v[0] == '1'` (default OFF, the VK parity guard that exists to stop VK
    fogging the sky pixels GL excludes). Same unset environment, opposite
    behaviour, and the guard was inert precisely when it was needed. Two shader
    comments asserted "default OFF" alongside. Unregistered, so rule 1 never
    looked; conflicting, so rule 3 dropped it. Three rules, zero coverage, for
    the entire life of the gate. Sweeping the readers instead of the registry
    also surfaced MC2_TERRAIN_SHORELINE, where a default flip updated two of
    three readers.

Rule 2 is now reader_conflicts(), a pass over EVERY var with a reader. Rule 3's
attribution moved to implemented_default(), which ignores UNCLASSIFIABLE siblings
when the classifiable readers agree -- "I cannot read this idiom" is not evidence
the default differs, and treating it as such is how docs/tier1_env_vars.md could
call MC2_WATER_REFLECTION "Default OFF" against a reader returning 1 when unset.

UNCLASSIFIABLE IS A FAILURE, NOT A PASS -- for Feature entries. A reader written
in an idiom this script cannot classify is exactly the situation that hides a
lie, so it fails and must either be rewritten into a known idiom or given an
explicit, reasoned entry in scripts/gate-defaults-allowlist.txt. Silence is never
evidence of agreement. For Trace/Retired it is REPORTED but not failed; see the
note in analyse() for why that asymmetry is deliberate.

COMMENTS ARE STRIPPED BEFORE ANY READER MATCHING. Two contract rules added
earlier in the same session passed their own self-tests while satisfied by
nothing more than an identifier sitting in a comment. Comment text is not code;
the self-test asserts this directly (see the "comment_only" case in _selftest).
Note that stripping comments for CLASSIFICATION and never CHECKING them are two
different decisions -- only the first was ever deliberate, and rule 3 above
closes the second.

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

# SHADER COMMENTS ARE CLAIM-SCANNED BUT NEVER READER-SCANNED (SHADER-COMMENT-TRUTH-1).
#
# The two lists above are the READER surface, and they were also -- silently --
# the whole comment surface, because load_comment_claims() reused them. So a
# checker whose stated rule 3 covers "every in-source comment" skipped every
# shader in the tree twice over: `shaders` is in neither SCAN_DIRS, nor is any
# GLSL extension in SCAN_EXTS. Nothing reported a gap; the sweep just returned
# fewer files. That is the false-assurance shape this file exists to kill, so it
# is worth naming: a checker that covers a class "except where it silently does
# not" is worse than one that never claimed the class at all.
#
# It hid three live inversions, all of the same shape -- a gate PROMOTED to
# default-ON in C++ and in docs/tier1_env_vars.md while the shader comment
# describing the very same gate kept saying OFF:
#   shaders/include/lighting.hglsl  MC2_LIGHT_POS_COLUMN_FIX    (VEHICLE-LIGHTING-PATH-1)
#   shaders/gos_terrain.frag        MC2_SHADOW_STATIC_BANDFIX   (SHADOW-BANDING-DEFAULT-ON)
#   shaders/gos_terrain.frag        MC2_SHADOW_STATIC_BANDFIX_V2(SHADOW-BANDING-DEFAULT-ON)
#
# Shaders are added to the CLAIM sweep only, never to load_sources(). GLSL has
# no getenv(), so a shader can never be a reader -- it receives its gate state as
# an injected #define or a uniform. Adding them to the reader scan could only
# manufacture noise, while the authority a shader comment must be checked
# against still lives in C++. Comment syntax is the same // and /* */, so
# comment_claims() needs no change.
CLAIM_DIRS = ["shaders"]
CLAIM_EXTS = (".hglsl", ".glsl", ".vert", ".frag", ".comp", ".geom",
              ".tesc", ".tese", ".mesh", ".task")

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
    # envIsSet("X") is literally a presence test, so unset means OFF. Needed once
    # Trace-kind entries are checked: visual_tuning_profile.cpp gates every
    # per-profile override on `if (!envIsSet(VAR))`, i.e. an operator-set value wins.
    (OFF, _R(r"\benvIsSet\s*\("), "envIsSet() presence test"),
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
    # Generalisation of the two above: `if (!v || <anything>) return X`. The
    # `!v ||` short-circuits when the var is unset, so X *is* the default no
    # matter what the second disjunct tests. shader_builder.cpp:420 writes it as
    # `if (!e || e[0] != '1') return false;` and no rule covered that, so it fell
    # through to whatever else happened to be in the enclosing block.
    (OFF, _R(r"if\s*\(\s*!\s*\w+\s*\|\|[^;{)]*\)\s*return\s+(?:false|0)\b"),
     "if(!v || ...) return false [explicit unset]"),
    (ON, _R(r"if\s*\(\s*!\s*\w+\s*\|\|[^;{)]*\)\s*return\s+(?:true|1)\b"),
     "if(!v || ...) return true [explicit unset]"),

    # v && v[0] && !(v[0] == '0' && v[1] == '\0')  -- PRESENCE-gated, default OFF.
    # Same shape as the rule below but with one extra conjunct, which is enough
    # to slip past `\w+\s*&&\s*!\s*\(` and land on the generic kill-switch rule.
    # Found by extending this checker to Trace-kind entries (ENV-DROP-CLOSE-1):
    # it manufactured a DISAGREEMENT against MC2_DIAG_VERBOSE
    # (GameOS/include/mc2_diag_verbose.h:58), an honest default-OFF gate.
    (OFF, _R(r"\w+\s*&&\s*\w+\s*\[\s*0\s*\]\s*&&\s*!\s*\([^;]*\[\s*0\s*\]\s*==\s*'0'"),
     "v && v[0] && !(v[0]=='0') presence-gated"),
    # ...and the same shape written out longhand:
    #   v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0')
    # Any number of leading conjuncts, as long as SOMETHING is AND-ed in front
    # of the `!(...=='0')`: that leading conjunct is what makes unset resolve
    # false. gos_terrain_surface.cpp:73 writes it this way and was classified ON.
    (OFF, _R(r"\w+\s*(?:!=\s*(?:nullptr|NULL)\s*)?&&\s*(?:[^;]*&&\s*)?"
             r"!\s*\([^;]*\[\s*0\s*\]\s*==\s*'0'"),
     "<presence> && ... && !(v[0]=='0') presence-gated"),
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
#
# atol/atoll/strtoll/strtof were MISSING until ENV-DROP-CLOSE-1. Every numeric
# Trace knob in code/ uses `(v && v[0]) ? atol(v) : DEFAULT` -- eight of them --
# and each one landed in UNCLASSIFIABLE (a FAILURE) purely because the most
# common integer parser in this codebase was not on the list.
VALUE_READ_RE = re.compile(
    r"\batof\s*\(|\batoi\s*\(|\batol\s*\(|\batoll\s*\(|\b_atoi64\s*\(|"
    r"\bstrtol\s*\(|\bstrtoll\s*\(|\bstrtod\s*\(|\bstrtof\s*\(|\bstrtoul\s*\(|"
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
            return _isolate_reader(text, blk[0], blk[1], pos)
    start = max(text.rfind(";", 0, pos), text.rfind("{", 0, pos),
                text.rfind("}", 0, pos))
    start = 0 if start == -1 else start + 1
    # The fallback tail is a fixed 400-char slice, so it happily runs straight
    # through the NEXT gate's resolution. Isolate here too: gos_postprocess.cpp's
    # SSAO block is over MAX_BLOCK, so MC2_SSAO_DEBUG took this path and picked
    # up MC2_SSAO_BLUR's kill-switch idiom. Isolating only the block path fixed
    # the small cases and left the big ones -- which are the poisonous ones.
    return _isolate_reader(text, start, min(len(text), pos + FALLBACK_TAIL), pos)


_GETENV_RE = re.compile(r"\bgetenv\s*\(")
#: `const char* v = std::getenv(` -- the local this read is bound to.
_BIND_RE = re.compile(r"([A-Za-z_]\w*)\s*=\s*(?:std\s*::\s*)?$")


def _isolate_reader(text, lo, hi, pos):
    """Restrict a block window to the statements that talk about THIS read.

    A block that resolves several gates in a row is common, and it silently
    poisons classification: the rules scan the whole window and the first match
    wins regardless of which variable it belongs to. Two real cases, both found
    by extending this checker to Trace-kind entries (ENV-DROP-CLOSE-1):

      gos_postprocess.cpp:975-983 resolves MC2_SSAO, MC2_SSAO_DEBUG and
      MC2_SSAO_BLUR in one brace block. MC2_SSAO_DEBUG is presence-gated
      (default OFF, registry says OFF) but the window also held MC2_SSAO_BLUR's
      `!(blurEnv[0]=='0')` kill-switch, so the checker said ON and reported a
      DISAGREEMENT against an honest entry -- inventing a lie in the tool whose
      only job is finding lies.

      spawn.cpp:32-39 hoists BOTH getenv() calls before either classification,
      so trimming at "the next sibling getenv" would cut the answer off. The
      binding identifier is what actually separates the two reads, not position.

    So: find the local the getenv result is bound to, then keep the binding
    statement plus every later statement in the block naming that local. With
    no binding (inline `if (getenv(X))`) the idiom is on the statement itself
    and the whole block is returned unchanged -- the pre-existing behaviour.
    """
    body = text[lo:hi]
    if len(_GETENV_RE.findall(body)) <= 1:
        return body
    stmt_start = max(text.rfind(";", lo, pos), text.rfind("{", lo, pos),
                     text.rfind("}", lo, pos))
    stmt_start = lo if stmt_start == -1 else stmt_start + 1
    gv = _GETENV_RE.search(text, stmt_start, pos + 1)
    m = _BIND_RE.search(text[stmt_start:gv.start()]) if gv else None
    if not m:
        return body
    name = m.group(1)
    named = re.compile(r"\b%s\b" % re.escape(name))
    parts = text[stmt_start:hi].split(";")
    kept = [parts[0]] + [p for p in parts[1:] if named.search(p)]
    return ";".join(kept) + ";"


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

#: helper("MC2_X", true)  -- the default is a LITERAL ARGUMENT, so there is
#: nothing to infer. Invisible until ENV-DROP-CLOSE-1 because the reader needle
#: only matched a single-argument call, and 22 sites use this form -- including
#: gos_static_prop_registry.cpp:141 `parseEnvBoolWithDefault(
#: "MC2_STATIC_PROP_MISSION_LOAD_REG", true)`, the reader that code/objmgr.cpp
#: contradicted for three months. The comment check could not have caught that
#: comment while the reader behind it was unreadable.
_DEFAULT_ARG_TAIL = re.compile(r"\A\s*,\s*(true|false)\s*\)")
#: Callees for which a ("MC2_X", bool) pair is NOT a default declaration.
_NOT_A_READER_CALLEE = re.compile(
    r"^(?:f?printf|vlog|setenv|_putenv(?:_s)?|SetEnvironmentVariable\w*|"
    r"make_pair|emplace_back|push_back|insert)$")


#: The SAME needle as the per-var one below, with the name captured instead of
#: interpolated. BACKEND-DEFAULT-PARITY-1 added it because every reader question
#: this file asks used to start from a LIST OF NAMES KNOWN IN ADVANCE (the
#: registry), and the one rule that does not need a name -- "do this var's own
#: readers contradict each other?" -- was asked var-by-var through that list
#: anyway. See reader_conflicts() for what that cost.
_ANY_VAR_NEEDLE = re.compile(r'\(\s*"(MC2_[A-Z0-9_]+)"\s*(?=[),])')


def _reader_hits(sources, helpers, var=None):
    """-> {var: [ {path, line, verdict, rule, stmt}, ... ]}.

    `var=None` scans EVERY MC2_* name in the call-argument position, in one pass.
    ONE implementation, two entry points (find_readers / find_all_readers) --
    deliberately, because a second copy of this classifier is a second opinion
    about what a default is, which is the very defect the file exists to catch.

    `sources` must already be comment-stripped, which is what makes a var named
    only in a comment incapable of registering as a reader.
    """
    helpers = helpers or {}
    needle = (_ANY_VAR_NEEDLE if var is None
              else re.compile(r'\(\s*"(' + re.escape(var) + r')"\s*(?=[),])'))
    out = {}
    for path, body in sources:
        for m in needle.finditer(body):
            name = m.group(1)
            # Identify the callee immediately left of the '(' -- getenv, or a
            # resolved helper, or something else entirely.
            callee_m = CALLEE_RE.search(body[max(0, m.start() - 64):m.start()])
            callee = callee_m.group(1) if callee_m else ""

            # helper("MC2_X", true) -- the default is stated outright.
            arg = _DEFAULT_ARG_TAIL.match(body[m.end():m.end() + 32])
            if arg:
                if _NOT_A_READER_CALLEE.match(callee):
                    continue
                out.setdefault(name, []).append({
                    "path": path,
                    "line": body.count("\n", 0, m.start()) + 1,
                    "verdict": ON if arg.group(1) == "true" else OFF,
                    "rule": f"explicit default argument -> {arg.group(1)}",
                    "stmt": f"{callee}(\"{name}\", {arg.group(1)})",
                })
                continue
            if body[m.end():m.end() + 1] != ")":
                continue   # some other multi-arg call; not a default declaration

            stmt = statement_around(body, m.start())

            if callee in helpers and helpers[callee] in (ON, OFF):
                verdict, rule = helpers[callee], f"helper {callee}() -> {helpers[callee]}"
            else:
                verdict, rule = classify_statement(stmt)

            if verdict is None:
                continue
            out.setdefault(name, []).append({
                "path": path,
                "line": body.count("\n", 0, m.start()) + 1,
                "verdict": verdict,
                "rule": rule,
                "stmt": re.sub(r"\s+", " ", stmt).strip()[:180],
            })
    return out


def find_readers(var, sources, helpers=None):
    """-> list of dicts {path, line, verdict, rule, stmt} for ONE var."""
    return _reader_hits(sources, helpers, var).get(var, [])


def find_all_readers(sources, helpers=None):
    """-> {var: [hit, ...]} for EVERY MC2_* var with a reader, registered or not."""
    return _reader_hits(sources, helpers, None)


def reader_conflicts(all_readers, allow):
    """-> [(var, readers)] for every var with BOTH an ON reader and an OFF one.

    BACKEND-DEFAULT-PARITY-1 -- WHY THIS IS ITS OWN PASS OVER THE READERS.
    ---------------------------------------------------------------------
    READER_CONFLICT was written for exactly one shape: "two readers of the same
    variable disagree about the unset default, so the same command line produces
    different behaviour depending on which one you land on." It lived INSIDE the
    `for e in registry_entries` loop, so it could only ever see that shape on a
    var somebody had already registered -- and an unregistered var is precisely
    the one nobody has audited. Worse, the loop's UNKNOWN branch ran FIRST, so a
    registered var with readers {ON, OFF, UNCLASSIFIABLE} was reported as merely
    unreadable rather than as contradictory.

    The comment rule then hid the same vars a second time. It attributes a claim
    only when a var has exactly one non-UNCLASSIFIABLE verdict, so a var whose
    readers CONFLICT gets no `implemented` value, and every comment and doc line
    about it is skipped -- silently. Two readers disagreeing therefore bought
    total immunity from the third rule as well.

    MC2_SKYBOX_FOG_EXCLUDE sat in that blind spot for the entire life of the
    gate: gos_postprocess.cpp:578 `!(v && v[0] == '0')` (default ON, GL) against
    vulkan_post_fog_native.cpp:589 `v && v[0] == '1'` (default OFF, VK), plus two
    shader comments asserting "default OFF". Unregistered, so rule 1 never looked;
    conflicting, so rule 3 dropped it. Three rules, zero coverage.

    Vars on the allowlist are skipped here as everywhere else -- that file is a
    DEBT list with a written reason per line, not silence.
    """
    out = []
    for var in sorted(all_readers):
        if var in allow:
            continue
        hard = {r["verdict"] for r in all_readers[var]} & {ON, OFF}
        if len(hard) > 1:
            out.append((var, all_readers[var]))
    return out


def implemented_default(readers):
    """-> ON / OFF when every CLASSIFIED reader agrees; None otherwise.

    UNCLASSIFIABLE readers are IGNORED here rather than fatal, which is the
    second half of the BACKEND-DEFAULT-PARITY-1 fix. "I cannot read this idiom"
    is not evidence that the default differs, but rule 3 treated it as such: it
    attributed a comment claim only when a var had exactly ONE verdict and that
    verdict was classifiable, so a single unreadable sibling reader made every
    comment and doc line about the var unauditable. That is how
    docs/tier1_env_vars.md:783 could say MC2_WATER_REFLECTION is "Default
    **OFF**" while gos_GetWaterReflectionGate() reads
    `(v && v[0] == '0') ? 0 : 1` -- default ON -- and its sibling
    gos_GetWaterReflStrength() returns 1.5 unset. Measured cost of the widening
    across the whole tree: 15 vars newly attributable, 2 new contradictions, of
    which 1 was a real lie and 1 was the `whenever` gap now closed in _COND_RE.

    Rule 1 (registry vs reader) still refuses to guess and keeps failing
    UNCLASSIFIABLE for Feature entries -- there the unreadable idiom IS the
    finding. These two rules want different things from the same input.
    """
    hard = {r["verdict"] for r in readers} & {ON, OFF}
    return hard.pop() if len(hard) == 1 else None


# ---------------------------------------------------------------------------
# Comment claims (ENV-DROP-CLOSE-1)
# ---------------------------------------------------------------------------
#
# The registry is not the only place a default is written down, and it is not
# where a reader looks first. code/objmgr.cpp:1610 said
#
#     // Default-off: MC2_STATIC_PROP_MISSION_LOAD_REG=0 (first-render fallback covers).
#
# for nearly three months while gos_static_prop_registry.cpp:141 read
# `parseEnvBoolWithDefault(..., true)`. Mission-load registration was LIVE and
# the comment said it was not. Nothing machine-checked that, because this file
# STRIPS comments -- correctly, so that a commented-out getenv cannot pose as a
# reader. Stripping them for classification and never checking them at all are
# two different decisions, and only the first one was deliberate.
#
# SCOPE IS DELIBERATELY NARROW, because a proximity-based version of this rule
# was tried first and produced 18 hits of which every single one was a false
# positive: comments about a NEIGHBOURING gate ("MC2_SSAO_BLUR defaults ON" four
# lines from MC2_SSAO_DEBUG), and conditional claims ("default-ON under the
# umbrella"). A rule with that hit rate gets deleted by the first person it
# annoys. So a claim counts only when all of the following hold:
#   * it is on a COMMENT line,
#   * that line names EXACTLY ONE MC2_* variable -- so the claim cannot be about
#     a neighbour,
#   * the claim is unconditional (no under/when/unless/if/pending/previously/...
#     qualifying it), and
#   * the variable has exactly one non-UNCLASSIFIABLE reader verdict.
# That leaves 106 assertions in-scope across the tree.

_VAR_RE = re.compile(r"MC2_[A-Z0-9_]+")
_CLAIM_RE = re.compile(
    r"default(?:s|ed)?\s*(?:is\s+|to\s+|=\s*|-|:|\s)\s*\**\s*(on|off)\b", re.I)
#: BACKEND-DEFAULT-PARITY-1 added `whenever`/`wherever`: `\bwhen\b` does not match
#: `whenever` (the boundary fails on the trailing "ever"), so two genuinely
#: conditional claims -- "Default ON whenever SSAO itself is on"
#: (gos_postprocess.h:362) and "default ON whenever MC2_SMOKE_MODE is set"
#: (mclib/vis_semantics.h:91, a claim about a DIFFERENT gate) -- read as
#: unconditional assertions. Both surfaced the moment attribution widened below;
#: the second would have been a false positive, which is how this rule dies.
_COND_RE = re.compile(
    r"\b(under|when(?:ever)?|wherever|unless|if|while|once|pending|until|"
    r"would|was|used to|previously|no longer|should)\b", re.I)
_COMMENT_LINE_RE = re.compile(r"^\s*(?://|\*|/\*)")


def comment_claims(lines, path, prose=False):
    """-> list of {var, claim, path, line, text} for attributable claims.

    `prose=True` skips the comment-marker test so the same rule can be applied
    to a markdown doc, where every line is prose.
    """
    out = []
    for i, ln in enumerate(lines, 1):
        if not prose and not _COMMENT_LINE_RE.match(ln):
            continue
        names = set(_VAR_RE.findall(ln))
        if len(names) != 1:
            continue
        a = _CLAIM_RE.search(ln)
        if not a:
            continue
        if _COND_RE.search(ln[:a.start()]) or _COND_RE.search(ln[a.end():]):
            continue
        out.append({"var": names.pop(), "claim": a.group(1).upper(),
                    "path": path, "line": i, "text": ln.strip()[:160]})
    return out


def load_comment_claims(root=WORKTREE, scan_dirs=None, exclude=(REGISTRY,),
                        claim_dirs=None):
    scan_dirs = SCAN_DIRS if scan_dirs is None else scan_dirs
    claim_dirs = CLAIM_DIRS if claim_dirs is None else claim_dirs
    excl = {os.path.normcase(os.path.abspath(p)) for p in exclude}
    claims = []

    def sweep(dirs, exts):
        for d in dirs:
            base = os.path.join(root, d)
            if not os.path.isdir(base):
                continue
            for dirpath, dirnames, filenames in os.walk(base):
                dirnames[:] = [x for x in dirnames
                               if x not in (".git", "build64", "third_party", "3rdparty")]
                for fn in filenames:
                    if not fn.endswith(exts):
                        continue
                    full = os.path.join(dirpath, fn)
                    if os.path.normcase(os.path.abspath(full)) in excl:
                        continue
                    try:
                        with open(full, "r", encoding="utf-8", errors="replace") as fh:
                            lines = fh.read().splitlines()
                    except OSError:
                        continue
                    claims.extend(comment_claims(
                        lines, os.path.relpath(full, root).replace("\\", "/")))

    sweep(scan_dirs, SCAN_EXTS)
    # SHADER-COMMENT-TRUTH-1: the same rule over GLSL. See CLAIM_DIRS above for
    # why shaders are swept for CLAIMS but never for READERS.
    sweep(claim_dirs, CLAIM_EXTS)
    # docs/tier1_env_vars.md is the OTHER place a default is written down, and
    # it is the one a human is told to read. The same three lies that shipped in
    # the registry shipped there too, and this sweep found three more that only
    # exist in the doc -- including MC2_STANDARD_LIT_V1, documented as "Default
    # ON. Kill=`=0`" while gos_mech_batcher.cpp:365 reads
    # `(v != nullptr && v[0] == '1')  // TEMPORARY DEFAULT-OFF`.
    doc = os.path.join(root, "docs", "tier1_env_vars.md")
    if os.path.isfile(doc):
        try:
            with open(doc, "r", encoding="utf-8", errors="replace") as fh:
                claims.extend(comment_claims(fh.read().splitlines(),
                                             "docs/tier1_env_vars.md",
                                             prose=True))
        except OSError:
            pass
    return claims


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


def analyse(registry_text, sources, allow, claims=None):
    """-> (findings, stats)."""
    entries = parse_registry(registry_text)
    helpers = resolve_helpers(sources)
    # BACKEND-DEFAULT-PARITY-1: ONE reader pass for the whole tree, keyed by var.
    # Everything below indexes into it instead of re-scanning per registry entry
    # (which was also ~340 full-tree scans), and the conflict rule below can now
    # see vars the registry has never heard of.
    all_readers = find_all_readers(sources, helpers)
    findings = []
    implemented_by_var: dict = {}
    stats = {"feature_entries": 0, "checked": 0, "no_reader": 0,
             "agree": 0, "disagree": 0, "unclassifiable": 0,
             "unclassifiable_nonfeature": 0,
             "comment_claims": 0, "comment_agree": 0, "comment_disagree": 0,
             "reader_conflict": 0, "allowlisted": 0,
             "helpers_resolved": len([v for v in helpers.values()
                                      if v in (ON, OFF)])}

    # --- rule 2, run over the READERS (BACKEND-DEFAULT-PARITY-1) --------------
    # Registered or not, allowlisted excepted. See reader_conflicts() for why
    # this is no longer a branch inside the registry loop.
    conflicts = dict(reader_conflicts(all_readers, allow))
    stats["reader_conflict"] = len(conflicts)
    stats["reader_vars_scanned"] = len(all_readers)
    for var in sorted(conflicts):
        rs = conflicts[var]
        reg = "REGISTERED" if any(e["var"] == var for e in entries) else "UNREGISTERED"
        findings.append({
            "kind": "READER_CONFLICT", "var": var,
            "msg": ("readers disagree with EACH OTHER about the unset default "
                    f"({reg}). The same unset environment therefore behaves "
                    "differently depending on which reader the code path hits."),
            "sites": [f"{r['path']}:{r['line']}  [{r['verdict']}] {r['rule']}"
                      for r in sorted(rs, key=lambda x: (x["verdict"], x["path"],
                                                         x["line"]))],
        })

    for e in entries:
        # ENV-DROP-CLOSE-1: ALL kinds are compared now. This used to be
        # Feature-only ("Trace/Retired: defaultOn carries no shipping
        # semantics"), which left 147 of the registry's 340 env entries
        # unchecked -- including boolean Trace gates, where an inverted
        # defaultOn misleads exactly the same way a Feature one does.
        #
        # UNCLASSIFIABLE stays a FAILURE for Feature and is REPORTED-ONLY for
        # the other kinds. That asymmetry is deliberate and is the one place
        # this file relaxes its own doctrine: a Trace entry's defaultOn is a
        # formality on what is usually a numeric or string knob
        # (MC2_LIGHT_POOL_MAX, MC2_STATIC_PROP_IBL_SH_SET, the *_GL_CANARY
        # probes), so failing on an unreadable idiom there is a false-positive
        # mill rather than a lie detector. DISAGREEMENT and READER_CONFLICT --
        # the two findings that mean a written default contradicts a binary --
        # fail for every kind.
        stats["feature_entries"] += 1
        strict = e["kind"] == "Feature"
        readers = all_readers.get(e["var"], [])

        if not readers:
            # No reader found. That is check-env-registry.sh / dead-gate
            # territory, not a default-agreement question. Counted, not failed.
            stats["no_reader"] += 1
            continue

        stats["checked"] += 1
        verdicts = {r["verdict"] for r in readers}
        impl0 = implemented_default(readers)
        if impl0 is not None:
            implemented_by_var[e["var"]] = impl0
        if e["var"] in allow:
            stats["allowlisted"] += 1
            continue

        stated = ON if e["default_on"] else OFF

        # Already reported by the reader-scan pass above, and no single
        # `implemented` default exists to compare the registry against. This
        # check precedes the UNKNOWN branch on purpose: readers {ON, OFF,
        # UNCLASSIFIABLE} is a CONTRADICTION first and an unreadable idiom
        # second, and the old ordering downgraded it to the weaker finding.
        if e["var"] in conflicts:
            continue

        if UNKNOWN in verdicts:
            stats["unclassifiable"] += 1
            if not strict:
                stats["unclassifiable_nonfeature"] += 1
                continue
            bad = [r for r in readers if r["verdict"] == UNKNOWN]
            findings.append({
                "kind": "UNCLASSIFIABLE", "var": e["var"],
                "msg": ("reader idiom not recognised, so its default cannot be "
                        f"compared with the registry's stated {stated}"),
                "sites": [f"{r['path']}:{r['line']}  {r['stmt']}" for r in bad],
            })
            continue

        # Guaranteed singleton here: not in `conflicts` (so at most one of
        # ON/OFF) and no UNKNOWN, with at least one reader.
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

    # --- comment claims vs the implemented default ------------------------
    # Deliberately NOT limited to registered vars. The comment defect this rule
    # exists for (code/objmgr.cpp:1610) named MC2_STATIC_PROP_MISSION_LOAD_REG,
    # which is not in RendererFeatureRegistry.h at all -- so scoping the sweep
    # to the registry would have missed the exemplar.
    for var in sorted({c["var"] for c in (claims or [])} - set(implemented_by_var)):
        impl0 = implemented_default(all_readers.get(var, []))
        if impl0 is not None:
            implemented_by_var[var] = impl0

    for c in (claims or []):
        impl = implemented_by_var.get(c["var"])
        if impl is None or c["var"] in allow:
            continue
        stats["comment_claims"] += 1
        if c["claim"] == impl:
            stats["comment_agree"] += 1
            continue
        stats["comment_disagree"] += 1
        findings.append({
            "kind": "COMMENT_DISAGREEMENT", "var": c["var"],
            "msg": (f"{c['path']} states default {c['claim']} but the "
                    f"reader implements default {impl}. The reader is the only "
                    f"authority; fix the comment (or the reader)."),
            "sites": [f"{c['path']}:{c['line']}  {c['text']}"],
        })

    return findings, stats


def run(quiet=False):
    with open(REGISTRY, "r", encoding="utf-8", errors="replace") as fh:
        registry_text = fh.read()
    sources = load_sources()
    allow, malformed = load_allowlist()
    findings, stats = analyse(registry_text, sources, allow,
                              claims=load_comment_claims())

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
        print(f"  Registry entries with an env var: {stats['feature_entries']}"
              "   (ALL kinds)")
        print(f"  ... with a reader found         : {stats['checked']}")
        print(f"  ... no reader in scanned source : {stats['no_reader']}")
        print(f"  allowlisted (explicit reason)   : {stats['allowlisted']}")
        print(f"  AGREE                           : {stats['agree']}")
        print(f"  DISAGREE                        : {stats['disagree']}")
        print(f"  READER CONFLICT                 : {stats['reader_conflict']}")
        print(f"  UNCLASSIFIABLE                  : {stats['unclassifiable']}"
              f"   (of which {stats['unclassifiable_nonfeature']} non-Feature "
              "= reported, not failed)")
        print(f"  comment/doc default claims      : {stats['comment_claims']}")
        print(f"  ... AGREE with the reader       : {stats['comment_agree']}")
        print(f"  ... CONTRADICT the reader       : {stats['comment_disagree']}")
        print()
        for f in findings:
            print(f"  [{f['kind']}] {f['var']}")
            print(f"      {f['msg']}")
            for s in f["sites"]:
                print(f"      - {s}")
            print()
        if not findings:
            print("  OK: every classifiable gate's registry default matches its "
                  "reader, and no in-source comment or tier1 doc line contradicts one.")
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
      "Trace kind numeric tunable -- atol() value read, no boolean to compare" },
    { "MC2_TRACE_ST_LIAR",         "MC2_ST_TRACE_LIAR", EnvVarKind::Trace,   false,
      "Trace kind BOOLEAN gate that lies -- must be caught now that all kinds "
      "are compared" },
    { "MC2_TRACE_ST_ODD",          "MC2_ST_TRACE_ODD",  EnvVarKind::Trace,   false,
      "Trace kind unreadable idiom -- reported, NOT failed" },
    { "MC2_FEATURE_ST_SIBLING",    "MC2_ST_SIBLING",    EnvVarKind::Feature, false,
      "presence-gated, but shares a block with a kill-switch sibling" },
    { "MC2_FEATURE_ST_HOISTED",    "MC2_ST_HOISTED",    EnvVarKind::Feature, false,
      "two getenv()s hoisted before either classification" },
    { "MC2_FEATURE_ST_ORGUARD",    "MC2_ST_ORGUARD",    EnvVarKind::Feature, false,
      "if (!v || v[0] != '1') return false" },
    { "MC2_FEATURE_ST_TRISPLIT",   "MC2_ST_TRISPLIT",   EnvVarKind::Feature, true,
      "GATE-PARITY-CLOSE-1: ON + OFF + UNREADABLE readers -- a CONTRADICTION "
      "that also happens to have an unreadable sibling. Must be reported as "
      "READER_CONFLICT, not downgraded to UNCLASSIFIABLE." },
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
// BACKEND-DEFAULT-PARITY-1 fixture: the EXACT shape of MC2_SKYBOX_FOG_EXCLUDE --
// two readers with opposite unset defaults on a var that is in NO registry table,
// plus a comment asserting the losing one. Before the conflict rule scanned
// readers instead of the registry this was invisible twice over: unregistered, so
// the registry loop never looked at it; conflicting, so the comment rule dropped
// it for having no single implemented verdict.
// MC2_ST_UNREG_SPLIT default OFF
bool unregSplitA() {
    const char* v = getenv("MC2_ST_UNREG_SPLIT");
    return !(v && v[0] == '0');
}
bool unregSplitB() {
    const char* v = getenv("MC2_ST_UNREG_SPLIT");
    return v && v[0] == '1';
}
// A var with ONE classified reader and one unreadable one must NOT be called a
// conflict -- "I cannot classify it" is not "it disagrees", and a rule that
// conflates them is the false-positive mill this file refuses to become.
bool mixedA() {
    const char* v = getenv("MC2_ST_MIXED");
    return !(v && v[0] == '0');
}
bool mixedB() {
    const char* v = getenv("MC2_ST_MIXED");
    return v ? someUnknownHelper(v) : fallbackFromConfigFile();
}
// GATE-PARITY-CLOSE-1 fixture: a REAL contradiction (ON vs OFF) that ALSO has a
// third, unreadable reader. Historically analyse()'s UNKNOWN branch ran before
// the conflict branch, so this shape was reported as "idiom not recognised"
// rather than "these two readers contradict each other" -- the weaker finding,
// naming the wrong file, and leaving the actual disagreement unstated. The
// {ON,OFF,UNKNOWN} shape had NO fixture until this one: re-introducing the old
// `drop any var with 2+ distinct verdicts` behaviour left the whole selftest
// green, which is how the defect survived its own regression suite.
bool triSplitA() {
    const char* v = getenv("MC2_ST_TRISPLIT");
    return !(v && v[0] == '0');
}
bool triSplitB() {
    const char* v = getenv("MC2_ST_TRISPLIT");
    return v && v[0] == '1';
}
bool triSplitC() {
    const char* v = getenv("MC2_ST_TRISPLIT");
    return v ? someUnknownHelper(v) : fallbackFromConfigFile();
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
    return v ? atol(v) : 2500;
}
bool traceLiar() {
    const char* v = getenv("MC2_ST_TRACE_LIAR");
    return !(v && v[0] == '0');
}
bool traceOdd() {
    const char* v = getenv("MC2_ST_TRACE_ODD");
    return v ? someUnknownHelper(v) : fallbackFromConfigFile();
}
// SIBLING/HOISTED: two gates resolved inside ONE block. The presence-gated one
// must not be classified from its neighbour's kill-switch idiom.
void resolveSiblings() {
    const char* sib = getenv("MC2_ST_SIBLING");
    s_sibling = (sib && sib[0] && sib[0] != '0') ? 1 : 0;
    const char* killer = getenv("MC2_ST_KILLSWITCH");
    s_killer = !(killer && killer[0] == '0');
}
void resolveHoisted() {
    const char* h  = getenv("MC2_ST_HOISTED");
    const char* hk = getenv("MC2_ST_HOISTED_KILL");
    s_hoisted = (h && h[0] == '1');
    s_hoistedKill = !(hk && hk[0] == '0');
}
bool orGuard() {
    const char* e = getenv("MC2_ST_ORGUARD");
    if (!e || e[0] != '1') return false;
    return true;
}
// COMMENT CLAIMS. The first line is the objmgr.cpp:1610 shape: one variable,
// an unconditional default assertion, and a reader that says the opposite.
// MC2_ST_HONEST_ON is Default-off: set =0 to disable.
// The next two must NOT be attributed -- two variables in one line, and a
// conditional claim. Both were real false-positive shapes in the live tree.
// MC2_ST_HONEST_OFF and MC2_ST_LIAR both default ON in that configuration.
// MC2_ST_HONEST_OFF is default-ON under the umbrella gate.
'''


def _selftest():
    failures = []
    ran = []

    def check(label, cond, detail=""):
        ran.append(label)
        if cond:
            print(f"  PASS  {label}")
        else:
            print(f"  FAIL  {label}  {detail}")
            failures.append(label)

    print("GATE-DEFAULT-TRUTH-1 self-test")
    print("-" * 70)

    sources = [("selftest_fake.cpp", strip_comments(_SELF_SOURCE))]
    claims = comment_claims(_SELF_SOURCE.splitlines(), "selftest_fake.cpp")
    findings, stats = analyse(_SELF_REGISTRY, sources, allow={}, claims=claims)
    by_var = {f["var"]: f for f in findings}
    by_kind = {(f["kind"], f["var"]) for f in findings}

    # 1. THE REAL THING: registry says OFF, code says ON -- the exact shape of
    #    MC2_SHADOW_ENABLE and MC2_SHADOW_BOUNDED_NEAR_FIT.
    check("catches a REAL default disagreement (registry OFF, reader ON)",
          by_var.get("MC2_ST_LIAR", {}).get("kind") == "DISAGREEMENT",
          str(by_var.get("MC2_ST_LIAR")))

    # 2. Honest entries must NOT be flagged, or the rule is noise and the first
    #    person it annoys deletes it.
    # (It DOES carry a deliberate comment lie for case 7c, so scope this to the
    # registry-vs-reader findings rather than "appears in findings at all".)
    check("does not flag an honest default-ON gate",
          not any(k for k, v in by_kind
                  if v == "MC2_ST_HONEST_ON" and k != "COMMENT_DISAGREEMENT"),
          str(by_kind))
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

    # 4b. BACKEND-DEFAULT-PARITY-1 -- THE HOLE THE RULE ABOVE HAD. Same shape,
    #     on a var in NO registry table. The rule was iterating the REGISTRY, so
    #     it structurally could not see the case it was written for whenever the
    #     var was unregistered -- which is the case nobody has audited. This is
    #     MC2_SKYBOX_FOG_EXCLUDE (GL default-ON vs VK default-OFF) exactly.
    check("an UNREGISTERED var with conflicting readers is caught",
          by_var.get("MC2_ST_UNREG_SPLIT", {}).get("kind") == "READER_CONFLICT",
          str(by_var.get("MC2_ST_UNREG_SPLIT")))
    check("the conflict finding says the var is UNREGISTERED",
          "UNREGISTERED" in by_var.get("MC2_ST_UNREG_SPLIT", {}).get("msg", ""),
          str(by_var.get("MC2_ST_UNREG_SPLIT")))

    # 4c. And the inverse, so the widened rule is not a false-positive mill:
    #     one classified reader plus one UNREADABLE one is not a contradiction.
    check("one classified + one unclassifiable reader is NOT a conflict",
          by_var.get("MC2_ST_MIXED", {}).get("kind") != "READER_CONFLICT",
          str(by_var.get("MC2_ST_MIXED")))

    # 4d. GATE-PARITY-CLOSE-1 -- THE SHAPE THAT HAD NO FIXTURE AT ALL.
    #     A genuine ON-vs-OFF contradiction that ALSO carries an unreadable third
    #     reader. Both 4b and 4c are two-reader cases, so neither pins the
    #     ORDERING between the conflict branch and the UNKNOWN branch, and neither
    #     pins that the verdict set is intersected with {ON,OFF} rather than
    #     counted. Re-introducing the historical "drop any var with 2+ distinct
    #     verdicts" rule leaves EVERY other assertion in this file green -- proven
    #     by injecting exactly that into the live tree. Asserting the KIND (not
    #     merely "something was reported") is the point: UNCLASSIFIABLE also
    #     fails the run, so a bare exit-code check cannot tell the strong finding
    #     from the weak one that names the wrong file and never says "disagree".
    check("a conflict WITH an unreadable third reader is still a CONFLICT",
          by_var.get("MC2_ST_TRISPLIT", {}).get("kind") == "READER_CONFLICT",
          str(by_var.get("MC2_ST_TRISPLIT")))

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

    # 7. ALL KINDS ARE COMPARED (ENV-DROP-CLOSE-1). A numeric Trace knob is still
    #    skipped -- but because atol() makes it a VALUE READ, not because of its
    #    kind. A BOOLEAN Trace gate that lies must now be caught: 144 Trace
    #    entries used to be exempt purely for being Trace.
    check("a numeric Trace knob is skipped as a value read",
          "MC2_ST_VALUE" not in by_var)
    check("a BOOLEAN Trace gate that lies IS caught (all kinds compared)",
          by_var.get("MC2_ST_TRACE_LIAR", {}).get("kind") == "DISAGREEMENT",
          str(by_var.get("MC2_ST_TRACE_LIAR")))
    check("an unreadable Trace idiom is reported, NOT failed",
          "MC2_ST_TRACE_ODD" not in by_var
          and stats["unclassifiable_nonfeature"] == 1,
          f"nonfeature_unclass={stats['unclassifiable_nonfeature']}")

    # 7b. BLOCK CONTAMINATION. Both shapes below put two gates in one block. The
    #     presence-gated gate is default OFF and the registry says OFF, so a
    #     finding here means the checker classified it from its NEIGHBOUR --
    #     which is what it did to MC2_SSAO_DEBUG (gos_postprocess.cpp) and would
    #     have done to MC2_VFX_AGE_SAMPLE (spawn.cpp) under a naive fix.
    check("a gate is not classified from a SIBLING gate in the same block",
          "MC2_ST_SIBLING" not in by_var, str(by_var.get("MC2_ST_SIBLING")))
    check("hoisted getenv()s do not cut the classification off",
          "MC2_ST_HOISTED" not in by_var, str(by_var.get("MC2_ST_HOISTED")))
    check("`if (!v || <anything>) return false` reads as default OFF",
          "MC2_ST_ORGUARD" not in by_var, str(by_var.get("MC2_ST_ORGUARD")))

    # 7c. COMMENTS ARE NOW CHECKED TOO. Stripping them so they cannot pose as a
    #     reader (case 5) is right; never comparing them to the reader is what
    #     let code/objmgr.cpp:1610 say "Default-off" for ~3 months about a gate
    #     whose reader defaulted true. Note this is the exact inverse of case 5,
    #     and both must hold: comment text is not a READER, and it is not
    #     exempt from being CHECKED.
    check("an in-source comment contradicting the reader is caught",
          ("COMMENT_DISAGREEMENT", "MC2_ST_HONEST_ON") in by_kind,
          str(findings))
    check("a comment naming TWO vars is not attributed to either",
          ("COMMENT_DISAGREEMENT", "MC2_ST_HONEST_OFF") not in by_kind,
          str([c for c in claims]))
    check("a CONDITIONAL default claim is not treated as an assertion",
          stats["comment_claims"] == 1,
          f"claims={claims}")

    # 7d. The tier1 doc is prose, not comment-marked, so it needs prose=True.
    #     Shape copied verbatim from the real docs/tier1_env_vars.md line for
    #     MC2_STANDARD_LIT_V1, which was a live lie until this rule landed.
    doc = comment_claims(
        ["- `MC2_ST_HONEST_ON` — GGX PBR on mechs. Default **OFF**. Kill=`=0`.",
         "  narrative line naming MC2_ST_HONEST_ON with no default assertion"],
        "docs/tier1_env_vars.md", prose=True)
    check("a markdown doc line asserting a default is attributed",
          len(doc) == 1 and doc[0]["claim"] == "OFF"
          and doc[0]["var"] == "MC2_ST_HONEST_ON", str(doc))
    check("prose=False still ignores the same line (comment marker required)",
          comment_claims(
              ["- `MC2_ST_HONEST_ON` — Default **OFF**."], "x.cpp") == [])

    # 7e. SHADER-COMMENT-TRUTH-1. The defect was never in comment_claims() -- it
    #     was in WHICH FILES were handed to it. So this case must exercise the
    #     real directory/extension sweep, not the regex: a unit test that fed
    #     comment_claims() a GLSL line directly would have passed on the broken
    #     version and proved nothing. (The live-tree injection that actually
    #     demonstrated the fix is recorded in the SHADER-COMMENT-TRUTH-1 commit;
    #     this case is the permanent regression guard for the wiring.)
    import shutil
    import tempfile   # NOTE: also imported by case 8 below; a function-local
                      # `import` binds the name for the WHOLE function, so this
                      # one must precede first use or it is an UnboundLocalError.
    tmproot = tempfile.mkdtemp()
    try:
        shdir = os.path.join(tmproot, "shaders", "include")
        os.makedirs(shdir)
        with open(os.path.join(shdir, "probe.hglsl"), "w", encoding="utf-8") as fh:
            fh.write("// gated by MC2_ST_SHADER_GATE (default OFF), decoded below\n"
                     "vec3 f() { return vec3(1); }\n")
        # A C++-only sweep must NOT see it (this is the pre-fix behaviour) ...
        pre = load_comment_claims(root=tmproot, scan_dirs=["shaders"],
                                  claim_dirs=[], exclude=())
        check("a .hglsl claim is INVISIBLE to the C++ extension list",
              pre == [], str(pre))
        # ... and the shader sweep must.
        got = load_comment_claims(root=tmproot, scan_dirs=[],
                                  claim_dirs=["shaders"], exclude=())
        check("a .hglsl comment asserting a default IS swept",
              len(got) == 1 and got[0]["var"] == "MC2_ST_SHADER_GATE"
              and got[0]["claim"] == "OFF"
              and got[0]["path"] == "shaders/include/probe.hglsl", str(got))
    finally:
        shutil.rmtree(tmproot, ignore_errors=True)

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
    print(f"SELF-TEST PASSED ({len(ran)} cases)")
    return 0


if __name__ == "__main__":
    argv = sys.argv[1:]
    if "--selftest" in argv or "--self-test" in argv:
        sys.exit(_selftest())
    sys.exit(run(quiet="--quiet" in argv))
