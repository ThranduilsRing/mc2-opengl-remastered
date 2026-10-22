#!/usr/bin/env python3
'Run the six legacy mod installs against the current mc2.exe: py -3 tools/mod_smoke.py --battery'
#
# Stages disposable copies under G:/mc2-battery/mod_smoke/ (never writes into a
# game install) and hands each staged install to scripts/run_smoke.py.
#
#     py -3 tools/mod_smoke.py --battery
#
# Each mod gets the current mc2.exe, runtime DLLs, shaders and PDB, beside a
# copy of its data/, root *.fst and root *.pak. The mapped first mission is
# staged as loose data/missions/<stem>.fit so run_smoke can launch it with
# --allow-any-mission. Exit 0 only if every mod's smoke passed.

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.append(str(Path(__file__).resolve().parent.parent / "scripts"))
import console_child  # noqa: E402  (windowless console children)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INSTALL_ROOT = Path('A:/Games/mc2-opengl')
DEFAULT_BATTERY_ROOT = Path('G:/mc2-battery/mod_smoke')
DEFAULT_BASE_INSTALL = Path('A:/Games/mc2-opengl/mc2-win64-v0.4')
GOLDEN_INSTALL = 'mc2-win64-v0.4c'

# (display name, install folder, mission stem, where the mission lives)
BATTERY = (
    ('MC2X-CVE-G', 'MC2X-CVE-G', 'mc2_01', 'mission.fst'),
    ('MC2X-REAP3', 'MC2X-REAP3', 'reap3m01', 'mission.fst'),
    ('MC2-Exodus', 'MC2-Exodus', 'a_0_1', 'loose'),
    ('MC2-MagicExpansion', 'MC2-MagicExpansion', 'mc2_01', 'loose'),
    ('MechCommanderOmnitech', 'MechCommanderOmnitech', 'a_0_1', 'loose'),
    ('MCO-Carver5', 'MCO-Carver5', 'mc2_01', 'loose'),
)

# Engine files deployed beside the legacy data. These mirror the game half of
# scripts/deploy_payload.py, without its canonical-install and manifest rules.
RUNTIME_FILES = (
    'mc2.exe', 'mc2.pdb', 'mc2res_64.dll', 'mc2res_64d.dll',
    'glew32.dll', 'SDL2.dll', 'SDL2_mixer.dll', 'avcodec-61.dll',
    'avformat-61.dll', 'avutil-59.dll', 'swscale-8.dll',
    'swresample-5.dll', 'zlib1.dll',
)
OPTIONAL_RUNTIME = frozenset({
    'mc2res_64d.dll', 'avcodec-61.dll', 'avformat-61.dll', 'avutil-59.dll',
    'swscale-8.dll', 'swresample-5.dll',
})
# Files deploy_payload.py copies from source when present.
SOURCE_FILES = ('data/terrain_materials.json', 'data/visual_tuning.json', 'system.cfg')
SOURCE_TREES = ('data/defs', 'data/vegetation/cards')
# Root data files that override the mod pack and must not be copied over it.
STOCK_ROOT_FILES = ('prefs.cfg', 'options.cfg', 'system.cfg')
CONFIG_FILES = ('prefs.cfg', 'options.cfg', 'minprefs.cfg')
STOCK_FSTS = ('mission.fst', 'tgl.fst', 'art.fst', 'textures.fst', 'misc.fst',
              'camera.fst', 'effect.fst', 'insignia.fst')


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
    return build, root


def copy_file(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)


def copy_tree(source: Path, target: Path) -> tuple[int, int]:
    files = total = 0
    for dirpath, dirnames, filenames in os.walk(source):
        dirnames.sort()
        rel_dir = Path(dirpath).relative_to(source)
        (target / rel_dir).mkdir(parents=True, exist_ok=True)
        for name in sorted(filenames):
            src = Path(dirpath) / name
            if src.is_symlink() or not src.is_file():
                continue
            dst = target / rel_dir / name
            copy_file(src, dst)
            files += 1
            total += dst.stat().st_size
    return files, total


def stage_engine(build: Path, build64: Path, target: Path) -> tuple[list[str], list[str]]:
    copied = []
    missing = []
    for name in RUNTIME_FILES:
        src = build / name
        if src.is_file():
            copy_file(src, target / name)
            copied.append(name)
        elif name not in OPTIONAL_RUNTIME:
            missing.append(name)
    if missing:
        raise FileNotFoundError('missing runtime file(s): ' + ', '.join(missing))
    for tree in SOURCE_TREES:
        src = ROOT / tree
        if not src.is_dir():
            raise FileNotFoundError(f'required runtime tree missing: {src}')
        count, _ = copy_tree(src, target / tree)
        copied.append(f'{tree}/ ({count} files)')
    for rel in SOURCE_FILES:
        src = ROOT / rel
        if src.is_file():
            copy_file(src, target / rel)
            copied.append(rel)
    shaders = ROOT / 'shaders'
    if not shaders.is_dir():
        raise FileNotFoundError(f'shader sources not found: {shaders}')
    count, _ = copy_tree(shaders, target / 'shaders')
    copied.append(f'shaders/ ({count} files)')
    vulkan = build64 / 'shaders' / 'vulkan'
    if vulkan.is_dir():
        count, _ = copy_tree(vulkan, target / 'shaders' / 'vulkan')
        copied.append(f'shaders/vulkan/ ({count} SPIR-V files)')
    return copied, missing


