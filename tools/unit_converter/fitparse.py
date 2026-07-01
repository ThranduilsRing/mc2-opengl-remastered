# tools/unit_converter/fitparse.py
"""Minimal, read-only parsers for the stock MC2 source formats.

Two formats are consumed:
  * FIT ini  -- vehicle .fit type files and per-unit data/tgl/<name>.ini.
    Line-oriented: `[BlockName]` headers, then `<type> Key = value` entries
    where <type> is a FitIni type sigil (st/l/f/uc/b/c...). `//` starts a
    comment; the stream is bracketed by `FITini` / `FITend` sentinels.
  * chassis CSV -- the mech BaseChassisInfo/VariantInfo grid. Comma-separated;
    the meaningful cell is (label in col0, value in col1) for the header block,
    and `ItemNMasterId` rows for the loadout.

Deterministic and side-effect free. No stdlib `random`, no network.
"""
from __future__ import annotations

import csv
import re
from typing import Dict, List, Optional, Tuple

# `st Key = "value"`  /  `l Key=5`  /  `f Distance0 = 0.0`  /  `b Flag=FALSE`
_FIT_ENTRY = re.compile(r"^\s*(st|l|f|uc|us|c|b|ul)\s+([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$")
_FIT_BLOCK = re.compile(r"^\s*\[([^\]]+)\]\s*$")


def _strip_comment(line: str) -> str:
    # FitIni comments are `//` to end of line. Quoted strings in stock data do
    # not contain `//`, so a plain split is safe for these files.
    idx = line.find("//")
    return line[:idx] if idx >= 0 else line


def _coerce(sigil: str, raw: str):
    raw = raw.strip()
    if sigil == "st":
        return raw.strip().strip('"')
    if sigil in ("l", "uc", "us", "c", "ul"):
        try:
            return int(raw)
        except ValueError:
            return raw
    if sigil == "f":
        try:
            return float(raw)
        except ValueError:
            return raw
    if sigil == "b":
        return raw.strip().upper() in ("TRUE", "1", "YES")
    return raw


class FitIni:
    """Parsed FIT ini: ordered blocks -> {key: value}. Block names are unique
    in the stock unit files consumed here; the first wins if duplicated."""

    def __init__(self) -> None:
        self.blocks: Dict[str, Dict[str, object]] = {}
        self.block_order: List[str] = []

    @classmethod
    def parse(cls, text: str) -> "FitIni":
        self = cls()
        current: Optional[str] = None
        for rawline in text.splitlines():
            line = _strip_comment(rawline).rstrip()
            if not line.strip():
                continue
            if line.strip() in ("FITini", "FITend"):
                continue
            mblock = _FIT_BLOCK.match(line)
            if mblock:
                current = mblock.group(1).strip()
                self.blocks.setdefault(current, {})
                if current not in self.block_order:
                    self.block_order.append(current)
                continue
            mentry = _FIT_ENTRY.match(line)
            if mentry and current is not None:
                sigil, key, raw = mentry.group(1), mentry.group(2), mentry.group(3)
                self.blocks[current][key] = _coerce(sigil, raw)
        return self

    @classmethod
    def load(cls, path: str) -> "FitIni":
        with open(path, "r", encoding="latin-1") as fh:
            return cls.parse(fh.read())

    def has_block(self, name: str) -> bool:
        return name in self.blocks

    def get(self, block: str, key: str, default=None):
        return self.blocks.get(block, {}).get(key, default)


# --------------------------------------------------------------------------
# chassis CSV
# --------------------------------------------------------------------------

class ChassisCsv:
    """Row-list view of a stock mech chassis CSV, with label lookups.

    The header block encodes (label, value) as (col0, col1) or (col3, col4)
    on the same physical row; we index every `label -> value` pair we can find
    from cols (0,1) and (3,4). Loadout `ItemNMasterId` rows are exposed raw."""

    def __init__(self, rows: List[List[str]]) -> None:
        self.rows = rows
        self._labels: Dict[str, str] = {}
        for r in rows:
            for lcol, vcol in ((0, 1), (3, 4)):
                if len(r) > vcol and r[lcol].strip():
                    # first occurrence wins (stable, deterministic)
                    self._labels.setdefault(r[lcol].strip(), r[vcol].strip())

    @classmethod
    def load(cls, path: str) -> "ChassisCsv":
        with open(path, "r", encoding="latin-1", newline="") as fh:
            rows = list(csv.reader(fh))
        return cls(rows)

    def label(self, name: str, default: Optional[str] = None) -> Optional[str]:
        return self._labels.get(name, default)

    def first_variant_name(self) -> Optional[str]:
        """VariantName of the first `VariantInfo` block (the stock default)."""
        seen_variant = False
        for r in self.rows:
            if r and r[0].strip() == "VariantInfo":
                seen_variant = True
            if seen_variant and r and r[0].strip() == "VariantName" and len(r) > 1:
                return r[1].strip()
        return None

    def item_rows(self, first_variant_only: bool = True) -> List[Tuple[int, List[str]]]:
        """(item_index, row) for `ItemNMasterId` loadout rows.

        A chassis CSV holds several `VariantInfo` blocks (Prime, then named
        variants). For a coherent single-loadout conversion we take only the
        FIRST variant's items by default (stop at the 2nd `VariantInfo`)."""
        out: List[Tuple[int, List[str]]] = []
        pat = re.compile(r"^Item(\d+)MasterId$")
        variant_count = 0
        for r in self.rows:
            if not r:
                continue
            label = r[0].strip()
            if label == "VariantInfo":
                variant_count += 1
                if first_variant_only and variant_count > 1:
                    break
            m = pat.match(label)
            if m:
                out.append((int(m.group(1)), r))
        return out


class CompBas:
    """compbas.csv component table: MasterID(col0) -> (Type, Name).

    MasterID is the join key both the mech loadout `CompBas#` cell and the
    vehicle `[Item:N] MasterID` reference. `Type` (EnergyWeapon /
    BallisticWeapon / MissileWeapon / JumpJet / Engine ...) yields the neutral
    weapon-class tag and the jumpjet signal."""

    def __init__(self, by_id: Dict[int, Tuple[str, str]]) -> None:
        self.by_id = by_id

    @classmethod
    def load(cls, path: str) -> "CompBas":
        by_id: Dict[int, Tuple[str, str]] = {}
        with open(path, "r", encoding="latin-1", newline="") as fh:
            for r in csv.reader(fh):
                if not r:
                    continue
                try:
                    mid = int(r[0].strip())
                except (ValueError, IndexError):
                    continue
                ctype = r[1].strip() if len(r) > 1 else ""
                name = r[2].strip() if len(r) > 2 else ""
                by_id.setdefault(mid, (ctype, name))
        return cls(by_id)

    def type_of(self, master_id: int) -> str:
        return self.by_id.get(master_id, ("", ""))[0]

    def name_of(self, master_id: int) -> str:
        return self.by_id.get(master_id, ("", ""))[1]
