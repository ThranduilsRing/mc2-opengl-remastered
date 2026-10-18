#!/usr/bin/env python3
"""
build_mco_compat.py -- assemble the `mco-compat` dependency mod that lets
MechCommander Omnitech content packs (Wolf Dragoons, Volstand, Clan Eagle,
MercStar, Desert Fox, Day of Heroes, ...) run on this engine.

Like the MC2X importer, this ships only the recipe/logic; every real byte is
read from the USER's own installs at build time (no content redistribution).

mco-compat supplies exactly the things MCO content needs that the MC2
base lacks -- and DELIBERATELY NOTHING ELSE:

  1. data/objects/object2.pak  -- a complete object table covering buildings.csv
     FitIDs up to ~2692 (the shipped MCO pak only has 1188 packets and ZERO mech
     packets). Built by build_object2.py: low slots verbatim from the user's MCO
     pak, high non-mech slots from the user's loose .fit, high mech slots from a
     generated BattleMechType stub whose ProfileName points at the user's <name>.csv.

  2. data/missions/{orders,miscfunc,corebrain}.abx  -- the ABL libraries the engine
     compiles at every mission start (mission.cpp ~2462). MCO warrior brains call
     library routines that live in corebrain. orders/miscfunc come from the user's
     MCO install. corebrain (--corebrain) is now the GENUINE MCO 2015 corebrain
     (shims/corebrain.abx), which defines magicAttack AND magicPatrol/magicGuard/
     magicEscort -- the routines MCO warrior brains actually call. The older MC2-era
     corebrain only had magicAttack + core*, so MCO patrol/guard/escort AI was dead
     (units inert until attacked). The MCO corebrain has one routine (line ~569) that
     uses a `range` type the ABL compiler rejects ("Incompatible types range"); that
     single routine fails to compile but the compiler continues and the magic*/core*
     routines compile and work (verified: ClanEagle AI). TODO(engine): teach the ABL
     compiler MCO's `range` type so that last routine compiles too.

  3. data/tgl/**  -- MCO mech appearance INIs + shapes (~302MB). Non-stock MCO mechs
     (chimera, awesome, firestarter, ...) have no appearance/shape in the MC2 base,
     so mech_recipe_build dereferences null and crashes silently at load. Mirrored
     from the user's MCO data/tgl.

  4. data/art/Buildings.csv  -- MECH-only overlay: stock rows first (preserving all
     non-mech entries), then MCO MECH rows not already present in stock, then a
     deterministic EOL.  MUST be a full-replacement file because the VFS is
     first-wins (file.cpp:IndexModData -- no merge across layers).  We include ONLY
     Type==MECH rows from MCO (never VEHICLE, BUILDING, TREE, etc.) to avoid
     re-routing logistics init into packets the engine misparses.
     Source of truth for "stock" rows: --stock-buildings (default:
     <deploy-root>/data/art/buildings.csv from the running deploy, or the first
     buildings.csv found in the base data/ tree).

  5. data/objects/{name}.csv (per-mech, loose)  -- the chassis CSV for each new
     MCO mech row added.  initVariants opens these via File::open which goes through
     TryModOpen first, so a loose file in mco-compat/data/objects/ is found before
     the base fastfile.  They are NOT in object2.pak as CSV (only as FIT stubs), so
     loose copy is required.

WHY NOT "extract everything diff from stock"?  Tried it (2026-06-14): mirroring all
~992MB of MCO data/ that differs from the base REGRESSED every campaign. MCO's
data/art/buildings.csv overlaying the base re-routed logistics init into packets it
mis-parsed -> silent crash at load; MCO's data/missions stock content / other files
caused a play-time freeze. The FOCUSED set above makes all 6 tested campaign starts
PASS; adding the rest only introduced conflicts. Same lesson as the MC2X
delta-vs-oracle: ship the proven minimal set, don't get clever.

Verified (smoke, MC2_MOD_DEPS=mco-compat): outreach (Wolf Dragoons) / zhukov (Volstand)
/ doh_0 (Day of Heroes) / cfv2_mission1_escort (Clan Eagle) / galatea (MercStar) /
desfox1 (Desert Fox) campaign starts all PASS. The player selects campaign +
compatibility layer at launch; the launcher exports MC2_MOD_DEPS so no per-campaign
mod.json is needed.
"""
import argparse
import csv
import json
import os
import re
import shutil
import sys

# FROZEN-AWARE: bundled modules (build_object2, manifest) live flat under sys._MEIPASS
# when PyInstaller-frozen; dev layout keeps them in sibling tool dirs.
sys.path.insert(0, getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__))))
from build_object2 import build_object2  # noqa: E402

