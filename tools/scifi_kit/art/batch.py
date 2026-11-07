"""One-command batch: `run.py batch --assets a,b,c` or `--category X --next N`; `run.py batch --commit`.

Per asset: analyze -> params (auto-draft, never overwrites an edited file) -> register -> PRE-GATE (contract.py + the Python model checks,
seconds, no Blender) -> numpy stock views -> Blender in two tiers -> post-bake validate + content.py IR -> sheets.

* status file (arch-proof/status.json) drives pick(): failed / unfinished / tier-A assets are re-selected by `--category X --next N`
  before any fresh one; an asset is never silently dropped. `--retry-failed` selects only those (and `stuck` ones), any category.
* `needs_builder`: the stock shape needs a builder capability that does not exist (structural failure: footprint, roof, features, or a
  silhouette/fit/size/round failure that 3 fix rounds did not clear). `stuck`: 3 rounds of other failures. Neither eats batch slots.
* `--recheck` (owner): re-validates committed assets against today's gates and demotes failures to status.state=draft.
* two-tier bake: TIER A (pristine + destroyed beauty, no export, 16 samples) while iterating; TIER B (all 5 states + 5-map bake + 3 LODs)
  only once the params hash is unchanged since a passing tier A (or with --full). A params-hash cache skips an unchanged asset entirely.
* `--commit` refuses any asset that does not pass today, stamps params.status.validated with the params and analysis hashes, commits by pathspec.
"""
import csv
import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def _child_kwargs():
    """No console window for the git / pytest children (scripts/console_child.py)."""
    sys.path.insert(0, str(ROOT / 'scripts'))
    from console_child import child_kwargs
    return child_kwargs(redirected=True)
OUT = Path('E:/mc2-ref/scifi-kit/art')
STOCK_VIEWS = Path('E:/mc2-ref/scifi-kit/studies/stock-views')
INVENTORY = Path(os.environ.get('MC2_REF_ROOT', 'E:/mc2-ref')) / 'scifi-kit/mapping/inventory.csv'
JOBS_FILE = Path('A:/Games/mc2-ops/JOBS')
PIPELINE = Path('A:/Games/mc2-flash4')
CONDS = ['pristine', 'worn', 'abandoned', 'partial', 'destroyed']
TIER_A_CONDS = ['pristine', 'destroyed']
BAKE_TIMEOUT_S = 360            # per asset; the batch kills its own Blender pid and the asset fails with "bake timeout"
COMMIT_MAX = 8                  # assets per commit (3 files each: params, analysis, shape; stays under the 25-file lane rule)
MAX_ROUNDS = 3                  # failing batch runs before an asset is parked as stuck / needs_builder
STRUCTURAL = ('archetype', 'size', 'fit', 'silhouette', 'round')    # params may move these; 3 failing rounds -> needs_builder
BUILDER_GAP = ('needs_builder', 'footprint', 'roof', 'shape')        # the shape-built gross form: params cannot move it -> needs_builder now
FORMULA = ('pipe', 'bridge', 'vehicle', 'junk', 'quonset')     # tagged fields come from the builder, not from stock measurement
HINTS = dict(counts='edit counts/features to the stock evidence (side views); a deliberate difference needs a waiver {check:counts, field, reason}',
             silhouette='edit dims (roof, eave_h, rise, peaks, batter, bays, legs, style...) so the shape matches the stock side views',
             palette='edit palette.<slot> (linear RGB) toward analysis palette.dominant; an exception needs a waiver {check:palette, slot, reason}',
             budget='cut detail (fewer bays/vents/panels); params.budget may only lower the budget',
             states='partial/destroyed must differ: removable skin + hit zones (dims), or damage',
             sockets='doors/windows/bays counts must have placements; check features[]',
             render='fix palette slots (lift near-black palette entries); winding is auto-recomputed in Blender',
             features='add/move/remove features[] so each side matches the painted openings of the stock texture; a stock painting the detector misread needs a waiver {check:features, side, kind, reason}',
             fit='the authored proportions are wrong (fit stretches 0.9-1.1 at most): fix dims/size-derived dims (eave_h, rise, roof) rather than the fit',
             round='round parts need enough ring vertices per LOD; use the builder round settings (tank sides, N) or waive {check:round, part, reason}',
             archetype='the archetype is fixed by classify; only a stock-evidence waiver changes it',
             schema='remove the unknown/missing key; see archetypes.DIM_KEYS / PAL_KEYS',
             evidence='do not edit params.evidence; record deviations as waivers',
             waiver='complete the waiver: {check, <keys>, reason (>= 15 chars, names the stock evidence)}',
             integrity='analysis.json / validate_thresholds.json are evidence: never edit them (re-run analyze only through the owner)',
             size='params.size must equal the stock bounds (2%)',
             content='the baked glb fails the content-pipeline IR validation: re-bake, or report to the owner',
             stock='re-run `run.py analyze <asset>`',
             bake='re-run the batch for this asset after fixing the message above',
             needs_builder='the builder lacks this capability: the asset is parked (needs_builder) for builder work; do not hand-build or waive',
             footprint='structural: the plan outline comes from shape.json; not a params fix (needs_builder)',
             roof='structural: the roof form comes from shape.json; not a params fix (needs_builder)',
             shape='structural: rooftop/side features come from shape.json; not a params fix (needs_builder)',
             pytest='the kit pytest is red for this asset: fix it before committing')


