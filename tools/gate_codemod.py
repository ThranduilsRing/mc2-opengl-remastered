#!/usr/bin/env python3
"""gate_codemod -- DRY-RUN planner for the G1b pilot of the typed gate registry.

WHAT IT ANSWERS
---------------
For every raw `getenv("MC2_*")` read in a list of C++ files: "if this site were rewritten to
`mc2::gate::get<"MC2_*">()` against the gate row that is committed today, would anything a user can
observe change, and what exact text keeps it from changing?"  Design: .claude/cleanup/
typed-gate-registry-design.md section 5.7 (migration mechanics) and the review amendments 1, 2 and 9
(per-site rule override, numeric range=Reject, caching shape preserved).

It writes NO C++ unless `--apply --i-mean-it --files F` is given: that rewrites F's NO-CHANGE / EDGE sites in the
working tree (one file per commit) and refuses without `--i-mean-it`.

HOW A SITE IS JUDGED
--------------------
 * Bool rows.  The site is run by the reader interpreter in scripts/gate_idioms.py against eight probe
   inputs (U unset, E "", Z "0", F "false", O "1", X "yes", X10 "10", X25 "2.5").  The result is matched
   against the truthiness table of every Rule in Gates/gate.h.  Same vector as the row's rule: NO-CHANGE.
   Differs only on E/F/X/X10/X25: EDGE.  Differs on U, Z or O: SEMANTICS, and the proposal is the
   per-site override `get<"N", Rule::R>()` that reproduces the old behaviour byte for byte.
 * Int and Float rows.  The old reader (whole lambda or initializer) is executed on a probe set that
   includes junk ("abc"), trailing junk ("12abc"), "2.0", negatives, and the values just inside and just
   outside every bound literal found in the code.  The result is compared with what the row's parser
   (Prefix, i.e. atoi/atof compatible, plus range Reject or Clamp) returns.  The tool searches for the
   range and default that make the two agree and lists them as ROW EDITS; whatever still differs is EDGE
   (SEMANTICS if the default itself differs).
 * The CACHING SHAPE is preserved: a `static const T s = <reader>` stays a static holding `get<>()`.

VERDICTS: NO-CHANGE | EDGE | SEMANTICS | UNHANDLED (the tool could not prove the site, so a human reads it).

USAGE
  py -3 tools/gate_codemod.py                       # TSV + summary of the G1b pilot files, from HEAD
  py -3 tools/gate_codemod.py --files A.cpp B.h     # other files
  py -3 tools/gate_codemod.py --write               # regenerate docs/gate-codemod-g1b.{md,tsv}
  py -3 tools/gate_codemod.py --worktree            # read the working tree instead of git HEAD
"""

import argparse
import json
import math
import os
import re
import struct
import subprocess
import sys
from collections import Counter

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import gate_idioms as gi  # noqa: E402
from console_child import child_kwargs  # noqa: E402

PILOT_FILES = (
    "GameOS/gameos/gos_terrain_patch_stream.cpp",
    "mclib/terrain_admission_mode.h",
    "mclib/terrain_surface_trace.h",
    "mclib/terrain_runtime.cpp",
    "GameOS/gameos/gos_terrain_surface.cpp",
    "GameOS/gameos/gos_terrain_lighting.cpp",
)
G3_FILES = (
    "GameOS/gameos/gos_terrain_lod_chunk.cpp",
    "GameOS/gameos/gos_terrain_indirect.cpp",
    "mclib/terrain.cpp",
    "GameOS/gameos/gos_terrain_water_stream.cpp",
    "GameOS/gameos/vulkan_terrain_island.cpp",
    "GameOS/gameos/gos_terrain_mask_dispatch.cpp",
    "GameOS/gameos/gos_terrain_height_tex.cpp",
)
G6_FILES = (
    "mclib/ablenv.cpp",
    "mclib/ablrtn.cpp",
    "mclib/ablsymt.cpp",
    "mclib/ablxstd.cpp",
    "mclib/ablxstmt.cpp",
    "mclib/anim_override_registry.cpp",
    "mclib/apprtype.cpp",
    "mclib/assimp_importer.cpp",
    "mclib/bdactor.cpp",
    "mclib/camera.cpp",
    "mclib/camera.h",
    "mclib/cliff_decal_tuning.cpp",
    "mclib/cone_glow.cpp",
    "mclib/cpu_proj_cost_split.cpp",
    "mclib/dynamic_decal_ring.cpp",
    "mclib/ffile.cpp",
    "mclib/file.cpp",
    "mclib/fx_def_registry.cpp",
    "mclib/fx_trace/fx_cost_split.cpp",
    "mclib/fx_trace/fx_trace.cpp",
    "mclib/gosfx/effect.cpp",
    "mclib/gosfx/tube.cpp",
    "mclib/gvactor.cpp",
    "mclib/gvactor_internal.h",
    "mclib/gvactor_update.cpp",
    "mclib/mapdata.cpp",
    "mclib/mc2_audit.cpp",
    "mclib/mech3d.cpp",
    "mclib/mech_skel_import.cpp",
    "mclib/mlr/mlr_gate.cpp",
    "mclib/model_override_registry.cpp",
    "mclib/move_recon.cpp",
    "mclib/move_solvectx.h",
    "mclib/msl.cpp",
    "mclib/object_admission_predicate.cpp",
    "mclib/particles/batcher.cpp",
    "mclib/particles/gpu_trail.cpp",
    "mclib/particles/spawn.cpp",
    "mclib/projectz_trace.cpp",
    "mclib/quad.cpp",
    "mclib/render_contract.cpp",
    "mclib/routines.cpp",
    "mclib/spotlight_diag.cpp",
    "mclib/terrtxm.cpp",
    "mclib/terrtxm2.cpp",
    "mclib/tex_resolve_table.cpp",
    "mclib/tgl.cpp",
    "mclib/tgl_load_guard.cpp",
    "mclib/tgl_render.cpp",
    "mclib/tgl_transform.cpp",
    "mclib/txmmgr.cpp",
    "mclib/userinput.cpp",
    "mclib/vis_semantics.cpp",
)

# G4: every file that still reads an MC2_VK_* variable through a raw getenv (the umbrella helpers and the
# envTriState readers are gone by then; what is left are the trace, perf and diagnostic reads).
G4_FILES = (
    "code/missiongui.cpp",
    "code/objmgr.cpp",
    "code/warrior.cpp",
    "code/weaponbolt_render.cpp",
    "GameOS/gameos/engine_present_vk.cpp",
    "GameOS/gameos/gameosmain.cpp",
    "GameOS/gameos/gameos_graphics.cpp",
    "GameOS/gameos/gos_postprocess.cpp",
    "GameOS/gameos/gos_static_prop_batcher.cpp",
    "GameOS/gameos/vk_feed_dump.h",
    "GameOS/gameos/vk_gpu_timestamps.h",
    "GameOS/gameos/vulkan_color_prop_island.cpp",
    "GameOS/gameos/vulkan_context.cpp",
    "GameOS/gameos/vulkan_mech_island.cpp",
    "GameOS/gameos/vulkan_shadow_island_live.cpp",
    "GameOS/gameos/vulkan_water_island.cpp",
)
# G5: the four GameOS giant singles, one file per cycle (the plan covers all four so the helper groups
# that span them are read once).
G5_FILES = (
    "GameOS/gameos/gos_postprocess.cpp",
    "GameOS/gameos/gos_static_prop_batcher.cpp",
    "GameOS/gameos/gameos_graphics.cpp",
    "GameOS/gameos/gameosmain.cpp",
)
# G7: code/, GameAdapters/, RenderWorld/, HostServices/ and RenderCore/ files that still read a gate raw, in migration order.
G7_FILES = (
    "code/mission.cpp",
    "code/warrior.cpp",
    "code/objmgr.cpp",
    "code/missiongui.cpp",
    "code/brain_special_dispatch.cpp",
    "code/logisticsdata.cpp",
    "code/gamecam.cpp",
    "code/simplecamera.cpp",
    "code/controlgui.cpp",
    "code/missionbegin.cpp",
    "code/job_system.cpp",
    "code/mechlopedia.cpp",
    "code/infowindow.cpp",
    "code/tacticaloverview.cpp",
    "code/saveload.cpp",
    "code/mainmenu.cpp",
    "code/terrobj.cpp",
    "code/missionresults.cpp",
    "code/missiongui_uicmd_fixture.cpp",
    "code/mechlistbox.cpp",
    "code/mechicon.cpp",
    "code/mechcmd2.cpp",
    "code/logistics.cpp",
    "code/weaponbolt_update.cpp",
    "code/unitprofile.cpp",
    "code/salvagemecharea.cpp",
    "code/pausewindow.cpp",
    "code/missiongui_uicmd.cpp",
    "code/mechlabscreen.cpp",
    "code/mech_weapons.cpp",
    "code/mech_update.cpp",
    "code/logisticsvariant.cpp",
    "code/loadscreen.cpp",
    "code/equipment_registry.h",
    "code/ablmc2_repro.cpp",
    "RenderWorld/RenderWorld.cpp",
    "HostServices/DefaultHostServices.cpp",
    "GameAdapters/VegetationAdapter.cpp",
    "GameAdapters/MechRenderAdapter.cpp",
    "code/weather.cpp",
    "code/sim_identity.cpp",
    "code/sensor_trace.h",
    "code/pilotreviewarea.cpp",
    "code/optionsarea.cpp",
    "code/object_walk_trace.h",
    "code/mover_combat.cpp",
    "code/mover_attack.cpp",
    "code/mechpurchasescreen.cpp",
    "code/mechcmd2_frame.cpp",
    "code/mechbayscreen.cpp",
    "code/mech_loadout.cpp",
    "code/mc2video.cpp",
    "code/mc2movie.cpp",
    "code/logisticsmechicon.cpp",
    "code/jobs_fatal.cpp",
    "code/hudimguipanel.cpp",
    "code/gvehicl_weapons.cpp",
    "code/gvehicl_update.cpp",
    "code/ground_contact_blob.cpp",
    "code/gameplay_pick.cpp",
    "code/gameobj.cpp",
    "code/collision_trace.h",
    "code/bldng.cpp",
    "code/abl_trace.h",
    "RenderCore/ResolutionContract.h",
    "RenderCore/RendererFeatureRegistry.h",
    "GameAdapters/StaticPropRenderAdapter.cpp",
)
SEED_REL = "tools/gate_table.seed.json"
TABLE_REL = "Gates/gate_table.json"
LEDGER_REL = "tools/gate_g0_ledger.json"
OUT_MD = "docs/gate-codemod-g1b.md"
OUT_TSV = "docs/gate-codemod-g1b.tsv"

# One entry per migration slice the planner prepares.  `g1b` is the pilot the planner was written for.
FAMILIES = {
    "g1b": {"title": "Gate codemod, G1b pilot plan (dry run)", "what": "pilot", "files": PILOT_FILES,
            "md": OUT_MD, "tsv": OUT_TSV, "cmd": "py -3 tools/gate_codemod.py --write"},
    "g3": {"title": "Gate codemod, G3 terrain-family plan (dry run)", "what": "G3 terrain migration", "files": G3_FILES,
           "md": "docs/gate-codemod-g3.md", "tsv": "docs/gate-codemod-g3.tsv",
           "cmd": "py -3 tools/gate_codemod.py --family g3 --write"},
    "g6": {"title": "Gate codemod, G6 mclib-family plan (dry run)", "what": "G6 mclib migration", "files": G6_FILES,
           "md": "docs/gate-codemod-g6.md", "tsv": "docs/gate-codemod-g6.tsv",
           "cmd": "py -3 tools/gate_codemod.py --family g6 --write"},
    "g4": {"title": "Gate codemod, G4 VK raw-read plan (dry run)", "what": "G4 VK raw-read migration", "files": G4_FILES,
           "md": "docs/gate-codemod-g4.md", "tsv": "docs/gate-codemod-g4.tsv",
           "cmd": "py -3 tools/gate_codemod.py --family g4 --write"},
    "g5": {"title": "Gate codemod, G5 GameOS giant singles plan (dry run)", "what": "G5 giant-single migration", "files": G5_FILES,
           "md": "docs/gate-codemod-g5.md", "tsv": "docs/gate-codemod-g5.tsv",
           "cmd": "py -3 tools/gate_codemod.py --family g5 --write"},
    "g7": {"title": "Gate codemod, G7 code/ and adapters plan (dry run)", "what": "G7 code/ migration", "files": G7_FILES,
           "md": "docs/gate-codemod-g7.md", "tsv": "docs/gate-codemod-g7.tsv",
           "cmd": "py -3 tools/gate_codemod.py --family g7 --write"},
}

NS = "mc2::gate"

# Callers of the VK umbrella resolver (GameOS/gameos/vk_shared_gbuffer_gate.h).  A site that reads a gate
# through one of these is NOT a plain env read: the answer depends on the umbrella (subGate,
# directChainGate, terrainNativeFirstOwnerOn) or on the unset/0/1 distinction (envTriState).  They need
# the Tri / state<> semantics of the G4 slice, so the planner lists them separately and never rewrites them.
UMBRELLA_HELPERS = ("subGate", "directChainGate", "terrainNativeFirstOwnerOn", "envTriState")

# Calls that mention a gate name but only WRITE the environment; they are not readers at all.
_NON_READER_CALLEES = {"putenv", "_putenv", "_putenv_s", "setenv", "SetEnvironmentVariable",
                       "SetEnvironmentVariableA", "SetEnvironmentVariableW"}

VERDICTS = ("NO-CHANGE", "EDGE", "SEMANTICS", "UNHANDLED")
SHAPES = ("namespace-static", "local-static", "lambda-static", "per-call")

