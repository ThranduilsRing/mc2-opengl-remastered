#!/usr/bin/env python3
"""save_inspect - read a quicksave (testgame.ims) packet by packet: map it, diff two of them, digest it.

WHAT: opens the PacketFile a Mission::save wrote, unpacks the zlib packets, and offers
  diff A B   the first differing packet and byte offset, the number of differing packets and bytes, and a per-record-class table of where the
             differences sit (a class is named by the packet's byte size, which is how the loader tells the object records apart);
             --mask FILE [--across-builds] classifies every differing byte as inside or outside the known-noise mask (tools/save_noise_mask.json)
  ranges A B the differing byte ranges per record class, coalesced over small gaps: how a mask entry is measured from two runs of one exe
  map A      one line per run of packets of one kind (kind, first index, count, bytes)
  digest A   a short digest of every run, the form tools/saveload_roundtrip.py stores as a baseline
  upgrade A B  (or --upgrade A B) rewrite a v1 save (MC2SAVE.WQT2) as v2: new packet-0 stamp, and the legacy R2 packet after END becomes the
             v2 section container (ANIM + ORDQ, code/save_sections.h); a v1 file without that packet gets an empty container (all defaults)
WHY: "the save is a function of the game state" is only checkable by comparing two files; a bare cmp says that they differ, not where. The
object records, the pilots and the ABL environment are raw structs and byte strings, so a difference of a few bytes at a fixed offset in a
class names an uninitialised padding byte or an unsaved-but-derived field directly.

  py -3 tools/save_inspect.py diff a.ims b.ims
  py -3 tools/save_inspect.py diff old.ims new.ims --mask tools/save_noise_mask.json --across-builds
  py -3 tools/save_inspect.py map a.ims
  py -3 tools/save_inspect.py --upgrade old_v1.ims new_v2.ims
  py -3 tools/save_inspect.py selftest
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import struct
import math
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import save_craft_stale as scs  # noqa: E402

#: Packet storage types in the PacketFile table: 0 raw, 4 zlib, 7 unwritten slot.
NUL = scs.NUL
#: Record class by payload byte size. code/save_layout_guard.h pins these numbers in the build (tools/test_save_inspect.py compares the two); the object
#: loader dispatches on them. StackItem is not a packet and is not listed.
RECORD_SIZES = {372: "GameObjectData", 3288: "MoverData", 720: "TerrainObjectData", 756: "BuildingData", 820: "TurretData", 760: "GateData",
                432: "ArtilleryData", 396: "CarnageData", 3992: "MechData", 3424: "GroundVehicleData", 600: "WeaponBoltData",
                60: "ObjectManagerData", 40: "StaticMechWarriorData", 19864: "MechWarriorData", 80: "TeamStaticData", 488: "TeamData",
                8: "StaticCommanderData", 1288: "CommanderData"}
PILOT_SIZE = 19864
PILOT_SUFFIX_MAX = 16 * 1024
#: A run of this many written packets of one kind is split so a digest mismatch still points into the run.
CHUNK = 256
#: Save v2 (code/save_sections.h): packet-0 stamps and the section container that follows END.
STAMP_V1 = b"MC2SAVE.WQT2\0"
STAMP_V2 = b"MC2SAVE.V2\0"
SECTION_MAGIC = b"MC2SECTS"
SECTION_CONTAINER_VERSION = 2
R2_TAG = 0x4E413252
REQUIRED = 1


def fourcc(text: str) -> int:
    return int.from_bytes(text.encode("ascii"), "little")


def encode_sections(sections) -> bytes:
    """[(tag, version, flags, payload)] -> the container bytes Mission::save writes: sorted by tag, reserved words zero."""
    tags = [s[0] for s in sections]
    if len(set(tags)) != len(tags):
        raise ValueError("duplicate section")
    out = bytearray(SECTION_MAGIC + struct.pack("<IIII", SECTION_CONTAINER_VERSION, 0, 0, len(sections)))
    for tag, version, flags, payload in sorted(sections, key=lambda s: s[0]):
        out += struct.pack("<IIIII", tag, version, flags, 0, len(payload)) + payload
    return bytes(out)


def decode_sections(raw: bytes):
    """Container bytes -> [(tag, version, flags, payload)]; raises ValueError on a malformed container."""
    if raw[:8] != SECTION_MAGIC:
        raise ValueError("not a section container")
    version, _, _, count = struct.unpack_from("<IIII", raw, 8)
    if version != SECTION_CONTAINER_VERSION:
        raise ValueError(f"container version {version}")
    pos, out = 24, []
    for _ in range(count):
        tag, sv, flags, _, n = struct.unpack_from("<IIIII", raw, pos)
        pos += 20
        if pos + n > len(raw):
            raise ValueError("truncated section")
        out.append((tag, sv, flags, raw[pos:pos + n]))
        pos += n
    if pos != len(raw):
        raise ValueError("trailing bytes")
    return out


def upgrade_r2(raw: bytes):
    """The v1 R2 packet (tag, version 2/3, u32-length animation blob, order queue) -> ANIM + ORDQ sections; b"" -> no sections."""
    if not raw:
        return []
    tag, version, n = struct.unpack_from("<III", raw, 0)
    if tag != R2_TAG or version not in (2, 3) or 12 + n > len(raw):
        raise ValueError("not a v1 R2 packet")
    return [(fourcc("ANIM"), version, REQUIRED, raw[12:12 + n]), (fourcc("ORDQ"), 1, REQUIRED, raw[12 + n:])]


def end_index(pk) -> int:
    ends = [i for i, (typ, raw) in enumerate(pk) if typ != NUL and raw == b"END\0"]
    if not ends:
        raise ValueError("no END packet")
    return ends[-1]


def upgrade_packets(pk):
    """v1 packet list -> v2 packet list: only the stamp and the slot after END change. Raises ValueError for a file that is not a v1 save."""
    if pk and pk[0][1] == STAMP_V2:
        raise ValueError("already a v2 save")
    if not pk or pk[0][1] != STAMP_V1:
        raise ValueError("packet 0 is not the v1 stamp MC2SAVE.WQT2")
    end = end_index(pk)
    out = [(0, STAMP_V2)] + list(pk[1:])
    r2 = out[end + 1][1] if end + 1 < len(out) and out[end + 1][0] != NUL else b""
    container = (4, encode_sections(upgrade_r2(r2)))
    if end + 1 < len(out):
        out[end + 1] = container
    else:
        out.append(container)
    return out


def upgrade(src: str, dst: str) -> list:
    """Write the v2 form of the v1 save `src` to `dst`; -> its sections."""
    first, pk = scs.read(src)
    out = upgrade_packets(pk)
    scs.write(dst, out, first != 0xFEEDFACE)
    return decode_sections(out[end_index(out) + 1][1])


def load(path: str):
    """-> (first word, [(storage type, payload bytes)]); the same reader tools/save_craft_stale.py uses."""
    return scs.read(path)


def kind(typ: int, size: int) -> str:
    """The class of one packet: unwritten, a named record by size, or `data` for everything else."""
    if typ == NUL:
        return "empty"
    return RECORD_SIZES.get(size, "data")


def is_text(raw: bytes) -> bool:
    """A packet that is a FIT/INI text file (the mission settings, the logistics data): its first bytes are printable ASCII or line breaks."""
    head = raw[:256]
    return bool(head) and all(c in (9, 10, 13) or 32 <= c < 127 for c in head)


def valid_pilot_suffix(raw: bytes) -> bool:
    """Validate the WQT2 v2 schema written by SavedQueueTargets, including exact consumption."""
    if not PILOT_SIZE < len(raw) <= PILOT_SIZE + PILOT_SUFFIX_MAX:
        return False
    data = raw[PILOT_SIZE:]
    pos = 0

    def take(fmt):
        nonlocal pos
        values = struct.unpack_from("<" + fmt, data, pos)
        pos += struct.calcsize("<" + fmt)
        return values

    try:
        if take("II") != (0x32545157, 2):
            return False
        count, = take("I")
        if count > 16:
            return False
        if any(t > 0x7fffffff for t in take("I" * count)):
            return False
        steps, = take("I")
        if steps > 256:
            return False
        for _ in range(steps):
            step = take("iii6fiii")
            if not all(math.isfinite(v) for v in step[3:9]):
                return False
        cursor, = take("i")
        if not -1 <= cursor <= (steps if steps else 256):
            return False
        goals, = take("I")
        if goals > 256:
            return False
        take("h" * goals)
        return pos == len(data)
    except struct.error:
        return False


def packet_kind(typ: int, raw: bytes) -> str:
    """Text takes precedence; variable pilot records require a validated suffix."""
    if typ == NUL:
        return "empty"
    if raw[:8] == SECTION_MAGIC:
        return "sections"
    if raw[:8] == b"OBMH\x01\0\0\0" and len(raw) == 68:
        return "ObjectManagerData"
    if (raw[:8] == b"WATC\x01\0\0\0" and len(raw) >= 12
            and len(raw) == 12 + 4 * struct.unpack_from("<I", raw, 8)[0]):
        return "WatchList"
    if raw[:8] == b"CMST\x01\0\0\0" and len(raw) == 16:
        return "StaticCommanderData"
    if raw[:8] == b"WSTA\x01\0\0\0" and len(raw) == 48:
        return "StaticMechWarriorData"
    if raw[:8] == b"WARR\x01\0\0\0" and len(raw) > 8:
        return "MechWarriorData"
    if raw[:8] == b"CMDR\x01\0\0\0" and len(raw) == 1248:
        return "CommanderData"
    if raw[:8] == b"TMST\x01\0\0\0" and len(raw) == 88:
        return "TeamStaticData"
    if raw[:8] == b"TEAM\x01\0\0\0" and len(raw) >= 496:
        return "TeamData"
    if is_text(raw):
        return "text"
    if valid_pilot_suffix(raw):
        return "MechWarriorData"
    return kind(typ, len(raw))


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


def load_mask(path: str) -> dict:
    """-> {record class: {"run": [(lo, hi)], "build": [(lo, hi)]}} from a noise-mask file (tools/save_noise_mask.json shows the format)."""
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    return {cls: {scope: [(int(lo), int(hi)) for lo, hi in e.get(scope, [])] for scope in ("run", "build")} for cls, e in doc["classes"].items()}


def masked(mask: dict, k: str, off: int, across_builds: bool) -> bool:
    """True when byte `off` of a record of class `k` lies in a mask range that applies: run-to-run ranges always, cross-build ranges on request."""
    e = mask.get(k)
    if not e:
        return False
    spans = e["run"] + (e["build"] if across_builds else [])
    return any(lo <= off <= hi for lo, hi in spans)


def compare(pa, pb, mask: dict | None = None, across_builds: bool = False) -> dict:
    """Compare two unpacked packet lists. -> {"equal", "packets_a", "packets_b", "differing", "first": (index, offset, kind, size_a, size_b) | None,
    "first_binary": the same for the first packet that is not a FIT text file, "by_kind": {kind: [differing packets, differing bytes]}}.
    With a mask the result also has "masked" {kind: bytes inside the mask}, "outside" {kind: [packets, bytes]} and "first_outside" (index, offset, kind) | None;
    a packet whose two sizes differ counts as wholly outside."""
    out = {"equal": True, "packets_a": len(pa), "packets_b": len(pb), "differing": 0, "first": None, "first_binary": None, "by_kind": {},
           "masked": {}, "outside": {}, "first_outside": None}
    for i in range(max(len(pa), len(pb))):
        ta, ra = pa[i] if i < len(pa) else (NUL, b"")
        tb, rb = pb[i] if i < len(pb) else (NUL, b"")
        if ta == tb and ra == rb:
            continue
        out["equal"] = False
        out["differing"] += 1
        off = first_diff(ra, rb)
        k = packet_kind(ta, ra)
        if out["first"] is None:
            out["first"] = (i, off, k, len(ra), len(rb))
        if out["first_binary"] is None and k != "text":
            out["first_binary"] = (i, off, k, len(ra), len(rb))
        n = min(len(ra), len(rb))
        nbytes = sum(1 for x, y in zip(ra[:n], rb[:n]) if x != y) + abs(len(ra) - len(rb))
        row = out["by_kind"].setdefault(k, [0, 0])
        row[0] += 1
        row[1] += nbytes
        if mask is not None:
            if len(ra) != len(rb) or ta != tb:
                bad, inside = [min(len(ra), len(rb))], 0
            else:
                offs = [o for o, (x, y) in enumerate(zip(ra, rb)) if x != y]
                bad = [o for o in offs if not masked(mask, k, o, across_builds)]
                inside = len(offs) - len(bad)
            if inside:
                out["masked"][k] = out["masked"].get(k, 0) + inside
            if bad:
                orow = out["outside"].setdefault(k, [0, 0])
                orow[0] += 1
                orow[1] += len(bad) if len(ra) == len(rb) and ta == tb else nbytes
                if out["first_outside"] is None:
                    out["first_outside"] = (i, bad[0], k)
    return out


def describe(res: dict) -> list:
    """Report lines for a compare() result."""
    if res["equal"]:
        return [f"identical: {res['packets_a']} packets"]
    i, off, k, sa, sb = res["first"]
    lines = [f"DIFFERENT: {res['differing']} of {max(res['packets_a'], res['packets_b'])} packets differ; first is packet {i} ({k}, {sa} vs {sb} bytes) at offset {off}"]
    if res["first_binary"] and res["first_binary"] != res["first"]:
        i, off, k, sa, sb = res["first_binary"]
        lines.append(f"first differing non-text packet: {i} ({k}, {sa} vs {sb} bytes) at offset {off}")
    for k2, (n, nb) in sorted(res["by_kind"].items(), key=lambda kv: -kv[1][0]):
        lines.append(f"  {k2}: {n} packet(s), {nb} byte(s) differ")
    if res["masked"] or res["outside"]:
        lines.append("with the noise mask: " + (", ".join(f"{k2} {nb} byte(s) masked" for k2, nb in sorted(res["masked"].items())) or "nothing masked"))
        if res["first_outside"]:
            i, off, k2 = res["first_outside"]
            lines.append(f"OUTSIDE THE MASK: first at packet {i} ({k2}) offset {off}")
            for k2, (n, nb) in sorted(res["outside"].items(), key=lambda kv: -kv[1][0]):
                lines.append(f"  {k2}: {n} packet(s), {nb} byte(s) outside the mask")
        else:
            lines.append("every differing byte lies inside the mask")
    return lines


def diff_ranges(pa, pb, gap: int = 16) -> dict:
    """-> {record class: [(lo, hi)]}: the byte offsets at which any same-sized packet pair of that class differs, ranges less than `gap` bytes apart joined."""
    seen: dict = {}
    for (ta, ra), (tb, rb) in zip(pa, pb):
        if ta != tb or len(ra) != len(rb) or ra == rb:
            continue
        seen.setdefault(packet_kind(ta, ra), set()).update(o for o, (x, y) in enumerate(zip(ra, rb)) if x != y)
    out = {}
    for k, offs in seen.items():
        spans: list = []
        for o in sorted(offs):
            if spans and o - spans[-1][1] <= gap:
                spans[-1][1] = o
            else:
                spans.append([o, o])
        out[k] = [tuple(sp) for sp in spans]
    return out


def runs(pk) -> list:
    """-> [(kind, first index, packet count, total bytes, digest)] over runs of packets of one kind, long runs cut into CHUNK packets."""
    out = []
    i = 0
    while i < len(pk):
        k = packet_kind(*pk[i])
        j = i
        h = hashlib.sha256()
        total = 0
        while j < len(pk) and (k == "empty" or j - i < CHUNK) and packet_kind(*pk[j]) == k:
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
    base = [(0, b"ver"), (4, b"" * 40), (4, b"" * 40), (NUL, b""), (4, b"" * 8)]
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
    RECORD_SIZES.clear()
    RECORD_SIZES[40] = "Rec40"
    try:
        res = compare(base, mod)
        expect(res["by_kind"].get("Rec40") == [1, 1] and res["by_kind"].get("data") == [1, 1], "differences are counted per record class")
        expect([r[0] for r in runs(base)] == ["text", "Rec40", "empty", "data"], "runs group packets of one kind")
        expect(packet_kind(4, b"[Part1]\nx = 1\n") == "text" and packet_kind(4, b"\x00\x01" * 21) == "data", "a FIT text packet is told from binary data")
    finally:
        RECORD_SIZES.clear()
        RECORD_SIZES.update(old)
    expect(overall(base) == overall(same) and overall(base) != overall(mod), "the overall digest sees a changed byte")
    long_run = [(4, bytes([1 + i % 3]) * 5) for i in range(600)]
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
    old = dict(RECORD_SIZES)
    RECORD_SIZES.clear()
    RECORD_SIZES[40] = "Rec40"
    try:
        a = [(0, b"ver"), (4, bytes(40)), (4, bytes(40))]
        b = [(t, bytearray(r)) for t, r in a]
        b[1][1][5] = 1                       # a run-to-run byte
        b[2][1][30] = 1                      # a cross-build byte
        b = [(t, bytes(r)) for t, r in b]
        mask = {"Rec40": {"run": [(4, 6)], "build": [(28, 31)]}}
        res = compare(a, b, mask)
        expect(res["masked"] == {"Rec40": 1} and res["outside"]["Rec40"] == [1, 1] and res["first_outside"] == (2, 30, "Rec40"),
               "a build-scope range is not applied without --across-builds")
        res = compare(a, b, mask, across_builds=True)
        expect(res["first_outside"] is None and res["masked"] == {"Rec40": 2}, "both scopes mask when comparing builds")
        res = compare([(4, bytes(40))], [(4, bytes(41))], mask, True)
        expect(res["first_outside"] is not None, "packets of different sizes are outside the mask")
        res = compare([(0, b"abc")], [(0, b"abd")], mask, True)
        expect(res["outside"].get("text") == [1, 1], "a class with no mask entry is never masked")
        expect(diff_ranges(a, b, gap=2) == {"Rec40": [(5, 5), (30, 30)]} and diff_ranges(a, b, gap=40) == {"Rec40": [(5, 30)]},
               "ranges join over the gap")
    finally:
        RECORD_SIZES.clear()
        RECORD_SIZES.update(old)
    golden = bytes.fromhex("4d43325345435453" "02000000" "00000000" "00000000" "02000000"
                           "414e494d" "03000000" "01000000" "00000000" "01000000" "aa"
                           "4f524451" "01000000" "01000000" "00000000" "02000000" "0102")   # tests/unit/test_save_sections.cpp
    r2 = struct.pack("<III", R2_TAG, 3, 1) + b"\xaa" + b"\x01\x02"
    expect(encode_sections(upgrade_r2(r2)) == golden, "an upgraded R2 packet encodes to the C++ golden container")
    expect(decode_sections(golden) == upgrade_r2(r2), "the golden container decodes to ANIM + ORDQ")
    v1 = [(0, STAMP_V1), (0, b"cmp\0"), (4, b"x" * 40), (0, b"\x01\0\0\0"), (0, b"END\0"), (4, r2), (NUL, b"")]
    v2 = upgrade_packets(v1)
    expect(v2[0] == (0, STAMP_V2) and v2[5] == (4, golden) and v2[1:5] == v1[1:5] and v2[6] == v1[6],
           "upgrade rewrites only the stamp and the R2 slot")
    expect(packet_kind(*v2[5]) == "sections", "the container is its own packet kind")
    expect(decode_sections(upgrade_packets(v1[:5])[5][1]) == [], "a v1 file without the R2 packet gets an empty container")
    expect(decode_sections(upgrade_packets(v1[:5] + [(NUL, b"")])[5][1]) == [], "an empty slot after END is no R2 packet")
    for bad in (v2, [(0, b"nope\0")] + v1[1:], v1[:4]):
        try:
            upgrade_packets(bad)
            expect(False, "a non-v1 packet list is refused")
        except ValueError:
            pass
    with tempfile.TemporaryDirectory() as td:
        a, b = os.path.join(td, "v1.ims"), os.path.join(td, "v2.ims")
        scs.write(a, v1, False)
        upgrade(a, b)
        expect(load(b)[1] == v2, "upgrade round-trips through the packet file")
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
    d.add_argument("--mask", help="noise-mask JSON; the exit code is then 0 only when every differing byte is inside it")
    d.add_argument("--across-builds", action="store_true", help="also apply the mask's cross-build ranges (the two saves come from different exes)")
    r = sub.add_parser("ranges")
    r.add_argument("a")
    r.add_argument("b")
    r.add_argument("--gap", type=int, default=16)
    m = sub.add_parser("map")
    m.add_argument("a")
    g = sub.add_parser("digest")
    g.add_argument("a")
    u = sub.add_parser("upgrade")
    u.add_argument("a")
    u.add_argument("b")
    sub.add_parser("selftest")
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "--upgrade":
        argv[0] = "upgrade"
    args = ap.parse_args(argv)
    if args.cmd == "selftest":
        return selftest()
    if args.cmd == "diff":
        mask = load_mask(args.mask) if args.mask else None
        res = compare(load(args.a)[1], load(args.b)[1], mask, args.across_builds)
        print("\n".join(describe(res)))
        if mask is not None:
            return 1 if res["first_outside"] else 0
        return 0 if res["equal"] else 1
    if args.cmd == "upgrade":
        try:
            sections = upgrade(args.a, args.b)
        except ValueError as err:
            print(f"save_inspect upgrade: refused: {err}")
            return 1
        print(f"wrote {args.b}: v2, " + (", ".join(f"{t.to_bytes(4, 'little').decode()} v{v} {len(p)} bytes" for t, v, _, p in sections)
                                         or "no sections (defaults)"))
        return 0
    if args.cmd == "ranges":
        for k, spans in sorted(diff_ranges(load(args.a)[1], load(args.b)[1], args.gap).items()):
            print(f"{k}: " + " ".join(f"{lo}-{hi}" for lo, hi in spans))
        return 0
    pk = load(args.a)[1]
    if args.cmd == "map":
        for k, i, n, total, dg in runs(pk):
            print(f"{k:24s} packets {i:6d}..{i + n - 1:6d} ({n:5d}) {total:9d} bytes")
    else:
        print(overall(pk))
    return 0


if __name__ == "__main__":
    sys.exit(main())
