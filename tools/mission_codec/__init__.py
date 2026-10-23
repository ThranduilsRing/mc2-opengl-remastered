"""Mission FIT, PacketFile, object and MOVE codecs shared by offline tools."""
from __future__ import annotations

import csv
import hashlib
import io
import math
import json
import re
import struct
import zlib
from pathlib import Path

import numpy as np

from tools.mc2x_import.fst import decompress_entry


OBJECT = struct.Struct("<i4f5i")
PASSABLE = 0x400


def parent_cell(parent):
    """Runtime parent identity is a packed cell; both sentinels mean no parent."""
    parent &= 0xFFFFFFFF
    return None if parent in (0, 0xFFFFFFFF) else [parent >> 16, parent & 0xFFFF]


def packed_cell(cell):
    row, col = map(int, cell)
    if not (0 <= row <= 0xFFFF and 0 <= col <= 0xFFFF):
        raise ValueError("parent cell outside packed range")
    return (row << 16) | col


def object_records(raw):
    if len(raw) < 4:
        raise ValueError("missing object count")
    count, = struct.unpack_from("<i", raw)
    if count < 0 or len(raw) != 4 + count * OBJECT.size:
        raise ValueError("mission object count/size mismatch")
    records = list(OBJECT.iter_unpack(raw[4:]))
    if any(not all(math.isfinite(v) for v in r[1:5]) for r in records):
        raise ValueError("nonfinite mission object position")
    return records


def encode_objects(records):
    records = list(records)
    raw = struct.pack("<i", len(records)) + b"".join(OBJECT.pack(*r) for r in records)
    object_records(raw)
    return raw


def decode_move(packets, first=0):
    """Read the four MissionMap packets, without reconstructing derived paths."""
    try:
        height, width, planet = (struct.unpack("<i", packets[first+i])[0] for i in range(3))
        if not (0 < height <= 3072 and 0 < width <= 3072):
            raise ValueError("MOVE local dimensions out of range")
        cells = np.frombuffer(packets[first+3], "<u4").reshape(height, width)
    except (IndexError, struct.error, ValueError) as error:
        raise ValueError("MOVE local dimensions/packet size mismatch") from error
    return height, width, planet, cells


def encode_move(cells, planet=0):
    cells = np.asarray(cells, dtype="<u4")
    height, width = cells.shape
    packets = [struct.pack("<i", n) for n in (height, width, planet)] + [cells.tobytes()]
    decode_move(packets)
    return packets


def move_fields(cells):
    """Local LOS height is a 4-bit level, not terrain elevation in world units."""
    return {"passable": (cells & PASSABLE) != 0,
            "local_height": (cells >> 18) & 15,
            "forest": (cells & 0x10000000) != 0}


def key(name):
    return str(name).replace("\\", "/").lower()


def fit_sections(data):
    """Read FIT scalar fields; retain strings and tolerate whitespace/case."""
    text = data.decode("latin1") if isinstance(data, bytes) else data
    sections, current = {}, None
    for line in text.replace("\r", "\n").splitlines():
        match = re.match(r"\s*\[([^]]+)\]", line)
        if match:
            current = sections.setdefault(match[1].strip().lower(), {})
        elif current is not None:
            match = re.match(r"\s*(\w+)(?:\[\d+\])?\s+([^=]+?)\s*=\s*(.*)", line)
            if match:
                raw = match[3].strip()
                if raw.startswith('"'):
                    value = raw[1:raw.find('"', 1)]
                else:
                    raw = raw.split("//")[0].strip()
                    if raw.lower() in ("true", "false"):
                        value = raw.lower() == "true"
                    else:
                        try:
                            value = int(raw, 16 if raw.lower().startswith("0x") else 10)
                        except ValueError:
                            try:
                                value = float(raw)
                            except ValueError:
                                value = raw
                if match[1].lower() == "ul" and isinstance(value, int):
                    value &= 0xFFFFFFFF
                current[match[2].strip().lower()] = value
    return sections


