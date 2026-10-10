#!/usr/bin/env python3
"""
MCP-ANTI-CHURN-1: gate_status.py
One-verdict MC2_* gate audit: merges FOUR sources that today live in four
different places and drifted independently (2 observed ENV-DROP incidents +
11 unregistered vars on 2026-07-01):

  1. Gates/gate_table.json registry rows         (env_index registry read)
  2. docs/tier1_env_vars.md                      (env_index tier1 parse)
  3. scripts/run_smoke.py passthrough allowlist  (anchors `if k in (` .. `)},`
                                                  — same extraction as
                                                  scripts/check-env-allowlist.sh)
  4. scripts/check-env-registry.sh ALLOWLIST=( ) (legacy/trace var allowlist)

plus a live `git grep` for code readers (fast single-var path; env_index's
full source walk is only used as fallback).

Verdict flags map 1:1 to the failure classes:
  REGISTRY_UNPARSED — the gate table could not be read COMPLETELY, so
                    `registered` is the string "UNKNOWN" and the verdict is
                    "UNDETERMINED". This is a THIRD state, added by
                    REGISTRY-PARSER-TRUTH-1 (2026-07-31) because the tool used
                    to collapse it into `registered: false, verdict: "OK",
                    flags: []` -- a confident wrong answer about a registered
                    gate. Never record an UNREGISTERED/dead-gate finding off it.
  ENV_UNDRIVEABLE — the smoke runner really does NOT deliver this var's ambient
                    value to the engine (runner.FORCED_ENV / POPPED_ENV)
                    -> a shell-set value is discarded; an A/B arm never ran
  ENV_DROP_RISK   — read by engine but NOT in the run_smoke passthrough tuple
                    AND the runner has stopped inheriting os.environ, so the
                    tuple is load-bearing again (see below). Conditional.
  UNREGISTERED    — no registry(...) row in the gate table AND not in the
                    check-env-registry.sh ALLOWLIST -> CI gate fails
  UNDOCUMENTED    — no docs/tier1_env_vars.md entry
  GHOST           — registered/documented but no code reader found

ENV_DROP_RISK IS CONDITIONAL, AND THAT CORRECTION IS THE POINT (ENV-DROP-CLOSE-1)
--------------------------------------------------------------------------------
This tool used to raise ENV_DROP_RISK on *any* engine-read gate missing from the
run_smoke.py `if k in ( ... )` tuple, and told the caller its "gate-ON smoke may
silently run the gate-OFF path". That was wrong for every one of the ~500 gates
it fired on, because `scripts/smoke_lib/runner.py` builds the child env as

    env = os.environ.copy()  ->  FORCED_ENV  ->  SOFT_ENV(setdefault)
                             ->  POPPED_ENV(pop)  ->  cfg.env_extra

so an ambient MC2_* reaches mc2.exe whether or not the tuple names it. The
tuple's os.environ-copying rows are NO-OPS. HARNESS-HYGIENE-3 established this
(scripts/check-env-drive-guard.py) and GATE-HYGIENE-CLOSE-1 made it a contract
(scripts/check-smoke-env-inheritance.py), which prints in so many words: "Do NOT
file this gap as 'voided A/B verifications'."

The cost of the false flag is not cosmetic and it is not symmetrical with a
missed one: it invites a reader to THROW AWAY VALID EVIDENCE. Sessions have been
spent re-running A/Bs that were fine, and a real, correctly-declared A/B on
MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE was written off as void on this tool's word.

So the flag is now computed from READER SOURCE rather than assumed: the runner's
inheritance line and its three undriveable tables are parsed out of
scripts/smoke_lib/runner.py. If someone ever changes the runner back to a
from-scratch env dict, `smoke_env_inherited` goes False, the tuple becomes
load-bearing, and ENV_DROP_RISK starts firing again — correctly, that time.
"""
from __future__ import annotations

import re
import subprocess
from pathlib import Path
from typing import Optional

import env_index as ei
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
# OWNER RULE: no window for any child process (hidden_kwargs: CREATE_NO_WINDOW + SW_HIDE).
from smoke_lib import nofocus  # noqa: E402

