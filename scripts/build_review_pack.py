#!/usr/bin/env python3
"""Render the v0.5.2 release REVIEW pack (INDEX.html) from a content manifest.

The review pack lives under release_assets/, which is .gitignored, so the
hand-edited INDEX.html is not recoverable if the tree is cleaned.  This script
makes the pack reproducible:

  * all human-authored content (title, lede, prose, captions, image ordering)
    lives in a TRACKED manifest -- release_assets_src/<ver>/review_pack.json
  * the GATE INVENTORY table (section 0) is rebuilt FROM SOURCE at render time
    by parsing RenderCore/RendererFeatureRegistry.h, so the published defaults
    can never drift from the compiled ones.  Gate defaults are deliberately
    NOT stored in the manifest.
  * every reviewable artifact (image, prose block, table row, section) carries a
    STABLE ANCHOR ID derived from manifest content -- never from DOM order -- so
    the in-page annotation layer (see release_assets_src/annotate/) can survive
    regeneration.  See iter_anchors() for the id contract.

Usage:
    py -3 scripts/build_review_pack.py --check
    py -3 scripts/build_review_pack.py --out /tmp/INDEX.html
    py -3 scripts/build_review_pack.py --dump-anchors anchors.json

Windows / Python 3 / standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULT_MANIFEST = os.path.join(REPO_ROOT, "release_assets_src", "0.5.2", "review_pack.json")
DEFAULT_OUT = os.path.join(REPO_ROOT, "release_assets", "0.5.2", "REVIEW", "INDEX.html")

ANNOTATE_DIR = os.path.join(REPO_ROOT, "release_assets_src", "annotate")

IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".gif", ".webp")


# ---------------------------------------------------------------------------
# RendererFeatureRegistry.h parsing
# ---------------------------------------------------------------------------

_ENTRY_RE = re.compile(
    r"\{\s*"
    r'"(?P<feature>MC2_[A-Z0-9_]+)"\s*,\s*'
    r'(?P<env>"(?:[^"\\]|\\.)*"|nullptr)\s*,\s*'
    r"EnvVarKind::(?P<kind>\w+)\s*,\s*"
    r"(?P<default>true|false)\s*,\s*"
    r'(?P<doc>"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*)\s*,?\s*'
    r"\}",
    re.S,
)


def _unquote(lit: str) -> str:
    """Join and unescape one or more adjacent C string literals."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', lit)
    s = "".join(parts)
    return (
        s.replace("\\\\", "\x00")
        .replace('\\"', '"')
        .replace("\\n", "\n")
        .replace("\\t", "\t")
        .replace("\x00", "\\")
    )


def _table_body(text: str, name: str) -> str:
    m = re.search(r"EnvVarDesc\s+%s\s*\[\s*\]\s*=\s*\{" % re.escape(name), text)
    if not m:
        return ""
    depth, i = 1, m.end()
    while depth and i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[m.end(): i - 1]


