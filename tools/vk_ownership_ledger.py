#!/usr/bin/env python3
"""vk_ownership_ledger.py -- lexical create/destroy balance of the Vulkan objects each file and island owns.

WHAT: counts, per Vulkan-owning translation unit and per island, the creation tokens (`vkCreate*`, `vkAllocate*`, `vmaCreate*`) against the release tokens
(`vkDestroy*`, `vkFree*`, `vmaDestroy*`) of each object kind, with comments and string literals blanked. The table is written to
`.claude/cleanup/vk-ownership-ledger.md`.
WHY: the ownership rewrite (RAII owners, review batch 4) converts one island at a time and its proof is that the island's balance reaches zero and stays there;
the counts are lexical (one token is not one object at run time) and are used as a ratchet on the manual code, not as a leak detector. A file that creates
what another file of the same island destroys is balanced only at island level, which is why both views are printed.

    py -3 tools/vk_ownership_ledger.py            # print the ledger
    py -3 tools/vk_ownership_ledger.py --write    # regenerate .claude/cleanup/vk-ownership-ledger.md
    py -3 tools/vk_ownership_ledger.py --check    # exit 1 when the committed ledger is not the current rendering
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "GameOS" / "gameos"
LEDGER = ROOT / ".claude" / "cleanup" / "vk-ownership-ledger.md"

# island -> the file stems it owns (the split islands spread one lifetime over several files)
ISLANDS = {
    "color_prop": ["vulkan_color_prop_island"],
    "legacy_shape": ["vulkan_legacy_shape_island"],
    "mech": ["vulkan_mech_island"],
    "post_fog": ["vulkan_post_fog_native"],
    "post_hzb": ["vulkan_post_hzb_native"],
    "post_screenfx": ["vulkan_post_screenfx_native"],
    "post_ssao": ["vulkan_post_ssao_native"],
    "shadow": ["vulkan_shadow_island_live"],
    "sky": ["vulkan_sky_island"],
    "terrain": ["vulkan_terrain_island", "vulkan_terrain_island_images", "vulkan_terrain_island_island", "vulkan_terrain_island_overlay",
                "vulkan_terrain_island_run"],
    "vfx": ["vulkan_vfx_island"],
    "water": ["vulkan_water_island", "vulkan_water_island_images", "vulkan_water_island_init", "vulkan_water_island_run"],
    "present": ["engine_present_vk"],
    "image_resource": ["vk_image_resource"],
    "shared_scene": ["vulkan_shared_scene_color", "vulkan_shared_scene_depth", "vk_first_layout"],
    "light_feed": ["vk_shared_light_feed"],
    "owners": ["vk_owned"],
    "context": ["vulkan_context"],
    "textures": ["vk_texture_residency"],
}

CREATE_RE = re.compile(r"\b(vkCreate|vkAllocate|vmaCreate)([A-Za-z0-9_]+)\s*\(")
DESTROY_RE = re.compile(r"\b(vkDestroy|vkFree|vmaDestroy)([A-Za-z0-9_]+)\s*\(")
# creation name -> the kind it releases through; a kind absent here is its own name
KIND = {"GraphicsPipelines": "Pipeline", "ComputePipelines": "Pipeline", "Pipelines": "Pipeline", "Memory": "Memory",
        "DescriptorSets": "DescriptorSets"}
# pool-owned or process-owned kinds: released with their parent, not one by one
PARENT_OWNED = {"DescriptorSets", "CommandBuffers", "Instance", "Device", "Allocator", "DebugUtilsMessengerEXT", "SwapchainKHR", "SurfaceKHR"}


def blank(text: str) -> str:
    """Comments and string/char literals replaced by spaces (newlines kept)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j])); i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + " " * max(0, j - i - 2) + c if j - i >= 2 else c); i = j
        else:
            out.append(c); i += 1
    return "".join(out)


def kind_of(name: str) -> str:
    if name in KIND:
        return KIND[name]
    if name.startswith("CommandBuffers"):
        return "CommandBuffers"
    return name


ADOPT_RE = re.compile(r"\bmc2vk::create(?:Checked)?\s*\(")


def adopted_spans(text: str) -> "list[tuple[int, int]]":
    """The character ranges of the `mc2vk::create(owner, device, call)` expressions: a creation inside one is handed to an owner, not released by hand."""
    spans = []
    for m in ADOPT_RE.finditer(text):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            depth += (text[i] == "(") - (text[i] == ")")
            i += 1
        spans.append((m.start(), i))
    return spans


def count_file(path: Path) -> "dict[str, list[int]]":
    """kind -> [created by hand, released by hand] for the file; a creation handed to an owner counts under no kind (it has no hand-written release)."""
    text = blank(path.read_text(encoding="utf-8", errors="replace"))
    spans = adopted_spans(text)
    kinds: "dict[str, list[int]]" = defaultdict(lambda: [0, 0])
    for m in CREATE_RE.finditer(text):
        if any(a <= m.start() < b for a, b in spans):
            continue
        kinds[kind_of(m.group(2))][0] += 1
    for m in DESTROY_RE.finditer(text):
        kinds[kind_of(m.group(2))][1] += 1
    return kinds


def collect() -> "dict[str, dict[str, dict[str, list[int]]]]":
    """island -> file stem -> kind -> [created, released]."""
    out: dict = {}
    for island, stems in ISLANDS.items():
        files = {}
        for stem in stems:
            for suffix in (".cpp", ".h"):
                p = SRC / (stem + suffix)
                if p.exists():
                    files[stem + suffix] = count_file(p)
        out[island] = files
    return out


