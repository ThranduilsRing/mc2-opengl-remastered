#!/usr/bin/env python3
"""file_callsites - turn the C4996 warnings of a deprecation build into the committed call-site list of the step 4 B3 file-and-stream ownership API.

WHAT: reads the log of a build in which the ownership API of File / PacketFile / FitIniFile / ABLFile carries `[[deprecated("B3")]]`
(tests/analyze/deprecate_b3.py on), de-duplicates the warnings (a header is compiled by many translation units) and writes
`.claude/cleanup/cxx23-step4-b3-callsites.txt`: one `file:line:kind` row per site, the counts per file, per kind and per batch on top, and the git blob id of
every file that has rows (the same host-independent seal as the EString and container lists). A kind is the declaring class and member
(`File::operator new`, `File::operator delete`, `File::getFilename`, `File::addChild`, ...).
WHY: grep finds the files that spell `new File`; only the compiler finds every `new` and `delete` that goes through a typedef (FilePtr, FitIniFilePtr), a derived
class or a member deleted far from its allocation. Each batch commit removes its rows (--drop / --remap) and the count only goes down.
Not seen by the compiler: inactive `#if` branches. `--ifdef-scan` greps those and prints the hits so the census file can carry them as comment lines.

  py -3 tests/analyze/file_callsites.py BUILD.log [--out PATH]    # census from a deprecation build
  py -3 tests/analyze/file_callsites.py --drop code/x.cpp code/y.cpp:42 ...
  py -3 tests/analyze/file_callsites.py --remap BASE code/x.cpp ...
  py -3 tests/analyze/file_callsites.py --add code/x.cpp:42:File::operator\\ new ...   # a call that moved onto a line a conversion wrote
  py -3 tests/analyze/file_callsites.py --seal | --check | --ifdef-scan
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, HERE)
import console_child  # noqa: E402
import estring_callsites as ec  # noqa: E402  (blob_hashes / recorded_blobs are not EString specific)

DEFAULT_OUT = os.path.join(ROOT, ".claude", "cleanup", "cxx23-step4-b3-callsites.txt")
FILES_JSON = os.path.join(ROOT, ".claude", "cleanup", "cxx23-step4-b3-files.json")
_DIAG = re.compile(r"^(?P<file>[A-Za-z]:[^()]*?)\((?P<line>\d+)(?:,(?P<col>\d+))?\)\s*:\s*(?:warning|error)\s+C4996:\s*'(?P<kind>[^']*)'\s*:\s*B3")
TYPES = ("File", "PacketFile", "FitIniFile", "ABLFile")
#: The classes' own declaring and defining files: a use of the API inside them is the class talking to itself (its batch is B3.1), not a caller.
SELF = {"mclib/file.h", "mclib/packet.h", "mclib/inifile.h", "mclib/ablscan.h"}
_MEMBER = re.compile(r"(?:^|::)(operator\s*(?:new|delete)|\w+)\s*$")


def normalise(kind: str) -> str:
    """'File::operator new' stays; 'ABLFile::operator delete' stays; spaces are kept inside 'operator new' and dropped elsewhere."""
    return re.sub(r"\s+", " ", kind.strip())


def tracked_case() -> dict:
    """lower-case path -> the path as git tracks it: the compiler prints a file as the #include spelled it."""
    out = subprocess.run(["git", "ls-files"], capture_output=True, text=True, cwd=ROOT, **console_child.child_kwargs(redirected=True)).stdout.splitlines()
    return {line.lower(): line for line in out if line}


def parse(log_path: str, root: str):
    root = root.replace("\\", "/").rstrip("/") + "/"
    case = tracked_case()
    seen = set()
    rows = []
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            m = _DIAG.match(ln.strip())
            if not m:
                continue
            kind = normalise(m["kind"])
            if kind.split("::")[0] not in TYPES:
                continue
            path = m["file"].replace("\\", "/")
            if path.lower().startswith(root.lower()):
                path = path[len(root):]
            path = case.get(path.lower(), path)
            if path.lower() in {s.lower() for s in SELF}:
                continue
            key = (path, int(m["line"]), m["col"] or "0", kind)
            if key not in seen:
                seen.add(key)
                rows.append(key)
    rows.sort()
    return rows


