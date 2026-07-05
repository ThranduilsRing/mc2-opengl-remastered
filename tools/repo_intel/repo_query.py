#!/usr/bin/env python3
"""
REPO-INTEL-1a/1b/1c/1d: repo_query.py
Read-only codebase intelligence CLI.

Usage:
    python tools/repo_intel/repo_query.py dirty
    python tools/repo_intel/repo_query.py harness build|deploy|smoke|tier1|all
    python tools/repo_intel/repo_query.py preflight
    python tools/repo_intel/repo_query.py env MC2_GPU_MECHS
    python tools/repo_intel/repo_query.py env --undocumented
    python tools/repo_intel/repo_query.py env --domain shadow
    python tools/repo_intel/repo_query.py env --all
    python tools/repo_intel/repo_query.py env          (summary only)
    python tools/repo_intel/repo_query.py binding 5
    python tools/repo_intel/repo_query.py binding --conflicts
    python tools/repo_intel/repo_query.py binding --namespace ssbo
    python tools/repo_intel/repo_query.py dead-gate-scan
    python tools/repo_intel/repo_query.py dead-gate-scan --tier A
    python tools/repo_intel/repo_query.py dead-gate-scan --name HDR

Guards (apply to dirty + preflight):
    --expect-branch BRANCH   fail if not on BRANCH (branch contamination guard)
    --expect-root   PATH     fail if worktree root != PATH (shared-worktree guard)

Example full preflight:
    py -3 tools\\repo_intel\\repo_query.py preflight \\
        --expect-branch claude/nifty-mendeleev \\
        --expect-root A:\\Games\\mc2-opengl-src\\.claude\\worktrees\\nifty-mendeleev
"""

import json
import os
import re
import csv
import subprocess
import sys
from pathlib import Path

# Allow sibling modules (env_index, etc.) to be imported when running as script
sys.path.insert(0, str(Path(__file__).parent))


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def git(args, cwd=None, strip=True, timeout=10):
    try:
        result = subprocess.run(
            ["git"] + args,
            capture_output=True, text=True, cwd=cwd, timeout=timeout,
            stdin=subprocess.DEVNULL,
        )
        out = result.stdout.strip() if strip else result.stdout
        return out, result.returncode
    except subprocess.TimeoutExpired:
        return "", 1


def find_repo_root():
    out, rc = git(["rev-parse", "--show-toplevel"])
    if rc != 0:
        return None
    return Path(out)


def find_claude_md(repo_root):
    candidate = repo_root / "CLAUDE.md"
    if candidate.exists():
        return candidate
    # fall back to project-level
    parent = repo_root.parent
    candidate2 = parent / "CLAUDE.md"
    if candidate2.exists():
        return candidate2
    return None


def out_json(obj):
    print(json.dumps(obj, indent=2))


def _normalize_path(p: str) -> str:
    """Normalize a path for comparison: resolve, lower-case, forward slashes."""
    return str(Path(p).resolve()).lower().replace("\\", "/")


# ---------------------------------------------------------------------------
# File classification
# ---------------------------------------------------------------------------

# Order matters: first match wins.
_CLASSIFICATION_RULES = [
    # protected - never touch
    (re.compile(r"^(build[^/]*/|3rdparty/|\.git/|releases?/|dist/)", re.I),
     "protected", "build/3rdparty/git/release tree - never touch"),
    # generated cmake/build artefacts
    (re.compile(r"\.(vcxproj|cmake_install\.cmake|sln)$", re.I),
     "protected", "generated build artefact"),
    (re.compile(r"CMakeCache\.txt$|CMakeFiles/", re.I),
     "protected", "CMake cache/generated - never touch"),
    # deploy rail
    (re.compile(r"^scripts/deploy_payload\.py$", re.I),
     "deploy_rail", "deploy harness - edit with care"),
    (re.compile(r"^scripts/run_smoke\.py$", re.I),
     "deploy_rail", "smoke harness - edit with care"),
    # key source (render core, engine, game code, adapters)
    (re.compile(r"^(code|GameOS|mclib|RenderCore|GameAdapters)/", re.I),
     "key_source", "key engine source - requires smoke gate before merge"),
    # shaders
    (re.compile(r"^shaders/.*\.(vert|frag|comp|geom|hglsl|tesc|tese)$", re.I),
     "shader", "shader file - subject to shader discipline"),
    # docs / markdown
    (re.compile(r"^docs/|\.md$", re.I),
     "docs", "documentation"),
    # tools and scripts (general - not deploy rail)
    (re.compile(r"^(tools|scripts)/", re.I),
     "normal", "tool/script"),
    # data assets
    (re.compile(r"^data/", re.I),
     "normal", "data asset"),
    # .claude/ internal (config, memory, skills)
    (re.compile(r"^\.claude/", re.I),
     "normal", "claude config/memory"),
    # test artifacts (baselines, visual captures, smoke outputs)
    (re.compile(r"^tests/", re.I),
     "normal", "test artifact"),
    # graphify output (generated graph - not source)
    (re.compile(r"^graphify-out/", re.I),
     "normal", "graphify-generated output"),
    # runtime / temp artifacts at repo root
    (re.compile(
        r"^(run/|imgui\.ini$|sniff\.dat$|.*_stdout\.txt$|.*_stderr\.txt$|"
        r"fire_wire\.png$|.*bridge_manual.*\.txt$|central_merge.*/)"),
     "normal", "runtime/temp artifact"),
]

