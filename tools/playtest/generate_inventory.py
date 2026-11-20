"""Read-only playtest inventory checks; trigger: tools/playtest/, package script."""
import argparse
import ast
from collections import Counter, defaultdict
import csv
import hashlib
import json
import os
from pathlib import Path
import sys
import subprocess
import re
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import make_playtest_package as package
sys.path.insert(0, str(package.REPOSITORY))
from tools.mc2data import stock_missions, stock_corpus

GOLDEN = Path(os.environ.get('MC2_PLAYTEST_GOLDEN', 'A:/Games/mc2-opengl/mc2-win64-v0.4c'))
EXTRACT = Path(os.environ['MC2_PLAYTEST_EXTRACT']) if os.environ.get('MC2_PLAYTEST_EXTRACT') else None
_ENGINE_TEXT = []


def engine_mentions(needle):
    """Literal search of mclib/ RenderCore/ code/ (no external rg dependency)."""
    if not _ENGINE_TEXT:
        for sub in ('mclib', 'RenderCore', 'code'):
            _ENGINE_TEXT.extend(p.read_bytes() for p in (package.REPOSITORY / sub).rglob('*') if p.is_file())
    needle = needle.encode()
    return any(needle in blob for blob in _ENGINE_TEXT)


TREE_SLUGS = ('island_tree_01', 'island_tree_02', 'island_tree_03', 'jacaranda_tree', 'tree_small_02')


def tier_rules():
    return json.loads((package.PERMISSIONS.parent / 'tier_rules.json').read_text())


def matching_rule(rel, rules):
    if package.hard_denied(rel):
        raise ValueError('hard-denied path: ' + rel)
    hits = [r for r in rules if re.fullmatch(r['regex'], rel)]
    if len(hits) > 1:
        raise ValueError('overlapping tier rules: ' + rel)
    return hits[0] if hits else None


def check_rule(rel, row, rule, source, executable, intentional, files):
    path = package.safe_path(source, rel)
    check = rule['extra_check']
    if check == 'intentional-asset-at-executable':
        if intentional.get(rel) != row['sha256']:
            raise ValueError('tree intentional hash mismatch: ' + rel)
    elif check == 'commit-blob':
        blob = subprocess.check_output(['git', '-C', str(package.REPOSITORY), 'show', executable + ':' + rel],
            **package.console_child.child_kwargs(redirected=True))
        if hashlib.sha256(blob).hexdigest() != row['sha256']:
            raise ValueError('commit blob mismatch: ' + rel)
    elif check.startswith('canonical-and-'):
        if package.sha256(package.safe_path(package.CREDITS_SOURCE, rel)) != row['sha256']:
            raise ValueError('tier D differs from v0.4: ' + rel)
        if check == 'canonical-and-shipped-txm':
            if str(Path(rel).with_suffix('.txm')).replace('\\', '/') not in files:
                raise ValueError('missing shipped TXM sibling: ' + rel)
        else:
            entries = [v.decode('ascii') for v in path.read_bytes().split(b'\0') if v]
            for entry in entries:
                normalized = entry.replace('\\', '/')
                if ':' in normalized or normalized.startswith('/') or '..' in normalized.split('/'):
                    raise ValueError('machine path in list: ' + rel)
                reference = normalized if normalized.startswith('data/') else 'data/textures/' + normalized
                if reference not in files:
                    raise ValueError('unshipped list reference: ' + entry)
            row['reference_count'] = len(entries)
    elif check == 'no-engine-reader':
        if engine_mentions('cook.json'):
            raise ValueError('engine cook.json reader check failed')


def apply_rules(pending, files, source, executable, intentional, excluded, class_counts):
    for rel, row, rule in pending:
        check_rule(rel, row, rule, source, executable, intentional, files)
        if rule['action'] == 'exclude':
            excluded[rule['reason']] += 1
            continue
        row.update(tier=rule['tier'], rule=rule['id'], origin=rule['origin'],
                   licence=rule['licence'], evidence=rule['evidence'])
        if rule.get('class'):
            row['parity_class'] = rule['class']
            class_counts[rule['class']] += 1
        files[rel] = row


