#!/usr/bin/env python3
"""check-beauty-sidecars.py — BEAUTY-SIDECAR-INSTALL-GATE-1.

Root cause this gate exists for (2026-07-30, SHORELINE-SIDECAR-INSTALL-1):
the terrain-beautify sidecars the engine loads from
`data/missions/<stem>.beauty/` are COOKED offline into `tests/terrain/beautify/`
and then INSTALLED into a deploy tree by a MANUAL, undocumented copy step.
`scripts/deploy_payload.py` deliberately never walks `data/missions/*.beauty/`
(see its post-deploy hygiene comment), so nothing anywhere could notice a
sidecar that was cooked, committed, and then never installed. The failure is
100% silent at runtime: the loaders `fopen()`, miss, log one diagnostic line and
carry on. `MC2_WATER_SHORE_MASK` in particular is DEFAULT-ON and degrades to a
pure no-op when `shoreline_mask.png` is absent.

`scripts/check-deploy-hygiene.py` is the MIRROR of this gate: it fails on cook
byproducts that LEAKED INTO a deploy tree. This one fails on runtime sidecars
that never REACHED one. Neither subsumes the other.

What this checks
----------------
1. RUNTIME SET (derived from engine source, never hand-listed): every
   `"data/missions/%s.beauty/<file>"` string literal in the engine loaders.
   Comment lines are excluded — a sidecar name that appears only in a comment
   does NOT count as a loader, and the self-test proves it.
2. SIBLING SET: sidecars whose path is built by overwriting the basename of an
   already-resolved `.beauty` path (visual_height_mips.r32, visual_damp.r32)
   cannot be found by pattern 1. They are declared in SIBLING_SIDECARS below and
   the checker VERIFIES each declared name still appears as a non-comment string
   literal in the engine source — so a rename or deletion fails this gate
   instead of silently shrinking it.
3. INSTALL COVERAGE: for every mission with a cooked runtime sidecar under
   `tests/terrain/beautify/`, that file must exist in
   `<target>/data/missions/<mission>.beauty/`. Missing -> FAIL.
4. CONTENT DRIFT: cooked and installed copies that differ byte-for-byte are
   reported. WARN by default (a deploy tree legitimately carries a newer
   re-cook that was never committed back); FAIL under --strict.
5. INVENTORY: `docs/beauty-sidecar-inventory.txt` records the cooked corpus and
   the runtime sidecar set. Stale -> FAIL, regenerate with --write. newline="\\n".

Usage
-----
  py -3 scripts/check-beauty-sidecars.py                 # verify (default lane)
  py -3 scripts/check-beauty-sidecars.py --target DIR    # verify one deploy tree
  py -3 scripts/check-beauty-sidecars.py --all-lanes     # verify every known lane
  py -3 scripts/check-beauty-sidecars.py --write         # regenerate inventory
  py -3 scripts/check-beauty-sidecars.py --install       # copy missing -> target
  py -3 scripts/check-beauty-sidecars.py --selftest      # injected-regression test
  py -3 scripts/check-beauty-sidecars.py --json

Exit codes: 0 pass, 2 fail, 1 usage/IO error.

Deliberate non-goal: this gate does NOT require every mission to have a cooked
sidecar. Cooking is an authoring decision. It only requires that what HAS been
cooked and committed actually reaches the tree the engine reads.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Files that resolve a `<stem>.beauty/` sidecar path. Found by:
#   grep -rn "\.beauty" --include=*.cpp --include=*.h .
# mclib/terrain.cpp is the GAME loader. editor/BeautySidecarPreview.cpp is the
# EDITOR loader and belongs here for a non-obvious reason: it tries
# `tests/terrain/beautify/<stem>.beauty/...` as a FALLBACK, so an uninstalled
# cook still resolves in a dev worktree and the gap stays invisible -- but
# "Mission Editor.exe" ships in the release, where no repo dir exists and the
# fallback dies. Same silent-miss bug class, one step further from view.
ENGINE_SOURCES = [
    "mclib/terrain.cpp",
    "editor/BeautySidecarPreview.cpp",
]

# Sidecars resolved by overwriting the basename of an already-built `.beauty`
# path (see module docstring #2). name -> engine file that must contain it.
SIBLING_SIDECARS = {
    "visual_height_mips.r32": "mclib/terrain.cpp",
    "visual_damp.r32": "mclib/terrain.cpp",
}

# Companion metadata files: required only when their principal is cooked.
COMPANIONS = {
    "shoreline_mask.png": ["shoreline_mask.bounds.txt"],
    "overlay_v2.png": ["overlay_v2.bounds.txt"],
}

# Where cooks land (tools/terrain_beautify/*.py --out default).
COOK_ROOT = "tests/terrain/beautify"

INVENTORY = "docs/beauty-sidecar-inventory.txt"

# Deploy lanes this gate knows about. Kept local and commented rather than
# imported from check-deploy-target.py: that module's GOOD_DIRS is a
# *smoke-selection* list with different semantics (it excludes user-gated
# release trees on purpose), and silently inheriting it would make this gate's
# coverage move whenever that list is retuned for unrelated reasons.
LANES = [
    # (label, path, enforced)
    # Canonical dev lane -- the tree scripts/build_release.sh reads by default,
    # so a gap here propagates straight into a shipped release.
    ("0.4", r"A:/Games/mc2-opengl/mc2-win64-v0.4", True),
    # Lease-only / release lanes: reported, not enforced. Deploying into
    # releases/ is USER-GATED (see scripts/check-deploy-lane-parity.py), so this
    # gate must not demand writes there.
    ("0.4c", r"A:/Games/mc2-opengl/mc2-win64-v0.4c", False),
    ("0.4d-rc1", r"A:/Games/mc2-opengl/releases/mc2-win64-v0.4d-rc1", False),
    ("0.5.1", r"A:/Games/mc2-opengl/releases/mc2-win64-v0.5.1", False),
]

MISSION_RE = re.compile(r"^[A-Za-z0-9_]+$")
LITERAL_RE = re.compile(r'"data/missions/%s\.beauty/([^"]+)"')


# --------------------------------------------------------------------------
# Engine-source extraction
# --------------------------------------------------------------------------
def _strip_comments(text: str) -> str:
    """Blank out // and /* */ comments, preserving offsets and newlines.

    String literals are respected so a '//' inside a path literal survives.
    """
    out = []
    i, n = 0, len(text)
    in_str = in_line_c = in_blk_c = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_line_c:
            if c == "\n":
                in_line_c = False
                out.append(c)
            else:
                out.append(" ")
            i += 1
        elif in_blk_c:
            if c == "*" and nxt == "/":
                in_blk_c = False
                out.append("  ")
                i += 2
            else:
                out.append("\n" if c == "\n" else " ")
                i += 1
        elif in_str:
            out.append(c)
            if c == "\\" and nxt:
                out.append(nxt)
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
        else:
            if c == "/" and nxt == "/":
                in_line_c = True
                out.append("  ")
                i += 2
            elif c == "/" and nxt == "*":
                in_blk_c = True
                out.append("  ")
                i += 2
            else:
                if c == '"':
                    in_str = True
                out.append(c)
                i += 1
    return "".join(out)


def runtime_sidecars(repo: str, sources=None, siblings=None):
    """Return (names, errors). Comment-only mentions are excluded by design."""
    sources = ENGINE_SOURCES if sources is None else sources
    siblings = SIBLING_SIDECARS if siblings is None else siblings
    names, errors = set(), []
    code_by_file = {}
    for rel in sources:
        path = os.path.join(repo, rel)
        if not os.path.isfile(path):
            errors.append(f"engine source missing: {rel}")
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            code = _strip_comments(f.read())
        code_by_file[rel] = code
        for m in LITERAL_RE.finditer(code):
            names.add(m.group(1))
    for name, rel in siblings.items():
        code = code_by_file.get(rel)
        if code is None:
            errors.append(f"sibling sidecar '{name}' declares unknown source {rel}")
            continue
        if f'"{name}"' not in code:
            errors.append(
                f"declared sibling sidecar '{name}' has NO non-comment string "
                f"literal in {rel} -- its loader was renamed or deleted. Update "
                f"SIBLING_SIDECARS in {os.path.basename(__file__)}.")
            continue
        names.add(name)
    return names, errors


# --------------------------------------------------------------------------
# Cooked corpus + install coverage
# --------------------------------------------------------------------------
def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cooked_corpus(repo: str, names: set, cook_root: str = COOK_ROOT,
                  tracked_only: bool = True):
    """mission -> {filename: abspath}, from tests/terrain/beautify/.

    Only git-TRACKED cooks count by default: an untracked bake in a scratch
    worktree is work in progress, not a shipping obligation.
    """
    root = os.path.join(repo, cook_root)
    corpus: dict[str, dict[str, str]] = {}
    if not os.path.isdir(root):
        return corpus
    tracked = None
    if tracked_only:
        try:
            out = subprocess.run(["git", "-C", repo, "ls-files", cook_root],
                                 capture_output=True, text=True, timeout=60)
            if out.returncode == 0:
                tracked = {line.strip() for line in out.stdout.splitlines()
                           if line.strip()}
        except Exception:  # noqa: BLE001 -- git absent: fall back to disk
            tracked = None
    wanted = set(names)
    for comps in COMPANIONS.values():
        wanted.update(comps)
    for entry in sorted(os.listdir(root)):
        d = os.path.join(root, entry)
        if not os.path.isdir(d):
            continue
        mission = entry[:-len(".beauty")] if entry.endswith(".beauty") else entry
        if not MISSION_RE.match(mission):
            continue
        for fn in sorted(os.listdir(d)):
            if fn not in wanted:
                continue
            fp = os.path.join(d, fn)
            if not os.path.isfile(fp):
                continue
            if tracked is not None:
                rel = os.path.relpath(fp, repo).replace(os.sep, "/")
                if rel not in tracked:
                    continue
            corpus.setdefault(mission, {})[fn] = fp
    # A companion is only an obligation when its principal is cooked.
    for mission, files in corpus.items():
        for principal, comps in COMPANIONS.items():
            if principal not in files:
                for c in comps:
                    files.pop(c, None)
    return corpus


def audit(corpus, target: str):
    """-> (missing, drift, ok) lists of (mission, filename, detail)."""
    missing, drift, ok = [], [], []
    for mission in sorted(corpus):
        for fn in sorted(corpus[mission]):
            src = corpus[mission][fn]
            dst = os.path.join(target, "data", "missions",
                               mission + ".beauty", fn)
            if not os.path.isfile(dst):
                missing.append((mission, fn, dst))
            elif sha256(src) != sha256(dst):
                drift.append((mission, fn, dst))
            else:
                ok.append((mission, fn, dst))
    return missing, drift, ok


# --------------------------------------------------------------------------
# Inventory artifact
# --------------------------------------------------------------------------
def render_inventory(names: set, corpus) -> str:
    lines = [
        "# beauty-sidecar inventory",
        "#",
        "# Generated by `py -3 scripts/check-beauty-sidecars.py --write`.",
        "# Do not edit by hand.",
        "#",
        "# runtime: sidecar basenames the engine actually fopen()s from",
        "#          data/missions/<stem>.beauty/ (extracted from engine source,",
        "#          comments excluded).",
        "# cooked:  git-tracked cooks under tests/terrain/beautify/ that MUST be",
        "#          installed into every enforced deploy lane.",
        "",
        "[runtime]",
    ]
    lines.extend(sorted(names))
    lines.append("")
    lines.append("[cooked]")
    for mission in sorted(corpus):
        for fn in sorted(corpus[mission]):
            lines.append(f"{mission}/{fn}")
    lines.append("")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# Self-test: inject the regression this gate exists to catch
# --------------------------------------------------------------------------
def selftest() -> int:
    failures = []

    def check(label, cond, detail=""):
        if cond:
            print(f"  ok   {label}")
        else:
            failures.append(label)
            print(f"  FAIL {label} {detail}")

    with tempfile.TemporaryDirectory() as tmp:
        repo = os.path.join(tmp, "repo")
        src_dir = os.path.join(repo, "mclib")
        os.makedirs(src_dir)
        # Synthetic engine source. `ghost_sidecar.png` appears ONLY inside
        # comments (both // and /* */ forms) and must NOT be extracted --
        # this is the trap that lets a rule pass while satisfied by a comment.
        engine = (
            'void load() {\n'
            '  // legacy: we used to read\n'
            '  // "data/missions/%s.beauty/ghost_sidecar.png" here\n'
            '  /* also "data/missions/%s.beauty/ghost_block.png" (removed) */\n'
            '  snprintf(p, n, "data/missions/%s.beauty/shoreline_mask.png", stem);\n'
            '  snprintf(p, n, "data/missions/%s.beauty/control_map.png", stem);\n'
            '  snprintf(q + d, n - d, "visual_damp.r32");\n'
            '}\n'
        )
        with open(os.path.join(src_dir, "terrain.cpp"), "w",
                  encoding="utf-8", newline="\n") as f:
            f.write(engine)

        srcs = ["mclib/terrain.cpp"]
        sibs = {"visual_damp.r32": "mclib/terrain.cpp"}
        names, errs = runtime_sidecars(repo, srcs, sibs)
        check("extraction: no errors", not errs, str(errs))
        check("extraction: finds shoreline_mask.png",
              "shoreline_mask.png" in names)
        check("extraction: finds control_map.png", "control_map.png" in names)
        check("extraction: finds declared sibling visual_damp.r32",
              "visual_damp.r32" in names)
        check("COMMENT TRAP: // -only ghost_sidecar.png NOT extracted",
              "ghost_sidecar.png" not in names, sorted(names))
        check("COMMENT TRAP: /* */-only ghost_block.png NOT extracted",
              "ghost_block.png" not in names, sorted(names))

        # A declared sibling with no literal anywhere must ERROR, not vanish.
        _, errs2 = runtime_sidecars(repo, srcs,
                                    {"vanished_sidecar.r32": "mclib/terrain.cpp"})
        check("sibling contract: undeclared-in-source sibling errors",
              any("vanished_sidecar.r32" in e for e in errs2), str(errs2))

        # SIBLING COMMENT TRAP: the sibling contract must NOT be satisfied by a
        # QUOTED occurrence that lives inside a comment. This is the precise
        # failure mode where a rule "passes" because an identifier appears in
        # prose: the loader is gone, the comment describing it remains.
        ghost_src = os.path.join(tmp, "ghostrepo", "mclib")
        os.makedirs(ghost_src)
        with open(os.path.join(ghost_src, "terrain.cpp"), "w",
                  encoding="utf-8", newline="\n") as f:
            f.write('void load() {\n'
                    '  // used to do snprintf(q, n, "commented_sibling.r32");\n'
                    '  /* and here too: "commented_sibling.r32" */\n'
                    '  snprintf(p, n, "data/missions/%s.beauty/real.png", stem);\n'
                    '}\n')
        ghost_repo = os.path.join(tmp, "ghostrepo")
        gnames, gerrs = runtime_sidecars(
            ghost_repo, srcs, {"commented_sibling.r32": "mclib/terrain.cpp"})
        check("SIBLING COMMENT TRAP: quoted-in-comment sibling ERRORS",
              any("commented_sibling.r32" in e for e in gerrs), str(gerrs))
        check("SIBLING COMMENT TRAP: quoted-in-comment sibling not in set",
              "commented_sibling.r32" not in gnames, sorted(gnames))

        # Cooked corpus (tracked_only=False: no git in the synthetic repo).
        cook = os.path.join(repo, COOK_ROOT, "mc2_99")
        os.makedirs(cook)
        with open(os.path.join(cook, "shoreline_mask.png"), "wb") as f:
            f.write(b"MASKBYTES")
        with open(os.path.join(cook, "shoreline_mask.bounds.txt"), "w",
                  encoding="utf-8", newline="\n") as f:
            f.write("0 0 1 1\n")
        corpus = cooked_corpus(repo, names, tracked_only=False)
        check("corpus: mission discovered", "mc2_99" in corpus, str(corpus))
        check("corpus: companion bounds.txt tracked alongside principal",
              "shoreline_mask.bounds.txt" in corpus.get("mc2_99", {}))

        # INJECTED REGRESSION: deploy tree exists, sidecar was never installed.
        target = os.path.join(tmp, "deploy")
        os.makedirs(os.path.join(target, "data", "missions", "mc2_99.beauty"))
        missing, drift, ok = audit(corpus, target)
        check("REGRESSION: uninstalled cooked sidecar reported MISSING",
              len(missing) == 2 and not ok, f"missing={missing} ok={ok}")

        # Now install it -> must pass.
        for fn, src in corpus["mc2_99"].items():
            shutil.copyfile(src, os.path.join(
                target, "data", "missions", "mc2_99.beauty", fn))
        missing, drift, ok = audit(corpus, target)
        check("installed sidecar passes", not missing and not drift and len(ok) == 2,
              f"missing={missing} drift={drift}")

        # Content drift must be distinguished from missing.
        with open(os.path.join(target, "data", "missions", "mc2_99.beauty",
                               "shoreline_mask.png"), "wb") as f:
            f.write(b"DIFFERENT")
        missing, drift, ok = audit(corpus, target)
        check("content drift detected and NOT reported as missing",
              not missing and len(drift) == 1, f"drift={drift}")

        # A companion with no cooked principal is not an obligation.
        cook2 = os.path.join(repo, COOK_ROOT, "mc2_98")
        os.makedirs(cook2)
        with open(os.path.join(cook2, "overlay_v2.bounds.txt"), "w",
                  encoding="utf-8", newline="\n") as f:
            f.write("0 0 1 1\n")
        corpus2 = cooked_corpus(repo, names, tracked_only=False)
        check("orphan companion (no cooked principal) is not an obligation",
              "mc2_98" not in corpus2 or not corpus2["mc2_98"],
              str(corpus2.get("mc2_98")))

    if failures:
        print(f"FAIL beauty-sidecars self-test: {len(failures)} check(s) failed")
        return 2
    print("PASS beauty-sidecars self-test (comment-only names excluded, "
          "uninstalled cook fails, drift distinguished)")
    return 0


# --------------------------------------------------------------------------
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--target", help="deploy tree to audit (default: enforced lanes)")
    ap.add_argument("--all-lanes", action="store_true",
                    help="audit every lane in LANES, enforced or not")
    ap.add_argument("--no-lanes", action="store_true",
                    help="repo-only: verify extraction + inventory freshness, "
                         "skip every deploy tree (machine-independent; this is "
                         "the mode check-contracts.sh runs)")
    ap.add_argument("--write", action="store_true",
                    help=f"regenerate {INVENTORY}")
    ap.add_argument("--install", action="store_true",
                    help="copy missing cooked sidecars into the target tree")
    ap.add_argument("--strict", action="store_true",
                    help="content drift is a failure, not a warning")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--self-test", dest="selftest", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    names, errors = runtime_sidecars(REPO)
    if errors:
        for e in errors:
            print(f"FAIL beauty-sidecars: {e}")
        return 2
    if not names:
        print("FAIL beauty-sidecars: extracted ZERO runtime sidecar names from "
              f"{', '.join(ENGINE_SOURCES)} -- the extractor is broken or the "
              "loaders moved. Refusing to pass vacuously.")
        return 2

    corpus = cooked_corpus(REPO, names)

    inv_path = os.path.join(REPO, INVENTORY)
    rendered = render_inventory(names, corpus)
    if args.write:
        os.makedirs(os.path.dirname(inv_path), exist_ok=True)
        with open(inv_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(rendered)
        print(f"wrote {INVENTORY}")
        return 0
    inv_stale = None
    if os.path.isfile(inv_path):
        with open(inv_path, "r", encoding="utf-8") as f:
            inv_stale = f.read() != rendered
    else:
        inv_stale = True

    if args.no_lanes:
        lanes = []
    elif args.target:
        lanes = [("target", args.target, True)]
    elif args.all_lanes:
        lanes = [(lb, p, True) for lb, p, _ in LANES]
    else:
        lanes = LANES

    report = {"runtime_sidecars": sorted(names),
              "cooked": {m: sorted(v) for m, v in sorted(corpus.items())},
              "inventory_stale": inv_stale, "lanes": {}}
    failed = False

    for label, path, enforced in lanes:
        if not os.path.isdir(path):
            report["lanes"][label] = {"path": path, "status": "absent"}
            if not args.quiet:
                print(f"  {label:<10} SKIP (tree absent) {path}")
            continue
        missing, drift, ok = audit(corpus, path)
        if args.install and enforced:
            for mission, fn, dst in missing:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(corpus[mission][fn], dst)
                print(f"  installed {mission}/{fn} -> {dst}")
            missing, drift, ok = audit(corpus, path)
        report["lanes"][label] = {
            "path": path, "enforced": enforced,
            "ok": len(ok),
            "missing": [f"{m}/{f}" for m, f, _ in missing],
            "drift": [f"{m}/{f}" for m, f, _ in drift],
        }
        bad = bool(missing) or (args.strict and bool(drift))
        if enforced and bad:
            failed = True
        if not args.quiet:
            tag = "FAIL" if (enforced and bad) else ("WARN" if (missing or drift) else "PASS")
            print(f"  {label:<10} {tag} ok={len(ok)} missing={len(missing)} "
                  f"drift={len(drift)}{'' if enforced else '  (not enforced)'}")
            for m, f_, dst in missing:
                print(f"      MISSING {m}/{f_} -> {dst}")
            for m, f_, dst in drift:
                print(f"      DRIFT   {m}/{f_} (installed copy differs from cook)")

    if inv_stale:
        failed = True
        print(f"FAIL beauty-sidecars: {INVENTORY} is stale. Re-run with --write.")

    if args.json:
        print(json.dumps(report, indent=2))

    if failed:
        print("FAIL beauty-sidecars: a cooked sidecar is not installed in an "
              "enforced deploy lane (or the inventory is stale). The engine "
              "loaders fail SILENTLY on a missing sidecar -- MC2_WATER_SHORE_MASK "
              "is default-ON and no-ops entirely. Fix with --install, or delete "
              "the cook if it is not meant to ship.")
        return 2
    if args.no_lanes:
        print(f"PASS beauty-sidecars (repo-only): {len(names)} runtime sidecar "
              f"names extracted, inventory current")
    else:
        print("PASS beauty-sidecars: every cooked runtime sidecar is installed "
              "in every enforced deploy lane")
    return 0


if __name__ == "__main__":
    sys.exit(main())
