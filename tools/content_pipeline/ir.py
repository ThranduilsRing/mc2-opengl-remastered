"""The canonical intermediate representation (IR) for every imported model.

WHAT: one Asset per replaced appearance. It keeps what the lossy legacy paths
dropped: the node hierarchy with local transforms, mesh instancing, normals,
tangents, vertex colours (stock lighting tags), per-slot materials with an
explicit alpha mode and material class, LODs, damage states, skeleton/clip
references for mechs, and the provenance of every source byte.
WHY: frontends (stock ASE, Blender, catalog selection, raw glTF/FBX, BT2018
dumps) used to cook straight to runtime files and disagreed about transforms,
alpha, normals and tiers. They now only produce this; one cooker consumes it.

Conventions (fixed; frontends convert into them):
  space    MC2 TG object space: X lateral, +Y up, Z forward; world units (wu).
  matrix   column-vector 4x4 (p' = M @ [p, 1]), node-local, parent-relative.
  uv       TG texture space: v_tg = 1 - v_gltf (what the engine stores).
  colour   base colour factors are linear; texture colour space comes from
           the texture role (see texcook.ROLES), never from the file name.
"""
from __future__ import annotations

import base64
import hashlib
import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

IR_VERSION = "mc2-content-ir/1"
KINDS = ("prop", "building", "tree", "mech")
STATES = ("intact", "destroyed")
# Building condition variants. "pristine" is the default variant and must stay
# stock-equivalent in silhouette/footprint; the others are opt-in per mission.
CONDITIONS = ("pristine", "worn", "abandoned")
DEFAULT_CONDITION = "pristine"
ALPHA_MODES = ("OPAQUE", "MASK", "BLEND")
# Material class is explicit authoring metadata per part/slot. It supplies the
# metallic/roughness response when no ORM map is authored: metal and roughness
# come from the class (or authored maps), NEVER from colour.
# legacy_prelit: stock appearance, vertex-lit tags, zero new specular.
# Runtime ids (RenderCore/SurfaceMaterial.h) are the tuple index; keep in sync.
MATERIAL_CLASSES = ("legacy_prelit", "dielectric", "painted_metal", "bare_metal", "rust",
                    "concrete", "glass", "grating", "wood", "fabric", "rubber", "emissive")
METALLIC_CLASSES = ("bare_metal", "grating")      # the only classes allowed metallic > 0
# (metallic, roughness) defaults applied when a slot authors no ORM map or scalar.
CLASS_DEFAULTS = {
    "legacy_prelit": (0.0, 1.0), "dielectric": (0.0, 0.8), "painted_metal": (0.0, 0.55),
    "bare_metal": (1.0, 0.45), "rust": (0.0, 0.9), "concrete": (0.0, 0.9), "glass": (0.0, 0.08),
    "grating": (1.0, 0.5), "wood": (0.0, 0.8), "fabric": (0.0, 0.95), "rubber": (0.0, 0.9),
    "emissive": (0.0, 0.9),
}
BASE_COLOR_POLICIES = ("stock", "authored")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path) -> str:
    return sha256_bytes(Path(path).read_bytes())


@dataclass
class Source:
    """One input file, identified by its resolved bytes (never by its name)."""
    role: str                 # e.g. "geometry", "base_color", "normal", "mapping"
    path: str
    sha256: str
    license: str = "unrecorded"
    provider: str = ""
    url: str = ""

    @classmethod
    def of(cls, role, path, **kw):
        return cls(role=role, path=str(path), sha256=sha256_file(path), **kw)


@dataclass
class TextureRef:
    path: str                 # resolved absolute path of the source image
    sha256: str
    role: str                 # texcook.ROLES key
    # Stock pixels pass through byte-for-byte; authored pixels are cooked.
    passthrough: bool = False

    @classmethod
    def of(cls, path, role, passthrough=False):
        return cls(str(path), sha256_file(path), role, passthrough)


