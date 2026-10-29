#!/usr/bin/env python3
'Smoke every supported mod against the current mc2.exe: py -3 tools/mod_smoke.py --battery'
#
# Uses isolated disposable installs and persistent, incrementally updated payload
# layers under E:/mc2-installs/mod_smoke-stage. Each game takes its own host lease.
#
#     py -3 tools/mod_smoke.py --battery [--mod NAME[,NAME]] [--mission STEM]
#
# Stage layout (lowest layer first; a later layer overwrites an earlier one):
#   1. engine payload, built ONCE per run in <stamp>/_engine and hardlinked into
#      every stage (same volume): exactly the files scripts/deploy_payload.py
#      enumerates for the game target (exe, DLLs, shaders, SPIR-V, data/defs,
#      PBR/terrain data), the extra generated defs listed in the base install's
#      .deployed_manifest.csv, plus the engine-owned data the manifest does not
#      list (assets/ fonts+graphics, data/art/asset_sizes.csv, data/art/gui, ...)
#      and the stock fast files / configs from the base install.
#   2. the mod's own payload (a mod that is an add-on, e.g. MCO-Carver5, stacks
#      on its base mod first): data/ tree, root *.fst and *.pak, mod configs.
#      Overlaid files are unlinked first so the shared engine inode is never
#      written through.
#   3. the mission's loose data/missions/<stem>.fit if it lives in mission.fst.
#
# The mission is chosen per mod from the mod's own campaign file (first mission
# that exists), else its first mission with a .fit+.pak pair; --mission overrides.
# Each run is then checked for the owner's known failure modes (see analyze_health).
# Exit 0 only if every mod passed smoke AND its health checks.

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.append(str(Path(__file__).resolve().parent.parent / "scripts"))
import console_child  # noqa: E402  (windowless console children)
import stage_root as stages  # noqa: E402  (default staging roots on E:, free-space preflight)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INSTALL_ROOT = Path('A:/Games/mc2-opengl')
DEFAULT_BATTERY_ROOT = stages.default_root('mod_smoke')
DEFAULT_BASE_INSTALL = Path('A:/Games/mc2-opengl/mc2-win64-v0.4')
GOLDEN_INSTALL = 'mc2-win64-v0.4c'

# (display name, install folder, base mod folder it stacks on or None)
BATTERY = (
    ('MC2X-CVE-G', 'MC2X-CVE-G', None),
    ('MC2X-REAP3', 'MC2X-REAP3', None),
    ('MC2-Exodus', 'MC2-Exodus', None),
    ('MC2-MagicExpansion', 'MC2-MagicExpansion', None),
    ('MechCommanderOmnitech', 'MechCommanderOmnitech', None),
    ('MCO-Carver5', 'MCO-Carver5', 'MechCommanderOmnitech'),
)

# Engine-owned loose data that deploy_payload does not enumerate (relative to the
# base install). Files only count when absent from the stock fast files, so the
# list can never shadow stock or mod content.
ENGINE_DATA_TREES = (
    'assets', 'data/effects', 'data/art/asset_sizes.csv', 'data/art/gui', 'data/cdag',
    'data/materials', 'data/terrain_layers', 'data/vegetation', 'data/model_overrides',
)
# Always staged, even when a mod's own fast file also carries them (the modern
# engine loads these loose, e.g. data/effects/mc2.fx).
ENGINE_DATA_UNFILTERED = ('assets', 'data/effects')
ENGINE_DATA_FILES = ('data/art/mcl_splashscreen_planet_1_opaque.tga',)
HDR_TREE = 'data/hdr'
HDR_MAX_BYTES = 20 * 1024 * 1024   # skip the 128 MB 16k sky cubes
STOCK_FSTS = ('mission.fst', 'tgl.fst', 'art.fst', 'textures.fst', 'misc.fst',
              'camera.fst', 'effect.fst', 'insignia.fst')
IMMUTABLE_SUFFIXES = {'.fst', '.pak', '.exe', '.dll', '.spv', '.glsl', '.hglsl', '.frag', '.vert', '.comp', '.png', '.tga', '.bmp', '.dds', '.ktx2', '.glb', '.wav', '.ogg', '.ttf', '.otf'}
# Cooked model entries replace via temp+rename (cook_cache.cpp), never write through
# an existing inode. Sharing these 500+ MB saves a copy for every mod.
IMMUTABLE_SUFFIXES.update(('.mci', '.exr'))
# Legacy campaign images and movies are read-only payload too.
IMMUTABLE_SUFFIXES.update(('.jpg', '.jpeg', '.bik'))
CONFIG_FILES = ('prefs.cfg', 'options.cfg', 'minprefs.cfg')
# Root data files that override the mod pack and must not be copied over it.
STOCK_ROOT_FILES = ('prefs.cfg', 'options.cfg', 'system.cfg')
# Manifest rows that are not runtime payload for a smoke stage.
MANIFEST_SKIP_PREFIXES = ('tools/', 'mods/')
MANIFEST_SKIP_NAMES = ('mc2-launcher.exe', 'run-mc2.bat', 'run-with-log.bat')

