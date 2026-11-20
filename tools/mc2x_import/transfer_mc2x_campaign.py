#!/usr/bin/env python3
"""
transfer_mc2x_campaign.py -- turn a full MC2X campaign install into a drop-in
campaign mod that runs on top of the mc2x-compat compatibility layer.

An MC2X install (e.g. <mc2x-source>) keeps its actual missions inside FST archives
at the install root (mission.fst, art.fst, tgl.fst, ...) plus a small loose data/
tree of campaign-specific overrides (warrior brains, purchase files, the ops/
campaign script, pilots, campaign music). Stock + MC2X shared content already
lives in the deploy base + mc2x-compat, so the campaign mod only needs:

  1. the campaign missions -> extracted from mission.fst (data\\missions\\*.fit/.pak)
  2. the loose campaign delta -> files under the install's data/ whose bytes differ
     from the first dependency providing that path (warriors, purchase, ops
     script, pilots, campaign-only art/sound/csv).

Each MC2X campaign retains its own loose object2.pak, even when identical to a
reference. data/art/buildings.csv stays EXCLUDED (overlaying it crashes logistics —
the same conflict seen with the MCO "everything diff" experiment).

Bytes come from the user's own install; nothing is redistributed. The player
selects this campaign + the mc2x-compat layer at launch (MC2_ACTIVE_MOD +
MC2_MOD_DEPS) — no mod.json needed.

Usage:
  py -3 transfer_mc2x_campaign.py --source <mc2x-source> \
      --refs <deploy>/mods/mc2x-compat/data <deploy>/data \
      --out  <deploy>/mods/MC2X-DEE
"""
import argparse
from pathlib import Path
import os
import sys

# FROZEN-AWARE: bundled modules live under sys._MEIPASS when PyInstaller-frozen.
sys.path.insert(0, getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__))))
import fst  # noqa: E402

EXCLUDE_REL = {
    "data/objects/object2.pak",
    "data/art/buildings.csv",
}

# Core engine GUI assets that must NEVER be overridden by a campaign mod.
# These ship in mc2x-compat (canonical versions from MC2X 1.2+) and the engine
# reads them from there.  Older MC2X installs (DWE, HPE, DEE era) ship
# incompatible old-format versions inside art.fst that cause jumbled GUI, no
# mouse cursor, and pink mech bay when the mod overrides the base.
# Explicit exclusion prevents poisoning even if
# refs are incomplete.
CORE_GUI_EXCLUDE = {
    # buttonlayout FITs — control positions/sprites; old format = jumbled layout
    "data/art/buttonlayout640.fit",
    "data/art/buttonlayout800.fit",
    "data/art/buttonlayout1024.fit",
    "data/art/buttonlayout1280.fit",
    "data/art/buttonlayout1366.fit",
    "data/art/buttonlayout1440.fit",
    "data/art/buttonlayout1600.fit",
    "data/art/buttonlayout1680.fit",
    "data/art/buttonlayout1920.fit",
    # cursor FITs — old format = no mouse cursor visible
    "data/art/cursors.fit",
    "data/art/cursorsa.fit",
    # cursor TGA atlases
    "data/art/cursors1.tga",  "data/art/cursors1a.tga",
    "data/art/cursors2.tga",  "data/art/cursors2a.tga",
    "data/art/cursors3.tga",  "data/art/cursors3a.tga",
    "data/art/cursors4.tga",  "data/art/cursors4a.tga",
    "data/art/cursors5.tga",  "data/art/cursors5a.tga",
    "data/art/cursors6.tga",  "data/art/cursors6a.tga",
    # loading screen FITs (engine version-sensitive)
    "data/art/mcl_loadingscreen.fit",
    "data/art/mcl_loadingscreen_640.fit",
    "data/art/mcl_loadingscreen_1024.fit",
    "data/art/mcl_loadingscreen_1280.fit",
    "data/art/mcl_loadingscreen_1366.fit",
    "data/art/mcl_loadingscreen_1440.fit",
    "data/art/mcl_loadingscreen_1600.fit",
    "data/art/mcl_loadingscreen_1680.fit",
    "data/art/mcl_loadingscreen_1920.fit",
    # options FITs (engine-version-sensitive layout)
    "data/art/mcl_options.fit",
    "data/art/mcl_optionsgameplay.fit",
    # shared UI micro-textures / atlas strips (version-sensitive or same in all installs)
    "data/art/a_compass_small.tga",
    "data/art/blip.tga",
    "data/art/circle.tga",
    "data/art/mcui_scroll.tga",
    # mcui_low5.tga: sprite strip whose width changed between old (625px) and new (675px)
    # MC2X installs; new buttonlayout FITs assume 675px -> old strip = garbled sprites / pink UI
    "data/art/mcui_low5.tga",
}


