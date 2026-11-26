#!/usr/bin/env python3
"""Static ABL compatibility check of warrior brains against the Smarter (Magic corebrain 1.45) profile.

    py -3 tools/abl_compat_check.py <mission.fst | data/missions tree> [--as-shipped] [--json out.json]
    py -3 tools/abl_compat_check.py --mods-root <install>/mods --json mods.json

(a) Compile compatibility: after FixABL (deploy_payload.magic_brain_145) every call to a core order must match the
    arity of the Magic library, and no constant or core order the replaced corebrain defined may be gone.
    --as-shipped skips FixABL: the deployment never migrates loose mod brains.
(b) Per mission, the semantics risk classes of the study smarter-ai-campaigns: ignore->engage (also when the brain
    is an objective unit or an ally), contact-state trigger, powered-down ambush, escort, hold->mobile. A class is
    reported only when the migrated call no longer carries the argument stock scripted on.
Exit status 1 when a brain a campaign mission uses fails (a).
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts"))
from deploy_payload import magic_brain_145  # noqa: E402
from tools.mc2x_import import fst  # noqa: E402

MAGIC_LIBRARY = ROOT / "mods/magic-brain-1.45/data/missions/corebrain.abx"
EXT = (".abl", ".abi", ".abx", ".fit")
COREBRAIN = "data/missions/corebrain.abx"
CAMPAIGN = re.compile(r"/(mc2|tut)_\d\d\.fit$")
COMMENT_RE = re.compile(r'//[^\r\n]*|/\*.*?\*/|"[^"\r\n]*"', re.S)
CALL_RE = re.compile(r'//[^\r\n]*|/\*.*?\*/|"[^"\r\n]*"|\b(\w+)\s*\(', re.S)
DECL_RE = re.compile(r"^\s*(?:order|function)\s+(\w+)\s*\(([^)]*)\)", re.I | re.M)
CONST_RE = re.compile(r"\b([A-Z][A-Z0-9_]*)\s*=\s*-?[\d.]+\s*;")
ETERNAL_RE = re.compile(r"\beternal\s+\w+(?:\[[^\]]*\])?\s+(\w+)", re.I)
INCLUDE_RE = re.compile(r'#include_\s*"([^"]+)"', re.I)
HANDLE_RE = re.compile(r'\b(\w+)\s*=\s*getStateHandle\s*\(\s*"(\w+)"\s*\)', re.I)
STATE_RE = re.compile(r"^\s*state\s+(\w+)\s*;(.*?)^\s*endstate\s*;", re.I | re.S | re.M)
CONTACT_ORDERS = {"corepatrol": 2, "corepatroldelayed": 3, "coreguard": 2, "corewait": 2}
HOLD = ("tactic_stop_and_attack", "tactic_fire_from_here", "tacorder_param_stop_and_fire",
        "tacorder_param_turret", "tactic_no_pursuit")


def identity(text):
    return text


def fixabl(text):
    return magic_brain_145(text.encode("latin1")).decode("latin1")


def read(path):
    return Path(path).read_bytes().decode("latin1")


def strip_comments(text):
    return COMMENT_RE.sub(lambda m: m.group(0) if m.group(0)[0] == '"' else "", text)


def key_of(name):
    return name.lower().replace("\\", "/")


def arity(params):
    depth, count = 0, 1
    for ch in params.strip():
        if ch in "[(":
            depth += 1
        elif ch in "])":
            depth -= 1
        elif ch == "," and depth == 0:
            count += 1
    return count if params.strip() else 0


def library(text):
    clean = strip_comments(text)
    block = re.search(r"\bconst\b(.*?)\btype\b", clean, re.I | re.S)
    return {"symbols": {m.group(1).lower(): arity(m.group(2)) for m in DECL_RE.finditer(clean)},
            "consts": set(CONST_RE.findall(block.group(1) if block else clean))}


def call_args(text, start):
    depth, args, begin = 1, [], start
    for pos in range(start, len(text)):
        ch = text[pos]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append(text[begin:pos].strip())
                return args
        elif ch == "," and depth == 1:
            args.append(text[begin:pos].strip())
            begin = pos + 1
    return args


def calls(text):
    """[(name, args, offset, line)] of every identifier call outside comments and strings."""
    return [(m.group(1).lower(), call_args(text, m.end()), m.start(), text.count("\n", 0, m.start()) + 1)
            for m in CALL_RE.finditer(text) if m.group(1)]


def load_tree(path):
    """{key: text} of the ABL and fit files under data/missions of an FST or of a directory tree."""
    path = Path(path)
    if path.is_file():
        return {key_of(name): fst.decompress_entry(off, comp, real, is_lz, data).decode("latin1")
                for off, comp, real, name, is_lz, data in fst.fst_entries(str(path))
                if key_of(name).startswith("data/missions/") and key_of(name).endswith(EXT)}
    base = next((d for d in (path, path / "data/missions") if d.is_dir() and (
        any(d.glob("*.fit")) or any(d.glob("*.abx")) or (d / "warriors").is_dir())), None)
    if base is None:
        raise SystemExit(f"abl_compat_check: {path} is neither a mission.fst nor a data/missions tree")
    return {"data/missions/" + p.relative_to(base).as_posix().lower(): read(p)
            for p in base.rglob("*") if p.is_file() and p.suffix.lower() in EXT}


def include_key(tree, includer, name):
    name = key_of(name)
    folders = (includer.rsplit("/", 1)[0], "data/missions/warriors", "data/missions")
    return next((f"{folder}/{name}" for folder in folders if f"{folder}/{name}" in tree), None)


def closure(tree, key, seen=None):
    seen = [] if seen is None else seen
    if key not in seen and key in tree:
        seen.append(key)
        for m in INCLUDE_RE.finditer(tree[key]):
            closure(tree, include_key(tree, key, m.group(1)), seen)
    return seen


def expand(tree, key, migrate, seen=None):
    seen = set() if seen is None else seen
    if key in seen or key not in tree:
        return ""
    seen.add(key)
    return INCLUDE_RE.sub(lambda m: expand(tree, include_key(tree, key, m.group(1)), migrate, seen),
                          migrate(tree[key]))


def brain_key(tree, name):
    name = key_of(name)
    for key in (f"data/missions/warriors/{name}.abl", f"data/missions/{name}", f"data/missions/{name}.abl"):
        if key in tree:
            return key
    return None


def compile_errors(tree, files, migrate, magic, removed_symbols, removed_consts):
    errors = []
    for key in files:
        try:
            text = migrate(tree[key])
        except ValueError as exc:
            errors.append({"file": key, "kind": "fixabl", "call": str(exc)})
            continue
        for name, args, _, line in calls(text):
            if name in magic["symbols"] and len(args) != magic["symbols"][name]:
                errors.append({"file": key, "line": line, "kind": "arity",
                               "call": f"{name}({', '.join(args)})", "magic_arity": magic["symbols"][name]})
            elif name in removed_symbols:
                errors.append({"file": key, "line": line, "kind": "symbol-removed", "call": name})
        for token in sorted(set(re.findall(r"\b[A-Z][A-Z0-9_]{2,}\b", strip_comments(text))) & removed_consts):
            errors.append({"file": key, "kind": "constant-removed", "call": token})
    return errors


def analyse(tree, key, migrate, magic, names, eternals):
    """Stock semantics Magic drops: the contactState argument of a core order, escort geometry, an ambush wake."""
    try:
        text = strip_comments(expand(tree, key, identity))
        moved = strip_comments(expand(tree, key, migrate))
    except ValueError as exc:
        return {"error": str(exc)}
    handles = {h.lower(): s.lower() for h, s in HANDLE_RE.findall(text)}
    neg = {m.group(1).lower() for m in re.finditer(r"\b(\w+)\s*=\s*-\s*1\s*;", text)}
    stock_calls = [c for c in calls(text) if c[0] in names]
    moved_calls = [c for c in calls(moved) if c[0] in names]
    paired = len(stock_calls) == len(moved_calls)
    honoured = [paired and m[0] in magic["symbols"] and len(m[1]) == magic["symbols"][m[0]] and len(m[1]) >= len(s[1])
                for s, m in zip(stock_calls, moved_calls)] if paired else [False] * len(stock_calls)
    flags = {}
    contact_states = set()

    def flag(name):
        flags[name] = flags.get(name, 0) + 1

    for (name, args, _, _), keep in zip(stock_calls, honoured):
        index = CONTACT_ORDERS.get(name)
        cs = args[index] if index is not None and len(args) > index else ("0" if name in ("patrol", "patroldelayed") else None)
        if cs is not None and not keep:
            c = cs.replace(" ", "").lower()
            crit = args[1].replace(" ", "").lower() if name in ("coreguard", "corewait") and len(args) > 1 else None
            if crit in ("0", "contact_criteria_none"):
                flag("cs_no_scan")
            elif c == "-1" or c in neg:
                flag("cs_ignore_contacts")
            elif c == "0":
                flag("cs_drop_through")
            elif c in handles:
                flag("cs_state")
                contact_states.add(handles[c])
            else:
                flag("cs_unassigned_handle_is_0")
        elif cs is not None:
            flag("cs_kept")
        if name == "coreescort" and not (keep and len(args) >= 4):
            flag("escort")
        if name == "coreattacktactic":
            flag("attack_tactic")
            if any(h in " ".join(args).lower() for h in HOLD):
                flag("hold_position_tactic")
        if name == "coreattack":
            a = " ".join(args).lower()
            if any(h in a for h in HOLD) or (len(args) > 1 and args[1].lower() == "tacorder_param_none"):
                flag("hold_position_attack")
    bodies = {m.group(1).lower(): (m.start(2), m.end(2)) for m in STATE_RE.finditer(text)}
    entered = {m.group(1).lower() for m in re.finditer(r"\btrans\s+(\w+)", text, re.I)}
    entered |= {handles.get(h.lower(), h.lower()) for h in re.findall(r"\b(?:setState|setPilotState)\s*\(\s*(\w+)\s*\)", text, re.I)}
    triggers, ambush = set(), False
    for state, (begin, end) in bodies.items():
        body = text[begin:end]
        if state in contact_states:
            writes = sorted({m.group(1).lower() for m in re.finditer(r"\b(\w+)\s*=(?!=)", body) if m.group(1).lower() in eternals
                             and not m.group(1).lower().startswith("attackmusictrigger")})
            triggers.update(w + ("(contact-only)" if state not in entered else "") for w in writes)
        if "noweapons" not in state and re.search(r"corePower\s*\(\s*false", body, re.I):
            wake = [i for i, (name, args, offset, _) in enumerate(stock_calls) if begin <= offset < end
                    and len(args) >= 3 and args[-1].strip().lower() in handles]
            ambush = ambush or bool(wake) and not any(honoured[i] for i in wake)
    return {"scanning": bool(re.search(r"setTargetPriority\s*\([^;]*CONTACT_CRITERIA_ENEMY", text, re.I)),
            "flags": flags, "triggers": sorted(triggers), "pd_ambush": ambush}


def parse_fit(text):
    blocks, current = {}, None
    for line in text.splitlines():
        m = re.match(r"^\[(.+)\]\s*$", line)
        if m:
            current = m.group(1)
            blocks[current] = {}
            continue
        m = re.match(r"^\s*\S+\s+(\w+)\s*=\s*(.*)$", line)
        if m and current:
            blocks[current][m.group(1).lower()] = m.group(2).strip().strip('"')
    return blocks


def mission_of(text):
    blocks = parse_fit(text)
    try:
        brains = {int(m.group(1)): v["brain"] for k, v in blocks.items() for m in [re.match(r"Warrior(\d+)$", k)] if m and "brain" in v}
        units = [{"part": int(m.group(1)), "team": v.get("teamid"), "brain": brains.get(int(v["pilot"])),
                  "x": float(v.get("positionx", 0)), "y": float(v.get("positiony", 0))}
                 for k, v in blocks.items() for m in [re.match(r"Part(\d+)$", k)] if m and "pilot" in v]
    except ValueError:
        return [], []
    objectives = [{"species": v.get("conditionspeciesstring") or v.get("failureconditionspeciesstring"),
                   "x": float(v["positionx"]) if "positionx" in v else None,
                   "y": float(v["positiony"]) if "positiony" in v else None, "brains": []}
                  for k, v in blocks.items() if re.match(r"Team0Objective\d+(Failure)?Condition\d+$", k)]
    for o in objectives:
        if o["x"] is not None and o["species"] and "Unit" in o["species"]:
            o["brains"] = [u["brain"].lower() for u in units if u["brain"] and abs(u["x"] - o["x"]) < 2 and abs(u["y"] - o["y"]) < 2]
    return units, objectives


def check(path, shipped=False, stock=None):
    tree = load_tree(path)
    magic = library(read(MAGIC_LIBRARY))
    migrate = identity if shipped else fixabl
    replaced = {"symbols": set(), "consts": set()}
    for source in (tree.get(COREBRAIN), stock and stock.get(COREBRAIN)):
        if source:
            lib = library(source)
            replaced["symbols"] |= set(lib["symbols"])
            replaced["consts"] |= lib["consts"]
    other = {"symbols": set(), "consts": set()}
    for key, text in tree.items():
        if key.endswith((".abx", ".abi")) and key != COREBRAIN:
            lib = library(text)
            other["symbols"] |= set(lib["symbols"])
            other["consts"] |= lib["consts"]
    removed_consts = replaced["consts"] - magic["consts"] - other["consts"]
    removed_symbols = replaced["symbols"] - set(magic["symbols"]) - other["symbols"]
    names = replaced["symbols"] | set(magic["symbols"]) | {"coreattack", "magicattack"}
    eternals = set()
    for key, text in tree.items():
        if key.endswith(".abl") and key.count("/") in (2, 3):
            eternals |= {e.lower() for e in ETERNAL_RE.findall(strip_comments(text))}
    warriors = sorted(k for k in tree if k.startswith("data/missions/warriors/") and k.endswith((".abl", ".abi")))
    errors = {}
    for key in warriors:
        found = compile_errors(tree, closure(tree, key), migrate, magic, removed_symbols, removed_consts)
        if found:
            errors[key] = found
    fits = sorted(k for k in tree if re.fullmatch(r"data/missions/[^/]+\.fit", k))
    missions, brains, used = {}, {}, set()
    for fit in [k for k in fits if CAMPAIGN.search(k)] or fits:
        units, objectives = mission_of(tree[fit])
        classes = {"ignore_engage": [], "ignore_engage_objective_or_ally": [], "escort": [], "pd_ambush": [],
                   "flow_trigger_in_contact_state": set(), "hold_mobile_units": 0}
        failed, missing = [], []
        for brain in dict.fromkeys((u["brain"] or "").lower() for u in units if u["brain"]):
            key = brain_key(tree, brain)
            if key is None:
                missing.append(brain)
                continue
            used.add(key)
            result = analyse(tree, key, migrate, magic, names, eternals)
            errors_here = errors.get(key, []) + ([result["error"]] if "error" in result else [])
            brains[key] = {"flags": result.get("flags", {}), "triggers": result.get("triggers", []), "errors": errors_here}
            if errors_here:
                failed.append(brain)
            count = sum(1 for u in units if (u["brain"] or "").lower() == brain)
            teams = {u["team"] for u in units if (u["brain"] or "").lower() == brain}
            flags = result.get("flags", {})
            tag = f"{brain}x{count}"
            if flags.get("cs_ignore_contacts") and result.get("scanning"):
                classes["ignore_engage"].append(tag)
                if any(brain in o["brains"] for o in objectives) or "0" in teams:
                    classes["ignore_engage_objective_or_ally"].append(
                        tag + ("(ally)" if "0" in teams else "") + ("(objective)" if any(brain in o["brains"] for o in objectives) else ""))
            if flags.get("escort"):
                classes["escort"].append(tag)
            if flags.get("hold_position_attack") or flags.get("hold_position_tactic"):
                classes["hold_mobile_units"] += count
            classes["flow_trigger_in_contact_state"].update(result.get("triggers", []))
            if result.get("pd_ambush"):
                classes["pd_ambush"].append(tag)
        classes["flow_trigger_in_contact_state"] = sorted(classes["flow_trigger_in_contact_state"])
        if failed:
            klass = "BROKEN (compile)"
        elif classes["ignore_engage_objective_or_ally"] or any("(contact-only)" in t for t in classes["flow_trigger_in_contact_state"]):
            klass = "AT RISK (high)"
        elif classes["ignore_engage"] or classes["flow_trigger_in_contact_state"] or classes["escort"] or classes["pd_ambush"]:
            klass = "AT RISK"
        else:
            klass = "SAFE (balance only)"
        missions[fit.rsplit("/", 1)[1][:-4]] = {"class": klass, "classes": classes, "failed": failed, "missing": missing}
    return {"source": str(path), "mode": "as-shipped" if shipped else "fixabl", "missions": missions, "brains": brains,
            "unused_with_errors": {k: v for k, v in errors.items() if k not in used},
            "replaced_corebrain": COREBRAIN in tree,
            "duplicate_symbols": sorted(set(magic["symbols"]) & other["symbols"]),
            "exit_fail": any(m["failed"] for m in missions.values())}


def mods_pass(root):
    """Every mod under root as the deployment uses it: unmigrated brains against the Magic library."""
    root = Path(root)
    stock_path = root.parent / "mission.fst"
    stock = load_tree(stock_path) if stock_path.is_file() else {}
    report = {}
    for mod in sorted(p for p in root.iterdir() if p.is_dir()):
        tree = next((d for d in (mod / "data/missions", mod / "missions", mod / "mission.fst", mod) if d.exists() and (
            d.is_file() or any(d.glob("*.fit")) or any(d.glob("*.abx")) or (d / "warriors").is_dir())), None)
        if tree is None:
            continue
        result = check(tree, shipped=True, stock=stock)
        failed = sorted({b for m in result["missions"].values() for b in m["failed"]})
        reasons = []
        if result["replaced_corebrain"]:
            reasons.append("ships its own corebrain.abx, which Smarter replaces with Magic's")
        if failed:
            reasons.append(f"{len(failed)} mission brains do not compile against Magic as shipped")
        if result["duplicate_symbols"]:
            reasons.append(f"redefines Magic core orders: {result['duplicate_symbols']}")
        if stock:
            loosed = load_tree(tree)
            shadowed = sorted(k for k in loosed if k.startswith("data/missions/warriors/") and k.endswith((".abl", ".abi"))
                              and k in stock and loosed[k] != stock[k])
            if shadowed:
                reasons.append(f"{len(shadowed)} warrior files share a stock name and are shadowed by the stock FixABL copy")
        report[mod.name] = {"status": "fail" if reasons else ("no-brains" if not result["brains"] else "pass"),
                            "reasons": reasons, "missions": len(result["missions"]), "brains": len(result["brains"]),
                            "failed": failed}
    return report


def table(result):
    lines = []
    for name, m in result["missions"].items():
        lines.append(f"{name}: {m['class']}")
        for klass, value in m["classes"].items():
            if value:
                lines.append(f"   {klass}: {value}")
        if m["failed"]:
            lines.append(f"   compile_fail: {m['failed']}")
    if result["unused_with_errors"]:
        lines.append(f"unused brains with compile errors: {sorted(result['unused_with_errors'])}")
    if result["duplicate_symbols"]:
        lines.append(f"orders defined by both Magic and this tree: {result['duplicate_symbols']}")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Magic corebrain compatibility of warrior brains")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("path", nargs="?", help="mission.fst or a data/missions tree")
    target.add_argument("--mods-root", help="check every mod under this directory as shipped")
    parser.add_argument("--as-shipped", action="store_true", help="do not apply FixABL (loose mod brains)")
    parser.add_argument("--json", help="write the JSON report here (default: stdout)")
    args = parser.parse_args(argv)
    if args.mods_root:
        report = mods_pass(args.mods_root)
        failed = [mod for mod, r in report.items() if r["status"] == "fail"]
        text = json.dumps(report, indent=1)
        print("\n".join(f"{mod}: {r['status']}" + (f"  {'; '.join(r['reasons'])}" if r["reasons"] else "")
                        for mod, r in report.items()))
        code = 1 if failed else 0
    else:
        result = check(args.path, shipped=args.as_shipped)
        text, code = json.dumps(result, indent=1), 1 if result["exit_fail"] else 0
        print(table(result))
    if args.json:
        Path(args.json).write_text(text)
    else:
        print(text)
    return code


if __name__ == "__main__":
    sys.exit(main())