# Health thresholds (scenario seconds).
AI_DEADLINE_S = 20.0


def split_csv(value: str) -> tuple[str, ...]:
    return tuple(p.strip() for p in value.split(',') if p.strip())


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def resolve_build(explicit: str | None, build_dir: str) -> tuple[Path, Path]:
    src = Path(explicit) if explicit else ROOT / build_dir
    if src.is_dir():
        src = src / 'mc2.exe'
    if not src.is_file():
        raise FileNotFoundError(f'current engine not found: {src}')
    build = src.resolve().parent
    root = build
    while root.name.lower() not in ('build64', 'build64-ninja') and root.parent != root:
        root = root.parent
    if root.parent == root:
        root = ROOT / build_dir  # installed exe or E: tree reached through a junction
    return build, root


# ---------------------------------------------------------------------------
# File placement
# ---------------------------------------------------------------------------

def place_file(source: Path, target: Path, link_ok: bool = True, incremental=False) -> str:
    """Put source at target, replacing whatever is there. Hardlink when the two
    share a volume; immutable assets may use a file symlink across volumes. The target is unlinked first so an existing
    hardlink is replaced rather than written through. Returns 'link' or 'copy'."""
    target.parent.mkdir(parents=True, exist_ok=True)
    if incremental and target.is_file() and not target.is_symlink():
        a, b = source.stat(), target.stat()
        if (a.st_size, a.st_mtime_ns) == (b.st_size, b.st_mtime_ns):
            return 'unchanged'
    # Runtime immutable files may share a cached layer's inode. Keep a shared layer's existing link.
    if link_ok and source.suffix.lower() in IMMUTABLE_SUFFIXES and target.is_file():
        if os.path.samefile(source, target):
            return 'link'
    if target.exists() or target.is_symlink():
        target.unlink()
    if link_ok:
        try:
            os.link(source, target)
            return 'link'
        except OSError:
            pass
        if source.suffix.lower() in IMMUTABLE_SUFFIXES - {'.exe', '.dll'}:
            try:
                os.symlink(source.resolve(), target)
                return 'link'
            except OSError:
                pass  # hosts without file-link privilege retain the copy fallback
    shutil.copy2(source, target)
    return 'copy'


def place_tree(source: Path, target: Path, link_ok: bool = True, keep=None, private_loose=False, cached_links=False, incremental=False) -> dict:
    stats = {'files': 0, 'bytes': 0, 'linked': 0}
    for dirpath, dirnames, filenames in os.walk(source):
        dirnames.sort()
        rel_dir = Path(dirpath).relative_to(source)
        (target / rel_dir).mkdir(parents=True, exist_ok=True)
        for name in sorted(filenames):
            src = Path(dirpath) / name
            if (src.is_symlink() and not cached_links) or not src.is_file():
                continue
            if keep is not None and not keep(src):
                continue
            how = place_file(src, target / rel_dir / name, link_ok and (not private_loose or src.suffix.lower() in IMMUTABLE_SUFFIXES), incremental)
            stats['files'] += 1
            stats['bytes'] += src.stat().st_size
            stats['linked'] += how == 'link'
    return stats


def same_volume(a: Path, b: Path) -> bool:
    return a.resolve().drive.lower() == b.resolve().drive.lower()


# ---------------------------------------------------------------------------
# Engine payload (deploy manifest)
# ---------------------------------------------------------------------------

def _load_deploy_payload():
    import importlib
    scripts = str(ROOT / 'scripts')
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    return importlib.import_module('deploy_payload')


def engine_payload_items(build: Path, build64: Path) -> list[tuple[Path, str]]:
    """(source, relpath) pairs the deploy script ships for the game target."""
    dp = _load_deploy_payload()
    items = dp.enumerate_payload(str(ROOT), str(build), 'mc2.exe', 'mc2.pdb',
                                 require_build=False, build64_root=str(build64))
    out = []
    for src, rel, _kind in items:
        rel = rel.replace('\\', '/')
        low = rel.lower()
        if low.startswith(MANIFEST_SKIP_PREFIXES) or low in MANIFEST_SKIP_NAMES or low.endswith('.pdb'):
            continue
        if not Path(src).is_file():
            continue
        out.append((Path(src), rel))
    return out


def manifest_extra_rels(base_install: Path, have: set[str]) -> list[str]:
    """Rows of the base install's .deployed_manifest.csv not already staged
    (generated unit/equipment defs)."""
    path = base_install / '.deployed_manifest.csv'
    rels = []
    if not path.is_file():
        return rels
    with path.open(newline='', encoding='utf-8') as fh:
        for row in csv.reader(fh):
            if not row or row[0] in ('manifest_version', 'relpath'):
                continue
            rel = row[0].replace('\\', '/')
            low = rel.lower()
            if low in have or low.startswith(MANIFEST_SKIP_PREFIXES) or low in MANIFEST_SKIP_NAMES or low.endswith('.pdb'):
                continue
            if (base_install / rel).is_file():
                rels.append(rel)
    return rels


