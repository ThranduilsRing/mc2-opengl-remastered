#!/usr/bin/env python3
"""
mc2asset.py — BLENDER-PIPELINE-UNIFY-1 thin dispatcher over the
blender_pipeline CLIs, all sharing srcdata.py resolution.

    py -3 tools/blender_pipeline/mc2asset.py edit status quonset
    py -3 tools/blender_pipeline/mc2asset.py list quon

No logic here beyond argv forwarding — each subcommand's own main() does the
real work.
"""
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asset_edit  # noqa: E402
import srcdata  # noqa: E402

USAGE = """usage: mc2asset.py SUBCOMMAND [ARGS...]

subcommands:
  edit ARGS...     forward to asset_edit.py (open/status/export edit loop)
  list SUBSTRING   list stock asset names matching SUBSTRING (default: all)
"""

DISPATCH = {
    "edit": asset_edit.main,
}


def cmd_list(argv):
    substring = argv[0] if argv else ""
    try:
        tgl_dir = srcdata.find_tgl_dir(None)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    for stem in srcdata.list_assets(tgl_dir, substring):
        print(stem)
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        print(USAGE, file=sys.stderr)
        return 2
    sub, rest = argv[0], argv[1:]
    if sub == "list":
        return cmd_list(rest)
    fn = DISPATCH.get(sub)
    if fn is None:
        print(f"ERROR: unknown subcommand {sub!r}\n\n{USAGE}", file=sys.stderr)
        return 2
    return fn(rest)


if __name__ == "__main__":
    sys.exit(main())