_SAFE_CLASSES = {"docs", "normal"}


def classify_file(rel_path: str):
    for pattern, cls, reason in _CLASSIFICATION_RULES:
        if pattern.search(rel_path):
            return cls, reason
    return "unknown", "unclassified - treat as sensitive"


def get_dirty_state(repo_root: Path, expect_branch: str = None,
                    expect_root: str = None, digest: bool = False):
    branch, _ = git(["rev-parse", "--abbrev-ref", "HEAD"], cwd=repo_root)
    head, _    = git(["rev-parse", "--short", "HEAD"],      cwd=repo_root)
    status_out, _ = git(["status", "--porcelain=v1"],        cwd=repo_root, strip=False)

    files = []
    for line in status_out.splitlines():
        if not line.strip():
            continue
        xy   = line[:2]
        path = line[3:].strip()
        # handle renames: "old -> new"
        if " -> " in path:
            path = path.split(" -> ")[-1]
        status = xy.strip() or "?"
        cls, reason = classify_file(path)
        files.append({
            "path":   path,
            "status": status,
            "class":  cls,
            "reason": reason,
        })

    dirty        = bool(files)
    unsafe_files = [f for f in files if f["class"] not in _SAFE_CLASSES]

    class_counts = {}
    for f in files:
        class_counts[f["class"]] = class_counts.get(f["class"], 0) + 1

    # Branch guard
    branch_ok      = True
    branch_warning = None
    if expect_branch:
        if branch != expect_branch:
            branch_ok      = False
            branch_warning = (
                f"current branch '{branch}' does not match expected "
                f"'{expect_branch}' - work may have landed on the wrong branch"
            )
    else:
        branch_warning = "no expected branch supplied (pass --expect-branch to enable guard)"

    # Root guard - catches shared-worktree contamination
    root_ok      = True
    root_warning = None
    actual_root  = str(repo_root)
    if expect_root:
        if _normalize_path(actual_root) != _normalize_path(expect_root):
            root_ok      = False
            root_warning = (
                f"current worktree root '{actual_root}' does not match expected "
                f"'{expect_root}' - agent is running in the wrong worktree"
            )
    else:
        root_warning = "no expected root supplied (pass --expect-root to enable guard)"

    safe_to_touch = not bool(unsafe_files) and branch_ok and root_ok
    requires_ack  = not safe_to_touch

    # Digest mode: omit SAFE-class files from the listing (token cut) but keep
    # full counts. Behavior with digest=False is unchanged.
    if digest:
        listed_files = [f for f in files if f["class"] not in _SAFE_CLASSES]
        omitted_safe = len(files) - len(listed_files)
    else:
        listed_files = files
        omitted_safe = 0

    summary = (
        f"{len(files)} dirty file(s); "
        f"{len(unsafe_files)} require ack"
        if dirty else "clean"
    )
    if digest and omitted_safe:
        summary += f" (digest: {omitted_safe} safe files omitted)"

    result = {
        "repo_root":         actual_root,
        "branch":            branch,
        "head":              head,
        "dirty":             dirty,
        "safe_to_touch":     safe_to_touch,
        "requires_user_ack": requires_ack,
        "files":             listed_files,
        "class_counts":      class_counts,
        "summary":           summary,
    }
    if digest:
        result["omitted_safe_count"] = omitted_safe
    if expect_branch:
        result["expected_branch"] = expect_branch
        result["branch_ok"]       = branch_ok
    if branch_warning:
        result["branch_warning"] = branch_warning
    if expect_root:
        result["expected_root"] = expect_root
        result["root_ok"]       = root_ok
    if root_warning:
        result["root_warning"] = root_warning
    return result