def batch_of() -> dict:
    try:
        with open(FILES_JSON, encoding="utf-8") as f:
            return json.load(f)["files"]
    except OSError:
        return {}


def render(rows, blobs=None) -> str:
    per_file = collections.Counter(r[0] for r in rows)
    batches = batch_of()
    per_batch = collections.Counter()
    unlisted = collections.Counter()
    for path, _line, _col, _kind in rows:
        if path in batches:
            per_batch[batches[path]] += 1
        else:
            unlisted[path] += 1
    per_kind = collections.Counter(r[3] for r in rows)
    out = ["# Step 4 B3 file-and-stream ownership sites from a [[deprecated(\"B3\")]] build (tests/analyze/file_callsites.py); a batch commit removes its rows.",
           f"# total {len(rows)} sites in {len(per_file)} files",
           "# per kind:"]
    out += [f"#   {n:5d}  {k}" for k, n in per_kind.most_common()]
    out += ["# per batch (cxx23-step4-b3-files.json):"]
    out += [f"#   {n:5d}  {b}" for b, n in sorted(per_batch.items())]
    out += [f"# sites in files the batch table does NOT list: {sum(unlisted.values())} in {len(unlisted)} files"]
    out += [f"#   {n:5d}  {p}" for p, n in sorted(unlisted.items(), key=lambda kv: (-kv[1], kv[0]))]
    out += ["# per file:"]
    out += [f"#   {n:5d}  {p}" for p, n in sorted(per_file.items(), key=lambda kv: (-kv[1], kv[0]))]
    out += [f"# blob {h} {p}" for p, h in sorted((blobs or {}).items()) if p in per_file]
    inactive = scan_inactive()
    out += [f"# mentions inside inactive #if branches (grep, the compiler cannot see them): {len(inactive)}"]
    out += [f"#   {h}" for h in inactive]
    out += ["# file:line:kind"]
    out += [f"{p}:{ln}:{k}" for p, ln, _col, k in rows]
    return "\n".join(out) + "\n"


def read_rows(path: str):
    rows = []
    with open(path, encoding="utf-8") as f:
        for ln in f:
            if ln.startswith("#") or not ln.strip():
                continue
            file, line, kind = ln.rstrip().split(":", 2)
            rows.append((file, int(line), "0", kind))
    return rows


def write(path: str, rows, blobs) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(render(rows, blobs))


def drop(path: str, targets: list[str]) -> int:
    whole = {t for t in targets if ":" not in t}
    lines = {tuple(t.rsplit(":", 1)) for t in targets if ":" in t}
    rows = [r for r in read_rows(path) if r[0] not in whole and (r[0], str(r[1])) not in lines]
    write(path, rows, {p: h for p, h in ec.recorded_blobs(path).items() if p in {r[0] for r in rows}})
    print(f"file_callsites: {len(rows)} sites in {len({r[0] for r in rows})} files remain -> {path}")
    return 0


