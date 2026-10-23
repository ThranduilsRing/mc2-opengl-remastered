"""One-command batch: `run.py batch --assets a,b,c` or `--category X --next N`.

Per asset: analyze -> params (auto-draft, never overwrites an agent-edited file) -> register -> pre-bake validate
(pure Python, seconds) -> Blender build + PBR bake + 3 LODs (<= 2 processes, JOBS threads) -> post-bake validate ->
contact sheet + stock-vs-remake five-side sheet -> commit list. Assets that fail the pre-bake validator are NOT
baked; they are listed as FAILURES with the exact messages so the agent can edit params and re-run the same command.
"""
import csv
import json
import os
import re
import sys
import threading
import time
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
OUT = Path('E:/mc2-ref/scifi-kit/art')
STOCK_VIEWS = Path('E:/mc2-ref/scifi-kit/studies/stock-views')
INVENTORY = Path('E:/mc2-ref/scifi-kit/mapping/inventory.csv')
JOBS_FILE = Path('A:/Games/mc2-ops/JOBS')
CONDS = ['pristine', 'worn', 'abandoned', 'partial', 'destroyed']
HINTS = dict(counts='edit params counts.* (and features[]) to what the stock really shows',
             silhouette='edit size/dims (roof, eave_h, rise, batter, bays, legs, style...) so the shape matches the stock side views',
             palette='edit palette.* (linear RGB) toward the stock swatches in the analysis',
             budget='cut detail: fewer dims.bays_*/vents/panels, or raise nothing',
             states='partial/destroyed must differ: needs removable skin + hit zones (dims), or set damage',
             sockets='doors/windows/bays counts must have placements; check features[]',
             bake='re-run the batch for this asset after fixing the message above')


def jobs_n():
    try:
        return int(JOBS_FILE.read_text().split()[0])
    except Exception:
        return 4


def pick(args):
    """Asset ids for the batch: --assets, or --category (inventory category or archetype) --next N."""
    import analyze
    import archetypes
    import drafts
    if args.assets:
        return [a.strip().lower() for a in args.assets.split(',') if a.strip()]
    rows = list(csv.DictReader(INVENTORY.open(encoding='utf-8-sig')))
    out = []
    for r in rows:
        name = r['id']
        if name in drafts.DRAFTS or not (analyze.STOCK / f'{name}.json').is_file():
            continue
        cat = r['category'].split(';')[0]
        arch = archetypes.classify(r)['archetype']
        if args.category in (cat, arch) and arch in archetypes.ARCHETYPES:
            out.append(name)
    return out[:args.next]


def stage_prepare(name, reanalyze=False):
    import analyze
    import archetypes
    t = {}
    t0 = time.time()
    ap = analyze.ASSETS / f'{name}.analysis.json'
    if reanalyze or not ap.is_file():
        analyze.write(name)
        a = json.loads(ap.read_text())
    else:
        a = json.loads(ap.read_text())
    t['analyze'] = round(time.time() - t0, 1)
    row = analyze.inventory_row(name)
    pp = analyze.ASSETS / f'{name}.params.json'
    drafted = not pp.is_file()
    if drafted:
        p = archetypes.draft_params(row, a)
        pp.write_text(json.dumps(p, indent=1))
    p = json.loads(pp.read_text())
    archetypes.register(p)
    return p, drafted, t


def edited_since_draft(name):
    """True when params.json differs from a fresh auto-draft (the agent touched it)."""
    import analyze
    import archetypes
    a = json.loads((analyze.ASSETS / f'{name}.analysis.json').read_text())
    fresh = archetypes.draft_params(analyze.inventory_row(name), a)
    cur = json.loads((analyze.ASSETS / f'{name}.params.json').read_text())
    return json.dumps(fresh, sort_keys=True) != json.dumps(cur, sort_keys=True)


def run_blender(tag, names, suffix, samples, log):
    import run as runmod
    import assemble
    a = SimpleNamespace(tag=tag, only=','.join(names), conditions=','.join(CONDS), samples=samples, no_export=False, suffix=suffix)
    names_ = list(names)
    job = {'kind': 'assembly', 'tag': f'{tag}-{suffix}', 'out': str(OUT / tag), 'assemblies': names_, 'conditions': CONDS, 'samples': samples, 'export': True}
    runmod.blender(job, OUT / 'logs' / f'{tag}-{suffix}.log')
    for n in names_:
        assemble.montage(OUT / tag, n, CONDS)


def run_stockviews(tag, names):
    import run as runmod
    job = {'kind': 'stock_views', 'tag': f'{tag}-stock', 'out': str(STOCK_VIEWS), 'assemblies': names}
    runmod.blender(job, OUT / 'logs' / f'{tag}-stock.log')


def blender_seconds(tag, names):
    """Per-asset Blender seconds from the job logs ('[art] done <name> <cond> <tris> s <sec>')."""
    res = {n: 0.0 for n in names}
    for log in (OUT / 'logs').glob(f'{tag}-*.log'):
        for m in re.finditer(r'\[art\] done (\S+) (\S+) \d+ s ([\d.]+)', log.read_text(errors='replace')):
            if m.group(1) in res:
                res[m.group(1)] += float(m.group(3))
    return {n: round(v, 1) for n, v in res.items()}


