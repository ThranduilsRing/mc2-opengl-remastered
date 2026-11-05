#!/usr/bin/env python3
"""crashpad_proof.py -- box proof for the crashpad minidump writer (run by `box_run.py -- "sh:python3 scripts/crashpad_proof.py"`).

Crashes the installed game on purpose (MC2_TEST_CRASH_FRAME) under strace (network syscalls), then reads the newest
crashpad minidump with a tiny parser (exception stream -> faulting thread RIP, module list) and symbolizes the RIP with
llvm-symbolizer/addr2line against the debug binary. Prints the [CRASH] log lines and any network syscall found.
"""
import glob
import os
import shutil
import struct
import subprocess
import sys


def parse(path):
    d = open(path, "rb").read()
    sig, ver, n, dirrva = struct.unpack_from("<IIII", d, 0)
    assert sig == 0x504D444D, "not a minidump"
    streams = {}
    for i in range(n):
        t, size, rva = struct.unpack_from("<III", d, dirrva + 12 * i)
        streams[t] = (size, rva)
    mods = []
    if 4 in streams:
        cnt, = struct.unpack_from("<I", d, streams[4][1])
        for i in range(cnt):
            o = streams[4][1] + 4 + 108 * i
            base, size = struct.unpack_from("<QI", d, o)
            nrva, = struct.unpack_from("<I", d, o + 20)
            ln, = struct.unpack_from("<I", d, nrva)
            mods.append((base, size, d[nrva + 4:nrva + 4 + ln].decode("utf-16le")))
    tid, code, addr, ctxsize, ctxrva = None, None, None, 0, 0
    if 6 in streams:
        o = streams[6][1]
        tid, = struct.unpack_from("<I", d, o)
        code, = struct.unpack_from("<I", d, o + 8)
        addr, = struct.unpack_from("<Q", d, o + 24)
        ctxsize, ctxrva = struct.unpack_from("<II", d, o + 8 + 4 + 4 + 8 + 8 + 4 + 4 + 15 * 8)
    rip = rsp = None
    if ctxrva and ctxsize >= 0x100:
        rsp, = struct.unpack_from("<Q", d, ctxrva + 0x98)
        rip, = struct.unpack_from("<Q", d, ctxrva + 0xF8)
    return mods, tid, code, addr, rip, rsp


def main():
    inst = os.environ["INSTALL"]
    out = os.environ.get("OUT", ".")
    exe = os.path.join(inst, "mc2")
    for p in glob.glob(os.path.join(inst, "crashes", "crashpad", "*", "*.dmp")):
        os.remove(p)
    env = dict(os.environ, MC2_TEST_CRASH_FRAME="200")
    strace = shutil.which("strace")
    cmd = [sys.executable, "scripts/run_smoke.py", "--mission", "mc2_01", "--duration", "15", "--keep-logs", "--exe", exe]
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    open(os.path.join(out, "crash_run.log"), "w").write(r.stdout)
    if strace:  # second run under strace for the network check only (a tracer would block the handler's own ptrace)
        subprocess.run([strace, "-f", "-e", "trace=connect,sendto,sendmsg", "-o", os.path.join(out, "net.txt")] + cmd,
                       env=dict(env, MC2_TEST_CRASH_FRAME="0"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("strace:", strace, "smoke rc:", r.returncode)
    for line in r.stdout.splitlines():
        if "[CRASH]" in line:
            print(line[:240])
    for p in glob.glob("smoke_logs/**/*", recursive=True):
        if os.path.isfile(p):
            for line in open(p, errors="replace"):
                if "[CRASH]" in line:
                    print(os.path.basename(p), line.strip()[:240])
    net = os.path.join(out, "net.txt")
    if os.path.exists(net):
        lines = [l for l in open(net, errors="replace") if "AF_INET" in l or "AF_INET6" in l]
        print("network connect/send syscalls with AF_INET:", len(lines))
        for l in lines[:5]:
            print("  ", l.strip()[:200])
    dmps = sorted(glob.glob(os.path.join(inst, "crashes", "crashpad", "*", "*.dmp")), key=os.path.getmtime)
    print("dumps:", dmps)
    if not dmps:
        return 1
    mods, tid, code, addr, rip, rsp = parse(dmps[-1])
    print("exception code=%s addr=%s rip=%s rsp=%s modules=%d size=%d" % (hex(code or 0), hex(addr or 0), hex(rip or 0), hex(rsp or 0), len(mods), os.path.getsize(dmps[-1])))
    main_mod = next((m for m in mods if m[2].endswith("/mc2")), None)
    print("main module:", main_mod and (hex(main_mod[0]), main_mod[2]))
    if main_mod and rip:
        sym = shutil.which("llvm-symbolizer") or shutil.which("llvm-symbolizer-18") or shutil.which("addr2line")
        for label, a in (("rva", rip - main_mod[0]), ("abs", rip)):
            args = [sym, "-e", exe, hex(a)] if sym and sym.endswith("addr2line") else [sym, "--obj=" + exe, "--inlining", hex(a)]
            args = [x for x in args if x]
            if sym and sym.endswith("addr2line"):
                args = [sym, "-f", "-C", "-i", "-e", exe, hex(a)]
            q = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            print("symbolize(%s) %s: %s" % (label, hex(a), q.stdout.strip().replace("\n", " | ")[:300]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