def stock_fst_keys(base_install: Path) -> set[str]:
    sys.path.insert(0, str(ROOT / 'tools' / 'mod_install'))
    import fst_listing
    keys = set()
    for name in STOCK_FSTS:
        p = base_install / name
        if p.is_file():
            keys.update(fst_listing.normalize_key(e['name']) for e in fst_listing.parse_fst(str(p)))
    return keys


def build_engine_cache(cache: Path, build: Path, build64: Path, base_install: Path) -> dict:
    """Assemble the shared engine payload in `cache`. Returns a summary."""
    cache.mkdir(parents=True, exist_ok=True)
    summary = {'payload': 0, 'manifest_extra': 0, 'engine_data': 0, 'stock_fsts': [], 'configs': [], 'dlls': []}
    staged: set[str] = set()
    for src, rel in engine_payload_items(build, build64):
        place_file(src, cache / rel, link_ok=src.suffix.lower() in IMMUTABLE_SUFFIXES)
        staged.add(rel.lower())
        summary['payload'] += 1
    for rel in manifest_extra_rels(base_install, staged):
        place_file(base_install / rel, cache / rel, link_ok=Path(rel).suffix.lower() in IMMUTABLE_SUFFIXES)
        staged.add(rel.lower())
        summary['manifest_extra'] += 1
    stock = stock_fst_keys(base_install)

    def not_stock(path: Path) -> bool:
        rel = path.relative_to(base_install).as_posix().lower()
        return rel not in stock

    def place_rel(rel: str, keep=None) -> None:
        src = base_install / rel
        if src.is_dir():
            summary['engine_data'] += place_tree(src, cache / rel, True, keep, private_loose=True)['files']
        elif src.is_file() and (keep is None or keep(src)):
            place_file(src, cache / rel, link_ok=src.suffix.lower() in IMMUTABLE_SUFFIXES)
            summary['engine_data'] += 1

    for rel in ENGINE_DATA_TREES:
        place_rel(rel, keep=None if rel in ENGINE_DATA_UNFILTERED else not_stock)
    for rel in ENGINE_DATA_FILES:
        place_rel(rel)
    # Exercise the shipped lowest-priority art layer alongside each campaign.
    place_rel('mods/mc2-industrial-kit/mod.json')
    place_rel('mods/mc2-industrial-kit/data')
    place_rel('cache/content-model')  # deploy-cooked recipes; no first-mission Assimp cook
    place_rel(HDR_TREE, keep=lambda p: p.stat().st_size <= HDR_MAX_BYTES and not_stock(p))
    for dll in sorted(base_install.glob('*.dll')):
        # runtime DLLs the build dir does not carry (SDL2*, glew, zlib, mc2res_64)
        if not (cache / dll.name).is_file():
            place_file(dll, cache / dll.name)
            summary['dlls'].append(dll.name)
    system_cfg = ROOT / 'system.cfg'
    if system_cfg.is_file() and not (cache / 'system.cfg').is_file():
        place_file(system_cfg, cache / 'system.cfg', link_ok=False)
    for name in STOCK_FSTS:
        src = base_install / name
        if src.is_file():
            place_file(src, cache / name, link_ok=src.suffix.lower() in IMMUTABLE_SUFFIXES)
            summary['stock_fsts'].append(name)
    for name in CONFIG_FILES:
        src = base_install / name
        if src.is_file():
            place_file(src, cache / name, link_ok=src.suffix.lower() in IMMUTABLE_SUFFIXES)
            summary['configs'].append(name)
    required = ['mc2.exe', 'SDL2.dll', 'mc2res_64.dll'] + list(STOCK_FSTS) + list(CONFIG_FILES)
    missing = [n for n in required if not (cache / n).is_file()]
    if missing:
        raise FileNotFoundError('engine payload incomplete, missing: ' + ', '.join(missing))
    return summary


# ---------------------------------------------------------------------------
# Mod overlay + mission selection
# ---------------------------------------------------------------------------

def mod_layers(mod_root: Path, folder: str, base_mod: str | None) -> list[Path]:
    layers = []
    if base_mod:
        layers.append(mod_root / base_mod)
    layers.append(mod_root / folder)
    return layers


def overlay_mod(layer: Path, target: Path, link_ok: bool = True, cached_links=False, incremental=False) -> dict:
    data = layer / 'data'
    if not data.is_dir():
        raise FileNotFoundError(f'mod has no data dir: {data}')
    stats = place_tree(data, target / 'data', link_ok=link_ok, private_loose=True, cached_links=cached_links, incremental=incremental)
    assets = layer / 'assets'
    if assets.is_dir():
        extra = place_tree(assets, target / 'assets', link_ok=link_ok, private_loose=True, cached_links=cached_links, incremental=incremental)
        stats['files'] += extra['files']
        stats['bytes'] += extra['bytes']
        stats['linked'] += extra['linked']
    stock = {s.lower() for s in STOCK_ROOT_FILES}
    for entry in sorted(layer.iterdir()):
        if not entry.is_file() or entry.name.lower() in stock:
            continue
        if entry.suffix.lower() not in ('.fst', '.pak'):
            continue
        how = place_file(entry, target / entry.name, link_ok=link_ok, incremental=incremental)
        stats['files'] += 1
        stats['bytes'] += entry.stat().st_size
        stats['linked'] += how == 'link'
    for name in CONFIG_FILES:
        src = layer / name
        if src.is_file():
            place_file(src, target / name, link_ok=False, incremental=incremental)
    return stats