def linked(path):
    """Reject links in the path itself or any of its parent directories."""
    path = Path(path)
    return any(p.is_symlink() or p.is_junction() for p in (path, *path.parents))


def loose_asset(layer, name):
    """Exact spelling first, then a bounded case-insensitive directory walk."""
    layer = Path(layer)
    if linked(layer):
        return None
    exact = layer / name
    if not linked(exact) and exact.is_file():
        return exact
    current = layer
    for part in name.replace("\\", "/").split("/"):
        if not current.is_dir():
            return None
        matches = sorted((p for p in current.iterdir() if p.name.casefold() == part.casefold()),
                         key=lambda p: (p.name != part, p.name))
        current = next((p for p in matches if not linked(p)), None)
        if current is None:
            return None
    return current if current.is_file() else None


def asset_bytes(layers, name, archive_entries=None):
    """Loose files win; archives use stable case-insensitive filename order."""
    reader = archive_entries or fst.fst_entries
    for layer in map(Path, layers):
        if linked(layer) or not layer.is_dir():
            continue
        path = loose_asset(layer, name)
        if path is not None:
            return path.read_bytes()
        archives = sorted((p for p in layer.glob("*.fst") if not linked(p)),
                          key=lambda p: (p.name.casefold(), p.name))
        for archive in archives:
            for off, comp, real, entry, lz, data in reader(archive):
                if entry.replace("\\", "/").casefold() == name.casefold():
                    return fst.decompress_entry(off, comp, real, lz, data)
    return None


def rel_norm(p):
    return p.replace("\\", "/").lower()


def in_refs(name, refs, content):
    rel = name.replace("\\", "/")
    # strip leading "data/" so we can test against ref dirs that ARE the data dir
    sub = rel[5:] if rel.lower().startswith("data/") else rel
    for ref in refs:
        for candidate in (sub, rel):
            path = os.path.join(ref, candidate.replace("/", os.sep))
            if os.path.isfile(path):
                with open(path, "rb") as stream:
                    return stream.read() == content
    return False


def extract_fst(fst_path, out_root, refs=None, drop_upscale=False, gui_canon=None):
    """Extract entries of an FST into out_root, preserving its data\\... path.
    If refs is given, skip entries identical to the first providing ref (delta only).
    drop_upscale -> skip entries under data/{textures,art,tgl} (upscale dirs).
    gui_canon -> skip data/art/* entries whose basename is canonical GUI (owned by
    base art.fst / compat); the engine must always use OUR GUI, never the campaign's."""
    n = 0
    seen = set()
    for off, comp, real, name, is_lz, data in fst.fst_entries(fst_path):
        if not name:
            continue
        rn = rel_norm(name)
        if rn in EXCLUDE_REL or rn in CORE_GUI_EXCLUDE or rn in seen:
            continue
        sub = rn[5:] if rn.startswith("data/") else rn
        keep_content = _is_campaign_unique_asset(sub)  # colormaps + matched icon set survive both filters
        if gui_canon and _is_canon_gui(rn, gui_canon) and not keep_content:
            continue
        if drop_upscale and sub.startswith(UPSCALE_DIRS) and not keep_content:
            continue
        seen.add(rn)
        out_path = os.path.join(out_root, name.replace("\\", os.sep))
        try:
            content = fst.decompress_entry(off, comp, real, is_lz, data)
        except Exception as e:
            print("  WARN: %s: %s" % (name, e))
            continue
        if refs and in_refs(name, refs, content):
            continue
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "wb") as f:
            f.write(content)
        n += 1
    return n


# Loose appearance/texture trees that the base + compat layers already own.
# Excluded under --drop-upscale to avoid bloating the campaign mod with an upscale dump.
UPSCALE_DIRS = ("textures/", "art/", "tgl/")


