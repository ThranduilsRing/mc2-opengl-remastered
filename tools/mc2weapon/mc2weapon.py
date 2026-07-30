#!/usr/bin/env python3
"""mc2weapon — MechCommander 2 weapon viewer / modder tool.

VIEWER (this slice): read every component from compbas.csv, resolve each weapon's
FX binding via effects.csv, and present a unified weapon catalog (human table or
--json). Editor commands (set / new / validate, writing a loose mods/<id>/ overlay)
build on this same data model — see docs/weapon-tool-design.md.

Data model (recon: docs/weapon-tool-design.md):
  compbas.csv  — one row per component; masterID = row's first column. Weapon stats:
                 damage, heat, recycle(cooldown s), Weight(tons), crit-hits(slots),
                 Range bracket (short/medium/long), Special FX ID, Missile type, Fields.
  effects.csv  — FX table indexed by Special FX ID (skip 1 header line, then row N =
                 FX id N): effectName / muzzleFlash / hitEffect / missEffect / objNum.

Stats are CSV TEXT — no engine/pak needed. compbas.csv is packed in v0.4 but a loose
mods/<id>/data/objects/compbas.csv overlay works (the editor writes that). Weapons have
NO 3D mesh: their visual is a procedural bolt (texture + gosFX), so "model" = bolt
texture + FX, not a glb. See the design doc.
"""

import argparse
import csv
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "mc2data"))
from stock_corpus import (  # noqa: E402
    StockDataError, add_allow_nonstock_arg, resolve as resolve_corpus,
)

WEAPON_TYPES = {"energyweapon", "ballisticweapon", "missileweapon"}

# compbas.csv columns are matched by header text (prefix/contains) so the tool
# survives column drift. Each logical field -> a predicate over the header cell.
COMPBAS_FIELDS = [
    ("masterID", "component table"),
    ("type", "type"),
    ("name", "name"),
    ("slots", "crit hits"),
    ("recycle", "recycle"),
    ("heat", "heat"),
    ("tons", "weight"),
    ("damage", "damage"),
    ("br", "br"),
    ("rp", "rp"),
    ("range", "range"),
    ("missileType", "missile type"),
    ("fields", "fields"),
    ("fxid", "special fx id"),
    ("ammoMasterId", "ammo master id"),
    ("iconX", "icon x"),
    ("iconY", "icon y"),
]

# ---------------------------------------------------------------------------
# DATA-PROVENANCE-CLOSE-1 (2026-07-30) -- where the stock CSVs come from.
#
# This used to be two hand-written candidate lists searched first-hit-wins:
#
#     COMPBAS_CANDIDATES = ["data/objects/compbas.csv",
#                           "A:/Games/mc2-srcdata/objects/compbas.csv",     <-- EDITABLE clone
#                           "A:/Games/mc2-opengl/mc2-win64-v0.4/data/objects/compbas.csv",
#                           "A:/Games/mc2-opengl/mc2srcdata-fresh/objects/compbas.csv"]
#
# Two defects, and the second is the interesting one:
#
#  1. The EDITABLE srcdata clone was searched BEFORE anything deployed, so a
#     local edit would silently win over the shipped data and the tool would
#     report the edited stats as stock.
#
#  2. Reordering alone would have fixed NOTHING. Measured 2026-07-30: NO deploy
#     lane on this machine carries a loose `data/objects/compbas.csv` or
#     `effects.csv` at all -- both files exist only inside `misc.fst`. So the
#     "deploy" rung it was ordered behind never resolved, and never will. The
#     clone was not merely preferred; it was the only rung that ever hit.
#
# Verification is the fix. The corpus resolver (tools/mc2data/stock_corpus.py)
# checks whatever it finds against the digests of the shipped `misc.fst`, per
# name, and falls back to extracting the CSVs straight out of that archive when
# no source tree verifies. `--allow-nonstock` is the loud escape hatch, and it
# is what a MOD overlay legitimately needs -- editing a mod's compbas is a
# supported workflow, calling a mod's numbers "stock" is not.
#
# Verified PER NAME on purpose: `badpilots.csv` differs from the shipped
# `misc.fst` in every source tree on this machine, so a whole-directory gate
# would refuse `compbas.csv` -- which does match -- over a file nothing here
# opens.
# ---------------------------------------------------------------------------
STOCK_CSVS = ("compbas.csv", "effects.csv")


