"""String-hunter diagnostic for the mc2_15 heap writer. Applies a DIAGNOSTIC patch (never commit):
gos_crashbundle.cpp gets a background thread that scans every committed, writable region of the
process (~4x per second) for the leaked digit string the crash dumps hold (553810841,-19983...)
and, on a hit, appends a hex+ASCII dump of the 512 bytes BEFORE it (the writer's buffer = the
string's prefix = the format shape) plus region info to <exe_dir>/crashes/strhunt.log, then arms a
hardware write watchpoint on the string's tail on every thread, so the next rewrite of the same
bytes ends in a crash bundle whose stack is the writer's. gameosmain.cpp calls the tick at the top
of draw_screen (starts the thread, counts frames).
usage: py -3 strhunt_patch.py apply|revert   (run from anywhere; paths are absolute)
"""
import sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
CB = os.path.join(ROOT, "GameOS", "gameos", "gos_crashbundle.cpp")
GM = os.path.join(ROOT, "GameOS", "gameos", "gameosmain.cpp")
MARK = "// [DIAG_STRHUNT]"

DIAG = r'''
// [DIAG_STRHUNT] diagnostic patch (strhunt_patch.py) -- never commit
#include <tlhelp32.h>
namespace diagsh {
// PSAPI_WORKING_SET_EX_INFORMATION without <psapi.h>: a shim header of that name in GameOS/include shadows the SDK's
struct WsEx { PVOID VirtualAddress; ULONG_PTR Flags; };   // Flags bit 0 = Valid (resident)
typedef BOOL (WINAPI* QwsExFn)(HANDLE, PVOID, DWORD);
static QwsExFn g_qws = nullptr;
static bool g_crashMode = false;   // set by the crash-time scan: shape matches allowed, no watchpoint, bigger report cap
// Valid (bit 0), not Shared (bit 15), not Locked (bit 22: driver-pinned), Win32Protection (bits 4..14) without
// PAGE_NOCACHE/PAGE_WRITECOMBINE: reading pinned write-combined pages ran at 16 MB/s (53 s per pass)
static inline bool pageOk(ULONG_PTR f) {
    if (!(f & 1) || (f & (1ull << 15)) || (f & (1ull << 22))) return false;
    const ULONG_PTR prot = (f >> 4) & 0x7FF;
    return (prot & (PAGE_NOCACHE | PAGE_WRITECOMBINE)) == 0;
}
// Owner table for 41..56-byte gos_Malloc blocks (the writer's buffer is a 50-byte heap block): pointer -> allocation
// stack, plus the free stack once freed. Direct-mapped, lossy, lock-free enough for a diagnostic.
struct Owner { void* p; size_t req; void* st[12]; USHORT n; void* fst[12]; USHORT fn; LONG frame;
               size_t preq; void* pst[12]; USHORT pn; void* pfst[12]; USHORT pfn; LONG pframe; };   // p* = the previous owner of the same address
static const size_t kOwnerTab = 1u << 20;
static Owner* g_owner = nullptr;
static inline size_t ownerSlot(void* p) { return (size_t)((((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull) >> 24) & (kOwnerTab - 1); }
static bool g_symInit = false;
static void symLine(void* a, char* out, size_t cap) {
    HANDLE proc = GetCurrentProcess();
    if (!g_symInit) { SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME); SymInitialize(proc, nullptr, TRUE); g_symInit = true; }
    char buf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO* si = (SYMBOL_INFO*)buf; si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
    DWORD64 d = 0; IMAGEHLP_LINE64 ln; ln.SizeOfStruct = sizeof(ln); DWORD ld = 0;
    if (SymFromAddr(proc, (DWORD64)a, &d, si)) {
        if (SymGetLineFromAddr64(proc, (DWORD64)a, &ld, &ln)) snprintf(out, cap, "%s (%s:%lu)", si->Name, ln.FileName, ln.LineNumber);
        else snprintf(out, cap, "%s+0x%llx", si->Name, (unsigned long long)d);
    } else snprintf(out, cap, "%p", a);
}
static void printStack(const char* tag, void* const* st, USHORT n);
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_thread = nullptr;
static volatile LONG g_frame = 0;
static int g_reports = 0;
static bool g_armed = false;
static uintptr_t g_reported[64]; static int g_nReported = 0;
static unsigned char g_needleX[16]; static size_t g_needleLen = 0;   // XOR 0x5A so the scan never finds its own needle
static const unsigned char kX = 0x5A;

static void outRaw(const char* s, size_t n) {
    DWORD w = 0;
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, s, (DWORD)n, &w, nullptr);
    fwrite(s, 1, n, stderr); fflush(stderr);
}
static void outf(const char* fmt, ...) {
    char line[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (n < 0) return; if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;
    outRaw(line, (size_t)n);
}
static bool safeRead(const unsigned char* p, size_t n, unsigned char* out) {
    __try { memcpy(out, p, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void dumpRange(const unsigned char* a, size_t n, const unsigned char* ref) {
    unsigned char row[32]; char line[200];
    for (size_t o = 0; o < n; o += 32) {
        size_t m = (n - o < 32) ? n - o : 32;
        if (!safeRead(a + o, m, row)) { outf("    %p: <unreadable>\n", (const void*)(a + o)); continue; }
        int pos = snprintf(line, sizeof line, "    %p %+6lld: ", (const void*)(a + o), (long long)((a + o) - ref));
        for (size_t k = 0; k < m; ++k) pos += snprintf(line + pos, sizeof line - pos, "%02X ", row[k]);
        for (size_t k = m; k < 32; ++k) pos += snprintf(line + pos, sizeof line - pos, "   ");
        pos += snprintf(line + pos, sizeof line - pos, "|");
        for (size_t k = 0; k < m; ++k) pos += snprintf(line + pos, sizeof line - pos, "%c", (row[k] >= 32 && row[k] < 127) ? (char)row[k] : '.');
        pos += snprintf(line + pos, sizeof line - pos, "|\n");
        outRaw(line, (size_t)pos);
    }
}
static int setDr0(HANDLE h, void* addr) {
    CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(h, &c)) return 0;
    c.Dr0 = (DWORD64)addr; c.Dr7 = (c.Dr7 & ~0xF0003ull) | 0x1ull | (0x1ull << 16) | (0x3ull << 18);  // L0, write, 8 bytes
    return SetThreadContext(h, &c) ? 1 : 0;
}
static void armWatch(void* addr) {
    if (g_armed) return;
    g_armed = true;
    const DWORD me = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) { outf("[STRHUNT] snapshot failed\n"); return; }
    THREADENTRY32 te; te.dwSize = sizeof te; int n = 0, tried = 0;
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!h) continue;
        ++tried;
        if (SuspendThread(h) != (DWORD)-1) { n += setDr0(h, addr); ResumeThread(h); }
        CloseHandle(h);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    outf("[STRHUNT] frame=%ld watchpoint armed at %p on %d/%d threads\n", g_frame, addr, n, tried);
}
static const unsigned char* scanRegion(const unsigned char* base, size_t len) {
    __try {
        const unsigned char* p = base; const unsigned char* end = base + len;
        // shape match, not content: >= 12 decimal digits, ",-", >= 12 decimal digits (the leaked pair is v,-v with 18
        // digits each; the content may differ between builds, the shape of the writer's output does not)
        // relaxed to ANY run of >= 15 decimal digits: the writer's block is freed soon after the write, the victim
        // may be reallocated (its header rewritten), and only the writer's own 50 bytes (prefix + "(1998397155...")
        // survive long enough for a 1 Hz scan
        while (p + 15 <= end) {
            // a 15-digit run must contain one of the bytes at p+14, p+29, ... : hop by 15, verify around the hit
            const unsigned char* q = p + 14;
            if (q[0] < '0' || q[0] > '9') { p += 15; continue; }
            const unsigned char* lo = q; while (lo > base && lo[-1] >= '0' && lo[-1] <= '9') --lo;
            const unsigned char* hi = q + 1; while (hi < end && hi[0] >= '0' && hi[0] <= '9') ++hi;
            // the leaked pair reads "<digits>,-<digits>)": accept a run only when ',' or ')' follows it (RTTI names such as
            // ratio<1,1000000000000000000> end in '>' and burned every report slot on the first pass)
            if (hi - lo >= 12 && hi < end && (hi[0] == ',' || hi[0] == ')')) {
                // exact content first (fixed for this code family), else a run in TEXT context: a printable separator
                // before it (pixel rows and shader float tables matched the bare shape)
                size_t k = 0; while (k < g_needleLen && lo + k < end && lo[k] == (unsigned char)(g_needleX[k] ^ kX)) ++k;
                if (k == g_needleLen) return lo;
                // (a text-context shape match was tried and still matched pixel rows: exact content only while live;
                // at crash time every shape match is reported, the real one is recognisable by its context)
                if (g_crashMode) return lo;
            }
            p = hi;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return nullptr;
}
static void printStack(const char* tag, void* const* st, USHORT n) {
    char line[600];
    for (USHORT i = 0; i < n; ++i) { symLine(st[i], line, sizeof line); outf("    %s #%u %s\n", tag, (unsigned)i, line); }
}
static void onAlloc(void* p, size_t req) {
    if (!p || !g_owner || req < 41 || req > 56) return;
    Owner& o = g_owner[ownerSlot(p)];
    if (o.p == p) { o.preq = o.req; memcpy(o.pst, o.st, sizeof o.st); o.pn = o.n; memcpy(o.pfst, o.fst, sizeof o.fst); o.pfn = o.fn; o.pframe = o.frame; }
    else { o.pn = 0; o.pfn = 0; }
    o.p = p; o.req = req; o.fn = 0; o.frame = g_frame;
    o.n = RtlCaptureStackBackTrace(1, 12, o.st, nullptr);
}
static void onFree(void* p) {
    if (!p || !g_owner) return;
    Owner& o = g_owner[ownerSlot(p)];
    if (o.p != p) return;
    o.fn = RtlCaptureStackBackTrace(1, 12, o.fst, nullptr);
}
// the digits start up to ~80 bytes into the writer's block: probe every 16-aligned candidate start before them
static void reportOwner(const unsigned char* q) {
    if (!g_owner) return;
    for (uintptr_t off = 0; off <= 96; off += 16) {
        void* p = (void*)(((uintptr_t)q & ~(uintptr_t)15) - off);
        const Owner& o = g_owner[ownerSlot(p)];
        if (o.p != p) continue;
        outf("[STRHUNT] owner block %p req=%zu alloc_frame=%ld freed=%d (digits at +%lld)\n", p, o.req, o.frame, o.fn ? 1 : 0, (long long)(q - (const unsigned char*)p));
        printStack("alloc", o.st, o.n);
        if (o.fn) printStack("free", o.fst, o.fn);
        if (o.pn) {
            outf("[STRHUNT] previous owner of %p req=%zu alloc_frame=%ld freed=%d\n", p, o.preq, o.pframe, o.pfn ? 1 : 0);
            printStack("prev-alloc", o.pst, o.pn);
            if (o.pfn) printStack("prev-free", o.pfst, o.pfn);
        }
    }
}
static void report(const unsigned char* q, const MEMORY_BASIC_INFORMATION& mbi) {
    for (int i = 0; i < g_nReported; ++i) { uintptr_t d = (uintptr_t)q > g_reported[i] ? (uintptr_t)q - g_reported[i] : g_reported[i] - (uintptr_t)q; if (d < 256) return; }
    if (g_nReported < 64) g_reported[g_nReported++] = (uintptr_t)q;
    if (g_reports >= (g_crashMode ? 160 : 40)) return;
    ++g_reports;
    reportOwner(q);
    const char* type = mbi.Type == MEM_PRIVATE ? "PRIVATE" : mbi.Type == MEM_IMAGE ? "IMAGE" : mbi.Type == MEM_MAPPED ? "MAPPED" : "?";
    outf("[STRHUNT] HIT #%d frame=%ld at %p region base=%p size=0x%llx type=%s protect=0x%lx\n",
         g_reports, g_frame, (const void*)q, mbi.BaseAddress, (unsigned long long)mbi.RegionSize, type, mbi.Protect);
    const unsigned char* lo = q - 512; if (lo < (const unsigned char*)mbi.BaseAddress) lo = (const unsigned char*)mbi.BaseAddress;
    const unsigned char* hi = q + 160; const unsigned char* rend = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize; if (hi > rend) hi = rend;
    dumpRange(lo, (size_t)(hi - lo), q);
    if (!g_armed && !g_crashMode) armWatch((void*)(((uintptr_t)q + 24) & ~(uintptr_t)7));   // the ")\0" tail, away from freelist links at a block's start
}
// one pass over every ordinary resident private RW page; selfSp = a stack address of the calling thread (its stack is skipped)
static void scanPass(uintptr_t selfSp, unsigned long long& scanned, unsigned& regions) {
    {
        uintptr_t addr = 0x10000; MEMORY_BASIC_INFORMATION mbi;
        while (addr < 0x00007FFFFFFF0000ull && VirtualQuery((LPCVOID)addr, &mbi, sizeof mbi) == sizeof mbi) {
            const uintptr_t base = (uintptr_t)mbi.BaseAddress, next = base + mbi.RegionSize;
            const bool rw = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
            const bool ownStack = selfSp >= base && selfSp < next;
            // heap segments only (the victims and the writer's buffer are LFH blocks): small private RW regions; big
            // regions are textures, staging buffers and driver memory and scanning them starved the main thread
            if (mbi.State == MEM_COMMIT && rw && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && !ownStack && mbi.Type == MEM_PRIVATE && mbi.RegionSize <= (64u << 20)) {
                // only RESIDENT pages: touching a committed-but-untouched page makes the OS materialise it, which
                // ballooned the working set to ~6 GB and paged the game down to 6 fps
                static WsEx ws[4096];
                const size_t kPage = 4096;
                for (uintptr_t pg = base; pg < next; ) {
                    size_t n = (next - pg) / kPage; if (n > 4096) n = 4096;
                    for (size_t i = 0; i < n; ++i) ws[i].VirtualAddress = (PVOID)(pg + i * kPage);
                    if (!g_qws || !g_qws(GetCurrentProcess(), ws, (DWORD)(n * sizeof(ws[0])))) break;
                    size_t i = 0;
                    while (i < n) {
                        if (!pageOk(ws[i].Flags)) { ++i; continue; }
                        size_t j = i; while (j < n && pageOk(ws[j].Flags)) ++j;   // one run of ordinary resident pages
                        const unsigned char* p = (const unsigned char*)(pg + i * kPage); size_t left = (j - i) * kPage;
                        scanned += left; ++regions;
                        while (left >= g_needleLen) {
                            const unsigned char* q = scanRegion(p, left);
                            if (!q) break;
                            report(q, mbi);
                            const unsigned char* np = q + 64; if (np >= p + left) break;
                            left -= (size_t)(np - p); p = np;
                        }
                        i = j;
                    }
                    pg += n * kPage;
                }
            }
            if (next <= addr) break;
            addr = next;
        }
    }
}
static DWORD WINAPI scanProc(LPVOID) {
    volatile unsigned char self; const uintptr_t selfSp = (uintptr_t)&self;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    unsigned pass = 0;
    for (;;) {
        Sleep(1000);
        if (g_crashMode) { Sleep(INFINITE); }
        const ULONGLONG t0 = GetTickCount64(); unsigned long long scanned = 0; unsigned regions = 0;
        scanPass(selfSp, scanned, regions);
        if ((++pass % 10u) == 1u) outf("[STRHUNT] pass=%u frame=%ld ms=%llu resident_MB=%llu runs=%u\n", pass, g_frame, (unsigned long long)(GetTickCount64() - t0), scanned >> 20, regions);
    }
}
// called from the heap-corruption vectored handler before the bundle: the string the heap just tripped over is still in memory
void onCrash() {
    g_crashMode = true;
    volatile unsigned char self;
    outf("[STRHUNT] crash-time scan frame=%ld\n", g_frame);
    unsigned long long scanned = 0; unsigned regions = 0;
    scanPass((uintptr_t)&self, scanned, regions);
    outf("[STRHUNT] crash-time scan done resident_MB=%llu reports=%d\n", scanned >> 20, g_reports);
}
static void start() {
    static const char kNeedle[] = "199839715553810841,-1998";   // the leaked pair as the quarantine dumps hold it
    g_needleLen = sizeof(kNeedle) - 1;
    for (size_t i = 0; i < g_needleLen; ++i) g_needleX[i] = (unsigned char)kNeedle[i] ^ kX;
    char path[MAX_PATH]; DWORD got = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (got > 0 && got < MAX_PATH) { for (int i = (int)got - 1; i >= 0; --i) if (path[i] == '\\' || path[i] == '/') { path[i] = 0; break; } }
    else strcpy_s(path, ".");
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\crashes", path); CreateDirectoryA(dir, nullptr);
    char file[MAX_PATH]; snprintf(file, sizeof file, "%s\\crashes\\strhunt.log", path);
    g_log = CreateFileA(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    SYSTEMTIME st; GetLocalTime(&st);
    outf("[STRHUNT] start %04u-%02u-%02u %02u:%02u:%02u pid=%lu log=%s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId(), file);
    g_owner = (Owner*)VirtualAlloc(nullptr, sizeof(Owner) * kOwnerTab, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (HMODULE k = GetModuleHandleA("kernel32.dll")) g_qws = (QwsExFn)GetProcAddress(k, "K32QueryWorkingSetEx");
    outf("[STRHUNT] K32QueryWorkingSetEx=%p\n", (void*)g_qws);
    g_thread = CreateThread(nullptr, 256 * 1024, scanProc, nullptr, 0, nullptr);
}
void tick() {
    if (InterlockedIncrement(&g_frame) == 1) start();
}
}  // namespace diagsh
void diag_strhunt_tick() { diagsh::tick(); }
void diag_strhunt_on_crash() { diagsh::onCrash(); }
void diag_strhunt_on_alloc(void* p, size_t req) { diagsh::onAlloc(p, req); }
void diag_strhunt_on_free(void* p) { diagsh::onFree(p); }
'''

