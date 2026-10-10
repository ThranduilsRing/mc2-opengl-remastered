#!/usr/bin/env python3
"""gate_gen -- generate and check everything derived from the typed gate table.

The single source is the C++ table, Gates/gate_table.cpp (one row per MC2_* variable). The compiler is its
parser: tools/gate_dump compiles the rows and prints them as Gates/gate_table.json, which is committed. From
that JSON this tool derives, and checks:

  Gates/gate_index.gen.h                  sorted {name, kind, rule, tokens}: what gate::get<"MC2_X">() resolves at
                                          compile time (a row missing from it fails the normal build)
  RenderCore/RendererFeatureRegistry.h    the kFeatureTable / kAuxEnvVars rows (the RendererFeature enum stays
                                          hand-ordered)
  docs/flags.auto.yaml                    one entry per gate (v3)

MODES
-----
  --sync-verified   after a migration: mark each row `verified` iff a gate::get<> reader exists, rebuild
                    tools/gate_dump, rewrite Gates/gate_table.json, then everything --write derives
  --write           regenerate the derived files from Gates/gate_table.json (after editing a row: run
                    --sync-verified, which also refreshes the JSON from the compiler)
  --check           hermetic checks: JSON sorted/unique, derived files current, every seed gate still a row,
                    registered gates agree with the table, `verified` equals "a typed reader exists", the
                    per-site override count did not rise; --rebuild adds the compiler check: rebuild gate_dump
                    and diff its output against the committed JSON

The one-shot importer that wrote the first table from the tree's getenv idioms is gone: the table is the
truth now, and tools/gate_table.seed.json is a frozen record of what the importer found.
"""

import argparse
import os
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--write", action="store_true", help="regenerate the files derived from Gates/gate_table.json")
    ap.add_argument("--check", action="store_true", help="gate table checks (stage A; --rebuild adds stage B)")
    ap.add_argument("--sync-verified", action="store_true",
                    help="mark rows verified iff a gate::get<> reader exists, rebuild gate_dump, rewrite the JSON "
                         "and the derived files")
    ap.add_argument("--rebuild", action="store_true", help="with --check: rebuild tools/gate_dump and diff its JSON")
    args = ap.parse_args(argv)
    if not (args.write or args.check or args.sync_verified):
        ap.print_help()
        return 2
    import gate_table_gen as tg  # noqa: E402
    if args.sync_verified:
        return tg.sync_verified(args.root)
    if args.write:
        return tg.write_index(args.root)
    fails = tg.check(args.root, args.rebuild)
    if fails:
        print("GATE-TABLE check FAILED")
        for f in fails:
            print("  - " + f)
        return 1
    print("gate_gen: gate table is consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