def states_of(args):
    """--states pristine -> ['pristine'] (owner 2026-10-23: pristine-only until the shapes are signed off); unset -> None (all five)."""
    s = getattr(args, 'states', None)
    if not s or s == 'all':
        return None
    if s != 'pristine':
        raise SystemExit(f'--states: only "pristine" or "all" are supported (got {s!r})')
    return ['pristine']


def tier_key(tier, states):
    """Stamp tier name; pristine-only stamps never satisfy a full-state gate."""
    return tier + 'p' if states else tier


def jobs_n():
    try:
        return int(JOBS_FILE.read_text().split()[0])
    except Exception:
        return 4


# ------------------------------------------------------------------ status file (drives pick) and stamps (hash cache)
STATUS = OUT / 'arch-proof' / 'status.json'


def load_status():
    try:
        return json.loads(STATUS.read_text())
    except Exception:
        return {}


def save_status(st):
    STATUS.parent.mkdir(parents=True, exist_ok=True)
    STATUS.write_text(json.dumps(st, indent=1, sort_keys=True))


def set_status(names, state, **extra):
    st = load_status()
    for n in names:
        st[n] = dict(st.get(n, {}), state=state, at=time.strftime('%Y-%m-%d %H:%M:%S'), **extra)
    save_status(st)


def stamp_file(tag, name, tier):
    return OUT / tag / '.stamp' / f'{name}.{tier}.json'


def stamp_ok(tag, name, tier, psha):
    import contract
    try:
        s = json.loads(stamp_file(tag, name, tier).read_text())
    except Exception:
        return False
    return s.get('params_sha') == psha and s.get('code_sha') == contract.code_sha()


def stamp_write(tag, name, tier, psha):
    import contract
    f = stamp_file(tag, name, tier)
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text(json.dumps(dict(params_sha=psha, code_sha=contract.code_sha(), at=time.strftime('%Y-%m-%d %H:%M:%S'))))


def committed(name):
    import analyze
    pp = analyze.ASSETS / f'{name}.params.json'
    if not pp.is_file():
        return False
    s = json.loads(pp.read_text()).get('status', {})
    return s.get('state') in ('runner', 'owner_approved') and bool(s.get('validated'))


