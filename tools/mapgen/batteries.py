"""Small deterministic firing-line plans for the existing road-first parcels."""
import math

import numpy as np

from tools.mapgen.common import CELL_WU as C
from tools.mapgen.fields_of_fire import DIFFICULTIES, FLAWS
from tools.mapgen.raster import world_position


def plan(layout, difficulty='standard', flaw=None, flaw_type='exposed_generator'):
    if difficulty not in DIFFICULTIES or flaw_type not in FLAWS:
        raise ValueError('unknown turret difficulty or flaw type')
    enabled = difficulty != 'hard' if flaw is None else flaw
    layout['source'].update(difficulty=difficulty, flaw_enabled=enabled, flaw_type=flaw_type)
    objects = layout['objects']
    plans = []
    for base in layout['bases']:
        members = [o for o in objects if o.get('base_id') == base['id']]
        old = [o for o in members if o['class'] == 'turret']
        if not old:
            continue
        control = next(o for o in members if o['class'] == 'turret_control')
        generator = next(o for o in members if o['class'] == 'turret_generator')
        r0, c0, r1, c1 = base['bounds']
        entrance = np.array(world_position(layout, {'cell': base['entry_cell']}))
        # Parcel edge, not the possibly bent road-anchor vector, defines inward.
        r, c = base['entry_cell']
        edge = min(((abs(r-r0), (0, -1)), (abs(r-r1+1), (0, 1)),
                    (abs(c-c0), (1, 0)), (abs(c-c1+1), (-1, 0))))[1]
        inward = np.array(edge, float)
        lateral = np.array([-inward[1], inward[0]])
        centre = np.array(world_position(layout, {'cell': [(r0+r1)//2, (c0+c1)//2]}))
        sign = 1 if np.dot(centre-entrance, lateral) >= 0 else -1
        count = DIFFICULTIES[difficulty][0]+(layout['source']['seed'] % 2 if difficulty != 'easy' else 0)
        def position(depth, across):
            return (entrance+C*(inward*depth+lateral*across)).tolist()
        def target(obj, depth, across, type_id=None):
            obj['battery_target'] = position(depth, across)
            if type_id is not None:
                obj['battery_type'] = type_id
            return obj
        target(control, 7.5, sign*5)
        target(generator, -.7 if enabled and flaw_type == 'exposed_generator' else 5.5, sign*4.5)
        screen = {**control, 'id': base['id']+'_tower_screen', 'class': 'building',
                  'role': 'military', 'parent_link': None, 'parent_cell': None, 'parent_id_raw': -1}
        # A native tall structure screens the control; no invented LOS rectangle.
        target(screen, 3.8, sign*3.5, 565)
        if enabled and flaw_type == 'tower_sightline':
            target(control, 5.5, -sign*3.5)
        objects[:] = [o for o in objects if o not in old]
        objects.append(screen)
        turrets = []
        for i in range(count):
            request = {**old[0], 'id': base['id']+f'_turret_{i}', 'pilot_skill': DIFFICULTIES[difficulty][2]}
            # Close-range pair prevents a frontal inner-annulus breakthrough.
            type_id = 175 if i < 2 else 173 if difficulty == 'standard' else 171
            depth = -2.7
            across = (-1 if i % 2 == 0 else 1)*(1.8+(i//2)*3.6)
            if enabled and flaw_type == 'lrm_pocket':
                type_id, depth = 170, -.7
            target(request, depth, across, type_id)
            objects.append(request); turrets.append(request['id'])
        approach = {'id': base['id']+'_front', 'mandatory': True, 'width_wu': C,
                    'points': [position(-7.5, 0), position(-.5, 0)]}
        if enabled and flaw_type == 'lrm_pocket':
            approach['points'] = [position(-18, 0), position(-8, 0)]
            target(control, 2, sign*3)
        battery = {'id': base['id'], 'difficulty': difficulty, 'turrets': turrets,
                   'control': control['id'], 'generator': generator['id'],
                   'entrance': position(-3.5, 0), 'inward': inward.tolist(),
                   'interior': position(-.5, 0),
                   'sector_half_deg': 10, 'approaches': [approach], 'flaw': None}
        if enabled:
            # Flaws are reachable tactical opportunities; frontal checks always apply.
            depth, across = (-.7, sign*3)
            if flaw_type == 'lrm_pocket':
                depth, across = -.7, 0
            elif flaw_type == 'tower_sightline':
                depth, across = -1, 0
            elif flaw_type == 'flank_route':
                depth, across = 11, -sign*4
            battery['flaw'] = {'type': flaw_type, 'start': approach['points'][0],
                               'target': position(depth, across), 'width_wu': C}
        plans.append(battery)
    layout['batteries'] = plans
    return layout


def finish(layout, scene, assets):
    """Resolve a free attack/capture position beside the declared flaw object."""
    from tools.mapgen.fields_of_fire import reachable
    by_id = {o['id']: o for o in layout['objects']}
    for battery in layout.get('batteries', []):
        flaw = battery.get('flaw')
        if not flaw:
            continue
        if flaw['type'] == 'exposed_generator':
            obj = by_id[battery['generator']]
            centre = scene.cell(obj['position_wu'][:2])
            choices = [np.array([r, c]) for r in range(centre[0]-4, centre[0]+5)
                       for c in range(centre[1]-4, centre[1]+5)
                       if 0 <= r < scene.side and 0 <= c < scene.side and scene.fields['passable'][r, c]]
            for cell in sorted(choices, key=lambda p: (np.linalg.norm(scene.world(p)-battery['entrance']), tuple(p))):
                xy = scene.world(cell)
                visible = scene.rays([*xy, float(scene.elevation(xy))+50], [obj['position_wu'][:2]],
                                     obj['footprint']['height_wu']/2, 45)[0][0]
                if visible and reachable(scene, scene.cell(flaw['start']), cell):
                    flaw['target'] = xy.tolist()
                    break


def candidates(request):
    """Bounded deterministic translations; no random parent-radius sampling."""
    x, y = request['battery_target']
    for radius in ((0, C/4, C/2, C, 1.5*C) if request['class'] == 'turret' else
                   (0, C/4, C/2, C, 1.5*C, 2*C, 3*C, 4*C, 5*C)):
        for angle in range(0, 360, 45):
            if radius == 0 and angle:
                continue
            yield (x+radius*math.cos(math.radians(angle)), y+radius*math.sin(math.radians(angle)))


def in_firing_lane(layout, obj):
    if obj['class'] not in ('building', 'tree') or obj['id'].endswith('_tower_screen'):
        return False
    x = np.asarray(obj['position_wu'][:2])
    half = max(obj['footprint']['width_wu'], obj['footprint']['depth_wu'])/2
    for battery in layout.get('batteries', []):
        for route in battery['approaches']:
            for a, b in zip(route['points'], route['points'][1:]):
                a, b = np.array(a), np.array(b)
                t = np.clip(np.dot(x-a, b-a)/np.dot(b-a, b-a), 0, 1)
                if np.linalg.norm(x-a-(b-a)*t) < half+route['width_wu']/2+C:
                    return True
    return False