# Campaign content atlases / layout fit that share a canonical name but carry
# CAMPAIGN-SPECIFIC content (custom mech/pilot icons + the cell geometry the engine reads).
# They are a matched set with the campaign's mech CSVs (iconPictureIndex), so they must be
# imported (and NOT stripped, see _strip_gui_overrides.CONTENT_KEEP) or icons go magenta.
CONTENT_KEEP_ART = {
    "mcui_gn_mechicons.tga", "mc2x_mechicons.tga",
    "mcl_gn_deploymentteams.fit",
    # pilot skill-icon + rank-insignia atlases (mcl_pr_pilotskillicons*) are GENERIC GUI
    # chrome owned by base/compat — a campaign's own copy overrides base's correct layout
    # and renders wrong skill/rank icons, so they are NOT kept (intentionally chrome).
}


def _is_campaign_unique_asset(sub):
    """Campaign-unique CONTENT under the upscale dirs (textures/ art/) that base/compat do
    NOT own. Must survive BOTH --drop-upscale and the gui_canon skip:
      * data/textures/<map>.burnin.* = per-MISSION baked terrain colormaps; dropping them
        leaves terrain BLACK (MapData::calcLight has no colormap).
      * the mech/pilot icon atlases + mcl_gn_deploymentteams.fit = the campaign's matched
        icon set; dropping them -> base atlas + campaign CSV indices mismatch -> magenta.
    (Bug: MCO black terrain + magenta icons, 2026-06-20.)"""
    s = sub.lower()
    if s.startswith("textures/") and ".burnin." in s:
        return True
    return s.rsplit("/", 1)[-1] in CONTENT_KEEP_ART

# Campaign roster files to pull SURGICALLY from misc.fst (NOT the whole archive).
# pilots.csv lives in the install's misc.fst, is NOT loose, and has no base/compat
# fallback -> without it LogisticsData::initPilots builds an empty pilot pool, so
# AVAILABLE PILOTS is empty and the deployment screen can never be filled to LAUNCH.
# MC2X renamed compbas.csv to mc2xcomp.csv; our engine still opens compbas.csv.
# Keep the campaign's weapon/ammo definitions with its object loadouts.
MISC_ROSTER = {"data/objects/pilots.csv": "data/objects/pilots.csv",
               "data/objects/compbas.csv": "data/objects/compbas.csv",
               "data/objects/mc2xcomp.csv": "data/objects/compbas.csv"}


def campaign_roster(entries):
    """Keep authored unit profiles with their component table; exclude misc GUI data."""
    return {**{rel_norm(e[3]): rel_norm(e[3]) for e in entries if e[3]
               and rel_norm(e[3]).startswith("data/objects/")
               and rel_norm(e[3]).endswith(".csv")}, **MISC_ROSTER}


def extract_fst_whitelist(fst_path, out_root, include):
    """Extract ONLY entries whose normalized name is in `include` (lowercased
    'data/...' paths). Surgical roster pull; everything else in the FST is ignored."""
    n = 0
    entries = sorted(fst.fst_entries(fst_path), key=lambda entry:
                     rel_norm(entry[3] or "") == "data/objects/mc2xcomp.csv")
    for off, comp, real, name, is_lz, data in entries:
        if not name or rel_norm(name) not in include:
            continue
        target = include[rel_norm(name)] if isinstance(include, dict) else name
        out_path = os.path.join(out_root, target.replace("\\", os.sep).replace("/", os.sep))
        try:
            content = fst.decompress_entry(off, comp, real, is_lz, data)
        except Exception as e:
            print("  WARN: %s: %s" % (name, e))
            continue
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "wb") as f:
            f.write(content)
        n += 1
    return n


def _is_canon_gui(name, gui_canon):
    """True if `name` (a 'data/...'-style rel path) is a data/art file whose basename
    is canonical GUI the engine already owns (base art.fst / compat). Such a campaign
    file is an OVERRIDE of our GUI and must not be imported."""
    rn = name.replace("\\", "/").lower()
    sub = rn[5:] if rn.startswith("data/") else rn
    return sub.startswith("art/") and os.path.basename(rn) in gui_canon


def build_canonical_gui_basenames(deploy_root, refs):
    """Lowercased basenames of canonical GUI art the engine already owns: every
    data/art/* entry in the deploy's base art.fst (GUI + icons live there, NOT in the
    loose ref dirs — which is why old-era campaign GUI used to slip in), plus any art
    file in a ref data dir (compat-provided GUI). A campaign's data/art file matching
    one of these is an override and is skipped at import."""
    names = set()
    if deploy_root:
        art_fst = os.path.join(deploy_root, "art.fst")
        if os.path.isfile(art_fst):
            try:
                for e in fst.fst_entries(art_fst):
                    nm = (e[3] or "").replace("\\", "/").lower()
                    if nm.startswith("data/art/"):
                        names.add(os.path.basename(nm))
            except Exception as ex:
                print("  WARN: could not read base art.fst for GUI canon: %s" % ex)
    for ref in refs or []:
        art_dir = os.path.join(ref, "art")
        if os.path.isdir(art_dir):
            for fn in os.listdir(art_dir):
                if os.path.isfile(os.path.join(art_dir, fn)):
                    names.add(fn.lower())
    return names


