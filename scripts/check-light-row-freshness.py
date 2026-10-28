#!/usr/bin/env python3
"""check-light-row-freshness.py -- LIGHTROW-FRESHNESS-1 recurrence guard.

THE DEFECT THIS PINS
--------------------
`TG_MultiShape::cachedGpuLightIndex_` holds a DYNAMIC light-table slot index.
Dynamic slot indices are rebased EVERY FRAME: `MC_TextureManager::resetLightData`
(mclib/txmmgr.cpp) resets `lightDataStructuresCount` to the static high-water and
clears every dedup map, and its own comment says stale hash->slot entries from
the prior frame "are invalid".  A slot number captured on frame N therefore names
a DIFFERENT actor's row -- or no written row at all -- on frame N+1.

The field ships with a companion stamp, `cachedFrame_`, whose whole purpose is to
say which frame the index was minted on.  Reading the index WITHOUT comparing the
stamp is the bug.  It shipped that way in the dynamic static-prop submit path
(GameOS/gameos/gos_static_prop_batcher.cpp) while BOTH sibling consumers --
mclib/msl.cpp and GameOS/gameos/gos_static_prop_registry.cpp -- already guarded
correctly.  shaders/static_prop.vert indexes `light[]` with no bounds test, so the
visible result is a pure-black, texture-independent, silhouette-shaped actor.

THE RULE
--------
Any C++ translation unit that READS `cachedGpuLightIndex_` (or the accessor
`getCachedGpuLightIndex()`) must ALSO reference the freshness stamp -- either
`cachedFrame_` or `getCachedFrame()` -- somewhere in the same file.

This is deliberately a coarse, file-level co-occurrence rule rather than a
dataflow proof.  It cannot verify that the comparison is CORRECT; it can only
verify that the stamp was not ignored outright, which is exactly the historical
failure.  A file that reads the index and never mentions the stamp is the
signature, and it is cheap and stable to detect.  Writers (the sites that ASSIGN
the index) are exempt: assigning is not trusting.

Exemptions require an explicit entry in EXEMPT below with a stated reason, so
that adding one is a visible decision rather than a silent omission.

Usage:
    py -3 scripts/check-light-row-freshness.py [--quiet]
    py -3 scripts/check-light-row-freshness.py --selftest

Exit 0 = pass, 1 = violation, 2 = selftest failure.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checklib  # noqa: E402

REPO = Path(__file__).resolve().parent.parent

# Files searched.  Kept narrow on purpose: these are the trees that can see
# TG_MultiShape at all.
SEARCH_DIRS = ("GameOS", "mclib", "code", "RenderCore", "RenderWorld", "editor")
SEARCH_EXTS = (".cpp", ".h", ".hpp", ".inl")

INDEX_READ = re.compile(r"\bcachedGpuLightIndex_\b|\bgetCachedGpuLightIndex\s*\(")
STAMP_REF = re.compile(r"\bcachedFrame_\b|\bgetCachedFrame\s*\(|\bsetCachedFrame\s*\(")

# A line that only ASSIGNS the index is a writer, not a truster.
#   cachedGpuLightIndex_ = <expr>;      (but NOT ==, !=, >=, <=)
INDEX_WRITE_ONLY = re.compile(r"^\s*(?:\w+\s*->\s*|\w+\s*\.\s*)?cachedGpuLightIndex_\s*=[^=]")

# path -> reason.  An entry here is a deliberate exception, and every
# reason is PRINTED on a passing run so an exemption cannot quietly become the
# place a defect lives.
_BDACTOR_PRODUCER = (
    "PRODUCER, guarded downstream. Its getCachedGpuLightIndex() reads all "
    "feed staticReg.lightDataIndex, and the consumer -- "
    "GameOS/gameos/gos_static_prop_registry.cpp flush() -- compares "
    "getCachedFrame() against the current frame at :1051 and suppresses the "
    "draw when stale. Handing an index onward to a checker is not trusting it.")

EXEMPT: dict[str, str] = {
    "mclib/bdactor_diag.cpp": _BDACTOR_PRODUCER,
    "mclib/bdactor_lightbake.cpp": _BDACTOR_PRODUCER,
    "mclib/bdactor_render.cpp": _BDACTOR_PRODUCER,
    "mclib/bdactor_static.cpp": _BDACTOR_PRODUCER,
    "mclib/bdactor_tree.cpp": _BDACTOR_PRODUCER,
    "mclib/bdactor_tree_static.cpp": _BDACTOR_PRODUCER,
    "mclib/bdactor_update.cpp": _BDACTOR_PRODUCER,
    "mclib/gvactor_update.cpp": (
        "PREDICTOR, not a consumer. It only mirrors the decline checks of "
        "GVAppearance::submitToMechBatcher (mclib/gvactor_render.cpp) to decide whether to reserve the "
        "CPU body; the real submit stamps the row fresh with setCachedFrame(g_mc2FrameCounter) right "
        "before it hands the index to the batcher, so the update-time read never reaches a draw."),
    # mclib/mech3d.cpp is intentionally absent from this table: the GPU-mech
    # submit path reads getCachedFrame() unconditionally. An exemption here
    # would advertise an open defect that is closed.
    #
    # Declining the submit on a stale stamp is wrong. The stated ground was that
    # the mech would still be drawn by the CPU MLR fallback, but inside the
    # submit block's own enclosing gate (!previewContext)
    # mechShape has been transformed by _HierarchyOnly, so TG_Shape::Render
    # returns at the lastTurnTransformed guard (mclib/tgl.cpp:3190) without
    # drawing, and the decline made the mech VANISH. The site RECORDS the stale
    # stamp and submits anyway (plus a bounds clamp). What this checker requires
    # is unchanged and still satisfied: the consumer references the freshness
    # stamp. The rule is "does not trust the index blindly", not "declines".
}


def _strip_comments(text: str) -> str:
    """Remove // and /* */ comments so a mention in prose never satisfies the rule.

    STRIPPER-AUDIT-1 (2026-07-31): delegated to the shared lexer. The two
    stateless regexes this replaced could not see a `/*` sitting inside a `//`
    comment -- notably the stock `//*****` banner, where `//` + `*` IS a `/*` --
    and so opened a phantom block running to the next `*/` anywhere in the file.

    MEASURED exposure for THIS checker specifically: low but non-zero and
    latent. Only files whose RAW text carries the needle reach the stripper, and
    today that is 10 files, of which one (mclib/bdactor.cpp, 225 hidden code
    lines) is affected -- and it is EXEMPT below, and its needle lines fall
    outside the hidden span. So the bug changes no verdict today. It arms itself
    the moment a `getCachedGpuLightIndex()` read lands after a banner comment in
    any scanned TU, at which point this rule would go quietly green on a real
    unguarded read. Repointed on that basis, not on a live failure.
    """
    return checklib.strip_comments(text)


def scan(repo: Path) -> list[tuple[str, list[int]]]:
    """Return [(relpath, [line numbers that read the index])] for violating files."""
    violations: list[tuple[str, list[int]]] = []
    for d in SEARCH_DIRS:
        root = repo / d
        if not root.is_dir():
            continue
        for p in sorted(root.rglob("*")):
            if p.suffix not in SEARCH_EXTS or not p.is_file():
                continue
            rel = p.relative_to(repo).as_posix()
            if rel in EXEMPT:
                continue
            try:
                raw = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if "cachedGpuLightIndex_" not in raw and "getCachedGpuLightIndex" not in raw:
                continue
            code = _strip_comments(raw)
            if not INDEX_READ.search(code):
                continue
            if STAMP_REF.search(code):
                continue
            # Reads the index, never mentions the stamp.  Collect the read lines
            # that are not pure writes, so the message points at real trust sites.
            hits: list[int] = []
            for i, line in enumerate(raw.splitlines(), 1):
                stripped = _strip_comments(line)
                if not INDEX_READ.search(stripped):
                    continue
                if INDEX_WRITE_ONLY.match(stripped):
                    continue
                hits.append(i)
            if hits:
                violations.append((rel, hits))
    return violations


def selftest() -> int:
    """Prove the rule can FAIL.

    Builds two synthetic sources in memory-equivalent form on a temp tree: one
    that reads the index while checking the stamp (must pass) and one that reads
    it without (must fail).  A checker that cannot demonstrate its own red state
    is indistinguishable from `exit 0`.
    """
    import tempfile

    ok = True
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        (tmp / "GameOS").mkdir(parents=True)

        good = tmp / "GameOS" / "good.cpp"
        good.write_text(
            "void f(M* m){\n"
            "  if (m->cachedFrame_ == g_mc2FrameCounter &&\n"
            "      m->cachedGpuLightIndex_ != 0xFFFFFFFFu) { use(m->cachedGpuLightIndex_); }\n"
            "}\n",
            encoding="utf-8",
        )
        bad = tmp / "GameOS" / "bad.cpp"
        bad.write_text(
            "void f(M* m){\n"
            "  // cachedFrame_ mentioned only in a comment -- must NOT satisfy the rule\n"
            "  if (m->cachedGpuLightIndex_ != 0xFFFFFFFFu) { use(m->cachedGpuLightIndex_); }\n"
            "}\n",
            encoding="utf-8",
        )
        writer = tmp / "GameOS" / "writer.cpp"
        writer.write_text(
            "void g(M* m){\n"
            "    m->cachedGpuLightIndex_ = compute();\n"
            "}\n",
            encoding="utf-8",
        )

        v = {rel for rel, _ in scan(tmp)}
        if "GameOS/good.cpp" in v:
            print("SELFTEST FAIL: guarded reader flagged")
            ok = False
        if "GameOS/bad.cpp" not in v:
            print("SELFTEST FAIL: unguarded reader NOT flagged "
                  "(the rule cannot detect its own regression)")
            ok = False
        if "GameOS/writer.cpp" in v:
            print("SELFTEST FAIL: pure writer flagged (assigning is not trusting)")
            ok = False

    if ok:
        print("PASS: check-light-row-freshness.py selftest "
              "(guarded=clean, unguarded=flagged, comment-only mention=flagged, writer=exempt)")
        return 0
    return 2


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quiet", action="store_true", help="only print on failure")
    ap.add_argument("--selftest", action="store_true", help="prove the rule can fail")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    violations = scan(REPO)
    if violations:
        print("FAIL: check-light-row-freshness.py -- LIGHTROW-FRESHNESS-1 violated")
        print()
        print("  These files READ TG_MultiShape::cachedGpuLightIndex_ but never")
        print("  reference its freshness stamp (cachedFrame_ / getCachedFrame()).")
        print("  A dynamic light slot is rebased every frame by")
        print("  MC_TextureManager::resetLightData (mclib/txmmgr.cpp), so an index")
        print("  from a prior frame names a different actor's row -- which the")
        print("  shader reads unchecked, rendering the actor pure black.")
        print()
        for rel, hits in violations:
            shown = ", ".join(str(h) for h in hits[:8])
            more = "" if len(hits) <= 8 else f" (+{len(hits) - 8} more)"
            print(f"  {rel}: line(s) {shown}{more}")
        print()
        print("  Fix: compare cachedFrame_ against g_mc2FrameCounter before")
        print("  trusting the index, and RE-GATHER (not substitute) when stale.")
        print("  See mclib/msl.cpp ResubmitCachedGpuLightData and")
        print("  GameOS/gameos/gos_static_prop_registry.cpp flush for the two")
        print("  reference implementations.")
        return 1

    if not args.quiet:
        print("PASS: check-light-row-freshness.py -- every cachedGpuLightIndex_ "
              "consumer references its freshness stamp")
        # Exemptions are printed on every passing run. An exemption list nobody
        # reads is where the next instance of this defect will live.
        for path, reason in sorted(EXEMPT.items()):
            print(f"  EXEMPT {path}\n    {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
