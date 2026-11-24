"""Pure helpers for the BT2018 prop library: mesh-name grouping, category classification and
stock-prop categories. No UnityPy import, so the unit tests run anywhere.

Vegetation (trees, bushes, grass) is excluded everywhere: the project ships its own tree pack.
"""
from __future__ import annotations

import re

CATEGORIES = ("building", "industrial", "military", "tower", "wall_fence", "bridge", "rock", "cliff", "debris", "vehicle")
EXCLUDED = "excluded"

_LOD = re.compile(r"^(?P<base>.+?)_LOD(?P<n>\d+)(?:_.*)?$")
_PIECE = re.compile(r"_(split|fract)_\d+$")

_VEGETATION = ("tree", "bush", "grass", "plant", "flower", "shrub", "foliage", "fern", "reed", "vine", "leaf", "hedge", "moss")
_JUNK = ("floor_", "polysurface", "greeble", "_col", "plane", "sphere", "cube", "cylinder", "quad", "water", "region", "decal",
         "terrain", "skybox", "proc_", "procedural", "uixsvg", "_head", "torso", "cockpit", "_umbra", "reference_mesh", "falling",
         "pcone", "pipe7", "ppipe", "mech_", "_nuv")

# Ordered: the first keyword hit wins. Name based, then bounds based as a fallback.
_NAME_RULES = (
    ("bridge", ("bridge",)),
    ("wall_fence", ("wall", "barrier", "fence", "gate", "razor", "sandbag", "barricade")),
    ("tower", ("tower", "antenna", "mast", "pylon", "smokestack", "chimney")),
    ("debris", ("rubble", "debris", "junk", "wreck", "scrap", "crate", "container", "dumpster", "pile")),
    ("vehicle", ("truck", "vehicle", "jeep", "car", "carrier", "dropship", "apc", "tank_")),
    ("cliff", ("cliff", "mesa", "canyon", "ridge")),
    ("rock", ("rock", "boulder", "stone", "spire", "crag")),
    ("military", ("military", "bunker", "turret", "longtom", "hangar", "barracks", "armory", "armored", "prison", "landing", "mechgate", "mechbay")),
    ("industrial", ("industrial", "tank", "pipe", "refinery", "factory", "mining", "chemical", "silo", "crane", "unloading",
                    "warehouse", "generator", "powerplant", "storage", "fuel", "smelter", "conveyor", "pump")),
    ("building", ("building", "bldg", "apartment", "villa", "village", "house", "office", "city", "civil", "comm", "dome",
                  "hotel", "shop", "market", "hospital", "school", "tent", "quonset")),
)


def parse_name(name):
    """-> dict(family, lod, kind). kind: lod | shell | piece | col | plain | instance."""
    if "|" in name:
        return {"family": name.split("|")[0], "lod": None, "kind": "instance"}
    low = name.lower()
    if low.endswith("_shell_col") or low.endswith("_col") or "_col_" in low:
        return {"family": re.sub(r"_(shell_)?COL.*$", "", name, flags=re.I), "lod": None, "kind": "col"}
    if low.endswith("_shell"):
        return {"family": name[:-6], "lod": 0, "kind": "shell"}
    if _PIECE.search(name):
        return {"family": _PIECE.sub("", name), "lod": None, "kind": "piece"}
    m = _LOD.match(name)
    if m:
        return {"family": m["base"], "lod": int(m["n"]), "kind": "lod"}
    return {"family": name, "lod": 0, "kind": "plain"}


def is_vegetation(name):
    low = name.lower()
    return any(k in low for k in _VEGETATION) or low.startswith(("envmdltree", "envmdlbush", "envmdldeco"))


def is_junk(name):
    low = name.lower()
    return any(k in low for k in _JUNK)