def copy_loose_delta(src_data, refs, out_data, drop_upscale=False, gui_canon=None):
    """Copy files under src_data whose bytes differ from the first providing ref.
    drop_upscale -> also exclude the loose data/{textures,art,tgl} trees (base+compat own them).
    gui_canon -> skip data/art/* files that are canonical GUI (use OUR GUI, not the campaign's)."""
    import shutil
    n = 0
    for root, _, files in os.walk(src_data):
        for fn in sorted(files, key=lambda name: name.lower() == "mc2xcomp.csv"):
            sp = os.path.join(root, fn)
            rel = os.path.relpath(sp, src_data).replace("\\", "/")
            rel_key = ("data/" + rel).lower()
            if rel_key == "data/objects/mc2xcomp.csv":
                rel = "objects/compbas.csv"
            own_table = rel_key == "data/objects/object2.pak" and not any(
                os.path.basename(os.path.dirname(os.path.normpath(ref))).lower() == "mco-compat"
                for ref in refs)
            if (rel_key in EXCLUDE_REL and not own_table) or rel_key in CORE_GUI_EXCLUDE:
                continue
            if own_table:
                rel = "objects/object2.pak"
            keep_content = _is_campaign_unique_asset(rel)  # colormaps + matched icon set survive both filters
            if gui_canon and _is_canon_gui(rel_key, gui_canon) and not keep_content:
                continue
            if drop_upscale and rel.lower().startswith(UPSCALE_DIRS) and not keep_content:
                continue
            out = os.path.join(out_data, rel.replace("/", os.sep))
            with open(sp, "rb") as stream:
                if not own_table and not os.path.isfile(out) and in_refs(rel, refs, stream.read()):
                    continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            shutil.copy2(sp, out)
            n += 1
    return n


def _write_marker(out_dir, source, fst_n, loose_n, drop_upscale, missing=None):
    """Provenance marker matching the mc2x_import contract (generated_by/source_fingerprint).
    FIX-4: files_missing is the count of requested FST inputs that were ABSENT from the
    install (the real loose/fst miss count), not a hardcoded 0 — so import_tool's
    validate-warn (A4) fires for transfers too. `missing` is the list of absent FST names."""
    import json
    missing = missing or []
    sys.path.insert(0, getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__))))
    try:
        import manifest
        fp = manifest.source_fingerprint(source)
    except Exception:
        fp = None
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "mc2x_import_report.json"), "w", encoding="utf-8") as f:
        json.dump({"generated_by": "transfer_mc2x_campaign", "tool_version": "1.0",
                   "source_path": os.path.abspath(source), "source_fingerprint": fp,
                   "drop_upscale": bool(drop_upscale),
                   "files_written": fst_n + loose_n, "fst_files": fst_n,
                   "loose_files": loose_n, "files_missing": len(missing),
                   "missing": missing}, f, indent=2)