def contact_sheet(tag, names, path):
    from PIL import Image, ImageDraw
    cell = 220
    cols = ['stock front', 'stock left'] + CONDS
    im = Image.new('RGB', (150 + cell * len(cols), 40 + cell * len(names)), (24, 28, 32))
    d = ImageDraw.Draw(im)
    for c, label in enumerate(cols):
        d.text((150 + c * cell + 6, 14), label, fill=(190, 196, 190))
    for r, n in enumerate(names):
        d.text((6, 40 + r * cell + 6), n, fill=(232, 234, 230))
        files = [STOCK_VIEWS / f'{n}_stock_front.png', STOCK_VIEWS / f'{n}_stock_left.png'] + [OUT / tag / f'{n}_{c}_beauty.png' for c in CONDS]
        for c, f in enumerate(files):
            if f.is_file():
                from PIL import ImageOps
                t = ImageOps.contain(Image.open(f).convert('RGB'), (cell - 6, cell - 6))
                im.paste(t, (150 + c * cell + 3, 40 + r * cell + 3))
    im.save(path)
    return path


def five_sides(tag, names, path):
    import review_drafts
    return review_drafts.count_sheet(tag, names=names, out=path)


def batch(args):
    import validate
    tag = args.tag
    t_all = time.time()
    names = pick(args)
    if not names:
        print('[batch] nothing to do')
        return {}
    os.environ.setdefault('MC2_BLENDER_THREADS', str(jobs_n()))
    summary = {n: dict(stage={}) for n in names}
    print(f'[batch] {len(names)} assets: {", ".join(names)}', flush=True)
    # 1-2. analyze + params + pre-bake validate (pure Python)
    ready = []
    for n in names:
        s = summary[n]
        try:
            p, drafted, t = stage_prepare(n, args.reanalyze)
            s.update(archetype=p['archetype'], drafted=drafted)
            s['stage'].update(t)
            t0 = time.time()
            res = validate.validate(n)
            s['stage']['validate_pre'] = round(time.time() - t0, 1)
            s['pre'] = res
            ok = res['ok']
        except Exception as exc:
            import traceback
            traceback.print_exc()
            s['pre'] = dict(asset=n, ok=False, failures=[f'build: {type(exc).__name__}: {exc}'], notes=[], metrics={})
            ok = False
        print(validate.report(s['pre']), flush=True)
        if ok or args.force_bake:
            ready.append(n)
    # 3. Blender: stock views (if missing) + build/bake, at most 2 processes
    t0 = time.time()
    if ready and not args.no_blender:
        todo_views = [n for n in names if not (STOCK_VIEWS / f'{n}_stock_front.png').is_file()]
        groups = [ready[i::2] for i in range(2)] if len(ready) > 1 else [ready]
        tasks = ([lambda: run_stockviews(tag, todo_views)] if todo_views else []) + \
                [(lambda g=g, i=i: run_blender(tag, g, 'abcd'[i], args.samples, None)) for i, g in enumerate(groups) if g]
        errors = []
        sem = threading.Semaphore(2)

        def worker(fn):
            with sem:
                try:
                    fn()
                except SystemExit as exc:
                    errors.append(str(exc))
                except Exception as exc:
                    errors.append(f'{type(exc).__name__}: {exc}')
        ths = [threading.Thread(target=worker, args=(fn,)) for fn in tasks]
        for th in ths:
            th.start()
        for th in ths:
            th.join()
        for e in errors:
            print('[batch] blender error:', e, flush=True)
    blender_wall = round(time.time() - t0, 1)
    secs = blender_seconds(tag, ready)
    # 4. post-bake validate + sheets
    passed, failed = [], []
    for n in names:
        s = summary[n]
        s['blender_s'] = secs.get(n)
        res = s['pre']
        if n in ready and not args.no_blender:
            res = validate.validate(n, pass_dir=OUT / tag)
            s['post'] = res
        s['ok'] = res['ok']
        (passed if res['ok'] else failed).append(n)
    sheets = {}
    if not args.no_blender and ready:
        try:
            sheets['contact'] = str(contact_sheet(tag, ready, OUT / tag / f'contact-{tag}.png'))
            sheets['five_sides'] = str(five_sides(tag, ready, OUT / tag / f'five-sides-{tag}.png'))
        except Exception as exc:
            print('[batch] sheet error:', exc)
    # 5. report: failures list + commit list
    lines = []
    for n in failed:
        res = summary[n].get('post') or summary[n]['pre']
        lines.append(f'FAIL {n}')
        for f in res['failures']:
            kind = f.split(':')[0]
            lines.append(f'   {f}')
            lines.append(f'      -> {HINTS.get(kind, "")}')
    commit = [f'git add tools/scifi_kit/art/assets/{n}.params.json tools/scifi_kit/art/assets/{n}.analysis.json' for n in passed]
    zero_edit = [n for n in passed if not edited_since_draft(n)]
    total = round(time.time() - t_all, 1)
    out = dict(tag=tag, assets=names, passed=passed, failed=failed, zero_edit_passed=zero_edit, wall_s=total, blender_wall_s=blender_wall,
               per_asset={n: dict(archetype=summary[n].get('archetype'), prep_s=round(sum(summary[n]['stage'].values()), 1), blender_s=summary[n].get('blender_s'),
                                  ok=summary[n]['ok'], failures=(summary[n].get('post') or summary[n]['pre'])['failures']) for n in names},
               sheets=sheets)
    d = OUT / tag
    d.mkdir(parents=True, exist_ok=True)
    (d / f'batch-{tag}.json').write_text(json.dumps(out, indent=1))
    (d / f'failures-{tag}.txt').write_text('\n'.join(lines) + '\n')
    (d / f'commit-list-{tag}.txt').write_text('\n'.join(commit) + '\n')
    print('\n[batch] RESULT', json.dumps({k: out[k] for k in ('passed', 'failed', 'zero_edit_passed', 'wall_s')}))
    if lines:
        print('[batch] FAILURES (edit params only, re-run the same command):\n' + '\n'.join(lines))
    if commit:
        print('[batch] COMMIT LIST:\n' + '\n'.join(commit))
    return out