def pick(args):
    """Asset ids: --assets (always honoured), else the category's assets that still need work: failed / unfinished / tier-A / drafted-but-unreviewed
    first (so a re-run of the same command re-selects them), then fresh ones. Passed and committed assets are skipped."""
    import analyze
    import archetypes
    import drafts
    if args.assets:
        return [a.strip().lower() for a in args.assets.split(',') if a.strip()]
    st = load_status()
    rows = list(csv.DictReader(INVENTORY.open(encoding='utf-8-sig')))
    retry = getattr(args, 'retry_failed', False)
    redo, unreviewed, fresh = [], [], []
    for r in rows:
        name = r['id']
        if name in drafts.LEGACY or not (analyze.STOCK / f'{name}.json').is_file():
            continue
        cat = r['category'].split(';')[0]
        arch = archetypes.classify(r)['archetype']
        if arch not in archetypes.ARCHETYPES or (args.category and args.category not in (cat, arch)):
            continue
        state = st.get(name, {}).get('state')
        if committed(name) or state in ('passed', 'committed', 'needs_builder'):
            continue
        if state == 'stuck' and not retry:
            continue                    # parked: only --retry-failed re-opens it
        has_params = (analyze.ASSETS / f'{name}.params.json').is_file()
        if state in ('failed', 'unfinished', 'tierA_ok', 'stuck'):
            redo.append(name)
        else:
            (unreviewed if has_params else fresh).append(name)
    if retry:
        return redo[:args.next]
    if not args.category:
        print('[batch] pass --category <building|prop|...>, --assets a,b or --retry-failed')
        return []
    return (redo + unreviewed + fresh)[:args.next]


def park(name, res):
    """Failing asset -> failed / stuck / needs_builder, by failure kind and the number of failing rounds."""
    st = load_status().get(name, {})
    rounds = int(st.get('rounds', 0)) + 1
    kinds = {f.split(':')[0] for f in res['failures']}
    gap = [f for f in res['failures'] if f.split(':')[0] in BUILDER_GAP]
    if gap:
        state, why = 'needs_builder', gap[0]
    elif rounds >= MAX_ROUNDS:
        state = 'needs_builder' if kinds & set(STRUCTURAL) else 'stuck'
        why = next((f for f in res['failures'] if f.split(':')[0] in STRUCTURAL), res['failures'][0])
    else:
        state, why = 'failed', res['failures'][0]
    set_status([name], state, failures=len(res['failures']), rounds=rounds, why=why[:300])
    return state


# ------------------------------------------------------------------ prepare
class Manual(Exception):
    pass


def stage_prepare(name, reanalyze=False):
    import analyze
    import archetypes
    import validate
    t = {}
    t0 = time.time()
    ap = analyze.ASSETS / f'{name}.analysis.json'
    if reanalyze or not ap.is_file():
        analyze.write(name)
    a = json.loads(ap.read_text())
    t['analyze'] = round(time.time() - t0, 1)
    row = analyze.inventory_row(name)
    cls = archetypes.classify(row)
    if cls['archetype'] == 'manual' or cls['archetype'] not in archetypes.ARCHETYPES:
        raise Manual(f'{name}: classifies as {cls["archetype"]} ({cls["why"]}); not batch-built (turrets: TODO part-identity gate)')
    pp = analyze.ASSETS / f'{name}.params.json'
    drafted = not pp.is_file()
    if drafted:
        p = archetypes.draft_params(row, a)
        if p['archetype'] in FORMULA:      # these tagged fields come from the builder: count them once from the drafted model
            import arch_props
            archetypes.register(p)
            rc = validate.recount(validate.build_game(name, 'pristine'))
            for f in arch_props.TAGGED[p['archetype']]:
                p['counts'][f] = rc.get(f, 0)
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
    for q in (fresh, cur):
        q.pop('status', None)
        for f in ('counts',):
            q[f] = {k: v for k, v in q.get(f, {}).items() if v}
    return json.dumps(fresh, sort_keys=True) != json.dumps(cur, sort_keys=True)


# ------------------------------------------------------------------ Blender
def run_blender(tag, names, suffix, samples, conds, export, threads):
    import run as runmod
    import assemble
    os.environ['MC2_BLENDER_THREADS'] = str(threads)
    job = {'kind': 'assembly', 'tag': f'{tag}-{suffix}', 'out': str(OUT / tag), 'assemblies': list(names), 'conditions': conds, 'samples': samples, 'export': export}
    runmod.blender(job, OUT / 'logs' / f'{tag}-{suffix}.log', timeout=BAKE_TIMEOUT_S * max(1, len(names)))
    for n in names:
        assemble.ASSEMBLIES.setdefault(n, (lambda k, n=n: assemble._draft_info(k, n)))
        assemble.montage(OUT / tag, n, conds)


