#!/usr/bin/env python3
"""pak_append.py -- append (or replace) a packet in an MC2 PacketFile (.pak).

MC2 .pak files are PacketFile-format archives (mclib/packet.h / packet.cpp).
This tool reads an existing .pak with the same parser used by
tools/pak_dump.py (verified against mclib/packet.cpp), then rewrites the
whole file with one packet appended (or an existing packet's payload
replaced), re-emitting every other packet byte-for-byte unchanged.

Format (mclib/packet.h):
  - header: u32 magic (0xFEEDFACE) + u32 firstPacketOffset
  - firstPacketOffset = (numPackets + 2) * 4   =>  TABLE_ENTRY(p) = (2+p)<<2
  - seek table: numPackets * u32, each = (offset & OFFSET_MASK) | (type << 29)
  - storage types (3 high bits): 0=RAW, 1=FWF, 2=LZD, 3=HF(unsupported),
    4=ZLIB, 7=NUL
  - ZLIB payload on disk = u32 unpacked_length + zlib.compress(data)

This tool only ever emits RAW or ZLIB packets (matches how new FIT text
packets already appear in object2.pak, e.g. packet 335 "Quonset" is ZLIB).

Usage:
  # append a new packet containing `data` (plain bytes/text) as the next
  # free index (== current numPackets); prints the new packet index.
  python pak_append.py append --pak object2.pak --text-file new_fit.txt

  # replace an existing packet's payload in place (packet count unchanged).
  python pak_append.py replace --pak object2.pak --packet 42 --text-file x.txt

Both subcommands write to --out (defaults to --pak, i.e. in place) and
never touch the input unless --out is omitted, in which case the caller
is expected to have already made a backup (this tool does not back up).
"""
from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

PACKET_MAGIC = 0xFEEDFACE
TYPE_SHIFT = 29
OFFSET_MASK = (1 << TYPE_SHIFT) - 1
ST_RAW, ST_FWF, ST_LZD, ST_HF, ST_ZLIB, ST_NUL = 0x0, 0x1, 0x2, 0x3, 0x4, 0x7


class PacketRecord:
    __slots__ = ("index", "storage_type", "payload")

    def __init__(self, index, storage_type, payload):
        self.index = index
        self.storage_type = storage_type
        self.payload = payload  # raw on-disk bytes (incl. any length prefix)


def read_packets(path: Path) -> list[PacketRecord]:
    data = path.read_bytes()
    if len(data) < 8:
        raise ValueError(f"{path}: file too small to be a PacketFile")

    magic = struct.unpack_from('<I', data, 0)[0]
    if magic != PACKET_MAGIC:
        raise ValueError(
            f"{path.name}: expected magic 0x{PACKET_MAGIC:08X}, got 0x{magic:08X} "
            "(checksum-armed PacketFile not supported)")

    first_off = struct.unpack_from('<I', data, 4)[0]
    num_packets = (first_off // 4) - 2
    if num_packets < 0 or num_packets > 1_000_000:
        raise ValueError(f"{path.name}: implausible numPackets={num_packets}")

    entries = [struct.unpack_from('<I', data, 8 + 4 * i)[0] for i in range(num_packets)]

    out = []
    for i in range(num_packets):
        storage_type = (entries[i] >> TYPE_SHIFT) & 0x7
        offset = entries[i] & OFFSET_MASK
        end = (entries[i + 1] & OFFSET_MASK) if i + 1 < num_packets else len(data)
        out.append(PacketRecord(i, storage_type, data[offset:end]))
    return out


def make_zlib_payload(raw: bytes) -> bytes:
    """On-disk ZLIB packet = u32 unpacked_length + zlib.compress(data)."""
    return struct.pack('<I', len(raw)) + zlib.compress(raw)


def write_pak(packets: list[PacketRecord], out_path: Path) -> None:
    num_packets = len(packets)
    first_off = (num_packets + 2) * 4  # TABLE_ENTRY(num_packets)

    # Compute each packet's on-disk offset (immediately after the seek table).
    offsets = []
    cur = first_off
    for rec in packets:
        offsets.append(cur)
        cur += len(rec.payload)

    header = struct.pack('<II', PACKET_MAGIC, first_off)
    seek_table = b"".join(
        struct.pack('<I', (offsets[i] & OFFSET_MASK) | (rec.storage_type << TYPE_SHIFT))
        for i, rec in enumerate(packets)
    )
    body = b"".join(rec.payload for rec in packets)

    out_path.write_bytes(header + seek_table + body)


def cmd_append(args) -> int:
    src = Path(args.pak)
    packets = read_packets(src)

    raw = Path(args.text_file).read_bytes() if args.text_file else args.bytes_literal.encode()
    payload = make_zlib_payload(raw)
    new_index = len(packets)
    packets.append(PacketRecord(new_index, ST_ZLIB, payload))

    out_path = Path(args.out) if args.out else src
    write_pak(packets, out_path)
    print(f"appended packet {new_index} ({len(raw)} bytes unpacked, "
          f"{len(payload)} bytes on disk, ZLIB) -> {out_path}")
    print(new_index)
    return 0


def cmd_replace(args) -> int:
    src = Path(args.pak)
    packets = read_packets(src)
    if args.packet < 0 or args.packet >= len(packets):
        print(f"error: packet {args.packet} out of range (0..{len(packets)-1})", file=sys.stderr)
        return 1

    raw = Path(args.text_file).read_bytes() if args.text_file else args.bytes_literal.encode()
    payload = make_zlib_payload(raw)
    packets[args.packet] = PacketRecord(args.packet, ST_ZLIB, payload)

    out_path = Path(args.out) if args.out else src
    write_pak(packets, out_path)
    print(f"replaced packet {args.packet} ({len(raw)} bytes unpacked, ZLIB) -> {out_path}")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Append/replace a packet in an MC2 PacketFile (.pak).")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_append = sub.add_parser("append", help="append a new packet at the next free index")
    p_append.add_argument("--pak", required=True)
    p_append.add_argument("--text-file", default=None, help="file whose bytes become the packet payload")
    p_append.add_argument("--bytes-literal", default=None, help="literal string payload (alt to --text-file)")
    p_append.add_argument("--out", default=None, help="output path (default: overwrite --pak)")
    p_append.set_defaults(func=cmd_append)

    p_replace = sub.add_parser("replace", help="replace an existing packet's payload in place")
    p_replace.add_argument("--pak", required=True)
    p_replace.add_argument("--packet", required=True, type=int)
    p_replace.add_argument("--text-file", default=None)
    p_replace.add_argument("--bytes-literal", default=None)
    p_replace.add_argument("--out", default=None)
    p_replace.set_defaults(func=cmd_replace)

    args = parser.parse_args(argv)
    if not args.text_file and not args.bytes_literal:
        print("error: need --text-file or --bytes-literal", file=sys.stderr)
        return 1
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