def remap(path: str, base: str, targets: list[str]) -> int:
    """Move the rows of `targets` from their lines at commit `base` to the working tree's. A row whose own line was edited is dropped: the edit is the
    conversion, and the next census settles what is left on that line."""
    hunks: dict[str, list[tuple[int, int, int]]] = {}
    for t in targets:
        diff = subprocess.run(["git", "diff", "-U0", base, "--", t], capture_output=True, text=True, cwd=ROOT, **console_child.child_kwargs(redirected=True)).stdout
        hunks[t] = [(int(m[1]), int(m[2]) if m[2] is not None else 1, int(m[4]) if m[4] is not None else 1)
                    for m in re.finditer(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", diff, re.M)]

    def moved(file: str, old: int):
        delta = 0
        for old_start, old_count, new_count in hunks[file]:
            if old_count == 0:
                if old_start < old:
                    delta += new_count
                continue
            if old < old_start:
                break
            if old < old_start + old_count:
                return None
            delta += new_count - old_count
        return old + delta

    rows, dropped = [], 0
    for file, line, col, kind in read_rows(path):
        if file in hunks:
            new = moved(file, line)
            if new is None:
                dropped += 1
                continue
            line = new
        rows.append((file, line, col, kind))
    blobs = ec.recorded_blobs(path)
    blobs.update(ec.blob_hashes(targets))
    write(path, rows, blobs)
    print(f"file_callsites: {dropped} row(s) on edited lines dropped; {len(rows)} sites in {len({r[0] for r in rows})} files remain -> {path}")
    return 0


def add(path: str, specs: list[str]) -> int:
    """Append rows `file:line:kind` for sites that now sit on a line a conversion wrote. The file's content seal is refreshed."""
    rows = read_rows(path)
    have = {(r[0], r[1], r[3]) for r in rows}
    touched = set()
    for spec in specs:
        file, line, kind = spec.split(":", 2)
        if (file, int(line), kind) not in have:
            rows.append((file, int(line), "0", kind))
            touched.add(file)
    rows.sort()
    blobs = ec.recorded_blobs(path)
    blobs.update(ec.blob_hashes(touched))
    write(path, rows, {p: h for p, h in blobs.items() if p in {r[0] for r in rows}})
    print(f"file_callsites: {len(rows)} sites in {len({r[0] for r in rows})} files -> {path}")
    return 0


def kind_token(kind: str) -> str | None:
    """The text a row's kind leaves on its source line. `operator new` leaves `new`; `operator delete` leaves `delete`; a member leaves its name."""
    m = _MEMBER.search(kind)
    if not m:
        return None
    member = m[1]
    if member.startswith("operator"):
        return member.split()[-1]
    return member


def check(path: str) -> int:
    total = files = None
    rows = []
    with open(path, encoding="utf-8") as f:
        for ln in f:
            m = re.match(r"# total (\d+) sites in (\d+) files", ln)
            if m:
                total, files = int(m[1]), int(m[2])
            if ln.startswith("#") or not ln.strip():
                continue
            file, line, kind = ln.rstrip().split(":", 2)
            rows.append((file, int(line), kind))
    problems = []
    recorded = ec.recorded_blobs(path)
    current = ec.blob_hashes({r[0] for r in rows})
    for file in sorted({r[0] for r in rows}):
        if file not in recorded:
            problems.append(f"{file}: no recorded content hash (run --seal right after a census, or --remap BASE {file})")
        elif current.get(file) != recorded[file]:
            problems.append(f"{file}: edited since its rows were verified (run --remap BASE {file}, or --drop {file} when it is converted)")
    if (total, files) != (len(rows), len({r[0] for r in rows})):
        problems.append(f"header says {total} sites in {files} files, the rows are {len(rows)} in {len({r[0] for r in rows})}")
    cache: dict[str, list[str] | None] = {}
    for file, line, kind in rows:
        if file not in cache:
            try:
                with open(os.path.join(ROOT, file), encoding="utf-8", errors="replace") as src:
                    cache[file] = src.read().splitlines()
            except OSError:
                cache[file] = None
        text = cache[file]
        if text is None:
            problems.append(f"{file}:{line}:{kind}: the file is gone")
        elif not 1 <= line <= len(text):
            problems.append(f"{file}:{line}:{kind}: the file has {len(text)} lines")
        else:
            token = kind_token(kind)
            # A delete of a File-family object may be spelled by a smart-pointer deleter or a macro (SAFE_DELETE, delete[]): the content seal covers those.
            if token not in (None, "delete") and token not in text[line - 1] and not re.search(r"\b[A-Z][A-Z0-9_]{3,}\(", text[line - 1]):
                problems.append(f"{file}:{line}:{kind}: the line no longer contains '{token}'")
    for p in problems:
        print(f"file_callsites: {p}")
    print(f"file_callsites: {'FAIL' if problems else 'OK'} - {len(rows)} sites, {len(problems)} problem(s)")
    return 1 if problems else 0


def seal(path: str) -> int:
    rows = read_rows(path)
    write(path, rows, ec.blob_hashes({r[0] for r in rows}))
    print(f"file_callsites: sealed {len({r[0] for r in rows})} files -> {path}")
    return 0


def scan_inactive() -> list[str]:
    """Every mention of a File-family `new` inside a conditional-compilation branch the default build does not compile (#if 0, LAB_ONLY, TERRAINEDIT,
    _DEBUG, ...), found by grep: the compiler cannot see these."""
    guard = re.compile(r"^\s*#\s*if(?:def|ndef)?\s+(\S.*)$")
    names = re.compile(r"\bnew\s+(File|PacketFile|FitIniFile|ABLFile)\b|\b(FilePtr|FitIniFilePtr|PacketFilePtr)\b[^;]*=\s*new\b")
    inactive = re.compile(r"\b(0|LAB_ONLY|TERRAINEDIT|_DEBUG|DEBUG|_ARMOR|KTL_DEBUG|DEBUG_\w+|LOG\w*FILE\w*)\b")
    files = subprocess.run(["git", "ls-files", "code", "mclib", "gui", "editor"], capture_output=True, text=True, cwd=ROOT,
                           **console_child.child_kwargs(redirected=True)).stdout.split()
    hits: list[str] = []
    for rel in files:
        if not rel.endswith((".cpp", ".h", ".hpp", ".inl")):
            continue
        try:
            with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
                text = f.read().splitlines()
        except OSError:
            continue
        stack: list[tuple[bool, int]] = []
        for no, ln in enumerate(text, 1):
            m = guard.match(ln)
            if m and not re.match(r"^\s*#\s*ifndef", ln):
                stack.append((bool(inactive.search(m[1])), no))
            elif re.match(r"^\s*#\s*ifndef", ln):
                stack.append((False, no))
            elif re.match(r"^\s*#\s*endif", ln) and stack:
                stack.pop()
            elif re.match(r"^\s*#\s*else", ln) and stack:
                flag, start = stack.pop()
                stack.append((not flag, start))
            if stack and any(flag for flag, _ in stack) and names.search(ln) and not ln.lstrip().startswith(("#if", "#else", "#endif")):
                hits.append(f"{rel}:{no} (branch opened at {stack[-1][1]}): {ln.strip()[:110]}")
    return [h for h in hits if h.split(":")[0] not in SELF]


def ifdef_scan() -> int:
    for line in scan_inactive():
        print(line)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", help="the deprecation build's log")
    ap.add_argument("--drop", nargs="+", metavar="FILE[:LINE]")
    ap.add_argument("--remap", nargs="+", metavar=("BASE", "FILE"))
    ap.add_argument("--add", nargs="+", metavar="FILE:LINE:KIND")
    ap.add_argument("--seal", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--ifdef-scan", action="store_true")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--root", default=ROOT)
    args = ap.parse_args()
    if args.remap:
        if len(args.remap) < 2:
            ap.error("--remap needs a base commit and at least one file")
        return remap(args.out, args.remap[0], args.remap[1:])
    if args.add:
        return add(args.out, args.add)
    if args.seal:
        return seal(args.out)
    if args.check:
        return check(args.out)
    if args.drop:
        return drop(args.out, args.drop)
    if args.ifdef_scan:
        return ifdef_scan()
    if not args.log:
        ap.error("a build log is required")
    rows = parse(args.log, args.root)
    if not rows:
        print("file_callsites: no B3 C4996 rows in the log (was the deprecation applied?)", file=sys.stderr)
        return 1
    write(args.out, rows, ec.blob_hashes({r[0] for r in rows}))
    print(f"file_callsites: {len(rows)} sites in {len({r[0] for r in rows})} files -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