def stage_mod(mod_dir: Path, target: Path, stem: str, shape: str,
              base_install: Path | None = None) -> dict:
    stats = {'files': 0, 'bytes': 0, 'mission_source': shape}
    data = mod_dir / 'data'
    if not data.is_dir():
        raise FileNotFoundError(f'mod has no data dir: {data}')
    stats['files'], stats['bytes'] = copy_tree(data, target / 'data')
    # Runtime fonts live outside data/. Preserve mod overrides and fill absent
    # engine assets from the read-only base, just as for stock fast files below.
    asset_sources = ([base_install / 'assets'] if base_install else []) + [mod_dir / 'assets']
    for assets in asset_sources:
        if assets.is_dir():
            count, size = copy_tree(assets, target / 'assets')
            stats['files'] += count
            stats['bytes'] += size
    stock = {s.lower() for s in STOCK_ROOT_FILES}
    for entry in sorted(mod_dir.iterdir()):
        if not entry.is_file() or entry.name.lower() in stock:
            continue
        if entry.suffix.lower() not in ('.fst', '.pak'):
            continue
        copy_file(entry, target / entry.name)
        stats['files'] += 1
        stats['bytes'] += entry.stat().st_size
    for name in STOCK_FSTS:
        dst = target / name
        if dst.is_file():
            continue
        src = base_install / name if base_install else None
        if src is None or not src.is_file():
            raise FileNotFoundError(f'missing required fast file {name} in {mod_dir}')
        copy_file(src, dst)
        stats['files'] += 1
        stats['bytes'] += dst.stat().st_size
    for name in CONFIG_FILES:
        dst = target / name
        src = mod_dir / name
        if not src.is_file():
            src = base_install / name if base_install else None
        if src is None or not src.is_file():
            raise FileNotFoundError(f'missing required config {name} in {mod_dir}')
        if not dst.is_file():
            copy_file(src, dst)
            stats['files'] += 1
            stats['bytes'] += dst.stat().st_size
    if shape == 'mission.fst':
        fst = mod_dir / 'mission.fst'
        if not fst.is_file():
            raise FileNotFoundError(f'mission.fst not found: {fst}')
        out = target / 'data' / 'missions' / f'{stem}.fit'
        extract_mission(fst, stem, out)
        stats['files'] += 1
        stats['bytes'] += out.stat().st_size
    else:
        fit = mod_dir / 'data' / 'missions' / f'{stem}.fit'
        if not fit.is_file():
            raise FileNotFoundError(f'mapped mission not found: {fit}')
    return stats


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


def tree_identity(path: Path) -> dict:
    count = total = 0
    digest = hashlib.sha256()
    for dirpath, dirnames, filenames in os.walk(path):
        dirnames.sort()
        for name in sorted(filenames):
            file = Path(dirpath) / name
            if not file.is_file() or file.is_symlink():
                continue
            rel = file.relative_to(path).as_posix()
            digest.update(rel.encode('utf-8') + b'\0')
            digest.update(sha256(file).encode('ascii'))
            count += 1
            total += file.stat().st_size
    return {'files': count, 'bytes': total, 'sha256': digest.hexdigest()}


def run_mod(staged: Path, stem: str, duration: int, timeout: int) -> dict:
    cmd = [
        sys.executable, str(ROOT / 'scripts' / 'run_smoke.py'),
        '--exe', str(staged / 'mc2.exe'),
        '--mission', stem,
        '--allow-any-mission',
        '--duration', str(duration),
        '--keep-logs',
    ]
    print('[mod_smoke] run:', ' '.join(cmd), flush=True)
    try:
        proc = subprocess.run(cmd, cwd=str(ROOT), capture_output=True,
                              text=True, timeout=timeout,
                              **console_child.child_kwargs(redirected=True))
        output = (proc.stdout or '') + (proc.stderr or '')
        rc = proc.returncode
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or '') + (exc.stderr or '')
        if isinstance(output, bytes):
            output = output.decode('utf-8', 'replace')
        output += f'\n[mod_smoke] TIMEOUT after {timeout}s\n'
        rc = 124
    log = staged / 'smoke.log'
    log.write_text(output, encoding='utf-8', errors='replace')
    fingerprint = next(
        (line.strip() for line in output.splitlines()
         if '[DEPLOY_FINGERPRINT]' in line), '')
    verdict = next(
        (line.strip() for line in output.splitlines()
         if line.startswith('|') and ('PASS' in line or 'FAIL' in line)), '')
    return {'exit': rc, 'fingerprint': fingerprint, 'verdict': verdict,
            'log': str(log)}