def _resolve_csvs(compbas_arg, effects_arg, allow_nonstock):
    """Return (compbas_path, effects_path), both proven or loudly flagged."""
    explicit = {}
    for what, arg in (("compbas", compbas_arg), ("effects", effects_arg)):
        if arg:
            if not os.path.isfile(arg):
                sys.exit(f"mc2weapon: --{what} not found: {arg}")
            explicit[what] = Path(arg)

    need = [f"{w}.csv" for w in ("compbas", "effects") if w not in explicit]
    resolved = {}
    if need:
        try:
            res = resolve_corpus("objects", names=need,
                                 allow_nonstock=allow_nonstock)
        except StockDataError as e:
            sys.exit(f"mc2weapon: {e}")
        for n in need:
            resolved[n[:-4]] = res.member(n)

    # An explicit path is verified too -- a path typed on the command line is
    # exactly as unproven as one baked into a list, which is why the original
    # STOCK-DATA-TRUTH-1 fix did not simply make the flag required.
    for what, p in explicit.items():
        try:
            resolve_corpus("objects", explicit=p.parent, names=[p.name],
                           allow_nonstock=allow_nonstock)
        except StockDataError as e:
            sys.exit(f"mc2weapon: --{what} {p}\n{e}")
        resolved[what] = p

    return str(resolved["compbas"]), str(resolved["effects"])


def _col_index(header, needle):
    """First header cell whose lowercased text starts with / contains needle."""
    nl = needle.lower()
    for i, h in enumerate(header):
        hl = h.strip().lower()
        if hl.startswith(nl) or nl in hl:
            return i
    return -1


def load_compbas(path):
    with open(path, newline="", encoding="latin-1") as f:
        rows = list(csv.reader(f))
    if not rows:
        sys.exit(f"mc2weapon: empty compbas: {path}")
    header = rows[0]
    idx = {field: _col_index(header, needle) for field, needle in COMPBAS_FIELDS}
    # Robustness: some mod compbas (e.g. Omnitech) ship a malformed fully-quoted
    # header that collapses to a few columns. The layout is positional/standard,
    # so fall back to canonical indices when the header didn't parse to columns.
    if len(header) < 20 or any("," in h for h in header):
        idx = {"masterID": 0, "type": 1, "name": 2, "slots": 3, "recycle": 4,
               "heat": 5, "tons": 6, "damage": 7, "br": 8, "rp": 9, "range": 10,
               "missileType": 19, "fields": 20, "fxid": 21, "ammoMasterId": 22,
               "iconX": 27, "iconY": 28}
    comps = []
    for r in rows[1:]:
        if not r or not r[0].strip():
            continue

        def get(field):
            i = idx.get(field, -1)
            return r[i].strip() if 0 <= i < len(r) else ""

        comps.append({field: get(field) for field, _ in COMPBAS_FIELDS})
    return comps, idx, header


def load_effects(path):
    """Returns dict: fxid (int) -> {name, muzzle, hit, miss, objNum, weaponName}.
    Loader semantics: skip 1 header line, then data row position == FX id."""
    with open(path, newline="", encoding="latin-1") as f:
        rows = list(csv.reader(f))
    fx = {}
    for fxid, r in enumerate(rows[1:]):  # row 0 = header; row 1 -> fxid 0
        if not r:
            continue
        def cell(i):
            return r[i].strip() if i < len(r) else ""
        fx[fxid] = {
            "name": cell(1), "muzzle": cell(2), "hit": cell(3),
            "objNum": cell(4), "miss": cell(5),
            "weaponName": cell(6) if len(r) > 6 else "",
        }
    return fx


