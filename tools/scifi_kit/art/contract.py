"""The pure-JSON pre-gate: everything about an asset that can be decided WITHOUT building geometry or starting Blender.

The contract comes from the stock ANALYSIS evidence (analysis.json), never from params: params may deviate only through an
explicit waiver {check, <keys>, reason} that the batch report lists for the owner.

  archetype lock   params.archetype == classify(inventory row); turrets are routed to manual
  schema           unknown params / dims / detail keys, damage allow-list, palette keys complete, budget only lowered
  size             params.size within 2% of the stock bounds
  counts           doors/windows/bays (box, tower, tent, container) and gate leaves equal the stock evidence
  not verifiable   a non-zero count for a field the archetype does not tag in the model fails
  palette          every checked slot within PALETTE_DE of a stock dominant colour (an unreadable stock palette fails)
  integrity        validate_thresholds.json and analysis.json are exactly what the code derives (hash / re-derivation)
Pure Python + numpy; never imported by Blender.
"""
import hashlib
import json
from pathlib import Path

import numpy as np

import analyze
import archetypes

HERE = Path(__file__).resolve().parent
THRESH = HERE / 'assets' / 'validate_thresholds.json'
THRESH_SHA = 'ff9c0abac8a80088'        # sha256[:16] of assets/validate_thresholds.json; `validate --calibrate` prints the new value to paste here
PALETTE_DE = 18.0                       # per-slot and area-weighted mean: the approved closed solids top out at about 14
SIZE_TOL = .02
WAIVER_KEYS = dict(counts=('field',), features=('side', 'kind'), palette=('slot',), lod=())
# gross-shape checks: never waived. A builder that cannot reach them sends the asset to state needs_builder (batch), not to a waiver.
STRUCTURAL = ('archetype', 'size', 'fit', 'silhouette', 'round', 'shape', 'footprint', 'roof')
SHAPE_BUILT = ('box', 'tank')           # archetypes whose gross shape is built from shape.json (shell.py)
CODE_FILES = ('shape.py', 'shell.py', 'contract.py', 'validate.py', 'archetypes.py', 'arch_props.py', 'batch3.py', 'drafts.py', 'geo.py', 'npviews.py', 'analyze.py', 'bake.py', 'blend.py', 'lang.json')


# ------------------------------------------------------------------ hashes
def file_sha(path):
    return hashlib.sha256(Path(path).read_bytes().replace(b'\r\n', b'\n')).hexdigest()[:16]      # newline-insensitive (CRLF working copies)


def params_sha(p):
    """Hash of the params minus the `status` block (status carries the stamp, so it cannot be part of its own hash)."""
    q = {k: v for k, v in p.items() if k != 'status'}
    return hashlib.sha256(json.dumps(q, sort_keys=True).encode()).hexdigest()[:16]


def code_sha():
    h = hashlib.sha256()
    for f in CODE_FILES:
        q = HERE / f
        if q.is_file():
            h.update(q.read_bytes())
    return h.hexdigest()[:16]


# ------------------------------------------------------------------ colour
def lab(rgb_lin):
    c = np.clip(np.asarray(rgb_lin, float), 0, 1)
    m = np.array([[.4124, .3576, .1805], [.2126, .7152, .0722], [.0193, .1192, .9505]])
    xyz = (m @ c) / np.array([.9505, 1.0, 1.089])
    f = np.where(xyz > .008856, np.cbrt(xyz), 7.787 * xyz + 16 / 116)
    return np.array([116 * f[1] - 16, 500 * (f[0] - f[1]), 200 * (f[1] - f[2])])


def delta_e(a, b):
    return float(np.linalg.norm(lab(a) - lab(b)))


# ------------------------------------------------------------------ waivers
def waiver_problems(p):
    out = []
    ws = p.get('waivers', [])
    if not isinstance(ws, list):
        return ['waiver: params.waivers must be a list of {check, <keys>, reason}']
    for w in ws:
        chk = w.get('check') if isinstance(w, dict) else None
        if chk in STRUCTURAL:
            out.append(f'waiver: {chk} is a structural check and cannot be waived; if the builder cannot reach the stock shape the asset is needs_builder')
            continue
        if chk not in WAIVER_KEYS:
            out.append(f'waiver: unknown check {chk!r} (allowed: {", ".join(WAIVER_KEYS)})')
            continue
        miss = [k for k in WAIVER_KEYS[chk] if k not in w]
        if miss:
            out.append(f'waiver: {chk} waiver needs keys {miss}')
        if len(str(w.get('reason', '')).strip()) < 15:
            out.append(f'waiver: {chk} waiver {({k: v for k, v in w.items() if k != "reason"})} needs a reason (>= 15 chars) that names the stock evidence')
    return out


