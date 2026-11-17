"""Bake a BT2018 chassis bundle's native AnimationClips (streamed/dense/constant muscle-clip
curves) into that chassis's GLB as `<family>_*` rotation clips at 30 Hz. Code only: it reads
the player's own bundle; no HBS bytes are stored in the repo.

Run with Python 3.12 / UnityPy 1.25.0 (pinned; the typetree field names below are that version's):
  py -3.12 tools/bt2018_extract/bake_clips.py <bundle> <family> <in.glb> <out.glb>
"""
import struct
import sys
import zlib

import numpy as np

UNITYPY_VERSION = '1.25.0'
RATE = 30.0
WIDTH = {1: 3, 2: 4, 3: 3, 4: 3}          # generic binding attribute -> curve count (pos, quat, scale, euler)


def streamed_keys(words):
    """Unity streamed clip: frames of (time, n, n x (curve, c0..c3)); value = ((c0 dt + c1) dt + c2) dt + c3."""
    raw = struct.pack(f'<{len(words)}I', *words)
    keys, at = {}, 0
    while at + 8 <= len(raw):
        time, n = struct.unpack_from('<fi', raw, at); at += 8
        for _ in range(n):
            idx, *c = struct.unpack_from('<i4f', raw, at); at += 20
            keys.setdefault(idx, []).append((time, c))
    return keys


def eval_streamed(keys, t):
    times = np.array([k[0] for k in keys])
    j = max(int(np.searchsorted(times, t, side='right')) - 1, 0)
    dt = t - keys[j][0]
    c0, c1, c2, c3 = keys[j][1]
    return ((c0 * dt + c1) * dt + c2) * dt + c3


def sample_clip(tt, times):
    """All curve values [len(times), curves] in binding order (streamed, dense, constant)."""
    clip = tt['m_MuscleClip']['m_Clip']
    clip = clip.get('data', clip)
    s, d, c = clip['m_StreamedClip'], clip['m_DenseClip'], clip['m_ConstantClip']['data']
    sk = streamed_keys(s['data'])
    cols = [[eval_streamed(sk[i], t) if i in sk else 0.0 for t in times] for i in range(s['curveCount'])]
    if d['m_CurveCount']:
        arr = np.array(d['m_SampleArray'], np.float64).reshape(-1, d['m_CurveCount'])
        f = np.clip((np.asarray(times) - d['m_BeginTime']) * d['m_SampleRate'], 0, len(arr) - 1)
        lo = np.floor(f).astype(int); hi = np.minimum(lo + 1, len(arr) - 1); w = (f - lo)[:, None]
        cols += list((arr[lo] * (1 - w) + arr[hi] * w).T)
    cols += [[v] * len(times) for v in c]
    return np.array(cols, np.float64).T


def path_names(env):
    """crc32(relative transform path) -> joint name, for every ancestor-relative path in the bundle."""
    tr = {o.path_id: o.read_typetree() for o in env.objects if o.type.name == 'Transform'}
    go = {o.path_id: o.peek_name() for o in env.objects if o.type.name == 'GameObject'}
    name = {k: go.get(v['m_GameObject']['m_PathID'], '') for k, v in tr.items()}
    parent = {k: v['m_Father']['m_PathID'] for k, v in tr.items()}
    out = {}
    for k in tr:
        path, p = name[k], parent[k]
        while p in tr:
            out.setdefault(zlib.crc32(path.encode()), name[k])
            path, p = f'{name[p]}/{path}', parent[p]
        out.setdefault(zlib.crc32(path.encode()), name[k])
    return out


def native_clips(bundle, family):
    """{clip name: (duration, {joint: quats[T,4] glTF xyzw})} for `<family>_*` clips."""
    import UnityPy
    assert UnityPy.__version__ == UNITYPY_VERSION, UnityPy.__version__
    env = UnityPy.load(bundle)
    crc = path_names(env)
    out = {}
    for o in env.objects:
        if o.type.name != 'AnimationClip' or not o.peek_name().startswith(family + '_'):
            continue
        tt = o.read_typetree()
        mc = tt['m_MuscleClip']
        dur = max(mc['m_StopTime'] - mc['m_StartTime'], 1.0 / RATE)
        times = mc['m_StartTime'] + np.arange(int(round(dur * RATE)) + 1) / RATE
        vals = sample_clip(tt, times)
        rots, col = {}, 0
        for b in tt['m_ClipBindingConstant']['genericBindings']:
            w = WIDTH.get(b['attribute'], 1) if b['typeID'] == 4 else 1
            if b['typeID'] == 4 and b['attribute'] == 2 and b['path'] in crc:
                q = vals[:, col:col + 4] * [1, -1, -1, 1]          # Unity (left-handed) -> glTF, as the FBX route
                rots[crc[b['path']]] = q / np.linalg.norm(q, axis=1, keepdims=True)
            col += w
        assert col == vals.shape[1], (o.peek_name(), col, vals.shape)
        out[o.peek_name()] = (dur, rots)
    return out


def add_to_glb(src, dst, clips):
    """Append each clip as a glTF animation (LINEAR rotation channels) to the GLB."""
    from pygltflib import GLTF2, Animation, AnimationSampler, AnimationChannel, AnimationChannelTarget, Accessor, BufferView
    g = GLTF2().load(src)
    blob = bytearray(g.binary_blob())
    node = {n.name: i for i, n in enumerate(g.nodes) if n.name}
    g.animations = [a for a in g.animations if a.name not in clips]
    def put(arr, kind, minmax=False):
        while len(blob) % 4: blob.append(0)
        arr = np.asarray(arr).astype('<f4')   # the view length must be the f4 bytes written, not the caller's (f8) nbytes
        g.bufferViews.append(BufferView(buffer=0, byteOffset=len(blob), byteLength=arr.nbytes))
        blob.extend(arr.tobytes())
        g.accessors.append(Accessor(bufferView=len(g.bufferViews) - 1, componentType=5126, count=len(arr), type=kind,
                                    min=[float(arr.min())] if minmax else None, max=[float(arr.max())] if minmax else None))
        return len(g.accessors) - 1
    for name, (dur, rots) in sorted(clips.items()):
        some = next(iter(rots.values()), None)
        if some is None:
            continue
        t = put(np.arange(len(some)) / RATE, 'SCALAR', True)
        a = Animation(name=name, samplers=[], channels=[])
        for joint, q in sorted(rots.items()):
            if joint in node:
                a.samplers.append(AnimationSampler(input=t, output=put(q, 'VEC4'), interpolation='LINEAR'))
                a.channels.append(AnimationChannel(sampler=len(a.samplers) - 1, target=AnimationChannelTarget(node=node[joint], path='rotation')))
        g.animations.append(a)
    g.buffers[0].byteLength = len(blob)
    g.set_binary_blob(bytes(blob))
    g.save(dst)
    return len(clips)


if __name__ == '__main__':
    bundle, family, src, dst = sys.argv[1:5]
    clips = native_clips(bundle, family)
    print(f'[bake] {family}: {add_to_glb(src, dst, clips)} native clips -> {dst}')