_MC2_NAME = re.compile(r"^MC2_[A-Z0-9_]+$")
_QUOTED_MC2 = re.compile(r'"(MC2_[A-Z0-9_]+)"')
_BARE_MC2 = re.compile(r"^\s*(MC2_[A-Z0-9_]+)\b")

_READER_DIRS = ("mclib", "GameOS", "code", "RenderCore", "RenderWorld",
                "GameAdapters", "editor", "gui")


# ---------------------------------------------------------------------------
# Source 3: run_smoke.py passthrough allowlist
# ---------------------------------------------------------------------------

def parse_smoke_allowlist(repo_root: Path) -> tuple[set, bool]:
    """Quoted MC2_* names inside every `if k in (` .. `)},` block of
    scripts/run_smoke.py. Returns (names, anchors_found)."""
    path = Path(repo_root) / "scripts" / "run_smoke.py"
    names: set = set()
    found = False
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return names, False
    in_block = False
    for line in text.splitlines():
        if not in_block and "if k in (" in line:
            in_block = True
            found = True
        if in_block:
            names.update(_QUOTED_MC2.findall(line))
            if ")}," in line:
                in_block = False
    return names, found


# ---------------------------------------------------------------------------
# Source 3b: smoke_lib/runner.py env-composition policy (the REAL drop mechanism)
# ---------------------------------------------------------------------------

#: Matches the runner's child-env base. Deliberately the SAME pattern
#: scripts/check-smoke-env-inheritance.py enforces as a CI contract, so the two
#: cannot drift into disagreeing about whether the harness inherits.
_INHERIT_RE = re.compile(r"^\s*env\s*=\s*os\.environ\.copy\(\)", re.M)


def _table_names(text: str, header: str) -> tuple[set, bool]:
    """Quoted MC2_* names in a `HEADER ... {` block up to its closing brace."""
    names: set = set()
    i = text.find(header)
    if i == -1:
        return names, False
    for line in text[i:].splitlines()[1:]:
        s = line.strip()
        if s.startswith("}"):
            break
        names.update(_QUOTED_MC2.findall(line))
    return names, True


def parse_runner_env_policy(repo_root: Path) -> dict:
    """What scripts/smoke_lib/runner.py ACTUALLY does to the child environment.

    Returns {inherits, forced, soft, popped, anchors_ok}. `anchors_ok=False`
    means the file could not be read or the tables could not be located — the
    caller must treat the result as unknown rather than as "nothing is dropped".
    """
    path = Path(repo_root) / "scripts" / "smoke_lib" / "runner.py"
    out = {"inherits": False, "forced": set(), "soft": set(),
           "popped": set(), "anchors_ok": False}
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return out
    forced, f_ok = _table_names(text, "FORCED_ENV: dict = {")
    soft, s_ok = _table_names(text, "SOFT_ENV: dict = {")
    popped, p_ok = _table_names(text, "POPPED_ENV: frozenset = frozenset({")
    out.update({
        "inherits": bool(_INHERIT_RE.search(text)),
        "forced": forced, "soft": soft, "popped": popped,
        "anchors_ok": f_ok and s_ok and p_ok,
    })
    return out


# ---------------------------------------------------------------------------
# Source 4: check-env-registry.sh ALLOWLIST
# ---------------------------------------------------------------------------

def parse_registry_check_allowlist(repo_root: Path) -> tuple[set, bool]:
    """MC2_* names inside the ALLOWLIST=( .. ) block of check-env-registry.sh."""
    path = Path(repo_root) / "scripts" / "check-env-registry.sh"
    names: set = set()
    found = False
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return names, False
    in_block = False
    for line in text.splitlines():
        if not in_block and line.strip().startswith("ALLOWLIST=("):
            in_block = True
            found = True
            continue
        if in_block:
            if line.strip() == ")":
                break
            m = _BARE_MC2.match(line)
            if m:
                names.add(m.group(1))
    return names, found


# ---------------------------------------------------------------------------
# Code readers (fast single-var git grep)
# ---------------------------------------------------------------------------