# ---------------------------------------------------------------------------
# Harness parsing
# ---------------------------------------------------------------------------

_HARNESSES = {
    "build": {
        "description": "Build mc2 exe (RelWithDebInfo)",
        # Extracted from CLAUDE.md 'Codex build/deploy rails' + cmake path
        "search_patterns": [
            # CMake path line
            re.compile(r"CMake:\s+(.+cmake\.exe)", re.I),
            # build flags
            re.compile(r"--build\s+build64\s+--config\s+RelWithDebInfo\s+--target\s+mc2"),
        ],
        "note": (
            "Invoke via /mc2-build skill. "
            "Do not substitute MSBuild, random cmake, or manual copy commands."
        ),
    },
    "deploy": {
        "description": "Deploy payload to release directory",
        "search_patterns": [
            re.compile(r"scripts/deploy_payload\.py[^\n]*", re.I),
        ],
        "note": (
            "Invoke scripts/deploy_payload.py with --source-root, --build-dir, --exe-name. "
            "For rc1 pass target dir positionally. Never copy files manually."
        ),
    },
    "smoke": {
        "description": "Smoke test gate (tier1, 30s, all 5 missions)",
        "search_patterns": [
            # Capture the canonical powershell block
            re.compile(
                r"Canonical invocation.*?```(?:powershell)?\n(.+?)\n```",
                re.S | re.I
            ),
        ],
        "note": "ALWAYS --keep-logs. NEVER --kill-existing. NEVER --duration >30.",
    },
    "tier1": {
        "description": "Smoke test gate - tier1 5-mission full run",
        "search_patterns": [
            re.compile(
                r"Canonical invocation.*?```(?:powershell)?\n(.+?)\n```",
                re.S | re.I
            ),
        ],
        "note": "Same command as smoke. --tier tier1 --duration 30 --keep-logs.",
    },
}


def parse_harness(name: str, claude_md_path: Path):
    if name not in _HARNESSES:
        return {
            "name": name,
            "error": f"Unknown harness '{name}'. Valid: build, deploy, smoke, tier1",
        }

    h      = _HARNESSES[name]
    text   = claude_md_path.read_text(encoding="utf-8", errors="replace")
    source = str(claude_md_path)

    command   = None
    evidence  = []
    ambiguous = False

    if name in ("smoke", "tier1"):
        # Extract the canonical powershell block verbatim
        m = re.search(
            r"Canonical invocation[^\n]*\n+```(?:powershell)?\n(.+?)\n```",
            text, re.S | re.I
        )
        if m:
            command  = m.group(1).strip()
            evidence = [{"match": command[:120] + ("..." if len(command) > 120 else "")}]
        else:
            ambiguous = True

    elif name == "build":
        cmake_path = None
        # CMake line may have backtick-quoted path with spaces
        m = re.search(r"CMake:\s+`?(.+?cmake\.exe)`?", text, re.I)
        if m:
            cmake_path = m.group(1).strip()
        if cmake_path:
            command = f'"{cmake_path}" --build build64 --config RelWithDebInfo --target mc2'
            evidence = [{"cmake_exe": cmake_path}]
        else:
            ambiguous = True

    elif name == "deploy":
        m = re.search(r"Deploy only with[^\n]*(`scripts/deploy_payload\.py`)[^\n]*", text, re.I)
        if m:
            # Return the canonical script name + documented arg pattern
            command  = "scripts/deploy_payload.py <rc1-target-dir> --source-root <path> --build-dir build64 --exe-name mc2.exe"
            evidence = [{"source_line": m.group(0)[:120]}]
        else:
            ambiguous = True

    result = {
        "name":        name,
        "source":      source,
        "description": h["description"],
        "confidence":  "ambiguous" if ambiguous else "documented",
        "note":        h["note"],
    }
    if command:
        result["command"] = command
    if evidence:
        result["evidence"] = evidence
    if ambiguous:
        result["warning"] = (
            f"Could not extract '{name}' command from {source}. "
            "Do not invent a command - read CLAUDE.md manually."
        )
    return result


# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------

def all_harnesses(claude_md_path: Path):
    names = ["build", "deploy", "smoke", "tier1"]
    return {name: parse_harness(name, claude_md_path) for name in names}


