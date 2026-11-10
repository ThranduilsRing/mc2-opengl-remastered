#!/usr/bin/env python3
"""check_deployed_spv.py -- is the SPIR-V DEPLOYED in an install the SPIR-V the build produced?

Compares <install>/shaders/vulkan/*.spv with the build tree's <build>/shaders/vulkan/*.spv:
  1. every build .spv is present in the install (FAIL if missing);
  2. identical sha256 (FAIL, named);
  3. the descriptor (set, binding, kind) and push-constant ABI parsed from each deployed .spv equals the build's
     (FAIL naming shader and binding; this names WHAT changed when the bytes differ, and parses with no SDK tool);
  4. an install .spv the build does not produce is an orphan (WARN; FAIL with --strict-orphans).
Seconds, no game run. Pure Python (reads SPIR-V words directly).

    py -3 tools/check_deployed_spv.py --install <dir> [--build build64-ninja] [--strict-orphans]
Exit 0 = ok (orphans may WARN), 1 = FAIL, 2 = usage/missing dir.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

MAGIC = 0x07230203
OP_TYPE_BOOL, OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_VECTOR, OP_TYPE_MATRIX = 20, 21, 22, 23, 24
OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_SAMPLED_IMAGE, OP_TYPE_ARRAY, OP_TYPE_RTARRAY = 25, 26, 27, 28, 29
OP_TYPE_STRUCT, OP_TYPE_POINTER, OP_CONSTANT, OP_VARIABLE = 30, 32, 43, 59
OP_DECORATE, OP_MEMBER_DECORATE = 71, 72
DEC_BLOCK, DEC_BUFFER_BLOCK, DEC_ARRAY_STRIDE, DEC_SET, DEC_BINDING, DEC_OFFSET = 2, 3, 6, 34, 33, 35
SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_PUSH, SC_STORAGE_BUFFER = 0, 2, 9, 12


def _kind(sc, ty, types, decos):
    """Descriptor kind name for a variable's pointee type."""
    while types.get(ty, (0,))[0] in (OP_TYPE_ARRAY, OP_TYPE_RTARRAY):
        ty = types[ty][1]
    op = types.get(ty, (0,))[0]
    if sc == SC_STORAGE_BUFFER:
        return "storage_buffer"
    if sc == SC_UNIFORM:
        return "storage_buffer" if (ty, DEC_BUFFER_BLOCK) in decos else "uniform_buffer"
    return {OP_TYPE_IMAGE: "image", OP_TYPE_SAMPLER: "sampler", OP_TYPE_SAMPLED_IMAGE: "combined_image_sampler"}.get(op, f"op{op}")


def _size(ty, types, member_off):
    t = types.get(ty)
    if t is None:
        return 0
    op = t[0]
    if op in (OP_TYPE_INT, OP_TYPE_FLOAT):
        return t[1] // 8
    if op == OP_TYPE_BOOL:
        return 4
    if op == OP_TYPE_VECTOR:
        return _size(t[1], types, member_off) * t[2]
    if op == OP_TYPE_MATRIX:
        return _size(t[1], types, member_off) * t[2]
    if op == OP_TYPE_ARRAY:
        return _size(t[1], types, member_off) * t[2]
    if op == OP_TYPE_STRUCT:
        end = 0
        for i, m in enumerate(t[1]):
            end = max(end, member_off.get((ty, i), 0) + _size(m, types, member_off))
        return end
    return 0


