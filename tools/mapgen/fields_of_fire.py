"""Offline battery evaluation on native weapon annuli and MOVE LOS cells.

Responsibility sectors are author intent, never a restriction on 360-degree aim.
Static engagement potential is separate from observer-dependent indirect fire.
"""
from __future__ import annotations

from collections import Counter, deque
import csv
from functools import lru_cache
import hashlib
import io
import json
import math
from pathlib import Path
import re

import numpy as np

from tools.mission_codec import Install, fit_sections, move_fields, packet
from tools.mapgen.common import CELL_WU as C, mask_of
from tools.mapgen.footprints import tgl_points, ase_points

STOCK = Path('A:/Games/mc2-opengl/mc2-win64-v0.4')
POLICY = 'turret-fof-1'
DIFFICULTIES = {'easy': (2, 2, 35, .70, .25, 2*C),
                'standard': (3, 4, 50, .80, .40, 2*C),
                'hard': (5, 6, 70, .90, .55, C)}
FLAWS = ('flank_route', 'lrm_pocket', 'exposed_generator', 'tower_sightline')
# These are declared representative profiles, not guessed stock object heights.
TARGETS = {'vehicle': (20., 12.), 'mech': (50., 20.)}


class Assets:
    def __init__(self, install):
        self.install = install
        self.objects = install.read('data/objects/object2.pak')[0]
        gamesys = install.read('data/missions/gamesys.fit')[0]
        self.gamesys = fit_sections(gamesys)
        rows = csv.reader(io.StringIO(install.read('data/objects/compbas.csv')[0].decode('latin1')))
        self.components = {int(r[0]): r for r in rows if r and r[0].isdigit()}
        self.ranges = {k: [float(x)/.2 for x in str(v).split(',')]
                       for k, v in self.gamesys['weaponranges'].items() if k in ('short', 'medium', 'long')}
        indirect = re.search(rb'IndirectFireWeaponComponentIndex\s*=\s*([-\d,\s]+)', gamesys, re.I)
        self.indirect = {int(v) for v in re.findall(rb'-?\d+', indirect[1]) if int(v) >= 0} if indirect else set()
        self.tree_limit = int(self.gamesys['general']['maxtreelosblock'])
        visual = re.search(rb'VisualRangeTable\s*=\s*([\d,\s]+)', gamesys, re.I)
        self.visual_ranges = [int(v)*25*5.01 for v in re.findall(rb'\d+', visual[1])] if visual else []
        if len(self.visual_ranges) != 256:
            raise ValueError('missing 256-entry visual-range table')

    @lru_cache(maxsize=None)
    def shape(self, appearance):
        fields = fit_sections(self.install.read(f'data/tgl/{appearance}.ini')[0]).get('tgldata', {})
        name = str(fields.get('filename', fields.get('filename0', appearance))).replace('\\', '/').rsplit('/', 1)[-1]
        name = re.sub(r'\.(ase|tgl)$', '', name, flags=re.I)
        nodes = {}
        try:
            points = tgl_points(self.install.read(f'data/tgl/{name}.tgl')[0], nodes)
        except FileNotFoundError:
            points = ase_points(self.install.read(f'data/tgl/{name}.ase')[0], nodes)
        return points, nodes

    @lru_cache(maxsize=None)
    def turret(self, type_id):
        fit = fit_sections(packet(self.objects, type_id))
        data = fit['turretdata']
        _, nodes = self.shape(fit['objecttype']['appearancename'])
        heights = [p[2] for n, p in nodes.items() if n.startswith('weapon')]
        if not heights:
            raise ValueError(f'type {type_id}: missing weapon nodes')
        weapons = []
        for name in ('weapontype', 'weapontype1', 'weapontype2', 'weapontype3'):
            component = data.get(name, -1)
            if component < 0:
                continue
            row = self.components[component]
            lo, hi = self.ranges[row[10].lower()]
            weapons.append({'component': component, 'min_wu': lo, 'max_wu': hi,
                            'indirect': component in self.indirect,
                            'damage': float(row[7]), 'recycle_s': float(row[4])})
        return {'weapons': weapons, 'acquisition_wu': data['attackradius'],
                'muzzle_wu': min(heights), 'yaw_deg_s': data['maxturretyawrate'],
                'pilot_skill': data['pilotskill'], 'visual_ranges_wu': self.visual_ranges}


@lru_cache(maxsize=1)
def stock_assets():
    return Assets(Install(STOCK))


