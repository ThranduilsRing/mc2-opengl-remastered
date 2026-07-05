#!/usr/bin/env python3
"""pak_dump.py -- reusable PacketFile (.pak) inspection tool.

MC2 .pak files are PacketFile-format archives (mclib/packet.h / packet.cpp).
This tool is read-only: it never mutates a .pak, never touches the engine
binary, and performs no gameplay/render work.

Format (verified against mclib/packet.h + packet.cpp):
  - header: u32 magic (0xFEEDFACE) + u32 firstPacketOffset
  - firstPacketOffset = (numPackets + 2) * 4  =>  numPackets = firstPacketOffset/4 - 2
  - seek table: numPackets * u32 entries, each = (offset & OFFSET_MASK) | (type << 29)
  - storage types (3 high bits): 0=RAW, 1=FWF, 2=LZD (mclib LZ), 3=HF (unsupported),
    4=ZLIB, 7=NUL (empty)
  - LZD/ZLIB payloads are prefixed with a u32 "unpacked length" before the
    compressed bytes.

The PacketFile reader / LZ decompressor here is reused verbatim from
tools/terrain_beautify/mission_terrain_analyzer.py (already verified against
mclib/packet.cpp and mclib/lzdecomp.cpp).
"""
from __future__ import annotations

import argparse
import string
import struct
import sys
import zlib
from pathlib import Path

# --- PacketFile constants (mclib/packet.h) -----------------------------------
PACKET_MAGIC = 0xFEEDFACE
TYPE_SHIFT = 29
OFFSET_MASK = (1 << TYPE_SHIFT) - 1
ST_RAW, ST_FWF, ST_LZD, ST_HF, ST_ZLIB, ST_NUL = 0x0, 0x1, 0x2, 0x3, 0x4, 0x7

STORAGE_NAMES = {
    ST_RAW: "RAW",
    ST_FWF: "FWF",
    ST_LZD: "LZD",
    ST_HF: "HF",
    ST_ZLIB: "ZLIB",
    ST_NUL: "NUL",
}


# --- LZ decoder (port of mclib/lzdecomp.cpp) ---------------------------------
def lzw_decompress(src: bytes) -> bytes:
    """Port of mclib/lzdecomp.cpp LZDecomp -- mclib's own LZW variant."""
    HASH_EOF, HASH_CLEAR, HASH_FREE = 257, 256, 258
    BASE_BITS, MAX_DICT = 9, 4096
    hash_chain = [0] * MAX_DICT
    hash_suffix = [0] * MAX_DICT
    dest = bytearray()
    src_len = len(src)
    bit_pos = 0

    def get_code(width: int) -> int:
        nonlocal bit_pos
        byte_idx = bit_pos >> 3
        val = 0
        for i in range(4):
            if byte_idx + i < src_len:
                val |= src[byte_idx + i] << (8 * i)
        code = (val >> (bit_pos & 7)) & ((1 << width) - 1)
        bit_pos += width
        return code

    code_width = BASE_BITS
    max_index = 1 << BASE_BITS
    free_index = HASH_FREE
    old_chain = old_suffix = 0
    while True:
        code = get_code(code_width)
        if code == HASH_EOF:
            break
        if code == HASH_CLEAR:
            code_width = BASE_BITS
            max_index = 1 << BASE_BITS
            free_index = HASH_FREE
            lit = get_code(code_width) & 0xFF
            dest.append(lit)
            old_chain = old_suffix = lit
            continue
        chain = code
        if chain >= free_index:
            hash_chain[chain] = old_chain
            hash_suffix[chain] = old_suffix & 0xFF
        stack = []
        cur = chain
        while cur >= 256:
            stack.append(hash_suffix[cur])
            cur = hash_chain[cur]
        stack.append(cur & 0xFF)
        old_suffix = cur & 0xFF
        for c in reversed(stack):
            dest.append(c)
        if free_index < MAX_DICT:
            hash_chain[free_index] = old_chain
            hash_suffix[free_index] = old_suffix
        free_index += 1
        old_chain = chain
        if free_index >= max_index and code_width < 12:
            code_width += 1
            max_index <<= 1
    return bytes(dest)


class PacketRecord:
    __slots__ = ("index", "storage_type", "offset", "raw_size", "payload")

    def __init__(self, index, storage_type, offset, raw_size, payload):
        self.index = index
        self.storage_type = storage_type
        self.offset = offset
        self.raw_size = raw_size
        self.payload = payload  # raw on-disk bytes for this packet (incl. any length prefix)


def read_packets(path: Path) -> list[PacketRecord]:
    """Parse the PacketFile seek table and slice out each packet's raw bytes."""
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
        out.append(PacketRecord(i, storage_type, offset, end - offset, data[offset:end]))
    return out