def waived(p, check, **q):
    if check in STRUCTURAL:
        return False
    for w in p.get('waivers', []):
        if isinstance(w, dict) and w.get('check') == check and len(str(w.get('reason', '')).strip()) >= 15 and all(w.get(k) == v for k, v in q.items()):
            return True
    return False


def waiver_list(p):
    return [{k: v for k, v in w.items()} for w in p.get('waivers', []) if isinstance(w, dict)]


# ------------------------------------------------------------------ integrity
def thresholds_intact():
    if not THRESH.is_file():
        return ['integrity: assets/validate_thresholds.json is missing']
    if file_sha(THRESH) != THRESH_SHA:
        return [f'integrity: assets/validate_thresholds.json was modified (sha {file_sha(THRESH)} != {THRESH_SHA}); thresholds are never edited by a runner']
    return []


def analysis_intact(name):
    """analysis.json must equal what the stock mesh derives today (a hand-edit of the evidence is a failure)."""
    ap = analyze.ASSETS / f'{name}.analysis.json'
    if not ap.is_file():
        return [f'integrity: {name}.analysis.json is missing (run `run.py analyze {name}`)']
    cur = json.loads(ap.read_text())
    cur.pop('archetype_guess', None)
    fresh = json.loads(json.dumps(analyze.analyze(name)))
    if cur != fresh:
        keys = sorted(k for k in set(cur) | set(fresh) if cur.get(k) != fresh.get(k))
        return [f'integrity: {name}.analysis.json differs from the stock re-derivation in {keys}; analysis is evidence, not an editable input']
    return []


def shape_intact(name):
    """shape.json must equal what the stock mesh derives today (shape.py); like analysis.json it is evidence, not an input."""
    sp = analyze.ASSETS / f'{name}.shape.json'
    if not sp.is_file():
        return [f'integrity: {name}.shape.json is missing (run `run.py analyze {name}`)']
    if json.loads(sp.read_text()) != analyze.shape_of(name):
        return [f'integrity: {name}.shape.json differs from the stock re-derivation (shape.py); the shape is evidence, not an editable input']
    return []


# ------------------------------------------------------------------ the gate
def _slot_targets(a, slot):
    pal = a.get('palette') or {}
    dom = [d['linear'] for d in pal.get('dominant', [])]
    if slot == 'roof':
        dom = [d['linear'] for d in pal.get('roof_dominant', [])] + dom
    return dom


def check_palette(p, a):
    fails, met = [], {}
    pal = p.get('palette', {})
    miss = [k for k in archetypes.PAL_KEYS if k not in pal]
    if miss:
        fails.append(f'schema: palette is missing keys {miss} (all of {list(archetypes.PAL_KEYS)} are required)')
    bad = [k for k, v in pal.items() if not (isinstance(v, (list, tuple)) and len(v) == 3 and all(isinstance(x, (int, float)) for x in v))]
    if bad:
        fails.append(f'schema: palette values must be [r,g,b] linear floats: {bad}')
    if not a.get('palette') or not a['palette'].get('dominant'):
        fails.append('palette: no stock palette evidence (stock texture unreadable); cannot verify the palette')
        return fails, met
    for slot in ('main', 'second', 'roof', 'trim'):
        if slot not in pal or slot in bad:
            continue
        de = min(delta_e(pal[slot], t) for t in _slot_targets(a, slot))
        met[slot] = round(de, 1)
        if de > PALETTE_DE and not waived(p, 'palette', slot=slot):
            fails.append(f'palette: slot {slot} dE {de:.1f} from the nearest stock dominant colour (max {PALETTE_DE:.0f}); edit palette.{slot} toward analysis palette.dominant, or waive {{check:palette, slot:{slot}, reason}}')
    return fails, met


