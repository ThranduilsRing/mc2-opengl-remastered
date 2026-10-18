"""
tools/mc2_import/import_tool.py -- the single brain for one-click MC2 mod import.

ALL detection, idempotency, atomicity, and routing live here (DESIGN v2). The C++
launcher is a thin GUI shell: it calls `probe` to classify a picked folder and `auto`
to perform the import, parsing the JSON on stdout. This tool never mutates a base
data/ tree; every underlying extractor writes only under mods/.

Subcommands:
  probe <folder> [--json]            classify a raw picked folder (READ-ONLY)
  auto  <folder> --deploy <dir> ...  classify + import (crash-safe staging)

Python 3 stdlib only. No emoji. ASCII output.

Family / kind detection (DESIGN v2 decision tree):
  1. no data/ AND no root *.fst        -> UNKNOWN (reject)
  2. root *.fst present                 -> INSTALL ; else loose data/ -> PACK
  3. family (peek INSIDE fsts for installs; loose-only scan misses FST-packed
     missions -- the MC2X-CVE-G nuance):
        magicAttack in any brain        -> MCO   (definitive, even at stock objNum)
        else mission ObjectNumber>1188  -> MC2X-fat
        else                            -> STOCK (ambiguous)
     mc2xres.dll at root                = strong MC2X corroborator
  4. kind -> action + needs_compat (see classify()).
"""
from __future__ import annotations
import argparse
import json
import os
import re
import shutil
import sys
import time

# Reuse the proven helpers from the sibling tool trees.
# FROZEN-AWARE: PyInstaller exposes bundled DATA + bundled python modules under
# sys._MEIPASS. When frozen, all sibling modules (fst/manifest/mc2x_import/...) are
# bundled flat into _MEIPASS by --paths/--hidden-import, so a single sys.path entry
# pointing at _MEIPASS lets `import fst` etc. resolve. When NOT frozen (dev `py -3`),
# fall back to the original sibling-dir layout relative to __file__.
_FROZEN = getattr(sys, "frozen", False)
_HERE = os.path.dirname(os.path.abspath(__file__))
_BASE = getattr(sys, "_MEIPASS", _HERE)   # bundle root when frozen, else this file's dir

if _FROZEN:
    # Bundled modules live flat at _BASE; bundled DATA (shims/, recipe) also at _BASE.
    if _BASE not in sys.path:
        sys.path.insert(0, _BASE)
    _MC2X = _BASE
    _MCO = _BASE
else:
    _TOOLS = os.path.dirname(_HERE)
    _MC2X = os.path.join(_TOOLS, "mc2x_import")
    _MCO = os.path.join(_TOOLS, "mco_import")
    for _p in (_MC2X, _MCO):
        if _p not in sys.path:
            sys.path.insert(0, _p)

import fst        # noqa: E402  (tools/mc2x_import/fst.py)
import manifest   # noqa: E402  (tools/mc2x_import/manifest.py)

# corebrain.abx is a bundled DATA asset. Frozen: --add-data drops it at _BASE/shims.
# Dev: it lives next to this file at tools/mc2_import/shims/corebrain.abx.
CORE_SHIM = os.path.join(_BASE if _FROZEN else _HERE, "shims", "corebrain.abx")

# CVE-G install fingerprint: the file set the mc2x importer requires.
CVEG_REQUIRED = ["tgl.fst", "mission.fst", "art.fst", "misc.fst",
                 "data/objects/object2.pak", "mc2xres.dll"]

OBJNUM_RE = re.compile(rb"ObjectNumber\s*=\s*(\d+)")
MAGIC_TOKEN = b"magicAttack"
STOCK_OBJNUM_CEIL = 1188   # buildings.csv MECH rows start at >=1188 in fat installs


# ----------------------------------------------------------------------------- helpers

def _norm(p):
    return p.replace("\\", "/")


def _isfile(root, rel):
    return os.path.isfile(os.path.join(root, rel.replace("/", os.sep)))


