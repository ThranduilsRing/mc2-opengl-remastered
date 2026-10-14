"""Grade identified legacy insertions and retain immutable semantic placement references."""
from collections import Counter
import hashlib
import json
import math
import re
import shutil
import struct
from pathlib import Path
from tools.mc2x_import.fst import decompress_entry

PLACEMENTS = {"mech": (53, "Anubis"), "vehicle": (117, "APC"), "building": (263, "Barrels"),
              "turret": (525, "AutoCannonCamo1"), "tree": (252, "Birch1"), "gate": (157, "D_HeavyGate"),
              "bridge": (607, "bridge"), "light": (68, "Spotlight"), "marker": (290, "Nav1")}
TEAMS = {family: -1 if family in ("building", "tree", "bridge", "marker") else 0 for family in PLACEMENTS}
FROZEN = Path(__file__).resolve().parent / "goldens/e5a-stock"


def reference_evidence(manifest):
    return "\n".join("[EDITOR_LEGACY_PLACEMENT v1] " + " ".join(f"{k}={v}" for k, v in row["facts"].items())
                     for row in manifest["placements"].values())


def verify_frozen(directory=FROZEN):
    """Pinned capture bytes are a prerequisite; this function never writes them."""
    def digest(name):
        return hashlib.sha256((directory / name).read_bytes()).hexdigest()
    if digest("placements.json") != "3e3ed40dfb5c057fadf4d88a2ee36c128335b78bedfa28a745eeb24bb2e5eb37":
        raise ValueError("frozen manifest fingerprint mismatch")
    manifest = json.loads((directory / "placements.json").read_bytes())
    if set(manifest["placements"]) != set(PLACEMENTS):
        raise ValueError("frozen family corpus incomplete")
    for extension, expected in (("fit", "95c715176dd664c68f1d04f386e954ef296724a4e0919bf33fa4e43dbef2d958"),
                                ("pak", "18273400ab8115c323278dd18ba284f3e0378bbe79b1df0550086bc71a7702d5")):
        if digest(f"e5_reference_before.{extension}") != expected:
            raise ValueError("frozen baseline fingerprint mismatch")
    for family, row in manifest["placements"].items():
        for extension in ("fit", "pak"):
            if digest(f"e5_reference_{family}.{extension}") != row[f"{extension}Sha256"]:
                raise ValueError(f"frozen placement fingerprint mismatch: {family}.{extension}")
    return manifest


def typed_fields(fields):
    """Whitespace and float spelling are syntax; declared types and all values remain semantic."""
    def value(item):
        kind, text = item.split(" ", 1)
        if kind == "st":
            return kind, text
        values = text.replace(",", " ").split()
        if kind.startswith("f"):
            numbers = tuple(float(v) for v in values)
            if any(not math.isfinite(v) for v in numbers):
                raise ValueError("nonfinite placement value")
            return kind, numbers
        if kind == "b":
            return kind, tuple(v.upper() for v in values)
        return kind, tuple(int(v, 0) if v.lower().startswith("0x") else int(v) for v in values)
    return {key: tuple(value(item) for item in values) for key, values in fields.items()}


def inserted_unit(row):
    fields, part = row["fitFields"], row["part"]
    prefix = next(iter(part)).split("/", 1)[0] + "/"
    pilot = int(part[prefix + "Pilot"][0].split(" ", 1)[1])
    result = {"Part/" + key[len(prefix):]: value for key, value in part.items() if key != prefix + "Pilot"}
    # Pilot is an export reference. Compare its complete target, including numbered cells.
    result["Part/Pilot"] = [part[prefix + "Pilot"][0].split(" ", 1)[0] + " 1"]
    for key, value in fields.items():
        match = re.fullmatch(r"Warrior(\d+)([^/]*)/(.+)", key)
        if match and int(match[1]) == pilot:
            result["Warrior" + match[2] + "/" + match[3]] = value
    return typed_fields(result)


def marker_values(fields):
    groups = {}
    for key, value in fields.items():
        match = re.fullmatch(r"NavMarker(\d+)/(.*)", key)
        if match:
            groups.setdefault(int(match[1]), {})[match[2]] = value
    count = fields.get("NavMarkers/NumNavMarkers", ["ul 0"])
    if len(count) != 1 or int(count[0].split(" ", 1)[1]) != len(groups):
        raise ValueError("marker count mismatch")
    if any(set(group) != {"xPos", "yPos", "radius"} for group in groups.values()):
        raise ValueError("incomplete marker values")
    return Counter(tuple(sorted(typed_fields(group).items())) for group in groups.values())


def preserve_authored_units(before, after):
    """Owned insertions append; every existing authored unit field keeps its exact spelling."""
    def identity(key):
        match = re.match(r"(Part|Warrior)(\d+)", key)
        return match.groups() if match else None
    identities = {identity(key) for key in before} - {None}
    original = {key: value for key, value in before.items() if identity(key) in identities}
    retained = {key: value for key, value in after.items() if identity(key) in identities}
    if original != retained:
        raise ValueError("pre-existing authored Part/Warrior/cell fields changed")


def compare_frozen(directory, evidence, expected=FROZEN):
    manifest = verify_frozen(expected)
    reference = grade(expected, reference_evidence(manifest))
    actual = grade(directory, evidence)
    for extension in ("fit", "pak"):
        if (directory / f"e5_reference_before.{extension}").read_bytes() != (expected / f"e5_reference_before.{extension}").read_bytes():
            raise ValueError("placement baseline differs from frozen input")
    for family in PLACEMENTS:
        wanted, got = reference["placements"][family], actual["placements"][family]
        if wanted["part"] is not None:
            if inserted_unit(wanted) != inserted_unit(got):
                raise ValueError(f"frozen complete Part/Warrior/cell values differ: {family}")
        elif wanted["building"] != got["building"]:
            raise ValueError(f"frozen decoded placement differs: {family}")
        if marker_values(wanted["fitFields"]) != marker_values(got["fitFields"]):
            raise ValueError(f"frozen marker FIT values differ: {family}")
    actual["frozenComparison"] = "9/9 complete inserted Part/Warrior/cell values and decoded building records equal"
    return actual


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
