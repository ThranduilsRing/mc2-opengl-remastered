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

Usage:
    py -3 scripts/build_review_pack.py --check
    py -3 scripts/build_review_pack.py --out /tmp/INDEX.html

Windows / Python 3 / standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULT_MANIFEST = os.path.join(REPO_ROOT, "release_assets_src", "0.5.2", "review_pack.json")
DEFAULT_OUT = os.path.join(REPO_ROOT, "release_assets", "0.5.2", "REVIEW", "INDEX.html")

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
# HTML helpers
# ---------------------------------------------------------------------------

def esc_attr(s: str) -> str:
    return s.replace("&", "&amp;").replace('"', "&quot;").replace("<", "&lt;")


def gate_lookup_key(gate_label: str) -> str:
    """'MC2_NIGHT_FOG_DIM (+_SCALE)' -> 'MC2_NIGHT_FOG_DIM'."""
    m = re.search(r"MC2_[A-Z0-9_]+", gate_label)
    return m.group(0) if m else gate_label.strip()


ON = '<b style="color:#a0f0a0">ON</b>'
OFF = "<b>OFF</b>"
UNREG = '<b style="color:#f0d890">NOT IN REGISTRY</b>'


def render_default_cell(gate_label: str, reg: dict) -> str:
    key = gate_lookup_key(gate_label)
    e = reg.get(key)
    if e is None:
        return UNREG + '<br><span style="font-size:12px;color:#9aa3ad">no ' \
                       "RendererFeatureRegistry.h entry &mdash; default is source-side only</span>"
    return ON if e["defaultOn"] else OFF


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


def render_block(b: dict, reg: dict, out: list) -> None:
    t = b["type"]
    if t == "h3":
        out.append("<h3>%s</h3>" % b["html"])
    elif t in ("what", "note", "pl", "lede", "para"):
        cls = "" if t == "para" else ' class="%s"' % t
        out.append("<p%s>%s</p>" % (cls, b["html"]))
    elif t == "meta":
        out.append('<table class="meta"%s>' % (" " + b["attrs"] if b.get("attrs") else ""))
        for row in b["rows"]:
            out.append("<tr>")
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
                cells.insert(dcol, render_default_cell(r["gate"], reg))
            cells[0] = render_gate_cell(r["gate"], reg)
            out.append("<tr>" + "".join("<td>%s</td>" % c for c in cells) + "</tr>")
        out.append("</table>")
    elif t == "figures":
        out.append('<div class="%s">' % b["layout"])
        for it in b["items"]:
            has_fig = bool(it.get("caption")) or b["layout"] != "single"
            if has_fig:
                out.append("<figure>")
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
    else:
        raise SystemExit("unknown block type %r" % t)


def render(manifest: dict, reg: dict) -> str:
    out = ["<!doctype html><html><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>%s</title>" % manifest["title"],
           "<style>", manifest["style"], "</style>",
           '</head><body><div class="wrap">',
           "<h1>%s</h1>" % manifest["h1"],
           '<p class="lede">%s</p>' % manifest["lede"],
           '<nav class="toc">%s</nav>' % manifest["toc_html"]]
    for s in manifest["sections"]:
        out.append('<section id="%s">' % s["id"])
        out.append("<h2>%s</h2>" % s["heading"])
        for b in s["blocks"]:
            render_block(b, reg, out)
        out.append("</section>")
    out.append("</div></body></html>")
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
    args = ap.parse_args(argv)

    with open(args.manifest, encoding="utf-8") as fh:
        manifest = json.load(fh)

    pack_dir = args.pack_dir or os.path.join(REPO_ROOT, manifest["pack_dir"])
    registry = args.registry or os.path.join(REPO_ROOT, manifest["registry_header"])

    if args.check:
        return do_check(manifest, pack_dir)

    reg = parse_registry(registry)
    html = render(manifest, reg)
    out_dir = os.path.dirname(os.path.abspath(args.out))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(html)
    print("wrote %s (%d bytes, %d sections, %d gate entries from %s)"
          % (args.out, len(html.encode("utf-8")), len(manifest["sections"]),
             len(reg), os.path.relpath(registry, REPO_ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
