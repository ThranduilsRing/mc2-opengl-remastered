#!/usr/bin/env python3
"""new_lane.py -- safe lane scaffolding generator for MC2 OpenGL.

Creates a fresh branch + isolated worktree + seed lane note in ONE command,
encoding the discipline that prevents the two recurring incident classes:

  1. stale-ROOT-edit: an agent edits a file in the repo ROOT checkout (an old
     unrelated branch) while the lane's real worktree sits elsewhere. The edit
     silently diverges. (See scripts/check-worktree-context.sh.)
  2. wrong-branch: work lands on a shared branch (e.g. claude/nifty-mendeleev)
     instead of an isolated lane branch.

Usage:
  py -3 scripts/new_lane.py <lane-slug> [--base <branch>] [--plan] [--dry-run]

Examples:
  py -3 scripts/new_lane.py shadow-cache-v2
  py -3 scripts/new_lane.py water-refl-s3 --base claude/nifty-mendeleev --plan
  py -3 scripts/new_lane.py demo-lane-xyz --dry-run

Behavior summary:
  - Validates the slug is kebab-case; derives branch claude/<slug>-1, bumping
    to -2/-3/... if a branch already exists.
  - Pre-flight safety: refuses if the base branch is missing / has no commits;
    warns loudly (echoing the check-worktree-context.sh signature) if invoked
    from a dirty repo ROOT; reports base cleanliness.
  - Creates the worktree at
    .claude/worktrees/<slug>/ on the new branch off base.
  - Seeds a lane note (docs/superpowers/lanes/<slug>.md) with branch/base/path,
    created date, build/deploy/smoke discipline, and empty
    Situation/Plan/Residuals sections.
  - --plan also drops a plan skeleton modeled on the HANDOFF exemplar.
  - --dry-run prints everything it WOULD do and creates nothing.
  - Commits the seed file(s) INTO the new worktree only. Never mutates the
    root tree.

This tool only creates a new worktree + commits seed docs into it. It never
edits engine code, run_smoke.py, or any other worktree.
"""

import argparse
import datetime
import os
import re
import subprocess
import sys
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

# --- constants -------------------------------------------------------------

REPO_ROOT = "A:/Games/mc2-opengl-src"
WORKTREES_DIR = REPO_ROOT + "/.claude/worktrees"
DEFAULT_BASE = "claude/nifty-mendeleev"
LANE_NOTE_RELDIR = "docs/superpowers/lanes"
PLAN_RELDIR = "docs/superpowers/plans"

KEBAB_RE = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")

# Canonical commands lifted from CLAUDE.md so seeded lanes inherit the gospel.
SMOKE_CMD = (
    "py -3 A:\\Games\\mc2-opengl-src\\.claude\\worktrees\\"
    "{slug}\\scripts\\run_smoke.py --tier tier1 --duration 30 --keep-logs"
)
DEPLOY_CMD = "py -3 scripts/deploy_payload.py --target game"


# --- shell helpers ---------------------------------------------------------

def git(args, cwd=REPO_ROOT, check=True):
    """Run a git command, return (rc, stdout, stderr)."""
    proc = subprocess.run(
        ["git", "-C", cwd] + args,
        capture_output=True, text=True,
        **console_child.child_kwargs(redirected=True),
    )
    if check and proc.returncode != 0:
        sys.stderr.write(
            "[new_lane] git {0} failed (rc={1}):\n{2}\n".format(
                " ".join(args), proc.returncode, proc.stderr.strip()
            )
        )
        sys.exit(1)
    return proc.returncode, proc.stdout.strip(), proc.stderr.strip()


def branch_exists(branch):
    rc, _, _ = git(
        ["rev-parse", "--verify", "--quiet", "refs/heads/" + branch],
        check=False,
    )
    return rc == 0


def ref_exists(ref):
    """True if ref resolves to a commit (local branch, remote branch, or any ref)."""
    rc, _, _ = git(["rev-parse", "--verify", "--quiet", ref + "^{commit}"], check=False)
    return rc == 0


# --- validation / derivation ----------------------------------------------

def validate_slug(slug):
    if not KEBAB_RE.match(slug):
        sys.stderr.write(
            "[new_lane] ERROR: lane-slug '{0}' is not kebab-case "
            "(lowercase letters/digits separated by single hyphens, "
            "e.g. 'shadow-cache-v2').\n".format(slug)
        )
        sys.exit(2)


