"""List threads in a Windows minidump and symbolize likely return addresses on each stack.
usage: dumpthreads.py dump.dmp [exe_pdb_dir]
"""
import ctypes, ctypes.wintypes as wt, struct, sys, os

path = sys.argv[1]
data = open(path, "rb").read()
sig, ver, nstreams, dir_rva = struct.unpack_from("<4sIII", data, 0)
assert sig == b"MDMP"
streams = {}
for i in range(nstreams):
    t, sz, rva = struct.unpack_from("<III", data, dir_rva + 12 * i)
    streams[t] = (sz, rva)

# modules (stream 4)
mods = []
sz, rva = streams[4]
n = struct.unpack_from("<I", data, rva)[0]
for i in range(n):
    off = rva + 4 + i * 108
    base, size, chk, ts, name_rva = struct.unpack_from("<QIIII", data, off)
    nlen = struct.unpack_from("<I", data, name_rva)[0]
    name = data[name_rva + 4:name_rva + 4 + nlen].decode("utf-16le")
    mods.append((base, size, name))

# memory ranges: MemoryListStream(5) and Memory64ListStream(9)
mem = []
if 5 in streams:
    sz, rva = streams[5]
    n = struct.unpack_from("<I", data, rva)[0]
    for i in range(n):
        start, dsz, drva = struct.unpack_from("<QII", data, rva + 4 + 16 * i)
        mem.append((start, dsz, drva))
if 9 in streams:
    sz, rva = streams[9]
    n, base_rva = struct.unpack_from("<QQ", data, rva)
    cur = base_rva
    for i in range(n):
        start, dsz = struct.unpack_from("<QQ", data, rva + 16 + 16 * i)
        mem.append((start, dsz, cur)); cur += dsz

def modfor(a):
    for b, s, nm in mods:
        if b <= a < b + s:
            return b, nm
    return None

# threads (stream 3)
sz, rva = streams[3]
n = struct.unpack_from("<I", data, rva)[0]
print(f"{n} threads, {len(mods)} modules")
exe = [m for m in mods if m[2].lower().endswith("mc2.exe")][0]

# dbghelp for the exe PDB
dbg = ctypes.WinDLL("dbghelp.dll")
dbg.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
proc = ctypes.windll.kernel32.GetCurrentProcess()
dbg.SymSetOptions(0x2 | 0x10 | 0x1000)  # UNDNAME|DEFERRED... |LOAD_LINES(0x10)
searchpath = os.path.dirname(sys.argv[2]).replace("/", chr(92)).encode() if len(sys.argv) > 2 else b"A:/Games/mc2-opengl/mc2-win64-lane3"
dbg.SymInitialize.restype = wt.BOOL
print("init", dbg.SymInitialize(proc, searchpath, False))
dbg.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p]
dbg.SymFromAddr.restype = wt.BOOL
dbg.SymLoadModuleEx.restype = ctypes.c_uint64
dbg.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64, wt.DWORD, ctypes.c_void_p, wt.DWORD]
exepath = (sys.argv[2] if len(sys.argv) > 2 else "A:/Games/mc2-opengl/mc2-win64-lane3/mc2.exe").replace("/", chr(92)).encode()
r=dbg.SymLoadModuleEx(proc, None, exepath, None, exe[0], exe[1], None, 0)
print("SymLoadModuleEx ->", hex(r), "err", ctypes.windll.kernel32.GetLastError(), exepath)

class SYM(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2), ("Index", wt.ULONG), ("Size", wt.ULONG),
                ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG), ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG), ("Name", ctypes.c_char * 300)]

def symname(a):
    s = SYM(); s.SizeOfStruct = 88; s.MaxNameLen = 299
    d = ctypes.c_uint64()
    ok = dbg.SymFromAddr(proc, ctypes.c_uint64(a), ctypes.byref(d), ctypes.byref(s))
    if not ok and not getattr(symname, "warned", False):
        symname.warned = True; print("SymFromAddr failed err", ctypes.windll.kernel32.GetLastError())
    if ok:
        return s.Name.decode(errors="replace") + f"+0x{d.value:x}"
    return None

def readmem(a, n):
    for start, dsz, drva in mem:
        if start <= a and a + n <= start + dsz:
            return data[drva + (a - start): drva + (a - start) + n]
    return None

for i in range(n):
    off = rva + 4 + i * 48
    tid, susp, prio_class, prio, teb, st_start, st_size, st_rva, ctx_size, ctx_rva = struct.unpack_from("<IIIIQQIIII", data, off)
    # CONTEXT x64: Rip at 0xF8, Rsp at 0x98
    rip = struct.unpack_from("<Q", data, ctx_rva + 0xF8)[0]
    rsp = struct.unpack_from("<Q", data, ctx_rva + 0x98)[0]
    print(f"--- thread {i} tid={tid} rip={rip:#x} rsp={rsp:#x}")
    def desc(a):
        m = modfor(a)
        if not m: return None
        s = symname(a) if m[1].lower().endswith("mc2.exe") else None
        return f"{os.path.basename(m[1])}+{a - m[0]:#x}" + (f" {s}" if s else "")
    print("   rip:", desc(rip))
    # scan stack words for code addresses in modules
    stack = readmem(st_start, st_size) if st_size else None
    if stack is None:
        # use the stack from rsp
        stack = readmem(rsp, min(0x4000, st_size or 0x4000))
        base = rsp
    else:
        base = st_start
    seen = 0
    if stack:
        lo = max(0, rsp - base)
        for k in range(lo, len(stack) - 8, 8):
            v = struct.unpack_from("<Q", stack, k)[0]
            m = modfor(v)
            if m and seen < 14:
                d = desc(v)
                if d:
                    print("     ", d); seen += 1