def parse_registry(header_path: str) -> dict:
    """Return {ENV_VAR: {featureId, envVar, kind, defaultOn, doc, table}}."""
    with open(header_path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()

    reg: dict = {}
    for table in ("kFeatureTable", "kAuxEnvVars"):
        body = _table_body(text, table)
        if not body:
            print("WARNING: table %s not found in %s" % (table, header_path), file=sys.stderr)
            continue
        for m in _ENTRY_RE.finditer(body):
            env = m.group("env")
            entry = {
                "featureId": m.group("feature"),
                "envVar": None if env == "nullptr" else _unquote(env),
                "kind": m.group("kind"),
                "defaultOn": m.group("default") == "true",
                "doc": _unquote(m.group("doc")),
                "table": table,
            }
            key = entry["envVar"] or entry["featureId"]
            reg.setdefault(key, entry)
    return reg


# ---------------------------------------------------------------------------
# READER-SOURCE default resolution (for gates with no registry entry)
# ---------------------------------------------------------------------------
#
# RendererFeatureRegistry.h is authoritative when it has an entry, but 18 of the
# gates named in this pack have none, so a registry-only table can say nothing
# about their real defaults.  Rather than let the manifest assert a default (the
# exact drift AUDIT-TRUTH-1 had to repair), resolve it from the C++ READER at
# render time by classifying the getenv idiom, and publish the matched source
# line as the citation.  Anything that does not match a known idiom renders
# UNRESOLVED -- never a guess.

_READER_SEARCH_DIRS = ("GameOS", "mclib", "code", "RenderCore")
_READER_EXTS = (".cpp", ".h", ".hpp", ".cc")

# (regex, verdict, kind).  Applied to the reader site plus the next few lines.
_IDIOM_RULES = (
    (re.compile(r"env_default_on\s*\(\s*\"%s\""), "ON", "bool"),
    (re.compile(r"return\s*!\s*\(\s*\w+\s*&&\s*\w+\[0\]\s*==\s*'0'"), "ON", "bool"),
    (re.compile(r"return\s*\(\s*\w+\s*&&\s*\w+\[0\]\s*==\s*'1'\s*\)"), "OFF", "bool"),
    (re.compile(r"return\s+\w+\s*&&\s*\w+\[0\]\s*&&\s*\w+\[0\]\s*!=\s*'0'"), "OFF", "bool"),
    (re.compile(r"if\s*\(\s*!\w+\s*\|\|\s*!\*?\w+(?:\[0\])?\s*\)\s*return\s+"
                r"(?P<val>-?[0-9.]+)f?\s*;"), "VALUE", "numeric"),
    (re.compile(r"\?\s*std::atoi\s*\(\s*\w+\s*\)\s*:\s*(?P<val>-?[0-9]+)"), "VALUE", "numeric"),
    (re.compile(r"\?\s*atoi\s*\(\s*\w+\s*\)\s*:\s*(?P<val>-?[0-9]+)"), "VALUE", "numeric"),
)

_READER_WINDOW = 4  # lines after the getenv site to inspect


def _iter_source_files(repo_root: str, hint: str | None):
    if hint:
        p = os.path.join(repo_root, hint)
        if os.path.isfile(p):
            yield p
            return
    for d in _READER_SEARCH_DIRS:
        base = os.path.join(repo_root, d)
        for root, _dirs, files in os.walk(base):
            for f in files:
                if f.endswith(_READER_EXTS):
                    yield os.path.join(root, f)


def resolve_reader_default(env: str, repo_root: str, hint: str | None = None) -> dict:
    """Classify a gate's default by reading its C++ getenv site.

    Returns {status, verdict, file, lineno, line}.  status is one of
    'resolved', 'unresolved' (reader found, idiom unrecognised) or
    'ghost' (no reader anywhere -- the gate does nothing).
    """
    needle = '"%s"' % env
    best = None
    resolved = None
    sites = []
    for path in _iter_source_files(repo_root, hint):
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                lines = fh.readlines()
        except OSError:
            continue
        for i, line in enumerate(lines):
            if needle not in line or ("getenv" not in line and "env_default_on" not in line):
                continue
            window = "".join(lines[i:i + _READER_WINDOW + 1])
            rel = os.path.relpath(path, repo_root).replace("\\", "/")
            sites.append("%s:%d" % (rel, i + 1))
            for rx, verdict, kind in _IDIOM_RULES:
                pat = rx.pattern
                m = re.search(pat % env, window) if "%s" in pat else rx.search(window)
                if not m:
                    continue
                if verdict == "VALUE":
                    val = m.group("val")
                    v = "OFF" if val in ("0", "0.0", "0.0f") else "DEFAULT %s" % val
                else:
                    v = verdict
                if resolved is None:
                    resolved = {"status": "resolved", "verdict": v, "kind": kind,
                                "file": rel, "lineno": i + 1, "line": line.strip()}
                elif resolved["verdict"] != v:
                    # two readers of the same gate disagree -- never publish a
                    # single verdict for that; make a human read both.
                    resolved = {"status": "unresolved", "verdict": None,
                                "kind": None, "file": rel, "lineno": i + 1,
                                "line": "conflicting readers: %s vs %s"
                                        % (resolved["verdict"], v)}
                break
            else:
                if best is None:
                    best = {"status": "unresolved", "verdict": None, "kind": None,
                            "file": rel, "lineno": i + 1, "line": line.strip()}
    out = resolved or best or {"status": "ghost", "verdict": None, "kind": None,
                               "file": None, "lineno": None, "line": None}
    out["sites"] = sites
    return out


# ---------------------------------------------------------------------------
# HTML helpers
# ---------------------------------------------------------------------------

def esc_attr(s: str) -> str:
    return s.replace("&", "&amp;").replace('"', "&quot;").replace("<", "&lt;")


def gate_lookup_key(gate_label: str) -> str:
    """'MC2_NIGHT_FOG_DIM (+_SCALE)' -> 'MC2_NIGHT_FOG_DIM'."""
    m = re.search(r"MC2_[A-Z0-9_]+", gate_label)
    return m.group(0) if m else gate_label.strip()


# ---------------------------------------------------------------------------
# STABLE ANCHOR IDS  (the contract the annotation layer depends on)
# ---------------------------------------------------------------------------
#
# The user annotates artifacts in the rendered page; the page is regenerated
# whenever the manifest or the source changes.  An annotation that silently
# re-attached to a DIFFERENT artifact would put his words against something he
# never saw -- strictly worse than having no annotation feature at all.  So:
#
#   rid    -- the LOOKUP key.  Derived only from content that identifies the
#             artifact (section id, image path, gate name, lane name) or, when
#             no such intrinsic key exists (prose, meta rows), from a hash of
#             the artifact's own content.  DOM order is never an input, so
#             inserting or deleting a section/block/figure cannot shift any
#             other artifact's id.
#   digest -- the ATTACHMENT PROOF.  A hash over the artifact's identity tuple
#             INCLUDING its displayed content.  The annotation stores the digest
#             it was written against; the page only silently re-attaches when
#             the live digest matches.  A digest mismatch renders the annotation
#             with a CHANGED badge (content edited under an existing comment); a
#             missing rid renders it in the ORPHANED drawer.  Neither case ever
#             quietly moves the user's words onto different content.
#
# Duplicate rids are resolved by appending "@<hash of the figure group's sibling
# image paths>" (image items) or "~<n>" (everything else), and the build prints
# a warning.  Both tiebreaks are content-derived, not positional.
#
# read_review_feedback.py imports iter_anchors() from this module, so the writer
# and the reader can never disagree about what an id means.

ANCHOR_SCHEMA = "mc2.review.anchor/1"

_TAG_RE = re.compile(r"<[^>]+>")
_WS_RE = re.compile(r"\s+")


def _h(*parts) -> str:
    """Short stable hash of a canonical JSON tuple."""
    blob = json.dumps(parts, sort_keys=True, ensure_ascii=False, separators=(",", ":"))
    return hashlib.sha256(blob.encode("utf-8")).hexdigest()


_IMG_HASH_CACHE: dict = {}


def image_content_hash(pack_dir: str, src: str) -> str:
    """Hash of the image FILE, folded into that image's digest.

    This pack re-captures poses under identical filenames constantly, so a
    manifest-only digest would leave a "this is wrong" comment silently riding
    onto a freshly captured frame the user never saw -- the exact mis-attachment
    this whole design exists to prevent.  Hashing all 255 images costs ~0.5s and
    is cached per render.  A missing file gets a stable sentinel, so an image
    that later appears correctly reads as changed.
    """
    path = os.path.normpath(os.path.join(pack_dir, src))
    hit = _IMG_HASH_CACHE.get(path)
    if hit is not None:
        return hit
    try:
        h = hashlib.sha256()
        with open(path, "rb") as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b""):
                h.update(chunk)
        out = h.hexdigest()[:12]
    except OSError:
        out = "absent"
    _IMG_HASH_CACHE[path] = out
    return out