def preflight(repo_root: Path, claude_md_path: Path, expect_branch: str = None,
              expect_root: str = None):
    dirty   = get_dirty_state(repo_root, expect_branch=expect_branch,
                               expect_root=expect_root, digest=True)
    harness = all_harnesses(claude_md_path)

    all_documented = all(
        h.get("confidence") == "documented" for h in harness.values()
    )
    ok = not dirty["requires_user_ack"] and all_documented

    harness_status = " ".join(
        f"{name}={h.get('confidence','unknown')}"
        for name, h in harness.items()
    )
    summary_parts = [
        f"dirty={str(dirty['dirty']).lower()}",
        f"safe_to_touch={str(dirty['safe_to_touch']).lower()}",
        f"branch={dirty['branch']}",
        f"head={dirty['head']}",
        f"harness=[{harness_status}]",
    ]
    if expect_branch:
        summary_parts.append(f"branch_ok={str(dirty.get('branch_ok', True)).lower()}")
    if expect_root:
        summary_parts.append(f"root_ok={str(dirty.get('root_ok', True)).lower()}")
    summary = "PRECHECK: " + " ".join(summary_parts)

    return {
        "ok":      ok,
        "summary": summary,
        "dirty":   dirty,
        "harness": harness,
    }


# ---------------------------------------------------------------------------
# Slice preflight - anti-rediscovery / stale-base gate (DEV-EFFICIENCY-BOOTSTRAP-1)
# ---------------------------------------------------------------------------

def slice_preflight(repo_root, *, slice_name=None, symbols=None, paths=None,
                    base=None, nifty="claude/nifty-mendeleev"):
    """Read-only gate to run BEFORE a recon-derived fix slice. Answers
    'should I proceed, or did someone already fix this / has my base gone stale?'

    Verdict: PASS / WARN / STOP. STOP = a symbol in --symbols changed on the
    mainline since --base (likely already fixed / will conflict) - re-recon
    against current HEAD before writing code."""
    symbols = symbols or []
    paths   = paths or []
    findings = []
    verdict  = "PASS"
    _rank = {"PASS": 0, "WARN": 1, "STOP": 2}

    def bump(v):
        nonlocal verdict
        if _rank[v] > _rank[verdict]:
            verdict = v

    branch, _      = git(["rev-parse", "--abbrev-ref", "HEAD"], cwd=repo_root)
    head, _        = git(["rev-parse", "--short", "HEAD"], cwd=repo_root)
    nifty_head, nrc = git(["rev-parse", "--short", nifty], cwd=repo_root)
    nifty_ok = (nrc == 0)

    # 1. Has a commit already mentioned this slice name?
    if slice_name:
        log, _ = git(["log", "--oneline", "-i", "--grep", slice_name, "-n", "20"],
                     cwd=repo_root)
        hits = [l for l in log.splitlines() if l.strip()]
        if hits:
            bump("WARN")
            findings.append({"check": "slice_name_in_log", "level": "WARN",
                             "detail": f"{len(hits)} commit(s) already mention '{slice_name}'",
                             "commits": hits})

    # 2/3. Drift since base on the mainline (base..nifty): symbols (-G) and paths.
    drift_range = None
    if base and nifty_ok:
        drift_range = f"{base}..{nifty}"
        # base-staleness: how far the mainline moved since the fork point.
        cnt, _ = git(["rev-list", "--count", drift_range], cwd=repo_root)
        try:
            ahead = int(cnt)
        except ValueError:
            ahead = 0
        if ahead > 0:
            lvl = "WARN" if ahead < 40 else "WARN"
            bump(lvl)
            findings.append({"check": "base_stale", "level": lvl,
                             "detail": f"mainline {nifty} is {ahead} commit(s) ahead of base {base}"})

        for sym in symbols:
            log, _ = git(["log", "--oneline", "-G", sym, drift_range,
                          *(["--"] + paths if paths else [])], cwd=repo_root)
            hits = [l for l in log.splitlines() if l.strip()]
            if hits:
                bump("STOP")
                findings.append({"check": "symbol_changed_since_base", "level": "STOP",
                                 "symbol": sym,
                                 "detail": f"'{sym}' changed on {nifty} since base - re-recon "
                                           "against current HEAD before coding",
                                 "commits": hits[:10]})

        if paths:
            log, _ = git(["log", "--oneline", drift_range, "--", *paths], cwd=repo_root)
            hits = [l for l in log.splitlines() if l.strip()]
            if hits:
                bump("WARN")
                findings.append({"check": "paths_changed_since_base", "level": "WARN",
                                 "detail": f"{len(hits)} commit(s) touched the target paths on "
                                           f"{nifty} since base",
                                 "commits": hits[:10]})
    elif base and not nifty_ok:
        bump("WARN")
        findings.append({"check": "nifty_ref", "level": "WARN",
                         "detail": f"mainline ref '{nifty}' not found - drift checks skipped"})
    else:
        findings.append({"check": "base", "level": "INFO",
                         "detail": "no --base given - drift/staleness checks skipped"})

    # 4. Dirty files overlapping the target paths (foreign WIP on my targets).
    if paths:
        status_out, _ = git(["status", "--porcelain=v1"], cwd=repo_root, strip=False)
        overlap = []
        for line in status_out.splitlines():
            if not line.strip():
                continue
            p = line[3:].strip()
            if " -> " in p:
                p = p.split(" -> ")[-1]
            if any(p == tp or p.endswith("/" + tp) or tp.endswith(p) for tp in paths):
                overlap.append(p)
        if overlap:
            bump("WARN")
            findings.append({"check": "dirty_overlap", "level": "WARN",
                             "detail": "uncommitted changes overlap target paths "
                                       "(foreign WIP?) - do not sweep",
                             "files": overlap})

    summary = (f"[slice-preflight] {verdict}: slice={slice_name or '-'} branch={branch} "
               f"head={head} nifty={nifty_head or '?'} "
               f"checks={len(findings)} "
               + ("(symbol changed since base - RE-RECON)" if verdict == "STOP" else
                  "(review warnings)" if verdict == "WARN" else "(clear to proceed)"))

    return {
        "verdict":   verdict,
        "summary":   summary,
        "slice":     slice_name,
        "branch":    branch,
        "head":      head,
        "nifty_head": nifty_head,
        "base":      base,
        "drift_range": drift_range,
        "symbols":   symbols,
        "paths":     paths,
        "findings":  findings,
    }