def is_weapon(comp):
    return comp.get("type", "").strip().lower() in WEAPON_TYPES


def build_catalog(compbas_path, effects_path, weapons_only=True):
    comps, _, _ = load_compbas(compbas_path)
    fx = load_effects(effects_path)
    out = []
    for c in comps:
        if weapons_only and not is_weapon(c):
            continue
        entry = dict(c)
        entry["isWeapon"] = is_weapon(c)
        fxid_s = c.get("fxid", "")
        try:
            fxid = int(float(fxid_s))
        except (ValueError, TypeError):
            fxid = None
        entry["fxResolved"] = fx.get(fxid) if fxid is not None else None
        out.append(entry)
    return out, fx


def cmd_list(args):
    cat, _ = build_catalog(args.compbas, args.effects, weapons_only=not args.all)
    if args.json:
        print(json.dumps(cat, indent=2))
        return
    hdr = f"{'ID':>3}  {'Name':<28} {'Type':<15} {'Dmg':>6} {'Heat':>5} " \
          f"{'Recyc':>6} {'Range':<7} {'Tons':>5} {'Slot':>4} {'FX':>3} {'FX name':<18}"
    print(hdr)
    print("-" * len(hdr))
    for e in cat:
        fxname = (e["fxResolved"] or {}).get("name", "") if e["fxResolved"] else ""
        print(f"{e['masterID']:>3}  {e['name'][:28]:<28} {e['type'][:15]:<15} "
              f"{e['damage']:>6} {e['heat']:>5} {e['recycle']:>6} "
              f"{e['range'][:7]:<7} {e['tons']:>5} {e['slots']:>4} {e['fxid']:>3} "
              f"{fxname[:18]:<18}")
    print(f"\n{len(cat)} {'component' if args.all else 'weapon'}(s).")


def cmd_show(args):
    cat, fx = build_catalog(args.compbas, args.effects, weapons_only=False)
    q = args.weapon.strip().lower()
    hit = None
    for e in cat:
        if e["masterID"] == args.weapon or e["name"].strip().lower() == q:
            hit = e
            break
    if hit is None:
        # substring fallback
        matches = [e for e in cat if q in e["name"].lower()]
        if len(matches) == 1:
            hit = matches[0]
        elif len(matches) > 1:
            print("Ambiguous; matches:")
            for m in matches:
                print(f"  [{m['masterID']}] {m['name']}")
            return
    if hit is None:
        sys.exit(f"mc2weapon: no component matching '{args.weapon}'")
    if args.json:
        print(json.dumps(hit, indent=2))
        return
    print(f"[{hit['masterID']}] {hit['name']}  ({hit['type']})")
    for k in ("damage", "heat", "recycle", "range", "tons", "slots", "br", "rp",
              "missileType", "fields", "ammoMasterId"):
        print(f"  {k:<13}: {hit[k]}")
    print(f"  {'fxid':<13}: {hit['fxid']}")
    r = hit["fxResolved"]
    if r:
        print(f"  FX -> trail={r['name']}  muzzle={r['muzzle']}  hit={r['hit']}  "
              f"miss={r['miss']}  objNum={r['objNum']}")
    else:
        print(f"  FX -> (Special FX ID {hit['fxid']} not found in effects.csv)")


# ----------------------------------------------------------------------------
# Editor: edits write a LOOSE mods/<id>/data/objects/compbas.csv overlay (full
# CSV, only edited cells differ from base). The game resolves the loose overlay
# first (MC2_ACTIVE_MOD=<id>) -- no .pak repack. Validation gates every write.
# ----------------------------------------------------------------------------

VALID_RANGES = {"short", "medium", "long", "0"}
# compbas "Missile type" column is an enum, not a number.
VALID_MTYPES = {"0", "1", "lrm", "st", "srm"}
# field -> (compbas idx key, kind) for `set`/`new`. kind drives validation.
EDITABLE = {
    "damage": "ufloat", "heat": "ufloat", "recycle": "ufloat", "tons": "ufloat",
    "slots": "uint", "range": "range", "missileType": "mtype", "fields": "int",
    "fxid": "fxid", "ammoMasterId": "int", "name": "str", "type": "wtype",
    "iconX": "uint", "iconY": "uint",
}