def _plain(html: str, limit: int = 160) -> str:
    """HTML -> collapsed plain text, for human-readable anchor labels."""
    s = _TAG_RE.sub(" ", html or "")
    s = (s.replace("&mdash;", "--").replace("&ndash;", "-").replace("&amp;", "&")
          .replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", '"')
          .replace("&rarr;", "->").replace("&nbsp;", " "))
    # The eight above were the entities this pack used when the reader was
    # written.  Every other entity in the manifest reached the EXPORTED label
    # undecoded -- a lane reading feedback got literal "&ldquo;" in the one
    # string that is supposed to be human-readable.  Measured over the current
    # manifest, the undecoded set was: ldquo/rdquo (246), times (67), sect (64),
    # rsquo (47), middot (18), bull (14), #9888 (13), hellip (12), minus (9),
    # asymp (7), #9940 (5).  Decoded to ASCII, not to the glyph, so an exported
    # label stays safe on a cp1252 console -- the same class of crash 07349d10
    # had to fix in mission_smooth.
    s = (s.replace("&ldquo;", '"').replace("&rdquo;", '"')
          .replace("&lsquo;", "'").replace("&rsquo;", "'")
          .replace("&hellip;", "...").replace("&times;", "x")
          .replace("&sect;", "section ").replace("&middot;", "-")
          .replace("&bull;", "*").replace("&minus;", "-")
          .replace("&asymp;", "~").replace("&#9888;", "(!)")
          .replace("&#9940;", "(X)"))
    s = _WS_RE.sub(" ", s).strip()
    return s[:limit]


_PROSE_TYPES = ("h3", "what", "note", "pl", "lede", "para")

_SLUG_BAD = re.compile(r"[^A-Za-z0-9_.+-]+")


