"""Stock TGL viewer exports -> IR (the calibrated GL-space compatibility frontend)."""
from pathlib import Path
import json

import numpy as np

from .. import ir
from .stock_ase import resolve_texture


def to_ir(source, *, asset_id=None, replaces=None, kind="prop", texture_roots=()):
    source = Path(source)
    dump = json.loads(source.read_text(encoding="utf-8"))
    if dump.get("space") != "gl":
        raise ValueError("stock meshdump must declare space=gl")
    aid = asset_id or Path(dump["tgl"]).stem.lower()
    materials, meshes, nodes = [], [], []
    # Historical calibrated x,-z,y GLB export followed by the importer's -x,-y,z.
    # Keep the stock pivot: ground fitting changes stock placement/pathing alignment.
    matrix = np.array([[-1, 0, 0], [0, 0, 1], [0, 1, 0]], float)
    provenance = [ir.Source.of("stock_meshdump", source, license="stock")]
    for sm in dump["submeshes"]:
        data = np.asarray(sm["verts"], float)
        if not data.size:
            continue
        slot = len(materials)
        texture = sm.get("textureName") or "NULLTXM"
        ref = None
        if texture.upper() != "NULLTXM":
            path = resolve_texture(texture, texture_roots)
            ref = ir.TextureRef.of(path, "base_color", passthrough=True)
            provenance.append(ir.Source("base_color", ref.path, ref.sha256, "stock"))
        materials.append(ir.Material(name=Path(texture).stem, base_color=ref,
                                     alpha_mode="MASK" if texture.lower().startswith("a_") else "OPAQUE"))
        normal = data[:, 3:6] @ matrix.T
        normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-12)
        primitive = ir.Primitive(data[:, :3] @ matrix.T, np.asarray(sm["idx"], np.int64).reshape(-1, 3), slot,
                                 normals=normal, uv0=np.stack([data[:, 6], 1-data[:, 7]], axis=1))
        name = f"{'SpotLight_' if sm.get('isSpotlight') else ''}{aid}_{slot}"
        nodes.append(ir.Node(name, mesh=slot))
        meshes.append(ir.Mesh(name, [primitive]))
    return ir.Asset(aid, kind, replaces or f"staticProp:{aid}", materials,
                    {"intact": [ir.Lod(ir.Scene(nodes, meshes))]}, provenance=provenance,
                    frontend={"name": "stock_meshdump", "collision": {"policy": "stock", "source": dump["tgl"]}}).check()