def link_engine(cache: Path, target: Path) -> dict:
    link_ok = same_volume(cache, target)
    return place_tree(cache, target, link_ok=link_ok, private_loose=True, cached_links=True)


def _campaign_stems(layer: Path) -> list[str]:
    folder = layer / 'data' / 'campaign'
    if not folder.is_dir():
        return []
    files = sorted(folder.glob('*.fit'), key=lambda p: (p.name.lower() != 'campaign.fit', p.name.lower()))
    for fit in files:
        text = fit.read_text(encoding='latin-1', errors='replace')
        stems = [m.group(1).lower() for m in re.finditer(
            r'^\s*st\s+FileName\s*=\s*"([^"]+)"', text, re.M | re.I)]
        if stems:
            return stems
    return []


def _fst_mission_stems(fst: Path) -> tuple[set[str], set[str]]:
    sys.path.insert(0, str(ROOT / 'tools' / 'mod_install'))
    import fst_listing
    fits, paks = set(), set()
    for e in fst_listing.parse_fst(str(fst)):
        key = fst_listing.normalize_key(e['name'])
        m = re.fullmatch(r'data/missions/([^/]+)\.(fit|pak)', key)
        if m:
            (fits if m.group(2) == 'fit' else paks).add(m.group(1))
    return fits, paks


def available_missions(layers: list[Path]) -> tuple[set[str], set[str]]:
    """(stems with a .fit, stems with a .fit and a .pak) across loose files and mission.fst."""
    fits, paks = set(), set()
    for layer in layers:
        missions = layer / 'data' / 'missions'
        if missions.is_dir():
            for f in missions.iterdir():
                if f.suffix.lower() == '.fit':
                    fits.add(f.stem.lower())
                elif f.suffix.lower() == '.pak':
                    paks.add(f.stem.lower())
        fst = layer / 'mission.fst'
        if fst.is_file():
            f2, p2 = _fst_mission_stems(fst)
            fits |= f2
            paks |= p2
    return fits, fits & paks


def _natural(s: str):
    return [int(t) if t.isdigit() else t for t in re.split(r'(\d+)', s)]


def _mission_rank(stem: str):
    # numbered campaign-style names first (mc2_01, a_0_1, reap3m01), test/demo maps last
    if re.fullmatch(r'mc2_\d+', stem):
        group = 0
    elif re.fullmatch(r'[a-z]_\d+_\d+', stem):
        group = 1
    elif re.search(r'\d+$', stem) and not stem.startswith(('ai_', 'm0')):
        group = 2
    else:
        group = 3
    return (group, _natural(stem))


def pick_mission(layers: list[Path], override: str | None = None) -> tuple[str, str]:
    """(stem, how) for a mod. Campaign first mission that exists, else first
    .fit+.pak pair. `override` wins (and must exist)."""
    fits, pairs = available_missions(layers)
    if override:
        stem = override.lower()
        if stem not in fits:
            raise ValueError(f'mission {override!r} not found in {[l.name for l in layers]}')
        return stem, 'override'
    for layer in reversed(layers):
        for stem in _campaign_stems(layer):
            if stem in fits:
                return stem, f'campaign:{layer.name}'
    ranked = sorted((s for s in pairs if 'special' not in s), key=_mission_rank)
    if ranked:
        return ranked[0], 'first-mission'
    raise ValueError(f'no mission found in {[l.name for l in layers]}')


def ensure_loose_mission(layers: list[Path], target: Path, stem: str) -> bool:
    """run_smoke --allow-any-mission launches data/missions/<stem>.fit; extract it
    from the topmost layer's mission.fst when it is not loose. True if extracted."""
    if (target / 'data' / 'missions' / f'{stem}.fit').is_file():
        return False
    for layer in reversed(layers):
        fst = layer / 'mission.fst'
        if fst.is_file():
            try:
                extract_mission(fst, stem, target / 'data' / 'missions' / f'{stem}.fit')
                return True
            except FileNotFoundError:
                continue
    raise FileNotFoundError(f'mission {stem} not loose and not in any mission.fst')


def extract_mission(fst: Path, stem: str, out: Path) -> None:
    import importlib
    sys.path.insert(0, str(ROOT / 'tools' / 'mc2x_import'))
    fst_lib = importlib.import_module('fst')
    wanted = f'data/missions/{stem}.fit'
    for off, comp, real, name, is_lz, blob in fst_lib.fst_entries(fst):
        if name.lower().replace(chr(92), '/') == wanted:
            content = fst_lib.decompress_entry(off, comp, real, is_lz, blob)
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(content)
            return
    raise FileNotFoundError(f'{wanted} not found inside {fst}')


GAP_SKIP_PREFIXES = ('data/movies/',)
GAP_SKIP_PARTS = ('.beauty/',)


