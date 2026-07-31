#!/usr/bin/env python3
"""VERIFY-DEBT-1 -- self-test for bless.py's three refusals.

A guard that has only ever been run against GOOD input is not known to work.
This builds synthetic capture runs that are deliberately bad in one specific way
each, and asserts bless.py refuses them. It is the standing proof for:

  T1  MIN_RUNS       -- a TWO-run set is refused (the three-run all-pairs rule
                        used to be prose; nothing enforced it).
  T2  duplicate runs -- passing the same run dir 3x is self-comparison, refused.
  T3  degenerate     -- an all-black region is refused, not blessed.
  T4  unstable       -- a region that agrees on pair r1/r2 but diverges on r1/r3
                        is refused. THIS IS THE CASE A TWO-RUN CHECK BLESSES: the
                        r1/r2 pair scores 0.0000%. It is why MIN_RUNS is 3.
  T5  resolution     -- mixed capture resolutions are refused.
  T6  happy path     -- a clean 3-run set DOES bless, with tool-emitted
                        provenance (identity.binary_provenance / capture_env /
                        gate_state) and all-pairs reproducibility per region.

GOLDEN-PROVENANCE-TRUTH-1 adds three more, and they are the reason this file
grew a synthetic BINARY: the runs now have to name one, by content, with its own
embedded build fingerprint, or the golden cannot be minted at all.

  T7  unattributable -- a legacy capture manifest that names its exe by PATH +
                        MTIME is refused. This is the exact shape of the manifest
                        that minted the committed tests/visual/goldens/
                        v0.5.1-baseline, whose binary was later overwritten in
                        place and is now unrecoverable.
  T8  two binaries   -- three runs that measured DIFFERENT builds are refused.
                        R1's all-pairs reproducibility number across different
                        builds measures a code change, not noise, and R1 alone
                        would have blessed it.
  T9  shared frames  -- three run manifests pointing at ONE set of PNGs are
                        refused. R1 rejects duplicate run DIRECTORIES; this is
                        the same self-comparison one level down, and it scores a
                        perfect 0.0000% on every pair.

Run:  py -3 tools/visual_verify/bless_selftest.py
Exit 0 = every guard fired exactly where it should.
"""
from __future__ import annotations

import json
import shutil
import sys
import tempfile
from pathlib import Path

_THIS = Path(__file__).resolve()
_ROOT = _THIS.parents[2]
sys.path.insert(0, str(_THIS.parents[1] / "visual"))
sys.path.insert(0, str(_THIS.parent))
sys.path.insert(0, str(_ROOT / "scripts"))

import regions as regionlib  # noqa: E402
import bless as blesslib  # noqa: E402
from png_io import encode_png  # noqa: E402
import manifest_schema as ms  # noqa: E402
import provenance_gate as pg  # noqa: E402

W, H = 400, 200
BM = "selftest_pose"

#: The commit compiled into the synthetic binary every non-legacy run measures.
BINARY_COMMIT = "a1b2c3d4e5f6"
#: The HEAD of the worktree that CAPTURED. Deliberately a full-length spelling
#: of BINARY_COMMIT: the honest arrangement is "the lane that captured is the
#: lane that built", and T10 proves that arrangement now reconciles instead of
#: being stamped SOURCE-HEAD-MISMATCH by a comparison against the blesser.
CAPTURE_LANE_HEAD = BINARY_COMMIT + "0" * (40 - len(BINARY_COMMIT))

# Region rects, in normalized coords, over disjoint horizontal bands.
#   good     y 0.00-0.25   textured, identical every run   -> blesses
#   void     y 0.25-0.50   solid black every run           -> T3 degenerate
#   unstable y 0.50-0.75   identical in r1,r2; differs r3  -> T4 all-pairs
MANIFEST = {
    "bookmark": BM,
    "mission": "selftest",
    "framing": "synthetic fixture (bless_selftest.py)",
    "regions": [
        {"id": "good", "severity": "medium",
         "rect": {"mode": "norm", "x": 0.0, "y": 0.00, "w": 1.0, "h": 0.25},
         "thresholds": {"mean_delta": 4.0, "changed_pct": 2.5, "struct": 10.0},
         "check": "synthetic stable textured band"},
        {"id": "void", "severity": "critical",
         "rect": {"mode": "norm", "x": 0.0, "y": 0.25, "w": 1.0, "h": 0.25},
         "thresholds": {"mean_delta": 4.0, "changed_pct": 2.5, "struct": 10.0},
         "check": "synthetic all-black band"},
        {"id": "unstable", "severity": "high",
         "rect": {"mode": "norm", "x": 0.0, "y": 0.50, "w": 1.0, "h": 0.25},
         "thresholds": {"mean_delta": 4.0, "changed_pct": 2.5, "struct": 10.0},
         "check": "synthetic band that only diverges on the r*/r3 pairs"},
    ],
}