# Numeric inputs whose result may differ after a rewrite without the site needing a human: empty text and the
# non-finite spellings (a registry row rejects them to its default). Any other input that changes means the old
# reader does something the row does not.
_BENIGN_NUMERIC_EDGES = ("''", "'1e999'", "'nan'", "'NaN'", "'inf'", "'-1e999'", "unset")
PROBE_LABELS = tuple(gi.PROBE_LABELS)
PROBE_VALUES = tuple(v for _, v in gi.PROBES)
#: Probe labels whose result may differ in an EDGE (never U, Z or O: those are SEMANTICS).
EDGE_LABELS = ("E", "F", "X", "X10", "X25", "X01")

# Rules that read the FIRST character: an old reader that tests for an EXACT "0" (v[1] == '\\0', strcmp(v, "0"))
# agrees with them on every gate_idioms probe but not on "01" / "00" / "0x".  The probe vector cannot see that, so
# the site text is searched for the exact-compare idioms and the verdict becomes EDGE (label X01).
LEAD0_RULES = ("Lead0Kill", "Lead0", "Lead0P", "DefOnLead0", "Lead0Def")
EXACT0_RE = re.compile(r"\[\s*1\s*\]\s*[!=]=\s*'(?:\\0|\x00)'|\bstr(?:i)?cmp\s*\([^;]*?,\s*\"0\"\s*\)|[!=]=\s*\"0\"")


def exact_zero_site(text):
    """True when the reader text compares the value against an exact "0" (not just its first character)."""
    return bool(text) and bool(EXACT0_RE.search(text))

# ---------------------------------------------------------------------------
# The Bool rules of Gates/gate.h, ported so the tool can ask "what does rule R say about value V"
# for harness values and for the unit tests (cross-checked against gi.rule_vector in the tests).
# ---------------------------------------------------------------------------

BOOL_RULES = ("Presence", "NonEmpty", "Exact1", "Lead1", "Lead0", "Lead0Kill", "Lead0P", "DefOnLead0",
              "AtoiNZ", "DefOnAtoiNZ", "Is01", "Std", "Lead0Def")
_FALSE_TOK = ("0", "false", "off", "no")
_TRUE_TOK = ("1", "true", "on", "yes")


def apply_rule(rule, v, default):
    """The row-rule truthiness of environment text `v` (None = unset); mirrors applyRule() in gate.h."""
    st = v is not None
    ne = bool(v)
    f0 = ne and v[0] == "0"
    f1 = ne and v[0] == "1"
    x0 = v == "0"
    x1 = v == "1"
    ftok = ne and v.lower() in _FALSE_TOK
    nz = st and gi._cstr_prefix_int(v) != 0
    if rule == "Std":
        return (not ftok) if ne else default
    if rule == "Presence":
        return st
    if rule == "NonEmpty":
        return ne
    if rule == "Exact1":
        return x1
    if rule == "Lead1":
        return f1
    if rule == "Lead0":
        return ne and not f0
    if rule == "Lead0Kill":
        return not (st and f0)
    if rule == "Lead0P":
        return st and not f0
    if rule == "DefOnLead0":
        return (not st) or (ne and not f0)
    if rule == "AtoiNZ":
        return st and nz
    if rule == "DefOnAtoiNZ":
        return (not st) or nz
    if rule == "Lead0Def":
        return (not f0) if ne else default
    if rule == "Is01":
        return x0 or x1
    raise ValueError(rule)


def rule_vec(rule, default):
    """[bool] x 8 for the gate_idioms probe order."""
    return [apply_rule(rule, v, default) for v in PROBE_VALUES]


def vec_str(bits):
    return "".join("1" if b else "0" for b in bits)


def _probe_class(v):
    """Which of the eight probe classes a harness value falls in (for 'does a harness hit this diff')."""
    if v is None:
        return {"U"}
    if v == "":
        return {"E"}
    if v == "0":
        return {"Z"}
    if v.lower() in ("false", "off", "no"):
        return {"F"}
    if v == "1":
        return {"O"}
    out = {"X"}
    if v[0] == "1":
        out.add("X10")
    if re.match(r"^\s*[+-]?\d*\.\d+", v):
        out.add("X25")
    return out


# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------

class Row:
    """One gate row as the migration sees it (committed table first, seed for the extras)."""

    def __init__(self, name, kind, rule, default, rng=None, harness=(), readers=(), owner="", conflict=False,
                 tokens=""):
        self.name = name
        self.tokens = tokens        # Enum rows: "|a|b|c" (empty for every other kind)
        self.kind = kind
        self.rule = rule
        self.default = default
        self.range = rng            # None | {"lo","hi","clamp"}
        self.harness = list(harness)
        self.readers = list(readers)
        self.owner = owner
        self.conflict = conflict


def load_rows(root=ROOT, rev=None):
    """{name: Row}.  Rule, kind, default and range come from the committed compiled table
    (Gates/gate_table.json) because that is what get<>() will do; the seed supplies the rows the table
    lacks plus harnessValues and the reader list."""
    def _json(rel):
        if rev:
            try:
                raw = subprocess.run(["git", "-C", root, "show", "%s:%s" % (rev, rel)], check=True,
                                     **child_kwargs(redirected=True, capture_output=True)).stdout.decode("utf-8", "replace")
                return json.loads(raw)
            except (subprocess.CalledProcessError, ValueError, OSError):
                return {}
        try:
            with open(os.path.join(root, rel), "r", encoding="utf-8") as fh:
                return json.load(fh)
        except (OSError, ValueError):
            return {}

    seed = {g["name"]: g for g in _json(SEED_REL).get("gates", [])}
    table = {g["name"]: g for g in _json(TABLE_REL).get("gates", [])}
    rows = {}
    for name in sorted(set(seed) | set(table)):
        s = seed.get(name, {})
        t = table.get(name)
        src = t if t is not None else s
        default = src.get("default")
        rows[name] = Row(name, src.get("kind", "Bool"), src.get("rule", "Std"), default,
                         rng=(t or {}).get("range"), harness=s.get("harnessValues", []),
                         readers=s.get("readers", []), owner=src.get("owner", ""),
                         conflict=bool(s.get("conflict")), tokens=src.get("tokens", "") or "")
    return rows


def load_ledger(root=ROOT):
    try:
        with open(os.path.join(root, LEDGER_REL), "r", encoding="utf-8") as fh:
            return json.load(fh).get("gates", {})
    except (OSError, ValueError):
        return {}


def git_head(root=ROOT):
    try:
        return subprocess.run(["git", "-C", root, "rev-parse", "--short=9", "HEAD"], check=True,
                              **child_kwargs(redirected=True, capture_output=True)).stdout.decode().strip()
    except (subprocess.CalledProcessError, OSError):
        return "unknown"


def read_source(root, rel, rev):
    """File text with CRLF folded to LF; `rev` None reads the working tree."""
    if rev:
        raw = subprocess.run(["git", "-C", root, "show", "%s:%s" % (rev, rel)], check=True,
                             **child_kwargs(redirected=True, capture_output=True)).stdout
    else:
        with open(os.path.join(root, rel), "rb") as fh:
            raw = fh.read()
    return raw.decode("utf-8", "replace").replace("\r\n", "\n")


# ---------------------------------------------------------------------------
# Syntax helpers (all offsets are into comment-stripped text, which keeps byte offsets of the raw text)
# ---------------------------------------------------------------------------

_QUALS = {"static", "constexpr", "const", "inline", "thread_local", "volatile"}
_LAMBDA_HEAD_RE = re.compile(r"\[([^\[\]]*)\]\s*(?:\(([^()]*)\))?\s*(?:mutable\s+)?(?:noexcept\s+)?"
                             r"(?:->\s*[\w:<>\s\*&,]+?)?\s*$")
_CTRL_RE = re.compile(r"\s*(?:else\s+)?(?:if|while|for|switch|return|case|do)\b")
_PARSERS = ("atoi", "atol", "atoll", "_atoi64", "atof", "strtol", "strtoll", "strtoul", "strtoull", "strtod",
            "strtof", "stoi", "stol", "stoll", "stof", "stod")
_PARSER_RE = re.compile(r"\b(?:std::)?(%s)\b" % "|".join(_PARSERS))
_SAFE_WORDS = set("""
const char bool int long short float double unsigned signed auto void static constexpr inline return if else
nullptr NULL true false std static_cast reinterpret_cast const_cast uint8_t uint16_t uint32_t uint64_t int8_t
int16_t int32_t int64_t size_t getenv _getenv atoi atol atoll _atoi64 atof strtol strtoll strtoul strtoull
strtod strtof stoi stol stoll stof stod strcmp _stricmp stricmp strcasecmp strncmp _strnicmp strlen max min
fmax fmin fmaxf fminf clamp abs fabs fabsf labs tolower toupper isdigit isalpha string_view
""".split())


def _match_paren(T, open_at):
    """Index just past the `)` that closes the `(` at open_at, or -1."""
    depth, j, n = 0, open_at, len(T)
    while j < n:
        c = T[j]
        if c in "\"'":
            q = c
            j += 1
            while j < n and T[j] != q:
                j += 2 if T[j] == "\\" else 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return j + 1
        j += 1
    return -1


def find_call(T, p):
    """-> (callee, call_start, call_end) of the call whose first argument literal starts at p, or None."""
    ws = max(0, p - 96)
    cm = gi._CALLEE_RE.search(T[ws:p])
    if not cm:
        return None
    cs = ws + cm.start(1)
    if T[max(0, cs - 2):cs] == "::":
        cs -= 2
    open_at = T.index("(", ws + cm.end(1))
    ce = _match_paren(T, open_at)
    if ce < 0:
        return None
    return cm.group(1), cs, ce


def find_lambda(T, p):
    """The immediately-invoked lambda whose body holds p.
    -> None | ("iife", lam_start, body_open, body_close, call_end) | ("other", reason)."""
    blk = gi.enclosing_block(T, p)
    while blk:
        before = T[max(0, blk[0] - 240):blk[0]]
        lm = _LAMBDA_HEAD_RE.search(before)
        if lm:
            lam_start = max(0, blk[0] - 240) + lm.start()
            if lm.group(1).strip():
                return ("other", "lambda with captures [%s]" % lm.group(1).strip())
            am = re.match(r"\s*\(\s*\)", T[blk[1]:blk[1] + 40])
            if not am:
                return ("other", "lambda is not invoked in place")
            return ("iife", lam_start, blk[0], blk[1], blk[1] + am.end())
        if gi._block_is_scope(T, blk[0]):
            return None
        blk = gi.enclosing_block(T, blk[0])
    return None


def _split_assign(st, before):
    """Offset of the first depth-0 plain `=` in st that lies before `before`, else -1."""
    depth = 0
    for i, c in enumerate(st[:before]):
        if c in "([{<" and c != "<":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "=" and depth == 0:
            prev = st[i - 1] if i else ""
            nxt = st[i + 1] if i + 1 < len(st) else ""
            if nxt == "=" or prev in "=!<>+-*/%&|^":
                continue
            return i
    return -1


def parse_lhs(lhs):
    """'static const bool s_x' -> dict(quals, type, name, is_decl); '  s_x' -> assignment."""
    toks = lhs.replace("*", " * ").replace("&", " & ").split()
    toks = [t for t in toks if t]
    if not toks:
        return None
    if len(toks) == 1 or (len(toks) >= 1 and not re.match(r"^[A-Za-z_]", toks[-1])):
        return {"quals": [], "type": "", "name": lhs.strip(), "is_decl": False}
    # an assignment to a member/element looks like one token; a declaration has a type word first
    name = toks[-1]
    head = toks[:-1]
    quals = [t for t in head if t in _QUALS]
    typ = " ".join(t for t in head if t not in _QUALS).replace(" *", "*").replace(" &", "&")
    if not typ:
        return {"quals": quals, "type": "", "name": name, "is_decl": False}
    return {"quals": quals, "type": typ, "name": name, "is_decl": True}


def at_namespace_scope(T, pos):
    blk = gi.enclosing_block(T, pos)
    return blk is None or gi._block_is_scope(T, blk[0])


def shape_of(is_lambda, is_decl, is_static, ns_scope):
    if is_lambda:
        return "lambda-static" if (is_static or (is_decl and ns_scope)) else "per-call"
    if is_decl and is_static:
        return "namespace-static" if ns_scope else "local-static"
    if is_decl and ns_scope:
        return "namespace-static"
    return "per-call"


def body_impure(body, extra_ok=()):
    """First identifier in `body` that is not a keyword, a libc parse helper or a name the body declares."""
    s = re.sub(r'"(?:[^"\\]|\\.)*"', '""', body)
    s = re.sub(r"'(?:[^'\\]|\\.)*'", "' '", s)
    declared = set(re.findall(r"\b(?:char|bool|int|long|float|double|unsigned|auto|u?int(?:8|16|32|64)_t|size_t)"
                              r"\b[\s\*&]*(?:const\s+)?([A-Za-z_]\w*)\s*[=;(,)]", s))
    declared |= set(re.findall(r"\bconst\s+(?:char|bool|int|long|float|double|auto)\b[\s\*&]*([A-Za-z_]\w*)", s))
    ok = _SAFE_WORDS | declared | set(extra_ok)
    for w in re.findall(r"(?<![\w.])([A-Za-z_]\w*)", s):
        # a literal suffix or hex digits are not identifiers; field access `.x` is filtered by the lookbehind
        if w in ok or re.fullmatch(r"[fFuUlL]+", w):
            continue
        return w
    return None


def _norm_ws(s, limit=320):
    s = re.sub(r"\s+", " ", s).strip()
    return s if len(s) <= limit else s[:limit - 3] + "..."


# ---------------------------------------------------------------------------
# Result record
# ---------------------------------------------------------------------------

