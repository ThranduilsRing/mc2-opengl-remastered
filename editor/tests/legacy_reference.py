"""Grade identified legacy insertions and retain immutable semantic placement references."""
from collections import Counter
import hashlib
import json
import re
import shutil
import struct
from tools.mc2x_import.fst import decompress_entry

PLACEMENTS = {"mech": (53, "Anubis"), "vehicle": (117, "APC"), "building": (263, "Barrels"),
              "turret": (525, "AutoCannonCamo1"), "tree": (252, "Birch1"), "gate": (157, "D_HeavyGate"),
              "bridge": (607, "bridge"), "light": (68, "Spotlight"), "marker": (290, "Nav1")}
TEAMS = {family: -1 if family in ("building", "tree", "bridge", "marker") else 0 for family in PLACEMENTS}


def packet(data, index):
    if len(data) < 8:
        raise ValueError("truncated PAK")
    end_table = struct.unpack_from("<I", data, 4)[0]
    if end_table < 12 or end_table > len(data) or end_table % 4 or index >= end_table // 4 - 2:
        raise ValueError("invalid packet table")
    entry = struct.unpack_from("<I", data, 8 + 4 * index)[0]
    begin = entry & 0x1fffffff
    end = len(data) if index + 1 == end_table // 4 - 2 else struct.unpack_from("<I", data, 12 + 4 * index)[0] & 0x1fffffff
    kind = entry >> 29
    if not end_table <= begin <= end <= len(data):
        raise ValueError("packet bounds")
    encoded = data[begin:end]
    if kind == 7:
        if encoded:
            raise ValueError("nonempty null packet")
        return b""
    if kind in (0, 1):
        return encoded
    if kind not in (2, 4) or len(encoded) < 5:
        raise ValueError("invalid packet compression")
    count = struct.unpack_from("<I", encoded)[0]
    if count > 64 * 1024 * 1024:
        raise ValueError("packet size limit")
    decoded = decompress_entry(4, len(encoded) - 4, count, kind == 2, encoded)
    if len(decoded) != count:
        raise ValueError("packet decoded size mismatch")
    return decoded


def buildings(data):
    decoded = packet(data, 1)
    if not decoded:
        return []
    if len(decoded) < 4:
        raise ValueError("placement count missing")
    count = struct.unpack_from("<I", decoded)[0]
    if len(decoded) != 4 + count * 40:
        raise ValueError("placement count mismatch")
    return [decoded[4 + i * 40:44 + i * 40] for i in range(count)]


def canonical_units(fields, removed_part=None, removed_warrior=None):
    """Retain unit values and pilot associations independently of export numbering."""
    parts, warriors = {}, {}
    for key, values in fields.items():
        match = re.fullmatch(r"(Part|Warrior)(\d+)([^/]*)/(.+)", key)
        if match:
            kind, number, suffix, name = match.groups()
            groups = parts if kind == "Part" else warriors
            groups.setdefault(int(number), {})[suffix + "/" + name] = tuple(values)
    def frozen(group):
        return tuple(sorted(group.items()))
    warrior_values = {number: frozen(group) for number, group in warriors.items()}
    def part_values(number, ancestors=()):
        if number in ancestors or number not in parts:
            raise ValueError("invalid legacy alternative reference")
        group = dict(parts[number])
        if "/Pilot" in group:
            values = group.pop("/Pilot")
            if len(values) != 1:
                raise ValueError("duplicate legacy pilot reference")
            kind, reference = values[0].split(" ", 1)
            pilot = int(reference)
            if pilot not in warrior_values:
                raise ValueError("missing pre-existing Warrior")
            group["/PilotValues"] = (kind, warrior_values[pilot])
        alternatives = int(group.get("/NumAlternatives", ("ul 0",))[0].split(" ", 1)[1])
        if "/IndicesOfAlternatives" in group:
            values = group.pop("/IndicesOfAlternatives")
            if len(values) != 1:
                raise ValueError("duplicate legacy alternative indices")
            kind, references = values[0].split(" ", 1)
            indices = [int(value) for value in references.replace(",", " ").split()]
            if alternatives < 0 or alternatives > len(indices):
                raise ValueError("invalid legacy alternative count")
            group["/AlternativeValues"] = tuple(part_values(index, (*ancestors, number)) for index in indices[:alternatives])
            group["/AlternativeIndexValues"] = (kind, tuple(indices[alternatives:]))
        elif alternatives:
            raise ValueError("missing legacy alternative indices")
        # The export cursor moves on insertion; actual alternative values retain identity.
        group.pop("/AlternativeStartIndex", None)
        return frozen(group)
    return (Counter(part_values(number) for number in parts if number != removed_part),
            Counter(value for number, value in warrior_values.items() if number != removed_warrior))


