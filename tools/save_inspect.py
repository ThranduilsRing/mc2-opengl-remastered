#!/usr/bin/env python3
"""save_inspect - read a quicksave (testgame.ims) packet by packet: map it, diff two of them, digest it.

WHAT: opens the PacketFile a Mission::save wrote, unpacks the zlib packets, and offers
  diff A B   the first differing packet and byte offset, the number of differing packets and bytes, and a per-record-class table of where the
             differences sit (a class is named by the packet's byte size, which is how the loader tells the object records apart)
  map A      one line per run of packets of one kind (kind, first index, count, bytes)
  digest A   a short digest of every run, the form tools/saveload_roundtrip.py stores as a baseline
WHY: "the save is a function of the game state" is only checkable by comparing two files; a bare cmp says that they differ, not where. The
object records, the pilots and the ABL environment are raw structs and byte strings, so a difference of a few bytes at a fixed offset in a
class names an uninitialised padding byte or an unsaved-but-derived field directly.

  py -3 tools/save_inspect.py diff a.ims b.ims
  py -3 tools/save_inspect.py map a.ims
  py -3 tools/save_inspect.py selftest
"""
from __future__ import annotations

import argparse
import hashlib
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import save_craft_stale as scs  # noqa: E402

#: Packet storage types in the PacketFile table: 0 raw, 4 zlib, 7 unwritten slot.
NUL = scs.NUL
#: Record class by payload byte size (code/save_layout_guard.h pins these; the object loader dispatches on them).
RECORD_SIZES = {}
#: A run of this many written packets of one kind is split so a digest mismatch still points into the run.
CHUNK = 256


def load(path: str):
    """-> (first word, [(storage type, payload bytes)]); the same reader tools/save_craft_stale.py uses."""
    return scs.read(path)


def kind(typ: int, size: int) -> str:
    """The class of one packet: unwritten, a named record by size, or `data` for everything else."""
    if typ == NUL:
        return "empty"
    return RECORD_SIZES.get(size, "data")


def first_diff(a: bytes, b: bytes) -> int:
    """Offset of the first differing byte; the shorter length when one is a prefix of the other; -1 when equal."""
    n = min(len(a), len(b))
    if a[:n] != b[:n]:
        lo, hi = 0, n
        while hi - lo > 1:                      # binary search on prefix equality: bytes compare in C
            mid = (lo + hi) // 2
            if a[lo:mid] == b[lo:mid]:
                lo = mid
            else:
                hi = mid
        return lo
    return -1 if len(a) == len(b) else n


def compare(pa, pb) -> dict:
    """Compare two unpacked packet lists. -> {"equal", "packets_a", "packets_b", "differing", "first": (index, offset, kind, size_a, size_b) | None,
    "by_kind": {kind: [differing packets, differing bytes]}}."""
    out = {"equal": True, "packets_a": len(pa), "packets_b": len(pb), "differing": 0, "first": None, "by_kind": {}}
    for i in range(max(len(pa), len(pb))):
        ta, ra = pa[i] if i < len(pa) else (NUL, b"")
        tb, rb = pb[i] if i < len(pb) else (NUL, b"")
        if ta == tb and ra == rb:
            continue
        out["equal"] = False
        out["differing"] += 1
        off = first_diff(ra, rb)
        k = kind(ta, len(ra))
        if out["first"] is None:
            out["first"] = (i, off, k, len(ra), len(rb))
        n = min(len(ra), len(rb))
        nbytes = sum(1 for x, y in zip(ra[:n], rb[:n]) if x != y) + abs(len(ra) - len(rb))
        row = out["by_kind"].setdefault(k, [0, 0])
        row[0] += 1
        row[1] += nbytes
    return out


def describe(res: dict) -> list:
    """Report lines for a compare() result."""
    if res["equal"]:
        return [f"identical: {res['packets_a']} packets"]
    i, off, k, sa, sb = res["first"]
    lines = [f"DIFFERENT: {res['differing']} of {max(res['packets_a'], res['packets_b'])} packets differ; first is packet {i} ({k}, {sa} vs {sb} bytes) at offset {off}"]
    for k2, (n, nb) in sorted(res["by_kind"].items(), key=lambda kv: -kv[1][0]):
        lines.append(f"  {k2}: {n} packet(s), {nb} byte(s) differ")
    return lines


def runs(pk) -> list:
    """-> [(kind, first index, packet count, total bytes, digest)] over runs of packets of one kind, long runs cut into CHUNK packets."""
    out = []
    i = 0
    while i < len(pk):
        k = kind(pk[i][0], len(pk[i][1]))
        j = i
        h = hashlib.sha256()
        total = 0
        while j < len(pk) and (k == "empty" or j - i < CHUNK) and kind(pk[j][0], len(pk[j][1])) == k:
            h.update(len(pk[j][1]).to_bytes(4, "little"))
            h.update(pk[j][1])
            total += len(pk[j][1])
            j += 1
        out.append((k, i, j - i, total, h.hexdigest()[:16]))
        i = j
    return out