class Result:
    __slots__ = ("file", "line", "gate", "callee", "idiom", "shape", "replacement", "verdict", "why", "old",
                 "row_edit", "diff", "harness_hits", "override", "reason", "mode", "kind", "helper_def",
                 "paired", "pp", "helper_read", "edit", "hand")

    def __init__(self, file, line, gate, callee):
        self.file, self.line, self.gate, self.callee = file, line, gate, callee
        self.idiom = ""
        self.shape = "per-call"
        self.replacement = "-"
        self.verdict = "UNHANDLED"
        self.why = ""
        self.old = ""
        self.row_edit = ""
        self.diff = []
        self.harness_hits = []
        self.override = ""
        self.reason = ""
        self.mode = ""
        self.kind = ""
        self.helper_def = ""        # helper-call readers: where the helper is defined and what it does
        self.helper_read = ""       # what the interpreter makes of a helper-call reader (evidence only, no rewrite)
        self.pp = ""                # preprocessor context holding the site ("inside #else of ...")
        self.paired = False         # the statement reads the same gate twice (presence test, then a character test)
        self.edit = None            # (start, end, new_text) in raw-file offsets: what --apply replaces
        self.hand = ""              # why --apply must leave the site to a human (a planned site that is not applied)

    @property
    def where(self):
        return "%s:%d" % (self.file, self.line)

    def tsv(self):
        cells = [self.where, self.gate, self.idiom, self.shape, self.replacement, self.verdict, self.why]
        return "\t".join(c.replace("\t", " ").replace("\n", " ") for c in cells)


TSV_HEADER = "\t".join(["file:line", "gate", "old idiom class", "old caching shape",
                        "proposed replacement", "verdict", "why"])


# ---------------------------------------------------------------------------
# Expression builders
# ---------------------------------------------------------------------------

def get_expr(name, rule=None, neg=False):
    e = "%s::get<\"%s\"%s>()" % (NS, name, (", %s::Rule::%s" % (NS, rule)) if rule else "")
    return ("!" + e) if neg else e


def _fmt_num(x):
    if isinstance(x, float) and x == int(x) and abs(x) < 1e15:
        return str(int(x))
    if not isinstance(x, float):
        return str(x)
    short = "%g" % x
    # A float32 constant such as 1.0f/384.0f needs more than six digits to stay distinguishable from the
    # row's rounded default; print the shortest text that still names the same float32.
    return short if _f32(float(short)) == _f32(x) else "%.9g" % x


# ---------------------------------------------------------------------------
# Bool resolution
# ---------------------------------------------------------------------------

_CAND_ORDER = ("Presence", "NonEmpty", "Exact1", "Lead1", "Lead0", "Lead0Kill", "Lead0P", "DefOnLead0",
               "AtoiNZ", "DefOnAtoiNZ", "Is01", "Std", "Lead0Def")


def _flip(x):
    return {"on": "off", "off": "on"}.get(x, x)


def _vec_match(site, ref):
    """site: list of on/off/D/?/inh per probe; ref: [bool].  True when every decided probe agrees."""
    for s, r in zip(site, ref):
        if s in ("on", "off") and (s == "on") != r:
            return False
    return True


def match_rule(site_vec, row_rule, row_default):
    """-> (rule, neg, default_used) of the first rule (preferring the row's own) whose vector equals the
    site's, with either polarity; None when no rule reproduces it."""
    dflts = (bool(row_default), not bool(row_default))
    cands = []
    order = ([row_rule] if row_rule in BOOL_RULES else []) + [r for r in _CAND_ORDER if r != row_rule]
    for neg in (False, True):
        for r in order:
            for d in (dflts if r in ("Std", "Lead0Def") else (dflts[0],)):
                cands.append((r, neg, d))
    # the row's own rule, either polarity, comes first; then the other rules
    cands.sort(key=lambda c: (c[0] != row_rule, c[2] != bool(row_default), c[1]))
    for r, neg, d in cands:
        ref = rule_vec(r, d)
        if neg:
            ref = [not b for b in ref]
        if _vec_match(site_vec, ref):
            return r, neg, d
    return None


def judge_bool(row, site_vec, why_prefix="", site_text=None):
    """Bool/Presence verdict for a site whose value (as written, not orientated) is `site_vec`.
    -> dict(verdict, rule_used, neg, override(rule|None), diff[labels], alt, why, reason)."""
    if any(x == "inh" for x in site_vec):
        return {"reason": "Tri/umbrella-inheriting reader (use state<> in G4)"}
    m = match_rule(site_vec, row.rule, row.default)
    if m is None:
        return {"reason": "truthiness vector [%s] matches no Rule" % ",".join(site_vec)}
    r, neg, d = m
    row_vec = rule_vec(row.rule, bool(row.default)) if row.rule in BOOL_RULES else None
    if row_vec is None:
        return {"reason": "row rule %s is not a Bool rule" % row.rule}
    site_rule_vec = rule_vec(r, d)
    diff = [PROBE_LABELS[i] for i in range(8) if site_rule_vec[i] != row_vec[i]]
    if not diff and row.rule in LEAD0_RULES and r in LEAD0_RULES and exact_zero_site(site_text):
        diff = ["X01"]
    out = {"rule": r, "neg": neg, "diff": diff, "default_used": d}
    if r in ("Std", "Lead0Def") and d != bool(row.default):
        out["reason"] = ("old reader is %s with default %s but the row's default is %s: no per-site override "
                         "can change a default (fix the row or keep the reader)" %
                         (r, "on" if d else "off", "on" if row.default else "off"))
        out["rule_for_text"] = r
        return out
    if not diff:
        out["verdict"] = "NO-CHANGE"
        out["override"] = None
    elif all(l in EDGE_LABELS for l in diff):
        out["verdict"] = "EDGE"
        out["override"] = None
        out["alt"] = r
    else:
        out["verdict"] = "SEMANTICS"
        out["override"] = r
    return out


def harness_impact(row, site_rule, site_default, diff):
    """Harness values (seed harnessValues) the row's rule would answer differently from the old reader."""
    if not diff or site_rule is None or row.rule not in BOOL_RULES:
        return []
    hits = []
    for h in row.harness:
        if h in ("String",):
            continue
        if apply_rule(site_rule, h, site_default) != apply_rule(row.rule, h, bool(row.default)):
            hits.append(h)
    return hits


# ---------------------------------------------------------------------------
# Numeric resolution
# ---------------------------------------------------------------------------

_LIT = r"[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?[fFuUlL]*"
_CMP_RE = re.compile(r"(?:[<>]=?\s*(%s))|(?:(%s)\s*[<>]=?)" % (_LIT, _LIT))
_CALLARG_RE = re.compile(r"\b(?:clamp|min|max|fmin|fmax|fminf|fmaxf)\s*\(([^()]*)\)")


def _lit_value(s):
    return float(re.sub(r"[fFuUlL]+$", "", s))


def bound_literals(text):
    """Numeric literals compared against or passed to clamp/min/max in `text`, in first-seen order."""
    seen = []
    for m in _CMP_RE.finditer(text):
        v = _lit_value(m.group(1) or m.group(2))
        if v not in seen:
            seen.append(v)
    for m in _CALLARG_RE.finditer(text):
        for lm in re.finditer(_LIT, m.group(1)):
            v = _lit_value(lm.group(0))
            if v not in seen:
                seen.append(v)
    return seen[:6]


def _prefix_num(s, is_int):
    if is_int:
        m = re.match(r"\s*([+-]?\d+)", s)
        v = int(m.group(1)) if m else 0
        return max(-2 ** 31, min(2 ** 31 - 1, v))
    m = re.match(r"\s*([+-]?(?:inf(?:inity)?|nan))", s, re.I)
    if m:
        return float("nan") if "nan" in m.group(1).lower() else (float("-inf") if m.group(1)[0] == "-" else float("inf"))
    return gi._cstr_prefix_float(s)


def _f32(x):
    try:
        return struct.unpack("f", struct.pack("f", x))[0]
    except (OverflowError, struct.error):
        return float("inf") if x > 0 else float("-inf")


def row_eval(kind, default, rng, probe):
    """What get<> returns for an Int/Float row (Prefix parse, optional range) on environment text `probe`."""
    is_int = kind == "Int"
    if probe is None or probe == "":
        return default
    v = _prefix_num(probe, is_int)
    if not is_int and not math.isfinite(v):
        return default
    if rng is not None:
        lo, hi, clamp = rng
        if (lo is not None and v < lo) or (hi is not None and v > hi):
            if clamp:
                v = min(max(v, lo if lo is not None else v), hi if hi is not None else v)
            else:
                return default
    return v


def _same(kind, a, b):
    if isinstance(a, bool) or isinstance(b, bool) or a is None or b is None:
        return False
    if kind == "Float":
        a, b = _f32(float(a)), _f32(float(b))
        if math.isnan(a) and math.isnan(b):
            return True
    return a == b


def _probe_set(kind, lits, harness):
    base = [None, "", "abc", "12abc", "2.0", "0", "-1", "1", "2", "1.5", "300", "5000", "1e9", " 7", "+5"]
    if kind == "Float":
        base += ["0.3", "0.5", "1e-9", "1e999"]
    extra = []
    for L in lits:
        if kind == "Int" or L == int(L):
            for d in (-1, 0, 1):
                extra.append(str(int(L) + d))
        if kind == "Float":
            for d in (-0.01, 0.0, 0.01):
                extra.append("%g" % (L + d))
    seen, out = set(), []
    for x in base + extra + [h for h in harness if isinstance(h, str) and h != "String"]:
        if x not in seen:
            seen.add(x)
            out.append(x)
    return out


def eval_unit_values(interp, path, T, pos, gate, probes, var_name=None):
    """Run the reader statements around `pos` on each probe string; -> [value] or None if unsupported.
    The WHOLE enclosing block runs (not only the tainted chain), so a default assigned after the read and
    a clamp after the parse both take effect."""
    gi._WIN_CACHE.clear()
    w = gi.build_window(path, T, pos)
    interp.gate, interp.site_off, interp.path = gate, pos, path
    stmts = []
    for a, b in w.spans:
        n = interp.parse_stmt(w.tokens, a, b)
        if n is not None:
            stmts.append(n)
    if not stmts or any(s[0] == "skip" and s[1] <= pos <= s[2] for s in stmts):
        return None
    out = []
    for val in probes:
        ctx = gi.Ctx(interp, val, top=True)
        try:
            r = interp.exec_list(stmts, ctx)
        except (gi._Unsupported, RecursionError):
            return None
        if isinstance(r, gi._Ret):
            out.append(r.val)
        elif var_name and var_name in ctx.env:
            out.append(ctx.env[var_name][0])
        else:
            out.append(None)
    return out


def judge_numeric(row, interp, path, T, pos, unit_text, var_name):
    """-> dict(verdict, diff[probe strings], edit(str), cand, why, reason)."""
    kind = row.kind
    lits = bound_literals(unit_text)
    probes = _probe_set(kind, lits, row.harness)
    olds = eval_unit_values(interp, path, T, pos, row.name, probes, var_name)
    if olds is None:
        return {"reason": "interpreter cannot execute the reader on numeric probes"}
    if any(not isinstance(o, (int, float)) or isinstance(o, bool) for o in olds):
        bad = [probes[i] for i, o in enumerate(olds) if not isinstance(o, (int, float)) or isinstance(o, bool)]
        return {"reason": "reader result is not a number for input %r" % (bad[0],), "boolish": all(
            isinstance(o, bool) or o is None for o in olds)}
    old_u = olds[probes.index(None)]
    dflt_row = row.default if isinstance(row.default, (int, float)) and not isinstance(row.default, bool) else 0
    row_rng = None
    if row.range:
        row_rng = (row.range.get("lo"), row.range.get("hi"), bool(row.range.get("clamp")))

    def score(cand_default, cand_rng):
        diffs = [probes[i] for i in range(len(probes))
                 if not _same(kind, olds[i], row_eval(kind, cand_default, cand_rng, probes[i]))]
        return diffs

    # candidates: (default, range, label); the as-is row first so it wins ties
    cands = [(dflt_row, row_rng, "as-is")]
    if not _same(kind, old_u, dflt_row):
        cands.append((old_u, row_rng, "default"))
    if row_rng is None or True:
        los = [None] + lits + ([L + 1 for L in lits] if kind == "Int" else [])
        his = [None] + lits + ([L - 1 for L in lits] if kind == "Int" else [])
        for style in (False, True):
            for lo in los:
                for hi in his:
                    if lo is None and hi is None:
                        continue
                    if lo is not None and hi is not None and lo > hi:
                        continue
                    cands.append((old_u, (lo, hi, style), "range"))
    best = None
    for i, (d, rg, lab) in enumerate(cands):
        diffs = score(d, rg)
        # fewer diffs first; then the as-is row; then fewer edits (default kept, no range, reject before clamp)
        penal = (len(diffs), 0 if lab == "as-is" else 1, 0 if rg is None else 1, 1 if (rg and rg[2]) else 0,
                 0 if _same(kind, d, dflt_row) else 1, i)
        if best is None or penal < best[0]:
            best = (penal, d, rg, lab, diffs)
    _, d, rg, lab, diffs = best

    edits = []
    if not _same(kind, d, dflt_row):
        edits.append("default %s -> %s" % (_fmt_num(dflt_row), _fmt_num(d)))
    if rg != row_rng:
        if rg is None:
            edits.append("drop the range")
        else:
            lo, hi, cl = rg
            nm = "clampRange" if cl else "range"
            if lo is not None and hi is not None:
                edits.append("%s(%s, %s)" % (nm, _fmt_num(lo), _fmt_num(hi)))
            elif lo is not None:
                edits.append("%s(%s, FLT_MAX)" % (nm, _fmt_num(lo)))
            else:
                edits.append("%s(-FLT_MAX, %s)" % (nm, _fmt_num(hi)))
    res = {"diff": diffs, "edit": "; ".join(edits), "probes": probes, "olds": olds, "cand_rng": rg,
           "cand_default": d, "as_is_ok": lab == "as-is" and not diffs}
    if not diffs:
        res["verdict"] = "NO-CHANGE"
    elif any(x is None for x in diffs):
        res["verdict"] = "SEMANTICS"      # unset differs: the default itself
    else:
        res["verdict"] = "EDGE"
    return res


# ---------------------------------------------------------------------------
# The per-file analysis
# ---------------------------------------------------------------------------

