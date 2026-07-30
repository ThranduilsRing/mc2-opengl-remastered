#!/usr/bin/env python3
"""Turn an exported review-pack annotation file into a lane-dispatchable list.

The user grades the review pack in his browser (release_assets/<ver>/REVIEW/
INDEX.html) and clicks EXPORT JSON.  That file -- schema "mc2.review.feedback/1",
see docs/review-feedback-schema.md -- is the input here.  This script:

  * validates it against the schema,
  * re-derives the pack's anchors from the TRACKED manifest via
    build_review_pack.iter_anchors(), so writer and reader can never disagree
    about what an id means,
  * classifies every annotation as ATTACHED / CHANGED / ORPHANED by comparing
    the stored digest against the current one,
  * prints a prioritised worklist an orchestrator can dispatch straight from.

Priority, highest first:
    BAD                 something he looked at is wrong
    NOT WHAT I MEANT    a lane solved the wrong problem -- re-read before coding
    ALREADY REPORTED    a regression or an unshipped fix
    (no verdict)        a comment with no grade
    GOOD                confirmations; no work implied

Usage:
    py -3 scripts/read_review_feedback.py FEEDBACK.json
    py -3 scripts/read_review_feedback.py FEEDBACK.json --json
    py -3 scripts/read_review_feedback.py FEEDBACK.json --verdict bad --verdict notit
    py -3 scripts/read_review_feedback.py --selftest

Windows / Python 3 / standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import build_review_pack as brp  # noqa: E402

FEEDBACK_SCHEMA = "mc2.review.feedback/1"

# lower sorts first
PRIORITY = {"bad": 0, "notit": 1, "known": 2, None: 3, "good": 4}
VERDICT_LABEL = {
    "bad": "BAD",
    "notit": "NOT WHAT I MEANT",
    "known": "ALREADY REPORTED",
    "good": "GOOD",
    None: "(no verdict)",
}
ACTION = {
    "bad": "FIX -- he looked at this and it is wrong",
    "notit": "RE-READ FIRST -- a lane solved the wrong problem here; do not code yet",
    "known": "CHECK SHIPPED STATE -- he has reported this before",
    None: "TRIAGE -- comment with no verdict",
    "good": "NO WORK -- confirmed good, keep it from regressing",
}


# ---------------------------------------------------------------------------

def load_feedback(path: str) -> dict:
    with open(path, encoding="utf-8") as fh:
        d = json.load(fh)
    if not isinstance(d, dict) or not isinstance(d.get("annotations"), list):
        raise SystemExit("ERROR: %s has no 'annotations' array -- not a feedback export" % path)
    if d.get("schema") != FEEDBACK_SCHEMA:
        print("WARNING: schema is %r, expected %r -- reading anyway"
              % (d.get("schema"), FEEDBACK_SCHEMA), file=sys.stderr)
    return d


def load_anchors(manifest_path: str) -> dict:
    with open(manifest_path, encoding="utf-8") as fh:
        manifest = json.load(fh)
    return {a["rid"]: a for a in brp.iter_anchors(manifest)}


def classify(a: dict, anchors: dict) -> str:
    """ATTACHED / CHANGED / ORPHANED, recomputed against the CURRENT manifest.

    The status the browser stored is only true for the page it was written in;
    the manifest may have moved on since.  Recomputing here is what stops a
    stale comment being dispatched as if it still described the current pack.
    """
    live = anchors.get(a.get("id"))
    if live is None:
        return "ORPHANED"
    stored = (a.get("anchor") or {}).get("digest")
    if stored and stored != live["digest"]:
        return "CHANGED"
    return "ATTACHED"


def region_text(r: dict) -> str:
    box = "x%.3f y%.3f w%.3f h%.3f" % (r.get("x", 0), r.get("y", 0),
                                       r.get("w", 0), r.get("h", 0))
    note = (r.get("note") or "").strip()
    return "%s%s" % (box, ("  -- " + note) if note else "")


def build_items(fb: dict, anchors: dict, want: set | None, pack_dir: str = "") -> list:
    items = []
    for a in fb["annotations"]:
        v = a.get("verdict")
        if want and (v or "none") not in want:
            continue
        an = a.get("anchor") or {}
        live = anchors.get(a.get("id"))
        src = an.get("src") or (live or {}).get("src")
        items.append({
            "id": a.get("id"),
            "kind": a.get("kind"),
            "verdict": v,
            "verdict_label": VERDICT_LABEL.get(v, str(v)),
            "action": ACTION.get(v, "TRIAGE"),
            "attachment": classify(a, anchors),
            "comment": (a.get("comment") or "").strip(),
            "regions": a.get("regions") or [],
            "section_id": an.get("section_id") or (live or {}).get("section_id"),
            "section_heading": an.get("section_heading"),
            "heading": an.get("heading") or (live or {}).get("heading"),
            "label": an.get("label") or (live or {}).get("label"),
            # absolute, so a lane can open it without knowing the pack layout
            "image": image_path(pack_dir, src) if src else None,
            "image_rel": src,
            "quoted_text": an.get("text"),
            "updated_utc": a.get("updated_utc"),
        })
    items.sort(key=lambda i: (PRIORITY.get(i["verdict"], 3),
                              i["attachment"] != "ATTACHED",
                              str(i["section_id"]), str(i["id"])))
    return items


def image_path(pack_dir: str, src: str) -> str:
    return os.path.normpath(os.path.join(pack_dir, src)).replace("\\", "/")


def report(fb: dict, items: list) -> None:
    pack = fb.get("pack") or {}
    print("=" * 78)
    print("REVIEW FEEDBACK  --  %s (pack %s)" % (pack.get("title", "?"), pack.get("id", "?")))
    print("exported %s   from %s" % (fb.get("exported_utc", "?"), fb.get("page_url", "?")))
    print("=" * 78)

    by_v, by_att = {}, {}
    for i in items:
        by_v[i["verdict_label"]] = by_v.get(i["verdict_label"], 0) + 1
        by_att[i["attachment"]] = by_att.get(i["attachment"], 0) + 1
    print("%d annotation(s):  %s" % (
        len(items), ",  ".join("%d %s" % (n, k) for k, n in sorted(by_v.items(), key=lambda x: -x[1]))))
    print("attachment     :  %s" % ",  ".join("%d %s" % (n, k) for k, n in sorted(by_att.items())))
    stale = by_att.get("CHANGED", 0) + by_att.get("ORPHANED", 0)
    if stale:
        print()
        print("!! %d annotation(s) no longer match the current manifest.  Their text is" % stale)
        print("   still his, but the artifact moved on -- re-read before acting on them.")
    print()

    cur = None
    for n, i in enumerate(items, 1):
        if i["verdict_label"] != cur:
            cur = i["verdict_label"]
            print("-" * 78)
            print("%s  --  %s" % (cur, i["action"]))
            print("-" * 78)
        tag = "" if i["attachment"] == "ATTACHED" else "  [%s]" % i["attachment"]
        print("%2d. %s%s" % (n, i["label"] or i["id"], tag))
        where = " / ".join(x for x in (i["section_id"], i["heading"]) if x)
        if where:
            print("    where   : %s" % where)
        print("    id      : %s" % i["id"])
        if i["image"]:
            print("    image   : %s" % i["image"])
        if i["comment"]:
            for ln in i["comment"].splitlines():
                print("    says    : %s" % ln)
        for ri, r in enumerate(i["regions"], 1):
            print("    box %-3s : %s" % (ri, region_text(r)))
        if not i["comment"] and not i["regions"]:
            print("    says    : (verdict only, no words)")
        print()

    if not items:
        print("(nothing matched)")


# ---------------------------------------------------------------------------

def selftest() -> int:
    """Prove the reader classifies attachment correctly without needing a
    hand-made file: build a synthetic export from the REAL anchors, then bend
    one digest and invent one id."""
    anchors = load_anchors(brp.DEFAULT_MANIFEST)
    imgs = [a for a in anchors.values() if a["kind"] == "image"][:2]
    if len(imgs) < 2:
        print("SELF-TEST FAIL: pack has fewer than 2 images")
        return 1
    fb = {
        "schema": FEEDBACK_SCHEMA,
        "pack": {"id": "selftest", "title": "selftest"},
        "exported_utc": "1970-01-01T00:00:00Z",
        "page_url": "file:///selftest",
        "annotations": [
            {"id": imgs[0]["rid"], "kind": "image", "verdict": "bad",
             "comment": "this concrete is too blue",
             "regions": [{"x": 0.1, "y": 0.2, "w": 0.3, "h": 0.25, "note": "here"}],
             "anchor": {"digest": imgs[0]["digest"], "src": imgs[0]["src"],
                        "section_id": imgs[0]["section_id"], "label": imgs[0]["label"]}},
            {"id": imgs[1]["rid"], "kind": "image", "verdict": "good", "comment": "",
             "regions": [], "anchor": {"digest": "0" * 16, "src": imgs[1]["src"],
                                       "section_id": imgs[1]["section_id"]}},
            {"id": "img/nope/does_not_exist.png", "kind": "image", "verdict": "notit",
             "comment": "gone", "regions": [], "anchor": {"digest": "deadbeef"}},
        ],
    }
    got = {i["id"]: i["attachment"] for i in build_items(fb, anchors, None)}
    want = {imgs[0]["rid"]: "ATTACHED", imgs[1]["rid"]: "CHANGED",
            "img/nope/does_not_exist.png": "ORPHANED"}
    bad = {k: (got.get(k), v) for k, v in want.items() if got.get(k) != v}
    if bad:
        print("SELF-TEST FAIL: attachment misclassified: %s" % bad)
        return 1
    items = build_items(fb, anchors, None)
    if items[0]["verdict"] != "bad":
        print("SELF-TEST FAIL: BAD did not sort first (got %r)" % items[0]["verdict"])
        return 1
    if not build_items(fb, anchors, {"bad"}) or len(build_items(fb, anchors, {"bad"})) != 1:
        print("SELF-TEST FAIL: --verdict filter did not select exactly the BAD item")
        return 1
    print("SELF-TEST PASS: ATTACHED/CHANGED/ORPHANED classification, BAD-first "
          "ordering and verdict filtering all behave")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("feedback", nargs="?", help="exported mc2.review.feedback/1 JSON")
    ap.add_argument("--manifest", default=brp.DEFAULT_MANIFEST)
    ap.add_argument("--json", action="store_true", help="machine-readable worklist on stdout")
    ap.add_argument("--verdict", action="append", default=[],
                    choices=["good", "bad", "notit", "known", "none"],
                    help="only these verdicts (repeatable)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.feedback:
        ap.error("a feedback JSON path is required (or --selftest)")

    # the pack's prose is full of em dashes; a cp1252 console would mangle or
    # crash on them, and a redirected --json file must be valid UTF-8.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")

    fb = load_feedback(args.feedback)
    anchors = load_anchors(args.manifest)
    with open(args.manifest, encoding="utf-8") as fh:
        pack_dir = os.path.join(REPO_ROOT, json.load(fh)["pack_dir"]).replace("\\", "/")
    items = build_items(fb, anchors, set(args.verdict) or None, pack_dir)

    if args.json:
        json.dump({"schema": "mc2.review.worklist/1",
                   "source": os.path.abspath(args.feedback).replace("\\", "/"),
                   "pack_dir": pack_dir,
                   "exported_utc": fb.get("exported_utc"),
                   "items": items}, sys.stdout, indent=1, ensure_ascii=False)
        sys.stdout.write("\n")
    else:
        report(fb, items)
    return 0


if __name__ == "__main__":
    sys.exit(main())
