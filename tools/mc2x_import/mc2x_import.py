"""
tools/mc2x_import/mc2x_import.py -- MC2X / CVE-G install -> mod converter.

Manifest-driven: reproduces the known-good 0.4c mc2x-compat + cveg mod file set by
extracting each file's BYTES from the end user's own MC2X install (legal -- the user
owns their install), guided by a shipped recipe of filenames + source mapping
(mc2x_recipe.json; filenames are not copyrightable). A handful of load-bearing compat
shims that are NOT MC2X content (appearance .ini stubs, nop.abl, cveg string aliases,
gn mech icons) ship under shims/ and are dropped in verbatim.

Emits:
  <deploy>/mods/mc2x-compat/   (type: dependency)
  <deploy>/mods/cveg/          (type: campaign)

Never writes under <deploy>/data/. Python 3 stdlib only. No emoji.
"""
from __future__ import annotations
import argparse, json, os, shutil, sys, tempfile
from struct import error as struct_error

# FROZEN-AWARE: when PyInstaller-frozen, bundled python modules + DATA (recipe, shims/)
# live under sys._MEIPASS. Dev: they live next to this file.
_FROZEN = getattr(sys, "frozen", False)
_HERE = os.path.dirname(os.path.abspath(__file__))
_BASE = getattr(sys, "_MEIPASS", _HERE)   # bundle root when frozen, else this file's dir
sys.path.insert(0, _BASE if _FROZEN else _HERE)
import fst        # local: FST/pak read helpers
import manifest   # local: provenance + marker
from transfer_mc2x_campaign import asset_bytes

RECIPE_PATH = os.path.join(_BASE, "mc2x_recipe.json")
SHIMS_DIR = os.path.join(_BASE, "shims")

REQUIRED_INPUTS = ["tgl.fst", "mission.fst", "art.fst", "misc.fst",
                   "data/objects/object2.pak", "mc2xres.dll"]
OPTIONAL_INPUTS = ["textures.fst", "data/objects/feet.pak"]

MODJSON = {
  "mc2x-compat": {"id": "mc2x-compat", "name": "MC2X Compatibility Base",
                  "type": "dependency", "version": "1.0",
                  "description": "Compatibility assets and definitions required by MC2X/CVE-G campaign packs."},
  "cveg":        {"id": "cveg", "name": "Carver V Extended (CVE-G)",
                  "type": "campaign", "version": "1.0", "dependencies": ["mc2x-compat"]},
}

def validate_source(source):
    missing = [p for p in REQUIRED_INPUTS
               if not os.path.isfile(os.path.join(source, p.replace("/", os.sep)))]
    missing_opt = [p for p in OPTIONAL_INPUTS
                   if not os.path.isfile(os.path.join(source, p.replace("/", os.sep)))]
    return missing, missing_opt

def load_recipe():
    with open(RECIPE_PATH, encoding="utf-8") as f:
        return json.load(f)

def _write(root, rel, content):
    import hashlib
    dest = os.path.join(root, rel.replace("/", os.sep))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "wb") as f:
        f.write(content)
    return (rel, len(content), hashlib.sha256(content).hexdigest())

def extract_mod(source, deploy, mod_id, entries):
    """Extract every recipe entry for mod_id from the MC2X install into the mod folder.
    Single pass per source FST. Returns (written[], missing[])."""
    root = os.path.join(deploy, "mods", mod_id)
    os.makedirs(root, exist_ok=True)
    written, missing = [], []
    # Group wanted source-names by their FST; loose handled directly.
    # One source entry may map to MULTIPLE targets (0.4c relocations, e.g.
    # warriors/pmwace.fit also lives at profiles/pmwace.fit) -> list of targets.
    by_fst = {}   # fst_name -> {src_name_lower: [target_rel, ...]}
    for e in entries:
        fstn, src, tgt = e["fst"], e["s"], e["t"]
        if tgt.lower() == "data/objects/object2.pak":
            content = asset_bytes([source], src)
            if content is None:
                missing.append(tgt)
            else:
                written.append(_write(root, tgt, content))
            continue
        if fstn == "loose":
            sp = os.path.join(source, src.replace("/", os.sep))
            if os.path.isfile(sp):
                with open(sp, "rb") as f:
                    written.append(_write(root, tgt, f.read()))
            else:
                missing.append(tgt)
        else:
            by_fst.setdefault(fstn, {}).setdefault(src, []).append(tgt)
    for fstn, wanted in by_fst.items():
        p = os.path.join(source, fstn)
        seen = set()
        for off, comp, real, name, is_lz, data in fst.fst_entries(p) if os.path.isfile(p) else []:
            key = name.replace("\\", "/").lower()
            tgts = wanted.get(key)
            if not tgts:
                continue
            content = fst.decompress_entry(off, comp, real, is_lz, data)
            for tgt in tgts:
                written.append(_write(root, tgt, content))
            seen.add(key)
        for k, tgts in wanted.items():
            if k not in seen:
                loose = os.path.join(source, k.replace("/", os.sep))
                if os.path.isfile(loose):
                    with open(loose, "rb") as stream:
                        content = stream.read()
                    for tgt in tgts:
                        written.append(_write(root, tgt, content))
                else:
                    missing.extend(tgts)
    return written, missing

