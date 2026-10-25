"""Read-only stock animation / remake parity audit. See README.md for usage.

Coordinates throughout are TG world units: X, Y up, Z depth. No art is written.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.content_pipeline.gltf_io import load_document, read_accessor, node_matrix, frame_matrix
from tools.content_pipeline import gltf_io, ir
from tools.mc2x_import.fst import decompress_entry
from tools.mission_codec import fit_sections


class Stock:
    """Header-only index; effective loose files take precedence over FSTs."""
    def __init__(self, root):
        self.root = Path(root)
        self.files = {}
        self.sources = {}
        self.cache = {}
        for path in sorted(self.root.glob('*.fst')):
            with path.open('rb') as f:
                magic, count = struct.unpack('<II', f.read(8))
                if magic not in (0xFADDECAF, 0xCADDECAF):
                    raise ValueError(f'unsupported FST {path}')
                table = f.read(count * 266)
            for i in range(count):
                off, comp, size, _ = struct.unpack_from('<4I', table, i * 266)
                name = table[i*266+16:(i+1)*266].split(b'\0')[0].decode('latin1').replace('\\', '/').lower()
                self.files[name] = path, off, comp, size, magic == 0xFADDECAF

    def read(self, name):
        name = name.lower().replace('\\', '/')
        if name in self.cache:
            return self.cache[name]
        loose = self.root / name
        if loose.is_file():
            raw = loose.read_bytes()
            source = str(loose)
        else:
            path, off, comp, size, lz = self.files[name]
            with path.open('rb') as f:
                f.seek(off)
                packed = f.read(comp)
            raw = decompress_entry(0, comp, size, lz, packed)
            if len(raw) != size:
                raise ValueError(f'bad member length: {name}')
            source = f'{path}::{name}'
        self.sources[name] = dict(source=source, sha256=hashlib.sha256(raw).hexdigest())
        self.cache[name] = raw
        return raw

    def names(self):
        return sorted(set(self.files) | {p.relative_to(self.root).as_posix().lower()
                                       for p in (self.root / 'data/tgl').glob('*.ini')})


def tgl(raw):
    """Disk layout: tgl_types.cpp; vertices are node-local, centres are absolute."""
    at = 0
    def take(n):
        nonlocal at
        if n < 0 or at+n > len(raw):
            raise ValueError('truncated TGL')
        b = raw[at:at+n]
        at += n
        return b
    def integer():
        n, = struct.unpack('<I', take(4))
        if n > 1000000:
            raise ValueError('invalid count')
        return n
    if struct.unpack('<I', take(4))[0] != 0xBAFDECAF:
        raise ValueError('unsupported TGL')
    count, textures = integer(), integer()
    take(textures*268)
    result = {}
    for _ in range(count):
        kind = integer()
        if kind not in (0, 1):
            raise ValueError('bad TGL node kind')
        vertices, faces = np.empty((0, 3)), np.empty((0, 3), int)
        if kind == 1:
            n = integer()
            vertices = np.frombuffer(take(n*28), dtype=np.dtype([('p','<f4',(3,)),('rest','u1',(16,))]))['p'].astype(float)
            n = integer()
            faces = np.frombuffer(take(n*56), dtype=np.dtype([('i','<u4',(3,)),('rest','u1',(44,))]))['i'].astype(int)
            take(integer()*12)
        centres = np.array(struct.unpack('<6f', take(24)))
        name = take(25).split(b'\0')[0].decode('latin1')
        parent = take(25).split(b'\0')[0].decode('latin1')
        result[name] = dict(parent=parent, pivot=centres[:3], relative=centres[3:],
                            triangles=vertices[faces]+centres[:3] if len(faces) else np.empty((0,3,3)))
    if at != len(raw):
        raise ValueError('unconsumed TGL data')
    return result


def agl(raw):
    """msl_anim.cpp: packed 25-byte name, header, XYZW quaternions, positions."""
    version, count = struct.unpack_from('<II', raw)
    if version != 0xBADDECAF or count > 4096:
        raise ValueError('unsupported AGL')
    at, result = 8, {}
    for _ in range(count):
        name = raw[at:at+25].split(b'\0')[0].decode('latin1')
        at += 25
        _, frames, fps, ticks, qb, pb = struct.unpack_from('<iIffii', raw, at)
        at += 24
        if frames > 100000 or qb not in (0, frames*16) or pb not in (0, frames*12):
            raise ValueError('invalid animation track sizes')
        q = np.frombuffer(raw[at:at+qb], '<f4').reshape(-1,4).astype(float)
        at += qb
        p = np.frombuffer(raw[at:at+pb], '<f4').reshape(-1,3).astype(float)
        at += pb
        result[name] = dict(frames=frames, fps=fps, ticks=ticks, q=q, p=p)
    if at != len(raw):
        raise ValueError('unconsumed AGL data')
    return result


def glb(path):
    doc, buffers, _ = load_document(path)
    side = path.with_suffix('.asset.json')
    if not side.exists():
        side = path.with_name(path.stem.rsplit('_lod',1)[0]+'_lod0.asset.json')
    meta = json.loads(side.read_text())
    basis = frame_matrix(meta['frame'], meta['wu_per_unit'])
    return document_model(doc, buffers, basis)


def document_model(doc, buffers, basis):
    nodes = doc.get('nodes', [])
    parents = {c:i for i,n in enumerate(nodes) for c in n.get('children', [])}
    worlds = {}
    def world(i, chain=()):
        if i in chain:
            raise ValueError('cyclic GLB')
        if i not in worlds:
            worlds[i] = (world(parents[i], (*chain,i)) if i in parents else basis) @ node_matrix(nodes[i])
        return worlds[i]
    result = {}
    for i,n in enumerate(nodes):
        w = world(i)
        tris = []
        if 'mesh' in n:
            for prim in doc['meshes'][n['mesh']]['primitives']:
                v = read_accessor(doc,buffers,prim['attributes']['POSITION'])
                v = v @ w[:3,:3].T + w[:3,3]
                indices = read_accessor(doc,buffers,prim['indices']).reshape(-1) if 'indices' in prim else np.arange(len(v))
                tris.append(v[indices.reshape(-1,3)])
        name = n.get('name', f'node{i}')
        if name in result:
            raise ValueError(f'duplicate GLB node {name}')
        result[name] = dict(parent=nodes[parents[i]].get('name') if i in parents else 'None',
                            pivot=w[:3,3], extras=n.get('extras', {}),
                            triangles=np.concatenate(tris) if tris else np.empty((0,3,3)))
    return result


def descendants(model, name):
    name = next((n for n in model if n.casefold()==name.casefold()),name)
    out = {name}
    while True:
        more = {n for n,v in model.items() if v['parent'] in out}
        if more <= out:
            return out
        out |= more


def geometry(model, name):
    rows = [model[n]['triangles'] for n in descendants(model,name) if n in model]
    return np.concatenate(rows) if rows else np.empty((0,3,3))


def rotation(q):
    x,y,z,w = q / max(np.linalg.norm(q),1e-20)
    return np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
                     [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
                     [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]])


def pose_matrices(model, tracks, fraction):
    """Compose root-to-leaf transforms. AGL translation replaces local centre."""
    matrices = {}
    bindings = {name.casefold():track for name,track in tracks.items()}
    def matrix(name, chain=()):
        if name in chain:
            raise ValueError('cyclic model')
        if name in matrices:
            return matrices[name]
        node = model[name]
        parent = node['parent']
        parent_pivot = model[parent]['pivot'] if parent in model else np.zeros(3)
        local = np.eye(4)
        local[:3,3] = node['pivot']-parent_pivot
        track = bindings.get(name.casefold())
        if track:
            i = min(round(fraction*(track['frames']-1)),track['frames']-1)
            if len(track['q']):
                local[:3,:3] = rotation(track['q'][i])
            if len(track['p']):
                local[:3,3] = track['p'][i]
        matrices[name] = (matrix(parent,(*chain,name)) if parent in model else np.eye(4)) @ local
        return matrices[name]
    for name in model:
        matrix(name)
    return matrices


def pose(model, tracks, fraction):
    matrices = pose_matrices(model, tracks, fraction)
    out = {}
    for name,node in model.items():
        m = matrices[name]
        out[name] = (node['triangles']-node['pivot']) @ m[:3,:3].T + m[:3,3]
    return out


def moving(track, tolerance=1e-4):
    return (len(track['p']) and np.ptp(track['p'],axis=0).max()>tolerance) or (
        len(track['q']) and np.ptp(track['q'],axis=0).max()>tolerance)


def motion_summary(t):
    parts = [f"{t['frames']}f/{t['fps']:g}fps ({(t['frames']-1)/t['fps']:.3g}s)"]
    if len(t['p']):
        parts.append('position TG min/max '+str(np.round([t['p'].min(0),t['p'].max(0)],3).tolist()))
    if len(t['q']):
        # Keep unwrapped relative axis angle, including full revolutions.
        q = t['q'].copy()
        for i in range(1,len(q)):
            if np.dot(q[i-1],q[i]) < 0:
                q[i] *= -1
        angle = np.unwrap(2*np.arctan2(np.linalg.norm(q[:,:3],axis=1),q[:,3]))
        ids = np.linalg.norm(q[:,:3],axis=1)>1e-5
        axis = q[np.flatnonzero(ids)[0],:3] if ids.any() else np.zeros(3)
        axis /= max(np.linalg.norm(axis),1e-20)
        parts.append(f'rotation axis TG {np.round(axis,3).tolist()}, angle {np.degrees(angle.min()):.2f}..{np.degrees(angle.max()):.2f}deg')
    return '; '.join(parts)


def cooked_model(remake):
    """Evaluate the actual runtime writer, retaining source metadata and pivots."""
    names = list(remake)
    if not any(len(node['triangles']) for node in remake.values()):
        return {}
    scene = ir.Scene()
    for name, node in remake.items():
        parent = node['parent']
        matrix = np.eye(4)
        matrix[:3, 3] = node['pivot'] - (remake[parent]['pivot'] if parent in remake else 0)
        mesh = None
        if len(node['triangles']):
            mesh = len(scene.meshes)
            points = (node['triangles'] - node['pivot']).reshape(-1, 3)
            scene.meshes.append(ir.Mesh(name, [ir.Primitive(points, np.arange(len(points)).reshape(-1, 3), 0)]))
        scene.nodes.append(ir.Node(name, names.index(parent) if parent in remake else -1,
                                   matrix, mesh, dict(node.get('extras', {}))))
    doc, binary = gltf_io.split_glb(gltf_io.write_runtime_glb(scene, [ir.Material('audit')], {}))
    return document_model(doc, [binary], gltf_io.TG_FROM_RUNTIME)


def check_nodes(stock, remake, required, tracks=None):
    """Classify playback failures separately from authoring/measurement warnings."""
    failures, warnings = [], []
    tracks = tracks or {}
    names = {n.casefold():n for n in remake}
    runtime = cooked_model(remake)
    for name in sorted(required):
        if name not in stock:
            warnings.append(f'{name}: absent in stock mesh; stock declaration issue')
            continue
        actual = names.get(name.casefold())
        if actual is None:
            failures.append(f'{name}: REAL/ART missing name-bound node; split only the moving part into this node in all LODs')
            continue
        s,r = stock[name],remake[actual]
        if actual != name:
            warnings.append(f'{name}: COSMETIC/TOOL spelling {actual}; engine lookup is case-insensitive')
        if s['parent'] != r['parent']:
            warnings.append(f"{name}: COSMETIC/TOOL source parent {r['parent']} != stock {s['parent']}; static parent name does not bind tracks")
        parent = s['parent']
        while parent in stock:
            if parent in tracks and moving(tracks[parent]):
                if actual not in descendants(runtime, parent):
                    failures.append(f'{name}: REAL/PIPELINE cooked hierarchy loses animated ancestor {parent}; preserve that animated edge and relative pivot in cooker/importer (authored parent is {r["parent"]})')
                break
            parent = stock[parent]['parent']
        error = float(np.linalg.norm(s['pivot']-r['pivot']))
        if error > .05:
            warnings.append(f'{name}: COSMETIC/TOOL authored pivot differs {error:.3f} WU; preserved by cooker, not a playback failure')
        track = tracks.get(name)
        if track and len(track['p']):
            parent_pivot = stock[s['parent']]['pivot'] if s['parent'] in stock else np.zeros(3)
            stock_jump = track['p'][0]+parent_pivot-s['pivot']
            runtime_parent = runtime.get(actual, {}).get('parent')
            runtime_pivot = runtime[runtime_parent]['pivot'] if runtime_parent in runtime else np.zeros(3)
            runtime_jump = track['p'][0]+runtime_pivot-r['pivot']
            displacement = float(np.linalg.norm(runtime_jump-stock_jump))
            if displacement > 1.:
                failures.append(f'{name}: REAL/ART stock absolute translation snaps cooked geometry by {displacement:.3f} WU relative to stock bind pose; author stock translation pivot/frame and compensate static-parent offset when cooking')
        sg,rg = geometry(stock,name),geometry(remake,actual)
        if not len(rg):
            failures.append(f'{name}: REAL/ART empty moving geometry; attach the moving part to the named node')
        elif len(sg):
            # Spatial ownership check: report gross reassignment, not stylistic surface differences.
            slo,shi = sg.min((0,1)),sg.max((0,1))
            rlo,rhi = rg.min((0,1)),rg.max((0,1))
            pad = np.maximum((shi-slo)*.2,1.)
            if np.any(rlo<slo-pad) or np.any(rhi>shi+pad):
                warnings.append(f'{name}: TOOL bounds exceed stock moving region; size/shape heuristic cannot establish wrong geometry ownership')
    return failures,warnings


def nested_composition(stock, remake, runtime, tracks):
    """Stock local-transform evaluation at authored pivots versus cooked playback."""
    nested = []
    for name in tracks:
        parent = stock.get(name, {}).get('parent')
        while parent in stock:
            if parent in tracks and moving(tracks[parent]):
                nested.append(name)
                break
            parent = stock[parent]['parent']
    if not nested:
        return None
    frames = max(t['frames'] for t in tracks.values())
    errors = {name: 0.0 for name in nested if name in remake and name in runtime}
    for frame in range(frames):
        fraction = frame / max(frames-1, 1)
        expected = pose_matrices(remake, tracks, fraction)
        actual = pose_matrices(runtime, tracks, fraction)
        for name in errors:
            errors[name] = max(errors[name], float(np.max(np.abs(expected[name]-actual[name]))))
    return dict(nodes=nested, frames=frames, max_world_matrix_error=errors,
                missing=[name for name in nested if name not in errors],
                passed=len(errors)==len(nested) and all(e < 1e-5 for e in errors.values()))


def gate_check(model, tracks, required):
    failures, metrics = [], {}
    moving_names = set().union(*(descendants(model,n) for n in required if n in model)) if required else set()
    # Triangle-vs-box SAT below detects a static kerb/frame surface crossing a leaf swept box.
    for name in required:
        if name not in model:
            failures.append(f'{name}: no leaf node to sink')
            continue
        leaves = descendants(model,name)
        per_pose = []
        for f in np.linspace(0,1,21):
            posed = pose(model,tracks,float(f))
            tris = np.concatenate([posed[n] for n in leaves])
            if not len(tris):
                continue
            lo,hi = tris.min((0,1)),tris.max((0,1))
            per_pose.append(dict(fraction=float(f), min=lo.tolist(),max=hi.tolist()))
            if f == 1 and hi[1] > .05:
                failures.append(f'{name}: fully open top {hi[1]:.3f} WU above ground')
            if 0 < f <= 1:
                for static in model.keys()-moving_names:
                    st = posed[static]
                    if len(st) and intersects_box(st,lo+.01,hi-.01).any():
                        failures.append(f'{name}: swept leaf box intersects static {static} at {f:.2f} (slot clearance FAIL)')
                        break
        metrics[name] = per_pose
    return list(dict.fromkeys(failures)),metrics


def candidate_gate(stock, remake, tracks, required):
    """Diagnostic only: select central-span faces from the fused mesh, never a fix.

    A separate kerb is identified by all vertices near the ground. A centre-span
    face selection cannot establish ownership of attached details at the piers.
    """
    result = {}
    for name in required:
        if name not in stock:
            continue
        sg = geometry(stock,name)
        if not len(sg):
            continue
        lo,hi = sg.min((0,1)),sg.max((0,1))
        span = 0 if hi[0]-lo[0]>hi[2]-lo[2] else 2
        alltris = np.concatenate([n['triangles'] for n in remake.values()])
        margin = max(.5,(hi[span]-lo[span])*.1)
        center = (alltris[:,:,span].min(1)>lo[span]+margin)&(alltris[:,:,span].max(1)<hi[span]-margin)
        kerb = center & (alltris[:,:,1].max(1)<max(1.,(hi[1]-lo[1])*.03))
        leaf = center & ~kerb
        t = alltris[leaf]
        if not len(t):
            result[name] = dict(reason='no central-span candidate geometry')
            continue
        track = tracks[name]
        travel = float(track['p'][0,1]-track['p'][-1,1]) if len(track['p']) else 0.
        closed_top = float(t[:,:,1].max())
        closed_bottom = float(t[:,:,1].min())
        result[name] = dict(method='central 80 percent stock leaf span, exclude low kerb faces; diagnostic selection',
                            triangles=len(t),closed_top_wu=closed_top,closed_bottom_wu=closed_bottom,
                            leaf_height_wu=closed_top-closed_bottom,stock_travel_wu=travel,
                            hypothetical_open_top_wu=closed_top-travel,kerb_faces=len(alltris[kerb]),
                            closed_span_stock=[float(lo[span]),float(hi[span])],
                            closed_span_candidate=[float(t[:,:,span].min()),float(t[:,:,span].max())])
    return result


def gate_blocking(stock,remake,tracks,required):
    """Compare closed coverage and residual open obstruction in the stock aperture."""
    sg = np.concatenate([geometry(stock,n) for n in required if n in stock])
    lo,hi = sg.min((0,1)),sg.max((0,1))
    span = 0 if hi[0]-lo[0]>hi[2]-lo[2] else 2
    def mask(model,f):
        im = Image.new('1',(256,128))
        draw = ImageDraw.Draw(im)
        for ts in pose(model,tracks,f).values():
            for t in ts:
                xy = [(float((v[span]-lo[span])*255/(hi[span]-lo[span])),
                       float((hi[1]-v[1])*127/(hi[1]-lo[1]))) for v in t]
                draw.polygon(xy,fill=1)
        return np.array(im,dtype=bool)
    reference = mask(stock,0)
    closed,opened = mask(remake,0),mask(remake,1)
    denom = max(int(reference.sum()),1)
    return dict(method='256x128 front projection in stock closed leaf bounds; includes static geometry',
                closed_stock_coverage=float(reference.mean()),closed_remake_coverage=float(closed.mean()),
                stock_blocking_missing_fraction=float((reference & ~closed).sum()/denom),
                remake_open_residual_fraction=float((reference & opened).sum()/denom))


def extremes(stock,remake,tracks):
    """Apply every sampled track extremum, not only the last frame of a loop."""
    indices = {0}
    count = max((t['frames'] for t in tracks.values()),default=1)
    indices.add(count-1)
    for t in tracks.values():
        for key in ('p','q'):
            if len(t[key]):
                indices.update(map(int,t[key].argmin(0)))
                indices.update(map(int,t[key].argmax(0)))
    result = []
    active = [n for n,t in tracks.items() if moving(t)]
    for i in sorted(indices):
        f = i/max(count-1,1)
        record = dict(frame=i,nodes={})
        for label,model in (('stock',stock),('remake',remake)):
            posed = pose(model,tracks,f)
            for n in active:
                ts = [posed[d] for d in descendants(model,n) if d in posed]
                g = np.concatenate(ts) if ts else np.empty((0,3,3))
                record['nodes'].setdefault(n,{})[label] = (dict(min=g.min((0,1)),max=g.max((0,1))) if len(g) else None)
        result.append(record)
    return result


def intersects_box(tris,lo,hi):
    """Triangle/AABB separating axis test; ignores boundary-only contact."""
    if np.any(hi<=lo):
        return np.zeros(len(tris),bool)
    center,half = (lo+hi)/2,(hi-lo)/2
    t = tris-center
    hit = np.all(t.max(1)>-half,axis=1)&np.all(t.min(1)<half,axis=1)
    edges = np.roll(t,-1,axis=1)-t
    axes = [np.cross(edges[:,0],edges[:,1])]
    axes += [np.cross(edges[:,i],a) for i in range(3) for a in np.eye(3)]
    for axis in axes:
        p = np.einsum('nvi,ni->nv',t,axis)
        radius = np.abs(axis) @ half
        valid = np.linalg.norm(axis,axis=1)>1e-10
        hit &= ~valid | ((p.min(1)<radius-1e-8)&(p.max(1)>-radius+1e-8))
    return hit


def sheet(path,stock,remake,tracks,required):
    img = Image.new('RGB',(1500,760),'#161b22')
    draw = ImageDraw.Draw(img)
    alltris = np.concatenate([n['triangles'] for m in (stock,remake) for n in m.values()])
    # Show below-ground geometry; front and small depth offset expose the slot.
    span = 0 if np.ptp(alltris[:,:,0])>np.ptp(alltris[:,:,2]) else 2
    depth = 2 if span==0 else 0
    def project(v):
        return np.stack([v[...,span]+.28*v[...,depth],v[...,1]+.16*v[...,depth]],axis=-1)
    p = project(alltris)
    lo,hi = p.min((0,1)),p.max((0,1))
    lo[1] -= max(hi[1]-lo[1],20)
    scale = min(450/max(hi[0]-lo[0],1),300/max(hi[1]-lo[1],1))
    for row,(label,model) in enumerate((('STOCK',stock),('REMAKE LOD0',remake))):
        moving_names = set().union(*(descendants(model,n) for n in required if n in model)) if required else set()
        for col,f in enumerate((0,.5,1)):
            ox,oy = col*500+250,row*380+335
            posed = pose(model,tracks,f)
            draw.text((col*500+15,row*380+12),f'{label} | '+('CLOSED','HALF OPEN','OPEN')[col],fill='white')
            ground = oy-(0-lo[1])*scale
            draw.line((col*500,ground,col*500+499,ground),fill='#66cc88',width=2)
            draw.text((col*500+10,ground+3),'ground Y=0; subsurface shown',fill='#66cc88')
            triangles = [(t,name) for name,ts in posed.items() for t in ts]
            triangles.sort(key=lambda item:float(item[0][:,depth].mean()))
            for t,name in triangles:
                pp = project(t)
                xy = [(float(ox+(v[0]-(lo[0]+hi[0])/2)*scale),float(oy-(v[1]-lo[1])*scale)) for v in pp]
                fill = '#e5a34b' if name in moving_names else '#727f90'
                draw.polygon(xy,fill=fill,outline='#303844')
            draw.line((col*500,ground,col*500+499,ground),fill='#66cc88')
            draw.rectangle((col*500,row*380,col*500+499,row*380+28),fill='#161b22')
            draw.text((col*500+15,row*380+9),f'{label} | '+('CLOSED','HALF OPEN','OPEN')[col],fill='white')
    img.save(path)


def serial(value):
    if isinstance(value,np.ndarray):
        return value.tolist()
    if isinstance(value,np.generic):
        return value.item()
    raise TypeError(type(value).__name__)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--install',type=Path,default=Path('A:/Games/mc2-opengl/mc2-win64-v0.4'))
    ap.add_argument('--art',type=Path,default=Path('E:/mc2-ref/scifi-kit/art/intent-pass'))
    ap.add_argument('--catalog',type=Path,default=Path('E:/mc2-archive-2026-10/asset-modernize/docs/assets/catalog.json'))
    ap.add_argument('--placements',type=Path,default=Path('E:/mc2-archive-2026-10/proofs/placement-survey/placements.json'))
    ap.add_argument('--out',type=Path,default=Path('E:/mc2-archive-2026-10/proofs/anim-parity'))
    args = ap.parse_args()
    out = args.out.resolve()
    for protected in (args.install,args.art,ROOT,Path('A:/Games/mc2-art3'),Path('A:/Games/mc2-s1')):
        if out.is_relative_to(protected.resolve()):
            raise ValueError('output must be outside read-only sources and repository')
    out.mkdir(parents=True,exist_ok=True)
    survey = json.loads(args.placements.read_text())
    gate_missions = [dict(mission=m,**p) for m,v in survey['missions'].items()
                    for p in v['placements'] if p['category']=='gate']
    (out/'gate-missions.json').write_text(json.dumps(gate_missions,indent=2),encoding='utf-8')
    stock = Stock(args.install)
    catalog = {o['id']:o for o in json.loads(args.catalog.read_text())['objects']}
    # Latest export per asset, not latest timestamp on the directory.
    bakes = {}
    candidates = sorted((args.art/'_work').glob('*/pbr/*_pristine_lod0.glb'))
    for p in candidates:
        name = p.stem.removesuffix('_pristine_lod0')
        if name not in bakes or p.stat().st_mtime_ns>bakes[name].stat().st_mtime_ns:
            bakes[name] = p
    rows, errors, excluded = [],[],[]
    for ini in stock.names():
        if not ini.startswith('data/tgl/') or not ini.endswith('.ini') or '/' in ini[len('data/tgl/'):]:
            continue
        sections = fit_sections(stock.read(ini))
        clips = {k:v for k,v in sections.items() if k.startswith('animation:')}
        rotational = sections.get('animationnode',{}).get('animationnodeid','NONE')
        spin = str(sections.get('tgldata',{}).get('spinme','false')).lower() in ('true','1')
        if not clips and rotational.lower()=='none' and not spin:
            continue
        asset = Path(ini).stem
        category = catalog.get(asset,{}).get('category','uncatalogued')
        if category in ('vehicle','mech') or (category=='uncatalogued' and asset not in bakes and not spin):
            excluded.append(dict(ini=ini,reason='vehicle/mech or unused appearance declaration outside static asset catalog'))
            continue
        row = dict(asset=asset, ini=ini,category=catalog.get(asset,{}).get('category','uncatalogued'),
                   clips=[], rotational=rotational,spin_me=spin,damage=sections.get('tgldamage',{}),
                   remade=asset in bakes,failures=[],warnings=[],lods=[])
        data = sections.get('tgldata',{})
        stem = data.get('filename0',data.get('filename',asset))
        row['stock_model'] = stem
        try:
            model = tgl(stock.read(f'data/tgl/{stem}.tgl'))
            required = set()
            clip_tracks = {}
            gate_tracks = None
            for state,fields in clips.items():
                clip = fields.get('animationname','')
                tracks = agl(stock.read(f'data/tgl/{clip}.agl'))
                clip_tracks[state] = tracks
                active = {n:t for n,t in tracks.items() if moving(t)}
                # Constant closed/open clips still bind named nodes.
                bound = {n:t for n,t in tracks.items() if len(t['p']) or len(t['q'])}
                required.update(bound)
                row['clips'].append(dict(state=state,fields=fields,nodes={n:motion_summary(t) for n,t in bound.items()},
                                         moving=list(active)))
                if state=='animation:1':
                    gate_tracks = tracks
            if rotational.lower()!='none':
                required.add(rotational)
            row['stock_nodes'] = {n:{k:v for k,v in model[n].items() if k!='triangles'} for n in required if n in model}
            row['required'] = sorted(required)
            binding_tracks = {}
            for tracks in clip_tracks.values():
                for name,track in tracks.items():
                    if name not in binding_tracks or moving(track):
                        binding_tracks[name] = track
            if row['damage'].get('filename'):
                damage_path = f"data/tgl/{row['damage']['filename']}.tgl"
                try:
                    dmg = tgl(stock.read(damage_path))
                    row['damage_nodes'] = {n:dict(parent=v['parent'],triangles=len(v['triangles'])) for n,v in dmg.items()}
                    row['damage_verified'] = True
                except (KeyError,ValueError) as e:
                    row['damage_verified'] = False
                    row['failures'].append(f'declared destroyed mesh unresolved: {e}')
            if asset in bakes:
                row['bake'] = str(bakes[asset])
                for lod in range(3):
                    path = bakes[asset].with_name(bakes[asset].name.replace('lod0','lod'+str(lod)))
                    if not path.exists():
                        row['failures'].append(f'LOD{lod}: missing export')
                        continue
                    remake = glb(path)
                    problems,warnings = check_nodes(model,remake,required,binding_tracks)
                    runtime = cooked_model(remake)
                    detail = dict(lod=lod,path=str(path),sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                                  nodes={n:dict(parent=v['parent'],pivot=v['pivot'],triangles=len(v['triangles'])) for n,v in remake.items()},
                                  failures=problems,warnings=warnings)
                    detail['extreme_poses'] = {state:extremes(model,runtime,tracks) for state,tracks in clip_tracks.items()}
                    detail['nested_composition'] = {state: proof for state, tracks in clip_tracks.items()
                                                    if (proof := nested_composition(model, remake, runtime, tracks))}
                    for state, proof in detail['nested_composition'].items():
                        if not proof['passed'] and not proof['missing']:
                            problems.append(f'REAL/PIPELINE nested world transforms differ in state {state}: {proof}')
                    if rotational in required and rotational.lower()!='none':
                        sweep = {rotational:dict(frames=5,fps=1,ticks=1,p=np.empty((0,3)),
                                                q=np.array([[0,np.sin(a/2),0,np.cos(a/2)] for a in np.linspace(0,2*np.pi,5)]))}
                        detail['aim_yaw_extremes'] = extremes(model,runtime,sweep)
                    if row['category']=='gate' and gate_tracks:
                        gate_nodes = {n for n,t in gate_tracks.items() if moving(t)}
                        extra,metrics = gate_check(remake,gate_tracks,gate_nodes)
                        problems += extra
                        detail['gate_poses'] = metrics
                        detail['candidate_leaf_diagnostic'] = candidate_gate(model,remake,gate_tracks,gate_nodes)
                        detail['blocking'] = gate_blocking(model,remake,gate_tracks,gate_nodes)
                        if lod==0:
                            row['stock_gate_failures'],row['stock_gate_poses'] = gate_check(model,gate_tracks,gate_nodes)
                            sheet(out/f'{asset}-poses.png',model,remake,gate_tracks,gate_nodes)
                    row['lods'].append(detail)
                    row['failures'] += [f'LOD{lod}: {p}' for p in problems]
                    row['warnings'] += [f'LOD{lod}: {w}' for w in warnings]
                row['status'] = 'FAIL' if row['failures'] else ('WARN' if row['warnings'] else 'PASS')
            else:
                row['status'] = 'NOT REMADE'
        except (KeyError,ValueError,FileNotFoundError,struct.error) as e:
            row['status'] = 'FAIL' if row['remade'] else 'UNRESOLVED'
            row['failures'].append(f'decode/coverage: {e}')
            errors.append(f'{asset}: {e}')
        rows.append(row)
    result = dict(schema=2,units='TG world units; Y up',install=str(args.install),art=str(args.art),
                  bake_selection='newest pristine LOD0 per asset; all candidate paths recorded',
                  bake_candidates=[str(p) for p in candidates],baked_assets=sorted(bakes),rows=rows,errors=errors,sources=stock.sources)
    result['excluded_declarations'] = excluded
    # Record nonanimated remakes too: they need no stock clip parity check.
    result['remakes_without_stock_animation'] = sorted(set(bakes)-{r['asset'] for r in rows})
    result['review_folders_without_pristine_bakes'] = sorted(p.name for p in args.art.iterdir()
        if p.is_dir() and not p.name.startswith('_') and p.name not in bakes and p.name!='owner-notes')
    (out/'inventory.json').write_text(json.dumps(result,indent=2,default=serial),encoding='utf-8')
    write_report(out,result)
    print(f"{len(rows)} animation-bearing appearances; {sum(r['remade'] for r in rows)} remade; "
          f"{sum(r['status']=='FAIL' for r in rows)} REAL FAIL; {sum(r['status']=='WARN' for r in rows)} WARN; {len(errors)} unresolved decodes. {out}")


def write_report(out,result):
    rows = result['rows']
    lines = ['# Animation parity audit','',
             'Revised for the current nifty kit importer: REAL failures affect the replacement motion; '
             'COSMETIC/TOOL warnings do not establish broken animation. Static parent names are not a binding requirement.','',
             '## REAL failures first (asset / node / cause / required fix)','']
    lines += [f"Scanned {len(result['baked_assets'])} pristine remake exports: {sum(r['remade'] for r in rows)} animation-bearing, "
              f"{len(result['remakes_without_stock_animation'])} without stock animation. "
              f"{sum(r['status']=='FAIL' for r in rows)} REAL FAIL, {sum(r['status']=='WARN' for r in rows)} WARN. "
              f"{len(result['errors'])} stock declarations have missing source meshes/clips (listed below).",'']
    for r in rows:
        if r['status']=='FAIL':
            lines += [f"- **{r['asset']}**: "+'; '.join(dict.fromkeys(p.removeprefix('LOD0: ').removeprefix('LOD1: ').removeprefix('LOD2: ') for p in r['failures']))]
    lines += ['', 'Missing clip nodes cause the current nifty compatibility guard to discard the replacement render chain '
              'and retain stock, rather than freezing the stock asset. An AnimationNodeId-only omission (spotlight) '
              'is not covered by that clip-node guard. REAL/PIPELINE findings need cooker/importer fixes, not art reparenting.',
              '', '## WARN: COSMETIC/TOOL (not confirmed playback defects)', '']
    for r in rows:
        if r['remade'] and r.get('warnings'):
            ws = dict.fromkeys(w.removeprefix('LOD0: ').removeprefix('LOD1: ').removeprefix('LOD2: ') for w in r['warnings'])
            lines += [f"- **{r['asset']}** ({r['status']}): "+'; '.join(ws)]
    lines += ['', 'Missing stock sources: '+ '; '.join(result['errors'])+'.',
              '', 'Review folders with no pristine export to audit: '+', '.join(result['review_folders_without_pristine_bakes'])+'.']
    lines += ['', '## Per-asset inventory', '', '| Asset | Stock nodes / motions | Remake | Reason / damage swap |',
              '|---|---|---|---|']
    for r in rows:
        motions = []
        grouped = {}
        for c in r['clips']:
            key = c['fields']['animationname']
            entry = grouped.setdefault(key,dict(states=[],nodes=c['nodes']))
            entry['states'].append(c['state'].removeprefix('animation:')+': '+str({k:v for k,v in c['fields'].items() if k!='animationname'}))
        for clip,c in grouped.items():
            motions += [clip+' [states '+', '.join(c['states'])+']: '+ '; '.join(f'{n}: {s}' for n,s in c['nodes'].items())]
        if r['rotational'].lower()!='none':
            motions += [f"{r['rotational']}: engine turret yaw/pitch"]
        if r['spin_me']:
            motions += ['SpinMe: whole object yaw 90deg/s']
        reason = '; '.join(r['failures']) if r['failures'] else ('name-bound moving geometry present; see WARN section for authoring differences' if r['remade'] else 'no pristine remake bake')
        reason += '; destroyed swap '+str(r['damage'])
        lines += [f"| {r['asset']} | "+'<br>'.join(motions).replace('|','/')+f" | {r['status']} | "+reason.replace('|','/')+' |']
    lines += ['', '## Gate pose sheets', '', 'Orange = geometry under animated leaf node; grey = static. '
              'Orthographic geometry proof, not textured game capture. Subsurface geometry remains visible.']
    lines += ['', '| Gate | Stock travel WU | Candidate leaf height WU | Hypothetical open top WU | Kerb faces in passage |',
              '|---|---:|---:|---:|---:|']
    for r in rows:
        if r['category']=='gate' and r['lods']:
            for n,d in r['lods'][0].get('candidate_leaf_diagnostic',{}).items():
                if 'stock_travel_wu' in d:
                    lines += [f"| {r['asset']} / {n} | {d['stock_travel_wu']:.3f} | {d['leaf_height_wu']:.3f} | {d['hypothetical_open_top_wu']:.3f} | {d['kerb_faces']} |"]
    lines += ['', '| Gate | Missing closed stock blocking | Open residual blocking |','|---|---:|---:|']
    for r in rows:
        if r['category']=='gate' and r['lods'] and 'blocking' in r['lods'][0]:
            b=r['lods'][0]['blocking']
            lines += [f"| {r['asset']} | {b['stock_blocking_missing_fraction']:.1%} | {b['remake_open_residual_fraction']:.1%} |"]
    lines += ['', 'Candidate leaf measurements are spatial diagnostics on a fused mesh, not authoritative part assignments. '
              'The actual exported leaves do not move. The art source creates a continuous ground track across the leaf span '
              '(intent_military.py:1391); a slot is needed when separating the leaves. '
              'Stock sheets may show tiny positive open heights and conservative box-overlap flags; see inventory.json stock_gate_poses.']
    for p in sorted(out.glob('*-poses.png')):
        lines += ['',f'### {p.stem}',f'![{p.stem}]({p.name})']
    lines += ['',(Path(__file__).with_name('evidence.md')).read_text(encoding='utf-8')]
    (out/'REPORT.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')


if __name__=='__main__':
    main()