def load_compbas_raw(path):
    with open(path, newline="", encoding="latin-1") as f:
        rows = list(csv.reader(f))
    if not rows:
        sys.exit(f"mc2weapon: empty compbas: {path}")
    header = rows[0]
    idx = {field: _col_index(header, needle) for field, needle in COMPBAS_FIELDS}
    return header, rows[1:], idx


def write_csv(path, header, data_rows):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", newline="", encoding="latin-1") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(data_rows)


def _validate_cell(field, value, effects):
    kind = EDITABLE.get(field)
    if kind in ("ufloat", "uint"):
        try:
            n = float(value)
        except ValueError:
            return f"{field}={value!r} is not numeric"
        if n < 0:
            return f"{field}={value} must be >= 0"
        if kind == "uint" and (n != int(n) or int(n) <= 0):
            return f"{field}={value} must be a positive integer"
    elif kind == "int":
        try:
            int(value)
        except ValueError:
            return f"{field}={value!r} is not an integer"
    elif kind == "range":
        if value.strip().lower() not in VALID_RANGES:
            return f"range={value!r} not in {sorted(VALID_RANGES)}"
    elif kind == "mtype":
        if value.strip().lower() not in VALID_MTYPES:
            return f"missileType={value!r} not in 0/1/LRM/ST/SRM"
    elif kind == "wtype":
        if value.strip().lower() not in WEAPON_TYPES:
            return f"type={value!r} not a weapon type {sorted(WEAPON_TYPES)}"
    elif kind == "fxid":
        try:
            fxid = int(float(value))
        except ValueError:
            return f"fxid={value!r} is not an integer"
        if fxid not in effects:
            return f"fxid={fxid} has no row in effects.csv"
    elif kind == "str":
        if not value.strip():
            return f"{field} must be non-empty"
    return None


def validate_rows(header, data_rows, idx, effects, weapons_only=True):
    """Returns list of (masterID, message) problems."""
    problems = []
    ti, ri, fi = idx["type"], idx["range"], idx["fxid"]
    mi = idx.get("missileType", -1)
    for r in data_rows:
        if not r or not r[0].strip():
            continue
        mid = r[0].strip()
        typ = (r[ti].strip().lower() if 0 <= ti < len(r) else "")
        if weapons_only and typ not in WEAPON_TYPES:
            continue
        # range bracket
        if 0 <= ri < len(r) and r[ri].strip().lower() not in VALID_RANGES:
            problems.append((mid, f"range={r[ri]!r} invalid"))
        # missile-type enum
        if 0 <= mi < len(r) and r[mi].strip().lower() not in VALID_MTYPES:
            problems.append((mid, f"missileType={r[mi]!r} not 0/1/LRM/ST/SRM"))
        # fx id present in effects.csv
        if 0 <= fi < len(r):
            try:
                fxid = int(float(r[fi]))
                if fxid not in effects:
                    problems.append((mid, f"Special FX ID {fxid} absent from effects.csv"))
            except ValueError:
                problems.append((mid, f"Special FX ID {r[fi]!r} not an integer"))
    return problems


def ensure_mod(mod_root, modid):
    """Create mods/<id>/ with mod.json if absent; return the data/objects dir."""
    moddir = os.path.join(mod_root, modid)
    objdir = os.path.join(moddir, "data", "objects")
    os.makedirs(objdir, exist_ok=True)
    modjson = os.path.join(moddir, "mod.json")
    if not os.path.isfile(modjson):
        with open(modjson, "w", encoding="utf-8") as f:
            json.dump({"schema": "mc2-mod/1", "id": modid, "name": modid,
                       "version": "1.0.0", "dependencies": []}, f, indent=2)
    return objdir