def _slug(s: str, limit: int = 72) -> str:
    """Readable, id-safe form of a label.  Kept readable on purpose: a lane
    reads exported ids by eye, so 'gate/gates/MC2_LIGHT_REALISM-6-tunables'
    beats an opaque hash whenever a human-meaningful key exists."""
    return _SLUG_BAD.sub("-", _plain(s, limit)).strip("-") or "x"


def _anchor(kind, rid, digest_tuple, **fields) -> dict:
    a = {"kind": kind, "rid": rid, "digest": _h(kind, *digest_tuple)[:16]}
    a.update(fields)
    return a


def iter_anchors(manifest: dict) -> list:
    """Every reviewable artifact in the pack, in manifest order.

    Pure function of the manifest.  Each record carries 'obj' -- the manifest
    object it came from -- so the renderer can look anchors up by identity;
    read_review_feedback.py ignores that field.
    """
    out = []
    pack_dir = os.path.join(REPO_ROOT, manifest.get("pack_dir", ""))
    for s in manifest["sections"]:
        sid = s["id"]
        out.append(_anchor("section", "sec/%s" % sid, (sid, s["heading"]),
                           obj=s, section_id=sid, section_heading=_plain(s["heading"]),
                           heading=None, label=_plain(s["heading"]), src=None,
                           caption=None, text=_plain(s["heading"])))
        h3 = None
        for b in s["blocks"]:
            t = b["type"]
            if t == "h3":
                h3 = _plain(b["html"])
            common = dict(section_id=sid, section_heading=_plain(s["heading"]), heading=h3)
            if t in _PROSE_TYPES:
                txt = _plain(b["html"])
                out.append(_anchor(
                    "text", "txt/%s/%s" % (sid, _h(t, b["html"])[:12]), (sid, t, b["html"]),
                    obj=b, label="%s: %s" % (t, txt), src=None, caption=None,
                    text=_plain(b["html"], 400), **common))
            elif t == "meta":
                for row in b["rows"]:
                    lbl = _plain(row[0]["html"]) if row else ""
                    body = " | ".join(_plain(c["html"], 200) for c in row[1:])
                    out.append(_anchor(
                        "row", "mrow/%s/%s" % (sid, _h(row)[:12]), (sid, row),
                        obj=row, label="%s: %s" % (lbl, body), src=None, caption=None,
                        text=body, **common))
            elif t == "gate_table":
                for r in b["rows"]:
                    # the FULL label, not gate_lookup_key(): two rows in one
                    # section can share a lookup key ("MC2_LIGHT_REALISM
                    # tunables" vs "MC2_LIGHT_REALISM (+6 tunables)") and must
                    # still get distinct ids without a positional tiebreak.
                    out.append(_anchor(
                        "gate", "gate/%s/%s" % (sid, _slug(r["gate"])),
                        (sid, r["gate"], r["cells"]),
                        obj=r, label=r["gate"], src=None, caption=None,
                        text=_plain(" | ".join(r["cells"]), 400), **common))
            elif t == "lanes_table":
                for ln in b["lanes"]:
                    out.append(_anchor(
                        "lane", "lane/%s" % _slug(ln["name"]),
                        (ln["name"], ln.get("sent"), ln.get("found"),
                         ln.get("commits"), ln.get("gates"), ln.get("verdict")),
                        obj=ln, label=ln["name"], src=None, caption=None,
                        text=_plain("%s -- %s" % (ln.get("sent", ""), ln.get("found", "")), 400),
                        **common))
            elif t == "figures":
                sibs = sorted((it.get("src") or "").replace("\\", "/") for it in b["items"])
                for it in b["items"]:
                    src = (it.get("src") or "").replace("\\", "/")
                    if not src:
                        continue  # download links, not visual artifacts
                    out.append(_anchor(
                        "image", "img/%s/%s" % (sid, src),
                        (sid, src, it.get("caption") or "",
                         image_content_hash(pack_dir, src)),
                        obj=it, label=_plain(it.get("caption") or "") or os.path.basename(src),
                        src=src, caption=_plain(it.get("caption") or ""),
                        text=None, _sibs=sibs, _cap=it.get("caption") or "", **common))
    _dedupe_rids(out)
    for a in out:
        a.pop("_sibs", None)
        a.pop("_cap", None)
    return out


def _rid_counts(anchors: list) -> set:
    counts = {}
    for a in anchors:
        counts[a["rid"]] = counts.get(a["rid"], 0) + 1
    return {r for r, c in counts.items() if c > 1}