def _cast_for(kind, typ):
    """Cast wrapper text for a numeric get<> assigned to C++ type `typ` ('' = none)."""
    t = (typ or "").replace("std::", "").strip()
    if not t or t == "auto":
        return None
    if kind == "Int" and t in ("int", "int32_t"):
        return None
    if kind == "Float" and t == "float":
        return None
    return t


def analyze_text(path, raw, rows, ledger=None, extra_sources=()):
    """All raw env reads in `raw` (the text of `path`) -> [Result], in source order."""
    ledger = ledger or {}
    T = gi.strip_comments(raw)
    fns = gi.FnTable([(path, T)] + [(p, gi.strip_comments(t)) for p, t in extra_sources])
    interp = gi.Interp(fns, "")
    gi._WIN_CACHE.clear()
    gi._CLEAN_CACHE.clear()
    results = []
    for m in gi._LITERAL_RE.finditer(T):
        gate = m.group(1)
        if gi.typed_gate_site(path, T, m, gate) is not None:
            continue                                # already migrated
        call = find_call(T, m.start())
        if call is None:
            continue
        callee, cs, ce = call
        base = callee.split("::")[-1]
        line = T.count("\n", 0, m.start()) + 1
        is_raw = base in ("getenv", "_getenv")
        if not is_raw and fns.resolve(callee, path) is None and base not in gi._RAW_ENV_CALLEES:
            # A helper whose body is outside the scanned files (the umbrella resolver lives in a header).
            # The site is still reported, so no reader is silently missing from the plan.
            if base in _NON_READER_CALLEES or gi._LOG_CALLEE_RE.match(base):
                continue                            # a _putenv or a log line: never a read
        res = Result(path, line, gate, base)
        if not is_raw:
            res.helper_def = _helper_definition(fns, callee, path)
        try:
            _analyze_site(res, interp, path, raw, T, m.start(), cs, ce, is_raw, rows, ledger)
        except Exception as exc:                    # a site must never abort the whole report
            res.verdict, res.reason = "UNHANDLED", "tool error: %s: %s" % (type(exc).__name__, exc)
            res.why = res.reason
        ctx = pp_context(T, m.start())
        if ctx:
            res.pp = ctx
            res.why = "%s; %s" % (res.why, ctx) if res.why else ctx
        results.append(res)
    return results


def _helper_definition(fns, callee, path):
    """Where a helper-call reader is defined and its body, or a note that it lives outside the scanned files."""
    d = fns.resolve(callee, path)
    if d is None:
        return "defined outside the scanned files"
    if d is gi._AMBIG:
        return "several differing definitions"
    return "%s:%d `%s`" % (d.path, d.text.count(chr(10), 0, d.body_lo) + 1,
                           _norm_ws(d.text[d.body_lo:d.body_hi], 420))


# A byte-order mark at the top of a file counts as blank: without it the first statement of a BOM-led file would
# start at offset 0 and swallow the whole preamble of includes and comments.
_PP_LINE_RE = re.compile(r"[ \t\r\n﻿]*#(?:[^\\\n]|\\[\s\S])*")


def _skip_pp(T, ss):
    """`ss` moved past any preprocessor lines (and blanks) that sit between the previous statement and this one."""
    while True:
        m = _PP_LINE_RE.match(T, ss)
        if not m or m.end() == ss:
            return ss
        ss = m.end()


def _stmt_begin(T, pos):
    """Start of the statement holding `pos`; a `#else` line above it is not part of the statement."""
    return _skip_pp(T, gi._stmt_start(T, pos))


def pp_context(T, pos):
    """Text naming the preprocessor conditions (or #define body) that hold `pos`, or ''.

    A site inside `#else` of `MC2_VULKAN_ISLAND` is compiled only in builds without the island, which the
    builder has to know before deciding how to test the rewrite."""
    stack = []
    lines = T.split("\n")
    target = T.count("\n", 0, pos)
    for i in range(target + 1):
        t = lines[i].strip()
        if not t.startswith("#"):
            continue
        d = re.match(r"#\s*(ifdef|ifndef|if|elif|else|endif)\b\s*(.*)", t)
        if not d:
            continue
        kind, cond = d.group(1), d.group(2).strip()
        if kind in ("if", "ifdef", "ifndef"):
            stack.append(["#%s %s" % (kind, cond), False])
        elif kind == "elif" and stack:
            stack[-1] = ["#elif %s" % cond, True]
        elif kind == "else" and stack:
            stack[-1] = ["#else of %s" % stack[-1][0], True]
        elif kind == "endif" and stack:
            stack.pop()
    out = [e[0] for e in stack]
    j = target
    while j > 0 and lines[j - 1].rstrip().endswith("\\"):
        j -= 1
    if re.match(r"#\s*define\b", lines[j].strip()):
        out.append("a #define body")
    return "inside " + "; ".join(out) if out else ""


def _helper_read(interp, path, T, p, gate, row):
    """What the reader interpreter makes of a helper-call site: the idiom and, for a Bool row, the verdict it would get.

    Evidence for the builder who writes the helper-aware rewrite; it proposes no replacement text."""
    try:
        r = gi.analyze_site(interp, path, T, p, gate)
    except Exception as exc:                        # evidence is optional, never a reason to fail the report
        return "interpreter error: %s" % type(exc).__name__
    if r["status"] != "reader":
        return "interpreter: %s" % r["status"]
    d = gi.describe(r)
    if not d:
        return ""
    fam, vec, cls, inv = d
    if fam == "bool" and any(x not in ("on", "off") for x in vec):
        return "bool %s; U,E,Z,F,O,X,X10,X25 = %s; inconclusive, the call sits in a larger expression" % (
            cls, ",".join(str(x) for x in vec))
    if fam == "bool" and row.kind == "Bool":
        jd = judge_bool(row, [_flip(x) for x in vec] if inv else list(vec))
        if "verdict" in jd:
            return "%s%s; vs row %s: %s%s" % ("!" if inv else "", cls, row.rule, jd["verdict"],
                                              (" (differs on %s)" % ",".join(jd["diff"])) if jd["diff"] else "")
    return "%s %s; U,E,Z,F,O,X,X10,X25 = %s" % (fam, cls, ",".join(str(x) for x in vec))


def _paired_reads(T, cs, gate):
    """How many times the statement holding the call at cs spells this gate (a `getenv(X) && getenv(X)[0]` is 2)."""
    ss = gi._stmt_start(T, cs)
    se = gi._stmt_end(T, ss)
    return T[ss:se].count('"%s"' % gate)


def _fail(res, reason):
    res.verdict = "UNHANDLED"
    res.reason = reason
    res.replacement = "-"
    res.why = reason
    return res


def _line_bounds(T, pos):
    a = T.rfind("\n", 0, pos) + 1
    b = T.find("\n", pos)
    return a, (len(T) if b < 0 else b)


def _analyze_site(res, interp, path, raw, T, p, cs, ce, is_raw, rows, ledger):
    gate = res.gate
    row = rows.get(gate)
    res.old = ""
    if row is None:
        return _fail(res, "gate is not in the gate table (Gates/gate_table.cpp)")
    res.kind = row.kind
    if not is_raw:
        if res.callee in UMBRELLA_HELPERS:
            return _fail(res, "umbrella helper %s(): needs Tri/state<> semantics (G4), not migrated in G3" % res.callee)
        res.helper_read = _helper_read(interp, path, T, p, gate, row)
        return _fail(res, "helper-call reader %s(): helper mapping lands in G4/G6" % res.callee)

    # ----- context ---------------------------------------------------------------------------------
    lam = find_lambda(T, p)
    unit = None                       # dict(span=(a,b), lhs, init_kind, ...)
    mode = None
    if lam and lam[0] == "other":
        return _fail(res, lam[1])
    if lam:
        _, lam_start, bo, bc, call_end = lam
        ss = _stmt_begin(T, lam_start)
        head = T[ss:lam_start]
        eq = _split_assign(head + "=", len(head) + 1)
        hm = re.match(r"^(.*?)=\s*$", head, re.S)
        sm = re.match(r"\s*;", T[call_end:call_end + 8]) or re.match(r"\s*(?=[,)])", T[call_end:call_end + 8])
        if not hm or not re.match(r"\s*;", T[call_end:call_end + 8]):
            if re.match(r"^\s*return\s*$", head) and re.match(r"\s*;", T[call_end:call_end + 8]):
                return _fail(res, "lambda returned directly (no variable to keep as a static)")
            return _fail(res, "lambda initializer is not a plain `T x = [..]{..}();` statement")
        lhs = parse_lhs(hm.group(1))
        if lhs is None:
            return _fail(res, "unrecognised lambda target")
        end = call_end + re.match(r"\s*;", T[call_end:call_end + 8]).end()
        body = T[bo + 1:bc - 1]
        bad = body_impure(body)
        if bad:
            return _fail(res, "lambda body names `%s`, which is state outside the read" % bad)
        gates_read = sorted(set(re.findall(r'"(MC2_[A-Z0-9_]+)"', body)))
        if len(gates_read) > 1:
            return _fail(res, "HAND: the lambda reads %d gates (%s); replacing it by one read would drop the others"
                         % (len(gates_read), ", ".join(gates_read)))
        is_static = "static" in lhs["quals"]
        ns_scope = at_namespace_scope(T, ss)
        res.shape = shape_of(True, lhs["is_decl"], is_static, ns_scope)
        unit = {"span": (ss, end), "lhs": lhs, "kind": "lambda", "text": T[lam_start:call_end], "var": lhs["name"]}
        mode = "lambda"
    else:
        ss = _stmt_begin(T, cs)
        se = gi._stmt_end(T, ss)
        st = T[ss:se]
        if _CTRL_RE.match(st) and not st.lstrip().startswith("return"):
            ctrl = True
        else:
            ctrl = False
        eq = -1 if ctrl else _split_assign(st, cs - ss)
        ns_scope = at_namespace_scope(T, ss)
        if eq >= 0 and not ctrl:
            lhs = parse_lhs(st[:eq])
            init = st[eq + 1:]
            init_text = init.rstrip().rstrip(";")
            is_static = lhs is not None and "static" in lhs["quals"]
            res.shape = shape_of(False, bool(lhs and lhs["is_decl"]), is_static, ns_scope)
            ptr_decl = (lhs and lhs["is_decl"] and re.match(r"^(?:const\s+)?(?:char|auto)\s*\*?\s*(?:const)?$",
                                                              lhs["type"].replace("const", "").strip() or "x")
                        and re.fullmatch(r"\s*(?:\(\s*const\s+char\s*\*\s*\)\s*)?%s\s*" % re.escape(T[cs:ce]),
                                         init_text))
            if ptr_decl:
                unit = {"span": (ss, se), "lhs": lhs, "kind": "local", "ptr": lhs["name"].lstrip("*&")}
                mode = "local"
            elif row.kind in ("Int", "Float"):
                if lhs is None:
                    return _fail(res, "unrecognised assignment target")
                bad = body_impure(init_text)
                if bad:
                    return _fail(res, "initializer names `%s`, which is state outside the read" % bad)
                unit = {"span": (ss, se), "lhs": lhs, "kind": "decl", "text": init_text, "var": lhs["name"]}
                mode = "decl-num"
        if mode is None:
            res.shape = shape_of(False, bool(eq >= 0 and parse_lhs(st[:eq]) and parse_lhs(st[:eq])["is_decl"]),
                                 eq >= 0 and "static" in (parse_lhs(st[:eq]) or {"quals": []})["quals"], ns_scope)
            if ctrl or eq < 0:
                res.shape = "per-call"
            mode = "expr"

    # ----- classify the read (not for expr mode, which is syntactic) --------------------------------
    ledger_row = ledger.get(gate)
    notes = []
    if ledger_row:
        st_ = ledger_row.get("status", "")
        notes.append("G0 ledger %s: %s" % (st_, ledger_row.get("rule", "")))
        if st_ in ("divergent", "enum"):
            # A level-valued or divergent gate is a Bool row with no tokens: migrating it would drop the levels.
            return _fail(res, "G0 ledger %s: %s -- level-valued or divergent, not migratable as a Bool until the row "
                              "models the levels" % (st_, ledger_row.get("rule", "")))
    if row.conflict:
        notes.append("readers disagree (seed conflict flag)")

    if row.kind in ("Int", "Float"):
        return _numeric(res, row, interp, path, T, p, cs, ce, unit, mode, notes, raw)
    if row.kind in ("Tri", "Enum", "String"):
        if mode == "expr":
            sub = _expr_parts(T, cs, ce)
            if sub and sub["neg"] is not None and not sub["prefix_pointer"]:
                # a bare presence test of a non-Bool row: isSet<>
                return _expr_emit(res, row, T, raw, p, cs, ce, sub, notes, presence_only=True)
        return _fail(res, "row kind %s: read through %s (G3 Enum / G4 Tri / String via str<>)" %
                     (row.kind, {"Tri": "state<>/Tri", "Enum": "is<>/Enum token", "String": "str<>"}[row.kind]))
    # Bool row
    if mode == "expr" and _paired_reads(T, cs, gate) > 1:
        # `getenv(X) && getenv(X)[0] == '1'` is ONE reader.  Judging the first call as a bare presence test
        # would invent a Presence override; judge the statement as the unit and leave the rewrite to a human.
        res.paired = True
        r = gi.analyze_site(interp, path, T, p, gate)
        d = gi.describe(r) if r["status"] == "reader" else None
        if d and d[0] == "bool":
            fam, vec, cls, inv = d
            res.idiom = cls
            jd = judge_bool(row, [_flip(x) for x in vec] if inv else list(vec),
                            site_text=T[gi._stmt_start(T, cs):gi._stmt_start(T, cs) + 400])
            if "verdict" in jd:
                return _fail(res, "paired reads: the statement reads this gate %d times; as one reader it is %s%s vs row "
                                  "%s: %s%s -- replace the whole expression by hand" %
                             (_paired_reads(T, cs, gate), "!" if inv else "", cls, row.rule, jd["verdict"],
                              (" (differs on %s; override %s)" % (",".join(jd["diff"]), jd.get("override"))
                               if jd["diff"] else "")))
        return _fail(res, "paired reads: the statement reads this gate more than once; replace the whole expression by hand")
    if mode == "expr":
        sub = _expr_parts(T, cs, ce)
        if sub is None:
            return _fail(res, "getenv result is used as a pointer here (not a bare presence test)")
        return _expr_emit(res, row, T, raw, p, cs, ce, sub, notes)
    r = gi.analyze_site(interp, path, T, p, gate)
    if r["status"] != "reader":
        why = r["status"]
        return _fail(res, "classifier: %s" % why)
    d = gi.describe(r)
    if d is None:
        return _fail(res, "classifier gave no outcome vector")
    fam, vec, cls, inv = d
    res.idiom = cls
    if fam != "bool":
        return _fail(res, "reader is %s-valued but the row is Bool" % fam)
    raw_vec = [_flip(x) for x in vec] if inv else list(vec)
    if mode == "local":
        pt = unit["ptr"]
        sub_unit = _local_unit(T, unit, pt)
        if isinstance(sub_unit, str):
            return _fail(res, sub_unit)
        unit.update(sub_unit)
    jd = judge_bool(row, raw_vec, site_text=T[gi._stmt_start(T, cs):gi._stmt_start(T, cs) + 400])
    if "reason" in jd and "verdict" not in jd:
        return _fail(res, jd["reason"])
    return _bool_emit(res, row, jd, unit, mode, notes, T, raw, inv)