def derive_branch(slug):
    """claude/<slug>-1, bumping -2/-3/... past existing branches."""
    n = 1
    while True:
        candidate = "claude/{0}-{1}".format(slug, n)
        if not branch_exists(candidate):
            return candidate
        n += 1


def worktree_paths():
    """Return set of existing worktree paths (normalized, forward slashes)."""
    _, out, _ = git(["worktree", "list", "--porcelain"])
    paths = set()
    for line in out.splitlines():
        if line.startswith("worktree "):
            p = line[len("worktree "):].strip().replace("\\", "/")
            paths.add(os.path.normpath(p).replace("\\", "/"))
    return paths


# --- pre-flight safety -----------------------------------------------------

def preflight_base(base):
    """Refuse if base branch is missing or has no commits."""
    if not ref_exists(base):
        sys.stderr.write(
            "[new_lane] ERROR: base branch '{0}' does not exist (or resolves "
            "to no commit). Cannot fork a lane off a non-existent base.\n".format(base)
        )
        sys.exit(3)
    # has at least one commit?
    rc, out, _ = git(["rev-list", "-n", "1", base], check=False)
    if rc != 0 or not out:
        sys.stderr.write(
            "[new_lane] ERROR: base branch '{0}' has no commits.\n".format(base)
        )
        sys.exit(3)


def report_base_cleanliness(base):
    """Report (do not require) base working-tree cleanliness against ROOT."""
    # We can only inspect the tree of a branch that is checked out somewhere.
    # Report the diff stat of base vs its own HEAD where checked out, else skip.
    rc, _, _ = git(["rev-parse", "--quiet", "--verify", base], check=False)
    _, sha, _ = git(["rev-parse", "--short", base], check=False)
    print("[new_lane] base '{0}' at {1} (has commits: OK)".format(base, sha))


def warn_if_dirty_root():
    """Echo the check-worktree-context.sh danger signature when invoked from a
    dirty repo ROOT not on a lane branch. Advisory only -- never blocks."""
    _, toplevel, _ = git(["rev-parse", "--show-toplevel"])
    toplevel = toplevel.replace("\\", "/")
    _, main_root, _ = git(["worktree", "list", "--porcelain"])
    main_root_path = ""
    for line in main_root.splitlines():
        if line.startswith("worktree "):
            main_root_path = line[len("worktree "):].strip().replace("\\", "/")
            break
    cwd = os.getcwd().replace("\\", "/")
    at_root = os.path.normpath(toplevel) == os.path.normpath(main_root_path)

    _, branch, _ = git(["rev-parse", "--abbrev-ref", "HEAD"])
    _, status, _ = git(["status", "--porcelain"])
    dirty = len([ln for ln in status.splitlines() if ln.strip()])

    on_lane_branch = branch.startswith("claude/nifty-mendeleev")

    if at_root and dirty > 0 and not on_lane_branch:
        sys.stderr.write(
            "[new_lane] WARNING (check-worktree-context signature): you are at "
            "the repo ROOT on branch '{0}' with a DIRTY tree ({1} files). "
            "Root edits are LIKELY WRONG -- lane work belongs in its worktree. "
            "Run `sh scripts/check-worktree-context.sh` before editing.\n".format(
                branch, dirty
            )
        )
    else:
        print(
            "[new_lane] worktree-context OK (cwd '{0}', branch '{1}', "
            "{2} dirty files at this checkout).".format(cwd, branch, dirty)
        )


# --- seed content ----------------------------------------------------------