class MoveLOS:
    """Team::lineOfSight cell walk: 1.5 samples/cell, extents, forest counter.

    Local heights are 4-bit levels in 4*5.01 wu; vertex baking uses .2/4.
    Terrain uses MapData's alternating triangles rather than bilinear heights.
    """
    def __init__(self, vertices, cells, tree_limit=6, water_elevation=None):
        self.vertices = np.asarray(vertices, dtype=float)
        self.cells = np.asarray(cells, dtype='<u4')
        self.side = len(self.cells)
        self.fields = move_fields(self.cells)
        self.tree_limit = tree_limit
        self.water_elevation = water_elevation

    def visual_range(self, position, table):
        base = max(float(self.vertices.min()), self.water_elevation if self.water_elevation is not None else float(self.vertices.min()))
        interval = (float(self.vertices.max())-base)/256
        level = int((position[2]-base)/interval) if interval > 1e-6 else 0
        return table[min(255, max(0, level))]

    def cell(self, xy):
        xy = np.asarray(xy)
        return np.floor(np.stack((self.side/2-xy[..., 1]/C, xy[..., 0]/C+self.side/2), axis=-1)).astype(int)

    def world(self, cells):
        cells = np.asarray(cells)
        return np.stack(((cells[..., 1]+.5-self.side/2)*C,
                         (self.side/2-cells[..., 0]-.5)*C), axis=-1)

    def elevation(self, xy):
        xy = np.asarray(xy)
        col, row = xy[..., 0]/128+self.side/6, self.side/6-xy[..., 1]/128
        r = np.clip(np.floor(row).astype(int), 0, len(self.vertices)-2)
        c = np.clip(np.floor(col).astype(int), 0, len(self.vertices)-2)
        u, v = np.clip(col-c, 0, 1), np.clip(row-r, 0, 1)
        a, b, d, e = self.vertices[r, c], self.vertices[r, c+1], self.vertices[r+1, c], self.vertices[r+1, c+1]
        diagonal = np.where(u > v, a+(b-a)*u+(e-b)*v, a+(e-d)*u+(d-a)*v)
        anti = np.where(u+v < 1, a+(b-a)*u+(d-a)*v, e+(d-e)*(1-u)+(b-e)*(1-v))
        return np.where((r+c) % 2 == 0, diagonal, anti)

    def rays(self, source, targets, target_height=20., target_extent=12., start_extent=0.):
        """Return visible flags and first blocker category for a batch of rays."""
        targets = np.atleast_2d(np.asarray(targets, dtype=float))
        source = np.broadcast_to(np.asarray(source, dtype=float), (len(targets), 3))
        sc, tc = self.cell(source[:, :2]), self.cell(targets)
        start, end = self.world(sc), self.world(tc)
        sh = self.elevation(start)+source[:, 2]-self.elevation(source[:, :2])
        eh = self.elevation(end)+target_height
        length = np.linalg.norm(tc-sc, axis=1)*1.5
        iterations = np.maximum(0, (length-.5).astype(int))
        step = (end-start)/np.maximum(length[:, None], 1e-6)
        dz = (eh-sh)/(length+1.5)
        visible, trees = np.ones(len(targets), bool), np.zeros(len(targets), int)
        reason = np.full(len(targets), '', dtype='<U12')
        finished = np.zeros(len(targets), bool)
        for i in range(int(iterations.max(initial=0))):
            xy = start+step*i
            ground = self.elevation(xy)
            rc = self.cell(xy)
            valid = ((rc >= 0) & (rc < self.side)).all(axis=1)
            rr, cc = np.clip(rc, 0, self.side-1).T
            active = visible & ~finished & (i < iterations)
            if target_extent > 0:
                distance = np.sqrt(np.sum((end-xy)**2, axis=1)+(eh-ground)**2)
                finished |= distance <= target_extent+C/2
                active &= ~finished
            outside = np.linalg.norm(xy-start, axis=1) > start_extent if start_extent > 0 else True
            local = self.fields['local_height'][rr, cc]*20.04
            below = sh+(i+1)*dz < ground+local
            forest = self.fields['forest'][rr, cc]
            trees += active & below & outside & forest
            blocked = active & outside & (~valid | (below & (~forest | (trees >= self.tree_limit))))
            reason[blocked] = np.where(~valid[blocked], 'outside', np.where(
                forest[blocked], 'forest', np.where((sh+(i+1)*dz)[blocked] < ground[blocked], 'terrain', 'local_height')))
            visible[blocked] = False
        return visible, reason


def bake(layout, assets=None):
    """Bake native shape VERTICES into MOVE, as markLOS does; no opaque boxes."""
    assets = assets or stock_assets()
    from tools.mapgen.mission_writer import collision_grid
    cells = np.where(collision_grid(layout), 2, 0x402).astype('<u4')
    local = np.zeros_like(cells)
    for obj in layout['objects']:
        if obj['class'] not in ('building', 'wall', 'gate', 'tree', 'turret_control', 'turret_generator', 'gate_control'):
            continue
        points, _ = assets.shape(obj['appearance'])
        angle = math.radians(obj.get('rotation', 0))
        co, si = math.cos(angle), math.sin(angle)
        xy = points[:, :2] @ np.array([[co, si], [-si, co]])+obj['position_wu'][:2]
        side = len(cells)
        rc = np.floor(np.stack((side/2-xy[:, 1]/C, xy[:, 0]/C+side/2), axis=-1)).astype(int)
        valid = ((rc >= 0) & (rc < side)).all(axis=1)
        rr, cc = rc[valid].T
        heights = np.clip(np.floor(np.minimum(points[valid, 2]*.05, 15)+.5), 0, 15).astype(np.uint32)
        np.maximum.at(local, (rr, cc), heights)
        if obj['class'] == 'tree':
            cells[rr, cc] |= 0x10000000
    cells |= local << 18
    height = np.asarray(layout['terrain']['elevation_wu'])
    vertices = height[::3, ::3] if layout['terrain']['cell_step'] == 1 else height
    return MoveLOS(vertices, cells, assets.tree_limit, layout['terrain'].get('water_elevation_wu'))