# ----- bool emission ---------------------------------------------------------------------------------

def _why_bool(row, jd, notes, res):
    r = jd["rule"]
    parts = ["site %s%s vs row %s%s" % ("!" if jd["neg"] else "", r, row.rule,
                                       "(def=%s)" % ("on" if row.default else "off") if row.rule in ("Std", "Lead0Def") else "")]
    if jd["diff"]:
        parts.append("differs on %s" % ",".join(jd["diff"]))
    if jd["verdict"] == "EDGE":
        parts.append("byte-identical alternative: %s" % get_expr(row.name, jd.get("alt"), jd["neg"]))
    if res.harness_hits:
        parts.append("harness sets %s (changes behavior)" % ",".join(repr(h) for h in res.harness_hits))
    elif jd["diff"]:
        parts.append("no harness value in the seed hits a changed input" +
                     (" (seed harnessValues=%s)" % row.harness if row.harness else ""))
    return "; ".join(parts + list(notes))


def _bool_emit(res, row, jd, unit, mode, notes, T, raw, inv):
    res.verdict = jd["verdict"]
    res.diff = jd["diff"]
    res.override = jd.get("override") or ""
    res.harness_hits = harness_impact(row, jd["rule"], jd["default_used"], jd["diff"])
    e = get_expr(row.name, res.override or None, jd["neg"])
    lhs = unit["lhs"]
    if mode == "local":
        res.replacement = "%s%s;" % (unit["tgt"], e)
        res.old = _norm_ws(T[unit["span"][0]:unit["span2"][1]])
        res.edit = (unit["span"][0], unit["span2"][1], res.replacement)
    else:
        res.replacement = "%s = %s;" % (_norm_ws(_lhs_text(lhs)), e)
        res.old = _norm_ws(T[unit["span"][0]:unit["span"][1]])
        res.edit = (unit["span"][0], unit["span"][1], res.replacement)
    res.mode = mode
    res.idiom = res.idiom or jd["rule"]
    if jd["neg"]:
        res.idiom += " (inverted use)"
    res.why = _why_bool(row, jd, notes, res)
    return res


def _lhs_text(lhs):
    return " ".join(lhs["quals"] + ([lhs["type"]] if lhs["type"] else []) + [lhs["name"]]).replace("* ", "*")


def _local_unit(T, unit, ptr):
    """For `const char* v = getenv(..);` followed by one simple statement that is the whole reader."""
    a, b = unit["span"]
    nxt_end = gi._stmt_end(T, b)
    st2 = T[b:nxt_end]
    if re.search(r"[{}]", st2) or _CTRL_RE.match(st2) and not st2.lstrip().startswith("return"):
        return "pointer %s is consumed by a block/branch statement: not a single expression" % ptr
    m = re.match(r"\s*return\s+(.*);\s*$", st2, re.S)
    tgt = None
    tgt_decl = ""
    if m:
        expr = m.group(1)
        tgt = "return "
    else:
        eq = _split_assign(st2, len(st2))
        if eq < 0:
            return "pointer %s is not consumed by a `return`/assignment" % ptr
        l2 = parse_lhs(st2[:eq])
        if l2 is None:
            return "pointer %s: unrecognised assignment target" % ptr
        expr = st2[eq + 1:].rstrip().rstrip(";")
        tgt = _norm_ws(_lhs_text(l2)) + " = "
        if l2["is_decl"]:
            tgt_decl = ""
    bad = body_impure(expr, extra_ok=(ptr,))
    if bad:
        return "expression using %s also names `%s`" % (ptr, bad)
    if not re.search(r"\b%s\b" % re.escape(ptr), expr):
        return "pointer %s is not used by the following statement" % ptr
    blk = gi.enclosing_block(T, a)
    tail = T[nxt_end:blk[1] if blk else len(T)]
    if re.search(r"\b%s\b" % re.escape(ptr), tail):
        return "pointer %s is used again later in the block" % ptr
    return {"span2": (b, nxt_end), "tgt": tgt, "tgt_decl": tgt_decl, "lhs": unit["lhs"]}


# ----- expression-mode (bare presence test or == / != nullptr) -----------------------------------------

_NULLCMP_RE = re.compile(r"\s*(!=|==)\s*(?:nullptr|NULL|0|\(\s*(?:const\s+)?char\s*\*\s*\)\s*0)(?![\w.])")


def _expr_parts(T, cs, ce):
    """-> dict(end, neg, prefix_pointer) for the sub-expression to replace, or None if the call is used as a
    pointer.  neg is True for `== nullptr`."""
    post = T[ce:ce + 60]
    nm = _NULLCMP_RE.match(post)
    if nm:
        return {"end": ce + nm.end(), "neg": nm.group(1) == "==", "prefix_pointer": False}
    pre = T[:cs].rstrip()
    after = post.lstrip()
    before_ok = pre.endswith(("(", "!", "&&", "||", "?", "=", ",", ":", "{")) or re.search(r"\breturn$", pre)
    after_ok = (after[:1] in (")", ";", "?", ",", ":", "}") or after.startswith("&&") or after.startswith("||"))
    whole_value = (pre.endswith("=") or re.search(r"return$", pre) or pre.endswith(",")) and after[:1] in (";", ",", ")")
    call_arg = False
    if pre.endswith("("):
        wm = re.search(r"([A-Za-z_]\w*|[>\]])\s*\($", pre)
        call_arg = bool(wm) and wm.group(1) not in ("if", "while", "for", "switch", "return", "case")
    if before_ok and after_ok and not whole_value and not call_arg:
        return {"end": ce, "neg": False, "prefix_pointer": False}
    return None


def _expr_emit(res, row, T, raw, p, cs, ce, sub, notes, presence_only=False):
    res.mode = "expr"
    res.idiom = "Presence" + (" (== nullptr)" if sub["neg"] else "")
    la, lb = _line_bounds(T, cs)
    old_line = raw[la:lb]
    if presence_only:
        new_e = "%s::isSet<\"%s\">()" % (NS, row.name)
        if sub["neg"]:
            new_e = "!" + new_e
        res.verdict = "NO-CHANGE"
        res.why = "presence test of a %s row: isSet<> is the presence read" % row.kind
        res.why = "; ".join([res.why] + list(notes))
    else:
        jd = judge_bool(row, ["on" if b else "off" for b in rule_vec("Presence", False)])
        if "verdict" not in jd:
            return _fail(res, jd.get("reason", "no verdict"))
        jd["neg"] = sub["neg"] if jd.get("rule") else sub["neg"]
        res.diff = jd["diff"]
        res.verdict = jd["verdict"]
        res.override = jd.get("override") or ""
        res.harness_hits = harness_impact(row, "Presence", False, jd["diff"])
        new_e = get_expr(row.name, res.override or None, sub["neg"])
        # the whole expression's value is the old test: a `== nullptr` flips it, nothing else changes
        jd2 = dict(jd)
        jd2["neg"] = sub["neg"]
        jd2["rule"] = "Presence"
        res.why = _why_bool(row, jd2, notes, res)
    # a declaration/assignment is shown whole (it may span lines); a control statement shows its own line
    ss = _stmt_begin(T, cs)
    se = gi._stmt_end(T, ss)
    if not _CTRL_RE.match(T[ss:se]) and "{" not in T[ss:cs] and ss < la:
        # the comment-stripped text keeps the raw offsets, so its first non-blank character is where the
        # statement really starts (a comment block above it is not part of the statement)
        la, lb = ss + (len(T[ss:se]) - len(T[ss:se].lstrip())), se
        old_line = raw[la:lb]
    new_line = raw[la:cs] + new_e + raw[sub["end"]:lb]
    # `x = (A);` loses the redundant parentheses a `!= nullptr` test needed
    new_line = re.sub(r"=\s*\(\s*(%s)\s*\)\s*;" % re.escape(new_e), r"= \1;", new_line)
    res.old = _norm_ws(old_line)
    res.replacement = _norm_ws(new_line)
    res.edit = (la, lb, new_line)
    return res


# ----- numeric emission -------------------------------------------------------------------------------

def _numeric(res, row, interp, path, T, p, cs, ce, unit, mode, notes, raw):
    if unit is None or mode not in ("lambda", "decl-num"):
        return _fail(res, "numeric reader is not a lambda/declaration initializer (e.g. multi-statement local)")
    lhs = unit["lhs"]
    res.idiom = "Num(%s)" % ",".join(sorted(set(_PARSER_RE.findall(unit["text"])))) if _PARSER_RE.search(unit["text"]) \
        else "Num"
    jd = judge_numeric(row, interp, path, T, p, unit["text"], unit["var"])
    if "verdict" not in jd:
        if jd.get("boolish"):
            return _fail(res, "numeric row read as a bool test: %s" % jd["reason"])
        return _fail(res, jd["reason"])
    cast = _cast_for(row.kind, lhs["type"])
    e = get_expr(row.name)
    if cast:
        e = "static_cast<%s>(%s)" % (cast, e)
    res.verdict = jd["verdict"]
    res.diff = [("unset" if x is None else repr(x)) for x in jd["diff"]]
    res.replacement = "%s = %s;" % (_norm_ws(_lhs_text(lhs)), e)
    res.old = _norm_ws(T[unit["span"][0]:unit["span"][1]])
    res.edit = (unit["span"][0], unit["span"][1], res.replacement)
    res.mode = mode
    res.row_edit = jd["edit"]
    hand = []
    if jd["edit"]:
        hand.append("the row needs an edit first (%s) %s: apply it by hand after checking it against the reader" % (row.name, jd["edit"]))
    bad_in = [d for d in res.diff if d not in _BENIGN_NUMERIC_EDGES]
    if bad_in:
        hand.append("the row reads %s differently from the old reader (the reader parses, converts or snaps beyond the "
                    "row's parse+default+range)" % ", ".join(bad_in[:6]))
    hits = [h for h in row.harness if h != "String" and h in [x for x in jd["diff"] if x is not None]]
    res.harness_hits = hits
    parts = []
    if jd["edit"]:
        parts.append("ROW EDIT needed first (%s) %s" % (row.name, jd["edit"]))
    else:
        parts.append("no row edit needed")
    if jd["diff"]:
        parts.append("inputs whose result changes after the row edit: %s" % ", ".join(res.diff))
        parts.append("harness sets %s" % (",".join(repr(h) for h in hits) if hits else "none of them") +
                     (" (seed harnessValues=%s)" % row.harness if row.harness else ""))
    res.why = "; ".join(parts + list(notes))
    if hand:
        # The plan keeps its verdict (the row-edit proposals feed the row-edit section), but --apply never rewrites
        # these: replacing the reader by get<>() would silently drop whatever it does besides parse (clamp to a set,
        # snap, convert units, fall back to a second gate). A human rewrites the site.
        res.hand = "; ".join(hand)
        res.why = "HAND (not applied by the tool): " + res.hand + " [" + res.why + "]"
    return res


# ---------------------------------------------------------------------------
# Whole run, summary and rendering
# ---------------------------------------------------------------------------

def run(files, root=ROOT, rev="HEAD", helpers=()):
    rows = load_rows(root, rev)
    ledger = load_ledger(root)
    results, srcs = [], []
    for rel in files:
        srcs.append((rel, read_source(root, rel, rev)))
    extra = [(p, t) for p, t in helpers]
    for rel, raw in srcs:
        others = [(p, t) for p, t in srcs if p != rel] + extra
        results.extend(analyze_text(rel, raw, rows, ledger, others))
    return results, rows


GATE_INCLUDE = '#include "Gates/gate.h"'


def _kept_comments(raw, T, a, b):
    """The comment-only lines inside the old statement raw[a:b], re-emitted above the replacement (they hold the WHY
    of the old reader: default history, measurements). Trailing comments on code lines are dropped with the code."""
    ls = raw.rfind("\n", 0, a) + 1
    indent = raw[ls:a] if not raw[ls:a].strip() else ""
    nl = "\r\n" if "\r\n" in raw else "\n"
    out = []
    pos = a
    for rl in raw[a:b].split("\n"):
        seg_t = T[pos:pos + len(rl)]
        pos += len(rl) + 1
        stripped = rl.strip()
        if stripped and not seg_t.strip():
            out.append(stripped.rstrip("\r"))
    return "".join(c + nl + indent for c in out)


_HELPER_NO_CHANGE = re.compile(r"vs row \w+: NO-CHANGE$")