HOOK_OLD = "static void draw_screen( void )\n{\n    g_disable_quads = false;\n"
HOOK_NEW = "static void draw_screen( void )\n{\n    { extern void diag_strhunt_tick(); diag_strhunt_tick(); }  " + MARK + "\n    g_disable_quads = false;\n"


def rd(p):
    with open(p, "rb") as f:
        return f.read().decode("utf-8")


def wr(p, s):
    with open(p, "wb") as f:
        f.write(s.encode("utf-8"))


GO = os.path.join(ROOT, "GameOS", "gameos", "gameos.cpp")
ALLOC_OLD = "    return malloc(bytes);\n}\nvoid __stdcall gos_Free(void* ptr)\n"
ALLOC_NEW = ("    void* r_ = malloc(bytes);  " + MARK + "\n    { extern void diag_strhunt_on_alloc(void*, size_t); diag_strhunt_on_alloc(r_, bytes); }\n"
             "    return r_;\n}\nvoid __stdcall gos_Free(void* ptr)\n")
FREE_OLD = "    free(ptr);\n}\n"
FREE_NEW = "    { extern void diag_strhunt_on_free(void*); diag_strhunt_on_free(ptr); }  " + MARK + "\n    free(ptr);\n}\n"
DUMP_OLD = "MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory"
DUMP_NEW = DUMP_OLD   # the bigger dumps never completed before the harness killed the process