def grade(directory, evidence):
    from scripts.run_editor_smoke import fit_fields
    def semantic_fit(data):
        return fit_fields(b"\n".join(line for line in data.splitlines() if line.strip().lower() not in (b"fitini", b"fitend")))
    facts = {}
    for line in evidence.splitlines():
        if line.startswith("[EDITOR_LEGACY_PLACEMENT v1]"):
            fact = dict(re.findall(r"(\w+)=([^\s]+)", line))
            family = fact.get("family")
            if family in facts:
                raise ValueError(f"duplicate legacy placement fact: {family}")
            facts[family] = fact
    if set(facts) != set(PLACEMENTS):
        raise ValueError("missing or unexpected legacy families")
    before_fit = (directory / "e5_reference_before.fit").read_bytes()
    before_pak = (directory / "e5_reference_before.pak").read_bytes()
    before_fields = semantic_fit(before_fit)
    before_count = int(before_fields["Parts/NumParts"][0].split(" ", 1)[1])
    before_buildings = Counter(buildings(before_pak))
    before_units = canonical_units(before_fields)
    result = {"comparison": "semantic FIT fields and decoded packet-1 placements; whole PAK byte equality is not required", "placements": {}}
    for family, (object_id, stem) in PLACEMENTS.items():
        fact = facts[family]
        if int(fact.get("fit_id", -1)) != object_id or fact.get("stem") != stem or fact.get("input_team") != "0" or fact.get("team") != str(TEAMS[family]) or any(
                fact.get(key) != "1" for key in ("painted", "undo", "redo", "saved", "removed", "reopened")):
            raise ValueError(f"unsuccessful identified placement: {family}")
        fit = (directory / f"e5_reference_{family}.fit").read_bytes()
        pak = (directory / f"e5_reference_{family}.pak").read_bytes()
        if fit == before_fit and pak == before_pak:
            raise ValueError(f"unchanged placement output: {family}")
        fields = semantic_fit(fit)
        count = int(fields["Parts/NumParts"][0].split(" ", 1)[1])
        after_buildings = Counter(buildings(pak))
        record, part = None, None
        if family in ("mech", "vehicle"):
            if count != before_count + 1 or after_buildings != before_buildings:
                raise ValueError(f"unit placement count delta: {family}")
            candidates = []
            for number in range(1, count + 1):
                candidate = f"Part{number}/"
                if candidate + "ObjectNumber" not in fields:
                    raise ValueError(f"missing main Part: {number}")
                if int(fields[candidate + "ObjectNumber"][0].split(" ", 1)[1]) == object_id and all(
                        abs(float(fields[candidate + f"Position{axis}"][0].split(" ", 1)[1]) - float(fact[axis.lower()])) <= 0.00001 for axis in ("X", "Y")):
                    candidates.append(candidate)
            if len(candidates) != 1:
                raise ValueError(f"identified unit location is missing or ambiguous: {family}")
            prefix = candidates[0]
            part = {k: v for k, v in fields.items() if k.startswith(prefix)}
            required = ("ObjectNumber", "ControlType", "ControlDataType", "PlayerPart", "MyIcon", "TeamID", "CommanderID", "Pilot",
                        "PositionX", "PositionY", "Rotation", "Active", "Exists", "Damage", "BaseColor", "HighlightColor1", "HighlightColor2",
                        "SelfRepairBehavior", "ObjectProfile", "CSVFile", "VariantNumber", "SquadNum", "NumAlternatives")
            if any(prefix + name not in part for name in required) or int(part[prefix + "ObjectNumber"][0].split(" ", 1)[1]) != object_id:
                raise ValueError(f"incomplete legacy unit output: {family}")
            if part[prefix + "CSVFile"] != [f'st "{stem}"']:
                raise ValueError(f"unit CSV identity changed: {family}")
            if part[prefix + "TeamID"] != [f'c {TEAMS[family]}'] or part[prefix + "CommanderID"] != [f'c {fact["input_team"]}'] or part[prefix + "PlayerPart"] != ['b TRUE']:
                raise ValueError(f"serialized unit alignment changed: {family}")
            pilot = int(part[prefix + "Pilot"][0].split(" ", 1)[1])
            warrior = f"Warrior{pilot}/"
            profile = fields.get(warrior + "Profile", [])
            if pilot <= 0 or len(profile) != 1 or not re.fullmatch(r'st "[^"\r\n]+"', profile[0]) or not profile[0][4:-1].strip() or fields.get(warrior + "Brain") != ['st "PBrain"'] or any(
                    fields.get(warrior + name) != ['l 0'] for name in ("NumCells", "NumStaticVars")):
                raise ValueError(f"missing pilot or brain defaults: {family}")
            if canonical_units(fields, int(prefix[4:-1]), pilot) != before_units:
                raise ValueError(f"pre-existing Parts or Warriors changed: {family}")
            for axis in ("X", "Y"):
                if abs(float(part[prefix + f"Position{axis}"][0].split(" ", 1)[1]) - float(fact[axis.lower()])) > 0.00001:
                    raise ValueError(f"unit coordinate changed: {family}")
        else:
            added = list((after_buildings - before_buildings).elements())
            if count != before_count or before_buildings - after_buildings or len(added) != 1:
                raise ValueError(f"building placement count delta: {family}")
            if canonical_units(fields) != before_units:
                raise ValueError(f"pre-existing Parts or Warriors changed: {family}")
            values = struct.unpack("<Iffffiiiii", added[0])
            record = dict(zip(("objectNumber", "x", "y", "z", "rotation", "damage", "team", "parent", "forestEncoded", "scaleEncoded"), values))
            if values[0] != object_id or values[6:] != (TEAMS[family], -1, 0, 0) or any(abs(record[axis] - float(fact[axis])) > 0.00001 for axis in ("x", "y", "z")):
                raise ValueError(f"building values or ordinary forest/scale encoding changed: {family}")
        result["placements"][family] = {"facts": fact, "part": part, "building": record,
            "fitFields": fields, "fitSha256": hashlib.sha256(fit).hexdigest(), "pakSha256": hashlib.sha256(pak).hexdigest()}
    return result


def retain(source, destination, result):
    destination.mkdir(parents=True, exist_ok=False)
    for label in ("before", *PLACEMENTS):
        for extension in ("fit", "pak"):
            name = f"e5_reference_{label}.{extension}"
            shutil.copyfile(source / name, destination / name)
    (destination / "placements.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