def copy_shims(deploy, mod_id):
    """Drop the bundled non-MC2X compat shims for mod_id. Returns count."""
    src_root = os.path.join(SHIMS_DIR, mod_id)
    if not os.path.isdir(src_root):
        return 0
    n = 0
    for r, _d, files in os.walk(src_root):
        for fn in files:
            sp = os.path.join(r, fn)
            rel = os.path.relpath(sp, src_root)
            dp = os.path.join(deploy, "mods", mod_id, rel)
            os.makedirs(os.path.dirname(dp), exist_ok=True)
            shutil.copy(sp, dp)
            n += 1
    return n

STRINGS_REL = "data/defs/text/en_us/y_mc2x_strings.fit"


def _fit_text(text):
    return text.replace("\\", "\\\\").replace('"', "'").replace("\r", "").replace("\n", "\\n")


def write_mc2x_strings(source, deploy):
    """MC2X names its units, pilots and weapons with mc2xres.dll string ids. The cveg aliases point at
    "cveg.dll.<id>" keys and ids past the stock table read "Unassigned"/"undefined" without these records,
    so every dll string becomes a String record (non-mc2res source, so it wins over the stock text)."""
    import strings as mc2x_strings
    try:
        table = mc2x_strings.extract_strings(os.path.join(source, "mc2xres.dll"))
    except (OSError, struct_error, IndexError, TypeError):
        table = {}
    if not table:
        print("WARNING: mc2xres.dll has no readable string table; MC2X names stay unresolved", file=sys.stderr)
        return 0
    body = ["FITini", "", "// MC2X strings from the user's mc2xres.dll (written by mc2x_import)."]
    for sid, text in sorted(table.items()):
        if text.strip():
            body += ["", "String {", f'    key = "cveg.dll.{sid}"', f'    text = "{_fit_text(text)}"',
                     f"    legacyId = {sid}", '    source = "cveg_dll"', "}"]
    _write(os.path.join(deploy, "mods", "mc2x-compat"), STRINGS_REL, ("\n".join(body) + "\n").encode("utf-8"))
    return sum(1 for t in table.values() if t.strip())


def write_cveg_roster(deploy):
    """CVE-G campaigns buy player pilots by their pmp_ names, but MC2X's roster file lists only the pmw
    warriors, so the bay had no pilots. cveg's roster is the compat roster plus every pmp_ pilot it ships."""
    from transfer_mc2x_campaign import write_player_roster
    return write_player_roster(os.path.join(deploy, "mods", "cveg"),
                              os.path.join(deploy, "mods", "mc2x-compat", "data/objects/pilots.csv"))


def _is_generated(deploy, mod_id):
    p = os.path.join(deploy, "mods", mod_id, "mc2x_import_report.json")
    try:
        with open(p) as f:
            return json.load(f).get("generated_by") == "mc2x_import"
    except Exception:
        return False

def _guard(args):
    for m in ("mc2x-compat", "cveg"):
        d = os.path.join(args.deploy, "mods", m)
        if not os.path.isdir(d):
            continue
        if not _is_generated(args.deploy, m):
            print(f"ERROR: {d} exists and was not generated by mc2x_import; refusing.", file=sys.stderr)
            return 3
        if not args.force:
            print(f"ERROR: {d} already imported; re-run with --force to rebuild.", file=sys.stderr)
            return 4
    return None

def build_argparser():
    ap = argparse.ArgumentParser(
        prog="mc2x_import",
        description="Reproduce the mc2x-compat + cveg mods from a user's MC2X / CVE-G install.")
    ap.add_argument("--source", required=True, help="Path to the MC2X / CVE-G install root")
    ap.add_argument("--deploy", required=True,
                    help="installed game folder containing mods/ (e.g. mc2-win64-*). NOT a release-zip staging dir.")
    ap.add_argument("--dry-run", action="store_true", help="Report the plan; write nothing.")
    ap.add_argument("--force", action="store_true",
                    help="Rebuild even existing mod folders, but ONLY if generated by mc2x_import.")
    return ap