def _frame(run_index: int, w: int = W, h: int = H) -> bytes:
    """Synthetic frame. Deterministic per (x,y); only the `unstable` band
    (y in [0.50,0.75)) varies, and only for run index 3."""
    px = bytearray(w * h * 3)
    y_void0, y_void1 = int(0.25 * h), int(0.50 * h)
    y_uns0, y_uns1 = int(0.50 * h), int(0.75 * h)
    for y in range(h):
        for x in range(w):
            i = (y * w + x) * 3
            if y_void0 <= y < y_void1:
                r = g = b = 0                      # dead flat black
            elif y_uns0 <= y < y_uns1:
                # r1 and r2 identical; r3 shifted hard -> only r*/r3 pairs fail
                base = (x * 7 + y * 13) % 256
                r = g = b = base if run_index < 3 else (base + 128) % 256
            else:
                r = (x * 5 + y * 3) % 256
                g = (x * 11 + y * 7) % 256
                b = (x * 3 + y * 17) % 256
            px[i], px[i + 1], px[i + 2] = r, g, b
    return encode_png(w, h, 3, px)


def _make_binary(path: Path, sha: str, branch: str = "claude/selftest") -> Path:
    """A file carrying a REAL, parseable build-fingerprint stamp -- the same
    .rdata layout MSVC emits for the printf in code/mechcmd2.cpp. The identity
    blocks below are then produced by the SHIPPING emitter
    (manifest_schema.exe_block) reading these actual bytes, so the provenance in
    the synthetic capture manifests is genuine, not typed in."""
    path.write_bytes(pg._synthetic_binary(sha, branch, "2026-07-30T00:00:00",
                                          "A:/Games/selftest", dirty=False))
    return path


def _make_run(root: Path, name: str, run_index: int, w: int = W, h: int = H,
              exe: "Path | None" = None, legacy: bool = False,
              share_png: "Path | None" = None) -> Path:
    """One synthetic capture run.

    exe        -- binary whose identity block the manifest carries. None with
                  legacy=False is a programming error in this file.
    legacy     -- emit the pre-CAPTURE-PROVENANCE-TRUTH-1 shape (path + mtime,
                  no identity block). This is what T7 feeds bless.
    share_png  -- point `primary_png` at somebody else's frame instead of
                  writing our own (T9).
    """
    rd = root / name
    pose_dir = rd / BM
    pose_dir.mkdir(parents=True, exist_ok=True)
    if share_png is not None:
        png = share_png
    else:
        png = pose_dir / f"{BM}.png"
        png.write_bytes(_frame(run_index, w, h))
    doc = {
        "kind": "visual-verify-capture", "schema_v": 1 if legacy else 2,
        "install": "SELFTEST",
        "exe": str(exe) if exe else "SELFTEST/mc2.exe",
        "exe_mtime": "2026-01-01T00:00:00Z",
        "frame": 431, "settle": 150, "deterministic_clock": True,
        "effective_mc2_env": ["MC2_SMOKE_MODE", "MC2_VISUAL_BOOKMARK_CAPTURE"],
        "poses": [{"bookmark": BM, "mission": "selftest",
                   "pose_dir": str(pose_dir), "primary_png": str(png),
                   "engine_deterministic": True, "all_present": True,
                   "engine_capture_fired": True}],
        "n_captured": 1, "n_requested": 1,
    }
    if not legacy:
        ms.attach(doc, ms.identity_block(generator="bless_selftest",
                                         exe_path=exe, repo_root=None,
                                         deploy_target="SELFTEST"))
        # PROVENANCE-FORGERY-CLOSE-1. A real capture records the CAPTURING
        # worktree's HEAD. Stamp a distinctive one here so T10 below can tell
        # whether the golden carried THIS forward or substituted the blesser's.
        # It is set to the same commit the synthetic binary is stamped with,
        # which is the HONEST case: the lane that captured is the lane that
        # built. The blesser is a different lane -- the normal arrangement, and
        # the one the old code mis-flagged as SOURCE-HEAD-MISMATCH.
        cap_head = dict(ms.git_block(None))
        cap_head.update({"commit": CAPTURE_LANE_HEAD,
                         "commit_short": CAPTURE_LANE_HEAD[:12],
                         "branch": "claude/capture-lane-selftest",
                         "dirty": False, "worktree": "SELFTEST/capture-lane"})
        doc["identity"]["source_worktree_head_at_capture"] = cap_head
        doc["identity"]["git"] = cap_head
        doc["identity"]["binary_provenance"] = ms.binary_provenance(
            doc["identity"].get("exe"), cap_head)
    (rd / "capture_run.json").write_text(json.dumps(doc, indent=2),
                                         encoding="utf-8")
    return rd


