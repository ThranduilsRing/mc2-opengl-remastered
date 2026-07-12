#!/usr/bin/env python3
"""
ase_to_glb.py -- Convert MC2 ASE mesh to GLB + mcasset.json sidecar.

Usage:
    python tools/ase_to_glb.py --ase PATH.ase --ini PATH.ini --out-glb OUTPUT.glb --out-sidecar OUTPUT.mcasset.json

Axis mapping: from mclib/assimp_importer.cpp (case 0, default MC2_GLTF_AXIS=0):
    gltf_x = -ase_x
    gltf_y = -ase_y
    gltf_z =  ase_z
UV V-flip: gltf_v = 1.0 - ase_v

The transform (-x,-y,z) is self-inverse so the same formula applies for
ASE->GLB (export) as for GLB->MC2 (import).

Stdlib only: no pip dependencies.
"""
import argparse
import json
import math
import os
import re
import sys
import struct
import sys

__version__ = '1.1.0'


# ---------------------------------------------------------------------------
# Axis transform  (matches assimp_importer.cpp axisMap case 0)
# ---------------------------------------------------------------------------

def ase_to_gltf_pos(x, y, z):
    return (-x, -y, z)


def ase_to_gltf_normal(nx, ny, nz):
    # Rotation only (no translation), same axis swap, no flip for normals
    return (-nx, -ny, nz)


def ase_to_gltf_uv(u, v):
    return (u, 1.0 - v)


# ---------------------------------------------------------------------------
# INI parser (FITini format)
# ---------------------------------------------------------------------------

def parse_fitini(path):
    sections = {}
    current = None
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith('//') or line in ('FITini', 'FITend'):
                continue
            m = re.match(r'^\[([^\]]+)\]$', line)
            if m:
                current = m.group(1)
                sections.setdefault(current, {})
                continue
            if current is None:
                continue
            m = re.match(r'^(\w+)\s+(\w[\w\s]*?)\s*=\s*(.+)$', line)
            if m:
                key = m.group(2).strip()
                val = m.group(3).strip().strip('"')
                sections[current][key] = val
    return sections


def ini_int(sections, section, key, default=0):
    try:
        return int(sections.get(section, {}).get(key, default))
    except (ValueError, TypeError):
        return default


def ini_float(sections, section, key, default=0.0):
    try:
        return float(sections.get(section, {}).get(key, default))
    except (ValueError, TypeError):
        return default


def ini_str(sections, section, key, default=''):
    return sections.get(section, {}).get(key, default)


# ---------------------------------------------------------------------------
# ASE parser — full multi-GEOMOBJECT support
# ---------------------------------------------------------------------------

class AseHelperObject:
    """One HELPEROBJECT block from an ASE file (joint/dummy/hardpoint node)."""
    def __init__(self):
        self.node_name = ''
        self.parent_name = ''
        self.translation = (0.0, 0.0, 0.0)  # TM_POS in MC2 space


class AseGeomObject:
    """One GEOMOBJECT block from an ASE file."""
    def __init__(self):
        self.node_name = ''
        self.parent_name = ''
        # Raw arrays
        self.positions = []   # list of (x,y,z)
        self.tverts = []      # list of (u,v)
        self.faces = []       # list of [ai,bi,ci,mtlid]
        self.tfaces = []      # list of [tai,tbi,tci]
        self.normals = {}     # dict face_idx -> (nx,ny,nz) face normal
        self.vnormals = {}    # dict (face_idx,vert_idx) -> (nx,ny,nz) vertex normal
        self.mat_ref = 0      # material reference index (into global material list)


class AseMaterial:
    def __init__(self):
        self.name = ''
        self.bitmap = ''      # basename of diffuse texture