@dataclass
class Material:
    name: str
    material_class: str = "legacy_prelit"
    alpha_mode: str = "OPAQUE"
    alpha_cutoff: float = 0.5
    double_sided: bool = False
    base_color_policy: str = "stock"
    base_color: TextureRef | None = None
    base_color_factor: tuple = (1.0, 1.0, 1.0, 1.0)
    normal: TextureRef | None = None
    normal_strength: float = 1.0
    orm: TextureRef | None = None          # R=AO, G=roughness, B=metallic, A=authored coverage
    roughness: float = 1.0
    metallic: float = 0.0
    ao_strength: float = 1.0
    emissive: TextureRef | None = None
    emissive_factor: tuple = (0.0, 0.0, 0.0)
    detail_uv_scale: tuple = (1.0, 1.0)
    reviewed: bool = False
    review_note: str = ""

    def authored(self) -> bool:
        return self.material_class != "legacy_prelit"

    @classmethod
    def for_class(cls, name, material_class, **kw):
        """A slot whose metallic/roughness default from its declared class (never its colour)."""
        if material_class not in CLASS_DEFAULTS:
            raise ValueError(f"{name}: unknown material class {material_class!r}")
        metallic, roughness = CLASS_DEFAULTS[material_class]
        kw.setdefault("metallic", metallic)
        kw.setdefault("roughness", roughness)
        return cls(name=name, material_class=material_class, **kw)


@dataclass
class Primitive:
    positions: np.ndarray                  # (n,3) float
    indices: np.ndarray                    # (m,3) int, counter-clockwise front faces
    material: int
    normals: np.ndarray | None = None      # (n,3) unit
    tangents: np.ndarray | None = None     # (n,4) xyz unit, w = +-1 bitangent sign
    uv0: np.ndarray | None = None          # (n,2) TG texture space
    color0: np.ndarray | None = None       # (n,4) float 0..1 (stock lighting tags)
    uv1: np.ndarray | None = None          # (n,2) authored glass cell IDs


@dataclass
class Mesh:
    name: str
    primitives: list[Primitive] = field(default_factory=list)


@dataclass
class Node:
    name: str
    parent: int = -1
    matrix: np.ndarray = field(default_factory=lambda: np.eye(4))
    mesh: int | None = None                # a mesh shared by several nodes is an instance
    extras: dict = field(default_factory=dict)


@dataclass
class Scene:
    nodes: list[Node] = field(default_factory=list)
    meshes: list[Mesh] = field(default_factory=list)

    def world_matrices(self):
        out = [None] * len(self.nodes)

        def resolve(i, chain=()):
            if out[i] is not None:
                return out[i]
            if i in chain:
                raise ValueError("cyclic node hierarchy")
            node = self.nodes[i]
            parent = np.eye(4) if node.parent < 0 else resolve(node.parent, (*chain, i))
            out[i] = parent @ node.matrix
            return out[i]

        return [resolve(i) for i in range(len(self.nodes))]

    def flattened(self):
        """World-space primitives, one per (node, primitive) instance."""
        result = []
        for node, world in zip(self.nodes, self.world_matrices()):
            if node.mesh is None:
                continue
            for prim in self.meshes[node.mesh].primitives:
                result.append((node.name, transform_primitive(prim, world)))
        return result

    def triangle_count(self):
        return sum(len(p.indices) for _, p in self.flattened())

    def bounds(self):
        points = [p.positions for _, p in self.flattened()]
        if not points:
            raise ValueError("scene has no geometry")
        allp = np.concatenate(points)
        return allp.min(axis=0), allp.max(axis=0)


@dataclass
class Lod:
    scene: Scene
    distance: float = 0.0


@dataclass
class SkeletonRef:
    """Mechs: skeleton and clips stay in their source files; the IR records identity.

    clip_events: clip name -> [(time_seconds, event)] e.g. foot_down_l, fire_ra,
    land; the runtime clock drives effects from these instead of legacy frames.
    """
    source: Source
    joints: list[str] = field(default_factory=list)
    clips: list[str] = field(default_factory=list)
    clip_events: dict[str, list] = field(default_factory=dict)


SOCKET_KINDS = ("muzzle", "foot", "jumpjet", "eject", "smoke", "attach", "light")


@dataclass
class Socket:
    """A named attachment frame: actor x posed parent joint/node x this local matrix."""
    name: str
    kind: str
    parent: str                            # joint or node name in the intact LOD0 / skeleton
    matrix: np.ndarray = field(default_factory=lambda: np.eye(4))
    properties: dict = field(default_factory=dict)


