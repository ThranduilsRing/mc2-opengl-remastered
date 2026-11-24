"""Bake the owner's BT2018 override clips, using Avatar paths, as rotation-only GLB data.

Decoder follows the verified bt-recon streamed/dense/constant and Avatar m_TOS route.
Install requirements.txt in this directory; bundles are strictly read-only inputs.
"""
import struct
import sys
from pathlib import Path

import numpy as np

UNITYPY_VERSION = '1.25.4'
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
    """Unity's authoritative binding hash -> joint name (not guessed Transform CRCs)."""
    return {h: p.rsplit('/', 1)[-1] for o in env.objects if o.type.name == 'Avatar'
            for h, p in o.read_typetree()['m_TOS'] if p}


def decode_clip(tt, paths):
    mc = tt['m_MuscleClip']
    start, stop = mc['m_StartTime'], mc['m_StopTime']
    dur = max(stop - start, 1.0 / RATE)
    times = np.linspace(start, stop, int(np.ceil(dur * RATE)) + 1)
    vals = sample_clip(tt, times)
    rots, col = {}, 0
    for b in tt['m_ClipBindingConstant']['genericBindings']:
        w = WIDTH.get(b['attribute'], 1) if b['typeID'] == 4 else 1
        # Bindings for helper objects outside the Avatar are not exported GLB joints.
        if b['typeID'] == 4 and b['attribute'] == 2 and b['path'] in paths:
            q = vals[:, col:col + 4] * [1, -1, -1, 1]
            norm = np.linalg.norm(q, axis=1, keepdims=True)
            if not np.isfinite(q).all() or np.any(norm < 1e-8):
                raise ValueError(f"{tt['m_Name']}: invalid quaternion at {paths[b['path']]}")
            q /= norm
            for i in range(1, len(q)):
                if np.dot(q[i - 1], q[i]) < 0:
                    q[i] *= -1
            rots[paths[b['path']]] = q
        col += w
    if col != vals.shape[1] or not rots:
        raise ValueError(f"{tt['m_Name']}: bindings {col}, curves {vals.shape[1]}, joints {len(rots)}")
    return dur, rots


def native_clips(bundle, family):
    """{atlas state: (duration, {joint: quats[T,4] glTF xyzw})} from the override controller."""
    import UnityPy
    assert UnityPy.__version__ == UNITYPY_VERSION, UnityPy.__version__
    env = UnityPy.load(bundle)
    paths = path_names(env)
    controllers = [o for o in env.objects if o.type.name == 'AnimatorOverrideController'
                   and o.peek_name().lower() == f'charoverctrl_{family}base-001']
    clips = {}
    if controllers:
        for entry in controllers[0].read().m_Clips:
            if not entry.m_OverrideClip.path_id:
                continue  # An unoverridden state belongs to the Atlas fallback.
            original = entry.m_OriginalClip.deref()
            override = entry.m_OverrideClip.deref()
            clips[original.peek_name()] = override
    elif family == 'atlas':
        clips = {o.peek_name(): o for o in env.objects if o.type.name == 'AnimationClip'
                 and o.peek_name().startswith('atlas_')}
    decoded = {}
    for name, obj in clips.items():
        if not name.startswith('atlas_'):
            raise ValueError(f'{family}: unexpected state {name}')
        key = obj.path_id
        if key not in decoded:
            decoded[key] = decode_clip(obj.read_typetree(), paths)
        clips[name] = decoded[key]
    return clips


def chassis_clips(bundles, family):
    """Search the base and DLC bundles for the chassis's own override states."""
    raw = Path(bundles)
    for name in (f'chrprfmech_{family}base-001', 'flashpoint', 'heavymetal', 'urbanwarfare', 'shadowhawkdlc'):
        bundle = raw / name
        if bundle.is_file():
            clips = native_clips(str(bundle), family)
            if clips:
                return clips
    return {}


def add_to_glb(src, dst, clips):
    """Append each clip as a glTF animation (LINEAR rotation channels) to the GLB."""
    from pygltflib import GLTF2, Animation, AnimationSampler, AnimationChannel, AnimationChannelTarget, Accessor, BufferView
    g = GLTF2().load(src)
    blob = bytearray(g.binary_blob())
    node = {n.name: i for i, n in enumerate(g.nodes) if n.name}
    for name, (_, rots) in clips.items():
        if not (rots.keys() & node.keys()):
            raise ValueError(f'{name}: no Avatar joints match the GLB')
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
        t = put(np.linspace(0, dur, len(some)), 'SCALAR', True)
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
