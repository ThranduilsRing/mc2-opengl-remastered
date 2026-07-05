#!/usr/bin/env python3
"""whatran.py — reconcile "what actually ran" for a deployed MC2 install.

Truth-First arc P1 #4. Answers the provenance question the arc exists to kill:
does the binary on disk, the deploy manifest, the run log's stamped git sha,
the gate state, and any crashbundle all agree — or is one of them lying?

Reconciles up to five independent identity sources for one install dir:
  1. exe bytes         — sha256(mc2.exe) + size (byte identity of the binary)
  2. deploy manifest   — .deployed_manifest.csv src_commit + version + rows
  3. run log           — [INSTR v1] `build=<sha>` + armed gate flags (--log)
  4. crashbundle       — newest crash_*/profile.json "build_hash" (if any)
  5. debug-state dump  — presence of debug_state/ (informational)

The git-sha verdict is the point: manifest src_commit vs run-log build= vs
crashbundle build_hash must MATCH (12-hex or a 'dirty' suffix of the same).
Any disagreement => STALE/MISMATCH => do not trust "shipping build good".

Usage:
    py -3 scripts/whatran.py <install-dir> [--log <stderr.log>] [--json]

Exit: 0 reconciled/clean, 2 mismatch, 1 usage/IO.
"""
import argparse
import csv
import glob
import hashlib
import json
import os
import re
import sys

INSTR_BUILD_RE = re.compile(r"\bbuild=([0-9a-fA-F]{7,40}|UNKNOWN)")
INSTR_LINE_RE = re.compile(r"\[INSTR v1\] enabled:(.*)")
GATE_KV_RE = re.compile(r"(\w+)=(\d+)")
MANIFEST_NAME = ".deployed_manifest.csv"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_manifest(install):
    path = os.path.join(install, MANIFEST_NAME)
    out = {"present": False, "path": path}
    if not os.path.isfile(path):
        return out
    out["present"] = True
    try:
        with open(path, newline="", encoding="utf-8") as f:
            rows = list(csv.reader(f))
        # Row 0: manifest_version,v1,,,  Row 1: column header  Row 2+: data.
        if rows and rows[0][:1] == ["manifest_version"] and len(rows[0]) >= 2:
            out["version"] = rows[0][1]
        col_header = rows[1] if len(rows) >= 2 else []
        try:
            sc_idx = col_header.index("src_commit")
        except ValueError:
            sc_idx = None
        data_rows = rows[2:]
        if sc_idx is not None:
            commits = {r[sc_idx].strip() for r in data_rows
                       if len(r) > sc_idx and r[sc_idx].strip()}
            if len(commits) == 1:
                out["src_commit"] = next(iter(commits))
            elif len(commits) > 1:
                out["src_commit"] = "MIXED"
                out["mixed_commits"] = sorted(commits)
        out["rows"] = len(data_rows)
    except Exception as e:  # noqa: BLE001
        out["error"] = str(e)
    return out


def parse_log(log_path):
    out = {"present": False, "path": log_path, "build": None, "gates_on": [],
           "gates_off": []}
    if not log_path or not os.path.isfile(log_path):
        return out
    out["present"] = True
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    m = INSTR_BUILD_RE.search(text)
    if m:
        out["build"] = m.group(1)
    lm = INSTR_LINE_RE.search(text)
    if lm:
        for k, v in GATE_KV_RE.findall(lm.group(1)):
            (out["gates_on"] if v != "0" else out["gates_off"]).append(k)
    return out


def read_crashbundle(install):
    out = {"present": False, "build_hash": None}
    cands = sorted(glob.glob(os.path.join(install, "crash_*", "profile.json")))
    if not cands:
        return out
    newest = cands[-1]
    out["present"] = True
    out["path"] = newest
    try:
        with open(newest, "r", encoding="utf-8") as f:
            out["build_hash"] = json.load(f).get("build_hash")
    except Exception as e:  # noqa: BLE001
        out["error"] = str(e)
    return out