def _overlay_or_base(mod_root, modid, base_path):
    """If a mod overlay compbas already exists, edit it (cumulative); else base."""
    overlay = os.path.join(mod_root, modid, "data", "objects", "compbas.csv")
    return overlay if os.path.isfile(overlay) else base_path


def _find_row(data_rows, weapon):
    q = weapon.strip().lower()
    for i, r in enumerate(data_rows):
        if r and (r[0].strip() == weapon or
                  (len(r) > 2 and r[2].strip().lower() == q)):
            return i
    return -1


def find_mods(mods_dir):
    """Mod ids under mods_dir that carry a data/objects/compbas.csv."""
    out = []
    if not os.path.isdir(mods_dir):
        return out
    for name in sorted(os.listdir(mods_dir)):
        cb = os.path.join(mods_dir, name, "data", "objects", "compbas.csv")
        if os.path.isfile(cb):
            out.append(name)
    return out


def _read_mod_type(modjson):
    try:
        with open(modjson, encoding="utf-8") as f:
            return json.load(f).get("type", "")
    except Exception:
        return ""


def cmd_list_mods(args):
    md = args.mods_dir
    if not os.path.isdir(md):
        print(f"(no such mods dir: {md})")
        return
    rows = []  # (name, hasWeapons, count, type)
    for name in sorted(os.listdir(md)):
        d = os.path.join(md, name)
        if not os.path.isdir(d) or name.startswith("."):
            continue
        cb = os.path.join(d, "data", "objects", "compbas.csv")
        if os.path.isfile(cb):
            try:
                comps, _, _ = load_compbas(cb)
                n = sum(1 for c in comps if is_weapon(c))
            except SystemExit:
                n = 0
            rows.append((name, True, n, ""))
        else:
            rows.append((name, False, 0, _read_mod_type(os.path.join(d, "mod.json"))))
    rows.sort(key=lambda r: (not r[1], r[0]))
    if args.json:
        print(json.dumps([{"id": r[0], "hasWeapons": r[1], "weapons": r[2],
                           "type": r[3]} for r in rows], indent=2))
        return
    for name, has, n, typ in rows:
        if has:
            print(f"  {name:<28} {n} weapon(s)")
        else:
            print(f"  {name:<28} -- {typ or 'no compbas'}, no weapons of its own")
    wc = sum(1 for r in rows if r[1])
    print(f"\n{len(rows)} mod(s), {wc} define weapons. Load a weapon mod with --from-mod <id>. "
          f"Campaign/dependency mods reuse base or a dependency's weapons (e.g. Omnitech).")


def cmd_list_fx(args):
    fx = load_effects(args.effects)
    if args.json:
        print(json.dumps(fx, indent=2))
        return
    print(f"{'FXid':>4}  {'trail':<20} {'muzzle':<18} {'hit':<18} {'miss':<18} obj")
    print("-" * 86)
    for fxid in sorted(fx):
        e = fx[fxid]
        print(f"{fxid:>4}  {e['name'][:20]:<20} {e['muzzle'][:18]:<18} "
              f"{e['hit'][:18]:<18} {e['miss'][:18]:<18} {e['objNum']}")
    print(f"\n{len(fx)} FX entries (assignable Special FX IDs).")


def cmd_validate(args):
    path = args.file or args.compbas
    header, data_rows, idx = load_compbas_raw(path)
    effects = load_effects(args.effects)
    problems = validate_rows(header, data_rows, idx, effects,
                             weapons_only=not args.all)
    if not problems:
        print(f"OK: {path} -- all {'components' if args.all else 'weapons'} valid.")
        return
    for mid, msg in problems:
        print(f"  [{mid}] {msg}")
    sys.exit(f"\n{len(problems)} problem(s) in {path}")