@dataclass
class Part:
    """A damageable region: the nodes that render it intact, the nodes that replace it
    when destroyed (in the "destroyed" state scene, if any), and its mounts."""
    name: str
    region: str                            # e.g. "left_arm", "center_torso", "roof"
    nodes: list[str] = field(default_factory=list)
    destroyed_nodes: list[str] = field(default_factory=list)
    sockets: list[str] = field(default_factory=list)


@dataclass
class Asset:
    id: str
    kind: str
    replaces: str                          # "staticProp:<lod0 key>", "mech:<chassis>", ...
    materials: list[Material] = field(default_factory=list)
    # Default (pristine) variant: state -> LOD chain (LOD0 first). "destroyed" is
    # optional; absent = the engine keeps the stock damage shape.
    states: dict[str, list[Lod]] = field(default_factory=dict)
    # Optional shadow-caster stand-ins per state (absent = the render shape casts).
    shadows: dict[str, Scene] = field(default_factory=dict)
    # Non-default condition variants: condition -> state -> LOD chain. Each carries
    # an intact LOD0; a missing destroyed state falls back to the default variant's.
    conditions: dict[str, dict[str, list[Lod]]] = field(default_factory=dict)

    def variants(self):
        """(condition, state, lods) for every chain, default variant first."""
        out = [("pristine", st, lods) for st, lods in self.states.items()]
        for cond, states in self.conditions.items():
            out += [(cond, st, lods) for st, lods in states.items()]
        return out
    skeleton: SkeletonRef | None = None
    provenance: list[Source] = field(default_factory=list)
    frontend: dict = field(default_factory=dict)
    sockets: list[Socket] = field(default_factory=list)
    parts: list[Part] = field(default_factory=list)

    def lod0(self, state="intact") -> Scene:
        return self.states[state][0].scene

    def check(self):
        """Structural invariants every frontend must satisfy (raises ValueError)."""
        if self.kind not in KINDS:
            raise ValueError(f"{self.id}: unknown kind {self.kind!r}")
        if "intact" not in self.states or not self.states["intact"]:
            raise ValueError(f"{self.id}: intact LOD0 required")
        for cond, states in self.conditions.items():
            if cond not in CONDITIONS or cond == DEFAULT_CONDITION:
                raise ValueError(f"{self.id}: unknown or default condition {cond!r}")
            if not states.get("intact"):
                raise ValueError(f"{self.id}: condition {cond} needs an intact LOD0")
        for state in self.shadows:
            if state not in STATES:
                raise ValueError(f"{self.id}: unknown shadow state {state!r}")
        scenes = list(self.shadows.values())
        for _, state, lods in self.variants():
            if state not in STATES:
                raise ValueError(f"{self.id}: unknown state {state!r}")
            scenes += [lod.scene for lod in lods]
        for scene in scenes:
            for node in scene.nodes:
                if node.mesh is not None and not 0 <= node.mesh < len(scene.meshes):
                    raise ValueError(f"{self.id}: node {node.name} references missing mesh")
                if node.parent >= len(scene.nodes):
                    raise ValueError(f"{self.id}: node {node.name} has a missing parent")
                if not np.isfinite(node.matrix).all():
                    raise ValueError(f"{self.id}: node {node.name} has a nonfinite transform")
            scene.world_matrices()
            for mesh in scene.meshes:
                for prim in mesh.primitives:
                    check_primitive(self.id, mesh.name, prim, len(self.materials))
        names = {n.name for _, _, lods in self.variants() for lod in lods for n in lod.scene.nodes}
        if self.skeleton:
            names |= set(self.skeleton.joints)
        socket_names = set()
        for s in self.sockets:
            if s.kind not in SOCKET_KINDS or s.name in socket_names:
                raise ValueError(f"{self.id}: socket {s.name} has an unknown kind or a duplicate name")
            if s.parent not in names:
                raise ValueError(f"{self.id}: socket {s.name} parent {s.parent!r} is not a node/joint")
            socket_names.add(s.name)
        for p in self.parts:
            missing = [n for n in p.nodes + p.destroyed_nodes if n not in names]
            if missing or any(s not in socket_names for s in p.sockets):
                raise ValueError(f"{self.id}: part {p.name} references unknown nodes/sockets {missing}")
        for m in self.materials:
            if m.material_class not in MATERIAL_CLASSES:
                raise ValueError(f"{self.id}/{m.name}: unknown material class {m.material_class!r}")
            if m.alpha_mode not in ALPHA_MODES:
                raise ValueError(f"{self.id}/{m.name}: unknown alpha mode {m.alpha_mode!r}")
            if m.base_color_policy not in BASE_COLOR_POLICIES:
                raise ValueError(f"{self.id}/{m.name}: unknown base colour policy")
            if m.authored() and not m.reviewed:
                raise ValueError(f"{self.id}/{m.name}: authored class requires a reviewed mapping")
            if m.material_class not in METALLIC_CLASSES and m.metallic != 0:
                raise ValueError(f"{self.id}/{m.name}: class {m.material_class} cannot be metallic")
            if not m.authored() and (m.normal or m.orm):
                raise ValueError(f"{self.id}/{m.name}: legacy_prelit carries no authored maps")
            for v in (m.roughness, m.metallic, m.ao_strength):
                if not 0 <= v <= 1:
                    raise ValueError(f"{self.id}/{m.name}: material scalar outside 0..1")
            if not 0 <= m.normal_strength <= 2:
                raise ValueError(f"{self.id}/{m.name}: normal strength outside 0..2")
        return self


