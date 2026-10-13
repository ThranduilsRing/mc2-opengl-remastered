#!/usr/bin/env python3
"""What: one committed line per capture set under .claude/l2cap/ (the scratch evidence directory): the set name, the backend it asked for, the backend that
drew it, where the pixels were read back from, the commit of the exe that drew it, the render options that differ from the defaults, whether the
animations were frozen, the pose count and the day it was taken.
Why: the capture PNGs are scratch (gigabytes, never committed), yet verdicts in the lane state files cite them by set name. Twenty-one of those sets said
"-vk" and were GL captures, and nothing outside the sidecars said so. The index is the committed record a later lane reads before it cites a set:
a VOID flag when the requested backend is not what drew it, "unfrozen" when the animation phase is part of the noise, "unrecorded" when the set
predates the record.

Usage:
  py -3 tools/visual_verify/capture_index.py [--root DIR] [--out FILE] [--check]
      writes <root>/index.md (default .claude/l2cap/index.md); --check exits 1 when the file on disk is not what a regeneration would write.
The set directories are sorted by name and nothing in a line depends on the clock, so a second run changes nothing."""
from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "scripts"))

from smoke_lib import capture_backend, render_options_pin  # noqa: E402

DEFAULT_ROOT = os.path.join(REPO, ".claude", "l2cap")
HEADER = [
    "# Capture sets in .claude/l2cap (scratch PNGs, not committed; this index is the record)",
    "",
    "One line per set: `name | requested | drew | readback | exe | options (differences from the defaults) | animation | poses with a sidecar/poses | date`.",
    "VOID = the requested backend is not the one that drew it (or the sidecars say nothing): never cite the set as evidence for the requested backend.",
    "unrecorded = the set predates that field. Regenerate with `py -3 tools/visual_verify/capture_index.py`.",
    "",
]


def _load(path: str):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def option_differences(values) -> str:
    """The options of a record that differ from the engine defaults as `Key=value,...`, 'defaults' when none, 'unrecorded' when there is no record."""
    if not isinstance(values, dict):
        return "unrecorded"
    defaults = {o.key: o.default for o in render_options_pin.table()}
    diff = [f"{k}={json.dumps(v)}" for k, v in sorted(values.items()) if k in defaults and v != defaults[k]]
    return ",".join(diff) if diff else "defaults"


def _sidecars(set_dir: str, run: dict):
    """(pose name, sidecar or None) for every pose of the set."""
    out = []
    for pose in run.get("poses") or []:
        bm = pose.get("bookmark")
        if bm:
            out.append((bm, _load(os.path.join(set_dir, bm, bm + ".json"))))
    return out


def describe(set_dir: str) -> str:
    """The index line of one set directory."""
    name = os.path.basename(set_dir)
    run = _load(os.path.join(set_dir, "capture_run.json"))
    if not isinstance(run, dict):
        return f"{name} | unrecorded | unrecorded | unrecorded | unrecorded | unrecorded | unrecorded | - | -"
    sidecars = _sidecars(set_dir, run)
    declared = run.get("declared_env") or {}
    requested = (run.get("backend") or {}).get("requested") or capture_backend.requested(declared)
    drew = sorted({d for _n, sc in sidecars if (d := capture_backend.drew(sc))})
    sources = sorted({str(sc.get("readback_source")) for _n, sc in sidecars if sc and sc.get("readback_source")})
    present = [(n, sc) for n, sc in sidecars if sc]
    void = not present or bool(capture_backend.check(requested, present))   # a pose whose sidecar is gone is shown as have/total, not as VOID
    provenance = (run.get("identity") or {}).get("binary_provenance") or {}
    commit = provenance.get("commit") or "unrecorded"
    if provenance.get("dirty"):
        commit += "+dirty"
    frozen = "frozen" if str(declared.get("MC2_SMOKE_FREEZE_ANIM", "")) == "1" else "unfrozen"
    options = option_differences(run.get("render_options"))
    when = str(run.get("generated_at") or "")[:10] or "-"
    flag = " **VOID**" if void else ""
    return (f"{name}{flag} | {requested} | {'+'.join(drew) or 'unrecorded'} | {'+'.join(sources) or 'unrecorded'} | {commit} | {options} | {frozen} | "
            f"{len(present)}/{len(sidecars)} | {when}")


def build(root: str) -> str:
    sets = sorted(d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d)) and d != "montages")
    return "\n".join(HEADER + [describe(os.path.join(root, d)) for d in sets]) + "\n"


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--out", default=None, help="default <root>/index.md")
    ap.add_argument("--check", action="store_true", help="exit 1 when the index on disk is not what a regeneration would write")
    args = ap.parse_args(argv)
    out = args.out or os.path.join(args.root, "index.md")
    text = build(args.root)
    if args.check:
        try:
            with open(out, encoding="utf-8", newline="") as f:
                same = f.read() == text
        except OSError:
            same = False
        print("capture index: " + ("OK" if same else f"STALE ({out}); regenerate with py -3 tools/visual_verify/capture_index.py"))
        return 0 if same else 1
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"capture index: {len(text.splitlines()) - len(HEADER)} set(s) -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