def _apply_edits(args, edits):
    """Shared by set/set-fx/new: load base-or-overlay, apply, validate, write."""
    effects = load_effects(args.effects)
    base = _overlay_or_base(args.mod_root, args.mod, args.compbas)
    header, data_rows, idx = load_compbas_raw(base)

    ri = _find_row(data_rows, args.weapon)
    if ri < 0:
        if not getattr(args, "create", False):
            sys.exit(f"mc2weapon: no component matching '{args.weapon}'")
        # `new`: weapon arg must be an unused masterID slot
        try:
            mid = int(args.weapon)
        except ValueError:
            sys.exit("mc2weapon new: <id> must be a numeric masterID")
        for i, r in enumerate(data_rows):
            if r and r[0].strip() == str(mid):
                ri = i
                cur = (r[idx["type"]].strip().lower() if 0 <= idx["type"] < len(r) else "")
                if cur in WEAPON_TYPES and not args.force:
                    sys.exit(f"mc2weapon new: masterID {mid} already a weapon "
                             f"({r[idx['type']]}); use --force to overwrite")
                break
        if ri < 0:
            sys.exit(f"mc2weapon new: masterID {mid} not in compbas (0..{len(data_rows)-1})")

    row = list(data_rows[ri])
    # validate every edit, then apply
    errors = []
    for field, value in edits.items():
        if field not in EDITABLE:
            errors.append(f"unknown field '{field}' (editable: {sorted(EDITABLE)})")
            continue
        err = _validate_cell(field, value, effects)
        if err:
            errors.append(err)
    if errors:
        for e in errors:
            print(f"  {e}")
        sys.exit(f"\n{len(errors)} validation error(s); nothing written.")
    for field, value in edits.items():
        ci = idx[field]
        if ci < 0:
            sys.exit(f"mc2weapon: field '{field}' has no column in this compbas")
        while len(row) <= ci:
            row.append("")
        row[ci] = value
    data_rows[ri] = row

    objdir = ensure_mod(args.mod_root, args.mod)
    out = os.path.join(objdir, "compbas.csv")
    write_csv(out, header, data_rows)
    print(f"wrote {out}")
    print(f"  [{row[0]}] {row[idx['name']] if 0 <= idx['name'] < len(row) else ''}: "
          + ", ".join(f"{k}={v}" for k, v in edits.items()))
    print(f"run with: MC2_ACTIVE_MOD={args.mod}")


def _parse_kv(pairs):
    out = {}
    for p in pairs:
        if "=" not in p:
            sys.exit(f"mc2weapon: expected field=value, got '{p}'")
        k, v = p.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def cmd_set(args):
    edits = _parse_kv(args.edits)
    if not edits:
        sys.exit("mc2weapon set: no field=value pairs given")
    _apply_edits(args, edits)


def cmd_set_fx(args):
    fx = load_effects(args.effects)
    val = args.fx.strip()
    try:
        fxid = int(val)
    except ValueError:
        # resolve by effect (trail) name
        matches = [k for k, e in fx.items() if e["name"].lower() == val.lower()]
        if not matches:
            sys.exit(f"mc2weapon set-fx: no effects.csv row named '{val}'")
        fxid = matches[0]
    _apply_edits(args, {"fxid": str(fxid)})


def cmd_new(args):
    edits = _parse_kv(args.edits)
    edits["type"] = args.type
    edits["name"] = args.name
    # sane defaults for required weapon stats if not supplied
    for k, d in (("damage", "1"), ("heat", "1"), ("recycle", "3"),
                 ("range", "medium"), ("slots", "1"), ("tons", "1"),
                 ("fxid", "4"), ("missileType", "0"), ("fields", "0"),
                 ("ammoMasterId", "0")):
        edits.setdefault(k, d)
    args.create = True
    args.weapon = args.id
    _apply_edits(args, edits)