# ---------------------------------------------------------------------------
# Commit plan - lane attribution for the multi-lane dirty tree
# ---------------------------------------------------------------------------

_TOKEN_RE = re.compile(r"MC2_[A-Z0-9_]+|\b[A-Za-z_][A-Za-z0-9_]{6,}\b")
_GATE_RE  = re.compile(r"MC2_[A-Z0-9_]+")


def _normalize_slice(name):
    return re.sub(r"[^A-Za-z0-9]", "", name or "").upper()


def _build_lane_index(repo_root):
    """Scan .claude/*.md docs; return list of (doc_name, norm_name, tokens)."""
    lanes = []
    claude_dir = Path(repo_root) / ".claude"
    if not claude_dir.is_dir():
        return lanes
    for doc in sorted(claude_dir.glob("*.md")):
        try:
            text = doc.read_text(encoding="utf-8", errors="replace")[:6144]
        except OSError:
            continue
        tokens = set(_GATE_RE.findall(text))
        lanes.append((doc.name, _normalize_slice(doc.name), tokens))
    return lanes


def commit_plan(repo_root, slice_name="", paths=None, base="HEAD"):
    """Heuristic, advisory lane attribution. Flags dirty files/hunks whose
    added tokens are mentioned ONLY in a foreign .claude/*.md slice doc, so a
    multi-lane dirty tree can be staged selectively."""
    repo_root = Path(repo_root)

    # Target paths: explicit, else elevated dirty files.
    if paths:
        target_paths = list(paths)
    else:
        ds = get_dirty_state(repo_root)
        target_paths = [f["path"] for f in ds["files"]
                        if f["class"] not in _SAFE_CLASSES]

    lanes = _build_lane_index(repo_root)
    norm_slice = _normalize_slice(slice_name)
    # Partition lane docs into this-slice vs foreign.
    this_docs = []
    foreign_docs = []
    for doc_name, norm_name, tokens in lanes:
        if norm_slice and norm_slice in norm_name:
            this_docs.append((doc_name, tokens))
        else:
            foreign_docs.append((doc_name, tokens))

    files_out = []
    for path in target_paths:
        diff, _ = git(["diff", base, "--", path], cwd=repo_root, strip=False)
        added_tokens = set()
        hunk_count = 0
        for line in diff.splitlines():
            if line.startswith("@@"):
                hunk_count += 1
            elif line.startswith("+") and not line.startswith("+++"):
                added_tokens.update(_TOKEN_RE.findall(line))

        foreign_signals = []
        this_slice_signals = []
        for tok in sorted(added_tokens):
            this_hit = [d for d, toks in this_docs if tok in toks]
            foreign_hit = [d for d, toks in foreign_docs if tok in toks]
            if this_hit:
                this_slice_signals.append(tok)
            elif foreign_hit:
                foreign_signals.append({"token": tok, "docs": sorted(foreign_hit)})

        foreign_signals = foreign_signals[:25]

        if foreign_signals and norm_slice and this_slice_signals:
            recommendation = "review_mixed"
        elif foreign_signals:
            recommendation = "skip_or_selective"
        else:
            recommendation = "stage"

        files_out.append({
            "path":               path,
            "recommendation":     recommendation,
            "foreign_signals":    foreign_signals,
            "this_slice_signals": this_slice_signals[:20],
            "hunk_count":         hunk_count,
        })

    return {
        "slice":      slice_name,
        "base":       base,
        "confidence": "heuristic",
        "note": ("Lexical lane attribution via .claude/*.md token mentions - "
                 "advisory, verify before staging"),
        "files":      files_out,
    }


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def _parse_flags(args):
    """Extract --expect-branch and --expect-root from args.
    Returns (remaining_args, expect_branch, expect_root)."""
    expect_branch = None
    expect_root   = None
    remaining     = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--expect-branch":
            if i + 1 < len(args):
                i += 1
                expect_branch = args[i]
        elif a.startswith("--expect-branch="):
            expect_branch = a.split("=", 1)[1]
        elif a == "--expect-root":
            if i + 1 < len(args):
                i += 1
                expect_root = args[i]
        elif a.startswith("--expect-root="):
            expect_root = a.split("=", 1)[1]
        else:
            remaining.append(a)
        i += 1
    return remaining, expect_branch, expect_root