def _apply_helper_calls(raw, results):
    """Rewrite `helper("MC2_X")` calls whose interpreter reading says the helper reads exactly what the row reads
    (NO-CHANGE, no differing input): the call becomes `get<"MC2_X">()`, the surrounding expression stays. Only
    single-literal-argument calls are touched, so a helper with a default argument stays a human's job. The helper
    definition itself is left in place; the author deletes it once its last caller is gone."""
    done = []
    seen = set()
    for r in results:
        if r.verdict != "UNHANDLED" or not r.callee or not _HELPER_NO_CHANGE.search(r.helper_read or ""):
            continue
        key = (r.callee, r.gate)
        if key in seen:
            continue
        seen.add(key)
        rx = re.compile(r"(?<![\w:.>])%s\(\s*\"%s\"\s*\)" % (re.escape(r.callee), re.escape(r.gate)))
        new, n = rx.subn(get_expr(r.gate), raw)
        if n:
            raw = new
            done.extend([r] * n)
    return raw, done


# The two-read spellings of one gate that the plan lists as "paired reads": each is a complete expression whose value
# is a known rule applied to the variable's text, so when the row carries that rule the whole expression becomes one
# typed read. {G} is the gate literal, {C} the getenv spelling (std:: optional).
_GE = r"(?:std::)?getenv\(\s*\"{G}\"\s*\)"
_PAIRED_FORMS = (
    ("AtoiNZ", _GE + r"\s*&&\s*(?:std::)?atoi\(\s*" + _GE + r"\s*\)\s*!=\s*0"),
    ("Lead0P", _GE + r"\s*!=\s*(?:nullptr|NULL)\s*&&\s*" + _GE + r"\s*\[\s*0\s*\]\s*!=\s*'0'"),
    ("Lead0", _GE + r"\s*&&\s*" + _GE + r"\s*\[\s*0\s*\]\s*&&\s*" + _GE + r"\s*\[\s*0\s*\]\s*!=\s*'0'"),
    ("Lead1", _GE + r"\s*(?:!=\s*(?:nullptr|NULL)\s*)?&&\s*" + _GE + r"\s*\[\s*0\s*\]\s*==\s*'1'"),
)


def _apply_paired_reads(raw, results):
    """Replace a gate's two-read expression by one typed read when the row has the rule that expression computes."""
    done = []
    T = gi.strip_comments(raw)
    edits = []
    # the row rule is not stored on a Result; look it up through the planner's table when it was loaded
    table = _CURRENT_ROWS or {}
    for gate in sorted({r.gate for r in results}):
        row = table.get(gate)
        if row is None or row.kind != "Bool":
            continue
        for rule, tmpl in _PAIRED_FORMS:
            if row.rule != rule:
                continue
            core = tmpl.replace("{G}", re.escape(gate))
            # a whole parenthesised expression, else the bare one: never swallow a `)` that belongs to an enclosing `if (`
            rx = re.compile(r"\(\s*" + core + r"\s*\)|" + core)
            for m in rx.finditer(T):
                edits.append((m.start(), m.end(), get_expr(gate), gate))
    edits.sort(key=lambda e: -e[0])
    last = len(raw) + 1
    for a, b, new, gate in edits:
        if b > last:
            continue
        # keep one pair of parentheses the original spelled around the expression
        old = raw[a:b]
        lead = old[:len(old) - len(old.lstrip())]
        wrap = old.lstrip().startswith("(") and old.rstrip().endswith(")")
        raw = raw[:a] + lead + ("(" + new + ")" if wrap else new) + raw[b:]
        last = a
        r = next(x for x in results if x.gate == gate)
        done.append(r)
    return raw, done


_CURRENT_ROWS = None


def apply_edits(raw, results):
    """The file text with every NO-CHANGE / EDGE site rewritten (descending offsets, overlaps skipped) and the gate
    include added. Returns (new_text, applied, skipped)."""
    # A statement's span starts right after the previous `;`, `{` or `}`: skip the blanks and comment blocks that
    # precede it (the comment-stripped text keeps raw offsets, with comments blanked) so they and the indentation stay.
    T = gi.strip_comments(raw)
    todo = sorted((r for r in results if r.edit and r.verdict in ("NO-CHANGE", "EDGE") and not getattr(r, "hand", "")),
                  key=lambda r: -r.edit[0])
    applied, skipped, limit = [], [], len(raw) + 1
    for r in todo:
        a, b, new = r.edit
        while a < b and T[a].isspace():
            a += 1
        new = _kept_comments(raw, T, a, b) + new.lstrip(" 	")
        if b > limit:
            skipped.append(r)
            continue
        raw = raw[:a] + new + raw[b:]
        limit = a
        applied.append(r)
    raw, helper_calls = _apply_helper_calls(raw, results)
    applied = applied + helper_calls
    raw, paired = _apply_paired_reads(raw, results)
    applied = applied + paired
    # `x = (mc2::gate::get<"X">());` keeps no parentheses of its own
    raw = re.sub(r'(=[ \t]*(?:\r?\n[ \t]*)?)\((mc2::gate::get<"[A-Z0-9_]+">\(\))\);', r'\1\2;', raw)
    if applied and "Gates/gate.h" not in raw and "Gates/gate_cold.h" not in raw:
        nl = "\r\n" if "\r\n" in raw else "\n"
        lines = raw.split(nl)
        # the last #include of the first 300 lines that is NOT inside an #if / #ifdef / #ifndef block
        depth, last = 0, -1
        for i, ln in enumerate(lines[:300]):
            s = ln.lstrip()
            if s.startswith("#") and s[1:].lstrip().startswith(("if", "ifdef", "ifndef")):
                depth += 1
            elif s.startswith("#") and s[1:].lstrip().startswith("endif"):
                depth = max(0, depth - 1)
            elif s.startswith("#include") and depth == 0:
                last = i
        # not inside an `extern "C" {` block (a template cannot have C linkage), and before the first use
        extern_depth, safe = 0, -1
        for i, ln in enumerate(lines[:300]):
            if re.match(r'\s*extern\s+"C"\s*\{', ln):
                extern_depth += 1
            elif extern_depth and ln.strip() == "}":
                extern_depth -= 1
            elif ln.lstrip().startswith("#include") and not extern_depth and i <= last:
                safe = i
        last = safe if safe >= 0 else last
        first_use = next((i for i, ln in enumerate(lines) if "mc2::gate::" in ln), len(lines))
        if last + 1 > first_use:
            last = next((i for i, ln in enumerate(lines[:300]) if ln.lstrip().startswith("#include")), -1)
        lines.insert(last + 1, GATE_INCLUDE)
        raw = nl.join(lines)
    return raw, applied, skipped


def summarize(results):
    c = Counter(r.verdict for r in results)
    by_file = {}
    for r in results:
        by_file.setdefault(r.file, Counter())[r.verdict] += 1
    shapes = Counter(r.shape for r in results)
    overrides = [r for r in results if r.override]
    edits = {}
    for r in results:
        if r.row_edit:
            edits[r.gate] = r.row_edit
    return {"total": len(results), "verdicts": {v: c.get(v, 0) for v in VERDICTS}, "by_file": by_file,
            "shapes": {s: shapes.get(s, 0) for s in SHAPES}, "overrides": len(overrides),
            "row_edits": edits}


def tsv_text(results):
    return TSV_HEADER + "\n" + "\n".join(r.tsv() for r in results) + "\n"


