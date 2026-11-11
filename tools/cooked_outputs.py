#!/usr/bin/env python3
"""The one list of gitignored, PC-cooked outputs the merge gate depends on (tools/cooked_outputs.json).

Readers: tools/visual_verify/ab.py (compare-captures window payload), tools/sim_determinism.py (data fingerprint pin file),
tools/laptop_gate.py (what the PC uploads to the laptop), tools/lane_merge.py --pre-gate (the `cooked_outputs` row) and
scripts/check-stock-mission-data.py (manifest shape). A cooked file that is absent or not the pinned bytes is a clear pre-gate red
here, not a laptop compare-captures diff (W138h: tree sidecars x4, W147: grass turf).

  py -3 tools/cooked_outputs.py check [--root <tree>]     # exit 1 and one line per problem
  py -3 tools/cooked_outputs.py list                      # the outputs, one per line
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
MANIFEST = os.path.join(ROOT, "tools", "cooked_outputs.json")
PROVISION_KINDS = ("worktree", "published", "none")


def load(path: str = MANIFEST) -> dict:
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def pinned_entry(entry_id: str, manifest: dict | None = None) -> dict:
    for e in (manifest or load())["pinned"]:
        if e["id"] == entry_id:
            return e
    raise KeyError(entry_id)


def expand_pins(entry: dict, pins: dict) -> dict:
    """{output relpath: pinned sha256} for one pinned entry, from its parsed pin file. Raises on a malformed row."""
    return {entry["out_template"].format(key=key): row[entry["sha_field"]] for key, row in pins.items()}


def unpinned_by_cook(manifest: dict | None = None) -> dict:
    """{cook script relpath: (output relpath, ...)}"""
    out: dict = {}
    for row in (manifest or load())["unpinned"]:
        out.setdefault(row["cook"], []).append(row["out"])
    return {cook: tuple(outs) for cook, outs in out.items()}


def provision_paths(kind: str, root: str = ROOT, manifest: dict | None = None) -> tuple:
    """Output relpaths a host provisions from `kind` ('worktree' or 'published'); pinned outputs are read from the pin file under `root`.
    A missing pin file yields none for it (the caller's preflight names the missing file)."""
    manifest = manifest or load()
    paths = [r["out"] for r in manifest["unpinned"] if r["provision"] == kind]
    for entry in manifest["pinned"]:
        if entry["provision"] != kind:
            continue
        try:
            with open(os.path.join(root, entry["pin_file"]), encoding="utf-8") as fh:
                paths += sorted(expand_pins(entry, json.load(fh)))
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            continue
    return tuple(paths)


def _sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def manifest_problems(manifest: dict | None = None, root: str = ROOT) -> list[str]:
    """Static shape of the manifest against the tree: every cook script and pin file exists, provision kinds are known. No cooked file is read."""
    try:
        manifest = manifest or load()
        rows = [(e["id"], e["pin_file"], e["cook"], e["provision"]) for e in manifest["pinned"]]
        rows += [(r["out"], None, r["cook"], r["provision"]) for r in manifest["unpinned"]]
    except (OSError, ValueError, KeyError, TypeError) as exc:
        return [f"tools/cooked_outputs.json unreadable or malformed ({type(exc).__name__}: {exc})"]
    bad = []
    for name, pin, cook, provision in rows:
        if not os.path.isfile(os.path.join(root, cook)):
            bad.append(f"cooked output {name}: cook script {cook} does not exist")
        if pin and not os.path.isfile(os.path.join(root, pin)):
            bad.append(f"cooked output {name}: pin file {pin} does not exist")
        if provision not in PROVISION_KINDS:
            bad.append(f"cooked output {name}: provision {provision!r} is not one of {', '.join(PROVISION_KINDS)}")
    return bad


def problems(root: str = ROOT, manifest: dict | None = None, hasher=_sha256) -> list[str]:
    """One line per problem under `root` (a PC worktree after a deploy or cook): a required unpinned output that is absent, a pinned output
    that is absent or not the pinned bytes. Missing and stale pinned outputs are summarised per pin file (the first three named)."""
    manifest = manifest or load()
    bad = manifest_problems(manifest, root)
    for entry in manifest["pinned"]:
        try:
            with open(os.path.join(root, entry["pin_file"]), encoding="utf-8") as fh:
                want = expand_pins(entry, json.load(fh))
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            continue   # manifest_problems already named the missing or unreadable pin file
        absent = [rel for rel in sorted(want) if not os.path.isfile(os.path.join(root, rel))]
        stale = [rel for rel in sorted(want) if rel not in absent and hasher(os.path.join(root, rel)) != want[rel]]
        for label, rels in (("absent", absent), ("not the pinned bytes", stale)):
            if rels:
                more = f" (+{len(rels) - 3} more)" if len(rels) > 3 else ""
                bad.append(f"{len(rels)} of {len(want)} {entry['id']} outputs {label}: {', '.join(rels[:3])}{more}; "
                           f"cook with {entry['cook']} (or deploy_payload.py) before the gate")
    for row in manifest["unpinned"]:
        if row.get("required") and not os.path.isfile(os.path.join(root, row["out"])):
            bad.append(f"required cooked output {row['out']} is absent; cook with {row['cook']} (or deploy_payload.py) before the gate")
    return bad


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=("check", "list"))
    ap.add_argument("--root", default=ROOT)
    args = ap.parse_args(argv)
    if args.cmd == "list":
        manifest = load()
        for e in manifest["pinned"]:
            print(f"pinned   {e['id']}: {e['out_template']} from {e['pin_file']} (cook {e['cook']})")
        for r in manifest["unpinned"]:
            print(f"unpinned {r['out']} (cook {r['cook']}, required={bool(r.get('required'))}, provision={r['provision']})")
        return 0
    bad = problems(args.root)
    for line in bad:
        print(line)
    print("cooked_outputs: " + (f"FAIL ({len(bad)})" if bad else "OK"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