def _dedupe_rids(anchors: list) -> None:
    """Make rids unique with content-derived tiebreaks, never DOM position.

    Applied in escalating tiers so the common case keeps the shortest, most
    edit-resistant id.  24 of the pack's 276 images repeat a path inside one
    section (the same OFF frame used in two comparison groups), so tier 2 is
    load-bearing, and tier 3 covers the 3 pairs whose captions match too.

      tier 1  img/<section>/<path>
      tier 2  + #<caption>.<hash>   -- survives adding/removing group siblings
      tier 3  + @<hash of the group's sibling PATHS>  -- survives caption edits

    The tier-2 slug is truncated for readability, so it carries a hash of the
    FULL caption as well: without it, two captions agreeing in their first 40
    characters collide, and the tiebreak silently degrades to the positional
    ~n it exists to avoid.
    """
    dupes = _rid_counts(anchors)
    if not dupes:
        return
    for a in anchors:
        if a["rid"] in dupes and a["kind"] == "image":
            a["rid"] = "%s#%s.%s" % (a["rid"], _slug(a["_cap"], 40), _h(a["_cap"])[:6])
    dupes = _rid_counts(anchors)
    for a in anchors:
        if a["rid"] in dupes and a["kind"] == "image":
            a["rid"] = "%s@%s" % (a["rid"], _h(a["_sibs"])[:8])

    seen = {}
    still = []
    for a in anchors:
        n = seen.get(a["rid"], 0)
        seen[a["rid"]] = n + 1
        if n:
            a["rid"] = "%s~%d" % (a["rid"], n + 1)
            still.append(a["rid"])
    if still:
        print("WARNING: %d anchor id(s) needed a positional ~n tiebreak "
              "(byte-identical content in one section): %s"
              % (len(still), ", ".join(still[:5])), file=sys.stderr)


def anchor_attrs(rec: dict | None) -> str:
    """Render the data-* attributes the annotation layer binds to."""
    if not rec:
        return ""
    out = ' data-rid="%s" data-adigest="%s" data-akind="%s"' % (
        esc_attr(rec["rid"]), rec["digest"], rec["kind"])
    if rec.get("label"):
        out += ' data-alabel="%s"' % esc_attr(rec["label"])
    return out


ON = '<b style="color:#a0f0a0">ON</b>'
OFF = "<b>OFF</b>"
UNREG = '<b style="color:#f0d890">NOT IN REGISTRY</b>'


SMALL = '<br><span style="font-size:11px;color:#9aa3ad">%s</span>'


# A numeric KNOB (pool sizes, grid dimensions, strengths) carries its real
# default in the registry DOC string, while the struct's boolean defaultOn
# field is meaningless for it and reads false.  Rendering those as a bold OFF
# actively misinforms -- MC2_LIGHT_POOL_MAX would publish as "OFF" when source
# says 256.  Detect the declared value and publish that instead, still entirely
# source-derived.  The guard is deliberately tight: a boolean gate can never
# declare "Default 256", so values of 0 and 1 are left alone as genuinely
# boolean and never reinterpreted.
_DOC_NUMERIC_DEFAULT = re.compile(r"\bDefault\s+(\d+(?:\.\d+)?)\b")


def _numeric_default(entry: dict) -> str | None:
    m = _DOC_NUMERIC_DEFAULT.search(entry.get("doc") or "")
    if not m:
        return None
    val = m.group(1)
    if val in ("0", "1", "0.0", "1.0"):
        return None
    return val


def render_default_cell(gate_label: str, reg: dict, readers: dict | None = None) -> str:
    key = gate_lookup_key(gate_label)
    e = reg.get(key)
    if e is not None:
        if not e["defaultOn"]:
            num = _numeric_default(e)
            if num is not None:
                return ("<b>DEFAULT %s</b>" % num + SMALL
                        % "numeric knob &mdash; value from the "
                          "RendererFeatureRegistry.h doc string")
        return (ON if e["defaultOn"] else OFF) + SMALL % "RendererFeatureRegistry.h"

    r = (readers or {}).get(key)
    if r is None:
        return UNREG + SMALL % ("no registry entry and reader not resolved")
    if r["status"] == "ghost":
        return ('<b style="color:#f0a0a0">GHOST</b>' + SMALL
                % "no reader found in GameOS/ mclib/ code/ RenderCore/ &mdash; "
                  "this gate does nothing")
    cite = "%s:%d" % (r["file"], r["lineno"])
    if r["status"] == "unresolved":
        return ('<b style="color:#f0d890">UNRESOLVED</b>' + SMALL
                % ("reader at <code>%s</code>, idiom not recognised &mdash; read it"
                   % esc_attr(cite)))
    v = r["verdict"]
    body = ON if v == "ON" else OFF if v == "OFF" else "<b>%s</b>" % v
    extra = ""
    if len(r.get("sites") or []) > 1:
        extra = " (%d reader sites, agreeing)" % len(r["sites"])
    return body + SMALL % ("from reader <code title=\"%s\">%s</code>%s"
                           % (esc_attr(r["line"]), esc_attr(cite), extra))