def parse_ase(path):
    """
    Parse an ASE file.
    Returns (list[AseGeomObject], list[AseMaterial], list[AseHelperObject]).
    """
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        lines = [l.rstrip('\n').rstrip('\r') for l in f]

    materials = []
    geom_objects = []
    helper_objects = []

    i = 0
    n = len(lines)

    def peek():
        return lines[i].strip() if i < n else ''

    def skip_block():
        """Skip to matching closing brace from current position (expects '{' already consumed)."""
        nonlocal i
        depth = 1
        while i < n and depth > 0:
            l = lines[i].strip()
            i += 1
            depth += l.count('{') - l.count('}')

    # ---- Parse material list ----
    while i < n:
        l = lines[i].strip()
        if l == '*MATERIAL_LIST {':
            i += 1
            # Parse material count then each material
            mat_count = 0
            while i < n:
                l2 = lines[i].strip()
                if l2 == '}':
                    i += 1
                    break
                m = re.match(r'^\*MATERIAL_COUNT\s+(\d+)', l2)
                if m:
                    mat_count = int(m.group(1))
                    i += 1
                    continue
                m = re.match(r'^\*MATERIAL\s+\d+\s+\{', l2)
                if m:
                    mat = AseMaterial()
                    i += 1
                    depth = 1
                    warned_submat = False
                    while i < n and depth > 0:
                        ml = lines[i].strip()
                        i += 1
                        depth += ml.count('{') - ml.count('}')
                        if ml.startswith('*SUBMATERIAL') and not warned_submat:
                            warned_submat = True
                            # Known limitation: nested *SUBMATERIAL name/bitmap
                            # lines overwrite the outer material's (last one
                            # wins) and MESH_MTLID is not remapped through the
                            # submaterial table — per-face textures on
                            # Multi/Sub-Object assets are unreliable.
                            sys.stderr.write(
                                "[ase_to_glb] WARNING: Multi/Sub-Object material "
                                "detected (*SUBMATERIAL) — per-face submaterial "
                                "textures are NOT fully supported; verify output "
                                "texturing.\n")
                        mm = re.match(r'^\*MATERIAL_NAME\s+"([^"]*)"', ml)
                        if mm:
                            mat.name = mm.group(1)
                        mm = re.match(r'^\*BITMAP\s+"([^"]*)"', ml)
                        if mm:
                            mat.bitmap = os.path.basename(mm.group(1).replace('\\', '/'))
                    materials.append(mat)
                    continue
                i += 1
            break
        i += 1

    # ---- Parse HELPEROBJECT and GEOMOBJECT blocks ----
    i = 0
    while i < n:
        l = lines[i].strip()
        if l == '*HELPEROBJECT {':
            helper = AseHelperObject()
            i += 1
            depth = 1
            while i < n and depth > 0:
                l2 = lines[i].strip()
                i += 1
                depth += l2.count('{') - l2.count('}')
                m = re.match(r'^\*NODE_NAME\s+"([^"]*)"', l2)
                if m and not helper.node_name:
                    helper.node_name = m.group(1)
                    continue
                m = re.match(r'^\*NODE_PARENT\s+"([^"]*)"', l2)
                if m:
                    helper.parent_name = m.group(1)
                    continue
                # TM_POS gives the world-space translation
                m = re.match(r'^\*TM_POS\s+([\-\d\.eE+]+)\s+([\-\d\.eE+]+)\s+([\-\d\.eE+]+)', l2)
                if m:
                    helper.translation = (float(m.group(1)), float(m.group(2)), float(m.group(3)))
            if helper.node_name:
                helper_objects.append(helper)
            continue

        if l == '*GEOMOBJECT {':
            obj = AseGeomObject()
            i += 1
            depth = 1
            in_mesh = False
            in_tverts = False
            in_tfaces = False
            in_faces = False
            in_normals = False
            cur_face_idx = -1

            while i < n and depth > 0:
                raw = lines[i]
                l2 = raw.strip()
                i += 1

                open_b = l2.count('{')
                close_b = l2.count('}')
                depth += open_b - close_b

                # Node name / parent
                m = re.match(r'^\*NODE_NAME\s+"([^"]*)"', l2)
                if m and not obj.node_name:
                    obj.node_name = m.group(1)
                    continue

                m = re.match(r'^\*NODE_PARENT\s+"([^"]*)"', l2)
                if m:
                    obj.parent_name = m.group(1)
                    continue

                m = re.match(r'^\*MATERIAL_REF\s+(\d+)', l2)
                if m:
                    obj.mat_ref = int(m.group(1))
                    continue

                # Block transitions
                if '*MESH {' in l2:
                    in_mesh = True
                    continue
                if in_mesh and '*MESH_TVERTLIST {' in l2:
                    in_tverts = True
                    continue
                if in_mesh and in_tverts and l2 == '}':
                    in_tverts = False
                    continue
                if in_mesh and '*MESH_TFACELIST {' in l2:
                    in_tfaces = True
                    continue
                if in_mesh and in_tfaces and l2 == '}':
                    in_tfaces = False
                    continue
                if in_mesh and '*MESH_FACE_LIST {' in l2:
                    in_faces = True
                    continue
                if in_mesh and in_faces and l2 == '}':
                    in_faces = False
                    continue
                if in_mesh and '*MESH_NORMALS {' in l2:
                    in_normals = True
                    continue
                if in_mesh and in_normals and l2 == '}':
                    in_normals = False
                    continue

                if not in_mesh:
                    continue

                # Vertices
                m = re.match(r'^\*MESH_VERTEX\s+\d+\s+([\-\d\.eE+]+)\s+([\-\d\.eE+]+)\s+([\-\d\.eE+]+)', l2)
                if m and not in_tverts and not in_normals:
                    obj.positions.append((float(m.group(1)), float(m.group(2)), float(m.group(3))))
                    continue

                # TVerts
                if in_tverts:
                    m = re.match(r'^\*MESH_TVERT\s+\d+\s+([\-\d\.eE+]+)\s+([\-\d\.eE+]+)', l2)
                    if m:
                        obj.tverts.append((float(m.group(1)), float(m.group(2))))
                    continue

                # Faces
                if in_faces:
                    # *MESH_FACE N:    A: i B: i C: i ... *MESH_MTLID m
                    m = re.match(r'^\*MESH_FACE\s+(\d+):\s+A:\s*(\d+)\s+B:\s*(\d+)\s+C:\s*(\d+)', l2)
                    if m:
                        fidx = int(m.group(1))
                        ai, bi, ci = int(m.group(2)), int(m.group(3)), int(m.group(4))
                        mtl = 0
                        mm = re.search(r'\*MESH_MTLID\s+(\d+)', l2)
                        if mm:
                            mtl = int(mm.group(1))
                        # Extend faces list to handle out-of-order
                        while len(obj.faces) <= fidx:
                            obj.faces.append([0, 0, 0, 0])
                        obj.faces[fidx] = [ai, bi, ci, mtl]
                    continue

                # TFaces
                if in_tfaces:
                    m = re.match(r'^\*MESH_TFACE\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)', l2)
                    if m:
                        fidx = int(m.group(1))
                        ta, tb, tc = int(m.group(2)), int(m.group(3)), int(m.group(4))
                        while len(obj.tfaces) <= fidx:
                            obj.tfaces.append([0, 0, 0])
                        obj.tfaces[fidx] = [ta, tb, tc]
                    continue

                # Normals
                if in_normals:
                    m = re.match(r'^\*MESH_FACENORMAL\s+(\d+)\s+([\-\d\.eE+]+)\s+([\-\d\.eE+]+)\s+([\-\d\.eE+]+)', l2)
                    if m:
                        cur_face_idx = int(m.group(1))
                        obj.normals[cur_face_idx] = (float(m.group(2)), float(m.group(3)), float(m.group(4)))
                        continue
                    m = re.match(r'^\*MESH_VERTEXNORMAL\s+(\d+)\s+([\-\d\.eE+]+)\s+([\-\d\.eE+]+)\s+([\-\d\.eE+]+)', l2)
                    if m and cur_face_idx >= 0:
                        vi = int(m.group(1))
                        obj.vnormals[(cur_face_idx, vi)] = (float(m.group(2)), float(m.group(3)), float(m.group(4)))
                    continue

            geom_objects.append(obj)
        else:
            i += 1

    return geom_objects, materials, helper_objects


