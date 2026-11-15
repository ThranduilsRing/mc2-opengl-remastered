"""Read-only BT2018 recon: emit names, counts, hashes and parity receipts, never assets.

Run with Python 3.12 / UnityPy 1.25.0. Output JSON is stdout; inputs are read only.
"""
import argparse
from collections import Counter
import hashlib
import json
import re
from pathlib import Path
import struct
import time


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def glb(path):
    with path.open('rb') as stream:
        assert stream.read(4) == b'glTF'
        stream.read(8)
        size, kind = struct.unpack('<II', stream.read(8))
        assert kind == 0x4E4F534A
        data = json.loads(stream.read(size))
    return dict(path=str(path), sha256=digest(path),
                nodes=[n.get('name', '') for n in data.get('nodes', [])],
                joints=sorted({data['nodes'][j].get('name', '')
                               for s in data.get('skins', []) for j in s['joints']}),
                meshes=[m.get('name', '') for m in data.get('meshes', [])],
                animations=[a.get('name', '') for a in data.get('animations', [])])


def fbx(path):
    """Inspect binary FBX metadata; skip geometry/curve arrays without decoding them."""
    rows = []
    with path.open('rb') as stream:
        assert stream.read(23) == b'Kaydara FBX Binary  \x00\x1a\x00'
        version, = struct.unpack('<I', stream.read(4))
        fmt, width = ('<QQQB', 25) if version >= 7500 else ('<IIIB', 13)

        def node():
            end, count, _, length = struct.unpack(fmt, stream.read(width))
            if not end:
                return False
            name = stream.read(length).decode('utf-8', 'replace')
            props = []
            for _ in range(count):
                code = stream.read(1).decode()
                if code in 'YCFDIL':
                    pf = dict(Y='h', C='?', F='f', D='d', I='i', L='q')[code]
                    props.append(struct.unpack('<'+pf, stream.read(struct.calcsize(pf)))[0])
                elif code in 'SR':
                    n, = struct.unpack('<I', stream.read(4))
                    value = stream.read(n)
                    props.append(value.decode('utf-8', 'replace') if code == 'S' else '<raw>')
                elif code in 'fdlibc':
                    n, encoding, size = struct.unpack('<III', stream.read(12))
                    stream.seek(size, 1)
                    props.append(dict(elements=n, encoding=encoding))
                else:
                    raise ValueError(f'Unknown FBX property {code}')
            if name in ('Model', 'Geometry', 'AnimationStack', 'AnimationLayer', 'LocalStart', 'LocalStop') or (name == 'P' and props and props[0] in ('LocalStart', 'LocalStop')):
                rows.append([name, props])
            while stream.tell() < end:
                if not node():
                    break
            stream.seek(end)
            return True

        while node():
            pass
    return dict(path=str(path), version=version, sha256=digest(path), objects=rows)


def obj_summary(text):
    import numpy as np
    vertices, faces = [], []
    for line in text.splitlines():
        if line.startswith('v '):
            vertices.append([float(v) for v in line.split()[1:]])
        elif line.startswith('f '):
            faces.append(line.split()[1:])
    return np.array(vertices), faces