def lane_note_body(slug, branch, base, worktree_path, created):
    return """# Lane: {slug}

- **Lane name:** {slug}
- **Branch:** `{branch}`
- **Base:** `{base}`
- **Worktree:** `{worktree}`
- **Created:** {created}

> Auto-generated by `scripts/new_lane.py`. Work ONLY in this worktree on the
> branch above. Before editing anything, run:
>
> ```sh
> sh scripts/check-worktree-context.sh
> ```
>
> If it reports the stale-ROOT-edit danger signature, you are in the wrong
> checkout -- `cd` into this worktree first.

## Build / deploy / smoke discipline

- **Build:** `--config RelWithDebInfo` always. A C++ change needs a full
  relink (delete `build64/RelWithDebInfo/mc2.exe` + changed `.obj`, or
  `--clean-first`). A shader-only change hot-reloads -- just redeploy the
  `.frag` and grep the engine log for `0(N): error` (hot-reload fails SILENTLY
  on a bad compile).
- **Deploy (game target):**

  ```sh
  {deploy_cmd}
  ```

  Deploy only to the intended target. Verify the deployed `mc2.exe` mtime is
  newer than your fix commit -- a stale deploy re-hits already-fixed bugs.
- **Smoke gate (canonical, copy verbatim):**

  ```powershell
  {smoke_cmd}
  ```

  ALWAYS `--keep-logs`. NEVER `--kill-existing` (it taskkills concurrent
  mc2.exe and produces false `crash_silent`). NEVER `--with-menu-canary`.
  NEVER `--duration` > 30. NEVER run concurrent with another smoke / mc2.exe
  trace. Exit `0` = pass; nonzero -> inspect
  `tests/smoke/artifacts/<timestamp>/`.

## Situation

<!-- One self-contained paragraph: what this lane is, why it exists. -->

## Plan

<!-- Ordered steps + gates. -->

## Residuals

<!-- Known-open items, deferred work, pre-existing issues NOT in lane scope. -->
""".format(
        slug=slug, branch=branch, base=base, worktree=worktree_path,
        created=created, deploy_cmd=DEPLOY_CMD,
        smoke_cmd=SMOKE_CMD.format(slug=slug),
    )


def plan_skeleton_body(slug, branch, base, worktree_path, created):
    return """# {slug} -- Implementation Plan

**Date:** {created}  **Branch:** `{branch}`  **Base:** `{base}`
**Worktree:** `{worktree}`

> Auto-generated plan skeleton (modeled on the water-v2-S3 HANDOFF exemplar).
> Fill each section before writing code. Keep it self-contained -- a fresh
> session must be able to pick this up cold.

## 0. One-paragraph situation

<!-- Self-contained. What is the want, what state are we picking up from. -->

## 1. Worktree, branch, deploy (ISOLATED -- do not deviate)

- **Worktree:** `{worktree}`
- **Branch:** `{branch}` (forked from `{base}`). Keep isolated; integration
  is the user's separate deliberate step.
- **Deploy:** `{deploy_cmd}` (game target). Never clobber another lane's
  deploy dir.

## 2. Build / deploy / smoke discipline

- Build `--config RelWithDebInfo`; C++ change -> full relink; shader-only ->
  hot-reload (grep log for `0(N): error`).
- Smoke (verbatim):

  ```powershell
  {smoke_cmd}
  ```

  `--keep-logs` always; never `--kill-existing`; gate by markers, not just
  exit code.

## 3. Steps + gates

| # | Step | Gate (how we know it's done) |
|---|------|------------------------------|
| 1 |      | tier1 5/5 PASS               |
| 2 |      |                              |

## 4. What NOT to touch (load-bearing)

<!-- Enumerate files/invariants this lane must NOT regress. Cite file:symbol
     (grep before trusting line numbers). -->

## 5. Residual ledger (pre-existing, NOT this lane's scope)

<!-- Pre-existing issues to explicitly leave alone. -->
""".format(
        slug=slug, branch=branch, base=base, worktree=worktree_path,
        created=created, deploy_cmd=DEPLOY_CMD,
        smoke_cmd=SMOKE_CMD.format(slug=slug),
    )