def render_gate_cell(gate_label: str, reg: dict) -> str:
    """Gate name + (from source) env var, kind and doc string as a tooltip."""
    key = gate_lookup_key(gate_label)
    e = reg.get(key)
    label = gate_label.replace(key, "", 1).strip()
    if e is None:
        out = "<code>%s</code>" % gate_label
    else:
        out = '<code title="%s">%s</code>' % (esc_attr(e["doc"]), e["envVar"] or e["featureId"])
        if label:
            out += " <span style='color:#9aa3ad'>%s</span>" % label
        out += '<br><span style="font-size:11px;color:#9aa3ad">%s / %s</span>' % (
            e["featureId"], e["kind"])
    return out


_VERDICT_STYLE = {
    "fixed": ("#a0f0a0", "FIXED"),
    "partial": ("#f0d890", "PARTIAL"),
    "rejected-by-user": ("#f0a0a0", "REJECTED BY USER"),
    "approved-by-user": ("#a0f0a0", "APPROVED BY USER"),
    "disproven": ("#c0a0f0", "DISPROVEN"),
    "diagnostic-only": ("#9aa3ad", "DIAGNOSTIC ONLY"),
    "blocked": ("#f0a0a0", "BLOCKED"),
    "cancelled": ("#9aa3ad", "CANCELLED"),
    "in-flight": ("#78b0f0", "IN FLIGHT"),
}


def render_verdict(v: str) -> str:
    colour, label = _VERDICT_STYLE.get(v, ("#f0d890", v.upper()))
    return '<b style="color:%s">%s</b>' % (colour, label)


def render_lanes_table(b: dict, reg: dict, readers: dict, out: list, amap: dict) -> None:
    """Section 14 -- one row per lane run this session.

    Sibling lanes still in flight append entries to the manifest's "lanes"
    list; nothing else in the page has to change.
    """
    out.append('<table class="meta" style="font-size:13px">')
    out.append("<tr>" + "".join(
        '<th style="text-align:left">%s</th>' % h
        for h in ("Lane", "Sent to do", "What it actually found",
                  "Commits", "Gate(s) &amp; CURRENT DEFAULT", "Verdict", "Evidence")) + "</tr>")
    for ln in b["lanes"]:
        gates_html = []
        for g in ln.get("gates") or []:
            gates_html.append("<code>%s</code> &rarr; %s"
                              % (esc_attr(g), render_default_cell(g, reg, readers)))
        ev = " ".join('<a href="%s">%s</a>' % (esc_attr(e["href"]), e.get("text", e["href"]))
                      for e in (ln.get("evidence") or []))
        cells = [
            "<b>%s</b>" % ln["name"],
            ln.get("sent", ""),
            ln.get("found", ""),
            " ".join("<code>%s</code>" % c for c in (ln.get("commits") or [])) or
            '<span style="color:#9aa3ad">no code shipped</span>',
            "<br>".join(gates_html) or '<span style="color:#9aa3ad">none</span>',
            render_verdict(ln.get("verdict", "")),
            ev or '<span style="color:#9aa3ad">&mdash;</span>',
        ]
        out.append("<tr%s>" % anchor_attrs(amap.get(id(ln)))
                   + "".join("<td>%s</td>" % c for c in cells) + "</tr>")
    out.append("</table>")


