"""Data and footprint planning shared by the CLI and Blender (no bpy dependency)."""
import json
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_OUT = Path('G:/mc2-assets/out/scifi-kit')
CATALOG = Path('E:/mc2-archive-2026-10/asset-modernize/docs/assets/catalog.json')


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def inside(point, polygon):
    x, y = point
    result = False
    for a, b in zip(polygon, polygon[1:] + polygon[:1]):
        cross = (x-a[0])*(b[1]-a[1])-(y-a[1])*(b[0]-a[0])
        if abs(cross) < 1e-5 and min(a[0], b[0])-1e-5 <= x <= max(a[0], b[0])+1e-5 and min(a[1], b[1])-1e-5 <= y <= max(a[1], b[1])+1e-5:
            return True
        if (a[1] > y) != (b[1] > y) and x < (b[0]-a[0])*(y-a[1])/(b[1]-a[1])+a[0]:
            result = not result
    return result


def crossing(a, b, c, d):
    def orient(p, q, r):
        return (q[0]-p[0])*(r[1]-p[1])-(q[1]-p[1])*(r[0]-p[0])
    return orient(a, b, c)*orient(a, b, d) < -1e-10 and orient(c, d, a)*orient(c, d, b) < -1e-10


def plan_wings(polygon, grid=3.0):
    """Conservative lot tessellation; never bridge a concavity with a bounding box."""
    if len(polygon) < 3 or not all(len(p) == 2 and all(math.isfinite(v) for v in p) for p in polygon):
        raise ValueError('Footprint needs at least three finite XY vertices')
    edges = list(zip(polygon, polygon[1:] + polygon[:1]))
    if any(crossing(a, b, c, d) for i, (a, b) in enumerate(edges) for c, d in edges[i+1:]):
        raise ValueError('Footprint must be a simple polygon without intersections')
    x0, y0 = [min(p[i] for p in polygon) for i in (0, 1)]
    x1, y1 = [max(p[i] for p in polygon) for i in (0, 1)]
    if min(x1-x0, y1-y0) < grid:
        raise ValueError('Lot is smaller than one module')
    if len(polygon) == 4 and all(abs(a[0]-b[0]) < 1e-8 or abs(a[1]-b[1]) < 1e-8 for a, b in edges):
        return [[x0, y0, x1, y1]]
    nx, ny = max(1, int((x1-x0)/grid)), max(1, int((y1-y0)/grid))
    if nx*ny > 4096:
        raise ValueError('Lot exceeds 4096 planning cells; increase module grid')
    dx, dy = (x1-x0)/nx, (y1-y0)/ny
    cells = set()
    for j in range(ny):
        for i in range(nx):
            corners = [[x0+i*dx, y0+j*dy], [x0+(i+1)*dx, y0+j*dy], [x0+(i+1)*dx, y0+(j+1)*dy], [x0+i*dx, y0+(j+1)*dy]]
            # Corners alone fail for reentrant lots: also exclude boundary vertices
            # strictly inside the cell, and edges crossing its perimeter.
            if all(inside(p, polygon) for p in corners) and not any(crossing(a, b, c, d) for a, b in edges for c, d in zip(corners, corners[1:]+corners[:1])) and not any(corners[0][0]+1e-8 < p[0] < corners[2][0]-1e-8 and corners[0][1]+1e-8 < p[1] < corners[2][1]-1e-8 for p in polygon):
                cells.add((i, j))
    wings = []
    while cells:
        i, j = min(cells, key=lambda p: (p[1], p[0]))
        end = i+1
        while (end, j) in cells:
            end += 1
        top = j+1
        while all((k, top) in cells for k in range(i, end)):
            top += 1
        for row in range(j, top):
            cells.difference_update((k, row) for k in range(i, end))
        wings.append([x0+i*dx, y0+j*dy, x0+end*dx, y0+top*dy])
    if not wings:
        raise ValueError('No full module fits inside footprint')
    return wings


def make_job(kind, seed, out, style_path=None, rules_path=None, footprint=None):
    style_path = Path(style_path or HERE/'style/battletech.json').resolve()
    rules_path = Path(rules_path or HERE/'rules/buildings.json').resolve()
    style, rules = read_json(style_path), read_json(rules_path)
    rule = rules['types'][kind]
    target = next(o for o in read_json(CATALOG)['objects'] if o['id'] == rule['stock'])
    model = next(m for m in target['models'] if m['role'] == 'intact' and m['lod'] == 0)
    unit = style['scale']['world_units_per_metre']
    size = model['bounds']['size']
    polygon = footprint or [[-size[0]/unit/2, -size[2]/unit/2], [size[0]/unit/2, -size[2]/unit/2], [size[0]/unit/2, size[2]/unit/2], [-size[0]/unit/2, size[2]/unit/2]]
    if any(abs(p[0]) > size[0]/unit/2+1e-7 or abs(p[1]) > size[2]/unit/2+1e-7 for p in polygon):
        raise ValueError('Custom footprint must fit centred stock rectangle (metres)')
    return {'kind': kind, 'seed': seed, 'out': str(Path(out).resolve()/f'{kind}_{seed}'), 'style_path': str(style_path), 'rules_path': str(rules_path), 'rule': rule, 'stock': {'id': target['id'], 'lod0_key': target['ini_sections']['tgldata']['filename0'], 'bounds': model['bounds'], 'sha256': model['sha256'], 'triangles': model['triangles'], 'textures': model['textures']}, 'footprint_m': polygon, 'wings_m': plan_wings(polygon, style['dimensions']['module_m']['value']), 'height_m': size[1]/unit}