def check_primitive(asset_id, mesh_name, prim, material_count):
    where = f"{asset_id}/{mesh_name}"
    n = len(prim.positions)
    if prim.positions.ndim != 2 or prim.positions.shape[1] != 3 or n == 0:
        raise ValueError(f"{where}: positions must be (n,3)")
    if prim.indices.ndim != 2 or prim.indices.shape[1] != 3 or len(prim.indices) == 0:
        raise ValueError(f"{where}: indices must be (m,3)")
    if prim.indices.min() < 0 or prim.indices.max() >= n:
        raise ValueError(f"{where}: index out of range")
    if not 0 <= prim.material < material_count:
        raise ValueError(f"{where}: material slot {prim.material} missing")
    for name, arr, width in (("normals", prim.normals, 3), ("tangents", prim.tangents, 4),
                             ("uv0", prim.uv0, 2), ("uv1", prim.uv1, 2), ("color0", prim.color0, 4)):
        if arr is not None and (arr.shape != (n, width) or not np.isfinite(arr).all()):
            raise ValueError(f"{where}: {name} must be finite ({n},{width})")
    if not np.isfinite(prim.positions).all():
        raise ValueError(f"{where}: nonfinite positions")
    if prim.normals is not None and np.any(np.abs(np.linalg.norm(prim.normals, axis=1) - 1) > 1e-3):
        raise ValueError(f"{where}: normals must be unit length")


def transform_primitive(prim: Primitive, matrix) -> Primitive:
    """Bake a node transform: positions affine, normals inverse-transpose, tangents linear."""
    linear = matrix[:3, :3]
    det = float(np.linalg.det(linear))
    if abs(det) < 1e-12:
        raise ValueError("singular node transform")
    positions = prim.positions @ linear.T + matrix[:3, 3]
    normals = tangents = None
    if prim.normals is not None:
        normals = prim.normals @ np.linalg.inv(linear)
        normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-20)
    if prim.tangents is not None:
        t = prim.tangents[:, :3] @ linear.T
        t /= np.maximum(np.linalg.norm(t, axis=1, keepdims=True), 1e-20)
        tangents = np.concatenate([t, prim.tangents[:, 3:4] * np.sign(det)], axis=1)
    indices = prim.indices if det > 0 else prim.indices[:, [0, 2, 1]]   # a mirror keeps faces front-facing
    return Primitive(positions, indices.copy(), prim.material, normals, tangents,
                     None if prim.uv0 is None else prim.uv0.copy(),
                     None if prim.color0 is None else prim.color0.copy(),
                     None if prim.uv1 is None else prim.uv1.copy())


def _ray_hits(origins, dirs, tris, chunk=256):
    """Any-hit Moller-Trumbore: for each ray, does it hit any triangle at t > 0?"""
    v0, e1, e2 = tris[:, 0], tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0]
    out = np.zeros(len(origins), dtype=bool)
    for s0 in range(0, len(origins), chunk):
        o, d = origins[s0:s0 + chunk, None, :], dirs[s0:s0 + chunk, None, :]
        pvec = np.cross(d, e2[None])
        det = np.einsum("rtk,tk->rt", pvec, e1)
        ok = np.abs(det) > 1e-12
        inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
        tvec = o - v0[None]
        u = np.einsum("rtk,rtk->rt", tvec, pvec) * inv
        qvec = np.cross(tvec, e1[None])
        v = np.einsum("rtk,rtk->rt", d, qvec) * inv
        t = np.einsum("tk,rtk->rt", e2, qvec) * inv
        hit = ok & (u >= 0) & (v >= 0) & (u + v <= 1) & (t > 1e-3)
        out[s0:s0 + chunk] = hit.any(axis=1)
    return out


