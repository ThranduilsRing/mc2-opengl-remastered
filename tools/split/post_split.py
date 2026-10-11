#!/usr/bin/env python3
"""What: the bookkeeping that follows an installed file split: the status row in giant-file-split-plan.md (built from the real line counts), the split map, the file-size
ratchet and the contract board, ending in a digest of the failing rows.
Why: every split repeats the same four steps and the same number-copying; the row's counts must match the files, and a red board row has to be read, not scrolled for.
Usage: py -3 tools/split/post_split.py --src code/x.cpp --stem x --note "what moves / what stays" [--tier 1000-1500] [--no-board] [--rows REGEX] [--known doc_links,...]
  Run it after the pieces are installed and built, before committing; it edits the plan doc, docs/split-map.md and (via --tighten) the size baseline.
  Prints the row it wrote and exits 1 when the board has a failing row outside --known (default: doc_links)."""
import argparse, glob, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "scripts"))
import console_child  # noqa: E402

PLAN = ".claude/cleanup/giant-file-split-plan.md"


def count_lines(data):
    return 0 if not data else data.count(b"\n") + (0 if data.endswith(b"\n") else 1)


def _run(cmd, **kw):
    return subprocess.run(cmd, cwd=REPO, capture_output=True, **console_child.child_kwargs(redirected=True), **kw)


def before_lines(src):
    r = _run(["git", "show", "HEAD:" + src])
    return count_lines(r.stdout) if r.returncode == 0 else 0


def pieces_of(src, stem):
    """(core_lines, [(suffix, lines)], header_exists) from the worktree."""
    d = os.path.dirname(src)
    core = count_lines(open(os.path.join(REPO, src), "rb").read())
    out = []
    for p in sorted(glob.glob(os.path.join(REPO, d, stem + "_*.cpp"))):
        suffix = os.path.basename(p)[len(stem):-len(".cpp")]
        out.append((suffix, count_lines(open(p, "rb").read())))
    return core, out, os.path.exists(os.path.join(REPO, d, stem + "_internal.h"))


def fmt(n):
    return "{:,}".format(n)


def plan_row(src, stem, before, core, pieces, note, tier):
    names = ", ".join(s.lstrip("_") for s, _ in pieces)
    sizes = ", ".join("`%s%s.cpp` (%d)" % (stem, s, n) for s, n in pieces)
    first = "done (%s tier, verbatim); %s" % (tier, note) if note else "done (%s tier, verbatim)" % tier
    return "| `%s` | %s | %s | %s + `%s_internal.h` | %s; pieces: %s |" % (src, fmt(before), fmt(core), names, stem, first, sizes)


def insert_row(plan_text, row, tier):
    """After the last row this tool (or the lane) wrote for the tier; falls back to the last table row that mentions a split.
    A row already written for the same file is replaced, so rerunning after a fix does not duplicate it."""
    nl = "\r\n" if "\r\n" in plan_text else "\n"
    lines = plan_text.split(nl)
    first_cell = row.split("|")[1]
    lines = [l for l in lines if not (l.startswith("|") and l.split("|")[1] == first_cell and "tier" in l)]
    key = "(%s tier" % tier
    idx = [i for i, l in enumerate(lines) if l.startswith("| `") and key in l]
    if not idx:
        idx = [i for i, l in enumerate(lines) if l.startswith("| `") and "| done" in l]
    lines.insert(max(idx) + 1, row)
    return nl.join(lines)


def digest(board_text, known):
    """[(row, first lines)] for the failing rows named in the board's WHY section."""
    m = re.search(r"WHY THE FAILING ROW\(S\) FAILED(.*)", board_text, re.S)
    out = []
    if not m:
        return out
    parts = re.split(r"^\s*--- (\S+) ---\s*$", m.group(1), flags=re.M)
    for name, body in zip(parts[1::2], parts[2::2]):
        lines = [l.strip() for l in body.splitlines() if l.strip() and not l.strip().startswith("re-run just")]
        out.append((name, lines[:4], name in known))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True)
    ap.add_argument("--stem", required=True)
    ap.add_argument("--note", default="")
    ap.add_argument("--tier", default="1000-1500")
    ap.add_argument("--no-board", action="store_true")
    ap.add_argument("--known", default="doc_links")
    ap.add_argument("--rows", default="", help="forwarded to check-contracts.sh --rows (a partial board; the full board is the default)")
    a = ap.parse_args(argv)
    known = set(x for x in a.known.split(",") if x)

    before = before_lines(a.src)
    core, pieces, has_header = pieces_of(a.src, a.stem)
    if not pieces or not has_header:
        print("post_split: no %s_*.cpp pieces or no %s_internal.h next to %s (install the split first)" % (a.stem, a.stem, a.src), file=sys.stderr)
        return 2
    row = plan_row(a.src, a.stem, before, core, pieces, a.note, a.tier)
    plan_path = os.path.join(REPO, PLAN)
    text = open(plan_path, "rb").read().decode("utf-8")
    open(plan_path, "wb").write(insert_row(text, row, a.tier).encode("utf-8"))
    print("plan row:", row)

    for cmd in ([sys.executable, "tools/split/split_map.py", "--write"],
                [sys.executable, "tools/file_size_ratchet.py", "--check", "--tighten"]):
        r = _run(cmd)
        print((r.stdout.decode("utf-8", "replace").strip().splitlines() or [""])[-1])
        if r.returncode:
            print(r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace"))
            return 1
    if a.no_board:
        return 0
    bash = shutil.which("bash") or "bash"     # the PATH lookup finds Git's bash; a bare "bash" can start WSL's
    r = _run([bash, "scripts/check-contracts.sh"] + (["--rows", a.rows] if a.rows else []))
    text = r.stdout.decode("utf-8", "replace") + r.stderr.decode("utf-8", "replace")
    summary = re.findall(r"^\s*\d+ passed, \d+ failed.*$", text, re.M)
    print(summary[-1].strip() if summary else "board: no summary line found")
    bad = 0
    for name, lines, is_known in digest(text, known):
        print("  %s%s" % (name, " (known)" if is_known else ""))
        for l in lines:
            print("      " + l[:200])
        bad += 0 if is_known else 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