# ---------------------------------------------------------------------------
# Build expanded vertex buffer for one mesh object
# ---------------------------------------------------------------------------

def build_expanded_mesh(obj, materials):
    """
    Expand (position_idx, uv_idx) pairs per face into a flat vertex buffer.
    Returns:
        positions_out  list of (gx,gy,gz)  -- glTF transformed
        normals_out    list of (gnx,gny,gnz)
        uvs_out        list of (u, gv)
        indices_out    list of int (one per triangle vertex)
        mtl_groups     dict mtl_id -> list of face indices using that material
    """
    # Map (pos_idx, uv_idx) -> output vertex index
    vertex_map = {}
    positions_out = []
    normals_out = []
    uvs_out = []
    indices_out = []
    mtl_groups = {}

    def compute_face_normal(ai, bi, ci, positions):
        ax, ay, az = positions[ai]
        bx, by, bz = positions[bi]
        cx, cy, cz = positions[ci]
        # Edge vectors
        ux, uy, uz = bx-ax, by-ay, bz-az
        vx, vy, vz = cx-ax, cy-ay, cz-az
        # Cross product
        nx = uy*vz - uz*vy
        ny = uz*vx - ux*vz
        nz = ux*vy - uy*vx
        length = math.sqrt(nx*nx + ny*ny + nz*nz)
        if length > 1e-12:
            nx, ny, nz = nx/length, ny/length, nz/length
        return nx, ny, nz

    for fidx, face in enumerate(obj.faces):
        ai, bi, ci, mtlid = face

        # UVs
        if fidx < len(obj.tfaces):
            ta, tb, tc = obj.tfaces[fidx]
        else:
            ta, tb, tc = 0, 0, 0

        # Track material groups
        mtl_groups.setdefault(mtlid, []).append(fidx)

        for vi, (pos_i, uv_i) in enumerate([(ai, ta), (bi, tb), (ci, tc)]):
            key = (pos_i, uv_i)
            if key not in vertex_map:
                # Position
                if pos_i < len(obj.positions):
                    px, py, pz = obj.positions[pos_i]
                else:
                    px, py, pz = 0.0, 0.0, 0.0
                gx, gy, gz = ase_to_gltf_pos(px, py, pz)
                positions_out.append((gx, gy, gz))

                # Normal: prefer vertex normal from normals block
                if (fidx, pos_i) in obj.vnormals:
                    nx, ny, nz = obj.vnormals[(fidx, pos_i)]
                elif fidx in obj.normals:
                    nx, ny, nz = obj.normals[fidx]
                else:
                    nx, ny, nz = compute_face_normal(ai, bi, ci, obj.positions)
                gnx, gny, gnz = ase_to_gltf_normal(nx, ny, nz)
                # Normalize
                length = math.sqrt(gnx*gnx + gny*gny + gnz*gnz)
                if length > 1e-12:
                    gnx, gny, gnz = gnx/length, gny/length, gnz/length
                normals_out.append((gnx, gny, gnz))

                # UV
                if uv_i < len(obj.tverts):
                    u, v = obj.tverts[uv_i]
                else:
                    u, v = 0.0, 0.0
                gu, gv = ase_to_gltf_uv(u, v)
                uvs_out.append((gu, gv))

                vertex_map[key] = len(positions_out) - 1

            indices_out.append(vertex_map[key])

    return positions_out, normals_out, uvs_out, indices_out, mtl_groups