def _isdir(root, rel):
    return os.path.isdir(os.path.join(root, rel.replace("/", os.sep)))


def _root_fsts(folder):
    try:
        return sorted(f for f in os.listdir(folder)
                      if f.lower().endswith(".fst")
                      and os.path.isfile(os.path.join(folder, f)))
    except OSError:
        return []


def _has_loose_magic_brain(folder, limit=400):
    """True if any loose warrior brain contains magicAttack (MCO definitive signal)."""
    wdir = os.path.join(folder, "data", "missions", "warriors")
    if not os.path.isdir(wdir):
        return False
    n = 0
    for fn in os.listdir(wdir):
        ext = fn.lower().rsplit(".", 1)[-1]
        if ext not in ("abx", "abl", "ab", "abm"):
            continue
        n += 1
        if n > limit:
            break
        try:
            with open(os.path.join(wdir, fn), "rb") as f:
                if MAGIC_TOKEN in f.read():
                    return True
        except OSError:
            continue
    return False


def _scan_fst_for_signals(fst_path, want_magic, want_objnum, file_cap=60):
    """Peek inside an FST. Returns (found_magic, max_objnum).
    Scans up to file_cap brain/mission entries (decompress is not free)."""
    found_magic = False
    max_obj = 0
    try:
        entries = fst.fst_entries(fst_path)
    except Exception:
        return found_magic, max_obj
    scanned = 0
    for off, comp, real, name, is_lz, data in entries:
        nl = name.lower()
        is_brain = want_magic and (nl.endswith(".abx") or nl.endswith(".abl")
                                   or "/warriors/" in nl.replace("\\", "/"))
        is_mission = want_objnum and (nl.endswith(".fit") or nl.endswith(".pak"))
        if not (is_brain or is_mission):
            continue
        try:
            content = fst.decompress_entry(off, comp, real, is_lz, data)
        except Exception:
            continue
        scanned += 1
        if is_brain and not found_magic and MAGIC_TOKEN in content:
            found_magic = True
        if is_mission:
            for m in OBJNUM_RE.finditer(content):
                v = int(m.group(1))
                if v > max_obj:
                    max_obj = v
        if found_magic and max_obj > STOCK_OBJNUM_CEIL:
            break
        if scanned >= file_cap:
            break
    return found_magic, max_obj


def _detect_family(folder, kind):
    """Return (family, max_objnum, has_mc2xres, signals[]).
    family in {MCO, MC2X, STOCK}."""
    signals = []
    has_mc2xres = _isfile(folder, "mc2xres.dll")
    if has_mc2xres:
        signals.append("mc2xres.dll")

    # (a) loose brains -- cheapest, definitive for MCO
    if _has_loose_magic_brain(folder):
        signals.append("magicAttack(loose-brain)")
        return "MCO", 0, has_mc2xres, signals

    # (b) peek inside FSTs (installs): brains for magicAttack, missions for ObjectNumber.
    # CVE-G keeps its brains+missions FST-packed -> loose scan above misses them.
    max_obj = 0
    if kind == "INSTALL":
        # brain-bearing FSTs first (mission.fst on MC2X carries brains too); then mission.fst.
        for fname in ("mission.fst", "tgl.fst", "misc.fst"):
            fp = os.path.join(folder, fname)
            if not os.path.isfile(fp):
                continue
            fm, mo = _scan_fst_for_signals(fp, want_magic=True, want_objnum=True)
            if fm:
                signals.append("magicAttack(fst:%s)" % fname)
                return "MCO", max(max_obj, mo), has_mc2xres, signals
            if mo > max_obj:
                max_obj = mo
            if max_obj > STOCK_OBJNUM_CEIL:
                break

    # (c) loose mission .fit scan (PACK or install loose overrides)
    mdir = os.path.join(folder, "data", "missions")
    if max_obj <= STOCK_OBJNUM_CEIL and os.path.isdir(mdir):
        scanned = 0
        for fn in os.listdir(mdir):
            if not fn.lower().endswith(".fit"):
                continue
            try:
                with open(os.path.join(mdir, fn), "rb") as f:
                    for m in OBJNUM_RE.finditer(f.read()):
                        v = int(m.group(1))
                        if v > max_obj:
                            max_obj = v
            except OSError:
                continue
            scanned += 1
            if max_obj > STOCK_OBJNUM_CEIL or scanned >= 60:
                break

    if max_obj > STOCK_OBJNUM_CEIL:
        signals.append("ObjectNumber=%d>%d" % (max_obj, STOCK_OBJNUM_CEIL))
        return "MC2X", max_obj, has_mc2xres, signals

    # No fat object table and no magic brains. mc2xres.dll alone still leans MC2X,
    # but without a fat object table there is nothing to import -> treat as STOCK.
    return "STOCK", max_obj, has_mc2xres, signals