CANONICAL_BRANCH = "claude/nifty-mendeleev"
_MANIFEST_NAME = ".deployed_manifest.csv"


def _short(sha, n=12):
    return sha[:n] if sha else sha


def _worktrees():
    """Parse `git worktree list --porcelain` -> [{path, head, branch}]."""
    out, rc = git(["worktree", "list", "--porcelain"])
    if rc != 0:
        return []
    trees, cur = [], {}
    for line in out.splitlines():
        if line.startswith("worktree "):
            if cur:
                trees.append(cur)
            cur = {"path": line[len("worktree "):].strip()}
        elif line.startswith("HEAD "):
            cur["head"] = line[len("HEAD "):].strip()
        elif line.startswith("branch "):
            cur["branch"] = line[len("branch "):].strip().replace("refs/heads/", "")
        elif line.startswith("detached"):
            cur["branch"] = "(detached)"
    if cur:
        trees.append(cur)
    return trees


def _manifest_commit(install_dir: str):
    """Read src_commit (data-row col 'src_commit') from a deployed install."""
    path = Path(install_dir) / _MANIFEST_NAME
    if not path.is_file():
        return None
    try:
        rows = list(csv.reader(path.open(newline="", encoding="utf-8")))
    except Exception:  # noqa: BLE001
        return None
    if len(rows) < 3:
        return None
    header = rows[1]
    if "src_commit" not in header:
        return None
    idx = header.index("src_commit")
    commits = {r[idx].strip() for r in rows[2:] if len(r) > idx and r[idx].strip()}
    if len(commits) == 1:
        return next(iter(commits))
    return "MIXED" if commits else None


def canonical_tip(repo_root: Path, branch: str = CANONICAL_BRANCH, install: str = None):
    """Report the real current viewing tip: the canonical worktree's HEAD +
    dirty state, and (if --install given) whether that deploy is at the tip."""
    result = {"canonical_branch": branch}
    sha, rc = git(["rev-parse", branch])
    if rc != 0:
        result["error"] = f"branch not found: {branch}"
        return result
    result["tip"] = _short(sha)
    result["tip_full"] = sha
    subj, _ = git(["log", "-1", "--format=%s", branch])
    result["tip_subject"] = subj
    # locate the canonical worktree + its dirty state
    for wt in _worktrees():
        if wt.get("branch") == branch:
            result["worktree"] = wt["path"]
            porc, _ = git(["status", "--porcelain"], cwd=wt["path"])
            result["dirty"] = bool(porc.strip())
            break
    if install:
        mc = _manifest_commit(install)
        result["install"] = install
        result["deployed_commit"] = _short(mc) if mc and mc != "MIXED" else mc
        if mc and mc != "MIXED":
            n = min(len(mc), len(sha))
            result["deploy_at_tip"] = mc[:n].lower() == sha[:n].lower()
        else:
            result["deploy_at_tip"] = None
    return result