def group_assets(mesh_names):
    """Mesh names -> {family: {"lods": [mesh names by LOD order], "shell": bool}}.

    A family with a *_shell mesh is an intact building (the split/fract pieces are destruction
    debris and are ignored). Otherwise the family's *_LODn meshes (or the plain mesh) are the LODs.
    Colliders, instances, pieces, vegetation and generic scene meshes are dropped.
    """
    fams = {}
    for name in mesh_names:
        info = parse_name(name)
        if info["kind"] in ("instance", "piece", "col") or is_vegetation(name) or is_junk(name):
            continue
        fam = fams.setdefault(info["family"], {"shell": None, "lods": {}})
        if info["kind"] == "shell":
            fam["shell"] = name
        else:
            fam["lods"][info["lod"]] = name
    out = {}
    for family, fam in fams.items():
        if is_vegetation(family) or is_junk(family):
            continue
        if fam["shell"]:
            lods = [fam["shell"]]
        else:
            lods = [fam["lods"][k] for k in sorted(fam["lods"])]
        if lods:
            out[family] = {"lods": lods, "shell": bool(fam["shell"])}
    return out


def classify(family, size_m=None):
    """Category for a BT2018 asset family; size_m = (x, y_up, z) metres when known."""
    low = family.lower()
    if is_vegetation(family):
        return EXCLUDED
    for cat, words in _NAME_RULES:
        if any(w in low for w in words):
            if cat == "rock" and size_m is not None and size_m[1] > 15:
                return "cliff"
            return cat
    if size_m is not None:
        x, y, z = size_m
        foot = max(x, z)
        if y > 2.5 * max(min(x, z), 0.1) and y > 12:
            return "tower"
        if y < 0.15 * foot:
            return EXCLUDED  # ground patch or decal-like
        if foot > 8:
            return "building"
    return EXCLUDED


_STOCK_RULES = (
    ("wall_fence", ("fence", "barricade", "sandbag", "wall", "netting", "razor")),
    ("tower", ("tower", "antenna", "mast")),
    ("debris", ("junk", "mechpiece", "crash", "lifeboat", "crate", "dumpster", "locker", "pile", "wreck", "rubble", "dfield")),
    ("vehicle", ("truck", "jeep", "buggy", "carprop", "motorcycle", "transport", "prop", "jet", "slayer", "shilone")),
    ("cliff", ("cliff", "formation", "spire")),
    ("rock", ("rock", "boulder")),
    ("military", ("bunker", "turret", "hangar", "barracks", "military", "checkpoint", "hq", "command", "armory", "pillbox", "gate")),
    ("industrial", ("tank", "pipe", "refinery", "factory", "silo", "crane", "generator", "power", "fuel", "warehouse", "plant")),
    ("building", ("tent", "quonset", "dome", "house", "building", "shack", "hut", "office", "hotel", "civ")),
)


def stock_category(appearance, size_m=None):
    """Category for a stock MC2 appearance name (lowercase); vegetation maps to EXCLUDED."""
    low = appearance.lower()
    if re.match(r"^(tc\d|palm|maple|oak|pine|elm|birch|bush|tree|cactus|shrub|fern|grass|dead_tree|sapling)", low):
        return EXCLUDED
    for cat, words in _STOCK_RULES:
        if any(w in low for w in words):
            return cat
    if size_m is not None:
        x, y, z = size_m
        return "building" if max(x, z) > 8 else "debris"
    return "debris"


def short_id(family, taken=()):
    """Cook id: 'bt_' + lowercase alphanumerics, at most 16 chars (cook.py node-name truncation limit).

    Long names keep a 3 hex digit hash suffix so distinct families never collide after trimming.
    """
    import hashlib
    core = re.sub(r"[^a-z0-9]", "", family.lower())
    cand = "bt_" + core
    if len(cand) > 16:
        cand = "bt_" + core[:10] + hashlib.sha1(family.encode()).hexdigest()[:3]
    n = 0
    base = cand
    while cand in taken:
        n += 1
        cand = base[:15] + str(n % 10)
    return cand
