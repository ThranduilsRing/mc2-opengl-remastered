"""CEMENT-ATLAS-CELL-2: append section 19 to the tracked review-pack manifest.

Run from A:/Games/mc2-main. Re-runnable: drops any existing 19_cement_concrete
section first, so it is safe to re-run after adding more captures.
"""
import json, glob, os

MAN = 'release_assets_src/0.5.2/review_pack.json'
DIR = '19_cement_concrete'
PACK = 'release_assets/0.5.2/REVIEW/' + DIR
METRICS = 'A:/Games/mc2-cac2/scratch/cac2/metrics_*.json'

d = json.load(open(MAN, encoding='utf-8'))
d['sections'] = [s for s in d['sections'] if s['id'] != '19_cement_concrete']

M = {}
for f in sorted(glob.glob(METRICS)):
    j = json.load(open(f))
    M[j['mission']] = j


def have(stem, tag):
    return os.path.exists('%s/%s_%s.png' % (PACK, stem, tag))


def cell(t, h, a=''):
    return {"tag": t, "attrs": a, "html": h}


B = []


def add(t, **kw):
    B.append(dict(type=t, **kw))


def floor_invalid(v):
    fl = v.get('floor_pct', -1)
    return fl > max(v.get('A_pct', 0), v.get('B_pct', 0)) * 0.5


add('what', html=(
    "The user's ruling: <i>\"do 3, in the index.html &mdash; also ... by all concrete I mean only the "
    "<b>actual concrete</b> ... obviously runways, roads, other overlays etc. are their own layer and are "
    "currently handled by the shader.\"</i> Two arms are built, both <b>default OFF</b>, and presented here "
    "to choose from. Neither is recommended by the lane &mdash; the frames are the decision."
    "<br><br><b>ARM A</b> <code>MC2_CEMENT_CONCRETE_TONE=1</code> &mdash; each concrete cell keeps its own "
    "art; only its <i>base tone</i> is pulled onto one reference. <b>ARM B</b> "
    "<code>MC2_CEMENT_CONCRETE_CANON=1</code> &mdash; every plain-concrete tile samples <i>one</i> canonical "
    "cell, so concrete cannot change colour tile to tile at all. Both act only on cells passing an explicit "
    "<b>is-this-actually-concrete</b> test (near-neutral chroma &le; 0.20, band luminance within "
    "[0.72,1.32] of the family centre)."))

add('note', html=(
    "<b>SCOPE QUESTION ANSWERED &mdash; markings do NOT live in the concrete cells.</b> This is what blocked "
    "the previous lane, and the ruling is confirmed twice over. <b>From source:</b> the solid-cement map "
    "<code>g_cementLayerIndexBySlot</code> that ARM B repoints is built only from slots where "
    "<code>isCement &amp;&amp; !isAlpha</code>; alpha cells are explicitly skipped when it is filled, and "
    "<code>TileIsRoadArt()</code> says it outright &mdash; a slot present in that map is <i>\"solid cement, "
    "not road\"</i>. Runway and road art arrives either as an <code>&amp;Overlays</code> id (its own sampler "
    "and blend, <code>u_overlaySidecar</code> / <code>terrain_overlay.frag</code>) or as a "
    "cement-<b>alpha</b> transition cell, which is appended past the solid range both arms operate on. "
    "<b>From measurement:</b> the cement atlas was dumped (new <code>MC2_CEMENT_ATLAS_DUMP</code> "
    "diagnostic) and inspected &mdash; on mc2_24, <b>not one of the 110 solid cells has a single texel more "
    "than 45 luma above its band mean</b> (largest excursion ~11). There is no painted marking art in solid "
    "concrete cells to erase. The census reports <code>family_marked=0</code>; a map that <i>does</i> pack "
    "paint into a solid cell is detected (&gt;2% of texels &gt;60 luma above band) and excluded from ARM B "
    "by index."))

add('note', html=(
    "<b>MEASURED &mdash; and it reframes the problem.</b> Dumping the atlas showed cross-cell <i>base tone</i> "
    "is far more uniform than assumed: on mc2_24 the median cell sits at L=146.3 with p05&ndash;p95 = "
    "133&ndash;148. What actually varies is <i>inside</i> the cells &mdash; <b>78 of 110 cells carry a large "
    "dark region</b>, 36&ndash;52% of the tile at L&asymp;60 against concrete at L&asymp;147. That in-cell "
    "dark art, not cross-cell tone, is the dominant reason concrete reads as changing colour here. It is "
    "why ARM B (which replaces the whole cell) moves ~11&ndash;15 luma while ARM A (which only rescales "
    "base tone) moves ~1&ndash;2."))

