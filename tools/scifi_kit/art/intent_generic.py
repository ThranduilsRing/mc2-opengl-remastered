"""Walls/gates completion pass, generic group: the loose stock INIs whose shapes never shipped in the base FSTs (cut
FASA content; MC2X-CVE-G carries the TGLs, decoded with tgl_decode.py --install A:/Games/mc2-opengl/MC2X-CVE-G).

Each piece below has the same massing as an approved Steiner piece (decoded and compared: the medium gates are
vertex-identical to s_mediumgate and share its open animation byte for byte; the walls and posts are low-poly copies of
the s_mediumwall / s_heavywall / s_mediumpost / s_heavypost taper and terminal pier), so they build on the approved
code via the spec's 'base' and differ in finish: neutral stencil plates on the gate leaves instead of the House fist,
and the stock texture's character in the palette (block masonry, riveted rust plate, scorched dark, whitewash).
"""
import batch3
import drafts
import intent_military as M

# stock root offsets (xc/4, -zc/4, ymin/4 of the raw TGL bounds, the counts.ORIGINS convention), from the decoded JSON
import json
import counts
from pathlib import Path

STOCK = Path('G:/mc2-assets/out/scifi-kit/round2/stock')   # analyze.STOCK (analyze needs PIL, absent in Blender)


def _origin(name):
    b = json.loads((STOCK / f'{name}.json').read_text())['bounds']
    lo, hi = b['min'], b['max']
    return ((lo[0] + hi[0]) / 8, -(lo[2] + hi[2]) / 8, lo[1] / 4)


def legacy_wall(k):
    """The approved legacy wall (batch3.wall, fitted to the stock size like drafts.build) under the base name."""
    batch3.wall(k)
    drafts.fit_stock(k, k.name)
    import intent
    intent.count(k, 'panels_sections', sum(1 for pc in k.pieces if 'course' in pc))   # span x cast-course units
    intent.count(k, 'legs_supports')                                                   # the +Y terminal pier


_GATE = ('{} on the approved s_mediumgate code (vertex-identical stock; same S_MediumGateDoor node, pivot and open animation): '
         'end piers and cast runs as the walls, gate-tower piers with slit windows, lamps and beacons, two armoured leaves that sink '
         'through the kerb slot; {} on both leaf faces and the tower piers in place of the House fist.')
_WALL = ('{} on the approved {} code (the stock is a low-poly copy of its taper and +Y terminal pier): footing, cast courses, '
         'face fields, coping and pier exactly as approved, finished as {}.')
_POST = ('{} on the approved {} pier (stock taper and plan), with a red beacon on the cap, finished as {}.')

SPECS = {
    'mediumgate': dict(fn=M.wall_gate, base='s_mediumgate', emblem='n1', palette='intent_wall_n1', cls='large',
                       counts=dict(doors=2, legs_supports=2, panels_sections=2),
                       intent=_GATE.format('Generic medium gate (stock Gate_1)', 'a neutral octagon stencil plate with hazard corners')),
    'mediumgate2': dict(fn=M.wall_gate, base='s_mediumgate', emblem='n2', palette='intent_wall_n2', cls='large',
                        counts=dict(doors=2, legs_supports=2, panels_sections=2),
                        intent=_GATE.format('Generic medium gate 2 (stock Gate_2, rust leaves)', 'a plain rust boss plate with four bolts')),
    'mediumgate3': dict(fn=M.wall_gate, base='s_mediumgate', emblem='n3', palette='intent_wall_n3', cls='large',
                        counts=dict(doors=2, legs_supports=2, panels_sections=2),
                        intent=_GATE.format('Generic medium gate 3 (stock Gate_3)', 'twin upward chevrons on a neutral plate')),
    'mediumwall2': dict(fn=legacy_wall, base='s_mediumwall', palette='intent_wall_g2', cls='large',
                        counts=dict(panels_sections=16, legs_supports=1),
                        intent=_WALL.format('Generic medium wall 2', 's_mediumwall', 'pale grey block masonry (stock medwall2)')),
    'mediumwall3': dict(fn=legacy_wall, base='s_mediumwall', palette='intent_wall_g3', cls='large',
                        counts=dict(panels_sections=16, legs_supports=1),
                        intent=_WALL.format('Generic medium wall 3', 's_mediumwall', 'grey masonry with riveted rust face plates (stock medwall3)')),
    'mediumwall5': dict(fn=legacy_wall, base='s_mediumwall', palette='intent_wall_g5', cls='large',
                        counts=dict(panels_sections=16, legs_supports=1),
                        intent=_WALL.format('Generic medium wall 5', 's_mediumwall', 'scorched dark masonry (stock medwall5)')),
    'heavywall2': dict(fn=legacy_wall, base='s_heavywall', palette='intent_wall_g2', cls='large',
                       counts=dict(panels_sections=16, legs_supports=1),
                       intent=_WALL.format('Generic heavy wall 2', 's_heavywall', 'pale grey block masonry (stock medwall2)')),
    'heavywall3': dict(fn=legacy_wall, base='s_heavywall', palette='intent_wall_g3', cls='large',
                       counts=dict(panels_sections=16, legs_supports=1),
                       intent=_WALL.format('Generic heavy wall 3', 's_heavywall', 'grey masonry with riveted rust face plates (stock medwall3)')),
    'mediumpost3': dict(fn=M.wall_post, base='s_mediumpost', palette='intent_wall_g3', cls='small', counts=dict(legs_supports=1),
                        intent=_POST.format('Generic medium post 3', 's_mediumwall', 'grey masonry and rust (stock medwall3)')),
    'mediumpost5': dict(fn=M.wall_post, base='s_mediumpost', palette='intent_wall_g5', cls='small', counts=dict(legs_supports=1),
                        intent=_POST.format('Generic medium post 5', 's_mediumwall', 'scorched dark masonry (stock medwall5)')),
    'heavypost3': dict(fn=M.wall_post, base='s_heavypost', palette='intent_wall_g3', cls='small', counts=dict(legs_supports=1),
                       intent=_POST.format('Generic heavy post 3', 's_heavywall', 'grey masonry and rust (stock medwall3)')),
    'billpost': dict(fn=M.wall_post, base='s_heavypost', palette='intent_wall_bill', cls='small', counts=dict(legs_supports=1),
                     intent=_POST.format('Bill post (stock billpost, vertex-identical to s_heavypost)', 's_heavywall', 'whitewashed cast with a pale cap')),
}