def render_block(b: dict, reg: dict, out: list, readers: dict | None = None,
                 amap: dict | None = None) -> None:
    readers = readers or {}
    amap = amap if amap is not None else {}
    A = anchor_attrs(amap.get(id(b)))
    t = b["type"]
    if t == "h3":
        out.append("<h3%s>%s</h3>" % (A, b["html"]))
    elif t in ("what", "note", "pl", "lede", "para"):
        cls = "" if t == "para" else ' class="%s"' % t
        out.append("<p%s%s>%s</p>" % (cls, A, b["html"]))
    elif t == "meta":
        out.append('<table class="meta"%s>' % (" " + b["attrs"] if b.get("attrs") else ""))
        for row in b["rows"]:
            out.append("<tr%s>" % anchor_attrs(amap.get(id(row))))
            for c in row:
                out.append("<%s%s>%s</%s>" % (c["tag"], c.get("attrs", ""), c["html"], c["tag"]))
            out.append("</tr>")
        out.append("</table>")
    elif t == "gate_table":
        out.append('<table class="meta"%s>' % (" " + b["attrs"] if b.get("attrs") else ""))
        out.append("<tr>")
        for hcell in b["headers"]:
            out.append('<th style="text-align:left">%s</th>' % hcell)
        out.append("</tr>")
        dcol = b.get("default_col")
        for r in b["rows"]:
            cells = list(r["cells"])
            if dcol is not None:
                cells.insert(dcol, render_default_cell(r["gate"], reg, readers))
            cells[0] = render_gate_cell(r["gate"], reg)
            out.append("<tr%s>" % anchor_attrs(amap.get(id(r)))
                       + "".join("<td>%s</td>" % c for c in cells) + "</tr>")
        out.append("</table>")
    elif t == "figures":
        out.append('<div class="%s">' % b["layout"])
        for it in b["items"]:
            rec = amap.get(id(it))
            # image items ALWAYS get a <figure> so the annotation layer has a
            # per-image container to hang its bar and region overlay on.
            # figure{margin:0} in the pack stylesheet, so this is visually inert.
            has_fig = bool(it.get("caption")) or b["layout"] != "single" or bool(rec)
            if has_fig:
                out.append("<figure%s>" % anchor_attrs(rec))
                if it.get("caption"):
                    out.append("<figcaption>%s</figcaption>" % it["caption"])
            if "src" in it:
                href = it.get("href", it["src"])
                out.append('<a href="%s" target="_blank"><img src="%s"></a>'
                           % (esc_attr(href), esc_attr(it["src"])))
            else:
                out.append('<a href="%s" target="_blank">%s</a>'
                           % (esc_attr(it.get("link") or ""), it.get("text", "")))
            if has_fig:
                out.append("</figure>")
        out.append("</div>")
    elif t == "lanes_table":
        render_lanes_table(b, reg, readers, out, amap)
    else:
        raise SystemExit("unknown block type %r" % t)


def build_reader_index(manifest: dict, reg: dict, repo_root: str) -> dict:
    """Resolve reader-source defaults for every gate the pack names that the
    registry does not cover.  Done once per render; hints come from the
    manifest's optional 'reader_hints' map (gate -> repo-relative file)."""
    hints = manifest.get("reader_hints", {})
    wanted = set()
    for s in manifest["sections"]:
        for b in s["blocks"]:
            if b["type"] == "gate_table":
                wanted.update(gate_lookup_key(r["gate"]) for r in b["rows"])
            elif b["type"] == "lanes_table":
                for ln in b["lanes"]:
                    wanted.update(gate_lookup_key(g) for g in (ln.get("gates") or []))
    out = {}
    for g in sorted(wanted):
        if g in reg:
            continue
        out[g] = resolve_reader_default(g, repo_root, hints.get(g))
    return out


def _read_asset(name: str) -> str:
    with open(os.path.join(ANNOTATE_DIR, name), encoding="utf-8") as fh:
        return fh.read()


def pack_id(manifest: dict) -> str:
    """Storage namespace for the annotation layer.  Explicit manifest key wins;
    otherwise the version directory under release_assets/."""
    if manifest.get("pack_id"):
        return manifest["pack_id"]
    parts = manifest["pack_dir"].replace("\\", "/").rstrip("/").split("/")
    return parts[-2] if len(parts) >= 2 else parts[-1]


def render(manifest: dict, reg: dict, readers: dict | None = None,
           annotate: bool = True) -> str:
    anchors = iter_anchors(manifest) if annotate else []
    amap = {id(a["obj"]): a for a in anchors}

    out = ["<!doctype html><html><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>%s</title>" % manifest["title"],
           "<style>", manifest["style"], "</style>"]
    if annotate:
        out += ["<style>", _read_asset("review_annotate.css"), "</style>"]
    out += ['</head><body><div class="wrap">',
            "<h1>%s</h1>" % manifest["h1"],
            '<p class="lede">%s</p>' % manifest["lede"],
            '<nav class="toc">%s</nav>' % manifest["toc_html"]]
    for s in manifest["sections"]:
        out.append('<section id="%s"%s>' % (s["id"], anchor_attrs(amap.get(id(s)))))
        out.append("<h2>%s</h2>" % s["heading"])
        for b in s["blocks"]:
            render_block(b, reg, out, readers, amap)
        out.append("</section>")
    out.append("</div>")
    if annotate:
        meta = {"schema": ANCHOR_SCHEMA, "pack_id": pack_id(manifest),
                "title": manifest["title"], "anchors": len(anchors),
                "images": sum(1 for a in anchors if a["kind"] == "image")}
        out.append('<script type="application/json" id="rv-pack-meta">%s</script>'
                   % json.dumps(meta, ensure_ascii=False).replace("</", "<\\/"))
        out.append("<script>%s</script>" % _read_asset("review_annotate.js"))
    out.append("</body></html>")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# --check