def run_tier(tag, names, tier, samples, tag_suffix, states=None):
    """Blender for `names` in at most 2 processes, threads = JOBS // processes (lane rule: one number N). -> list of error strings."""
    if not names:
        return []
    groups = [g for g in ([names[i::2] for i in range(2)] if len(names) > 1 else [names]) if g]
    threads = max(1, jobs_n() // len(groups))
    conds, export = (states or CONDS, True) if tier == 'B' else (states or TIER_A_CONDS, False)
    errors = []

    def worker(g, i):
        try:
            run_blender(tag, g, tag_suffix + 'abcd'[i], samples if tier == 'B' else min(samples, 16), conds, export, threads)
        except SystemExit as exc:
            errors.extend(f'{n}: {exc}' for n in g)
        except Exception as exc:
            errors.extend(f'{n}: {type(exc).__name__}: {exc}' for n in g)
    ths = [threading.Thread(target=worker, args=(g, i)) for i, g in enumerate(groups)]
    for th in ths:
        th.start()
    for th in ths:
        th.join()
    return errors


def blender_seconds(tag, names):
    """Per-asset Blender seconds from the job logs ('[art] done <name> <cond> <tris> s <sec>')."""
    res = {n: 0.0 for n in names}
    for log in (OUT / 'logs').glob(f'{tag}-*.log'):
        for m in re.finditer(r'\[art\] done (\S+) (\S+) \d+ s ([\d.]+)', log.read_text(errors='replace')):
            if m.group(1) in res:
                res[m.group(1)] += float(m.group(3))
    return {n: round(v, 1) for n, v in res.items()}


def stock_views(names):
    """The five stock sheets per asset from the numpy rasteriser (no Blender process)."""
    import npviews
    for n in names:
        if not all((STOCK_VIEWS / f'{n}_stock_{v}.png').is_file() for v in ('front', 'left', 'rear', 'right', 'top')):
            npviews.save_stock_views(n, STOCK_VIEWS)


def content_ir(tag, name, states=None):
    """content.py IR validation of the 15 baked glbs (ART-BAR 10.3); -> failure strings."""
    if not (PIPELINE / 'tools' / 'content_pipeline').is_dir():
        return [f'content: content pipeline not found at {PIPELINE}; IR validation cannot run']
    try:
        import content
        if states:
            return content.validate_states(OUT / tag, PIPELINE, name, states)
        content.prepare(OUT / tag, PIPELINE, OUT / tag / 'ir', [name])
        return []
    except Exception as exc:
        return [f'content: IR validation failed ({type(exc).__name__}: {str(exc)[:300]})']


def contact_sheet(tag, names, path, states=None):
    from PIL import Image, ImageDraw, ImageOps
    cell = 220
    conds = states or CONDS
    cols = ['stock front', 'stock left'] + conds
    im = Image.new('RGB', (150 + cell * len(cols), 40 + cell * len(names)), (24, 28, 32))
    d = ImageDraw.Draw(im)
    for c, label in enumerate(cols):
        d.text((150 + c * cell + 6, 14), label, fill=(190, 196, 190))
    for r, n in enumerate(names):
        d.text((6, 40 + r * cell + 6), n, fill=(232, 234, 230))
        files = [STOCK_VIEWS / f'{n}_stock_front.png', STOCK_VIEWS / f'{n}_stock_left.png'] + [OUT / tag / f'{n}_{c}_beauty.png' for c in conds]
        for c, f in enumerate(files):
            if f.is_file():
                t = ImageOps.contain(Image.open(f).convert('RGB'), (cell - 6, cell - 6))
                im.paste(t, (150 + c * cell + 3, 40 + r * cell + 3))
    im.save(path)
    return path


def five_sides(tag, names, path):
    import review_drafts
    return review_drafts.count_sheet(tag, names=names, out=path)


# ------------------------------------------------------------------ the batch
def batch(args):
    import contract
    import validate
    tag = args.tag
    if getattr(args, 'commit', False):
        return commit(args)
    if getattr(args, 'recheck', False):
        return recheck(args)
    expert = os.environ.get('MC2_KIT_EXPERT') == '1'     # --force-bake / --reanalyze are owner tools, hidden from runners
    if (args.force_bake or args.reanalyze) and not expert:
        print('[batch] --force-bake/--reanalyze are owner tools (set MC2_KIT_EXPERT=1); ignored')
        args.force_bake = args.reanalyze = False
    t_all = time.time()
    states = states_of(args)
    names = pick(args)
    if not names:
        print('[batch] nothing to do')
        return {}
    summary = {n: dict(stage={}) for n in names}
    print(f'[batch] {len(names)} assets: {", ".join(names)}', flush=True)
    set_status(names, 'unfinished')
    # 1. analyze + params + PRE-GATE (pure Python, no Blender)
    ready = []
    for n in names:
        s = summary[n]
        try:
            p, drafted, t = stage_prepare(n, args.reanalyze)
            s.update(archetype=p['archetype'], drafted=drafted, psha=contract.params_sha(p))
            s['stage'].update(t)
            t0 = time.time()
            res = validate.validate(n, states=states)
            s['stage']['validate_pre'] = round(time.time() - t0, 1)
            s['pre'] = res
            ok = res['ok']
        except Manual as exc:
            s['pre'] = dict(asset=n, ok=False, failures=[f'archetype: {exc}'], notes=[], waivers=[], metrics={})
            ok = False
        except Exception as exc:
            import traceback
            traceback.print_exc()
            s['pre'] = dict(asset=n, ok=False, failures=[f'build: {type(exc).__name__}: {exc}'], notes=[], waivers=[], metrics={})
            ok = False
        print(validate.report(s['pre']), flush=True)
        if ok or (args.force_bake and not s['pre']['failures'][0].startswith('archetype')):
            ready.append(n)
        else:
            s['state'] = park(n, s['pre'])
    # 2. stock views, numpy (no Blender)
    t0 = time.time()
    try:
        stock_views(names)
    except Exception as exc:
        print('[batch] stock views error:', exc)
    stage_views = round(time.time() - t0, 1)
    # 3. Blender, two tiers + params-hash cache
    t0 = time.time()
    tierA, tierB, cached = [], [], []
    for n in ready:
        psha = summary[n]['psha']
        if args.no_blender:
            continue
        if stamp_ok(tag, n, tier_key('B', states), psha):
            cached.append(n)
        elif args.full or stamp_ok(tag, n, tier_key('A', states), psha):
            tierB.append(n)
        else:
            tierA.append(n)
    summary_tier = {n: 'cached' for n in cached}
    summary_tier.update({n: 'A' for n in tierA})
    summary_tier.update({n: 'B' for n in tierB})
    print(f'[batch] bake: tier A {tierA}, tier B {tierB}, cached (params unchanged) {cached}', flush=True)
    errs = run_tier(tag, tierA, 'A', args.samples, 'a', states) + run_tier(tag, tierB, 'B', args.samples, 'b', states)
    bad = {e.split(':')[0] for e in errs}
    for e in errs:
        print('[batch] blender error:', e, flush=True)
    for n in tierA:
        if n not in bad:
            stamp_write(tag, n, tier_key('A', states), summary[n]['psha'])
    for n in tierB:
        if n not in bad:
            stamp_write(tag, n, tier_key('B', states), summary[n]['psha'])
    blender_wall = round(time.time() - t0, 1)
    secs = blender_seconds(tag, ready)
    # 4. post-bake validate + content IR (tier B / cached only), sheets
    passed, failed, iterating = [], [], []
    for n in names:
        s = summary[n]
        s['blender_s'] = secs.get(n)
        s['tier'] = summary_tier.get(n)
        res = s['pre']
        if n in ready and not args.no_blender:
            if n in bad:
                res = dict(res, ok=False, failures=res['failures'] + [f'bake: Blender failed or timed out ({[e for e in errs if e.startswith(n + ":")]})'])
            elif s['tier'] in ('B', 'cached'):
                res = validate.validate(n, pass_dir=OUT / tag, states=states)
                if res['ok']:
                    cf = content_ir(tag, n, states)
                    res = dict(res, ok=not cf, failures=res['failures'] + cf)
                s['post'] = res
            else:
                s['post'] = res
        s['ok'] = res['ok']
        if not res['ok']:
            failed.append(n)
            if n in ready:              # pre-gate failures were parked above
                park(n, res)
        elif s['tier'] == 'A':
            iterating.append(n)
            set_status([n], 'tierA_ok', psha=s['psha'])
        elif args.no_blender or s['tier'] is None:
            iterating.append(n)
            set_status([n], 'unfinished')
        else:
            passed.append(n)
            set_status([n], 'passed', psha=s['psha'])
    sheets = {}
    shown = [n for n in ready if n not in bad]
    if not args.no_blender and shown:
        try:
            sheets['contact'] = str(contact_sheet(tag, shown, OUT / tag / f'contact-{tag}.png', states))
            sheets['five_sides'] = str(five_sides(tag, shown, OUT / tag / f'five-sides-{tag}.png'))
        except Exception as exc:
            print('[batch] sheet error:', exc)
    # 5. report
    lines = []
    for n in failed:
        res = summary[n].get('post') or summary[n]['pre']
        lines.append(f'FAIL {n}')
        for f in res['failures']:
            lines.append(f'   {f}')
            lines.append(f'      -> {HINTS.get(f.split(":")[0], "")}')
    waivers = {n: (summary[n].get('post') or summary[n]['pre']).get('waivers', []) for n in names if (summary[n].get('post') or summary[n]['pre']).get('waivers')}
    zero_edit = [n for n in passed if not edited_since_draft(n)]
    total = round(time.time() - t_all, 1)
    parked = {n: load_status().get(n, {}).get('state') for n in failed if load_status().get(n, {}).get('state') in ('stuck', 'needs_builder')}
    if parked:
        print('[batch] PARKED (out of the batch picks):\n' + '\n'.join(f'   {n}: {s_} ({load_status()[n].get("why", "")})' for n, s_ in parked.items()))
    out = dict(tag=tag, assets=names, passed=passed, iterating=iterating, failed=failed, parked=parked, zero_edit_passed=zero_edit, waivers=waivers, wall_s=total, blender_wall_s=blender_wall,
               stock_views_s=stage_views,
               per_asset={n: dict(archetype=summary[n].get('archetype'), tier=summary[n].get('tier'), prep_s=round(sum(summary[n]['stage'].values()), 1), blender_s=summary[n].get('blender_s'),
                                  ok=summary[n]['ok'], failures=(summary[n].get('post') or summary[n]['pre'])['failures']) for n in names},
               sheets=sheets)
    d = OUT / tag
    d.mkdir(parents=True, exist_ok=True)
    (d / f'batch-{tag}.json').write_text(json.dumps(out, indent=1))
    (d / f'failures-{tag}.txt').write_text('\n'.join(lines) + '\n')
    print('\n[batch] RESULT', json.dumps({k: out[k] for k in ('passed', 'iterating', 'failed', 'zero_edit_passed', 'wall_s')}))
    if lines:
        print('[batch] FAILURES (edit params only, re-run the same command):\n' + '\n'.join(lines))
    if waivers:
        print('[batch] WAIVERS (listed for the owner review):\n' + '\n'.join(f'   {n}: {w}' for n, ws in waivers.items() for w in ws))
    if iterating:
        print(f'[batch] TIER A only (re-run the same command with unchanged params for the full bake): {iterating}')
    if passed:
        print(f'[batch] PASSED (run `run.py batch --commit` to commit): {passed}')
    return out


# ------------------------------------------------------------------ the commit gate
def kit_pytest(name):
    """The kit pytest for this asset's parametrised cases (build, damage lockstep, budgets); -> failure strings (empty when green)."""
    r = subprocess.run([sys.executable, '-B', '-m', 'pytest', str(ROOT / 'tools' / 'scifi_kit' / 'tests'), '-q', '-p', 'no:cacheprovider', '-k', name],
                       capture_output=True, text=True, cwd=str(ROOT), **_child_kwargs())
    if r.returncode == 0:
        return []
    tail = [ln for ln in r.stdout.splitlines() if ln.startswith(('FAILED', 'ERROR'))][:6] or r.stdout.splitlines()[-3:]
    return [f'pytest: kit pytest -k {name} is red (rc {r.returncode}): ' + ' | '.join(tail)]


def recheck(args):
    """Owner: committed assets (or --assets) against today's gates (pre-gate + model checks); failures are demoted to draft, parked by kind."""
    import analyze
    import validate
    names = [a.strip().lower() for a in args.assets.split(',')] if args.assets else [n for n in sorted(q.name.split('.params.json')[0] for q in analyze.ASSETS.glob('*.params.json')) if committed(n)]
    kept, demoted = [], {}
    for n in names:
        res = validate.validate(n)
        if res['ok']:
            kept.append(n)
            continue
        pp = analyze.ASSETS / f'{n}.params.json'
        p = json.loads(pp.read_text())
        p['status'] = dict(state='draft', demoted=dict(at=time.strftime('%Y-%m-%d'), failures=res['failures'][:6]))
        pp.write_text(json.dumps(p, indent=1))
        demoted[n] = park(n, res)
        print(f'[recheck] DEMOTED {n} -> draft ({demoted[n]}):\n   ' + '\n   '.join(res['failures'][:6]))
    print(f'[recheck] kept {kept}')
    return dict(kept=kept, demoted=demoted)



def commit(args):
    """Commit only assets that pass TODAY (pre-gate, baked, params unchanged since the passing bake); stamp the hashes."""
    import analyze
    import contract
    import validate
    tag = args.tag
    states = states_of(args)
    st = load_status()
    names = [a.strip().lower() for a in args.assets.split(',') if a.strip()] if args.assets else [n for n, v in st.items() if v.get('state') == 'passed']
    names = names[:COMMIT_MAX]
    if not names:
        print('[commit] nothing passed')
        return {}
    ok, refused = [], {}
    for n in names:
        pp = analyze.ASSETS / f'{n}.params.json'
        try:
            p = json.loads(pp.read_text())
            import archetypes
            archetypes.register(p)
            psha = contract.params_sha(p)
            if st.get(n, {}).get('state') != 'passed' or st[n].get('psha') != psha:
                refused[n] = ['status: no passing batch for this exact params hash; re-run the batch']
                continue
            if not stamp_ok(tag, n, tier_key('B', states), psha):
                refused[n] = ['bake: the full (tier B) bake is not current for these params/code']
                continue
            res = validate.validate(n, pass_dir=OUT / tag, states=states)
            fl = res['failures'] + (content_ir(tag, n, states) if res['ok'] else [])
            if fl:
                refused[n] = fl
                continue
            before = pp.read_text()
            p['status'] = dict(state='runner', validated=dict(params_sha=psha, analysis_sha=archetypes.analysis_sha(n), at=time.strftime('%Y-%m-%d')))
            pp.write_text(json.dumps(p, indent=1))
            fl = kit_pytest(n)                  # with the stamp in place (the stamp test reads it); red -> unstamp and refuse
            if fl:
                pp.write_text(before)
                refused[n] = fl
                continue
            ok.append(n)
        except Exception as exc:
            refused[n] = [f'commit: {type(exc).__name__}: {exc}']
    for n, fl in refused.items():
        print(f'[commit] REFUSED {n}:\n   ' + '\n   '.join(fl))
    if ok:
        paths = [f'tools/scifi_kit/art/assets/{n}.{k}.json' for n in ok for k in ('params', 'analysis', 'shape')]
        subprocess.run(['git', '-C', str(ROOT), 'add', '--'] + paths, check=True, **_child_kwargs())
        msg = f'feat(scifi-art): archetype batch {", ".join(ok)}'
        if os.environ.get('MC2_COMMIT_TRAILER'):
            msg += '\n\n' + os.environ['MC2_COMMIT_TRAILER']
        subprocess.run(['git', '-C', str(ROOT), 'commit', '-m', msg, '--'] + paths, check=True, **_child_kwargs())
        set_status(ok, 'committed')
        print(f'[commit] committed {ok}')
    if refused:
        sys.exit(1)
    return dict(committed=ok, refused=refused)