def main(argv=None):
    p = argparse.ArgumentParser(prog="mc2weapon",
                                description="MC2 weapon viewer / modder tool")
    p.add_argument("--compbas", help="path to compbas.csv (auto-resolved and "
                                     "byte-verified against the shipped "
                                     "misc.fst if omitted; verified if given)")
    p.add_argument("--effects", help="path to effects.csv (same treatment)")
    add_allow_nonstock_arg(p)
    p.add_argument("--mods-dir", default="mods", help="dir holding mod folders")
    p.add_argument("--from-mod", help="load weapons from <mods-dir>/<id>/data/objects/compbas.csv")
    sub = p.add_subparsers(dest="cmd", required=True)

    pl = sub.add_parser("list", help="list weapons (or --all components) with stats + FX")
    pl.add_argument("--all", action="store_true", help="include non-weapon components")
    pl.add_argument("--json", action="store_true", help="emit JSON")
    pl.set_defaults(func=cmd_list)

    ps = sub.add_parser("show", help="show one component's full stats + resolved FX")
    ps.add_argument("weapon", help="masterID or name (substring ok)")
    ps.add_argument("--json", action="store_true", help="emit JSON")
    ps.set_defaults(func=cmd_show)

    pf = sub.add_parser("list-fx", help="list effects.csv rows (assignable FX palette)")
    pf.add_argument("--json", action="store_true", help="emit JSON")
    pf.set_defaults(func=cmd_list_fx)

    pm = sub.add_parser("list-mods", help="list mods (under --mods-dir) that have weapons")
    pm.add_argument("--mods-dir", default=argparse.SUPPRESS,
                    help="dir holding mod folders (accepted before or after the command)")
    pm.add_argument("--json", action="store_true", help="emit JSON")
    pm.set_defaults(func=cmd_list_mods, _no_csv=True)

    pv = sub.add_parser("validate", help="validate a compbas.csv (the 'just works' gate)")
    pv.add_argument("file", nargs="?", help="compbas.csv to check (default: auto-detected base)")
    pv.add_argument("--all", action="store_true", help="check all components, not just weapons")
    pv.set_defaults(func=cmd_validate)

    # editor commands share --mod / --mod-root (where the loose overlay is written)
    def add_mod_args(sp):
        sp.add_argument("--mod", default="my-weapons", help="mod id (overlay -> mods/<id>/)")
        sp.add_argument("--mod-root", default="mods", help="dir holding mod folders")
        sp.add_argument("--force", action="store_true", help="override safety checks")

    pset = sub.add_parser("set", help="edit a weapon's stats -> loose mod overlay")
    pset.add_argument("weapon", help="masterID or name")
    pset.add_argument("edits", nargs="+", help="field=value (damage/heat/recycle/range/"
                      "tons/slots/missileType/fields/fxid/ammoMasterId/name/type)")
    add_mod_args(pset)
    pset.set_defaults(func=cmd_set)

    psf = sub.add_parser("set-fx", help="assign a weapon's FX -> loose mod overlay")
    psf.add_argument("weapon", help="masterID or name")
    psf.add_argument("fx", help="Special FX ID (int) or effect trail name")
    add_mod_args(psf)
    psf.set_defaults(func=cmd_set_fx)

    pn = sub.add_parser("new", help="create a weapon in an unused masterID slot")
    pn.add_argument("id", help="unused masterID (0..254)")
    pn.add_argument("name", help="weapon name")
    pn.add_argument("--type", required=True,
                    help="EnergyWeapon | BallisticWeapon | MissileWeapon")
    pn.add_argument("edits", nargs="*", help="field=value overrides (defaults applied otherwise)")
    add_mod_args(pn)
    pn.set_defaults(func=cmd_new)

    args = p.parse_args(argv)
    # --from-mod loads weapons from a mod's overlay compbas. A mod overlay is
    # BY DEFINITION not stock, so it implies --allow-nonstock: the resolver
    # still prints the loud "does NOT describe stock data" banner, which is the
    # correct thing to say about a mod, but it does not refuse the run.
    if getattr(args, "from_mod", None) and not args.compbas:
        args.compbas = os.path.join(args.mods_dir, args.from_mod,
                                    "data", "objects", "compbas.csv")
        args.allow_nonstock = True
    if not getattr(args, "_no_csv", False):
        args.compbas, args.effects = _resolve_csvs(
            args.compbas, args.effects, args.allow_nonstock)
    args.func(args)


if __name__ == "__main__":
    main()