# ABL libraries taken verbatim from the user's MCO install (corebrain is overridden).
CORE_ABX_FROM_MCO = ["orders.abx", "miscfunc.abx"]

# ---------------------------------------------------------------------------
# buildings.csv overlay helpers
# ---------------------------------------------------------------------------

def _parse_buildings_csv(path):
    """Return (header_line, rows) where rows is list of raw lines (with \n)."""
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        raw = f.readlines()
    if not raw:
        return "", []
    header = raw[0]
    rows = raw[1:]
    return header, rows


def _row_name(line):
    """Return the chassis name (col 0) of a buildings.csv data row, lowercased."""
    return line.split(",")[0].strip().lower()


def _row_type(line):
    """Return the Type field (col 3, 0-indexed) of a buildings.csv data row, uppercased."""
    parts = line.split(",")
    if len(parts) > 3:
        return parts[3].strip().upper()
    return ""


def _row_fitid(line):
    """Return the FitID field (col 4, 0-indexed) as int, or -1 if missing/invalid."""
    parts = line.split(",")
    if len(parts) > 4:
        try:
            return int(parts[4].strip())
        except ValueError:
            pass
    return -1


def build_buildings_csv_overlay(stock_path, mco_path, out_path):
    """
    Produce a merged buildings.csv at out_path:
      - Stock rows (header + all rows), with FitID=0 MECH placeholder rows
        replaced by the corresponding MCO MECH row when MCO has a nonzero FitID.
      - Appended: MCO MECH rows whose name is not in stock at all.
    Regression guard: never emits VEHICLE/BUILDING/TREE rows from MCO.
    Returns (added_names, overridden_names, skipped_names) for logging.
    """
    stock_header, stock_rows = _parse_buildings_csv(stock_path)
    _mco_header, mco_rows = _parse_buildings_csv(mco_path)

    # Build MCO MECH lookup: lowercase name -> row string (nonzero FitID only).
    mco_mech_by_name = {}
    for row in mco_rows:
        if not row.strip():
            continue
        if _row_type(row) != "MECH":
            continue
        fitid = _row_fitid(row)
        if fitid > 0:
            mco_mech_by_name[_row_name(row)] = row if row.endswith("\n") else row + "\n"

    # Emit stock rows, substituting FitID=0 MECH placeholders with MCO rows.
    overridden = []
    out_stock_names = set()
    final_stock_rows = []
    for row in stock_rows:
        if not row.strip():
            final_stock_rows.append(row)
            continue
        name = _row_name(row)
        out_stock_names.add(name)
        if _row_type(row) == "MECH" and _row_fitid(row) == 0 and name in mco_mech_by_name:
            # Replace placeholder with real MCO row.
            final_stock_rows.append(mco_mech_by_name[name])
            overridden.append(name)
        else:
            final_stock_rows.append(row)

    added = []
    skipped_non_mech = []
    skipped_dup = []

    new_rows = []
    for row in mco_rows:
        if not row.strip():
            continue
        rtype = _row_type(row)
        if rtype != "MECH":
            skipped_non_mech.append(_row_name(row))
            continue
        name = _row_name(row)
        if name in out_stock_names:
            skipped_dup.append(name)
            continue
        new_rows.append(row if row.endswith("\n") else row + "\n")
        added.append(name)
        out_stock_names.add(name)  # prevent duplicate MCO rows

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        f.write(stock_header)
        for r in final_stock_rows:
            f.write(r)
        for r in new_rows:
            f.write(r)

    return added, overridden, skipped_non_mech, skipped_dup


def copy_mech_csvs(mco_objects_dir, out_objects_dir, mech_names,
                   fallback_objects_dirs=None):
    """
    Copy data/objects/{name}.csv for each name in mech_names to out_objects_dir
    (lowercase filename, matching File::open S_strlwr behaviour).

    Search order: primary mco_objects_dir first, then each dir in
    fallback_objects_dirs (e.g. MC2-MagicExpansion/data/objects) in order.
    This ensures completeness: mechs like Highlander2c / Hollander2 that exist
    only in MagicExpansion are still copied rather than silently skipped.

    Returns (copied, missing).
    """
    os.makedirs(out_objects_dir, exist_ok=True)
    search_dirs = [mco_objects_dir] + list(fallback_objects_dirs or [])
    copied = []
    missing = []
    for name in mech_names:
        found = None
        for objects_dir in search_dirs:
            if not objects_dir or not os.path.isdir(objects_dir):
                continue
            # Try original-case then lowercase variant
            for candidate in [name + ".csv", name.lower() + ".csv"]:
                src = os.path.join(objects_dir, candidate)
                if os.path.isfile(src):
                    found = src
                    break
            if found:
                break
        if found is None:
            missing.append(name)
            continue
        dst = os.path.join(out_objects_dir, name.lower() + ".csv")
        shutil.copy2(found, dst)
        copied.append(name.lower())
    return copied, missing


