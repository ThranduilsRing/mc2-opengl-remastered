#!/usr/bin/env python3
# scripts/check-hardlink-isolation.py
"""Hardlink-isolation guard -- "is this deploy dir really MINE?"

WHY THIS EXISTS (2026-07-30 shared-install incident)
----------------------------------------------------
Four deploy dirs under A:/Games/mc2-opengl shared ONE hardlinked mc2.exe and
one shared shader tree:

    mc2-win64-mc203-regress
    mc2-win64-mc203air
    mc2-win64-treesoff-1
    mc2-win64-v0.4d-rc1          <-- and this one is in DEPLOY_ALLOWLIST

`scripts/deploy_payload.py` copies with `shutil.copyfile`, which opens the
destination `"wb"` -- it TRUNCATES AND REWRITES THE EXISTING INODE. It does not
unlink first, so it does not break the link. Deploying into any one of those
four dirs therefore rewrote the mc2.exe of the other three, in place, silently.
Measured directly rather than assumed:

    A/mc2.exe and B/mc2.exe hardlinked, A = b'ORIGINAL-BASELINE-BYTES'
    shutil.copyfile(new, B)
    -> A now reads b'NEW-LANE-BYTES', inode unchanged, nlink still 2

That is the whole bug. It cost real evidence twice in one day: one lane
destroyed mc2-win64-v0.4 as a comparison baseline mid-session, and another
lane's 07-28/29 findings turned out to rest on self-report because every lane
had been deploying into bytes somebody else was also reading.

WHAT MADE IT INVISIBLE
----------------------
Every existing guard answered a DIFFERENT question and answered it correctly:

  * DEPLOY_ALLOWLIST      -- "is this a canonical folder?"  (yes, v0.4d-rc1 is)
  * .mc2_deploy_lane.json -- "is another lane holding THIS PATH?"  (no)
  * .smoke_leases.json    -- "is another lane smoking THIS PATH?"  (no)
  * deploy_status()       -- "is the exe at THIS PATH stale?"  (no)

All of them are path-keyed. A hardlink is precisely the case where two paths
are one object, so a path-keyed registry reports a directory as FREE while
writing it mutates somebody else's install. There was no question anyone could
ask that would have returned the true answer. This script is that question.

HARDLINKS ARE NOT THE ENEMY
---------------------------
The A: volume had 8-11 GB free while this was written and a full install
MEASURES 12.26 GB / 40135 files (mc2-win64-v0.4), so a single naive per-lane
copy does not even fit -- and an earlier lane got a private install for free
precisely BY hardlinking, which was the right instinct. The rule is not "no
hardlinks", it is:

    data that is only ever READ   -> share it, linking is free and correct
    artifacts that get WRITTEN    -> must be a private inode per lane

`scripts/make-isolated-deploy.py` builds exactly that split.

WHAT COUNTS AS "WRITTEN" (and why it is a root list, not a glob list)
---------------------------------------------------------------------
MUTATED_ROOTS below is the deploy-written + runtime-written surface, taken
from what deploy_payload.py's enumerate_payload() actually emits plus the state
the running game writes:

    <root>/*     (non-recursive)  exe, pdb, dlls, *.bat, options.cfg,
                                  .deployed_manifest.csv, .mc2_deploy_lane.json
    shaders/**                    every deploy rewrites the whole tree
    tools/**                      terrain_gen authoring tree (support payload)
    data/defs/**                  UI defs payload (kind="defs")
    mods/{mc2-industrial-kit,core_unlocks,sample_*}/**  shipped mod payload
    data/tgl/*.cookstamp           stock cook receipts (all subdirectories)
    data/textures/**              generated terrain material sidecars
    data/{kit,model_overrides,vegetation,terrain_layers}/**  renderer assets
    3rdparty/{smaa,cmaa2}/**       shipped notices
    cache/content-model/**        native model recipes and kit receipt
    data/{terrain_materials,visual_tuning}.json             renderer config

It is a list of ROOTS rather than a list of extensions on purpose. An extension
allowlist drifts silently the moment somebody adds a payload class; a root list
over-approximates instead, so a new payload file lands inside an
already-covered root and is protected by default. Over-approximating fails
LOUD (a spurious refusal someone investigates); under-approximating fails OPEN
(the exact silence this incident was made of).

`--payload-list` bypasses the heuristic entirely: deploy_payload.py passes the
literal relpaths it is about to write, so at the site that matters the
classification is not an approximation at all.

VERDICTS (one, most-severe wins):  OK | LINKED-DATA | SHARED
    OK           No mutated-class file in the target shares an inode with
                 anything. The install is yours.
    LINKED-DATA  Mutated files are private; read-only data is shared with
                 other dirs. This is the GOOD state -- it is what makes a
                 private install cost megabytes instead of gigabytes. Only
                 reported when asked (--data-links).
    SHARED       At least one mutated-class file has st_nlink > 1. Writing
                 this directory mutates another install.

Exit codes (the check-disk-space.py / check-deploy-target.py convention):
    0  OK
    1  LINKED-DATA   (advisory: this is fine, and cheap)
    3  SHARED        (hard: caller should abort)
    2  usage / path error

WHY st_nlink IS THE PRIMARY SIGNAL
----------------------------------
Detection does NOT depend on enumerating sibling directories. `st_nlink > 1`
is the filesystem's own answer to "does another name point at these bytes",
and it is true even if the other name lives somewhere this script would never
think to look -- another drive letter's mount, a path outside
A:/Games/mc2-opengl, a dir created after this list was written. Peer
attribution (naming WHICH dirs) is a best-effort convenience layered on top;
it can come back empty and the refusal still stands. A guard whose correctness
depended on a hardcoded search list would have the same blind spot as the
path-keyed registries it exists to backstop.

Pure stdlib. Read-only. No build. Sub-second on the default bounded roots
(~1.1-1.5k files vs the 28.8k-40k in a full install).

Examples:
  # before deploying: is this dir mine?
  py -3 scripts/check-hardlink-isolation.py --target A:/Games/mc2-opengl/mc2-win64-v0.4

  # exact mode: only these relpaths are about to be written
  py -3 scripts/check-hardlink-isolation.py --target "$T" --payload-list payload.txt

  # full walk incl. data/, and report the (good) read-only sharing too
  py -3 scripts/check-hardlink-isolation.py --target "$T" --deep --data-links

  # ratchet: prove the detector actually fires
  py -3 scripts/check-hardlink-isolation.py --selftest
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

PREFIX = "[LINK_GUARD]"

VERDICT_EXIT = {
    "OK": 0,
    "LINKED-DATA": 1,
    "ERROR": 2,
    "SHARED": 3,
}

# The deploy-written + runtime-written surface. "" means the target root,
# non-recursively. See the module docstring for why this is a root list.
MUTATED_ROOTS = (
    "", "shaders", "tools", "3rdparty/smaa", "3rdparty/cmaa2",
    "mods/mc2-industrial-kit", "mods/core_unlocks", "mods/sample_equipment",
    "mods/sample_new_variant", "mods/sample_variant",
    "data/defs", "data/kit", "data/model_overrides",
    "data/vegetation", "data/terrain_layers", "data/textures",
    "cache/content-model",
)
MUTATED_FILES = (
    "data/terrain_materials.json", "data/visual_tuning.json",
    "data/tgl/spotlight.ini", "data/tgl/marblecliff.ini", "data/tgl/MarbleCliffGLB.glb",
)
_MUTATED_FILES_FOLDED = frozenset(rel.casefold() for rel in MUTATED_FILES)

# The one carve-out from "everything in the target root is written": the stock
# FST/PAK asset archives happen to live at the root, and they are the single
# largest read-only payload in the install (textures.fst alone is 184 MB;
# mc2-win64-v0.3 shares 240 MB of them with a dir outside the searched roots).
#
# This is a precision fix, not a hole. "fst" and "pak" appear ZERO times in
# deploy_payload.py -- they are provably not deploy payload items, and the
# engine only ever reads them. Without the carve-out the guard refuses a dir
# whose only sharing is exactly the sharing this design recommends, and a
# guard that cries wolf on a correct setup teaches people to set
# MC2_DEPLOY_ALLOW_SHARED_LINKS=1 by reflex -- which costs more than the
# over-approximation saves. Narrow, extension-pinned, and root-only.
DATA_EXTS_AT_ROOT = (".fst", ".pak")

# Cap on how many shared files get named individually in the human report.
# The full set is always in the --json payload.
MAX_LISTED = 12

# Cap on how many shared files are probed when NAMING the peer dirs.
# Attribution costs (shared files x candidate dirs) stats; on the real
# incident that was 1530 x ~30 = ~45k, about 5 s. Sampling the first N keeps
# the common path fast. It can only ever shorten the PEER NAME LIST -- the
# SHARED verdict comes from st_nlink and is unaffected -- and the report says
# so when the cap bites, so a short list is never mistaken for a complete one.
MAX_ATTRIBUTE_PROBES = 128


def out(msg: str) -> None:
    print(f"{PREFIX} {msg}")


def err(msg: str) -> None:
    print(f"{PREFIX} {msg}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Enumeration
# ---------------------------------------------------------------------------

def iter_mutated_relpaths(target: Path, deep: bool = False):
    """Yield target-relative POSIX paths of mutated-class files.

    Default walks only MUTATED_ROOTS (bounded, sub-second). --deep walks the
    whole tree and still yields only mutated-class entries, so `deep` changes
    coverage, never classification.
    """
    if deep:
        for dirpath, _dirs, files in os.walk(target):
            for fn in files:
                p = Path(dirpath) / fn
                rel = p.relative_to(target).as_posix()
                if classify(rel) == "mutated":
                    yield rel
        return

    # TGL sources remain shared; deploy only changes the three shipped sources
    # above and cooker stamps. Scan this tree through the same classifier.
    for path in (target / "data/tgl").rglob("*"):
        rel = path.relative_to(target).as_posix()
        if path.is_file() and rel.casefold() not in _MUTATED_FILES_FOLDED and classify(rel) == "mutated":
            yield rel
    for rel in MUTATED_FILES:
        if (target / rel).is_file():
            yield rel
    for root in MUTATED_ROOTS:
        base = target / root if root else target
        if not base.is_dir():
            continue
        if root == "":
            for entry in sorted(os.listdir(base)):
                # classify() -- not "everything at root" -- so the
                # DATA_EXTS_AT_ROOT carve-out applies here too. Yielding raw
                # root entries was the original bug: it made the enumerator
                # and the classifier disagree, and mc2-win64-v0.3 was reported
                # SHARED over 240 MB of read-only .fst archives.
                if classify(entry) == "mutated" and (base / entry).is_file():
                    yield entry
        else:
            for dirpath, _dirs, files in os.walk(base):
                for fn in sorted(files):
                    rel = (Path(dirpath) / fn).relative_to(target).as_posix()
                    if classify(rel) == "mutated":
                        yield rel


def classify(rel: str) -> str:
    """'mutated' (must be a private inode) or 'data' (safe to share)."""
    rel = rel.replace("\\", "/").lstrip("/").casefold()
    if rel in _MUTATED_FILES_FOLDED:
        return "mutated"
    if rel.startswith("data/tgl/") and rel.endswith(".cookstamp"):
        return "mutated"
    if "/" not in rel:
        # Anything sitting in the target root, except the stock read-only
        # asset archives that happen to live there (see DATA_EXTS_AT_ROOT).
        if rel.lower().endswith(DATA_EXTS_AT_ROOT):
            return "data"
        return "mutated"
    for root in MUTATED_ROOTS:
        if root and (rel == root or rel.startswith(root + "/")):
            return "mutated"
    return "data"


def iter_data_relpaths(target: Path):
    """Yield target-relative paths of data-class files (full walk; only used
    for the informational --data-links report)."""
    for dirpath, _dirs, files in os.walk(target):
        for fn in files:
            rel = (Path(dirpath) / fn).relative_to(target).as_posix()
            if classify(rel) == "data":
                yield rel


# ---------------------------------------------------------------------------
# Peer attribution (best effort -- see docstring)
# ---------------------------------------------------------------------------

def default_peer_roots(target: Path) -> list[Path]:
    """Dirs whose children are plausible sibling installs.

    Covers both A:/Games/mc2-opengl and A:/Games/mc2-opengl/releases whichever
    side the target sits on, because the incident spanned both.
    """
    roots: list[Path] = []
    parent = target.parent
    if parent.is_dir():
        roots.append(parent)
    if parent.name.lower() == "releases":
        if parent.parent.is_dir():
            roots.append(parent.parent)
    else:
        rel = parent / "releases"
        if rel.is_dir():
            roots.append(rel)
    seen, uniq = set(), []
    for r in roots:
        k = str(r).replace("\\", "/").casefold()
        if k not in seen:
            seen.add(k)
            uniq.append(r)
    return uniq


def candidate_peers(target: Path, peer_roots: list[Path]) -> list[Path]:
    tkey = str(target).replace("\\", "/").casefold()
    peers, seen = [], set()
    for root in peer_roots:
        try:
            entries = sorted(os.listdir(root))
        except OSError:
            continue
        for name in entries:
            p = root / name
            k = str(p).replace("\\", "/").casefold()
            if k == tkey or k in seen or not p.is_dir():
                continue
            seen.add(k)
            peers.append(p)
    return peers


def file_id(path: Path):
    try:
        st = path.stat()
    except OSError:
        return None
    return (st.st_dev, st.st_ino, st.st_nlink, st.st_size)


def attribute(target: Path, shared_rels: list[str],
              peers: list[Path]) -> dict[str, list[str]]:
    """Map peer dir -> which of `shared_rels` it shares an inode with."""
    want = {}
    for rel in shared_rels:
        fid = file_id(target / rel)
        if fid:
            want[rel] = (fid[0], fid[1])
    hits: dict[str, list[str]] = {}
    for peer in peers:
        for rel, key in want.items():
            fid = file_id(peer / rel)
            if fid and (fid[0], fid[1]) == key:
                hits.setdefault(str(peer), []).append(rel)
    return hits


# ---------------------------------------------------------------------------
# Core check
# ---------------------------------------------------------------------------

def run_check(target_str: str, payload_rels: list[str] | None = None,
              deep: bool = False, data_links: bool = False,
              peer_roots: list[str] | None = None,
              label: str | None = None) -> tuple[str, dict, list[str]]:
    """Returns (verdict, facts, messages). No writes, ever."""
    msgs: list[str] = []
    if not target_str or "\x00" in target_str:
        note = f"invalid target {target_str!r} (empty or contains NUL)"
        return "ERROR", {"target": target_str, "error": note}, [f"FAIL: {note}"]
    try:
        target = Path(target_str).expanduser()
        target = target.resolve() if target.exists() else target.absolute()
    except (OSError, ValueError) as e:
        note = f"unusable target {target_str!r}: {e}"
        return "ERROR", {"target": target_str, "error": note}, [f"FAIL: {note}"]

    if not target.is_dir():
        note = f"target dir does not exist: {target}"
        return "ERROR", {"target": str(target), "error": note}, [f"FAIL: {note}"]

    if payload_rels is not None:
        rels = [r.replace("\\", "/").lstrip("/") for r in payload_rels if r.strip()]
        source = f"payload-list ({len(rels)} relpaths)"
    else:
        rels = list(iter_mutated_relpaths(target, deep=deep))
        source = ("deep walk" if deep else
                  "bounded roots " + ", ".join(r or "<root>" for r in MUTATED_ROOTS))

    shared, checked = [], 0
    for rel in rels:
        fid = file_id(target / rel)
        if fid is None:
            continue           # not deployed yet -- nothing to clobber
        checked += 1
        if fid[2] > 1:
            shared.append({"rel": rel, "nlink": fid[2], "bytes": fid[3],
                           "ino": fid[1]})

    facts = {
        "target": str(target),
        "op": label,
        "scan_source": source,
        "mutated_files_checked": checked,
        "shared_count": len(shared),
        "shared": shared,
        "peers": {},
        "data_links": None,
    }

    if shared:
        roots = ([Path(p) for p in peer_roots] if peer_roots
                 else default_peer_roots(target))
        facts["peer_roots"] = [str(r) for r in roots]
        probe_rels = [s["rel"] for s in shared][:MAX_ATTRIBUTE_PROBES]
        facts["attribution_sampled"] = len(shared) > MAX_ATTRIBUTE_PROBES
        facts["attribution_probes"] = len(probe_rels)
        facts["peers"] = attribute(target, probe_rels,
                                   candidate_peers(target, roots))

    if data_links:
        n_linked = n_total = 0
        for rel in iter_data_relpaths(target):
            fid = file_id(target / rel)
            if fid is None:
                continue
            n_total += 1
            if fid[2] > 1:
                n_linked += 1
        facts["data_links"] = {"linked": n_linked, "total": n_total}

    if shared:
        verdict = "SHARED"
    elif data_links and facts["data_links"] and facts["data_links"]["linked"]:
        verdict = "LINKED-DATA"
    else:
        verdict = "OK"
    facts["verdict"] = verdict

    what = f" for {label}" if label else ""
    if verdict == "SHARED":
        msgs.append(f"FAIL: DEPLOY TARGET IS NOT ISOLATED{what}")
        msgs.append(f"FAIL:   target : {target}")
        msgs.append(f"FAIL:   {len(shared)} of {checked} mutated-class file(s) "
                    f"are HARDLINKED to another install.")
        for s in shared[:MAX_LISTED]:
            msgs.append(f"FAIL:     {s['rel']}  (nlink={s['nlink']}, "
                        f"{s['bytes']} bytes)")
        if len(shared) > MAX_LISTED:
            msgs.append(f"FAIL:     ... and {len(shared) - MAX_LISTED} more "
                        f"(use --json for the full list)")
        if facts["peers"]:
            msgs.append("FAIL:   THE SAME BYTES ARE ALSO AT:")
            for peer, rels_hit in sorted(facts["peers"].items()):
                msgs.append(f"FAIL:     {peer}  ({len(rels_hit)} shared file(s))")
            if facts.get("attribution_sampled"):
                msgs.append(f"FAIL:   (peer search probed the first "
                            f"{facts['attribution_probes']} of "
                            f"{len(shared)} shared files -- there may be "
                            f"further dirs in this link group)")
        else:
            msgs.append("FAIL:   Could not NAME the other dir(s) -- they are "
                        "outside the searched roots. st_nlink is the "
                        "filesystem's own answer and it says shared; the "
                        "refusal stands.")
        msgs.append("FAIL: WRITING THIS DIRECTORY REWRITES THOSE FILES IN "
                    "PLACE. shutil.copyfile opens the destination \"wb\", "
                    "which truncates the shared inode -- it does not break "
                    "the link (2026-07-30: a lane destroyed another lane's "
                    "comparison baseline exactly this way).")
        msgs.append("FAIL: WHAT TO DO: build a private install --")
        msgs.append(f"FAIL:   py -3 scripts/make-isolated-deploy.py "
                    f"--from \"{target}\" --to \"{target}-<lane>\"")
        msgs.append("FAIL:   That hardlinks the read-only data (free) and "
                    "gives you private copies of only the written artifacts.")
        msgs.append("FAIL: Or deliberately accept shared mutation with "
                    "MC2_DEPLOY_ALLOW_SHARED_LINKS=1.")
    elif verdict == "LINKED-DATA":
        d = facts["data_links"]
        msgs.append(f"OK: mutated artifacts are PRIVATE ({checked} checked); "
                    f"{d['linked']}/{d['total']} read-only data files are "
                    f"shared with other installs. That sharing is correct and "
                    f"is what makes this install cheap.")
    else:
        extra = ""
        if facts["data_links"]:
            extra = (f"; {facts['data_links']['total']} data files, none "
                     f"shared (this install is a full private copy)")
        msgs.append(f"OK: isolated -- {checked} mutated-class file(s) checked, "
                    f"none hardlinked{extra}.")
    return verdict, facts, msgs


# ---------------------------------------------------------------------------
# Selftest
# ---------------------------------------------------------------------------

def selftest() -> int:
    """Prove the detector fires, using real os.link on real files.

    Deliberately does NOT touch A:/Games/mc2-opengl. The check-disk-space.py
    lane learned this the expensive way: its first selftest attempt "tested"
    a full-disk guard by actually filling the disk, which killed three other
    lanes. A guard's selftest must not be able to reproduce the incident it
    guards against. Everything here happens in a temp dir and is removed.
    """
    failures: list[str] = []

    def check(name: str, cond: bool, detail: str = "") -> None:
        if cond:
            out(f"  selftest PASS: {name}")
        else:
            failures.append(f"{name}{': ' + detail if detail else ''}")
            err(f"  selftest FAIL: {name} {detail}")

    tmp = Path(tempfile.mkdtemp(prefix="mc2_linkguard_"))
    try:
        a, b = tmp / "lane-A", tmp / "lane-B"
        for d in (a, b):
            (d / "shaders").mkdir(parents=True)
            (d / "data" / "terrain").mkdir(parents=True)
        (a / "mc2.exe").write_bytes(b"ORIGINAL-BASELINE-BYTES")
        (a / "shaders" / "gos_terrain.frag").write_text("// A\n")
        (a / "data" / "terrain" / "big.pak").write_bytes(b"x" * 4096)
        (b / "mc2.exe").write_bytes(b"B-private")
        (b / "shaders" / "gos_terrain.frag").write_text("// B\n")

        # data-class link only -> isolated, and reported as the GOOD state
        os.link(a / "data" / "terrain" / "big.pak",
                b / "data" / "terrain" / "big.pak")
        v, f, _ = run_check(str(b))
        check("data-only link is NOT flagged", v == "OK", f"got {v}")
        v, f, _ = run_check(str(b), data_links=True)
        check("data-only link reports LINKED-DATA with --data-links",
              v == "LINKED-DATA" and f["data_links"]["linked"] == 1,
              f"got {v} {f['data_links']}")

        # mutated-class link -> SHARED, peer named
        (b / "mc2.exe").unlink()
        os.link(a / "mc2.exe", b / "mc2.exe")
        v, f, _ = run_check(str(b))
        check("hardlinked mc2.exe is flagged SHARED", v == "SHARED", f"got {v}")
        check("shared file is named",
              any(s["rel"] == "mc2.exe" for s in f["shared"]))
        check("peer dir is named",
              any(Path(p).name == "lane-A" for p in f["peers"]),
              f"peers={f['peers']}")

        # THE ACTUAL BUG: a copy into B rewrites A through the link.
        src = tmp / "new-mc2.exe"
        src.write_bytes(b"NEW-LANE-BYTES")
        shutil.copyfile(src, b / "mc2.exe")
        check("shutil.copyfile writes THROUGH the hardlink (the bug)",
              (a / "mc2.exe").read_bytes() == b"NEW-LANE-BYTES",
              f"A reads {(a / 'mc2.exe').read_bytes()!r}")

        # shader-tree link -> SHARED
        (b / "mc2.exe").unlink()
        (b / "mc2.exe").write_bytes(b"B-private-again")
        v, _, _ = run_check(str(b))
        check("unlinking the exe restores OK", v == "OK", f"got {v}")
        (b / "shaders" / "gos_terrain.frag").unlink()
        os.link(a / "shaders" / "gos_terrain.frag",
                b / "shaders" / "gos_terrain.frag")
        v, _, _ = run_check(str(b))
        check("hardlinked shader is flagged SHARED", v == "SHARED", f"got {v}")

        # payload-list mode is authoritative and narrower
        v, _, _ = run_check(str(b), payload_rels=["mc2.exe"])
        check("payload-list excludes unlisted shared files", v == "OK",
              f"got {v}")
        v, _, _ = run_check(str(b),
                            payload_rels=["shaders/gos_terrain.frag"])
        check("payload-list catches a listed shared file", v == "SHARED",
              f"got {v}")

        # classification does not silently widen
        check("classify(): root file is mutated",
              classify("mc2.exe") == "mutated")
        check("classify(): shaders/ is mutated",
              classify("shaders/x/y.frag") == "mutated")
        check("classify(): data/defs is mutated",
              classify("data/defs/ui/PACKAGE.fit") == "mutated")
        check("classify(): data/ payload is data",
              classify("data/terrain/big.pak") == "data")
        check("classify(): root .fst archive is data (read-only carve-out)",
              classify("textures.fst") == "data")
        check("classify(): root .pak archive is data",
              classify("mc2_01.pak") == "data")
        check("classify(): the carve-out is extension-pinned, not a hole",
              classify("mc2.exe") == "mutated" and
              classify("options.cfg") == "mutated" and
              classify("fst") == "mutated")

        # A shared root-level .fst must NOT trip the guard (v0.3's real case).
        (a / "textures.fst").write_bytes(b"F" * 512)
        (b / "shaders" / "gos_terrain.frag").unlink()
        (b / "shaders" / "gos_terrain.frag").write_text("// B again\n")
        os.link(a / "textures.fst", b / "textures.fst")
        v, _, _ = run_check(str(b))
        check("shared root .fst is NOT flagged", v == "OK", f"got {v}")

        # errors are ERROR, not a silent pass
        v, _, _ = run_check(str(tmp / "does-not-exist"))
        check("missing target is ERROR (never a pass)", v == "ERROR",
              f"got {v}")
        v, _, _ = run_check("")
        check("empty target is ERROR", v == "ERROR", f"got {v}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        err(f"SELFTEST FAILED ({len(failures)}): " + "; ".join(failures))
        return 1
    out("selftest OK -- the guard detects shared inodes, names the peer, "
        "leaves read-only sharing alone, and fails closed on a bad path.")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Refuse to write a deploy dir whose mutated artifacts are "
                    "hardlinked to another install.")
    ap.add_argument("--target", help="deploy dir to check")
    ap.add_argument("--payload-list", metavar="FILE",
                    help="file of newline-delimited target-relative paths "
                         "about to be written; exact mode, bypasses the "
                         "MUTATED_ROOTS heuristic")
    ap.add_argument("--peer-root", action="append", default=[], metavar="DIR",
                    help="extra root whose children may be sibling installs "
                         "(repeatable). Attribution only; never affects the "
                         "verdict.")
    ap.add_argument("--deep", action="store_true",
                    help="walk the entire target instead of the bounded roots")
    ap.add_argument("--data-links", action="store_true",
                    help="also count shared read-only data files "
                         "(informational; that sharing is desirable)")
    ap.add_argument("--label", help="what the caller is about to do")
    ap.add_argument("--json", action="store_true", help="machine-readable")
    ap.add_argument("--quiet", action="store_true",
                    help="suppress the OK line (failures always print)")
    ap.add_argument("--selftest", action="store_true",
                    help="prove the detector fires; writes only to a temp dir")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.target:
        err("FAIL: --target is required (or --selftest)")
        return VERDICT_EXIT["ERROR"]

    payload_rels = None
    if args.payload_list:
        try:
            payload_rels = Path(args.payload_list).read_text(
                encoding="utf-8").splitlines()
        except OSError as e:
            err(f"FAIL: cannot read --payload-list {args.payload_list}: {e}")
            return VERDICT_EXIT["ERROR"]

    verdict, facts, msgs = run_check(
        args.target, payload_rels=payload_rels, deep=args.deep,
        data_links=args.data_links, peer_roots=args.peer_root or None,
        label=args.label)

    if args.json:
        print(json.dumps(facts, indent=2))
    else:
        for m in msgs:
            if m.startswith("FAIL"):
                err(m)
            elif not args.quiet:
                out(m)
    return VERDICT_EXIT[verdict]


if __name__ == "__main__":
    sys.exit(main())
