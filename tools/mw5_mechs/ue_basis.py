"""ActorX (umodel PSK/PSA) to umodel glTF local bone coordinates.

ActorX exports already reflect UE Y. The remaining basis is a proper
-90 degree X rotation: (x,y,z) -> (x,z,-y). Conjugating non-root ActorX
orientations then gives (x,z,-y,-w), up to quaternion sign. Root orientations
are not conjugated: (x,z,-y,+w). The 121-bone reference independently locks
the child convention; its identity root cannot distinguish root signs.
verify_basis explicitly reports that degeneracy. No facing correction.
"""
import numpy as np


def psa_quat_to_gltf(q, is_root):
    """Map xyzw quaternion(s), with root identified by hierarchy, not name."""
    q = np.asarray(q, dtype=np.float64)[..., [0, 2, 1, 3]].copy()
    q[..., 2] *= -1
    q[..., 3] *= 1 if is_root else -1
    norm = np.linalg.norm(q, axis=-1, keepdims=True)
    if not np.all(np.isfinite(q)) or np.any(norm == 0):
        raise ValueError("invalid ActorX quaternion")
    return q / norm


def psa_pos_to_gltf(p):
    """Map ActorX centimetre position(s) to glTF Y-up metres."""
    return np.asarray(p, dtype=np.float64)[..., [0, 2, 1]] * [0.01, 0.01, -0.01]