add('note', html=(
    "<b>HONEST CAVEAT ON ARM A &mdash; read before grading.</b> On mc2_24 ARM A's census line is "
    "<code>family=110 adjusted=96 ref_rgb=149.0,146.4,138.4</code>, which is <b>numerically identical</b> to "
    "the census of the gate already rejected (<code>MC2_CEMENT_TONE_MATCH</code>, <i>\"after is WAY "
    "worse\"</i>). On that map every solid cell passes the concrete test, so concrete-only scoping buys "
    "nothing and ARM A <b>is</b> that rejected lever with a narrower clamp. It differs only where a map "
    "packs genuinely non-concrete cement cells. If mc2_24 is the reference, expect to reject ARM A again "
    "&mdash; the lane will not claim otherwise."))

add('meta', attrs='class="meta"', rows=[
    [cell('th', 'Gates'), cell('td',
        '<code>MC2_CEMENT_CONCRETE_TONE=1</code> (ARM A) &middot; '
        '<code>MC2_CEMENT_CONCRETE_CANON=1</code> (ARM B) &mdash; both DEFAULT OFF, byte-identical when off')],
    [cell('th', 'Commits'), cell('td',
        '<code>d6ae45f9</code> both arms + concrete-family classifier &middot; '
        '<code>7c80c004</code> marking detector recalibrated against the dumped atlas')],
    [cell('th', 'Build'), cell('td',
        'exe sha <code>7c80c004ada9</code>, DIRTY=0, built in an <b>isolated worktree</b> (the shared tree was '
        'under live sibling edits mid-session). <b>tier1 5/5 PASS</b> with '
        '<code>[DEPLOY_FINGERPRINT] OK</code> logged on all five missions.')],
    [cell('th', 'Measurement floor'), cell('td',
        'Two-run floor published per pose below, before any claim. <b>0.0000% exact</b> on the headline poses '
        '(mc2_01 &times;3, mc2_03 &times;2, mc2_05 control, mc2_23 runway_ground, mc2_24 industrial + water '
        'control). Poses whose floor exceeds the effect are marked FLOOR-INVALID and carry no claim.')],
    [cell('th', 'No-pavement control'), cell('td',
        'mc2_24 water_near, mc2_01 beach_north_wide, mc2_05 control_no_buildings, mc2_03 civstore_lightaway, '
        'mc2_10 saved_212019 &mdash; all measure <b>exactly 0.00%</b> changed on both arms. Neither gate '
        'touches anything but concrete.')],
])

# ---- atlas spread table
rows = [[cell('th', 'Map'), cell('th', 'OFF cross-cell spread'), cell('th', 'ARM A'), cell('th', 'ARM B')]]
for m in ['mc2_01', 'mc2_03', 'mc2_05', 'mc2_10', 'mc2_23', 'mc2_24']:
    a = M.get(m, {}).get('atlas', {})
    if 'OFF' not in a:
        continue
    rows.append([
        cell('td', '<code>%s</code>' % m),
        cell('td', '%.1f luma (std %.2f)' % (a['OFF']['spread'], a['OFF']['std'])),
        cell('td', '%.1f luma (std %.2f)' % (a['A']['spread'], a['A']['std'])),
        cell('td', 'atlas unchanged &mdash; collapses to <b>one cell in use</b>')])
add('h3', html='Concrete tone spread across atlas cells (the &ldquo;43 &rarr; 135&rdquo; number, measured at source)')
add('note', html=(
    "ARM A rescales the cells, so its effect shows here directly. ARM B leaves the atlas untouched and "
    "instead makes every plain-concrete tile sample a <b>single</b> cell &mdash; so its effective in-use "
    "spread is <b>zero by construction</b>, which no atlas statistic can express."))
add('meta', attrs='class="meta"', rows=rows)

# ---- per-pose metrics table
rows = [[cell('th', 'Map'), cell('th', 'Pose'), cell('th', '2-run floor'),
         cell('th', 'ARM A: px / mean &Delta;L'), cell('th', 'ARM B: px / mean &Delta;L'),
         cell('th', 'hard-step % OFF / A / B')]]