def apply():
    cb = rd(CB)
    gm = rd(GM)
    go = rd(GO)
    if MARK in cb or MARK in gm or MARK in go:
        print("already applied"); return 1
    nl = "\r\n" if "\r\n" in cb else "\n"
    if DUMP_OLD not in cb:
        print("minidump type anchor not found"); return 3
    cb = cb.replace(DUMP_OLD, DUMP_NEW, 1)
    hook_old = "    __try {\n        build_crash_folder();\n".replace("\n", nl)
    if cb.count(hook_old) != 1:
        print("heap-corruption handler anchor not found", cb.count(hook_old)); return 5
    cb = cb.replace(hook_old, ("    __try {\n        { extern void diag_strhunt_on_crash(); diag_strhunt_on_crash(); }  " + MARK + "\n        build_crash_folder();\n").replace("\n", nl), 1)
    cb = cb + nl + DIAG.replace("\n", nl)
    gnl = "\r\n" if "\r\n" in gm else "\n"
    old = HOOK_OLD.replace("\n", gnl); new = HOOK_NEW.replace("\n", gnl)
    if old not in gm:
        print("draw_screen anchor not found"); return 2
    gm = gm.replace(old, new, 1)
    onl = "\r\n" if "\r\n" in go else "\n"
    ao = ALLOC_OLD.replace("\n", onl); fo = FREE_OLD.replace("\n", onl)
    if go.count(ao) != 1 or go.count(fo) != 1:
        print("gos_Malloc/gos_Free anchors not found", go.count(ao), go.count(fo)); return 4
    go = go.replace(ao, ALLOC_NEW.replace("\n", onl), 1).replace(fo, FREE_NEW.replace("\n", onl), 1)
    wr(CB, cb); wr(GM, gm); wr(GO, go)
    print("applied"); return 0


def revert():
    import subprocess
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import console_child
    r = subprocess.run(["git", "-C", ROOT, "checkout", "--", "GameOS/gameos/gos_crashbundle.cpp", "GameOS/gameos/gameosmain.cpp", "GameOS/gameos/gameos.cpp"],
                       **console_child.child_kwargs())
    print("reverted rc", r.returncode); return r.returncode


if __name__ == "__main__":
    sys.exit(apply() if (len(sys.argv) > 1 and sys.argv[1] == "apply") else revert())