def stage_keys(target: Path) -> set[str]:
    """Normalized keys the stage can already resolve: its fast files + loose data."""
    sys.path.insert(0, str(ROOT / 'tools' / 'mod_install'))
    import fst_listing
    keys = set()
    for fst in target.glob('*.fst'):
        keys.update(fst_listing.normalize_key(e['name']) for e in fst_listing.parse_fst(str(fst)))
    for dirpath, _dirs, files in os.walk(target / 'data'):
        for name in files:
            keys.add((Path(dirpath) / name).relative_to(target).as_posix().lower())
    return keys


def fill_stock_gap(base_install: Path, target: Path, cache: Path | None = None) -> dict:
    """Retail MC2 mods (MC2X, MCO) ship only the files they change and rely on the
    retail install for the rest, sometimes renaming members inside a replaced fast
    file (camera.fst: mc2xcams.fit for cameras.fit). Materialize every stock member
    the stage cannot resolve as a loose file; never touches a key the mod provides."""
    sys.path.insert(0, str(ROOT / 'tools' / 'mc2x_import'))
    sys.path.insert(0, str(ROOT / 'tools' / 'mod_install'))
    import importlib
    fst_lib = importlib.import_module('fst')
    import fst_listing
    have = stage_keys(target)
    stats = {'from_fst': 0, 'bytes': 0}

    def wanted(key: str) -> bool:
        return (key not in have and key.startswith('data/')
                and not key.startswith(GAP_SKIP_PREFIXES)
                and not any(p in key for p in GAP_SKIP_PARTS))

    for name in STOCK_FSTS:
        src = base_install / name
        if not src.is_file():
            continue
        # Extract missing members once, even when several mods replace this FST.
        cached = None
        if cache is not None:
            stat = src.stat()
            key = hashlib.sha256(f'{src.resolve()}:{stat.st_size}:{stat.st_mtime_ns}'.encode()).hexdigest()
            cached = cache / name / key
            if cached.parent.is_dir():
                for obsolete in cached.parent.iterdir():
                    if obsolete.name != key and re.fullmatch('[0-9a-f]{64}', obsolete.name):
                        shutil.rmtree(obsolete)
            # Only read archive payload bytes when a missing member needs extraction.
            entries = fst_listing.parse_fst(str(src))
            for entry in entries:
                member = fst_listing.normalize_key(entry['name'])
                saved = cached / member
                if wanted(member) and saved.is_file() and saved.stat().st_size == entry['realSize']:
                    out = target / member
                    place_file(saved, out, link_ok=out.suffix.lower() in IMMUTABLE_SUFFIXES)
                    have.add(member)
                    stats['from_fst'] += 1
                    stats['bytes'] += entry['realSize']
            if not any(wanted(fst_listing.normalize_key(e['name']))
                       for e in entries):
                continue
        for off, comp, real, member, is_lz, blob in fst_lib.fst_entries(src):
            key = member.lower().replace(chr(92), '/')
            if not wanted(key):
                continue
            out = target / key
            out.parent.mkdir(parents=True, exist_ok=True)
            if cached is None:
                out.write_bytes(fst_lib.decompress_entry(off, comp, real, is_lz, blob))
            else:
                saved = cached / key
                if not saved.is_file() or saved.stat().st_size != real:
                    saved.parent.mkdir(parents=True, exist_ok=True)
                    saved.write_bytes(fst_lib.decompress_entry(off, comp, real, is_lz, blob))
                place_file(saved, out, link_ok=out.suffix.lower() in IMMUTABLE_SUFFIXES)
            have.add(key)
            stats['from_fst'] += 1
            stats['bytes'] += real
    return stats


def stage_mod(cache: Path, layers: list[Path], target: Path, stem: str,
              base_install: Path | None = None, cached_layers=False, gap_cache=None) -> dict:
    stats = {'engine': link_engine(cache, target), 'mod_files': 0, 'mod_bytes': 0, 'mod_linked': 0}
    for layer in layers:
        s = overlay_mod(layer, target, cached_links=cached_layers)
        stats['mod_files'] += s['files']
        stats['mod_bytes'] += s['bytes']
        stats['mod_linked'] += s['linked']
    if base_install is not None:
        stats['stock_gap'] = fill_stock_gap(base_install, target, gap_cache)
    stats['mission_extracted'] = ensure_loose_mission(layers, target, stem)
    return stats


def stage_bookmarks(base_install: Path, target: Path, keep_stages: bool):
    """Kept smoke installs are also capture targets; give them independent camera files."""
    source = base_install / 'saved_bookmarks'
    if keep_stages and source.is_dir():
        place_tree(source, target / 'saved_bookmarks', False)


# ---------------------------------------------------------------------------
# Health assertions
# ---------------------------------------------------------------------------

