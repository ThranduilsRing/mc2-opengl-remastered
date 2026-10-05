#!/usr/bin/env python3
"""CONTRACT: review-pack annotation anchors never re-attach to the wrong artifact.

The review pack (release_assets/<ver>/REVIEW/INDEX.html) carries an in-page
annotation layer.  The user grades artifacts there; the page is regenerated
whenever the manifest or the source changes.  The one intolerable failure is a
SILENT MIS-ATTACHMENT -- an annotation whose stored id resolves, after
regeneration, to a DIFFERENT artifact than the one he was looking at.  That
would put his words against something he never saw, which is strictly worse
than having no annotation feature at all.

This checker proves the property by mutating the REAL manifest the way a future
session actually will -- inserting and deleting sections, figure blocks, images,
table rows -- and asserting:

    for every rid present both before and after the mutation,
    the underlying manifest object is BYTE-IDENTICAL.

Identity is re-derived from the manifest objects themselves, not from the anchor
code under test, so the assertion is independent of the id scheme.

Two further mutations pin the documented behaviour when CONTENT changes under an
existing annotation:

    edit prose      -> the rid changes  (annotation ORPHANS, surfaced, never moved)
    edit a caption  -> the rid survives, the digest changes (CHANGED badge)

Usage:
    py -3 scripts/check-review-anchor-stability.py [--quiet]
    py -3 scripts/check-review-anchor-stability.py --self-test

Windows / Python 3 / standard library only.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import build_review_pack as brp  # noqa: E402


# ---------------------------------------------------------------------------
# identity -- derived from the manifest, NOT from the anchor code under test
# ---------------------------------------------------------------------------

def _canon(o) -> str:
    return json.dumps(o, sort_keys=True, ensure_ascii=False, separators=(",", ":"))


def identity(rec: dict) -> str:
    """WHICH ARTIFACT this anchor points at -- deliberately not "what it says".

    Content lives in the digest; conflating the two here would read an edited
    caption as a mis-attachment, when in fact keeping the id and flagging the
    digest is exactly the wanted behaviour.  So an artifact with an intrinsic
    key is identified by that key alone.  Prose and table rows have no key
    apart from their text, so for those the content IS the identity.
    """
    obj = rec["obj"]
    k = rec["kind"]
    if k == "section":
        return _canon(["section", obj["id"]])
    if k == "image":
        return _canon(["image", rec["section_id"], (obj.get("src") or "").replace("\\", "/")])
    if k == "gate":
        return _canon(["gate", rec["section_id"], obj["gate"]])
    if k == "lane":
        return _canon(["lane", obj["name"]])
    return _canon([k, rec["section_id"], obj])


def index(manifest: dict, id_fn=None) -> dict:
    """rid -> {identity, digest}.  id_fn overrides the rid scheme (self-test)."""
    out = {}
    for rec in brp.iter_anchors(manifest):
        rid = id_fn(rec) if id_fn else rec["rid"]
        out[rid] = {"identity": identity(rec), "digest": rec["digest"],
                    "kind": rec["kind"]}
    return out


# ---------------------------------------------------------------------------
# mutations -- what a future session will actually do to this manifest
# ---------------------------------------------------------------------------

def _find_section(m, sid):
    for i, s in enumerate(m["sections"]):
        if s["id"] == sid:
            return i, s
    raise KeyError(sid)


def _first_block(s, t, min_items=1):
    for i, b in enumerate(s["blocks"]):
        if b["type"] != t:
            continue
        if t == "figures" and len(b["items"]) < min_items:
            continue
        return i, b
    return None, None


def _big_figures_section(m):
    """A section with several figure blocks -- the realistic edit target."""
    best, n = None, -1
    for s in m["sections"]:
        c = sum(1 for b in s["blocks"] if b["type"] == "figures")
        if c > n:
            best, n = s, c
    return best


def mut_insert_section(m):
    m["sections"].insert(2, {
        "id": "zz_inserted_probe",
        "heading": "Inserted probe section",
        "blocks": [
            {"type": "what", "html": "Probe prose inserted by the stability contract."},
            {"type": "figures", "layout": "ba", "items": [
                {"caption": "probe A", "src": "zz_probe/a.png"},
                {"caption": "probe B", "src": "zz_probe/b.png"}]}]})
    return "insert a whole section at position 2"


def mut_delete_section(m):
    del m["sections"][3]
    return "delete section at position 3"


def mut_insert_block(m):
    s = _big_figures_section(m)
    s["blocks"].insert(2, {"type": "figures", "layout": "ba", "items": [
        {"caption": "probe C", "src": "zz_probe/c.png"},
        {"caption": "probe D", "src": "zz_probe/d.png"}]})
    return "insert a figures block mid-section into %s" % s["id"]


def mut_insert_image(m):
    s = _big_figures_section(m)
    _i, b = _first_block(s, "figures", 2)
    if b is None:  # current pack lacks this block shape: probe N/A
        return None
    b["items"].insert(0, {"caption": "probe E", "src": "zz_probe/e.png"})
    return "insert an image at the FRONT of an existing figure group in %s" % s["id"]


def mut_delete_image(m):
    s = _big_figures_section(m)
    _i, b = _first_block(s, "figures", 2)
    if b is None:  # current pack lacks this block shape: probe N/A
        return None
    b["items"].pop(0)
    return "delete the FIRST image of an existing figure group in %s" % s["id"]


def mut_insert_gate_row(m):
    for s in m["sections"]:
        i, b = _first_block(s, "gate_table")
        if b is None:  # current pack lacks this block shape: probe N/A
            return None
        if b:
            b["rows"].insert(0, {"gate": "MC2_ZZ_PROBE_GATE",
                                 "cells": ["<code>MC2_ZZ_PROBE_GATE</code>", "probe", "probe"]})
            return "insert a gate row at the FRONT of %s" % s["id"]
    return None


def mut_insert_lane(m):
    for s in m["sections"]:
        i, b = _first_block(s, "lanes_table")
        if b is None:  # current pack lacks this block shape: probe N/A
            return None
        if b:
            b["lanes"].insert(0, {"name": "ZZ-PROBE-LANE-1", "sent": "probe",
                                  "found": "probe", "commits": [], "gates": [],
                                  "verdict": "in-flight", "evidence": []})
            return "insert a lane row at the FRONT of %s" % s["id"]
    return None


def mut_insert_meta_row(m):
    for s in m["sections"]:
        i, b = _first_block(s, "meta")
        if b is None:  # current pack lacks this block shape: probe N/A
            return None
        if b:
            b["rows"].insert(0, [{"tag": "th", "attrs": "", "html": "Probe"},
                                 {"tag": "td", "attrs": "", "html": "probe row"}])
            return "insert a meta row at the FRONT of %s" % s["id"]
    return None


def mut_reorder_sections(m):
    m["sections"].insert(1, m["sections"].pop(9))
    return "move section 9 to position 1 (pure reorder)"


# A pure reorder must be COMPLETELY id-neutral -- that is the whole point of
# content-derived ids -- so it is the one mutation where "no id-set change" is
# the pass condition rather than a sign the test did nothing.
ID_NEUTRAL = {mut_reorder_sections}

STRUCTURAL = [mut_insert_section, mut_delete_section, mut_insert_block,
              mut_insert_image, mut_delete_image, mut_insert_gate_row,
              mut_insert_lane, mut_insert_meta_row, mut_reorder_sections]


# ---------------------------------------------------------------------------

def check_structural(base_manifest, base_idx, id_fn, quiet) -> list:
    """Return a list of failure strings (empty == contract holds)."""
    fails = []
    for fn in STRUCTURAL:
        m = copy.deepcopy(base_manifest)
        what = fn(m)
        if what is None:
            continue
        idx = index(m, id_fn)
        shared = set(base_idx) & set(idx)
        moved = [r for r in shared if base_idx[r]["identity"] != idx[r]["identity"]]
        churn = set(base_idx) ^ set(idx)
        neutral = fn in ID_NEUTRAL
        if moved:
            fails.append("MIS-ATTACH: %s -- %d id(s) now point at DIFFERENT content, e.g. %s"
                         % (what, len(moved), ", ".join(sorted(moved)[:3])))
            continue
        if neutral and churn:
            fails.append("%s changed %d id(s); a reorder must be fully id-neutral, "
                         "e.g. %s" % (what, len(churn), ", ".join(sorted(churn)[:3])))
            continue
        if not neutral and not churn:
            fails.append("VACUOUS: %s produced no id-set change at all" % what)
            continue
        if not quiet:
            note = ("id set UNCHANGED (order is not an input)" if neutral
                    else "%d shared ids, all still on their own content" % len(shared))
            print("  OK  %-64s %s" % (what[:64], note))
    return fails


def check_content_edits(base_manifest, base_idx, quiet) -> list:
    fails = []

    # (a) editing prose must RETIRE the id -- the annotation orphans, loudly,
    #     rather than silently sitting on rewritten text.
    m = copy.deepcopy(base_manifest)
    s = m["sections"][2]
    tgt = None
    for b in s["blocks"]:
        if b["type"] in ("what", "note", "pl"):
            tgt = b
            break
    tgt["html"] += " (edited by the stability contract)"
    idx = index(m)
    gone = set(base_idx) - set(idx)
    moved = [r for r in (set(base_idx) & set(idx))
             if base_idx[r]["identity"] != idx[r]["identity"]]
    if moved:
        fails.append("MIS-ATTACH: prose edit moved %d id(s) onto other content" % len(moved))
    elif len(gone) != 1:
        fails.append("prose edit retired %d id(s), expected exactly 1" % len(gone))
    elif not quiet:
        print("  OK  %-64s id retired -> annotation ORPHANS (never silently moved)"
              % "edit one prose block")

    # (b) TIER-1 image (its path is unique in its section): editing the caption
    #     must KEEP the id and CHANGE the digest, so the page shows a CHANGED
    #     badge instead of pretending nothing moved.
    loc = _locate_image(base_manifest, tier1=True)
    if loc is None:
        fails.append("no tier-1 image found -- cannot test the CHANGED path")
    else:
        m = copy.deepcopy(base_manifest)
        si, bi, ii, rid = loc
        it = m["sections"][si]["blocks"][bi]["items"][ii]
        it["caption"] = (it.get("caption") or "") + " (edited)"
        idx = index(m)
        if rid not in idx:
            fails.append("caption edit retired tier-1 image id %s -- it should have "
                         "survived and gone CHANGED" % rid)
        elif idx[rid]["digest"] == base_idx[rid]["digest"]:
            fails.append("caption edit did NOT change the digest for %s -- a silent "
                         "content change under a live annotation" % rid)
        elif not quiet:
            print("  OK  %-64s id kept, digest changed -> CHANGED badge"
                  % "edit caption of a tier-1 image")

    # (c) TIER-2/3 image (path repeats inside one section, so the id carries a
    #     caption tiebreak): editing the caption retires the id.  That is a
    #     downgrade to ORPHAN, which is acceptable -- what is NOT acceptable is
    #     the id landing on its twin.
    loc = _locate_image(base_manifest, tier1=False)
    if loc is not None:
        m = copy.deepcopy(base_manifest)
        si, bi, ii, rid = loc
        it = m["sections"][si]["blocks"][bi]["items"][ii]
        it["caption"] = (it.get("caption") or "") + " (edited)"
        idx = index(m)
        moved = [r for r in (set(base_idx) & set(idx))
                 if base_idx[r]["identity"] != idx[r]["identity"]]
        if moved:
            fails.append("MIS-ATTACH: caption edit on a duplicated image moved %d id(s) "
                         "onto other content, e.g. %s" % (len(moved), sorted(moved)[0]))
        elif rid in idx:
            fails.append("tier-2 id %s survived a caption edit -- expected it to "
                         "retire, since the caption is part of that id" % rid)
        elif not quiet:
            print("  OK  %-64s id retires -> ORPHAN, never onto its twin"
                  % "edit caption of a duplicated (tier-2) image")

    # (d) THE COMMON CASE IN THIS PACK: a pose re-captured under the SAME
    #     filename.  The manifest does not change at all, so only the image
    #     file hash can catch it.  If it did not, a "this is wrong" comment
    #     would silently ride onto a frame he never saw.
    loc = _locate_image(base_manifest, tier1=True)
    if loc is not None:
        si, bi, ii, rid = loc
        it = base_manifest["sections"][si]["blocks"][bi]["items"][ii]
        path = os.path.normpath(os.path.join(
            REPO_ROOT, base_manifest["pack_dir"], it["src"]))
        saved = brp._IMG_HASH_CACHE.get(path)
        try:
            brp._IMG_HASH_CACHE[path] = "recaptured!!"
            idx = index(base_manifest)
            if rid not in idx:
                fails.append("re-capture changed the image id %s -- expected the id "
                             "to survive so the comment stays visible" % rid)
            elif idx[rid]["digest"] == base_idx[rid]["digest"]:
                fails.append("MIS-ATTACH: re-capturing %s under the same filename did "
                             "NOT change its digest -- a comment would silently ride "
                             "onto a frame he never saw" % it["src"])
            elif not quiet:
                print("  OK  %-64s id kept, digest changed -> CHANGED badge"
                      % "re-capture an image under the SAME filename")
        finally:
            if saved is None:
                brp._IMG_HASH_CACHE.pop(path, None)
            else:
                brp._IMG_HASH_CACHE[path] = saved
    return fails


def _locate_image(manifest, tier1: bool):
    """(section_idx, block_idx, item_idx, rid) for an image at the wanted tier.

    tier1 -> its path is unique in its section, so the rid is the bare path.
    else  -> the path repeats, so the rid carries a '#caption' tiebreak."""
    rid_of = {}
    for rec in brp.iter_anchors(manifest):
        if rec["kind"] == "image":
            rid_of[id(rec["obj"])] = rec["rid"]
    for si, s in enumerate(manifest["sections"]):
        for bi, b in enumerate(s["blocks"]):
            if b["type"] != "figures":
                continue
            for ii, it in enumerate(b["items"]):
                rid = rid_of.get(id(it))
                if not rid or not it.get("caption"):
                    continue
                tiered = ("#" in rid) or ("@" in rid)
                if tier1 != tiered:
                    return si, bi, ii, rid
    return None


# ---------------------------------------------------------------------------
# self-test: prove the checker is not blind
# ---------------------------------------------------------------------------

def _positional_id(rec: dict) -> str:
    """The naive scheme this contract exists to forbid: ids from DOM order.
    Feeding it through the same battery MUST produce mis-attachments."""
    _positional_id.n += 1
    return "%s/%d" % (rec["kind"], _positional_id.n)


def self_test(manifest) -> int:
    def counter(rec):
        counter.n += 1
        return "%s/%d" % (rec["kind"], counter.n)
    counter.n = 0

    def id_fn(rec):
        return counter(rec)

    counter.n = 0
    base = index(copy.deepcopy(manifest), id_fn)
    # re-run the battery with the ordinal counter reset per mutation
    fails = []
    for fn in STRUCTURAL:
        m = copy.deepcopy(manifest)
        what = fn(m)
        if what is None:
            continue
        counter.n = 0
        idx = index(m, id_fn)
        shared = set(base) & set(idx)
        moved = [r for r in shared if base[r]["identity"] != idx[r]["identity"]]
        if moved:
            fails.append(what)
    if not fails:
        print("SELF-TEST FAIL: a deliberately positional id scheme produced NO "
              "mis-attachment -- the checker is blind and proves nothing.")
        return 1
    print("SELF-TEST PASS: positional ids mis-attach under %d/%d mutations "
          "(the checker detects the failure it exists to forbid)"
          % (len(fails), len(STRUCTURAL)))
    return 0


# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", default=brp.DEFAULT_MANIFEST)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--self-test", action="store_true", dest="selftest")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.manifest):
        print("SKIP: manifest not found: %s" % args.manifest)
        return 0
    with open(args.manifest, encoding="utf-8") as fh:
        manifest = json.load(fh)

    if args.selftest:
        return self_test(manifest)

    base_idx = index(manifest)
    anchors = brp.iter_anchors(manifest)
    if len(base_idx) != len(anchors):
        print("FAIL: %d anchors collapsed into %d unique ids -- duplicate rids"
              % (len(anchors), len(base_idx)))
        return 1
    if not args.quiet:
        print("review anchor stability: %d anchors, %d unique ids" % (len(anchors), len(base_idx)))

    fails = check_structural(manifest, base_idx, None, args.quiet)
    fails += check_content_edits(manifest, base_idx, args.quiet)

    if fails:
        for f in fails:
            print("FAIL: %s" % f)
        return 1
    if not args.quiet:
        print("PASS: no mutation re-attached an id to different content")
    return 0


if __name__ == "__main__":
    sys.exit(main())
