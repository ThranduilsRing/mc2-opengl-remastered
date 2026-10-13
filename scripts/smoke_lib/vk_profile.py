# scripts/smoke_lib/vk_profile.py
"""The `[VK_PROFILE v1]` line the engine prints once when the shared Vulkan device is chosen (GameOS/gameos/vk_device_profile.h).

WHAT: `parse(text)` returns one `Profile` per line (the device, both API versions, which required and wanted items are missing, the route of each item,
the features and extensions enabled); `grade(profiles)` returns the problems a smoke run must go red for.
WHY: the Vulkan context used to ask for 1.1 and enable nothing, so "Vulkan 1.2 minimum" was not checked anywhere. The engine now refuses a device that
lacks a REQUIRED item (the 1.2 floor, the limits and, by the owner ruling of 2026-10-13, dynamic rendering, synchronization2, timeline semaphores,
descriptor indexing and buffer device address; it falls back to GL and says so), but a refused Vulkan run still plays on GL and would pass; the line is
what lets a Vulkan smoke notice that the Vulkan it asked for is not the Vulkan it got. An optional item that is missing (`wanted_missing`) is not a
failure; it is reported so a later reader can see which routes a device offers.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field

LINE_RE = re.compile(
    r'\[VK_PROFILE v1\] verdict=(?P<verdict>\w+) device="(?P<device>[^"]*)" api=(?P<api>[\d.]+) instance_api=(?P<instance_api>[\d.]+) '
    r"floor=(?P<floor>[\d.]+) required_missing=(?P<required_missing>\S+) wanted_missing=(?P<wanted_missing>\S+) items=(?P<items>\S+) "
    r"enabled=(?P<enabled>\S+) extensions=(?P<extensions>\S+)\s*$")


def _names(text: str) -> "list[str]":
    return [] if text == "-" else text.split(",")


@dataclass
class Profile:
    verdict: str
    device: str
    api: str
    instance_api: str
    floor: str
    required_missing: "list[str]" = field(default_factory=list)
    wanted_missing: "list[str]" = field(default_factory=list)
    routes: "dict[str, str]" = field(default_factory=dict)     # item id -> core | ext | limit | -
    enabled: "list[str]" = field(default_factory=list)         # feature names enabled on the device
    extensions: "list[str]" = field(default_factory=list)      # device extensions the profile enabled

    @property
    def accepted(self) -> bool:
        return self.verdict == "ok" and not self.required_missing


def parse(text: str) -> "list[Profile]":
    """Every well-formed `[VK_PROFILE v1]` line of `text`, in order. A truncated line is skipped."""
    out = []
    for line in text.splitlines():
        at = line.find("[VK_PROFILE v1]")
        if at < 0:
            continue
        m = LINE_RE.search(line[at:])
        if not m:
            continue
        routes = dict(item.partition(":")[::2] for item in _names(m.group("items")))
        out.append(Profile(m.group("verdict"), m.group("device"), m.group("api"), m.group("instance_api"), m.group("floor"),
                           _names(m.group("required_missing")), _names(m.group("wanted_missing")), routes, _names(m.group("enabled")),
                           _names(m.group("extensions"))))
    return out


def grade(profiles: "list[Profile]") -> "list[str]":
    """One message per reason the run is red: a refused verdict, a missing required item, or a verdict that contradicts its own item list."""
    problems = []
    for p in profiles:
        if p.verdict != "ok":
            problems.append(f"Vulkan device profile REFUSED {p.device!r} (api {p.api}, floor {p.floor}): required item(s) missing: "
                            f"{','.join(p.required_missing) or 'unspecified'}; the engine fell back to GL")
        elif p.required_missing:
            problems.append(f"Vulkan device profile says ok for {p.device!r} but lists required item(s) missing: {','.join(p.required_missing)}")
    return problems


def describe(p: Profile) -> str:
    """`ok RX 7900 XTX api 1.4.0: core dynamic_rendering, ext ...; wanted missing: -`."""
    by_route: "dict[str, list[str]]" = {}
    for item, route in p.routes.items():
        by_route.setdefault(route, []).append(item)
    routes = ", ".join(f"{route}: {' '.join(items)}" for route, items in sorted(by_route.items()))
    return f"{p.verdict} {p.device} api {p.api} (instance {p.instance_api}): {routes}; wanted missing: {','.join(p.wanted_missing) or '-'}"