def totals(kinds: "dict[str, list[int]]") -> "tuple[int, int]":
    """(created, released) over the kinds that are released one by one."""
    c = sum(v[0] for k, v in kinds.items() if k not in PARENT_OWNED)
    r = sum(v[1] for k, v in kinds.items() if k not in PARENT_OWNED)
    return c, r


def merge(files: "dict[str, dict[str, list[int]]]") -> "dict[str, list[int]]":
    out: "dict[str, list[int]]" = defaultdict(lambda: [0, 0])
    for kinds in files.values():
        for k, (c, r) in kinds.items():
            out[k][0] += c
            out[k][1] += r
    return out


def render(data) -> str:
    lines = ["<!-- doc-status: generated | tool: tools/vk_ownership_ledger.py --write -->",
             "# Vulkan object ownership ledger",
             "",
             "Generated by `py -3 tools/vk_ownership_ledger.py --write`; every number below was printed by that tool from the source (comments and strings blanked).",
             "WHAT: lexical creation tokens (`vkCreate*`, `vkAllocate*`, `vmaCreate*`) against release tokens (`vkDestroy*`, `vkFree*`, `vmaDestroy*`) per island and per file,",
             "kinds released one by one only (command buffers, descriptor sets and the instance / device / allocator are owned by their parent and listed apart).",
             "Imbalance = created - released; Manual = created + released, the lexical size of the hand-written ownership code left in the island (owner types hold the tokens for converted code,",
             "listed as island `owners`, where every create is paired with its destroy once).",
             "WHY: the RAII ownership rewrite converts one island at a time; an island is done when it creates and releases nothing by hand (Manual 0, so Imbalance 0) and its",
             "manual release code is deleted. One token is not one object at run time (a token inside a loop or a helper creates many), so the counts are a ratchet on the manual",
             "code and the proof of a finished conversion, not a leak detector.",
             "",
             "## Islands", "", "| Island | Files | Created | Released | Imbalance | Manual | Kinds out of balance |", "|---|---:|---:|---:|---:|---:|---|"]
    grand_c = grand_r = 0
    detail = []
    for island, files in data.items():
        if island == "owners":
            continue
        merged = merge(files)
        c, r = totals(merged)
        grand_c += c
        grand_r += r
        off = [f"{k} {v[0]}/{v[1]}" for k, v in sorted(merged.items()) if k not in PARENT_OWNED and v[0] != v[1]]
        lines.append(f"| {island} | {len(files)} | {c} | {r} | {c - r:+d} | {c + r} | {', '.join(off) or '-'} |")
        if len(files) > 1 or off:
            detail.append((island, files))
    lines += ["", f"Total over all islands: created {grand_c}, released {grand_r}, imbalance {grand_c - grand_r:+d}.", "",
              "## Files", "", "| File | Created | Released | Imbalance |", "|---|---:|---:|---:|"]
    for island, files in data.items():
        if island == "owners":
            continue
        for name, kinds in files.items():
            c, r = totals(kinds)
            if c or r:
                lines.append(f"| `{name}` | {c} | {r} | {c - r:+d} |")
    pending = sorted(((totals(merge(files))[0] + totals(merge(files))[1], island) for island, files in data.items() if island not in ("owners", "context")))
    lines += ["", "## Conversion order", "",
              "Converted (the shared context is the root owner of the instance, device and allocator and is not an island): " + (", ".join(i for m, i in pending if m == 0) or "none") + ". Remaining, smallest hand-written footprint first (the batch list of the ownership rewrite):", ""]
    lines += [f"{n}. {island} ({m} manual tokens)" for n, (m, island) in enumerate([p for p in pending if p[0] > 0], start=1)]
    owners = merge(data.get("owners", {}))
    lines += ["", "## Owner types (`vk_owned.h/.cpp`)", "",
              "Release tokens inside the destructors of the owner types: every one runs once, from the destructor of the object that owns the handle. Not counted above.", "",
              "| Kind | Release tokens |", "|---|---:|"]
    for k, v in sorted(owners.items()):
        if v[1]:
            lines.append(f"| {k} | {v[1]} |")
    lines += ["", "## Parent-owned kinds (not part of the balance)", "", "| Island | Kind | Created | Released |", "|---|---|---:|---:|"]
    for island, files in data.items():
        for k, v in sorted(merge(files).items()):
            if island != "owners" and k in PARENT_OWNED and (v[0] or v[1]):
                lines.append(f"| {island} | {k} | {v[0]} | {v[1]} |")
    return "\n".join(lines) + "\n"


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--src", default=None, help="count another copy of GameOS/gameos (a `git archive` of an earlier commit) instead of this tree; print only")
    args = ap.parse_args(argv)
    if args.src:
        global SRC
        SRC = Path(args.src)
    text = render(collect())
    if args.write:
        LEDGER.write_text(text, encoding="utf-8", newline="\n")
        print(f"vk_ownership_ledger: wrote {LEDGER.relative_to(ROOT)}")
        return 0
    if args.check:
        try:
            same = LEDGER.read_text(encoding="utf-8") == text
        except OSError:
            same = False
        print("vk_ownership_ledger: " + ("OK" if same else "STALE; run py -3 tools/vk_ownership_ledger.py --write"))
        return 0 if same else 1
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