def decode_packet(record: PacketRecord) -> bytes | None:
    """Return decompressed bytes for a packet, or None if unsupported/corrupt."""
    t, payload = record.storage_type, record.payload
    if t in (ST_RAW, ST_FWF):
        return payload
    if t == ST_LZD:
        if len(payload) < 4:
            return None
        return lzw_decompress(payload[4:])
    if t == ST_ZLIB:
        if len(payload) < 4:
            return None
        unpacked_size = struct.unpack_from('<I', payload, 0)[0]
        try:
            dec = zlib.decompress(payload[4:])
        except zlib.error:
            return None
        if len(dec) != unpacked_size:
            # Non-fatal: report what we got, caller can inspect.
            pass
        return dec
    if t == ST_NUL:
        return b""
    # STORAGE_TYPE_HF (Huffman) -- "No Longer Supported" per packet.cpp.
    return None


def ascii_preview(data: bytes, n: int = 32) -> str:
    chunk = data[:n]
    out = []
    for b in chunk:
        c = chr(b)
        out.append(c if c in string.printable and c not in "\r\n\t\x0b\x0c" else ".")
    return "".join(out)


# --- subcommands --------------------------------------------------------------
def cmd_list(args) -> int:
    path = Path(args.pak)
    packets = read_packets(path)
    print(f"{path}: {len(packets)} packets")
    print(f"{'idx':>5}  {'type':<5} {'offset':>10} {'size':>10}  preview")
    for rec in packets:
        dec = decode_packet(rec)
        preview = ascii_preview(dec) if dec else "<undecoded>"
        type_name = STORAGE_NAMES.get(rec.storage_type, f"?{rec.storage_type}")
        print(f"{rec.index:>5}  {type_name:<5} {rec.offset:>10} {rec.raw_size:>10}  {preview}")
    return 0


def cmd_dump(args) -> int:
    path = Path(args.pak)
    packets = read_packets(path)
    if args.packet < 0 or args.packet >= len(packets):
        print(f"error: packet {args.packet} out of range (0..{len(packets)-1})", file=sys.stderr)
        return 1

    rec = packets[args.packet]
    dec = decode_packet(rec)
    if dec is None:
        print(f"error: packet {args.packet} storage type "
              f"{STORAGE_NAMES.get(rec.storage_type, rec.storage_type)} could not be decoded",
              file=sys.stderr)
        return 1

    if args.out:
        Path(args.out).write_bytes(dec)
        print(f"wrote {len(dec)} bytes to {args.out}")
        return 0

    # Heuristic: dump as text if it looks printable, else describe + hexdump head.
    sample = dec[:512]
    printable_ratio = sum(1 for b in sample if 32 <= b < 127 or b in (9, 10, 13)) / max(1, len(sample))
    if printable_ratio > 0.85:
        sys.stdout.buffer.write(dec)
    else:
        print(f"<binary packet, {len(dec)} bytes, storage={STORAGE_NAMES.get(rec.storage_type)}>")
        print(dec[:256].hex())
    return 0


def cmd_find_fit(args) -> int:
    path = Path(args.pak)
    packets = read_packets(path)
    needle = args.text.encode("ascii", errors="ignore")
    hits = 0
    for rec in packets:
        dec = decode_packet(rec)
        if dec is None:
            continue
        if needle in dec:
            hits += 1
            type_name = STORAGE_NAMES.get(rec.storage_type, f"?{rec.storage_type}")
            pos = dec.find(needle)
            ctx = dec[max(0, pos - 16):pos + len(needle) + 16]
            print(f"packet {rec.index} ({type_name}, {len(dec)} bytes): ...{ascii_preview(ctx, len(ctx))}...")
    if not hits:
        print(f"no packets contain '{args.text}'")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Inspect MC2 .pak (PacketFile) archives, read-only.")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_list = sub.add_parser("list", help="list packets with storage type + preview")
    p_list.add_argument("--pak", required=True)
    p_list.set_defaults(func=cmd_list)

    p_dump = sub.add_parser("dump", help="dump a decompressed packet to stdout or a file")
    p_dump.add_argument("--pak", required=True)
    p_dump.add_argument("--packet", required=True, type=int)
    p_dump.add_argument("--out", default=None)
    p_dump.set_defaults(func=cmd_dump)

    p_find = sub.add_parser("find-fit", help="scan all packets for an ASCII substring")
    p_find.add_argument("--pak", required=True)
    p_find.add_argument("--text", required=True)
    p_find.set_defaults(func=cmd_find_fit)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