def verify_tgl(path, extract, *, manifest=None, pairs=None):
    package.safe_path(path.parent, path.name)
    data, members = stock_missions.fst_members(path)
    names = [row[0] for row in members]
    if len(set(names)) != len(names) or any(not n.startswith('data/tgl/') for n in names):
        raise ValueError('tgl.fst duplicate or outside-prefix member')
    for name in names:
        if package.hard_denied(name) or '..' in Path(name).parts:
            raise ValueError('tgl.fst forbidden member: ' + name)
    digest_file = stock_corpus.MANIFEST_PATH
    expected = manifest if manifest is not None else stock_corpus.load_corpus_manifest(stock_corpus.corpus('tgl'))
    actual = stock_missions.fst_digests(path, 'data/tgl/')
    for name, digest in expected.items():
        if actual.get(name) != digest:
            raise ValueError('tgl.fst stock member missing/altered: ' + name)
    index = stock_corpus.source_index(extract)
    extras = set(actual) - set(expected)
    for name in sorted(extras):
        counterpart = index.get(name)
        if counterpart is None:
            raise ValueError('tgl.fst extra lacks extract counterpart: ' + name)
        package.safe_path(extract, counterpart.relative_to(extract).as_posix())
        with counterpart.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'md5').hexdigest()
        if digest != actual[name]:
            raise ValueError('tgl.fst extra differs from extract: ' + name)
    if pairs is None:
        header = (package.REPOSITORY / 'RenderCore/StockPbr.h').read_text()
        pairs = {(int(size), int(crc, 16)) for size, crc in re.findall(
            r'bytes == (\d+)u && crc == (0x[0-9a-f]+)u', header)}
    crc = zlib.crc32(data)
    if (len(data), crc) not in pairs:
        raise ValueError('tgl.fst bytes/CRC pair absent from StockPbr.h')
    return dict(tier='S', sha256=hashlib.sha256(data).hexdigest(), bytes=len(data),
        crc32=f'0x{crc:08x}', members=len(names), digest_matched=len(expected),
        extract_matched=len(extras), stock_data_md5_sha256=package.sha256(digest_file))


def tree_asset(rel):
    return bool(re.fullmatch(r'data/model_overrides/(broadleaf-[^/]+|kit-broadleaf|white-poplar)/.+', rel)
        or re.fullmatch(r'data/tgl/(32|64|128|256|512|1024)/(a_)?cp_'
            r'(broadleaf-[^/]+|kit-broadleaf|white-poplar)_\d+\.tga', rel))


def parity_class(rel, terrain):
    if re.fullmatch(r'data/defs/(.*/)?[^/]+\.fit', rel):
        return 'D1', 'project-authored UI definitions; owner brief 2026-11-16'
    if re.fullmatch(r'data/missions/[^/]+\.beauty/.+', rel):
        return 'D2', 'terrain sidecars derived from stock by project tools; owner brief 2026-11-16'
    if rel.startswith('data/textures/256Overlays/'):
        return 'D3', 'docs/asset-pipeline.md:150; tools/overlay_upscale'
    if rel in terrain:
        return 'D4', terrain[rel]
    if re.fullmatch(r'data/textures/[^/]+\.txm', rel):
        return 'D5', 'derived runtime texture cache; owner brief 2026-11-16'
    return None, None


