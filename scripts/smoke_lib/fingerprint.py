# scripts/smoke_lib/fingerprint.py
"""Deploy-fingerprint parsing/compare for run_smoke.

The exe prints (code/mechcmd2.cpp InitializeGameEngine, build-time stamped by
cmake/GenerateBuildFingerprint.cmake):

  [BUILD_FINGERPRINT v1] sha=<12-hex> dirty=<0|1> branch=<name> built=<iso> src=<worktree>

run_smoke defaults to worktree HEAD. An explicit commit reference requires a
clean fingerprint; an executable reference requires SHA-256 byte identity.
DEFAULT = HARD FAIL on mismatch/absence (DEPLOY-FINGERPRINT-DEFAULT-ON-1): a
smoke that ran a stale or wrong-lane exe must never print PASS. Opt OUT with
MC2_SMOKE_REQUIRE_FINGERPRINT=0 for the narrow case of deliberately smoking an
external prebuilt exe.

No game launch — unit-tested in
scripts/tests/test_fingerprint.py.
"""
from __future__ import annotations

import hashlib
import re
import subprocess
from pathlib import Path
from typing import Callable, Optional, Sequence
from . import nofocus  # noqa: E402  (OWNER RULE: no window for any child process)

FINGERPRINT_RE = re.compile(
    r"\[BUILD_FINGERPRINT v1\] "
    r"sha=(?P<sha>[0-9a-fA-F]+) "
    r"dirty=(?P<dirty>[01]) "
    r"branch=(?P<branch>\S+) "
    r"built=(?P<built>\S+) "
    r"src=(?P<src>\S+)")


def parse_fingerprint(log_text: str) -> Optional[dict]:
    """Return {sha, dirty, branch, built, src} from the first banner line, or None."""
    m = FINGERPRINT_RE.search(log_text or "")
    if not m:
        return None
    d = m.groupdict()
    d["sha"] = d["sha"].lower()
    d["dirty"] = int(d["dirty"])
    return d


# Paths whose change alters the built exe: one shared list read by this module
# and by cmake/GenerateBuildFingerprint.cmake. Directory entries end with "/".
BUILD_INPUTS_FILE = Path(__file__).resolve().parents[1] / "build_inputs.txt"


def load_build_inputs(path=BUILD_INPUTS_FILE) -> tuple[tuple[str, ...], tuple[str, ...]]:
    """Return (directory_prefixes, exact_files) from the shared list."""
    prefixes: list[str] = []
    files: list[str] = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        (prefixes if line.endswith("/") else files).append(line)
    return tuple(prefixes), tuple(files)


BUILD_INPUT_PREFIXES, BUILD_INPUT_FILES = load_build_inputs()


def is_build_input(path: str) -> bool:
    p = path.replace("\\", "/")
    return p in BUILD_INPUT_FILES or p.startswith(BUILD_INPUT_PREFIXES)


def build_inputs_changed(paths: Sequence[str]) -> list[str]:
    return [p for p in paths if is_build_input(p)]


def _git(repo_root, *args) -> Optional[str]:
    try:
        return subprocess.check_output(
            ["git", "-C", str(repo_root), *args], text=True,
            stderr=subprocess.DEVNULL, timeout=30,
            **nofocus.hidden_kwargs()).strip()
    except Exception:
        return None


def sha256_file(path) -> str:
    """Hash executable bytes; embedded commits alone do not identify a build."""
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def resolve_reference(repo_root, *, ref_commit=None, ref_exe=None,
                      git: Callable = _git) -> dict:
    """Pin the selected reference before any mission; invalid references abort."""
    if ref_commit is not None and ref_exe is not None:
        raise ValueError("--ref-commit and --ref-exe are mutually exclusive")
    if ref_exe is not None:
        path = Path(ref_exe).resolve()
        try:
            digest = sha256_file(path)
        except OSError as exc:
            raise ValueError(f"cannot read --ref-exe {path}: {exc}") from exc
        return {"mode": "exe", "path": str(path), "sha256": digest}
    if ref_commit is not None and not re.fullmatch(r"[0-9a-fA-F]{7,40}", ref_commit):
        raise ValueError("--ref-commit requires a git commit SHA (7-40 hex digits)")
    target = ref_commit if ref_commit is not None else "HEAD"
    sha = git(repo_root, "rev-parse", "--verify", f"{target}^{{commit}}")
    if not sha or not re.fullmatch(r"[0-9a-fA-F]{40}", sha):
        raise ValueError(f"cannot resolve fingerprint commit reference {target}")
    return {"mode": "commit" if ref_commit is not None else "head",
            "requested": target, "sha": sha.lower()}


def describe_reference(reference: dict) -> str:
    if reference["mode"] == "exe":
        return f"--ref-exe {reference['path']} sha256={reference['sha256']} (byte identity)"
    if reference["mode"] == "commit":
        return f"--ref-commit {reference['requested']} resolved={reference['sha']} (clean commit)"
    return f"worktree HEAD {reference['sha']}"


