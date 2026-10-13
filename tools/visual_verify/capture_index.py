#!/usr/bin/env python3
"""What: the committed record of every capture set that was ever taken under .claude/l2cap/ (the scratch evidence directory): the set name, the backend
it asked for, the backend that drew it, where the pixels were read back from, the commit of the exe that drew it, the render options that differ from
that exe's own defaults, whether the animations were frozen, the pose count and the day it was taken, and a VOID flag with its reason.
Why: the capture PNGs are scratch (gigabytes, never committed), yet verdicts in the lane state files cite them by set name. Twenty-one of those sets said
"-vk" and were GL captures, and nothing outside the sidecars said so. The record must outlive the PNGs: it lives in `ledger.json` (one entry per set,
committed) and `index.md` is only its rendering, so deleting a set directory, or never having it on a clone, changes neither file. A VOID flag is sticky: a
later description of the same set name can add one, never clear it.

Usage:
  py -3 tools/visual_verify/capture_index.py [--root DIR] [--ledger FILE] [--out FILE] [--refresh [NAME ...]] [--note NAME TEXT] [--check]
      adds every set directory under <root> that the ledger does not know, rewrites the ledger and the index. A set already in the ledger is not
      described again unless --refresh names it (no names = every set still on disk). --check exits 1 when a set on disk is missing from the ledger or
      the index is not the rendering of the ledger; on a clone with no set directories it checks the committed pair only.
      --note NAME TEXT attaches a one-line note (what changed in the install under the set, a ruling) that the index shows after the set's line.
Nothing in a line depends on the clock, and the ledger is sorted by name, so a second run changes nothing."""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "scripts"))

import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)
from smoke_lib import capture_backend, render_options_pin  # noqa: E402

DEFAULT_ROOT = os.path.join(REPO, ".claude", "l2cap")
LEDGER_NAME = "ledger.json"
INDEX_NAME = "index.md"
LEDGER_VERSION = 1

HEADER = [
    "# Capture sets taken under .claude/l2cap (scratch PNGs, not committed; `ledger.json` is the record and this index is its rendering)",
    "",
    "One line per set: `name | requested | drew | readback | exe | options (differences from the defaults of the exe that drew the set) | animation | poses with a sidecar/poses | date`.",
    "A set listed here may no longer exist on disk: deleting a set directory never removes its line.",
    "VOID = the requested backend is not the one that drew it, or the set has no capture record: never cite the set as evidence for the requested backend. A VOID flag is sticky.",
    "unrecorded = the set predates that field. `[vs HEAD defaults]` = the exe's own option table could not be read, so the differences are against the defaults of the commit this index was written at.",
    "Add new sets with `py -3 tools/visual_verify/capture_index.py`.",
]

_defaults_cache: "dict[str, dict | None]" = {}


def _load(path: str):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def _defaults_of_exe(commit: str):
    """{option key: default} of the exe built at `commit` (read from Gates/render_options.h at that commit), or None when that header cannot be read."""
    commit = commit.split("+", 1)[0]
    if commit in _defaults_cache:
        return _defaults_cache[commit]
    table = None
    if commit and commit != "unrecorded":
        try:
            src = subprocess.run(["git", "-C", REPO, "show", f"{commit}:Gates/render_options.h"], capture_output=True, text=True, check=True,
                                 encoding="utf-8", errors="replace", **console_child.child_kwargs(redirected=True)).stdout
            table = {o.key: o.default for o in render_options_pin.table_from_text(src)}
        except (subprocess.CalledProcessError, OSError, ValueError, RuntimeError):
            table = None
    _defaults_cache[commit] = table
    return table