def analyze_health(text: str, ai_deadline: float = AI_DEADLINE_S) -> dict:
    """Mod-health verdicts from engine output.

    mechs ("invisible mechs"): MC2_MECH_BATCHER_STATS summary lines report the
    per-frame mech instance count (GL `instances=`, VK `vk_instances=`); the
    MLR summary reports CPU-path mech draws. Any positive value passes.
    ai ("dead AI on POAR"): [MOD_HEALTH v1] event=brain_runs / first_general_order,
    first seen within `ai_deadline` scenario seconds of mission start.
    """
    inst = [int(x) for x in re.findall(r'\b(?:vk_)?instances=(\d+)', text)]
    mlr = [int(x) for x in re.findall(r'mlr_mech_draws=(\d+)', text)]
    brain = re.search(r'\[MOD_HEALTH v1\] event=brain_runs count=(\d+) scenarioTime=([\d.]+)', text)
    order = re.search(r'\[MOD_HEALTH v1\] event=first_general_order scenarioTime=([\d.]+)', text)
    brain_t = float(brain.group(2)) if brain else None
    order_t = float(order.group(1)) if order else None
    max_inst = max(inst) if inst else 0
    max_mlr = max(mlr) if mlr else 0
    mech_ok = max_inst > 0 or max_mlr > 0
    ai_times = [t for t in (brain_t, order_t) if t is not None]
    ai_ok = bool(ai_times) and min(ai_times) <= ai_deadline
    return {
        'mech_ok': mech_ok, 'mech_instances_max': max_inst, 'mlr_mech_draws': max_mlr,
        'mech_evidence': bool(inst or mlr),
        'ai_ok': ai_ok, 'first_brain_s': brain_t, 'first_order_s': order_t,
    }


def health_cell(h: dict) -> str:
    mech = f"mechs={'OK' if h['mech_ok'] else 'FAIL'}({h['mech_instances_max']}/{h['mlr_mech_draws']})"
    ai = 'ai=OK' if h['ai_ok'] else 'ai=FAIL'
    if h['first_brain_s'] is not None:
        ai += f"(brain@{h['first_brain_s']:.1f}s)"
    return f'{mech} {ai}'


def artifact_log_text(started: float, stem: str, root: Path | None = None) -> str:
    """Engine log run_smoke saved for this mission (tests/smoke/artifacts/<stamp>/<stem>.log)."""
    root = root or Path(os.environ.get('MC2_SMOKE_ARTIFACT_ROOT', ROOT / 'tests' / 'smoke' / 'artifacts'))
    if not root.is_dir():
        return ''
    best = None
    for d in root.iterdir():
        log = d / f'{stem}.log'
        try:
            if log.is_file() and log.stat().st_mtime >= started and (
                    best is None or log.stat().st_mtime > best.stat().st_mtime):
                best = log
        except OSError:
            continue
    return best.read_text(encoding='utf-8', errors='replace') if best else ''


def collect_log_text(smoke_output: str) -> str:
    """Smoke stdout plus every engine log run_smoke names or leaves nearby."""
    parts = [smoke_output]
    for m in re.finditer(r'([A-Za-z]:[\\/][^\s|"\']+\.(?:log|txt|jsonl))', smoke_output):
        p = Path(m.group(1))
        try:
            if p.is_file() and p.stat().st_size < 64 * 1024 * 1024:
                parts.append(p.read_text(encoding='utf-8', errors='replace'))
        except OSError:
            pass
    return '\n'.join(parts)


# ---------------------------------------------------------------------------
# Running
# ---------------------------------------------------------------------------

def tree_identity(path: Path) -> dict:
    count = total = 0
    for dirpath, dirnames, filenames in os.walk(path):
        dirnames.sort()
        for name in sorted(filenames):
            file = Path(dirpath) / name
            if not file.is_file():
                continue
            count += 1
            total += file.stat().st_size
    return {'files': count, 'bytes': total}


def refresh_layer(layer: Path, target: Path) -> dict:
    """Keep only the current mod payload; unchanged files retain their inode."""
    wanted = set()
    for name in ('data', 'assets'):
        wanted.update(p.relative_to(layer) for p in (layer / name).rglob('*')
                      if p.is_file() and not p.is_symlink())
    wanted.update(p.relative_to(layer) for p in layer.iterdir()
                  if p.is_file() and (p.suffix.lower() in ('.fst', '.pak') or p.name in CONFIG_FILES))
    for old in target.rglob('*'):
        if (old.is_file() or old.is_symlink()) and old.relative_to(target) not in wanted:
            old.unlink()
    # Materialize cross-volume sources once in the persistent cache. Stages
    # then hardlink local payload instead of inheriting source-volume symlinks.
    return overlay_mod(layer, target, link_ok=same_volume(layer, target), incremental=True)


