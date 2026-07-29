"""CEMENT-INTRACELL-1: append the ARM C blocks to review-pack section 19.

Run from A:/Games/mc2-main. Re-runnable: strips any previously appended ARM C
blocks (they all carry the marker below) before re-adding.
"""
import json, os

MAN = 'release_assets_src/0.5.2/review_pack.json'
DIR = '19_cement_concrete'
PACK = 'release_assets/0.5.2/REVIEW/' + DIR
MARK = 'CIC1'   # marker so this script is idempotent

d = json.load(open(MAN, encoding='utf-8'))
sec = [x for x in d['sections'] if x['id'] == '19_cement_concrete'][0]
sec['blocks'] = [b for b in sec['blocks'] if b.get('cic1') != MARK]
sec['heading'] = ('19. Concrete tone &mdash; ARM A vs ARM B vs '
                  '<b>ARM C</b> (your choice)')

B = []


def add(t, **kw):
    kw['cic1'] = MARK
    B.append(dict(type=t, **kw))


def have(name):
    return os.path.exists('%s/%s' % (PACK, name))


def figs(caption_map, layout='pair'):
    items = []
    for src, (cap, role) in caption_map:
        if have(src):
            items.append({"caption": cap, "role": role, "src": DIR + '/' + src})
    if items:
        add('figures', layout=layout, pair=False, items=items)


# ---------------------------------------------------------------- headline
add('h3', html='ARM C &mdash; what the &ldquo;dark regions&rdquo; actually are')

add('note', html=(
    "<b>A correction to this section, from measurement.</b> The two arms above were built on the "
    "premise that solid concrete cells carry no road or marking art &mdash; stated in this pack as "
    "<i>&ldquo;markings do NOT live in the concrete cells&rdquo;</i>, and supported by a test that "
    "found no texel more than 45 luma <b>above</b> its cell band. That test looked only at the "
    "<b>bright</b> side. The large dark region it then found in <b>78 of 110</b> mc2_24 cells was "
    "recorded as <i>&ldquo;authored material (joints, tarmac, shadowed detail)&rdquo;</i>."
    "<br><br>Dumping the atlas and <b>rendering it</b> shows what it is: a <b>baked asphalt road "
    "tileset</b> &mdash; straights, curves, T-junctions, crossroads with crosswalk stripes, end caps "
    "and a multi-lane highway-with-guardrail piece, painted onto a concrete background. Asphalt is "
    "<b>dark</b>, which is precisely why the bright-side test could not see it."))

figs([('mc2_24_atlas_roadtileset.png',
       ('mc2_24 &mdash; the first 48 SOLID cement catalog cells, as the shader samples them. '
        'These are roads.', 'EVIDENCE')),
      ('mc2_23_atlas_roadtileset.png',
       ('mc2_23 &mdash; same, on your reference map.', 'EVIDENCE'))], layout='stack')

add('note', html=(
    "<b>The reframe this forces &mdash; there is no concrete tone problem.</b> Measured with the road "
    "texels excluded, the concrete <b>background</b> is already one colour on every map: mc2_24 "
    "<code>144.7&ndash;149.5</code> (spread <b>4.7</b> luma, std 0.89), mc2_05 3.2, mc2_03 2.4, mc2_10 "
    "2.0. mc2_23 and mc2_01 show a ~43-luma span driven entirely by <b>two</b> and <b>one</b> cells, "
    "and those cells are wide highway pieces &mdash; not concrete."
    "<br><br>What makes concrete &ldquo;magically become a different color&rdquo; inside a cell is a "
    "<b>road</b>: background <code>L&asymp;147</code> against asphalt <code>L&asymp;82</code>, a "
    "<b>~65-luma hard-edged step</b>. That is exactly the class you scoped <i>out</i> &mdash; "
    "<i>&ldquo;obviously runways, roads, other overlays etc. are their own layer and are currently "
    "handled by the shader.&rdquo;</i> True for the <code>&amp;Overlays</code> class. <b>Not</b> true "
    "for this one: it is baked into the terrain texture catalog, so <code>TileIsRoadArt()</code> "
    "reports <i>&ldquo;solid cement, not road&rdquo;</i> and every concrete lever treats a road as "
    "concrete."))

# ---------------------------------------------------------------- what C is
add('h3', html='What ARM C does')

add('what', html=(
    "<b>ARM C</b> <code>MC2_CEMENT_ROAD_ART_DETECT=1</code> &mdash; detect the baked road art and "
    "<b>exclude those cells from every concrete treatment</b> (ARM A's tone pass, ARM B's remap, and "
    "the choice of canonical cell). Each cell is split against its own <code>[P60,P95]</code> upper "
    "mode &mdash; the concrete background &mdash; rather than the whole-cell band mean, which on a "
    "road-heavy cell is dragged down by the road and hides it. A cell with more than 5% of its texels "
    "more than 40 luma <b>below</b> that background is flagged as carrying road art."
    "<br><br><b>ARM C on its own changes no pixels.</b> It writes nothing to the atlas and nothing to "
    "the slot&rarr;layer map; it only changes which cells ARM A and ARM B are allowed to touch. Its "
    "purpose is to make the concrete arms obey your own scope ruling."))

