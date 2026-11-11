#!/usr/bin/env python3
"""Generate one-page lane-rules digests (tools-only, engine, merge) from lane-rules.md.

Each digest is a deterministic selection of lane-rules.md lines (bullets and
proof-table rows are copied verbatim, so no rule is reworded or dropped). A
digest is at most MAX_LINES lines and ends with the FOOTER line. Every
must/never line of lane-rules.md lands in at least one digest (checked by
scripts/tests/test_rules_digest.py).

    py -3 tools/rules_digest.py                      # writes A:/Games/mc2-ops/digest-<type>.md
    py -3 tools/rules_digest.py --out-dir <dir> --check   # exit 1 if the files on disk are stale
"""
import argparse
import re
import sys
from pathlib import Path

DEFAULT_RULES = Path("A:/Games/mc2-ops/lane-rules.md")
DEFAULT_OUT = Path("A:/Games/mc2-ops")
MAX_LINES = 60
FOOTER = "full rules: lane-rules.md; read it only if a case is not covered"
TYPES = ("tools-only", "engine", "merge")
T, E, M = TYPES

# Whole sections (key: "owner" or the section number) -> the types that carry every line of it.
SECTIONS = {
    "owner": (T, E, M),
    "1": (T, E, M),
    "2": (T, E, M),
    "3": (T, E, M),
    "4": (T, E, M),
    "5": (T, E, M),
    "6": (E, M),
    "8": (T, E, M),
    "9": (T, E, M),
}
# Section 7 lines are picked by their leading text (bullet or table row).
SECTION7 = [
    ("**Unit tests", (T, E)),
    ("Run the rung", (T, E, M)),
    ("| Change |", (T, E)),
    ("|---|", (T, E)),
    ("| Docs |", (T,)),
    ("| Tooling", (T,)),
    ("| Data / art |", (T, E)),
    ("| C++, not sim", (E,)),
    ("| Sim-visible", (E,)),
    ("| Shader", (E,)),
    ("| Pixels change", (E,)),
    ("| Windows-only", (E,)),
    ("| Shared header", (E,)),
    ("PC compile:", (E,)),
    ("Lint once", (T, E, M)),
    ("Never cut:", (T, E, M)),
    ("Merge-window/scheduled only", (T, M)),
    ("Perf:", (E, M)),
    ("Game-run defaults", (E,)),
    ("Mods:", (M,)),
    ("A base break", (T, E, M)),
]
# Section 6 lines a tools-only lane still needs (host bans, not host instructions).
SECTION6_TOOLS = ("Laptop (GTX 1050 Ti)",)

HEAD = {
    T: "Tools-only lane (Python/scripts/docs/tests; no game runs, no deploy).",
    E: "Engine lane (C++/shader/render/sim; box iterates, PC does one final compile).",
    M: "Merge/review runner (merge windows, gates, adversarial reviews; also read lane-rules-merge.md).",
}


def parse(text):
    """Return [(section_key, line)] for every bullet / table line, in file order."""
    out, sec = [], None
    for raw in text.splitlines():
        line = raw.rstrip()
        m = re.match(r"^## (\d+)\.", line)
        if m:
            sec = m.group(1)
            continue
        if line.startswith("## Owner rules"):
            sec = "owner"
            continue
        if sec and (line.startswith("- ") or line.startswith("|")):
            out.append((sec, line))
    return out


def select(items, typ):
    picked = []
    for sec, line in items:
        if sec in SECTIONS:
            types = SECTIONS[sec]
            if sec == "6" and typ == T and line[2:].startswith(SECTION6_TOOLS):
                picked.append(line)
            elif typ in types:
                picked.append(line)
        elif sec == "7":
            body = line[2:] if line.startswith("- ") else line
            for prefix, types in SECTION7:
                if body.startswith(prefix):
                    if typ in types:
                        picked.append(line)
                    break
    return picked


def render(items, typ):
    lines = ["# MC2 lane rules digest: " + typ, HEAD[typ], ""]
    lines += select(items, typ)
    lines += ["", FOOTER]
    if len(lines) > MAX_LINES:
        raise SystemExit(f"digest {typ} is {len(lines)} lines (max {MAX_LINES}); trim SECTIONS/SECTION7")
    return "\n".join(lines) + "\n"


def generate(rules_path=DEFAULT_RULES):
    items = parse(Path(rules_path).read_text(encoding="utf-8"))
    return {t: render(items, t) for t in TYPES}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rules", default=str(DEFAULT_RULES))
    ap.add_argument("--out-dir", default=str(DEFAULT_OUT))
    ap.add_argument("--check", action="store_true", help="exit 1 if a digest on disk differs from the generated one")
    args = ap.parse_args(argv)
    digests = generate(args.rules)
    stale = 0
    for typ, body in digests.items():
        path = Path(args.out_dir) / f"digest-{typ}.md"
        if args.check:
            if not path.exists() or path.read_text(encoding="utf-8") != body:
                print(f"STALE {path}")
                stale += 1
            continue
        path.write_text(body, encoding="utf-8", newline="\n")
        print(f"{path}: {body.count(chr(10))} lines")
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