# ---------------------------------------------------------------------------

def manifest_images(manifest: dict):
    seen = []
    for s in manifest["sections"]:
        for b in s["blocks"]:
            if b["type"] != "figures":
                continue
            for it in b["items"]:
                for k in ("src", "href", "link"):
                    v = it.get(k)
                    if v and v.lower().endswith(IMAGE_EXTS):
                        seen.append(v.replace("\\", "/"))
    return seen


def disk_images(pack_dir: str):
    found = []
    for root, _dirs, files in os.walk(pack_dir):
        for f in files:
            if f.lower().endswith(IMAGE_EXTS):
                rel = os.path.relpath(os.path.join(root, f), pack_dir)
                found.append(rel.replace("\\", "/"))
    return found


def do_check(manifest: dict, pack_dir: str) -> int:
    refs = manifest_images(manifest)
    ref_set = set(refs)
    if not os.path.isdir(pack_dir):
        print("ERROR: pack dir not found: %s" % pack_dir)
        return 1
    on_disk = set(disk_images(pack_dir))

    orphans = sorted(on_disk - ref_set)
    missing = sorted(ref_set - on_disk)

    print("pack dir            : %s" % pack_dir)
    print("images on disk      : %d" % len(on_disk))
    print("referenced (unique) : %d  (%d <img> references)" % (len(ref_set), len(refs)))
    print("orphans (on disk, not referenced): %d" % len(orphans))
    for o in orphans:
        print("  ORPHAN  %s" % o)
    print("missing (referenced, not on disk): %d" % len(missing))
    for m in missing:
        print("  MISSING %s" % m)
    return 1 if (orphans or missing) else 0


# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", default=DEFAULT_MANIFEST)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--registry", default=None,
                    help="path to RendererFeatureRegistry.h (default: from manifest)")
    ap.add_argument("--pack-dir", default=None,
                    help="image root (default: from manifest 'pack_dir')")
    ap.add_argument("--check", action="store_true",
                    help="validate manifest<->disk image sets; exit 1 on mismatch")
    ap.add_argument("--dump-anchors", metavar="PATH",
                    help="write the stable anchor-id table as JSON and exit "
                         "(no rendering; used by the id-stability test)")
    ap.add_argument("--no-annotate", action="store_true",
                    help="render without the annotation layer (diff/regression aid)")
    args = ap.parse_args(argv)

    with open(args.manifest, encoding="utf-8") as fh:
        manifest = json.load(fh)

    pack_dir = args.pack_dir or os.path.join(REPO_ROOT, manifest["pack_dir"])
    registry = args.registry or os.path.join(REPO_ROOT, manifest["registry_header"])

    if args.check:
        return do_check(manifest, pack_dir)

    if args.dump_anchors:
        anchors = iter_anchors(manifest)
        payload = {"schema": ANCHOR_SCHEMA, "pack_id": pack_id(manifest),
                   "anchors": [{k: v for k, v in a.items() if k != "obj"}
                               for a in anchors]}
        with open(args.dump_anchors, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(payload, fh, indent=1, ensure_ascii=False, sort_keys=True)
        print("wrote %s (%d anchors, %d unique rids)"
              % (args.dump_anchors, len(anchors), len({a["rid"] for a in anchors})))
        return 0

    reg = parse_registry(registry)
    readers = build_reader_index(manifest, reg, REPO_ROOT)
    ghosts = sorted(g for g, r in readers.items() if r["status"] == "ghost")
    unres = sorted(g for g, r in readers.items() if r["status"] == "unresolved")
    html = render(manifest, reg, readers, annotate=not args.no_annotate)
    out_dir = os.path.dirname(os.path.abspath(args.out))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(html)
    print("wrote %s (%d bytes, %d sections, %d gate entries from %s)"
          % (args.out, len(html.encode("utf-8")), len(manifest["sections"]),
             len(reg), os.path.relpath(registry, REPO_ROOT)))
    print("reader-source defaults resolved for %d gate(s) with no registry entry"
          % sum(1 for r in readers.values() if r["status"] == "resolved"))
    if ghosts:
        print("GHOST (named in the pack, no reader in source): %s" % ", ".join(ghosts))
    if unres:
        print("UNRESOLVED (reader found, idiom unrecognised): %s" % ", ".join(unres))
    return 0


if __name__ == "__main__":
    sys.exit(main())