for m in ['mc2_01', 'mc2_03', 'mc2_05', 'mc2_10', 'mc2_23', 'mc2_24']:
    if m not in M:
        continue
    for k, v in sorted(M[m]['poses'].items(), key=lambda kv: -kv[1].get('B_mean', 0)):
        stem = k[:-4]
        if not have(stem, 'off'):
            continue
        fl = v.get('floor_pct', -1)
        fls = ('<b style="color:#c0392b">%.4f%% FLOOR-INVALID</b>' % fl) if floor_invalid(v) else ('%.4f%%' % fl)
        rows.append([
            cell('td', '<code>%s</code>' % m),
            cell('td', stem.replace(m + '_', '')),
            cell('td', fls),
            cell('td', '%.1f%% / %.2f' % (v.get('A_pct', 0), v.get('A_mean', 0))),
            cell('td', '%.1f%% / %.2f' % (v.get('B_pct', 0), v.get('B_mean', 0))),
            cell('td', '%.3f / %.3f / %.3f' % (v.get('hardstep_OFF', 0), v.get('hardstep_A', 0),
                                               v.get('hardstep_B', 0)))])
add('h3', html='Per-pose measurement (two-run floor first, as required)')
add('note', html=(
    "<b>Hard-step %</b> is the fraction of adjacent-pixel luminance jumps &gt;16 &mdash; introducing new hard "
    "steps is what sank <code>MC2_PAVED_OVERLAY_CLASS</code> (0.6&ndash;10.8pp). <b>ARM A adds at most "
    "+0.06pp; ARM B at most +0.62pp</b> (mc2_24 saved_192316) &mdash; both at or below the bottom of that "
    "rejected range, and on several poses both <i>reduce</i> it."))
add('meta', attrs='class="meta"', rows=rows)

TITLES = {
    'mc2_01': 'mc2_01 &mdash; DAY airbase runway apron (floor 0.0000%)',
    'mc2_03': 'mc2_03 &mdash; NIGHT industrial (floor 0.0000%)',
    'mc2_05': 'mc2_05 &mdash; user bookmark pose + compound',
    'mc2_10': 'mc2_10 &mdash; NIGHT airbase (dynamic poses FLOOR-INVALID, shown for completeness only)',
    'mc2_23': 'mc2_23 &mdash; NIGHT runway, the reference map for this complaint (floor 0.0000%)',
    'mc2_24': 'mc2_24 &mdash; DAY airbase apron, the richest concrete atlas (110 cells)',
}

for m in ['mc2_24', 'mc2_23', 'mc2_01', 'mc2_03', 'mc2_05', 'mc2_10']:
    if m not in M:
        continue
    stems = [os.path.basename(p)[:-8] for p in sorted(glob.glob('%s/%s_*_off.png' % (PACK, m)))]
    if not stems:
        continue
    add('h3', html=TITLES.get(m, m))
    for stem in stems:
        v = M[m]['poses'].get(stem + '.png', {})
        pose = stem.replace(m + '_', '')
        tag = ''
        if floor_invalid(v):
            tag = (' &mdash; <b style="color:#c0392b">FLOOR-INVALID (two-run floor %.1f%% exceeds the '
                   'effect)</b>' % v.get('floor_pct', -1))
        ctrl = ''
        if v.get('A_mean', 0) == 0 and v.get('B_mean', 0) == 0:
            ctrl = ' &mdash; <b>no-pavement CONTROL: both arms exactly 0.00%</b>'
        add('pl', html='<b>%s</b>%s%s' % (pose, ctrl, tag))
        items = []
        for tg, lab, role in [('off', 'OFF (default / shipped)', 'BEFORE'),
                              ('armA', 'ARM A &mdash; concrete tone normalised', 'AFTER'),
                              ('armB', 'ARM B &mdash; one canonical concrete cell', 'AFTER')]:
            if have(stem, tg):
                items.append({"caption": lab, "role": role, "src": "%s/%s_%s.png" % (DIR, stem, tg)})
        if items:
            add('figures', layout='pair', pair=False, items=items)
        if have(stem, 'off_crop'):
            add('figures', layout='ba', pair=True, items=[
                {"caption": "OFF &mdash; detail (2&times;, largest-change region)", "role": "BEFORE",
                 "src": "%s/%s_off_crop.png" % (DIR, stem)},
                {"caption": "ARM A &mdash; same detail (2&times;)", "role": "AFTER",
                 "src": "%s/%s_armA_crop.png" % (DIR, stem)}])
            add('figures', layout='ba', pair=True, items=[
                {"caption": "OFF &mdash; detail (2&times;)", "role": "BEFORE",
                 "src": "%s/%s_off_crop.png" % (DIR, stem)},
                {"caption": "ARM B &mdash; same detail (2&times;)", "role": "AFTER",
                 "src": "%s/%s_armB_crop.png" % (DIR, stem)}])

d['sections'].append({
    "id": "19_cement_concrete",
    "anchor": "#19_cement_concrete",
    "heading": "19. Concrete tone &mdash; ARM A vs ARM B (your choice)",
    "blocks": B,
})
json.dump(d, open(MAN, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
print('section 19 written: %d blocks, %d sections total' % (len(B), len(d['sections'])))