def lane_status(repo_root: Path, canonical: str = CANONICAL_BRANCH,
                base: str = None):
    """Classify every lane worktree vs the canonical tip + flag deploy-slot
    contention. Verdicts: current | ancestor(behind) | ahead | diverged |
    wrong-base | canonical."""
    tip, rc = git(["rev-parse", canonical])
    if rc != 0:
        return {"error": f"canonical branch not found: {canonical}"}
    lanes = []
    slot_map = {}
    for wt in _worktrees():
        br = wt.get("branch", "(detached)")
        head = wt.get("head", "")
        entry = {"path": wt["path"], "branch": br, "head": _short(head)}
        if br == canonical:
            entry["verdict"] = "canonical"
        elif not head:
            entry["verdict"] = "unknown"
        else:
            # relationship of lane HEAD to canonical tip
            _, anc_rc = git(["merge-base", "--is-ancestor", head, tip])
            _, desc_rc = git(["merge-base", "--is-ancestor", tip, head])
            if head.lower() == tip.lower():
                entry["verdict"] = "current"
            elif anc_rc == 0:
                entry["verdict"] = "ancestor"      # lane behind canonical
                cnt, _ = git(["rev-list", "--count", f"{head}..{tip}"])
                entry["behind"] = cnt
            elif desc_rc == 0:
                entry["verdict"] = "ahead"          # lane ahead of canonical
                cnt, _ = git(["rev-list", "--count", f"{tip}..{head}"])
                entry["ahead"] = cnt
            else:
                entry["verdict"] = "diverged"
            if base:
                _, base_rc = git(["merge-base", "--is-ancestor", base, head])
                if base_rc != 0:
                    entry["verdict"] = "wrong-base"
                    entry["expected_base"] = _short(base)
        # deploy-slot contention: does a sibling install dir mention this lane?
        slot_map.setdefault(br, []).append(wt["path"])
        lanes.append(entry)
    # contention = two worktrees on the same branch name (shared-branch hazard)
    contention = {b: paths for b, paths in slot_map.items() if len(paths) > 1}
    return {
        "canonical_branch": canonical,
        "tip": _short(tip),
        "lane_count": len(lanes),
        "lanes": lanes,
        "shared_branch_contention": contention,
    }


