#!/usr/bin/env python3
"""check_boundaries.py -- the one boundary scanner.

Reads the declarative table scripts/boundary_rules.json (path globs -> forbidden includes/symbols, with allowlists
and corpus floors) and checks every rule in ONE traversal of the repo file list (git ls-files, tracked + untracked,
ignores honoured). It replaces seven board rows: include_firewall, sim_no_audio_reads, no_gl_census, json_isolation,
particles_no_cpu_projection, unified_projection_retirement and gate_echo_reads. The gate-echo decision logic (print
argument-list context) stays in scripts/check-gate-echo-reads.py and is called per file by its `py_call` rule.

An absence rule over an empty corpus is not a pass: rules with a `floor` fail ("WENT BLIND") when the corpus is
smaller than the floor.

USAGE
  py -3 scripts/check_boundaries.py [--quiet] [--root DIR] [--table FILE] [--only RULE_ID_PREFIX]
Exit 0 clean, 1 violation or blind rule, 2 bad table.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_TABLE = HERE / "boundary_rules.json"


def list_files(root: Path) -> list[str]:
    r = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                       capture_output=True, check=True)
    return sorted({p for p in r.stdout.decode("utf-8", "replace").split("\0") if p and (root / p).is_file()})


class Corpus:
    def __init__(self, root: Path):
        self.root = root
        self.files = list_files(root)
        self._text: dict[str, str | None] = {}

    def text(self, rel: str) -> str | None:
        if rel not in self._text:
            try:
                data = (self.root / rel).read_bytes()
            except OSError:
                data = b""
            self._text[rel] = None if b"\0" in data[:4096] else data.decode("utf-8", "replace")
        return self._text[rel]


def scope(corpus: Corpus, rule: dict, exts=...) -> list[str]:
    exts = rule.get("exts") if exts is ... else exts
    ex = tuple(rule.get("exclude_prefixes", []))
    out = []
    for rel in corpus.files:
        if not any(rel.startswith(d) for d in rule["dirs"]):
            continue
        if ex and rel.startswith(ex):
            continue
        if exts is not None and not rel.endswith(tuple(exts)):
            continue
        out.append(rel)
    return out


def load_list(root: Path, rel: str) -> list[str]:
    p = root / rel
    return [ln.rstrip("\r") for ln in p.read_text(encoding="utf-8", errors="replace").split("\n")]


STR_RX = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def clean(content: str, pat: dict) -> str:
    if pat.get("strip_strings"):
        content = STR_RX.sub("", content)
    if pat.get("strip_line_comment"):
        content = re.sub(r"//.*$", "", content)
    for m in pat.get("mask", []):
        content = content.replace(m, "")
    return content


def run_line_rule(corpus: Corpus, rule: dict, files: list[str]) -> tuple[list[str], int]:
    root = corpus.root
    allow_files = set(rule.get("allow_files", []))
    if "allow_list_file" in rule:
        allow_files |= {ln for ln in load_list(root, rule["allow_list_file"]) if ln and not ln.startswith("#")}
    allow_base = set(rule.get("allow_basenames", []))
    subs = []
    if "allow_substrings_file" in rule:
        subs = [ln for ln in load_list(root, rule["allow_substrings_file"]) if ln.strip() and not re.match(r"^\s*#", ln)]
    prefixes = tuple(rule.get("comment_prefixes", []))
    pats = [(re.compile(p["re"]), p) for p in rule["patterns"]]
    errs: list[str] = []
    matching_files = 0
    for rel in files:
        text = corpus.text(rel)
        if text is None:
            continue
        file_hit = False
        for no, line in enumerate(text.split("\n"), 1):
            line = line.rstrip("\r")
            for rx, p in pats:
                if not rx.search(line):
                    continue
                if rx.search(clean(line, p)) is None:
                    continue
                file_hit = True
                if rel in allow_files or rel.rsplit("/", 1)[-1] in allow_base:
                    continue
                if prefixes and line.lstrip().startswith(prefixes):
                    continue
                if p.get("skip_line_re") and re.search(p["skip_line_re"], line):
                    continue
                if subs and any(s in f"{rel}:{line}" for s in subs):
                    continue
                errs.append(f"[boundary:{rule['id']}] {rel}:{no}: {p['msg']}: {line.strip()[:120]}")
        matching_files += file_hit
    return errs, matching_files


def run_py_rule(corpus: Corpus, rule: dict, files: list[str]) -> list[str]:
    path = corpus.root / rule["module"]
    spec = importlib.util.spec_from_file_location("boundary_" + path.stem.replace("-", "_"), path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    fn = getattr(mod, rule["func"])
    errs: list[str] = []
    for rel in files:
        text = corpus.text(rel)
        if text is not None:
            errs += [f"[boundary:{rule['id']}] {e}" for e in fn(rel, text)]
    return errs


def run(root: Path, table: Path, only: str | None = None) -> tuple[list[str], list[str]]:
    rules = json.loads(table.read_text(encoding="utf-8"))["rules"]
    corpus = Corpus(root)
    errs: list[str] = []
    notes: list[str] = []
    for rule in rules:
        if only and not rule["id"].startswith(only):
            continue
        files = scope(corpus, rule)
        if rule.get("kind") == "py_call":
            errs += run_py_rule(corpus, rule, files)
            notes.append(f"{rule['id']}: {len(files)} files")
            continue
        got, matching = run_line_rule(corpus, rule, files)
        floor = rule.get("floor")
        if floor:
            n = matching if floor["kind"] == "matching_files" else len(scope(corpus, rule, floor.get("exts", rule.get("exts"))))
            if n < floor["min"]:
                errs.append(f"[boundary:{rule['id']}] WENT BLIND: {n} {floor['kind']} file(s), floor is {floor['min']}; "
                            "an absence verdict over an empty corpus is not a pass")
        errs += got
        notes.append(f"{rule['id']}: {len(files)} files")
    return errs, notes


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--root", default=str(HERE.parent))
    ap.add_argument("--table", default=str(DEFAULT_TABLE))
    ap.add_argument("--only")
    a = ap.parse_args()
    try:
        errs, notes = run(Path(a.root), Path(a.table), a.only)
    except (OSError, KeyError, ValueError, json.JSONDecodeError) as exc:
        print(f"check_boundaries: bad table or input: {exc}", file=sys.stderr)
        return 2
    for e in errs:
        print(e)
    if errs:
        print(f"check_boundaries: FAILED ({len(errs)})")
        return 1
    if not a.quiet:
        print("check_boundaries: OK (" + "; ".join(notes) + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
