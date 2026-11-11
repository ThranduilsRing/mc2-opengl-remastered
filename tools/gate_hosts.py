#!/usr/bin/env python3
"""Host capabilities for the merge gate (tools/gate_hosts.json): which host is this, and can it build the MFC editor.

The host name is $MC2_GATE_HOST (tools/laptop_gate.py sets it to `laptop` in the gate environment), else the file's `default` (`pc`).
cmake/GateHosts.cmake reads the same file for the default build targets; lane_merge --pre-gate prints the editor stage's verdict here.

  py -3 tools/gate_hosts.py [--host <name>]     # prints the host, its capabilities and the editor stage line
"""
from __future__ import annotations

import argparse
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
HOSTS_FILE = os.path.join(ROOT, "tools", "gate_hosts.json")
ENV_VAR = "MC2_GATE_HOST"


def load(path: str = HOSTS_FILE) -> dict:
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def current_host(env=None, table: dict | None = None) -> str:
    table = table or load()
    name = (os.environ if env is None else env).get(ENV_VAR, "").strip().lower()
    return name or table["default"]


def capability(cap: str, host: str | None = None, table: dict | None = None) -> bool:
    """True when `host` (default: this one) has capability `cap`. An unknown host or capability is False (fail closed, so an
    unrecognised host never claims to build the editor)."""
    table = table or load()
    host = host or current_host(table=table)
    return bool(table.get("hosts", {}).get(host, {}).get(cap, False))


def editor_stage(host: str | None = None, table: dict | None = None) -> tuple[str, str]:
    """(verdict, detail) for the gate's editor stage: 'RUN', or 'SKIPPED' with the reason (never a silent omission)."""
    table = table or load()
    host = host or current_host(table=table)
    if capability("mfc", host, table):
        return "RUN", f"host {host}: mfc=yes"
    return "SKIPPED", f"host {host}: mfc=no (tools/gate_hosts.json); EditRel/gameos_editor are not built by default here, the PC compile is the editor proof"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host")
    args = ap.parse_args(argv)
    table = load()
    host = args.host or current_host(table=table)
    print(f"host={host} mfc={capability('mfc', host, table)}")
    verdict, detail = editor_stage(host, table)
    print(f"editor stage: {verdict} ({detail})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