def run_mod(staged: Path, stem: str, duration: int, timeout: int, ref_exe: Path | None = None) -> dict:
    # Give the bounded host-lease wait its own allowance in the outer watchdog;
    # a queue wait is not a game hang.
    lease_wait = min(float(os.environ.get('MC2_HOST_LEASE_TIMEOUT') or 1200), 1200)
    wall_timeout = timeout + lease_wait
    cmd = [
        sys.executable, str(ROOT / 'scripts' / 'run_smoke.py'),
        '--exe', str(staged / 'mc2.exe'),
        '--mission', stem,
        '--allow-any-mission',
        '--duration', str(duration),
        '--keep-logs',
        '--wait-timeout', str(lease_wait),
    ]
    if ref_exe is not None:
        cmd += ['--ref-exe', str(ref_exe)]
    started = dt.datetime.now().timestamp() - 2
    env = os.environ.copy()
    # Several campaigns use mc2_01: never collect another worker's health evidence.
    artifacts = staged.parent / 'artifacts' / staged.name
    env['MC2_SMOKE_ARTIFACT_ROOT'] = str(artifacts)
    env.setdefault('MC2_MECH_BATCHER_STATS', '1')
    env.setdefault('MC2_MOD_HEALTH_DIAG', '1')  # enables the [MOD_HEALTH v1] AI-liveness lines
    print('[mod_smoke] run:', ' '.join(cmd), flush=True)
    try:
        proc = subprocess.run(cmd, cwd=str(ROOT), capture_output=True,
                              text=True, timeout=wall_timeout, env=env,
                              **console_child.child_kwargs(redirected=True))
        output = (proc.stdout or '') + (proc.stderr or '')
        rc = proc.returncode
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or '') + (exc.stderr or '')
        if isinstance(output, bytes):
            output = output.decode('utf-8', 'replace')
        output += f'\n[mod_smoke] TIMEOUT after {wall_timeout:g}s (includes lease wait)\n'
        rc = 124
    log = staged / 'smoke.log'
    log.write_text(output, encoding='utf-8', errors='replace')
    fingerprint = next(
        (line.strip() for line in output.splitlines()
         if '[DEPLOY_FINGERPRINT]' in line), '')
    verdict = next(
        (line.strip() for line in output.splitlines()
         if line.startswith('|') and ('PASS' in line or 'FAIL' in line)), '')
    text = '\n'.join([collect_log_text(output), artifact_log_text(started, stem, artifacts)])
    health = analyze_health(text)
    return {'exit': rc, 'fingerprint': fingerprint, 'verdict': verdict,
            'log': str(log), 'health': health, 'note': failure_note(text)}


def failure_note(text: str) -> str:
    """First engine-side fatal line (STOP / FATAL / CRASH / VERIFY), trimmed."""
    for line in text.splitlines():
        if re.search(r'STOP|FATAL|\[CRASH\]|\[VERIFY\]', line):
            return line.strip()[:110]
    return ''


def result_word(row: dict) -> str:
    if row['exit'] != 0:
        return 'FAIL(exit %s)' % row['exit']
    h = row['health']
    if not (h['mech_ok'] and h['ai_ok']):
        return 'HEALTH-FAIL'
    return 'PASS'


def format_table(rows: list[dict]) -> str:
    head = ('mod', 'mission', 'result', 'health', 'note')
    body = [(r['mod'], r['mission'], result_word(r), health_cell(r['health']),
             r.get('note', '') if result_word(r) != 'PASS' else '') for r in rows]
    widths = [max(len(str(x[i])) for x in [head] + body) for i in range(5)]
    line = lambda cells: '| ' + ' | '.join(str(c).ljust(w) for c, w in zip(cells, widths)) + ' |'
    return '\n'.join([line(head), line(['-' * w for w in widths])] + [line(b) for b in body])


def select_mods(arg: str) -> tuple:
    if not arg:
        return BATTERY
    wanted = {m.casefold() for m in split_csv(arg)}
    selected = tuple(row for row in BATTERY if row[0].casefold() in wanted)
    missing = wanted - {row[0].casefold() for row in selected}
    if missing:
        raise ValueError('unknown battery mod(s): ' + ', '.join(sorted(missing)))
    return selected


def discard_legacy_cooked_cache(persistent):
    # Runtime cooks stay disposable; content keys alone cannot bound a shared cache.
    cooked = persistent / 'cooked'
    if cooked.is_dir():
        shutil.rmtree(cooked)