def active_ids(objects, lost=()):
    by_id = {o['id']: o for o in objects}
    active, errors = set(), []
    for obj in objects:
        seen, current = set(), obj
        while current:
            ident = current['id']
            if ident in seen:
                errors.append(f'{obj["id"]}: parent cycle')
                break
            seen.add(ident)
            if ident in lost or current.get('disabled') or current.get('awake') is False or current.get('destroyed'):
                break
            parent = current.get('parent_link')
            if parent is None:
                if current.get('parent_cell') is not None:
                    errors.append(f'{ident}: unresolved parent')
                    break
                active.add(obj['id'])
                break
            if parent not in by_id:
                errors.append(f'{ident}: invalid parent {parent}')
                break
            current = by_id[parent]
            if current['team'] != obj['team']:
                errors.append(f'{obj["id"]}: parent team mismatch')
                break
    return active, sorted(set(errors))


def coverage(turrets, points, scene, assets, profile='vehicle', active=None, observers=()):
    points = np.asarray(points, dtype=float).reshape(-1, 2)
    if not turrets or not len(points):
        empty = np.zeros((len(turrets), len(points)), bool)
        return empty, empty.copy()
    height, extent = TARGETS[profile]
    direct, indirect = [], []
    for turret in turrets:
        data = turret.get('fire') or assets.turret(turret['type_id'])
        distance = np.linalg.norm(points-np.asarray(turret['position_wu'][:2]), axis=1)
        eligible, arcing = np.zeros(len(points), bool), np.zeros(len(points), bool)
        for weapon in data['weapons']:
            mask = (distance > weapon['min_wu']) & (distance <= min(weapon['max_wu'], data['acquisition_wu']))
            eligible |= mask
            if weapon['indirect']:
                arcing |= mask
        live = active is None or turret['id'] in active
        source = [*turret['position_wu'][:2], turret['position_wu'][2]+data['muzzle_wu']]
        visible = np.zeros(len(points), bool)
        if eligible.any() and live:
            visible[eligible] = scene.rays(source, points[eligible], height, extent)[0]
            if 'visual_range_wu' in data:
                visible &= distance <= data['visual_range_wu']
            if 'visual_ranges_wu' in data:
                # GameObject::lineOfSight filters 3-D pivot distance before the cell walk.
                delta_z = scene.elevation(points)-turret['position_wu'][2]
                visible &= np.hypot(distance, delta_z) <= scene.visual_range(turret['position_wu'], data['visual_ranges_wu'])
        spotted = np.zeros(len(points), bool)
        for observer in observers:
            if observer['team'] == turret['team'] and (active is None or observer['id'] in active):
                within = np.linalg.norm(points-np.asarray(observer['position_wu'][:2]), axis=1) <= observer['visual_range_wu']
                spotted[within] |= scene.rays(observer['position_wu'], points[within], height, extent)[0]
        direct.append(eligible & visible & live)
        indirect.append(arcing & ~visible & spotted & live)
    return np.asarray(direct, bool).reshape(len(turrets), -1), np.asarray(indirect, bool).reshape(len(turrets), -1)


def corridor_samples(route, step=C/4):
    """Length-weighted stations and full-width offsets, including failure edges."""
    points, weights, stations = [], [], []
    station = 0
    for a, b in zip(route['points'], route['points'][1:]):
        a, b = np.asarray(a, float), np.asarray(b, float)
        length = np.linalg.norm(b-a)
        if length == 0:
            continue
        count = max(1, math.ceil(length/step))
        normal = np.array([a[1]-b[1], b[0]-a[0]])/length
        width = route.get('width_wu', C)
        offsets = np.linspace(-width/2, width/2, max(3, math.ceil(width/(C/2))+1))
        # MOVE cells are half-open; test the corridor's interior at its two edges.
        offsets *= 1-1e-8
        for p in np.linspace(a, b, count, endpoint=False)+((b-a)/count/2):
            for offset in offsets:
                points.append(p+normal*offset)
                weights.append(length/count/len(offsets))
                stations.append(station)
            station += 1
    return np.asarray(points), np.asarray(weights), np.asarray(stations)


