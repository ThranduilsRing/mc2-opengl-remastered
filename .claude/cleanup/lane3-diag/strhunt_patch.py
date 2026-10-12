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
#include <psapi.h>
namespace diagsh {
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_thread = nullptr;
static volatile LONG g_frame = 0;
static int g_reports = 0;
static bool g_armed = false;
static uintptr_t g_reported[32]; static int g_nReported = 0;
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
        const unsigned char first = (unsigned char)(g_needleX[0] ^ kX);
        while (p < end) {
            const unsigned char* q = (const unsigned char*)memchr(p, first, (size_t)(end - p));
            if (!q) return nullptr;
            if ((size_t)(end - q) >= g_needleLen) {
                size_t k = 1; while (k < g_needleLen && q[k] == (unsigned char)(g_needleX[k] ^ kX)) ++k;
                if (k == g_needleLen) return q;
            }
            p = q + 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return nullptr;
}
static void report(const unsigned char* q, const MEMORY_BASIC_INFORMATION& mbi) {
    for (int i = 0; i < g_nReported; ++i) { uintptr_t d = (uintptr_t)q > g_reported[i] ? (uintptr_t)q - g_reported[i] : g_reported[i] - (uintptr_t)q; if (d < 256) return; }
    if (g_nReported < 32) g_reported[g_nReported++] = (uintptr_t)q;
    if (g_reports >= 12) return;
    ++g_reports;
    const char* type = mbi.Type == MEM_PRIVATE ? "PRIVATE" : mbi.Type == MEM_IMAGE ? "IMAGE" : mbi.Type == MEM_MAPPED ? "MAPPED" : "?";
    outf("[STRHUNT] HIT #%d frame=%ld at %p region base=%p size=0x%llx type=%s protect=0x%lx\n",
         g_reports, g_frame, (const void*)q, mbi.BaseAddress, (unsigned long long)mbi.RegionSize, type, mbi.Protect);
    const unsigned char* lo = q - 512; if (lo < (const unsigned char*)mbi.BaseAddress) lo = (const unsigned char*)mbi.BaseAddress;
    const unsigned char* hi = q + 160; const unsigned char* rend = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize; if (hi > rend) hi = rend;
    dumpRange(lo, (size_t)(hi - lo), q);
    if (!g_armed) armWatch((void*)(((uintptr_t)q + 24) & ~(uintptr_t)7));   // the ")\0" tail, away from freelist links at a block's start
}
static DWORD WINAPI scanProc(LPVOID) {
    volatile unsigned char self; const uintptr_t selfSp = (uintptr_t)&self;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Sleep(1000);
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
                static PSAPI_WORKING_SET_EX_INFORMATION ws[4096];
                const size_t kPage = 4096;
                for (uintptr_t pg = base; pg < next; ) {
                    size_t n = (next - pg) / kPage; if (n > 4096) n = 4096;
                    for (size_t i = 0; i < n; ++i) ws[i].VirtualAddress = (PVOID)(pg + i * kPage);
                    if (!QueryWorkingSetEx(GetCurrentProcess(), ws, (DWORD)(n * sizeof(ws[0])))) break;
                    size_t i = 0;
                    while (i < n) {
                        if (!ws[i].VirtualAttributes.Valid) { ++i; continue; }
                        size_t j = i; while (j < n && ws[j].VirtualAttributes.Valid) ++j;   // one resident run
                        const unsigned char* p = (const unsigned char*)(pg + i * kPage); size_t left = (j - i) * kPage;
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
static void start() {
    static const char kNeedle[] = "553810841,-1998";   // the first 15 bytes of every victim string in the crash dumps
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
    g_thread = CreateThread(nullptr, 256 * 1024, scanProc, nullptr, 0, nullptr);
}
void tick() {
    if (InterlockedIncrement(&g_frame) == 1) start();
}
}  // namespace diagsh
void diag_strhunt_tick() { diagsh::tick(); }
'''

HOOK_OLD = "static void draw_screen( void )\n{\n    g_disable_quads = false;\n"
HOOK_NEW = "static void draw_screen( void )\n{\n    { extern void diag_strhunt_tick(); diag_strhunt_tick(); }  " + MARK + "\n    g_disable_quads = false;\n"


def rd(p):
    with open(p, "rb") as f:
        return f.read().decode("utf-8")


def wr(p, s):
    with open(p, "wb") as f:
        f.write(s.encode("utf-8"))


def apply():
    cb = rd(CB)
    gm = rd(GM)
    if MARK in cb or MARK in gm:
        print("already applied"); return 1
    nl = "\r\n" if "\r\n" in cb else "\n"
    cb = cb + nl + DIAG.replace("\n", nl)
    gnl = "\r\n" if "\r\n" in gm else "\n"
    old = HOOK_OLD.replace("\n", gnl); new = HOOK_NEW.replace("\n", gnl)
    if old not in gm:
        print("draw_screen anchor not found"); return 2
    gm = gm.replace(old, new, 1)
    wr(CB, cb); wr(GM, gm)
    print("applied"); return 0


def revert():
    import subprocess
    r = subprocess.run(["git", "-C", ROOT, "checkout", "--", "GameOS/gameos/gos_crashbundle.cpp", "GameOS/gameos/gameosmain.cpp"])
    print("reverted rc", r.returncode); return r.returncode


if __name__ == "__main__":
    sys.exit(apply() if (len(sys.argv) > 1 and sys.argv[1] == "apply") else revert())