def overall(pk) -> str:
    """One digest over every packet's size and payload."""
    h = hashlib.sha256()
    for typ, raw in pk:
        h.update(bytes([typ & 0xFF]))
        h.update(len(raw).to_bytes(4, "little"))
        h.update(raw)
    return h.hexdigest()[:16]


def compare_runs(base: list, cur: list):
    """Compare two runs() lists (a baseline's, as lists, and a fresh one). -> (ok, message)."""
    for n, (b, c) in enumerate(zip(base, cur)):
        if list(b) != list(c):
            why = "digest" if list(b[:4]) == list(c[:4]) else "shape"
            return False, (f"run {n} differs ({why}): baseline {b[0]} packets {b[1]}..{b[1] + b[2] - 1} ({b[3]} bytes, {b[4]}), "
                           f"now {c[0]} packets {c[1]}..{c[1] + c[2] - 1} ({c[3]} bytes, {c[4]})")
    if len(base) != len(cur):
        return False, f"the save has {len(cur)} runs, the baseline {len(base)}"
    return True, f"all {len(cur)} runs match"


def selftest() -> int:
    fails = []

    def expect(cond, what):
        if not cond:
            fails.append(what)

    expect(first_diff(b"abcd", b"abcd") == -1, "equal bytes give -1")
    expect(first_diff(b"abcd", b"abXd") == 2, "the first differing byte is found")
    expect(first_diff(b"abcd", b"abcdef") == 4, "a prefix differs at its end")
    expect(first_diff(b"Xbcd", b"abcd") == 0, "a difference at offset 0 is found")
    expect(first_diff(b"abcdefgh" * 100, b"abcdefgh" * 99 + b"abcdefgX") == 799, "the binary search lands on the last byte")
    base = [(0, b"ver"), (4, b"x" * 40), (4, b"y" * 40), (NUL, b""), (4, b"z" * 8)]
    same = [(t, bytes(r)) for t, r in base]
    expect(compare(base, same)["equal"], "identical packet lists compare equal")
    mod = [(t, bytearray(r)) for t, r in base]
    mod[2][1][7] = ord("Q")
    mod[4][1][0] = ord("Q")
    mod = [(t, bytes(r)) for t, r in mod]
    res = compare(base, mod)
    expect(not res["equal"] and res["first"][:2] == (2, 7) and res["differing"] == 2, "the first differing packet and offset are named")
    expect("packet 2" in " ".join(describe(res)), "describe names the first packet")
    old = dict(RECORD_SIZES)
    RECORD_SIZES[40] = "Rec40"
    try:
        res = compare(base, mod)
        expect(res["by_kind"].get("Rec40") == [1, 1] and res["by_kind"].get("data") == [1, 1], "differences are counted per record class")
        expect([r[0] for r in runs(base)] == ["data", "Rec40", "empty", "data"], "runs group packets of one kind")
    finally:
        RECORD_SIZES.clear()
        RECORD_SIZES.update(old)
    expect(overall(base) == overall(same) and overall(base) != overall(mod), "the overall digest sees a changed byte")
    long_run = [(4, bytes([i % 251]) * 5) for i in range(600)]
    expect(len(runs(long_run)) == 3, "a long run is cut into chunks")
    expect(len(runs([(NUL, b"")] * 600)) == 1, "a run of unwritten slots is one run")
    ok, _ = compare_runs(runs(base), runs(same))
    expect(ok, "equal runs match")
    ok, msg = compare_runs(runs(base), runs(mod))
    expect(not ok and "packets" in msg, "a changed run is named")
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "t.ims")
        scs.write(p, [(0, b"ver"), (4, b"x" * 40), (NUL, b"")], False)
        first, pk = load(p)
        expect(len(pk) == 3 and pk[1] == (4, b"x" * 40), "load reads back what save_craft_stale wrote")
    for f in fails:
        print("save_inspect selftest FAIL: " + f)
    print("save_inspect selftest: " + ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("diff")
    d.add_argument("a")
    d.add_argument("b")
    m = sub.add_parser("map")
    m.add_argument("a")
    g = sub.add_parser("digest")
    g.add_argument("a")
    sub.add_parser("selftest")
    args = ap.parse_args(argv)
    if args.cmd == "selftest":
        return selftest()
    if args.cmd == "diff":
        res = compare(load(args.a)[1], load(args.b)[1])
        print("\n".join(describe(res)))
        return 0 if res["equal"] else 1
    pk = load(args.a)[1]
    if args.cmd == "map":
        for k, i, n, total, dg in runs(pk):
            print(f"{k:24s} packets {i:6d}..{i + n - 1:6d} ({n:5d}) {total:9d} bytes")
    else:
        print(overall(pk))
    return 0


if __name__ == "__main__":
    sys.exit(main())