# ---------------------------------------------------------------------------
# GLB binary buffer builder
# ---------------------------------------------------------------------------

def pack_f32(values):
    return struct.pack(f'<{len(values)}f', *values)


def build_binary_buffer(objects_data):
    """
    Build a combined binary buffer for all mesh objects.
    objects_data: list of (positions, normals, uvs, indices_per_mtl)
    Returns (buffer_bytes, accessor_specs) where accessor_specs maps
    each mesh object's data to byte offsets.
    """
    buf = b''
    specs = []

    for positions, normals, uvs, mtl_index_groups in objects_data:
        obj_spec = {}

        vert_count = len(positions)
        use_uint32 = vert_count > 65535

        # POSITION
        pos_offset = len(buf)
        flat_pos = []
        for x, y, z in positions:
            flat_pos.extend([x, y, z])
        pos_bytes = pack_f32(flat_pos)
        buf += pos_bytes

        # Pad to 4-byte boundary
        while len(buf) % 4 != 0:
            buf += b'\x00'

        # NORMAL
        norm_offset = len(buf)
        flat_norm = []
        for nx, ny, nz in normals:
            flat_norm.extend([nx, ny, nz])
        norm_bytes = pack_f32(flat_norm)
        buf += norm_bytes
        while len(buf) % 4 != 0:
            buf += b'\x00'

        # TEXCOORD_0
        uv_offset = len(buf)
        flat_uv = []
        for u, v in uvs:
            flat_uv.extend([u, v])
        uv_bytes = pack_f32(flat_uv)
        buf += uv_bytes
        while len(buf) % 4 != 0:
            buf += b'\x00'

        # INDEX buffers (one per material group)
        idx_specs = {}
        for mtlid, face_list in mtl_index_groups.items():
            idx_offset = len(buf)
            # Build flat index list for this material's faces
            idxs = []
            for fidx in face_list:
                base = fidx * 3
                idxs.extend([face_list.index(fidx) * 3, face_list.index(fidx) * 3 + 1, face_list.index(fidx) * 3 + 2])
            # Actually we need the global indices already computed
            idx_specs[mtlid] = (idx_offset, 0, 0)  # placeholder

        obj_spec = {
            'pos_offset': pos_offset, 'pos_len': len(pos_bytes), 'vert_count': vert_count,
            'norm_offset': norm_offset, 'norm_len': len(norm_bytes),
            'uv_offset': uv_offset, 'uv_len': len(uv_bytes),
            'use_uint32': use_uint32,
        }
        specs.append((obj_spec, buf))  # carry buf snapshot for index offset tracking

    return buf, specs