def _is_cveg_shaped(folder):
    """CVE-G install fingerprint: the exact inputs mc2x_import.py needs."""
    return all(_isfile(folder, p) for p in CVEG_REQUIRED)


def _detect_upscale_bloat(folder):
    """Heuristic: a loose data/textures|art|tgl tree large enough to be an upscale dump
    (base+compat already own appearances; importing these bloats the mod). Returns bool."""
    THRESHOLD = 200 * 1024 * 1024  # 200 MB of loose appearance/texture trees
    total = 0
    for sub in ("data/textures", "data/art", "data/tgl"):
        d = os.path.join(folder, sub.replace("/", os.sep))
        if not os.path.isdir(d):
            continue
        for root, _dirs, files in os.walk(d):
            for fn in files:
                try:
                    total += os.path.getsize(os.path.join(root, fn))
                except OSError:
                    pass
            if total >= THRESHOLD:
                return True
    return total >= THRESHOLD


# ----------------------------------------------------------------------------- classify

def classify(folder):
    """Full decision tree. Returns the probe result dict."""
    warnings = []
    folder = os.path.abspath(folder)

    has_data = _isdir(folder, "data")
    root_fsts = _root_fsts(folder)

    # (1) neither data/ nor root *.fst -> UNKNOWN
    if not has_data and not root_fsts:
        return {
            "kind": "UNKNOWN", "action": "reject", "needs_compat": None,
            "source_fingerprint": None, "upscale_bloat": False,
            "warnings": warnings,
            "reject_reason": "No data/ and no root *.fst found. Pick the install ROOT "
                             "(the folder that contains data/ and/or the .fst archives), "
                             "not a data/ or sub-folder.",
        }

    # (2) INSTALL vs PACK
    kind = "INSTALL" if root_fsts else "PACK"

    # (3) family
    family, max_obj, has_mc2xres, signals = _detect_family(folder, kind)

    fp = manifest.source_fingerprint(folder)
    upscale = _detect_upscale_bloat(folder)
    cveg_shaped = (kind == "INSTALL" and family == "MC2X" and _is_cveg_shaped(folder))

    result = {
        "kind": None, "action": None, "needs_compat": None,
        "source_fingerprint": fp, "upscale_bloat": upscale,
        "family": family, "max_object_number": max_obj,
        "mc2xres": has_mc2xres, "cveg_shaped": cveg_shaped,
        "signals": signals, "warnings": warnings, "reject_reason": None,
    }

    # Importable content present? Root campaign FSTs for an INSTALL, or loose data/missions
    # for a PACK. A stock-RANGE object table does NOT mean "nothing to import": low-objtype
    # MC2X campaigns (wolfman DEE/DWE, DarkRain, TangoMaster, ...) are real importable
    # campaigns that are simply indistinguishable from pure stock by object number alone ->
    # ambiguous compat, NOT a rejection. (DEE imports fine via transfer_mc2x_campaign;
    # its mission.fst max ObjectNumber=969.)
    has_root_mission = kind == "INSTALL" and (_isfile(folder, "mission.fst") or bool(root_fsts))
    has_loose_missions = _isdir(folder, "data/missions")
    importable = has_root_mission or has_loose_missions

    # (4) kind+family -> action
    if family == "STOCK":
        if not importable:
            result["kind"] = "STOCK"
            result["action"] = "reject"
            result["reject_reason"] = (
                "No importable mission content found (no campaign .fst archives and no "
                "data/missions; no magicAttack brains, no ObjectNumber>%d). If this is an "
                "install, pick its ROOT folder (the one containing mission.fst or a data/ tree)."
                % STOCK_OBJNUM_CEIL)
            return result
        # AMBIGUOUS but importable: a stock-range MC2X campaign. Do NOT block; import as an
        # MC2X campaign with a default, overridable compat layer.
        result["family"] = "MC2X-AMB"
        result["kind"] = ("INSTALL" if kind == "INSTALL" else "PACK") + "+AMBIGUOUS"
        result["action"] = "transfer"
        result["needs_compat"] = "mc2x-compat"
        result["warnings"].append(
            "Could not auto-detect the mod type (stock-range object numbers, indistinguishable "
            "from pure stock by content). Importing as an MC2X campaign; mc2x-compat is selected "
            "by default. In the launcher, UNTICK the compatibility layer if this is actually a "
            "pure-stock pack, or tick a different one.")
        return result

    if kind == "INSTALL" and family == "MC2X":
        if cveg_shaped:
            result["kind"] = "INSTALL+MC2X-CVEG"
            result["action"] = "mc2x_import"          # builds mc2x-compat + cveg
            result["needs_compat"] = "mc2x-compat"    # self-built by mc2x_import
        else:
            result["kind"] = "INSTALL+MC2X"
            result["action"] = "transfer"             # campaign-only transfer
            result["needs_compat"] = "mc2x-compat"
            warnings.append("MC2X install is not CVE-G-shaped; it cannot build its own "
                            "mc2x-compat. Import the MC2X/CVE-G base first so mc2x-compat exists.")
        return result

    if kind == "INSTALL" and family == "MCO":
        result["kind"] = "INSTALL+MCO"
        result["action"] = "build_mco_compat+transfer"   # AUTO-CHAIN compat then campaign
        result["needs_compat"] = "mco-compat"
        return result

    if kind == "PACK":
        result["kind"] = "PACK+" + family
        result["action"] = "transfer"
        result["needs_compat"] = "mco-compat" if family == "MCO" else "mc2x-compat"
        warnings.append("PACK (loose data/ only). Transfer as a campaign mod; the matching "
                        "compat layer (%s) must already be present." % result["needs_compat"])
        return result

    # Fallthrough (should not happen)
    result["kind"] = "UNKNOWN"
    result["action"] = "reject"
    result["reject_reason"] = "Unclassifiable (kind=%s family=%s)." % (kind, family)
    return result