def option_differences(values, commit: str = "") -> "tuple[str, str]":
    """(differences, base) of a record's options. `differences` is `Key=value,...`, 'defaults' when none, 'unrecorded' when there is no record;
    `base` is 'exe' when the differences are against the defaults of the exe at `commit`, else 'head' (the tree this runs in)."""
    if not isinstance(values, dict):
        return "unrecorded", "exe"
    defaults = _defaults_of_exe(commit)
    base = "exe"
    if defaults is None:
        defaults = {o.key: o.default for o in render_options_pin.table()}
        base = "head"
    diff = [f"{k}={json.dumps(v)}" for k, v in sorted(values.items()) if k in defaults and v != defaults[k]]
    return (",".join(diff) if diff else "defaults"), base


def _sidecars(set_dir: str, run: dict):
    """(pose name, sidecar or None) for every pose of the set."""
    out = []
    for pose in run.get("poses") or []:
        bm = pose.get("bookmark")
        if bm:
            out.append((bm, _load(os.path.join(set_dir, bm, bm + ".json"))))
    return out


def describe_row(set_dir: str) -> dict:
    """The ledger entry of one set directory, read from its capture_run.json and pose sidecars."""
    run = _load(os.path.join(set_dir, "capture_run.json"))
    if not isinstance(run, dict):
        return {"requested": "unrecorded", "drew": [], "readback": [], "exe": "unrecorded", "options": "unrecorded", "options_base": "exe",
                "animation": "unrecorded", "poses_present": 0, "poses_total": 0, "date": "-", "void": True, "void_reason": "no capture_run.json"}
    sidecars = _sidecars(set_dir, run)
    declared = run.get("declared_env") or {}
    requested = (run.get("backend") or {}).get("requested") or capture_backend.requested(declared)
    drew = sorted({d for _n, sc in sidecars if (d := capture_backend.drew(sc))})
    sources = sorted({str(sc.get("readback_source")) for _n, sc in sidecars if sc and sc.get("readback_source")})
    present = [(n, sc) for n, sc in sidecars if sc]
    problems = capture_backend.check(requested, present) if present else ["no pose has a sidecar"]
    provenance = (run.get("identity") or {}).get("binary_provenance") or {}
    commit = provenance.get("commit") or "unrecorded"
    exe = commit + ("+dirty" if provenance.get("dirty") else "")
    options, base = option_differences(run.get("render_options"), commit)
    return {"requested": requested, "drew": drew, "readback": sources, "exe": exe, "options": options, "options_base": base,
            "animation": "frozen" if str(declared.get("MC2_SMOKE_FREEZE_ANIM", "")) == "1" else "unfrozen",
            "poses_present": len(present), "poses_total": len(sidecars), "date": str(run.get("generated_at") or "")[:10] or "-",
            "void": bool(problems), "void_reason": problems[0] if problems else ""}


def render_line(name: str, row: dict) -> str:
    """One index line."""
    if row.get("requested") == "unrecorded":
        options, poses = "unrecorded", "-"
        return (f"{name} **VOID** | unrecorded | unrecorded | unrecorded | unrecorded | {options} | unrecorded | {poses} | - | {row.get('void_reason', '')}")
    options = row["options"] + (" [vs HEAD defaults]" if row.get("options_base") == "head" and row["options"] not in ("defaults", "unrecorded") else "")
    flag = " **VOID**" if row.get("void") else ""
    reason = f" | {row['void_reason']}" if row.get("void") and row.get("void_reason") else ""
    reason += f" | note: {row['note']}" if row.get("note") else ""
    return (f"{name}{flag} | {row['requested']} | {'+'.join(row['drew']) or 'unrecorded'} | {'+'.join(row['readback']) or 'unrecorded'} | {row['exe']} | "
            f"{options} | {row['animation']} | {row['poses_present']}/{row['poses_total']} | {row['date']}{reason}")


def describe(set_dir: str) -> str:
    """The index line of one set directory."""
    return render_line(os.path.basename(set_dir), describe_row(set_dir))


def read_ledger(path: str) -> "dict[str, dict]":
    data = _load(path)
    return dict(data.get("sets") or {}) if isinstance(data, dict) else {}


