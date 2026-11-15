"""Offline tree GLB cook: source, output directory, vendored meshoptimizer DLL."""
import copy
import ctypes
import json
import sys
from pathlib import Path
import numpy as np
import meshoptimizer as mo
from meshoptimizer import _loader, optimizer, simplifier
sys.path.insert(0, str(Path(__file__).parent / 'scifi_kit/art'))
from meshopt_lods import read_glb, write_glb, accessor


def cook(source, output, dll):
    lib = ctypes.CDLL(str(Path(dll).resolve()))
    for name in ('meshopt_optimizeVertexCache', 'meshopt_optimizeOverdraw',
                 'meshopt_optimizeVertexFetch', 'meshopt_simplifyWithAttributes'):
        getattr(lib, name).argtypes = getattr(_loader.lib, name).argtypes
        getattr(lib, name).restype = getattr(_loader.lib, name).restype
    optimizer.lib = simplifier.lib = lib
    p, n = ctypes.c_void_p, ctypes.c_size_t
    lib.meshopt_simplifyWithAttributes.argtypes = [p, p, n, p, n, n, p, n, p, n, p, n, ctypes.c_float, ctypes.c_uint, p]
    lib.meshopt_simplifyWithAttributes.restype = n
    original, binary = read_glb(source)
    assert not original.get('skins') and not original.get('animations')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    for level, ratio in enumerate((1., .25, .06, .015, .5)):
        doc = copy.deepcopy(original)
        doc['accessors'], doc['bufferViews'], data, report = [], [], bytearray(), []

        def view(raw):
            data.extend(b'\0' * (-len(data) % 4))
            doc['bufferViews'].append(dict(buffer=0, byteOffset=len(data), byteLength=len(raw)))
            data.extend(raw)
            return len(doc['bufferViews']) - 1

        for image in doc.get('images', []):
            if 'bufferView' in image:
                old = original['bufferViews'][image['bufferView']]
                offset = old.get('byteOffset', 0)
                image['bufferView'] = view(binary[offset:offset + old['byteLength']])
        for mesh, oldmesh in zip(doc['meshes'], original['meshes']):
            for prim, old in zip(mesh['primitives'], oldmesh['primitives']):
                assert old.get('mode', 4) == 4 and not old.get('targets')
                arrays = {k: accessor(original, binary, a) for k, a in old['attributes'].items()}
                assert all(a.dtype == np.float32 or (a.dtype == np.uint8 and original['accessors'][old['attributes'][k]].get('normalized')) for k, a in arrays.items())
                arrays = {k: a.astype(np.float32)/255 if a.dtype == np.uint8 else a for k, a in arrays.items()}
                pos = np.ascontiguousarray(arrays['POSITION'])
                idx = accessor(original, binary, old['indices']).ravel().astype(np.uint32)
                result, error = np.empty_like(idx), np.zeros(1, np.float32)
                attrs = np.ascontiguousarray(np.concatenate([arrays['NORMAL'], arrays['TEXCOORD_0']], axis=1))
                count = len(idx) if ratio == 1 else mo.simplify_with_attributes(result, idx, pos, attrs,
                    np.array([.5, .5, .5, 1., 1.], np.float32), target_index_count=max(6, int(len(idx)*ratio)//3*3),
                    target_error=1., options=mo.SIMPLIFY_LOCK_BORDER, result_error=error)
                result = idx.copy() if ratio == 1 else result[:count].copy()
                mo.optimize_vertex_cache(result, result, vertex_count=len(pos))
                mo.optimize_overdraw(result, result, pos)
                vertices = np.ascontiguousarray(np.concatenate(list(arrays.values()), axis=1))
                fetched = np.empty_like(vertices)
                before_fetch = result.copy()
                used = mo.optimize_vertex_fetch(fetched, result, vertices)
                assert np.array_equal(vertices[before_fetch], fetched[result])
                offset = 0
                for key, values in arrays.items():
                    a = copy.deepcopy(original['accessors'][old['attributes'][key]])
                    a.update(bufferView=view(fetched[:used, offset:offset+values.shape[1]].copy().tobytes()), byteOffset=0, count=used, componentType=5126)
                    a.pop('normalized', None)
                    a.pop('min', None); a.pop('max', None)
                    if key == 'POSITION': a.update(min=fetched[:used, offset:offset+3].min(0).tolist(), max=fetched[:used, offset:offset+3].max(0).tolist())
                    doc['accessors'].append(a)
                    prim['attributes'][key] = len(doc['accessors']) - 1
                    offset += values.shape[1]
                doc['accessors'].append(dict(bufferView=view(result.tobytes()), componentType=5125, count=len(result), type='SCALAR'))
                prim['indices'] = len(doc['accessors']) - 1
                report.append(dict(source_triangles=len(idx)//3, triangles=len(result)//3, relative_error=float(error[0]), absolute_error=float(error[0])*float(np.ptp(pos, axis=0).max())))
        name = Path(source).stem.replace('lod0', f'lod{level}' if level < 4 else 'option50')
        write_glb(output / f'{name}.glb', doc, data)
        print(json.dumps(dict(lod=level, ratio=ratio, primitives=report, bytes=len(data))), flush=True)


if __name__ == '__main__':
    cook(*sys.argv[1:])