def inward_hull_faces(scene: Scene, min_area: float = 1.0):
    """Outer-skin triangles that face into the building.

    A building's skin must face out; an inverted one is back-face culled from outside and the wall
    reads as a hole (the sci-fi kit hangar's gables, inset from the bounding box). For each face
    of at least min_area, rays leave from three interior points along +normal and -normal: when
    the front side is enclosed (every +normal ray hits the asset) while the back side escapes
    (most -normal rays reach open space), the face is inverted. Returns
    [(node index, primitive index, triangle indices)]."""
    worlds = scene.world_matrices()
    items, all_tris = [], []
    for ni, node in enumerate(scene.nodes):
        if node.mesh is None:
            continue
        for pi, prim in enumerate(scene.meshes[node.mesh].primitives):
            p = transform_primitive(prim, worlds[ni])
            t = p.positions[p.indices]
            items.append((ni, pi, t))
            all_tris.append(t)
    if not all_tris:
        return []
    every = np.concatenate(all_tris)
    found = []
    weights = np.array([[0.6, 0.2, 0.2], [0.2, 0.6, 0.2], [0.2, 0.2, 0.6]])
    for ni, pi, t in items:
        n = np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0])
        area = np.linalg.norm(n, axis=1) / 2
        cand = np.nonzero(area >= min_area)[0]
        if not len(cand):
            continue
        n = n[cand] / np.maximum(2 * area[cand, None], 1e-20)
        pts = np.einsum("sk,tkd->tsd", weights, t[cand]).reshape(-1, 3)
        dirs = np.repeat(n, 3, axis=0)
        offset = 0.01 * dirs
        front = _ray_hits(pts + offset, dirs, every).reshape(-1, 3)
        back = _ray_hits(pts - offset, -dirs, every).reshape(-1, 3)
        bad = front.all(axis=1) & ((~back).sum(axis=1) >= 2)
        if bad.any():
            found.append((ni, pi, cand[bad]))
    return found


def flip_faces(scene: Scene, faces):
    """Reverse winding (and the vertex normals they use) of the listed triangles."""
    for ni, pi, tris in faces:
        prim = scene.meshes[scene.nodes[ni].mesh].primitives[pi]
        prim.indices[tris] = prim.indices[tris][:, [0, 2, 1]]
        if prim.normals is not None:
            others = np.setdiff1d(np.arange(len(prim.indices)), tris)
            exclusive = np.setdiff1d(np.unique(prim.indices[tris]), np.unique(prim.indices[others]))
            prim.normals[exclusive] = -prim.normals[exclusive]   # shared vertices keep their smooth normal
    return sum(len(t) for _, _, t in faces)


# --------------------------------------------------------------------------- JSON

def _arr(a):
    if a is None:
        return None
    a = np.ascontiguousarray(a)
    dtype = "<f4" if a.dtype.kind == "f" else "<u4"
    return {"dtype": dtype, "shape": list(a.shape),
            "b64": base64.b64encode(a.astype(dtype).tobytes()).decode("ascii")}


def _unarr(d):
    if d is None:
        return None
    a = np.frombuffer(base64.b64decode(d["b64"]), dtype=d["dtype"]).reshape(d["shape"])
    return a.astype(np.float64 if d["dtype"] == "<f4" else np.int64)


def _tex(t):
    return None if t is None else {"path": t.path, "sha256": t.sha256, "role": t.role, "passthrough": t.passthrough}


def _untex(d):
    return None if d is None else TextureRef(**d)