def write_ledger(path: str, sets: "dict[str, dict]") -> None:
    """One entry per line so a diff of the ledger shows exactly the sets that changed."""
    entries = [f'  {json.dumps(name)}: {json.dumps(sets[name], sort_keys=True)}' for name in sorted(sets)]
    text = '{\n "version": %d,\n "sets": {\n%s\n }\n}\n' % (LEDGER_VERSION, ",\n".join(entries))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def disk_sets(root: str) -> "list[str]":
    if not os.path.isdir(root):
        return []
    return sorted(d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d)) and d != "montages")


def merge(ledger: "dict[str, dict]", root: str, refresh: "list[str] | None") -> "dict[str, dict]":
    """The ledger plus every set on disk it does not know; `refresh` (names, or [] for all on disk) describes those again with the VOID flag kept."""
    out = dict(ledger)
    for name in disk_sets(root):
        if name in out and refresh is None:
            continue
        if name in out and refresh and name not in refresh:
            continue
        row = describe_row(os.path.join(root, name))
        old = out.get(name)
        if old and old.get("void") and not row["void"]:
            row["void"], row["void_reason"] = True, old.get("void_reason") or "flagged VOID earlier"
        if old and row["poses_present"] < old.get("poses_present", 0):
            # Pose files were deleted from a set whose directory remains: what the poses drew and read back was recorded while they existed, and
            # describing the set again must not turn that into "unrecorded".
            row["drew"], row["readback"] = old.get("drew", row["drew"]), old.get("readback", row["readback"])
        if old and old.get("note"):
            row["note"] = old["note"]
        out[name] = row
    return out


def render_index(sets: "dict[str, dict]") -> str:
    void = sum(1 for r in sets.values() if r.get("void"))
    counts = f"{len(sets)} set(s), {void} VOID (counted from the ledger when this file was written)."
    return "\n".join(HEADER + [counts, ""] + [render_line(n, sets[n]) for n in sorted(sets)]) + "\n"


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--ledger", default=None, help=f"default <root>/{LEDGER_NAME}")
    ap.add_argument("--out", default=None, help=f"default <root>/{INDEX_NAME}")
    ap.add_argument("--refresh", nargs="*", default=None, metavar="NAME", help="describe these sets again (none named = all on disk); VOID stays")
    ap.add_argument("--note", nargs=2, action="append", default=[], metavar=("NAME", "TEXT"), help="attach a one-line note to a set already in the ledger")
    ap.add_argument("--check", action="store_true", help="exit 1 when a set on disk is not in the ledger or the index is not the ledger's rendering")
    args = ap.parse_args(argv)
    ledger_path = args.ledger or os.path.join(args.root, LEDGER_NAME)
    out = args.out or os.path.join(args.root, INDEX_NAME)
    ledger = read_ledger(ledger_path)
    if args.check:
        missing = [n for n in disk_sets(args.root) if n not in ledger]
        try:
            with open(out, encoding="utf-8", newline="") as f:
                same = f.read() == render_index(ledger)
        except OSError:
            same = False
        ok = same and not missing and bool(ledger)
        why = ([f"{len(missing)} set(s) on disk not in the ledger: {', '.join(missing[:5])}"] if missing else []) + ([] if same else [f"{out} is not the rendering of the ledger"])
        print("capture index: " + ("OK" if ok else "STALE (" + "; ".join(why or ["empty ledger"]) + "); run py -3 tools/visual_verify/capture_index.py"))
        return 0 if ok else 1
    merged = merge(ledger, args.root, args.refresh)
    for name, text in args.note:
        if name not in merged:
            print(f"capture index: no set {name!r} in the ledger", file=sys.stderr)
            return 1
        merged[name]["note"] = " ".join(text.split()).replace("|", "/")
    write_ledger(ledger_path, merged)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(render_index(merged))
    added = len(merged) - len(ledger)
    print(f"capture index: {len(merged)} set(s) in the ledger ({added} added), {sum(1 for r in merged.values() if r.get('void'))} VOID -> {ledger_path}, {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