def battery(args) -> int:
    source_root = Path(args.install_root).resolve()
    if source_root.name.lower() == GOLDEN_INSTALL:
        raise ValueError(f'refusing golden install: {source_root}')
    stage_root = Path(args.stage_root).resolve()
    if stage_root.is_relative_to(source_root):
        raise ValueError(f'refusing stage root inside install root: {stage_root}')
    base_install = Path(args.base_install).resolve()
    if base_install.name.lower() == GOLDEN_INSTALL:
        raise ValueError(f'refusing golden base install: {base_install}')
    build, build64 = resolve_build(args.exe, args.build_dir)
    selected = select_mods(args.mod)
    if args.mission and len(selected) != 1:
        raise ValueError('--mission needs exactly one --mod')
    plan = []
    for display, folder, base_mod in selected:
        layers = mod_layers(source_root, folder, base_mod)
        for layer in layers:
            if not layer.is_dir():
                raise FileNotFoundError(f'mod install not found: {layer}')
        stem, how = pick_mission(layers, args.mission)
        plan.append((display, folder, layers, stem, how))
    stages.preflight(stage_root)
    # _cache is persistent payload, never an aged disposable run.
    if not args.keep_stages:
        stages.prune_old(stage_root, exclude=('_cache',))
    run_root = stage_root / dt.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    run_root.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    print('[mod_smoke] engine:', build)
    cache = run_root / '_engine'
    persistent = stage_root / '_cache'
    rows, futures = [], []
    # A batch token pins one slot. Sharing it across children would bypass isolation.
    workers = 1 if os.environ.get('MC2_HOST_LEASE_BATCH') or args.fail_fast else getattr(args, 'workers', 2)
    def smoke(entry, staged, stats, stage_s):
        display, folder, layers, stem, how = entry
        run_started = time.monotonic()
        timeout = max(args.timeout, 600) if folder == 'MCO-Carver5' else args.timeout
        result = run_mod(staged, stem, args.duration, timeout, getattr(args, 'ref_exe', None))
        log = run_root / (folder + '.log')
        shutil.copy2(staged / 'smoke.log', log)
        result.update({'mod': display, 'mission': stem, 'mission_source': how,
                       'staged': str(staged), 'stage_stats': stats, 'log': str(log),
                       'stage_s': stage_s, 'run_s': time.monotonic() - run_started,
                       'identity': tree_identity(staged)})
        print('[mod_smoke]', display, result_word(result), health_cell(result['health']),
              f"stage={stage_s:.1f}s run={result['run_s']:.1f}s", flush=True)
        return result
    try:
        engine_started = time.monotonic()
        summary = build_engine_cache(cache, build, build64, base_install)
        discard_legacy_cooked_cache(persistent)
        print('[mod_smoke] engine payload:', json.dumps(summary),
              f'elapsed={time.monotonic() - engine_started:.1f}s', flush=True)
        layer_cache = {}
        # Staging stays serial: shared extraction/cache writes have one owner.
        # Each child acquires its own normal FIFO game-worker lease.
        with ThreadPoolExecutor(max_workers=workers) as pool:
            for entry in plan:
                display, folder, layers, stem, how = entry
                stage_started = time.monotonic()
                cached = []
                for layer in layers:
                    if layer not in layer_cache:
                        dest = persistent / 'layers' / layer.name
                        refresh_layer(layer, dest)
                        layer_cache[layer] = dest
                    cached.append(layer_cache[layer])
                staged = run_root / folder
                stats = stage_mod(cache, cached, staged, stem, base_install,
                                  cached_layers=True, gap_cache=persistent / 'stock-gap')
                stage_bookmarks(base_install, staged, args.keep_stages)
                _load_deploy_payload().ensure_terrain_materials(str(staged))
                stage_s = time.monotonic() - stage_started
                print(f'[mod_smoke] {display}: mission {stem} ({how}); staged {stats}', flush=True)
                future = pool.submit(smoke, entry, staged, stats, stage_s)
                futures.append(future)
                if workers == 1:
                    result = future.result()
                if args.fail_fast and result_word(result) != 'PASS':
                    break
            rows = [future.result() for future in futures]
    finally:
        if not args.keep_stages:
            for disposable in [cache, *(run_root / entry[1] for entry in plan)]:
                if disposable.exists():
                    shutil.rmtree(disposable)
            for result in rows:
                result['staged'] = 'deleted (logs retained)'
        print(f'[mod_smoke] total={time.monotonic() - started:.1f}s workers={workers}', flush=True)
    run_root.mkdir(exist_ok=True)
    report = run_root / 'battery.json'
    report.write_text(json.dumps(rows, indent=2), encoding='utf-8')
    print(format_table(rows))
    print('[mod_smoke] report:', report)
    return 0 if rows and all(result_word(r) == 'PASS' for r in rows) else 1


def main() -> int:
    ap = argparse.ArgumentParser(description='Smoke every supported mod against current mc2.exe')
    ap.add_argument('--battery', action='store_true', help='run all six mods across leased game workers')
    ap.add_argument('--mod', default='', help='comma-separated mod name(s)')
    ap.add_argument('--mission', default='', help='mission stem override (needs a single --mod)')
    ap.add_argument('--exe', default=None, help='current mc2.exe')
    ap.add_argument('--ref-exe', type=Path, help='explicit smoke byte-identity reference (default: worktree HEAD)')
    ap.add_argument('--build-dir', default='build64-ninja')
    ap.add_argument('--install-root', default=str(DEFAULT_INSTALL_ROOT))
    ap.add_argument('--stage-root', default=str(DEFAULT_BATTERY_ROOT))
    ap.add_argument('--base-install', default=str(DEFAULT_BASE_INSTALL),
                    help='read-only install supplying assets, stock FSTs and configs')
    ap.add_argument('--workers', type=int, choices=(1, 2), default=2)
    ap.add_argument('--duration', type=int, default=5)
    ap.add_argument('--timeout', type=int, default=300)
    ap.add_argument('--fail-fast', action='store_true')
    ap.add_argument('--keep-stages', action='store_true',
                    help='keep every staged install (default: delete a stage after a PASS)')
    args = ap.parse_args()
    if not 1 <= args.duration <= 30:
        ap.error('--duration must be between 1 and 30')
    if not args.battery and not args.mod:
        ap.error('pass --battery or --mod NAME')
    try:
        return battery(args)
    except (OSError, ValueError) as exc:
        print(f'[mod_smoke] ERROR: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