def reachable(scene, start, goal, width=C, forbidden=None, bounds=None):
    """Four-connected swept-footprint route; never jump an impassable flank."""
    free = scene.fields['passable'].copy()
    radius = max(0, math.ceil(width/(2*C)-.5))
    if radius:
        padded = np.pad(free, radius)
        free = np.logical_and.reduce([padded[r:r+scene.side, c:c+scene.side]
                                     for r in range(2*radius+1) for c in range(2*radius+1)])
    if forbidden is not None:
        free &= ~forbidden
    start, goal = tuple(map(int, start)), tuple(map(int, goal))
    if any(min(p) < 0 or max(p) >= scene.side or not free[p] for p in (start, goal)):
        return []
    queue, parent = deque([start]), {start: None}
    r0, c0, r1, c1 = bounds or (0, 0, scene.side, scene.side)
    while queue:
        p = queue.popleft()
        if p == goal:
            path = []
            while p is not None:
                path.append(list(p)); p = parent[p]
            return path[::-1]
        for dr, dc in ((-1, 0), (0, 1), (1, 0), (0, -1)):
            q = p[0]+dr, p[1]+dc
            if r0 <= q[0] < r1 and c0 <= q[1] < c1 and free[q] and q not in parent:
                parent[q] = p; queue.append(q)
    return []


