"""The fall clips of the MW5 pack lie the mech on its chest (tools/mw5_mechs/prone_pose.py)."""
import json
import struct
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "mw5_mechs"))
import prone_pose  # noqa: E402
from sockets import load_glb  # noqa: E402

UPRIGHT = [0.0, 0.0, 0.0, 1.0]


def make_glb(path):
    blob, views, accs = bytearray(), [], []

    def add(arr, typ):
        arr = np.ascontiguousarray(arr, np.float32)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": arr.nbytes})
        blob.extend(arr.tobytes())
        accs.append({"bufferView": len(views) - 1, "componentType": 5126, "count": len(arr), "type": typ})
        return len(accs) - 1
    nodes = [{"name": "Root", "children": [1]}, {"name": "Pelvis", "rotation": [-0.5, -0.5, 0.5, 0.5], "children": [2]},
             {"name": "j_COCKPIT", "translation": [3, 0, 0]}]
    clips = []
    for name in ("atlas_moveCoreIdle", "atlas_hitReactKnockdownInpl", "atlas_downedIdle", "atlas_deathKnockdown", "atlas_deathIdle"):
        t = add(np.zeros(1), "SCALAR")
        clips.append({"name": name, "samplers": [{"input": t, "output": add([UPRIGHT], "VEC4")}],
                      "channels": [{"sampler": 0, "target": {"node": 0, "path": "rotation"}}]})
    js = json.dumps({"asset": {"version": "2.0"}, "nodes": nodes, "animations": clips, "accessors": accs, "bufferViews": views,
                     "buffers": [{"byteLength": len(blob)}]}).encode()
    js += b" " * (-len(js) % 4)
    path.write_bytes(struct.pack("<4sII", b"glTF", 2, 28 + len(js) + len(blob)) + struct.pack("<I4s", len(js), b"JSON") + js
                     + struct.pack("<I4s", len(blob), b"BIN\0") + bytes(blob))


def test_fall_clips_pitch_forward_and_ramp(tmp_path):
    src, dst = tmp_path / "in.glb", tmp_path / "out.glb"
    make_glb(src)
    prone_pose.rewrite(src, dst, 85.0)
    doc, buf = load_glb(dst)
    pelvis_bind = np.array([-0.5, -0.5, 0.5, 0.5])
    conj = lambda q: np.array([-q[0], -q[1], -q[2], q[3]])  # noqa: E731
    for name in ("atlas_downedIdle", "atlas_deathIdle"):
        pitch = prone_pose.qmul(prone_pose.clip_rotations(doc, buf, name)[1], conj(pelvis_bind))
        head = prone_pose.qmul(prone_pose.qmul(pitch, [1, 0, 0, 0]), conj(pitch))   # the mech's forward (+X) after the pitch
        assert head[1] == pytest.approx(-np.sin(np.radians(85)), abs=1e-6)   # nose down onto the chest
    clip = next(a for a in doc["animations"] if a["name"] == "atlas_deathKnockdown")
    times = prone_pose.read_accessor(doc, buf, clip["samplers"][0]["input"]).ravel()
    assert len(times) == 3 and times[0] == 0 and times[2] > times[1] > 0   # stand -> airborne -> prone ramp