# MechCommander Omnitech gates shipped as loose INIs (texture paths D:/MCO/...): their own stock layouts, measured
# from the decoded static part and door node (stock_runs / topdown part='static'/'door'); the door node is the
# Davion D_HeavyGateDoor and the open animations are byte-identical to d_heavygateopen (hangar, 84.96 WU drop) or
# their own (tunnel, 97.96 WU drop).
_GATE_MCO = ('{}: approved cast end blocks, gate jambs with slit windows, lamps, beacons and neutral stencil plates, and two '
             'armoured leaves under the stock D_HeavyGateDoor node and pivot that sink fully through the kerb slot; {}.')
SPECS.update({
    'hangargate': dict(fn=M.wall_gate, emblem='n1', wall_family=('s', 'heavy'), palette='intent_wall_n1', cls='large',
                       counts=dict(doors=2, legs_supports=2, panels_sections=2),
                       intent=_GATE_MCO.format('Hangar gate (stock HangarGate, MCO art)', 'a 49 m opening between 8 m pilasters, '
                                               'the leaves standing in for the stock roller shutter')),
    'tunnelgate': dict(fn=M.wall_gate, emblem='n3', wall_family=('s', 'heavy'), palette='intent_wall_n3', cls='large',
                       counts=dict(doors=2, legs_supports=2),
                       intent=_GATE_MCO.format('Tunnel gate (stock TunnelGate, MCO art)', 'a tunnel portal: two 8 m jambs, the '
                                               'stock low slab beyond the -Y jamb, and a hipped riveted lintel riding on the leaves')),
    'x_tunnelgate': dict(fn=M.wall_gate, emblem='n3', wall_family=('s', 'heavy'), palette='intent_wall_g5', cls='large',
                         counts=dict(doors=2, legs_supports=2),
                         intent=_GATE_MCO.format('Tunnel gate, dark (stock X_TunnelGate, MCO art; vertex-identical to tunnelgate)',
                                                 'the tunnel portal in scorched dark finish')),
})
for _n in ('hangargate', 'tunnelgate', 'x_tunnelgate'):
    _o = _origin(_n)                            # door node pivot = the stock node in the stock frame (anim_parity):
    _piv = (-_o[0], _o[1], -_o[2])              # -origin in x and z, +origin in y (the older -origin rule flips y)
    if _n == 'hangargate':
        M.GATES[_n] = (70.96, 20.04, 20.04, 19.93, 19.37, 6.86, 8.0, 3.18)
        M.GATE_RUNS[_n] = (35.48, 28.62, 24.70, -24.71, -28.65, 8.0, 8.0)
        M.GATE_DOORS[_n] = ('D_HeavyGateDoor', _piv, 84.964 / 4, 3.18, 19.37, 6.26, .21)
    else:
        M.GATES[_n] = (28.27, 20.05, 20.05, 20.05, 22.71, 8.0, 8.0, 3.18)
        M.GATE_RUNS[_n] = (14.135, 14.13, 12.55, -11.38, -12.6, 8.0, 8.0)
        M.GATE_DOORS[_n] = ('D_HeavyGateDoor', _piv, 97.964 / 4, 3.18, 25.02, 6.26, .21)
        M.GATE_ENDPAD[(_n, -1)] = (6.26, .21)
        M.GATE_CAP[_n] = (22.71, 2.5)

for _n in SPECS:
    counts.ORIGINS[_n] = _origin(_n)
try:                    # check side only (Blender's Python lacks the survey tooling's imports)
    import cells  # noqa: E402
    for _n in SPECS:
        if 'base' in SPECS[_n]:
            cells.SURVEY_AS[_n] = SPECS[_n]['base']   # never placed: blocking cells and the joint pitch from the base piece's placements
except ImportError:
    pass