def build_glb_data(all_geom, materials, helper_objects=None):
    """
    Build GLTF JSON dict + binary buffer for all mesh objects.
    Helper objects (joints, hardpoints) are added as empty mesh-less nodes.
    Returns (gltf_dict, binary_bytes, stats).
    """
    buf = b''
    gltf_bufferviews = []
    gltf_accessors = []
    gltf_meshes = []
    gltf_nodes = []

    total_verts = 0
    total_tris = 0

    # Build material list for GLTF
    gltf_materials = []
    gltf_textures = []
    gltf_images = []
    tex_name_to_idx = {}

    for mat_idx, mat in enumerate(materials):
        if mat.bitmap and mat.bitmap not in tex_name_to_idx:
            tex_name_to_idx[mat.bitmap] = len(gltf_images)
            gltf_images.append({'uri': mat.bitmap})
            gltf_textures.append({'source': len(gltf_images) - 1})

        gltf_mat = {
            'name': mat.name if mat.name else f'mat_{mat_idx}',
            'pbrMetallicRoughness': {
                'metallicFactor': 0.0,
                'roughnessFactor': 0.8,
            }
        }
        if mat.bitmap and mat.bitmap in tex_name_to_idx:
            gltf_mat['pbrMetallicRoughness']['baseColorTexture'] = {
                'index': tex_name_to_idx[mat.bitmap]
            }
        gltf_materials.append(gltf_mat)

    def add_bufferview(byte_offset, byte_length, target=None):
        bv = {'buffer': 0, 'byteOffset': byte_offset, 'byteLength': byte_length}
        if target is not None:
            bv['target'] = target
        gltf_bufferviews.append(bv)
        return len(gltf_bufferviews) - 1

    def add_accessor(bv_idx, component_type, count, acc_type, min_vals=None, max_vals=None, byte_offset=0):
        acc = {
            'bufferView': bv_idx,
            'componentType': component_type,
            'count': count,
            'type': acc_type,
        }
        if byte_offset:
            acc['byteOffset'] = byte_offset
        if min_vals is not None:
            acc['min'] = [round(v, 6) for v in min_vals]
        if max_vals is not None:
            acc['max'] = [round(v, 6) for v in max_vals]
        gltf_accessors.append(acc)
        return len(gltf_accessors) - 1

    ARRAY_BUFFER = 34962
    ELEMENT_ARRAY_BUFFER = 34963
    FLOAT = 5126
    UNSIGNED_SHORT = 5123
    UNSIGNED_INT = 5125

    for obj in all_geom:
        if not obj.faces:
            continue

        positions, normals, uvs, flat_indices, mtl_groups = build_expanded_mesh(obj, materials)

        if not positions:
            continue

        vert_count = len(positions)
        total_verts += vert_count

        use_uint32 = vert_count > 65535

        # POSITION buffer view
        pos_offset = len(buf)
        pos_flat = []
        for x, y, z in positions:
            pos_flat.extend([x, y, z])
        pos_bytes = struct.pack(f'<{len(pos_flat)}f', *pos_flat)
        buf += pos_bytes
        while len(buf) % 4 != 0:
            buf += b'\x00'

        pos_bv = add_bufferview(pos_offset, len(pos_bytes), ARRAY_BUFFER)
        min_pos = [min(p[i] for p in positions) for i in range(3)]
        max_pos = [max(p[i] for p in positions) for i in range(3)]
        pos_acc = add_accessor(pos_bv, FLOAT, vert_count, 'VEC3', min_pos, max_pos)

        # NORMAL
        norm_offset = len(buf)
        norm_flat = []
        for nx, ny, nz in normals:
            norm_flat.extend([nx, ny, nz])
        norm_bytes = struct.pack(f'<{len(norm_flat)}f', *norm_flat)
        buf += norm_bytes
        while len(buf) % 4 != 0:
            buf += b'\x00'

        norm_bv = add_bufferview(norm_offset, len(norm_bytes), ARRAY_BUFFER)
        norm_acc = add_accessor(norm_bv, FLOAT, vert_count, 'VEC3')

        # TEXCOORD_0
        uv_offset = len(buf)
        uv_flat = []
        for u, v in uvs:
            uv_flat.extend([u, v])
        uv_bytes = struct.pack(f'<{len(uv_flat)}f', *uv_flat)
        buf += uv_bytes
        while len(buf) % 4 != 0:
            buf += b'\x00'

        uv_bv = add_bufferview(uv_offset, len(uv_bytes), ARRAY_BUFFER)
        uv_acc = add_accessor(uv_bv, FLOAT, vert_count, 'VEC2')

        # Primitives (one per material group)
        primitives = []
        for mtlid in sorted(mtl_groups.keys()):
            face_indices_for_mtl = mtl_groups[mtlid]
            # Build index buffer for this material
            idx_list = []
            for fi in face_indices_for_mtl:
                base = fi * 3
                idx_list.extend([flat_indices[base], flat_indices[base+1], flat_indices[base+2]])

            total_tris += len(idx_list) // 3

            idx_offset = len(buf)
            if use_uint32:
                idx_bytes = struct.pack(f'<{len(idx_list)}I', *idx_list)
                comp_type = UNSIGNED_INT
            else:
                idx_bytes = struct.pack(f'<{len(idx_list)}H', *idx_list)
                comp_type = UNSIGNED_SHORT
            buf += idx_bytes
            while len(buf) % 4 != 0:
                buf += b'\x00'

            idx_bv = add_bufferview(idx_offset, len(idx_bytes), ELEMENT_ARRAY_BUFFER)
            idx_acc = add_accessor(idx_bv, comp_type, len(idx_list), 'SCALAR')

            prim = {
                'attributes': {
                    'POSITION': pos_acc,
                    'NORMAL': norm_acc,
                    'TEXCOORD_0': uv_acc,
                },
                'indices': idx_acc,
                'mode': 4,  # TRIANGLES
            }
            # Assign material
            global_mtl_idx = obj.mat_ref  # single-material objects
            if len(materials) > 1 and mtlid < len(materials):
                global_mtl_idx = mtlid
            if global_mtl_idx < len(gltf_materials):
                prim['material'] = global_mtl_idx

            primitives.append(prim)

        mesh_name = obj.node_name if obj.node_name else f'mesh_{len(gltf_meshes)}'
        gltf_meshes.append({'name': mesh_name, 'primitives': primitives})
        node = {'name': obj.node_name, 'mesh': len(gltf_meshes) - 1,
                '_parent_name': obj.parent_name}  # temp field, removed below
        gltf_nodes.append(node)

    # Add helper objects (joints, hardpoints) as empty nodes
    # Track which names already exist from mesh objects
    existing_names = {n.get('name', '') for n in gltf_nodes}
    if helper_objects:
        for h in helper_objects:
            if h.node_name and h.node_name not in existing_names:
                tx, ty, tz = ase_to_gltf_pos(*h.translation)
                helper_node = {
                    'name': h.node_name,
                    'translation': [round(tx, 6), round(ty, 6), round(tz, 6)],
                    '_parent_name': h.parent_name,  # temp field, removed below
                }
                gltf_nodes.append(helper_node)
                existing_names.add(h.node_name)

    # ------------------------------------------------------------------
    # P1-C: wire parent-child node hierarchy
    # ------------------------------------------------------------------
    # Build name -> index map for all nodes in this GLB
    node_name_to_idx = {n['name']: idx for idx, n in enumerate(gltf_nodes) if n.get('name')}

    # For each node, attach children list to its parent
    for idx, node in enumerate(gltf_nodes):
        parent_name = node.pop('_parent_name', '')
        # A node is a root if it has no parent, or if its parent is not in our set
        if parent_name and parent_name in node_name_to_idx:
            parent_idx = node_name_to_idx[parent_name]
            parent_node = gltf_nodes[parent_idx]
            if 'children' not in parent_node:
                parent_node['children'] = []
            parent_node['children'].append(idx)

    # Scene root nodes: only those whose _parent_name was absent or unresolved
    # We already popped _parent_name above, so re-derive from ASE source:
    # root = any node not appearing as a child of another
    child_indices = set()
    for node in gltf_nodes:
        for c in node.get('children', []):
            child_indices.add(c)
    root_indices = [i for i in range(len(gltf_nodes)) if i not in child_indices]

    gltf = {
        'asset': {'version': '2.0', 'generator': 'mc2-ase_to_glb'},
        'scene': 0,
        'scenes': [{'nodes': root_indices}],
        'nodes': gltf_nodes,
        'meshes': gltf_meshes,
        'accessors': gltf_accessors,
        'bufferViews': gltf_bufferviews,
        'buffers': [{'byteLength': len(buf)}],
    }
    if gltf_materials:
        gltf['materials'] = gltf_materials
    if gltf_textures:
        gltf['textures'] = gltf_textures
    if gltf_images:
        gltf['images'] = gltf_images

    stats = {'vertex_count': total_verts, 'triangle_count': total_tris,
             'mesh_count': len(gltf_meshes), 'material_count': len(gltf_materials),
             'node_count': len(gltf_nodes), 'root_node_count': len(root_indices)}
    return gltf, buf, stats


