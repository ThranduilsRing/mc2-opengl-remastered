#!/usr/bin/env python3
"""Generate one-page lane-rules digests (tools-only, engine, merge) from lane-rules.md.

Tokens are cut by SELECTION, never by paraphrase: every kept line (bullet, table row, prose,
heading) is copied verbatim from lane-rules.md. A lane type drops whole lines (DROP below);
each section that lost lines gets one generated pointer line
"see <lane-rules.md> section N (<heading>)". Section headings are kept so "section N"
references in the rules resolve. A line not named in DROP is kept (new rules are in by default).

Each digest has a byte budget (BUDGET); an over-budget digest is still written but fails the
run / --check with a clear message. The footer carries the absolute rules path (lane slots
cannot resolve a relative one).

    py -3 tools/rules_digest.py                           # writes A:/Games/mc2-ops/digest-<type>.md
    py -3 tools/rules_digest.py --out-dir <dir> --check   # exit 1 if a file is stale or over budget
"""
import argparse
import re
import sys
from pathlib import Path

DEFAULT_RULES = Path("A:/Games/mc2-ops/lane-rules.md")  # drive-path: ok PC ops tool (orchestrator/lane prefix); never runs on the laptop gate
DEFAULT_OUT = Path("A:/Games/mc2-ops")  # drive-path: ok PC ops tool output dir
RULES_ABS = "A:/Games/mc2-ops/lane-rules.md"  # drive-path: ok PC ops rules path printed in the digest footer
FOOTER = f"full rules: {RULES_ABS}; read it only if a case is not covered"
TYPES = ("tools-only", "engine", "merge")
T, E, M = TYPES

# Byte budget per digest (UTF-8 bytes of the whole file).
BUDGET = {T: 8750, E: 8300, M: 8500}

HEAD = {
    T: "Generated header (not a rule): tools-only lane (Python/scripts/docs/tests; no game runs, no deploy).",
    E: "Generated header (not a rule): engine lane (C++/shader/render/sim; PC does one final compile).",
    M: "Generated header (not a rule): merge/review runner (merge windows, gates, adversarial reviews; also read lane-rules-merge.md).",
}

# Lines a lane type does not carry, as (section_key, line-body prefix). The section key is the
# section number as a string or "owner"; the body is the line without a leading "- ".
# Each section that loses lines gets a pointer to it.
_PC = ("6", "PC (8 cores")
_AB = ("6", "A/B:")
_LAPTOP = ("6", "Laptop (GTX 1050 Ti)")
_STARTUP = ("6", "At the first startup timeout")
_C7 = [("7", p) for p in ("| C++, not sim", "| Sim-visible", "| Shader", "| Pixels change", "| Windows-only", "| Shared header")]
_ROWS7 = _C7 + [("7", p) for p in ("| Docs |", "| Tooling", "| Data / art |", "| Change |", "|---|")]
_SEC5 = [("5", p) for p in ("Worktree:", "Install (", "One install per lane", "Drives (")]
_SEC6 = [_PC, _AB, _LAPTOP, _STARTUP]
_REPORT = ("9", "At most 12 summary lines")
# Detailed procedure stays available through the generated section pointers.
_DETAILS = [("1", "Briefs quote"), ("2", "Never poll or sleep-loop"), ("4", "Long runs:"), ("8", "Delete only enumerated")]
DROP = {
    T: [*_DETAILS, ("1", "**Look lanes"), _PC, _AB, _STARTUP, *_C7, ("7", "PC compile:"), ("7", "Perf briefs"), ("7", "Game-run defaults"), ("7", "Mods:"),
        *_SEC5, ("2", "Do not read whole files"), ("2", "Use the one-verdict"), ("4", "Priority:"), _REPORT],
    E: [*_DETAILS, *_SEC5, *_SEC6, ("2", "A continuation names"), ("2", "Do not read whole files"), ("2", "Use the one-verdict"),
        ("2", "Write scripts with"), ("4", "Priority:"), ("4", "Never `find /`"), ("7", "**Unit tests"), ("7", "| Windows-only"), ("7", "| Shared header"),
        ("7", "Merge-window/scheduled only"), ("7", "Mods:"), ("7", "Perf briefs"), ("7", "Game-run defaults"), _REPORT],
    M: [*_DETAILS, ("1", "**Look lanes"), _PC, _AB, _STARTUP, *_ROWS7, ("7", "**Unit tests"), ("7", "PC compile:"), ("7", "Game-run defaults"),
        *_SEC5, ("2", "Do not read whole files"), ("2", "Use the one-verdict"), ("2", "Write scripts with"), ("4", "Priority:"), _REPORT],
}