FAILURES = []


def check(label: str, ok: bool, detail: str = "") -> None:
    print(f"  [{'PASS' if ok else 'FAIL'}] {label}" + (f" -- {detail}" if detail else ""))
    if not ok:
        FAILURES.append(label)


def expect_refusal(label: str, fn, needle: str) -> None:
    try:
        fn()
    except SystemExit as e:
        msg = str(e)
        check(label, needle.lower() in msg.lower(),
              f"refused with: {msg.strip()[:130]}")
        return
    except Exception as e:  # pragma: no cover
        check(label, False, f"raised {type(e).__name__}: {e}")
        return
    check(label, False, "bless SUCCEEDED but should have refused")


def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="bless_selftest_"))
    orig_regions_dir = regionlib.REGIONS_DIR
    try:
        rdir = tmp / "regions"
        rdir.mkdir()
        (rdir / f"{BM}.json").write_text(json.dumps(MANIFEST, indent=2),
                                         encoding="utf-8")
        regionlib.REGIONS_DIR = rdir

        exe = _make_binary(tmp / "engine_a.bin", "a1b2c3d4e5f6")
        exe_other = _make_binary(tmp / "engine_b.bin", "ffffffff0000",
                                 branch="claude/other-lane")

        r1 = _make_run(tmp, "r1", 1, exe=exe)
        r2 = _make_run(tmp, "r2", 2, exe=exe)
        r3 = _make_run(tmp, "r3", 3, exe=exe)

        print("bless_selftest: proving each refusal fires on synthetic bad input")

        # T1 -- two runs is not enough.
        expect_refusal("T1 MIN_RUNS: a TWO-run set is refused",
                       lambda: blesslib.bless([r1, r2], tmp / "g_t1"),
                       "minimum is 3")

        # T2 -- duplicate run dirs are self-comparison.
        expect_refusal("T2 duplicate run dirs refused",
                       lambda: blesslib.bless([r1, r1, r1], tmp / "g_t2"),
                       "more than once")

        # T5 -- mixed capture resolution.
        r_odd = _make_run(tmp, "r_odd", 1, w=200, h=100, exe=exe)
        expect_refusal("T5 mixed capture resolution refused",
                       lambda: blesslib.bless([r1, r2, r_odd], tmp / "g_t5"),
                       "resolution")

        # ---- GOLDEN-PROVENANCE-TRUTH-1 -----------------------------------
        # T7 -- the exact shape of the manifest that minted the committed
        # v0.5.1-baseline golden: exe named by path + mtime, no digest, no
        # embedded build stamp. Unattributable in, unattributable out.
        l1 = _make_run(tmp, "l1", 1, legacy=True)
        l2 = _make_run(tmp, "l2", 2, legacy=True)
        l3 = _make_run(tmp, "l3", 3, legacy=True)
        expect_refusal("T7 path+mtime capture (no binary identity) refused",
                       lambda: blesslib.bless([l1, l2, l3], tmp / "g_t7"),
                       "not an identity")

        # T8 -- three runs, two different binaries. R1 would have blessed this
        # and recorded the resulting all-pairs number as run-to-run NOISE.
        x3 = _make_run(tmp, "x3", 3, exe=exe_other)
        expect_refusal("T8 runs that measured DIFFERENT binaries refused",
                       lambda: blesslib.bless([r1, r2, x3], tmp / "g_t8"),
                       "did NOT measure the same binary")

        # T9 -- three manifests, one set of frames: R1's duplicate-directory
        # rule does not see it, and every pair scores a perfect 0.0000%.
        shared = tmp / "r1" / BM / f"{BM}.png"
        s2 = _make_run(tmp, "s2", 2, exe=exe, share_png=shared)
        s3 = _make_run(tmp, "s3", 3, exe=exe, share_png=shared)
        expect_refusal("T9 runs sharing ONE primary PNG refused",
                       lambda: blesslib.bless([r1, s2, s3], tmp / "g_t9"),
                       "same primary PNG")

        # T6 (+T3, +T4) -- the clean 3-run path.
        g = blesslib.bless([r1, r2, r3], tmp / "g_ok")
        blessed = {m["region"] for m in g["members"]}
        refused = {r["region"]: r for r in g["refused"]}

        # T3 -- degenerate all-black region refused, NOT blessed.
        check("T3 degenerate all-black region refused",
              "void" in refused and "void" not in blessed,
              refused.get("void", {}).get("reason", "")[:100])
        check("T3 degeneracy cited unique-colour floor",
              refused.get("void", {}).get("rule") == "DEGENERATE-CROP-REFUSAL-1"
              and refused.get("void", {}).get("stats", {}).get("unique_colors") == 1)
        check("T3 no crop file written for the refused region",
              not (tmp / "g_ok" / BM / "void.png").exists())

        # T4 -- the region a TWO-run check would have blessed.
        check("T4 unstable region refused on the 3-run all-pairs rule",
              "unstable" in refused and "unstable" not in blessed,
              refused.get("unstable", {}).get("reason", "")[:100])
        rep = refused.get("unstable", {}).get("reproducibility", {})
        check("T4 pair r1/r2 scored 0.0000% (a two-run bless would have PASSED it)",
              rep.get("r1_vs_r2_changed_pct") == 0.0,
              f"r1_vs_r2={rep.get('r1_vs_r2_changed_pct')} "
              f"r1_vs_r3={rep.get('r1_vs_r3_changed_pct')} "
              f"r2_vs_r3={rep.get('r2_vs_r3_changed_pct')}")
        check("T4 all 3 pairs were graded", rep.get("pairs_graded") == 3)

        # T6 -- the good region blesses, with tool-emitted provenance.
        check("T6 stable textured region blessed", "good" in blessed)
        check("T6 crop file written", (tmp / "g_ok" / BM / "good.png").exists())
        good = next(m for m in g["members"] if m["region"] == "good")
        check("T6 blessed member carries all-pairs reproducibility",
              good["reproducibility"]["pairs_graded"] == 3
              and good["reproducibility"]["runs"] == 3)
        # T6 provenance: the binary, read out of the binary.
        bp = (g.get("identity") or {}).get("binary_provenance") or {}
        check("T6 provenance names the BINARY's commit, read from its bytes",
              bp.get("commit") == "a1b2c3d4e5f6"
              and bp.get("commit_source") == "exe-embedded-build-fingerprint",
              f"commit={bp.get('commit')} source={bp.get('commit_source')}")
        check("T6 golden identifies its binary by CONTENT (sha256), not mtime",
              isinstance(g.get("exe_sha256"), str) and len(g["exe_sha256"]) == 64,
              str(g.get("exe_sha256"))[:16])
        # THE DEFECT THIS SLICE REMOVED: `blessed_commit` was the BLESSER's
        # worktree HEAD sitting at the top level of the most authoritative
        # artifact in the repo, indistinguishable from provenance. It must be
        # GONE -- not renamed in place, not null -- so a reader looking for it
        # fails to find it and goes to identity.binary_provenance instead.
        check("T6 top-level `blessed_commit` (source HEAD as provenance) is GONE",
              "blessed_commit" not in g, str(sorted(g.keys()))[:120])
        swh = g.get("source_worktree_head_at_bless") or {}
        check("T6 blesser's HEAD kept, but named + labelled describes_binary=false",
              swh.get("describes_binary") is False
              and swh.get("describes") == "source_worktree_head_at_bless",
              f"commit={str(swh.get('commit'))[:12]}")
        check("T6 the golden ESTABLISHES its own provenance (gate agrees)",
              pg.establish(g, "golden")["ok"] is True,
              pg.establish(g, "golden")["level"])
        check("T6 provenance tool-emitted: capture_env.resolution",
              g["capture_env"]["resolution"] == f"{W}x{H}")
        check("T6 provenance tool-emitted: capture_env.runs == 3",
              g["capture_env"]["runs"] == 3)
        check("T6 provenance tool-emitted: gate_state present",
              "gate_state" in g and "effective_mc2_env" in g["gate_state"])
        check("T6 provenance names its emitter",
              "bless.py" in (g.get("provenance_emitted_by") or ""))
        check("T6 refusals are recorded in golden.json, not silently dropped",
              g["n_refused"] == 2 and len(g["refused"]) == 2)

        # ------------------------------------------------------------------ #
        # T10 -- PROVENANCE-FORGERY-CLOSE-1: bless.py was WRITING the defect
        # it named. `ms.identity_block(repo_root=str(ROOT))` filled
        # `source_worktree_head_at_capture`, its alias `git`, and
        # `binary_provenance` from the BLESSER'S worktree -- two of the exact
        # spellings provenance_gate lists under "none of them describe the
        # binary", one of them named `..._at_capture` while describing neither
        # the capture's worktree nor its moment. T6 above could not see it: it
        # only checked that the top-level `blessed_commit` was gone, i.e. the
        # field that was deleted rather than the mechanism that filled it.
        # ------------------------------------------------------------------ #
        ident = g.get("identity") or {}
        blesser_head = blesslib._git(blesslib.ROOT, "rev-parse", "HEAD")
        capture_recorded = (ident.get("source_worktree_head_at_capture") or {})
        check("T10 control: blesser's HEAD really is a DIFFERENT commit from "
              "the capture lane's (else T10 proves nothing)",
              bool(blesser_head) and blesser_head != CAPTURE_LANE_HEAD,
              f"blesser={str(blesser_head)[:12]} capture={CAPTURE_LANE_HEAD[:12]}")
        check("T10 `source_worktree_head_at_capture` carries the CAPTURE's head, "
              "not the blesser's",
              capture_recorded.get("commit") == CAPTURE_LANE_HEAD,
              f"recorded={str(capture_recorded.get('commit'))[:12]}")
        check("T10 legacy alias `identity.git` agrees with it (not the blesser)",
              (ident.get("git") or {}).get("commit") == CAPTURE_LANE_HEAD,
              f"recorded={str((ident.get('git') or {}).get('commit'))[:12]}")
        blesser_spellings = [k for k, v in ident.items()
                             if isinstance(v, dict) and v.get("commit") == blesser_head]
        check("T10 the blesser's HEAD appears in NO identity field at all",
              not blesser_spellings, f"found in {blesser_spellings}")
        check("T10 the blesser's HEAD is still recorded ONCE, honestly named",
              (g.get("source_worktree_head_at_bless") or {}).get("commit")
              == blesser_head)
        # The false alarm: reconciling the binary against the BLESSER's head
        # stamped SOURCE-HEAD-MISMATCH on every honest cross-lane golden.
        check("T10 an HONEST cross-lane golden reconciles instead of being "
              "stamped SOURCE-HEAD-MISMATCH",
              bp.get("verdict") == "reconciled"
              and bp.get("source_head_commit") == CAPTURE_LANE_HEAD,
              f"verdict={bp.get('verdict')} "
              f"vs_head={str(bp.get('source_head_commit'))[:12]}")

        # ------------------------------------------------------------------ #
        # T11 -- the golden must name INDEPENDENT witnesses, because
        # provenance_gate.establish() no longer grades a record that only
        # agrees with itself.
        # ------------------------------------------------------------------ #
        bc = ident.get("binary_corroboration") or {}
        wits = bc.get("witnesses") or []
        check("T11 golden names its source capture manifests as witnesses",
              len(wits) == 3, f"{len(wits)} witness(es)")
        check("T11 every witness is pinned by its OWN sha256 and records the "
              "same exe digest",
              all(isinstance(w.get("artifact_sha256"), str)
                  and len(w["artifact_sha256"]) == 64
                  and w.get("records_exe_sha256") == g.get("exe_sha256")
                  for w in wits))
        check("T11 witnesses are files that actually exist on disk",
              all(Path(w["path"]).exists() for w in wits))
        # And the whole point: with the binary DELETED, the witnesses are what
        # keeps a legitimate golden gradeable. This is the "goldens outlive
        # their binaries" case that a refuse-everything gate would break.
        Path(exe).unlink()
        est_gone = pg.establish(g, "golden-after-binary-deleted")
        check("T12 with its binary DELETED the golden still grades, on the "
              "strength of its witnesses",
              est_gone["ok"] and est_gone["level"] == "CORROBORATED-AT-MINT",
              est_gone["level"])
        # ...and strip the witnesses and it does NOT. Same record, same missing
        # binary: the only difference is whether anything outside it agrees.
        g_naked = json.loads(json.dumps(g))
        g_naked["identity"].pop("binary_corroboration", None)
        est_naked = pg.establish(g_naked, "golden-witnesses-stripped")
        check("T12 strip the witnesses and the same record is REFUSED "
              "(so T12 above is the witnesses, not luck)",
              not est_naked["ok"] and est_naked["level"] == "UNCORROBORATED",
              est_naked["level"])

        print()
        if FAILURES:
            print(f"bless_selftest: FAIL ({len(FAILURES)} guard(s) did not fire): "
                  + ", ".join(FAILURES))
            return 1
        print("bless_selftest: OK -- every refusal fired on its synthetic bad input")
        return 0
    finally:
        regionlib.REGIONS_DIR = orig_regions_dir
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