def generate_inventory(source, golden, extract):
    rules = tier_rules()
    with (source / '.deployed_manifest.csv').open(newline='', encoding='utf-8') as stream:
        reader = csv.reader(stream)
        if next(reader)[:2] != ['manifest_version', 'v1']:
            raise ValueError('unsupported deploy manifest')
        payload = {row['relpath']: row for row in csv.DictReader(stream)}
    executable = payload.get('mc2.exe', {}).get('src_commit')
    if not executable or subprocess.run(['git', '-C', str(package.REPOSITORY), 'merge-base',
            '--is-ancestor', executable, 'claude/play-1113'],
            **package.console_child.child_kwargs(redirected=True)).returncode:
        raise ValueError('executable commit is not an ancestor of claude/play-1113')
    permissions = json.loads(package.PERMISSIONS.read_text())
    intentional = json.loads(git('show', executable + ':intentional-asset-changes.json'))
    pending = []
    mods = {mod for mod, record in permissions.items() if record.get('bundle') is not False
            and record.get('kind') != 'loose'}
    terrain = {}
    for rel in json.loads((package.REPOSITORY / 'scripts/terrain_materials.json').read_text()):
        stem = rel.removesuffix('.tga')
        for ext in ('.tga', '.ktx2', '.tga.cookver', '.ktx2.cookver'):
            terrain[stem + ext] = 'scripts/terrain_materials.json: ' + rel
    for stem in ('asphalt_albedo', 'gravel_albedo', 'concrete_detail_road', 'concrete_detail_pad'):
        for ext in ('.tga', '.ktx2', '.tga.cookver', '.ktx2.cookver'):
            terrain['data/textures/' + stem + ext] = 'docs/texture-tiers-overlay-pbr-design.md:27-28'
    files, excluded, unmatched, class_counts = {}, Counter(), defaultdict(list), Counter()
    golden_hashes = {}
    # FST rules take precedence over deploy rows and all loose-file tiers.
    for name in ('art', 'camera', 'effect', 'insignia', 'misc', 'mission', 'textures'):
        rel = name + '.fst'
        path = package.safe_path(source, rel)
        digest = package.sha256(path)
        reference = package.safe_path(golden, rel)
        golden_hashes[rel] = package.sha256(reference)
        if digest != golden_hashes[rel]:
            raise ValueError('non-tgl FST differs from golden: ' + rel)
        files[rel] = dict(tier='B', sha256=digest, bytes=path.stat().st_size)
    tgl = verify_tgl(source / 'tgl.fst', extract)
    # The assignment's expected values are stop checks, never generated pins.
    if (tgl['members'], tgl['digest_matched'], tgl['extract_matched'], tgl['bytes'],
            tgl['crc32'], tgl['sha256']) != (4667, 4212, 455, 34295854, '0xaef6a9dd',
            '18e0f57996b212aac3fa8ddd36c5d73aa7e6fd14099e04934ae704bd6f0b9d14'):
        raise ValueError('tgl.fst differs from assignment expected values')
    files['tgl.fst'] = tgl
    for path in sorted(source.rglob('*')):
        if not path.is_file():
            continue
        rel = path.relative_to(source).as_posix()
        if package.hard_denied(rel):
            excluded['hard-denied'] += 1
            continue
        if package.excluded(rel):
            excluded['development/cache/personal'] += 1
            continue
        if package.bt2018_extractor_only(rel):
            excluded[package.BT2018_REASON] += 1
            continue
        if rel in files:
            continue
        bundled_mod = rel.startswith('mods/') and len(Path(rel).parts) > 2 and Path(rel).parts[1] in mods
        candidate = rel in payload or rel in package.STOCK_ROOT or rel.startswith(('data/', 'assets/')) or bundled_mod
        if not candidate:
            excluded['unbundled-mod' if rel.startswith('mods/') else 'outside-payload'] += 1
            continue
        path = package.safe_path(source, rel)
        digest = package.sha256(path)
        row = dict(sha256=digest, bytes=path.stat().st_size)
        if rel in payload:
            if digest != payload[rel]['sha256']:
                raise ValueError('deploy hash mismatch: ' + rel)
            row['tier'] = 'A'
        else:
            reference = package.safe_path(golden, rel)
            if reference.is_file():
                golden_hashes[rel] = package.sha256(reference)
            if digest == golden_hashes.get(rel):
                row['tier'] = 'B'
            elif bundled_mod or tree_asset(rel):
                # The canonical source is also the parity reference; compare the
                # same relpath explicitly so an alternate source cannot qualify.
                if digest != package.sha256(package.safe_path(package.CREDITS_SOURCE, rel)):
                    raise ValueError('tier C differs from v0.4: ' + rel)
                row['tier'] = 'C'
            else:
                cls, evidence = parity_class(rel, terrain)
                if cls:
                    if digest != package.sha256(package.safe_path(package.CREDITS_SOURCE, rel)):
                        if cls == 'D5':
                            excluded['runtime-txm-cache'] += 1
                            continue
                        raise ValueError('tier D differs from v0.4: ' + rel)
                    row.update(tier='D', parity_class=cls, evidence=evidence)
                    class_counts[cls] += 1
                else:
                    rule = matching_rule(rel, rules)
                    if rule:
                        pending.append((rel, row, rule))
                        continue
                    generated = (re.fullmatch(r'data/tgl/\d+/cook\.json', rel)
                        or rel == 'data/model_overrides/models.json'
                        or re.fullmatch(r'data/textures/[^/]+\.lst', rel))
                    canonical = package.safe_path(package.CREDITS_SOURCE, rel)
                    if generated and (not canonical.is_file() or digest != package.sha256(canonical)):
                        fallback = any(rel.lower() in {entry[0] for entry in stock_missions.fst_members(source / name)[1]}
                            for name in files if '/' not in name and name.endswith('.fst'))
                        if engine_mentions(Path(rel).name) and not fallback:
                            raise ValueError('engine reads generated metadata with no FST fallback: ' + rel)
                        excluded['generated-metadata'] += 1
                        continue
                    cls = str(Path(rel).parent)
                    unmatched[cls].append(dict(path=rel, bytes=row['bytes']))
                    continue
        files[rel] = row
    apply_rules(pending, files, source, executable, intentional, excluded, class_counts)
    attribution_only, tree_provenance = [], []
    for rel, row in files.items():
        slug = next((s for s in TREE_SLUGS if s in rel), None)
        if slug:
            tree_provenance.append(dict(path=rel, sha256=row['sha256'], asset=slug,
                URL='https://polyhaven.com/a/' + slug, licence='CC0 1.0', evidence=rules[0]['evidence']))
            if not re.fullmatch(rules[0]['regex'], rel):
                attribution_only.append(rel)
    summary = [{'class': cls, 'count': len(rows), 'bytes': sum(r['bytes'] for r in rows),
        'examples': [r['path'] for r in rows[:3]]} for cls, rows in sorted(unmatched.items())]
    classes = []
    for cls, glob, origin, licence, evidence in (
        ('D1', 'data/defs/**/*.fit', 'project UI definitions', 'project-authored', 'Owner brief 2026-11-16'),
        ('D2', 'data/missions/*.beauty/**', 'stock terrain sidecars', 'derived stock MC2', 'project terrain tools'),
        ('D3', 'data/textures/256Overlays/**', 'upscaled stock overlays', 'derived stock MC2', 'docs/asset-pipeline.md:150'),
        ('D4', 'explicit evidence rows', 'terrain materials', 'project/CC0 detail; stock albedo', 'per-file evidence'),
        ('D5', 'data/textures/*.txm', 'derived texture caches', 'derived stock MC2', 'Owner brief 2026-11-16')):
        classes.append(dict(id=cls, glob=glob, origin=origin, licence=licence, evidence=evidence,
            count=class_counts[cls], files=[dict(path=rel, evidence=row['evidence']) for rel, row in files.items()
                if row.get('parity_class') == cls]))
    result = dict(status='STOP' if summary else 'PASS', executable_commit=executable,
        source_manifest_sha256=package.sha256(source / '.deployed_manifest.csv'),
        files=files, counts=dict(Counter(row['tier'] for row in files.values())),
        parity_counts=dict(class_counts), excluded=dict(excluded),
        unmatched=sum(row['count'] for row in summary), tgl=tgl,
        attribution_only=attribution_only, tree_provenance=tree_provenance)
    for rule in rules:
        if rule.get('class'):
            cls = rule['class']
            classes.append(dict(id=cls, glob=rule['regex'], origin=rule['origin'],
                licence=rule['licence'], evidence=rule['evidence'], action=rule['action'],
                count=class_counts[cls], files=[dict(path=rel, evidence=row['evidence'])
                    for rel, row in files.items() if row.get('parity_class') == cls]))
    root = package.PERMISSIONS.parent
    sources = json.loads((root / 'kit_cc0_sources.json').read_text())
    bake_sources = [r for r in sources if r['asset'] not in TREE_SLUGS]
    if len(bake_sources) != 23:
        raise ValueError('expected 23 bake sources')
    trees = [dict(asset=s, site='Poly Haven', URL='https://polyhaven.com/a/' + s,
        licence='CC0 1.0', evidence=[rules[0]['evidence']]) for s in TREE_SLUGS]
    (root / 'kit_cc0_sources.json').write_text(json.dumps(bake_sources + trees, indent=2) + '\n', encoding='utf-8')
    permissions['polyhaven-cc0-trees'] = dict(bundle=True, kind='loose',
        permission_source='Owner confirmation 2026-11-16 ~22:00', terms='CC0 1.0',
        author='Poly Haven contributors; MC2 remaster asset contributors', credits_file='CREDITS.txt',
        credits_sha256=package.sha256(package.safe_path(package.CREDITS_SOURCE, 'CREDITS.txt')),
        provenance='tools/playtest/kit_cc0_sources.json#Poly-Haven-trees', sources=trees)
    package.PERMISSIONS.write_text(json.dumps(permissions, indent=2) + '\n', encoding='utf-8')
    out = package.GENERATED
    out.mkdir(parents=True, exist_ok=True)
    package.INVENTORY.write_text(json.dumps(result, indent=2) + '\n')
    (out / 'parity_classes.json').write_text(json.dumps(classes, indent=2) + '\n')
    (out / 'unmatched_by_class.json').write_text(json.dumps(summary, indent=2) + '\n')
    return result