def packet(data, index):
    if len(data) < 8:
        raise ValueError("truncated PacketFile header")
    magic, first = struct.unpack_from("<II", data)
    if magic != 0xFEEDFACE or first % 4 or not 8 <= first <= len(data):
        raise ValueError("invalid PacketFile header")
    count = first // 4 - 2
    if not 0 <= index < count:
        raise ValueError(f"packet {index} outside {count} packets")
    entries = struct.unpack_from(f"<{count}I", data, 8)
    entry = entries[index]
    storage, offset = entry >> 29, entry & 0x1FFFFFFF
    end = entries[index+1] & 0x1FFFFFFF if index+1 < count else len(data)
    if not first <= offset <= end <= len(data):
        raise ValueError("invalid PacketFile offsets")
    raw = data[offset:end]
    if storage in (0, 1):
        return raw
    if storage == 7:
        return b""
    if storage not in (2, 4) or len(raw) < 4:
        raise ValueError(f"unsupported/corrupt packet storage {storage}")
    size = struct.unpack_from("<I", raw)[0]
    if storage == 4:
        decoded = zlib.decompress(raw[4:])
    else:
        decoded = decompress_entry(0, len(raw)-4, size, True, raw[4:])
    if len(decoded) != size:
        raise ValueError(f"packet length {len(decoded)} != declared {size}")
    return decoded


class Install:
    """Index headers only; never load a whole multi-GB FST into memory.

    Equal-name archive members use sorted archive order, recorded in provenance.
    A supplied fallback supplies dependencies absent from expansion-only installs.
    """

    def __init__(self, root, fallback=None, *, include_loose=True):
        self.root = Path(root).resolve()
        self.fallback = fallback
        self.files = {}
        self.collisions = []
        for archive in sorted(self.root.glob("*.fst")):
            with archive.open("rb") as stream:
                header = stream.read(8)
                if len(header) != 8:
                    raise ValueError(f"truncated FST: {archive}")
                magic, count = struct.unpack("<II", header)
                if magic not in (0xFADDECAF, 0xCADDECAF) or count > 2_000_000:
                    raise ValueError(f"bad FST header: {archive}")
                table = stream.read(count * 266)
            if len(table) != count * 266:
                raise ValueError(f"truncated FST index: {archive}")
            for off in range(0, len(table), 266):
                offset, comp, size, _ = struct.unpack_from("<IIII", table, off)
                name = key(table[off+16:off+266].split(b"\0")[0].decode("latin1"))
                if name in self.files:
                    self.collisions.append(name)
                self.files[name] = (archive, offset, comp, size, magic == 0xFADDECAF)
        # Limit the loose scan to relevant data, excluding backup/mod overlays.
        for relative in (("data/missions", "data/objects", "data/art", "data/textures", "data/tgl") if include_loose else ()):
            folder = self.root / relative
            if folder.exists():
                for path in sorted(folder.rglob("*")):
                    if path.is_file() and path.suffix.lower() in (".fit", ".pak", ".csv", ".ini", ".tgl", ".ase", ".tga"):
                        self.files[key(path.relative_to(self.root))] = (path,)

    def has(self, name):
        return key(name) in self.files or bool(self.fallback and self.fallback.has(name))

    def read_alternative(self, names):
        for name in names:
            if key(name) in self.files:
                return self.read(name)
        if self.fallback:
            return self.fallback.read_alternative(names)
        raise FileNotFoundError(f"{self.root}: none of {names}")

    def read(self, name):
        name = key(name)
        record = self.files.get(name)
        if record is None:
            if self.fallback:
                return self.fallback.read(name)
            raise FileNotFoundError(f"{self.root}: {name}")
        path = record[0]
        if len(record) == 1:
            return path.read_bytes(), str(path)
        _, offset, comp, size, lzw = record
        with path.open("rb") as stream:
            stream.seek(offset)
            raw = stream.read(comp)
        if len(raw) != comp:
            raise ValueError(f"short FST member: {name}")
        decoded = decompress_entry(0, comp, size, lzw, raw)
        if len(decoded) != size:
            raise ValueError(f"FST member length mismatch: {name}")
        return decoded, f"{path}::{name}"


CLASS_NAMES = {0: "tree", 1: "building", 2: "mech", 3: "vehicle",
               11: "terrain_object", 15: "bridge", 18: "tree",
               21: "turret", 22: "gate", 23: "light"}