def to_json(asset: Asset) -> dict:
    def scene(s):
        return {"nodes": [{"name": n.name, "parent": n.parent, "matrix": np.asarray(n.matrix).reshape(-1).tolist(),
                           "mesh": n.mesh, "extras": n.extras} for n in s.nodes],
                "meshes": [{"name": m.name, "primitives": [
                    {"material": p.material, "positions": _arr(p.positions), "indices": _arr(p.indices),
                     "normals": _arr(p.normals), "tangents": _arr(p.tangents), "uv0": _arr(p.uv0),
                     "color0": _arr(p.color0), "uv1": _arr(p.uv1)} for p in m.primitives]} for m in s.meshes]}
    mats = []
    for m in asset.materials:
        d = dict(m.__dict__)
        for k in ("base_color", "normal", "orm", "emissive"):
            d[k] = _tex(d[k])
        for k in ("base_color_factor", "emissive_factor", "detail_uv_scale"):
            d[k] = list(d[k])
        mats.append(d)
    def chain(states):
        return {k: [{"distance": l.distance, "scene": scene(l.scene)} for l in v] for k, v in states.items()}
    return {"version": IR_VERSION, "id": asset.id, "kind": asset.kind, "replaces": asset.replaces,
            "materials": mats, "states": chain(asset.states),
            "shadows": {k: scene(v) for k, v in asset.shadows.items()},
            "conditions": {c: chain(v) for c, v in asset.conditions.items()},
            "skeleton": None if asset.skeleton is None else {
                "source": asset.skeleton.source.__dict__, "joints": asset.skeleton.joints, "clips": asset.skeleton.clips,
                "clip_events": asset.skeleton.clip_events},
            "sockets": [{"name": s.name, "kind": s.kind, "parent": s.parent,
                         "matrix": np.asarray(s.matrix).reshape(-1).tolist(), "properties": s.properties} for s in asset.sockets],
            "parts": [p.__dict__ for p in asset.parts],
            "provenance": [s.__dict__ for s in asset.provenance], "frontend": asset.frontend}


def from_json(d: dict) -> Asset:
    if d.get("version") != IR_VERSION:
        raise ValueError(f"unsupported IR version {d.get('version')!r}")

    def scene(s):
        return Scene(
            nodes=[Node(n["name"], n["parent"], np.array(n["matrix"], dtype=float).reshape(4, 4), n["mesh"],
                        n.get("extras", {})) for n in s["nodes"]],
            meshes=[Mesh(m["name"], [Primitive(_unarr(p["positions"]), _unarr(p["indices"]), p["material"],
                                               _unarr(p["normals"]), _unarr(p["tangents"]), _unarr(p["uv0"]),
                                               _unarr(p["color0"]), _unarr(p.get("uv1"))) for p in m["primitives"]]) for m in s["meshes"]])
    mats = []
    for m in d["materials"]:
        m = dict(m)
        for k in ("base_color", "normal", "orm", "emissive"):
            m[k] = _untex(m[k])
        for k in ("base_color_factor", "emissive_factor", "detail_uv_scale"):
            m[k] = tuple(m[k])
        mats.append(Material(**m))
    sk = d.get("skeleton")

    def chain(states):
        return {k: [Lod(scene(l["scene"]), l["distance"]) for l in v] for k, v in states.items()}
    return Asset(id=d["id"], kind=d["kind"], replaces=d["replaces"], materials=mats, states=chain(d["states"]),
                 shadows={k: scene(v) for k, v in d.get("shadows", {}).items()},
                 conditions={c: chain(v) for c, v in d.get("conditions", {}).items()},
                 skeleton=None if sk is None else SkeletonRef(Source(**sk["source"]), sk["joints"], sk["clips"],
                                                              sk.get("clip_events", {})),
                 provenance=[Source(**s) for s in d["provenance"]], frontend=d.get("frontend", {}),
                 sockets=[Socket(s["name"], s["kind"], s["parent"], np.array(s["matrix"], dtype=float).reshape(4, 4),
                                 s.get("properties", {}))
                          for s in d.get("sockets", [])],
                 parts=[Part(**p) for p in d.get("parts", [])])


def save(asset: Asset, path):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(to_json(asset), indent=1, sort_keys=True) + "\n", encoding="utf-8")
    tmp.replace(path)


def load(path) -> Asset:
    return from_json(json.loads(Path(path).read_text(encoding="utf-8")))


def fingerprint(asset: Asset) -> str:
    """Content identity of the whole IR (geometry, materials, source bytes)."""
    return sha256_bytes(json.dumps(to_json(asset), sort_keys=True).encode("utf-8"))