def git(*args):
    return subprocess.check_output(['git', '-C', str(package.REPOSITORY), *args],
        text=True, **package.console_child.child_kwargs(redirected=True)).strip()


def kit_sources():
    ph = ('concrete_panels worn_corrugated_iron weathered_planks rough_wood hessian_230 '
          'gravel_road rubberized_track metal_plate red_brick_03 japanese_stone_wall '
          'granite_wall marble_01 clay_roof_tiles_02 roof_slates_03 precast_concrete_wall '
          'concrete_block_wall concrete_slab_wall').split()
    acg = 'CorrugatedSteel009 PaintedMetal002 PaintedMetal003 Metal052C Metal061B'.split()
    assets = ph + acg + ['uploads_files_4996646_Broadleaf_GLTF']
    commits = set(git('log', '--format=%H', '--ancestry-path',
        '9711e7083^..9f71cda79', '--', 'tools/scifi_kit', 'data/kit').splitlines())
    for name in ('industrial-kit.json', 'vehicle-kit.json'):
        manifest = json.loads((package.REPOSITORY / 'data/kit' / name).read_text())
        commits.update(v for k, v in manifest.items() if k.endswith('source_bake_commit'))
    used = {asset: [] for asset in assets}
    for commit in sorted(commits):
        text = git('show', commit + ':tools/scifi_kit/art/material_sources.py')
        for call in ast.walk(ast.parse(text)):
            if not isinstance(call, ast.Call) or not isinstance(call.func, ast.Name):
                continue
            index = {'ph': 1, 'acg': 1, 'phc': 0}.get(call.func.id)
            if index is None:
                continue
            asset = ast.literal_eval(call.args[index])
            if asset not in used:
                raise ValueError(f'unapproved bake source: {commit}: {asset}')
            if commit not in used[asset]:
                used[asset].append(commit)
    rows = []
    for asset in assets:
        site = 'Poly Haven' if asset in ph else 'ambientCG' if asset in acg else 'CGTrader'
        url = ('https://polyhaven.com/a/' + asset if asset in ph else
               'https://ambientcg.com/view?id=' + asset if asset in acg else
               'https://www.cgtrader.com/')
        rows.append(dict(asset=asset, site=site, URL=url,
            licence='CC0 1.0' if site != 'CGTrader' else 'CC0 1.0 per owner ruling 2026-11-16',
            evidence=['gameasset-inventory-2026-10-21.md (E archive plans)',
                'provenance.json (G drive mc2-assets/pbr/polyhaven; nine civil sets)']
                if site == 'Poly Haven' else ['Owner ruling 2026-11-16'] if site == 'CGTrader'
                else ['gameasset-inventory-2026-10-21.md (E archive plans)'],
            bake_commits=used[asset]))
    (package.PERMISSIONS.parent / 'kit_cc0_sources.json').write_text(json.dumps(rows, indent=2) + '\n')
    permissions = json.loads(package.PERMISSIONS.read_text())
    for mod in ('mc2-industrial-kit', 'mc2-vehicle-kit'):
        permissions[mod] = dict(bundle=True,
            permission_source='Owner playtester-build-1 brief, 2026-11-15; owner CC0 source ruling 2026-10-21',
            terms='CC0 1.0 sources; original kit geometry/shaders; attribution given',
            author='MC2 remaster asset contributors; Poly Haven; ambientCG',
            credits_file='CREDITS.txt', provenance='tools/playtest/kit_cc0_sources.json')
    permissions['owner-cc0-trees'] = dict(bundle=True, kind='loose',
        permission_source='Owner ruling 2026-11-16 (CGTrader Broadleaf pack is CC0)',
        terms='CC0 1.0', author='CGTrader Broadleaf pack contributors; MC2 remaster asset contributors',
        credits_file='CREDITS.txt', provenance='tools/playtest/kit_cc0_sources.json#uploads_files_4996646_Broadleaf_GLTF')
    package.PERMISSIONS.write_text(json.dumps(permissions, indent=2) + '\n')
    return {'sources': len(rows), 'commits': len(commits), 'status': 'PASS'}