# ---------------------------------------------------------------------------
# GLB writer
# ---------------------------------------------------------------------------

def write_glb(gltf_dict, binary_data, out_path):
    json_bytes = json.dumps(gltf_dict, separators=(',', ':')).encode('utf-8')
    while len(json_bytes) % 4 != 0:
        json_bytes += b' '

    bin_padded = binary_data
    while len(bin_padded) % 4 != 0:
        bin_padded += b'\x00'

    json_chunk = struct.pack('<II', len(json_bytes), 0x4E4F534A) + json_bytes
    bin_chunk  = struct.pack('<II', len(bin_padded), 0x004E4942) + bin_padded
    header     = struct.pack('<III', 0x46546C67, 2, 12 + len(json_chunk) + len(bin_chunk))

    with open(out_path, 'wb') as f:
        f.write(header + json_chunk + bin_chunk)


# ---------------------------------------------------------------------------
# Sidecar JSON builder
# ---------------------------------------------------------------------------

def build_sidecar(ase_path, ini_path, glb_path, geom_objects, materials, ini_sections, stats,
                  asset_class='mech', upscale_method=None):
    # Parse LODs and shadow from INI
    lods = []
    for idx in range(10):
        fn = ini_str(ini_sections, 'TGLData', f'FileName{idx}')
        if not fn:
            break
        dist = ini_float(ini_sections, 'TGLData', f'Distance{idx}', 0.0)
        entry = {'index': idx}
        if idx == 0:
            entry['glb'] = os.path.basename(glb_path)
        else:
            entry['ase'] = f'{fn}.ase'
            entry['status'] = 'legacy'
        entry['distance'] = dist
        lods.append(entry)

    shadow_name = ini_str(ini_sections, 'TGLData', 'ShadowName', '')

    # Hardpoints
    num_smoke   = ini_int(ini_sections, 'Nodes', 'NumSmoke', 0)
    num_jumpjet = ini_int(ini_sections, 'Nodes', 'NumJumpJet', 0)
    num_weapon  = ini_int(ini_sections, 'Nodes', 'NumWeapon', 0)
    num_feet    = ini_int(ini_sections, 'Nodes', 'NumFeet', 0)

    smoke_nodes = []
    for i in range(num_smoke):
        name = ini_str(ini_sections, f'SmokeNode{i}', 'SmokeNodeName', '')
        if name:
            smoke_nodes.append({'name': name})

    jumpjet_nodes = []
    for i in range(num_jumpjet):
        name = ini_str(ini_sections, f'JumpJetNode{i}', 'JumpNodeName', '')
        if name:
            jumpjet_nodes.append({'name': name})

    weapon_nodes = []
    for i in range(num_weapon):
        name = ini_str(ini_sections, f'WeaponNode{i}', 'WeaponNodeName', '')
        wtype = ini_int(ini_sections, f'WeaponNode{i}', 'WeaponType', 0)
        if name:
            weapon_nodes.append({'name': name, 'type': wtype})

    foot_nodes = []
    for i in range(num_feet):
        name = ini_str(ini_sections, f'FootNode{i}', 'FootNodeName', '')
        if name:
            foot_nodes.append({'name': name})

    # Compute bbox in MC2 space (before axis transform)
    all_verts = []
    for obj in geom_objects:
        all_verts.extend(obj.positions)

    if all_verts:
        xs = [v[0] for v in all_verts]
        ys = [v[1] for v in all_verts]
        zs = [v[2] for v in all_verts]
        bbox = {
            'min': [min(xs), min(ys), min(zs)],
            'max': [max(xs), max(ys), max(zs)],
        }
    else:
        bbox = {'min': [0.0, 0.0, 0.0], 'max': [0.0, 0.0, 0.0]}

    # Material map
    mat_map = []
    for midx, mat in enumerate(materials):
        mat_map.append({
            'slot': midx,
            'texture': mat.bitmap if mat.bitmap else '',
            'alpha': False,
        })

    asset_name = ini_str(ini_sections, 'TGLData', 'FileName0', '')
    if not asset_name:
        asset_name = os.path.splitext(os.path.basename(ase_path))[0]

    # Determine upscale_method if not explicitly provided:
    # "none" by default; callers or the CLI may override with
    # "user_upscaled_external" when they know the texture has been upscaled.
    if upscale_method is None:
        upscale_method = 'none'

    sidecar = {
        'schema_version': '1.0',
        'asset_name': asset_name,
        'source_ase': os.path.abspath(ase_path),
        'asset_class': asset_class,
        'lods': lods,
        'shadow_mesh': {'ase': f'{shadow_name}.ase', 'status': 'legacy'} if shadow_name else None,
        'hardpoints': {
            'weapon': weapon_nodes,
            'smoke': smoke_nodes,
            'jumpjet': jumpjet_nodes,
            'foot': foot_nodes,
        },
        'collision_bounds': {
            'source': 'legacy',
            'minBox': bbox['min'],
            'maxBox': bbox['max'],
        },
        'axis_mapping': 'MC2_GLTF_AXIS=0',
        'material_map': mat_map,
        'upscale_method': upscale_method,
        'validation_baseline': {
            'vertex_count': stats['vertex_count'],
            'triangle_count': stats['triangle_count'],
            'material_slots': stats['material_count'],
            'bbox': bbox,
            'bbox_tolerance': 0.01,
        },
        'generated_by': f'ase_to_glb.py v{__version__}',
    }

    return sidecar


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description='Convert MC2 ASE to GLB + mcasset.json sidecar.')
    p.add_argument('--ase', required=True, help='Path to source .ase file')
    p.add_argument('--ini', required=True, help='Path to source FITini file')
    p.add_argument('--out-glb', required=True, help='Output .glb path')
    p.add_argument('--out-sidecar', required=True, help='Output .mcasset.json path')
    p.add_argument('--asset-class',
                   choices=['mech', 'prop', 'building', 'vehicle', 'tree'],
                   default='mech',
                   help='Asset class for sidecar (default: mech)')
    p.add_argument('--upscale-method',
                   default=None,
                   help='upscale_method value for sidecar '
                        '(e.g. user_upscaled_external). Default: none')
    args = p.parse_args()

    if not os.path.isfile(args.ase):
        print(f"ERROR: ASE not found: {args.ase}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(args.ini):
        print(f"ERROR: INI not found: {args.ini}", file=sys.stderr)
        sys.exit(1)

    print(f"Parsing ASE: {args.ase}")
    geom_objects, materials, helper_objects = parse_ase(args.ase)
    print(f"  {len(geom_objects)} GEOMOBJECT(s), {len(materials)} material(s), {len(helper_objects)} HELPEROBJECT(s)")

    if not geom_objects:
        print("ERROR: no GEOMOBJECT blocks found in ASE", file=sys.stderr)
        sys.exit(1)

    print("Building GLB data...")
    gltf, binary, stats = build_glb_data(geom_objects, materials, helper_objects)
    print(f"  vertices={stats['vertex_count']}, triangles={stats['triangle_count']}, "
          f"meshes={stats['mesh_count']}, materials={stats['material_count']}, "
          f"nodes={stats['node_count']}, root_nodes={stats['root_node_count']}")

    os.makedirs(os.path.dirname(os.path.abspath(args.out_glb)), exist_ok=True)
    write_glb(gltf, binary, args.out_glb)
    print(f"Wrote GLB: {args.out_glb}  ({os.path.getsize(args.out_glb)} bytes)")

    ini_sections = parse_fitini(args.ini)
    sidecar = build_sidecar(args.ase, args.ini, args.out_glb, geom_objects, materials, ini_sections, stats,
                            asset_class=args.asset_class,
                            upscale_method=args.upscale_method)

    os.makedirs(os.path.dirname(os.path.abspath(args.out_sidecar)), exist_ok=True)
    with open(args.out_sidecar, 'w', encoding='utf-8') as f:
        json.dump(sidecar, f, indent=2)
    print(f"Wrote sidecar: {args.out_sidecar}")


if __name__ == '__main__':
    main()