def evaluate(layout, scene=None, assets=None):
    result = {'policy': POLICY, 'status': 'PASS', 'batteries': [], 'errors': [], 'incomplete': [], 'warnings': []}
    try:
        if assets is None and (scene is None or any(o['class'] == 'turret' and 'fire' not in o for o in layout['objects'])):
            assets = stock_assets()
        scene = scene or bake(layout, assets)
    except (ValueError, FileNotFoundError, KeyError) as error:
        return {**result, 'status': 'INCOMPLETE', 'incomplete': [str(error)]}
    objects = layout['objects']
    observers = {o['id']: o for b in layout.get('batteries', []) for o in b.get('observers', [])}
    active, errors = active_ids(objects+list(observers.values()))
    result['errors'].extend(errors)
    by_id = {o['id']: o for o in objects}
    if len(by_id) != len(objects):
        result['errors'].append('duplicate object IDs')
    by_cell = Counter(tuple(o['cell']) for o in objects)
    for obj in objects:
        if obj.get('parent_cell') is not None and by_cell[tuple(obj['parent_cell'])] != 1:
            result['errors'].append(obj['id']+': ambiguous/missing parent cell')
    plans = layout.get('batteries', [])
    if any(o['class'] == 'turret' for o in objects) and not plans:
        result['incomplete'].append('missing authored/inferred approaches')
    assigned = Counter(t for p in plans for t in p['turrets'])
    for obj in objects:
        if obj['class'] == 'turret' and assigned[obj['id']] != 1:
            result['incomplete'].append(obj['id']+': requires exactly one battery assignment')
    for plan in plans:
        turrets = [by_id[i] for i in plan['turrets'] if i in by_id]
        report = {'id': plan['id'], 'flaw': plan.get('flaw'), 'approaches': {}, 'blocked_arc': {}, 'states': {}}
        result['batteries'].append(report)
        if len(turrets) != len(plan['turrets']) or not turrets or any(t['class'] != 'turret' for t in turrets):
            result['errors'].append(plan['id']+': missing turret records'); continue
        if len({t['team'] for t in turrets}) != 1:
            result['errors'].append(plan['id']+': mixed-team battery')
        try:
            for turret in turrets:
                if 'fire' not in turret:
                    assets.turret(turret['type_id'])
                data = turret.get('fire') or assets.turret(turret['type_id'])
                if not data['weapons'] or data['yaw_deg_s'] <= 0:
                    raise ValueError(turret['id']+': missing weapons or invalid yaw rate')
        except (KeyError, ValueError, FileNotFoundError) as error:
            result['incomplete'].append(str(error)); continue
        policy = DIFFICULTIES[plan.get('difficulty', 'standard')]
        for a, turret in enumerate(turrets):
            for peer in turrets[a+1:]:
                if math.dist(turret['position_wu'][:2], peer['position_wu'][:2]) < 3*C-.01:
                    result['errors'].append(plan['id']+': turret spacing below 3C')
        routes = plan.get('approaches', [])
        if not routes:
            result['incomplete'].append(plan['id']+': missing approaches'); continue
        overlap_edges = set()
        for route in routes:
            points, weights, stations = corridor_samples(route)
            if not len(points):
                result['incomplete'].append(route['id']+': empty corridor'); continue
            rc = scene.cell(points)
            valid = ((rc >= 0) & (rc < scene.side)).all(axis=1)
            walkable = np.zeros(len(rc), bool)
            walkable[valid] = scene.fields['passable'][tuple(rc[valid].T)]
            if route.get('mandatory', True) and not walkable.all():
                result['errors'].append(route['id']+': mandatory corridor leaves passable full-width terrain')
            if not walkable.any():
                result['incomplete'].append(route['id']+': no reachable corridor samples'); continue
            profiles = {}
            for profile in TARGETS:
                direct, indirect = coverage(turrets, points, scene, assets, profile, active, plan.get('observers', ()))
                counts = (direct | indirect).sum(axis=0)
                w = weights*walkable
                covered = float(w[counts > 0].sum())
                total = float(w.sum())
                c1, o2 = covered/total, float(w[counts >= 2].sum())/covered if covered else 0.
                longest, run = 0., 0.
                for station in np.unique(stations):
                    selected = (stations == station) & walkable
                    run = run+float(weights[stations == station].sum()) if selected.any() and (counts[selected] == 0).any() else 0.
                    longest = max(longest, run)
                profiles[profile] = {'coverage': c1, 'overlap': o2, 'max_dead_run_wu': longest,
                    'direct_coverage': float(w[direct.any(axis=0)].sum())/total,
                    'conditional_indirect_coverage': float(w[indirect.any(axis=0)].sum())/total,
                    'dead_cells': rc[(counts == 0) & walkable].tolist()[:12]}
                for i in range(len(turrets)):
                    for j in range(i+1, len(turrets)):
                        if ((direct[i] | indirect[i]) & (direct[j] | indirect[j]) & walkable).any():
                            overlap_edges.add((turrets[i]['id'], turrets[j]['id']))
                if route.get('mandatory', True) and (c1 < policy[3] or o2 < policy[4] or longest > policy[5]+.01):
                    result['errors'].append(f"{plan['id']}/{route['id']}/{profile}: frontal coverage/overlap/dead-zone failure")
            report['approaches'][route['id']] = profiles
            # Attacker-to-tower visibility, using attacker LOS height and tower extent.
            tower = by_id.get(plan.get('control'))
            if tower:
                pre = np.dot(points-np.asarray(plan['entrance']), np.asarray(plan['inward'])) < 0
                sources = np.column_stack((points[pre], scene.elevation(points[pre])+TARGETS['mech'][0]))
                exposed = scene.rays(sources, np.tile(tower['position_wu'][:2], (len(sources), 1)),
                    tower.get('footprint', {}).get('height_wu', 50)/2, tower.get('extent_wu', 35))[0]
                exposure = float(np.dot(weights[pre], exposed)/weights[pre].sum()) if pre.any() else None
                profiles['tower_pre_entry_exposure'] = exposure
                if route.get('mandatory', True) and exposure and (plan.get('flaw') or {}).get('type') != 'tower_sightline':
                    result['errors'].append(plan['id']+': exposed tower on frontal approach')
        report['overlap_edges'] = sorted(overlap_edges)
        remaining = {t['id'] for t in turrets}
        report['support_components'] = []
        while remaining:
            component, pending = set(), [min(remaining)]
            while pending:
                ident = pending.pop()
                if ident in component:
                    continue
                component.add(ident)
                pending.extend(b if a == ident else a for a, b in overlap_edges if ident in (a, b))
            remaining -= component
            report['support_components'].append(sorted(component))
        if len(report['support_components']) > 1:
            result['errors'].append(plan['id']+': disconnected mutual-support graph')
        tower = by_id.get(plan.get('control'))
        if tower:
            angles = np.arange(0, 2*math.pi, math.pi/24)
            perimeter = np.asarray(tower['position_wu'][:2])+np.stack((np.cos(angles), np.sin(angles)), axis=1)*plan.get('capture_radius_wu', 2*C)
            rc = scene.cell(perimeter)
            keep = ((rc >= 0) & (rc < scene.side)).all(axis=1)
            keep[keep] &= scene.fields['passable'][tuple(rc[keep].T)]
            if keep.any():
                d, i = coverage(turrets, perimeter[keep], scene, assets, active=active)
                report['tower_protection'] = float(((d | i).sum(axis=0) >= 2).mean())
                if report['tower_protection'] < (.8 if plan.get('difficulty') == 'easy' else .9):
                    result['warnings'].append(plan['id']+': capture perimeter lacks proposed double coverage')
            else:
                report['tower_protection'] = None
                result['incomplete'].append(plan['id']+': empty capture perimeter')
        # Check a full-width uncovered path into the actual entrance, not just
        # the authored centreline. Obstacles use MOVE passability, never LOS masks.
        if plan.get('interior'):
            goal = scene.cell(plan['interior'])
            starts = [scene.cell(route['points'][0]) for route in routes if route.get('mandatory', True)]
            points = np.asarray([*plan['entrance'], *plan['interior']]).reshape(2, 2)
            lo, hi = np.min(scene.cell(points), axis=0)-22, np.max(scene.cell(points), axis=0)+23
            r0, c0 = np.maximum(lo, 0); r1, c1 = np.minimum(hi, scene.side)
            rc = np.stack(np.meshgrid(np.arange(r0, r1), np.arange(c0, c1), indexing='ij'), axis=-1)
            d, i = coverage(turrets, scene.world(rc.reshape(-1, 2)), scene, assets, active=active)
            defended = np.zeros_like(scene.cells, bool)
            defended[r0:r1, c0:c1] = (d | i).any(axis=0).reshape(rc.shape[:2])
            # A candidate must enter from the exterior of the frontage window.
            boundary = [tuple(p) for p in rc.reshape(-1, 2) if p[0] in (r0, r1-1) or p[1] in (c0, c1-1)]
            bypass = []
            # Flood from the goal once, then look for an uncovered exterior exit.
            if not defended[tuple(goal)] and scene.fields['passable'][tuple(goal)]:
                free_scene = MoveLOS(scene.vertices, np.where(defended, scene.cells & np.uint32(~0x400 & 0xffffffff), scene.cells))
                # Connected components avoid one BFS per boundary cell.
                from tools.mapgen.common import components
                free = free_scene.fields['passable'][r0:r1, c0:c1]
                for component in components(free):
                    cells = {(int(r)+r0, int(c)+c0) for r, c in component}
                    if tuple(goal) in cells and cells.intersection(boundary):
                        start = min(cells.intersection(boundary))
                        bypass = reachable(free_scene, start, goal, bounds=(r0, c0, r1, c1))
                        break
            report['uncovered_entry_path'] = bypass
            if bypass and (plan.get('flaw') or {}).get('type') not in ('flank_route', 'lrm_pocket'):
                result['errors'].append(plan['id']+': unplanned uncovered entry path')
        report['traverse_worst_idle_s'] = {t['id']: 170/(t.get('fire') or assets.turret(t['type_id']))['yaw_deg_s'] for t in turrets}
        # Sample eligible assigned-sector bearings at 5 degrees and C/2 radial spacing.
        for turret in turrets:
            data = turret.get('fire') or assets.turret(turret['type_id'])
            aim = np.mean([route['points'][0] for route in routes if route.get('mandatory', True)], axis=0)
            direction = aim-np.asarray(turret['position_wu'][:2])
            bearing = math.degrees(math.atan2(direction[1], direction[0]))
            angles = np.radians(np.arange(bearing-plan.get('sector_half_deg', 20), bearing+plan.get('sector_half_deg', 20)+.01, 5))
            radial = np.arange(C/2, min(data['acquisition_wu'], max(w['max_wu'] for w in data['weapons']))+1, C/2)
            points = np.asarray(turret['position_wu'][:2])+np.stack((np.cos(angles), np.sin(angles)), axis=1)[:, None, :]*radial[None, :, None]
            points = points.reshape(-1, 2)
            rc = scene.cell(points)
            valid = ((rc >= 0) & (rc < scene.side)).all(axis=1)
            keep = np.zeros(len(rc), bool)
            keep[valid] = scene.fields['passable'][tuple(rc[valid].T)]
            plausible = np.zeros(len(points), bool)
            for route in routes:
                for a, b in zip(route['points'], route['points'][1:]):
                    a, b = np.asarray(a), np.asarray(b)
                    delta = b-a
                    if np.dot(delta, delta) == 0:
                        continue
                    fraction = (points-a) @ delta / np.dot(delta, delta)
                    plausible |= ((fraction >= 0) & (fraction <= 1) &
                        (np.linalg.norm(points-a-fraction[:, None]*delta, axis=1) <= route.get('width_wu', C)/2))
            keep &= plausible
            points = points[keep]
            distance = np.linalg.norm(points-turret['position_wu'][:2], axis=1)
            eligible = np.zeros(len(points), bool)
            for w in data['weapons']:
                eligible |= (distance > w['min_wu']) & (distance <= min(w['max_wu'], data['acquisition_wu']))
            source = [*turret['position_wu'][:2], turret['position_wu'][2]+data['muzzle_wu']]
            visible, reasons = scene.rays(source, points[eligible])
            blocked = float((~visible).mean()) if len(visible) else None
            report['blocked_arc'][turret['id']] = {'fraction': blocked, 'blockers': dict(Counter(reasons[~visible]))}
            if blocked is None:
                result['incomplete'].append(turret['id']+': empty assigned sector')
            elif blocked > .35:
                result['errors'].append(turret['id']+': assigned sector blocked >35%')
        for state, lost in (('control_lost', [plan.get('control')]), ('generator_lost', [plan.get('generator')]),
                            ('observer_lost', [o['id'] for o in plan.get('observers', [])])):
            surviving, _ = active_ids(objects+list(observers.values()), lost)
            report['states'][state] = {'active_turrets': [t['id'] for t in turrets if t['id'] in surviving],
                                      'lost_observers': [o['id'] for o in plan.get('observers', []) if o['id'] not in surviving]}
            report['states'][state]['approaches'] = {}
            for route in routes:
                points, weights, _ = corridor_samples(route)
                if not len(points):
                    continue
                rc = scene.cell(points)
                valid = ((rc >= 0) & (rc < scene.side)).all(axis=1)
                keep = np.zeros(len(rc), bool)
                keep[valid] = scene.fields['passable'][tuple(rc[valid].T)]
                weights = weights*keep
                if not weights.sum():
                    continue
                d, i = coverage(turrets, points, scene, assets, active=surviving, observers=plan.get('observers', ()))
                report['states'][state]['approaches'][route['id']] = {
                    'coverage': float(weights[(d | i).any(axis=0)].sum()/weights.sum()),
                    'conditional_indirect_coverage': float(weights[i.any(axis=0)].sum()/weights.sum())}
        flaw = plan.get('flaw')
        if flaw:
            if flaw.get('type') not in FLAWS:
                result['errors'].append(plan['id']+': unknown flaw type')
                continue
            start, goal = scene.cell(flaw['start']), scene.cell(flaw['target'])
            path = reachable(scene, start, goal, flaw.get('width_wu', C))
            report['flaw_reachable'] = bool(path)
            report['flaw_path'] = path
            if not path:
                result['errors'].append(plan['id']+': declared flaw unreachable')
            elif flaw['type'] in ('flank_route', 'lrm_pocket'):
                d, i = coverage(turrets, scene.world(np.asarray(path)), scene, assets, active=active,
                                observers=plan.get('observers', ()))
                counts = (d | i).sum(axis=0)
                report['flaw_path_coverage'] = float((counts > 0).mean())
                report['flaw_target_firing_turrets'] = int(counts[-1])
                if counts[-1] != 0:
                    result['errors'].append(plan['id']+': declared flank/pocket target is covered')
            elif flaw['type'] == 'exposed_generator':
                generator = by_id.get(plan.get('generator'))
                if generator is None:
                    result['incomplete'].append(plan['id']+': flaw generator missing')
                else:
                    xy = flaw['target']
                    source = [*xy, float(scene.elevation(xy))+TARGETS['mech'][0]]
                    visible = scene.rays(source, [generator['position_wu'][:2]],
                        generator.get('footprint', {}).get('height_wu', 50)/2, 45)[0][0]
                    report['flaw_target_visible'] = bool(visible)
                    if not visible:
                        result['errors'].append(plan['id']+': declared generator flaw is occluded')
        else:
            report['flaw_reachable'] = None
    fire = {}
    for obj in objects:
        if obj['class'] == 'turret':
            try:
                fire[obj['id']] = obj.get('fire') or assets.turret(obj['type_id'])
            except (KeyError, ValueError, FileNotFoundError) as error:
                result['incomplete'].append(str(error))
    result['inputs_sha256'] = hashlib.sha256(scene.cells.tobytes()+scene.vertices.tobytes()+
        json.dumps([objects, plans, fire, scene.tree_limit, scene.water_elevation, TARGETS, DIFFICULTIES], sort_keys=True).encode()).hexdigest()
    result['status'] = 'FAIL' if result['errors'] else 'INCOMPLETE' if result['incomplete'] else 'WARN' if result['warnings'] or any(
        v['fraction'] is not None and v['fraction'] > .20 for b in result['batteries'] for v in b['blocked_arc'].values()) else 'PASS'
    return result