def norm_sha(s):
    """Normalize a git sha for comparison: lowercase, strip dirty markers,
    truncate to the shorter length so 12-hex vs 40-hex still compares."""
    if not s or s == "UNKNOWN":
        return None
    return re.sub(r"[^0-9a-f]", "", s.lower())


def sha_match(a, b):
    na, nb = norm_sha(a), norm_sha(b)
    if not na or not nb:
        return None
    n = min(len(na), len(nb))
    return na[:n] == nb[:n]


def main(argv):
    ap = argparse.ArgumentParser(description="Reconcile what actually ran for an install.")
    ap.add_argument("install", help="deployed install directory")
    ap.add_argument("--log", help="a run's stderr/stdout log to read build= + gates from")
    ap.add_argument("--exe-name", default="mc2.exe")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)

    if not os.path.isdir(args.install):
        print(f"[whatran] FAIL: not a directory: {args.install}", file=sys.stderr)
        return 1

    exe = os.path.join(args.install, args.exe_name)
    exe_info = {"present": os.path.isfile(exe), "name": args.exe_name}
    if exe_info["present"]:
        exe_info["sha256"] = sha256_file(exe)
        exe_info["size"] = os.path.getsize(exe)

    manifest = read_manifest(args.install)
    log = parse_log(args.log)
    crash = read_crashbundle(args.install)
    dump_present = os.path.isdir(os.path.join(args.install, "debug_state"))

    # Reconcile git shas across the sources that carry one.
    sources = {}
    if manifest.get("src_commit"):
        sources["manifest"] = manifest["src_commit"]
    if log.get("build"):
        sources["run_log"] = log["build"]
    if crash.get("build_hash"):
        sources["crashbundle"] = crash["build_hash"]

    verdict = "UNKNOWN"
    mismatches = []
    unknown_sha = any(v == "UNKNOWN" for v in sources.values())
    if len(sources) >= 2:
        keys = list(sources)
        ref = sources[keys[0]]
        agree = True
        for k in keys[1:]:
            r = sha_match(ref, sources[k])
            if r is False:
                agree = False
                mismatches.append((keys[0], sources[keys[0]], k, sources[k]))
        verdict = "MISMATCH" if (not agree or unknown_sha) else "RECONCILED"
    elif len(sources) == 1:
        verdict = "SINGLE_SOURCE" + ("_UNKNOWN" if unknown_sha else "")

    result = {
        "install": args.install,
        "exe": exe_info,
        "manifest": manifest,
        "run_log": log,
        "crashbundle": crash,
        "debug_state_dump_present": dump_present,
        "git_sha_sources": sources,
        "verdict": verdict,
        "mismatches": mismatches,
    }

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(f"[whatran] install: {args.install}")
        if exe_info["present"]:
            print(f"  exe        : {args.exe_name} sha256={exe_info['sha256'][:16]}… "
                  f"size={exe_info['size']}")
        else:
            print(f"  exe        : MISSING ({args.exe_name})")
        print(f"  manifest   : "
              + (f"src_commit={manifest.get('src_commit','?')} "
                 f"rows={manifest.get('rows','?')}" if manifest["present"]
                 else "ABSENT (no .deployed_manifest.csv)"))
        if log["present"]:
            print(f"  run log    : build={log.get('build','?')} "
                  f"gates_on={len(log['gates_on'])}")
        else:
            print("  run log    : (none provided — pass --log to reconcile the running stamp)")
        print(f"  crashbundle: "
              + (f"build_hash={crash.get('build_hash','?')}" if crash["present"]
                 else "none"))
        print(f"  debug dump : {'present' if dump_present else 'absent'}")
        print(f"  git-sha sources: {sources}")
        print(f"  VERDICT    : {verdict}")
        for a_k, a_v, b_k, b_v in mismatches:
            print(f"    MISMATCH: {a_k}={a_v} != {b_k}={b_v}")

    return 2 if verdict == "MISMATCH" else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