def check_ancestor_fingerprint(fp: dict, expected_sha: str, repo_root,
                               git: Callable = _git):
    """Accept an exe built at an ancestor of the reference when nothing since touched a
    build input. Returns (lines, ok)."""
    lines: list[str] = []
    if fp.get("dirty"):
        lines.append("[DEPLOY_FINGERPRINT] MISMATCH: exe sha="
                     f"{fp['sha']} differs from the reference and was built from a dirty tree")
        return lines, False
    sha = fp["sha"]
    if git(repo_root, "merge-base", "--is-ancestor", sha, expected_sha) is None:
        lines.append(f"[DEPLOY_FINGERPRINT] MISMATCH: exe sha={sha} "
                     f"expected={expected_sha[:12]} (not an ancestor of reference)")
        return lines, False
    diff = git(repo_root, "diff", "--name-only", f"{sha}..{expected_sha}")
    if diff is None:
        lines.append(f"[DEPLOY_FINGERPRINT] MISMATCH: exe sha={sha} "
                     f"expected={expected_sha[:12]} (git diff failed)")
        return lines, False
    bad = build_inputs_changed([l for l in diff.splitlines() if l])
    if bad:
        shown = ", ".join(bad[:3]) + (f" (+{len(bad) - 3} more)" if len(bad) > 3 else "")
        lines.append(f"[DEPLOY_FINGERPRINT] MISMATCH: exe sha={sha} expected="
                     f"{expected_sha[:12]}; build inputs changed since: {shown}")
        return lines, False
    count = git(repo_root, "rev-list", "--count", f"{sha}..{expected_sha}") or "?"
    lines.append(f"[DEPLOY_FINGERPRINT] OK (ancestor {sha}, {count} doc-only "
                 "commits since)")
    return lines, True


def check_fingerprint(fp: Optional[dict], expected_sha: Optional[str],
                      repo_root=None, git: Callable = _git, *,
                      reference: Optional[dict] = None, exe_path=None):
    """Compare a parsed fingerprint against HEAD or the pinned CLI reference.

    Returns (lines, hard_fail). hard_fail is enforced by the caller BY DEFAULT
    (run_smoke clears its PASS verdict); MC2_SMOKE_REQUIRE_FINGERPRINT=0
    downgrades it to advisory. In default HEAD mode with repo_root, a clean
    ancestor passes when no build input changed in between. Explicit commit
    references require that exact clean commit; exe references compare bytes.
    """
    lines: list[str] = []
    try:  # run_player_path/run_smoke put scripts/ (not the repo root) on sys.path and import this as `smoke_lib.fingerprint`
        from scripts.manifest_schema import shader_dirty
    except ImportError:
        from manifest_schema import shader_dirty
    shaders = shader_dirty(exe_path)
    if shaders:
        lines.append(f"[DEPLOY_FINGERPRINT] shader-dirty shaders={len(shaders['shaders'])}; binary stamp covers exe only")
        if reference is not None and reference["mode"] == "commit":
            return lines + ["[DEPLOY_FINGERPRINT] MISMATCH: shader-dirty install cannot match a committed build"], True
    if fp is None:
        lines.append("[DEPLOY_FINGERPRINT] fingerprint absent (pre-fingerprint exe)")
        return lines, True
    if reference is not None and reference["mode"] == "exe":
        try:
            actual = sha256_file(exe_path)
        except (OSError, TypeError) as exc:
            return [f"[DEPLOY_FINGERPRINT] MISMATCH: cannot hash deployed exe: {exc}"], True
        expected = reference["sha256"]
        ok = actual == expected
        return lines + [f"[DEPLOY_FINGERPRINT] {'OK' if ok else 'MISMATCH'}: "
                f"exe sha256={actual} expected={expected} against "
                f"{describe_reference(reference)}"], not ok
    if reference is not None:
        expected_sha = reference["sha"]
        if reference["mode"] == "commit" and fp.get("dirty"):
            return ["[DEPLOY_FINGERPRINT] MISMATCH: dirty exe cannot match "
                    f"{describe_reference(reference)}"], True
    if fp.get("dirty"):
        lines.append("[DEPLOY_FINGERPRINT] WARNING: exe BUILT-FROM-DIRTY-TREE "
                     f"(sha={fp['sha']} branch={fp.get('branch')})")
    if not expected_sha:
        lines.append("[DEPLOY_FINGERPRINT] expected sha unavailable (git rev-parse failed); "
                     f"exe sha={fp['sha']}")
        return lines, True
    exp = expected_sha.lower()
    n = min(len(fp["sha"]), len(exp))
    if n >= 7 and fp["sha"][:n] == exp[:n]:
        target = describe_reference(reference) if reference else "worktree HEAD"
        lines.append(f"[DEPLOY_FINGERPRINT] OK: exe sha={fp['sha']} matches {target}")
        return lines, False
    if repo_root is not None and (reference is None or reference["mode"] == "head"):
        anc_lines, ok = check_ancestor_fingerprint(fp, exp, repo_root, git)
        if ok:
            return lines + anc_lines, False
        return lines + anc_lines, True
    lines.append(f"[DEPLOY_FINGERPRINT] MISMATCH: exe sha={fp['sha']} expected={exp[:12]}")
    return lines, True