def main(argv=None):
    args = build_argparser().parse_args(argv)
    missing_req, missing_opt = validate_source(args.source)
    if missing_req:
        print(f"ERROR: --source {args.source!r} is missing required MC2X inputs: "
              + ", ".join(missing_req), file=sys.stderr)
        return 2
    for p in missing_opt:
        print(f"WARNING: optional input not found (continuing): {p}")
    recipe = load_recipe()
    for m in ("mc2x-compat", "cveg"):
        n = len(recipe["mods"].get(m, []))
        shims = sum(len(files) for _r, _d, files in os.walk(os.path.join(SHIMS_DIR, m))) \
                if os.path.isdir(os.path.join(SHIMS_DIR, m)) else 0
        print(f"{m}: {n} files from install + {shims} bundled shims")
    if args.dry_run:
        return 0
    code = _guard(args)
    if code:
        return code
    os.makedirs(args.deploy, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".mc2x-import-", dir=args.deploy) as scratch:
        staged = argparse.Namespace(**vars(args))
        staged.deploy = os.path.join(scratch, "install")
        reports = os.path.join(args.deploy, "mods", ".import_reports")
        actions = []
        try:
            if os.path.isdir(reports):
                shutil.copytree(reports, os.path.join(staged.deploy, "mods", ".import_reports"))
            written, all_missing = {}, {}
            for m in ("mc2x-compat", "cveg"):
                w, miss = extract_mod(args.source, staged.deploy, m, recipe["mods"].get(m, []))
                sh = copy_shims(staged.deploy, m)
                with open(os.path.join(staged.deploy, "mods", m, "mod.json"), "w", encoding="utf-8") as f:
                    json.dump(MODJSON[m], f, indent=2)
                written[m], all_missing[m] = w, miss
                print(f"  {m}: extracted {len(w)} + {sh} shims, {len(miss)} missing-from-install")
                for name in miss:
                    print(f"WARNING: {m}: missing recipe file: {name}", file=sys.stderr)
            print(f"  mc2x-compat: {write_mc2x_strings(args.source, staged.deploy)} mc2xres.dll strings")
            print(f"  cveg: {write_cveg_roster(staged.deploy)} player pilots in the roster")
            if not os.path.isfile(os.path.join(staged.deploy, "mods", "mc2x-compat", "data/objects/object2.pak")):
                print("ERROR: no data/objects/object2.pak produced for mc2x-compat", file=sys.stderr)
                return 2
            _write_provenance(staged, written, all_missing)
            os.makedirs(os.path.join(args.deploy, "mods"), exist_ok=True)
            for m in ("mc2x-compat", "cveg", ".import_reports"):
                target = os.path.join(args.deploy, "mods", m)
                backup = os.path.join(scratch, m)
                existed = os.path.exists(target)
                if existed:
                    os.rename(target, backup)
                actions.append((target, backup if existed else None))
                os.rename(os.path.join(staged.deploy, "mods", m), target)
        except Exception as exc:
            for target, backup in reversed(actions):
                if os.path.isdir(target):
                    shutil.rmtree(target)
                if backup is not None:
                    os.replace(backup, target)
            print(f"ERROR: import failed: {exc}", file=sys.stderr)
            return 2
    return 0


def _write_provenance(args, written, all_missing):
    import datetime
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    base = {"generated_by": "mc2x_import", "tool_version": manifest.tool_version(),
            "git_commit": manifest.git_commit(_HERE),
            "source_path": os.path.abspath(args.source),
            "source_fingerprint": manifest.source_fingerprint(args.source), "stamp": stamp}
    rows = []
    for m in ("mc2x-compat", "cveg"):
        for rel, size, sha in written[m]:
            rows.append([m, rel, size, sha, "import"])
        manifest.write_marker(args.deploy, m,
                              dict(base, mod=m, files_written=len(written[m]),
                                   files_missing=len(all_missing[m]), missing=all_missing[m], files_skipped=0))
    manifest.write_reports(args.deploy, stamp,
                           dict(base, totals={m: len(written[m]) for m in written},
                                missing={m: all_missing[m] for m in all_missing}),
                           rows)

if __name__ == "__main__":
    raise SystemExit(main())
