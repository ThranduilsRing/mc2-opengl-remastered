#!/usr/bin/env python3
"""residency_slim.py - residency-aware slim deploy for data/tgl/128 (v1).

Slimming moves only observed GPU-only loose tgas into a local backup.
The complete stock FST is the fallback beneath loose overrides and must never
lose entries. Neither apply nor restore writes an FST (including .preslim).
"""
import argparse
import os
import re
import shutil
import sys

# Reuse the FST parser for the non-removing legacy API.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fst_repack_drop as fstlib  # parse(), ENTRY_SIZE

BACKUP_DIR = "_tga_slim_backup"
TEXLOAD_RE = re.compile(r"\[TEXLOAD\]\s+uniq=(\d+)\s+name=(.+?)\s*$")


def load_trace(trace_paths):
    """Parse [TEXLOAD] lines -> (observed_paint:set, observed:set) of /128 basenames.
    A texture is CPU_RGBA_REQUIRED iff it was EVER loaded with uniq!=0."""
    paint, seen = set(), set()
    for tp in trace_paths:
        try:
            with open(tp, "r", errors="ignore") as f:
                for line in f:
                    m = TEXLOAD_RE.search(line)
                    if not m:
                        continue
                    uniq = int(m.group(1))
                    name = m.group(2).replace("\\", "/").lower()
                    if "/data/tgl/128/" in ("/" + name) or name.startswith("data/tgl/128/"):
                        base = os.path.basename(name)
                        if base.endswith(".tga"):
                            seen.add(base)
                            if uniq != 0:
                                paint.add(base)
        except OSError:
            pass
    return paint, seen


def classify(d128, observed_paint, observed):
    """Ground-truth classify. DROP only textures POSITIVELY observed as
    GPU-only (never painted) that also have a .ktx2. Everything else is kept:
    paint (CPU_RGBA_REQUIRED), no-ktx2 (no GPU substitute), and unobserved
    (cannot prove GPU-only -> conservative keep)."""
    tgas = [f for f in os.listdir(d128) if f.lower().endswith(".tga")]
    ktx_stems = {os.path.splitext(f)[0].lower()
                 for f in os.listdir(d128) if f.lower().endswith(".ktx2")}
    drop, keep_cpu, keep_noktx, keep_unobs = set(), [], [], []
    for t in tgas:
        low = t.lower()
        stem = os.path.splitext(low)[0]
        has_ktx = stem in ktx_stems
        if low in observed_paint:
            keep_cpu.append(t)                     # observed painted -> CPU_RGBA_REQUIRED
        elif not has_ktx:
            keep_noktx.append(t)                   # no GPU substitute
        elif low not in observed:
            keep_unobs.append(t)                   # never observed -> conservative keep
        else:
            drop.add(t)                            # observed GPU-only + has ktx2 -> safe
    return drop, {
        "drop": drop, "keep_cpu_required": keep_cpu,
        "keep_no_ktx2": keep_noktx, "unknown": keep_unobs,  # unobserved == "unknown"
    }


def repack_fst(src_fst, dst_fst, drop_paths_lower):
    """Legacy API: entry removal is forbidden, including calls outside main()."""
    if drop_paths_lower:
        raise ValueError("FST entry removal is forbidden; slim loose files only")
    data = open(src_fst, "rb").read()
    _, entries = fstlib.parse(data)
    if os.path.abspath(src_fst) != os.path.abspath(dst_fst):
        shutil.copyfile(src_fst, dst_fst)
    return len(entries), len(entries), 0


def main():
    ap = argparse.ArgumentParser(description="residency-aware slim deploy for data/tgl/128 (v1)")
    ap.add_argument("deploy_root")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--dry-run", action="store_true", help="classify + print only (default)")
    g.add_argument("--apply", action="store_true", help="move DROP loose tgas to backup; keep stock FST intact")
    g.add_argument("--restore", action="store_true", help="undo a prior --apply")
    ap.add_argument("--trace", nargs="+", default=[],
                    help="[TEXLOAD] trace file(s) (MC2_TEXMGR_LOAD_TRACE=1 output / "
                         "smoke artifact *.log). Ground truth for paint vs GPU-only. "
                         "Wider mission coverage -> fewer 'unknown' (unobserved) -> more reclaim.")
    ap.add_argument("--allow-unknown", action="store_true",
                    help="proceed even though some textures were never observed in the trace "
                         "(they are still KEPT -- conservative slim, smaller reclaim).")
    args = ap.parse_args()

    d128 = os.path.join(args.deploy_root, "data", "tgl", "128")
    bk = os.path.join(d128, BACKUP_DIR)
    if not os.path.isdir(d128):
        print(f"FAIL: {d128} not found"); sys.exit(2)

    if args.restore:
        n = 0
        if os.path.isdir(bk):
            for f in os.listdir(bk):
                os.replace(os.path.join(bk, f), os.path.join(d128, f)); n += 1
            os.rmdir(bk)
        print(f"[slim] restored {n} tgas"); return

    observed_paint, observed = load_trace(args.trace)
    if not args.trace:
        print("FAIL: --trace is required (ground truth). Run a smoke with "
              "MC2_TEXMGR_LOAD_TRACE=1 and pass the artifact *.log files.")
        sys.exit(2)
    drop, b = classify(d128, observed_paint, observed)
    def sz(names): return sum(os.path.getsize(os.path.join(d128, n)) for n in names)
    drop_bytes = sz(drop)
    print("[slim] VALIDATOR:")
    print(f"  trace: {len(observed)} textures observed, {len(observed_paint)} painted")
    print(f"  drop_count               = {len(drop)}")
    print(f"  drop_bytes               = {drop_bytes/1048576:.1f} MB")
    print(f"  keep_cpu_required_count  = {len(b['keep_cpu_required'])}  (observed paint)")
    print(f"  keep_no_ktx2_count       = {len(b['keep_no_ktx2'])}")
    print(f"  keep_terrain_splat_count = 0   (out of /128 scope)")
    print(f"  keep_colormap_count      = 0   (out of /128 scope)")
    print(f"  unknown_count            = {len(b['unknown'])}  (unobserved -> kept conservatively)")
    if b["unknown"] and not args.allow_unknown:
        print(f"FAIL: {len(b['unknown'])} textures never observed in the trace; cannot prove "
              "GPU-only. Widen trace coverage (more missions) to reclaim them, or pass "
              "--allow-unknown to slim now (unobserved stay KEPT -> smaller, safe reclaim).")
        sys.exit(1)

    if not args.apply:
        print("[slim] dry-run only (use --apply to move loose files).")
        return

    # Loose overrides may be slimmed; the stock archive stays complete.
    os.makedirs(bk, exist_ok=True)
    for n in drop:
        os.replace(os.path.join(d128, n), os.path.join(bk, n))
    print(f"[slim] moved {len(drop)} loose tgas to {BACKUP_DIR}/ ; kept "
          f"{len(b['keep_cpu_required'])} cpu + {len(b['keep_no_ktx2'])} no-ktx2")


if __name__ == "__main__":
    main()