def overlay(layout, path, scene=None, assets=None):
    """Proof image: annuli, clipped direct fire, approach widths, declared flaw."""
    from PIL import Image, ImageDraw
    assets = assets or stock_assets()
    scene = scene or bake(layout, assets)
    plans = layout.get('batteries', [])
    if not plans:
        raise ValueError('no batteries to draw')
    plan = plans[0]
    by_id = {o['id']: o for o in layout['objects']}
    turrets = [by_id[i] for i in plan['turrets']]
    centre = scene.cell(plan['entrance'])
    r0, c0 = np.maximum(centre-25, 0)
    r1, c1 = np.minimum(centre+26, scene.side)
    rc = np.stack(np.meshgrid(np.arange(r0, r1), np.arange(c0, c1), indexing='ij'), axis=-1)
    points = scene.world(rc.reshape(-1, 2))
    direct, indirect = coverage(turrets, points, scene, assets)
    counts = direct.sum(axis=0).reshape(rc.shape[:2])
    rgb = np.zeros((*counts.shape, 3), np.uint8)+[67, 75, 57]
    rgb[counts == 1] = [105, 145, 65]
    rgb[counts >= 2] = [55, 175, 140]
    rgb[~scene.fields['passable'][r0:r1, c0:c1]] //= 2
    image = Image.fromarray(rgb.astype(np.uint8)).resize(((c1-c0)*12, (r1-r0)*12))
    draw = ImageDraw.Draw(image)
    def pixel(p):
        return ((p[0]/C+scene.side/2-c0)*12, (scene.side/2-p[1]/C-r0)*12)
    for turret in turrets:
        data = turret.get('fire') or assets.turret(turret['type_id'])
        x, y = pixel(turret['position_wu'])
        for w in data['weapons']:
            for radius in (w['min_wu'], min(w['max_wu'], data['acquisition_wu'])):
                radius = radius/C*12
                draw.ellipse((x-radius, y-radius, x+radius, y+radius), outline='#f7d879', width=1)
        draw.ellipse((x-4, y-4, x+4, y+4), fill='white')
    for route in plan['approaches']:
        draw.line([pixel(p) for p in route['points']], fill='#ee9857', width=3)
    if plan.get('flaw'):
        x, y = pixel(plan['flaw']['target'])
        draw.ellipse((x-8, y-8, x+8, y+8), outline='#ff55dd', width=3)
        draw.text((8, 26), 'FLAW: '+plan['flaw']['type'], fill='#ff88ee')
    draw.text((8, 8), plan['difficulty']+' | annuli yellow | direct green | overlap teal', fill='white')
    image.save(path)