def battery(args) -> int:
    source_root = Path(args.install_root).resolve()
    if source_root.name.lower() == GOLDEN_INSTALL:
        raise ValueError(f'refusing golden install: {source_root}')
    stage_root = Path(args.stage_root).resolve()
    if args.install_root and stage_root.is_relative_to(Path(args.install_root).resolve()):
        raise ValueError(f'refusing stage root inside install root: {stage_root}')
    base_install = Path(args.base_install).resolve() if args.base_install else None
    if base_install and base_install.name.lower() == GOLDEN_INSTALL:
        raise ValueError(f'refusing golden base install: {base_install}')
    build, build64 = resolve_build(args.exe, args.build_dir)
    run_root = stage_root / dt.datetime.now().strftime('%Y%m%d-%H%M%S')
    run_root.mkdir(parents=True, exist_ok=False)
    selected = BATTERY
    if args.mod:
        wanted = {m.casefold() for m in split_csv(args.mod)}
        selected = tuple(row for row in BATTERY if row[0].casefold() in wanted)
        missing = wanted - {row[0].casefold() for row in selected}
        if missing:
            raise ValueError('unknown battery mod(s): ' + ', '.join(sorted(missing)))
    for display, folder, stem, shape in selected:
        mod_dir = source_root / folder
        if not mod_dir.is_dir():
            raise FileNotFoundError(f'mod install not found: {mod_dir}')
        if shape == 'mission.fst':
            if not (mod_dir / 'mission.fst').is_file():
                raise FileNotFoundError(f'mission.fst not found: {mod_dir}')
        elif not (mod_dir / 'data' / 'missions' / f'{stem}.fit').is_file():
            raise FileNotFoundError(f'mapped mission not found in {mod_dir}: {stem}')
        missing = [name for name in STOCK_FSTS if not (mod_dir / name).is_file()]
        for name in missing:
            if base_install is None or not (base_install / name).is_file():
                raise FileNotFoundError(
                    f'missing required fast file {name} in {mod_dir} and no base install')
        for name in CONFIG_FILES:
            if (mod_dir / name).is_file():
                continue
            if base_install is None or not (base_install / name).is_file():
                raise FileNotFoundError(
                    f'missing required config {name} in {mod_dir} and no base install')
    return _run_battery(args, build, build64, run_root, source_root, selected, base_install)


def _run_battery(args, build, build64, run_root, source_root, selected, base_install) -> int:
    print('[mod_smoke] engine:', build)
    rows = []
    for display, folder, stem, shape in selected:
        mod_dir = source_root / folder
        staged = run_root / folder
        staged.mkdir()
        copied, _ = stage_engine(build, build64, staged)
        stats = stage_mod(mod_dir, staged, stem, shape, base_install)
        identity = tree_identity(staged)
        result = run_mod(staged, stem, args.duration, args.timeout)
        result.update({'mod': display, 'mission': stem, 'staged': str(staged), 'data_identity': identity, 'runtime_files': copied})
        rows.append(result)
        print('[mod_smoke]', display, 'exit', result['exit'])
        if result['exit'] != 0 and args.fail_fast:
            break
    report = run_root / 'battery.json'
    report.write_text(json.dumps(rows, indent=2), encoding='utf-8')
    print('[mod_smoke] report:', report)
    return 0 if rows and all(row['exit'] == 0 for row in rows) else 1


def main() -> int:
    ap = argparse.ArgumentParser(description='Run the six legacy mods against current mc2.exe')
    ap.add_argument('--battery', action='store_true', help='run all six legacy mods (default)')
    ap.add_argument('--mod', default='', help='comma-separated mod name(s)')
    ap.add_argument('--exe', default=None, help='current mc2.exe')
    ap.add_argument('--build-dir', default='build64-ninja')
    ap.add_argument('--install-root', default=str(DEFAULT_INSTALL_ROOT))
    ap.add_argument('--stage-root', default=str(DEFAULT_BATTERY_ROOT))
    ap.add_argument('--base-install', default=str(DEFAULT_BASE_INSTALL),
                    help='read-only install supplying stock FSTs the mod lacks')
    ap.add_argument('--duration', type=int, default=30)
    ap.add_argument('--timeout', type=int, default=300)
    ap.add_argument('--fail-fast', action='store_true')
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
