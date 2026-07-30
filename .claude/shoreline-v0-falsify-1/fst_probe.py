#!/usr/bin/env python3
"""Extract named members from an MC2 .fst FastFile (read-only)."""
import struct, sys, zlib, hashlib
from pathlib import Path

ENTRY = struct.Struct("<IIII250s")


def members(path):
    data = Path(path).read_bytes()
    ver, n = struct.unpack_from("<II", data, 0)
    out = []
    for i in range(n):
        off, size, real, h, name = ENTRY.unpack_from(data, 8 + i * 266)
        name = name.split(b"\0", 1)[0].decode("latin-1")
        out.append((name, off, size, real))
    return data, ver, out


def extract(data, off, size, real):
    raw = data[off:off + size]
    if size == real:
        return raw
    try:
        return zlib.decompress(raw)
    except zlib.error:
        return None


if __name__ == "__main__":
    fst = sys.argv[1]
    pat = sys.argv[2]
    outdir = Path(sys.argv[3]) if len(sys.argv) > 3 else None
    data, ver, ms = members(fst)
    print(f"version=0x{ver:08X} numFiles={len(ms)}")
    for name, off, size, real in ms:
        if pat.lower() in name.lower():
            blob = extract(data, off, size, real)
            md5 = hashlib.md5(blob).hexdigest()[:12] if blob else "DECOMP-FAIL"
            n = len(blob) if blob else -1
            print(f"{name}  csize={size} real={real} got={n} md5={md5}")
            if outdir and blob:
                outdir.mkdir(parents=True, exist_ok=True)
                (outdir / Path(name).name).write_bytes(blob)