def _cell(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def _md_table(rs):
    out = ["| file:line | gate | old idiom class | old caching shape | proposed replacement | verdict | why |",
           "|---|---|---|---|---|---|---|"]
    for r in rs:
        out.append("| %s |" % " | ".join(_cell("`%s`" % x if k in (0, 1, 4) and x not in ("", "-") else x)
                                           for k, x in enumerate([r.where, r.gate, r.idiom, r.shape, r.replacement,
                                                                  r.verdict, r.why])))
    return out


def render_md(results, rows, files, rev_label, family="g1b", ledger=None, umbrella=None, writes=None):
    s = summarize(results)
    fam = FAMILIES[family]
    L = []
    L.append("# " + fam["title"])
    L.append("")
    # The old-idiom text and the row defaults quoted below are history, not current claims about a default:
    # the default-agreement checker skips documents that carry this marker.
    L.append(gi.QUOTED_HISTORY_MARKER)
    L.append("")
    L.append("Generated by `%s`. **No C++ was changed.** Each row says what happens if "
             "the raw `getenv` read at `file:line` is rewritten to `%s::get<\"MC2_X\">()` against the gate row "
             "committed in `Gates/gate_table.json` (design `typed-gate-registry-design.md` section 5.7, review "
             "amendments 1, 2, 9). Scanned: commit `%s` (files read from git, not the working tree)." % (fam["cmd"], NS, rev_label))
    L.append("")
    L.append("How to read the verdicts:")
    L.append("")
    L.append("* **NO-CHANGE** -- the old reader and the row answer identically for every probe input. Rewrite.")
    L.append("* **EDGE** -- differs only on `\"\"`, `false`, other text, or (numeric) junk / out-of-range input. Rewrite and list the file in the PR; "
             "the byte-identical alternative is named in the row.")
    L.append("* **SEMANTICS** -- differs on unset, `\"0\"` or `\"1\"`, or on the default. The proposed text is the per-site override "
             "(`get<\"N\", Rule::R>()`), never Std, so behavior is byte-identical; converting it to the row rule is a separate named commit.")
    L.append("* **UNHANDLED** -- the tool could not prove the site; a human reads the code line.")
    L.append("")
    L.append("Rows in the committed table still carry the legacy rule of their readers (`L(Presence, ...)` etc.), so NO-CHANGE here means "
             "\"identical to the row as committed\"; converting a row to Std is a separate named commit and would change these verdicts.")
    L.append("")
    L.append("Probe inputs: **U** unset, **E** `\"\"`, **Z** `\"0\"`, **F** `\"false\"`, **O** `\"1\"`, **X** `\"yes\"`, **X10** `\"10\"`, **X25** `\"2.5\"`. "
             "Caching shape is preserved: a `static const T s = <reader>` stays a static holding `get<>()`; a lambda static loses only the lambda.")
    L.append("")
    L.append("Every file that gets a replacement needs `#include \"Gates/gate.h\"`.")
    L.append("")
    L.append("## Summary")
    L.append("")
    L.append("| Measure | Count |")
    L.append("|---|---|")
    L.append("| sites analysed | %d |" % s["total"])
    for v in VERDICTS:
        L.append("| **%s** | %d |" % (v, s["verdicts"][v]))
    L.append("| per-site `Rule::` overrides proposed (each raises the `gate_overrides` baseline from 0) | %d |" % s["overrides"])
    L.append("| gates needing a row edit first | %d |" % len(s["row_edits"]))
    for sh in SHAPES:
        L.append("| shape `%s` | %d |" % (sh, s["shapes"][sh]))
    n_help = sum(len(v) for v in helper_groups(results).values())
    n_umb = sum(1 for r in results if r.verdict == "UNHANDLED" and is_umbrella_result(r))
    n_pair = sum(1 for r in results if r.paired)
    if n_help or n_umb or n_pair:
        L.append("| UNHANDLED, helper-call reader (%d helpers) | %d |" % (len(helper_groups(results)), n_help))
        L.append("| UNHANDLED, VK umbrella resolver (G4) | %d |" % n_umb)
        L.append("| UNHANDLED, paired reads of one gate in one statement | %d |" % n_pair)
    L.append("")
    L.append("| File | " + " | ".join(VERDICTS) + " | total |")
    L.append("|---|" + "---|" * (len(VERDICTS) + 1))
    for f in files:
        c = s["by_file"].get(f, Counter())
        L.append("| `%s` | %s | %d |" % (f, " | ".join(str(c.get(v, 0)) for v in VERDICTS), sum(c.values())))
    L.append("")
    for verdict, title, blurb in (
            ("SEMANTICS", "SEMANTICS rows", "Each must migrate with the override shown, never with the row rule."),
            ("EDGE", "EDGE rows", "Listed inputs change result if the row's own rule is used."),
            ("UNHANDLED", "Sites the tool could not handle", "The code line is the reader as written.")):
        rs = [r for r in results if r.verdict == verdict]
        L.append("## %s (%d)" % (title, len(rs)))
        L.append("")
        L.append(blurb)
        L.append("")
        if not rs:
            L.append("None.")
            L.append("")
            continue
        L.extend(_md_table(rs))
        L.append("")
        if verdict == "UNHANDLED":
            for r in rs:
                L.append("* `%s` (%s): `%s`" % (r.where, r.gate, r.old or _code_line_hint(r)))
            L.append("")
    L.extend(render_family_sections(results, rows, ledger, umbrella, writes))
    L.append("## Row edits required before the %s applies" % fam["what"])
    L.append("")
    L.append("These change `Gates/gate_table.cpp` only; every other reader of the gate is still a raw `getenv` until it migrates, "
             "so nothing observable moves. After the edit the listed sites verify as shown above.")
    L.append("")
    if s["row_edits"]:
        L.append("| gate | row now | edit |")
        L.append("|---|---|---|")
        for g in sorted(s["row_edits"]):
            rw = rows[g]
            now = "%s %s default=%s range=%s" % (rw.kind, rw.rule, rw.default,
                                                 "none" if not rw.range else "%s..%s%s" % (
                                                     rw.range.get("lo"), rw.range.get("hi"),
                                                     " clamp" if rw.range.get("clamp") else " reject"))
            L.append("| `%s` | %s | %s |" % (g, now, _cell(s["row_edits"][g])))
    else:
        L.append("None.")
    L.append("")
    rowfix = sorted({r.gate for r in results if r.verdict == "SEMANTICS" and r.override and r.kind == "Bool"
                     and rows[r.gate].rule == "Std"})
    if rowfix:
        L.append("Bool rows whose committed rule is Std but whose reader is not: " +
                 ", ".join("`%s`" % g for g in rowfix) +
                 ". Fix the row (`L(Rule, ...)`) or keep the per-site override.")
        L.append("")
    L.append("## Full table")
    L.append("")
    L.extend(_md_table(results))
    L.append("")
    L.append("## Before and after")
    L.append("")
    for r in results:
        L.append("* `%s` %s (%s, %s)" % (r.where, r.gate, r.verdict, r.shape))
        L.append("  * before: `%s`" % (r.old or _code_line_hint(r)))
        L.append("  * after: `%s`" % r.replacement)
    L.append("")
    L.append("## Limits of this plan")
    L.append("")
    L.append("* The interpreter in `scripts/gate_idioms.py` models C string reads only (`getenv`, `atoi`/`atol`/`atof`, character tests, `strcmp`); "
             "a site that needs more is UNHANDLED, never guessed.")
    L.append("* `inf`/`nan` and float-to-double rounding at a bound are modelled only through the `1e999` probe; `NaN` input is not probed.")
    L.append("* Harness impact reads `harnessValues` in the gate seed (values found in scripts, tests, tools, data). Launcher `offVal`s and installed "
             "`launcher_env.json` files are not in the seed.")
    L.append("* A replacement that drops a `const char*` local also drops the local's name, so the tool refuses when the pointer is used again.")
    L.append("")
    return "\n".join(L).rstrip() + "\n"


def _code_line_hint(r):
    return r.old or "(see %s)" % r.where


def unhandled_code_lines(results, root, rev):
    """Fill r.old with the raw source line of UNHANDLED sites so the report shows the code."""
    cache = {}
    for r in results:
        if r.old:
            continue
        if r.file not in cache:
            cache[r.file] = read_source(root, r.file, rev).split("\n")
        lines = cache[r.file]
        if 0 < r.line <= len(lines):
            r.old = _norm_ws(lines[r.line - 1])


# ---------------------------------------------------------------------------
# Family reports (G3): helper groups, umbrella coupling, ledger kind edits
#
# The per-site verdicts above answer "may this raw getenv become get<>()".  A family such as the terrain
# files also holds readers the planner must NOT guess at: helper-call readers (tglc_envOn, envF, ...), the VK
# umbrella resolver (subGate, envTriState), reads that span several statements, and rows whose kind the
# G0 ledger says is wrong.  These functions collect those so the migration builder starts from a list of
# exactly what is left, grouped the way the work is split.
# ---------------------------------------------------------------------------

_RAW_READ_NAMES = frozenset(gi._RAW_ENV_CALLEES)


def is_helper_result(r):
    """True when the site reads the gate through a helper instead of a raw getenv."""
    return r.callee not in _RAW_READ_NAMES


def is_umbrella_result(r):
    return r.callee in UMBRELLA_HELPERS


def helper_groups(results):
    """{helper name: [Result]} for UNHANDLED helper-call readers that are not the VK umbrella resolver."""
    groups = {}
    for r in results:
        if r.verdict == "UNHANDLED" and is_helper_result(r) and not is_umbrella_result(r):
            groups.setdefault(r.callee, []).append(r)
    return groups


_REASON_NORMALISERS = (
    (re.compile(r"`[^`]*`"), "`X`"),
    (re.compile(r"\bpointer \w+"), "pointer P"),
    (re.compile(r" for input .*$"), ""),
)


def reason_class(reason):
    """A reason with the names and numbers removed, so equal causes group together."""
    if reason.startswith("paired reads"):
        return "paired reads: one gate read twice in one statement (replace the whole expression by hand)"
    for rx, sub in _REASON_NORMALISERS:
        reason = rx.sub(sub, reason)
    return reason.strip()


def reason_groups(results):
    """{reason class: [Result]} for UNHANDLED sites that are neither helper calls nor umbrella callers."""
    groups = {}
    for r in results:
        if r.verdict == "UNHANDLED" and not is_helper_result(r):
            groups.setdefault(reason_class(r.reason or r.why), []).append(r)
    return groups


_ENUM_TOKENS_RE = re.compile(r"Enum\s*\{([^}]*)\}")


def ledger_kind_edits(results, rows, ledger):
    """Gates the G0 ledger declares as an Enum while the committed row is another kind.

    -> {gate: dict(row, tokens, rule, edge_inputs)}.  `edge_inputs` are atoi-prefix spellings that select a
    token under the legacy reader but would not match a whole-token Enum row.
    """
    out = {}
    for g in sorted({r.gate for r in results}):
        entry = (ledger or {}).get(g)
        row = rows.get(g)
        if not entry or row is None or row.kind == "Enum":
            continue
        m = _ENUM_TOKENS_RE.search(entry.get("rule", ""))
        if not m:
            continue
        toks = [t.strip() for t in m.group(1).split(",") if t.strip()]
        probes = ("02", "+2", " 2", "2abc", "2.5", "04", "4.9", "1x")
        edge = [p for p in probes if str(_prefix_num(p, True)) in toks and p not in toks]
        out[g] = {"row": row, "tokens": toks, "rule": entry.get("rule", ""), "edge_inputs": edge,
                  "evidence": entry.get("evidence", "")}
    return out


def ledger_notes(results, ledger):
    """[(gate, entry, [sites])] for every gate of the plan the G0 ledger has something to say about."""
    by_gate = {}
    for r in results:
        by_gate.setdefault(r.gate, []).append(r)
    return [(g, (ledger or {})[g], by_gate[g]) for g in sorted(by_gate) if g in (ledger or {})]


# ----- umbrella coupling -------------------------------------------------------------------------------

# Resolvers read the environment through the umbrella; consumers only ask a wrapper for its answer, so a
# consumer carries no gate-name literal yet still depends on the umbrella (the island's exported predicates
# `mc2_vk_terrain_*_enabled`, and the header's umbrellaOn / terrainNativeFirstOwnerOn / postDirectChainUpstreamOn).
UMBRELLA_CONSUMER_NAMES = ("umbrellaOn", "postDirectChainUpstreamOn")
_DEF_PREFIX_RE = re.compile(r"\b(?:bool|int|void|inline|static)\s+$")
_CTRL_HEAD_RE = re.compile(r"^(?:if|for|while|switch|else|return|do|namespace|struct|class|enum|union|typedef|using|case|catch|try)\b")


def enclosing_function(T, pos):
    """-> (name, line) of the function (or namespace-scope initializer) holding `pos`; best effort, column-0 heads only."""
    lines = T.split("\n")
    for i in range(T.count("\n", 0, pos), -1, -1):
        t = lines[i]
        if not t or t[0] in " \t}#/{":
            if t[:1] == "}":
                return "<namespace scope>", i + 1
            continue
        if _CTRL_HEAD_RE.match(t):
            continue
        if re.match(r"[\w:<>,\s\*&]+=\s*\[", t):
            return "<namespace-scope initializer>", i + 1      # `T x = [] {` has no parameter list to find
        if "(" not in t:
            continue
        pre = t.split("(", 1)[0]
        if "=" in pre:
            return "<namespace-scope initializer>", i + 1
        mm = re.search(r"(~?\w+)\s*$", pre.rstrip())
        if mm:
            return mm.group(1), i + 1
    return "<unknown>", 0


def _umbrella_calls(T, names):
    """[(match, name)] for each call of one of `names` in the comment-stripped text, definitions excluded."""
    rx = re.compile(r"\b(%s)\s*\(" % "|".join(re.escape(n) for n in sorted(names)))
    return [m for m in rx.finditer(T) if not _DEF_PREFIX_RE.search(T[max(0, m.start() - 12):m.start()])]


def umbrella_report(files, root, rev, results):
    """Every place in `files` that depends on the VK umbrella, with the function holding it.

    Resolvers (subGate, envTriState, ...) read the environment through the umbrella.  A function that calls
    one depends on the umbrella, and so does every function that calls such a function; the exported
    `mc2_vk_*_enabled` predicates among them are the cross-file consumers (they carry no gate literal).

    -> dict(sites=[...], wrappers=[...], vk_family=[(Result, nearest umbrella line)])."""
    texts = {}
    for rel in files:
        raw = read_source(root, rel, rev)
        texts[rel] = (raw, gi.strip_comments(raw))
    roots = set(UMBRELLA_HELPERS) | set(UMBRELLA_CONSUMER_NAMES)

    # functions that depend on the umbrella, to a fixed point
    dependent = {}                                      # name -> (file, definition line)
    frontier = set(roots)
    while frontier:
        found = set()
        for rel, (_, T) in texts.items():
            for m in _umbrella_calls(T, frontier):
                fn, fn_line = enclosing_function(T, m.start())
                if not fn.startswith("<") and fn not in dependent and fn not in roots:
                    dependent[fn] = (rel, fn_line)
                    found.add(fn)
        frontier = found
    exported = {n for n in dependent if re.fullmatch(r"mc2_vk_\w+_enabled", n)}

    sites, funcs = [], {}
    for rel, (raw, T) in texts.items():
        src_lines = raw.split("\n")
        for m in _umbrella_calls(T, roots | exported):
            name = m.group(1)
            open_at = m.end() - 1
            ce = _match_paren(T, open_at)
            arg = T[open_at + 1:ce - 1] if ce > 0 else ""
            lit = re.match(r'\s*"(MC2_[A-Z0-9_]+)"', arg)
            line = T.count("\n", 0, m.start()) + 1
            fn, fn_line = enclosing_function(T, m.start())
            is_resolver = name in UMBRELLA_HELPERS and name != "terrainNativeFirstOwnerOn"
            sites.append({"file": rel, "line": line, "helper": name, "gate": lit.group(1) if lit else "",
                          "func": fn, "func_line": fn_line, "code": _norm_ws(src_lines[line - 1]),
                          "role": "resolver" if is_resolver else "consumer"})
            if is_resolver and not fn.startswith("<"):
                callers = len(re.findall(r"\b%s\s*\(" % re.escape(fn), T)) - 1
                e = funcs.setdefault((rel, fn), {"file": rel, "func": fn, "line": fn_line, "callers": max(callers, 0),
                                                 "helpers": [], "gates": []})
                e["helpers"].append(name)
                if lit:
                    e["gates"].append(lit.group(1))
    lines_by_file = {}
    for st in sites:
        lines_by_file.setdefault(st["file"], []).append(st["line"])
    vk = []
    for r in results:
        if r.gate.startswith("MC2_VK_") and not is_umbrella_result(r):
            near = [ln for ln in lines_by_file.get(r.file, []) if abs(ln - r.line) <= 12]
            vk.append((r, min(near, key=lambda ln: abs(ln - r.line)) if near else 0))
    wrappers = sorted(funcs.values(), key=lambda e: (e["file"], e["line"]))
    return {"sites": sites, "wrappers": wrappers, "vk_family": vk, "dependent": dependent}


_WRITE_RE = re.compile(r"\b(?:_?putenv(?:_s)?|setenv|SetEnvironmentVariable[AW]?)\s*\(\s*\"(MC2_[A-Z0-9_]+)")


def runtime_writes(root=ROOT, rev="HEAD"):
    """{gate: ["file:line", ...]} for every source that WRITES a gate into the environment (_putenv, setenv, ...).

    A reader that re-reads the variable on every call sees such a write; a typed `get<>()` caches the first
    answer, so a per-call site of a gate that is written at run time changes behavior unless the row is `reread`.
    """
    pat = r"(putenv|setenv|_putenv_s|SetEnvironmentVariable[AW]?)[[:space:]]*\([[:space:]]*\"MC2_"
    cmd = ["git", "-C", root, "grep", "-n", "-E", pat] + ([rev] if rev else []) + ["--", "*.cpp", "*.h", "*.cc", "*.mm"]
    try:
        out = subprocess.run(cmd, **child_kwargs(redirected=True, capture_output=True)).stdout.decode("utf-8", "replace")
    except OSError:
        return {}
    writes = {}
    for ln in out.split("\n"):
        m = _WRITE_RE.search(ln)
        if not m:
            continue
        head = ln.split(":")                    # [rev:]path:line:text
        if rev and head and head[0] == rev:
            head = head[1:]
        if len(head) >= 2:
            writes.setdefault(m.group(1), []).append("%s:%s" % (head[0], head[1]))
    return writes


def row_now(rw):
    """One-cell description of a committed gate row."""
    if rw is None:
        return "(no row)"
    bits = ["%s %s" % (rw.kind, rw.rule)]
    if rw.kind == "Bool":
        bits.append("default=%s" % ("on" if rw.default else "off"))
    elif rw.kind in ("Int", "Float"):
        bits.append("default=%s" % rw.default)
        bits.append("range=%s" % ("none" if not rw.range else "%s..%s%s" % (
            rw.range.get("lo"), rw.range.get("hi"), " clamp" if rw.range.get("clamp") else " reject")))
    elif rw.kind == "Enum":
        bits.append("tokens=%s" % (rw.tokens or "none"))
    return " ".join(bits)


def _umbrella_note(st, rows):
    """Factual remark about one umbrella site; never a replacement."""
    if st["role"] == "consumer":
        return "asks a wrapper for the umbrella answer; changes with the wrapper"
    row = rows.get(st["gate"])
    if st["helper"] == "envTriState" and row is not None:
        if re.search(r"envTriState\(\"%s\"\)\s*==\s*1\b" % re.escape(st["gate"]), st["code"]) and row.rule == "Exact1":
            return "`== 1` test on an Exact1 row: same truth table as get<> on that row"
        return "unset/0/1 distinction is used: needs state<>()"
    if st["helper"] == "subGate":
        return "explicit 0/1 else the umbrella: needs the Tri row and state<>()"
    return ""


def render_family_sections(results, rows, ledger=None, umbrella=None, writes=None):
    """Markdown lines for the UNHANDLED breakdown; empty when the run has nothing of the kinds below."""
    helpers = helper_groups(results)
    others = reason_groups(results)
    kind_edits = ledger_kind_edits(results, rows, ledger)
    umb = umbrella or {"sites": [], "wrappers": [], "vk_family": []}
    hazard = [r for r in results if r.shape == "per-call" and r.gate in (writes or {})]
    notes = ledger_notes(results, ledger)
    if not (helpers or umb["sites"] or kind_edits or hazard or notes or any(r.paired for r in results)):
        return []
    L = []
    umb_results = [r for r in results if r.verdict == "UNHANDLED" and is_umbrella_result(r)]
    L.append("## UNHANDLED sites, grouped for the migration builder")
    L.append("")
    L.append("Every UNHANDLED site above falls in exactly one group below. Nothing here is guessed: a helper-call reader is "
             "listed with the helper's own body so the builder can write a helper-aware rewrite and extend the tool.")
    L.append("")
    L.append("| group | sites |")
    L.append("|---|---|")
    for h in sorted(helpers):
        L.append("| helper-call reader `%s()` | %d |" % (h, len(helpers[h])))
    L.append("| VK umbrella resolver (G4, see the umbrella section) | %d |" % len(umb_results))
    for rc in sorted(others):
        L.append("| %s | %d |" % (_cell(rc), len(others[rc])))
    L.append("")

    if helpers:
        L.append("## Helper-call readers, by helper (%d sites)" % sum(len(v) for v in helpers.values()))
        L.append("")
        for h in sorted(helpers, key=lambda k: (-len(helpers[k]), k)):
            rs = helpers[h]
            L.append("### `%s()` -- %d site%s" % (h, len(rs), "" if len(rs) == 1 else "s"))
            L.append("")
            L.append("Helper: %s" % _cell(rs[0].helper_def or "(unknown)"))
            L.append("")
            L.append("| file:line | gate | row now | code line | interpreter reading (evidence, not a rewrite) |")
            L.append("|---|---|---|---|---|")
            for r in rs:
                L.append("| `%s` | `%s` | %s | `%s` | %s |" % (r.where, r.gate, _cell(row_now(rows.get(r.gate))),
                                                            _cell(r.old or _code_line_hint(r)), _cell(r.helper_read)))
            L.append("")

    if others:
        L.append("## Other UNHANDLED sites, by reason (%d sites)" % sum(len(v) for v in others.values()))
        L.append("")
        for rc in sorted(others, key=lambda k: (-len(others[k]), k)):
            rs = others[rc]
            L.append("### %s -- %d site%s" % (_cell(rc), len(rs), "" if len(rs) == 1 else "s"))
            L.append("")
            L.append("| file:line | gate | row now | code line | detail |")
            L.append("|---|---|---|---|---|")
            for r in rs:
                L.append("| `%s` | `%s` | %s | `%s` | %s |" % (r.where, r.gate, _cell(row_now(rows.get(r.gate))),
                                                              _cell(r.old or _code_line_hint(r)), _cell(r.reason or r.why)))
            L.append("")

    if kind_edits:
        L.append("## Row edits the G0 ledger asks for (kind or token set)")
        L.append("")
        L.append("The ledger says the legitimate values are a closed set, but the committed row is not an Enum. These are row edits "
                 "(`Gates/gate_table.cpp`), made before the site migrates; the site stays UNHANDLED because its reader is a "
                 "multi-statement function the planner will not rewrite.")
        L.append("")
        L.append("| gate | row now | edit | ledger | inputs the legacy reader accepts that a whole-token Enum would not |")
        L.append("|---|---|---|---|---|")
        for g, e in kind_edits.items():
            toks = "|".join(e["tokens"])
            L.append("| `%s` | %s | kind %s -> Enum, tokens `%s`, default `%s` | %s | %s |" % (
                g, _cell(row_now(e["row"])), e["row"].kind, toks, e["row"].default,
                _cell(e["rule"] + " (" + e["evidence"] + ")"),
                ", ".join("`%s`" % x for x in e["edge_inputs"]) or "none"))
        L.append("")
        L.append("Those inputs (`atoi` accepts a prefix, a leading blank and a sign) change result under an Enum row, which matches whole "
                 "tokens only (case-insensitively, see `enumIndex` in `Gates/gate.cpp`): an EDGE, to be listed in the PR. An "
                 "unmatched value keeps the default token, which is what the legacy reader returns for it (1).")
        L.append("")
        for g, e in kind_edits.items():
            if e["tokens"]:
                L.append("Indicative site rewrite for `%s` (hand-written from the `is<>` contract in `Gates/gate.h`, not produced or "
                         "verified by this tool):" % g)
                L.append("")
                L.append("```cpp")
                for t in e["tokens"][1:]:
                    L.append("if (%s::is<\"%s\", \"%s\">()) return %s;" % (NS, g, t, t))
                L.append("return %s;" % e["tokens"][0])
                L.append("```")
                L.append("")

    if notes:
        L.append("## G0 ledger entries that touch this plan")
        L.append("")
        L.append("| gate | status | ruling | delta | sites in this plan |")
        L.append("|---|---|---|---|---|")
        for g, e, rs in notes:
            L.append("| `%s` | %s | %s | %s | %s |" % (
                g, e.get("status", ""), _cell(e.get("rule", "")), _cell(e.get("delta", "")),
                ", ".join("`%s` (%s)" % (r.where, r.verdict) for r in rs)))
        L.append("")
        pend = [g for g, e, _ in notes if e.get("status") in ("divergent", "enum")]
        if pend:
            L.append("Owner decision pending for: " + ", ".join("`%s`" % g for g in pend) +
                     ". Do not migrate those sites until the ledger entry is resolved.")
            L.append("")

    if hazard:
        L.append("## Caching hazard: per-call readers of gates that are written at run time")
        L.append("")
        L.append("A per-call `getenv` sees a later `_putenv`; a typed `get<>()` caches its first answer. These gates are written into "
                 "the environment by the sources listed, so the site migrates only if the row is `reread` or the write happens before "
                 "the first read (the writer must also call `gate::invalidate`). Sites whose shape is shown as per-call because the "
                 "tool could not analyse them (UNHANDLED) are listed too.")
        L.append("")
        L.append("| file:line | gate | verdict | written by |")
        L.append("|---|---|---|---|")
        for r in hazard:
            L.append("| `%s` | `%s` | %s | %s |" % (r.where, r.gate, r.verdict,
                                                  ", ".join("`%s`" % w for w in (writes or {})[r.gate][:4])))
        L.append("")

    if writes is not None and not hazard:
        per_call = sorted({r.gate for r in results if r.shape == "per-call"})
        L.append("## Caching check: per-call readers versus run-time writers")
        L.append("")
        L.append("A typed `get<>()` caches its first answer, a per-call `getenv` does not. None of the %d gates read per call in this plan "
                 "is written into the environment by a literal `_putenv`, `putenv`, `setenv` or `SetEnvironmentVariable` in the tracked C++ "
                 "sources (%d other gates are), so no per-call site needs a `reread` row. A write through a computed name is not seen "
                 "by this check." % (len(per_call), len(writes)))
        L.append("")

    if umb["sites"] or umb_results:
        L.append("## VK umbrella-coupled sites (G4: Tri / state<> semantics; NOT migrated in G3)")
        L.append("")
        L.append("These read a gate through the umbrella resolver in `GameOS/gameos/vk_shared_gbuffer_gate.h`. `subGate(name)` is "
                 "`explicit env, else the umbrella (MC2_VK_SHARED_GBUFFER)`; `directChainGate(name)` is the same with the direct-chain "
                 "default; `terrainNativeFirstOwnerOn()` is the umbrella-derived owner test; `envTriState(name)` is the raw "
                 "unset/0/1 read. A plain `get<>()` loses the unset case, which is what the umbrella keys on, so every row here "
                 "needs a Tri row and `state<>()`. They are UNHANDLED on purpose.")
        L.append("")
        L.append("Sites that call `directChainGate` or `terrainNativeFirstOwnerOn`: %s." % (
            ", ".join("`%s:%d`" % (st["file"], st["line"]) for st in umb["sites"]
                      if st["helper"] in ("directChainGate", "terrainNativeFirstOwnerOn")) or "none in the scanned files"))
        L.append("`terrainNativeFirstOwnerOn()` mirrors `terrainTier1Requested()` (`vulkan_terrain_island.cpp`); the header says to keep "
                 "them in lockstep, so G4 moves both together.")
        L.append("")
        L.append("| file:line | helper | role | gate | row now | in function | code line | note |")
        L.append("|---|---|---|---|---|---|---|---|")
        for st in umb["sites"]:
            note = _umbrella_note(st, rows)
            L.append("| `%s:%d` | `%s` | %s | %s | %s | `%s` | `%s` | %s |" % (
                st["file"], st["line"], st["helper"], st["role"], ("`%s`" % st["gate"]) if st["gate"] else "(no literal)",
                _cell(row_now(rows.get(st["gate"]))) if st["gate"] else "-", st["func"], _cell(st["code"]), _cell(note)))
        L.append("")
        wr = [w for w in umb["wrappers"] if w["callers"] > 0]
        if wr:
            L.append("Functions that hold an umbrella read and are called from elsewhere in the same file (the umbrella answer reaches "
                     "every caller, so none of those callers may be migrated independently of the Tri row):")
            L.append("")
            L.append("| function | defined | umbrella calls inside | in-file callers |")
            L.append("|---|---|---|---|")
            for w in wr:
                L.append("| `%s` | `%s:%d` | %s | %d |" % (w["func"], w["file"], w["line"],
                                                           _cell(", ".join(sorted(set(w["helpers"])))), w["callers"]))
            L.append("")
        vkf = umb["vk_family"]
        if vkf:
            L.append("Other `MC2_VK_*` readers in the same files. They do not call the resolver, so G3 may migrate them when the "
                     "verdict says so; the `near` column marks the ones inside 12 lines of an umbrella call, which the builder "
                     "should read in context first.")
            L.append("")
            L.append("| file:line | gate | verdict | near umbrella call at | detail |")
            L.append("|---|---|---|---|---|")
            for r, near in vkf:
                L.append("| `%s` | `%s` | %s | %s | %s |" % (r.where, r.gate, r.verdict, ("line %d" % near) if near else "-",
                                                          _cell((r.reason or r.why)[:200])))
            L.append("")
    return L

def main(argv=None):
    ap = argparse.ArgumentParser(description="DRY-RUN planner for the gate migration slices (writes no C++).")
    ap.add_argument("--family", choices=sorted(FAMILIES), default="g1b",
                    help="which slice to plan: sets the default files, the title and the --write targets (default g1b)")
    ap.add_argument("--files", nargs="*", help="repo-relative files (default: the family's files)")
    ap.add_argument("--rev", default="HEAD", help="git revision to read C++ and tables from (default HEAD)")
    ap.add_argument("--worktree", action="store_true", help="read the working tree instead of git")
    ap.add_argument("--write", action="store_true", help="write the family's .md and .tsv under docs/ (g1b: %s, %s)" % (OUT_MD, OUT_TSV))
    ap.add_argument("--summary", action="store_true", help="print only the summary counts")
    ap.add_argument("--apply", action="store_true",
                    help="rewrite the NO-CHANGE / EDGE sites of --files in the working tree (needs --i-mean-it)")
    ap.add_argument("--i-mean-it", action="store_true")
    ap.add_argument("--gate-prefix", default="",
                    help="only plan / rewrite the sites that read a gate whose name starts with this prefix "
                         "(the g4 family uses MC2_VK_ so a file shared with another slice keeps its other reads)")
    a = ap.parse_args(argv)
    if a.family == "g4" and not a.gate_prefix:
        a.gate_prefix = "MC2_VK_"
    if a.apply:
        if not a.i_mean_it:
            print("gate_codemod: --apply refused: pass --i-mean-it as well", file=sys.stderr)
            return 2
        if not a.files:
            print("gate_codemod: --apply needs --files (one file per commit)", file=sys.stderr)
            return 2
        # Reads and rewrites the WORKING TREE files; rows come from the committed table at HEAD.
        rows = load_rows(ROOT, "HEAD")
        global _CURRENT_ROWS
        _CURRENT_ROWS = rows
        ledger = load_ledger(ROOT)
        total = 0
        for rel in a.files:
            path = os.path.join(ROOT, rel)
            with open(path, "r", encoding="utf-8", newline="") as fh:
                raw = fh.read()
            results = analyze_text(rel, raw, rows, ledger, [])
            if a.gate_prefix:
                results = [r for r in results if r.gate.startswith(a.gate_prefix)]
            new, applied, skipped = apply_edits(raw, results)
            with open(path, "w", encoding="utf-8", newline="") as fh:
                fh.write(new)
            left = [r for r in results if r not in applied]
            print("gate_codemod: %s: rewrote %d site(s), skipped %d overlapping, %d left (UNHANDLED %d)" % (
                rel, len(applied), len(skipped), len(left), sum(1 for r in left if r.verdict == "UNHANDLED")))
            for r in applied:
                if r.verdict == "EDGE":
                    print("  EDGE  %s:%d %s: %s" % (rel, r.line, r.gate, r.why))
            total += len(applied)
        print("gate_codemod: %d site(s) rewritten" % total)
        return 0
    fam = FAMILIES[a.family]
    files = a.files or list(fam["files"])
    rev = None if a.worktree else a.rev
    results, rows = run(files, ROOT, rev)
    if a.gate_prefix:
        results = [r for r in results if r.gate.startswith(a.gate_prefix)]
    unhandled_code_lines(results, ROOT, rev)
    ledger = load_ledger(ROOT)
    umbrella = umbrella_report(files, ROOT, rev, results)
    writes = runtime_writes(ROOT, rev)
    s = summarize(results)
    if a.write:
        label = git_head(ROOT) if rev == "HEAD" else (a.rev if rev else "working tree")
        with open(os.path.join(ROOT, fam["md"]), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(render_md(results, rows, files, label, a.family, ledger, umbrella, writes))
        with open(os.path.join(ROOT, fam["tsv"]), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(tsv_text(results))
        print("gate_codemod: wrote %s and %s (%d sites)" % (fam["md"], fam["tsv"], s["total"]))
    elif not a.summary:
        sys.stdout.write(tsv_text(results))
    print("gate_codemod: %d sites: %s; overrides proposed %d; gates needing a row edit %d" % (
        s["total"], ", ".join("%s %d" % (v, s["verdicts"][v]) for v in VERDICTS), s["overrides"],
        len(s["row_edits"])), file=sys.stderr if not a.summary else sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
