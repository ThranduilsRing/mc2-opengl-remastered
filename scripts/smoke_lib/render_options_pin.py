# scripts/smoke_lib/render_options_pin.py
"""The render options a proof ran with, recorded and compared.

WHAT: the 17 options.cfg render options (Gates/render_options.h) as one value record per run, the rule that reads them from an install the way the
engine does (options.cfg, then MC2_OPTIONS_OVERRIDE), and the comparison that refuses to grade two runs whose options differ.
WHY: the settings moved 14 look/lighting switches and 3 post-FX keys out of the environment into the install's options.cfg. An install whose
options.cfg says `StaticPropPbr = TRUE` renders a different picture and costs a different frame time than a clean one, and nothing in the run said
so. Like the drawable size (render_resolution.py), the options are an input of every capture, Tracy trace and smoke report; a mismatch between the
two sides of an A/B is a hard failure unless the caller names the keys it varies on purpose.
GRAMMAR: the same strict one the engine uses (Gates/render_options.h setFromText, Gates/render_options_cfg.h loadFromText): a bool is exactly
TRUE/FALSE/1/0 in any case, a long a whole integer, a float a finite number, nothing after the value; a rejected value keeps the default. The table
(keys, kinds, defaults, bounds) is read from the header so it cannot drift from the engine. Where the engine reports its own values (the capture
sidecar `render_options`), the engine's values are the record and a disagreement with this module's reading is a failure.
"""
from __future__ import annotations

import json
import re
import struct
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Iterable, Mapping, Optional

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "Gates" / "render_options.h"
OVERRIDE_ENV = "MC2_OPTIONS_OVERRIDE"
BLOCK = "[mechcommander2]"


class OptionsMismatch(Exception):
    """Two runs ran with different render options (or one of them did not record them)."""


@dataclass(frozen=True)
class Option:
    key: str
    kind: str          # "bool" | "long" | "float"
    default: object
    lo: float
    hi: float


def _default_of(kind: str, text: str):
    text = text.strip()
    if kind == "bool":
        return text == "true"
    if kind == "long":
        return int(text)
    return float(text.rstrip("fF"))


@lru_cache(maxsize=None)
def table(header: Path = HEADER) -> "tuple[Option, ...]":
    """The option table in declaration order, read from Gates/render_options.h."""
    src = header.read_text(encoding="utf-8", errors="replace")
    body = src[src.index("struct RenderOptions {"):src.index("bool operator==")]
    defaults = {m.group(2): (m.group(1), m.group(3))
                for m in re.finditer(r"^\s*(bool|int|float)\s+(\w+)\s*=\s*([^;]+);", body, re.M)}
    rows = []
    pat = re.compile(r'detail::(bool|long|float)Opt\s*\(\s*"(\w+)"\s*,\s*&RenderOptions::(\w+)\s*(?:,\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+))?\s*\)')
    for m in pat.finditer(src):
        kind, key, member, lo, hi = m.groups()
        ctype, text = defaults[member]
        rows.append(Option(key, kind, _default_of(kind, text), float(lo) if lo else 0.0, float(hi) if hi else 1.0))
    if not rows:
        raise RuntimeError(f"no option rows found in {header}")
    return tuple(rows)


def keys() -> "tuple[str, ...]":
    return tuple(o.key for o in table())


def _f32(x: float) -> float:
    return struct.unpack("f", struct.pack("f", x))[0]


_LONG = re.compile(r"-?\d+")
_FLOAT = re.compile(r"-?(\d+(\.\d*)?|\.\d+)([eE][+-]?\d+)?")


def parse_value(opt: Option, text: str):
    """(value, None) for an accepted value, (None, reason) for a rejected one; the reason is the engine's `why=` word."""
    text = text.strip(" \t\r")
    if opt.kind == "bool":
        low = text.lower()
        if low in ("true", "1"):
            return True, None
        if low in ("false", "0"):
            return False, None
        return None, "garbage"
    if opt.kind == "long":
        if not _LONG.fullmatch(text):
            return None, "garbage"
        value = int(text)
    else:
        if not _FLOAT.fullmatch(text):
            return None, "garbage"
        value = float(text)
        if value != value or value in (float("inf"), float("-inf")):
            return None, "garbage"
    if value < opt.lo or value > opt.hi:
        return None, "out_of_range"
    return value, None


def _lines(text: str) -> "list[str]":
    return re.split(r"\r\n|\r|\n", text)


def cfg_values(text: str) -> "dict[str, str]":
    """key (lower case) -> value text of the first line of each key in the [MechCommander2] block, like cfg::get."""
    found: "dict[str, str]" = {}
    in_block = False
    for raw in _lines(text):
        line = raw.strip(" \t\r")
        if line.startswith("["):
            in_block = line.lower() == BLOCK
            continue
        if not in_block or len(line) < 5 or line[1] not in " \t":
            continue
        rest = line[2:].strip(" \t\r")
        k = 0
        while k < len(rest) and rest[k] not in " \t=":
            k += 1
        key, rest = rest[:k], rest[k:].strip(" \t\r")
        if key and rest.startswith("="):
            found.setdefault(key.lower(), rest[1:].strip(" \t\r"))
    return found