def check_schema(p, arch):
    fails = []
    extra = sorted(set(p) - set(archetypes.TOP_KEYS))
    if extra:
        fails.append(f'schema: unknown params keys {extra}')
    dims = p.get('dims', {})
    if not isinstance(dims, dict):
        return fails + ['schema: dims must be an object']
    unk = sorted(set(dims) - set(archetypes.DIM_KEYS.get(arch, ())))
    if unk:
        fails.append(f'schema: unknown dims keys for archetype {arch}: {unk} (allowed {list(archetypes.DIM_KEYS.get(arch, ()))})')
    unk = sorted(set(p.get('detail', {})) - set(archetypes.DETAIL_KEYS))
    if unk:
        fails.append(f'schema: unknown detail keys {unk} (allowed {list(archetypes.DETAIL_KEYS)})')
    if p.get('damage') is not None and p['damage'] not in archetypes.DAMAGE_ALLOWED:
        fails.append(f'schema: damage {p["damage"]!r} not in {list(archetypes.DAMAGE_ALLOWED)}')
    cnt = sorted(set(p.get('counts', {})) - set(archetypes.FIELD.values()))
    if cnt:
        fails.append(f'schema: unknown counts keys {cnt}')
    if 'budget' in p and int(p['budget']) > archetypes.class_budget(p):
        fails.append(f'budget: params.budget {p["budget"]} is above the class budget {archetypes.class_budget(p)} (params may only lower it)')
    return fails


def evidence_ok(p, a, arch):
    return [] if p.get('evidence') == archetypes.evidence_of(a, arch) else [
        'evidence: params.evidence differs from the analysis-derived evidence (edited or stale); do not edit it, record deviations in waivers']


def pre_gate(name, p, deep=True):
    """-> (fails, notes, metrics). `deep` re-derives analysis.json (about a second); the batch always passes it."""
    import arch_props
    fails, notes, met = [], [], {}
    fails += thresholds_intact()
    fails += waiver_problems(p)
    ap = analyze.ASSETS / f'{name}.analysis.json'
    if not ap.is_file():
        return fails + [f'integrity: {name}.analysis.json is missing (run `run.py analyze {name}`)'], notes, met
    a = json.loads(ap.read_text())
    if deep:
        fails += analysis_intact(name)
        fails += shape_intact(name)
    arch = p.get('archetype')
    # --- archetype lock
    row = analyze.inventory_row(name)
    cls = archetypes.classify(row)['archetype']
    met['archetype'] = cls
    if cls == 'manual':
        fails.append(f'archetype: {name} classifies as manual (turret/dropship/...): it is not batch-built (TODO turret part-identity gate)')
    elif arch != cls:
        fails.append(f'archetype: params say {arch!r} but classify/analysis evidence says {cls!r}; the archetype is locked (structural: not waivable)')
    fails += check_schema(p, arch)
    if arch in archetypes.DIM_KEYS:
        pass
    else:
        fails.append(f'schema: unknown archetype {arch!r}')
        return fails, notes, met
    # --- size
    bs = a['bounds']['size_m']
    sz = p.get('size', [])
    if len(sz) != 3 or any(abs(x - y) > SIZE_TOL * max(y, .05) for x, y in zip(sz, bs)):
        fails.append(f'size: params.size {list(sz)} differs from the stock bounds {bs} by more than {SIZE_TOL:.0%} (structural: not waivable)')
    # --- evidence + counts
    fails += evidence_ok(p, a, cls if cls != 'manual' else arch)
    ev = archetypes.evidence_of(a, arch)['counts']
    counts = p.get('counts', {})
    for f, n in ev.items():
        have = int(counts.get(f, 0))
        if have != n and not waived(p, 'counts', field=f):
            fails.append(f'counts: {f} is {have} in params but the stock evidence says {n} (side views / texture); edit counts only with {{check:counts, field:{f}, reason}}')
    tagged = set(arch_props.TAGGED.get(arch, ()))
    if not tagged:
        fails.append(f'counts: archetype {arch} has no tagged required fields')
    for f, n in counts.items():
        if int(n) and f not in tagged and not waived(p, 'counts', field=f):
            fails.append(f'counts: {f}={n} is not verifiable for archetype {arch} (tagged: {sorted(tagged)}); remove it or waive {{check:counts, field:{f}, reason}}')
    for f in ev:
        if f in ('doors', 'windows', 'openings_bays') and ev[f] and f not in tagged and not waived(p, 'counts', field=f):
            fails.append(f'counts: the stock shows {ev[f]} painted {f} but {arch} models cannot carry/verify them; waive {{check:counts, field:{f}, reason}} if that is right')
    # --- builder capability for the stock shape (shape-built archetypes): a missing capability is needs_builder, not a params fix
    if arch in SHAPE_BUILT and (analyze.ASSETS / f'{name}.shape.json').is_file():
        import shell
        S = json.loads((analyze.ASSETS / f'{name}.shape.json').read_text())
        why = shell.unsupported(S, arch)
        if why and not (arch == 'tank' and S['footprint']['kind'] == 'circle'):
            fails.append(f'needs_builder: {why}')
    # --- palette
    pf, pm = check_palette(p, a)
    fails += pf
    met['palette_slots'] = pm
    return fails, notes, met
