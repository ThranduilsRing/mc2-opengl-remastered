# scripts/smoke_lib/vk_validation.py
"""The `[VK_VALIDATION v1]` lines the engine prints for every message of the Khronos validation layer (GameOS/gameos/vulkan_context.cpp).

WHAT: `parse(text)` returns one `Message` per well-formed line (severity, message id, one-line text); `summarize(messages)` counts errors and warnings and the
distinct ids; `describe(messages)` is the one report line. The smoke runner grades any `severity=error` line red (marker bucket `vk_validation_error`);
warnings are counted and listed, never red.
WHY: the layer is only on when a Vulkan smoke asks for it (`MC2_VULKAN_VALIDATION=1`, never for a player), and before this line its messages went to stderr as
free text that nothing graded, so a mission could be green with validation errors in its log. A requested layer that is not installed prints an error line
with the id `layer_unavailable`: a validation tier that silently runs without the layer would look green with no coverage.
The layer limits repeats of one message id itself (ten by default), so the counts here are messages delivered, not draw calls affected.
"""
from __future__ import annotations

import json
import re
from collections import Counter
from dataclasses import dataclass, field
from datetime import date
from pathlib import Path

LINE_RE = re.compile(r'\[VK_VALIDATION v1\] severity=(?P<severity>\w+) id=(?P<id>\S+) text="(?P<text>.*)"\s*$')

ERROR = "error"
WARNING = "warning"
INFO = "info"
UNCONFIRMED = "Vulkan validation was requested (MC2_VULKAN_VALIDATION) and the run never confirmed the layer on (no layer_enabled line, or no canary_fired): it was not validated"
LAYER_ENABLED_ID = "layer_enabled"       # the engine's info line when the messenger is up: the proof that a clean run was actually validated


@dataclass(frozen=True)
class Message:
    severity: str
    id: str
    text: str


@dataclass
class Summary:
    errors: int = 0
    warnings: int = 0
    error_ids: "Counter[str]" = field(default_factory=Counter)
    warning_ids: "Counter[str]" = field(default_factory=Counter)

    @property
    def distinct_ids(self) -> "list[str]":
        """Every id seen, errors first, each list sorted for a stable report."""
        return sorted(self.error_ids) + sorted(i for i in self.warning_ids if i not in self.error_ids)


def parse(text: str) -> "list[Message]":
    """Every well-formed `[VK_VALIDATION v1]` line of `text`, in order. A truncated or foreign line is skipped."""
    out = []
    for line in text.splitlines():
        at = line.find("[VK_VALIDATION v1]")
        if at < 0:
            continue
        m = LINE_RE.search(line[at:])
        if m:
            out.append(Message(m.group("severity"), m.group("id"), m.group("text")))
    return out


def summarize(messages: "list[Message]") -> Summary:
    s = Summary()
    for m in messages:
        if m.severity == ERROR:
            s.errors += 1
            s.error_ids[m.id] += 1
        elif m.severity == WARNING:
            s.warnings += 1
            s.warning_ids[m.id] += 1
    return s


def layer_enabled(messages: "list[Message]") -> bool:
    """True when the run printed the engine's `layer_enabled` info line, i.e. the layer and its messenger were live."""
    return any(m.severity == INFO and m.id == LAYER_ENABLED_ID for m in messages)


def requested(environ) -> bool:
    """True when the run's environment asks for the layer: `MC2_VULKAN_VALIDATION` set to anything but the engine's own "off" spellings (an empty value means the core preset)."""
    value = environ.get("MC2_VULKAN_VALIDATION")
    return value is not None and value.strip().lower() not in ("0", "off")


def grade(messages: "list[Message]", asked: bool = False) -> "list[str]":
    """One message per distinct error id, plus one when the run asked for validation and never confirmed the layer: why the run is red. Warnings never appear here.

    A run that asks for validation and prints no `layer_enabled` line (a GL fallback without a refusal, a mission that never creates the Vulkan context) was not
    validated at all; it must not look like a clean one.
    """
    s = summarize(messages)
    reasons = [f"Vulkan validation error {i} x{n}" for i, n in sorted(s.error_ids.items())]
    if asked and not confirmed(messages):
        reasons.append(UNCONFIRMED)
    return reasons


def describe(messages: "list[Message]") -> str:
    """`errors=2 (VUID-a x1, VUID-b x1) warnings=3 (VUID-c x3)`, ids sorted; `errors=0 warnings=0` for a clean run."""
    s = summarize(messages)

    def part(label: str, n: int, ids: "Counter[str]") -> str:
        listed = ", ".join(f"{i} x{c}" for i, c in sorted(ids.items()))
        return f"{label}={n}" + (f" ({listed})" if listed else "")

    canary = "canary=fired" if canary_fired(messages) else "canary=MISSING"
    return part("errors", s.errors, s.error_ids) + " " + part("warnings", s.warnings, s.warning_ids) + (f" {canary}" if layer_enabled(messages) else " (layer not confirmed on)")


# ---- per-host canary, raw-VUID backstop and the reviewed known list (lane vk-validation-ci) ----
CANARY_ID = "canary_fired"          # the engine's info line when its deliberate zero-size vkCreateBuffer drew a layer message: the layer is live on THIS host
KNOWN_FILE = Path(__file__).resolve().parents[2] / "tests" / "syncval" / "known_vuids.json"
# A raw layer line the engine callback did not format (loader/layer output on stderr before the messenger, a second instance, a tool process): the backstop.
RAW_RE = re.compile(r"^(?!.*\[VK_VALIDATION v1\]).*Validation Error: \[ ")


def canary_fired(messages: "list[Message]") -> bool:
    return any(m.severity == INFO and m.id == CANARY_ID for m in messages)


def confirmed(messages: "list[Message]") -> bool:
    """Layer messenger up AND the canary reported: a clean run on a layer that is merely loaded but mute is not validated."""
    return layer_enabled(messages) and canary_fired(messages)


def known_entries(today: "date | None" = None, path: Path = KNOWN_FILE) -> "list[dict]":
    """The reviewed known-hit entries that have not expired. A missing or unreadable file is an empty list: everything is red."""
    today = today or date.today()
    try:
        entries = json.loads(path.read_text(encoding="utf-8")).get("known", [])
    except (OSError, ValueError):
        return []
    return [e for e in entries if date.fromisoformat(e["expires"]) >= today]


def is_known(msg_id: str, text: str, entries: "list[dict]") -> bool:
    return any(e["id"] == msg_id and e.get("text_contains", "") in text for e in entries)


def known_error_count(messages: "list[Message]", entries: "list[dict] | None" = None) -> int:
    """Error messages covered by an unexpired known entry (not red)."""
    entries = known_entries() if entries is None else entries
    return sum(1 for m in messages if m.severity == ERROR and is_known(m.id, m.text, entries))


def raw_is_known(line: str, entries: "list[dict] | None" = None) -> bool:
    entries = known_entries() if entries is None else entries
    return any(e["id"] in line and e.get("text_contains", "") in line for e in entries)