def apply_cfg(values: "dict[str, object]", text: str, rejected: "list[dict]") -> None:
    raw = cfg_values(text)
    for opt in table():
        if opt.key.lower() not in raw:
            continue
        v, why = parse_value(opt, raw[opt.key.lower()])
        if why is None:
            values[opt.key] = v
        else:
            rejected.append({"key": opt.key, "value": raw[opt.key.lower()], "why": why, "from": "options.cfg"})


def apply_override(values: "dict[str, object]", spec: str, rejected: "list[dict]") -> None:
    by_lower = {o.key.lower(): o for o in table()}
    for item in spec.split(","):
        item = item.strip(" \t\r")
        if not item:
            continue
        key, eq, val = item.partition("=")
        opt = by_lower.get(key.strip(" \t\r").lower())
        if opt is None:
            rejected.append({"key": key.strip(), "value": val.strip(), "why": "unknown_key", "from": OVERRIDE_ENV})
            continue
        if not eq:
            rejected.append({"key": opt.key, "value": "", "why": "garbage", "from": OVERRIDE_ENV})
            continue
        v, why = parse_value(opt, val)
        if why is None:
            values[opt.key] = v
        else:
            rejected.append({"key": opt.key, "value": val.strip(), "why": why, "from": OVERRIDE_ENV})


def record(install: "Optional[Path]", env: "Optional[Mapping[str, str]]" = None) -> dict:
    """The effective options of a run launched from `install` with child environment `env`: defaults, then the install's options.cfg
    (a missing file keeps the defaults), then MC2_OPTIONS_OVERRIDE. The record is what gets written into manifests and reports."""
    values: "dict[str, object]" = {o.key: o.default for o in table()}
    rejected: "list[dict]" = []
    cfg_path = Path(install) / "options.cfg" if install else None
    if cfg_path is not None and cfg_path.is_file():
        apply_cfg(values, cfg_path.read_text(encoding="latin-1"), rejected)
    spec = (env or {}).get(OVERRIDE_ENV, "")
    if spec:
        apply_override(values, spec, rejected)
    return {"schema": 1, "source": "options.cfg + " + OVERRIDE_ENV, "values": values, "rejected": rejected}


def canon(opt: Option, v):
    """One comparable form per value (floats at the engine's float32 precision)."""
    if opt.kind == "float":
        return _f32(float(v))
    if opt.kind == "bool":
        return bool(v)
    return int(v)


def differences(a: Mapping[str, object], b: Mapping[str, object], vary: Iterable[str] = ()) -> "list[str]":
    """One line per key whose value differs between the two value maps, ignoring the keys named in `vary`."""
    allowed = {k.lower() for k in vary}
    known = {k.lower() for k in keys()}
    bad = sorted(allowed - known)
    if bad:
        raise ValueError(f"--vary names unknown option(s) {bad}; known: {', '.join(keys())}")
    out = []
    for opt in table():
        if opt.key.lower() in allowed:
            continue
        if opt.key not in a or opt.key not in b:
            out.append(f"{opt.key}: A={a.get(opt.key, 'unrecorded')} B={b.get(opt.key, 'unrecorded')}")
        elif canon(opt, a[opt.key]) != canon(opt, b[opt.key]):
            out.append(f"{opt.key}: A={a[opt.key]} B={b[opt.key]}")
    return out


def describe(values: Mapping[str, object]) -> str:
    """'all defaults', or the options that differ from the table defaults as Key=value pairs (what a report line shows)."""
    diff = [f"{o.key}={values[o.key]}" for o in table() if o.key in values and canon(o, values[o.key]) != canon(o, o.default)]
    return ", ".join(diff) if diff else "all defaults"


def values_of(rec: "Optional[Mapping]") -> "Optional[Mapping[str, object]]":
    return (rec or {}).get("values") if isinstance(rec, Mapping) else None


def require_same(rec_a, rec_b, vary: Iterable[str] = (), a_name: str = "A", b_name: str = "B") -> None:
    """Raise OptionsMismatch unless both runs recorded their options and they agree on every key not in `vary`."""
    va, vb = values_of(rec_a), values_of(rec_b)
    for name, v in ((a_name, va), (b_name, vb)):
        if not v:
            raise OptionsMismatch(f"{name} carries no render-option record, so its options are unknown; re-run it with a harness that records them "
                                  f"(scripts/smoke_lib/render_options_pin.py)")
    diffs = differences(va, vb, vary)
    if diffs:
        raise OptionsMismatch(
            f"render options differ between {a_name} and {b_name} ({len(diffs)} key(s)): " + "; ".join(diffs) +
            ". An A/B over different options is not a comparison of the change; make the options.cfg files agree, or name the keys that "
            "differ on purpose with --vary Key[,Key].")


def check_engine(declared: Mapping[str, object], engine: Optional[Mapping[str, object]]) -> "list[str]":
    """Disagreements between this module's reading of the install and what the engine reported (empty when it did not report)."""
    if not engine:
        return []
    return differences(declared, engine)


def write(path: Path, rec: dict) -> None:
    Path(path).write_text(json.dumps(rec, indent=2) + "\n", encoding="utf-8", newline="\n")


def read(path: Path) -> "Optional[dict]":
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