def reflect(data: bytes) -> dict:
    """{'descriptors': sorted [(set, binding, kind, count)], 'push': bytes} from a SPIR-V binary."""
    if len(data) < 20 or len(data) % 4:
        raise ValueError("not a SPIR-V binary (size)")
    w = struct.unpack("<%dI" % (len(data) // 4), data)
    if w[0] != MAGIC:
        raise ValueError("not a SPIR-V binary (magic)")
    types, consts, decos, set_of, bind_of, member_off, vars_ = {}, {}, set(), {}, {}, {}, []
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        if n == 0:
            raise ValueError("zero-length instruction")
        a = w[i + 1:i + n]
        if op == OP_TYPE_INT:
            types[a[0]] = (op, a[1])
        elif op == OP_TYPE_FLOAT:
            types[a[0]] = (op, a[1])
        elif op in (OP_TYPE_VECTOR, OP_TYPE_MATRIX):
            types[a[0]] = (op, a[1], a[2])
        elif op == OP_TYPE_ARRAY:
            types[a[0]] = (op, a[1], consts.get(a[2], 0))
        elif op == OP_TYPE_RTARRAY:
            types[a[0]] = (op, a[1])
        elif op == OP_TYPE_STRUCT:
            types[a[0]] = (op, list(a[1:]))
        elif op in (OP_TYPE_BOOL, OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_SAMPLED_IMAGE):
            types[a[0]] = (op,) + ((a[1],) if op == OP_TYPE_SAMPLED_IMAGE else ())
        elif op == OP_TYPE_POINTER:
            types[a[0]] = (op, a[2], a[1])  # pointee, storage class
        elif op == OP_CONSTANT:
            consts[a[1]] = a[2]
        elif op == OP_VARIABLE:
            vars_.append((a[1], a[0], a[2]))  # id, ptr type, storage class
        elif op == OP_DECORATE:
            if a[1] == DEC_SET:
                set_of[a[0]] = a[2]
            elif a[1] == DEC_BINDING:
                bind_of[a[0]] = a[2]
            else:
                decos.add((a[0], a[1]))
        elif op == OP_MEMBER_DECORATE and a[2] == DEC_OFFSET:
            member_off[(a[0], a[1])] = a[3]
        i += n
    descs, push = [], 0
    for vid, pty, sc in vars_:
        ptr = types.get(pty)
        if ptr is None:
            continue
        pointee = ptr[1]
        if sc == SC_PUSH:
            push = max(push, _size(pointee, types, member_off))
        elif vid in bind_of:
            count, ty = 1, pointee
            while types.get(ty, (0,))[0] == OP_TYPE_ARRAY:
                count *= max(types[ty][2], 1)
                ty = types[ty][1]
            descs.append((set_of.get(vid, 0), bind_of[vid], _kind(sc, pointee, types, decos), count))
    return {"descriptors": sorted(descs), "push": push}


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def audit(install, build, strict_orphans=False):
    """Returns (fails, warns, checked). Messages are strings naming shader and binding."""
    inst_dir = Path(install) / "shaders" / "vulkan"
    build_dir = Path(build) / "shaders" / "vulkan"
    fails, warns = [], []
    if not build_dir.is_dir():
        return [f"build shader dir missing: {build_dir}"], warns, 0
    if not inst_dir.is_dir():
        return [f"install shader dir missing: {inst_dir}"], warns, 0
    build_spv = {p.name: p for p in build_dir.glob("*.spv")}
    inst_spv = {p.name: p for p in inst_dir.glob("*.spv")}
    for name, bp in sorted(build_spv.items()):
        ip = inst_spv.get(name)
        if ip is None:
            fails.append(f"{name}: missing from install")
            continue
        if sha(bp) == sha(ip):
            continue
        fails.append(f"{name}: sha256 differs (deployed is not the build's SPIR-V)")
        try:
            rb, ri = reflect(bp.read_bytes()), reflect(ip.read_bytes())
        except ValueError as exc:
            fails.append(f"{name}: deployed SPIR-V unparsable: {exc}")
            continue
        db, di = set(rb["descriptors"]), set(ri["descriptors"])
        for d in sorted(db - di):
            fails.append(f"{name}: ABI set={d[0]} binding={d[1]} {d[2]}[{d[3]}] in build, absent/different in install")
        for d in sorted(di - db):
            fails.append(f"{name}: ABI set={d[0]} binding={d[1]} {d[2]}[{d[3]}] in install, absent/different in build")
        if rb["push"] != ri["push"]:
            fails.append(f"{name}: ABI push-constant size build={rb['push']} install={ri['push']}")
    orphans = sorted(set(inst_spv) - set(build_spv))
    for name in orphans:
        (fails if strict_orphans else warns).append(f"{name}: orphan in install, the build does not produce it")
    return fails, warns, len(build_spv)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--install", required=True)
    ap.add_argument("--build", default="build64-ninja")
    ap.add_argument("--strict-orphans", action="store_true")
    args = ap.parse_args(argv)
    fails, warns, n = audit(args.install, args.build, args.strict_orphans)
    for m in warns:
        print("WARN " + m)
    for m in fails:
        print("FAIL " + m)
    print(f"check_deployed_spv: {n} build .spv checked, {len(fails)} FAIL, {len(warns)} WARN ({args.install})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