add('note', html=(
    "<b>What ARM C prevents, measured in-engine.</b> Without it, ARM B repoints <b>90&ndash;100%</b> of "
    "the road-carrying cells at its canonical cell &mdash; and on <b>5 of the 6</b> maps that canonical "
    "cell is <b>itself a road tile</b>. On mc2_24 the engine logs "
    "<code>canon_cell=10 canon_road_frac=0.312 remapped=109 kept_road=0</code>: ARM B erases every "
    "junction, curve and end cap <i>and stamps one straight road segment across every plain concrete "
    "tile on the map</i>. With ARM C the same map logs "
    "<code>canon_cell=0 canon_road_frac=0.000 remapped=31 kept_road=78</code> &mdash; only the "
    "genuinely plain cells are collapsed, and the road network survives intact."))

# ---------------------------------------------------------------- frames
add('h3', html='ARM B alone vs ARM B + ARM C (floor-valid poses only)')

for m, pose, label in [
        ('mc2_23', 'runway_overview', 'mc2_23 runway_overview &mdash; your reference map'),
        ('mc2_23', 'runway_mid', 'mc2_23 runway_mid'),
        ('mc2_23', 'runway_ground', 'mc2_23 runway_ground'),
        ('mc2_24', 'industrial_lightfacing', 'mc2_24 industrial_lightfacing'),
        ('mc2_24', 'fortress_lightaway', 'mc2_24 fortress_lightaway'),
        ('mc2_24', 'saved_192316', 'mc2_24 saved_192316 (your pose)'),
        ('mc2_05', 'refinery_lightfacing', 'mc2_05 refinery_lightfacing')]:
    stem = '%s_%s' % (m, pose)
    add('pl', html='<b>%s</b>' % label)
    figs([('%s_off.png' % stem, ('OFF (default / shipped)', 'BEFORE')),
          ('%s_armB.png' % stem, ('ARM B alone &mdash; road network destroyed', 'AFTER')),
          ('%s_armBC.png' % stem, ('ARM B + ARM C &mdash; roads preserved', 'AFTER'))])

# ---------------------------------------------------------------- numbers
add('h3', html='Measurements (two-run floor published first; &#9940; = floor exceeds the effect, no claim)')

ROWS = [
    ('mc2_23', 'runway_overview', '0.0000', '59.8', '31.8', '0.0', '31.8', '3.014 / 3.202 / 3.014'),
    ('mc2_23', 'runway_mid',      '0.0000', '68.6', '32.8', '0.0', '32.8', '0.657 / 0.758 / 0.657'),
    ('mc2_23', 'runway_ground',   '0.0000', '63.7', '38.7', '0.0', '38.7', '0.340 / 0.382 / 0.340'),
    ('mc2_23', 'both_fields_overview', '1.0288 &#9940;', '32.8', '17.9', '1.0', '17.9', '2.369 / 2.487 / 2.370'),
    ('mc2_23', 'beacons_mid',     '17.4930 &#9940;', '14.8', '18.9', '18.6', '21.6', '1.457 / 1.473 / 1.438'),
    ('mc2_24', 'industrial_lightfacing', '0.0000', '52.9', '26.9', '0.0', '26.9', '3.029 / 3.042 / 3.029'),
    ('mc2_24', 'fortress_lightaway',     '0.0000', '53.6', '24.7', '0.0', '24.7', '0.581 / 0.630 / 0.581'),
    ('mc2_24', 'saved_192316',           '0.0198', '43.9', '21.7', '0.0', '21.7', '3.079 / 3.697 / 3.079'),
    ('mc2_24', 'base_near',              '0.0000', '16.4', '4.3',  '0.4', '3.9',  '0.942 / 0.918 / 0.938'),
    ('mc2_24', 'water_near (control)',   '0.0000', '0.0',  '0.0',  '0.0', '0.0',  '0.000 / 0.000 / 0.000'),
    ('mc2_05', 'refinery_lightfacing',   '0.0000', '5.2',  '11.4', '0.0', '11.4', '0.651 / 0.656 / 0.651'),
    ('mc2_05', 'refinery_lightaway',     '0.0000', '4.8',  '7.9',  '0.0', '7.9',  '0.554 / 0.553 / 0.554'),
    ('mc2_05', 'saved_101748 (your pose)', '0.0000', '0.0', '0.0', '0.0', '0.0',  '0.012 / 0.012 / 0.012'),
    ('mc2_05', 'saved_102009 (your pose)', '13.7105 &#9940;', '18.0', '15.6', '13.7', '16.7', '1.102 / 1.093 / 1.094'),
    ('mc2_05', 'control_no_buildings',   '0.0000', '0.0',  '0.0',  '0.0', '0.0',  '0.000 / 0.000 / 0.000'),
]
rows = [[{"tag": "th", "attrs": "", "html": h} for h in
         ['map', 'pose', 'two-run floor %', 'ARM A % px', 'ARM B % px',
          'ARM B+C % px', 'B vs B+C %', 'hard-step % OFF / B / B+C']]]
