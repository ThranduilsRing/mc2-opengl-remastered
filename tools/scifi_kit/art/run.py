#!/usr/bin/env python3
"""Art-directed kit CLI: render primitive contact sheets and building assemblies.

CPU rules: one Blender job at a time, --threads 4, Cycles threads 4, below-normal
priority. Rendering uses the GPU when
Cycles finds one.

  py -3 tools/scifi_kit/art/run.py prims --pass p1 [--only wall_panel,tank] [--samples 48]
  py -3 tools/scifi_kit/art/run.py sheet --pass p1
  py -3 tools/scifi_kit/art/run.py build --pass b1 [--only barracks]

Archetype pipeline (see archetypes.py; assets live in tools/scifi_kit/art/assets/):
  run.py classify                       inventory -> archetype coverage table
  run.py analyze <asset>                stock measurement -> assets/<asset>.analysis.json
  run.py params <asset> [--force]       auto-draft assets/<asset>.params.json (never overwrites edits)
  run.py preview <asset>                fast Blender-free 5-view + 5-state preview
  run.py validate <asset>[,..] [--pre | --pass TAG]   silhouette/palette/counts/budget/states/sockets
  run.py validate --calibrate           recompute assets/validate_thresholds.json on the 24 approved assets
  run.py batch --assets a,b,c | --category X --next N [--pass arch-proof] [--full] [--retry-failed]   pre-gate, two-tier bake, failures
  run.py batch --commit [--assets a,b]  commit what passes today (refuses failing assets, stamps hashes)
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / 'tools/blender_pipeline'))
from blender_runner import find_blender_exe  # noqa: E402

OUT = Path('E:/mc2-ref/scifi-kit/art')
THUMBS = Path('E:/mc2-archive-2026-10/asset-modernize/docs/assets/sheets/thumbs')
CONDITIONS = ['pristine', 'worn', 'abandoned', 'partial', 'destroyed']


def blender(job, log, timeout=None):
    import psutil
    path = OUT / 'jobs' / (job['tag'] + '.json')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(job, indent=1), encoding='utf-8')
    log.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(find_blender_exe()), '--background', '--factory-startup', '--threads', os.environ.get('MC2_BLENDER_THREADS', '4'), '--python-exit-code', '1',
           '--python', str(HERE / 'blend.py'), '--', str(path)]
    print('[art]', ' '.join(cmd), flush=True)
    sys.path.insert(0,str(ROOT/'scripts'))
    from console_child import child_kwargs
    kwargs=child_kwargs(redirected=True)
    kwargs['creationflags']=kwargs.get('creationflags',0)|0x00004000
    with log.open('w', encoding='utf-8') as fh:
        proc = subprocess.Popen(cmd, stdout=fh, stderr=subprocess.STDOUT, **kwargs)
        try:
            cores = list(range(psutil.cpu_count()))
            psutil.Process(proc.pid).cpu_affinity(cores)
        except Exception as exc:  # affinity is best effort; threads are still 4
            print('[art] affinity not set:', exc)
        try:
            rc = proc.wait(timeout)
        except subprocess.TimeoutExpired:
            proc.kill()                 # our own pid only
            proc.wait()
            raise SystemExit(f'bake timeout after {timeout} s')
    if rc:
        print(log.read_text(encoding='utf-8', errors='replace')[-5000:])
        raise SystemExit(f'Blender failed ({rc}); log {log}')


def font(size):
    from PIL import ImageFont
    return ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf', size)


def stock_crop(stock_id, box):
    from PIL import Image, ImageOps
    src = THUMBS / f'{stock_id}.png'
    alt = Path('G:/mc2-assets/out/scifi-kit/round2/stock/previews') / f'{stock_id}.png'
    if alt.is_file():
        src = alt
    if not src.is_file():
        return None
    im = Image.open(src)
    if im.mode in ('RGBA', 'LA', 'P'):
        im = im.convert('RGBA')
        bg = Image.new('RGBA', im.size, (32, 36, 44, 255))
        bg.alpha_composite(im)
        im = bg
    im = im.convert('RGB')
    return ImageOps.contain(im, box, Image.Resampling.LANCZOS)


def sheet(pass_dir, names, title, out_name, cell=360):
    from PIL import Image, ImageDraw
    sys.path.insert(0, str(HERE))
    import geo
    cols = ['stock'] + CONDITIONS
    head = 70
    rowh = cell + 34
    pages = [names[i:i + 6] for i in range(0, len(names), 6)]
    outs = []
    for pi, page in enumerate(pages):
        im = Image.new('RGB', (cell * len(cols) + 20, head + rowh * len(page)), (24, 28, 32))
        d = ImageDraw.Draw(im)
        d.text((12, 10), title + (f'  ({pi + 1}/{len(pages)})' if len(pages) > 1 else ''), font=font(26), fill=(230, 232, 228))
        for ci, c in enumerate(cols):
            d.text((14 + ci * cell, 44), c.upper(), font=font(17), fill=(170, 176, 170))
        for ri, name in enumerate(page):
            y = head + ri * rowh
            _, palette, stock = geo.PRIMITIVES.get(name, (None, '', name))
            d.text((14, y + 4), f'{name}  /  palette {palette}  /  stock {stock}', font=font(18), fill=(225, 228, 222))
            crop = stock_crop(stock, (cell - 16, cell - 16))
            if crop:
                im.paste(crop, (8 + (cell - crop.width) // 2, y + 30 + (cell - crop.height) // 2))
            for ci, c in enumerate(CONDITIONS, 1):
                p = pass_dir / f'{name}_{c}.png'
                if p.is_file():
                    t = Image.open(p).convert('RGB').resize((cell - 8, cell - 8), Image.Resampling.LANCZOS)
                    im.paste(t, (ci * cell + 4, y + 30))
        path = pass_dir.parent / f'{out_name}-{pi + 1:02d}.png'
        im.save(path)
        outs.append(path)
        print('[art] sheet', path)
    return outs


SOL = Path('G:/mc2-assets/out/scifi-kit/round2/parts')
SOL_NAME = {'quonset_shell': 'barrel_roof'}


def before_after(pass_dir, names, out_name='before-after'):
    """Sol round-2 vs art-directed rework, same primitive per row."""
    from PIL import Image, ImageDraw
    conds = ['pristine', 'abandoned', 'destroyed']
    cell = 260
    head = 96
    pages = [names[i:i + 6] for i in range(0, len(names), 6)]
    for pi, page in enumerate(pages):
        W = 200 + cell * 6 + 40
        im = Image.new('RGB', (W, head + len(page) * (cell + 10)), (24, 28, 32))
        d = ImageDraw.Draw(im)
        d.text((12, 10), f'BEFORE (Sol round 2)  vs  AFTER (art-directed kit, pass {pass_dir.name})  ({pi + 1}/{len(pages)})', font=font(26), fill=(232, 234, 230))
        for i, c in enumerate(conds):
            d.text((200 + i * cell + 8, 56), 'before / ' + c, font=font(16), fill=(200, 150, 140))
            d.text((200 + (i + 3) * cell + 48, 56), 'after / ' + c, font=font(16), fill=(150, 200, 160))
        d.line([(200 + 3 * cell + 20, 50), (200 + 3 * cell + 20, im.height)], fill=(90, 96, 100), width=3)
        for ri, name in enumerate(page):
            y = head + ri * (cell + 10)
            d.text((12, y + cell // 2 - 12), name, font=font(19), fill=(225, 228, 222))
            for i, c in enumerate(conds):
                a = SOL / f'{SOL_NAME.get(name, name)}_{c}.png'
                b = pass_dir / f'{name}_{c}.png'
                for path, x in ((a, 200 + i * cell), (b, 200 + (i + 3) * cell + 40)):
                    if path.is_file():
                        t = Image.open(path).convert('RGB').resize((cell - 6, cell - 6), Image.Resampling.LANCZOS)
                        im.paste(t, (x, y))
        path = pass_dir.parent / f'{out_name}-{pi + 1:02d}.png'
        im.save(path)
        print('[art] before/after', path)


def archetype_cli(a):
    import analyze
    import archetypes
    names = [x.strip().lower() for chunk in ([a.assets] if a.assets else []) + a.positional for x in chunk.split(',') if x.strip()]
    if a.command == 'classify':
        return classify_table()
    if a.command == 'analyze':
        for n in names:
            path, an = analyze.write(n)
            print(path, an['archetype_guess'], {k: v['n'] for k, v in an['counts'].items() if v['n']})
        return
    if a.command == 'params':
        for n in names:
            pp = analyze.ASSETS / f'{n}.params.json'
            if pp.is_file() and not a.force:
                print(pp, 'exists (use --force to re-draft)')
                continue
            ap_ = analyze.ASSETS / f'{n}.analysis.json'
            an = json.loads(ap_.read_text()) if ap_.is_file() else analyze.write(n)[1]
            p = archetypes.draft_params(analyze.inventory_row(n), an)
            pp.write_text(json.dumps(p, indent=1))
            print(pp, p['archetype'], p['counts'])
        return
    if a.command == 'preview':
        import preview
        for n in names:
            print(*preview.sheet(n, OUT / (a.tag or 'arch-proof') / 'preview'))
        return
    if a.command == 'validate':
        import validate
        if a.calibrate:
            res = validate.calibrate(pass_dir=OUT / 'mc2-r11')
            keep = {k: v for k, v in res.items() if k != 'rows'}
            keep['calibrated_on'] = sorted(res['rows'])
            keep['arch_floors'] = json.loads(validate.THRESH.read_text()).get('arch_floors', {})
            validate.THRESH.write_text(json.dumps(keep, indent=1))
            print('[calibrate] thresholds are hash-locked: paste contract.file_sha(THRESH) into contract.THRESH_SHA ->', __import__('contract').file_sha(validate.THRESH))
            (HERE / 'assets' / 'calibration_rows.json').write_text(json.dumps(res['rows'], indent=1))
            print(json.dumps(keep, indent=1))
            return
        bad = 0
        for n in names or list(__import__('drafts').DRAFTS):
            res = validate.validate(n, pass_dir=(OUT / a.tag) if a.tag and not a.pre else None)
            print(validate.report(res))
            bad += not res['ok']
        raise SystemExit(1 if bad else 0)
    if a.command == 'batch':
        import batch
        a.tag = a.tag or 'arch-proof'
        a.samples = getattr(a, 'samples', 48)
        batch.batch(a)


def classify_table():
    import csv
    import analyze
    import archetypes
    import drafts
    rows = list(csv.DictReader(analyze.INVENTORY.open(encoding='utf-8-sig')))
    tab = {}
    for r in rows:
        c = archetypes.classify(r)
        done = r['id'] in drafts.LEGACY
        t = tab.setdefault(c['archetype'], dict(n=0, done=0, names=[]))
        t['n'] += 1
        t['done'] += done
        t['names'].append(r['id'] + ('*' if done else ''))
    print(f'{"archetype":10s} {"assets":>6s} {"done":>5s}  members (* = approved)')
    for arch in list(archetypes.ARCHETYPES) + ['manual']:
        t = tab.get(arch, dict(n=0, done=0, names=[]))
        print(f'{arch:10s} {t["n"]:6d} {t["done"]:5d}  {", ".join(t["names"][:7])}{" ..." if len(t["names"]) > 7 else ""}')
    covered = sum(t['n'] for k, t in tab.items() if k != 'manual')
    print(f'covered {covered}/{len(rows)} ({100 * covered // len(rows)}%), manual {tab.get("manual", dict(n=0))["n"]}, approved {sum(t["done"] for t in tab.values())}')
    (analyze.ASSETS).mkdir(exist_ok=True)
    (analyze.ASSETS / 'coverage.json').write_text(json.dumps({k: dict(n=v['n'], done=v['done'], assets=v['names']) for k, v in tab.items()}, indent=1))


def main():
    import psutil
    parent = psutil.Process()
    # The child inherits this affinity at spawn, before its own explicit pin.
    parent.cpu_affinity(list(range(psutil.cpu_count())))
    parent.nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('command', choices=['prims', 'sheet', 'build', 'beforeafter', 'stockviews', 'classify', 'analyze', 'params', 'preview', 'validate', 'batch'])
    ap.add_argument('positional', nargs='*', help='asset ids for analyze/params/preview/validate')
    ap.add_argument('--pass', dest='tag', default=None)
    ap.add_argument('--assets')
    ap.add_argument('--category')
    ap.add_argument('--next', type=int, default=6)
    ap.add_argument('--calibrate', action='store_true')
    ap.add_argument('--pre', action='store_true', help='validate: pure-Python checks only')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--reanalyze', action='store_true')
    ap.add_argument('--force-bake', action='store_true', help='owner tool (MC2_KIT_EXPERT=1)')
    ap.add_argument('--full', action='store_true', help='batch: full 5-state bake now (default: tier A first, full once the params hash is stable)')
    ap.add_argument('--retry-failed', action='store_true', help='batch: only failed/unfinished assets of the category')
    ap.add_argument('--commit', action='store_true', help='batch: commit the assets that pass today (stamps hashes; refuses failing ones)')
    ap.add_argument('--recheck', action='store_true', help='batch (owner): re-validate committed assets, demote failures to draft')
    ap.add_argument('--no-blender', action='store_true')
    ap.add_argument('--only')
    ap.add_argument('--samples', type=int, default=48)
    ap.add_argument('--conditions', default=','.join(CONDITIONS))
    ap.add_argument('--no-export', action='store_true')
    ap.add_argument('--suffix', help='job/log/report suffix when two workers share one pass')
    a = ap.parse_args()
    sys.path.insert(0, str(HERE))
    if a.command in ('classify', 'analyze', 'params', 'preview', 'validate', 'batch'):
        return archetype_cli(a)
    if not a.tag:
        ap.error('--pass is required for this command')
    import geo
    names = a.only.split(',') if a.only else list(geo.PRIMITIVES)
    pass_dir = OUT / a.tag
    if a.command == 'prims':
        job = {'kind': 'primitives', 'tag': a.tag, 'out': str(pass_dir), 'primitives': names, 'conditions': a.conditions.split(','),
               'samples': a.samples, 'export': not a.no_export}
        blender(job, OUT / 'logs' / f'{a.tag}.log')
        sheet(pass_dir, names, f'Sci-fi kit primitives / pass {a.tag} / stock | pristine | worn | abandoned | destroyed', f'contact-{a.tag}')
    elif a.command == 'sheet':
        sheet(pass_dir, names, f'Sci-fi kit primitives / pass {a.tag} / stock | pristine | worn | abandoned | destroyed', f'contact-{a.tag}')
    elif a.command == 'beforeafter':
        before_after(pass_dir, names)
    elif a.command == 'stockviews':
        # Full stock model from all sides (never a single iso view) for counts/silhouette.
        import drafts
        job = {'kind': 'stock_views', 'tag': a.tag, 'out': 'E:/mc2-ref/scifi-kit/studies/stock-views',
               'assemblies': a.only.split(',') if a.only else list(drafts.DRAFTS)}
        blender(job, OUT / 'logs' / f'{a.tag}.log')
    elif a.command == 'build':
        import assemble
        assemble.run(a, OUT, blender)


if __name__ == '__main__':
    main()