# --- main flow -------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Scaffold a safe lane: branch + isolated worktree + seed note.",
    )
    ap.add_argument("slug", help="kebab-case lane slug, e.g. shadow-cache-v2")
    ap.add_argument("--base", default=DEFAULT_BASE,
                    help="base branch to fork from (default: %(default)s)")
    ap.add_argument("--plan", action="store_true",
                    help="also drop a plan skeleton")
    ap.add_argument("--dry-run", action="store_true",
                    help="print what would happen; create nothing")
    args = ap.parse_args()

    slug = args.slug
    validate_slug(slug)

    # Pre-flight safety.
    preflight_base(args.base)
    report_base_cleanliness(args.base)
    warn_if_dirty_root()

    branch = derive_branch(slug)
    worktree_path = "{0}/{1}".format(WORKTREES_DIR, slug)
    worktree_path_norm = os.path.normpath(worktree_path).replace("\\", "/")

    # Idempotency: detect a pre-existing worktree at the target path.
    if worktree_path_norm in worktree_paths():
        sys.stderr.write(
            "[new_lane] ERROR: a worktree already exists at '{0}'. Refusing to "
            "clobber. Remove it first (git worktree remove ...) or choose a "
            "different slug.\n".format(worktree_path_norm)
        )
        sys.exit(4)
    if os.path.exists(worktree_path):
        sys.stderr.write(
            "[new_lane] ERROR: path '{0}' already exists on disk. Refusing to "
            "clobber.\n".format(worktree_path)
        )
        sys.exit(4)

    created = datetime.date.today().isoformat()

    lane_note_dir = "{0}/{1}".format(worktree_path, LANE_NOTE_RELDIR)
    lane_note_path = "{0}/{1}.md".format(lane_note_dir, slug)
    plan_dir = "{0}/{1}".format(worktree_path, PLAN_RELDIR)
    plan_path = "{0}/{1}-plan.md".format(plan_dir, slug)

    # --- report plan -------------------------------------------------------
    print("")
    print("[new_lane] === lane scaffold plan ===")
    print("  slug:          {0}".format(slug))
    print("  branch:        {0}  (off {1})".format(branch, args.base))
    print("  worktree:      {0}".format(worktree_path_norm))
    print("  lane note:     {0}".format(
        lane_note_path.replace(WORKTREES_DIR + "/" + slug + "/", "")))
    if args.plan:
        print("  plan skeleton: {0}".format(
            plan_path.replace(WORKTREES_DIR + "/" + slug + "/", "")))
    print("  git op:        git worktree add -b {0} {1} {2}".format(
        branch, worktree_path_norm, args.base))
    print("")

    if args.dry_run:
        print("[new_lane] --dry-run: creating NOTHING. Above is what would happen.")
        print_next_steps(slug, branch, worktree_path_norm, args.plan)
        return

    # --- create worktree ---------------------------------------------------
    git(["worktree", "add", "-b", branch, worktree_path, args.base])
    print("[new_lane] created worktree + branch.")

    # --- seed files --------------------------------------------------------
    os.makedirs(lane_note_dir, exist_ok=True)
    with open(lane_note_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(lane_note_body(slug, branch, args.base, worktree_path_norm, created))
    seeded = [lane_note_path]

    if args.plan:
        os.makedirs(plan_dir, exist_ok=True)
        with open(plan_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(plan_skeleton_body(slug, branch, args.base, worktree_path_norm, created))
        seeded.append(plan_path)

    # --- commit seed files INTO the new worktree only ----------------------
    rel = [os.path.relpath(p, worktree_path).replace("\\", "/") for p in seeded]
    # Force-add: docs/superpowers/* is gitignored by default (scratch noise),
    # but lane notes/plans are intentional tracked artifacts -- same as the
    # already-committed specs under docs/superpowers/specs/.
    git(["add", "-f"] + rel, cwd=worktree_path)
    msg = "lane: seed scaffold for {0}\n\nnew rock. git tool make worktree {1}, branch {2}, " \
          "lane note. caveman put smoke+deploy rule in note so future agent no break.\n\n" \
          "Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>".format(slug, slug, branch)
    git(["commit", "-m", msg], cwd=worktree_path)
    print("[new_lane] seeded + committed {0} file(s) into the worktree.".format(len(seeded)))

    print_next_steps(slug, branch, worktree_path_norm, args.plan)


def print_next_steps(slug, branch, worktree_path, has_plan):
    print("")
    print("[new_lane] === next steps ===")
    print("  cd {0}".format(worktree_path))
    print("  # branch: {0}".format(branch))
    print("  # build:  use the MC2 build skill / cmake --config RelWithDebInfo")
    print("  # deploy: {0}".format(DEPLOY_CMD))
    print("  # smoke:  {0}".format(SMOKE_CMD.format(slug=slug)))
    print("  # ALWAYS run `sh scripts/check-worktree-context.sh` before editing.")
    note = "docs/superpowers/lanes/{0}.md".format(slug)
    print("  lane note: {0}".format(note))
    if has_plan:
        print("  plan:      docs/superpowers/plans/{0}-plan.md".format(slug))
    print("")


if __name__ == "__main__":
    main()
