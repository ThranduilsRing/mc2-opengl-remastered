#!/usr/bin/env python3
"""Legacy FST repacker: refuses entry removal; stock archives stay complete."""
import struct
import sys

ENTRY_SIZE = 266
NAME_OFF = 16
NAME_LEN = 250


def parse(data):
    version, num = struct.unpack_from("<II", data, 0)
    entries = []
    for i in range(num):
        b = 8 + i * ENTRY_SIZE
        off, size, real, h = struct.unpack_from("<IIII", data, b)
        name_field = data[b + NAME_OFF:b + NAME_OFF + NAME_LEN]
        name = name_field.split(b"\0")[0].decode("latin1")
        entries.append({
            "name": name, "off": off, "size": size, "real": real,
            "hash": h, "name_field": name_field,
            "payload": bytes(data[off:off + size]),
        })
    return version, entries


def drop_pred(name):
    n = name.lower().replace("\\", "/")
    return n.startswith("data/tgl/128/") and n.endswith(".tga")


def repack(src, dst, dry_run=False):
    data = open(src, "rb").read()
    version, entries = parse(data)
    kept = [e for e in entries if not drop_pred(e["name"])]
    dropped = len(entries) - len(kept)
    dropped_bytes = sum(e["size"] for e in entries if drop_pred(e["name"]))
    print(f"[fst_repack] {src}: {len(entries)} entries -> {len(kept)} kept, "
          f"{dropped} dropped ({dropped_bytes/1048576:.1f} MB compressed payload)")
    if dropped:
        raise ValueError("FST entry removal is forbidden; slim loose files only")
    if dry_run:
        return len(entries), len(kept)
    if src != dst:
        with open(dst, "wb") as out:
            out.write(data)
    return len(entries), len(entries)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: fst_repack_drop.py <src.fst> <dst.fst> [--dry-run]")
        sys.exit(2)
    repack(sys.argv[1], sys.argv[2], "--dry-run" in sys.argv)
