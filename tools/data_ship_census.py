#!/usr/bin/env python3
"""data_ship_census.py — shipped-data audit census for MC2 install trees (DATA-SHIP-AUDIT-1).

Answers, for one or more deployed install roots (e.g.
A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0):

  1. CENSUS      — per-extension count + MB, per-top-dir MB, top-N largest files
                   (loose tree), plus per-archive entry/size stats for every
                   root-level .fst FastFile.
  2. SHADOWS     — which loose files under the install root SHADOW an FST entry
                   with the same normalized key (File::open resolves loose FIRST,
                   mclib/file.cpp: mod > base-loose > base-strip > fastfile > cd).
                   Size prefilter + LZW/zlib decompress + sha256 confirms
                   identical-vs-override.  Identical shadows are redundant bytes,
                   EXCEPT bypass-loader types (BYPASS_LOADER_EXTS below): those
                   loaders never consult the FST, so their loose copies are
                   load-bearing and are reported separately as do-not-delete.
  3. DUPES       — same-content loose files at different paths (size-bucketed
                   sha256), ranked by reclaimable MB.
  4. STALE-GEN   — heuristic generations: .tga with a same-stem .ktx2 sidecar
                   (droppable under MC2_TEXMGR_KTX_PRIMARY>=1, cf. txmmgr.cpp
                   ROUTE-2), numeric-size-subdir files whose base-strip parent
                   twin exists (file.cpp strip rule), and .beauty sidecar dirs.
  5. DRIFT       — when two roots are given, data/-tree relpath+size drift
                   (only-in-A / only-in-B / size-changed).

FST binary format ground truth: mclib/ffile.h (+ tools/mod_install/fst_listing.py).
LZW decompressor is the proven port of LZDecompClassicLZW_ (lzdecomp.cpp) already
used verbatim in tools/mc2x_import/fst.py — duplicated here to keep this script
stdlib-only and standalone (same pattern as tools/fst_repack_drop.py).

USAGE
-----
  py -3 tools/data_ship_census.py "<install_root>" [second_root]
       [--top 20] [--no-shadow] [--no-dupes] [--deep-mb 8] [--json out.json]

  --deep-mb N   confirm shadow content by decompress+hash only when the entry's
                realSize <= N MB (default 8); larger size-matching pairs are
                reported as "size-match (content unverified)".
  --no-shadow / --no-dupes  skip the expensive passes (census is always run).

Read-only with respect to the install: nothing under the roots is written.
Python 3 stdlib only.  Exit 0 on success, 1 on bad args / parse failure.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib
from collections import defaultdict

MB = 1024.0 * 1024.0

# ---------------------------------------------------------------------------
# Extensions whose loaders BYPASS File::open (raw fopen / SDL / ffmpeg / Assimp /
# std::ifstream) and therefore CANNOT be served from an FST archive.  An
# "identical FST shadow" of one of these types is NOT safe to delete: the FST
# copy is unreachable at runtime and deletion silently degrades the engine.
# Shadow analysis routes them to "identical_bypass_loader" (do-not-delete)
# instead of "identical" so a manifest-driven dedupe can never remove them.
# (Hardening added after the DATA-SHIP-DEDUPE-1 regression re-check, 2026-07-01;
#  that dedupe was verified clean — this is belt-and-suspenders for next time.)
#   .ktx2        RenderCore/KtxLoader.cpp std::fopen; also the .burnin.ktx2
#                colormap probe (mclib/terrtxm2.cpp ~1677) — raw fopen, silent
#                low-capability fallback when absent
#   .wav         SDL_LoadWAV / Mix_LoadWAV by path (gameos_sound.cpp)
#   .bik         ffmpeg avformat by path
#   .glb / .fbx  Assimp by path; probe is fileExists(FILE_ON_DISK) — loose-only
#                by design (mclib/msl.cpp LoadFromFile)
#   .json        modern config readers use std::ifstream (terrain_materials.json,
#                visual_tuning.json, model_overrides manifests)
BYPASS_LOADER_EXTS = {".ktx2", ".wav", ".bik", ".glb", ".fbx", ".json"}

# ---------------------------------------------------------------------------
# FST parsing (format: mclib/ffile.h, 266-byte packed entries)
# ---------------------------------------------------------------------------
FST_VERSION_ZLIB = 0xCADDECAF
FST_VERSION_LZ = 0xFADDECAF
ENTRY_SIZE = 266
_ENTRY = struct.Struct("<IIII250s")

# LZW dictionary constants (lzdecomp.cpp LINUX_BUILD path)
HASH_CLEAR, HASH_EOF, HASH_FREE = 256, 257, 258
MAX_BITS, BASE_BITS = 12, 9


def lzw_decomp(src: bytes, dest_max: int) -> bytes:
    """Classic variable-width LZW decompress (proven port, tools/mc2x_import/fst.py)."""
    parent = [0] * 4096
    suffix = [0] * 4096
    src_len = len(src)
    src_pos = 0
    bit_buf = 0
    bits_in = 0
    dest = bytearray()

    def read_code(nbits):
        nonlocal src_pos, bit_buf, bits_in
        while bits_in < nbits:
            if src_pos >= src_len:
                return -1
            bit_buf |= src[src_pos] << bits_in
            bits_in += 8
            src_pos += 1
        code = bit_buf & ((1 << nbits) - 1)
        bit_buf >>= nbits
        bits_in -= nbits
        return code

    code_width = BASE_BITS
    max_code = 1 << code_width
    free_idx = HASH_FREE
    prev_code = -1
    prev_first = 0
    stk = []
    while True:
        code = read_code(code_width)
        if code < 0 or code == HASH_EOF:
            break
        if code == HASH_CLEAR:
            code_width = BASE_BITS
            max_code = 1 << code_width
            free_idx = HASH_FREE
            prev_code = -1
            stk.clear()
            continue
        c = code
        stk.clear()
        if c < free_idx:
            while c >= HASH_FREE:
                stk.append(suffix[c])
                c = parent[c]
            first_byte = c & 0xFF
            stk.append(first_byte)
        elif c == free_idx and prev_code >= 0:
            stk.append(prev_first)
            pc = prev_code
            while pc >= HASH_FREE:
                stk.append(suffix[pc])
                pc = parent[pc]
            first_byte = pc & 0xFF
            stk.append(first_byte)
        else:
            raise ValueError(f"LZW: invalid code {code}")
        while stk:
            if len(dest) >= dest_max:
                return bytes(dest)
            dest.append(stk.pop())
        if prev_code >= 0 and free_idx < (1 << MAX_BITS):
            parent[free_idx] = prev_code
            suffix[free_idx] = first_byte
            free_idx += 1
            if free_idx >= max_code and code_width < MAX_BITS:
                code_width += 1
                max_code = 1 << code_width
        prev_code = code
        prev_first = first_byte
    return bytes(dest)


def normalize_key(path: str) -> str:
    return path.replace("\\", "/").lower()


def parse_fst(path: str):
    """Return (is_lz, [{name,key,offset,size,realSize,hash}]) or raise ValueError."""
    with open(path, "rb") as f:
        head = f.read(8)
        if len(head) != 8:
            raise ValueError("truncated header")
        version, num = struct.unpack("<II", head)
        if version not in (FST_VERSION_ZLIB, FST_VERSION_LZ):
            raise ValueError(f"bad version 0x{version:08X}")
        table = f.read(ENTRY_SIZE * num)
    entries = []
    for i in range(num):
        off, size, real, h, raw = _ENTRY.unpack_from(table, i * ENTRY_SIZE)
        name = raw.split(b"\x00", 1)[0].decode("latin1")
        entries.append({"name": name, "key": normalize_key(name),
                        "offset": off, "size": size, "realSize": real, "hash": h})
    return version == FST_VERSION_LZ, entries


def fst_read_entry(path: str, e: dict, is_lz: bool) -> bytes:
    """Decompress one entry, mirroring the engine's LZDecomp dispatch
    (mclib/lzdecomp.cpp): sniff the zlib magic PER PAYLOAD, fall back to
    classic LZW.  The archive-level 0xFADDECAF stamp does NOT mean the
    payloads are LZW — makefst zlib-compresses everything regardless
    (ffile.cpp readFast: 'ALL files in the fast file are now zLib
    compressed. NO EXCEPTIONS!!')."""
    with open(path, "rb") as f:
        f.seek(e["offset"])
        raw = f.read(e["size"])
    if e["size"] == e["realSize"]:
        return raw
    looks_zlib = (len(raw) >= 2 and (raw[0] & 0x0F) == 0x08
                  and ((raw[0] * 256 + raw[1]) % 31) == 0)
    if looks_zlib:
        try:
            return zlib.decompress(raw)
        except zlib.error:
            pass
    return lzw_decomp(raw, e["realSize"])


# ---------------------------------------------------------------------------
# Loose-tree census
# ---------------------------------------------------------------------------
def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def walk_tree(root: str):
    """Yield (relkey, abspath, size) for every file under root (skips symlinks)."""
    for dirpath, dirnames, filenames in os.walk(root):
        for fn in filenames:
            ap = os.path.join(dirpath, fn)
            try:
                st = os.stat(ap)
            except OSError:
                continue
            rel = os.path.relpath(ap, root)
            yield normalize_key(rel), ap, st.st_size


def ext_of(key: str) -> str:
    base = key.rsplit("/", 1)[-1]
    dot = base.rfind(".")
    return base[dot:] if dot > 0 else "(none)"


def census_tree(root: str, files):
    by_ext = defaultdict(lambda: [0, 0])       # ext -> [count, bytes]
    by_topdir = defaultdict(lambda: [0, 0])    # first path component -> [count, bytes]
    total = [0, 0]
    largest = []
    for key, ap, size in files:
        e = ext_of(key)
        by_ext[e][0] += 1
        by_ext[e][1] += size
        top = key.split("/", 1)[0] if "/" in key else "(root)"
        by_topdir[top][0] += 1
        by_topdir[top][1] += size
        total[0] += 1
        total[1] += size
        largest.append((size, key))
    largest.sort(reverse=True)
    return {"total_files": total[0], "total_bytes": total[1],
            "by_ext": {k: {"count": v[0], "bytes": v[1]} for k, v in
                       sorted(by_ext.items(), key=lambda kv: -kv[1][1])},
            "by_topdir": {k: {"count": v[0], "bytes": v[1]} for k, v in
                          sorted(by_topdir.items(), key=lambda kv: -kv[1][1])},
            "largest": largest}


# ---------------------------------------------------------------------------
# Analyses
# ---------------------------------------------------------------------------
def shadow_analysis(root: str, loose_index: dict, fst_infos: list, deep_mb: float):
    """loose_index: key -> (abspath, size). fst_infos: [(fst_path, is_lz, entries)]."""
    res = {"identical": [], "identical_bypass_loader": [], "override_size": [],
           "override_content": [], "size_match_unverified": [], "decomp_errors": []}
    deep_bytes = deep_mb * MB
    for fst_path, is_lz, entries in fst_infos:
        fst_name = os.path.basename(fst_path)
        for e in entries:
            hit = loose_index.get(e["key"])
            if hit is None:
                continue
            ap, lsize = hit
            rec = {"key": e["key"], "fst": fst_name,
                   "loose_bytes": lsize, "fst_comp_bytes": e["size"],
                   "fst_real_bytes": e["realSize"]}
            if lsize != e["realSize"]:
                res["override_size"].append(rec)
                continue
            if e["realSize"] > deep_bytes:
                res["size_match_unverified"].append(rec)
                continue
            try:
                content = fst_read_entry(fst_path, e, is_lz)
            except (ValueError, zlib.error, OSError) as ex:
                rec["error"] = str(ex)
                res["decomp_errors"].append(rec)
                continue
            if hashlib.sha256(content).hexdigest() == sha256_file(ap):
                if ext_of(e["key"]) in BYPASS_LOADER_EXTS:
                    rec["do_not_delete"] = ("bypass-loader extension: engine loads "
                                            "this type outside File::open; the FST "
                                            "copy is unreachable at runtime")
                    res["identical_bypass_loader"].append(rec)
                else:
                    res["identical"].append(rec)
            else:
                res["override_content"].append(rec)
    return res


def dupe_analysis(loose_files):
    """loose_files: [(key, abspath, size)] -> duplicate groups ranked by waste."""
    by_size = defaultdict(list)
    for key, ap, size in loose_files:
        if size > 0:
            by_size[size].append((key, ap))
    groups = []
    for size, items in by_size.items():
        if len(items) < 2:
            continue
        by_hash = defaultdict(list)
        for key, ap in items:
            try:
                by_hash[sha256_file(ap)].append(key)
            except OSError:
                continue
        for h, keys in by_hash.items():
            if len(keys) > 1:
                groups.append({"bytes_each": size, "count": len(keys),
                               "waste_bytes": size * (len(keys) - 1),
                               "sha256": h[:16], "paths": sorted(keys)})
    groups.sort(key=lambda g: -g["waste_bytes"])
    return groups


def stale_gen_analysis(loose_index: dict):
    """Heuristic stale generations over the loose tree."""
    keys = set(loose_index)
    tga_with_ktx2 = []
    for key in keys:
        if key.endswith(".tga"):
            sidecar = key[:-4] + ".ktx2"
            if sidecar in keys:
                tga_with_ktx2.append({"tga": key, "tga_bytes": loose_index[key][1],
                                      "ktx2_bytes": loose_index[sidecar][1]})
    tga_with_ktx2.sort(key=lambda r: -r["tga_bytes"])

    # numeric size-subdir twin: data/tgl/128/foo.tga whose strip parent
    # data/tgl/foo.tga also exists (File::open base-strip rule).
    strip_twins = []
    for key in keys:
        parts = key.split("/")
        for i, p in enumerate(parts[:-1]):
            if p.isdigit():
                stripped = "/".join(parts[:i] + parts[i + 1:])
                if stripped in keys:
                    strip_twins.append({"sized": key, "sized_bytes": loose_index[key][1],
                                        "parent": stripped,
                                        "parent_bytes": loose_index[stripped][1]})
                break
    strip_twins.sort(key=lambda r: -r["sized_bytes"])

    beauty = defaultdict(lambda: [0, 0])
    for key, (ap, size) in loose_index.items():
        if "/" in key and any(seg.endswith(".beauty") for seg in key.split("/")[:-1]):
            top = key.split("/")
            root_seg = next(seg for seg in top if seg.endswith(".beauty"))
            beauty[root_seg][0] += 1
            beauty[root_seg][1] += size
    return {"tga_with_ktx2": tga_with_ktx2, "strip_twins": strip_twins,
            "beauty_dirs": {k: {"count": v[0], "bytes": v[1]} for k, v in
                            sorted(beauty.items(), key=lambda kv: -kv[1][1])}}


def drift_analysis(index_a: dict, index_b: dict):
    keys_a, keys_b = set(index_a), set(index_b)
    only_a = sorted(keys_a - keys_b)
    only_b = sorted(keys_b - keys_a)
    changed = sorted(k for k in keys_a & keys_b if index_a[k][1] != index_b[k][1])
    return {"only_in_first": only_a, "only_in_second": only_b, "size_changed": changed,
            "only_in_first_bytes": sum(index_a[k][1] for k in only_a),
            "only_in_second_bytes": sum(index_b[k][1] for k in only_b)}


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def fmt_mb(b):
    return f"{b / MB:8.1f}"


def print_census(root, cen, fst_stats, top):
    print(f"\n=== CENSUS: {root} ===")
    print(f"loose files: {cen['total_files']}  total {fmt_mb(cen['total_bytes'])} MB")
    print("\n-- by extension (top 25 by MB) --")
    print(f"{'ext':>12} {'count':>7} {'MB':>10}")
    for ext, v in list(cen["by_ext"].items())[:25]:
        print(f"{ext:>12} {v['count']:>7} {fmt_mb(v['bytes']):>10}")
    print("\n-- by top-level dir --")
    for d, v in cen["by_topdir"].items():
        print(f"{d:>20} {v['count']:>7} {fmt_mb(v['bytes']):>10}")
    print(f"\n-- top {top} largest loose files --")
    for size, key in cen["largest"][:top]:
        print(f"{fmt_mb(size):>10}  {key}")
    if fst_stats:
        print("\n-- FST archives (root level) --")
        print(f"{'archive':>16} {'entries':>8} {'compMB':>8} {'realMB':>8}  top ext by realMB")
        for s in fst_stats:
            exts = ", ".join(f"{e}:{fmt_mb(b).strip()}" for e, b in s["ext_real"][:4])
            print(f"{s['name']:>16} {s['entries']:>8} {fmt_mb(s['comp']):>8}"
                  f" {fmt_mb(s['real']):>8}  {exts}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roots", nargs="+", help="install root(s); 2nd root = drift compare")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--deep-mb", type=float, default=8.0)
    ap.add_argument("--no-shadow", action="store_true")
    ap.add_argument("--no-dupes", action="store_true")
    ap.add_argument("--json", dest="json_out", default=None)
    args = ap.parse_args(argv)

    out = {"roots": []}
    indexes = []
    for root in args.roots:
        root = os.path.abspath(root)
        if not os.path.isdir(root):
            print(f"ERROR: not a directory: {root}", file=sys.stderr)
            return 1
        files = list(walk_tree(root))
        # exclude the .fst archives themselves + logs from the loose census? No:
        # census everything, but shadow/dupe/stale operate on data-ish files only.
        loose_index = {key: (ap_, size) for key, ap_, size in files}
        indexes.append(loose_index)
        cen = census_tree(root, files)

        fst_infos, fst_stats = [], []
        for fn in sorted(os.listdir(root)):
            if not fn.lower().endswith(".fst"):
                continue
            fp = os.path.join(root, fn)
            try:
                is_lz, entries = parse_fst(fp)
            except (ValueError, OSError) as e:
                print(f"WARNING: {fn}: {e}", file=sys.stderr)
                continue
            fst_infos.append((fp, is_lz, entries))
            ext_real = defaultdict(int)
            for e in entries:
                ext_real[ext_of(e["key"])] += e["realSize"]
            fst_stats.append({"name": fn, "entries": len(entries),
                              "comp": sum(e["size"] for e in entries),
                              "real": sum(e["realSize"] for e in entries),
                              "ext_real": sorted(ext_real.items(), key=lambda kv: -kv[1])})

        print_census(root, cen, fst_stats, args.top)
        root_rec = {"root": root, "census": {k: cen[k] for k in
                    ("total_files", "total_bytes", "by_ext", "by_topdir")},
                    "largest": [{"bytes": s, "key": k} for s, k in cen["largest"][:args.top]],
                    "fst": [{k: s[k] for k in ("name", "entries", "comp", "real")}
                            for s in fst_stats]}

        if not args.no_shadow and fst_infos:
            sh = shadow_analysis(root, loose_index, fst_infos, args.deep_mb)
            n_ident = len(sh["identical"])
            n_over = len(sh["override_size"]) + len(sh["override_content"])
            n_unv = len(sh["size_match_unverified"])
            ident_bytes = sum(r["loose_bytes"] for r in sh["identical"])
            fst_keys = {e["key"] for _, _, es in fst_infos for e in es}
            n_match = sum(1 for k in loose_index if k in fst_keys)
            print(f"\n-- SHADOW (loose vs FST) --")
            print(f"loose files whose key exists in an FST: {n_match}")
            n_bypass = len(sh["identical_bypass_loader"])
            print(f"  identical content (redundant shadow): {n_ident}"
                  f"  ({fmt_mb(ident_bytes).strip()} MB loose)")
            if n_bypass:
                print(f"  identical but BYPASS-LOADER type — DO NOT DELETE: {n_bypass}"
                      f"  (loader reads loose only; FST copy unreachable)")
                for r in sh["identical_bypass_loader"][:10]:
                    print(f"    keep-loose: {r['key']}")
            print(f"  overrides (differ by size/content):   {n_over}")
            print(f"  size-match unverified (> deep-mb):    {n_unv}")
            if sh["decomp_errors"]:
                print(f"  decompress errors:                    {len(sh['decomp_errors'])}")
            for r in sh["identical"][:10]:
                print(f"    identical: {r['key']} ({fmt_mb(r['loose_bytes']).strip()} MB)")
            root_rec["shadow"] = {k: sh[k] for k in sh}
        if not args.no_dupes:
            groups = dupe_analysis(files)
            waste = sum(g["waste_bytes"] for g in groups)
            print(f"\n-- DUPLICATES (same content, different path) --")
            print(f"groups: {len(groups)}  reclaimable if deduped: {fmt_mb(waste).strip()} MB")
            for g in groups[:args.top]:
                print(f"  {fmt_mb(g['waste_bytes']):>10} MB waste x{g['count']}: "
                      f"{g['paths'][0]}  (+{len(g['paths'])-1} more)")
            root_rec["dupes"] = groups
        stale = stale_gen_analysis(loose_index)
        tga_b = sum(r["tga_bytes"] for r in stale["tga_with_ktx2"])
        strip_b = sum(r["sized_bytes"] for r in stale["strip_twins"])
        beauty_b = sum(v["bytes"] for v in stale["beauty_dirs"].values())
        print(f"\n-- STALE-GEN heuristics --")
        print(f".tga with same-stem .ktx2 sidecar: {len(stale['tga_with_ktx2'])}"
              f"  ({fmt_mb(tga_b).strip()} MB of .tga)")
        print(f"numeric-subdir file with strip-parent twin: {len(stale['strip_twins'])}"
              f"  ({fmt_mb(strip_b).strip()} MB in sized copies)")
        print(f".beauty sidecar dirs: {len(stale['beauty_dirs'])}"
              f"  ({fmt_mb(beauty_b).strip()} MB total)")
        root_rec["stale_gen"] = stale
        out["roots"].append(root_rec)

    if len(indexes) == 2:
        dr = drift_analysis(indexes[0], indexes[1])
        print(f"\n=== DRIFT {args.roots[0]}  vs  {args.roots[1]} ===")
        print(f"only in first:  {len(dr['only_in_first'])} files"
              f" ({fmt_mb(dr['only_in_first_bytes']).strip()} MB)")
        print(f"only in second: {len(dr['only_in_second'])} files"
              f" ({fmt_mb(dr['only_in_second_bytes']).strip()} MB)")
        print(f"size changed:   {len(dr['size_changed'])} files")
        out["drift"] = dr

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1)
        print(f"\nJSON written: {args.json_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