def catalog(install):
    raw, source = install.read("data/objects/object2.pak")
    count = struct.unpack_from("<I", raw, 4)[0] // 4 - 2
    special = {}
    csv_classes = {}
    csv_source = None
    # GUI relocated in modern installs; legacy archive path remains valid.
    try:
        text, csv_source = install.read_alternative(("data/art/gui/buildings.csv", "data/art/buildings.csv"))
        for row in csv.reader(io.StringIO(text.decode("latin1"))):
            if len(row) > 5 and row[4].strip().isdigit():
                special[int(row[4])] = (row[0], row[5].upper().strip(), row[1])
                csv_classes[int(row[4])] = row[3].upper().strip()
    except FileNotFoundError:
        pass
    result, loose_sources = {}, {}

    def describe(i, sections, csv_only=False):
        clsnum = sections.get("objectclass", {}).get("objecttypenum", -1)
        obj = sections.get("objecttype", {})
        name = obj.get("appearancename", obj.get("name", f"type_{i}"))
        csv_name, tag, group = special.get(i, (name, "", ""))
        cls = CLASS_NAMES.get(clsnum, "other")
        if csv_only:
            cls = {"BUILDING": "building", "TREE": "prop", "MECH": "mech", "VEHICLE": "vehicle"}.get(csv_classes.get(i), "unknown")
        if tag in ("WALL", "GATE", "TURRET", "TURRET_CONTROL", "TURRET_GENERATOR", "GATE_CONTROL"):
            cls = tag.lower()
        if group == "1" or tag.startswith("TREE"):
            cls = "tree"
        elif group == "2" and cls in ("tree", "building", "terrain_object"):
            cls = "prop"
        # Tree class includes crates and tents; only CSV vegetation is foliage.
        if cls == "tree" and group and group != "1":
            cls = "prop"
        low = str(name).lower()
        # The Omnitech CSV omits WALL tags on the unchanged tiered wall models.
        if re.search(r"(?:^|_)(?:light|medium|heavy)(?:wall|post)", str(csv_name).lower()):
            cls = "wall"
        if str(csv_name).lower().endswith("prop") or "banner" in low:
            cls = "prop"
        if low.startswith("bridge"):
            cls = "bridge"
        if any(word in low for word in ("wirefence", "woodenfence", "sandbagwall")):
            cls = "fence"
        if cls == "building" and any(s in low for s in ("rock", "cliff", "boulder")):
            cls = "rock"
        return {"class": cls, "type_name": str(csv_name), "appearance": str(name),
                "object_class": clsnum, "special_type": tag,
                "catalog_resolution": "csv" if csv_only else "object_fit"}

    for i in range(count):
        sections = fit_sections(packet(raw, i))
        if sections:
            result[i] = describe(i, sections)
    # Omnitech extends past object2.pak with Buildings.csv IDs and named FITs.
    for i, (name, _, _) in special.items():
        if i not in result:
            filename = f"data/objects/{name}.fit"
            if install.has(filename):
                content, origin = install.read(filename)
                sections = fit_sections(content)
                loose_sources[str(i)] = origin
                result[i] = describe(i, sections)
                result[i]["catalog_fit"] = origin
            else:
                result[i] = describe(i, {"objecttype": {"appearancename": name}}, csv_only=True)
    return result, {"object_catalog": source, "building_catalog": csv_source,
                    "extended_catalog_fit_count": len(loose_sources),
                    "extended_catalog_fit_index_sha256": hashlib.sha256(
                        json.dumps(loose_sources, sort_keys=True).encode()).hexdigest()}


def overlay_catalog(install):
    name = "data/textures/textures.fit"
    if not install.has(name):
        return {}, None
    raw, source = install.read(name)
    sections = fit_sections(raw)
    first = sections.get("main", {}).get("maxterraintypes", 255)
    result = {}
    for i in range(sections.get("main", {}).get("maxterrainoverlays", 17)):
        values = sections.get(f"overlaytype{i}", {})
        count = values.get("numoverlays", 0)
        for handle in range(first, first + count):
            result[handle] = {"id": i, "name": values.get("overlayname", str(i)),
                              "tile": handle-first}
        first += count
    return result, source