def _grep_readers(repo_root: Path, var: str, cap: int = 20) -> list[dict]:
    try:
        r = subprocess.run(
            ["git", "-C", str(repo_root), "grep", "-n", "--fixed-strings",
             f'"{var}"', "--", *_READER_DIRS],
            capture_output=True, text=True, timeout=15,
            stdin=subprocess.DEVNULL,
            **nofocus.hidden_kwargs()
        )
    except Exception:
        return []
    readers = []
    for line in r.stdout.splitlines():
        parts = line.split(":", 2)
        if len(parts) == 3:
            readers.append({"file": parts[0].replace("\\", "/"),
                            "line": int(parts[1]) if parts[1].isdigit() else 0,
                            "snippet": parts[2].strip()[:160]})
        if len(readers) >= cap:
            break
    return readers


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def gate_status(repo_root: Path, var: str) -> dict:
    """Merged registered/allowlisted/documented/default verdict for one gate."""
    var = (var or "").strip()
    if not _MC2_NAME.match(var):
        return {"error": f"invalid gate name {var!r} (expected MC2_[A-Z0-9_]+)"}
    repo_root = Path(repo_root)

    # REGISTRY-PARSER-TRUTH-1: "the registry did not parse" must NEVER be
    # rendered as "this gate is not registered".  Until 2026-07-31 this tool
    # answered `registered: false, registry_entry: null, verdict: "OK",
    # flags: []` about MC2_LIGHTING_DEBUG_VIEW -- a registered gate -- while its
    # own reader list cited the registry line the parser had choked on.  That is
    # the worst output shape available: a confident wrong answer with nothing to
    # warn the reader.  Now the refusal propagates as a THIRD state, and the
    # verdict cannot read "OK".
    registry_error = None
    try:
        registry = ei._parse_registry(repo_root)
    except ei.RegistryParseError as exc:
        registry = None
        registry_error = str(exc)

    tier1 = ei._parse_tier1_docs(repo_root)
    smoke_allow, smoke_anchor_ok = parse_smoke_allowlist(repo_root)
    check_allow, check_anchor_ok = parse_registry_check_allowlist(repo_root)
    runner_env = parse_runner_env_policy(repo_root)
    readers = _grep_readers(repo_root, var)

    reg = registry.get(var) if registry is not None else None
    t1 = tier1.get(var)
    # Tri-state. The string "UNKNOWN" is deliberately neither True nor False:
    # a JSON null would be read as falsy by the next tool down the chain, which
    # is exactly the collapse this is here to prevent.
    in_registry = (reg is not None) if registry is not None else "UNKNOWN"
    in_tier1 = t1 is not None
    in_smoke_allow = var in smoke_allow
    in_check_allow = var in check_allow
    has_readers = bool(readers)

    default = None
    if reg and reg.get("default"):
        default = str(reg["default"]).upper()
    elif t1 and t1.get("default"):
        default = str(t1["default"]).upper()

    inherits = runner_env["inherits"]
    in_forced = var in runner_env["forced"]
    in_popped = var in runner_env["popped"]
    in_soft = var in runner_env["soft"]

    flags: list[str] = []
    if registry_error is not None:
        flags.append(
            "REGISTRY_UNPARSED: Gates/gate_table.json could not "
            "be read completely, so whether this gate is registered is "
            "UNKNOWN -- NOT 'no'. Do not record an UNREGISTERED finding, do not "
            "add it to an allowlist, and do not file it as a dead gate off this "
            "answer. Fix the table or the reader first. Reason: "
            + registry_error)
    # REAL undriveability, independent of the vestigial passthrough tuple.
    if in_popped:
        flags.append("ENV_UNDRIVEABLE: scripts/smoke_lib/runner.py DELETES this "
                     "var from the child env (POPPED_ENV) — a shell-set value "
                     "never reaches mc2.exe under the smoke harness")
    if in_forced:
        flags.append("ENV_UNDRIVEABLE: scripts/smoke_lib/runner.py HARD-FORCES "
                     "this var (FORCED_ENV) — a conflicting shell value is "
                     "discarded, so an A/B arm that sets it never runs")
    # Conditional: only a real drop once the runner stops inheriting os.environ.
    if has_readers and not in_smoke_allow and not inherits:
        flags.append("ENV_DROP_RISK: read by code, NOT in the run_smoke.py "
                     "passthrough tuple, and the runner no longer inherits "
                     "os.environ — the tuple is load-bearing, so a gate-ON "
                     "smoke silently runs the gate-OFF path")
    # Every flag below is a claim ABOUT THE REGISTRY. None of them may be made
    # when the registry did not parse: an unparsed registry is not an empty one,
    # and "UNREGISTERED" / "GHOST" / "no trace anywhere" asserted off a failed
    # read are precisely the confident wrong answers this lane exists to remove.
    reg_known = registry is not None
    if reg_known:
        if has_readers and not in_registry and not in_check_allow:
            flags.append("UNREGISTERED: no registry(...) row in the gate table and not in "
                         "check-env-registry.sh ALLOWLIST — CI env-registry gate fails")
    if has_readers and not in_tier1:
        flags.append("UNDOCUMENTED: no docs/tier1_env_vars.md entry")
    if reg_known and not has_readers:
        if in_registry or in_tier1 or in_smoke_allow or in_check_allow:
            flags.append("GHOST: registered/documented/allowlisted but no code reader "
                         "found (removed feature? doc litter?)")
        else:
            flags.append("UNKNOWN: no trace of this var anywhere — typo?")

    parse_notes = []
    if not smoke_anchor_ok:
        parse_notes.append("run_smoke.py allowlist anchors not found — "
                           "smoke_allowlist result unreliable")
    if not check_anchor_ok:
        parse_notes.append("check-env-registry.sh ALLOWLIST block not found — "
                           "registry_check_allowlist result unreliable")
    if not runner_env["anchors_ok"]:
        parse_notes.append("smoke_lib/runner.py FORCED_ENV/SOFT_ENV/POPPED_ENV "
                           "tables not found — ENV_UNDRIVEABLE result unreliable")

    # Advisory, NOT a flag: missing from the tuple is a convention gap, not a
    # drop, while the runner inherits. Stated positively so nobody re-derives
    # "void A/B" from a bare boolean. See scripts/check-smoke-env-inheritance.py.
    if has_readers and not in_smoke_allow and inherits:
        parse_notes.append(
            "not in the run_smoke.py passthrough tuple — convention gap ONLY: "
            "the runner inherits os.environ (smoke_lib/runner.py), so the "
            "ambient value reaches mc2.exe and both A/B arms are real. Do NOT "
            "treat this as a voided verification.")

    # Three verdicts, not two. "GAPS" means "I looked and found problems";
    # UNDETERMINED means "I could not look", and collapsing the two into OK/GAPS
    # is how a parse failure became a clean bill of health.
    if registry_error is not None:
        verdict = "UNDETERMINED"
    elif flags:
        verdict = "GAPS"
    else:
        verdict = "OK"

    return {
        "var": var,
        "verdict": verdict,
        "flags": flags,
        "default": default,
        "default_source": ("registry" if (reg and reg.get("default"))
                           else "tier1_doc" if (t1 and t1.get("default"))
                           else None),
        "registered": in_registry,
        "registry_parse_ok": registry is not None,
        "registry_parse_error": registry_error,
        "registry_entry": ({"feature_id": reg.get("feature_id"),
                            "kind": reg.get("kind"),
                            "default": reg.get("default"),
                            "doc": (reg.get("doc") or "")[:200]} if reg else None),
        "tier1_documented": in_tier1,
        "tier1_line": (t1.get("raw_line", "")[:200] if t1 else None),
        "smoke_allowlisted": in_smoke_allow,
        # The env-composition facts, read out of smoke_lib/runner.py rather than
        # assumed. `smoke_env_inherited` is what makes `smoke_allowlisted` a
        # convention fact instead of a delivery fact.
        "smoke_env_inherited": inherits,
        "smoke_env_forced": in_forced,
        "smoke_env_popped": in_popped,
        "smoke_env_soft_default": in_soft,
        "registry_check_allowlisted": in_check_allow,
        "reader_count": len(readers),
        "readers": readers,
        "parse_notes": parse_notes,
        "confidence": "lexical",
    }