# ----------------------------------------------------------------------------- staging / GC

COMPAT_TARGETS = ("mco-compat", "mc2x-compat")


def _is_compat_target(t):
    return t in COMPAT_TARGETS


def _compat_present_with_data(deploy, t):
    """FIX-1: a compat layer is ADOPTABLE if its mod dir exists and carries a data/ tree."""
    d = os.path.join(deploy, "mods", t)
    return os.path.isdir(d) and os.path.isdir(os.path.join(d, "data"))


def _staging_root(deploy):
    return os.path.join(deploy, "mods")


def gc_stale_staging(deploy):
    """Remove orphaned .import-staging-* dirs and *.importbak left by a crash/cancel (FIX-3)."""
    mroot = _staging_root(deploy)
    if not os.path.isdir(mroot):
        return []
    removed = []
    for fn in os.listdir(mroot):
        if fn.startswith(".import-staging-") or fn.endswith(".importbak"):
            p = os.path.join(mroot, fn)
            if os.path.isdir(p):
                shutil.rmtree(p, ignore_errors=True)
                removed.append(fn)
    return removed


def _swap_in(staged_mod_dir, final_mod_dir):
    """Swap-aside swap-in (FIX-3): move old final aside to *.importbak, move staged in,
    then delete the backup. Same volume => near-atomic, and a crash mid-swap leaves the
    target EITHER intact (old, as *.importbak — GC'd or hand-recoverable) OR replaced,
    never GONE (the rmtree-then-replace gap is closed)."""
    os.makedirs(os.path.dirname(final_mod_dir), exist_ok=True)
    bak = final_mod_dir + ".importbak"
    if os.path.isdir(final_mod_dir):
        if os.path.isdir(bak):
            shutil.rmtree(bak, ignore_errors=True)
        os.replace(final_mod_dir, bak)
    os.replace(staged_mod_dir, final_mod_dir)
    if os.path.isdir(bak):
        shutil.rmtree(bak, ignore_errors=True)