def main():
    raw_args = sys.argv[1:]
    if not raw_args:
        print(
            "Usage: repo_query.py <dirty|harness <name>|preflight|\n"
            "                      env [MC2_NAME|--undocumented|--domain D|--all]|\n"
            "                      binding [N|--conflicts|--namespace NS|--all]>\n"
            "       [--expect-branch BRANCH] [--expect-root PATH]\n"
            "\n"
            "Example full preflight:\n"
            r"  py -3 tools\repo_intel\repo_query.py preflight"
            " --expect-branch claude/nifty-mendeleev"
            r" --expect-root A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev",
            file=sys.stderr,
        )
        sys.exit(1)

    args, expect_branch, expect_root = _parse_flags(raw_args)

    repo_root = find_repo_root()
    if repo_root is None:
        out_json({"error": "Not inside a git repository"})
        sys.exit(1)

    claude_md = find_claude_md(repo_root)

    cmd = args[0].lower() if args else ""

    if cmd == "dirty":
        out_json(get_dirty_state(repo_root, expect_branch=expect_branch,
                                 expect_root=expect_root))

    elif cmd == "harness":
        if len(args) < 2:
            out_json({"error": "harness requires a name: build | deploy | smoke | tier1 | all"})
            sys.exit(1)
        if claude_md is None:
            out_json({"error": "CLAUDE.md not found - cannot parse harnesses"})
            sys.exit(1)
        name = args[1].lower()
        if name == "all":
            out_json(all_harnesses(claude_md))
        else:
            out_json(parse_harness(name, claude_md))

    elif cmd == "preflight":
        if claude_md is None:
            out_json({"error": "CLAUDE.md not found - cannot parse harnesses"})
            sys.exit(1)
        result = preflight(repo_root, claude_md, expect_branch=expect_branch,
                           expect_root=expect_root)
        print(result["summary"])
        out_json(result)

    elif cmd == "slice-preflight":
        rest = args[1:]
        slice_name = None
        base = None
        nifty = "claude/nifty-mendeleev"
        symbols = []
        paths = []
        i = 0
        while i < len(rest):
            a = rest[i]
            if a == "--slice" and i + 1 < len(rest):
                i += 1; slice_name = rest[i]
            elif a == "--base" and i + 1 < len(rest):
                i += 1; base = rest[i]
            elif a == "--nifty" and i + 1 < len(rest):
                i += 1; nifty = rest[i]
            elif a == "--symbols" and i + 1 < len(rest):
                i += 1
                symbols = [s for s in rest[i].replace(",", " ").split() if s]
            elif a == "--paths":
                i += 1
                while i < len(rest) and not rest[i].startswith("--"):
                    paths.append(rest[i]); i += 1
                continue
            i += 1
        result = slice_preflight(repo_root, slice_name=slice_name, symbols=symbols,
                                 paths=paths, base=base, nifty=nifty)
        print(result["summary"], file=sys.stderr)
        out_json(result)
        sys.exit(2 if result["verdict"] == "STOP" else 0)

    elif cmd == "commit-plan":
        rest = args[1:]
        slice_name = ""
        base = "HEAD"
        paths = []
        i = 0
        while i < len(rest):
            a = rest[i]
            if a == "--slice" and i + 1 < len(rest):
                i += 1; slice_name = rest[i]
            elif a == "--base" and i + 1 < len(rest):
                i += 1; base = rest[i]
            elif a == "--paths":
                i += 1
                while i < len(rest) and not rest[i].startswith("--"):
                    paths.append(rest[i]); i += 1
                continue
            i += 1
        out_json(commit_plan(repo_root, slice_name=slice_name,
                             paths=(paths or None), base=base))

    elif cmd == "binding":
        from binding_index import query_binding
        rest      = args[1:]
        binding_n  = None
        namespace  = None
        conflicts  = False
        show_all   = False
        i = 0
        while i < len(rest):
            a = rest[i]
            if a == '--conflicts':
                conflicts = True
            elif a == '--all':
                show_all = True
            elif a.startswith('--namespace'):
                if '=' in a:
                    namespace = a.split('=', 1)[1]
                elif i + 1 < len(rest):
                    i += 1
                    namespace = rest[i]
            elif not a.startswith('--'):
                try:
                    binding_n = int(a)
                except ValueError:
                    out_json({'error': f"Binding number must be an integer, got '{a}'"})
                    sys.exit(1)
            i += 1
        out_json(query_binding(repo_root, binding=binding_n, namespace=namespace,
                               conflicts=conflicts, show_all=show_all))

    elif cmd == "dead-gate-scan":
        from dead_gate_scan import dead_gate_scan
        rest = args[1:]
        tier = "all"
        name = ""
        i = 0
        while i < len(rest):
            a = rest[i]
            if a.startswith("--tier"):
                if "=" in a:
                    tier = a.split("=", 1)[1]
                elif i + 1 < len(rest):
                    i += 1
                    tier = rest[i]
            elif a.startswith("--name"):
                if "=" in a:
                    name = a.split("=", 1)[1]
                elif i + 1 < len(rest):
                    i += 1
                    name = rest[i]
            i += 1
        out_json(dead_gate_scan(repo_root, tier=tier, name=name))

    elif cmd == "env":
        from env_index import query_env
        rest = args[1:]
        name         = None
        domain       = None
        undocumented = False
        show_all     = False
        i = 0
        while i < len(rest):
            a = rest[i]
            if a == "--undocumented":
                undocumented = True
            elif a == "--all":
                show_all = True
            elif a.startswith("--domain"):
                if "=" in a:
                    domain = a.split("=", 1)[1]
                elif i + 1 < len(rest):
                    i += 1
                    domain = rest[i]
            elif not a.startswith("--"):
                name = a
            i += 1
        out_json(query_env(repo_root, name=name, domain=domain,
                           undocumented=undocumented, show_all=show_all))

    elif cmd in ("canonical-tip", "canonical_tip"):
        rest = args[1:]
        branch = CANONICAL_BRANCH
        install = None
        i = 0
        while i < len(rest):
            a = rest[i]
            if a.startswith("--branch"):
                branch = a.split("=", 1)[1] if "=" in a else (rest[i := i + 1])
            elif a.startswith("--install"):
                install = a.split("=", 1)[1] if "=" in a else (rest[i := i + 1])
            i += 1
        out_json(canonical_tip(repo_root, branch=branch, install=install))

    elif cmd in ("lane-status", "lane_status"):
        rest = args[1:]
        canonical = CANONICAL_BRANCH
        base = None
        i = 0
        while i < len(rest):
            a = rest[i]
            if a.startswith("--canonical"):
                canonical = a.split("=", 1)[1] if "=" in a else (rest[i := i + 1])
            elif a.startswith("--base"):
                base = a.split("=", 1)[1] if "=" in a else (rest[i := i + 1])
            i += 1
        out_json(lane_status(repo_root, canonical=canonical, base=base))

    else:
        out_json({"error": f"Unknown command '{cmd}'. Valid: dirty, harness, preflight, slice-preflight, commit-plan, env, binding, dead-gate-scan, canonical-tip, lane-status"})
        sys.exit(1)


if __name__ == "__main__":
    main()