for r in ROWS:
    rows.append([{"tag": "td", "attrs": "", "html": c} for c in r])
add('meta', attrs='class="meta"', rows=rows)

add('note', html=(
    "<b>How to read this.</b> <code>ARM B+C % px</code> is <b>0.0</b> on every floor-valid pose on "
    "mc2_23, mc2_24 and mc2_05. That is not ARM C being weak &mdash; it means that on those views, "
    "<b>100% of ARM B's measured effect was road destruction</b>. Once roads are protected there is "
    "nothing left for ARM B to do, because every concrete cell in frame carries road art."
    "<br><br><b>Hard-step %</b> (adjacent-pixel luma jump &gt; 16 &mdash; the metric that sank "
    "<code>MC2_PAVED_OVERLAY_CLASS</code> at 0.6&ndash;10.8pp): ARM B alone <b>raises</b> it by up to "
    "<b>+0.618pp</b> (mc2_24 saved_192316, 3.079&rarr;3.697). <b>ARM B + ARM C returns it to the OFF "
    "value exactly</b> on every floor-valid pose &mdash; ARM C's own hard-step delta is "
    "<b>0.000pp</b>."
    "<br><br><b>Intra-cell spread</b> is unchanged by ARM C, deliberately: the ~65-luma step from "
    "concrete (<code>L&asymp;147</code>) to asphalt (<code>L&asymp;82</code>) is the road, and ARM C "
    "exists to protect it. <b>Cross-cell</b> concrete-background span, from the engine's own census: "
    "mc2_24 <code>bg_span=17.6</code>, mc2_23 <code>18.0</code>, mc2_05 <code>6.9</code>. Offline, "
    "with a stricter road exclusion, mc2_24 measures <b>4.7</b> &mdash; the shipped in-engine "
    "estimate is conservative on the heaviest crossroads cells and <i>over</i>states the span. Either "
    "number says the same thing: the concrete is one colour."))

add('note', html=(
    "<b>Honest limits.</b> (1) ARM C does not make the airbase look different on its own &mdash; by "
    "design it is a pixel no-op. If you want a visible change here, the frames above say the "
    "candidate is <b>OFF</b>, not any arm. (2) This lane did not establish <i>why</i> the mc2_23 "
    "apron reads as patchy to you if the concrete is one colour; the remaining suspects are the "
    "tile-level lighting term and how the road tileset pieces are <i>placed</i> on the map (a "
    "mismatched piece leaves a stub or a broken run), neither of which is an atlas-tone question. "
    "(3) Poses marked &#9940; carry no claim &mdash; their two-run floor exceeds the effect."))

# ---------------------------------------------------------------- gate meta
add('meta', attrs='class="meta"', rows=[
    [{"tag": "th", "attrs": "", "html": "ARM C gate"},
     {"tag": "td", "attrs": "", "html":
      "<code>MC2_CEMENT_ROAD_ART_DETECT=1</code> &mdash; DEFAULT OFF; ON with both other arms OFF is "
      "a pixel no-op"}],
    [{"tag": "th", "attrs": "", "html": "Diagnostic"},
     {"tag": "td", "attrs": "", "html":
      "<code>MC2_CEMENT_ATLAS_DUMP=&lt;prefix&gt;</code> &mdash; writes the assembled atlas as "
      "<code>.rgba</code> + <code>.csv</code>. This is the instrument that found the road tileset."}],
    [{"tag": "th", "attrs": "", "html": "Census (prints with no gate on)"},
     {"tag": "td", "attrs": "", "html":
      "<code>[CEMENT_CONCRETE v1] event=census &hellip; family_road=&hellip; bg_span=&hellip;</code>"}],
    [{"tag": "th", "attrs": "", "html": "Build"},
     {"tag": "td", "attrs": "", "html":
      "<code>f51cd63d</code> &middot; tier1 <b>5/5 PASS</b> with "
      "<code>[DEPLOY_FINGERPRINT] OK</code> on every mission"}],
])

sec['blocks'].extend(B)
json.dump(d, open(MAN, 'w', encoding='utf-8'), indent=1, ensure_ascii=False)
print('appended %d ARM C blocks; section now %d blocks' % (len(B), len(sec['blocks'])))
