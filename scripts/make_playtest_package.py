"""Assemble a clean player tree; trigger paths: this script and tools/playtest/."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

import console_child
import release_asset_guard

PERMISSIONS = Path(__file__).resolve().parents[1] / 'tools/playtest/bundle_permissions.json'
# Generated inventory outputs never live in the repo: MC2_PLAYTEST_OUT, else $MC2_LANE_SCRATCH, else the temp dir.
GENERATED = Path(os.environ.get('MC2_PLAYTEST_OUT') or os.environ.get('MC2_LANE_SCRATCH')
                 or os.environ.get('TEMP', '.')) / 'playtest-generated'
INVENTORY = GENERATED / 'inventory.json'
REPOSITORY = PERMISSIONS.parents[2]
# BT2018-derived loose files flagged by release_asset_guard: extractor-only, never packaged.
# Keyed by physical path (case-folded); the loader falls back to the stock model when absent.
BT2018_REASON = 'BT2018 extractor-only'
BT2018_LIST = PERMISSIONS.with_name('bt2018_extractor_only.txt')


def bt2018_extractor_only(rel):
    global _BT2018
    if '_BT2018' not in globals():
        _BT2018 = {x.strip().casefold() for x in BT2018_LIST.read_text().splitlines() if x.strip()}
    return rel.casefold() in _BT2018
CREDITS_SOURCE = Path(os.environ.get('MC2_PLAYTEST_CREDITS_SOURCE', 'A:/Games/mc2-opengl/mc2-win64-v0.4'))
DESERT_EULA = 'mods/desertfoxformcosetup/MCO EULA campaigns.txt'
DENY_PATTERNS = (r'(^|/)mw5[^/]*(/|$)', r'(^|/)bt2018[^/]*(/|$)', r'(^|/)hbs(/|$)',
                 r'(^|/)pgi(/|$)', r'\.uasset$', r'\.assetbundle$',
                 r'\.(uexp|ubulk|umap|utoc|ucas)$',
                 r'^data/tgl/mw5/.*\.glb$')
STOCK_ROOT = ('art.fst', 'camera.fst', 'effect.fst', 'insignia.fst', 'misc.fst',
              'mission.fst', 'textures.fst', 'tgl.fst', 'system.cfg', 'prefs.cfg',
              'minprefs.cfg', 'orgprefs.cfg', 'license.txt', 'EULA.txt')
DEV_ROOT = {'Mission Editor.exe', 'mc2_asset_viewer.exe', 'run-editor.bat',
            'run-mc2.bat', 'run-with-log.bat'}
SKIP_DIRS = {'cache', 'cook-cache', '_aoktxtmp', 'terrain_gen_out', 'dev_shell_out',
             'debug_state', 'crashes', 'savegame', '_txm_cache_bak', 'txm_backup'}
MACHINE = re.compile(r'(?:[AEG]:[/\\]|C:[/\\]Users[/\\])', re.I)
EXTERNAL = json.loads((PERMISSIONS.parents[2] / 'docs/testing/mech_glb_external_pack.json').read_text())
PERSONAL_NAMES = {Path(r['glb']).stem.lower() + suffix for r in EXTERNAL['entries']
                  if r.get('source_type') == 'external'
                  for suffix in ('.glb', '.package.json', '.mcasset.json')}
# Legacy loose files confirmed by the release asset signature guard.
PERSONAL_NAMES.update({'archer.glb', 'archer_bha.glb', 'atlas.glb', 'awesome.glb', 'banshee.glb', 'madcat.glb'})


def hard_denied(rel):
    return any(re.search(pattern, rel.replace('\\', '/'), re.I) for pattern in DENY_PATTERNS)


def clean_head(repository=REPOSITORY):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(repository), *args], text=True,
                                       **console_child.child_kwargs(redirected=True)).strip()
    head = git('rev-parse', 'HEAD')
    if git('status', '--porcelain', '--untracked-files=all'):
        raise ValueError('package receipt requires a clean tree')
    return head


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def safe_path(root, rel):
    if hard_denied(rel):
        raise ValueError(f'hard-denied path: {rel}')
    path = (root / rel).resolve()
    if (not rel or ':' in rel or '\\' in rel or '..' in Path(rel).parts
            or not path.is_relative_to(root.resolve())):
        raise ValueError(f'unsafe path: {rel}')
    # Name denials above must run before even the four-byte packet header read.
    if path.suffix.lower() == '.pak' and path.is_file():
        with path.open('rb') as stream:
            if stream.read(4) != b'\xce\xfa\xed\xfe':
                raise ValueError(f'hard-denied PAK header: {rel}')
    return path


def excluded(rel):
    p = Path(rel)
    low = p.name.lower()
    importer = rel in {'tools/import_campaign.py', 'scripts/console_child.py',
        'scripts/smoke_lib/child_registry.py'} or rel.startswith(
        ('tools/mc2_import/', 'tools/mc2x_import/', 'tools/mco_import/'))
    return (bool(set(p.parts) & SKIP_DIRS) or any(x.startswith('sample_') for x in p.parts)
            or 'bt2018_mechs' in p.parts
            or low.startswith(tuple(token.decode().lower() for token in release_asset_guard.TOKENS))
            or any(x in low for x in ('.bak', '.orig'))
            or low.endswith(('.pdb', '.log', '.dmp', '.cookstamp', '.tmp'))
            or low == '.modindex-cache' or low.endswith('.mc2ir.json')
            or low in {'mc2x_import_report.json', 'mco_compat_report.json'}
            or ('data/tgl/' in rel.lower() and low in PERSONAL_NAMES)
            or rel in DEV_ROOT or (p.parts[0] in {'tools', 'scripts'} and not importer))


def credit_files(mod, record):
    yield record['credits_file'], 'credits_sha256', mod + '.txt'
    if mod == 'desert-fox-478ca2f8':
        yield DESERT_EULA, 'eula_sha256', 'MCO-EULA-campaigns.txt'


def verified_credits(source, mod, record):
    result = []
    for rel, pin, destination in credit_files(mod, record):
        path = safe_path(source, rel)
        if path.suffix.lower() not in {'.txt', '.md'} or not path.is_file():
            raise ValueError(f'missing credits text: {mod}: {rel}')
        if not record.get(pin) or sha256(path) != record[pin]:
            raise ValueError(f'credits hash mismatch: {mod}: {rel}')
        result.append((path, destination, record[pin]))
    return result


def permitted_mods(source, requested, permissions, credits_source=None):
    result = {}
    for mod in requested:
        if (mod not in permissions or permissions[mod].get('bundle') is False
                or excluded('mods/' + mod + '/mod.json')):
            raise ValueError(f'unlisted or forbidden mod: {mod}')
        record = permissions[mod]
        if not all(record.get(k) for k in ('permission_source', 'terms', 'author', 'credits_file')):
            raise ValueError(f'incomplete permission: {mod}')
        verified_credits(credits_source or source, mod, record)
        if record.get('kind') != 'loose':
            manifest = json.loads(safe_path(source, f'mods/{mod}/mod.json').read_text())
            if manifest['id'] != mod or set(manifest.get('dependencies', [])) - set(requested):
                raise ValueError(f'mod id/dependency mismatch: {mod}')
        result[mod] = record
    return result


def scan(tree):
    failures = []
    for path in tree.rglob('*'):
        if not path.is_file():
            continue
        rel = path.relative_to(tree).as_posix()
        if hard_denied(rel):
            raise ValueError(f'hard-denied path: {rel}')
        safe_path(tree, rel)
        if excluded(rel):
            failures.append(rel)
        if path.suffix.lower() in {'.cfg', '.json', '.ini', '.md', '.py', '.txt', '.bat'} and MACHINE.search(path.read_text(errors='replace')):
            failures.append('machine path: ' + rel)
    if failures:
        raise ValueError('\n'.join(failures))


def assemble(source, target, permissions, requested, *, inventory=None, repository=REPOSITORY,
             credits_source=None):
    head = clean_head(repository)
    inventory = inventory if inventory is not None else json.loads(INVENTORY.read_text())['files']
    rules = json.loads(PERMISSIONS.with_name('tier_rules.json').read_text())
    def rule_exclusion(rel):
        if bt2018_extractor_only(rel):
            return BT2018_REASON
        return next((r['reason'] for r in rules if r['action'] == 'exclude'
                     and re.fullmatch(r['regex'], rel)), None)
    present = {p.relative_to(source).as_posix() for p in source.rglob('*') if p.is_file()}
    # Reject candidate assets before permission processing can hash credit texts.
    for rel in sorted(present):
        if rel.lower().startswith(('data/', 'assets/')) or any(
                rel.startswith('mods/' + mod + '/') for mod in requested):
            safe_path(source, rel)
    credits_source = credits_source or source
    mods = permitted_mods(source, requested, permissions, credits_source)
    if target.exists():
        raise ValueError('target must be a new empty staging path')
    if target.resolve().is_relative_to(source.resolve()):
        raise ValueError('target must be outside source')
    with (source / '.deployed_manifest.csv').open(newline='', encoding='utf-8') as stream:
        if next(csv.reader(stream))[:2] != ['manifest_version', 'v1']:
            raise ValueError('unsupported deploy manifest')
        payload = {r['relpath']: r for r in csv.DictReader(stream)}
    # Enumerate names only; no denied asset bytes may be opened, even for hashing.
    rows = {rel: None for rel in present if rel.lower().startswith(('data/', 'assets/'))
            or ('/' not in rel and rel.lower().endswith('.fst'))}
    rows.update({rel: None for rel in STOCK_ROOT})
    rows.update(payload)
    rows = {rel: row for rel, row in rows.items() if not rel.lower().startswith('mods/')}
    # Mods are never flattened: each ships under mods/<id>/. Only the same physical
    # packaged path claimed twice is a collision; logical overlap is reported later.
    logical_owners = {}
    physical = {rel.casefold(): rel for rel in rows}
    for mod in mods:
        for path in (source / 'mods' / mod).rglob('*'):
            if path.is_file():
                rel = path.relative_to(source).as_posix()
                if rel.casefold() in physical:
                    raise ValueError(f'mod physical path claimed twice: {rel}')
                physical[rel.casefold()] = rel
                rows[rel] = None
                logical = path.relative_to(source / 'mods' / mod).as_posix()
                if logical.lower().startswith(('data/', 'assets/')):
                    logical_owners.setdefault(logical.casefold(), []).append((mod, rel, logical))
    # Hard deny precedes exclusions, stock pins, mod permission and byte reads.
    for rel in rows:
        safe_path(source, rel)
    rows = {rel: row for rel, row in rows.items() if not excluded(rel)}
    rows = {rel: row for rel, row in rows.items() if not rule_exclusion(rel)}
    excluded_counts = {}
    for rel in present - rows.keys():
        reason = ('hard-denied' if hard_denied(rel) else 'development/cache/personal' if excluded(rel)
                  else rule_exclusion(rel) or ('unbundled-mod' if rel.startswith('mods/') else 'outside-payload'))
        excluded_counts[reason] = excluded_counts.get(reason, 0) + 1
    # Approvals are keyed by physical path (mods/<id>/<rel>). Sources: the record's own
    # 'files' map (physical keys) and the generator-written inventory rows; never current bytes.
    approvals = {}
    for mod, record in mods.items():
        for rel, digest in record.get('files', {}).items():
            key = rel.casefold()
            if key in approvals:
                raise ValueError(f'mod physical path claimed twice: {rel}')
            approvals[key] = digest
    def approval(rel):
        digest = approvals.get(rel.casefold())
        if digest is None:
            row = inventory.get(rel)
            digest = row.get('sha256') if isinstance(row, dict) else row
        return digest
    verified = {}
    for rel, row in sorted(rows.items()):
        digest = sha256(safe_path(source, rel))
        if row and digest != row['sha256']:
            raise ValueError(f'deploy hash mismatch: {rel}')
        if (rel.lower().startswith(('data/', 'assets/')) or rel.lower() in {p.lower() for p in STOCK_ROOT}
                or ('/' not in rel and rel.lower().endswith('.fst'))):
            if inventory.get(rel, {}).get('sha256') != digest:
                raise ValueError(f'unapproved stock path/hash: {rel}')
        if rel.startswith('mods/') and rel.split('/')[1] in mods:
            approved = approval(rel)
            if approved is None:
                raise ValueError(f'mod file without approval: {rel}')
            if approved != digest:
                raise ValueError(f'mod file approval digest mismatch: {rel}')
        verified[rel] = digest
    overlaps = []
    for owners in logical_owners.values():
        if len(owners) > 1:
            first = owners[0]
            for other in owners[1:]:
                same = verified[first[1]] == verified[other[1]]
                overlaps.append({'level': 'INFO' if same else 'WARN', 'path': first[2],
                                 'mods': [first[0], other[0]],
                                 'sha256': [verified[first[1]], verified[other[1]]]})
    overlaps.sort(key=lambda o: (o['level'], o['path'], o['mods']))
    target.mkdir(parents=True)
    receipt = []
    for rel, row in sorted(rows.items()):
        src = safe_path(source, rel)
        dst = safe_path(target, rel)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        with dst.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != verified[rel]:
            raise ValueError(f'source changed during packaging: {rel}')
        if rel.endswith('.mcasset.json'):
            metadata = json.loads(dst.read_text())
            # Runtime reads axis/yaw; source_ase is authoring provenance only.
            if 'source_ase' in metadata:
                metadata['source_ase'] = metadata['source_ase'].replace('\\', '/').split('/')[-1]
                dst.write_text(json.dumps(metadata, indent=2) + '\n')
                with dst.open('rb') as stream:
                    digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        receipt.append({'relpath': rel, 'sha256': digest, 'bytes': dst.stat().st_size,
                        'source': 'stock-inventory' if inventory.get(rel, {}).get('sha256') == verified[rel]
                        else 'permitted-mod' if rel.startswith('mods/') or rel in approvals else 'deploy-manifest'})
    for mod, record in mods.items():
        for src, destination, digest in verified_credits(credits_source, mod, record):
            credit = target / 'credits' / destination
            credit.parent.mkdir(exist_ok=True)
            shutil.copyfile(src, credit)
            if sha256(credit) != digest:
                raise ValueError(f'credits hash mismatch after copy: {destination}')
    (target / 'bundle_permissions.json').write_text(json.dumps(mods, indent=2) + '\n')
    # The engine selects desktop borderless for FullScreen; omit owner resolution and state.
    (target / 'options.cfg').write_text('FITini\n[MechCommander2]\nb FullScreen = TRUE\n'
        'b Shadows = TRUE\nb DetailTexture = TRUE\nb HighObjectDetail = TRUE\nl Difficulty = 1\nFITend\n')
    addons = ','.join(m for m in mods if m not in {'exodus-dca4afc8', 'desert-fox-478ca2f8'}
                     and mods[m].get('kind') != 'loose')
    launch = '@echo off\ncd /d "%~dp0"\nset MC2_MOD_DEPS=' + addons + '\nset MC2_NO_LAUNCHER=1\n'
    for name, active, campaign in [('PLAY', '', ''), ('PLAY-EXODUS', 'exodus-dca4afc8', 'campaign'),
            ('PLAY-MW2', 'exodus-dca4afc8', 'mw2mercs'), ('PLAY-DESERT-FOX', 'desert-fox-478ca2f8', 'desertfox')]:
        (target / (name + '.bat')).write_text(launch + f'set MC2_ACTIVE_MOD={active}\n'
            + f'set MC2_BOOT_TO_BAY={campaign}\nmc2.exe\n')
    (target / 'PLAYTEST-README.txt').write_text(
        'MECHCOMMANDER 2 - PRIVATE COMMUNITY PLAYTEST\n'
        'Windows x64; Vulkan 1.3 capable GPU and current graphics drivers required.\n'
        'Extract the entire ZIP to a writable folder. Run PLAY.bat for stock campaign.\n'
        'Run PLAY-EXODUS.bat, PLAY-MW2.bat or PLAY-DESERT-FOX.bat for imported campaigns.\n'
        'These shortcuts enable the bundled Magic weapon and gamesys changes.\n'
        'Borderless desktop display; use the in-game graphics options to change quality/resolution.\n'
        'Credits: Microsoft/FASA, original MechCommander 2 source and game; MC2 remaster contributors;\n'
        'Magic and the MCO team; Exodus/MW2 campaign contributors; Mechlab (Desert Fox).\n'
        'Industrial/vehicle kits included: original geometry/shaders and attributed CC0 texture sources.\n'
        'Broadleaf trees: CGTrader pack, CC0 1.0 per owner ruling 2026-11-16.\n'
        'Poly Haven CC0 1.0 trees: island_tree_01/02/03, jacaranda_tree, tree_small_02; polyhaven.com.\n'
        'Magic permission: Discord, 2026-11-15, with attribution. See credits/ and bundle_permissions.json.\n'
        'Library and asset notices: THIRD_PARTY_NOTICES.txt, CREDITS.txt and 3rdparty/.\n'
        'Known issues: imported save/load may change a hidden Designer variant flag; avoid relying on\n'
        'save round trips for a long campaign until validated. Imported missions may log recovered\n'
        'commander/team warnings. Their player-visible impact has not yet been established for this package.\n'
        'The upstream FSR SDK can log a storage-image validation warning; use native rendering.\n'
        'If a crash occurs, send the crashes/ bundle and reproduction steps to the person who sent\n'
        'this build. Include campaign, mission, screenshot and your GPU/driver version.\n'
        'Noncommercial playtest. Do not remove original notices.\n')
    with (target / 'THIRD_PARTY_NOTICES.txt').open('a', encoding='utf-8') as stream:
        stream.write('\nCGTrader Broadleaf pack: CC0 1.0 per owner ruling 2026-11-16.\n')
        stream.write('\nPoly Haven CC0 1.0: island_tree_01, island_tree_02, island_tree_03, jacaranda_tree, tree_small_02; polyhaven.com.\n')
        stream.write('\nCommunity campaign credits and permission:\n')
        for mod, r in mods.items():
            stream.write(f"{mod}: {r['author']}; {r['permission_source']}; {r['terms']}; credits/{mod}.txt\n")
    if any(record.get('provenance', '').startswith('tools/playtest/kit_cc0_sources.json')
           for record in mods.values()):
        shutil.copyfile(PERMISSIONS.parent / 'kit_cc0_sources.json', target / 'credits/kit_cc0_sources.json')
    scan(target)
    with (target / '.deployed_manifest.csv').open('w', newline='') as stream:
        csv.writer(stream).writerow(['manifest_version', 'v1'])
        writer = csv.DictWriter(stream, fieldnames=['relpath', 'sha256', 'bytes', 'src_commit', 'timestamp', 'mtime_ns'])
        writer.writeheader()
        for row in receipt:
            path = target / row['relpath']
            if row['relpath'] == 'THIRD_PARTY_NOTICES.txt':
                with path.open('rb') as data:
                    row['sha256'] = hashlib.file_digest(data, 'sha256').hexdigest()
                row['bytes'] = path.stat().st_size
            writer.writerow({k: row[k] for k in ('relpath', 'sha256', 'bytes')} |
                {'src_commit': head,
                 'timestamp': '', 'mtime_ns': path.stat().st_mtime_ns})
    if clean_head(repository) != head:
        raise ValueError('HEAD changed during packaging')
    return {'head': head, 'executable_commit': payload.get('mc2.exe', {}).get('src_commit'),
            'files': receipt, 'excluded': excluded_counts, 'mod_overlaps': overlaps}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--credits-source', type=Path, default=CREDITS_SOURCE)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--permissions', type=Path, default=PERMISSIONS)
    parser.add_argument('--mod', action='append')
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--dry-run', action='store_true', help='stage and validate locally; do not create a ZIP')
    args = parser.parse_args()
    permissions = json.loads(args.permissions.read_text())
    archive = args.out.with_suffix('.zip')
    checksum = archive.with_suffix('.zip.sha256')
    if archive.exists() or checksum.exists():
        raise ValueError('archive and checksum paths must be new')
    try:
        result = assemble(args.source, args.out, permissions, args.mod if args.mod is not None else
                          [mod for mod, record in permissions.items() if record.get('bundle') is not False],
                          credits_source=args.credits_source)
        rows = result['files']
        violations = release_asset_guard.scan(args.out)
        if violations:
            raise ValueError('\n'.join(violations))
        result['folder_release_asset_guard'] = 'PASS'
        if clean_head() != result['head']:
            raise ValueError('HEAD changed during packaging')
        if args.dry_run:
            args.receipt.write_text(json.dumps(result, indent=2) + '\n')
            print(json.dumps({'head': result['head'], 'files': len(rows), 'excluded': result['excluded'],
                              'mod_overlaps': len(result['mod_overlaps'])}))
            return
        with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED, compresslevel=1) as zf:
            for path in sorted(args.out.rglob('*')):
                if path.is_file():
                    zf.write(path, path.relative_to(args.out).as_posix())
        violations = release_asset_guard.scan(archive)
        if violations:
            raise ValueError('\n'.join(violations))
        result['zip_release_asset_guard'] = 'PASS'
        with archive.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        checksum.write_text(f'{digest}  {archive.name}\n')
        if clean_head() != result['head']:
            raise ValueError('HEAD changed during packaging')
        args.receipt.write_text(json.dumps(result, indent=2) + '\n')
    except BaseException:
        archive.unlink(missing_ok=True)
        checksum.unlink(missing_ok=True)
        raise
    print(json.dumps({'files': len(rows), 'folder_bytes': sum(p.stat().st_size for p in args.out.rglob('*') if p.is_file()),
                      'zip_bytes': archive.stat().st_size, 'sha256': digest,
                      'mod_overlaps': len(result.get('mod_overlaps', []))}), flush=True)


if __name__ == '__main__':
    main()
