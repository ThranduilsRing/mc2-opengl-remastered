# tools/unit_converter/qualified_id.py
"""UNIT-ID-NAMESPACE-1 (Phase-3 slice 6) — Python mirror of code/qualified_id.h.

The `namespace:name` string-convention + deterministic override-precedence layer.
Keep this in lockstep with the C++ header (code/qualified_id.h) — the doctest and
these pytest cases assert the SAME charset rules, canonicalization, core-reserved
policy, and precedence (core < earlier-mod < later-mod).

Pure/offline: stdlib only, no engine, no I/O.

FORMAT: `namespace:name`
  namespace : starts with [a-z], then [a-z0-9_], length 1..32
  name      : starts with [a-z], then [a-z0-9_], length 1..64
  A bare `name` (no ':') canonicalizes to `core:name` (stock reserved ns).
  `core` is the RESERVED stock namespace — mods must NOT author into `core:`.

OVERRIDE PRECEDENCE (deterministic): `core` < earlier-mod < later-mod. Load order
is authoritative; `core` is the base layer (load order 0). A later-loaded source
overrides an earlier one for the same canonical id.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

CORE_NAMESPACE = "core"
CORE_LOAD_ORDER = 0

_NAMESPACE_RE = re.compile(r"^[a-z][a-z0-9_]{0,31}$")   # 1..32 chars
_NAME_RE = re.compile(r"^[a-z][a-z0-9_]{0,63}$")        # 1..64 chars


def is_valid_namespace(ns: str) -> bool:
    return bool(_NAMESPACE_RE.match(ns))


def is_valid_name(nm: str) -> bool:
    return bool(_NAME_RE.match(nm))


@dataclass(frozen=True)
class QualifiedId:
    ns: str
    name: str

    def __str__(self) -> str:
        return f"{self.ns}:{self.name}"

    @property
    def is_core(self) -> bool:
        return self.ns == CORE_NAMESPACE


def parse_qualified_id(raw: str) -> Optional[QualifiedId]:
    """Parse an id into a canonical QualifiedId, or None when invalid.

    "core:hunchback" -> QualifiedId(core, hunchback)
    "MyMod:UrbanMech" -> QualifiedId(mymod, urbanmech)   (lowercased)
    "hunchback"       -> QualifiedId(core, hunchback)     (bare -> reserved core:)
    "" / "a:b:c" / bad-charset / over-length -> None
    """
    if not raw:
        return None
    s = raw.lower()
    parts = s.split(":")
    if len(parts) == 1:
        nm = parts[0]
        if not is_valid_name(nm):
            return None
        return QualifiedId(CORE_NAMESPACE, nm)
    if len(parts) != 2:
        return None  # zero or >1 separators
    ns, nm = parts
    if not is_valid_namespace(ns) or not is_valid_name(nm):
        return None
    return QualifiedId(ns, nm)


def is_valid_qualified_id(raw: str) -> bool:
    return parse_qualified_id(raw) is not None


def canonicalize_qualified_id(raw: str) -> Optional[str]:
    """Canonical `namespace:name` string, or None when invalid."""
    q = parse_qualified_id(raw)
    return str(q) if q is not None else None


def is_mod_authorable(raw: str) -> bool:
    """True iff valid AND not in the reserved `core` namespace."""
    q = parse_qualified_id(raw)
    return q is not None and not q.is_core


def overrides(candidate_order: int, incumbent_order: int) -> bool:
    """True if a def at `candidate_order` overrides one at `incumbent_order`.

    Strictly-higher load order wins; equal orders do NOT override. `core` is
    load order 0, so any mod (order > 0) beats stock and later mods beat earlier.
    """
    return candidate_order > incumbent_order


def resolve_winning_order(orders: Sequence[int]) -> int:
    """Winning load order among several definitions (max = last-loaded wins)."""
    return max(orders) if orders else CORE_LOAD_ORDER


def merge_id_sources(sources: Iterable[Tuple[int, Iterable[str]]]) -> Dict[str, int]:
    """Deterministically merge multiple id sources by load order.

    `sources` is an iterable of (load_order, ids). Returns a dict mapping each
    canonical id -> the winning (max) load order that defines it. Later-loaded
    sources override earlier ones for the same canonical id. Invalid ids raise
    ValueError (author error — surface it, don't silently drop). Deterministic:
    the result depends only on the ids and their load orders, not iteration
    order (ties resolve to the strictly-higher order via `overrides`).
    """
    winners: Dict[str, int] = {}
    for load_order, ids in sources:
        for raw in ids:
            canon = canonicalize_qualified_id(raw)
            if canon is None:
                raise ValueError(f"invalid qualified id: {raw!r}")
            prev = winners.get(canon)
            if prev is None or overrides(load_order, prev):
                winners[canon] = load_order
    return winners


def validate_ids(ids: Iterable[str]) -> List[str]:
    """Return the list of ids that FAIL validation (empty list == all conform)."""
    return [i for i in ids if not is_valid_qualified_id(i)]