def _body(line):
    return line[2:] if line.startswith("- ") else line


def parse(text):
    """Return (preamble, items): preamble = prose lines between the title and the first section
    heading; items = [(section_key, heading_line, line)] for every non-blank line of a section
    (bullets, table rows and prose), in file order."""
    pre, items, sec, head, seen_title = [], [], None, None, False
    for raw in text.splitlines():
        line = raw.rstrip()
        if not line.strip():
            continue
        if line.startswith("## "):
            m = re.match(r"^## (\d+)\.", line)
            sec = m.group(1) if m else ("owner" if line.startswith("## Owner rules") else line[3:].split()[0].lower())
            head = line
            continue
        if line.startswith("# ") and not seen_title:
            seen_title = True
            continue
        if sec is None:
            pre.append(line)
        else:
            items.append((sec, head, line))
    return pre, items


def dropped(sec, line, typ):
    b = _body(line)
    return any(sec == s and b.startswith(p) for s, p in DROP[typ])


def pointer(sec, head):
    title = re.sub(r"\s*\(.*$", "", re.sub(r"^## (\d+\.\s*)?", "", head)).strip()
    where = f"section {sec}" if sec.isdigit() else "its Owner rules section"
    return f"see {RULES_ABS} {where} ({title})"


def render(pre, items, typ):
    out = ["# MC2 lane rules digest: " + typ, HEAD[typ]]
    out += pre
    order, by_sec, heads = [], {}, {}
    for sec, head, line in items:
        if sec not in by_sec:
            order.append(sec)
            by_sec[sec], heads[sec] = [], head
        by_sec[sec].append((line, dropped(sec, line, typ)))
    for sec in order:
        kept = [l for l, d in by_sec[sec] if not d]
        out.append("")
        if kept:
            out.append(heads[sec])
        prev_table = False
        for l in kept:
            tab = l.startswith("|")
            if tab != prev_table:
                out.append("")
            out.append(l)
            prev_table = tab
        if any(d for _l, d in by_sec[sec]):
            if prev_table:
                out.append("")
            out.append(pointer(sec, heads[sec]))
    out += ["", FOOTER]
    text = "\n".join(out) + "\n"
    while "\n\n\n" in text:
        text = text.replace("\n\n\n", "\n\n")
    return text


def generate(rules_path=DEFAULT_RULES):
    pre, items = parse(Path(rules_path).read_text(encoding="utf-8"))
    return {t: render(pre, items, t) for t in TYPES}


def over_budget(digests):
    return [f"OVER BUDGET: digest-{t}.md is {len(b.encode('utf-8'))} bytes (budget {BUDGET[t]}); add lines to DROP[{t!r}] in tools/rules_digest.py"
            for t, b in digests.items() if len(b.encode("utf-8")) > BUDGET[t]]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rules", default=str(DEFAULT_RULES))
    ap.add_argument("--out-dir", default=str(DEFAULT_OUT))
    ap.add_argument("--check", action="store_true", help="exit 1 if a digest on disk differs from the generated one or is over budget")
    args = ap.parse_args(argv)
    digests = generate(args.rules)
    bad = 0
    for typ, body in digests.items():
        path = Path(args.out_dir) / f"digest-{typ}.md"
        if args.check:
            if not path.exists() or path.read_text(encoding="utf-8") != body:
                print(f"STALE {path}")
                bad += 1
            continue
        path.write_text(body, encoding="utf-8", newline="\n")
        print(f"{path}: {len(body.encode('utf-8'))} bytes (budget {BUDGET[typ]})")
    for msg in over_budget(digests):
        print(msg)
        bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
