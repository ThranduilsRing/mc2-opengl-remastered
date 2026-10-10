#!/usr/bin/env python3
"""verify_campaign_assets.py — static pre-flight that each campaign's missions have the
assets they need, resolved through the mod VFS layers (campaign > deps > base loose >
base FSTs). Catches the import gaps that render black terrain / empty pilots:

  * COLORMAP: every mission <name>.pak (data/missions/) must have a matching
    <name>.burnin.{tga,jpg} (mission basename == map name == colormap basename).
  * PILOTS:   data/objects/pilots.csv must resolve (loose layer or base misc.fst),
    else AVAILABLE PILOTS is empty and the launch screen can't be filled.

Read-only. Resolves deps via resolve_campaign_config.py (no folder-name guessing).

Usage:
  py -3 scripts/verify_campaign_assets.py [--deploy <dir>] [--campaign <name> | --all]
  --deploy defaults to $MC2_DEPLOY_DIR (computer-agnostic).
Exit code: 0 = all checked campaigns OK, 1 = at least one missing asset.
"""
import argparse, importlib.util, json, os, subprocess, sys
from pathlib import Path
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

HERE = Path(__file__).resolve().parent
DEFAULT_DEPLOY = os.environ.get("MC2_DEPLOY_DIR", "A:/Games/mc2-opengl/mc2-win64-v0.4")

# Optional FST reader (base textures.fst / misc.fst). Absent -> FST layer skipped (loose only).
def _load_fst():
    p = HERE.parent / "tools" / "mc2x_import" / "fst.py"
    if not p.is_file():
        return None
    spec = importlib.util.spec_from_file_location("fst", str(p))
    m = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(m)
        return m
    except Exception:
        return None

FST = _load_fst()

def resolve(deploy, name):
    out = subprocess.run([sys.executable, str(HERE / "resolve_campaign_config.py"), deploy, name],
                         capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    try:
        return json.loads(out.stdout)
    except Exception:
        return {}

def fst_names(fst_path):
    """lowercased data/... entry names in an FST, or empty set."""
    s = set()
    if FST and os.path.isfile(fst_path):
        try:
            for e in FST.fst_entries(fst_path):
                n = e[3]
                if n:
                    s.add(n.replace("\\", "/").lower())
        except Exception:
            pass
    return s

def layer_dirs(deploy, campaign, cfg):
    """VFS search order: campaign mod, each dep, base loose data."""
    dirs = [os.path.join(deploy, "mods", campaign, "data")]
    for dep in (cfg.get("mc2_mod_deps_string") or "").split(","):
        dep = dep.strip()
        if dep:
            dirs.append(os.path.join(deploy, "mods", dep, "data"))
    dirs.append(os.path.join(deploy, "data"))
    return [d for d in dirs if os.path.isdir(d)]

def loose_has(dirs, rel):
    rel = rel.lower()
    for d in dirs:
        if os.path.isfile(os.path.join(d, rel.replace("/", os.sep))):
            return True
    return False

def check_campaign(deploy, campaign):
    cfg = resolve(deploy, campaign)
    dirs = layer_dirs(deploy, campaign, cfg)
    # base FST asset names (textures.fst colormaps + misc.fst pilots), via base root *.fst
    base = os.path.join(deploy)
    fst_set = set()
    for fn in ("textures.fst", "misc.fst", "art.fst"):
        fst_set |= fst_names(os.path.join(base, fn))

    def resolves(rel):
        return loose_has(dirs, rel) or (("data/" + rel).lower() in fst_set)

    # missions = data/missions/*.pak basename (skip *_purchase helpers)
    mdir = os.path.join(deploy, "mods", campaign, "data", "missions")
    missions = []
    if os.path.isdir(mdir):
        for fn in os.listdir(mdir):
            if fn.lower().endswith(".pak") and not fn.lower().endswith("_purchase.pak"):
                missions.append(fn[:-4])
    missions.sort()

    def colormap_ok(name):
        # direct: <name>.burnin.{tga,jpg}
        if resolves("textures/%s.burnin.tga" % name) or resolves("textures/%s.burnin.jpg" % name):
            return True
        # variant fallback: playtest/dev copies (e.g. "area16.playtest-orig",
        # "foo.playtest", "bar_orig") reuse the BASE map's terrain+colormap. Strip the
        # variant suffix and re-check the base map name.
        base = name.split(".")[0]
        for suf in ("_playtest", "-playtest", "_orig", "-orig", "_copy", "-copy"):
            if base.endswith(suf):
                base = base[: -len(suf)]
        if base != name:
            return resolves("textures/%s.burnin.tga" % base) or resolves("textures/%s.burnin.jpg" % base)
        return False

    missing_cmap = [m for m in missions if not colormap_ok(m)]

    pilots_ok = resolves("objects/pilots.csv")

    return {
        "campaign": campaign,
        "deps": cfg.get("mc2_mod_deps_string"),
        "missions": len(missions),
        "missing_colormaps": missing_cmap,
        "pilots_ok": pilots_ok,
        "ok": (not missing_cmap) and pilots_ok,
    }

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--deploy", default=DEFAULT_DEPLOY)
    ap.add_argument("--campaign")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    if args.all or not args.campaign:
        mods = os.path.join(args.deploy, "mods")
        skip = ("compat", "cveg", "shims", "upscaled")
        camps = sorted(d for d in os.listdir(mods)
                       if os.path.isdir(os.path.join(mods, d))
                       and not any(s in d.lower() for s in skip)
                       and os.path.isdir(os.path.join(mods, d, "data", "missions")))
    else:
        camps = [args.campaign]

    any_bad = False
    for c in camps:
        r = check_campaign(args.deploy, c)
        flag = "OK " if r["ok"] else "BAD"
        print("[%s] %-24s missions=%-3d colormaps_missing=%-2d pilots=%s%s"
              % (flag, c, r["missions"], len(r["missing_colormaps"]),
                 "yes" if r["pilots_ok"] else "NO ",
                 ("  -> " + ", ".join(r["missing_colormaps"][:8])) if r["missing_colormaps"] else ""))
        if not r["ok"]:
            any_bad = True
    sys.exit(1 if any_bad else 0)

if __name__ == "__main__":
    main()