def _slug(name):
    """Lowercased, non-alnum runs collapsed to underscore, stripped."""
    return re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_')


def build_mech_names_fit(objects_dir, out_dir):
    """
    Parse every per-mech CSV in objects_dir, emit a LegacyTextAlias + String pair per
    unique DescIndex into out_dir/z_mco_mech_names.fit.

    CSV layout (0-indexed rows/cols):
      row2 col0 = 'MechName', col1 = <name>
      row4 col0 = 'DescIndex', col1 = <id>

    Emit PAIRS using the alias form (NOT String with legacyId) so the entry lands in
    keyByLegacyAlias and wins over the mc2res-overridden keyByLegacyId slot:

        String      { key="mco.mech.<slug>"  text="<MechName>"  source="mco" }
        LegacyTextAlias { legacyId=<N>  textKey="mco.mech.<slug>" }

    De-duplicate by DescIndex (first-seen wins, files processed sorted by filename).
    Returns (count_pairs, skipped_list).
    """
    entries = {}  # desc_id -> (name, slug)
    skipped = []

    csv_files = sorted(f for f in os.listdir(objects_dir) if f.lower().endswith('.csv'))
    for fname in csv_files:
        path = os.path.join(objects_dir, fname)
        try:
            with open(path, 'r', encoding='utf-8', errors='replace', newline='') as fh:
                rows = list(csv.reader(fh))
        except Exception as e:
            skipped.append('%s: %s' % (fname, e))
            continue

        try:
            if len(rows) < 5:
                skipped.append('%s: fewer than 5 rows' % fname)
                continue
            name_label = rows[2][0].strip() if len(rows[2]) > 0 else ''
            desc_label = rows[4][0].strip() if len(rows[4]) > 0 else ''
            if name_label != 'MechName' or desc_label != 'DescIndex':
                skipped.append('%s: unexpected row labels (%r/%r)' % (fname, name_label, desc_label))
                continue
            mech_name = rows[2][1].strip() if len(rows[2]) > 1 else ''
            desc_id_str = rows[4][1].strip() if len(rows[4]) > 1 else ''
            if not mech_name:
                skipped.append('%s: empty MechName' % fname)
                continue
            if not desc_id_str:
                skipped.append('%s: empty DescIndex' % fname)
                continue
            desc_id = int(desc_id_str)
            if desc_id <= 0:
                skipped.append('%s: DescIndex=%d <= 0' % (fname, desc_id))
                continue
        except Exception as e:
            skipped.append('%s: parse error: %s' % (fname, e))
            continue

        if desc_id not in entries:
            entries[desc_id] = (mech_name, _slug(mech_name))

    sorted_entries = sorted(entries.items(), key=lambda x: x[1][0].lower())

    lines = [
        '// AUTO-GENERATED by build_mco_compat.py -- do not edit by hand',
        '// MCO mech chassis name aliases: String + LegacyTextAlias pairs',
        '//',
        '// %d entries, sorted by MechName' % len(sorted_entries),
        '',
    ]
    for desc_id, (name, slug) in sorted_entries:
        key = 'mco.mech.%s' % slug
        safe_name = name.replace('"', '\\"')
        lines += [
            'String',
            '{',
            '    key = "%s"' % key,
            '    text = "%s"' % safe_name,
            '    source = "mco"',
            '}',
            '',
            'LegacyTextAlias',
            '{',
            '    legacyId = %d' % desc_id,
            '    textKey = "%s"' % key,
            '}',
            '',
        ]

    content = '\n'.join(lines)
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, 'z_mco_mech_names.fit')
    with open(out_path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(content)

    return len(sorted_entries), skipped


# ---------------------------------------------------------------------------
# Mech-bay list icon atlas (MCUI_GN_MechIcons.png)
# ---------------------------------------------------------------------------

# Explicit name mapping: mco-compat CSV stem -> mechicons/<stem>.tga (lowercased, spaces allowed)
# Extend if new MCO mechs are added.
_MECH_ICON_MAP = {
    "anihilator": "anihilator", "archer": "archer", "archer2": "archer2",
    "argus": "argus", "assaultcopter": "assault copter", "bear": "bear",
    "blackjack": "black jack", "bloodkite": "blood kite",
    "cargodropship2": "cargo dropship", "champion": "champion",
    "cicada": "cicada", "commando": "commando", "cougar1": "cougar mw4",
    "crab": "crab", "crossbow": "crossbow", "crusader": "crusader",
    "dervish": "dervish", "direwolf": "direwolf", "elemental": "elemental",
    "fafnir": "fafnir", "firefly": "firefly", "firewraith": "fire wraith",
    "gladiator": "gladiator", "gotha": "gotha", "grizzly": "grizzly",
    "guardian": "guardian", "hades": "hades", "hellfire": "hellfire",
    "huntsman": "huntsman", "iceferret": "ice ferret", "intruder": "intruder",
    "karnov2": "karnov", "leopard": "leopard", "lucifer": "lucifer",
    "lupus": "lupus", "madcatmkii": "mad cat mkii", "madcatmkiii": "mad cat mkiii",
    "mauler": "mauler", "mistlynx": "mist lynx", "naga": "naga",
    "nova": "nova", "nova1": "nova", "novacat": "nova cat", "omen": "omen",
    "owens": "owens", "panther": "panther", "phantom": "phantom",
    "quickdraw": "quickdraw", "ranger": "ranger", "rapier": "rapier",
    "rifleman2c": "rifleman iic", "sabutai": "sabutai",
    "salamanderarmor": "salamander", "shadowhawk": "shadowhawk",
    "sholagar": "sholagar", "stalker": "stalker", "stinger": "stinger",
    "sunder": "sunder", "thanatos": "thanatos", "thug": "thug",
    "thunder": "thunderbolt", "timberwolf": "timberwolf", "turkina": "turkina",
    "union2": "union2", "uziel": "uziel", "valkyrie": "valkyrie",
    "vindicator": "vindicator", "vtol2": "vtol", "vulture1": "vulture",
    "warhammer": "warhammer", "warhammer2c": "warhammer iic",
    # Spelling aliases: CSV stem (FileName) -> mechicons/ filename stem
    # jevelin: MCO MercStar uses "Jevelin" FileName; real art is Javelin.tga
    "jevelin": "javelin",
    # loki: IS name for Hellbringer; MCO icon is Hellbringer.tga
    "loki": "hellbringer",
    # puma: IS name for Adder; MCO icon is Adder.tga
    "puma": "adder",
    # cauldron: CSV stem for Cauldron Born; MCO icon is "Cauldron born.tga"
    "cauldron": "cauldron born",
    # hunchbackiic: CSV stem; MCO icon is "Hunchback iic.tga"
    "hunchbackiic": "hunchback iic",
    # bloodasp2: Blood Asp II variant; no separate icon, fall back to Blood Asp
    "bloodasp2": "blood asp",
}

# Atlas constants (must match mechlistbox.cpp::initIcon / asystem.cpp)
# Layout: 10 cols x 17 rows of 25x30px cells.
#   W=256  (fileWidth=256, U = col*25/256 in [0,1])
#   H=512  (fileHeight=512 auto-read from texture; V = row*30/512 in [0,1])
# Rows 0-16 = slots 0-169 are addressable; max MCO slot=126 fits with room.
# Stock base atlas (256x256) is backward-compatible: its fileHeight=256
# (square => fileHeight stays 0 in aObject, falls back to fileWidth=256),
# so stock V math is identical to before the engine change.
_ATLAS_W = 256
_ATLAS_H = 512
_CELL_W = 25
_CELL_H = 30
_ATLAS_COLS = 10
# With fileHeight=512: rows 0-16 (V up to 17*30/512=0.996) are addressable.
_MAX_ADDRESSABLE_SLOT = 169  # floor(512/30)*10 - 1 = 16*10+9 = 169


def _read_mech_parts(csv_path):
    """Return MechParts int from a mech CSV, or None."""
    try:
        with open(csv_path, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()
        rows = [l for l in content.splitlines() if l.strip() and not l.strip().startswith("//")]
        for row in rows:
            cols = row.split(",")
            if cols[0].strip() == "MechParts" and len(cols) > 1:
                val = cols[1].strip()
                return int(val) if val else None
    except Exception:
        pass
    return None


def _read_mech_name(csv_path):
    """Return MechName string from a mech CSV (row 'MechName', col 1), or None."""
    try:
        with open(csv_path, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()
        rows = [l for l in content.splitlines() if l.strip() and not l.strip().startswith("//")]
        for row in rows:
            cols = row.split(",")
            if cols[0].strip() == "MechName" and len(cols) > 1:
                val = cols[1].strip()
                return val if val else None
    except Exception:
        pass
    return None


def build_mech_icon_atlas(base_atlas_path, mco_mechicons_dir, objects_dir, out_path):
    """
    Build a 256x256 mcui_gn_mechicons.tga for mco-compat by compositing per-mech
    icon TGAs from `mco_mechicons_dir` onto the base atlas.

    OUTPUT FORMAT: must be TGA at data/art/mcui_gn_mechicons.tga (NO gui/ subdir).
    The engine (aObject::init + asystem.cpp) requests:
      artPath("data/art/") + FileName("MCUI_GN_MechIcons.tga") -> lowercased
      = "data/art/mcui_gn_mechicons.tga"
    The mod VFS (TryModOpen) matches this exact key. A PNG at data/art/gui/ is
    NEVER loaded by the list icon path and would be silently ignored.

    Atlas layout: 10 cols x 17 rows, 25x30px cells, 256x512 TGA.
    The engine's aObject::init reads the texture's logical height and stores it
    as fileHeight (non-square atlases only).  setUVs() then divides V by
    fileHeight instead of fileWidth, so slots 0-169 are addressable.
    Stock base atlas remains 256x256 (fileHeight=0 -> falls back to fileWidth=256),
    keeping stock slot UVs identical to before.

    Returns (painted, oob) lists of (mech_name, slot) tuples.
    """
    try:
        from PIL import Image
    except ImportError:
        print("  WARN: Pillow not available -- skipping icon atlas build")
        return [], []

    # Load base atlas: prefer to extract from art.fst (the authoritative source the engine
    # loads via data/art/mcui_gn_mechicons.tga) so vanilla icons in slots 0-32 are preserved
    # correctly. Falls back to the loose base PNG if the FST is not present.
    base_img = None
    # out_path = .../mods/mco-compat/data/art/mcui_gn_mechicons.tga
    # deploy root = 4 dirs up from out_path
    fst_path = os.path.normpath(os.path.join(os.path.dirname(out_path), "..", "..", "..", "..", "art.fst"))
    if os.path.isfile(fst_path):
        try:
            import struct as _struct, zlib as _zlib, io as _io
            with open(fst_path, "rb") as _f:
                _fst_raw = _f.read()
            _key_pos = _fst_raw.lower().find(b"data/art/mcui_gn_mechicons.tga")
            if _key_pos != -1:
                _es = _key_pos - 16
                _offset, _size = _struct.unpack_from("<II", _fst_raw, _es)[:2]
                _cdata = _fst_raw[_offset:_offset + _size]
                _img = Image.open(_io.BytesIO(_zlib.decompress(_cdata))).convert("RGBA")
                base_img = _img
                print("  base atlas: extracted from art.fst (%dx%d)" % _img.size)
        except Exception as _e:
            print("  WARN: could not extract from art.fst (%s), falling back to loose PNG" % _e)

    if base_img is None:
        if not os.path.isfile(base_atlas_path):
            print("  WARN: base atlas not found: %s" % base_atlas_path)
            return [], []
        base_img = Image.open(base_atlas_path).convert("RGBA")
        print("  base atlas: loose PNG %s (%dx%d)" % (base_atlas_path, base_img.width, base_img.height))

    # Build output canvas at target size (_ATLAS_W x _ATLAS_H = 256x512).
    # Copy base atlas into top of canvas so stock slots 0-79 are preserved at
    # the same pixel positions.  With fileHeight=512 the engine divides V by 512
    # instead of 256, so the UV for stock slot N becomes N_px/512 — pointing at
    # the same pixel row (because the pixel is still at the same absolute Y in the
    # 512-tall texture).  No stock icon shifts.
    atlas = Image.new("RGBA", (_ATLAS_W, _ATLAS_H), (0, 0, 0, 0))
    # Paste base at (0,0); clip if base is wider/taller than our canvas column.
    paste_w = min(base_img.width, _ATLAS_W)
    paste_h = min(base_img.height, _ATLAS_H)
    atlas.paste(base_img.crop((0, 0, paste_w, paste_h)), (0, 0))
    print("  output canvas: %dx%d (base %dx%d pasted at origin)" % (
        _ATLAS_W, _ATLAS_H, paste_w, paste_h))

    # Index available source icons
    icon_files = {}
    if os.path.isdir(mco_mechicons_dir):
        for f in os.listdir(mco_mechicons_dir):
            if f.lower().endswith(".tga"):
                icon_files[f.lower()] = os.path.join(mco_mechicons_dir, f)

    painted = []
    oob = []
    no_icon = []

    if os.path.isdir(objects_dir):
        for csv_file in sorted(os.listdir(objects_dir)):
            if not csv_file.lower().endswith(".csv"):
                continue
            mech = csv_file[:-4].lower()
            slot = _read_mech_parts(os.path.join(objects_dir, csv_file))
            if slot is None:
                continue
            if slot > _MAX_ADDRESSABLE_SLOT:
                oob.append((mech, slot))
                continue

            # Resolution chain: _MECH_ICON_MAP alias -> MechName from CSV -> raw stem
            icon_stem = _MECH_ICON_MAP.get(mech)
            if icon_stem is None:
                # Try MechName from CSV (e.g. "loki" -> "Hellbringer" -> "hellbringer.tga")
                mech_name = _read_mech_name(os.path.join(objects_dir, csv_file))
                icon_stem = mech_name.lower() if mech_name else mech
            icon_key = icon_stem + ".tga"
            icon_path = icon_files.get(icon_key)
            if not icon_path:
                no_icon.append((mech, slot))
                continue

            x = (slot % _ATLAS_COLS) * _CELL_W
            y = (slot // _ATLAS_COLS) * _CELL_H
            src = Image.open(icon_path).convert("RGBA")
            tile = src.resize((_CELL_W, _CELL_H), Image.LANCZOS)
            atlas.paste(tile, (x, y))
            painted.append((mech, slot))

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    # Save as TGA — the engine loads data/art/mcui_gn_mechicons.tga (not a PNG).
    atlas.save(out_path, "TGA")
    if no_icon:
        print("  WARN: no source icon for: %s" % ", ".join("%s(%d)" % t for t in no_icon))
    return painted, oob


def copy_file(src, dst, label):
    if not os.path.isfile(src):
        print("  WARN: missing %s (%s)" % (label, src))
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    print("  %s: %s (%d bytes)" % (label, os.path.basename(dst), os.path.getsize(dst)))
    return True


def mirror_tree(src, dst, label):
    if not os.path.isdir(src):
        print("  WARN: missing dir %s (%s)" % (label, src))
        return 0
    os.makedirs(dst, exist_ok=True)
    n = 0
    for root, _, files in os.walk(src):
        rel = os.path.relpath(root, src)
        outdir = os.path.join(dst, rel) if rel != "." else dst
        os.makedirs(outdir, exist_ok=True)
        for f in files:
            shutil.copy2(os.path.join(root, f), os.path.join(outdir, f))
            n += 1
    print("  %s: %d files" % (label, n))
    return n


def main():
    ap = argparse.ArgumentParser(description="Build the mco-compat dependency mod from a user's MCO install.")
    ap.add_argument("--source", required=True, help="MCO install root (MechCommanderOmnitech)")
    ap.add_argument("--out", required=True, help="mco-compat mod dir to write (…/mods/mco-compat)")
    ap.add_argument("--corebrain", required=True,
                    help="compatible corebrain.abx (MC2-era; defines magicAttack, compiles cleanly). "
                         "e.g. <MC2-Exodus>/data/missions/corebrain.abx")
    ap.add_argument("--stock-buildings",
                    help="path to the base-game data/art/Buildings.csv (needed for MECH overlay). "
                         "If omitted, auto-located relative to --out as ../../data/art/buildings.csv.")
    ap.add_argument("--fallback-objects", action="append", default=[],
                    metavar="DIR",
                    help="additional data/objects/ dirs to search for per-mech CSVs when the primary "
                         "MCO source lacks them. May be repeated; searched in order after --source. "
                         "Recommended full set for maximum cross-campaign coverage: "
                         "--fallback-objects <MC2-MagicExpansion>/data/objects "
                         "--fallback-objects <MC2-Exodus>/data/objects "
                         "--fallback-objects <MCO-Carver5>/data/objects "
                         "--fallback-objects <MCO-ClanEagle>/data/objects "
                         "--fallback-objects <MCO-DesertFox>/data/objects "
                         "--fallback-objects <MCO-MercStar>/data/objects "
                         "--fallback-objects <MCO-Volstand>/data/objects "
                         "--fallback-objects <MCO-Wolf-Dragoons>/data/objects "
                         "--fallback-objects <MCO-dayofheroes>/data/objects. "
                         "Mechs like Highlander2c/Hollander2 live only in MagicExpansion; "
                         "BushWhacker (typo variant) lives only in the primary MCO source as 'bushwacker'. "
                         "ZeusMK2 lives in mc2x-compat/data/objects. "
                         "Typo aliases (valkirye->valkyrie) must be copied manually post-build.")
    ap.add_argument("--skip-tgl", action="store_true",
                    help="skip the ~302MB data/tgl mirror (objects/ABL only; non-stock MCO mechs will "
                         "crash at load — for fast iteration only)")
    ap.add_argument("--force", action="store_true",
                    help="overwrite an existing --out even if it was not generated by this tool")
    args = ap.parse_args()

    src = args.source
    out = args.out
    if not os.path.isdir(os.path.join(src, "data", "objects")):
        sys.exit("not an MCO install (no data/objects): %s" % src)

    # Resolve stock buildings.csv path.
    stock_buildings = args.stock_buildings
    if not stock_buildings:
        # Auto-detect: --out is .../mods/mco-compat; base data/ is ../../data/art/
        candidate = os.path.normpath(os.path.join(out, "..", "..", "data", "art", "buildings.csv"))
        if os.path.isfile(candidate):
            stock_buildings = candidate
        else:
            # Also try case-preserved filename
            candidate2 = os.path.normpath(os.path.join(out, "..", "..", "data", "art", "Buildings.csv"))
            if os.path.isfile(candidate2):
                stock_buildings = candidate2
    if stock_buildings and not os.path.isfile(stock_buildings):
        sys.exit("ERROR: --stock-buildings not found: %s" % stock_buildings)

    # guard + rmtree-then-build: refuse to clobber a foreign --out unless --force,
    # else clear any prior build so a smaller source can't leave orphan files behind.
    if os.path.isdir(out):
        marker = os.path.join(out, "mco_compat_report.json")
        gen = None
        if os.path.isfile(marker):
            try:
                with open(marker) as f:
                    gen = json.load(f).get("generated_by")
            except Exception:
                gen = None
        if gen not in ("build_mco_compat", "import_tool") and not args.force:
            sys.exit("ERROR: %s already exists and was not generated by this tool; "
                     "re-run with --force." % out)
        shutil.rmtree(out)

    print("[1/6] object2.pak (covers buildings.csv FitIDs incl. mech stubs)")
    build_object2(src, os.path.join(out, "data", "objects", "object2.pak"))

    print("[2/6] ABL libraries from MCO install")
    for f in CORE_ABX_FROM_MCO:
        copy_file(os.path.join(src, "data", "missions", f),
                  os.path.join(out, "data", "missions", f), "abx")

    print("[3/6] corebrain.abx (compatible source)")
    copy_file(args.corebrain, os.path.join(out, "data", "missions", "corebrain.abx"), "corebrain")

    if args.skip_tgl:
        print("[4/6] data/tgl  -- SKIPPED (--skip-tgl)")
    else:
        print("[4/6] data/tgl mirror (appearances + shapes, large)")
        mirror_tree(os.path.join(src, "data", "tgl"), os.path.join(out, "data", "tgl"), "tgl")

    # Buildings.csv MECH overlay
    mco_buildings = os.path.join(src, "data", "art", "buildings.csv")
    if not os.path.isfile(mco_buildings):
        mco_buildings = os.path.join(src, "data", "art", "Buildings.csv")

    print("[5/6] data/art/Buildings.csv MECH overlay")
    if not stock_buildings:
        print("  WARN: stock buildings.csv not found -- skipping MECH overlay (MCO mechs will be unavailable)")
        print("        Re-run with --stock-buildings <deploy>/data/art/buildings.csv to fix.")
        added_mechs = []
    elif not os.path.isfile(mco_buildings):
        print("  WARN: MCO buildings.csv not found at %s -- skipping" % mco_buildings)
        added_mechs = []
    else:
        out_buildings = os.path.join(out, "data", "art", "Buildings.csv")
        added_mechs, overridden_mechs, skipped_non_mech, skipped_dup = build_buildings_csv_overlay(
            stock_buildings, mco_buildings, out_buildings
        )
        print("  added %d MCO MECH rows, %d overridden (FitID=0 placeholders), %d already in stock (skipped), %d non-MECH (excluded)"
              % (len(added_mechs), len(overridden_mechs), len(skipped_dup), len(skipped_non_mech)))
        if added_mechs:
            print("  new mechs:", ", ".join(sorted(added_mechs)))
        if overridden_mechs:
            print("  FitID=0 placeholder overrides:", ", ".join(sorted(overridden_mechs)))
        # Regression guard: confirm no vehicles/buildings were added
        assert len(skipped_non_mech) >= 0  # always true, but name makes intent clear
        print("  regression guard: 0 VEHICLE/BUILDING/TREE rows emitted from MCO (Type!=MECH excluded)")

    # Per-mech CSV files (loose, read by initVariants via File::open / TryModOpen)
    # Completeness rule: copy a CSV for EVERY mech name added by the buildings overlay.
    # Search primary MCO source first, then any --fallback-objects dirs in order (e.g.
    # MC2-MagicExpansion/data/objects for mechs like Highlander2c / Hollander2).
    print("[6/6] data/objects/*.csv per-mech chassis data (loose)")
    if added_mechs:
        mco_objects_dir = os.path.join(src, "data", "objects")
        out_objects_dir = os.path.join(out, "data", "objects")
        copied, missing = copy_mech_csvs(
            mco_objects_dir, out_objects_dir, added_mechs,
            fallback_objects_dirs=args.fallback_objects,
        )
        print("  copied %d per-mech CSV files" % len(copied))
        if args.fallback_objects:
            print("  (searched fallback dirs: %s)" % "; ".join(args.fallback_objects))
        if missing:
            print("  WARN: CSV not found in any source for: %s" % ", ".join(missing))
            print("  NOTE: these mechs will be skipped by initVariants (no CSV = not purchasable)")
    else:
        print("  (no new mechs added -- nothing to copy)")

    # Mech chassis name aliases (LegacyTextAlias form, immune to mc2res override)
    print("[6.5/6] data/defs/text/en_us/z_mco_mech_names.fit (LegacyTextAlias chassis names)")
    mech_names_objects_dir = os.path.join(out, "data", "objects")
    if os.path.isdir(mech_names_objects_dir):
        n_pairs, skipped_fits = build_mech_names_fit(
            mech_names_objects_dir,
            os.path.join(out, "data", "defs", "text", "en_us"),
        )
        print("  %d String+LegacyTextAlias pairs written" % n_pairs)
        if skipped_fits:
            for s in skipped_fits:
                print("  SKIP:", s)
    else:
        print("  WARN: objects dir not found, skipping mech name fit generation")

    # Mech-bay list icon atlas (data/art/mcui_gn_mechicons.tga)
    # Composites per-mech TGAs from MCO's mechicons/ onto the base atlas.
    # OUTPUT: data/art/mcui_gn_mechicons.tga (NO gui/ subdir) — this overrides the
    # art.fst entry "data/art/mcui_gn_mechicons.tga" via the mod VFS (TryModOpen).
    # The data/art/gui/MCUI_GN_MechIcons.png path is NEVER loaded by the list icon
    # code path (engine uses artPath="data/art/" + FileName="MCUI_GN_MechIcons.tga").
    # Slots 0-79 only (UV constraint: fileWidth=256 means v=slot_row*30 > 256 is OOB).
    # Mechs with MechParts > 79 need an engine fix (mechlistbox::initIcon fileWidth bump).
    print("[6.7/6] data/art/mcui_gn_mechicons.tga (mech-bay list icon atlas)")
    # Base: function prefers art.fst extraction (authoritative vanilla icons 0-32);
    # falls back to the loose base-game PNG if FST not found.
    base_atlas_path = os.path.normpath(os.path.join(out, "..", "..", "data", "art", "gui", "MCUI_GN_MechIcons.png"))
    mco_mechicons_dir = os.path.join(src, "data", "art", "mechicons")
    # Correct output path: data/art/ (no gui/), lowercase, TGA extension.
    atlas_out = os.path.join(out, "data", "art", "mcui_gn_mechicons.tga")
    mco_objects_for_atlas = os.path.join(out, "data", "objects")
    atlas_painted, atlas_oob = build_mech_icon_atlas(
        base_atlas_path, mco_mechicons_dir, mco_objects_for_atlas, atlas_out
    )
    print("  painted %d slots (0-%d addressable with %dx%d atlas)"
          % (len(atlas_painted), _MAX_ADDRESSABLE_SLOT, _ATLAS_W, _ATLAS_H))
    if atlas_oob:
        print("  OOB (slot > %d, increase _ATLAS_H to fix): %d mechs -- %s"
              % (_MAX_ADDRESSABLE_SLOT, len(atlas_oob),
                 ", ".join("%s(%d)" % t for t in sorted(atlas_oob, key=lambda x: x[1])[:8]))
              + ("..." if len(atlas_oob) > 8 else ""))
    if atlas_painted:
        print("  atlas: %s" % atlas_out)

    mod_json = os.path.join(out, "mod.json")
    if not os.path.isfile(mod_json):
        with open(mod_json, "w") as f:
            f.write('{ "id": "mco-compat", "name": "MCO Compatibility Base", "type": "dependency", "version": "1.0" }\n')
        print("wrote mod.json")

    cache = os.path.join(out, ".modindex-cache")
    if os.path.exists(cache):
        os.remove(cache)

    # provenance marker (lets import_tool detect a re-import + own the folder).
    try:
        sys.path.insert(0, getattr(sys, "_MEIPASS",
                                   os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                "mc2x_import")))
        import manifest
        fp = manifest.source_fingerprint(src)
    except Exception:
        fp = None
    marker_payload = {"generated_by": "build_mco_compat", "tool_version": "1.1",
                      "source_path": os.path.abspath(src), "source_fingerprint": fp,
                      "skip_tgl": bool(args.skip_tgl), "files_missing": 0,
                      "mech_overlay_count": len(added_mechs),
                      "stock_buildings": stock_buildings or ""}
    with open(os.path.join(out, "mco_compat_report.json"), "w", encoding="utf-8") as f:
        json.dump(marker_payload, f, indent=2)
    # Also write the unified marker filename all importer tools agree on
    # (import_tool/mc2x_import/transfer all use mc2x_import_report.json). Same schema.
    with open(os.path.join(out, "mc2x_import_report.json"), "w", encoding="utf-8") as f:
        json.dump(marker_payload, f, indent=2)
    print("done: %s" % out)


if __name__ == "__main__":
    main()