def bundle(path, dump):
    import UnityPy
    import numpy as np
    from PIL import Image
    started = time.perf_counter()
    env = UnityPy.load(str(path))
    result = dict(path=str(path), sha256=digest(path), unitypy=UnityPy.__version__,
                  types=dict(Counter(o.type.name for o in env.objects)),
                  versions=sorted({o.assets_file.unity_version for o in env.objects}),
                  externals=sorted({e.path for o in env.objects for e in o.assets_file.externals}),
                  textures=[], meshes=[], clips=[], transforms=[], renderers=[])
    for o in env.objects:
        if o.type.name not in ('Texture2D', 'Mesh', 'AnimationClip', 'Transform', 'SkinnedMeshRenderer'):
            continue
        d = o.read()
        if o.type.name == 'Texture2D':
            im = np.asarray(d.image.convert('RGBA')).astype('int16')
            row = dict(name=d.m_Name, format=int(d.m_TextureFormat), size=[d.m_Width, d.m_Height], matches=[])
            for p in sorted((dump / 'Texture2D').glob(d.m_Name+'*.png')):
                ref = np.asarray(Image.open(p).convert('RGBA')).astype('int16')
                row['matches'].append(dict(file=p.name, equal=bool(np.array_equal(im, ref)),
                    flipped_equal=bool(np.array_equal(im[::-1], ref)),
                    max_abs=int(np.max(np.abs(im-ref))) if im.shape == ref.shape else None))
            result['textures'].append(row)
        elif o.type.name == 'Mesh':
            vertices, faces = obj_summary(d.export())
            row = dict(name=d.m_Name, vertices=len(vertices), faces=len(faces), matches=[])
            for p in sorted((dump / 'Mesh').glob(d.m_Name+'*.obj')):
                rv, rf = obj_summary(p.read_text())
                row['matches'].append(dict(file=p.name, vertices=len(rv), faces=len(rf),
                    max_abs=float(np.max(np.abs(vertices-rv))) if vertices.shape == rv.shape else None,
                    faces_equal=faces == rf))
            result['meshes'].append(row)
        elif o.type.name == 'AnimationClip':
            tree = o.read_typetree()
            muscle = tree.get('m_MuscleClip', {})
            result['clips'].append(dict(name=d.m_Name, length=muscle.get('m_StopTime'), sample_rate=tree.get('m_SampleRate'),
                fields=list(tree), muscle_fields=list(muscle),
                clip_summary={k: {kk: len(vv) if isinstance(vv, (list, bytes)) else vv
                                 for kk, vv in v.items() if not isinstance(vv, dict)}
                              for k, v in muscle.get('m_Clip', {}).get('data', {}).items() if isinstance(v, dict)},
                binding_count=len(tree.get('m_ClipBindingConstant', {}).get('genericBindings', []))))
        elif o.type.name == 'Transform':
            result['transforms'].append(d.m_GameObject.read().m_Name)
        else:
            bones = []
            for b in d.m_Bones:
                try:
                    bones.append(b.read().m_GameObject.read().m_Name)
                except Exception as error:
                    bones.append('UNRESOLVED: '+str(error))
            result['renderers'].append(dict(name=d.m_GameObject.read().m_Name, bones=bones))
    result['seconds'] = time.perf_counter()-started
    return result


def compare(export, dump):
    import numpy as np
    from PIL import Image
    index = {}
    hashes = {}
    def normalized(name):
        return re.sub(r' #[^ .]+(?=\.)', '', name.lower())
    for p in dump.rglob('*'):
        if p.is_file():
            index.setdefault(normalized(p.name), []).append(p)
    rows = []
    for p in sorted(export.rglob('*')):
        if not p.is_file():
            continue
        candidates = index.get(normalized(p.name), [])
        sha = digest(p)
        matches = []
        for q in candidates:
            if q not in hashes:
                hashes[q] = digest(q)
            m = dict(path=str(q.relative_to(dump)), equal=sha == hashes[q])
            if not m['equal'] and p.suffix.lower() == '.png':
                a = np.asarray(Image.open(p).convert('RGBA')).astype('int16')
                b = np.asarray(Image.open(q).convert('RGBA')).astype('int16')
                m.update(pixel_equal=bool(np.array_equal(a, b)), flipped_equal=bool(np.array_equal(a[::-1], b)),
                         max_abs=int(np.max(np.abs(a-b))) if a.shape == b.shape else None)
            elif not m['equal'] and p.suffix.lower() == '.obj':
                a, af = obj_summary(p.read_text())
                b, bf = obj_summary(q.read_text())
                m.update(vertices=[len(a),len(b)], faces_equal=af == bf,
                         max_abs=float(np.max(np.abs(a-b))) if a.shape == b.shape and a.size else None)
            matches.append(m)
        rows.append(dict(path=str(p.relative_to(export)), bytes=p.stat().st_size, sha256=sha, matches=matches))
    return dict(files=len(rows), bytes=sum(r['bytes'] for r in rows),
                extensions=dict(Counter(Path(r['path']).suffix for r in rows)), rows=rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('mode', choices=('bundle', 'glb', 'fbx', 'compare'))
    ap.add_argument('path', type=Path)
    ap.add_argument('--dump', type=Path, default=Path('A:/Games/mc2-opengl/BattleTech_2018_Dump'))
    args = ap.parse_args()
    if args.mode in ('bundle', 'compare'):
        result = globals()[args.mode](args.path, args.dump)
    else:
        result = globals()[args.mode](args.path)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