def audit_packets(source):
    allowed, denied, excluded = [], [], []
    for path in sorted(source.rglob('*')):
        if not path.is_file() or path.suffix.lower() != '.pak':
            continue
        rel = path.relative_to(source).as_posix()
        # Denied names and non-shipping content are counted without opening.
        if package.hard_denied(rel) or package.excluded(rel):
            excluded.append(rel)
            continue
        try:
            package.safe_path(source, rel)
        except ValueError:
            denied.append(rel)
        else:
            allowed.append(rel)
    return {'status': 'STOP' if denied else 'PASS', 'allowed': len(allowed),
            'denied': len(denied), 'allowed_paths': allowed,
            'denied_paths': denied, 'excluded_without_reading': excluded}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--pin-credits', action='store_true')
    parser.add_argument('--kit-sources', action='store_true')
    parser.add_argument('--inventory', action='store_true')
    parser.add_argument('--golden', type=Path, default=GOLDEN)
    parser.add_argument('--extract', type=Path, default=EXTRACT)
    parser.add_argument('--credits-source', type=Path, default=package.CREDITS_SOURCE)
    parser.add_argument('--permissions', type=Path, default=package.PERMISSIONS)
    args = parser.parse_args()
    if args.inventory:
        if args.extract is None:
            parser.error('--extract (or MC2_PLAYTEST_EXTRACT) is required with --inventory')
        result = generate_inventory(args.source, args.golden, args.extract)
        receipt = {k: v for k, v in result.items() if k != 'files'}
        args.receipt.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
        print(json.dumps(receipt, indent=2))
        return int(result['status'] != 'PASS')
    if args.kit_sources:
        result = kit_sources()
        args.receipt.write_text(json.dumps(result) + '\n')
        print(f"R4 sources={result['sources']} commits={result['commits']}")
        return 0
    if args.pin_credits:
        permissions = json.loads(args.permissions.read_text())
        count = 0
        for mod, record in permissions.items():
            if record.get('bundle') is False:
                continue
            for rel, pin, destination in package.credit_files(mod, record):
                path = package.safe_path(args.credits_source, rel)
                if path.suffix.lower() not in {'.txt', '.md'} or not path.is_file():
                    raise ValueError(f'missing credits text: {mod}: {rel}')
                digest = package.sha256(path)
                if record.get(pin) and record[pin] != digest:
                    raise ValueError(f'credits hash mismatch: {mod}: {rel}')
                record[pin] = digest
                count += 1
        args.permissions.write_text(json.dumps(permissions, indent=2) + '\n')
        args.receipt.write_text(json.dumps({'credits': count, 'status': 'PASS'}) + '\n')
        print(f'R3 credits={count} sha-pinned')
        return 0
    result = audit_packets(args.source)
    args.receipt.write_text(json.dumps(result, indent=2) + '\n')
    print(f"R2 pak allowed={result['allowed']} denied={result['denied']}")
    for rel in result['denied_paths']:
        print('STOP: ' + rel)
    return int(result['status'] != 'PASS')


if __name__ == '__main__':
    sys.exit(main())