def audit_stock(install, missions):
    """Read-only stock screen. Inferred roads/profiles never certify stock balance."""
    from tools.mission_codec import catalog, overlay_catalog, decode_move
    from tools.mapgen.extract import extract_mission, VERTEX
    assets = Assets(install)
    types, provenance = catalog(install)
    overlays, overlay_source = overlay_catalog(install)
    reports = {}
    for name in missions:
        layout = extract_mission(install, name, types, provenance, overlays, overlay_source, 'stock')
        raw = install.read('data/missions/'+name+'.pak')[0]
        cells = decode_move([packet(raw, i) for i in range(4, 8)])[3]
        vertices = np.frombuffer(packet(raw, 0), VERTEX)['height'].reshape(len(cells)//3, -1)
        scene = MoveLOS(vertices, cells, assets.tree_limit, layout['terrain'].get('water_elevation_wu'))
        turrets = [o for o in layout['objects'] if o['class'] == 'turret']
        by_id = {o['id']: o for o in layout['objects']}
        groups = {}
        for turret in turrets:
            try:
                if not assets.turret(turret['type_id'])['weapons']:
                    continue
            except (ValueError, KeyError, FileNotFoundError):
                continue
            groups.setdefault(turret['parent_link'] or turret['id'], []).append(turret)
        layout['batteries'] = []
        roads = np.argwhere(((cells & 0x02000000) != 0) & scene.fields['passable'])
        for ident, members in groups.items():
            centre = np.mean([o['position_wu'][:2] for o in members], axis=0)
            distance = np.linalg.norm(scene.world(roads)-centre, axis=1)
            eligible = roads[(distance >= 3*C) & (distance <= 12*C)]
            if len(eligible) < 2:
                continue
            goal = min(eligible, key=lambda p: (np.linalg.norm(scene.world(p)-centre), tuple(p)))
            start = max(eligible, key=lambda p: (np.linalg.norm(scene.world(p)-centre), tuple(p)))
            path = reachable(scene, start, goal)
            if len(path) < 2:
                continue
            # Keep all bends so the route stays on actual MOVE-passable cells.
            points = scene.world(path).tolist()
            inward = centre-np.array(points[0]); inward /= np.linalg.norm(inward)
            control = by_id.get(members[0].get('parent_link'))
            layout['batteries'].append({'id': ident, 'difficulty': 'standard',
                'turrets': [o['id'] for o in members], 'control': control['id'] if control else None,
                'generator': control.get('parent_link') if control else None,
                'entrance': centre.tolist(), 'inward': inward.tolist(),
                'sector_half_deg': 30, 'flaw': None,
                'approaches': [{'id': 'inferred_road', 'mandatory': True, 'points': points, 'width_wu': C}]})
        report = evaluate(layout, scene, assets)
        report.update(turret_records=len(turrets), evaluated_turrets=sum(len(b['turrets']) for b in layout['batteries']),
            source_install=str(install.root),
            route_provenance='inferred MOVE roads; vehicle/mech reference profiles; no observer plan',
            stock_disposition='read-only; preserve authored flaws, including mc2_20 LRM pocket')
        if turrets:
            report['incomplete'].append('stock routes and target/observer profiles require author confirmation')
            if not report['errors']:
                report['status'] = 'INCOMPLETE'
        reports[name] = report
    return reports


def audit_source(root):
    """Accept an install or the owner's extracted stock data directory, read-only."""
    install = Install(root)
    if (install.root/'objects/object2.pak').is_file():
        for folder in ('missions', 'objects', 'art', 'textures', 'tgl'):
            for path in sorted((install.root/folder).rglob('*')):
                if path.is_file() and path.suffix.lower() in ('.fit', '.pak', '.csv', '.ini', '.tgl', '.ase'):
                    install.files['data/'+path.relative_to(install.root).as_posix().lower()] = (path,)
    return install


def main(argv=None):
    import argparse
    from tools.mapgen.common import load_json, output_path, write_json
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('layout', type=Path, nargs='?')
    parser.add_argument('--stock', action='store_true', help='read-only stock mission audit')
    parser.add_argument('--install', type=Path, default=STOCK)
    parser.add_argument('--missions', nargs='+', default=[f'mc2_{i:02}' for i in range(1, 25)])
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args(argv)
    if args.stock == bool(args.layout):
        parser.error('choose a layout or --stock')
    source = audit_source(args.install)
    report = audit_stock(source, args.missions) if args.stock else evaluate(
        load_json(args.layout), assets=Assets(source))
    write_json(output_path(args.out, [args.install]), report, pretty=True)
    if args.stock:
        for name, result in report.items():
            print(f"{name}: {result['status']} turrets={result['turret_records']} "
                  f"evaluated={result['evaluated_turrets']} errors={len(result['errors'])} "
                  f"incomplete={len(result['incomplete'])}")
        return 0  # An audit records findings; it does not approve stock maps.
    print(f"{report['status']}: {len(report['errors'])} errors, {len(report['incomplete'])} incomplete")
    return int(report['status'] in ('FAIL', 'INCOMPLETE'))


if __name__ == '__main__':
    raise SystemExit(main())