def main():
    ap = argparse.ArgumentParser(description="Transfer an MC2X campaign install into a campaign mod.")
    ap.add_argument("--source", required=True, help="MC2X install root (has *.fst + data/)")
    ap.add_argument("--refs", nargs="+", required=True,
                    help="ref data dirs already provided elsewhere (mc2x-compat/data, base/data)")
    ap.add_argument("--out", required=True, help="campaign mod dir to write (…/mods/<name>)")
    ap.add_argument("--fsts", nargs="*", default=["mission.fst"],
                    help="FSTs extracted IN FULL (campaign-unique). default: mission.fst")
    ap.add_argument("--delta-fsts", nargs="*", default=["tgl.fst", "art.fst", "textures.fst"],
                    help="FSTs extracted as DELTA vs --refs (campaign-specific appearances/shapes/"
                         "textures only). default: tgl.fst art.fst textures.fst")
    ap.add_argument("--drop-upscale", action="store_true",
                    help="exclude the loose data/{textures,art,tgl} trees (base+compat own "
                         "appearances; avoids bloating the mod with an upscale dump)")
    ap.add_argument("--force", action="store_true",
                    help="overwrite an existing --out even if it was not generated by this tool")
    ap.add_argument("--deploy", default=None,
                    help="deploy root (has base art.fst). Default: derived as the parent of "
                         "the mods/ dir containing --out. Used to skip campaign GUI overrides.")
    args = ap.parse_args()

    src = args.source
    if not os.path.isdir(src):
        sys.exit("source not found: %s" % src)

    # Canonical GUI set: campaign data/art files that duplicate base art.fst / compat GUI
    # are NOT imported -- the engine always uses OUR GUI (the old-era campaign GUI is
    # mis-packed and renders garbled/pink/no-cursor). deploy root holds base art.fst.
    deploy_root = args.deploy or os.path.dirname(os.path.dirname(os.path.abspath(args.out)))
    gui_canon = build_canonical_gui_basenames(deploy_root, args.refs)
    print("  GUI canon: %d canonical data/art basenames (base art.fst + refs); campaign "
          "overrides of these are NOT imported" % len(gui_canon))

    # guard: refuse to clobber a foreign --out unless --force.
    if os.path.isdir(args.out) and not args.force:
        marker = os.path.join(args.out, "mc2x_import_report.json")
        gen = None
        if os.path.isfile(marker):
            try:
                import json
                with open(marker) as f:
                    gen = json.load(f).get("generated_by")
            except Exception:
                gen = None
        if gen not in ("transfer_mc2x_campaign", "import_tool"):
            sys.exit("ERROR: %s already exists and was not generated by this tool; "
                     "re-run with --force." % args.out)

    total_fst = 0
    missing_fsts = []  # FIX-4: requested FST inputs absent from the install
    for fname in args.fsts:
        fpath = os.path.join(src, fname)
        if not os.path.isfile(fpath):
            print("  (no %s — skipping)" % fname)
            missing_fsts.append(fname)
            continue
        n = extract_fst(fpath, args.out, gui_canon=gui_canon)
        print("  extracted %s (full): %d files" % (fname, n))
        total_fst += n
    for fname in args.delta_fsts:
        fpath = os.path.join(src, fname)
        if not os.path.isfile(fpath):
            # delta FSTs are OPTIONAL appearance overlays (absent on loose-data/MCO
            # installs) -> NOT counted as missing inputs (would false-warn every MCO import).
            print("  (no %s — skipping, optional delta)" % fname)
            continue
        n = extract_fst(fpath, args.out, refs=args.refs, drop_upscale=args.drop_upscale,
                        gui_canon=gui_canon)
        print("  extracted %s (delta): %d files" % (fname, n))
        total_fst += n

    # Surgical roster pull from misc.fst (campaign pilots.csv; see MISC_ROSTER).
    misc_path = os.path.join(src, "misc.fst")
    if os.path.isfile(misc_path):
        roster = campaign_roster(fst.fst_entries(misc_path))
        n_misc = extract_fst_whitelist(misc_path, args.out, roster)
        if n_misc:
            print("  extracted misc.fst roster (pilots.csv): %d file(s)" % n_misc)
            total_fst += n_misc
        else:
            print("  (misc.fst present but no roster CSV matched MISC_ROSTER)")

    src_data = os.path.join(src, "data")
    n_loose = 0
    if os.path.isdir(src_data):
        n_loose = copy_loose_delta(src_data, args.refs, os.path.join(args.out, "data"),
                                   drop_upscale=args.drop_upscale, gui_canon=gui_canon)
    if not any(os.path.basename(os.path.dirname(os.path.normpath(ref))).lower() == "mco-compat"
               for ref in args.refs):
        own = asset_bytes([src], "data/objects/object2.pak")
        if own is not None:
            destination = Path(args.out) / "data/objects/object2.pak"
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(own)
    print("  loose campaign delta: %d files%s"
          % (n_loose, " (--drop-upscale)" if args.drop_upscale else ""))
    _write_marker(args.out, src, total_fst, n_loose, args.drop_upscale, missing=missing_fsts)
    if missing_fsts:
        print("  NOTE: %d requested FST input(s) absent: %s"
              % (len(missing_fsts), ", ".join(missing_fsts)))
    print("done: %s  (fst=%d loose=%d)" % (args.out, total_fst, n_loose))


if __name__ == "__main__":
    main()