def _clear_modindex_cache(mod_dir):
    c = os.path.join(mod_dir, ".modindex-cache")
    try:
        if os.path.exists(c):
            os.remove(c)
    except OSError:
        pass


# Marker filenames any importer tool may have written (unified, but accept both
# for back-compat with mco-compat dirs built before the unification).
_MARKER_NAMES = ("mc2x_import_report.json", "mco_compat_report.json")
# generated_by values that mean "this folder is OWNED by our importer suite".
_OWNED_BY = ("import_tool", "build_mco_compat", "transfer_mc2x_campaign", "mc2x_import")


def _read_marker_fp(deploy, mod_id):
    """Return (fingerprint, generated_by) of an existing mod marker, or (None, None).
    Accepts either unified or legacy marker filename (FIX-2)."""
    for name in _MARKER_NAMES:
        p = os.path.join(deploy, "mods", mod_id, name)
        try:
            with open(p) as f:
                d = json.load(f)
            return d.get("source_fingerprint"), d.get("generated_by")
        except Exception:
            continue
    return None, None


def _write_import_marker(mod_dir, payload):
    os.makedirs(mod_dir, exist_ok=True)
    with open(os.path.join(mod_dir, "mc2x_import_report.json"), "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2)


# ----------------------------------------------------------------------------- auto

def _campaign_mod_id(folder):
    """Derive a campaign mod folder name from the source folder basename."""
    base = os.path.basename(os.path.abspath(folder).rstrip("/\\"))
    return re.sub(r"[^A-Za-z0-9_.-]", "_", base) or "imported-campaign"


def _plan(folder, probe, deploy):
    """Build the ordered tool plan (list of dicts) for an action. No side effects."""
    src = os.path.abspath(folder)
    fp_corebrain = CORE_SHIM
    steps = []
    action = probe["action"]
    campaign_id = _campaign_mod_id(folder)
    drop_upscale = probe.get("upscale_bloat", False)

    if action == "mc2x_import":
        steps.append({
            "tool": "mc2x_import.py",
            "module": os.path.join(_MC2X, "mc2x_import.py"),
            "args": ["--source", src, "--deploy", "<STAGING>"],
            "targets": ["mc2x-compat", "cveg"],
            "kind": "mc2x_import",
        })
    elif action == "build_mco_compat+transfer":
        steps.append({
            "tool": "build_mco_compat.py",
            "module": os.path.join(_MCO, "build_mco_compat.py"),
            "args": ["--source", src, "--out", "<STAGING>/mods/mco-compat",
                     "--corebrain", fp_corebrain],
            "targets": ["mco-compat"],
            "kind": "build_mco_compat",
        })
        steps.append({
            "tool": "transfer_mc2x_campaign.py",
            "module": os.path.join(_MC2X, "transfer_mc2x_campaign.py"),
            # MCO campaigns keep missions LOOSE under data/missions (no mission.fst) ->
            # pass --fsts empty so transfer doesn't false-report mission.fst as missing.
            "args": ["--source", src,
                     "--refs", "<DEPLOY>/mods/mco-compat/data", "<DEPLOY>/data",
                     "--out", "<STAGING>/mods/" + campaign_id, "--fsts"]
                    + (["--drop-upscale"] if drop_upscale else []),
            "targets": [campaign_id],
            "kind": "transfer",
        })
    elif action == "transfer":
        compat = probe.get("needs_compat") or "mc2x-compat"
        steps.append({
            "tool": "transfer_mc2x_campaign.py",
            "module": os.path.join(_MC2X, "transfer_mc2x_campaign.py"),
            "args": ["--source", src,
                     "--refs", "<DEPLOY>/mods/%s/data" % compat, "<DEPLOY>/data",
                     "--out", "<STAGING>/mods/" + campaign_id]
                    + (["--drop-upscale"] if drop_upscale else []),
            "targets": [campaign_id],
            "kind": "transfer",
        })
    return steps


def _subst(args, staging, deploy):
    out = []
    for a in args:
        a = a.replace("<STAGING>", staging).replace("<DEPLOY>", deploy)
        out.append(a)
    return out


def cmd_auto(folder, deploy, force=False, dry_run=False, want_json=False):
    deploy = os.path.abspath(deploy)
    probe = classify(folder)

    # reject early
    if probe["action"] == "reject":
        res = {"status": "error", "mods_written": [], "compat": [], "warnings": probe["warnings"],
               "message": probe.get("reject_reason") or "rejected", "probe": probe}
        return res

    # GC stale staging on startup
    gc = gc_stale_staging(deploy)
    if gc:
        probe["warnings"].append("GC removed stale staging: " + ", ".join(gc))

    steps = _plan(folder, probe, deploy)
    all_targets = []
    for s in steps:
        all_targets += s["targets"]

    # DRY-RUN: side-effect-free preview. Show the plan regardless of force / idempotency /
    # foreign-exists state (must precede those guards so a previously-imported folder still
    # previews instead of erroring "already exists").
    if dry_run:
        plan_out = [{"tool": s["tool"],
                     "args": _subst(s["args"], "<deploy>/mods/.import-staging-<id>", deploy),
                     "targets": s["targets"]} for s in steps]
        return {"status": "plan", "kind": probe["kind"],
                "needs_compat": probe.get("needs_compat"),
                "mods_written": [], "compat": [], "warnings": probe["warnings"],
                "plan": plan_out,
                "message": "DRY-RUN: planned %d step(s); nothing extracted." % len(steps),
                "probe": probe}

    # IDEMPOTENCY: campaign target marker fingerprint == this source -> skip.
    fp = probe["source_fingerprint"]
    campaign_targets = [t for t in all_targets if t not in ("mco-compat", "mc2x-compat")]
    if not force and campaign_targets:
        all_match = True
        for t in campaign_targets:
            mfp, gen = _read_marker_fp(deploy, t)
            if mfp != fp or gen != "import_tool":
                all_match = False
                break
        if all_match:
            return {"status": "skipped", "mods_written": [], "compat": [],
                    "warnings": probe["warnings"],
                    "message": "Already imported, unchanged (fingerprint %s)." % fp,
                    "probe": probe}

    # Decide per-step which compat builds to ADOPT (present-with-data) vs build.
    # An existing compat layer with a data/ tree is reused as-is regardless of marker; we
    # never error and never demand --force for a compat target. Only CAMPAIGN targets keep
    # the foreign-guard (clobbering a user's campaign mod is what actually matters).
    adopted_compat = []
    for t in all_targets:
        if _is_compat_target(t) and _compat_present_with_data(deploy, t):
            adopted_compat.append(t)
    if adopted_compat:
        probe["warnings"].append("Adopting existing compat layer(s): " + ", ".join(adopted_compat)
                                 + " (reused as-is; build skipped).")

    # EXISTS-FOREIGN / DIFFERENT: a CAMPAIGN target exists but is foreign or different fp ->
    # need --force. Compat targets are never guarded here (handled by adopt above).
    if not force:
        for t in all_targets:
            if _is_compat_target(t):
                continue
            if not os.path.isdir(os.path.join(deploy, "mods", t)):
                continue
            mfp, gen = _read_marker_fp(deploy, t)
            if gen not in _OWNED_BY or mfp != fp:
                return {"status": "error", "mods_written": [], "compat": [],
                        "warnings": probe["warnings"],
                        "message": ("mods/%s already exists (%s). Re-run with --force to overwrite."
                                    % (t, "foreign" if gen not in _OWNED_BY else "different source")),
                        "probe": probe}

    # AUTO-CHAIN check: a transfer that depends on a compat layer not produced by this plan
    # and not present in deploy -> warn loudly (cannot build from this source).
    produced = set(all_targets)
    nc = probe.get("needs_compat")
    if nc and nc not in produced and not os.path.isdir(os.path.join(deploy, "mods", nc)):
        probe["warnings"].append("Required compat layer '%s' is absent and cannot be built from "
                                 "this source. Import the matching MC2X/MCO install first." % nc)

    # CRASH-SAFE STAGING
    staging_id = "%d" % int(time.time())
    staging = os.path.join(deploy, "mods", ".import-staging-" + staging_id)
    os.makedirs(os.path.join(staging, "mods"), exist_ok=True)

    mods_written = []
    compat_written = []
    all_missing = {}
    adopted_set = set(adopted_compat)

    def _swap_target(t):
        """Validate + swap one staged target into DEPLOY, marker it, classify it."""
        # Adopted compat is left in place untouched (no swap, no re-marker).
        if t in adopted_set:
            if t not in compat_written:
                compat_written.append(t)
            return
        staged_t = os.path.join(staging, "mods", t)
        if not os.path.isdir(staged_t):
            probe["warnings"].append("expected staged mod '%s' not produced" % t)
            return
        miss = _collect_missing(staged_t)
        if miss:
            all_missing[t] = miss
        final_t = os.path.join(deploy, "mods", t)
        _swap_in(staged_t, final_t)
        _clear_modindex_cache(final_t)
        _write_import_marker(final_t, {
            "generated_by": "import_tool", "tool_version": "1.0",
            "source_fingerprint": fp, "source_path": os.path.abspath(folder),
            "kind": probe["kind"], "action": probe["action"],
            "files_missing": len(miss),
            "stamp": time.strftime("%Y%m%d-%H%M%S"),
        })
        if t in ("mco-compat", "mc2x-compat"):
            if t not in compat_written:
                compat_written.append(t)
        else:
            if t not in mods_written:
                mods_written.append(t)

    try:
        # Run each step into staging, then IMMEDIATELY swap its targets into
        # DEPLOY before the next step runs. The plan is ordered compat-build first,
        # campaign-transfer second; a transfer step's `--refs <DEPLOY>/mods/<compat>/data`
        # only resolves on a fresh machine if the freshly-built compat has already been
        # swapped from STAGING into DEPLOY. Build-all-then-swap-all (the old order) left
        # the compat in STAGING during the transfer -> broken refs on fresh installs.
        # Staging crash-safety is preserved: each target swaps via _swap_in (swap-aside
        # *.importbak), and any unswapped staging is rmtree'd in finally.
        for s in steps:
            # Skip a compat-BUILD step entirely when that compat is adopted (present
            # with data/). Don't rebuild a heavy 302MB mco-compat that already exists.
            if adopted_set and all(t in adopted_set for t in s["targets"]):
                probe["warnings"].append("skipped build step '%s' (compat adopted)" % s["tool"])
                for t in s["targets"]:
                    _swap_target(t)  # records adopted compat into compat_written
                continue
            args = _subst(s["args"], staging, deploy)
            rc = _run_step(s, args)
            if rc not in (0, None):
                raise RuntimeError("%s exited %s" % (s["tool"], rc))
            # Swap this step's freshly-built targets into DEPLOY NOW, so a later step's
            # --refs against <DEPLOY>/mods/<target>/data resolve against the real install.
            for t in s["targets"]:
                _swap_target(t)
    finally:
        # leftover staging (e.g. partial on raise) is orphaned -> remove; real mods untouched
        if os.path.isdir(staging):
            shutil.rmtree(staging, ignore_errors=True)

    status = "ok"
    msg = "Imported: " + ", ".join(mods_written + compat_written)
    if all_missing:
        status = "warn"
        total = sum(len(v) for v in all_missing.values())
        msg += " (WARNING: %d file(s) missing from install across %d mod(s))" % (total, len(all_missing))
    return {"status": status, "mods_written": mods_written, "compat": compat_written,
            "warnings": probe["warnings"],
            "missing": {k: len(v) for k, v in all_missing.items()},
            "message": msg, "probe": probe}


def _run_step(step, args):
    """Drive an underlying tool in-process by calling its main(argv)."""
    kind = step["kind"]
    if kind == "mc2x_import":
        import mc2x_import
        return mc2x_import.main(args)
    if kind == "build_mco_compat":
        # build_mco_compat.main() parses sys.argv; shim argv.
        import build_mco_compat
        return _call_argv_main(build_mco_compat.main, args)
    if kind == "transfer":
        import transfer_mc2x_campaign
        return _call_argv_main(transfer_mc2x_campaign.main, args)
    raise RuntimeError("unknown step kind: " + kind)


def _call_argv_main(fn, args):
    """Call a main() that reads sys.argv (no argv param) by swapping sys.argv."""
    old = sys.argv
    sys.argv = ["tool"] + args
    try:
        rc = fn()
        return 0 if rc is None else rc
    finally:
        sys.argv = old


def _collect_missing(mod_dir):
    """Read any report markers under a staged mod dir to surface missing[]."""
    miss = []
    rep = os.path.join(mod_dir, "mc2x_import_report.json")
    try:
        with open(rep) as f:
            d = json.load(f)
        fm = d.get("files_missing", 0)
        if isinstance(fm, int) and fm:
            miss = ["<%d files>" % fm]
        elif isinstance(d.get("missing"), list):
            miss = d["missing"]
    except Exception:
        pass
    return miss


# ----------------------------------------------------------------------------- cli

def main(argv=None):
    ap = argparse.ArgumentParser(prog="import_tool",
                                 description="One-click MC2 mod importer brain (probe + auto).")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("probe", help="Classify a picked install/pack folder (read-only).")
    p.add_argument("folder")
    p.add_argument("--json", action="store_true")

    a = sub.add_parser("auto", help="Classify then import (crash-safe staging).")
    a.add_argument("folder")
    a.add_argument("--deploy", required=True, help="installed game folder containing mods/")
    a.add_argument("--force", action="store_true")
    a.add_argument("--dry-run", action="store_true")
    a.add_argument("--json", action="store_true")

    args = ap.parse_args(argv)

    if args.cmd == "probe":
        res = classify(args.folder)
        if args.json:
            print(json.dumps(res))
        else:
            print(json.dumps(res, indent=2))
        return 0 if res["action"] != "reject" else 0  # probe always exits 0 (classification ok)

    if args.cmd == "auto":
        res = cmd_auto(args.folder, args.deploy, force=args.force,
                       dry_run=args.dry_run, want_json=args.json)
        if args.json:
            # drop the bulky probe sub-dict from JSON unless useful; keep it for the launcher.
            print(json.dumps(res))
        else:
            print(json.dumps(res, indent=2))
        return {"ok": 0, "skipped": 0, "warn": 0, "plan": 0, "error": 1}.get(res["status"], 1)

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
