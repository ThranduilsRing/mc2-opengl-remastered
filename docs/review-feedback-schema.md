# Review-pack feedback — annotation schema and how a lane consumes it

The release review pack (`release_assets/<ver>/REVIEW/INDEX.html`) is the user's
primary feedback channel. He grades artifacts in the page, clicks **EXPORT
JSON**, and hands the file back. This document defines that file and the
guarantees behind it.

Everything here is generated. Do not hand-edit `INDEX.html` — it is rebuilt from
`release_assets_src/<ver>/review_pack.json` by `scripts/build_review_pack.py`,
and `release_assets/` is gitignored. The annotation layer lives in
`release_assets_src/annotate/review_annotate.{css,js}`, inlined at render time.

```
release_assets_src/<ver>/review_pack.json   content manifest        (tracked)
release_assets_src/annotate/*.css|js        annotation layer        (tracked)
scripts/build_review_pack.py                renderer + anchor ids   (tracked)
scripts/read_review_feedback.py             export -> worklist      (tracked)
scripts/check-review-anchor-stability.py    the id contract         (tracked)
release_assets/<ver>/REVIEW/INDEX.html      the page he opens       (GENERATED)
```

## Using the page

Open `release_assets/0.5.2/REVIEW/INDEX.html` in a browser. Hover any image,
paragraph, or table row: a bar appears with **GOOD / BAD / NOT IT / KNOWN**, plus
**REGION** (images) and **COMMENT**. Keys while hovering: `g` `b` `n` `k`, `r`
box, `c` comment, `Esc` closes.

**REGION** is the one that saves the most time. Arm it, drag a box on the image,
and type what is wrong inside that box. Boxes are stored as fractions of the
image, so they survive any resolution or rescaling.

### Storage — read this before spending an hour grading

Work autosaves to `localStorage`, which is scoped to **one browser profile and
one page location**. Opening a copy from another folder, a different browser, a
private window, or clearing site data will all show **nothing**. The notes are
not gone, they are just not visible there. **Export** is the durable artifact;
the page says so next to the button. If storage is unavailable the page shows a
red `AUTOSAVE IS OFF` banner instead of failing silently.

## Export format — `mc2.review.feedback/1`

```jsonc
{
  "schema": "mc2.review.feedback/1",
  "pack": { "id": "0.5.2", "title": "...", "anchor_schema": "mc2.review.anchor/1",
            "anchors": 815, "images": 276 },
  "exported_utc": "2026-07-30T21:42:27Z",
  "page_url": "file:///A:/Games/mc2-main/release_assets/0.5.2/REVIEW/INDEX.html",
  "counts": { "annotations": 5, "regions": 1,
              "verdicts": {"good":1,"bad":2,"notit":1,"known":1,"none":0},
              "changed": 0, "orphaned": 0 },
  "annotations": [ /* see below */ ]
}
```

One annotation:

```jsonc
{
  "id": "img/01_shoreline/01_shoreline/01b_mc2_10_inlet_BEFORE_gateOFF.png",
  "kind": "image",              // image | text | row | gate | lane | section
  "verdict": "bad",             // good | bad | notit | known | null
  "comment": "Still wrong. I mean the edge is faceted, not in the wrong place.",
  "regions": [                  // images only; normalised 0..1, origin top-left
    { "x": 0.34, "y": 0.4196, "w": 0.2089, "h": 0.1998,
      "note": "the waterline still stair-steps here" }
  ],
  "anchor": {                   // snapshot of WHAT HE WAS LOOKING AT
    "section_id": "01_shoreline",
    "section_heading": "1. Shoreline meander (water outline staircase) ...",
    "heading": "0a. Gates that ship default-ON",   // nearest <h3>, or null
    "label": "BEFORE (gate OFF) -- the rejected state",
    "src": "01_shoreline/01b_mc2_10_inlet_BEFORE_gateOFF.png",   // images only
    "caption": "BEFORE (gate OFF) — the rejected state",
    "text": "…",                // non-image artifacts: the text he graded
    "digest": "8d17be15f31f1ef1"   // content proof, see below
  },
  "created_utc": "…", "updated_utc": "…",
  "status": "ok"                // ok | changed | orphan, as of export time
}
```

`verdict` meanings, and what each implies for a lane:

| verdict | in the page | what it means |
|---|---|---|
| `bad` | BAD | he looked at this and it is wrong — fix it |
| `notit` | NOT WHAT I MEANT | a lane solved the wrong problem — re-read before coding |
| `known` | ALREADY REPORTED | he has said this before; likely a regression or an unshipped fix |
| `good` | GOOD | confirmed good; no work, but do not regress it |
| `null` | — | a comment or box with no grade — triage |

## Stable ids: `rid` and `digest`

The intolerable failure is a **silent mis-attachment** — a stored comment that,
after the page is regenerated, resolves to a *different* artifact than the one he
graded. That would put his words against something he never saw. Two mechanisms
prevent it.

**`id` (rid) is the lookup key**, derived only from manifest content, never from
DOM order. Inserting, deleting or reordering sections, blocks, figures or rows
cannot shift any other artifact's id.

| kind | id shape | keyed on |
|---|---|---|
| section | `sec/<section_id>` | manifest section id |
| image | `img/<section_id>/<path>` | the image file path |
| gate | `gate/<section_id>/<gate label>` | the gate label |
| lane | `lane/<lane name>` | the lane name |
| text | `txt/<section_id>/<hash>` | hash of the block's own HTML |
| row | `mrow/<section_id>/<hash>` | hash of the row's own cells |

24 of the pack's 276 images repeat a path inside one section (the same OFF frame
reused across comparison groups), so image ids escalate through content-derived
tiers — never a positional counter:

```
tier 1  img/<section>/<path>
tier 2  + #<caption>.<hash-of-full-caption>   when the path repeats in a section
tier 3  + @<hash of the group's sibling PATHS>   when caption matches too
```

**`digest` is the attachment proof** — a hash over the artifact's identity *and
its displayed content*. For an image that includes a **hash of the image file
itself**, because this pack re-captures poses under identical filenames all the
time; without it a "this is wrong" comment would silently ride onto a freshly
captured frame he never saw. (Hashing all 255 images costs about 0.5s per
render.) The page re-attaches silently only when the id resolves **and** the
digest matches. Otherwise:

| situation | what the page does | what the export says |
|---|---|---|
| id found, digest matches | attaches normally | `status: "ok"` |
| id found, digest differs | shows a purple **CHANGED** badge and a banner explaining the item was regenerated with different content; his words are kept verbatim against the original snapshot | `status: "changed"` |
| id not found | listed in the **ORPHANED** group in ALL NOTES; still exported with its snapshot so it can be traced | `status: "orphan"` |

So editing prose retires that block's id (the annotation orphans, loudly).
Editing an image caption, or re-capturing the image under the same filename,
keeps the id and trips the digest (CHANGED). Neither ever moves his words onto
content he did not see.

`scripts/check-review-anchor-stability.py` enforces this. It mutates the real
manifest — inserting and deleting sections, figure blocks, images, gate/lane/meta
rows, plus a pure reorder — and asserts that every id shared across the mutation
still points at the same artifact. Its `--self-test` re-runs the whole battery
under a deliberately positional id scheme and requires that mis-attachments are
detected, so the checker cannot pass by being blind. Both are wired into
`scripts/check-contracts.sh` as `review_anchor_stability{,_selftest}`.

## Consuming it in a lane

```sh
py -3 scripts/read_review_feedback.py FEEDBACK.json              # prioritised report
py -3 scripts/read_review_feedback.py FEEDBACK.json --json       # worklist for an orchestrator
py -3 scripts/read_review_feedback.py FEEDBACK.json --verdict bad --verdict notit
py -3 scripts/read_review_feedback.py --selftest
```

The reader re-derives anchors from the **current** tracked manifest using the
same `iter_anchors()` the renderer uses, so writer and reader can never disagree
about what an id means. Crucially it **recomputes** `ATTACHED` / `CHANGED` /
`ORPHANED` rather than trusting the `status` in the file: the browser's status was
only true for the page he had open, and the manifest may have moved on since.

Items sort `bad` → `notit` → `known` → ungraded → `good`. `--json` emits
`mc2.review.worklist/1`, one entry per annotation with `id`, `verdict`, `action`,
`attachment`, `comment`, `regions`, `section_id`, `heading`, and an absolute
`image` path a lane can open directly.

Region coordinates are fractions of the image. To convert to pixels:

```py
px = round(r["x"] * width);  py = round(r["y"] * height)
pw = round(r["w"] * width);  ph = round(r["h"] * height)
```

Treat `CHANGED` and `ORPHANED` as **read before acting**. The words are still
his, but the artifact underneath them has moved.
